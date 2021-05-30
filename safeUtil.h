//
// Writen by Hugh Smith, April 2020
//
// Put in system calls with error checking.

#ifndef __SAFEUTIL_H__
#define __SAFEUTIL_H__

struct sockaddr;

#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>

void * srealloc(void *ptr, size_t size);
void * sCalloc(size_t nmemb, size_t size);


#endif
