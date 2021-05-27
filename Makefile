# udpCode makefile
# written by Hugh Smith - Feb 2021

CC = gcc
CFLAGS = -g -Wall


SRC = networks.c srej.c gethostbyname.c pollLib.c safeUtil.c
#SRC = networks.c  gethostbyname.c safeUtil.c pdu.c pollLib.c
OBJS = networks.o srej.o gethostbyname.o pollLib.o safeUtil.o

#uncomment next two lines if your using sendtoErr() library
LIBS += libcpe464.2.21.a -lstdc++ -ldl
CFLAGS += -D__LIBCPE464_

all:  rcopy server

rcopy: rcopy.c $(OBJS)
	$(CC) $(CFLAGS) -o rcopy rcopy.c $(OBJS) $(LIBS)

server: server.c $(OBJS)
	$(CC) $(CFLAGS) -o server server.c  $(OBJS) $(LIBS)


%.o: %.c *.h
	gcc -c $(CFLAGS) $< -o $@

cleano:
	rm -f *.o

clean:
	rm -f server rcopy *.o
