/* observer -- measure input to first captured output commit.

This file is part of GNU Emacs, as modified by the native Wayland
backend fork.  It is free software under
the GNU General Public License, version 3 or later.

For each sample the observer captures a baseline of a region of an
output through wlr-screencopy, arms a capture that completes on the
next damaged output commit, records CLOCK_MONOTONIC, and presses a key
through virtual-keyboard.  The first capture whose pixels differ from
the baseline is the input's effect, and its timestamp ends the sample.
The baseline capture completes on an output commit, so injecting at
once would always land at the same phase of the output's refresh, and
any client fast enough for the next commit would measure the same.
--phase MS waits a random part of MS after the baseline, so that the
samples cover the whole refresh period.

With --watch MS it presses nothing: after printing "ready" it captures
the region on each damaged output commit for MS milliseconds and prints
the timestamp of each capture whose pixels changed, for update cadence.

wlroots stamps each capture with CLOCK_MONOTONIC when the output
commit is sent (types/output/output.c, output_send_commit_event, in
wlroots 0.20.2), so the two clocks need no conversion.  This measures
input to the compositor's output commit, not true presentation.  */

#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <poll.h>
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

#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"

static struct
{
  struct wl_display *display;
  struct wl_shm *shm;
  struct wl_seat *seat;
  struct zwlr_screencopy_manager_v1 *screencopy;
  struct zwp_virtual_keyboard_manager_v1 *keyboards;
  struct zwp_virtual_keyboard_v1 *keyboard;
  struct wl_output *output;
  const char *output_name;
  int region[4];
} ob;

/* One capture: its frame, its shared-memory buffer, and its state.  */

struct capture
{
  struct zwlr_screencopy_frame_v1 *frame;
  struct wl_buffer *buffer;
  void *data;
  size_t size;
  uint32_t format, width, height, stride;
  bool have_shm, buffer_done, ready, failed;
  int64_t when_ns;
};

static void
die (const char *message)
{
  fprintf (stderr, "observer: %s\n", message);
  exit (2);
}

static int64_t
monotonic_ns (void)
{
  struct timespec ts;
  clock_gettime (CLOCK_MONOTONIC, &ts);
  return (int64_t) ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void
output_name (void *data, struct wl_output *output, const char *name)
{
  if (!ob.output && ob.output_name && !strcmp (name, ob.output_name))
    ob.output = output;
}

static void output_geometry (void *d, struct wl_output *o, int32_t x,
			     int32_t y, int32_t pw, int32_t ph, int32_t s,
			     const char *make, const char *model,
			     int32_t t) {}
static void output_mode (void *d, struct wl_output *o, uint32_t flags,
			 int32_t w, int32_t h, int32_t refresh) {}
static void output_done (void *d, struct wl_output *o) {}
static void output_scale (void *d, struct wl_output *o, int32_t f) {}
static void output_description (void *d, struct wl_output *o,
				const char *s) {}

static const struct wl_output_listener output_listener =
  {
    .geometry = output_geometry, .mode = output_mode, .done = output_done,
    .scale = output_scale, .name = output_name,
    .description = output_description,
  };

static void
registry_global (void *data, struct wl_registry *registry, uint32_t name,
		 const char *interface, uint32_t version)
{
  if (!strcmp (interface, wl_shm_interface.name))
    ob.shm = wl_registry_bind (registry, name, &wl_shm_interface, 1);
  else if (!strcmp (interface, wl_seat_interface.name) && !ob.seat)
    ob.seat = wl_registry_bind (registry, name, &wl_seat_interface, 1);
  else if (!strcmp (interface, zwlr_screencopy_manager_v1_interface.name))
    ob.screencopy = wl_registry_bind (registry, name,
				      &zwlr_screencopy_manager_v1_interface,
				      version < 3 ? version : 3);
  else if (!strcmp (interface,
		    zwp_virtual_keyboard_manager_v1_interface.name))
    ob.keyboards
      = wl_registry_bind (registry, name,
			  &zwp_virtual_keyboard_manager_v1_interface, 1);
  else if (!strcmp (interface, wl_output_interface.name) && version >= 4)
    {
      struct wl_output *output
	= wl_registry_bind (registry, name, &wl_output_interface, 4);
      wl_output_add_listener (output, &output_listener, NULL);
    }
}

static void
registry_remove (void *data, struct wl_registry *registry, uint32_t name)
{
}

static const struct wl_registry_listener registry_listener =
  { .global = registry_global, .global_remove = registry_remove };

/* Captures.  */

static void
frame_buffer (void *data, struct zwlr_screencopy_frame_v1 *frame,
	      uint32_t format, uint32_t width, uint32_t height,
	      uint32_t stride)
{
  struct capture *c = data;
  c->have_shm = true;
  c->format = format;
  c->width = width;
  c->height = height;
  c->stride = stride;
}

static void
make_buffer (struct capture *c)
{
  size_t size = (size_t) c->stride * c->height;
  int fd;
  struct wl_shm_pool *pool;

  if (c->buffer && c->size == size)
    return;
  if (c->buffer)
    {
      wl_buffer_destroy (c->buffer);
      munmap (c->data, c->size);
    }
  fd = memfd_create ("observer", MFD_CLOEXEC);
  if (fd < 0 || ftruncate (fd, size) < 0)
    die ("could not allocate a capture buffer");
  c->data = mmap (NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (c->data == MAP_FAILED)
    die ("could not map a capture buffer");
  pool = wl_shm_create_pool (ob.shm, fd, size);
  c->buffer = wl_shm_pool_create_buffer (pool, 0, c->width, c->height,
					 c->stride, c->format);
  wl_shm_pool_destroy (pool);
  close (fd);
  c->size = size;
}

static void
frame_flags (void *data, struct zwlr_screencopy_frame_v1 *frame,
	     uint32_t flags)
{
}

static void
frame_ready (void *data, struct zwlr_screencopy_frame_v1 *frame,
	     uint32_t sec_hi, uint32_t sec_lo, uint32_t nsec)
{
  struct capture *c = data;
  c->ready = true;
  c->when_ns = ((((int64_t) sec_hi << 32) | sec_lo) * 1000000000
		+ nsec);
}

static void
frame_failed (void *data, struct zwlr_screencopy_frame_v1 *frame)
{
  struct capture *c = data;
  c->failed = true;
}

static void
frame_damage (void *data, struct zwlr_screencopy_frame_v1 *frame,
	      uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
}

static void
frame_linux_dmabuf (void *data, struct zwlr_screencopy_frame_v1 *frame,
		    uint32_t format, uint32_t width, uint32_t height)
{
}

static void
frame_buffer_done (void *data, struct zwlr_screencopy_frame_v1 *frame)
{
  struct capture *c = data;
  c->buffer_done = true;
}

static const struct zwlr_screencopy_frame_v1_listener frame_listener =
  {
    .buffer = frame_buffer, .flags = frame_flags, .ready = frame_ready,
    .failed = frame_failed, .damage = frame_damage,
    .linux_dmabuf = frame_linux_dmabuf, .buffer_done = frame_buffer_done,
  };

static void
dispatch_until (bool *flag, int64_t deadline)
{
  while (!*flag)
    {
      struct pollfd pfd = { wl_display_get_fd (ob.display), POLLIN, 0 };
      int64_t left = (deadline - monotonic_ns ()) / 1000000;

      if (left <= 0)
	return;
      while (wl_display_prepare_read (ob.display) != 0)
	wl_display_dispatch_pending (ob.display);
      wl_display_flush (ob.display);
      if (poll (&pfd, 1, left) > 0)
	wl_display_read_events (ob.display);
      else
	wl_display_cancel_read (ob.display);
      wl_display_dispatch_pending (ob.display);
    }
}

/* Request a capture of the region and wait until its buffer is known.
   The copy itself is sent by arm.  */

static void
request (struct capture *c, int64_t deadline)
{
  c->have_shm = c->buffer_done = c->ready = c->failed = false;
  c->frame = zwlr_screencopy_manager_v1_capture_output_region
    (ob.screencopy, 0, ob.output, ob.region[0], ob.region[1],
     ob.region[2], ob.region[3]);
  zwlr_screencopy_frame_v1_add_listener (c->frame, &frame_listener, c);
  dispatch_until (&c->buffer_done, deadline);
  if (!c->buffer_done || !c->have_shm)
    die ("no shared-memory capture buffer");
  make_buffer (c);
}

/* Send the copy: at once, or on the next damaged commit.  A roundtrip
   makes sure the compositor has it before the caller goes on.  */

static void
arm (struct capture *c, bool damage)
{
  if (damage)
    zwlr_screencopy_frame_v1_copy_with_damage (c->frame, c->buffer);
  else
    zwlr_screencopy_frame_v1_copy (c->frame, c->buffer);
  wl_display_roundtrip (ob.display);
}

static bool
finish (struct capture *c, int64_t deadline)
{
  dispatch_until (&c->ready, deadline);
  if (!c->ready && !c->failed)
    dispatch_until (&c->failed, deadline);
  zwlr_screencopy_frame_v1_destroy (c->frame);
  c->frame = NULL;
  return c->ready;
}

/* The virtual keyboard, with a "us" keymap.  */

static void
make_keyboard (void)
{
  struct xkb_context *context = xkb_context_new (XKB_CONTEXT_NO_FLAGS);
  struct xkb_rule_names names = { .layout = "us" };
  struct xkb_keymap *keymap
    = context ? xkb_keymap_new_from_names (context, &names, 0) : NULL;
  char *string = keymap ? xkb_keymap_get_as_string (keymap,
						    XKB_KEYMAP_FORMAT_TEXT_V1)
    : NULL;
  size_t size;
  int fd;

  if (!string)
    die ("could not compile a keymap");
  size = strlen (string) + 1;
  fd = memfd_create ("observer-keymap", MFD_CLOEXEC);
  if (fd < 0 || write (fd, string, size) != (ssize_t) size)
    die ("could not write the keymap");
  ob.keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard
    (ob.keyboards, ob.seat);
  zwp_virtual_keyboard_v1_keymap (ob.keyboard,
				  WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, size);
  close (fd);
  free (string);
  xkb_keymap_unref (keymap);
  xkb_context_unref (context);
  wl_display_roundtrip (ob.display);
}

static void
key (uint32_t code, bool pressed)
{
  zwp_virtual_keyboard_v1_key (ob.keyboard, monotonic_ns () / 1000000, code,
			       (pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
				: WL_KEYBOARD_KEY_STATE_RELEASED));
  wl_display_flush (ob.display);
}

static void
settle (int ms)
{
  bool never = false;
  dispatch_until (&never, monotonic_ns () + (int64_t) ms * 1000000);
}

/* Print the time of each changed capture until END.  */

static void
watch (int64_t end)
{
  struct capture previous = { 0 }, current = { 0 }, swap;
  int captures = 0, changed = 0;

  request (&previous, end);
  arm (&previous, false);
  if (!finish (&previous, end))
    die ("the baseline capture failed");
  puts ("{\"ready\": true}");
  fflush (stdout);
  for (;;)
    {
      /* A request needs only a roundtrip, so allow it past END.  */
      request (&current, end + INT64_C (2000000000));
      arm (&current, true);
      if (!finish (&current, end))
	break;
      captures++;
      if (current.size != previous.size
	  || memcmp (current.data, previous.data, current.size))
	{
	  changed++;
	  printf ("{\"captured_ns\": %" PRId64, current.when_ns);
	  if (current.format == WL_SHM_FORMAT_XRGB8888
	      || current.format == WL_SHM_FORMAT_ARGB8888
	      || current.format == WL_SHM_FORMAT_XBGR8888
	      || current.format == WL_SHM_FORMAT_ABGR8888)
	    {
	      uint32_t rgb;
	      const char *pixels = current.data;
	      size_t offset = (size_t) current.stride * (current.height / 2)
		+ sizeof rgb * (current.width / 2);
	      memcpy (&rgb, pixels + offset, sizeof rgb);
	      if (current.format == WL_SHM_FORMAT_XBGR8888
		  || current.format == WL_SHM_FORMAT_ABGR8888)
		rgb = ((rgb & 0xff) << 16) | (rgb & 0xff00)
		  | ((rgb >> 16) & 0xff);
	      printf (", \"center_rgb\": %" PRIu32, rgb & 0xffffff);
	    }
	  puts ("}");
	  swap = previous, previous = current, current = swap;
	}
    }
  printf ("{\"captures\": %d, \"changed\": %d}\n", captures, changed);
}

int
main (int argc, char **argv)
{
  static const struct option options[] =
    {
      { "output", required_argument, NULL, 'o' },
      { "region", required_argument, NULL, 'r' },
      { "samples", required_argument, NULL, 'n' },
      { "key", required_argument, NULL, 'k' },
      { "settle", required_argument, NULL, 's' },
      { "timeout", required_argument, NULL, 't' },
      { "watch", required_argument, NULL, 'w' },
      { "phase", required_argument, NULL, 'p' },
      { "seed", required_argument, NULL, 'e' },
      { NULL, 0, NULL, 0 },
    };
  int samples = 20, settle_ms = 250, timeout_ms = 2000, watch_ms = 0, opt;
  double phase_ms = 0;
  unsigned seed = 1;
  uint32_t code = 30;		/* KEY_A */
  struct capture baseline = { 0 }, effect = { 0 };

  while ((opt = getopt_long (argc, argv, "", options, NULL)) != -1)
    switch (opt)
      {
      case 'o': ob.output_name = optarg; break;
      case 'r':
	if (sscanf (optarg, "%d,%d,%d,%d", &ob.region[0], &ob.region[1],
		    &ob.region[2], &ob.region[3]) != 4)
	  die ("--region wants X,Y,WIDTH,HEIGHT");
	break;
      case 'n': samples = atoi (optarg); break;
      case 'k': code = strtoul (optarg, NULL, 10); break;
      case 's': settle_ms = atoi (optarg); break;
      case 't': timeout_ms = atoi (optarg); break;
      case 'w': watch_ms = atoi (optarg); break;
      case 'p': phase_ms = atof (optarg); break;
      case 'e': seed = strtoul (optarg, NULL, 10); break;
      default: die ("usage: observer --output NAME --region X,Y,W,H"
		    " [--samples N] [--key CODE] [--settle MS]"
		    " [--timeout MS] [--watch MS] [--phase MS]"
		    " [--seed N]");
      }
  if (!ob.output_name || ob.region[2] <= 0 || ob.region[3] <= 0)
    die ("--output and --region are required");

  ob.display = wl_display_connect (NULL);
  if (!ob.display)
    die ("could not connect");
  wl_registry_add_listener (wl_display_get_registry (ob.display),
			    &registry_listener, NULL);
  wl_display_roundtrip (ob.display);
  wl_display_roundtrip (ob.display);
  if (!ob.shm || !ob.seat || !ob.screencopy || !ob.keyboards || !ob.output)
    die ("the compositor lacks a required global or output");
  if (watch_ms > 0)
    {
      watch (monotonic_ns () + (int64_t) watch_ms * 1000000);
      return 0;
    }
  make_keyboard ();
  settle (settle_ms);

  for (int i = 0; i < samples; i++)
    {
      int64_t deadline = monotonic_ns () + (int64_t) timeout_ms * 1000000;
      int64_t injected, latency = -1;
      int captures = 0;

      request (&baseline, deadline);
      arm (&baseline, false);
      if (!finish (&baseline, deadline))
	die ("the baseline capture failed");

      request (&effect, deadline);
      arm (&effect, true);
      if (phase_ms > 0)
	{
	  int64_t wait = rand_r (&seed) / (RAND_MAX + 1.0) * phase_ms * 1e6;
	  bool never = false;
	  dispatch_until (&never, monotonic_ns () + wait);
	}
      injected = monotonic_ns ();
      key (code, true);

      /* Damage elsewhere in the region can complete a capture first;
	 re-arm until the pixels change.  */
      while (finish (&effect, deadline))
	{
	  captures++;
	  if (effect.size == baseline.size
	      && memcmp (effect.data, baseline.data, effect.size))
	    {
	      latency = effect.when_ns - injected;
	      break;
	    }
	  request (&effect, deadline);
	  arm (&effect, true);
	}
      key (code, false);
      printf ("{\"sample\": %d, \"latency_ns\": %" PRId64
	      ", \"injected_ns\": %" PRId64 ", \"captured_ns\": %" PRId64
	      ", \"captures\": %d}\n",
	      i, latency, injected, latency < 0 ? -1 : effect.when_ns, captures);
      fflush (stdout);
      settle (settle_ms);
    }
  return 0;
}
