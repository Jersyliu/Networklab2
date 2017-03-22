#include "gbn.h"

state_t s;

/*calculate checksum with buf*/
uint16_t checksum(uint16_t *buf, int nwords) {
	uint32_t sum;
	for (sum = 0; nwords > 0; nwords--)
		sum += *buf++;
	sum = (sum >> 16) + (sum & 0xffff);
	sum += (sum >> 16);
	return ~sum;
}
uint16_t get_checksum(gbnhdr *segment) {
	int nwords = sizeof(segment) / 2 - 1;
	return checksum((uint16_t *)segment + 1, nwords);
}

/*compare two sockaddr*/
int sockaddr_cmp(const struct sockaddr *x, const struct sockaddr *y) {
#define CMP(a, b) if (a != b) return a < b ? -1 : 1

    CMP(x->sa_family, y->sa_family);
    struct sockaddr_in *xin = (void*)x, *yin = (void*)y;
    CMP(ntohl(xin->sin_addr.s_addr), ntohl(yin->sin_addr.s_addr));
    CMP(ntohs(xin->sin_port), ntohs(yin->sin_port));
    
#undef CMP
    return 0;
}

/*make the buf and nwords, devide the whole segment into 16-bits a group*/
/*
uint16_t get_checksum(gbnhdr *segment) {
	int type_len, seq_len, data_len, segment_length, nwords, index, i;
	type_len = sizeof(segment->type);
	seq_len = sizeof(segment->seqnum);
	data_len = sizeof(segment->data);
	segment_length = type_len + seq_len + data_len;
	nwords = segment_length / sizeof(uint16_t);
	uint16_t buf[nwords];
	buf[0] = (((uint16_t)segment->type)<<8) + (uint16_t)segment->seqnum;
	for(i = 0; i < data_len; ++i) {
		index = i / 2 + 1;
		if (i % 2 == 0) {
			buf[index] = (uint16_t)segment->data[i];
		}
		else {
			buf[index] <<= 8;
			buf[index] += segment->data[i];
		}
	}
	return checksum(buf, nwords);
}
*/

/*generate segments*/
gbnhdr generate_hdr(uint8_t type, uint8_t seqnum, uint8_t *data) {
	gbnhdr segment;
	segment.type = type;
	segment.seqnum = seqnum;
	memset(segment.data, 0, DATALEN);
	if (data) {
		memcpy(segment.data, data, DATALEN);	
	}
	segment.checksum = get_checksum(&segment);
	return segment;
}
/*generate acks*/
gbnhdr generate_ack(gbnhdr segment, uint8_t *last_seqnum) {
	gbnhdr ack;
	ack.type = segment.type + 1;
	if (segment.checksum == get_checksum(&segment) && segment.seqnum == *last_seqnum + 1) {
		ack.seqnum = segment.seqnum;
		(*last_seqnum)++;
	} 
	else {
		ack.seqnum = *last_seqnum;
	}
	ack.checksum = get_checksum(&ack);
	return ack;
}
void timeout_handler(int signal_type) { 
	signal(SIGALRM, timeout_handler); 
	fprintf(stderr, "SIGALRM!!!\n"); 
}
void wait_to_close_handler(int signal_type) {
	s.state = CLOSED;
}

ssize_t gbn_send(int sockfd, const void *buf, size_t len, int flags){

	printf("\n\n\n==================== Enter gbn_send ====================\n");
	/* Hint: Check the data length field 'len'.
	 *       If it is > DATALEN, you will have to split the data
	 *       up into multiple packets - you don't have to worry
	 *       about getting more than N * DATALEN.
	 */
	signal(SIGALRM, timeout_handler);
	const void *progress = buf;
	uint8_t temp_buf[DATALEN];
	uint8_t last_seqnum_sent = s.last_seqnum;
	gbnhdr generated_packets[N];
	gbnhdr receiver = generate_hdr(0,0,NULL);
	uint8_t base = s.last_seqnum;
	ssize_t total_send = 0;
	int attemptions = 0;
	int rece_attemp = 0;
	struct sockaddr_in from;
	socklen_t fromlen = sizeof(from);
	int data_size;
	int last_segment = 0;
	int time_out = 0;

	socklen_t socklen = sizeof(struct sockaddr);

	/*printf("State Change: from ESTABLISHED to DATA_SENDING !!\n");*/
	s.state = DATA_SENDING;

	while (s.state != ESTABLISHED && s.state != RESET && s.state != CLOSED) {
		switch(s.state) {
			case DATA_SENDING:
				if (last_seqnum_sent == s.last_seqnum + s.windowsize) {
					printf("The window is full now, wait for DATAACK to move on.\n");
					/*printf("Stage Change: from DATA_SENDING to DATAACK_WAITING !!\n");*/
					s.state = DATAACK_WAITING;
					break;
				}
				/*if (generated_packets[last_seqnum_sent - base] == NULL) {*/
				if (progress >= buf + len - DATALEN && progress < buf + len) {
					printf("This is the last data segment.\n");
					data_size = (int)(uintptr_t)buf + len - (int)(uintptr_t)progress;
					last_segment = 1;
				}
				else {
					printf("This is a normal data segment.\n");
					data_size = DATALEN;
				}
				memset(temp_buf, 0, DATALEN);
				memcpy(temp_buf, progress, data_size);
				progress += data_size;
				gbnhdr temp = generate_hdr(DATA, last_seqnum_sent, temp_buf);
				generated_packets[last_seqnum_sent - base] = temp;
				/*}*/
				printf("Sending type: %d -- seqnum: %d\n", generated_packets[last_seqnum_sent - base].type, generated_packets[last_seqnum_sent - base].seqnum);
				printf("last_seqnum_sent: %d,     s.last_seqnum: %d\n", last_seqnum_sent, s.last_seqnum);
				int send_num = maybe_sendto(sockfd, &generated_packets[last_seqnum_sent - base], sizeof(gbnhdr), 0, (struct sockaddr *)s.server, socklen);
				if (send_num == -1) {
					printf("Send DATA fail, attemptions: %d !! %s\n", ++attemptions, strerror(errno));
					break;
				}
				else {
					attemptions = 0;
					printf("Send DATA succeed.\n");
					last_seqnum_sent++;
					total_send += send_num;
				}
				break;

			case DATAACK_WAITING:
				alarm(TIMEOUT);
				if (recvfrom(sockfd, &receiver, sizeof(gbnhdr), 0, (struct sockaddr *)&from, &fromlen) == -1) {
                    			/*didn't receive because of timeout or some other issue*/
                    			/*if timeout, try again*/
					printf("Receive DATAACK fail, attemptions: %d !! %s\n", ++rece_attemp, strerror(errno));
                    			if(errno != EINTR) {
                        			/*some problem other than timeout*/
                        			s.state = CLOSED;
						printf("Some problem with receive %s\n", strerror(errno));	
                    			}
					else { 
						printf("No DATAACK. DATA timeout, resent all DATA.\n"); 
						printf("Set mode to SLOW !!\n");
						s.windowsize = 1;
						int i;
						int a;
						time_out = 1;
						printf("last_seqnum_sent: %d,     s.last_seqnum: %d\n", last_seqnum_sent, s.last_seqnum);
						for (i = s.last_seqnum; i < last_seqnum_sent; ++i) {
							a = 0;
							while (a < 5) {
								printf("Sending type: %d -- seqnum: %d\n", generated_packets[i - base].type, generated_packets[i - base].seqnum);
								if (maybe_sendto(sockfd, &generated_packets[i - base], sizeof(gbnhdr), 0, (struct sockaddr *)s.server, socklen) == -1) {
									a++;
									printf("Send DATA fail !!\n");
									continue;
								}
								else {
									printf("Send DATA succeed.\n");
									break;
								}
							}
							if (a == 5) {
								printf("fail too many times");
								s.state = CLOSED;
								break;
							}
						}
						/*for (i = 0; i < N; ++i)	{ generated_packets[i] = NULL; }*/
					}
                		}
				else {
                    			printf("\nGot something...\n");
					if (sockaddr_cmp((struct sockaddr *)&from, (struct sockaddr *)s.server) != 0) { printf("Reject processing.\n"); s.state = RESET; break; }
                    			printf("type: %d -- seqnum: %d -- checksum: %d -- get_checksum: %d\n", receiver.type, receiver.seqnum, receiver.checksum, get_checksum(&receiver));
					rece_attemp = 0;
                    			if (receiver.type == DATAACK && receiver.checksum == get_checksum(&receiver)) {
						printf("Receive DATAACK. Cancel time out.\n");
						if (s.last_seqnum > receiver.seqnum) {
							break;
						}
						alarm(0);
						s.last_seqnum = receiver.seqnum + 1;
						if (last_segment == 1) {
							/*printf("State Change: from DATA_WAITING to ESTABLISHED !!\n");*/
							s.state = ESTABLISHED;	
						}
						else {
							/*printf("State Change: from DATA_WAITING to DATA_SENDING !!\n");*/
							s.state = DATA_SENDING;
						}
						if (time_out == 1) {
							time_out = 0;
							last_seqnum_sent = s.last_seqnum;
						}
						else {
							printf("Set mode to FAST !!\n");
							s.windowsize = 2;
						}
                    			}
                		}
				break;
			default:
				break;
		}
		if (attemptions > 5 || rece_attemp > 5) { break; }
	}
	printf("\n==================== Leave gbn_send ====================\n\n\n");
	return s.state == ESTABLISHED ? total_send : -1;
}

ssize_t gbn_recv(int sockfd, void *buf, size_t len, int flags) {
	printf("\n\n\n==================== Enter gbn_recv ====================\n");

	signal(SIGALRM, timeout_handler);
	gbnhdr receiver = generate_hdr(0,0,NULL);
	gbnhdr *ack_copy = NULL;
	int attemptions = 0;
	int rece_attemp = 0;
	struct sockaddr_in from;
	socklen_t fromlen = sizeof(from);
	ssize_t result;
	int outrange = 0;

	/*printf("State Change: from ESTABLISHED to DATA_PENDING !!\n");*/
	s.state = DATA_PENDING;

	while (s.state != ESTABLISHED && s.state != RESET) {
		printf("s.last_seqnum: %d\n", s.last_seqnum);
		switch(s.state) {
			case DATA_PENDING:
				alarm(TIMEOUT);	
				if (recvfrom(sockfd, &receiver, sizeof(gbnhdr), 0, (struct sockaddr *)&from, &fromlen) == -1) {
		    			/*didn't receive because of timeout or some other issue*/
		    			/*if timeout, try again*/
					printf("Receive DATA or FIN fail, attemptions: %d !! %s\n", ++rece_attemp, strerror(errno));
		    			if(errno != EINTR) {
		        			/*some problem other than timeout*/
		        			s.state = CLOSED;
						printf("Some problem with receive %s\n", strerror(errno));
		    			}
				}
				else {	
					printf("\nGot something...\n");
					if (sockaddr_cmp((struct sockaddr *)&from, (struct sockaddr *)s.client) != 0) { printf("Reject processing.\n"); s.state = RESET; break; }
					printf("type: %d -- seqnum: %d -- checksum: %d -- get_checksum: %d\n", receiver.type, receiver.seqnum, receiver.checksum, get_checksum(&receiver));
					rece_attemp = 0;
					s.state = DATA_RCVD;
					if (receiver.seqnum != s.last_seqnum + 1) { 
						outrange = 1;
						break; 
					}
					if (receiver.type != FIN && receiver.checksum == get_checksum(&receiver)) {
						alarm(0);
						memcpy(buf, receiver.data, len);
						int i = DATALEN - 1;
						while (i >= 0 && receiver.data[i] == 0) i--;
						result = i + 1;
					}
					else if (receiver.type == FIN && receiver.checksum == get_checksum(&receiver)) {
						result = 0;
					}
					/*printf("State Change: from DATA_PENDING to DATA_RCVD !!\n");*/
					/*s.state = DATA_RCVD;*/
				}
				break;
			case DATA_RCVD:
				if (ack_copy == NULL) {
					gbnhdr ack = generate_ack(receiver, &s.last_seqnum);
					ack_copy = &ack;
				}
				printf("Sending type: %d -- seqnum: %d\n", ack_copy->type, ack_copy->seqnum);
				if (maybe_sendto(sockfd, ack_copy, sizeof(gbnhdr), 0, (struct sockaddr *)s.client, *s.socklen) == -1) {
					printf("Send DATAACK or FINACK fail, attemptions: %d !! %s\n", ++attemptions, strerror(errno));
					break;
				}
				else {
					attemptions = 0;
					printf("Send DATAACK or FINACK succeed.\n");
					/*printf("State Change: from DATA_RCVD to ESABLISHED !!\n");*/
					if (outrange == 1) {
						outrange = 0;
						ack_copy = NULL;
						s.state	= DATA_PENDING;
					}
					else {
						s.state = ESTABLISHED;
					}
				}
				break;
			default:
				break;
		}
		if (attemptions > 5 || rece_attemp > 5) { break; }
	}
	printf("\n==================== Leave gbn_recv ====================\n\n\n");
	return s.state == ESTABLISHED ? result : -1;
}

/* 2 way handwave */
int gbn_close(int sockfd) {
	printf("\n\n\n==================== Enter gbn_close ====================\n");

	/*Create FIN segment*/
	gbnhdr fin = generate_hdr(FIN, s.last_seqnum, NULL);
	/*Create receiver segment*/
	gbnhdr receiver = generate_hdr(0, 0, NULL);
	int attemptions = 0;
	int rece_attemp = 0;
	
	struct sockaddr_in *address = s.server == NULL ? s.client : s.server;
	
	struct sockaddr_in from;
	socklen_t fromlen = sizeof(from);

	socklen_t socklen = sizeof(struct sockaddr);

	while (s.state != CLOSED && s.state != RESET) {
		switch(s.state) {
			case ESTABLISHED:/*send FIN, receive FINACK, state ESTABLISHED => FIN_WAIT*/
				if (maybe_sendto(sockfd, &fin, sizeof(gbnhdr), 0, (struct sockaddr *)address, socklen) == -1) {
					printf("Send FIN fail, attemptions: %d !! %s\n", ++attemptions, strerror(errno));
					break;
				}
				else {
					attemptions = 0;
					printf("Send FIN succeed. Waiting for FINACK. Time out: %d\n", TIMEOUT);
				}
				signal(SIGALRM, timeout_handler);
				alarm(TIMEOUT);
				if (recvfrom(sockfd, &receiver, sizeof(gbnhdr), 0, (struct sockaddr *)&from, &fromlen) == -1) {
                    			/*didn't receive because of timeout or some other issue*/
                    			/*if timeout, try again*/
					printf("Receive FINACK fail, attemptions: %d !! %s\n", ++rece_attemp, strerror(errno));
                    			if(errno != EINTR) {
                        			/*some problem other than timeout*/
                        			s.state = CLOSED;
						printf("Some problem with receive %s\n", strerror(errno));
                    			}
					else { printf("No FINACK. FIN timeout, resent FIN.\n"); }
                		}
				else {
                    			printf("\nGot something...\n");
					if (sockaddr_cmp((struct sockaddr *)&from, (struct sockaddr *)address) != 0) { printf("Reject processing.\n"); s.state = RESET; break; }
                    			printf("type: %d -- seqnum: %d -- checksum: %d -- get_checksum: %d\n", receiver.type, receiver.seqnum, receiver.checksum, get_checksum(&receiver));
					rece_attemp = 0;
                    			if (receiver.type == FINACK && receiver.checksum == get_checksum(&receiver)) {
						printf("Receive FINACK. Cancel time out.\n");
						alarm(0);
                        			s.state = FIN_WAIT;
						s.last_seqnum = receiver.seqnum + 1;
						printf("State Change: from FIN_SENT to FIN_WAIT !!\n");
                    			}
                		}
				break;

			case FIN_WAIT:/*receive FIN, state FIN_WAIT => CLOSED*/
				signal(SIGALRM, wait_to_close_handler);
				alarm(2*TIMEOUT);
				if (recvfrom(sockfd, &receiver, sizeof(gbnhdr), 0, (struct sockaddr *)&from, &fromlen) == -1) {
                    			/*didn't receive because of timeout or some other issue*/
                    			/*if timeout, try again*/
					printf("Receive FIN fail, attemptions: %d !! %s\n", ++rece_attemp, strerror(errno));
                    			if(errno != EINTR) {
                        			/*some problem other than timeout*/
						printf("Some problem with receive %s\n", strerror(errno));
                    			}
					else { 
						printf("FIN_WAIT Time out. State Change: from FIN_WAIT to CLOSED !!\n");
					}
					s.state = CLOSED;
                		}
				else {
                    			printf("\nGot something...\n");
					if (sockaddr_cmp((struct sockaddr *)&from, (struct sockaddr *)address) != 0) { printf("Reject processing.\n"); s.state = RESET; break; }
                    			printf("type: %d -- seqnum: %d -- checksum: %d -- get_checksum: %d\n", receiver.type, receiver.seqnum, receiver.checksum, get_checksum(&receiver));
					rece_attemp = 0;
                    			if (receiver.type == FIN && receiver.checksum == get_checksum(&receiver)) {
						alarm(0);
                        			s.state = CLOSED;
						printf("State Change: from FIN_WAIT to CLOSED !!\n");
						gbnhdr finack = generate_ack(receiver, &s.last_seqnum);
						if (maybe_sendto(sockfd, &finack, sizeof(gbnhdr), 0, (struct sockaddr *)address, socklen) == -1) {
							printf("Send FINACK fail, attemptions: %d !! %s\n", ++attemptions, strerror(errno));
						}
						else {
							address = NULL;
							s.socklen = NULL;
							attemptions = 0;
							printf("==========Close Socket=========== !!");
						}
                    			}
                		}
				break;
			default:
				break;
		}
		if (attemptions > 5 || rece_attemp > 5) { break; }
	}
	printf("\n==================== Leave gbn_close ====================\n\n\n");
	return s.state == CLOSED ? close(sockfd) : -1;
}

/*3-way handshake client side*/
int gbn_connect(int sockfd, const struct sockaddr *server, socklen_t socklen) {
	printf("\n\n\n==================== Enter gbn_connect ====================\n");
	signal(SIGALRM, timeout_handler);
	s.last_seqnum = 1;

	/*Create SYN segment*/
	gbnhdr syn = generate_hdr(SYN, 1, NULL);

	/*Create the RECIVER segment for third handshake*/
	gbnhdr receiver = generate_hdr(0,0,NULL);

	gbnhdr *dataack_copy = NULL;
	
	struct sockaddr_in from;
	socklen_t fromlen = sizeof(from);

	int attemptions = 0;
	int rece_attemp = 0;
	s.state = CLOSED;
	while (s.state != ESTABLISHED && s.state != RESET) {
		switch(s.state) {
			case CLOSED:/*send SYN, state CLOSED => SYN_SENT*/
				if (maybe_sendto(sockfd, &syn, sizeof(gbnhdr), 0, (struct sockaddr *)server, socklen) == -1) {
					printf("Send SYN fail, attemptions: %d !! %s\n", ++attemptions, strerror(errno));
					break;
				}
				else {
					attemptions = 0;
					printf("Send SYN succeed. Waiting for SYNACK. Time out: %d\n", TIMEOUT);
				}
				alarm(TIMEOUT);
				if (recvfrom(sockfd, &receiver, sizeof(gbnhdr), 0, (struct sockaddr *)&from, &fromlen) == -1) {
                    			/*didn't receive because of timeout or some other issue*/
                    			/*if timeout, try again*/
					printf("Receive SYNACK fail, attemptions: %d !! %s\n", ++rece_attemp, strerror(errno));
                    			if(errno != EINTR) {
                        			/*some problem other than timeout*/
                        			s.state = CLOSED;
						printf("Some problem with receive %s\n", strerror(errno));	
                    			}
					else { printf("No SYNACK. SYN timeout, resent SYN.\n"); }
                		}
				else {
                    			printf("\nGot something...\n");
					if (sockaddr_cmp((struct sockaddr *)&from, (struct sockaddr *)server) != 0) { printf("Reject processing.\n"); s.state = RESET; break; }
                    			printf("type: %d -- seqnum: %d -- checksum: %d -- get_checksum: %d\n", receiver.type, receiver.seqnum, receiver.checksum, get_checksum(&receiver));
					rece_attemp = 0;
                    			if (receiver.type == SYNACK && receiver.checksum == get_checksum(&receiver)) {
						printf("Receive SYNACK. Cancel time out.\n");
						alarm(0);
                        			s.state = SYNACK_SENT;
						s.last_seqnum = receiver.seqnum + 1;
						printf("State Change: from CLOSED to SYNACK_SENT !!\n");
                    			}
                		}
				break;

			case SYNACK_SENT:/*send DATAACK, state SYNACK_SENT => ESTABLISHED*/
				if (dataack_copy == NULL) {
					gbnhdr dataack = generate_hdr(DATAACK, s.last_seqnum, NULL);
					s.last_seqnum++;
					dataack_copy = &dataack;
				}
				if (maybe_sendto(sockfd, dataack_copy, sizeof(gbnhdr), 0, (struct sockaddr *)server, socklen) == -1) {
					printf("Send DATAACK fail, attemptions: %d !! %s\n", ++attemptions, strerror(errno));
					break;
				}
				printf("Send DATAACK succeed.\n");
				s.state = ESTABLISHED;
				printf("State Change: from SYNACK_SENT to ESTABLISHED !!\n");
				s.server = (struct sockaddr_in *)server;
				s.client = NULL;
				s.socklen = &socklen;
				break;
			default:
				break;
		}
		if (attemptions > 5 || rece_attemp > 5)
			break;
	}
	printf("\n==================== Leave gbn_connect ====================\n\n\n");
	return s.state == ESTABLISHED ? 0 : -1;
}

/*DONE!!*/
int gbn_listen(int sockfd, int backlog) {
	printf("\n\n\n==================== Enter gbn_listen ====================\n");
	s.last_seqnum = 0;
	s.state = CLOSED;
	s.server = NULL;
	printf("\n==================== Leave gbn_listen ====================\n\n\n");
	return 0;
}
/*DONE!!*/
int gbn_bind(int sockfd, const struct sockaddr *server, socklen_t socklen){
	printf("\n\n\n==================== Enter gbn_bind ====================\n");
	printf("\n==================== Leave gbn_bind ====================\n\n\n");
	return bind(sockfd, server, socklen);
}	

/*DONE!!*/
int gbn_socket(int domain, int type, int protocol){
	printf("\n\n\n==================== Enter gbn_socket ====================\n");
		
	/*----- Randomizing the seed. This is used by the rand() function -----*/
	srand((unsigned)time(0));
	
	/*initial the state_t s	*/
	s = *(state_t *)malloc(sizeof(state_t));
	s.windowsize = 1;
	printf("\n==================== Leave gbn_socket ====================\n\n\n");
	return socket(domain, type, protocol);
}


/*3-way handshake server side*/
int gbn_accept(int sockfd, struct sockaddr *client, socklen_t *socklen) {
	printf("\n\n\n==================== Enter gbn_accept ====================\n");
	signal(SIGALRM, timeout_handler);
	/*Create the RECIVER segment for third handshake*/
	gbnhdr receiver = generate_hdr(0,0,NULL);

	gbnhdr *synack_copy = NULL;

	int attemptions = 0;
	int rece_attemp = 0;

	while (s.state != ESTABLISHED && s.state != RESET) {
		switch(s.state) {
			case CLOSED:/*receive SYN, state CLOSED => SYN_RCVD*/
				if (recvfrom(sockfd, &receiver, sizeof(gbnhdr), 0, (struct sockaddr *)client, socklen) == -1) {
                    			/*didn't receive because of timeout or some other issue*/
                    			/*if timeout, try again*/
					printf("Receive SYN fail, attemptions: %d !! %s\n", ++rece_attemp, strerror(errno));
                    			if(errno != EINTR) {
                        			/*some problem other than timeout*/
                        			s.state = CLOSED;
						printf("Some problem with receive %s\n", strerror(errno));		
                    			}
                		}
				else {
                    			printf("\nGot something...\n");
                    			printf("type: %d -- seqnum: %d -- checksum: %d -- get_checksum: %d\n", receiver.type, receiver.seqnum, receiver.checksum, get_checksum(&receiver));
					rece_attemp = 0;
                    			if (receiver.type == SYN && receiver.checksum == get_checksum(&receiver)) {
						printf("Receive SYN. Ready to send SYNACK.\n");
                        			s.state = SYN_RCVD;
						attemptions = 0;
						printf("State Change: from CLOSED to SYN_RCVD !!\n");
                    			}
					else if (receiver.type == RESET && receiver.checksum == get_checksum(&receiver)) {
						s.state = RESET;
						printf("State Change: from SYN_SENT to RESET !!\n");
					}
                		}	
				break;

			case SYN_RCVD:/*send SYNACK, state SYN_RCVD => SYNACK_SENT*/
				if (synack_copy == NULL) {
					gbnhdr synack = generate_ack(receiver, &s.last_seqnum);
					synack_copy = &synack;
				}
				if (maybe_sendto(sockfd, synack_copy, sizeof(gbnhdr), 0, (struct sockaddr *)client, *socklen) == -1) {
					printf("Send SYNACK fail, attemptions: %d !! %s\n", ++attemptions, strerror(errno));
					break;
				}
				else {
					attemptions = 0;
					printf("Send SYNACK succeed. Waiting for DATAACK. Time out: %d\n", TIMEOUT);
				}
				alarm(TIMEOUT);
				if (recvfrom(sockfd, &receiver, sizeof(gbnhdr), 0, (struct sockaddr *)client, socklen) == -1) {
                    			/* didn't receive because of timeout or some other issue*/
                    			/* if timeout, try again*/
					printf("Receive DATAACK fail, attemptions: %d !! %s\n", ++rece_attemp, strerror(errno));
                    			if(errno != EINTR) {
                        			/* some problem other than timeout*/
                        			s.state = CLOSED;
						printf("Some problem with receive %s\n", strerror(errno));		
                    			}
					else { printf("No DATAACK. SYNACK timeout, resent SYNACK.\n"); }
                		}
				else {
                    			printf("\nGot something...\n");
                    			printf("type: %d -- seqnum: %d -- checksum: %d -- get_checksum: %d\n", receiver.type, receiver.seqnum, receiver.checksum, get_checksum(&receiver));
					rece_attemp = 0;
                    			if (receiver.type == DATAACK && receiver.checksum == get_checksum(&receiver)) {	
						printf("Receive DATAACK. Cancel time out.\n");
						alarm(0);
                        			s.state = ESTABLISHED;
						s.last_seqnum = receiver.seqnum;
						s.client = (struct sockaddr_in *)client;
						s.socklen = socklen;
						printf("State Change: from SYNACK_RCVD to ESTABLISHED !!\n");
                    			}
					else { s.state = RESET; }
                		}	
				break;
			default:
				break;
			
		}
		if (attemptions > 5 || rece_attemp > 5) { break; }
	}
	printf("\n==================== Leave gbn_accept ====================\n\n\n");
	return s.state == ESTABLISHED ? sockfd : -1;
}


ssize_t maybe_sendto(int  s, const void *buf, size_t len, int flags, \
                     const struct sockaddr *to, socklen_t tolen) {

	char *buffer = malloc(len);
	memcpy(buffer, buf, len);
	
	
	/*----- Packet not lost -----*/
	if (rand() > LOSS_PROB*RAND_MAX){
		/*----- Packet corrupted -----*/
		if (rand() < CORR_PROB*RAND_MAX){
			
			/*----- Selecting a random byte inside the packet -----*/
			int index = (int)((len-1)*rand()/(RAND_MAX + 1.0));

			/*----- Inverting a bit -----*/
			char c = buffer[index];
			if (c & 0x01)
				c &= 0xFE;
			else
				c |= 0x01;
			buffer[index] = c;
		}

		/*----- Sending the packet -----*/
		int retval = sendto(s, buffer, len, flags, to, tolen);
		free(buffer);
		return retval;
	}
	/*----- Packet lost -----*/
	else
		return(len);  /* Simulate a success */
}
