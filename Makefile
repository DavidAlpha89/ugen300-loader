CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wpedantic -std=c11
LIBUSB_CFLAGS := $(shell pkg-config --cflags libusb-1.0)
LIBUSB_LIBS   := $(shell pkg-config --libs libusb-1.0)
PREFIX  ?= /usr/local

ugen300-loader: src/ugen300_loader.c
	$(CC) $(CFLAGS) -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE $(LIBUSB_CFLAGS) -o $@ $< $(LIBUSB_LIBS)

install: ugen300-loader
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 ugen300-loader $(DESTDIR)$(PREFIX)/bin/

clean:
	rm -f ugen300-loader

.PHONY: install clean
