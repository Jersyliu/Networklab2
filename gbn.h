#ifndef _gbn_h
#define _gbn_h

#include<sys/types.h>
#include<sys/socket.h>
#include<sys/ioctl.h>
#include<signal.h>
#include<unistd.h>
#include<fcntl.h>
#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<netinet/in.h>
#include<errno.h>
#include<netdb.h>
#include<time.h>

/*----- Error variables -----*/
extern int h_errno;
extern int errno;

/*----- Protocol parameters -----*/
#define LOSS_PROB 0.2    /* loss probability                            */
#define CORR_PROB 0.2    /* corruption probability                      */
#define DATALEN   1024    /* length of the payload                       */
#define N         1024    /* Max number of packets a single call to gbn_send can process */
#define TIMEOUT      1    /* timeout to resend packets (1 second)        */

/*----- Packet types -----*/
#define SYN      0        /* Opens a connection                          */
#define SYNACK   1        /* Acknowledgement of the SYN packet           */
#define DATA     2        /* Data packets                                */
#define DATAACK  3        /* Acknowledgement of the DATA packet          */
#define FIN      4        /* Ends a connection                           */
#define FINACK   5        /* Acknowledgement of the FIN packet           */
#define RST      6        /* Reset packet used to reject new connections */

/*----- Go-Back-n packet format -----*/
typedef struct {
	uint16_t checksum;        /* header and payload checksum                */
	uint8_t type;            /* packet type (e.g. SYN, DATA, ACK, FIN)     */
	uint8_t seqnum;          /* sequence number of the packet              */
	uint8_t data[DATALEN];    /* pointer to the payload                     */
} __attribute__((packed)) gbnhdr;

typedef struct state_t{

	/* TODO: Your state information could be encoded here. */
	int state;
	uint8_t last_seqnum;
	uint8_t windowsize;
	struct sockaddr_in *server;
	struct sockaddr_in *client;
	socklen_t *socklen;

} state_t;

enum {
	CLOSED=0,
	SYN_SENT,
	SYNACK_SENT,
	SYN_RCVD,
	SYNACK_RCVD,
	ESTABLISHED,
	FIN_SENT,
	FINACK_SENT,
	FIN_RCVD,
	FINACK_RCVD,
	FIN_WAIT,
	RESET,
	DATA_PENDING,
	DATA_RCVD,
	DATA_SENDING,
	DATAACK_WAITING
};

extern state_t s;

void gbn_init();
int gbn_connect(int sockfd, const struct sockaddr *server, socklen_t socklen);
int gbn_listen(int sockfd, int backlog);
int gbn_bind(int sockfd, const struct sockaddr *server, socklen_t socklen);
int gbn_socket(int domain, int type, int protocol);
int gbn_accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
int gbn_close(int sockfd);
ssize_t gbn_send(int sockfd, const void *buf, size_t len, int flags);
ssize_t gbn_recv(int sockfd, void *buf, size_t len, int flags);
ssize_t  maybe_sendto(int  s, const void *buf, size_t len, int flags, const struct sockaddr *to, socklen_t tolen);
uint16_t checksum(uint16_t *buf, int nwords);

uint16_t get_checksum(gbnhdr *segment);
void signal_handler(int signal);
gbnhdr generate_hdr(uint8_t type, uint8_t seqnum, uint8_t *data);
gbnhdr generate_ack(gbnhdr segment, uint8_t *last_seqnum);
#endif
