#! GNU make

CC	= gcc
CCLD	= $(CC)
CFLAGS	= -O2 -Wall -g
CPPFLAGS = $(if $(DEBUG),-DDEBUG)
CPPDEPFLAGS = -MMD -MF .deps/$(*F).d -MP
override CPPFLAGS += $(CPPDEPFLAGS)
LDFLAGS	=

all: uncompress_lzma2.o

%.o: %.c .deps/.stamp
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< $(OUTPUT_OPTION)

clean:
	rm -f test_decomp_lzma2 *.o
	rm -rf .deps

.deps/.stamp:
	mkdir -p $(@D)
	@touch $@

-include .deps/*.d

.PHONY: all clean
