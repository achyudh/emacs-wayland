### Makefile for the capture observer and its reference client.  Run
### inside `nix develop`.

## This file is part of GNU Emacs, as modified by the native Wayland
## backend fork.  It is free software under
## the GNU General Public License, version 3 or later.

top_srcdir = ../../..
PROTOCOLS = protocols
BACKEND_PROTOCOLS = $(top_srcdir)/src/wayland-protocols

PKG_CONFIG ?= pkg-config
WAYLAND_SCANNER ?= wayland-scanner
CFLAGS ?= -Og -g3
WARN_CFLAGS = -Wall -Wextra -Wno-unused-parameter

all: observer refclient vkbd vptr

%-client-protocol.h: $(PROTOCOLS)/%.xml
	$(WAYLAND_SCANNER) client-header $< $@
%-protocol.c: $(PROTOCOLS)/%.xml
	$(WAYLAND_SCANNER) private-code $< $@
# The reference client uses protocols the backend also uses.
REFCLIENT_PROTOCOLS = xdg-shell viewporter fractional-scale-v1
$(addsuffix -client-protocol.h,$(REFCLIENT_PROTOCOLS)): %-client-protocol.h: \
  $(BACKEND_PROTOCOLS)/%.xml
	$(WAYLAND_SCANNER) client-header $< $@
$(addsuffix -protocol.c,$(REFCLIENT_PROTOCOLS)): %-protocol.c: \
  $(BACKEND_PROTOCOLS)/%.xml
	$(WAYLAND_SCANNER) private-code $< $@

%-protocol.o: %-protocol.c
	$(CC) -c $(CFLAGS) $$($(PKG_CONFIG) --cflags wayland-client) $<

OBSERVER_PROTOCOLS = wlr-screencopy-unstable-v1 virtual-keyboard-unstable-v1

observer.o: observer.c $(addsuffix -client-protocol.h,$(OBSERVER_PROTOCOLS))
	$(CC) -c $(WARN_CFLAGS) $(CFLAGS) \
	  $$($(PKG_CONFIG) --cflags wayland-client xkbcommon) -I. $<
observer: observer.o $(addsuffix -protocol.o,$(OBSERVER_PROTOCOLS))
	$(CC) $(CFLAGS) -o $@ $^ \
	  $$($(PKG_CONFIG) --libs wayland-client xkbcommon)

vkbd.o: vkbd.c virtual-keyboard-unstable-v1-client-protocol.h
	$(CC) -c $(WARN_CFLAGS) $(CFLAGS) \
	  $$($(PKG_CONFIG) --cflags wayland-client xkbcommon) -I. $<
vkbd: vkbd.o virtual-keyboard-unstable-v1-protocol.o
	$(CC) $(CFLAGS) -o $@ $^ \
	  $$($(PKG_CONFIG) --libs wayland-client xkbcommon)

vptr.o: vptr.c wlr-virtual-pointer-unstable-v1-client-protocol.h
	$(CC) -c $(WARN_CFLAGS) $(CFLAGS) \
	  $$($(PKG_CONFIG) --cflags wayland-client) -I. $<
vptr: vptr.o wlr-virtual-pointer-unstable-v1-protocol.o
	$(CC) $(CFLAGS) -o $@ $^ $$($(PKG_CONFIG) --libs wayland-client)

refclient.o: refclient.c $(addsuffix -client-protocol.h,$(REFCLIENT_PROTOCOLS))
	$(CC) -c $(WARN_CFLAGS) $(CFLAGS) \
	  $$($(PKG_CONFIG) --cflags wayland-client) -I. $<
refclient: refclient.o $(addsuffix -protocol.o,$(REFCLIENT_PROTOCOLS))
	$(CC) $(CFLAGS) -o $@ $^ $$($(PKG_CONFIG) --libs wayland-client)

clean:
	rm -f observer refclient vkbd vptr *.o *-protocol.c *-client-protocol.h

.PHONY: all clean
