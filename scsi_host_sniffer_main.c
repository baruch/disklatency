/*
    This code is part of Disk Latency
    Copyright (C) 2015 Baruch Even

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#include <linux/version.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/delay.h>

#include <linux/relay.h>
#include <linux/debugfs.h>

#include <scsi/scsi.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_dbg.h>

#include <linux/time.h>

// TODO: take refcount of module we attach to to prevent incorrect order of module removals

static struct scsi_host_template *old_scsi_host_template;
static int (*old_scsi_queuecommand)(struct Scsi_Host *scsi_host, struct scsi_cmnd *cmnd);
static struct rchan *relay_chan;

static int hostnum = -1;
module_param(hostnum, int, 0644);

struct cmnd_track {
	struct list_head list;
	struct scsi_cmnd *cmnd;
	void (*scsi_done)(struct scsi_cmnd *);
	s64 start_time;
};

static DEFINE_SPINLOCK(track_lock);
static LIST_HEAD(track_list);

static s64 get_current_time(void)
{
	struct timespec ts;
	getrawmonotonic(&ts);
	return timespec_to_ns(&ts);
}

static struct cmnd_track *cmnd_track_from_cmnd(struct scsi_cmnd *cmnd)
{
	struct cmnd_track *track;

	list_for_each_entry(track, &track_list, list) {
		if (track->cmnd == cmnd)
			return track;
	}

	return NULL;
}

static void process_cmnd_track_done(struct cmnd_track *track, s64 end_time)
{
	// TODO: Do something useful here
	kfree(track);
}

static void sniffer_scsi_done(struct scsi_cmnd *cmnd)
{
	s64 end_time;
	struct cmnd_track *track;
	void (*scsi_done)(struct scsi_cmnd *);
	unsigned long flags;

	end_time = get_current_time();

	printk(KERN_INFO "done cmd %p", cmnd);
	spin_lock_irqsave(&track_lock, flags);
	track = cmnd_track_from_cmnd(cmnd);
	list_del(&track->list);
	spin_unlock_irqrestore(&track_lock, flags);

	scsi_done = track->scsi_done;
	process_cmnd_track_done(track, end_time);
	if (scsi_done)
		scsi_done(cmnd);
}

static int sniffer_scsi_queuecommand(struct Scsi_Host *scsi_host, struct scsi_cmnd *cmnd)
{
	unsigned long flags;
	// TODO: Need to check if we really need GFP_ATOMIC here
	struct cmnd_track *track = kzalloc(sizeof(*track), GFP_ATOMIC);
	if (!track)
		return old_scsi_queuecommand(scsi_host, cmnd);

	printk(KERN_INFO "track cmd %p track %p", cmnd, track);
	track->start_time = get_current_time();
	track->cmnd = cmnd;
	track->scsi_done = cmnd->scsi_done;
	cmnd->scsi_done = sniffer_scsi_done;

	spin_lock_irqsave(&track_lock, flags);
	printk(KERN_INFO "list head %p item %p", &track_list, &track->list);
	list_add_tail(&track->list, &track_list);
	spin_unlock_irqrestore(&track_lock, flags);

	return old_scsi_queuecommand(scsi_host, cmnd);
}

/*
 * create_buf_file() callback.  Creates relay file in debugfs.
 */
static struct dentry *create_buf_file_handler(const char *filename,
                                              struct dentry *parent,
                                              umode_t mode,
                                              struct rchan_buf *buf,
                                              int *is_global)
{
        return debugfs_create_file(filename, mode, parent, buf,
                                   &relay_file_operations);
}

/*
 * remove_buf_file() callback.  Removes relay file from debugfs.
 */
static int remove_buf_file_handler(struct dentry *dentry)
{
        debugfs_remove(dentry);

        return 0;
}

/*
 * relay interface callbacks
 */
static struct rchan_callbacks relay_callbacks =
{
        .create_buf_file = create_buf_file_handler,
        .remove_buf_file = remove_buf_file_handler,
};


static int __init disk_sniffer_init(void)
{
	struct Scsi_Host *scsi_host;
	struct scsi_host_template *host_template;

	scsi_host = scsi_host_lookup(hostnum);
	if (!scsi_host) {
		printk(KERN_ERR "scsi_host_sniffer: failed to locate host %d\n", hostnum);
		return -ENODEV;
	}

	relay_chan = relay_open("scsi_host_sniffer", NULL, 1024*1024, 10, &relay_callbacks, NULL);
	if (!relay_chan) {
		printk(KERN_ERR "scsi_host_sniffer: failed to create relay channel\n");
		return -ENOMEM;
	}

	INIT_LIST_HEAD(&track_list);

	host_template = scsi_host->hostt;
	old_scsi_host_template = host_template;
	old_scsi_queuecommand = host_template->queuecommand;
	smp_mb();

	host_template->queuecommand = sniffer_scsi_queuecommand;
	printk(KERN_INFO "scsi_host_sniffer: installed for hostnum %d\n", hostnum);
	return 0;
}

static void __exit disk_sniffer_exit(void)
{
	struct Scsi_Host *scsi_host;
	struct scsi_host_template *host_template;

	scsi_host = scsi_host_lookup(hostnum);
	if (!scsi_host) {
		printk(KERN_ERR "scsi_host_sniffer: failed to locate host %d\n", hostnum);
		return;
	}

	host_template = scsi_host->hostt;

	if (host_template->queuecommand == sniffer_scsi_queuecommand) {
		host_template->queuecommand = old_scsi_queuecommand;
		printk(KERN_INFO "scsi_host_sniffer: hook removed\n");
	} else {
		printk(KERN_ERR "scsi_host_sniffer: unknown queuecommand function found, not doing anything\n");
	}

	/* make sure that all hooks exit prior to removal of the module */
	while (!list_empty(&track_list))
		msleep(HZ/10);

	relay_flush(relay_chan);
	relay_close(relay_chan);
}

module_init(disk_sniffer_init);
module_exit(disk_sniffer_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Baruch Even <baruch@ev-en.org>");
