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
    setupPollSet();

    processFile(argv);

    return 0;
}

STATE new_recv_data(int32_t output_file, Connection * server, uint32_t * clientSeqNum){
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

    if(flag == END_OF_FILE)
    {
        /*  send ACK  */
        send_buf(packet, 1, server, EOF_ACK, *clientSeqNum, packet);
        (*clientSeqNum)++;
        printf("File done\n");
        return DONE;
    }

    /* Send RR of lowest if seq_num is too small */
    if(seq_num < expected_seq_num){
        uint32_t rrSeqNum = htonl(win_getLower());
        send_buf((uint8_t *)&rrSeqNum, sizeof(rrSeqNum), server, RR, *clientSeqNum, packet);
        (*clientSeqNum)++;
        return RECV_DATA;
    }

    int32_t current_seq_num = win_add(data_buf, data_len);

    /* Send a SREJ for every packet skipped */
    int i;
    for (i = expected_seq_num; i < current_seq_num; ++i) {
        uint32_t srejSeqNum = htonl(win_getLower());
        send_buf((uint8_t *)&srejSeqNum, sizeof(srejSeqNum), server, SREJ, *clientSeqNum, packet);
        (*clientSeqNum)++;
    }

    expected_seq_num = current_seq_num;

    uint32_t pdu_len;
    uint8_t * dummy_flag = NULL;
    uint32_t * dummy_seq_num = NULL;
    while((pdu_len = win_deQueue(data_buf)) != 0) {
        data_len = retrieveHeader(data_buf, pdu_len, dummy_flag, dummy_seq_num);
        memcpy(data_buf, &data_buf[sizeof(Header)], data_len);
        write(output_file, &data_buf, data_len);
    }

    return RECV_DATA;
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
                state = start_state(argv, server, &clientSeqNum);
                break;

            case FILENAME:
                state = filename(argv[1], atoi(argv[4]), server);
                break;

            case FILE_OK:
                state = file_ok(&output_file_fd, argv[2]);
                break;

            case RECV_DATA:
                state = new_recv_data(output_file_fd, server, &clientSeqNum);
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

    // if we have connected to server before,  close it before reconnect
    if(server->sk_num>0){
        close(server->sk_num);
    }

    if(udpClientSetup(argv[6], atoi(argv[7]), server)<0){
        // error creating socket to server
        returnValue = DONE;
    } else {
        // put in buffer size (for sending data) and filename
        bufferSize = htonl(atoi(argv[4]));
        memcpy(buf, &bufferSize, SIZE_OF_BUF_SIZE);
        memcpy(&buf[SIZE_OF_BUF_SIZE], argv[1], fileNameLen);
        printIPv6Info(&server->remote);
        send_buf(buf, fileNameLen+SIZE_OF_BUF_SIZE, server, FNAME,  * clientSeqNum, packet);
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
            returnValue = FILE_OK;
        }
    }

    return (returnValue);
    }

STATE file_ok(int * outputFileFd, char * outputFileName){
    STATE returnValue = DONE;

    if(( * outputFileFd = open(outputFileName, O_CREAT|O_TRUNC|O_WRONLY, 0600)) <0){
        perror("File open error: ");
        /* TODO: Send an error packet to rcopy */
        returnValue = DONE;
    } else {
        returnValue = RECV_DATA;
    }
    return returnValue;
}


STATE recv_data(int32_t output_file, Connection * server, uint32_t * clientSeqNum){
    uint32_t seq_num = 0;
    uint32_t ackSeqNum = 0;
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

    if(flag == END_OF_FILE)
    {
        /*  send ACK  */
        send_buf(packet, 1, server, EOF_ACK,  * clientSeqNum, packet);
        ( * clientSeqNum)++;
        printf("File done\n");
        return DONE;
    } else {
        /*  send ACK  */
        ackSeqNum = htonl(seq_num);
        send_buf((uint8_t * )&ackSeqNum, sizeof(ackSeqNum), server, ACK, *clientSeqNum, packet);
        ( * clientSeqNum)++;
    }

    if(seq_num == expected_seq_num){
        expected_seq_num++;
        write(output_file, &data_buf, data_len);
    }

    return RECV_DATA;
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
