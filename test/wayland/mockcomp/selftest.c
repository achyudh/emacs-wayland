/* selftest -- a minimal Wayland client for checking mockcomp.

This file is part of GNU Emacs, as modified by the native Wayland
backend fork.

GNU Emacs is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or (at
your option) any later version.

GNU Emacs is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GNU Emacs.  If not, see <https://www.gnu.org/licenses/>.  */

/* Usage: selftest MODE, with WAYLAND_DISPLAY naming mockcomp's socket.
   Modes:
     good          map a toplevel correctly and wait for a frame callback
     damage        then commit damage outside the buffer
     held          then reattach a buffer mockcomp still holds
                   (run with mockcomp's hold-buffers on)
     early-attach  attach a buffer before the first configure; succeed
                   only if mockcomp answers with a protocol error
     listen SECS   map, then answer pings and count keys for SECS
                   seconds; print "keys=N pings=N keymap=ok|bad"  */

#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"

enum { WIDTH = 64, HEIGHT = 64 };

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_subcompositor *subcompositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct xdg_wm_base *wm_base;
static bool configured, frame_done, keymap_ok;
static int keys, pings;

static void
fail (const char *msg)
{
  fprintf (stderr, "selftest: %s\n", msg);
  exit (EXIT_FAILURE);
}

static void
wm_base_ping (void *data, struct xdg_wm_base *base, uint32_t serial)
{
  pings++;
  xdg_wm_base_pong (base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener =
  { .ping = wm_base_ping };

static void
keyboard_keymap (void *data, struct wl_keyboard *keyboard, uint32_t format,
		 int32_t fd, uint32_t size)
{
  char *map = mmap (NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);

  keymap_ok = (map != MAP_FAILED && size > 10
	       && !strncmp (map, "xkb_keymap", 10));
  if (map != MAP_FAILED)
    munmap (map, size);
  close (fd);
}

static void
keyboard_enter (void *data, struct wl_keyboard *keyboard, uint32_t serial,
		struct wl_surface *surface, struct wl_array *pressed)
{
}

static void
keyboard_leave (void *data, struct wl_keyboard *keyboard, uint32_t serial,
		struct wl_surface *surface)
{
}

static void
keyboard_key (void *data, struct wl_keyboard *keyboard, uint32_t serial,
	      uint32_t time, uint32_t key, uint32_t state)
{
  keys++;
}

static void
keyboard_modifiers (void *data, struct wl_keyboard *keyboard,
		    uint32_t serial, uint32_t depressed, uint32_t latched,
		    uint32_t locked, uint32_t group)
{
}

static void
keyboard_repeat_info (void *data, struct wl_keyboard *keyboard,
		      int32_t rate, int32_t delay)
{
}

static const struct wl_keyboard_listener keyboard_listener =
  {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
  };

static void
registry_global (void *data, struct wl_registry *registry, uint32_t name,
		 const char *interface, uint32_t version)
{
  if (!strcmp (interface, wl_compositor_interface.name))
    compositor = wl_registry_bind (registry, name, &wl_compositor_interface,
				   4);
  else if (!strcmp (interface, wl_subcompositor_interface.name))
    subcompositor = wl_registry_bind (registry, name, &wl_subcompositor_interface,
				      1);
  else if (!strcmp (interface, wl_shm_interface.name))
    shm = wl_registry_bind (registry, name, &wl_shm_interface, 1);
  else if (!strcmp (interface, wl_seat_interface.name))
    seat = wl_registry_bind (registry, name, &wl_seat_interface, 4);
  else if (!strcmp (interface, xdg_wm_base_interface.name))
    {
      wm_base = wl_registry_bind (registry, name, &xdg_wm_base_interface, 1);
      xdg_wm_base_add_listener (wm_base, &wm_base_listener, NULL);
    }
}

static void
registry_global_remove (void *data, struct wl_registry *registry,
			uint32_t name)
{
}

static const struct wl_registry_listener registry_listener =
  {
    .global = registry_global,
    .global_remove = registry_global_remove,
  };

static void
xdg_surface_configure (void *data, struct xdg_surface *surface,
		       uint32_t serial)
{
  xdg_surface_ack_configure (surface, serial);
  configured = true;
}

static const struct xdg_surface_listener xdg_surface_listener =
  { .configure = xdg_surface_configure };

static void
toplevel_configure (void *data, struct xdg_toplevel *toplevel,
		    int32_t width, int32_t height, struct wl_array *states)
{
}

static void
toplevel_close (void *data, struct xdg_toplevel *toplevel)
{
}

static void
toplevel_configure_bounds (void *data, struct xdg_toplevel *toplevel,
			   int32_t width, int32_t height)
{
}

static void
toplevel_wm_capabilities (void *data, struct xdg_toplevel *toplevel,
			  struct wl_array *capabilities)
{
}

static const struct xdg_toplevel_listener toplevel_listener =
  {
    .configure = toplevel_configure,
    .close = toplevel_close,
    .configure_bounds = toplevel_configure_bounds,
    .wm_capabilities = toplevel_wm_capabilities,
  };

static void
frame_callback_done (void *data, struct wl_callback *callback,
		     uint32_t time)
{
  frame_done = true;
  wl_callback_destroy (callback);
}

static const struct wl_callback_listener frame_listener =
  { .done = frame_callback_done };

static struct wl_buffer *
make_buffer (uint32_t color)
{
  int stride = WIDTH * 4, size = stride * HEIGHT;
  int fd = memfd_create ("selftest-buffer", MFD_CLOEXEC);
  uint32_t *pixels;
  struct wl_shm_pool *pool;
  struct wl_buffer *buffer;

  if (fd < 0 || ftruncate (fd, size) < 0)
    fail ("could not create shared memory");
  pixels = mmap (NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (pixels == MAP_FAILED)
    fail ("mmap failed");
  for (int i = 0; i < WIDTH * HEIGHT; i++)
    pixels[i] = color;
  munmap (pixels, size);
  pool = wl_shm_create_pool (shm, fd, size);
  buffer = wl_shm_pool_create_buffer (pool, 0, WIDTH, HEIGHT, stride,
				      WL_SHM_FORMAT_XRGB8888);
  wl_shm_pool_destroy (pool);
  close (fd);
  return buffer;
}

static void
roundtrip (void)
{
  if (wl_display_roundtrip (display) < 0)
    fail ("connection failed");
}

static struct wl_surface *
make_toplevel (struct xdg_surface **xdg_surface_out)
{
  struct wl_surface *surface = wl_compositor_create_surface (compositor);
  struct xdg_surface *xdg_surface
    = xdg_wm_base_get_xdg_surface (wm_base, surface);
  struct xdg_toplevel *toplevel = xdg_surface_get_toplevel (xdg_surface);

  xdg_surface_add_listener (xdg_surface, &xdg_surface_listener, NULL);
  xdg_toplevel_add_listener (toplevel, &toplevel_listener, NULL);
  xdg_toplevel_set_title (toplevel, "selftest");
  *xdg_surface_out = xdg_surface;
  return surface;
}

/* Map a toplevel correctly and wait for its first frame callback.  */
static struct wl_surface *
map_toplevel (struct wl_buffer *buffer)
{
  struct xdg_surface *xdg_surface;
  struct wl_surface *surface = make_toplevel (&xdg_surface);
  struct wl_callback *callback;

  wl_surface_commit (surface);
  while (!configured)
    if (wl_display_dispatch (display) < 0)
      fail ("connection failed before configure");
  wl_surface_attach (surface, buffer, 0, 0);
  wl_surface_damage_buffer (surface, 0, 0, WIDTH, HEIGHT);
  callback = wl_surface_frame (surface);
  wl_callback_add_listener (callback, &frame_listener, NULL);
  wl_surface_commit (surface);
  while (!frame_done)
    if (wl_display_dispatch (display) < 0)
      fail ("connection failed before the frame callback");
  return surface;
}

static void
listen_for (int seconds)
{
  struct timespec start, now;
  int fd = wl_display_get_fd (display);

  clock_gettime (CLOCK_MONOTONIC, &start);
  for (;;)
    {
      struct pollfd pfd = { .fd = fd, .events = POLLIN };

      clock_gettime (CLOCK_MONOTONIC, &now);
      if (now.tv_sec - start.tv_sec >= seconds)
	break;
      wl_display_flush (display);
      if (poll (&pfd, 1, 100) > 0 && wl_display_dispatch (display) < 0)
	fail ("connection failed while listening");
    }
}

int
main (int argc, char **argv)
{
  const char *mode = argc > 1 ? argv[1] : "good";
  struct wl_registry *registry;

  display = wl_display_connect (NULL);
  if (!display)
    fail ("could not connect");
  registry = wl_display_get_registry (display);
  wl_registry_add_listener (registry, &registry_listener, NULL);
  roundtrip ();
  if (!compositor || !shm || !wm_base || !seat)
    fail ("a required global is missing");

  bool role_conflict = !strcmp (mode, "role-conflict");
  if (role_conflict || !strcmp (mode, "parent-cycle"))
    {
      struct xdg_surface *xdg_surface;
      if (!subcompositor)
	fail ("a subcompositor is required");
      struct wl_surface *a = role_conflict ? make_toplevel (&xdg_surface)
	: wl_compositor_create_surface (compositor);
      struct wl_surface *b = wl_compositor_create_surface (compositor);
      wl_subcompositor_get_subsurface (subcompositor, a, b);
      if (!role_conflict)
	wl_subcompositor_get_subsurface (subcompositor, b, a);
      wl_display_roundtrip (display);
      if (wl_display_get_error (display) != EPROTO)
	fail ("the invalid subsurface did not cause a protocol error");
      uint32_t code = wl_display_get_protocol_error (display, NULL, NULL);
      if (code != (role_conflict ? WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE
		   : WL_SUBCOMPOSITOR_ERROR_BAD_PARENT))
	fail ("the invalid subsurface gave the wrong protocol error");
      return EXIT_SUCCESS;
    }

  if (!strcmp (mode, "early-attach"))
    {
      struct xdg_surface *xdg_surface;
      struct wl_surface *surface = make_toplevel (&xdg_surface);

      wl_surface_attach (surface, make_buffer (0xff0000), 0, 0);
      wl_surface_commit (surface);
      wl_display_roundtrip (display);
      if (wl_display_get_error (display) == 0)
	fail ("no protocol error for a buffer attached before configure");
      return EXIT_SUCCESS;
    }

  struct wl_buffer *first = make_buffer (0x336699);
  struct wl_surface *surface = map_toplevel (first);

  if (!strcmp (mode, "good"))
    ;
  else if (!strcmp (mode, "damage"))
    {
      wl_surface_attach (surface, make_buffer (0x993366), 0, 0);
      wl_surface_damage_buffer (surface, 0, 0, WIDTH * 4, HEIGHT * 4);
      wl_surface_commit (surface);
    }
  else if (!strcmp (mode, "held"))
    {
      /* With hold-buffers on, FIRST stays held after SECOND replaces
	 it, so attaching FIRST again is a reuse of a held buffer.  */
      wl_surface_attach (surface, make_buffer (0x669933), 0, 0);
      wl_surface_damage_buffer (surface, 0, 0, WIDTH, HEIGHT);
      wl_surface_commit (surface);
      roundtrip ();
      wl_surface_attach (surface, first, 0, 0);
      wl_surface_damage_buffer (surface, 0, 0, WIDTH, HEIGHT);
      wl_surface_commit (surface);
    }
  else if (!strcmp (mode, "listen"))
    {
      struct wl_keyboard *keyboard = wl_seat_get_keyboard (seat);

      wl_keyboard_add_listener (keyboard, &keyboard_listener, NULL);
      listen_for (argc > 2 ? atoi (argv[2]) : 2);
      printf ("keys=%d pings=%d keymap=%s\n", keys, pings,
	      keymap_ok ? "ok" : "bad");
    }
  else
    fail ("unknown mode");

  roundtrip ();
  wl_display_disconnect (display);
  return EXIT_SUCCESS;
}
