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
#include <linux/jiffies.h>

#include "sniffer_data.h"

// TODO: take refcount of module we attach to to prevent incorrect order of module removals

static struct rchan *relay_chan;
static unsigned last_cmd_id;

static int hostnum = -1;
module_param(hostnum, int, 0644);

static int sniffer_enabled = 0;
module_param(sniffer_enabled, int, 0644);

struct cmnd_track {
	struct list_head list;
	struct scsi_cmnd *cmnd;
	void (*scsi_done)(struct scsi_cmnd *);
	unsigned id;
	s64 start_time;
};

#define NUM_HOSTS 4
typedef int (*queuecommand_cb_t)(struct Scsi_Host *scsi_host, struct scsi_cmnd *cmnd);
struct host_info {
	struct Scsi_Host *scsi_host;
	struct scsi_host_template *scsi_host_template;
	int host_num;

	queuecommand_cb_t queuecommand;
};

static DEFINE_SPINLOCK(track_lock);
static LIST_HEAD(track_list);
static struct host_info host_infos[NUM_HOSTS];

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
	if (sniffer_enabled) {
		struct sniffer_data data;

		data.ts = end_time;
		data.id = track->id;
		data.type = SNIFFER_DATA_TYPE_RESPONSE;
		data.queue_time_usec = 0;

		if (track->cmnd->sense_buffer) {
			memcpy(data.data, track->cmnd->sense_buffer, sizeof(data.data)); // TODO: Find how to know sense buffer size
		} else {
			memset(data.data, 0, sizeof(data.data));
		}

		relay_write(relay_chan, &data, sizeof(data));
	}

	kfree(track);
}

static void sniffer_scsi_done(struct scsi_cmnd *cmnd)
{
	s64 end_time;
	struct cmnd_track *track;
	void (*scsi_done)(struct scsi_cmnd *);
	unsigned long flags;

	end_time = get_current_time();

	spin_lock_irqsave(&track_lock, flags);
	track = cmnd_track_from_cmnd(cmnd);
	list_del(&track->list);
	spin_unlock_irqrestore(&track_lock, flags);

	scsi_done = track->scsi_done;
	process_cmnd_track_done(track, end_time);
	if (scsi_done)
		scsi_done(cmnd);
}

static int sniffer_scsi_queuecommand(struct Scsi_Host *scsi_host, struct scsi_cmnd *cmnd, int host_idx)
{
	unsigned long flags;
	int ret;

	// TODO: Need to check if we really need GFP_ATOMIC here
	struct cmnd_track *track = kzalloc(sizeof(*track), GFP_ATOMIC);
	if (!track)
		return host_infos[host_idx].queuecommand(scsi_host, cmnd);

	track->cmnd = cmnd;
	track->scsi_done = cmnd->scsi_done;
	cmnd->scsi_done = sniffer_scsi_done;

	spin_lock_irqsave(&track_lock, flags);
	track->id = last_cmd_id++;
	list_add_tail(&track->list, &track_list);
	spin_unlock_irqrestore(&track_lock, flags);

	track->start_time = get_current_time();
	ret = host_infos[host_idx].queuecommand(scsi_host, cmnd);

	if (sniffer_enabled) {
		struct sniffer_data data;
		data.ts = track->start_time;
		data.queue_time_usec = jiffies_to_usecs(jiffies - track->cmnd->jiffies_at_alloc);
		data.id = track->id;
		data.type = SNIFFER_DATA_TYPE_SUBMIT;
		if (track->cmnd->cmd_len < 16) {
			memcpy(data.data, track->cmnd->cmnd, track->cmnd->cmd_len);
			memset(data.data + track->cmnd->cmd_len, 0, 16 - track->cmnd->cmd_len);
		} else {
			memcpy(data.data, track->cmnd->cmnd, 16);
		}
		relay_write(relay_chan, &data, sizeof(data));
	}

	return ret;
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

static struct host_info *find_scsi_host(struct Scsi_Host *host)
{
	struct host_info *info;
	int i;

	for (i = 0; i < NUM_HOSTS; i++) {
		info = &host_infos[i];
		if (info->scsi_host == host)
			return info;
	}

	return NULL;
}

#define QUEUECOMMAND_LIST() \
	QUEUECOMMAND(0) \
	QUEUECOMMAND(1) \
	QUEUECOMMAND(2) \
	QUEUECOMMAND(3)


#define QUEUECOMMAND(idx) \
	static int host_queuecmd_##idx(struct Scsi_Host *scsi_host, struct scsi_cmnd *cmnd) \
	{ \
		static int logged; if (unlikely(!logged)) { printk(KERN_INFO "first logged cmd on host %d\n", idx); logged = 1; } \
		return sniffer_scsi_queuecommand(scsi_host, cmnd, idx); \
	}
QUEUECOMMAND_LIST()
#undef QUEUECOMMAND

static queuecommand_cb_t assign_host_info(struct Scsi_Host *host, struct host_info **host_info_ptr)
{
	struct host_info *info;
	int i;

	for (i = 0; i < NUM_HOSTS; i++) {
		info = &host_infos[i];
		if (info->scsi_host == NULL) {
			info->scsi_host = host;
			info->scsi_host_template = host->hostt;
			info->queuecommand = host->hostt->queuecommand;
			*host_info_ptr = info;
			smp_mb();

#define QUEUECOMMAND(idx) case idx: return host_queuecmd_##idx;
			switch (i) {
				QUEUECOMMAND_LIST()
			}
#undef QUEUECOMMAND
			return NULL;
		}
	}

	return NULL;
}

static int attach_host(int hostnum)
{
	struct Scsi_Host *scsi_host;
	struct host_info *host_info;
	queuecommand_cb_t queuecommand;

	scsi_host = scsi_host_lookup(hostnum);
	if (!scsi_host) {
		return -ENODEV;
	}

	host_info = find_scsi_host(scsi_host);
	if (host_info)
		return -ENODEV;

	queuecommand = assign_host_info(scsi_host, &host_info);
	if (!host_info->queuecommand) {
		printk(KERN_ERR "scsi_host_sniffer: failed to install sniffer on host %d\n", hostnum);
		return -ENOSPC;
	}

	host_info->host_num = hostnum;
	scsi_host->hostt->queuecommand = queuecommand;
	printk(KERN_INFO "scsi_host_sniffer: installed for hostnum %d\n", hostnum);
	return 0;
}

static void attach_all_hosts(void)
{
	int hostnum;
	for (hostnum = 0; hostnum < 16; hostnum++) {
		attach_host(hostnum);
	}
}

static int __init disk_sniffer_init(void)
{
	relay_chan = relay_open("scsi_host_sniffer", NULL, 1024*1024, 10, &relay_callbacks, NULL);
	if (!relay_chan) {
		printk(KERN_ERR "scsi_host_sniffer: failed to create relay channel\n");
		return -ENOMEM;
	}

	INIT_LIST_HEAD(&track_list);

	if (hostnum >= 0) {
		int ret = attach_host(hostnum);
		if (!ret) {
			printk(KERN_ERR "scsi_host_sniffer: failed to locate host %d\n", hostnum);
			relay_close(relay_chan);
			return -ENODEV;
		}
	} else {
		attach_all_hosts();
	}

	__module_get(THIS_MODULE);

	return 0;
}

static void __exit disk_sniffer_exit(void)
{
	BUG();
}

module_init(disk_sniffer_init);
module_exit(disk_sniffer_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Baruch Even <baruch@ev-en.org>");
