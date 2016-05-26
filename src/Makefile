#
# Makefile
#
# Author: Fernando Rodriguez
# Email: frodriguez.developer@outlook.com
# 
# Copyright 2014-2015 Fernando Rodriguez
# All rights reserved.
# 
# This code is distributed for educational purposes
# only. It may not be distributed without written 
# permission from the author.
# 

#CC=../tools/bin/i386-hydrax-gcc
#AS=../tools/bin/i386-hydrax-as
#LD=../tools/bin/i386-hydrax-ld
ECHOFLAGS=-e

ifneq ($(shell echo -e "Hello"),Hello)
	ECHOFLAGS=
endif

CFLAGS=-ggdb -std=gnu99 -fno-stack-protector -I/usr/include/directfb -Wunused-variable -Wall
ASFLAGS=-ggdb
LDFLAGS=-ldirectfb -pthread -lavutil -lavformat -lavcodec -lswscale -lavfilter -lz -lm


SOURCES=main.o \
	video.o \
	video-directfb.o \
	input.o \
	input-directfb.o \
	input-tcp.o \
	pipe_util.o \
	player-ffmpeg.o \
	shell.o \
	ui-menu.o \
	mainmenu.o \
	library.o

all: kernel

clean: 
	rm -f *.o 
	rm -f mediabox

kernel: $(SOURCES)
	@echo $(ECHOFLAGS) "  LINK\t\tmediabox"
	@$(CC) $(LDFLAGS) -o mediabox $(SOURCES)

.s.o:
	@echo $(ECHOFLAGS) "  AS\t\t"$<
	@$(AS) $(ASFLAGS) -c $< -o $@

.c.o:
	@echo $(ECHOFLAGS) "  CC\t\t"$<
	@$(CC) $(CFLAGS) -c $< -o $@

