CC?=gcc
CFLAGS?=-Wall -Wextra -Wpedantic -std=c11 -g

logmerger: logmerger.c
	$(CC) -o logmerger logmerger.c $(CFLAGS)

clean:
	rm -f logmerger

all: logmerger

.PHONY: clean all
