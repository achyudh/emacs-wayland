/* refclient -- the observer's reference client.

This file is part of GNU Emacs, as modified by the native Wayland
backend fork.  It is free software under
the GNU General Public License, version 3 or later.

A toplevel that fills itself with one color and, on every key press,
switches to the other and commits at once, without waiting for a frame
callback.  Like the backend, it draws at the output's physical size
through fractional-scale-v1 and viewporter, so the compositor never
has to scale it.  The observer's latencies against it are the floor of the
pipeline, and their spread across runs says how repeatable the
observer is.  */

#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"

static struct
{
  struct wl_display *display;
  struct wl_compositor *compositor;
  struct wl_shm *shm;
  struct wl_seat *seat;
  struct xdg_wm_base *wm_base;
  struct wl_surface *surface;
  struct xdg_surface *xdg_surface;
  struct xdg_toplevel *toplevel;
  int width, height, pending_width, pending_height;
  bool configured, dark;
  /* The light and dark buffers, drawn once per size so that a key
     press only attaches and commits.  */
  struct wl_buffer *buffers[2];
  int buffer_width, buffer_height;
  struct wp_viewporter *viewporter;
  struct wp_fractional_scale_manager_v1 *fractions;
  struct wp_viewport *viewport;
  uint32_t scale;		/* In 120ths.  */
} rc;

static void
die (const char *message)
{
  fprintf (stderr, "refclient: %s\n", message);
  exit (2);
}

static int
physical (int logical)
{
  return (logical * rc.scale + 60) / 120;
}

static struct wl_buffer *
make_buffer (uint32_t color)
{
  int width = physical (rc.width), height = physical (rc.height);
  int stride = width * 4;
  size_t size = (size_t) stride * height;
  int fd = memfd_create ("refclient", MFD_CLOEXEC);
  uint32_t *pixels;
  struct wl_shm_pool *pool;
  struct wl_buffer *buffer;

  if (fd < 0 || ftruncate (fd, size) < 0)
    die ("could not allocate a buffer");
  pixels = mmap (NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (pixels == MAP_FAILED)
    die ("could not map a buffer");
  for (size_t i = 0; i < size / 4; i++)
    pixels[i] = color;
  munmap (pixels, size);
  pool = wl_shm_create_pool (rc.shm, fd, size);
  buffer = wl_shm_pool_create_buffer (pool, 0, width, height, stride,
				      WL_SHM_FORMAT_XRGB8888);
  wl_shm_pool_destroy (pool);
  close (fd);
  return buffer;
}

/* Attach the buffer of the current color and commit.  */

static void
draw (void)
{
  if (rc.buffer_width != physical (rc.width)
      || rc.buffer_height != physical (rc.height))
    {
      for (int i = 0; i < 2; i++)
	{
	  if (rc.buffers[i])
	    wl_buffer_destroy (rc.buffers[i]);
	  rc.buffers[i] = make_buffer (i ? 0xff202020 : 0xffe0e0e0);
	}
      rc.buffer_width = physical (rc.width);
      rc.buffer_height = physical (rc.height);
    }
  if (rc.viewport)
    wp_viewport_set_destination (rc.viewport, rc.width, rc.height);
  wl_surface_attach (rc.surface, rc.buffers[rc.dark], 0, 0);
  wl_surface_damage_buffer (rc.surface, 0, 0, rc.buffer_width,
			    rc.buffer_height);
  wl_surface_commit (rc.surface);
  wl_display_flush (rc.display);
}

static void
keyboard_keymap (void *d, struct wl_keyboard *k, uint32_t format,
		 int32_t fd, uint32_t size)
{
  close (fd);
}

static void keyboard_enter (void *d, struct wl_keyboard *k, uint32_t s,
			    struct wl_surface *f, struct wl_array *keys) {}
static void keyboard_leave (void *d, struct wl_keyboard *k, uint32_t s,
			    struct wl_surface *f) {}
static void keyboard_modifiers (void *d, struct wl_keyboard *k, uint32_t s,
				uint32_t dep, uint32_t lat, uint32_t lock,
				uint32_t group) {}
static void keyboard_repeat (void *d, struct wl_keyboard *k, int32_t rate,
			     int32_t delay) {}

static void
keyboard_key (void *data, struct wl_keyboard *keyboard, uint32_t serial,
	      uint32_t time, uint32_t key, uint32_t state)
{
  if (state != WL_KEYBOARD_KEY_STATE_PRESSED || !rc.configured)
    return;
  rc.dark = !rc.dark;
  draw ();
}

static const struct wl_keyboard_listener keyboard_listener =
  {
    .keymap = keyboard_keymap, .enter = keyboard_enter,
    .leave = keyboard_leave, .key = keyboard_key,
    .modifiers = keyboard_modifiers, .repeat_info = keyboard_repeat,
  };

/* A headless compositor has a keyboard only while an observer's
   virtual keyboard exists, so it comes and goes between runs.  */

static void
seat_capabilities (void *data, struct wl_seat *seat, uint32_t caps)
{
  static struct wl_keyboard *keyboard;

  if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard)
    {
      keyboard = wl_seat_get_keyboard (seat);
      wl_keyboard_add_listener (keyboard, &keyboard_listener, NULL);
    }
  else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && keyboard)
    {
      wl_keyboard_destroy (keyboard);
      keyboard = NULL;
    }
}

static void
seat_name (void *data, struct wl_seat *seat, const char *name)
{
}

static const struct wl_seat_listener seat_listener =
  { .capabilities = seat_capabilities, .name = seat_name };

static void
wm_base_ping (void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
  xdg_wm_base_pong (wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener =
  { .ping = wm_base_ping };

static void
toplevel_configure (void *data, struct xdg_toplevel *toplevel,
		    int32_t width, int32_t height, struct wl_array *states)
{
  rc.pending_width = width > 0 ? width : 640;
  rc.pending_height = height > 0 ? height : 480;
}

static void toplevel_close (void *d, struct xdg_toplevel *t) { exit (0); }
static void toplevel_bounds (void *d, struct xdg_toplevel *t, int32_t w,
			     int32_t h) {}
static void toplevel_capabilities (void *d, struct xdg_toplevel *t,
				   struct wl_array *c) {}

static const struct xdg_toplevel_listener toplevel_listener =
  {
    .configure = toplevel_configure, .close = toplevel_close,
    .configure_bounds = toplevel_bounds,
    .wm_capabilities = toplevel_capabilities,
  };

static void
xdg_surface_configure (void *data, struct xdg_surface *xdg_surface,
		       uint32_t serial)
{
  xdg_surface_ack_configure (xdg_surface, serial);
  rc.width = rc.pending_width;
  rc.height = rc.pending_height;
  rc.configured = true;
  draw ();
}

static const struct xdg_surface_listener xdg_surface_listener =
  { .configure = xdg_surface_configure };

static void
preferred_scale (void *data, struct wp_fractional_scale_v1 *fraction,
		 uint32_t scale)
{
  rc.scale = scale;
  if (rc.configured)
    draw ();
}

static const struct wp_fractional_scale_v1_listener fraction_listener =
  { .preferred_scale = preferred_scale };

static void
registry_global (void *data, struct wl_registry *registry, uint32_t name,
		 const char *interface, uint32_t version)
{
  if (!strcmp (interface, wl_compositor_interface.name))
    rc.compositor = wl_registry_bind (registry, name,
				      &wl_compositor_interface, 4);
  else if (!strcmp (interface, wl_shm_interface.name))
    rc.shm = wl_registry_bind (registry, name, &wl_shm_interface, 1);
  else if (!strcmp (interface, wl_seat_interface.name) && !rc.seat)
    rc.seat = wl_registry_bind (registry, name, &wl_seat_interface, 4);
  else if (!strcmp (interface, wp_viewporter_interface.name))
    rc.viewporter = wl_registry_bind (registry, name,
				      &wp_viewporter_interface, 1);
  else if (!strcmp (interface, wp_fractional_scale_manager_v1_interface.name))
    rc.fractions = wl_registry_bind (registry, name,
				     &wp_fractional_scale_manager_v1_interface,
				     1);
  else if (!strcmp (interface, xdg_wm_base_interface.name))
    rc.wm_base = wl_registry_bind (registry, name, &xdg_wm_base_interface,
				   1);
}

static void
registry_remove (void *data, struct wl_registry *registry, uint32_t name)
{
}

static const struct wl_registry_listener registry_listener =
  { .global = registry_global, .global_remove = registry_remove };

int
main (void)
{
  rc.display = wl_display_connect (NULL);
  if (!rc.display)
    die ("could not connect");
  wl_registry_add_listener (wl_display_get_registry (rc.display),
			    &registry_listener, NULL);
  wl_display_roundtrip (rc.display);
  if (!rc.compositor || !rc.shm || !rc.seat || !rc.wm_base)
    die ("the compositor lacks a required global");
  xdg_wm_base_add_listener (rc.wm_base, &wm_base_listener, NULL);
  wl_seat_add_listener (rc.seat, &seat_listener, NULL);
  rc.scale = 120;
  rc.surface = wl_compositor_create_surface (rc.compositor);
  if (rc.viewporter && rc.fractions)
    {
      rc.viewport = wp_viewporter_get_viewport (rc.viewporter, rc.surface);
      wp_fractional_scale_v1_add_listener
	(wp_fractional_scale_manager_v1_get_fractional_scale (rc.fractions,
							      rc.surface),
	 &fraction_listener, NULL);
    }
  rc.xdg_surface = xdg_wm_base_get_xdg_surface (rc.wm_base, rc.surface);
  xdg_surface_add_listener (rc.xdg_surface, &xdg_surface_listener, NULL);
  rc.toplevel = xdg_surface_get_toplevel (rc.xdg_surface);
  xdg_toplevel_add_listener (rc.toplevel, &toplevel_listener, NULL);
  xdg_toplevel_set_app_id (rc.toplevel, "refclient");
  wl_surface_commit (rc.surface);
  while (wl_display_dispatch (rc.display) >= 0)
    ;
  return 0;
}
