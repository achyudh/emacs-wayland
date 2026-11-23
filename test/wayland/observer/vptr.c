/* vptr -- a long-lived virtual pointer for tests.

This file is part of GNU Emacs, as modified by the native Wayland
backend fork.  It is free software under
the GNU General Public License, version 3 or later.

It creates one virtual pointer and keeps it until stdin closes, so the
seat keeps its pointer between commands.  Each line of stdin is one
command; "ok" is printed after the compositor has received it.

  move X Y W H              warp to X,Y of a W by H layout
  press BUTTON              press an evdev button
  release BUTTON            release it
  click BUTTON              press and release it
  wheel AXIS STEPS          turn a wheel STEPS notches
  finger AXIS DELTA N HZ    scroll N frames of DELTA at HZ, then stop

AXIS is 0 for vertical and 1 for horizontal scrolling.  The finger
command keeps its own absolute schedule, so the frames reach the
compositor at HZ however slowly the caller reads, and reports the
latest frame relative to that schedule as "ok N LATE_US".  */

#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <wayland-client.h>

#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

static struct wl_seat *seat;
static struct zwlr_virtual_pointer_manager_v1 *manager;

static void
die (const char *message)
{
  fprintf (stderr, "vptr: %s\n", message);
  exit (2);
}

static void
registry_global (void *data, struct wl_registry *registry, uint32_t name,
		 const char *interface, uint32_t version)
{
  if (!strcmp (interface, wl_seat_interface.name) && !seat)
    seat = wl_registry_bind (registry, name, &wl_seat_interface, 1);
  else if (!strcmp (interface,
		    zwlr_virtual_pointer_manager_v1_interface.name))
    manager = wl_registry_bind (registry, name,
				&zwlr_virtual_pointer_manager_v1_interface, 1);
}

static void
registry_remove (void *data, struct wl_registry *registry, uint32_t name)
{
}

static const struct wl_registry_listener registry_listener =
  { .global = registry_global, .global_remove = registry_remove };

static int64_t
now_ns (void)
{
  struct timespec ts;
  clock_gettime (CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * INT64_C (1000000000) + ts.tv_nsec;
}

static uint32_t
now_ms (void)
{
  return now_ns () / 1000000;
}

static void
sleep_until (int64_t when)
{
  struct timespec ts = { when / 1000000000, when % 1000000000 };
  while (clock_nanosleep (CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
    ;
}

int
main (void)
{
  struct wl_display *display = wl_display_connect (NULL);
  struct zwlr_virtual_pointer_v1 *pointer;
  char line[256];

  if (!display)
    die ("could not connect");
  wl_registry_add_listener (wl_display_get_registry (display),
			    &registry_listener, NULL);
  wl_display_roundtrip (display);
  if (!seat || !manager)
    die ("the compositor has no virtual pointers");
  pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer
    (manager, seat);
  wl_display_roundtrip (display);
  puts ("ready");
  fflush (stdout);

  while (fgets (line, sizeof line, stdin))
    {
      unsigned x, y, w, h, button, axis, n, hz;
      int steps;
      double delta;
      char state[8];
      int64_t late = 0;
      bool finger = false;

      if (sscanf (line, "move %u %u %u %u", &x, &y, &w, &h) == 4)
	zwlr_virtual_pointer_v1_motion_absolute (pointer, now_ms (),
						 x, y, w, h);
      else if (sscanf (line, "%7s %u", state, &button) == 2
	       && (!strcmp (state, "press") || !strcmp (state, "release")
		   || !strcmp (state, "click")))
	{
	  if (strcmp (state, "release"))
	    {
	      zwlr_virtual_pointer_v1_button
		(pointer, now_ms (), button, WL_POINTER_BUTTON_STATE_PRESSED);
	      zwlr_virtual_pointer_v1_frame (pointer);
	    }
	  if (strcmp (state, "press"))
	    zwlr_virtual_pointer_v1_button
	      (pointer, now_ms (), button, WL_POINTER_BUTTON_STATE_RELEASED);
	}
      else if (sscanf (line, "wheel %u %d", &axis, &steps) == 2 && axis < 2)
	{
	  zwlr_virtual_pointer_v1_axis_source (pointer,
					       WL_POINTER_AXIS_SOURCE_WHEEL);
	  zwlr_virtual_pointer_v1_axis_discrete
	    (pointer, now_ms (), axis, wl_fixed_from_int (15 * steps), steps);
	}
      else if (sscanf (line, "finger %u %lf %u %u", &axis, &delta, &n, &hz) == 4
	       && axis < 2 && hz > 0)
	{
	  int64_t start = now_ns (), period = 1000000000 / hz;

	  finger = true;

	  for (unsigned i = 0; i < n; i++)
	    {
	      int64_t when = start + i * period, lag;

	      sleep_until (when);
	      lag = now_ns () - when;
	      if (lag > late)
		late = lag;
	      zwlr_virtual_pointer_v1_axis_source
		(pointer, WL_POINTER_AXIS_SOURCE_FINGER);
	      zwlr_virtual_pointer_v1_axis (pointer, now_ms (), axis,
					    wl_fixed_from_double (delta));
	      zwlr_virtual_pointer_v1_frame (pointer);
	      if (wl_display_flush (display) < 0)
		die ("the connection failed");
	    }
	  sleep_until (start + n * period);
	  zwlr_virtual_pointer_v1_axis_source (pointer,
					       WL_POINTER_AXIS_SOURCE_FINGER);
	  zwlr_virtual_pointer_v1_axis_stop (pointer, now_ms (), axis);
	}
      else
	{
	  fprintf (stderr, "vptr: bad command: %s", line);
	  return 2;
	}
      zwlr_virtual_pointer_v1_frame (pointer);
      if (wl_display_roundtrip (display) < 0)
	die ("the connection failed");
      if (finger)
	printf ("ok %u %" PRId64 "\n", n, late / 1000);
      else
	puts ("ok");
      fflush (stdout);
    }
  return 0;
}
