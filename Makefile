#! GNU make

CC	= gcc
CCLD	= $(CC)
CFLAGS	= -O2 -g $(CWARNFLAGS)
CWARNFLAGS = -Wall
CPPFLAGS = $(if $(DEBUG),-DDEBUG)
CPPDEPFLAGS = -MMD -MF .deps/$(*F).d -MP
override CPPFLAGS += $(CPPDEPFLAGS)
LDFLAGS	=
XZ	= xz

# Executable suffix
X =

all: test-unlzma2$X

test-unlzma2$X: test-unlzma2.o uncompress_lzma2.o

%.o: %.c .deps/.stamp
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< $(OUTPUT_OPTION)

clean:
	rm -f test_decomp_lzma2 *.o
	rm -rf .deps

test: test-unlzma2$X
	$(if $(TESTDATA),\
	$(XZ) -F raw -c $(TESTDATA) | ./test-unlzma2 -v - | cmp $(TESTDATA) -,\
	$(error Specify test data with TESTDATA make variable))

.deps/.stamp:
	mkdir -p $(@D)
	@touch $@

-include .deps/*.d

.PHONY: all clean test
