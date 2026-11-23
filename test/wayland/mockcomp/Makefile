### Makefile for mockcomp, the scriptable test compositor for the
### native Wayland backend.  Run inside `nix develop`.

## This file is part of GNU Emacs, as modified by the native Wayland
## backend fork.  It is free software under
## the GNU General Public License, version 3 or later.

top_srcdir = ../../..
PROTOCOLS = $(top_srcdir)/src/wayland-protocols

PKG_CONFIG ?= pkg-config
WAYLAND_SCANNER ?= wayland-scanner
CFLAGS ?= -Og -g3
WARN_CFLAGS = -Wall -Wextra -Wno-unused-parameter

SERVER_PKGS = wayland-server xkbcommon cairo pixman-1
CLIENT_PKGS = wayland-client

all: mockcomp selftest

## Generated protocol glue.
%-server-protocol.h: $(PROTOCOLS)/%.xml
	$(WAYLAND_SCANNER) server-header $< $@
%-client-protocol.h: $(PROTOCOLS)/%.xml
	$(WAYLAND_SCANNER) client-header $< $@
%-protocol.c: $(PROTOCOLS)/%.xml
	$(WAYLAND_SCANNER) private-code $< $@

%-protocol.o: %-protocol.c
	$(CC) -c $(CFLAGS) $$($(PKG_CONFIG) --cflags wayland-server) $<

SERVER_PROTOCOLS = xdg-shell viewporter fractional-scale-v1 \
  xdg-output-unstable-v1 tablet-v2 cursor-shape-v1 text-input-unstable-v3 \
  xdg-activation-v1

mockcomp.o: mockcomp.c $(addsuffix -server-protocol.h,$(SERVER_PROTOCOLS))
	$(CC) -c $(WARN_CFLAGS) $(CFLAGS) \
	  $$($(PKG_CONFIG) --cflags $(SERVER_PKGS)) -I. $<
mockcomp: mockcomp.o $(addsuffix -protocol.o,$(SERVER_PROTOCOLS))
	$(CC) $(CFLAGS) -o $@ $^ $$($(PKG_CONFIG) --libs $(SERVER_PKGS))

selftest.o: selftest.c xdg-shell-client-protocol.h
	$(CC) -c $(WARN_CFLAGS) $(CFLAGS) \
	  $$($(PKG_CONFIG) --cflags $(CLIENT_PKGS)) -I. $<
selftest: selftest.o xdg-shell-protocol.o
	$(CC) $(CFLAGS) -o $@ $^ $$($(PKG_CONFIG) --libs $(CLIENT_PKGS))

## Check that mockcomp works and that its invariant checks fire.
check: mockcomp selftest
	./run-selftest.sh

clean:
	rm -f mockcomp selftest *.o xdg-shell-server-protocol.h \
	  xdg-shell-client-protocol.h *-protocol.c *-server-protocol.h

.PHONY: all check clean
