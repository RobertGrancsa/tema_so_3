CC = gcc
CFLAGS = -fPIC -Wall -g -DDEBUG

objects := $(patsubst %.c,%.o,$(wildcard *.c))

.PHONY: build
build: aws

aws: $(objects) http-parser/http_parser.o
		$(CC) $(LDFLAGS) -o $@ $^ -laio

http-parser/http_parser.o: http-parser/http_parser.c http-parser/http_parser.h
	make -C http-parser/ http_parser.o

%.o: %.c
		$(CC) $(CFLAGS) -c $<

.PHONY: clean
clean:
		-rm -f *.o aws