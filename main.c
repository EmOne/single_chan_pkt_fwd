/*******************************************************************************
 *
 * Copyright (c) 2015 Thomas Telkamp
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * which accompanies this distribution, and is available at
 * http://www.eclipse.org/legal/epl-v10.html
 *******************************************************************************/
/*
	Maintained by Anol Paisal <anol.p@emone.co.th>
*/
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netdb.h>

#include <errno.h>

#include <signal.h>         /* sigaction */
#include <pthread.h>

#include "loragw_aux.h"
#include "loragw_hal.h"
#include "jitqueue.h"
#include "base64.h"
#include "parson.h"
#include "trace.h"

#include <wiringPi.h>
#include <wiringSerial.h>

#define ARRAY_SIZE(a)   (sizeof(a) / sizeof((a)[0]))
#define STRINGIFY(x)	#x
#define STR(x)	STRINGIFY(x)

typedef bool boolean;
typedef unsigned char byte;

extern int errno;

//static const int CHANNEL = 0;

byte currentMode = 0x81;

char message[256];
char b64[256];

bool sx1272 = true;

byte receivedbytes;

struct sockaddr_in si_other;
int s, slen=sizeof(si_other);
//struct ifreq ifr;

uint32_t cp_nb_rx_rcv;
uint32_t cp_nb_rx_ok;
uint32_t cp_nb_rx_bad;
uint32_t cp_nb_rx_nocrc;
uint32_t cp_up_pkt_fwd;
uint32_t cp_up_network_byte;
uint32_t cp_up_payload_byte;
uint32_t cp_up_dgram_sent;
uint32_t cp_up_ack_rcv;
uint32_t cp_dw_pull_sent;
uint32_t cp_dw_ack_rcv;
uint32_t cp_dw_dgram_rcv;
uint32_t cp_dw_network_byte;
uint32_t cp_dw_payload_byte;
uint32_t cp_nb_tx_ok;
uint32_t cp_nb_tx_fail;
uint32_t cp_nb_tx_requested = 0;

time_t t;
char stat_timestamp[24];
float rx_ok_ratio;
float rx_bad_ratio;
float rx_nocrc_ratio;
float up_ack_ratio;
float dw_ack_ratio;

enum sf_t { SF7=7, SF8, SF9, SF10, SF11, SF12 };

/*******************************************************************************
 *
 * Configure these values!
 *
 *******************************************************************************/

// SX1272 - Raspberry connections
int ssPin = 6;
int dio0  = 7;
int RST   = 0;

// Set spreading factor (SF7 - SF12)
//sf_t sf = SF7;

// Set center frequency
uint32_t  freq = 923200000; // in Mhz! (868.1)

// Set location
float lat=100.0;
float lon=13.0;
int   alt=1;

/* Informal status fields */
//static char platform[24]    = "Single Channel Gateway";  /* platform definition */
//static char email[40]       = "";                        /* used for contact email */
//static char description[64] = "";                        /* used for free form description */

// define servers
#define PUSH_TIMEOUT_MS 100
#define PULL_TIMEOUT_MS 200

static struct timeval push_timeout_half = {0, (PUSH_TIMEOUT_MS * 500)};
static struct timeval pull_timeout_half = {0, (PULL_TIMEOUT_MS * 1000)};

// TODO: use host names and dns
#define SERVER1 "localhost"    // The Things Network: croft.thethings.girovito.nl
//#define SERVER1 "127.0.0.1"    // The Things Network: croft.thethings.girovito.nl
//#define SERVER2 "192.168.1.10"      // local
#define PORT 1680                   // The port on which to send data
// network configuration variablrs 
#define gateway_id  "B827EBFFFF3658DA" // lora gateway mac address
//#define gateway_id  "AABBCCDD00112233" // lora gateway mac address

#define FETCH_SLEEP_MS      10          /* nb of ms waited when a fetch return no packets */
#define DEFAULT_STAT        30          /* default time interval for statistics */
#define DEFAULT_KEEPALIVE   5           /* default time interval for downstream keep-alive packet */

static uint64_t lgwm = 0; // lora gateway mac address

static char serv_addr[64] = STR(SERVER1);
static char serv_port_up[8] =STR(PORT);
static char serv_port_down[8] =STR(PORT);

static int keepalive_time = DEFAULT_KEEPALIVE; /* send a PULL_DATA request every X seconds, negative = disabled */

static uint32_t net_mac_h; //MSN network order
static uint32_t net_mac_l; //LSN network order

static int sock_up; //socket for upstream
static int sock_down; //socket for upstream

static struct jit_queue_s jit_queue;

/* signal handling variables */
volatile bool exit_sig = false; /* 1 -> application terminates cleanly (shut down hardware, close open files, etc) */
volatile bool quit_sig = false; /* 1 -> application terminates without shutting down the hardware */

static void sig_handler(int sigio);

static uint16_t crc16(const uint8_t * data, unsigned size);

static double difftimespec(struct timespec end, struct timespec beginning);

static int send_tx_ack(uint8_t token_h, uint8_t token_l, enum jit_error_e error) ;

int PacketValidate(uint8_t * data, uint16_t* len);

void thread_up(void);
void thread_down(void);
void thread_jit(void);
// #############################################
// #############################################
static int fd;

typedef enum
{
    LOWPOWER,
    RX,
    RX_TIMEOUT,
    RX_ERROR,
    TX,
    TX_TIMEOUT,
    PING,
} States_t;

typedef enum
{
    DOWNLINK,
    FREQ,
    SF,
    PWR,
    PINGPONG,
} Request_t;

#define BUFLEN 2048  //Max length of buffer

#define PROTOCOL_VERSION  1
#define PKT_PUSH_DATA 0
#define PKT_PUSH_ACK  1
#define PKT_PULL_DATA 2
#define PKT_PULL_RESP 3
#define PKT_PULL_ACK  4
#define PKT_TX_ACK    5

#define NB_PKT_MAX      1 /* max number of packets per fetch/send cycle */

#define MIN_LORA_PREAMB 6 /* minimum Lora preamble length for this application */
#define STD_LORA_PREAMB 8

#define STATUS_SIZE	  200
//#define TX_BUFF_SIZE  2048
#define TX_BUFF_SIZE    ((540 * NB_PKT_MAX) + 30 + STATUS_SIZE)

/* statistics collection configuration variables */
static unsigned stat_interval = DEFAULT_STAT; /* time interval (in sec) at which statistics are collected and displayed */

/* hardware access control and correction */
pthread_mutex_t mx_concent = PTHREAD_MUTEX_INITIALIZER; /* control access to the concentrator */

/* measurements to establish statistics */
static pthread_mutex_t mx_meas_up = PTHREAD_MUTEX_INITIALIZER; /* control access to the upstream measurements */
static uint32_t meas_nb_rx_rcv = 0; /* count packets received */
static uint32_t meas_nb_rx_ok = 0; /* count packets received with PAYLOAD CRC OK */
static uint32_t meas_nb_rx_bad = 0; /* count packets received with PAYLOAD CRC ERROR */
static uint32_t meas_nb_rx_nocrc = 0; /* count packets received with NO PAYLOAD CRC */
static uint32_t meas_up_pkt_fwd = 0; /* number of radio packet forwarded to the server */
static uint32_t meas_up_network_byte = 0; /* sum of UDP bytes sent for upstream traffic */
static uint32_t meas_up_payload_byte = 0; /* sum of radio payload bytes sent for upstream traffic */
static uint32_t meas_up_dgram_sent = 0; /* number of datagrams sent for upstream traffic */
static uint32_t meas_up_ack_rcv = 0; /* number of datagrams acknowledged for upstream traffic */

static pthread_mutex_t mx_meas_dw = PTHREAD_MUTEX_INITIALIZER; /* control access to the downstream measurements */
static uint32_t meas_dw_pull_sent = 0; /* number of PULL requests sent for downstream traffic */
static uint32_t meas_dw_ack_rcv = 0; /* number of PULL requests acknowledged for downstream traffic */
static uint32_t meas_dw_dgram_rcv = 0; /* count PULL response packets received for downstream traffic */
static uint32_t meas_dw_network_byte = 0; /* sum of UDP bytes sent for upstream traffic */
static uint32_t meas_dw_payload_byte = 0; /* sum of radio payload bytes sent for upstream traffic */
static uint32_t meas_nb_tx_ok = 0; /* count packets emitted successfully */
static uint32_t meas_nb_tx_fail = 0; /* count packets were TX failed for other reasons */
static uint32_t meas_nb_tx_requested = 0; /* count TX request from server (downlinks) */

static pthread_mutex_t mx_stat_rep = PTHREAD_MUTEX_INITIALIZER; /* control access to the status report */
static bool report_ready = false; /* true when there is a new report to send to the server */
static char status_report[STATUS_SIZE]; /* status report as a JSON object */


/* -------------------------------------------------------------------------- */
/* --- PRIVATE FUNCTIONS DEFINITION ----------------------------------------- */

static uint16_t crc16(const uint8_t * data, unsigned size) {
    const uint16_t crc_poly = 0x1021;
    const uint16_t init_val = 0x0000;
    uint16_t x = init_val;
    unsigned i, j;

    if (data == NULL)  {
        return 0;
    }

    for (i=0; i<size; ++i) {
        x ^= (uint16_t)data[i] << 8;
        for (j=0; j<8; ++j) {
            x = (x & 0x8000) ? (x<<1) ^ crc_poly : (x<<1);
        }
    }

    return x;
}

static double difftimespec(struct timespec end, struct timespec beginning) {
    double x;

    x = 1E-9 * (double)(end.tv_nsec - beginning.tv_nsec);
    x += (double)(end.tv_sec - beginning.tv_sec);

    return x;
}

static int send_tx_ack(uint8_t token_h, uint8_t token_l, enum jit_error_e error) {
    uint8_t buff_ack[64]; /* buffer to give feedback to server */
    int buff_index;

    /* reset buffer */
    memset(&buff_ack, 0, sizeof buff_ack);

    /* Prepare downlink feedback to be sent to server */
    buff_ack[0] = PROTOCOL_VERSION;
    buff_ack[1] = token_h;
    buff_ack[2] = token_l;
    buff_ack[3] = PKT_TX_ACK;
    *(uint32_t *)(buff_ack + 4) = net_mac_h;
    *(uint32_t *)(buff_ack + 8) = net_mac_l;
    buff_index = 12; /* 12-byte header */

    /* Put no JSON string if there is nothing to report */
    if (error != JIT_ERROR_OK) {
        /* start of JSON structure */
        memcpy((void *)(buff_ack + buff_index), (void *)"{\"txpk_ack\":{", 13);
        buff_index += 13;
        /* set downlink error status in JSON structure */
        memcpy((void *)(buff_ack + buff_index), (void *)"\"error\":", 8);
        buff_index += 8;
        switch (error) {
//            case JIT_ERROR_FULL:
//            case JIT_ERROR_COLLISION_PACKET:
//                memcpy((void *)(buff_ack + buff_index), (void *)"\"COLLISION_PACKET\"", 18);
//                buff_index += 18;
//                /* update stats */
//                pthread_mutex_lock(&mx_meas_dw);
//                meas_nb_tx_rejected_collision_packet += 1;
//                pthread_mutex_unlock(&mx_meas_dw);
//                break;
//            case JIT_ERROR_TOO_LATE:
//                memcpy((void *)(buff_ack + buff_index), (void *)"\"TOO_LATE\"", 10);
//                buff_index += 10;
//                /* update stats */
//                pthread_mutex_lock(&mx_meas_dw);
//                meas_nb_tx_rejected_too_late += 1;
//                pthread_mutex_unlock(&mx_meas_dw);
//                break;
//            case JIT_ERROR_TOO_EARLY:
//                memcpy((void *)(buff_ack + buff_index), (void *)"\"TOO_EARLY\"", 11);
//                buff_index += 11;
//                /* update stats */
//                pthread_mutex_lock(&mx_meas_dw);
//                meas_nb_tx_rejected_too_early += 1;
//                pthread_mutex_unlock(&mx_meas_dw);
//                break;
//            case JIT_ERROR_COLLISION_BEACON:
//                memcpy((void *)(buff_ack + buff_index), (void *)"\"COLLISION_BEACON\"", 18);
//                buff_index += 18;
//                /* update stats */
//                pthread_mutex_lock(&mx_meas_dw);
//                meas_nb_tx_rejected_collision_beacon += 1;
//                pthread_mutex_unlock(&mx_meas_dw);
//                break;
//            case JIT_ERROR_TX_FREQ:
//                memcpy((void *)(buff_ack + buff_index), (void *)"\"TX_FREQ\"", 9);
//                buff_index += 9;
//                break;
//            case JIT_ERROR_TX_POWER:
//                memcpy((void *)(buff_ack + buff_index), (void *)"\"TX_POWER\"", 10);
//                buff_index += 10;
//                break;
//            case JIT_ERROR_GPS_UNLOCKED:
//                memcpy((void *)(buff_ack + buff_index), (void *)"\"GPS_UNLOCKED\"", 14);
//                buff_index += 14;
//                break;
            default:
                memcpy((void *)(buff_ack + buff_index), (void *)"\"UNKNOWN\"", 9);
                buff_index += 9;
                break;
        }
        /* end of JSON structure */
        memcpy((void *)(buff_ack + buff_index), (void *)"}}", 2);
        buff_index += 2;
    }

    buff_ack[buff_index] = 0; /* add string terminator, for safety */

    /* send datagram to server */
    return send(sock_down, (void *)buff_ack, buff_index, 0);
}

static void sig_handler(int sigio) {
    if (sigio == SIGQUIT) {
        quit_sig = true;
    } else if ((sigio == SIGINT) || (sigio == SIGTERM)) {
        exit_sig = true;
    }
    return;
}

void die(const char *s)
{
    perror(s);
    exit(1);
}

boolean receivePkt(char *payload)
{

    // clear rxDone
//    writeRegister(REG_IRQ_FLAGS, 0x40);
//
//    int irqflags = readRegister(REG_IRQ_FLAGS);
//
//    cp_nb_rx_rcv++;
//
//    //  payload crc: 0x20
//    if((irqflags & 0x20) == 0x20)
//    {
//        printf("CRC error\n");
//        writeRegister(REG_IRQ_FLAGS, 0x20);
//        return false;
//    } else {
//
//        cp_nb_rx_ok++;
//
//        byte currentAddr = readRegister(REG_FIFO_RX_CURRENT_ADDR);
//        byte receivedCount = readRegister(REG_RX_NB_BYTES);
//        receivedbytes = receivedCount;
//
//        writeRegister(REG_FIFO_ADDR_PTR, currentAddr);
//
//        for(int i = 0; i < receivedCount; i++)
//        {
//            payload[i] = (char)readRegister(REG_FIFO);
//        }
//    }
//    return true;
}

void SetupLoRa()
{
	//TODO: Check ping

	//TODO: set frequency

	//TODO: set SF

	//TODO: set power

}

//void sendudp(char *msg, int length) {
//
////send the update
//#ifdef SERVER1
//    if(inet_aton(SERVER1 , &si_other.sin_addr)==0)
//    {
//	fprintf(stderr, "inet_aton() failed\n");
//	exit(1);
//    }
//    if (sendto(s, (char *)msg, length, 0 , (struct sockaddr *) &si_other, slen)==-1)
//    {
//        die("sendto()");
//    }
//#endif
//
//#ifdef SERVER2
//    inet_aton(SERVER2 , &si_other.sin_addr);
//    if (sendto(s, (char *)msg, length , 0 , (struct sockaddr *) &si_other, slen)==-1)
//    {
//        die("sendto()");
//    }
//#endif
//}

int PacketValidate(uint8_t * data, uint16_t* len) {
    uint16_t count;

    if (data[0] == 0x1) {
        count = (uint16_t) data[2] << 8 | (uint16_t) data[3];
        if (count == *len && memcmp(data, "\r\n", *len)) {

            switch (data[1])
            {
                case RX:
                default:
                    count = *len - 10;
                    memcpy(data, data + 8, count);
                    *len = count;
                    break;
            }

        }
        else {
            return 1;
        }
    }
    else {
        return 1;
    }


    return 0;
}

int lgw_receive(uint8_t max_pkt, struct lgw_pkt_rx_s *pkt_data) {
//#ifdef IM980A
	int buff;
	uint16_t buff_size = 0;
	struct timeval now;
	gettimeofday(&now, NULL);
	uint32_t tmst = (uint32_t)(now.tv_sec*1000000 + now.tv_usec);


	while (serialDataAvail (fd))
	{
		buff = serialGetchar (fd);
		if(buff == -1) break;
		message[buff_size++] = buff;
	}

	//TODO: Validate message
	if(PacketValidate((uint8_t*) message, &buff_size) == 1) {
		return 0;
	}

	pkt_data->bandwidth = BW_125KHZ;
	pkt_data->coderate = CR_LORA_4_5;
	pkt_data->count_us = tmst;
	memcpy((uint8_t*)pkt_data->crc, message + (buff_size - 2), 2);
	pkt_data->datarate = DR_LORA_SF7;
	pkt_data->freq_hz = freq;
	pkt_data->if_chain = 0;
	pkt_data->modulation = MOD_LORA;

	memcpy(pkt_data->payload, (uint8_t*) message, buff_size);
	pkt_data->rf_chain = 0;
	pkt_data->rssi = message[4];
	pkt_data->size = buff_size;
	pkt_data->snr = message[5];
	pkt_data->status = STAT_CRC_OK;
    return 1;
//#else
//    long int SNR;
//    int rssicorr;
//    uint8_t buff_ack[32];
//
//    if(digitalRead(dio0) == 1)
//    {
//        if(receivePkt(message)) {
//            byte value = readRegister(REG_PKT_SNR_VALUE);
//            if( value & 0x80 ) // The SNR sign bit is 1
//            {
//                // Invert and divide by 4
//                value = ( ( ~value + 1 ) & 0xFF ) >> 2;
//                SNR = -value;
//            }
//            else
//            {
//                // Divide by 4
//                SNR = ( value & 0xFF ) >> 2;
//            }
//
//            if (sx1272) {
//                rssicorr = 139;
//            } else {
//                rssicorr = 157;
//            }
//
//            printf("Packet RSSI: %d, ",readRegister(0x1A)-rssicorr);
//            printf("RSSI: %d, ",readRegister(0x1B)-rssicorr);
//            printf("SNR: %li, ",SNR);
//            printf("Length: %i",(int)receivedbytes);
//            printf("\n");
//
//            int j;
//            j = bin_to_b64((uint8_t *)message, receivedbytes, (char *)(b64), 341);
//            //fwrite(b64, sizeof(char), j, stdout);
//
//            char buff_up[TX_BUFF_SIZE]; /* buffer to compose the upstream packet */
//            int buff_index=0;
//
//	    int i = setsockopt(sock_up, SOL_SOCKET, SO_RCVTIMEO,
//			    (void*) &push_timeout_half, sizeof push_timeout_half);
//    	    if (i != 0) {
//	         printf("ERR: [up] sertsockopt returned %s\n", strerror (errno));
//	         exit(EXIT_FAILURE);
//            }
//
//
//            /* gateway <-> MAC protocol variables */
//            //static uint32_t net_mac_h; /* Most Significant Nibble, network order */
//            //static uint32_t net_mac_l; /* Least Significant Nibble, network order */
//
//            /* pre-fill the data buffer with fixed fields */
//            buff_up[0] = PROTOCOL_VERSION;
//            buff_up[3] = PKT_PUSH_DATA;
//
//            /* process some of the configuration variables */
//            //net_mac_h = htonl((uint32_t)(0xFFFFFFFF & (lgwm>>32)));
//            //net_mac_l = htonl((uint32_t)(0xFFFFFFFF &  lgwm  ));
//            *(uint32_t *)(buff_up + 4) = net_mac_h;
//            *(uint32_t *)(buff_up + 8) = net_mac_l;
//
///*          buff_up[4] = (unsigned char)ifr.ifr_hwaddr.sa_data[0];
//            buff_up[5] = (unsigned char)ifr.ifr_hwaddr.sa_data[1];
//            buff_up[6] = (unsigned char)ifr.ifr_hwaddr.sa_data[2];
//            buff_up[7] = 0xFF;
//            buff_up[8] = 0xFF;
//            buff_up[9] = (unsigned char)ifr.ifr_hwaddr.sa_data[3];
//            buff_up[10] = (unsigned char)ifr.ifr_hwaddr.sa_data[4];
//            buff_up[11] = (unsigned char)ifr.ifr_hwaddr.sa_data[5];
//*/
//            /* start composing datagram with the header */
//            uint8_t token_h = (uint8_t)rand(); /* random token */
//            uint8_t token_l = (uint8_t)rand(); /* random token */
//            buff_up[1] = token_h;
//            buff_up[2] = token_l;
//            buff_index = 12; /* 12-byte header */
//
//            // TODO: tmst can jump is time is (re)set, not good.
//            struct timeval now;
//            gettimeofday(&now, NULL);
//            uint32_t tmst = (uint32_t)(now.tv_sec*1000000 + now.tv_usec);
//
//            /* start of JSON structure */
//            memcpy((void *)(buff_up + buff_index), (void *)"{\"rxpk\":[", 9);
//            buff_index += 9;
//            buff_up[buff_index] = '{';
//            ++buff_index;
//            j = snprintf((char *)(buff_up + buff_index), TX_BUFF_SIZE-buff_index, "\"tmst\":%u", tmst);
//            buff_index += j;
//            j = snprintf((char *)(buff_up + buff_index), TX_BUFF_SIZE-buff_index, ",\"chan\":%1u,\"rfch\":%1u,\"freq\":%.6lf", 0, 0, (double)freq/1000000);
//            buff_index += j;
//            memcpy((void *)(buff_up + buff_index), (void *)",\"stat\":1", 9);
//            buff_index += 9;
//            memcpy((void *)(buff_up + buff_index), (void *)",\"modu\":\"LORA\"", 14);
//            buff_index += 14;
//            /* Lora datarate & bandwidth, 16-19 useful chars */
//            switch (sf) {
//            case SF7:
//                memcpy((void *)(buff_up + buff_index), (void *)",\"datr\":\"SF7", 12);
//                buff_index += 12;
//                break;
//            case SF8:
//                memcpy((void *)(buff_up + buff_index), (void *)",\"datr\":\"SF8", 12);
//                buff_index += 12;
//                break;
//            case SF9:
//                memcpy((void *)(buff_up + buff_index), (void *)",\"datr\":\"SF9", 12);
//                buff_index += 12;
//                break;
//            case SF10:
//                memcpy((void *)(buff_up + buff_index), (void *)",\"datr\":\"SF10", 13);
//                buff_index += 13;
//                break;
//            case SF11:
//                memcpy((void *)(buff_up + buff_index), (void *)",\"datr\":\"SF11", 13);
//                buff_index += 13;
//                break;
//            case SF12:
//                memcpy((void *)(buff_up + buff_index), (void *)",\"datr\":\"SF12", 13);
//                buff_index += 13;
//                break;
//            default:
//                memcpy((void *)(buff_up + buff_index), (void *)",\"datr\":\"SF?", 12);
//                buff_index += 12;
//            }
//            memcpy((void *)(buff_up + buff_index), (void *)"BW125\"", 6);
//            buff_index += 6;
//            memcpy((void *)(buff_up + buff_index), (void *)",\"codr\":\"4/5\"", 13);
//            buff_index += 13;
//            j = snprintf((char *)(buff_up + buff_index), TX_BUFF_SIZE-buff_index, ",\"lsnr\":%li", SNR);
//            buff_index += j;
//            j = snprintf((char *)(buff_up + buff_index), TX_BUFF_SIZE-buff_index, ",\"rssi\":%d,\"size\":%u", readRegister(0x1A)-rssicorr, receivedbytes);
//            buff_index += j;
//            memcpy((void *)(buff_up + buff_index), (void *)",\"data\":\"", 9);
//            buff_index += 9;
//            j = bin_to_b64((uint8_t *)message, receivedbytes, (char *)(buff_up + buff_index), 341);
//            buff_index += j;
//            buff_up[buff_index] = '"';
//            ++buff_index;
//
//            /* End of packet serialization */
//            buff_up[buff_index] = '}';
//            ++buff_index;
//            buff_up[buff_index] = ']';
//            ++buff_index;
//            /* end of JSON datagram payload */
//            buff_up[buff_index] = '}';
//            ++buff_index;
//            buff_up[buff_index] = 0; /* add string terminator, for safety */
//
//            printf("rxpk update: %s\n", (char *)(buff_up + 12)); /* DEBUG: display JSON payload */
//
//            //send the messages
//            send(sock_up, (void *)buff_up, buff_index, 0);
//            //sendudp(buff_up, buff_index);
//	  for (int j = 0; j<2; ++j) {
//	   	 i = recv(sock_up, (void *)buff_ack, sizeof buff_ack, 0);
//    		if (i == -1) {
//
//			printf("INFO: [up] ack receive is retured (%s)\n", strerror(i));
//			if(errno == EAGAIN) {
//				continue;
//			} else {
//				break;
//			}
//		} else if ((i < 4) || (buff_ack[0] != PROTOCOL_VERSION) || (buff_ack[3] != PKT_PUSH_ACK)) {
//			continue;
//		} else if ((buff_ack[1] != token_h) || (buff_ack[2] != token_l)) {
//			continue;
//		} else {
//			printf("INFO: [up] PUSH ACK received\n");
//			break;
//		}
//   	 }
//         fflush(stdout);
//
//        } // received a message
//
//    } // dio0=1
//#endif
}

int lgw_send(struct lgw_pkt_tx_s pkt_data) {
	return LGW_HAL_SUCCESS;
}

//void pull_data(void) {
//	uint8_t buff_pull[16], buff_ack[8];
//        uint8_t buff_index;
//	int i = setsockopt(sock_up, SOL_SOCKET, SO_RCVTIMEO,
//			    (void*) &pull_timeout_half, sizeof pull_timeout_half);
//    	    if (i != 0) {
//	         printf("ERR: [up] setsockopt returned %s\n", strerror (errno));
//	         exit(EXIT_FAILURE);
//            }
//
//
//            /* gateway <-> MAC protocol variables */
//            //static uint32_t net_mac_h; /* Most Significant Nibble, network order */
//            //static uint32_t net_mac_l; /* Least Significant Nibble, network order */
//
//            /* pre-fill the data buffer with fixed fields */
//            buff_pull[0] = PROTOCOL_VERSION;
//            buff_pull[3] = PKT_PULL_DATA;
//
//            /* process some of the configuration variables */
//            //net_mac_h = htonl((uint32_t)(0xFFFFFFFF & (lgwm>>32)));
//            //net_mac_l = htonl((uint32_t)(0xFFFFFFFF &  lgwm  ));
//            *(uint32_t *)(buff_pull + 4) = net_mac_h;
//            *(uint32_t *)(buff_pull + 8) = net_mac_l;
//
//            /* start composing datagram with the header */
//            uint8_t token_h = (uint8_t)rand(); /* random token */
//            uint8_t token_l = (uint8_t)rand(); /* random token */
//            buff_pull[1] = token_h;
//            buff_pull[2] = token_l;
//            buff_index = 12; /* 12-byte header */
//   //send the messages
//            send(sock_up, (void *)buff_pull, buff_index, 0);
//            //sendudp(buff_up, buff_index);
//	  for (int j = 0; j<2; ++j) {
//	   	 i = recv(sock_up, (void *)buff_ack, sizeof buff_ack, 0);
//    		if (i == -1) {
//
//			printf("INFO: [up] ack receive is retured (%s)\n", strerror(i));
//			if(errno == EAGAIN) {
//				continue;
//			} else {
//				break;
//			}
//		} else if ((i < 4) || (buff_ack[0] != PROTOCOL_VERSION) || (buff_ack[3] != PKT_PULL_ACK)) {
//			continue;
//		} else if ((buff_ack[1] != token_h) || (buff_ack[2] != token_l)) {
//			continue;
//		} else {
//			printf("INFO: [up] PULL ACK received\n");
//			break;
//		}
//   	 }
//
//
//}

//void pull_resp(void) {
//	int ret;
//
//	ret = setsockopt(sock_down, SOL_SOCKET, SO_RCVTIMEO,
//		(void*)&pull_timeout_half, sizeof pull_timeout_half);
//	if (ret != 0) {
//		printf("ERR: [down] setsockopt returned %s\n", strerror(errno));
//		exit(EXIT_FAILURE);
//	}
//
//	ret = recv(sock_down, (void *)buff_resp, sizeof buff_resp, 0);
//}
 
//void sendpacket(void) {
//
//}

int main (void) {

	struct sigaction sigact; /* SIGQUIT&SIGINT&SIGTERM signal handling */
	/* threads */
	pthread_t thrid_up;
	pthread_t thrid_down;
	pthread_t thrid_jit;

    struct timeval nowtime;
    uint32_t lasttime;

    struct addrinfo hints;
    struct addrinfo *result;
    struct addrinfo *q;
    
    char host_name[64];
    char port_name[64];
    int i;

    if (wiringPiSetup () == -1)
	{
		fprintf (stdout, "Unable to start wiringPi: %s\n", strerror (errno)) ;
		return 1 ;
	}
    if ((fd = serialOpen ("/dev/ttyAMA0", 115200)) < 0)
   	{
       	fprintf (stderr, "Unable to open serial device: %s\n", strerror (errno)) ;
       	return 1 ;
   	}
    printf("Init result: %d\n" ,fd);

    SetupLoRa();

    strncpy(serv_addr, SERVER1, sizeof serv_addr);
    printf("INFO: server is configured to \"%s\"\n", serv_addr);
    snprintf(serv_port_up, sizeof serv_port_up, "%u", (uint16_t) PORT);
    printf("INFO: upstream port is configured to \"%s\"\n", serv_port_up);

    sscanf(gateway_id, "%llx", &lgwm);
    printf("INFO: gateway MAC is configured to %016llX\n", lgwm);

    net_mac_h = htonl((uint32_t)(0xFFFFFFFF & (lgwm>>32)));    
    net_mac_l = htonl((uint32_t)(0xFFFFFFFF & (lgwm)));    

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    i = getaddrinfo(serv_addr, serv_port_up, &hints, &result);
    if(i != 0) {
		printf("ERR: [up] getaddrinfo on address %s (PORT %s) return %s\n", serv_addr, serv_port_up, gai_strerror(i));
		exit(EXIT_FAILURE);
    }

    for (q=result; q!=NULL; q=q->ai_next) {
		sock_up = socket(q->ai_family, q->ai_socktype, q->ai_protocol);
		if (sock_up == -1) continue;
		else break;
    }

    if (q == NULL) {
		printf("ERR: failed to open socket to any fo server %s (port %s)\n", serv_addr, serv_port_up);
		i =1;
		for (q=result; q!=NULL; q=q->ai_next) {
			getnameinfo(q->ai_addr, q->ai_addrlen, host_name, sizeof host_name, port_name, sizeof port_name, NI_NUMERICHOST);
			printf("INFO: [up] result %i host:%s service:%s\n",i,host_name,port_name);
			++i;
		}
		exit(EXIT_FAILURE);
    }

    i = connect(sock_up, q->ai_addr, q->ai_addrlen);
    if (i != 0) {
    	printf("ERR: [up] connect returned %s\n", strerror(errno));
    }
    freeaddrinfo(result);

    /* look for server address w/ downstream port */
   i = getaddrinfo(serv_addr, serv_port_down, &hints, &result);
   if (i != 0) {
	   printf("ERROR: [down] getaddrinfo on address %s (port %s) returned %s\n", serv_addr, serv_port_up, gai_strerror(i));
	   exit(EXIT_FAILURE);
   }

   /* try to open socket for downstream traffic */
   for (q=result; q!=NULL; q=q->ai_next) {
	   sock_down = socket(q->ai_family, q->ai_socktype,q->ai_protocol);
	   if (sock_down == -1) continue; /* try next field */
	   else break; /* success, get out of loop */
   }
   if (q == NULL) {
	   printf("ERROR: [down] failed to open socket to any of server %s addresses (port %s)\n", serv_addr, serv_port_up);
	   i = 1;
	   for (q=result; q!=NULL; q=q->ai_next) {
		   getnameinfo(q->ai_addr, q->ai_addrlen, host_name, sizeof host_name, port_name, sizeof port_name, NI_NUMERICHOST);
		   printf("INFO: [down] result %i host:%s service:%s\n", i, host_name, port_name);
		   ++i;
	   }
	   exit(EXIT_FAILURE);
   }

   /* connect so we can send/receive packet with the server only */
   i = connect(sock_down, q->ai_addr, q->ai_addrlen);
   if (i != 0) {
	   printf("ERROR: [down] connect returned %s\n", strerror(errno));
	   exit(EXIT_FAILURE);
   }
   freeaddrinfo(result);

/*   
    if ( (s=socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
    {
        die("socket");
    }
    memset((char *) &si_other, 0, sizeof(si_other));
    si_other.sin_family = AF_INET;
    si_other.sin_port = htons(PORT);

    ifr.ifr_addr.sa_family = AF_INET;
    strncpy(ifr.ifr_name, "enxb827eb3658da", IFNAMSIZ-1);  // can we rely on eth0?
    ioctl(s, SIOCGIFHWADDR, &ifr);
*/
    /* display result */
/*
 printf("Gateway ID: %.2x:%.2x:%.2x:ff:ff:%.2x:%.2x:%.2x\n",
           (unsigned char)ifr.ifr_hwaddr.sa_data[0],
           (unsigned char)ifr.ifr_hwaddr.sa_data[1],
           (unsigned char)ifr.ifr_hwaddr.sa_data[2],
           (unsigned char)ifr.ifr_hwaddr.sa_data[3],
           (unsigned char)ifr.ifr_hwaddr.sa_data[4],
           (unsigned char)ifr.ifr_hwaddr.sa_data[5]
	   );
*/

    printf("Listening at SF%i on %.6lf Mhz.\n", SF7,(double)freq/1000000);
    printf("------------------\n");


    /* spawn threads to manage upstream and downstream */
    i = pthread_create( &thrid_up, NULL, (void * (*)(void *))thread_up, NULL);
    if (i != 0) {
    	printf("ERROR: [main] impossible to create upstream thread\n");
        exit(EXIT_FAILURE);
    }
    i = pthread_create( &thrid_down, NULL, (void * (*)(void *))thread_down, NULL);
    if (i != 0) {
    	printf("ERROR: [main] impossible to create downstream thread\n");
        exit(EXIT_FAILURE);
    }
    i = pthread_create( &thrid_jit, NULL, (void * (*)(void *))thread_jit, NULL);
    if (i != 0) {
    	printf("ERROR: [main] impossible to create JIT thread\n");
        exit(EXIT_FAILURE);
    }

    /* configure signal handling */
	sigemptyset(&sigact.sa_mask);
	sigact.sa_flags = 0;
	sigact.sa_handler = sig_handler;
	sigaction(SIGQUIT, &sigact, NULL); /* Ctrl-\ */
	sigaction(SIGINT, &sigact, NULL); /* Ctrl-C */
	sigaction(SIGTERM, &sigact, NULL); /* default "kill" command */

	/* main loop task : statistics collection */
	 while (!exit_sig && !quit_sig) {
	        /* wait for next reporting interval */
	        wait_ms(1000 * stat_interval);

	        /* get timestamp for statistics */
	        t = time(NULL);
	        strftime(stat_timestamp, sizeof stat_timestamp, "%F %T %Z", gmtime(&t));

	        /* access upstream statistics, copy and reset them */
	        pthread_mutex_lock(&mx_meas_up);
	        cp_nb_rx_rcv       = meas_nb_rx_rcv;
	        cp_nb_rx_ok        = meas_nb_rx_ok;
	        cp_nb_rx_bad       = meas_nb_rx_bad;
	        cp_nb_rx_nocrc     = meas_nb_rx_nocrc;
	        cp_up_pkt_fwd      = meas_up_pkt_fwd;
	        cp_up_network_byte = meas_up_network_byte;
	        cp_up_payload_byte = meas_up_payload_byte;
	        cp_up_dgram_sent   = meas_up_dgram_sent;
	        cp_up_ack_rcv      = meas_up_ack_rcv;
	        meas_nb_rx_rcv = 0;
	        meas_nb_rx_ok = 0;
	        meas_nb_rx_bad = 0;
	        meas_nb_rx_nocrc = 0;
	        meas_up_pkt_fwd = 0;
	        meas_up_network_byte = 0;
	        meas_up_payload_byte = 0;
	        meas_up_dgram_sent = 0;
	        meas_up_ack_rcv = 0;
	        pthread_mutex_unlock(&mx_meas_up);
	        if (cp_nb_rx_rcv > 0) {
	            rx_ok_ratio = (float)cp_nb_rx_ok / (float)cp_nb_rx_rcv;
	            rx_bad_ratio = (float)cp_nb_rx_bad / (float)cp_nb_rx_rcv;
	            rx_nocrc_ratio = (float)cp_nb_rx_nocrc / (float)cp_nb_rx_rcv;
	        } else {
	            rx_ok_ratio = 0.0;
	            rx_bad_ratio = 0.0;
	            rx_nocrc_ratio = 0.0;
	        }
	        if (cp_up_dgram_sent > 0) {
	            up_ack_ratio = (float)cp_up_ack_rcv / (float)cp_up_dgram_sent;
	        } else {
	            up_ack_ratio = 0.0;
	        }

	        /* access downstream statistics, copy and reset them */
	        pthread_mutex_lock(&mx_meas_dw);
	        cp_dw_pull_sent    =  meas_dw_pull_sent;
	        cp_dw_ack_rcv      =  meas_dw_ack_rcv;
	        cp_dw_dgram_rcv    =  meas_dw_dgram_rcv;
	        cp_dw_network_byte =  meas_dw_network_byte;
	        cp_dw_payload_byte =  meas_dw_payload_byte;
	        cp_nb_tx_ok        =  meas_nb_tx_ok;
	        cp_nb_tx_fail      =  meas_nb_tx_fail;
	        cp_nb_tx_requested                 +=  meas_nb_tx_requested;
	        meas_dw_pull_sent = 0;
	        meas_dw_ack_rcv = 0;
	        meas_dw_dgram_rcv = 0;
	        meas_dw_network_byte = 0;
	        meas_dw_payload_byte = 0;
	        meas_nb_tx_ok = 0;
	        meas_nb_tx_fail = 0;
	        meas_nb_tx_requested = 0;

	        pthread_mutex_unlock(&mx_meas_dw);
	        if (cp_dw_pull_sent > 0) {
	            dw_ack_ratio = (float)cp_dw_ack_rcv / (float)cp_dw_pull_sent;
	        } else {
	            dw_ack_ratio = 0.0;
	        }

	        /* display a report */
	        printf("\n##### %s #####\n", stat_timestamp);
	        printf("### [UPSTREAM] ###\n");
	        printf("# RF packets received by concentrator: %u\n", cp_nb_rx_rcv);
	        printf("# CRC_OK: %.2f%%, CRC_FAIL: %.2f%%, NO_CRC: %.2f%%\n", 100.0 * rx_ok_ratio, 100.0 * rx_bad_ratio, 100.0 * rx_nocrc_ratio);
	        printf("# RF packets forwarded: %u (%u bytes)\n", cp_up_pkt_fwd, cp_up_payload_byte);
	        printf("# PUSH_DATA datagrams sent: %u (%u bytes)\n", cp_up_dgram_sent, cp_up_network_byte);
	        printf("# PUSH_DATA acknowledged: %.2f%%\n", 100.0 * up_ack_ratio);
	        printf("### [DOWNSTREAM] ###\n");
	        printf("# PULL_DATA sent: %u (%.2f%% acknowledged)\n", cp_dw_pull_sent, 100.0 * dw_ack_ratio);
	        printf("# PULL_RESP(onse) datagrams received: %u (%u bytes)\n", cp_dw_dgram_rcv, cp_dw_network_byte);
	        printf("# RF packets sent to concentrator: %u (%u bytes)\n", (cp_nb_tx_ok+cp_nb_tx_fail), cp_dw_payload_byte);
	        printf("# TX errors: %u\n", cp_nb_tx_fail);
//	        if (cp_nb_tx_requested != 0 ) {
//	            printf("# TX rejected (collision packet): %.2f%% (req:%u, rej:%u)\n", 100.0 * cp_nb_tx_rejected_collision_packet / cp_nb_tx_requested, cp_nb_tx_requested, cp_nb_tx_rejected_collision_packet);
//	            printf("# TX rejected (collision beacon): %.2f%% (req:%u, rej:%u)\n", 100.0 * cp_nb_tx_rejected_collision_beacon / cp_nb_tx_requested, cp_nb_tx_requested, cp_nb_tx_rejected_collision_beacon);
//	            printf("# TX rejected (too late): %.2f%% (req:%u, rej:%u)\n", 100.0 * cp_nb_tx_rejected_too_late / cp_nb_tx_requested, cp_nb_tx_requested, cp_nb_tx_rejected_too_late);
//	            printf("# TX rejected (too early): %.2f%% (req:%u, rej:%u)\n", 100.0 * cp_nb_tx_rejected_too_early / cp_nb_tx_requested, cp_nb_tx_requested, cp_nb_tx_rejected_too_early);
//	        }
//	        printf("# BEACON queued: %u\n", cp_nb_beacon_queued);
//	        printf("# BEACON sent so far: %u\n", cp_nb_beacon_sent);
//	        printf("# BEACON rejected: %u\n", cp_nb_beacon_rejected);
	        printf("### [JIT] ###\n");

	        jit_print_queue (&jit_queue, false, DEBUG_LOG);
	        printf("##### END #####\n");

	        /* generate a JSON report (will be sent to server by upstream thread) */
	        pthread_mutex_lock(&mx_stat_rep);
	        snprintf(status_report, STATUS_SIZE, "\"stat\":{\"time\":\"%s\",\"lati\":%.5f,\"long\":%.5f,\"alti\":%i,\"rxnb\":%u,\"rxok\":%u,\"rxfw\":%u,\"ackr\":%.1f,\"dwnb\":%u,\"txnb\":%u}", stat_timestamp, lat, lon, (int)alt, cp_nb_rx_rcv, cp_nb_rx_ok, cp_up_pkt_fwd, 100.0 * up_ack_ratio, cp_dw_dgram_rcv, cp_nb_tx_ok);
	        report_ready = true;
	        pthread_mutex_unlock(&mx_stat_rep);
	    }

	    /* wait for upstream thread to finish (1 fetch cycle max) */
	    pthread_join(thrid_up, NULL);
	    pthread_cancel(thrid_down); /* don't wait for downstream thread */
	    pthread_cancel(thrid_jit); /* don't wait for jit thread */

	    /* if an exit signal was received, try to quit properly */
	    if (exit_sig) {
	        /* shut down network sockets */
	        shutdown(sock_up, SHUT_RDWR);
	        shutdown(sock_down, SHUT_RDWR);
	        /* stop the hardware */
	        serialClose (fd) ;
//	        if (i == LGW_HAL_SUCCESS) {
	        	printf("INFO: concentrator stopped successfully\n");
//	        } else {
//	        	printf("WARNING: failed to stop concentrator successfully\n");
//	        }
	    }

	    printf("INFO: Exiting packet forwarder program\n");
	    exit(EXIT_SUCCESS);

}


/* -------------------------------------------------------------------------- */
/* --- THREAD 1: RECEIVING PACKETS AND FORWARDING THEM ---------------------- */

void thread_up(void) {
	int i, j; /* loop variables */
	    unsigned pkt_in_dgram; /* nb on Lora packet in the current datagram */

	    /* allocate memory for packet fetching and processing */
	    struct lgw_pkt_rx_s rxpkt[NB_PKT_MAX]; /* array containing inbound packets + metadata */
	    struct lgw_pkt_rx_s *p; /* pointer on a RX packet */
	    int nb_pkt;

	    /* local copy of GPS time reference */
//	    bool ref_ok = false; /* determine if GPS time reference must be used or not */
//	    struct tref local_ref; /* time reference used for UTC <-> timestamp conversion */

	    /* data buffers */
	    uint8_t buff_up[TX_BUFF_SIZE]; /* buffer to compose the upstream packet */
	    int buff_index;
	    uint8_t buff_ack[32]; /* buffer to receive acknowledges */

	    /* protocol variables */
	    uint8_t token_h; /* random token for acknowledgement matching */
	    uint8_t token_l; /* random token for acknowledgement matching */

	    /* ping measurement variables */
	    struct timespec send_time;
	    struct timespec recv_time;

	    /* GPS synchronization variables */
//	    struct timespec pkt_utc_time;
//	    struct tm * x; /* broken-up UTC time */
//	    struct timespec pkt_gps_time;
//	    uint64_t pkt_gps_time_ms;

	    /* report management variable */
	    bool send_report = false;

	    /* mote info variables */
	    uint32_t mote_addr = 0;
	    uint16_t mote_fcnt = 0;

	    /* set upstream socket RX timeout */
	    i = setsockopt(sock_up, SOL_SOCKET, SO_RCVTIMEO, (void *)&push_timeout_half, sizeof push_timeout_half);
	    if (i != 0) {
	    	printf("ERROR: [up] setsockopt returned %s\n", strerror(errno));
	        exit(EXIT_FAILURE);
	    }

	    /* pre-fill the data buffer with fixed fields */
	    buff_up[0] = PROTOCOL_VERSION;
	    buff_up[3] = PKT_PUSH_DATA;
	    *(uint32_t *)(buff_up + 4) = net_mac_h;
	    *(uint32_t *)(buff_up + 8) = net_mac_l;

	while (!exit_sig && !quit_sig) {
		 /* fetch packets */
		pthread_mutex_lock(&mx_concent);
		nb_pkt = lgw_receive(0, rxpkt);
		pthread_mutex_unlock(&mx_concent);
		if (nb_pkt == LGW_HAL_ERROR) {
			printf("ERROR: [up] failed packet fetch, exiting\n");
			exit(EXIT_FAILURE);
		}

		 /* check if there are status report to send */
		send_report = report_ready; /* copy the variable so it doesn't change mid-function */
		/* no mutex, we're only reading */

		/* wait a short time if no packets, nor status report */
		if ((nb_pkt == 0) && (send_report == false)) {
			wait_ms(FETCH_SLEEP_MS);
			continue;
		}

		/* start composing datagram with the header */
		token_h = (uint8_t)rand(); /* random token */
		token_l = (uint8_t)rand(); /* random token */
		buff_up[1] = token_h;
		buff_up[2] = token_l;
		buff_index = 12; /* 12-byte header */

		 /* start of JSON structure */
		memcpy((void *)(buff_up + buff_index), (void *)"{\"rxpk\":[", 9);
		buff_index += 9;

		/* serialize Lora packets metadata and payload */
		pkt_in_dgram = 0;
		for (i=0; i < nb_pkt; ++i) {
			p = &rxpkt[i];

			/* Get mote information from current packet (addr, fcnt) */
			/* FHDR - DevAddr */
			mote_addr  = p->payload[1];
			mote_addr |= p->payload[2] << 8;
			mote_addr |= p->payload[3] << 16;
			mote_addr |= p->payload[4] << 24;
			/* FHDR - FCnt */
			mote_fcnt  = p->payload[6];
			mote_fcnt |= p->payload[7] << 8;

			/* basic packet filtering */
			pthread_mutex_lock(&mx_meas_up);
			meas_nb_rx_rcv += 1;
//			switch(p->status) {
//				case STAT_CRC_OK:
//					meas_nb_rx_ok += 1;
//					printf( "\nINFO: Received pkt from mote: %08X (fcnt=%u)\n", mote_addr, mote_fcnt );
//					if (!fwd_valid_pkt) {
//						pthread_mutex_unlock(&mx_meas_up);
//						continue; /* skip that packet */
//					}
//					break;
//				case STAT_CRC_BAD:
//					meas_nb_rx_bad += 1;
//					if (!fwd_error_pkt) {
//						pthread_mutex_unlock(&mx_meas_up);
//						continue; /* skip that packet */
//					}
//					break;
//				case STAT_NO_CRC:
//					meas_nb_rx_nocrc += 1;
//					if (!fwd_nocrc_pkt) {
//						pthread_mutex_unlock(&mx_meas_up);
//						continue; /* skip that packet */
//					}
//					break;
//				default:
//					MSG("WARNING: [up] received packet with unknown status %u (size %u, modulation %u, BW %u, DR %u, RSSI %.1f)\n", p->status, p->size, p->modulation, p->bandwidth, p->datarate, p->rssi);
//					pthread_mutex_unlock(&mx_meas_up);
//					continue; /* skip that packet */
//					// exit(EXIT_FAILURE);
//			}
			meas_up_pkt_fwd += 1;
			meas_up_payload_byte += p->size;
			pthread_mutex_unlock(&mx_meas_up);

			/* Start of packet, add inter-packet separator if necessary */
			if (pkt_in_dgram == 0) {
				buff_up[buff_index] = '{';
				++buff_index;
			} else {
				buff_up[buff_index] = ',';
				buff_up[buff_index+1] = '{';
				buff_index += 2;
			}


			/* RAW timestamp, 8-17 useful chars */
			j = snprintf((char *)(buff_up + buff_index), TX_BUFF_SIZE-buff_index, "\"tmst\":%u", p->count_us);
			if (j > 0) {
				buff_index += j;
			} else {
				printf("ERROR: [up] snprintf failed line %u\n", (__LINE__ - 4));
				exit(EXIT_FAILURE);
			}

			/* Packet RX time (GPS based), 37 useful chars */
//			if (ref_ok == true) {
//				/* convert packet timestamp to UTC absolute time */
//				j = lgw_cnt2utc(local_ref, p->count_us, &pkt_utc_time);
//				if (j == LGW_GPS_SUCCESS) {
//					/* split the UNIX timestamp to its calendar components */
//					x = gmtime(&(pkt_utc_time.tv_sec));
//					j = snprintf((char *)(buff_up + buff_index), TX_BUFF_SIZE-buff_index, ",\"time\":\"%04i-%02i-%02iT%02i:%02i:%02i.%06liZ\"", (x->tm_year)+1900, (x->tm_mon)+1, x->tm_mday, x->tm_hour, x->tm_min, x->tm_sec, (pkt_utc_time.tv_nsec)/1000); /* ISO 8601 format */
//					if (j > 0) {
//						buff_index += j;
//					} else {
//						MSG("ERROR: [up] snprintf failed line %u\n", (__LINE__ - 4));
//						exit(EXIT_FAILURE);
//					}
//				}
//				/* convert packet timestamp to GPS absolute time */
//				j = lgw_cnt2gps(local_ref, p->count_us, &pkt_gps_time);
//				if (j == LGW_GPS_SUCCESS) {
//					pkt_gps_time_ms = pkt_gps_time.tv_sec * 1E3 + pkt_gps_time.tv_nsec / 1E6;
//					j = snprintf((char *)(buff_up + buff_index), TX_BUFF_SIZE-buff_index, ",\"tmms\":%llu",
//									pkt_gps_time_ms); /* GPS time in milliseconds since 06.Jan.1980 */
//					if (j > 0) {
//						buff_index += j;
//					} else {
//						MSG("ERROR: [up] snprintf failed line %u\n", (__LINE__ - 4));
//						exit(EXIT_FAILURE);
//					}
//				}
//			}

			/* Packet concentrator channel, RF chain & RX frequency, 34-36 useful chars */
			j = snprintf((char *)(buff_up + buff_index), TX_BUFF_SIZE-buff_index, ",\"chan\":%1u,\"rfch\":%1u,\"freq\":%.6lf", p->if_chain, p->rf_chain, ((double)p->freq_hz / 1e6));
			if (j > 0) {
				buff_index += j;
			} else {
				printf("ERROR: [up] snprintf failed line %u\n", (__LINE__ - 4));
				exit(EXIT_FAILURE);
			}

			/* Packet status, 9-10 useful chars */
			switch (p->status) {
				case STAT_CRC_OK:
					memcpy((void *)(buff_up + buff_index), (void *)",\"stat\":1", 9);
					buff_index += 9;
					break;
				case STAT_CRC_BAD:
					memcpy((void *)(buff_up + buff_index), (void *)",\"stat\":-1", 10);
					buff_index += 10;
					break;
				case STAT_NO_CRC:
					memcpy((void *)(buff_up + buff_index), (void *)",\"stat\":0", 9);
					buff_index += 9;
					break;
				default:
					printf("ERROR: [up] received packet with unknown status\n");
					memcpy((void *)(buff_up + buff_index), (void *)",\"stat\":?", 9);
					buff_index += 9;
					exit(EXIT_FAILURE);
			}

			/* Packet modulation, 13-14 useful chars */
			if (p->modulation == MOD_LORA) {
				memcpy((void *)(buff_up + buff_index), (void *)",\"modu\":\"LORA\"", 14);
				buff_index += 14;

				/* Lora datarate & bandwidth, 16-19 useful chars */
				switch (p->datarate) {
					case DR_LORA_SF7:
						memcpy((void *)(buff_up + buff_index), (void *)",\"datr\":\"SF7", 12);
						buff_index += 12;
						break;
					case DR_LORA_SF8:
						memcpy((void *)(buff_up + buff_index), (void *)",\"datr\":\"SF8", 12);
						buff_index += 12;
						break;
					case DR_LORA_SF9:
						memcpy((void *)(buff_up + buff_index), (void *)",\"datr\":\"SF9", 12);
						buff_index += 12;
						break;
					case DR_LORA_SF10:
						memcpy((void *)(buff_up + buff_index), (void *)",\"datr\":\"SF10", 13);
						buff_index += 13;
						break;
					case DR_LORA_SF11:
						memcpy((void *)(buff_up + buff_index), (void *)",\"datr\":\"SF11", 13);
						buff_index += 13;
						break;
					case DR_LORA_SF12:
						memcpy((void *)(buff_up + buff_index), (void *)",\"datr\":\"SF12", 13);
						buff_index += 13;
						break;
					default:
						printf("ERROR: [up] lora packet with unknown datarate\n");
						memcpy((void *)(buff_up + buff_index), (void *)",\"datr\":\"SF?", 12);
						buff_index += 12;
						exit(EXIT_FAILURE);
				}
				switch (p->bandwidth) {
					case BW_125KHZ:
						memcpy((void *)(buff_up + buff_index), (void *)"BW125\"", 6);
						buff_index += 6;
						break;
					case BW_250KHZ:
						memcpy((void *)(buff_up + buff_index), (void *)"BW250\"", 6);
						buff_index += 6;
						break;
					case BW_500KHZ:
						memcpy((void *)(buff_up + buff_index), (void *)"BW500\"", 6);
						buff_index += 6;
						break;
					default:
						printf("ERROR: [up] lora packet with unknown bandwidth\n");
						memcpy((void *)(buff_up + buff_index), (void *)"BW?\"", 4);
						buff_index += 4;
						exit(EXIT_FAILURE);
				}

				/* Packet ECC coding rate, 11-13 useful chars */
				switch (p->coderate) {
					case CR_LORA_4_5:
						memcpy((void *)(buff_up + buff_index), (void *)",\"codr\":\"4/5\"", 13);
						buff_index += 13;
						break;
					case CR_LORA_4_6:
						memcpy((void *)(buff_up + buff_index), (void *)",\"codr\":\"4/6\"", 13);
						buff_index += 13;
						break;
					case CR_LORA_4_7:
						memcpy((void *)(buff_up + buff_index), (void *)",\"codr\":\"4/7\"", 13);
						buff_index += 13;
						break;
					case CR_LORA_4_8:
						memcpy((void *)(buff_up + buff_index), (void *)",\"codr\":\"4/8\"", 13);
						buff_index += 13;
						break;
					case 0: /* treat the CR0 case (mostly false sync) */
						memcpy((void *)(buff_up + buff_index), (void *)",\"codr\":\"OFF\"", 13);
						buff_index += 13;
						break;
					default:
						printf("ERROR: [up] lora packet with unknown coderate\n");
						memcpy((void *)(buff_up + buff_index), (void *)",\"codr\":\"?\"", 11);
						buff_index += 11;
						exit(EXIT_FAILURE);
				}

				/* Lora SNR, 11-13 useful chars */
				j = snprintf((char *)(buff_up + buff_index), TX_BUFF_SIZE-buff_index, ",\"lsnr\":%.1f", p->snr);
				if (j > 0) {
					buff_index += j;
				} else {
					printf("ERROR: [up] snprintf failed line %u\n", (__LINE__ - 4));
					exit(EXIT_FAILURE);
				}
			}
			else if (p->modulation == MOD_FSK) {
				memcpy((void *)(buff_up + buff_index), (void *)",\"modu\":\"FSK\"", 13);
				buff_index += 13;

				/* FSK datarate, 11-14 useful chars */
				j = snprintf((char *)(buff_up + buff_index), TX_BUFF_SIZE-buff_index, ",\"datr\":%u", p->datarate);
				if (j > 0) {
					buff_index += j;
				} else {
					printf("ERROR: [up] snprintf failed line %u\n", (__LINE__ - 4));
					exit(EXIT_FAILURE);
				}
			}
			else
			{
				printf("ERROR: [up] received packet with unknown modulation\n");
				exit(EXIT_FAILURE);
			}

			/* Packet RSSI, payload size, 18-23 useful chars */
			j = snprintf((char *)(buff_up + buff_index), TX_BUFF_SIZE-buff_index, ",\"rssi\":%.0f,\"size\":%u", p->rssi, p->size);
			if (j > 0) {
				buff_index += j;
			} else {
				printf("ERROR: [up] snprintf failed line %u\n", (__LINE__ - 4));
				exit(EXIT_FAILURE);
			}

			/* Packet base64-encoded payload, 14-350 useful chars */
			memcpy((void *)(buff_up + buff_index), (void *)",\"data\":\"", 9);
			buff_index += 9;

			j = bin_to_b64(p->payload, p->size, (char *)(buff_up + buff_index), 341); /* 255 bytes = 340 chars in b64 + null char */
			if (j>=0) {
				buff_index += j;
			} else {
				printf("ERROR: [up] bin_to_b64 failed line %u\n", (__LINE__ - 5));
				exit(EXIT_FAILURE);
			}
			buff_up[buff_index] = '"';
			++buff_index;

			/* End of packet serialization */
			buff_up[buff_index] = '}';
			++buff_index;
			++pkt_in_dgram;
		}

		/* restart fetch sequence without sending empty JSON if all packets have been filtered out */
		if (pkt_in_dgram == 0) {
			if (send_report == true) {
				/* need to clean up the beginning of the payload */
				buff_index -= 8; /* removes "rxpk":[ */
			} else {
				/* all packet have been filtered out and no report, restart loop */
				continue;
			}
		} else {
			/* end of packet array */
			buff_up[buff_index] = ']';
			++buff_index;
			/* add separator if needed */
			if (send_report == true) {
				buff_up[buff_index] = ',';
				++buff_index;
			}
		}

		/* add status report if a new one is available */
		if (send_report == true) {
			pthread_mutex_lock(&mx_stat_rep);
			report_ready = false;
			j = snprintf((char *)(buff_up + buff_index), TX_BUFF_SIZE-buff_index, "%s", status_report);
			pthread_mutex_unlock(&mx_stat_rep);
			if (j > 0) {
				buff_index += j;
			} else {
				printf("ERROR: [up] snprintf failed line %u\n", (__LINE__ - 5));
				exit(EXIT_FAILURE);
			}
		}

		/* end of JSON datagram payload */
		buff_up[buff_index] = '}';
		++buff_index;
		buff_up[buff_index] = 0; /* add string terminator, for safety */

		printf("\nJSON up: %s\n", (char *)(buff_up + 12)); /* DEBUG: display JSON payload */

		/* send datagram to server */
		send(sock_up, (void *)buff_up, buff_index, 0);
		clock_gettime(CLOCK_MONOTONIC, &send_time);
		pthread_mutex_lock(&mx_meas_up);
		meas_up_dgram_sent += 1;
		meas_up_network_byte += buff_index;

		/* wait for acknowledge (in 2 times, to catch extra packets) */
		for (i=0; i<2; ++i) {
			j = recv(sock_up, (void *)buff_ack, sizeof buff_ack, 0);
			clock_gettime(CLOCK_MONOTONIC, &recv_time);
			if (j == -1) {
				if (errno == EAGAIN) { /* timeout */
					continue;
				} else { /* server connection error */
					break;
				}
			} else if ((j < 4) || (buff_ack[0] != PROTOCOL_VERSION) || (buff_ack[3] != PKT_PUSH_ACK)) {
				//MSG("WARNING: [up] ignored invalid non-ACL packet\n");
				continue;
			} else if ((buff_ack[1] != token_h) || (buff_ack[2] != token_l)) {
				//MSG("WARNING: [up] ignored out-of sync ACK packet\n");
				continue;
			} else {
				printf("INFO: [up] PUSH_ACK received in %i ms\n", (int)(1000 * difftimespec(recv_time, send_time)));
				meas_up_ack_rcv += 1;
				break;
			}
		}
		pthread_mutex_unlock(&mx_meas_up);
	}
	printf("\nINFO: End of upstream thread\n");
}

/* -------------------------------------------------------------------------- */
/* --- THREAD 2: POLLING SERVER AND ENQUEUING PACKETS IN JIT QUEUE ---------- */

void thread_down(void) {

	int i; /* loop variables */

	/* configuration and metadata for an outbound packet */
	struct lgw_pkt_tx_s txpkt;
	bool sent_immediate = false; /* option to sent the packet immediately */

	/* local timekeeping variables */
	struct timespec send_time; /* time of the pull request */
	struct timespec recv_time; /* time of return from recv socket call */

	/* data buffers */
	uint8_t buff_down[1000]; /* buffer to receive downstream packets */
	uint8_t buff_req[12]; /* buffer to compose pull requests */
	int msg_len;

	/* protocol variables */
	uint8_t token_h; /* random token for acknowledgement matching */
	uint8_t token_l; /* random token for acknowledgement matching */
	bool req_ack = false; /* keep track of whether PULL_DATA was acknowledged or not */

	/* JSON parsing variables */
	JSON_Value *root_val = NULL;
	JSON_Object *txpk_obj = NULL;
	JSON_Value *val = NULL; /* needed to detect the absence of some fields */
	const char *str; /* pointer to sub-strings in the JSON data */
	short x0, x1;
	uint64_t x2;
	double x3, x4;

	/* variables to send on GPS timestamp */
//	struct tref local_ref; /* time reference used for GPS <-> timestamp conversion */
//	struct timespec gps_tx; /* GPS time that needs to be converted to timestamp */

	/* beacon variables */
//	struct lgw_pkt_tx_s beacon_pkt;
//	uint8_t beacon_chan;
//	uint8_t beacon_loop;
//	size_t beacon_RFU1_size = 0;
//	size_t beacon_RFU2_size = 0;
//	uint8_t beacon_pyld_idx = 0;
//	time_t diff_beacon_time;
//	struct timespec next_beacon_gps_time; /* gps time of next beacon packet */
//	struct timespec last_beacon_gps_time; /* gps time of last enqueued beacon packet */
//	int retry;
//
//	/* beacon data fields, byte 0 is Least Significant Byte */
//	int32_t field_latitude; /* 3 bytes, derived from reference latitude */
//	int32_t field_longitude; /* 3 bytes, derived from reference longitude */
//	uint16_t field_crc1, field_crc2;

	/* auto-quit variable */
	uint32_t autoquit_cnt = 0; /* count the number of PULL_DATA sent since the latest PULL_ACK */

	/* Just In Time downlink */
	struct timeval current_unix_time;
	struct timeval current_concentrator_time;
	enum jit_error_e jit_result = JIT_ERROR_OK;
	enum jit_pkt_type_e downlink_type;

	/* set downstream socket RX timeout */
	i = setsockopt(sock_down, SOL_SOCKET, SO_RCVTIMEO, (void *)&pull_timeout_half, sizeof pull_timeout_half);
	if (i != 0) {
		printf("ERROR: [down] setsockopt returned %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}

	/* pre-fill the pull request buffer with fixed fields */
	buff_req[0] = PROTOCOL_VERSION;
	buff_req[3] = PKT_PULL_DATA;
	*(uint32_t *)(buff_req + 4) = net_mac_h;
	*(uint32_t *)(buff_req + 8) = net_mac_l;

	/* beacon variables initialization */
//	last_beacon_gps_time.tv_sec = 0;
//	last_beacon_gps_time.tv_nsec = 0;
//
//	/* beacon packet parameters */
//	beacon_pkt.tx_mode = ON_GPS; /* send on PPS pulse */
//	beacon_pkt.rf_chain = 0; /* antenna A */
//	beacon_pkt.rf_power = beacon_power;
//	beacon_pkt.modulation = MOD_LORA;
//	switch (beacon_bw_hz) {
//		case 125000:
//			beacon_pkt.bandwidth = BW_125KHZ;
//			break;
//		case 500000:
//			beacon_pkt.bandwidth = BW_500KHZ;
//			break;
//		default:
//			/* should not happen */
//			MSG("ERROR: unsupported bandwidth for beacon\n");
//			exit(EXIT_FAILURE);
//	}
//	switch (beacon_datarate) {
//		case 8:
//			beacon_pkt.datarate = DR_LORA_SF8;
//			beacon_RFU1_size = 1;
//			beacon_RFU2_size = 3;
//			break;
//		case 9:
//			beacon_pkt.datarate = DR_LORA_SF9;
//			beacon_RFU1_size = 2;
//			beacon_RFU2_size = 0;
//			break;
//		case 10:
//			beacon_pkt.datarate = DR_LORA_SF10;
//			beacon_RFU1_size = 3;
//			beacon_RFU2_size = 1;
//			break;
//		case 12:
//			beacon_pkt.datarate = DR_LORA_SF12;
//			beacon_RFU1_size = 5;
//			beacon_RFU2_size = 3;
//			break;
//		default:
//			/* should not happen */
//			MSG("ERROR: unsupported datarate for beacon\n");
//			exit(EXIT_FAILURE);
//	}
//	beacon_pkt.size = beacon_RFU1_size + 4 + 2 + 7 + beacon_RFU2_size + 2;
//	beacon_pkt.coderate = CR_LORA_4_5;
//	beacon_pkt.invert_pol = false;
//	beacon_pkt.preamble = 10;
//	beacon_pkt.no_crc = true;
//	beacon_pkt.no_header = true;
//
//	/* network common part beacon fields (little endian) */
//	for (i = 0; i < (int)beacon_RFU1_size; i++) {
//		beacon_pkt.payload[beacon_pyld_idx++] = 0x0;
//	}
//
//	/* network common part beacon fields (little endian) */
//	beacon_pyld_idx += 4; /* time (variable), filled later */
//	beacon_pyld_idx += 2; /* crc1 (variable), filled later */
//
//	/* calculate the latitude and longitude that must be publicly reported */
//	field_latitude = (int32_t)((reference_coord.lat / 90.0) * (double)(1<<23));
//	if (field_latitude > (int32_t)0x007FFFFF) {
//		field_latitude = (int32_t)0x007FFFFF; /* +90 N is represented as 89.99999 N */
//	} else if (field_latitude < (int32_t)0xFF800000) {
//		field_latitude = (int32_t)0xFF800000;
//	}
//	field_longitude = (int32_t)((reference_coord.lon / 180.0) * (double)(1<<23));
//	if (field_longitude > (int32_t)0x007FFFFF) {
//		field_longitude = (int32_t)0x007FFFFF; /* +180 E is represented as 179.99999 E */
//	} else if (field_longitude < (int32_t)0xFF800000) {
//		field_longitude = (int32_t)0xFF800000;
//	}
//
//	/* gateway specific beacon fields */
//	beacon_pkt.payload[beacon_pyld_idx++] = beacon_infodesc;
//	beacon_pkt.payload[beacon_pyld_idx++] = 0xFF &  field_latitude;
//	beacon_pkt.payload[beacon_pyld_idx++] = 0xFF & (field_latitude >>  8);
//	beacon_pkt.payload[beacon_pyld_idx++] = 0xFF & (field_latitude >> 16);
//	beacon_pkt.payload[beacon_pyld_idx++] = 0xFF &  field_longitude;
//	beacon_pkt.payload[beacon_pyld_idx++] = 0xFF & (field_longitude >>  8);
//	beacon_pkt.payload[beacon_pyld_idx++] = 0xFF & (field_longitude >> 16);
//
//	/* RFU */
//	for (i = 0; i < (int)beacon_RFU2_size; i++) {
//		beacon_pkt.payload[beacon_pyld_idx++] = 0x0;
//	}
//
//	/* CRC of the beacon gateway specific part fields */
//	field_crc2 = crc16((beacon_pkt.payload + 6 + beacon_RFU1_size), 7 + beacon_RFU2_size);
//	beacon_pkt.payload[beacon_pyld_idx++] = 0xFF &  field_crc2;
//	beacon_pkt.payload[beacon_pyld_idx++] = 0xFF & (field_crc2 >> 8);


    /* JIT queue initialization */
    jit_queue_init(&jit_queue);

	while (!exit_sig && !quit_sig) {
		/* generate random token for request */
		token_h = (uint8_t)rand(); /* random token */
		token_l = (uint8_t)rand(); /* random token */
		buff_req[1] = token_h;
		buff_req[2] = token_l;

		/* send PULL request and record time */
		send(sock_down, (void *)buff_req, sizeof buff_req, 0);
		clock_gettime(CLOCK_MONOTONIC, &send_time);
		pthread_mutex_lock(&mx_meas_dw);
		meas_dw_pull_sent += 1;
		pthread_mutex_unlock(&mx_meas_dw);
		req_ack = false;
		autoquit_cnt++;

		/* listen to packets and process them until a new PULL request must be sent */
		recv_time = send_time;
		 while ((int)difftimespec(recv_time, send_time) < keepalive_time) {

			/* try to receive a datagram */
			msg_len = recv(sock_down, (void *)buff_down, (sizeof buff_down)-1, 0);
			clock_gettime(CLOCK_MONOTONIC, &recv_time);

//			/* Pre-allocate beacon slots in JiT queue, to check downlink collisions */
//			beacon_loop = JIT_NUM_BEACON_IN_QUEUE - jit_queue.num_beacon;
//			retry = 0;
//			while (beacon_loop && (beacon_period != 0)) {
//				pthread_mutex_lock(&mx_timeref);
//				/* Wait for GPS to be ready before inserting beacons in JiT queue */
//				if ((gps_ref_valid == true) && (xtal_correct_ok == true)) {
//
//					/* compute GPS time for next beacon to come      */
//					/*   LoRaWAN: T = k*beacon_period + TBeaconDelay */
//					/*            with TBeaconDelay = [1.5ms +/- 1µs]*/
//					if (last_beacon_gps_time.tv_sec == 0) {
//						/* if no beacon has been queued, get next slot from current GPS time */
//						diff_beacon_time = time_reference_gps.gps.tv_sec % ((time_t)beacon_period);
//						next_beacon_gps_time.tv_sec = time_reference_gps.gps.tv_sec +
//														((time_t)beacon_period - diff_beacon_time);
//					} else {
//						/* if there is already a beacon, take it as reference */
//						next_beacon_gps_time.tv_sec = last_beacon_gps_time.tv_sec + beacon_period;
//					}
//					/* now we can add a beacon_period to the reference to get next beacon GPS time */
//					next_beacon_gps_time.tv_sec += (retry * beacon_period);
//					next_beacon_gps_time.tv_nsec = 0;
//
//#if DEBUG_BEACON
//					{
//					time_t time_unix;
//
//					time_unix = time_reference_gps.gps.tv_sec + UNIX_GPS_EPOCH_OFFSET;
//					MSG_DEBUG(DEBUG_BEACON, "GPS-now : %s", ctime(&time_unix));
//					time_unix = last_beacon_gps_time.tv_sec + UNIX_GPS_EPOCH_OFFSET;
//					MSG_DEBUG(DEBUG_BEACON, "GPS-last: %s", ctime(&time_unix));
//					time_unix = next_beacon_gps_time.tv_sec + UNIX_GPS_EPOCH_OFFSET;
//					MSG_DEBUG(DEBUG_BEACON, "GPS-next: %s", ctime(&time_unix));
//					}
//#endif
//
//					/* convert GPS time to concentrator time, and set packet counter for JiT trigger */
//					lgw_gps2cnt(time_reference_gps, next_beacon_gps_time, &(beacon_pkt.count_us));
//					pthread_mutex_unlock(&mx_timeref);
//
//					/* apply frequency correction to beacon TX frequency */
//					if (beacon_freq_nb > 1) {
//						beacon_chan = (next_beacon_gps_time.tv_sec / beacon_period) % beacon_freq_nb; /* floor rounding */
//					} else {
//						beacon_chan = 0;
//					}
//					/* Compute beacon frequency */
//					beacon_pkt.freq_hz = beacon_freq_hz + (beacon_chan * beacon_freq_step);
//
//					/* load time in beacon payload */
//					beacon_pyld_idx = beacon_RFU1_size;
//					beacon_pkt.payload[beacon_pyld_idx++] = 0xFF &  next_beacon_gps_time.tv_sec;
//					beacon_pkt.payload[beacon_pyld_idx++] = 0xFF & (next_beacon_gps_time.tv_sec >>  8);
//					beacon_pkt.payload[beacon_pyld_idx++] = 0xFF & (next_beacon_gps_time.tv_sec >> 16);
//					beacon_pkt.payload[beacon_pyld_idx++] = 0xFF & (next_beacon_gps_time.tv_sec >> 24);
//
//					/* calculate CRC */
//					field_crc1 = crc16(beacon_pkt.payload, 4 + beacon_RFU1_size); /* CRC for the network common part */
//					beacon_pkt.payload[beacon_pyld_idx++] = 0xFF & field_crc1;
//					beacon_pkt.payload[beacon_pyld_idx++] = 0xFF & (field_crc1 >> 8);
//
//					/* Insert beacon packet in JiT queue */
//					gettimeofday(&current_unix_time, NULL);
//					get_concentrator_time(&current_concentrator_time, current_unix_time);
//					jit_result = jit_enqueue(&jit_queue, &current_concentrator_time, &beacon_pkt, JIT_PKT_TYPE_BEACON);
//					if (jit_result == JIT_ERROR_OK) {
//						/* update stats */
//						pthread_mutex_lock(&mx_meas_dw);
//						meas_nb_beacon_queued += 1;
//						pthread_mutex_unlock(&mx_meas_dw);
//
//						/* One more beacon in the queue */
//						beacon_loop--;
//						retry = 0;
//						last_beacon_gps_time.tv_sec = next_beacon_gps_time.tv_sec; /* keep this beacon time as reference for next one to be programmed */
//
//						/* display beacon payload */
//						MSG("INFO: Beacon queued (count_us=%u, freq_hz=%u, size=%u):\n", beacon_pkt.count_us, beacon_pkt.freq_hz, beacon_pkt.size);
//						printf( "   => " );
//						for (i = 0; i < beacon_pkt.size; ++i) {
//							MSG("%02X ", beacon_pkt.payload[i]);
//						}
//						MSG("\n");
//					} else {
//						MSG_DEBUG(DEBUG_BEACON, "--> beacon queuing failed with %d\n", jit_result);
//						/* update stats */
//						pthread_mutex_lock(&mx_meas_dw);
//						if (jit_result != JIT_ERROR_COLLISION_BEACON) {
//							meas_nb_beacon_rejected += 1;
//						}
//						pthread_mutex_unlock(&mx_meas_dw);
//						/* In case previous enqueue failed, we retry one period later until it succeeds */
//						/* Note: In case the GPS has been unlocked for a while, there can be lots of retries */
//						/*       to be done from last beacon time to a new valid one */
//						retry++;
//						MSG_DEBUG(DEBUG_BEACON, "--> beacon queuing retry=%d\n", retry);
//					}
//				} else {
//					pthread_mutex_unlock(&mx_timeref);
//					break;
//				}
//			}

			/* if no network message was received, got back to listening sock_down socket */
			if (msg_len == -1) {
				//MSG("WARNING: [down] recv returned %s\n", strerror(errno)); /* too verbose */
				continue;
			}

			/* if the datagram does not respect protocol, just ignore it */
			if ((msg_len < 4) || (buff_down[0] != PROTOCOL_VERSION) || ((buff_down[3] != PKT_PULL_RESP) && (buff_down[3] != PKT_PULL_ACK))) {
				MSG("WARNING: [down] ignoring invalid packet len=%d, protocol_version=%d, id=%d\n",
						msg_len, buff_down[0], buff_down[3]);
				continue;
			}

			/* if the datagram is an ACK, check token */
			if (buff_down[3] == PKT_PULL_ACK) {
				if ((buff_down[1] == token_h) && (buff_down[2] == token_l)) {
					if (req_ack) {
						MSG("INFO: [down] duplicate ACK received :)\n");
					} else { /* if that packet was not already acknowledged */
						req_ack = true;
						autoquit_cnt = 0;
						pthread_mutex_lock(&mx_meas_dw);
						meas_dw_ack_rcv += 1;
						pthread_mutex_unlock(&mx_meas_dw);
						MSG("INFO: [down] PULL_ACK received in %i ms\n", (int)(1000 * difftimespec(recv_time, send_time)));
					}
				} else { /* out-of-sync token */
					MSG("INFO: [down] received out-of-sync ACK\n");
				}
				continue;
			}

			/* the datagram is a PULL_RESP */
			buff_down[msg_len] = 0; /* add string terminator, just to be safe */
			MSG("INFO: [down] PULL_RESP received  - token[%d:%d] :)\n", buff_down[1], buff_down[2]); /* very verbose */
			printf("\nJSON down: %s\n", (char *)(buff_down + 4)); /* DEBUG: display JSON payload */

			/* initialize TX struct and try to parse JSON */
			memset(&txpkt, 0, sizeof txpkt);
			root_val = json_parse_string_with_comments((const char *)(buff_down + 4)); /* JSON offset */
			if (root_val == NULL) {
				MSG("WARNING: [down] invalid JSON, TX aborted\n");
				continue;
			}

			/* look for JSON sub-object 'txpk' */
			txpk_obj = json_object_get_object(json_value_get_object(root_val), "txpk");
			if (txpk_obj == NULL) {
				MSG("WARNING: [down] no \"txpk\" object in JSON, TX aborted\n");
				json_value_free(root_val);
				continue;
			}

			/* Parse "immediate" tag, or target timestamp, or UTC time to be converted by GPS (mandatory) */
			i = json_object_get_boolean(txpk_obj,"imme"); /* can be 1 if true, 0 if false, or -1 if not a JSON boolean */
			if (i == 1) {
				/* TX procedure: send immediately */
				sent_immediate = true;
				downlink_type = JIT_PKT_TYPE_DOWNLINK_CLASS_C;
				MSG("INFO: [down] a packet will be sent in \"immediate\" mode\n");
			} else {
				sent_immediate = false;
				val = json_object_get_value(txpk_obj,"tmst");
				if (val != NULL) {
					/* TX procedure: send on timestamp value */
					txpkt.count_us = (uint32_t)json_value_get_number(val);

					/* Concentrator timestamp is given, we consider it is a Class A downlink */
					downlink_type = JIT_PKT_TYPE_DOWNLINK_CLASS_A;
				} else {
					/* TX procedure: send on GPS time (converted to timestamp value) */
					val = json_object_get_value(txpk_obj, "tmms");
					if (val == NULL) {
						MSG("WARNING: [down] no mandatory \"txpk.tmst\" or \"txpk.tmms\" objects in JSON, TX aborted\n");
						json_value_free(root_val);
						continue;
					}
//					if (gps_enabled == true) {
//						pthread_mutex_lock(&mx_timeref);
//						if (gps_ref_valid == true) {
//							local_ref = time_reference_gps;
//							pthread_mutex_unlock(&mx_timeref);
//						} else {
//							pthread_mutex_unlock(&mx_timeref);
//							MSG("WARNING: [down] no valid GPS time reference yet, impossible to send packet on specific GPS time, TX aborted\n");
//							json_value_free(root_val);
//
//							/* send acknoledge datagram to server */
//							send_tx_ack(buff_down[1], buff_down[2], JIT_ERROR_GPS_UNLOCKED);
//							continue;
//						}
//					} else {
//						MSG("WARNING: [down] GPS disabled, impossible to send packet on specific GPS time, TX aborted\n");
//						json_value_free(root_val);
//
//						/* send acknoledge datagram to server */
//						send_tx_ack(buff_down[1], buff_down[2], JIT_ERROR_GPS_UNLOCKED);
//						continue;
//					}

					/* Get GPS time from JSON */
//					x2 = (uint64_t)json_value_get_number(val);
//
//					/* Convert GPS time from milliseconds to timespec */
//					x3 = modf((double)x2/1E3, &x4);
//					gps_tx.tv_sec = (time_t)x4; /* get seconds from integer part */
//					gps_tx.tv_nsec = (long)(x3 * 1E9); /* get nanoseconds from fractional part */
//
//					/* transform GPS time to timestamp */
//					i = lgw_gps2cnt(local_ref, gps_tx, &(txpkt.count_us));
//					if (i != LGW_GPS_SUCCESS) {
//						MSG("WARNING: [down] could not convert GPS time to timestamp, TX aborted\n");
//						json_value_free(root_val);
//						continue;
//					} else {
//						MSG("INFO: [down] a packet will be sent on timestamp value %u (calculated from GPS time)\n", txpkt.count_us);
//					}

					/* GPS timestamp is given, we consider it is a Class B downlink */
					downlink_type = JIT_PKT_TYPE_DOWNLINK_CLASS_B;
				}
			}

			/* Parse "No CRC" flag (optional field) */
			val = json_object_get_value(txpk_obj,"ncrc");
			if (val != NULL) {
				txpkt.no_crc = (bool)json_value_get_boolean(val);
			}

			/* parse target frequency (mandatory) */
			val = json_object_get_value(txpk_obj,"freq");
			if (val == NULL) {
				MSG("WARNING: [down] no mandatory \"txpk.freq\" object in JSON, TX aborted\n");
				json_value_free(root_val);
				continue;
			}
			txpkt.freq_hz = (uint32_t)((double)(1.0e6) * 923.2f);
//			txpkt.freq_hz = (uint32_t)((double)(1.0e6) * json_value_get_number(val));

			/* parse RF chain used for TX (mandatory) */
			val = json_object_get_value(txpk_obj,"rfch");
			if (val == NULL) {
				MSG("WARNING: [down] no mandatory \"txpk.rfch\" object in JSON, TX aborted\n");
				json_value_free(root_val);
				continue;
			}
			txpkt.rf_chain = (uint8_t)0;
//			txpkt.rf_chain = (uint8_t)json_value_get_number(val);

			/* parse TX power (optional field) */
			val = json_object_get_value(txpk_obj,"powe");
			if (val != NULL) {
				txpkt.rf_power = 20;
//				txpkt.rf_power = (int8_t)json_value_get_number(val) - antenna_gain;
			}

			/* Parse modulation (mandatory) */
			str = json_object_get_string(txpk_obj, "modu");
			if (str == NULL) {
				MSG("WARNING: [down] no mandatory \"txpk.modu\" object in JSON, TX aborted\n");
				json_value_free(root_val);
				continue;
			}
			if (strcmp(str, "LORA") == 0) {
				/* Lora modulation */
				txpkt.modulation = MOD_LORA;

				/* Parse Lora spreading-factor and modulation bandwidth (mandatory) */
				str = json_object_get_string(txpk_obj, "datr");
				if (str == NULL) {
					MSG("WARNING: [down] no mandatory \"txpk.datr\" object in JSON, TX aborted\n");
					json_value_free(root_val);
					continue;
				}
				i = sscanf(str, "SF%2hdBW%3hd", &x0, &x1);
				if (i != 2) {
					MSG("WARNING: [down] format error in \"txpk.datr\", TX aborted\n");
					json_value_free(root_val);
					continue;
				}
				switch (x0) {
					case  7: txpkt.datarate = DR_LORA_SF7;  break;
					case  8: txpkt.datarate = DR_LORA_SF8;  break;
					case  9: txpkt.datarate = DR_LORA_SF9;  break;
					case 10: txpkt.datarate = DR_LORA_SF10; break;
					case 11: txpkt.datarate = DR_LORA_SF11; break;
					case 12: txpkt.datarate = DR_LORA_SF12; break;
					default:
						MSG("WARNING: [down] format error in \"txpk.datr\", invalid SF, TX aborted\n");
						json_value_free(root_val);
						continue;
				}
				switch (x1) {
					case 125: txpkt.bandwidth = BW_125KHZ; break;
					case 250: txpkt.bandwidth = BW_250KHZ; break;
					case 500: txpkt.bandwidth = BW_500KHZ; break;
					default:
						MSG("WARNING: [down] format error in \"txpk.datr\", invalid BW, TX aborted\n");
						json_value_free(root_val);
						continue;
				}

				/* Parse ECC coding rate (optional field) */
				str = json_object_get_string(txpk_obj, "codr");
				if (str == NULL) {
					MSG("WARNING: [down] no mandatory \"txpk.codr\" object in json, TX aborted\n");
					json_value_free(root_val);
					continue;
				}
				if      (strcmp(str, "4/5") == 0) txpkt.coderate = CR_LORA_4_5;
				else if (strcmp(str, "4/6") == 0) txpkt.coderate = CR_LORA_4_6;
				else if (strcmp(str, "2/3") == 0) txpkt.coderate = CR_LORA_4_6;
				else if (strcmp(str, "4/7") == 0) txpkt.coderate = CR_LORA_4_7;
				else if (strcmp(str, "4/8") == 0) txpkt.coderate = CR_LORA_4_8;
				else if (strcmp(str, "1/2") == 0) txpkt.coderate = CR_LORA_4_8;
				else {
					MSG("WARNING: [down] format error in \"txpk.codr\", TX aborted\n");
					json_value_free(root_val);
					continue;
				}

				/* Parse signal polarity switch (optional field) */
				val = json_object_get_value(txpk_obj,"ipol");
				if (val != NULL) {
					txpkt.invert_pol = (bool)json_value_get_boolean(val);
				}

				/* parse Lora preamble length (optional field, optimum min value enforced) */
				val = json_object_get_value(txpk_obj,"prea");
				if (val != NULL) {
					i = (int)json_value_get_number(val);
					if (i >= MIN_LORA_PREAMB) {
						txpkt.preamble = (uint16_t)i;
					} else {
						txpkt.preamble = (uint16_t)MIN_LORA_PREAMB;
					}
				} else {
					txpkt.preamble = (uint16_t)STD_LORA_PREAMB;
				}

			}
//			else if (strcmp(str, "FSK") == 0) {
//				/* FSK modulation */
//				txpkt.modulation = MOD_FSK;
//
//				/* parse FSK bitrate (mandatory) */
//				val = json_object_get_value(txpk_obj,"datr");
//				if (val == NULL) {
//					MSG("WARNING: [down] no mandatory \"txpk.datr\" object in JSON, TX aborted\n");
//					json_value_free(root_val);
//					continue;
//				}
//				txpkt.datarate = (uint32_t)(json_value_get_number(val));
//
//				/* parse frequency deviation (mandatory) */
//				val = json_object_get_value(txpk_obj,"fdev");
//				if (val == NULL) {
//					MSG("WARNING: [down] no mandatory \"txpk.fdev\" object in JSON, TX aborted\n");
//					json_value_free(root_val);
//					continue;
//				}
//				txpkt.f_dev = (uint8_t)(json_value_get_number(val) / 1000.0); /* JSON value in Hz, txpkt.f_dev in kHz */

//				/* parse FSK preamble length (optional field, optimum min value enforced) */
//				val = json_object_get_value(txpk_obj,"prea");
//				if (val != NULL) {
//					i = (int)json_value_get_number(val);
//					if (i >= MIN_FSK_PREAMB) {
//						txpkt.preamble = (uint16_t)i;
//					} else {
//						txpkt.preamble = (uint16_t)MIN_FSK_PREAMB;
//					}
//				} else {
//					txpkt.preamble = (uint16_t)STD_FSK_PREAMB;
//				}

//			}
			else {
				MSG("WARNING: [down] invalid modulation in \"txpk.modu\", TX aborted\n");
				json_value_free(root_val);
				continue;
			}

			/* Parse payload length (mandatory) */
			val = json_object_get_value(txpk_obj,"size");
			if (val == NULL) {
				MSG("WARNING: [down] no mandatory \"txpk.size\" object in JSON, TX aborted\n");
				json_value_free(root_val);
				continue;
			}
			txpkt.size = (uint16_t)json_value_get_number(val);

			/* Parse payload data (mandatory) */
			str = json_object_get_string(txpk_obj, "data");
			if (str == NULL) {
				MSG("WARNING: [down] no mandatory \"txpk.data\" object in JSON, TX aborted\n");
				json_value_free(root_val);
				continue;
			}
			i = b64_to_bin(str, strlen(str), txpkt.payload, sizeof txpkt.payload);
			if (i != txpkt.size) {
				MSG("WARNING: [down] mismatch between .size and .data size once converter to binary\n");
			}

			/* free the JSON parse tree from memory */
			json_value_free(root_val);

			/* select TX mode */
			if (sent_immediate) {
				txpkt.tx_mode = IMMEDIATE;
			} else {
				txpkt.tx_mode = TIMESTAMPED;
			}

			/* record measurement data */
			pthread_mutex_lock(&mx_meas_dw);
			meas_dw_dgram_rcv += 1; /* count only datagrams with no JSON errors */
			meas_dw_network_byte += msg_len; /* meas_dw_network_byte */
			meas_dw_payload_byte += txpkt.size;
			pthread_mutex_unlock(&mx_meas_dw);

			/* check TX parameter before trying to queue packet */
			jit_result = JIT_ERROR_OK;
//			if ((txpkt.freq_hz < tx_freq_min[txpkt.rf_chain]) || (txpkt.freq_hz > tx_freq_max[txpkt.rf_chain])) {
//				jit_result = JIT_ERROR_TX_FREQ;
//				MSG("ERROR: Packet REJECTED, unsupported frequency - %u (min:%u,max:%u)\n", txpkt.freq_hz, tx_freq_min[txpkt.rf_chain], tx_freq_max[txpkt.rf_chain]);
//			}
//			if (jit_result == JIT_ERROR_OK) {
//				for (i=0; i<txlut.size; i++) {
//					if (txlut.lut[i].rf_power == txpkt.rf_power) {
//						/* this RF power is supported, we can continue */
//						break;
//					}
//				}
//				if (i == txlut.size) {
//					/* this RF power is not supported */
//					jit_result = JIT_ERROR_TX_POWER;
//					printf("ERROR: Packet REJECTED, unsupported RF power for TX - %d\n", txpkt.rf_power);
//				}
//			}

			/* insert packet to be sent into JIT queue */
			if (jit_result == JIT_ERROR_OK) {
				gettimeofday(&current_unix_time, NULL);
//				get_concentrator_time(&current_concentrator_time, current_unix_time);
				jit_result = jit_enqueue(&jit_queue, &current_unix_time, &txpkt, downlink_type);
				if (jit_result != JIT_ERROR_OK) {
					printf("ERROR: Packet REJECTED (jit error=%d)\n", jit_result);
				}
				pthread_mutex_lock(&mx_meas_dw);
				meas_nb_tx_requested += 1;
				pthread_mutex_unlock(&mx_meas_dw);
			}

			/* Send acknoledge datagram to server */
			send_tx_ack(buff_down[1], buff_down[2], jit_result);
		}
	}
	MSG("\nINFO: End of downstream thread\n");
}

/* -------------------------------------------------------------------------- */
/* --- THREAD 3: CHECKING PACKETS TO BE SENT FROM JIT QUEUE AND SEND THEM --- */

void thread_jit(void) {
	int result = LGW_HAL_SUCCESS;
	struct lgw_pkt_tx_s pkt;
	int pkt_index = -1;
	struct timeval current_unix_time;
//	struct timeval current_concentrator_time;
	enum jit_error_e jit_result;
	enum jit_pkt_type_e pkt_type;
	uint8_t tx_status;

	while (!exit_sig && !quit_sig) {
		wait_ms(10);

		/* transfer data and metadata to the concentrator, and schedule TX */
		gettimeofday(&current_unix_time, NULL);
//		get_concentrator_time(&current_concentrator_time, current_unix_time);
		jit_result = jit_peek(&jit_queue, &current_unix_time, &pkt_index);
		if (jit_result == JIT_ERROR_OK) {
			if (pkt_index > -1) {
				jit_result = jit_dequeue(&jit_queue, pkt_index, &pkt, &pkt_type);
				if (jit_result == JIT_ERROR_OK) {
					/* update beacon stats */
//					if (pkt_type == JIT_PKT_TYPE_BEACON) {
//						/* Compensate breacon frequency with xtal error */
//						pthread_mutex_lock(&mx_xcorr);
//						pkt.freq_hz = (uint32_t)(xtal_correct * (double)pkt.freq_hz);
//						MSG_DEBUG(DEBUG_BEACON, "beacon_pkt.freq_hz=%u (xtal_correct=%.15lf)\n", pkt.freq_hz, xtal_correct);
//						pthread_mutex_unlock(&mx_xcorr);
//
//						/* Update statistics */
//						pthread_mutex_lock(&mx_meas_dw);
//						meas_nb_beacon_sent += 1;
//						pthread_mutex_unlock(&mx_meas_dw);
//						MSG("INFO: Beacon dequeued (count_us=%u)\n", pkt.count_us);
//					}

					/* check if concentrator is free for sending new packet */
//					pthread_mutex_lock(&mx_concent); /* may have to wait for a fetch to finish */
//					result = lgw_status(TX_STATUS, &tx_status);
//					pthread_mutex_unlock(&mx_concent); /* free concentrator ASAP */
//					if (result == LGW_HAL_ERROR) {
//						MSG("WARNING: [jit] lgw_status failed\n");
//					} else {
//						if (tx_status == TX_EMITTING) {
//							MSG("ERROR: concentrator is currently emitting\n");
//							print_tx_status(tx_status);
//							continue;
//						} else if (tx_status == TX_SCHEDULED) {
//							MSG("WARNING: a downlink was already scheduled, overwritting it...\n");
//							print_tx_status(tx_status);
//						} else {
//							/* Nothing to do */
//						}
//					}

					/* send packet to concentrator */
					pthread_mutex_lock(&mx_concent); /* may have to wait for a fetch to finish */
					result = lgw_send(pkt);
					pthread_mutex_unlock(&mx_concent); /* free concentrator ASAP */
					if (result == LGW_HAL_ERROR) {
						pthread_mutex_lock(&mx_meas_dw);
						meas_nb_tx_fail += 1;
						pthread_mutex_unlock(&mx_meas_dw);
						MSG("WARNING: [jit] lgw_send failed\n");
						continue;
					} else {
						pthread_mutex_lock(&mx_meas_dw);
						meas_nb_tx_ok += 1;
						pthread_mutex_unlock(&mx_meas_dw);
						MSG_DEBUG(DEBUG_PKT_FWD, "lgw_send done: count_us=%u\n", pkt.count_us);
					}
				} else {
					MSG("ERROR: jit_dequeue failed with %d\n", jit_result);
				}
			}
		} else if (jit_result == JIT_ERROR_EMPTY) {
			/* Do nothing, it can happen */
		} else {
			MSG("ERROR: jit_peek failed with %d\n", jit_result);
		}
	}
}
