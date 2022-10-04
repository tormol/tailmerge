CC?=gcc
CFLAGS?=-Wall -Wextra -Wpedantic -std=c17 -g

tailmerge: tailmerge.c
	$(CC) -o tailmerge tailmerge.c $(CFLAGS)

uring: uring.c uring_reader.c utils.c
	$(CC) -o $@ $^ $(CFLAGS) -Wno-pointer-arith

clean:
	rm -f tailmerge uring

all: tailmerge uring

.PHONY: clean all
