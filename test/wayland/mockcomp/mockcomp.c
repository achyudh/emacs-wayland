/* mockcomp -- a scriptable Wayland compositor for testing the native
   Wayland backend.

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

/* The compositor implements only what the backend's tests need: the
   required globals, a request trace, protocol invariant checks, and
   faults the external test runner triggers over a control socket.
   See README in this directory for the control commands.  */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <cairo.h>
#include <pixman.h>
#include <wayland-server.h>
#include <xkbcommon/xkbcommon.h>

#include "xdg-shell-server-protocol.h"
#include "xdg-activation-v1-server-protocol.h"
#include "fractional-scale-v1-server-protocol.h"
#include "viewporter-server-protocol.h"
#include "xdg-output-unstable-v1-server-protocol.h"
#include "cursor-shape-v1-server-protocol.h"
#include "text-input-unstable-v3-server-protocol.h"

enum
  {
    COMPOSITOR_VERSION = 6,
    SEAT_VERSION = 8,
    OUTPUT_VERSION = 4,
    DATA_DEVICE_MANAGER_VERSION = 3,
    XDG_WM_BASE_VERSION = 6,
    MAX_VIOLATIONS = 64,
    FRAME_INTERVAL_MS = 16,
  };

struct buffer_ref
{
  struct wl_resource *buffer;
  struct wl_listener destroy;
};

/* A buffer the compositor holds: committed and not yet released.  */
struct held_buffer
{
  struct wl_list link;
  struct surface *surface;
  struct buffer_ref ref;
  unsigned char *pixels;
  size_t size;
  unsigned long commit;
};

struct mock_output
{
  const char *name;
  /* SCALE is what wl_output.scale sends: LOGICAL_SCALE, in 120ths,
     rounded up, as compositors do for fractional scales.  Only
     xdg-output reports the exact logical size.  */
  int scale, logical_scale, x;
  struct wl_global *global;
  /* A removed global, which still accepts binds until its timer
     destroys it, as wlroots' wlr_global_destroy_safe does: a client
     may bind it before it reads the global_remove event.  */
  struct wl_global *removed;
  struct wl_event_source *removal_timer;
  struct wl_list resources;
};

struct surface
{
  struct wl_list link;
  struct wl_resource *resource;
  struct wl_resource *xdg_surface, *xdg_toplevel;
  struct wl_resource *subsurface;
  enum { NO_ROLE, XDG_ROLE, SUBSURFACE_ROLE } role;
  struct surface *requested_parent, *parent;
  struct wl_list siblings, children, parent_in_stack;
  int32_t requested_x, requested_y, x, y;
  int stacking;
  bool synchronized;
  bool cached_relationship;
  int32_t cached_x, cached_y;
  int cached_stacking;
  struct wl_resource *fractional, *viewport;
  struct mock_output *output;
  int32_t scale, destination_width, destination_height;

  /* Pending state, applied on commit.  */
  bool attached;
  struct buffer_ref pending;
  struct wl_list pending_frames;
  pixman_region32_t damage;

  /* Commits cached by synchronization with a parent.  */
  bool cached_commit, cached_attached;
  struct buffer_ref cached;
  struct wl_list cached_frames;
  pixman_region32_t cached_damage;

  /* Current state.  */
  struct buffer_ref current;
  struct wl_list frames;

  /* xdg-shell state.  */
  bool initial_commit_done;
  uint32_t last_configure_serial;
  uint32_t last_acked_serial;
  bool acked;
  bool mapped;
  int32_t requested_geometry[4], geometry[4];
  unsigned long commits;

  /* Size of the last committed shm buffer.  */
  int32_t buffer_width, buffer_height;
  uint32_t *image;
  uint64_t damage_pixels;
  unsigned long content_commits;
};

static bool
surface_shown (struct surface *surface)
{
  while (surface && surface->mapped)
    {
      if (surface->role != SUBSURFACE_ROLE)
	return true;
      surface = surface->parent;
    }
  return false;
}

static bool
surface_synchronized (struct surface *surface)
{
  for (; surface->requested_parent; surface = surface->requested_parent)
    if (surface->synchronized)
      return true;
  return false;
}

/* A text-input-v3 object (see the `im' command).  */

struct text_input
{
  struct wl_list link;
  struct wl_resource *resource;
  struct surface *entered;
  bool pending_enabled, enabled;
  int32_t pending_rect[4], rect[4];
  uint32_t pending_hint, pending_purpose, hint, purpose;
  uint32_t commits;
  /* Whether the pending state sets a rectangle, and the first one
     committed since the last enter, which shows whether a client
     resent a rectangle from another surface.  */
  bool rect_pending, first_rect_seen;
  int32_t first_rect[4];
};

static struct
{
  struct wl_display *display;
  struct wl_event_loop *loop;
  struct wl_event_source *frame_timer;
  struct mock_output outputs[2];
  bool have_fractional, have_viewporter, have_cursor_shape, have_subcompositor;
  bool have_activation, minimal_protocols;
  unsigned int activations, activation_requests;
  uint32_t activated_surface, activation_serial;
  char activation_token[80];
  int seat_version;
  uint32_t seat_capabilities;
  struct wl_list seats;
  int compositor_version;
  FILE *trace;
  const char *snapshots;

  int control_listen_fd;
  int control_fd;
  char control_buf[8192];
  size_t control_len;

  /* Faults.  */
  bool stalled;
  bool hold_buffers;
  bool frame_callbacks;
  bool disconnect_on_commit;
  bool quit;

  struct wl_resource *selection_source;
  int selection_pipe;
  int held_offer_pipe;
  bool hold_offer_pipe;

  unsigned long commits, pings, pongs, keys, selections_set;
  unsigned long frames_done, damage_checks, held_checks;
  unsigned long violation_total;
  char *violations[MAX_VIOLATIONS];
  int n_violations;

  char *keymap;
  size_t keymap_size;

  /* The seat: one keyboard focus, and modifiers computed by mockcomp's
     own xkb state from the keys it sends, as a compositor does.  */
  struct xkb_context *xkb_context;
  struct xkb_state *xkb_state;
  uint32_t mods_depressed, mods_latched, mods_locked, group;
  int32_t repeat_rate, repeat_delay;
  struct surface *keyboard_focus, *pointer_focus;
  wl_fixed_t pointer_x, pointer_y;
  unsigned long buttons, axis_events;

  /* The pointer cursor: the last cursor-shape-v1 shape, how many
     shapes and wl_pointer.set_cursor requests arrived, and whether the
     last of those hid the pointer.  */
  uint32_t cursor_shape;
  unsigned long cursor_shapes, cursor_sets;
  bool cursor_hidden;

  /* Event timestamps: real milliseconds, or a manual clock.  */
  bool manual_clock;
  uint32_t clock_ms;

  struct wl_list surfaces;
  struct wl_list held;
  struct wl_list wm_bases;
  struct wl_list keyboards;
  struct wl_list pointers;
  struct wl_list text_inputs;
  struct wl_list data_devices;
} mc;

static uint32_t
now_ms (void)
{
  struct timespec ts;
  clock_gettime (CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000u + ts.tv_nsec / 1000000;
}

/* The timestamp for input events.  */
static uint32_t
event_time (void)
{
  return mc.manual_clock ? mc.clock_ms : now_ms ();
}

static void
die (const char *fmt, ...)
{
  va_list ap;
  va_start (ap, fmt);
  fputs ("mockcomp: ", stderr);
  vfprintf (stderr, fmt, ap);
  va_end (ap);
  fputc ('\n', stderr);
  exit (EXIT_FAILURE);
}

/* Record a protocol invariant violation.  The runner fails the test
   on any.  */
static void
violation (struct wl_resource *resource, const char *fmt, ...)
{
  char *msg, *full;
  va_list ap;

  va_start (ap, fmt);
  if (vasprintf (&msg, fmt, ap) < 0)
    die ("out of memory");
  va_end (ap);
  if (asprintf (&full, "%s %s@%u", msg,
		resource ? wl_resource_get_class (resource) : "-",
		resource ? wl_resource_get_id (resource) : 0) < 0)
    die ("out of memory");
  free (msg);

  mc.violation_total++;
  if (mc.trace)
    fprintf (mc.trace, "!! violation: %s\n", full);
  if (mc.n_violations < MAX_VIOLATIONS)
    mc.violations[mc.n_violations++] = full;
  else
    free (full);
}

/* Remove RESOURCE from whichever list it was added to.  */
static void
unlink_resource (struct wl_resource *resource)
{
  wl_list_remove (wl_resource_get_link (resource));
}

static void
destroy_resource (struct wl_client *client, struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

/* Buffer references.  */

static void
buffer_ref_destroyed (struct wl_listener *listener, void *data)
{
  struct buffer_ref *ref = wl_container_of (listener, ref, destroy);
  ref->buffer = NULL;
  wl_list_remove (&listener->link);
  wl_list_init (&listener->link);
}

static void
buffer_ref_init (struct buffer_ref *ref)
{
  ref->buffer = NULL;
  ref->destroy.notify = buffer_ref_destroyed;
  wl_list_init (&ref->destroy.link);
}

static void
buffer_ref_set (struct buffer_ref *ref, struct wl_resource *buffer)
{
  wl_list_remove (&ref->destroy.link);
  wl_list_init (&ref->destroy.link);
  ref->buffer = buffer;
  if (buffer)
    wl_resource_add_destroy_listener (buffer, &ref->destroy);
}

static struct held_buffer *
find_held (struct wl_resource *buffer)
{
  struct held_buffer *held;
  wl_list_for_each (held, &mc.held, link)
    if (held->ref.buffer == buffer)
      return held;
  return NULL;
}

static void
release_held (struct held_buffer *held)
{
  if (held->ref.buffer)
    {
      if (held->pixels)
        {
          struct wl_shm_buffer *shm = wl_shm_buffer_get (held->ref.buffer);
          wl_shm_buffer_begin_access (shm);
          if (memcmp (held->pixels, wl_shm_buffer_get_data (shm), held->size))
            violation (held->surface->resource, "held-buffer-changed");
          wl_shm_buffer_end_access (shm);
          mc.held_checks++;
        }
      wl_buffer_send_release (held->ref.buffer);
    }
  buffer_ref_set (&held->ref, NULL);
  wl_list_remove (&held->link);
  free (held->pixels);
  free (held);
}

/* Release every buffer SURFACE holds except its current one, unless
   the runner asked the compositor to hold buffers.  */
static void
release_superseded (struct surface *surface)
{
  struct held_buffer *held, *tmp;

  if (mc.hold_buffers)
    return;
  wl_list_for_each_safe (held, tmp, &mc.held, link)
    if (held->surface == surface
	&& held->ref.buffer != surface->current.buffer)
      release_held (held);
}

/* wl_region.  */

static void
region_add (struct wl_client *client, struct wl_resource *resource,
	    int32_t x, int32_t y, int32_t width, int32_t height)
{
}

static const struct wl_region_interface region_impl =
  {
    .destroy = destroy_resource,
    .add = region_add,
    .subtract = region_add,
  };

/* wl_surface.  */

/* Send a configure whose states are the bits set in STATES, bit N
   standing for xdg_toplevel state N.  */

static void
send_state_configure (struct surface *surface, int32_t width,
		      int32_t height, uint32_t state_bits)
{
  struct wl_array states;

  wl_array_init (&states);
  for (uint32_t state = 0; state < 32; state++)
    if (state_bits & (UINT32_C (1) << state))
      {
	uint32_t *value = wl_array_add (&states, sizeof *value);
	if (!value)
	  die ("could not allocate configure state");
	*value = state;
      }
  surface->last_configure_serial = wl_display_next_serial (mc.display);
  if (surface->xdg_toplevel)
    xdg_toplevel_send_configure (surface->xdg_toplevel, width, height,
				 &states);
  xdg_surface_send_configure (surface->xdg_surface,
			      surface->last_configure_serial);
  wl_array_release (&states);
}

static void
send_sized_configure (struct surface *surface, int32_t width, int32_t height)
{
  send_state_configure (surface, width, height, 0);
}

/* The configure answering an initial commit leaves the size to the
   client.  */

static void
send_configure (struct surface *surface)
{
  send_sized_configure (surface, 0, 0);
}

static void
surface_attach (struct wl_client *client, struct wl_resource *resource,
		struct wl_resource *buffer, int32_t x, int32_t y)
{
  struct surface *surface = wl_resource_get_user_data (resource);
  struct held_buffer *held;

  if (buffer && buffer != surface->current.buffer
      && (held = find_held (buffer)))
    violation (buffer, "reuse-of-held-buffer");
  surface->attached = true;
  buffer_ref_set (&surface->pending, buffer);
}

static void
surface_damage (struct wl_client *client, struct wl_resource *resource,
		int32_t x, int32_t y, int32_t width, int32_t height)
{
}

static void
surface_damage_buffer (struct wl_client *client,
		       struct wl_resource *resource,
		       int32_t x, int32_t y, int32_t width, int32_t height)
{
  struct surface *surface = wl_resource_get_user_data (resource);

  if (width <= 0 || height <= 0 || (int64_t) x + width > INT32_MAX
      || (int64_t) y + height > INT32_MAX)
    violation (resource, "damage-outside-buffer");
  else if (!pixman_region32_union_rect (&surface->damage, &surface->damage,
                                       x, y, width, height))
    die ("out of memory recording damage");
}

static void
callback_destroyed (struct wl_resource *resource)
{
  unlink_resource (resource);
}

static void
surface_frame (struct wl_client *client, struct wl_resource *resource,
	       uint32_t id)
{
  struct surface *surface = wl_resource_get_user_data (resource);
  struct wl_resource *callback
    = wl_resource_create (client, &wl_callback_interface, 1, id);

  if (!callback)
    {
      wl_client_post_no_memory (client);
      return;
    }
  wl_resource_set_implementation (callback, NULL, NULL, callback_destroyed);
  wl_list_insert (surface->pending_frames.prev,
		  wl_resource_get_link (callback));
}

static void
surface_set_region (struct wl_client *client, struct wl_resource *resource,
		    struct wl_resource *region)
{
}

static void
check_damage (struct surface *surface, struct wl_resource *buffer,
	      pixman_region32_t *damage)
{
  struct wl_shm_buffer *shm = wl_shm_buffer_get (buffer);

  if (!shm || !pixman_region32_not_empty (damage))
    return;
  const pixman_box32_t *box = pixman_region32_extents (damage);
  if (box->x1 < 0 || box->y1 < 0 || box->x2 > wl_shm_buffer_get_width (shm)
      || box->y2 > wl_shm_buffer_get_height (shm))
    violation (surface->resource, "damage-outside-buffer");
}

static void
verify_damaged_image (struct surface *surface, struct wl_shm_buffer *shm,
		      pixman_region32_t *damage)
{
  int width = wl_shm_buffer_get_width (shm);
  int height = wl_shm_buffer_get_height (shm);
  int stride = wl_shm_buffer_get_stride (shm) / 4;
  if (!surface->image || width != surface->buffer_width
      || height != surface->buffer_height)
    {
      free (surface->image);
      surface->image = calloc ((size_t) width * height,
                               sizeof *surface->image);
      if (!surface->image)
        die ("out of memory reconstructing damage");
      /* The contents outside damage are undefined after a size change.
         A fresh buffer must therefore start with full surface damage.  */
      if (pixman_region32_contains_rectangle
          (damage, &(pixman_box32_t) {0, 0, width, height})
          != PIXMAN_REGION_IN)
        violation (surface->resource, "missing-full-damage");
    }

  wl_shm_buffer_begin_access (shm);
  const uint32_t *src = wl_shm_buffer_get_data (shm);
  int count;
  const pixman_box32_t *boxes = pixman_region32_rectangles (damage, &count);
  surface->damage_pixels = 0;
  for (int i = 0; i < count; i++)
    {
      int x = boxes[i].x1, y = boxes[i].y1;
      int x2 = boxes[i].x2, y2 = boxes[i].y2;
      if (x < 0 || y < 0 || x2 > width || y2 > height)
        continue;
      for (int row = y; row < y2; row++)
        memcpy (surface->image + (size_t) row * width + x,
                src + (size_t) row * stride + x, (x2 - x) * sizeof *src);
      surface->damage_pixels += (uint64_t) (x2 - x) * (y2 - y);
    }
  uint32_t mask = (wl_shm_buffer_get_format (shm) == WL_SHM_FORMAT_XRGB8888
                   ? 0xffffff : UINT32_MAX);
  for (int y = 0; y < height; y++)
    for (int x = 0; x < width; x++)
      if ((surface->image[(size_t) y * width + x]
           ^ src[(size_t) y * stride + x]) & mask)
        {
          violation (surface->resource, "missing-damage");
          if (mc.trace)
            fprintf (mc.trace, "## first unmatched damaged pixel %d,%d\n",
                     x, y);
          goto done;
        }
 done:
  wl_shm_buffer_end_access (shm);
  mc.damage_checks++;
}

static void
snapshot_buffer (struct surface *surface, struct wl_shm_buffer *shm)
{
  uint32_t format = wl_shm_buffer_get_format (shm);
  if (format != WL_SHM_FORMAT_XRGB8888 && format != WL_SHM_FORMAT_ARGB8888)
    die ("unsupported snapshot format %" PRIu32, format);

  char *path;
  if (asprintf (&path, "%s/surface-%" PRIu32 "-%lu.png",
		mc.snapshots, wl_resource_get_id (surface->resource),
		surface->content_commits) < 0)
    die ("out of memory");

  wl_shm_buffer_begin_access (shm);
  cairo_format_t image_format = (format == WL_SHM_FORMAT_XRGB8888
				 ? CAIRO_FORMAT_RGB24 : CAIRO_FORMAT_ARGB32);
  cairo_surface_t *image = cairo_image_surface_create_for_data
    (wl_shm_buffer_get_data (shm), image_format,
     wl_shm_buffer_get_width (shm), wl_shm_buffer_get_height (shm),
     wl_shm_buffer_get_stride (shm));
  cairo_status_t status = cairo_surface_write_to_png (image, path);
  cairo_surface_destroy (image);
  wl_shm_buffer_end_access (shm);
  if (status != CAIRO_STATUS_SUCCESS)
    die ("writing %s: %s", path, cairo_status_to_string (status));
  free (path);
}

static void update_children (struct surface *, bool, bool);

static void
apply_surface (struct surface *surface, bool attached, struct buffer_ref *ref,
	       struct wl_list *frames, pixman_region32_t *damage, bool cached)
{
  struct wl_resource *resource = surface->resource;
  struct wl_resource *buffer = ref->buffer;
  struct wl_client *client = wl_resource_get_client (resource);

  if (surface->xdg_surface)
    {
      if (!surface->initial_commit_done)
	{
	  /* The initial commit must carry no buffer; the compositor
	     answers it with a configure.  */
	  if (attached && buffer)
	    {
	      violation (resource, "attach-before-configure");
	      wl_resource_post_error (surface->xdg_surface,
				      XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER,
				      "buffer attached before the first "
				      "configure");
	      return;
	    }
	  surface->initial_commit_done = true;
	  send_configure (surface);
	}
      else if (attached && buffer && !surface->acked)
	violation (resource, "commit-before-ack");
      memcpy (surface->geometry, surface->requested_geometry,
	      sizeof surface->geometry);
    }

  if (attached)
    {
      if (buffer)
	{
	  struct held_buffer *held;

	  struct wl_shm_buffer *shm = wl_shm_buffer_get (buffer);

	  surface->content_commits++;
	  check_damage (surface, buffer, damage);
	  if (shm)
	    {
	      if (mc.snapshots)
		{
		  verify_damaged_image (surface, shm, damage);
		  snapshot_buffer (surface, shm);
		}
	      surface->buffer_width = wl_shm_buffer_get_width (shm);
	      surface->buffer_height = wl_shm_buffer_get_height (shm);
	      if (surface->buffer_width % surface->scale
		  || surface->buffer_height % surface->scale)
		violation (resource, "nonintegral-buffer-scale");
	    }
	  buffer_ref_set (&surface->current, buffer);
	  if (!find_held (buffer))
	    {
	      held = calloc (1, sizeof *held);
	      if (!held)
		die ("out of memory");
	      held->surface = surface;
	      held->commit = surface->content_commits;
	      buffer_ref_init (&held->ref);
	      buffer_ref_set (&held->ref, buffer);
	      wl_list_insert (&mc.held, &held->link);
	      if (mc.snapshots && shm)
	        {
	          held->size = (size_t) wl_shm_buffer_get_height (shm)
	            * wl_shm_buffer_get_stride (shm);
	          held->pixels = malloc (held->size);
	          if (!held->pixels)
	            die ("out of memory retaining buffer contents");
	          wl_shm_buffer_begin_access (shm);
	          memcpy (held->pixels, wl_shm_buffer_get_data (shm),
                          held->size);
	          wl_shm_buffer_end_access (shm);
	        }
	    }
	  release_superseded (surface);
	  surface->mapped = surface->acked || surface->subsurface;
	  if (mc.disconnect_on_commit)
	    {
	      mc.disconnect_on_commit = false;
	      if (mc.trace)
		fputs ("## disconnect on content commit\n", mc.trace);
	      shutdown (wl_client_get_fd (client), SHUT_RDWR);
	    }
	}
      else
	{
	  /* A null buffer unmaps the toplevel; remapping takes a new
	     bufferless commit and configure.  */
	  buffer_ref_set (&surface->current, NULL);
	  release_superseded (surface);
	  free (surface->image);
	  surface->image = NULL;
	  surface->mapped = false;
	  surface->initial_commit_done = false;
	  surface->acked = false;
	}
    }

  wl_list_insert_list (surface->frames.prev, frames);
  wl_list_init (frames);
  update_children (surface, false, cached);
}

static void
apply_cached (struct surface *surface)
{
  if (!surface->cached_commit)
    return;
  surface->cached_commit = false;
  apply_surface (surface, surface->cached_attached, &surface->cached,
		 &surface->cached_frames, &surface->cached_damage, true);
  surface->cached_attached = false;
  buffer_ref_set (&surface->cached, NULL);
  pixman_region32_clear (&surface->cached_damage);
}

static void
update_children (struct surface *parent, bool cache, bool cached)
{
  int order = 0;
  struct wl_list *link;
  for (link = parent->children.next; link != &parent->parent_in_stack;
       link = link->next)
    order--;
  for (link = parent->children.next; link != &parent->children;
       link = link->next)
    {
      if (link == &parent->parent_in_stack)
	continue;
      struct surface *child = wl_container_of (link, child, siblings);
      int stacking = order < 0 ? order : order + 1;
      order++;
      if (cache)
	{
	  child->cached_relationship = true;
	  child->cached_x = child->requested_x;
	  child->cached_y = child->requested_y;
	  child->cached_stacking = stacking;
	  continue;
	}
      if (!cached || child->cached_relationship)
	{
	  child->parent = parent;
	  child->x = cached ? child->cached_x : child->requested_x;
	  child->y = cached ? child->cached_y : child->requested_y;
	  child->stacking = cached ? child->cached_stacking : stacking;
	  child->cached_relationship = false;
	}
      if (child->parent && surface_synchronized (child))
	apply_cached (child);
    }
}

static void
surface_commit (struct wl_client *client, struct wl_resource *resource)
{
  struct surface *surface = wl_resource_get_user_data (resource);
  mc.commits++;
  surface->commits++;
  if (surface_synchronized (surface))
    {
      surface->cached_commit = true;
      update_children (surface, true, false);
      if (surface->attached)
	{
	  surface->cached_attached = true;
	  buffer_ref_set (&surface->cached, surface->pending.buffer);
	}
      if (!pixman_region32_union (&surface->cached_damage,
				  &surface->cached_damage, &surface->damage))
	die ("out of memory caching damage");
      wl_list_insert_list (surface->cached_frames.prev,
			   &surface->pending_frames);
      wl_list_init (&surface->pending_frames);
    }
  else
    apply_surface (surface, surface->attached, &surface->pending,
		   &surface->pending_frames, &surface->damage, false);
  surface->attached = false;
  buffer_ref_set (&surface->pending, NULL);
  pixman_region32_clear (&surface->damage);
}

static void
surface_set_int (struct wl_client *client, struct wl_resource *resource,
		 int32_t value)
{
}

static void
surface_set_scale (struct wl_client *client, struct wl_resource *resource,
                   int32_t scale)
{
  struct surface *surface = wl_resource_get_user_data (resource);
  if (scale > 0)
    surface->scale = scale;
  else
    violation (resource, "invalid-buffer-scale");
}

static void
surface_offset (struct wl_client *client, struct wl_resource *resource,
		int32_t x, int32_t y)
{
}

static void
surface_get_release (struct wl_client *client, struct wl_resource *resource,
		     uint32_t callback)
{
  wl_client_post_implementation_error (client, "mockcomp does not "
				      "support wl_surface.get_release");
}

static const struct wl_surface_interface surface_impl =
  {
    .destroy = destroy_resource,
    .attach = surface_attach,
    .damage = surface_damage,
    .frame = surface_frame,
    .set_opaque_region = surface_set_region,
    .set_input_region = surface_set_region,
    .commit = surface_commit,
    .set_buffer_transform = surface_set_int,
    .set_buffer_scale = surface_set_scale,
    .damage_buffer = surface_damage_buffer,
    .offset = surface_offset,
    .get_release = surface_get_release,
  };

static void
destroy_frame_list (struct wl_list *list)
{
  struct wl_resource *callback, *tmp;
  wl_resource_for_each_safe (callback, tmp, list)
    wl_resource_destroy (callback);
}

static void
subsurface_destroyed (struct wl_resource *resource)
{
  struct surface *surface = wl_resource_get_user_data (resource);
  if (!surface)
    return;
  wl_list_remove (&surface->siblings);
  wl_list_init (&surface->siblings);
  surface->subsurface = NULL;
  surface->parent = surface->requested_parent = NULL;
  surface->mapped = false;
  surface->requested_x = surface->requested_y = surface->x = surface->y = 0;
  surface->stacking = 0;
  surface->cached_relationship = false;
  surface->cached_commit = surface->cached_attached = false;
  buffer_ref_set (&surface->cached, NULL);
  buffer_ref_set (&surface->current, NULL);
  release_superseded (surface);
  destroy_frame_list (&surface->cached_frames);
  pixman_region32_clear (&surface->cached_damage);
  free (surface->image);
  surface->image = NULL;
}

static void
subsurface_position (struct wl_client *client, struct wl_resource *resource,
		     int32_t x, int32_t y)
{
  struct surface *surface = wl_resource_get_user_data (resource);
  if (surface)
    {
      surface->requested_x = x;
      surface->requested_y = y;
    }
}

static void
subsurface_place (struct wl_resource *resource, struct wl_resource *reference,
		  bool above)
{
  struct surface *surface = wl_resource_get_user_data (resource);
  struct surface *sibling = wl_resource_get_user_data (reference);
  if (!surface || !surface->requested_parent)
    return;
  struct surface *parent = surface->requested_parent;
  struct wl_list *anchor = NULL;
  if (sibling == parent)
    anchor = &parent->parent_in_stack;
  else if (sibling && sibling != surface
	   && sibling->requested_parent == parent)
    anchor = &sibling->siblings;
  if (!anchor)
    {
      violation (resource, "invalid-sub-surface-sibling");
      wl_resource_post_error (resource, WL_SUBSURFACE_ERROR_BAD_SURFACE,
			      "relative surface is neither sibling nor parent");
      return;
    }
  wl_list_remove (&surface->siblings);
  wl_list_insert (above ? anchor : anchor->prev, &surface->siblings);
}

static void
subsurface_above (struct wl_client *client, struct wl_resource *resource,
		  struct wl_resource *reference)
{
  subsurface_place (resource, reference, true);
}

static void
subsurface_below (struct wl_client *client, struct wl_resource *resource,
		  struct wl_resource *reference)
{
  subsurface_place (resource, reference, false);
}

static void
subsurface_sync (struct wl_client *client, struct wl_resource *resource)
{
  struct surface *surface = wl_resource_get_user_data (resource);
  if (surface)
    surface->synchronized = true;
}

static void
subsurface_desync (struct wl_client *client, struct wl_resource *resource)
{
  struct surface *surface = wl_resource_get_user_data (resource);
  if (!surface)
    return;
  surface->synchronized = false;
  if (!surface_synchronized (surface))
    {
      /* Desynchronizing an ancestor can release descendants' cached
	 states without a commit on those descendants.  */
      struct surface *child;
      wl_list_for_each (child, &mc.surfaces, link)
	if (child->role == SUBSURFACE_ROLE && !surface_synchronized (child))
	  apply_cached (child);
    }
}

static const struct wl_subsurface_interface subsurface_impl =
  {
    .destroy = destroy_resource,
    .set_position = subsurface_position,
    .place_above = subsurface_above,
    .place_below = subsurface_below,
    .set_sync = subsurface_sync,
    .set_desync = subsurface_desync,
  };

static void
get_subsurface (struct wl_client *client, struct wl_resource *resource,
		uint32_t id, struct wl_resource *child_resource,
		struct wl_resource *parent_resource)
{
  struct surface *child = wl_resource_get_user_data (child_resource);
  struct surface *parent = wl_resource_get_user_data (parent_resource);
  if (child->role == XDG_ROLE || child->subsurface)
    {
      violation (child_resource, "subsurface-role-conflict");
      wl_resource_post_error (resource, WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE,
			      "the child already has a role object");
      return;
    }
  for (struct surface *ancestor = parent; ancestor;
       ancestor = ancestor->requested_parent)
    if (ancestor == child)
      {
	violation (child_resource, "subsurface-parent-cycle");
	wl_resource_post_error (resource, WL_SUBCOMPOSITOR_ERROR_BAD_PARENT,
				"subsurface parent creates a cycle");
	return;
      }
  child->subsurface = wl_resource_create (client, &wl_subsurface_interface,
					 1, id);
  if (!child->subsurface)
    {
      wl_client_post_no_memory (client);
      return;
    }
  child->role = SUBSURFACE_ROLE;
  child->requested_parent = parent;
  child->synchronized = true;
  wl_list_insert (parent->children.prev, &child->siblings);
  wl_resource_set_implementation (child->subsurface, &subsurface_impl,
				  child, subsurface_destroyed);
}

static const struct wl_subcompositor_interface subcompositor_impl =
  { .destroy = destroy_resource, .get_subsurface = get_subsurface };

static void
bind_subcompositor (struct wl_client *client, void *data, uint32_t version,
		    uint32_t id)
{
  struct wl_resource *resource
    = wl_resource_create (client, &wl_subcompositor_interface, version, id);
  if (!resource)
    {
      wl_client_post_no_memory (client);
      return;
    }
  wl_resource_set_implementation (resource, &subcompositor_impl, NULL, NULL);
}

static void
activation_token_serial (struct wl_client *client, struct wl_resource *resource,
			 uint32_t serial, struct wl_resource *seat)
{
  mc.activation_serial = serial;
}

static void
activation_token_app (struct wl_client *client, struct wl_resource *resource,
		      const char *app)
{
}

static void
activation_token_surface (struct wl_client *client, struct wl_resource *resource,
			  struct wl_resource *surface)
{
}

static void
activation_token_commit (struct wl_client *client, struct wl_resource *resource)
{
  mc.activation_requests++;
  xdg_activation_token_v1_send_done (resource, "mock-requested-token");
}

static const struct xdg_activation_token_v1_interface activation_token_impl =
  {.set_serial = activation_token_serial, .set_app_id = activation_token_app,
   .set_surface = activation_token_surface, .commit = activation_token_commit,
   .destroy = destroy_resource};

static void
activation_get_token (struct wl_client *client, struct wl_resource *resource,
		      uint32_t id)
{
  struct wl_resource *token
    = wl_resource_create (client, &xdg_activation_token_v1_interface, 1, id);
  if (token)
    wl_resource_set_implementation (token, &activation_token_impl, NULL, NULL);
  else
    wl_client_post_no_memory (client);
}

static void
activation_activate (struct wl_client *client, struct wl_resource *resource,
		     const char *token, struct wl_resource *surface)
{
  struct surface *target = wl_resource_get_user_data (surface);
  if (!target->xdg_toplevel || !target->mapped)
    violation (resource, "activate-before-toplevel-map");
  mc.activations++;
  mc.activated_surface = wl_resource_get_id (surface);
  /* The fixture sends alphanumeric tokens with hyphens.  */
  snprintf (mc.activation_token, sizeof mc.activation_token, "%.*s",
	    (int) strspn (token, "abcdefghijklmnopqrstuvwxyz0123456789-"), token);
}

static const struct xdg_activation_v1_interface activation_impl =
  {.destroy = destroy_resource, .get_activation_token = activation_get_token,
   .activate = activation_activate};

static void
bind_activation (struct wl_client *client, void *data, uint32_t version,
		 uint32_t id)
{
  struct wl_resource *resource
    = wl_resource_create (client, &xdg_activation_v1_interface, version, id);
  if (resource)
    wl_resource_set_implementation (resource, &activation_impl, NULL, NULL);
  else
    wl_client_post_no_memory (client);
}

static void
surface_destroyed (struct wl_resource *resource)
{
  struct surface *surface = wl_resource_get_user_data (resource);
  struct held_buffer *held, *tmp;

  wl_list_for_each_safe (held, tmp, &mc.held, link)
    if (held->surface == surface)
      {
	buffer_ref_set (&held->ref, NULL);
	wl_list_remove (&held->link);
	free (held->pixels);
	free (held);
      }
  if (surface->viewport)
    wl_resource_set_user_data (surface->viewport, NULL);
  if (surface->fractional)
    wl_resource_set_user_data (surface->fractional, NULL);
  /* On disconnect the role objects may outlive the surface.  */
  if (surface->xdg_surface)
    wl_resource_set_user_data (surface->xdg_surface, NULL);
  if (surface->xdg_toplevel)
    wl_resource_set_user_data (surface->xdg_toplevel, NULL);
  if (surface->subsurface)
    wl_resource_set_user_data (surface->subsurface, NULL);
  wl_list_remove (&surface->siblings);
  wl_list_remove (&surface->parent_in_stack);
  while (!wl_list_empty (&surface->children))
    {
      struct surface *child
	= wl_container_of (surface->children.next, child, siblings);
      wl_list_remove (&child->siblings);
      wl_list_init (&child->siblings);
      child->parent = child->requested_parent = NULL;
      child->cached_relationship = false;
    }
  destroy_frame_list (&surface->pending_frames);
  destroy_frame_list (&surface->cached_frames);
  destroy_frame_list (&surface->frames);
  buffer_ref_set (&surface->pending, NULL);
  buffer_ref_set (&surface->cached, NULL);
  buffer_ref_set (&surface->current, NULL);
  if (mc.keyboard_focus == surface)
    mc.keyboard_focus = NULL;
  struct text_input *input;
  wl_list_for_each (input, &mc.text_inputs, link)
    if (input->entered == surface)
      input->entered = NULL;
  if (mc.pointer_focus == surface)
    mc.pointer_focus = NULL;
  wl_list_remove (&surface->link);
  pixman_region32_fini (&surface->damage);
  pixman_region32_fini (&surface->cached_damage);
  free (surface->image);
  free (surface);
}

/* wl_compositor.  */

static void
compositor_create_surface (struct wl_client *client,
			   struct wl_resource *resource, uint32_t id)
{
  struct surface *surface = calloc (1, sizeof *surface);

  if (!surface)
    {
      wl_client_post_no_memory (client);
      return;
    }
  surface->resource
    = wl_resource_create (client, &wl_surface_interface,
			  wl_resource_get_version (resource), id);
  if (!surface->resource)
    {
      free (surface);
      wl_client_post_no_memory (client);
      return;
    }
  surface->scale = 1;
  pixman_region32_init (&surface->damage);
  pixman_region32_init (&surface->cached_damage);
  buffer_ref_init (&surface->pending);
  buffer_ref_init (&surface->cached);
  buffer_ref_init (&surface->current);
  wl_list_init (&surface->pending_frames);
  wl_list_init (&surface->cached_frames);
  wl_list_init (&surface->frames);
  wl_list_init (&surface->siblings);
  wl_list_init (&surface->children);
  wl_list_insert (&surface->children, &surface->parent_in_stack);
  wl_list_insert (&mc.surfaces, &surface->link);
  wl_resource_set_implementation (surface->resource, &surface_impl, surface,
				  surface_destroyed);
}

static void
compositor_create_region (struct wl_client *client,
			  struct wl_resource *resource, uint32_t id)
{
  struct wl_resource *region
    = wl_resource_create (client, &wl_region_interface, 1, id);
  if (!region)
    {
      wl_client_post_no_memory (client);
      return;
    }
  wl_resource_set_implementation (region, &region_impl, NULL, NULL);
}

static const struct wl_compositor_interface compositor_impl =
  {
    .create_surface = compositor_create_surface,
    .create_region = compositor_create_region,
    .release = destroy_resource,
  };

static void
bind_compositor (struct wl_client *client, void *data, uint32_t version,
		 uint32_t id)
{
  struct wl_resource *resource
    = wl_resource_create (client, &wl_compositor_interface, version, id);
  if (!resource)
    {
      wl_client_post_no_memory (client);
      return;
    }
  wl_resource_set_implementation (resource, &compositor_impl, NULL, NULL);
}

/* wl_seat, wl_keyboard and wl_pointer.  */

static void
keyboard_destroyed (struct wl_resource *resource)
{
  unlink_resource (resource);
}

static const struct wl_keyboard_interface keyboard_impl =
  {
    .release = destroy_resource,
  };

static void
send_keymap (struct wl_resource *keyboard)
{
  int fd = memfd_create ("mockcomp-keymap", MFD_CLOEXEC);

  if (fd < 0)
    die ("memfd_create: %s", strerror (errno));
  if (write (fd, mc.keymap, mc.keymap_size) != (ssize_t) mc.keymap_size)
    die ("writing the keymap: %s", strerror (errno));
  wl_keyboard_send_keymap (keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd,
			   mc.keymap_size);
  close (fd);
}

static void
seat_get_keyboard (struct wl_client *client, struct wl_resource *resource,
		   uint32_t id)
{
  struct wl_resource *keyboard
    = wl_resource_create (client, &wl_keyboard_interface,
			  wl_resource_get_version (resource), id);
  if (!keyboard)
    {
      wl_client_post_no_memory (client);
      return;
    }
  wl_resource_set_implementation (keyboard, &keyboard_impl, NULL,
				  keyboard_destroyed);
  wl_list_insert (&mc.keyboards, wl_resource_get_link (keyboard));
  send_keymap (keyboard);
  if (wl_resource_get_version (keyboard)
      >= WL_KEYBOARD_REPEAT_INFO_SINCE_VERSION)
    wl_keyboard_send_repeat_info (keyboard, mc.repeat_rate, mc.repeat_delay);
}

static void
pointer_set_cursor (struct wl_client *client, struct wl_resource *resource,
		    uint32_t serial, struct wl_resource *surface,
		    int32_t hotspot_x, int32_t hotspot_y)
{
  mc.cursor_sets++;
  mc.cursor_hidden = !surface;
}

/* cursor-shape-v1: mockcomp records the shapes clients ask for.  */

static void
shape_device_set_shape (struct wl_client *client,
			struct wl_resource *resource, uint32_t serial,
			uint32_t shape)
{
  mc.cursor_shape = shape;
  mc.cursor_shapes++;
  mc.cursor_hidden = false;
}

static const struct wp_cursor_shape_device_v1_interface shape_device_impl =
  {
    .destroy = destroy_resource,
    .set_shape = shape_device_set_shape,
  };

static void
shape_manager_get_pointer (struct wl_client *client,
			   struct wl_resource *resource, uint32_t id,
			   struct wl_resource *pointer)
{
  struct wl_resource *device
    = wl_resource_create (client, &wp_cursor_shape_device_v1_interface,
			  wl_resource_get_version (resource), id);
  if (!device)
    {
      wl_client_post_no_memory (client);
      return;
    }
  wl_resource_set_implementation (device, &shape_device_impl, NULL, NULL);
}

static void
shape_manager_get_tablet_tool (struct wl_client *client,
			       struct wl_resource *resource, uint32_t id,
			       struct wl_resource *tool)
{
  wl_client_post_implementation_error (client, "mockcomp has no tablets");
}

static const struct wp_cursor_shape_manager_v1_interface shape_manager_impl =
  {
    .destroy = destroy_resource,
    .get_pointer = shape_manager_get_pointer,
    .get_tablet_tool_v2 = shape_manager_get_tablet_tool,
  };

static void
bind_shape_manager (struct wl_client *client, void *data, uint32_t version,
		    uint32_t id)
{
  struct wl_resource *resource
    = wl_resource_create (client, &wp_cursor_shape_manager_v1_interface,
			  version, id);
  if (resource)
    wl_resource_set_implementation (resource, &shape_manager_impl, NULL,
				    NULL);
  else
    wl_client_post_no_memory (client);
}

static const struct wl_pointer_interface pointer_impl =
  {
    .set_cursor = pointer_set_cursor,
    .release = destroy_resource,
  };

static void
seat_get_pointer (struct wl_client *client, struct wl_resource *resource,
		  uint32_t id)
{
  struct wl_resource *pointer
    = wl_resource_create (client, &wl_pointer_interface,
			  wl_resource_get_version (resource), id);
  if (!pointer)
    {
      wl_client_post_no_memory (client);
      return;
    }
  wl_resource_set_implementation (pointer, &pointer_impl, NULL,
				  unlink_resource);
  wl_list_insert (&mc.pointers, wl_resource_get_link (pointer));
}

static void
seat_get_touch (struct wl_client *client, struct wl_resource *resource,
		uint32_t id)
{
  wl_resource_post_error (resource, WL_SEAT_ERROR_MISSING_CAPABILITY,
			  "mockcomp has no touch device");
}

static const struct wl_seat_interface seat_impl =
  {
    .get_pointer = seat_get_pointer,
    .get_keyboard = seat_get_keyboard,
    .get_touch = seat_get_touch,
    .release = destroy_resource,
  };

static void
bind_seat (struct wl_client *client, void *data, uint32_t version,
	   uint32_t id)
{
  struct wl_resource *resource
    = wl_resource_create (client, &wl_seat_interface, version, id);
  if (!resource)
    {
      wl_client_post_no_memory (client);
      return;
    }
  wl_resource_set_implementation (resource, &seat_impl, NULL,
				  unlink_resource);
  wl_list_insert (&mc.seats, wl_resource_get_link (resource));
  wl_seat_send_capabilities (resource, mc.seat_capabilities);
  if (version >= WL_SEAT_NAME_SINCE_VERSION)
    wl_seat_send_name (resource, "seat0");
}

/* One default output and one attachable high-density output.  */

static const struct wl_output_interface output_impl =
  {
    .release = destroy_resource,
  };

static void
bind_output (struct wl_client *client, void *data, uint32_t version,
	     uint32_t id)
{
  struct wl_resource *resource
    = wl_resource_create (client, &wl_output_interface, version, id);
  if (!resource)
    {
      wl_client_post_no_memory (client);
      return;
    }
  struct mock_output *output = data;
  wl_list_insert (&output->resources, wl_resource_get_link (resource));
  wl_resource_set_implementation (resource, &output_impl, output,
                                  unlink_resource);
  wl_output_send_geometry (resource, output->x, 0, 520, 290,
                           WL_OUTPUT_SUBPIXEL_UNKNOWN, "mockcomp",
                           output->name,
                           WL_OUTPUT_TRANSFORM_NORMAL);
  wl_output_send_mode (resource,
                       WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
                       1920, 1080, 60000);
  if (version >= WL_OUTPUT_SCALE_SINCE_VERSION)
    wl_output_send_scale (resource, output->scale);
  if (version >= WL_OUTPUT_NAME_SINCE_VERSION)
    {
      wl_output_send_name (resource, output->name);
      wl_output_send_description (resource, "mockcomp test output");
    }
  if (version >= WL_OUTPUT_DONE_SINCE_VERSION)
    wl_output_send_done (resource);
}

static struct wl_resource *
client_output (struct mock_output *output, struct wl_client *client)
{
  struct wl_resource *resource;
  wl_resource_for_each (resource, &output->resources)
    if (wl_resource_get_client (resource) == client)
      return resource;
  return NULL;
}

static void
fractional_destroyed (struct wl_resource *resource)
{
  struct surface *surface = wl_resource_get_user_data (resource);
  if (surface)
    surface->fractional = NULL;
}

static const struct wp_fractional_scale_v1_interface fractional_impl =
{
  .destroy = destroy_resource,
};

static void
get_fractional_scale (struct wl_client *client, struct wl_resource *resource,
                      uint32_t id, struct wl_resource *surface_resource)
{
  struct surface *surface = wl_resource_get_user_data (surface_resource);
  if (surface->fractional)
    {
      wl_resource_post_error
	(resource,
	 WP_FRACTIONAL_SCALE_MANAGER_V1_ERROR_FRACTIONAL_SCALE_EXISTS,
         "surface already has a fractional scale object");
      return;
    }
  surface->fractional = wl_resource_create (client,
                                           &wp_fractional_scale_v1_interface,
                                           1, id);
  if (!surface->fractional)
    {
      wl_client_post_no_memory (client);
      return;
    }
  wl_resource_set_implementation (surface->fractional, &fractional_impl,
                                  surface, fractional_destroyed);
}

static const struct wp_fractional_scale_manager_v1_interface fractionals_impl =
{
  .destroy = destroy_resource,
  .get_fractional_scale = get_fractional_scale,
};

static void
bind_fractionals (struct wl_client *client, void *data, uint32_t version,
                  uint32_t id)
{
  struct wl_resource *resource = wl_resource_create
    (client, &wp_fractional_scale_manager_v1_interface, version, id);
  if (resource)
    wl_resource_set_implementation (resource, &fractionals_impl, NULL, NULL);
  else
    wl_client_post_no_memory (client);
}

static void
viewport_destroyed (struct wl_resource *resource)
{
  struct surface *surface = wl_resource_get_user_data (resource);
  if (surface)
    {
      surface->viewport = NULL;
      surface->destination_width = surface->destination_height = 0;
    }
}

static void
viewport_source (struct wl_client *client, struct wl_resource *resource,
                 wl_fixed_t x, wl_fixed_t y,
                 wl_fixed_t width, wl_fixed_t height)
{
  wl_client_post_implementation_error (client, "unexpected viewport source");
}

static void
viewport_destination (struct wl_client *client, struct wl_resource *resource,
                      int32_t width, int32_t height)
{
  struct surface *surface = wl_resource_get_user_data (resource);
  if (surface)
    {
      surface->destination_width = width;
      surface->destination_height = height;
    }
}

static const struct wp_viewport_interface viewport_impl =
{
  .destroy = destroy_resource,
  .set_source = viewport_source,
  .set_destination = viewport_destination,
};

static void
get_viewport (struct wl_client *client, struct wl_resource *resource,
              uint32_t id, struct wl_resource *surface_resource)
{
  struct surface *surface = wl_resource_get_user_data (surface_resource);
  if (surface->viewport)
    {
      wl_resource_post_error (resource, WP_VIEWPORTER_ERROR_VIEWPORT_EXISTS,
                              "surface already has a viewport");
      return;
    }
  surface->viewport = wl_resource_create (client, &wp_viewport_interface,
                                          1, id);
  if (!surface->viewport)
    {
      wl_client_post_no_memory (client);
      return;
    }
  wl_resource_set_implementation (surface->viewport, &viewport_impl,
                                  surface, viewport_destroyed);
}

static const struct wp_viewporter_interface viewporter_impl =
{
  .destroy = destroy_resource,
  .get_viewport = get_viewport,
};

static void
bind_viewporter (struct wl_client *client, void *data, uint32_t version,
                 uint32_t id)
{
  struct wl_resource *resource = wl_resource_create
    (client, &wp_viewporter_interface, version, id);
  if (resource)
    wl_resource_set_implementation (resource, &viewporter_impl, NULL, NULL);
  else
    wl_client_post_no_memory (client);
}

/* xdg-output: exact logical geometry for each output.  Version 3,
   so wl_output.done follows the xdg_output events.  */

static const struct zxdg_output_v1_interface xdg_output_impl =
{
  .destroy = destroy_resource,
};

static void
get_xdg_output (struct wl_client *client, struct wl_resource *resource,
		uint32_t id, struct wl_resource *output_resource)
{
  struct mock_output *output = wl_resource_get_user_data (output_resource);
  struct wl_resource *xdg_output
    = wl_resource_create (client, &zxdg_output_v1_interface,
			  wl_resource_get_version (resource), id);
  if (!xdg_output)
    {
      wl_client_post_no_memory (client);
      return;
    }
  wl_resource_set_implementation (xdg_output, &xdg_output_impl, NULL, NULL);
  zxdg_output_v1_send_logical_position (xdg_output, output->x, 0);
  zxdg_output_v1_send_logical_size
    (xdg_output, 1920 * 120 / output->logical_scale,
     1080 * 120 / output->logical_scale);
  if (wl_resource_get_version (xdg_output)
      >= ZXDG_OUTPUT_V1_NAME_SINCE_VERSION)
    zxdg_output_v1_send_name (xdg_output, output->name);
  if (wl_resource_get_version (xdg_output) < 3)
    zxdg_output_v1_send_done (xdg_output);
  else if (wl_resource_get_version (output_resource)
	   >= WL_OUTPUT_DONE_SINCE_VERSION)
    wl_output_send_done (output_resource);
}

static const struct zxdg_output_manager_v1_interface output_manager_impl =
{
  .destroy = destroy_resource,
  .get_xdg_output = get_xdg_output,
};

static void
bind_output_manager (struct wl_client *client, void *data, uint32_t version,
		     uint32_t id)
{
  struct wl_resource *resource = wl_resource_create
    (client, &zxdg_output_manager_v1_interface, version, id);
  if (resource)
    wl_resource_set_implementation (resource, &output_manager_impl, NULL,
				    NULL);
  else
    wl_client_post_no_memory (client);
}

/* Data devices.  mockcomp can offer a selection of its own, for the
   backend's debug pipe transfer; selections Emacs sets are counted.  */

static void
offer_accept (struct wl_client *client, struct wl_resource *resource,
	      uint32_t serial, const char *mime_type)
{
}

static void
offer_receive (struct wl_client *client, struct wl_resource *resource,
	       const char *mime_type, int32_t fd)
{
  if (mc.hold_offer_pipe && mc.held_offer_pipe < 0)
    {
      mc.held_offer_pipe = fd;
      return;
    }
  const char *text = wl_resource_get_user_data (resource);
  size_t len = text ? strlen (text) : 0;
  ssize_t written;

  /* The client reads the pipe after this request; small payloads fit
     in the pipe buffer, so a plain write does not block.  */
  while (len > 0 && (written = write (fd, text, len)) > 0)
    {
      text += written;
      len -= written;
    }
  close (fd);
}

static void
offer_finish (struct wl_client *client, struct wl_resource *resource)
{
}

static void
offer_set_actions (struct wl_client *client, struct wl_resource *resource,
		   uint32_t dnd_actions, uint32_t preferred_action)
{
}

static const struct wl_data_offer_interface offer_impl =
  {
    .accept = offer_accept,
    .receive = offer_receive,
    .destroy = destroy_resource,
    .finish = offer_finish,
    .set_actions = offer_set_actions,
  };

static void
source_offer (struct wl_client *client, struct wl_resource *resource,
	      const char *mime_type)
{
}

static void
source_set_actions (struct wl_client *client, struct wl_resource *resource,
		    uint32_t dnd_actions)
{
}

static const struct wl_data_source_interface source_impl =
  {
    .offer = source_offer,
    .destroy = destroy_resource,
    .set_actions = source_set_actions,
  };

static void
device_start_drag (struct wl_client *client, struct wl_resource *resource,
		   struct wl_resource *source, struct wl_resource *origin,
		   struct wl_resource *icon, uint32_t serial)
{
}

static void
device_set_selection (struct wl_client *client, struct wl_resource *resource,
		      struct wl_resource *source, uint32_t serial)
{
  if (mc.selection_source && mc.selection_source != source)
    wl_data_source_send_cancelled (mc.selection_source);
  mc.selection_source = source;
  mc.selections_set++;
}

static void
source_destroyed (struct wl_resource *source)
{
  if (mc.selection_source == source)
    mc.selection_source = NULL;
}

static void
device_destroyed (struct wl_resource *resource)
{
  unlink_resource (resource);
}

static const struct wl_data_device_interface device_impl =
  {
    .start_drag = device_start_drag,
    .set_selection = device_set_selection,
    .release = destroy_resource,
  };

static void
manager_create_data_source (struct wl_client *client,
			    struct wl_resource *resource, uint32_t id)
{
  struct wl_resource *source
    = wl_resource_create (client, &wl_data_source_interface,
			  wl_resource_get_version (resource), id);
  if (!source)
    {
      wl_client_post_no_memory (client);
      return;
    }
  wl_resource_set_implementation (source, &source_impl, NULL,
				  source_destroyed);
}

static void
manager_get_data_device (struct wl_client *client,
			 struct wl_resource *resource, uint32_t id,
			 struct wl_resource *seat)
{
  struct wl_resource *device
    = wl_resource_create (client, &wl_data_device_interface,
			  wl_resource_get_version (resource), id);
  if (!device)
    {
      wl_client_post_no_memory (client);
      return;
    }
  wl_resource_set_implementation (device, &device_impl, NULL,
				  device_destroyed);
  wl_list_insert (&mc.data_devices, wl_resource_get_link (device));
}

static const struct wl_data_device_manager_interface manager_impl =
  {
    .create_data_source = manager_create_data_source,
    .get_data_device = manager_get_data_device,
    .release = destroy_resource,
  };

static void
bind_data_device_manager (struct wl_client *client, void *data,
			  uint32_t version, uint32_t id)
{
  struct wl_resource *resource
    = wl_resource_create (client, &wl_data_device_manager_interface,
			  version, id);
  if (!resource)
    {
      wl_client_post_no_memory (client);
      return;
    }
  wl_resource_set_implementation (resource, &manager_impl, NULL, NULL);
}

/* xdg-shell.  */

/* Typed no-ops for the requests mockcomp accepts and ignores.  */

static void
noop (struct wl_client *client, struct wl_resource *resource)
{
}

static void
noop_u (struct wl_client *client, struct wl_resource *resource, uint32_t u)
{
}

static void
noop_ii (struct wl_client *client, struct wl_resource *resource,
	 int32_t a, int32_t b)
{
}

static void
noop_iiii (struct wl_client *client, struct wl_resource *resource,
	   int32_t a, int32_t b, int32_t c, int32_t d)
{
}

static void
noop_s (struct wl_client *client, struct wl_resource *resource,
	const char *s)
{
}

static void
noop_o (struct wl_client *client, struct wl_resource *resource,
	struct wl_resource *object)
{
}

static void
toplevel_show_window_menu (struct wl_client *client,
			   struct wl_resource *resource,
			   struct wl_resource *seat, uint32_t serial,
			   int32_t x, int32_t y)
{
}

static void
toplevel_move (struct wl_client *client, struct wl_resource *resource,
	       struct wl_resource *seat, uint32_t serial)
{
}

static void
toplevel_resize (struct wl_client *client, struct wl_resource *resource,
		 struct wl_resource *seat, uint32_t serial, uint32_t edges)
{
}

static const struct xdg_positioner_interface positioner_impl =
  {
    .destroy = destroy_resource,
    .set_size = noop_ii,
    .set_anchor_rect = noop_iiii,
    .set_anchor = noop_u,
    .set_gravity = noop_u,
    .set_constraint_adjustment = noop_u,
    .set_offset = noop_ii,
    .set_reactive = noop,
    .set_parent_size = noop_ii,
    .set_parent_configure = noop_u,
  };

static const struct xdg_toplevel_interface toplevel_impl =
  {
    .destroy = destroy_resource,
    .set_parent = noop_o,
    .set_title = noop_s,
    .set_app_id = noop_s,
    .show_window_menu = toplevel_show_window_menu,
    .move = toplevel_move,
    .resize = toplevel_resize,
    .set_max_size = noop_ii,
    .set_min_size = noop_ii,
    .set_maximized = noop,
    .unset_maximized = noop,
    .set_fullscreen = noop_o,
    .unset_fullscreen = noop,
    .set_minimized = noop,
  };

static void
toplevel_destroyed (struct wl_resource *resource)
{
  struct surface *surface = wl_resource_get_user_data (resource);
  if (surface)
    surface->xdg_toplevel = NULL;
}

static void
xdg_surface_get_toplevel (struct wl_client *client,
			  struct wl_resource *resource, uint32_t id)
{
  struct surface *surface = wl_resource_get_user_data (resource);
  struct wl_resource *toplevel
    = wl_resource_create (client, &xdg_toplevel_interface,
			  wl_resource_get_version (resource), id);
  if (!toplevel)
    {
      wl_client_post_no_memory (client);
      return;
    }
  wl_resource_set_implementation (toplevel, &toplevel_impl, surface,
				  toplevel_destroyed);
  if (surface)
    surface->xdg_toplevel = toplevel;
}

static void
xdg_surface_get_popup (struct wl_client *client, struct wl_resource *resource,
		       uint32_t id, struct wl_resource *parent,
		       struct wl_resource *positioner)
{
  /* The backend has no popups.  */
  violation (resource, "unexpected-popup");
  wl_resource_post_error (resource, XDG_SURFACE_ERROR_NOT_CONSTRUCTED,
			  "mockcomp does not implement popups");
}

static void
xdg_surface_set_window_geometry (struct wl_client *client,
				 struct wl_resource *resource, int32_t x,
				 int32_t y, int32_t width, int32_t height)
{
  struct surface *surface = wl_resource_get_user_data (resource);
  if (!surface)
    return;
  if (width <= 0 || height <= 0)
    {
      violation (resource, "invalid-window-geometry");
      wl_resource_post_error (resource, XDG_SURFACE_ERROR_INVALID_SIZE,
			      "window geometry must have positive size");
      return;
    }
  surface->requested_geometry[0] = x;
  surface->requested_geometry[1] = y;
  surface->requested_geometry[2] = width;
  surface->requested_geometry[3] = height;
}

static void
xdg_surface_ack_configure (struct wl_client *client,
			   struct wl_resource *resource, uint32_t serial)
{
  struct surface *surface = wl_resource_get_user_data (resource);

  if (!surface)
    return;
  /* Serials only grow (wrap-around is far beyond any test), so a valid
     ack names a configure we sent and is newer than the last ack.  */
  if (serial > surface->last_configure_serial
      || (surface->acked && serial <= surface->last_acked_serial))
    {
      violation (resource, "invalid-ack-serial");
      wl_resource_post_error (resource, XDG_SURFACE_ERROR_INVALID_SERIAL,
			      "ack_configure with serial %u", serial);
      return;
    }
  surface->last_acked_serial = serial;
  surface->acked = true;
}

static const struct xdg_surface_interface xdg_surface_impl =
  {
    .destroy = destroy_resource,
    .get_toplevel = xdg_surface_get_toplevel,
    .get_popup = xdg_surface_get_popup,
    .set_window_geometry = xdg_surface_set_window_geometry,
    .ack_configure = xdg_surface_ack_configure,
  };

static void
xdg_surface_destroyed (struct wl_resource *resource)
{
  struct surface *surface = wl_resource_get_user_data (resource);
  if (surface)
    {
      surface->xdg_surface = NULL;
      surface->initial_commit_done = false;
      surface->acked = false;
      surface->mapped = false;
    }
}

static void
wm_base_create_positioner (struct wl_client *client,
			   struct wl_resource *resource, uint32_t id)
{
  struct wl_resource *positioner
    = wl_resource_create (client, &xdg_positioner_interface,
			  wl_resource_get_version (resource), id);
  if (!positioner)
    {
      wl_client_post_no_memory (client);
      return;
    }
  wl_resource_set_implementation (positioner, &positioner_impl, NULL, NULL);
}

static void
wm_base_get_xdg_surface (struct wl_client *client,
			 struct wl_resource *resource, uint32_t id,
			 struct wl_resource *surface_resource)
{
  struct surface *surface = wl_resource_get_user_data (surface_resource);
  struct wl_resource *xdg_surface;

  if (surface->role == SUBSURFACE_ROLE || surface->xdg_surface)
    {
      violation (surface_resource, "xdg-surface-role-conflict");
      wl_resource_post_error (resource, XDG_WM_BASE_ERROR_ROLE,
			      "surface already has a role");
      return;
    }
  if (surface->current.buffer || surface->attached)
    {
      violation (surface_resource, "xdg-surface-with-buffer");
      wl_resource_post_error (resource, XDG_WM_BASE_ERROR_INVALID_SURFACE_STATE,
			      "surface already has a buffer");
      return;
    }
  xdg_surface = wl_resource_create (client, &xdg_surface_interface,
				    wl_resource_get_version (resource), id);
  if (!xdg_surface)
    {
      wl_client_post_no_memory (client);
      return;
    }
  wl_resource_set_implementation (xdg_surface, &xdg_surface_impl, surface,
				  xdg_surface_destroyed);
  surface->xdg_surface = xdg_surface;
  surface->role = XDG_ROLE;
}

static void
wm_base_pong (struct wl_client *client, struct wl_resource *resource,
	      uint32_t serial)
{
  mc.pongs++;
}

static void
wm_base_destroyed (struct wl_resource *resource)
{
  unlink_resource (resource);
}

static const struct xdg_wm_base_interface wm_base_impl =
  {
    .destroy = destroy_resource,
    .create_positioner = wm_base_create_positioner,
    .get_xdg_surface = wm_base_get_xdg_surface,
    .pong = wm_base_pong,
  };

static void
bind_wm_base (struct wl_client *client, void *data, uint32_t version,
	      uint32_t id)
{
  struct wl_resource *resource
    = wl_resource_create (client, &xdg_wm_base_interface, version, id);
  if (!resource)
    {
      wl_client_post_no_memory (client);
      return;
    }
  wl_resource_set_implementation (resource, &wm_base_impl, NULL,
				  wm_base_destroyed);
  wl_list_insert (&mc.wm_bases, wl_resource_get_link (resource));
}

/* Frame callbacks fire on a fixed tick unless the runner withholds
   them.  */

static int
frame_tick (void *data)
{
  struct surface *surface;
  struct wl_resource *callback, *tmp;
  uint32_t time = now_ms ();

  if (mc.frame_callbacks)
    wl_list_for_each (surface, &mc.surfaces, link)
      wl_resource_for_each_safe (callback, tmp, &surface->frames)
	{
	  wl_callback_send_done (callback, time);
	  wl_resource_destroy (callback);
	  mc.frames_done++;
	}
  wl_event_source_timer_update (mc.frame_timer, FRAME_INTERVAL_MS);
  return 0;
}

/* The request and event trace.  */

static void
trace_message (void *data, enum wl_protocol_logger_type direction,
	       const struct wl_protocol_logger_message *message)
{
  const char *sig = message->message->signature;
  int i = 0;

  if (!mc.trace)
    return;
  fprintf (mc.trace, "%u %s %s@%u.%s(", now_ms (),
	   direction == WL_PROTOCOL_LOGGER_REQUEST ? "->" : "<-",
	   wl_resource_get_class (message->resource),
	   wl_resource_get_id (message->resource), message->message->name);
  for (; *sig && i < message->arguments_count; sig++)
    {
      const union wl_argument *arg = &message->arguments[i];

      if ((*sig >= '0' && *sig <= '9') || *sig == '?')
	continue;
      if (i > 0)
	fputs (", ", mc.trace);
      switch (*sig)
	{
	case 'i':
	  fprintf (mc.trace, "%d", arg->i);
	  break;
	case 'u':
	  fprintf (mc.trace, "%u", arg->u);
	  break;
	case 'f':
	  fprintf (mc.trace, "%f", wl_fixed_to_double (arg->f));
	  break;
	case 's':
	  fprintf (mc.trace, "\"%s\"", arg->s ? arg->s : "");
	  break;
	case 'o':
	  fprintf (mc.trace, "obj %u",
		   arg->o ? wl_resource_get_id ((struct wl_resource *) arg->o)
		   : 0);
	  break;
	case 'n':
	  fprintf (mc.trace, "new_id %u", arg->n);
	  break;
	case 'a':
	  fprintf (mc.trace, "array[%zu]", arg->a ? arg->a->size : 0);
	  break;
	case 'h':
	  fprintf (mc.trace, "fd %d", arg->h);
	  break;
	}
      i++;
    }
  fputs (")\n", mc.trace);
}

/* Control commands.  One command per line; each gets one reply line,
   "ok", "error <reason>", or a JSON object.  */

static struct wl_client *
first_client (void)
{
  struct wl_list *clients = wl_display_get_client_list (mc.display);
  return wl_list_empty (clients) ? NULL : wl_client_from_link (clients->next);
}

static void
control_reply (const char *fmt, ...)
{
  char *reply;
  va_list ap;

  if (mc.control_fd < 0)
    return;
  va_start (ap, fmt);
  if (vasprintf (&reply, fmt, ap) < 0)
    die ("out of memory");
  va_end (ap);
  if (write (mc.control_fd, reply, strlen (reply)) < 0
      || write (mc.control_fd, "\n", 1) < 0)
    {
      close (mc.control_fd);
      mc.control_fd = -1;
    }
  free (reply);
}

/* Live resources of every client by interface, for the soak's leak
   check.  */

struct resource_counts
{
  struct { const char *name; unsigned long count; } entries[64];
  int n;
};

static enum wl_iterator_result
count_resource (struct wl_resource *resource, void *data)
{
  struct resource_counts *counts = data;
  const char *name = wl_resource_get_class (resource);

  for (int i = 0; i < counts->n; i++)
    if (!strcmp (counts->entries[i].name, name))
      {
	counts->entries[i].count++;
	return WL_ITERATOR_CONTINUE;
      }
  if (counts->n < 64)
    {
      counts->entries[counts->n].name = name;
      counts->entries[counts->n++].count = 1;
    }
  return WL_ITERATOR_CONTINUE;
}

static void
cmd_status (void)
{
  char *buf;
  size_t len;
  FILE *out = open_memstream (&buf, &len);
  struct surface *surface;
  int n_surfaces = 0, n_mapped = 0, n_held = 0;
  struct held_buffer *held;
  struct surface *first_mapped = NULL;

  wl_list_for_each (surface, &mc.surfaces, link)
    {
      n_surfaces++;
      n_mapped += surface_shown (surface);
      if (surface->xdg_toplevel && surface->mapped && !first_mapped)
	first_mapped = surface;
    }
  wl_list_for_each (held, &mc.held, link)
    n_held++;

  fprintf (out, "{\"clients\": %d, \"surfaces\": %d, \"mapped\": %d, "
	   "\"held_buffers\": %d, \"commits\": %lu, \"pings\": %lu, "
	   "\"pongs\": %lu, \"keys\": %lu, \"selections_set\": %lu, "
	   "\"frames_done\": %lu, \"keyboard_focus\": %u, "
	   "\"pointer_focus\": %u, \"modifiers\": [%u, %u, %u, %u], "
	   "\"buttons\": %lu, \"axis_events\": %lu, "
	   "\"cursor_shape\": %u, \"cursor_shapes\": %lu, "
	   "\"cursor_sets\": %lu, \"cursor_hidden\": %s, "
	   "\"stalled\": %s, \"violation_total\": %lu, \"violations\": [",
	   wl_list_length (wl_display_get_client_list (mc.display)),
	   n_surfaces, n_mapped, n_held, mc.commits, mc.pings, mc.pongs,
	   mc.keys, mc.selections_set, mc.frames_done,
	   (mc.keyboard_focus
	    ? wl_resource_get_id (mc.keyboard_focus->resource) : 0),
	   (mc.pointer_focus
	    ? wl_resource_get_id (mc.pointer_focus->resource) : 0),
	   mc.mods_depressed, mc.mods_latched, mc.mods_locked, mc.group,
	   mc.buttons, mc.axis_events, mc.cursor_shape, mc.cursor_shapes,
	   mc.cursor_sets, mc.cursor_hidden ? "true" : "false",
	   mc.stalled ? "true" : "false",
	   mc.violation_total);
  for (int i = 0; i < mc.n_violations; i++)
    fprintf (out, "%s\"%s\"", i ? ", " : "", mc.violations[i]);
  fputs ("]", out);
  fprintf (out, ", \"activation\": [%u, %u, %u, %u, \"%s\"]",
	   mc.activations, mc.activation_requests, mc.activated_surface,
	   mc.activation_serial, mc.activation_token);
  fprintf (out, ", \"held_offer_pipe\": %s",
	   mc.held_offer_pipe >= 0 ? "true" : "false");
  struct resource_counts counts = { .n = 0 };
  struct wl_client *client;
  wl_client_for_each (client, wl_display_get_client_list (mc.display))
    wl_client_for_each_resource (client, count_resource, &counts);
  fputs (", \"resources\": {", out);
  for (int i = 0; i < counts.n; i++)
    fprintf (out, "%s\"%s\": %lu", i ? ", " : "", counts.entries[i].name,
	     counts.entries[i].count);
  fputs ("}", out);
  /* The text input most recently created: whether it is enabled, its
     cursor rectangle, content type and commit count.  */
  if (!wl_list_empty (&mc.text_inputs))
    {
      struct text_input *input
	= wl_container_of (mc.text_inputs.next, input, link);
      fprintf (out, ", \"im\": {\"enabled\": %s, \"entered\": %s, "
	       "\"rect\": [%d, %d, %d, %d], \"content\": [%u, %u], "
	       "\"commits\": %u, \"first_rect\": ",
	       input->enabled ? "true" : "false",
	       input->entered ? "true" : "false",
	       input->rect[0], input->rect[1], input->rect[2], input->rect[3],
	       input->hint, input->purpose, input->commits);
      if (input->first_rect_seen)
	fprintf (out, "[%d, %d, %d, %d]}", input->first_rect[0],
		 input->first_rect[1], input->first_rect[2],
		 input->first_rect[3]);
      else
	fputs ("null}", out);
    }
  /* The first mapped surface's last buffer size, and whether its latest
     configure has been acked.  */
  if (first_mapped)
    fprintf (out, ", \"buffer_size\": [%d, %d], \"content_commits\": %lu, "
	     "\"acked_latest\": %s",
	     first_mapped->buffer_width, first_mapped->buffer_height,
	     first_mapped->content_commits,
	     (first_mapped->acked
	      && first_mapped->last_acked_serial
		 == first_mapped->last_configure_serial)
	     ? "true" : "false");
  fprintf (out, ", \"damage_checks\": %lu, \"held_checks\": %lu, "
           "\"toplevels\": [", mc.damage_checks, mc.held_checks);
  const char *sep = "";
  wl_list_for_each (surface, &mc.surfaces, link)
    if (surface->xdg_toplevel)
      {
        fprintf (out, "%s{\"id\": %" PRIu32 ", \"mapped\": %s, "
                 "\"buffer_size\": [%d, %d], \"content_commits\": %lu, "
                 "\"acked_latest\": %s, \"damage_pixels\": %" PRIu64 ", "
                 "\"scale\": %d, \"viewport\": [%d, %d], "
                 "\"commits\": %lu, \"geometry\": [%d, %d, %d, %d]}",
                 sep, wl_resource_get_id (surface->resource),
                 surface->mapped ? "true" : "false", surface->buffer_width,
                 surface->buffer_height, surface->content_commits,
                 surface->acked && (surface->last_acked_serial
                                    == surface->last_configure_serial)
                 ? "true" : "false", surface->damage_pixels, surface->scale,
                 surface->destination_width, surface->destination_height,
                 surface->commits, surface->geometry[0], surface->geometry[1],
                 surface->geometry[2], surface->geometry[3]);
        sep = ", ";
      }
  fputs ("], \"subsurfaces\": [", out);
  sep = "";
  wl_list_for_each (surface, &mc.surfaces, link)
    if (surface->subsurface)
      {
	struct surface *parent = surface->parent;
	struct surface *requested = surface->requested_parent;
	fprintf (out, "%s{\"id\": %" PRIu32 ", \"parent\": %" PRIu32
		 ", \"requested_parent\": %" PRIu32
		 ", \"position\": [%d, %d], \"requested_position\": [%d, %d], "
		 "\"stacking\": %d, \"mapped\": %s, \"synchronized\": %s, "
		 "\"buffer_size\": [%d, %d], \"scale\": %d, "
		 "\"viewport\": [%d, %d], \"commits\": %lu, "
		 "\"content_commits\": %lu}",
		 sep, wl_resource_get_id (surface->resource),
		 parent ? wl_resource_get_id (parent->resource) : 0,
		 requested ? wl_resource_get_id (requested->resource) : 0,
		 surface->x, surface->y, surface->requested_x,
		 surface->requested_y, surface->stacking,
		 surface_shown (surface) ? "true" : "false",
		 surface_synchronized (surface) ? "true" : "false",
		 surface->buffer_width, surface->buffer_height, surface->scale,
		 surface->destination_width, surface->destination_height,
		 surface->commits, surface->content_commits);
	sep = ", ";
      }
  fputs ("], \"held\": [", out);
  sep = "";
  wl_list_for_each (held, &mc.held, link)
    if (held->ref.buffer)
      {
        fprintf (out, "%s[%" PRIu32 ", %lu]", sep,
                 wl_resource_get_id (held->surface->resource), held->commit);
        sep = ", ";
      }
  /* The same buffers with their sizes, so a test can tell which pool
     each belongs to.  */
  fputs ("], \"held_sizes\": [", out);
  sep = "";
  wl_list_for_each (held, &mc.held, link)
    if (held->ref.buffer)
      {
        struct wl_shm_buffer *shm = wl_shm_buffer_get (held->ref.buffer);
        fprintf (out, "%s[%" PRIu32 ", %lu, %d, %d]", sep,
                 wl_resource_get_id (held->surface->resource), held->commit,
                 shm ? wl_shm_buffer_get_width (shm) : 0,
                 shm ? wl_shm_buffer_get_height (shm) : 0);
        sep = ", ";
      }
  fputs ("]}", out);
  fclose (out);
  control_reply ("%s", buf);
  free (buf);
}

static void
cmd_ping (void)
{
  struct wl_resource *wm_base;
  uint32_t serial = wl_display_next_serial (mc.display);

  wl_resource_for_each (wm_base, &mc.wm_bases)
    {
      xdg_wm_base_send_ping (wm_base, serial);
      mc.pings++;
    }
  control_reply ("{\"serial\": %u}", serial);
}

/* The input method: text-input-v3 objects follow keyboard focus, as in
   compositors, and the `im' command sends their events.  Mockcomp
   applies each object's state on commit and counts the commits, which
   done events report as their serial.  */

static void
text_input_destroyed (struct wl_resource *resource)
{
  struct text_input *input = wl_resource_get_user_data (resource);
  wl_list_remove (&input->link);
  free (input);
}

static void
text_input_enable (struct wl_client *client, struct wl_resource *resource)
{
  struct text_input *input = wl_resource_get_user_data (resource);
  /* Enabling resets the rest of the state.  */
  input->pending_enabled = true;
  memset (input->pending_rect, 0, sizeof input->pending_rect);
  input->pending_hint = input->pending_purpose = 0;
  input->rect_pending = false;
}

static void
text_input_disable (struct wl_client *client, struct wl_resource *resource)
{
  struct text_input *input = wl_resource_get_user_data (resource);
  input->pending_enabled = false;
}

static void
text_input_set_surrounding (struct wl_client *client,
			    struct wl_resource *resource, const char *text,
			    int32_t cursor, int32_t anchor)
{
}

static void
text_input_set_change_cause (struct wl_client *client,
			     struct wl_resource *resource, uint32_t cause)
{
}

static void
text_input_set_content_type (struct wl_client *client,
			     struct wl_resource *resource, uint32_t hint,
			     uint32_t purpose)
{
  struct text_input *input = wl_resource_get_user_data (resource);
  input->pending_hint = hint;
  input->pending_purpose = purpose;
}

static void
text_input_set_cursor_rectangle (struct wl_client *client,
				 struct wl_resource *resource, int32_t x,
				 int32_t y, int32_t width, int32_t height)
{
  struct text_input *input = wl_resource_get_user_data (resource);
  input->pending_rect[0] = x;
  input->pending_rect[1] = y;
  input->pending_rect[2] = width;
  input->pending_rect[3] = height;
  input->rect_pending = true;
}

static void
text_input_commit (struct wl_client *client, struct wl_resource *resource)
{
  struct text_input *input = wl_resource_get_user_data (resource);
  input->enabled = input->pending_enabled;
  memcpy (input->rect, input->pending_rect, sizeof input->rect);
  if (input->rect_pending && !input->first_rect_seen)
    {
      memcpy (input->first_rect, input->pending_rect,
	      sizeof input->first_rect);
      input->first_rect_seen = true;
    }
  input->rect_pending = false;
  input->hint = input->pending_hint;
  input->purpose = input->pending_purpose;
  input->commits++;
}

static const struct zwp_text_input_v3_interface text_input_impl =
  {
    .destroy = destroy_resource,
    .enable = text_input_enable,
    .disable = text_input_disable,
    .set_surrounding_text = text_input_set_surrounding,
    .set_text_change_cause = text_input_set_change_cause,
    .set_content_type = text_input_set_content_type,
    .set_cursor_rectangle = text_input_set_cursor_rectangle,
    .commit = text_input_commit,
  };

static void
text_input_focus (struct text_input *input, struct surface *surface)
{
  if (input->entered && input->entered != surface)
    {
      zwp_text_input_v3_send_leave (input->resource,
				    input->entered->resource);
      input->entered = NULL;
    }
  if (surface && !input->entered
      && wl_resource_get_client (surface->resource)
	 == wl_resource_get_client (input->resource))
    {
      zwp_text_input_v3_send_enter (input->resource, surface->resource);
      input->entered = surface;
      input->first_rect_seen = false;
    }
}

static struct surface *keyboard_focus_surface (void);

static void
text_input_manager_get (struct wl_client *client,
			struct wl_resource *resource, uint32_t id,
			struct wl_resource *seat)
{
  struct text_input *input = calloc (1, sizeof *input);
  if (!input)
    die ("out of memory");
  input->resource
    = wl_resource_create (client, &zwp_text_input_v3_interface,
			  wl_resource_get_version (resource), id);
  if (!input->resource)
    {
      free (input);
      wl_client_post_no_memory (client);
      return;
    }
  wl_resource_set_implementation (input->resource, &text_input_impl, input,
				  text_input_destroyed);
  wl_list_insert (&mc.text_inputs, &input->link);
  text_input_focus (input, keyboard_focus_surface ());
}

static const struct zwp_text_input_manager_v3_interface
  text_input_manager_impl =
  {
    .destroy = destroy_resource,
    .get_text_input = text_input_manager_get,
  };

static void
bind_text_input_manager (struct wl_client *client, void *data,
			 uint32_t version, uint32_t id)
{
  struct wl_resource *resource
    = wl_resource_create (client, &zwp_text_input_manager_v3_interface,
			  version, id);
  if (resource)
    wl_resource_set_implementation (resource, &text_input_manager_impl,
				    NULL, NULL);
  else
    wl_client_post_no_memory (client);
}

/* The keyboard.  Keys go to the focused surface's client; mockcomp
   updates its xkb state from each key and sends modifiers when they
   change, after the key, as compositors do.  */

static struct surface *find_surface (uint32_t);

static struct wl_client *
surface_client (struct surface *surface)
{
  return surface ? wl_resource_get_client (surface->resource) : NULL;
}

static void
send_modifiers (struct wl_resource *keyboard)
{
  wl_keyboard_send_modifiers (keyboard, wl_display_next_serial (mc.display),
			      mc.mods_depressed, mc.mods_latched,
			      mc.mods_locked, mc.group);
}

/* Recompute the serialized modifiers.  Return true if they changed.  */
static bool
update_modifiers (void)
{
  uint32_t depressed
    = xkb_state_serialize_mods (mc.xkb_state, XKB_STATE_MODS_DEPRESSED);
  uint32_t latched
    = xkb_state_serialize_mods (mc.xkb_state, XKB_STATE_MODS_LATCHED);
  uint32_t locked
    = xkb_state_serialize_mods (mc.xkb_state, XKB_STATE_MODS_LOCKED);
  uint32_t group
    = xkb_state_serialize_layout (mc.xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);
  bool changed = (depressed != mc.mods_depressed || latched != mc.mods_latched
		  || locked != mc.mods_locked || group != mc.group);
  mc.mods_depressed = depressed;
  mc.mods_latched = latched;
  mc.mods_locked = locked;
  mc.group = group;
  return changed;
}

/* Move keyboard focus to SURFACE, or nowhere if it is null.  */
static void
set_keyboard_focus (struct surface *surface)
{
  struct wl_resource *keyboard;
  struct wl_client *old = surface_client (mc.keyboard_focus);
  struct wl_client *new = surface_client (surface);

  if (surface == mc.keyboard_focus)
    return;
  wl_resource_for_each (keyboard, &mc.keyboards)
    if (old && wl_resource_get_client (keyboard) == old)
      wl_keyboard_send_leave (keyboard, wl_display_next_serial (mc.display),
			      mc.keyboard_focus->resource);
  mc.keyboard_focus = surface;
  struct text_input *input;
  wl_list_for_each (input, &mc.text_inputs, link)
    text_input_focus (input, surface);
  wl_resource_for_each (keyboard, &mc.keyboards)
    if (new && wl_resource_get_client (keyboard) == new)
      {
	struct wl_array keys;
	wl_array_init (&keys);
	wl_keyboard_send_enter (keyboard, wl_display_next_serial (mc.display),
				surface->resource, &keys);
	wl_array_release (&keys);
	send_modifiers (keyboard);
      }
}

static struct surface *
first_mapped_surface (void)
{
  struct surface *surface;
  wl_list_for_each (surface, &mc.surfaces, link)
    if (surface->xdg_toplevel && surface->mapped)
      return surface;
  return NULL;
}

/* Send key KEYCODE (an evdev code) to the focused surface, focusing the
   first mapped surface first if there is no focus.  */
static void
send_key (unsigned keycode, bool pressed)
{
  struct wl_resource *keyboard;
  struct wl_client *client;
  bool changed;

  if (!mc.keyboard_focus)
    set_keyboard_focus (first_mapped_surface ());
  client = surface_client (mc.keyboard_focus);
  xkb_state_update_key (mc.xkb_state, keycode + 8,
			pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
  changed = update_modifiers ();
  wl_resource_for_each (keyboard, &mc.keyboards)
    if (client && wl_resource_get_client (keyboard) == client)
      {
	wl_keyboard_send_key (keyboard, wl_display_next_serial (mc.display),
			      event_time (), keycode,
			      (pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
			       : WL_KEYBOARD_KEY_STATE_RELEASED));
	if (changed)
	  send_modifiers (keyboard);
	mc.keys++;
      }
}

static struct surface *
keyboard_focus_surface (void)
{
  return mc.keyboard_focus;
}

/* im preedit BEGIN END TEXT | commit TEXT | delete BEFORE AFTER
   | done [SERIAL]: events for the text inputs of the focused client.
   Done's serial defaults to each object's commit count.  */
static void
cmd_im (const char *arg)
{
  char verb[16];
  int n = 0, begin, end, used = 0;
  unsigned before, after, serial;
  struct text_input *input;
  bool sent = false;

  if (!arg || sscanf (arg, "%15s%n", verb, &n) != 1)
    goto usage;
  arg += n;
  while (*arg == ' ')
    arg++;
  if (!strcmp (verb, "preedit")
      && sscanf (arg, "%d %d %n", &begin, &end, &used) == 2)
    arg += used;
  else if (!strcmp (verb, "delete")
	   && sscanf (arg, "%u %u", &before, &after) != 2)
    goto usage;
  else if (strcmp (verb, "commit") && strcmp (verb, "done")
	   && strcmp (verb, "delete"))
    goto usage;

  wl_list_for_each (input, &mc.text_inputs, link)
    {
      if (!input->entered)
	continue;
      if (!strcmp (verb, "preedit"))
	zwp_text_input_v3_send_preedit_string (input->resource, arg, begin,
					       end);
      else if (!strcmp (verb, "commit"))
	zwp_text_input_v3_send_commit_string (input->resource, arg);
      else if (!strcmp (verb, "delete"))
	zwp_text_input_v3_send_delete_surrounding_text (input->resource,
							before, after);
      else
	zwp_text_input_v3_send_done (input->resource,
				     sscanf (arg, "%u", &serial) == 1
				     ? serial : input->commits);
      sent = true;
    }
  if (sent)
    control_reply ("ok");
  else
    control_reply ("error no text input has focus");
  return;

 usage:
  control_reply ("error usage: im preedit BEGIN END TEXT | commit TEXT"
		 " | delete BEFORE AFTER | done [SERIAL]");
}

static void
cmd_focus (const char *arg)
{
  uint32_t id;
  struct surface *surface = NULL;

  if (!arg || (strcmp (arg, "none")
	       && (sscanf (arg, "%u", &id) != 1
		   || !(surface = find_surface (id)) || !surface->xdg_toplevel)))
    {
      control_reply ("error usage: focus SURFACE|none");
      return;
    }
  set_keyboard_focus (surface);
  control_reply ("ok");
}

static void
cmd_repeat_info (const char *arg)
{
  struct wl_resource *keyboard;
  int rate, delay;

  if (!arg || sscanf (arg, "%d %d", &rate, &delay) != 2 || rate < 0
      || delay < 0)
    {
      control_reply ("error usage: repeat-info RATE DELAY");
      return;
    }
  mc.repeat_rate = rate;
  mc.repeat_delay = delay;
  wl_resource_for_each (keyboard, &mc.keyboards)
    if (wl_resource_get_version (keyboard)
	>= WL_KEYBOARD_REPEAT_INFO_SINCE_VERSION)
      wl_keyboard_send_repeat_info (keyboard, rate, delay);
  control_reply ("ok");
}

static void
cmd_capabilities (const char *arg)
{
  struct wl_resource *seat;
  unsigned mask;

  if (!arg || sscanf (arg, "%u", &mask) != 1 || mask > 3)
    {
      control_reply ("error usage: capabilities MASK (pointer 1, keyboard 2)");
      return;
    }
  mc.seat_capabilities = mask;
  wl_resource_for_each (seat, &mc.seats)
    wl_seat_send_capabilities (seat, mask);
  control_reply ("ok");
}

/* Compile the keymap for LAYOUT (an XKB layout name, with an optional
   variant after a colon) and make it current.  */
static bool
set_keymap (const char *layout)
{
  char name[64], *variant;
  struct xkb_rule_names names = {0};
  struct xkb_keymap *keymap;
  char *string;

  snprintf (name, sizeof name, "%s", layout);
  variant = strchr (name, ':');
  if (variant)
    *variant++ = '\0';
  names.layout = name;
  names.variant = variant;
  keymap = xkb_keymap_new_from_names (mc.xkb_context, &names,
				      XKB_KEYMAP_COMPILE_NO_FLAGS);
  if (!keymap)
    return false;
  string = xkb_keymap_get_as_string (keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
  if (!string)
    {
      xkb_keymap_unref (keymap);
      return false;
    }
  free (mc.keymap);
  mc.keymap = string;
  mc.keymap_size = strlen (string) + 1;
  if (mc.xkb_state)
    xkb_state_unref (mc.xkb_state);
  mc.xkb_state = xkb_state_new (keymap);
  xkb_keymap_unref (keymap);
  if (!mc.xkb_state)
    die ("xkb_state_new failed");
  update_modifiers ();
  return true;
}

static void
cmd_layout (const char *arg)
{
  struct wl_resource *keyboard;

  if (!arg || !set_keymap (arg))
    {
      control_reply ("error usage: layout LAYOUT[:VARIANT]");
      return;
    }
  wl_resource_for_each (keyboard, &mc.keyboards)
    {
      send_keymap (keyboard);
      if (mc.keyboard_focus && wl_resource_get_client (keyboard)
	  == surface_client (mc.keyboard_focus))
	send_modifiers (keyboard);
    }
  control_reply ("ok");
}

/* The pointer.  Each command's events end with wl_pointer.frame.  */

static void
pointer_frame (struct wl_resource *pointer)
{
  if (wl_resource_get_version (pointer) >= WL_POINTER_FRAME_SINCE_VERSION)
    wl_pointer_send_frame (pointer);
}

static enum wl_pointer_axis
parse_axis (const char *name, bool *ok)
{
  *ok = true;
  if (!strcmp (name, "vertical"))
    return WL_POINTER_AXIS_VERTICAL_SCROLL;
  if (!strcmp (name, "horizontal"))
    return WL_POINTER_AXIS_HORIZONTAL_SCROLL;
  *ok = false;
  return 0;
}

/* pointer enter SURFACE X Y | motion X Y | button CODE press|release
   | axis AXIS VALUE wheel|finger|continuous [VALUE120] | stop AXIS
   | leave.  Coordinates are surface-local and logical.  */
static void
cmd_pointer (const char *arg)
{
  char verb[16], name[16], source[16];
  double x, y, value;
  unsigned code, id;
  int value120 = 0, n = 0;
  struct wl_resource *pointer;
  struct wl_client *client;
  enum wl_pointer_axis axis;
  bool ok;

  if (!arg || sscanf (arg, "%15s%n", verb, &n) != 1)
    goto usage;
  arg += n;

  if (!strcmp (verb, "enter"))
    {
      struct surface *surface;
      if (sscanf (arg, "%u %lf %lf", &id, &x, &y) != 3
	  || !(surface = find_surface (id)))
	goto usage;
      if (mc.pointer_focus && mc.pointer_focus != surface)
	wl_resource_for_each (pointer, &mc.pointers)
	  if (wl_resource_get_client (pointer)
	      == surface_client (mc.pointer_focus))
	    {
	      wl_pointer_send_leave (pointer,
				     wl_display_next_serial (mc.display),
				     mc.pointer_focus->resource);
	      pointer_frame (pointer);
	    }
      mc.pointer_focus = surface;
      mc.pointer_x = wl_fixed_from_double (x);
      mc.pointer_y = wl_fixed_from_double (y);
      wl_resource_for_each (pointer, &mc.pointers)
	if (wl_resource_get_client (pointer) == surface_client (surface))
	  {
	    wl_pointer_send_enter (pointer,
				   wl_display_next_serial (mc.display),
				   surface->resource, mc.pointer_x,
				   mc.pointer_y);
	    pointer_frame (pointer);
	  }
      control_reply ("ok");
      return;
    }

  client = surface_client (mc.pointer_focus);
  if (!client)
    {
      control_reply ("error no pointer focus");
      return;
    }
  if (!strcmp (verb, "leave"))
    {
      wl_resource_for_each (pointer, &mc.pointers)
	if (wl_resource_get_client (pointer) == client)
	  {
	    wl_pointer_send_leave (pointer,
				   wl_display_next_serial (mc.display),
				   mc.pointer_focus->resource);
	    pointer_frame (pointer);
	  }
      mc.pointer_focus = NULL;
    }
  else if (!strcmp (verb, "motion"))
    {
      if (sscanf (arg, "%lf %lf", &x, &y) != 2)
	goto usage;
      mc.pointer_x = wl_fixed_from_double (x);
      mc.pointer_y = wl_fixed_from_double (y);
      wl_resource_for_each (pointer, &mc.pointers)
	if (wl_resource_get_client (pointer) == client)
	  {
	    wl_pointer_send_motion (pointer, event_time (), mc.pointer_x,
				    mc.pointer_y);
	    pointer_frame (pointer);
	  }
    }
  else if (!strcmp (verb, "press-at"))
    {
      /* Motion and a press in one flush, so the client dispatches the
	 press before it handles the motion.  */
      if (sscanf (arg, "%lf %lf %u", &x, &y, &code) != 3)
	goto usage;
      mc.pointer_x = wl_fixed_from_double (x);
      mc.pointer_y = wl_fixed_from_double (y);
      wl_resource_for_each (pointer, &mc.pointers)
	if (wl_resource_get_client (pointer) == client)
	  {
	    wl_pointer_send_motion (pointer, event_time (), mc.pointer_x,
				    mc.pointer_y);
	    pointer_frame (pointer);
	    wl_pointer_send_button (pointer,
				    wl_display_next_serial (mc.display),
				    event_time (), code,
				    WL_POINTER_BUTTON_STATE_PRESSED);
	    pointer_frame (pointer);
	  }
      mc.buttons++;
    }
  else if (!strcmp (verb, "button"))
    {
      if (sscanf (arg, "%u %15s", &code, name) != 2
	  || (strcmp (name, "press") && strcmp (name, "release")))
	goto usage;
      wl_resource_for_each (pointer, &mc.pointers)
	if (wl_resource_get_client (pointer) == client)
	  {
	    wl_pointer_send_button (pointer,
				    wl_display_next_serial (mc.display),
				    event_time (), code,
				    (strcmp (name, "press")
				     ? WL_POINTER_BUTTON_STATE_RELEASED
				     : WL_POINTER_BUTTON_STATE_PRESSED));
	    pointer_frame (pointer);
	  }
      mc.buttons++;
    }
  else if (!strcmp (verb, "axis"))
    {
      enum wl_pointer_axis_source src;
      int fields = sscanf (arg, "%15s %lf %15s %d", name, &value, source,
			   &value120);
      axis = parse_axis (name, &ok);
      if (fields < 3 || !ok)
	goto usage;
      if (!strcmp (source, "wheel"))
	src = WL_POINTER_AXIS_SOURCE_WHEEL;
      else if (!strcmp (source, "finger"))
	src = WL_POINTER_AXIS_SOURCE_FINGER;
      else if (!strcmp (source, "continuous"))
	src = WL_POINTER_AXIS_SOURCE_CONTINUOUS;
      else
	goto usage;
      wl_resource_for_each (pointer, &mc.pointers)
	if (wl_resource_get_client (pointer) == client)
	  {
	    int version = wl_resource_get_version (pointer);
	    if (version >= WL_POINTER_AXIS_SOURCE_SINCE_VERSION)
	      wl_pointer_send_axis_source (pointer, src);
	    /* High-resolution steps from version 8, discrete steps
	       before it, for wheels only.  */
	    if (fields == 4
		&& version >= WL_POINTER_AXIS_VALUE120_SINCE_VERSION)
	      wl_pointer_send_axis_value120 (pointer, axis, value120);
	    else if (fields == 4
		     && version >= WL_POINTER_AXIS_DISCRETE_SINCE_VERSION)
	      wl_pointer_send_axis_discrete (pointer, axis, value120 / 120);
	    wl_pointer_send_axis (pointer, event_time (), axis,
				  wl_fixed_from_double (value));
	    pointer_frame (pointer);
	  }
      mc.axis_events++;
    }
  else if (!strcmp (verb, "stop"))
    {
      if (sscanf (arg, "%15s", name) != 1)
	goto usage;
      axis = parse_axis (name, &ok);
      if (!ok)
	goto usage;
      wl_resource_for_each (pointer, &mc.pointers)
	if (wl_resource_get_client (pointer) == client
	    && wl_resource_get_version (pointer)
	       >= WL_POINTER_AXIS_STOP_SINCE_VERSION)
	  {
	    wl_pointer_send_axis_stop (pointer, event_time (), axis);
	    pointer_frame (pointer);
	  }
    }
  else
    goto usage;
  control_reply ("ok");
  return;

 usage:
  control_reply ("error usage: pointer enter SURFACE X Y | motion X Y"
		 " | button CODE press|release | press-at X Y CODE"
		 " | axis AXIS VALUE SOURCE"
		 " [VALUE120] | stop AXIS | leave");
}

/* clock real | clock MS: event timestamps from the real clock, or
   from a manual one set to MS.  */
static void
cmd_clock (const char *arg)
{
  unsigned long ms;

  if (arg && !strcmp (arg, "real"))
    mc.manual_clock = false;
  else if (arg && sscanf (arg, "%lu", &ms) == 1)
    {
      mc.manual_clock = true;
      mc.clock_ms = ms;
    }
  else
    {
      control_reply ("error usage: clock real|MS");
      return;
    }
  control_reply ("ok");
}

static void
offer_destroyed (struct wl_resource *offer)
{
  free (wl_resource_get_user_data (offer));
}

static void
cmd_offer_selection (const char *text)
{
  struct wl_resource *device;

  if (mc.selection_source)
    {
      wl_data_source_send_cancelled (mc.selection_source);
      mc.selection_source = NULL;
    }

  wl_resource_for_each (device, &mc.data_devices)
    {
      struct wl_resource *offer
	= wl_resource_create (wl_resource_get_client (device),
			      &wl_data_offer_interface,
			      wl_resource_get_version (device), 0);
      if (!offer)
	continue;
      char *payload = strdup (text);
      if (!payload)
	die ("out of memory");
      wl_resource_set_implementation (offer, &offer_impl, payload,
				      offer_destroyed);
      wl_data_device_send_data_offer (device, offer);
      wl_data_offer_send_offer (offer, "text/plain;charset=utf-8");
      wl_data_device_send_selection (device, offer);
    }
  control_reply ("ok");
}

static void
cmd_request_selection (const char *mime)
{
  int pipefd[2];
  if (!mc.selection_source || mc.selection_pipe >= 0)
    {
      control_reply ("error no source or request already pending");
      return;
    }
  if (pipe2 (pipefd, O_NONBLOCK | O_CLOEXEC) < 0)
    {
      control_reply ("error pipe2: %s", strerror (errno));
      return;
    }
  mc.selection_pipe = pipefd[0];
  wl_data_source_send_send (mc.selection_source,
			    mime ? mime : "application/x-emacs-wayland-test",
			    pipefd[1]);
  close (pipefd[1]);
  control_reply ("ok");
}

static void
cmd_selection_result (const char *expected)
{
  char buf[64];
  ssize_t len;
  struct pollfd pfd = { .fd = mc.selection_pipe, .events = POLLIN };
  if (mc.selection_pipe < 0)
    {
      control_reply ("error no selection request");
      return;
    }
  /* Require EOF as well as the bytes so this catches a leaked write fd.  */
  if (poll (&pfd, 1, 0) <= 0 || !(pfd.revents & POLLHUP))
    {
      control_reply ("pending");
      return;
    }
  len = read (mc.selection_pipe, buf, sizeof buf);
  if (len < 0 && (errno == EAGAIN || errno == EINTR))
    {
      control_reply ("pending");
      return;
    }
  close (mc.selection_pipe);
  mc.selection_pipe = -1;
  if (!expected)
    expected = "emacs-wayland-m0";
  if (len >= 0 && (size_t) len == strlen (expected) && !memcmp (buf, expected, len))
    control_reply ("ok");
  else
    control_reply ("error unexpected selection payload (%zd bytes)", len);
}

static void
cmd_selection_chunk (const char *expected)
{
  char buf[16384];
  if (mc.selection_pipe < 0 || !expected || strlen (expected) != 1)
    {
      control_reply ("error incomplete selection chunk request");
      return;
    }
  ssize_t n = read (mc.selection_pipe, buf, sizeof buf);
  if (n < 0)
    {
      if (errno == EAGAIN || errno == EINTR)
	control_reply ("pending");
      else
	control_reply ("error reading selection: %s", strerror (errno));
      return;
    }
  if (n == 0)
    {
      close (mc.selection_pipe);
      mc.selection_pipe = -1;
      control_reply ("eof");
      return;
    }
  for (ssize_t i = 0; i < n; i++)
    if (buf[i] != *expected)
      {
	control_reply ("error unexpected selection data");
	return;
      }
  control_reply ("%zd", n);
}

static void
cmd_protocol_error (bool epipe)
{
  struct wl_resource *wm_base;

  if (wl_list_empty (&mc.wm_bases))
    {
      control_reply ("error no xdg_wm_base bound");
      return;
    }
  wm_base = wl_resource_from_link (mc.wm_bases.next);
  wl_resource_post_error (wm_base, XDG_WM_BASE_ERROR_ROLE,
			  "mockcomp: injected protocol error");
  if (epipe)
    {
      struct wl_client *client = wl_resource_get_client (wm_base);
      int fd = wl_client_get_fd (client);
      wl_client_flush (client);
      /* Keep the error available for reading, but reject the next
	 client write.  */
      shutdown (fd, SHUT_RD);
    }
  control_reply ("ok");
}

static void
cmd_disconnect (void)
{
  struct wl_client *client;

  while ((client = first_client ()))
    wl_client_destroy (client);
  control_reply ("ok");
}

/* Send each WIDTH HEIGHT pair in ARG as a configure to every
   configured toplevel, back to back, as a burst.  */

static void
cmd_configure (const char *arg, bool targeted)
{
  int32_t sizes[64][2];
  int n = 0, used;
  struct surface *surface;
  uint32_t id = 0;

  if (targeted)
    {
      if (!arg || sscanf (arg, "%" SCNu32 "%n", &id, &used) != 1 || !id)
        {
          control_reply ("error surface ID required");
          return;
        }
      arg += used;
    }

  while (arg && n < 64
	 && sscanf (arg, "%d %d%n", &sizes[n][0], &sizes[n][1], &used) == 2)
    {
      if (sizes[n][0] < 0 || sizes[n][1] < 0)
	{
	  control_reply ("error sizes must be nonnegative");
	  return;
	}
      n++;
      arg += used;
    }
  if (!n)
    {
      control_reply ("error usage: configure WIDTH HEIGHT [WIDTH HEIGHT]...");
      return;
    }
  wl_list_for_each (surface, &mc.surfaces, link)
    if (surface->xdg_toplevel && surface->initial_commit_done
	&& (!id || wl_resource_get_id (surface->resource) == id))
      for (int i = 0; i < n; i++)
	send_sized_configure (surface, sizes[i][0], sizes[i][1]);
  control_reply ("ok");
}

static void
cmd_toplevel_state (const char *arg)
{
  char operation[16];
  uint32_t id, states = 0;
  int32_t width = 0, height = 0;
  int fields;
  struct surface *surface;

  if (!arg
      || (fields = sscanf (arg, "%" SCNu32 " %15s %" SCNd32 " %" SCNd32,
			   &id, operation, &width, &height)) < 2
      || fields == 3 || width < 0 || height < 0
      || !(surface = find_surface (id)) || !surface->xdg_toplevel
      || !surface->initial_commit_done)
    goto usage;
  if (!strcmp (operation, "suspended"))
    {
      if (wl_resource_get_version (surface->xdg_toplevel) < 6)
	goto usage;
      states = UINT32_C (1) << XDG_TOPLEVEL_STATE_SUSPENDED;
    }
  else if (!strcmp (operation, "fullscreen"))
    states = UINT32_C (1) << XDG_TOPLEVEL_STATE_FULLSCREEN;
  else if (!strcmp (operation, "maximized"))
    states = UINT32_C (1) << XDG_TOPLEVEL_STATE_MAXIMIZED;
  else if (!strcmp (operation, "tiled"))
    {
      if (wl_resource_get_version (surface->xdg_toplevel) < 2)
	goto usage;
      states = ((UINT32_C (1) << XDG_TOPLEVEL_STATE_TILED_LEFT)
		| (UINT32_C (1) << XDG_TOPLEVEL_STATE_TILED_RIGHT)
		| (UINT32_C (1) << XDG_TOPLEVEL_STATE_TILED_TOP)
		| (UINT32_C (1) << XDG_TOPLEVEL_STATE_TILED_BOTTOM));
    }
  else if (strcmp (operation, "normal"))
    goto usage;
  send_state_configure (surface, width, height, states);
  control_reply ("ok");
  return;

 usage:
  control_reply ("error usage: toplevel-state SURFACE"
		 " suspended|fullscreen|maximized|tiled|normal [WIDTH HEIGHT]");
}

static void
cmd_release (const char *arg)
{
  uint32_t id;
  unsigned long commit;
  if (arg && sscanf (arg, "%" SCNu32 " %lu", &id, &commit) == 2)
    {
      struct held_buffer *held;
      wl_list_for_each (held, &mc.held, link)
        if (held->ref.buffer && held->commit == commit
            && wl_resource_get_id (held->surface->resource) == id)
          {
            release_held (held);
            control_reply ("ok");
            return;
          }
    }
  control_reply ("error no held buffer for surface and commit");
}

static struct surface *
find_surface (uint32_t id)
{
  struct surface *surface;
  wl_list_for_each (surface, &mc.surfaces, link)
    if (wl_resource_get_id (surface->resource) == id)
      return surface;
  return NULL;
}

static void
cmd_preferred_scale (const char *arg)
{
  uint32_t id, fractional;
  int integer;
  if (arg && sscanf (arg, "%" SCNu32 " %" SCNu32 " %d",
                    &id, &fractional, &integer) == 3
      && fractional > 0 && integer > 0)
    {
      struct surface *surface = find_surface (id);
      if (surface)
        {
          if (surface->fractional)
            wp_fractional_scale_v1_send_preferred_scale (surface->fractional,
                                                        fractional);
          if (wl_resource_get_version (surface->resource)
              >= WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION)
            wl_surface_send_preferred_buffer_scale (surface->resource,
                                                    integer);
          control_reply ("ok");
          return;
        }
    }
  control_reply ("error usage: preferred-scale SURFACE FRACTIONAL INTEGER");
}

static void
cmd_move_output (const char *arg)
{
  uint32_t id;
  int index;
  if (arg && sscanf (arg, "%" SCNu32 " %d", &id, &index) == 2
      && index >= 1 && index <= 2 && mc.outputs[index - 1].global)
    {
      struct surface *surface = find_surface (id);
      if (surface)
        {
          struct wl_client *client
            = wl_resource_get_client (surface->resource);
          struct mock_output *output = &mc.outputs[index - 1];
          struct wl_resource *enter = client_output (output, client);
          if (enter && surface->output != output)
            {
              struct wl_resource *leave = surface->output
                ? client_output (surface->output, client) : NULL;
              wl_surface_send_enter (surface->resource, enter);
              if (leave)
                wl_surface_send_leave (surface->resource, leave);
              surface->output = output;
              control_reply ("ok");
              return;
            }
        }
    }
  control_reply ("error usage: move-output SURFACE OUTPUT");
}

static int
destroy_removed_output (void *data)
{
  struct mock_output *output = data;

  if (output->removed)
    wl_global_destroy (output->removed);
  output->removed = NULL;
  return 0;
}

static void
cmd_output (const char *arg)
{
  struct mock_output *output = &mc.outputs[1];
  if (arg && !strcmp (arg, "add") && !output->global)
    {
      output->global = wl_global_create (mc.display, &wl_output_interface,
                                         OUTPUT_VERSION, output, bind_output);
      if (!output->global)
        die ("could not add output");
    }
  else if (arg && !strcmp (arg, "remove") && output->global)
    {
      struct surface *surface;
      wl_list_for_each (surface, &mc.surfaces, link)
        if (surface->output == output)
          surface->output = NULL;
      destroy_removed_output (output);
      wl_global_remove (output->global);
      output->removed = output->global;
      output->global = NULL;
      if (!output->removal_timer)
        output->removal_timer
          = wl_event_loop_add_timer (mc.loop, destroy_removed_output, output);
      wl_event_source_timer_update (output->removal_timer, 5000);
    }
  else
    {
      control_reply ("error usage: output add|remove");
      return;
    }
  control_reply ("ok");
}

static bool
parse_switch (const char *arg, bool *value)
{
  if (!strcmp (arg, "on"))
    *value = true;
  else if (!strcmp (arg, "off"))
    *value = false;
  else
    return false;
  return true;
}

static void
run_command (char *line)
{
  char *cmd = strtok (line, " \t");
  char *arg = strtok (NULL, "");

  if (!cmd)
    return;
  if (mc.trace)
    fprintf (mc.trace, "## control: %s%s%s\n", cmd, arg ? " " : "",
	     arg ? arg : "");

  if (!strcmp (cmd, "status"))
    cmd_status ();
  else if (!strcmp (cmd, "ping"))
    cmd_ping ();
  else if (!strcmp (cmd, "key"))
    {
      unsigned keycode;
      char state[16];

      if (!arg || sscanf (arg, "%u %15s", &keycode, state) != 2
	  || (strcmp (state, "press") && strcmp (state, "release")))
	control_reply ("error usage: key KEYCODE press|release");
      else
	{
	  send_key (keycode, !strcmp (state, "press"));
	  control_reply ("ok");
	}
    }
  else if (!strcmp (cmd, "key-burst"))
    {
      unsigned keycode, count;
      if (!arg || sscanf (arg, "%u %u", &keycode, &count) != 2 || count > 1024)
        control_reply ("error usage: key-burst KEYCODE COUNT (up to 1024)");
      else
        {
          for (unsigned i = 0; i < count; i++)
            {
              send_key (keycode, true);
              send_key (keycode, false);
            }
          control_reply ("ok");
        }
    }
  else if (!strcmp (cmd, "focus"))
    cmd_focus (arg);
  else if (!strcmp (cmd, "im"))
    cmd_im (arg);
  else if (!strcmp (cmd, "repeat-info"))
    cmd_repeat_info (arg);
  else if (!strcmp (cmd, "capabilities"))
    cmd_capabilities (arg);
  else if (!strcmp (cmd, "layout"))
    cmd_layout (arg);
  else if (!strcmp (cmd, "pointer"))
    cmd_pointer (arg);
  else if (!strcmp (cmd, "clock"))
    cmd_clock (arg);
  else if (!strcmp (cmd, "offer-selection"))
    cmd_offer_selection (arg ? arg : "");
  else if (!strcmp (cmd, "request-selection"))
    cmd_request_selection (arg);
  else if (!strcmp (cmd, "selection-result"))
    cmd_selection_result (arg);
  else if (!strcmp (cmd, "selection-chunk"))
    cmd_selection_chunk (arg);
  else if (!strcmp (cmd, "hold-offer-pipe"))
    {
      mc.hold_offer_pipe = true;
      control_reply ("ok");
    }
  else if (!strcmp (cmd, "offer-pipe-result"))
    {
      if (mc.held_offer_pipe < 0)
	control_reply ("error no held offer pipe");
      else
	{
	  struct pollfd fd = {.fd = mc.held_offer_pipe, .events = POLLOUT};
	  if (poll (&fd, 1, 0) > 0 && (fd.revents & POLLERR))
	    {
	      close (mc.held_offer_pipe);
	      mc.held_offer_pipe = -1;
	      control_reply ("ok");
	    }
	  else
	    control_reply ("pending");
	}
    }
  else if (!strcmp (cmd, "stall")
	   || !strcmp (cmd, "hold-buffers")
	   || !strcmp (cmd, "frame-callbacks"))
    {
      bool value;

      if (!arg || !parse_switch (arg, &value))
	control_reply ("error usage: %s on|off", cmd);
      else
	{
	  if (!strcmp (cmd, "stall"))
	    mc.stalled = value;
	  else if (!strcmp (cmd, "frame-callbacks"))
	    mc.frame_callbacks = value;
	  else
	    {
	      mc.hold_buffers = value;
	      if (!value)
		{
		  struct surface *surface;
		  wl_list_for_each (surface, &mc.surfaces, link)
		    release_superseded (surface);
		}
	    }
	  control_reply ("ok");
	}
    }
  else if (!strcmp (cmd, "configure"))
    cmd_configure (arg, false);
  else if (!strcmp (cmd, "configure-surface"))
    cmd_configure (arg, true);
  else if (!strcmp (cmd, "toplevel-state"))
    cmd_toplevel_state (arg);
  else if (!strcmp (cmd, "release"))
    cmd_release (arg);
  else if (!strcmp (cmd, "preferred-scale"))
    cmd_preferred_scale (arg);
  else if (!strcmp (cmd, "move-output"))
    cmd_move_output (arg);
  else if (!strcmp (cmd, "output"))
    cmd_output (arg);
  else if (!strcmp (cmd, "protocol-error")
	   || !strcmp (cmd, "protocol-error-epipe"))
    cmd_protocol_error (!strcmp (cmd, "protocol-error-epipe"));
  else if (!strcmp (cmd, "disconnect-on-commit"))
    {
      mc.disconnect_on_commit = true;
      control_reply ("ok");
    }
  else if (!strcmp (cmd, "disconnect"))
    cmd_disconnect ();
  else if (!strcmp (cmd, "quit"))
    {
      mc.quit = true;
      control_reply ("ok");
    }
  else
    control_reply ("error unknown command %s", cmd);

  /* Events a command sends go out at once, even while stalled:
     stalling stops reading requests, not sending events.  */
  wl_display_flush_clients (mc.display);
}

static void
read_control (void)
{
  ssize_t n = read (mc.control_fd, mc.control_buf + mc.control_len,
		    sizeof mc.control_buf - 1 - mc.control_len);
  char *start, *newline;

  if (n <= 0)
    {
      close (mc.control_fd);
      mc.control_fd = -1;
      mc.control_len = 0;
      return;
    }
  mc.control_len += n;
  mc.control_buf[mc.control_len] = '\0';

  start = mc.control_buf;
  while ((newline = strchr (start, '\n')))
    {
      *newline = '\0';
      run_command (start);
      start = newline + 1;
      if (mc.control_fd < 0)
	return;
    }
  mc.control_len -= start - mc.control_buf;
  memmove (mc.control_buf, start, mc.control_len);
  if (mc.control_len == sizeof mc.control_buf - 1)
    mc.control_len = 0;		/* Overlong line; drop it.  */
}

static int
open_control_socket (const char *path)
{
  struct sockaddr_un addr = { .sun_family = AF_UNIX };
  int fd = socket (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

  if (fd < 0)
    die ("socket: %s", strerror (errno));
  if (strlen (path) >= sizeof addr.sun_path)
    die ("control socket path too long: %s", path);
  strcpy (addr.sun_path, path);
  unlink (path);
  if (bind (fd, (struct sockaddr *) &addr, sizeof addr) < 0
      || listen (fd, 1) < 0)
    die ("control socket %s: %s", path, strerror (errno));
  return fd;
}

static void
make_keymap (void)
{
  mc.xkb_context = xkb_context_new (XKB_CONTEXT_NO_FLAGS);
  if (!mc.xkb_context)
    die ("xkb_context_new failed");
  if (!set_keymap ("us"))
    die ("could not compile the us keymap");
  mc.repeat_rate = 25;
  mc.repeat_delay = 600;
}

static void
usage (void)
{
  fputs ("Usage: mockcomp --socket NAME --control PATH [--trace FILE]\n",
	 stderr);
  fputs ("                [--snapshots DIRECTORY]\n", stderr);
  exit (EXIT_FAILURE);
}

int
main (int argc, char **argv)
{
  static const struct option options[] =
    {
      { "socket", required_argument, NULL, 's' },
      { "control", required_argument, NULL, 'c' },
      { "trace", required_argument, NULL, 't' },
      { "snapshots", required_argument, NULL, 'p' },
      { "scale-fixture", required_argument, NULL, 'f' },
      { "no-cursor-shape", no_argument, NULL, 'n' },
      { "no-subcompositor", no_argument, NULL, 'u' },
      { "activation-fixture", no_argument, NULL, 'a' },
      { "seat-version", required_argument, NULL, 'v' },
      { "minimal-protocols", no_argument, NULL, 'm' },
      { NULL, 0, NULL, 0 },
    };
  const char *socket_name = NULL, *control_path = NULL, *trace_path = NULL;
  int opt;
  mc.compositor_version = COMPOSITOR_VERSION;
  mc.have_cursor_shape = true;
  mc.have_subcompositor = true;
  mc.seat_version = SEAT_VERSION;

  while ((opt = getopt_long (argc, argv, "", options, NULL)) != -1)
    switch (opt)
      {
      case 's': socket_name = optarg; break;
      case 'c': control_path = optarg; break;
      case 't': trace_path = optarg; break;
      case 'p': mc.snapshots = optarg; break;
      case 'n': mc.have_cursor_shape = false; break;
      case 'u': mc.have_subcompositor = false; break;
      case 'a': mc.have_activation = true; break;
      case 'm': mc.minimal_protocols = true; break;
      case 'v':
	mc.seat_version = atoi (optarg);
	if (mc.seat_version < 1 || mc.seat_version > SEAT_VERSION)
	  usage ();
	break;
      case 'f':
        mc.have_fractional = !strcmp (optarg, "fractional")
          || !strcmp (optarg, "no-viewporter");
        mc.have_viewporter = !strcmp (optarg, "fractional")
          || !strcmp (optarg, "no-fractional");
        if (!strcmp (optarg, "outputs"))
          mc.compositor_version = 4;
        else if (!mc.have_fractional && !mc.have_viewporter)
          usage ();
        break;
      default: usage ();
      }
  if (!socket_name || !control_path)
    usage ();

  signal (SIGPIPE, SIG_IGN);
  mc.seat_capabilities
    = WL_SEAT_CAPABILITY_KEYBOARD | WL_SEAT_CAPABILITY_POINTER;
  if (mc.minimal_protocols)
    {
      mc.compositor_version = 4;
      mc.seat_version = 4;
      mc.have_activation = mc.have_cursor_shape = mc.have_subcompositor = false;
      mc.have_fractional = mc.have_viewporter = false;
    }
  wl_list_init (&mc.seats);
  wl_list_init (&mc.surfaces);
  wl_list_init (&mc.held);
  wl_list_init (&mc.wm_bases);
  wl_list_init (&mc.keyboards);
  wl_list_init (&mc.pointers);
  wl_list_init (&mc.text_inputs);
  wl_list_init (&mc.data_devices);
  mc.frame_callbacks = true;
  mc.selection_pipe = -1;
  mc.held_offer_pipe = -1;
  mc.control_fd = -1;

  if (trace_path)
    {
      mc.trace = fopen (trace_path, "w");
      if (!mc.trace)
	die ("opening %s: %s", trace_path, strerror (errno));
      setvbuf (mc.trace, NULL, _IOLBF, 0);
    }

  make_keymap ();

  mc.display = wl_display_create ();
  if (!mc.display)
    die ("wl_display_create failed");
  if (wl_display_add_socket (mc.display, socket_name) < 0)
    die ("could not add socket %s: %s", socket_name, strerror (errno));
  if (wl_display_init_shm (mc.display) < 0)
    die ("wl_display_init_shm failed");
  mc.outputs[0] = (struct mock_output)
    {.name = "MOCK-1", .scale = 1, .logical_scale = 120};
  /* 150%, which wl_output.scale can only report as 2.  */
  mc.outputs[1] = (struct mock_output)
    {.name = "MOCK-2", .scale = 2, .logical_scale = 180, .x = 1920};
  wl_list_init (&mc.outputs[0].resources);
  wl_list_init (&mc.outputs[1].resources);
  mc.outputs[0].global = wl_global_create (mc.display, &wl_output_interface,
                                          (mc.minimal_protocols ? 1
					   : OUTPUT_VERSION),
					  &mc.outputs[0],
                                          bind_output);
  if (!mc.outputs[0].global
      || !wl_global_create (mc.display, &wl_compositor_interface,
			 mc.compositor_version, NULL, bind_compositor)
      || !wl_global_create (mc.display, &wl_seat_interface, mc.seat_version,
			    NULL, bind_seat)
      || !wl_global_create (mc.display, &wl_data_device_manager_interface,
			    (mc.minimal_protocols ? 1
			     : DATA_DEVICE_MANAGER_VERSION), NULL,
			    bind_data_device_manager)
      || !wl_global_create (mc.display, &xdg_wm_base_interface,
			    (mc.minimal_protocols ? 1 : XDG_WM_BASE_VERSION),
			    NULL,
			    bind_wm_base))
    die ("could not create the globals");
  if (mc.have_subcompositor
      && !wl_global_create (mc.display, &wl_subcompositor_interface, 1,
			    NULL, bind_subcompositor))
    die ("could not create subcompositor");
  if (mc.have_activation
      && !wl_global_create (mc.display, &xdg_activation_v1_interface, 1,
			    NULL, bind_activation))
    die ("could not create activation manager");
  if (mc.have_fractional
      && !wl_global_create (mc.display,
                            &wp_fractional_scale_manager_v1_interface,
                            1, NULL, bind_fractionals))
    die ("could not create fractional scale manager");
  if (mc.have_viewporter
      && !wl_global_create (mc.display, &wp_viewporter_interface, 1, NULL,
                            bind_viewporter))
    die ("could not create viewporter");
  if (!mc.minimal_protocols
      && !wl_global_create (mc.display, &zxdg_output_manager_v1_interface, 3,
			 NULL, bind_output_manager))
    die ("could not create xdg-output manager");
  if (mc.have_cursor_shape
      && !wl_global_create (mc.display, &wp_cursor_shape_manager_v1_interface,
			    1, NULL, bind_shape_manager))
    die ("could not create cursor shape manager");
  if (!mc.minimal_protocols
      && !wl_global_create (mc.display, &zwp_text_input_manager_v3_interface, 1,
			 NULL, bind_text_input_manager))
    die ("could not create text input manager");
  wl_display_add_protocol_logger (mc.display, trace_message, NULL);

  mc.loop = wl_display_get_event_loop (mc.display);
  mc.frame_timer = wl_event_loop_add_timer (mc.loop, frame_tick, NULL);
  wl_event_source_timer_update (mc.frame_timer, FRAME_INTERVAL_MS);
  mc.control_listen_fd = open_control_socket (control_path);

  /* Ready: the runner waits for this line before starting Emacs.  */
  printf ("mockcomp ready socket=%s control=%s\n", socket_name,
	  control_path);
  fflush (stdout);

  while (!mc.quit)
    {
      struct pollfd fds[3] =
	{
	  /* While stalled, client requests stay unread.  */
	  { .fd = mc.stalled ? -1 : wl_event_loop_get_fd (mc.loop),
	    .events = POLLIN },
	  { .fd = mc.control_listen_fd, .events = POLLIN },
	  { .fd = mc.control_fd, .events = POLLIN },
	};

      wl_display_flush_clients (mc.display);
      if (poll (fds, 3, -1) < 0)
	{
	  if (errno == EINTR)
	    continue;
	  die ("poll: %s", strerror (errno));
	}
      if (fds[0].revents)
	wl_event_loop_dispatch (mc.loop, 0);
      if (fds[1].revents & POLLIN)
	{
	  int fd = accept4 (mc.control_listen_fd, NULL, NULL, SOCK_CLOEXEC);
	  if (fd >= 0)
	    {
	      if (mc.control_fd >= 0)
		close (mc.control_fd);
	      mc.control_fd = fd;
	      mc.control_len = 0;
	    }
	}
      if (mc.control_fd >= 0 && fds[2].revents)
	read_control ();
    }

  if (mc.selection_pipe >= 0)
    close (mc.selection_pipe);
  wl_display_destroy_clients (mc.display);
  wl_display_destroy (mc.display);
  unlink (control_path);
  return mc.violation_total ? 2 : 0;
}
