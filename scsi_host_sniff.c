#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>

#include "sniffer_data.h"

#define BUF_SIZE (16*1024)

static int usage(char *name)
{
	fprintf(stderr, "Usage:\n"
			"  %s <filename>\n"
			"  f.ex. %s /sys/kernel/debug/scsi_host_sniffer0\n"
			, name, name);
	return 1;
}

static void print_sniff_data(struct sniffer_data *sniff)
{
	printf("%ld %lu %u %u %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n", sniff->ts, sniff->queue_time_usec, sniff->id, sniff->type, sniff->data[0], sniff->data[1], sniff->data[2], sniff->data[3], sniff->data[4], sniff->data[5], sniff->data[6], sniff->data[7], sniff->data[8], sniff->data[9]);
}

static void process_buffer(char *buf, ssize_t len)
{
	struct sniffer_data *sniff = (struct sniffer_data *)buf;

	while (((char*)sniff) - buf < len) {
		print_sniff_data(sniff);
		sniff++;
	}
}

static void process_data(char *filename)
{
	int fd;
	char buf[BUF_SIZE];

	fd = open(filename, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "Failed to open sniffer file %s\n", filename);
		return;
	}

	while (1) {
		ssize_t read_bytes = read(fd, buf, (BUF_SIZE / sizeof(struct sniffer_data)) * sizeof(struct sniffer_data));
		if (read_bytes < 0) {
			if (errno == EINTR)
				continue;
			else
				break;
		}

		process_buffer(buf, read_bytes);
	}

	close(fd);
}

void handle_sigint(int signum)
{
	(void)signum; // unused
	fflush(stdout);
}

int main(int argc, char **argv)
{
	if (argc != 2)
		return usage(argv[0]);

	signal(SIGINT, handle_sigint);
	process_data(argv[1]);

	return 0;
}
