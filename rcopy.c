/*  rcopy - client in stop and wait protocol  Writen: Hugh Smith   */

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/time.h>
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
    DONE, FILENAME, RECV_DATA, FILE_OK, START_STATE
};

void processFile(char * argv[]);
STATE start_state(char ** argv, Connection * server, uint32_t * clientSeqNum);
STATE filename(char * fname, int32_t buf_size, Connection * server);
STATE recv_data(int32_t output_file, Connection * server, uint32_t * clientSeqNum);
STATE file_ok(int * outputFileFd, char * outputFileName);
void check_args(int argc, char ** argv);

int main(int argc, char * argv[] )
{
    check_args(argc, argv);

    sendtoErr_init(atof(argv[5]), DROP_ON, FLIP_ON, DEBUG_ON, RSEED_ON);
    win_init("Client Window", atoi(argv[3]), atoi(argv[4]));
    setupPollSet();

    processFile(argv);

    return 0;
}

STATE dequeue_data(int32_t output_file, Connection *server, uint32_t * clientSeqNum){
    uint8_t data_buf[MAX_LEN];
    uint8_t packet[MAX_LEN];

    uint32_t pdu_len;
    while((pdu_len = win_deQueue(data_buf)) != 0) {
        Header *aHeader = (Header *)data_buf;
        int data_len = pdu_len - sizeof(Header);

        if(aHeader->flag == DATA){
            /* Write to out file */
            write(output_file, aHeader+1, data_len);
            /* Send an RR of Dequeued data */
            uint32_t rrSeqNum = aHeader->seq_num;
            send_buf((uint8_t *)&rrSeqNum, sizeof(rrSeqNum), server,
                    RR, *clientSeqNum, packet);
            (*clientSeqNum)++;

        } else if (aHeader->flag == END_OF_FILE) {
            /*  send ACK for EOF  */
            send_buf(packet, 1, server, EOF_ACK, *clientSeqNum, packet);
            (*clientSeqNum)++;
            printf("File done\n");
            return DONE;
        } else if (aHeader->flag == FILE_OK) {
            // Do nothing. This is just to initalize queue to correct seq
        }
    }
    return RECV_DATA;

}

STATE recv_data(int32_t output_file, Connection * server, uint32_t * clientSeqNum){
    uint32_t seq_num = 0;
    uint8_t flag = 0;
    int32_t data_len = 0;
    uint8_t data_buf[MAX_LEN];
    uint8_t packet[MAX_LEN];
    static int32_t expected_seq_num = START_SEQ_NUM;

    if(pollCall(LONG_TIME) == TIMEOUT){
        printf("Timeout after 10 seconds,  server must be gone.\n");
        return DONE;
    }

    data_len = recv_buf(data_buf, MAX_LEN, server->sk_num, server, &flag, &seq_num);

    /*  do state RECV_DATA again if there is a crc error (don't send ack,  don't write data)  */
    if(data_len == CRC_ERROR){
        return RECV_DATA;
    }

    /* Remake packet from data */
    int packet_len = 0;
    memmove(&packet[sizeof(Header)], data_buf, data_len);
    packet_len = createHeader(data_len, flag, seq_num, packet);

    /* Add packet to Window Queue */
    int32_t current_seq_num = win_add(packet, packet_len);

    /* Check if we need to send full window recovery RR
            - Check if the seq_num is the value we need next, so we
                can send the function to the following dequeue chunk
     */
    if(seq_num < expected_seq_num && seq_num != win_getLower()){
        uint32_t rrSeqNum = htonl(win_getLower());
        send_buf((uint8_t *)&rrSeqNum, sizeof(rrSeqNum), server, SREJ, *clientSeqNum, packet);
        (*clientSeqNum)++;
        return RECV_DATA;
    }

    /* Send a SREJ for every packet skipped */
    int i;
    for (i = expected_seq_num; i < current_seq_num; ++i) {
        uint32_t srejSeqNum = htonl(i);
        send_buf((uint8_t *)&srejSeqNum, sizeof(srejSeqNum), server,
                SREJ, *clientSeqNum, packet);
        (*clientSeqNum)++;
    }

    expected_seq_num = current_seq_num+1;

    /* For each popped of the queue if:
          Data - send an RR and write to file
          EOF - send EOF_ACK
          FILE_OK - Do nothing
    */
    return dequeue_data(output_file, server, clientSeqNum);
}


void processFile(char * argv[])
{
    // argv needed to get file names,  server name and server port number
    Connection * server = (Connection * )calloc(1, sizeof(Connection));
    uint32_t clientSeqNum = 0;
    int32_t output_file_fd = 0;
    STATE state = START_STATE;

    while(state != DONE){
        switch(state)
        {
            case START_STATE:
                printf("[START_STATE]:\n");
                state = start_state(argv, server, &clientSeqNum);
                break;

            case FILENAME:
                printf("[FILENAME]:\n");
                state = filename(argv[1], atoi(argv[4]), server);
                break;

            case FILE_OK:
                printf("[FILE_OK]:\n");
                state = file_ok(&output_file_fd, argv[2]);
                break;

            case RECV_DATA:
                printf("[RECV_DATA]:\n");
                state = recv_data(output_file_fd, server, &clientSeqNum);
                break;

            case DONE:
                break;

            default:
                printf("ERROR - in default state\n");
                break;
        }
    }
}


STATE start_state(char ** argv, Connection * server, uint32_t * clientSeqNum){
    // Returns FILENAME if no error,  otherwise DONE (to many connects,  cannot connect to sever)
    uint8_t packet[MAX_LEN];
    uint8_t buf[MAX_LEN];
    int fileNameLen = strlen(argv[1]);
    STATE returnValue = FILENAME;
    uint32_t bufferSize = 0;
    uint32_t windowSize = 0;

    // if we have connected to server before,  close it before reconnect
    if(server->sk_num>0){
        close(server->sk_num);
    }

    if(udpClientSetup(argv[6], atoi(argv[7]), server)<0){
        // error creating socket to server
        returnValue = DONE;
    } else {
        // put in buffer size (for sending data) and filename
        windowSize = htonl(atoi(argv[3]));
        bufferSize = htonl(atoi(argv[4]));
        memcpy(buf, &windowSize, SIZE_OF_WIN_SIZE);
        memcpy(&buf[SIZE_OF_WIN_SIZE], &bufferSize, SIZE_OF_BUF_SIZE);
        memcpy(&buf[SIZE_OF_WIN_SIZE + SIZE_OF_BUF_SIZE], argv[1], fileNameLen);
        printIPv6Info(&server->remote);
        send_buf(buf, fileNameLen + SIZE_OF_WIN_SIZE + SIZE_OF_BUF_SIZE,
                server, FNAME,  * clientSeqNum, packet);
        addToPollSet(server->sk_num);

        (*clientSeqNum)++;

        returnValue = FILENAME;
    }

    return returnValue;
}

STATE filename(char * fname, int32_t buf_size, Connection * server){
    // Send the file name,  get response
    // return START_STATE if no reply from server,  DONE if bad filename,  FILE_OK otherwise
    int returnValue = START_STATE;
    uint8_t packet[MAX_LEN];
    uint8_t flag = 0;
    uint32_t seq_num = 0;
    int32_t recv_check = 0;
    static int retryCount = 0;

    if((returnValue = processSelect(server, &retryCount, START_STATE, FILE_OK, DONE)) == FILE_OK)
    {
        /* TODO: Make resend filename */
        recv_check = recv_buf(packet, MAX_LEN, server->sk_num, server, &flag, &seq_num);

        /*  check for bit flip   */
        if(recv_check == CRC_ERROR) {
            returnValue = START_STATE;
        } else if(flag == FNAME_BAD) {
            printf("File %s not found\n", fname);
            returnValue = DONE;
        } else if(flag == DATA) {
            // file yes/no packet lost - instead its a data packet
            returnValue = FILENAME;
        }
    }

    return (returnValue);
    }

STATE file_ok(int * outputFileFd, char * outputFileName){
    STATE returnValue = DONE;

    uint8_t buf[MAX_LEN];
    Header aHeader;
    aHeader.seq_num = START_SEQ_NUM - 1;
    aHeader.flag = FNAME_OK;

    win_add((uint8_t *)&aHeader, sizeof(Header));
    win_deQueue(buf);

    if(( * outputFileFd = open(outputFileName, O_CREAT|O_TRUNC|O_WRONLY, 0600)) <0){
        perror("File open error: ");
        /* TODO: Send an error packet to rcopy */
        returnValue = DONE;
    } else {
        returnValue = RECV_DATA;
    }
    return returnValue;
}


void check_args(int argc, char ** argv)
{

    if(argc != 8)
    {
        printf("Usage %s fromFile toFile window-size buffer_size error_rate hostname port\n", argv[0]);
        exit(-1);
    }

    if(strlen(argv[1]) > 1000)
    {
        printf("FROM filename to long needs to be less than 1000 and is: %ld\n", strlen(argv[1]));
        exit(-1);
    }

    if(strlen(argv[2]) > 1000)
    {
        printf("TO filename to long needs to be less than 1000 and is: %ld\n", strlen(argv[1]));
        exit(-1);
    }

    if(atoi(argv[3]) < 1||atoi(argv[3]) > (1<<30))
    {
        printf("Window size needs to be between 1 and 2^30 and is: %d\n", atoi(argv[3]));
        exit(-1);
    }

    if(atoi(argv[4]) < 400||atoi(argv[4]) > 1400)
    {
        printf("Buffer size needs to be between 400 and 1400 and is: %d\n", atoi(argv[3]));
        exit(-1);
    }

    if(atoi(argv[5]) <0||atoi(argv[5]) >= 1)
    {
        printf("Error rate needs to be between 0 and less then 1 and is: %d\n", atoi(argv[4]));
        exit(-1);
    }
}
