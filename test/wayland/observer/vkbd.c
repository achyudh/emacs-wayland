/* vkbd -- a long-lived virtual keyboard for tests.

This file is part of GNU Emacs, as modified by the native Wayland
backend fork.  It is free software under
the GNU General Public License, version 3 or later.

It creates one virtual keyboard with a "us" keymap and keeps it until
stdin closes, so the seat has a keyboard before an input method grabs
it, as with a real one.  Each line of stdin is a list of evdev codes,
pressed and released in turn; "ok" is printed after each line.  A code
written +CODE is only pressed and -CODE only released, to hold a
modifier across other input.  wlroots does not derive modifiers from a
virtual keyboard's keys, so vkbd tracks them and sends each change.  */

#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "virtual-keyboard-unstable-v1-client-protocol.h"

static struct wl_seat *seat;
static struct zwp_virtual_keyboard_manager_v1 *manager;

static void
die (const char *message)
{
  fprintf (stderr, "vkbd: %s\n", message);
  exit (2);
}

static void
registry_global (void *data, struct wl_registry *registry, uint32_t name,
		 const char *interface, uint32_t version)
{
  if (!strcmp (interface, wl_seat_interface.name) && !seat)
    seat = wl_registry_bind (registry, name, &wl_seat_interface, 1);
  else if (!strcmp (interface,
		    zwp_virtual_keyboard_manager_v1_interface.name))
    manager = wl_registry_bind (registry, name,
				&zwp_virtual_keyboard_manager_v1_interface, 1);
}

static void
registry_remove (void *data, struct wl_registry *registry, uint32_t name)
{
}

static const struct wl_registry_listener registry_listener =
  { .global = registry_global, .global_remove = registry_remove };

static struct zwp_virtual_keyboard_v1 *keyboard;
static struct xkb_state *state;

static uint32_t
now_ms (void)
{
  struct timespec ts;
  clock_gettime (CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000u + ts.tv_nsec / 1000000;
}

static void
send_key (uint32_t code, bool pressed)
{
  zwp_virtual_keyboard_v1_key (keyboard, now_ms (), code,
			       (pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
				: WL_KEYBOARD_KEY_STATE_RELEASED));
  if (xkb_state_update_key (state, code + 8,
			    pressed ? XKB_KEY_DOWN : XKB_KEY_UP)
      & (XKB_STATE_MODS_EFFECTIVE | XKB_STATE_LAYOUT_EFFECTIVE))
    zwp_virtual_keyboard_v1_modifiers
      (keyboard,
       xkb_state_serialize_mods (state, XKB_STATE_MODS_DEPRESSED),
       xkb_state_serialize_mods (state, XKB_STATE_MODS_LATCHED),
       xkb_state_serialize_mods (state, XKB_STATE_MODS_LOCKED),
       xkb_state_serialize_layout (state, XKB_STATE_LAYOUT_EFFECTIVE));
}

int
main (void)
{
  struct wl_display *display = wl_display_connect (NULL);
  struct xkb_context *context = xkb_context_new (XKB_CONTEXT_NO_FLAGS);
  struct xkb_rule_names names = { .layout = "us" };
  struct xkb_keymap *keymap;
  char *string, line[4096];
  size_t size;
  int fd;

  if (!display || !context)
    die ("could not connect");
  wl_registry_add_listener (wl_display_get_registry (display),
			    &registry_listener, NULL);
  wl_display_roundtrip (display);
  if (!seat || !manager)
    die ("the compositor has no virtual keyboards");

  keymap = xkb_keymap_new_from_names (context, &names, 0);
  string = keymap ? xkb_keymap_get_as_string (keymap,
					      XKB_KEYMAP_FORMAT_TEXT_V1)
    : NULL;
  if (!string || !(state = xkb_state_new (keymap)))
    die ("could not compile a keymap");
  size = strlen (string) + 1;
  fd = memfd_create ("vkbd-keymap", MFD_CLOEXEC);
  if (fd < 0 || write (fd, string, size) != (ssize_t) size)
    die ("could not write the keymap");
  keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard
    (manager, seat);
  zwp_virtual_keyboard_v1_keymap (keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
				  fd, size);
  close (fd);
  wl_display_roundtrip (display);
  puts ("ready");
  fflush (stdout);

  while (fgets (line, sizeof line, stdin))
    {
      char *rest = line + strspn (line, " \t"), *end;
      unsigned long code;

      while (*rest == '+' || *rest == '-'
	     ? (code = strtoul (rest + 1, &end, 10)), end != rest + 1
	     : ((code = strtoul (rest, &end, 10)), end != rest))
	{
	  if (*rest != '-')
	    send_key (code, true);
	  if (*rest != '+')
	    send_key (code, false);
	  wl_display_roundtrip (display);
	  rest = end + strspn (end, " \t");
	}
      puts ("ok");
      fflush (stdout);
    }
  return 0;
}
