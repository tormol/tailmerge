CC?=gcc
CFLAGS?=-Wall -Wextra -Wpedantic -std=c11 -g

tailmerge: tailmerge.c
	$(CC) -o tailmerge tailmerge.c $(CFLAGS)

clean:
	rm -f tailmerge

all: tailmerge

.PHONY: clean all
