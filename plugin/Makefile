# Out-of-tree build for the bitwidth QEMU TCG plugin.
#
# QEMU_SRC must point at a QEMU source/install tree that provides
# include/qemu-plugin.h (either a built qemu-v9 prefix, or the qemu source
# checkout itself). Override on the command line if needed:
#   make QEMU_SRC=/path/to/qemu
QEMU_SRC ?= /home/sena/app/qemu-v9

CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2 -g -fPIC
PLUGIN_CFLAGS := $(shell pkg-config --cflags glib-2.0) -I$(QEMU_SRC)/include
PLUGIN_LIBS := $(shell pkg-config --libs glib-2.0)

SRCS := bitwidth.c decode.c
OBJS := $(SRCS:.c=.o)
TARGET := libbitwidth.so

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -shared -o $@ $^ $(PLUGIN_LIBS)

%.o: %.c decode.h
	$(CC) $(CFLAGS) $(PLUGIN_CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
