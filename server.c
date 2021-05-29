/*  Server stop and wait code - Writen: Hugh Smith    */

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#include "networks.h"
#include "srej.h"
#include "pollLib.h"
#include "window.h"

#include "cpe464.h"

typedef enum State STATE;

enum State
{
    START, DONE, FILENAME, SEND_DATA, WAIT_ON_ACK, TIMEOUT_ON_ACK,

    NEW_SEND_DATA, CHECK_FOR_RESPONSE, WAIT_ON_RESPONSE, TIMEOUT_ON_RESONSE, PROCESS_RR, PROCESS_SREJ,

    WAIT_ON_EOF_ACK, TIMEOUT_ON_EOF_ACK
};

void process_server(int serverSocketNumber);
void process_client(int32_t serverSocketNumber, uint8_t * buf, int32_t recv_len, Connection * client);
STATE filename(Connection * client, uint8_t * buf, int32_t recv_len, int32_t * data_file, int32_t * buf_size);
STATE send_data(Connection * client, uint8_t * packet, int32_t * packet_len, int32_t data_file, int32_t buf_size, uint32_t * seq_num);

STATE timeout_on_ack(Connection * client, uint8_t * packet, int32_t packet_len);
STATE wait_on_ack(Connection * client);

STATE wait_on_eof_ack(Connection * client);
STATE timeout_on_eof_ack(Connection * client, uint8_t * packet, int32_t packet_len);

int processArgs(int argc, char ** argv);
void handleZombies(int sig);


int main(int argc, char * argv[])
{
    int32_t  serverSocketNumber = 0;
    int portNumber = 0;

    portNumber = processArgs(argc, argv);

    sendtoErr_init(atof(argv[1]), DROP_ON, FLIP_ON, DEBUG_ON, RSEED_ON);
    setupPollSet();

    /*  set up the main server port  */
    serverSocketNumber = udpServerSetup(portNumber);

    process_server(serverSocketNumber);

    return 0;
}


void process_server(int serverSocketNumber)
{
    pid_t pid = 0;
    uint8_t  buf[MAX_LEN];
    Connection  * client = (Connection * )calloc(1, sizeof(Connection));
    uint8_t  flag = 0;
    uint32_t  seq_num = 0;
    int32_t  recv_len = 0;

    // We are going to fork() so need to clean up (SIGCHLD)
    signal(SIGCHLD, handleZombies);

    // get new client connection,  fork() child,
    while(1){
        // block waiting for a new client
        recv_len = recv_buf(buf, MAX_LEN, serverSocketNumber, client, &flag, &seq_num);

        if(recv_len != CRC_ERROR){
            if((pid = fork()) <0){
                perror("fork");
                exit(-1);
            }

            if(pid == 0){
                //  child process - a new process for each client
                printf("Child fork() - child pid: %d\n", getpid());
                process_client(serverSocketNumber, buf, recv_len, client);
                exit(0);
            }
        }
    }
}

STATE new_send_data(Connection * client, uint8_t * packet, int32_t * packet_len, int32_t data_file, int buf_size, uint32_t * seq_num)
{
    uint8_t  buf[MAX_LEN];
    int32_t  len_read = 0;
    STATE returnValue = DONE;

    if(win_isFull()){
        returnValue = WAIT_ON_RESPONSE;
        return returnValue;
    }

    len_read = read(data_file, buf, buf_size);

    switch(len_read) {
    case -1:
        perror("send_data,  read error");
        returnValue = DONE;
        break;

    case 0:
        (* packet_len) = send_buf(buf, 1, client, END_OF_FILE,  * seq_num, packet);
        returnValue = WAIT_ON_EOF_ACK;
        break;

    default:
        (* packet_len) = send_buf(buf, len_read, client, DATA,  * seq_num, packet);
        (* seq_num)++;
        returnValue = CHECK_FOR_RESPONSE;
        break;
    }

    return returnValue;
}

/* Helper function parsing flags for next state */
STATE next_state_from_response(uint8_t flag, uint32_t crc_check, STATE crc_state, char * func_name)
{
    STATE returnValue = NEW_SEND_DATA;

    // if crc error ignore packet
    if(crc_check == CRC_ERROR) {
        returnValue = crc_state;
    } else if(flag == RR) {
        returnValue = PROCESS_RR;
    } else if(flag == SREJ) {
        returnValue = PROCESS_SREJ;
    } else  {
        printf("In %s but its not an SREJ or RR flag (this should never happen) is: %d\n",func_name, flag);
        returnValue = DONE;
    }
    return returnValue;
}

/* Check for a response packet without blocking */
STATE check_for_response(Connection * client){
    STATE returnValue = NEW_SEND_DATA;
    uint32_t  crc_check = 0;
    uint8_t  buf[MAX_LEN];
    int32_t  len = MAX_LEN;
    uint8_t  flag = 0;
    uint32_t  seq_num = 0;

    if(pollCall(0) != TIMEOUT){
        crc_check = recv_buf(buf, len, client->sk_num, client, &flag, &seq_num);
        returnValue = next_state_from_response(flag, crc_check, NEW_SEND_DATA, "check_for_response");
    }
    return returnValue;
}

/* Check for a response packet and block */
STATE wait_on_response(Connection * client)
{
    STATE returnValue = DONE;
    uint32_t  crc_check = 0;
    uint8_t  buf[MAX_LEN];
    int32_t  len = MAX_LEN;
    uint8_t  flag = 0;
    uint32_t  seq_num = 0;
    static int retryCount = 0;

    if((returnValue = processSelect(client, &retryCount, TIMEOUT_ON_ACK, SEND_DATA, DONE))  == SEND_DATA)
    {
        crc_check = recv_buf(buf, len, client->sk_num, client, &flag, &seq_num);
        /* Run processSelect again, if invalid packet */
        returnValue = next_state_from_response(flag, crc_check, WAIT_ON_RESPONSE, "wait_on_response");
    }
    return returnValue;
}

/* Resend lowest packet: We do this by setting window.current to the lowest seq_num */
STATE timeout_on_response(Connection * client, uint8_t * packet, int32_t packet_len)
{
    safeSendto(packet, packet_len, client);
    return WAIT_ON_RESPONSE;
}

/* Set window to the new window size */
STATE process_rr(Connection * client, uint8_t * packet, int32_t * packet_len, int buf_size, uint32_t * seq_num)
{
    // window.RR() //
    return NEW_SEND_DATA;
}

STATE process_srej(Connection * client, uint8_t * packet, int32_t packet_len)
{
    /* Send packet with requested sequence number */
    safeSendto(packet, packet_len, client);
    return NEW_SEND_DATA;
}

/*
   Actually Recieve RR's and SREJ's
*/
STATE new_wait_on_eof_ack(Connection * client)
{
    STATE returnValue = DONE;
    uint32_t crc_check = 0;
    uint8_t buf[MAX_LEN];
    int32_t len = MAX_LEN;
    uint8_t flag = 0;
    uint32_t seq_num = 0;
    static int retryCount = 0;

    if((returnValue = processSelect(client, &retryCount, TIMEOUT_ON_EOF_ACK, DONE, DONE))  == DONE)
    {
        crc_check = recv_buf(buf, len, client->sk_num, client, &flag, &seq_num);

        // if crc error ignore packet
        if(crc_check == CRC_ERROR){
            returnValue = WAIT_ON_EOF_ACK;
        } else if(flag == EOF_ACK) {
            printf("File transfer completed successfully.\n");
            returnValue = DONE;
        } else if(flag == RR) {
            returnValue = PROCESS_RR;
        } else if(flag == SREJ) {
            returnValue = PROCESS_SREJ;
        } else {
            printf("In wait_on_eof_ack but its not an EOF_ACK flag (this should never happen) is: %d\n", flag);
            returnValue = DONE;
        }
    }
    return returnValue;
}

void process_client(int32_t serverSocketNumber, uint8_t * buf, int32_t recv_len, Connection  *  client){

    STATE state = START;
    int32_t  data_file = 0;
    int32_t  packet_len = 0;
    uint8_t  packet[MAX_LEN];
    int32_t  buf_size = 0;
    uint32_t  seq_num = START_SEQ_NUM;

    while(state != DONE)
    {

        switch(state) {
        case START:
            state = FILENAME;
            break;

        case FILENAME:
            state = filename(client, buf, recv_len, &data_file, &buf_size);
            break;

        case SEND_DATA:
            state = send_data(client, packet, &packet_len, data_file, buf_size, &seq_num);
            break;

        case WAIT_ON_ACK:
            state = wait_on_ack(client);
            break;

        case TIMEOUT_ON_ACK:
            state = timeout_on_ack(client, packet, packet_len);
            break;


        /* ETHAN'S */
        case NEW_SEND_DATA:
            state = new_send_data(client, packet, &packet_len, data_file, buf_size, &seq_num);
            break;

        case CHECK_FOR_RESPONSE:
            state = check_for_response(client);
            break;

        case WAIT_ON_RESPONSE:
            state = wait_on_response(client);
            break;

        case TIMEOUT_ON_RESONSE:
            /* Send Lowest packet, to break stall. return WAIT_ON_RESPONSE */
            state = timeout_on_response(client, packet, packet_len);
            break;

        case PROCESS_RR:
            state = process_rr(client, packet, packet_len);
            break;

        case PROCESS_SREJ:
            state = process_srej(client, packet, packet_len);
            break;
        /* ETHAN'S */


        case WAIT_ON_EOF_ACK:
            state = wait_on_eof_ack(client);
            break;

        case TIMEOUT_ON_EOF_ACK:
            state = timeout_on_eof_ack(client, packet, packet_len);
            break;

        case DONE:
            break;

        default:
            printf("In default and you should not be here!!!!\n");
            state = DONE;
            break;
        }
    }
}


STATE filename(Connection * client, uint8_t * buf, int32_t recv_len, int32_t * data_file, int32_t * buf_size)
{
    uint8_t response[1];
    char fname[MAX_LEN];
    STATE returnValue = DONE;

    // extract buffer sized used for sending data and also filename
    memcpy(buf_size, buf, SIZE_OF_BUF_SIZE);
    *buf_size = ntohl( * buf_size);
    memcpy(fname, &buf[sizeof( * buf_size)], recv_len-SIZE_OF_BUF_SIZE);

    /*  Create client socket to allow for processing this particular client  */
    client->sk_num = safeGetUdpSocket();
    addToPollSet(client->sk_num);

    if(((* data_file) = open(fname, O_RDONLY)) <0) {
        send_buf(response, 0, client, FNAME_BAD, 0, buf);
        returnValue = DONE;
    } else {
        send_buf(response, 0, client, FNAME_OK, 0, buf);
        returnValue = NEW_SEND_DATA;
    }
    return returnValue;
}


STATE send_data(Connection * client, uint8_t * packet, int32_t * packet_len, int32_t data_file, int buf_size, uint32_t * seq_num)
{
    uint8_t  buf[MAX_LEN];
    int32_t  len_read = 0;
    STATE returnValue = DONE;

    len_read = read(data_file, buf, buf_size);

    switch(len_read) {
    case -1:
        perror("send_data,  read error");
        returnValue = DONE;
        break;

    case 0:
        /* TODO: Change to send multiple */
        (* packet_len) = send_buf(buf, 1, client, END_OF_FILE,  * seq_num, packet);
        returnValue = WAIT_ON_EOF_ACK;
        break;

    default:
        /* TODO: Change to send multiple */
        (* packet_len) = send_buf(buf, len_read, client, DATA,  * seq_num, packet);
        (* seq_num)++;
        returnValue = WAIT_ON_ACK;
        break;
    }

    return returnValue;
}


STATE wait_on_ack(Connection * client)
{
    STATE returnValue = DONE;
    uint32_t  crc_check = 0;
    uint8_t  buf[MAX_LEN];
    int32_t  len = MAX_LEN;
    uint8_t  flag = 0;
    uint32_t  seq_num = 0;
    static int retryCount = 0;

    if((returnValue = processSelect(client, &retryCount, TIMEOUT_ON_ACK, SEND_DATA, DONE))  == SEND_DATA)
    {
        crc_check = recv_buf(buf, len, client->sk_num, client, &flag, &seq_num);

        // if crc error ignore packet
        if(crc_check == CRC_ERROR) {
            returnValue = WAIT_ON_ACK;
        } else if(flag != ACK) {
            printf("In wait_on_ack but its not an ACK flag (this should never happen) is: %d\n", flag);
            returnValue = DONE;
        }
    }
    return returnValue;
}

/*
   Actually Recieve RR's and SREJ's
*/
STATE wait_on_eof_ack(Connection * client)
{
    STATE returnValue = DONE;
    uint32_t crc_check = 0;
    uint8_t buf[MAX_LEN];
    int32_t len = MAX_LEN;
    uint8_t flag = 0;
    uint32_t seq_num = 0;
    static int retryCount = 0;

    if((returnValue = processSelect(client, &retryCount, TIMEOUT_ON_EOF_ACK, DONE, DONE))  == DONE)
    {
        crc_check = recv_buf(buf, len, client->sk_num, client, &flag, &seq_num);


        // if crc error ignore packet
        if(crc_check == CRC_ERROR){
            returnValue = WAIT_ON_EOF_ACK;
        } else if(flag != EOF_ACK) {
            printf("In wait_on_eof_ack but its not an EOF_ACK flag (this should never happen) is: %d\n", flag);
            returnValue = DONE;
        } else {
            printf("File transfer completed successfully.\n");
            returnValue = DONE;
        }
    }
    return returnValue;
}


STATE timeout_on_ack(Connection * client, uint8_t * packet, int32_t packet_len)
{
    safeSendto(packet, packet_len, client);
    return WAIT_ON_ACK;
}

STATE timeout_on_eof_ack(Connection * client, uint8_t * packet, int32_t packet_len)
{
    safeSendto(packet, packet_len, client);
    return WAIT_ON_EOF_ACK;
}

int processArgs(int argc, char ** argv){
    int portNumber = 0;

    if(argc<2||argc>3){
        printf("Usage %s error_rate [port number]\n", argv[0]);
        exit(-1);
    }

    if(argc == 3){
        portNumber = atoi(argv[2]);
    } else {
        portNumber = 0;
    }

    return portNumber;
}

// SIGCHLD handler - clean up terminated processes
void handleZombies(int sig)
{
int stat = 0;
while(waitpid(-1, &stat, WNOHANG)>0);
}



