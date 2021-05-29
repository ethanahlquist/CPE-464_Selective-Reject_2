#ifndef __WINDOW_H__
#define __WINDOW_H__

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>


#define MAX_NAME (16)

void win_print_index(uint32_t index);
void win_print();
uint32_t win_index(uint32_t seq_num);
uint8_t * win_get(uint32_t seq_num);
int win_dist(uint32_t i1, uint32_t i2);
void win_set_lower(uint32_t val);
uint32_t win_getLower();
int win_isFull();
int win_skipping_enQueue(uint8_t * pdu, uint16_t pduSize);

int32_t win_add(uint8_t *pdu, uint16_t pduSize);
/* win_add() return flags */
#define TOO_LOW (-1)
#define TOO_HIGH (-2)
#define FULL_CELL (-3)

int win_oneElement();
int win_isEmpty();
uint8_t win_deQueue(uint8_t * buf);
void win_RR(uint32_t rr);
void win_SREJ(uint32_t srej);
#endif
