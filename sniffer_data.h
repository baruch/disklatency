#ifndef SNIFFER_DATA_H
#define SNIFFER_DATA_H

struct sniffer_data {
	int64_t ts;
	unsigned long queue_time_usec;
	unsigned id;
	uint8_t type; // 0=Request, 1=Response (future would be abort task, target reset)
	char data[16]; // CDB or Sense
};

#define SNIFFER_DATA_TYPE_SUBMIT 0
#define SNIFFER_DATA_TYPE_RESPONSE 1

#endif
