# hidws - generic WebSocket <-> USB HID gateway
#
# Native build on Ubuntu/Debian (host):
#   sudo apt install libhidapi-dev libwebsockets-dev
#   make
#
# Builds:
#   hidws     - WebSocket <-> HID bridge daemon (libwebsockets + hidapi-libusb)
#   hid-list  - enumerate USB HID devices and probe their reports (hidapi-libusb)
#
# Both binaries use the hidapi libusb backend and do not require kernel
# HID/INPUT support.

CC ?= gcc

CFLAGS ?= -Wall -Wextra -O2 -pthread -MMD -MP
LDLIBS_HIDWS   ?= -lhidapi-libusb -lwebsockets -lpthread
LDLIBS_HIDLIST ?= -lhidapi-libusb

PREFIX ?= /usr/local

PKG_CFLAGS := $(shell pkg-config --cflags hidapi-libusb libwebsockets 2>/dev/null)
PKG_LIBS_HIDWS   := $(shell pkg-config --libs hidapi-libusb libwebsockets 2>/dev/null)
PKG_LIBS_HIDLIST := $(shell pkg-config --libs hidapi-libusb 2>/dev/null)

TARGETS = hidws hid-list

all: $(TARGETS)

hidws: hidws.c
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -o $@ $< $(if $(PKG_LIBS_HIDWS),$(PKG_LIBS_HIDWS),$(LDLIBS_HIDWS))

hid-list: hid-list.c
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -o $@ $< $(if $(PKG_LIBS_HIDLIST),$(PKG_LIBS_HIDLIST),$(LDLIBS_HIDLIST))

clean:
	rm -f $(TARGETS) *.d

install: $(TARGETS)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(TARGETS) $(DESTDIR)$(PREFIX)/bin/

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/hidws $(DESTDIR)$(PREFIX)/bin/hid-list

-include $(wildcard *.d)

.PHONY: all clean install uninstall
