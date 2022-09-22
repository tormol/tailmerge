CC?=gcc
CFLAGS?=-Wall -Wextra -Wpedantic -std=c11 -g

tailmerge: tailmerge.c
	$(CC) -o tailmerge tailmerge.c $(CFLAGS)

uring: uring.c
	$(CC) -o uring uring.c $(CFLAGS) -Wno-pointer-arith

clean:
	rm -f tailmerge uring

all: tailmerge uring

.PHONY: clean all
