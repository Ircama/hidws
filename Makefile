# hidws - generic WebSocket <-> USB HID gateway
#
# Native build on Ubuntu/Debian (host):
#   sudo apt install libhidapi-dev libwebsockets-dev
#   make
#
# Builds:
#   hidws     - WebSocket <-> HID bridge daemon (libwebsockets + hidapi)
#   hid-list  - enumerate USB HID devices and probe their reports (hidapi)
#
# hidapi backend selection (BACKEND=libusb is the default):
#   make BACKEND=libusb   -> hidapi-libusb  (raw USB interrupt transfers;
#                            no kernel HID/INPUT support needed)
#   make BACKEND=hidraw   -> hidapi-hidraw  (talks to the kernel's /dev/hidraw
#                            node; the SAME path WebHID uses on Linux).
#
# Note: if a device answers WebHID (hidraw) but NOT libusb, rebuild with
#       BACKEND=hidraw to match WebHID's transport.

BACKEND ?= libusb

HIDAPI_PKG := hidapi-$(BACKEND)
ifeq ($(BACKEND),hidraw)
HIDAPI_LDLIBS := -lhidapi-hidraw
else
HIDAPI_LDLIBS := -lhidapi-libusb
endif

CC ?= gcc

CFLAGS ?= -Wall -Wextra -O2 -pthread -MMD -MP
LDLIBS_HIDWS   ?= $(HIDAPI_LDLIBS) -lwebsockets -lpthread
LDLIBS_HIDLIST ?= $(HIDAPI_LDLIBS)

PREFIX ?= /usr/local

PKG_CFLAGS := $(shell pkg-config --cflags $(HIDAPI_PKG) libwebsockets 2>/dev/null)
PKG_LIBS_HIDWS   := $(shell pkg-config --libs $(HIDAPI_PKG) libwebsockets 2>/dev/null)
PKG_LIBS_HIDLIST := $(shell pkg-config --libs $(HIDAPI_PKG) 2>/dev/null)

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
