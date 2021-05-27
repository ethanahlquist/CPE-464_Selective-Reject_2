#include "window.h"
#include "safeUtil.h"
#include "pdu.h"

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
    uint32_t seq_num = getSeqPDU(pdu);
    uint16_t pdu_size = win.pdu_sizes[index];

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

int win_dist(uint32_t i1, uint32_t i2){

    i1 = win_index(i1);
    i2 = win_index(i2);
    if(i1 <= i2){
        return i2 - i1;
    } else {
        uint32_t dist_to_end = win.size - win_index(i1);
        uint32_t dist_start_to_i2 = win_index(i2);
        return dist_to_end + dist_start_to_i2;
    }
}

void win_set_lower(uint32_t val){
    win.lower = val;
    win.upper = val + win.size;
}

int win_isFull(){
    return win.current +1 >= win.upper;
}

/* Returns the difference between the starting value of current and end value
   0 - window full?
   1 - Normal function (NO SREJ)
   N - Send multiple SREJ
*/
int win_skipping_enQueue(uint8_t * pdu, uint16_t pduSize){

    uint32_t seq_num = getSeqPDU(pdu);
    uint32_t prev_current = win.current;

    if(win_isFull() || seq_num >= win.upper){
        printf("Full Queue (Enqueue)\n");
        return 0;
    }

    win.current = seq_num;

    /* Empty queue */
    if(win_isEmpty()){
        win_set_lower(win.current);
    }

    if(win_dist(win.current, seq_num) > win_dist(win.current, win.upper)){
        printf("This is not allowed\n");
        return -1;
    }

    memcpy(win.buffers[win_index(win.current)], pdu, pduSize);

    uint32_t num_moves = prev_current - win.current;
    if(num_moves == 0){
        printf("This should not happen\n");
    }

    win.pdu_sizes[win_index(win.current)] = pduSize;

    return num_moves;
}

void win_add(uint8_t *pdu, uint16_t pduSize){

    win_skipping_enQueue(pdu, pduSize);
}

int win_oneElement(){
    return win.lower == win.current;
}

int win_isEmpty(){
    return win.current == win.lower;
    return win.current == -1;
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

void win_SREJ(uint32_t srej){
    uint8_t *pdu = win_get(srej);
    uint32_t index = win_index(srej);
    uint32_t seq_num = getSeqPDU(pdu);
    uint16_t pdu_size = win.pdu_sizes[index];

    printf("Pdu from window: Seq Num: %d pduSize: %d\n", seq_num, pdu_size);
}

/*
uint8_t * win_deQueue(){

    int temp;

    if(win_isEmpty()){
        printf("Window is empty\n");
        return NULL;
    }

    if(win_oneElement()){
        win.lower = win.current;

    }

    return NULL;
}

*/
