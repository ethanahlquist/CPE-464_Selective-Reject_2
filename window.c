#include "window.h"
#include "safeUtil.h"
#include "srej.h"

struct win {
    char name[MAX_NAME];
    int32_t lower;
    int32_t current;
    int32_t upper;
    int32_t size;
    uint16_t buffer_size;
    uint8_t  ** buffers;
    uint16_t *pdu_sizes;
} win;


void win_init(const char *name, int32_t window_size, uint16_t buffer_size){

    /* Check argument sizes */
    if(buffer_size > 1400){
        fprintf(stderr, "Buffer size too big");
        exit(1);
    } else if(buffer_size < 1){
        fprintf(stderr, "Buffer size too small");
        exit(1);
    }

    if(window_size > ((int32_t)1) << 30) {
        fprintf(stderr, "Window size too big");
        exit(1);
    } else if(window_size < 1){
        fprintf(stderr, "Window size too small");
        exit(1);
    }

    win.buffer_size = buffer_size;
    win.size = window_size;

    /* Copy name of Window into struct (for print commands) */
    strncpy(win.name, name, MAX_NAME);

    /* Create buffer array */
    win.buffers = sCalloc(window_size, sizeof(uint8_t *));
    int i;
    for (i = 0; i < window_size; ++i) {
        win.buffers[i] = sCalloc(buffer_size, sizeof(uint8_t));
    }

    /* Create array to hold pdu sizes */
    win.pdu_sizes = sCalloc(window_size, sizeof(uint16_t));

    /* Init lower, upper, and current pointers */

    win.lower = 0;
    win.current = -1;
    //win.current = 0;

    win.upper = win.lower + win.size;
}

void win_metadata(){

    printf("%s - ", win.name);
    printf("Window Size: %d, ", win.size);
    printf("lower: %d, ", win.lower);
    printf("Upper: %d, ", win.upper);
    printf("Current: %d, ", win.current +1);
    printf("window open?: %d\n", !win_isFull());

}

/* Print single index of form "0 sequenceNumber: 16 pduSize: 12" */
void win_print_index(uint32_t index){

    uint8_t *pdu = win_get(index);
    uint16_t pdu_size = win.pdu_sizes[index];
    uint32_t seq_num = getSeqPDU(pdu);

    if(pdu_size == 0){
        printf("\t%d not valid\n", index);
    } else {
        printf("\t%d sequenceNumber: %d pduSize: %d\n", index, seq_num, pdu_size);
    }
}

/* Print all indexes */
void win_print(){

    printf("Window Size is: %d\n", win.size);
    int i;
    for (i = 0; i < win.size; ++i) {
        win_print_index(i);
    }
    printf("\n");

}

/* Get index from sequence Number */
uint32_t win_index(uint32_t seq_num){

    uint32_t index = seq_num % win.size;
    return index;
}

/* Get pdu from sequenceNumber */
uint8_t * win_get(uint32_t seq_num){

    uint32_t index = win_index(seq_num);
    return win.buffers[index];
}

/* Get pdu size from sequenceNumber */
uint16_t win_getSize(uint32_t seq_num){

    uint32_t index = win_index(seq_num);
    return win.pdu_sizes[index];
}


void win_set_lower(uint32_t val){
    win.lower = val;
    win.upper = val + win.size;
}

uint32_t win_getLower(){
    return win.lower;
}

int win_isFull(){
    return win.current +1 >= win.upper;
}

int win_cellEmpty(int seq_num){
    int index = win_index(seq_num);
    return win.pdu_sizes[index] == 0;
}

int win_lowest_cell_isFull(){
    int index = win_index(win.lower);
    return win.pdu_sizes[index] != 0;
}

int win_seqInFrame(int seq_num){
    return (seq_num >= win.lower) && (seq_num < win.upper);
}

int win_isEmpty(){
    return win.current == win.lower;
}

void win_RR(uint32_t rr){
    if(rr > win.lower){
        int i;
        for (i = 0; i < win.size; ++i) {
            uint32_t seq_num = getSeqPDU(win.buffers[i]);
            if (seq_num < rr) {
                win.pdu_sizes[i] = 0;
            }
        }
        win_set_lower(rr);
    }
}

void win_SREJ(uint32_t srej)
{
    uint8_t *pdu = win_get(srej);
    uint32_t index = win_index(srej);
    uint16_t pdu_size = win.pdu_sizes[index];

    uint32_t seq_num = getSeqPDU(pdu);

    printf("Pdu from window: Seq Num: %d pduSize: %d\n", seq_num, pdu_size);
}

/* Returns the current sequenceNumber, so it can be compared with the expected
 * in order to send SREJs
 */
int32_t win_add(uint8_t * pdu, uint16_t pduSize)
{
    uint32_t seq_num = getSeqPDU(pdu);

    if (seq_num < win.lower){
        printf("Sequence is lower that window range\n");
        return TOO_LOW;
    } else if (seq_num >= win.upper){
        win.current = win.upper-1;
        printf("Sequence is higher that window range\n");
        return TOO_HIGH;
    } else if (!win_cellEmpty(seq_num)){
        printf("Cell already filled\n");
        return FULL_CELL;
    }

    if(win.current == -1){
        win.current = seq_num;
        win_set_lower(seq_num);
    }

    if((int)seq_num >= win.current){
        win.current = seq_num;
    }

    memcpy(win.buffers[win_index(seq_num)], pdu, pduSize);
    win.pdu_sizes[win_index(seq_num)] = pduSize;

    win_metadata();
    return win.current;
}

/* Only pops off when the bottom cell is full */
uint16_t win_deQueue(uint8_t * buf)
{
    if(!win_lowest_cell_isFull()){
        printf("Next cell is empty\n");
        return 0;
    }

    /* get info from cell */
    uint8_t *pdu = win_get(win.lower);
    uint16_t pduSize = win.pdu_sizes[win_index(win.lower)];

    /* Assume buf size has not changed since initialization */
    memcpy(buf, pdu, pduSize);
    win_RR(win.lower + 1);

    return pduSize;
}

