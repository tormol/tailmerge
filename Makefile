CC?=gcc
CFLAGS?=-Wall -Wextra -Wpedantic -std=c11 -g

# only build the main program if no target is given
tailmerge: tailmerge.c
	$(CC) -o tailmerge tailmerge.c $(CFLAGS)

test_heap: test_heap.c heap.c
	$(CC) -o $@ $^ $(CFLAGS) -Wno-pointer-arith

test: test_heap test.sh
	./test.sh

all: tailmerge test_heap

clean:
	rm -f tailmerge test_heap

.PHONY: clean all test
