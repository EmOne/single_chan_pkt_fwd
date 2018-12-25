# single_chan_pkt_fwd
# Single Channel LoRaWAN Gateway
### Application-specific constants

APP_NAME := single_chan_pkt_fwd

ARCH ?= arm
CROSS_COMPILE ?= arm-linux-gnueabi-

OBJDIR = obj
INCLUDES = $(wildcard *.h)

CC := $(CROSS_COMPILE)gcc
AR := $(CROSS_COMPILE)ar

CFLAGS=-c -Wall -g -O0 -Wall -Wextra -std=c99 -I. -I../wiringPi/wiringPi

### Linking options
LIBS=-lwiringPi -lrt -lpthread -lm

### General build targets

all: $(APP_NAME)

clean:
	rm -f $(OBJDIR)/*.o
	rm -f $(APP_NAME)

### Sub-modules compilation

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(OBJDIR)/%.o: src/%.c $(INCLUDES) | $(OBJDIR)
	$(CC) -c $(CFLAGS) $< -o $@

### Main program compilation and assembly

$(OBJDIR)/main.o: main.c $(LGW_INC) $(INCLUDES) | $(OBJDIR)
	$(CC) -c $(CFLAGS) $(VFLAG) $< -o $@

$(APP_NAME): $(OBJDIR)/main.o $(OBJDIR)/parson.o $(OBJDIR)/base64.o $(OBJDIR)/jitqueue.o $(OBJDIR)/loragw_hal.o  $(OBJDIR)/loragw_aux.o 
	$(CC) $< $(OBJDIR)/parson.o $(OBJDIR)/base64.o $(OBJDIR)/jitqueue.o $(OBJDIR)/loragw_hal.o $(OBJDIR)/loragw_aux.o -o $@ $(LIBS)

### EOF