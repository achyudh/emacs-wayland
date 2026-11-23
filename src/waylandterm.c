/* Communication with a Wayland compositor for the native backend.

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

/* The Wayland event loop:

   - The input hook never blocks.  Each call dispatches pending events,
     prepares a read, tries one nonblocking flush, reads only if the
     socket is readable (otherwise cancels), dispatches again and
     flushes once more.  Every successful prepare is paired with a read
     or a cancel, and nothing in between can release Emacs's global
     lock.

   - A duplicate of the display socket, registered as a non-keyboard
     read fd, runs the same steps during waits that leave keyboard
     descriptors out (an ordinary `accept-process-output').

   - When the socket is full, output becomes pending: a second
     duplicate carries write interest alone, and its callback resumes
     output.  Any successful flush returns output to idle.

   - Wayland callbacks only record state and queue events.  Losing the
     connection disarms the display at once; deleting the terminal
     waits for a guarded safe point reached through the backend timer
     and `pending_funcalls'.  */

#include <config.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "lisp.h"
#include "blockinput.h"
#include "coding.h"
#include "composite.h"
#include "keyboard.h"
#include "process.h"
#include "termopts.h"
#include "termchar.h"
#include "termhooks.h"
#include "frame.h"
#include "waylandterm.h"

#include <pixman.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>
#include <wayland-cursor.h>

/* Generated headers are not ours to fix; some carry trailing whitespace
   from their protocol descriptions.  */
#if 15 <= __GNUC__
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wtrailing-whitespace"
#endif
#include "xdg-shell-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "presentation-time-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "xdg-activation-v1-client-protocol.h"
#include "cursor-shape-v1-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "primary-selection-unstable-v1-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#if 15 <= __GNUC__
# pragma GCC diagnostic pop
#endif

/* libwayland's outgoing buffer cap.  Overflowing it is a fatal display
   error, handled as connection loss.  */
enum { WAYLAND_MAX_BUFFER_SIZE = 4 * 1024 * 1024 };

/* The evdev keycode range is 0 through 0x2ff.  */
enum { WAYLAND_RAW_KEYCODES = 0x300 };

/* Deadline for the blocking startup roundtrips, in milliseconds.  */
enum { WAYLAND_STARTUP_TIMEOUT_MS = 5000 };

/* Emacs events a burst of Wayland events can queue before the input
   hook runs.  */
enum { WAYLAND_EVENT_QUEUE_SIZE = 256 };

/* Tool bar and tab bar clicks that can wait for the backend timer.  */
enum { WAYLAND_MAX_BAR_CLICKS = 8 };

/* Delay before retrying finalization when its guard fails.  */
enum { WAYLAND_FINALIZE_RETRY_NS = 10 * 1000 * 1000 };

struct wayland_display_info *x_display_list;

static void wayland_present_marked (struct wayland_display_info *);
static void wayland_present_backgrounds (struct wayland_display_info *);
static int wayland_flush_events (struct wayland_display_info *,
				 struct input_event *);
static void wayland_repeat (struct wayland_display_info *);
static void wayland_stop_repeat (struct wayland_display_info *);
static bool wayland_commits_wanted (struct wayland_display_info *);
static void wayland_pointer_work (struct wayland_display_info *);
static void wayland_focus_frame (struct frame *, bool);
static void wayland_activate (struct frame *, const char *);

/* Waits that leave the keyboard out never run the input hook, so
   callbacks on other descriptors move queued events to the keyboard
   buffer themselves, with the quit handling gobble_input gives them:
   a C-g sets `quit-flag'.  */

static void
wayland_flush_with_quit (struct wayland_display_info *dpyinfo)
{
  struct input_event hold_quit;

  EVENT_INIT (hold_quit);
  hold_quit.kind = NO_EVENT;
  wayland_flush_events (dpyinfo, &hold_quit);
  if (hold_quit.kind != NO_EVENT)
    kbd_buffer_store_event (&hold_quit);
}
static void wayland_set_frame_hooks (struct terminal *);
static void wayland_flush_display (struct frame *);
static void wayland_update_pool_budget (struct wayland_display_info *);
struct wayland_monitor;
static void wayland_output_changed (struct wayland_monitor *, bool);

/* Bound globals.  */

struct wayland_globals
{
  struct wl_compositor *compositor;
  struct wl_subcompositor *subcompositor;
  struct wl_shm *shm;
  struct wl_seat *seat;
  struct xdg_wm_base *wm_base;
  struct wl_data_device_manager *data_device_manager;
  struct wp_viewporter *viewporter;
  struct wp_presentation *presentation;
  struct wp_fractional_scale_manager_v1 *fractional_scale_manager;
  struct xdg_activation_v1 *activation;
  struct wp_cursor_shape_manager_v1 *cursor_shape_manager;
  struct zxdg_decoration_manager_v1 *decoration_manager;
  struct zwp_primary_selection_device_manager_v1 *primary_selection_manager;
  struct zwp_text_input_manager_v3 *text_input_manager;
  struct zxdg_output_manager_v1 *output_manager;

  /* The registry name of the bound seat.  */
  uint32_t seat_name;
};

/* For each global, the minimum version we can use and the maximum
   version we support.  REQUIRED globals must provide the minimum.  */

struct global_spec
{
  const struct wl_interface *interface;
  uint32_t min_version, max_version;
  bool required;
  size_t offset;
};

#define GLOBAL(iface, field, min, max, req)			\
  { &iface##_interface, min, max, req,				\
    offsetof (struct wayland_globals, field) }

static const struct global_spec global_specs[] =
  {
    GLOBAL (wl_compositor, compositor, 4, 6, true),
    GLOBAL (wl_subcompositor, subcompositor, 1, 1, false),
    GLOBAL (wl_shm, shm, 1, 1, true),
    GLOBAL (wl_seat, seat, 4, 8, true),
    GLOBAL (xdg_wm_base, wm_base, 1, 6, true),
    GLOBAL (wl_data_device_manager, data_device_manager, 1, 3, true),
    GLOBAL (wp_viewporter, viewporter, 1, 1, false),
    GLOBAL (wp_presentation, presentation, 1, 1, false),
    GLOBAL (wp_fractional_scale_manager_v1, fractional_scale_manager, 1, 1,
	    false),
    GLOBAL (xdg_activation_v1, activation, 1, 1, false),
    GLOBAL (wp_cursor_shape_manager_v1, cursor_shape_manager, 1, 1, false),
    GLOBAL (zxdg_decoration_manager_v1, decoration_manager, 1, 1, false),
    GLOBAL (zwp_primary_selection_device_manager_v1,
	    primary_selection_manager, 1, 1, false),
    GLOBAL (zwp_text_input_manager_v3, text_input_manager, 1, 1, false),
    GLOBAL (zxdg_output_manager_v1, output_manager, 1, 3, false),
  };

#undef GLOBAL

enum { N_GLOBALS = countof (global_specs) };

/* The version each global was bound at, or 0; and the highest version
   seen for a global that was too old to bind.  Kept beside the
   globals so `wayland--metrics' can report them.  */

struct wayland_binding
{
  uint32_t bound[N_GLOBALS];
  uint32_t too_old[N_GLOBALS];
};

/* The backend handles one seat.  */

struct wayland_seat
{
  struct wl_keyboard *keyboard;
  struct xkb_context *xkb_context;
  struct xkb_keymap *keymap;
  struct xkb_state *state;
  uint32_t capabilities;

  /* Dead keys and compose sequences, from the locale's compose table;
     null if it has none.  */
  struct xkb_compose_table *compose_table;
  struct xkb_compose_state *compose_state;

  /* The keymap's indices of the real modifiers Emacs maps.  */
  xkb_mod_index_t mod_shift, mod_ctrl, mod_alt, mod_logo, mod_mod3;

  /* The pointer, the surface it is over and the serials its requests
     need, and how its cursor is drawn: through cursor-shape-v1 if the
     compositor has it, otherwise from the cursor theme.  */
  struct wl_pointer *pointer;
  struct wl_surface *pointer_focus;
  uint32_t pointer_enter_serial, last_button_serial;
  struct wp_cursor_shape_device_v1 *cursor_device;
  struct wl_surface *cursor_surface;
  struct wl_cursor_theme *cursor_theme;
  int cursor_theme_size;

  /* Axis events of the current wl_pointer.frame, index 0 vertical and
     1 horizontal, and what carries over between frames: partial
     high-resolution wheel steps and, while scroll events are
     coalesced, pixels short of a line or column.  */
  struct
  {
    bool any, source_known;
    uint32_t source, time;
    double value[2];
    int32_t value120[2], discrete[2];
    bool has_value120[2], has_discrete[2], stop[2];
  } axis;
  int32_t value120_carry[2];
  /* Continuous scrolling not yet sent, in compositor pixels.  Axis
     values are wl_fixed_t, so their sums stay exact; tenths would not
     (ten values of 0.1 add up to less than 1).  */
  double pixel_carry[2];

  /* Serials for the requests that need one.  */
  uint32_t last_input_serial;
  uint32_t keyboard_enter_serial;
  struct wl_surface *keyboard_focus;

  int32_t repeat_rate, repeat_delay;
  /* The evdev code of the key repeating, and whether one is.  */
  uint32_t repeat_key, repeat_time;
  bool repeating;
  /* Linux evdev codes, before Emacs key translation.  */
  unsigned long raw_keycodes[WAYLAND_RAW_KEYCODES];
};

struct wayland_monitor
{
  struct wayland_monitor *next;
  struct wayland_display_info *dpyinfo;
  struct wl_output *proxy;
  uint32_t name;
  char *label;
  int x, y, width, height, mm_width, mm_height, scale;
  int transform;

  /* Logical geometry from xdg-output, which unlike wl_output.scale is
     exact at fractional scales.  */
  struct zxdg_output_v1 *xdg_output;
  int logical_x, logical_y, logical_width, logical_height;
};

/* A diagnostic timing trace, off unless `wayland--trace' starts it:
   the stages between a key press and its commit, on the observer's
   clock, with the garbage collection count, to explain slow samples.  */

enum wayland_trace_kind
  {
    WAYLAND_TRACE_KEY, WAYLAND_TRACE_DELIVER, WAYLAND_TRACE_BEGIN,
    WAYLAND_TRACE_UP_TO_DATE, WAYLAND_TRACE_DEFER, WAYLAND_TRACE_NO_BUFFER,
    WAYLAND_TRACE_BUFFER, WAYLAND_TRACE_REPAIRED, WAYLAND_TRACE_COMMIT,
    WAYLAND_TRACE_FRAME_DONE, WAYLAND_TRACE_CLEAR,
    WAYLAND_TRACE_RESTORE_START, WAYLAND_TRACE_RESTORE,
  };

static const char *const wayland_trace_names[] =
  { "key", "deliver", "begin", "up-to-date", "defer", "no-buffer", "buffer",
    "repaired", "commit", "frame-done", "clear", "restore-start",
    "restore" };

enum { WAYLAND_TRACE_SIZE = 8192 };

struct wayland_trace_entry
{
  enum wayland_trace_kind kind;
  long long ns;
  intmax_t gcs;
};

struct wayland_presentation_feedback
{
  struct wl_list link;
  struct wayland_private *owner;
  struct wp_presentation_feedback *proxy;
};

/* Everything private to this file hangs off the display info through
   these two pointers, whose types the header only forward-declares.  */

struct wayland_private
{
  struct wayland_globals globals;
  struct wayland_binding binding;
  struct wayland_seat seat;
  struct wayland_monitor *outputs;

  /* Emacs events queued by listeners, moved into the keyboard buffer
     by the input hook.  */
  union buffered_input_event events[WAYLAND_EVENT_QUEUE_SIZE];
  int event_head, event_count;
  uint64_t dropped_events;
  uint64_t pool_bytes, retired_bytes, charged_pool_bytes, pool_budget;
  uint64_t repair_bytes, full_repairs, scroll_bytes, last_buffer_age;
  /* Times a frame found no buffer to draw into.  */
  uint64_t discards;
  uint64_t releases, last_release_ns, ack_only_commits, background_commits;
  /* The input hook left commits for the backend timer.  */
  bool present_queued;

  /* Pointer work that draws (mouse highlight, help echo, tool bar and
     tab bar buttons), left by listeners for the backend timer.  */
  bool pointer_work, pointer_moved;
  /* Where the pressed frame was in its root frame when the first
     button went down, in logical surface coordinates.  While buttons
     are held, compositors report the pointer relative to that place,
     even after the frame moves.  */
  int press_left, press_top;
  /* The buttons held at the latest motion.  A press dispatched in the
     same batch must not stop that motion highlighting the tool-bar
     item it then clicks.  */
  int pointer_grabbed;
  struct frame *pointer_left;
  struct wayland_bar_click
  {
    struct frame *frame;
    int x, y, modifiers;
    bool down, tool_bar;
  } bar_clicks[WAYLAND_MAX_BAR_CLICKS];
  int bar_click_count;
  bool scale_pending, scale_queued;
  struct xdg_activation_token_v1 *activation_token;
  struct frame *activation_frame;

  /* The diagnostic trace, a ring of its latest entries.  */
  struct wayland_trace_entry *trace;
  int trace_head, trace_count;

  /* Opt-in presentation feedback for content commits.  */
  bool presentation_enabled;
  struct wl_list presentation_pending;
  unsigned int presentation_count;
  uint64_t presentation_requested, presented, discarded, presentation_skipped;

  /* Transport probes, created only on explicit Lisp request.  */
  struct wl_region *debug_region;
  struct wl_data_device *debug_device;
  struct wl_data_source *debug_source;
  int debug_draws_remaining;
  bool debug_interrupt_draw;
};

static struct wayland_private *
private_of (struct wayland_display_info *dpyinfo)
{
  return (struct wayland_private *) dpyinfo->globals;
}

static long long monotonic_ns (void);

static void
wayland_trace (struct wayland_display_info *dpyinfo,
	       enum wayland_trace_kind kind)
{
  struct wayland_private *priv = private_of (dpyinfo);

  if (!priv->trace)
    return;
  int slot = (priv->trace_head + priv->trace_count) % WAYLAND_TRACE_SIZE;
  priv->trace[slot] = (struct wayland_trace_entry) { kind, monotonic_ns (),
						     gcs_done };
  if (priv->trace_count < WAYLAND_TRACE_SIZE)
    priv->trace_count++;
  else
    priv->trace_head = (priv->trace_head + 1) % WAYLAND_TRACE_SIZE;
}

static long long
monotonic_ns (void)
{
  struct timespec ts;
  clock_gettime (CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* Connection loss, stage 1.  */

static void wayland_arm_timer (struct wayland_display_info *, long);

/* Mark DPYINFO dead and disarm it.  Callable anywhere, including from
   inside redisplay: it only drops descriptors from Emacs's wait sets,
   records why, and fires the backend timer, whose callback starts
   stage 2.  */

static void
wayland_connection_lost (struct wayland_display_info *dpyinfo)
{
  int err;

  if (dpyinfo->state == WAYLAND_DISPLAY_DEAD)
    return;
  dpyinfo->state = WAYLAND_DISPLAY_DEAD;

  err = wl_display_get_error (dpyinfo->display);
  if (err == EPROTO)
    {
      const struct wl_interface *interface = NULL;
      uint32_t id = 0;
      uint32_t code = wl_display_get_protocol_error (dpyinfo->display,
						     &interface, &id);
      snprintf (dpyinfo->death_message, sizeof dpyinfo->death_message,
		"Wayland protocol error %" PRIu32 " on %s@%" PRIu32,
		code, interface ? interface->name : "unknown", id);
    }
  else
    snprintf (dpyinfo->death_message, sizeof dpyinfo->death_message,
	      "Connection to the Wayland compositor lost: %s",
	      err ? strerror (err) : "unknown reason");
  fprintf (stderr, "emacs: %s\n", dpyinfo->death_message);

  if (dpyinfo->service_fd >= 0)
    {
      delete_keyboard_wait_descriptor (dpyinfo->connection);
      delete_read_fd (dpyinfo->service_fd);
    }
  if (dpyinfo->repeat_fd >= 0)
    delete_read_fd (dpyinfo->repeat_fd);
  if (dpyinfo->output_state == WAYLAND_OUTPUT_PENDING
      && dpyinfo->write_fd >= 0)
    delete_write_fd (dpyinfo->write_fd);
  dpyinfo->output_state = WAYLAND_OUTPUT_IDLE;

  wayland_selection_connection_lost (dpyinfo);

  if (dpyinfo->timer_fd >= 0)
    wayland_arm_timer (dpyinfo, 1);
}

/* Flushing.  */

enum flush_result
{
  FLUSH_DONE,
  FLUSH_PENDING,
  FLUSH_BROKEN,
};

static void wayland_write_ready (int, void *);

/* Try one nonblocking flush, updating the output state.  */

static enum flush_result
wayland_flush (struct wayland_display_info *dpyinfo)
{
  if (wl_display_flush (dpyinfo->display) >= 0)
    {
      if (dpyinfo->output_state == WAYLAND_OUTPUT_PENDING)
	{
	  /* Pending to idle.  The write callback can present held-back
	     commits once frames exist.  */
	  if (dpyinfo->write_fd >= 0)
	    delete_write_fd (dpyinfo->write_fd);
	  dpyinfo->output_state = WAYLAND_OUTPUT_IDLE;
	  dpyinfo->metrics.output_resumed++;
	}
      return FLUSH_DONE;
    }

  if (errno == EAGAIN)
    {
      dpyinfo->metrics.flush_eagain++;
      if (dpyinfo->output_state == WAYLAND_OUTPUT_IDLE)
	{
	  /* Idle to pending: wait for writability on the duplicate fd,
	     whose callback slot is ours alone.  */
	  if (dpyinfo->write_fd >= 0)
	    add_write_fd (dpyinfo->write_fd, wayland_write_ready, dpyinfo);
	  dpyinfo->output_state = WAYLAND_OUTPUT_PENDING;
	}
      return FLUSH_PENDING;
    }

  if (errno == EPIPE)
    dpyinfo->metrics.flush_epipe++;
  return FLUSH_BROKEN;
}

/* After a broken flush outside the input hook (no read is prepared),
   read once without blocking so a protocol error the compositor sent
   before closing the socket is dispatched and logged.  libwayland
   keeps EPIPE non-fatal on flush for exactly this.  */

static void
wayland_read_after_broken_flush (struct wayland_display_info *dpyinfo)
{
  struct pollfd pfd = { .fd = dpyinfo->connection, .events = POLLIN };

  if (wl_display_prepare_read (dpyinfo->display) == 0)
    {
      if (poll (&pfd, 1, 0) > 0 && (pfd.revents & POLLIN))
	wl_display_read_events (dpyinfo->display);
      else
	wl_display_cancel_read (dpyinfo->display);
    }
  wl_display_dispatch_pending (dpyinfo->display);
  wayland_connection_lost (dpyinfo);
}

/* Send queued requests now, outside a dispatch, without blocking.  */

static void
wayland_flush_now (struct wayland_display_info *dpyinfo)
{
  if (dpyinfo->state == WAYLAND_DISPLAY_DEAD || dpyinfo->dispatching)
    return;
  if (wayland_flush (dpyinfo) == FLUSH_BROKEN)
    wayland_read_after_broken_flush (dpyinfo);
}

/* The nonblocking service routine shared by the input hook and the
   service-fd callback.  PRESENT says whether it may commit frames that
   callbacks marked.  The input hook runs wherever Emacs processes
   pending signals, even inside a frame resize between clearing the
   pending size and applying it (bug found with SIGIO), so it passes
   false and leaves the commits to the backend timer, whose callback
   runs only from Emacs's wait loop.  */

static void
wayland_service (struct wayland_display_info *dpyinfo, bool present)
{
  struct wl_display *display = dpyinfo->display;
  struct pollfd pfd = { .fd = dpyinfo->connection, .events = POLLIN };
  bool broken = false;

  if (dpyinfo->state == WAYLAND_DISPLAY_DEAD || dpyinfo->dispatching)
    return;
  dpyinfo->dispatching = true;

  if (wl_display_dispatch_pending (display) < 0)
    goto lost;
  while (wl_display_prepare_read (display) != 0)
    if (wl_display_dispatch_pending (display) < 0)
      goto lost;

  /* A broken flush is handled after the read, so that the read prepared
     above is used to collect a protocol error.  */
  broken = wayland_flush (dpyinfo) == FLUSH_BROKEN;

  if (poll (&pfd, 1, 0) > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR)))
    {
      if (wl_display_read_events (display) < 0 && !broken)
	goto lost;
    }
  else
    wl_display_cancel_read (display);

  if (wl_display_dispatch_pending (display) < 0 || broken)
    goto lost;

  /* Commits that callbacks above only marked as wanted (a configure, a
     frame callback, a buffer release) are made here, or at the next
     safe point.  */
  if (present)
    wayland_present_marked (dpyinfo);
  else if (wayland_commits_wanted (dpyinfo))
    {
      private_of (dpyinfo)->present_queued = true;
      wayland_arm_timer (dpyinfo, 1);
    }
  wayland_present_backgrounds (dpyinfo);

  /* Requests queued by the dispatch above, such as a pong or a commit,
     leave in this same call.  */
  if (wayland_flush (dpyinfo) == FLUSH_BROKEN)
    {
      dpyinfo->dispatching = false;
      wayland_read_after_broken_flush (dpyinfo);
      return;
    }

  dpyinfo->dispatching = false;
  return;

 lost:
  dpyinfo->dispatching = false;
  wayland_connection_lost (dpyinfo);
}

bool
wayland_selection_focused_p (struct wayland_display_info *dpyinfo)
{
  return dpyinfo->state == WAYLAND_DISPLAY_LIVE
    && private_of (dpyinfo)->seat.keyboard_focus;
}

bool
wayland_display_has_pointer (struct wayland_display_info *dpyinfo)
{
  return dpyinfo->state == WAYLAND_DISPLAY_LIVE
    && private_of (dpyinfo)->seat.pointer;
}

uint32_t
wayland_selection_serial (struct wayland_display_info *dpyinfo)
{
  return wayland_selection_focused_p (dpyinfo)
    ? private_of (dpyinfo)->seat.last_input_serial : 0;
}

void
wayland_selection_flush (struct wayland_display_info *dpyinfo)
{
  wayland_flush_now (dpyinfo);
}

/* The terminal's read_socket_hook.  It never waits: `gobble_input'
   calls it whether or not the socket is readable and keeps calling it
   while it returns a positive count.  */

static int
wayland_read_socket (struct terminal *terminal, struct input_event *hold_quit)
{
  struct wayland_display_info *dpyinfo = terminal->display_info.wayland;
  long long start = monotonic_ns (), elapsed;

  if (!dpyinfo || dpyinfo->state == WAYLAND_DISPLAY_DEAD)
    return 0;
  dpyinfo->metrics.hook_calls++;
  wayland_service (dpyinfo, false);
  elapsed = monotonic_ns () - start;
  if (elapsed > dpyinfo->metrics.max_hook_ns)
    dpyinfo->metrics.max_hook_ns = elapsed;

  return wayland_flush_events (dpyinfo, hold_quit);
}

/* Waits that leave keyboard descriptors out still run the service
   routine through this callback on the duplicate socket.  */

static void
wayland_service_ready (int fd, void *data)
{
  struct wayland_display_info *dpyinfo = data;

  dpyinfo->metrics.service_calls++;
  wayland_service (dpyinfo, true);
  wayland_flush_with_quit (dpyinfo);
}

/* The key repeat timer's callback, in every wait.  */

static void
wayland_repeat_ready (int fd, void *data)
{
  struct wayland_display_info *dpyinfo = data;

  wayland_repeat (dpyinfo);
  wayland_flush_with_quit (dpyinfo);
}

static void
wayland_write_ready (int fd, void *data)
{
  struct wayland_display_info *dpyinfo = data;

  if (dpyinfo->state == WAYLAND_DISPLAY_DEAD || dpyinfo->dispatching)
    return;
  dpyinfo->metrics.write_callbacks++;
  switch (wayland_flush (dpyinfo))
    {
    case FLUSH_BROKEN:
      wayland_read_after_broken_flush (dpyinfo);
      break;
    case FLUSH_DONE:
      /* Commits deferred while output was pending go out now.  */
      dpyinfo->metrics.write_resumes++;
      wayland_present_marked (dpyinfo);
      if (wayland_flush (dpyinfo) == FLUSH_BROKEN)
	wayland_read_after_broken_flush (dpyinfo);
      break;
    default:
      break;
    }
}

/* Blocking waits.  Startup roundtrips and clipboard reads use an
   absolute deadline.  */

static void
roundtrip_done (void *data, struct wl_callback *callback, uint32_t serial)
{
  *(bool *) data = true;
  wl_callback_destroy (callback);
}

static const struct wl_callback_listener roundtrip_listener =
  { .done = roundtrip_done };

/* Wait until the compositor has processed every request sent so far,
   or until TIMEOUT_MS passes.  Return true on success.  */

static bool
wayland_roundtrip (struct wayland_display_info *dpyinfo, int timeout_ms)
{
  struct wl_display *display = dpyinfo->display;
  struct wl_callback *callback = wl_display_sync (display);
  long long deadline = monotonic_ns () + timeout_ms * 1000000LL;
  bool done = false;

  if (!callback)
    goto lost;
  wl_callback_add_listener (callback, &roundtrip_listener, &done);

  while (!done)
    {
      struct pollfd pfd = { .fd = dpyinfo->connection, .events = POLLIN };
      long long remaining_ms;
      enum flush_result flushed;

      if (wl_display_dispatch_pending (display) < 0)
	goto lost;
      if (done)
	break;
      while (wl_display_prepare_read (display) != 0)
	if (wl_display_dispatch_pending (display) < 0)
	  goto lost;

      flushed = wayland_flush (dpyinfo);
      if (flushed == FLUSH_PENDING)
	pfd.events |= POLLOUT;

      remaining_ms = (deadline - monotonic_ns ()) / 1000000;
      if (remaining_ms <= 0 || flushed == FLUSH_BROKEN)
	{
	  wl_display_cancel_read (display);
	  if (flushed == FLUSH_BROKEN)
	    wayland_read_after_broken_flush (dpyinfo);
	  goto failed;
	}

      if (poll (&pfd, 1, remaining_ms) > 0
	  && (pfd.revents & (POLLIN | POLLHUP | POLLERR)))
	{
	  if (wl_display_read_events (display) < 0)
	    goto lost;
	}
      else
	wl_display_cancel_read (display);
    }
  return true;

 lost:
  wayland_connection_lost (dpyinfo);
 failed:
  if (callback && !done)
    wl_callback_destroy (callback);
  return false;
}

/* Connection loss, stage 2.  */

static void
wayland_arm_timer (struct wayland_display_info *dpyinfo, long delay_ns)
{
  struct itimerspec spec = { .it_value = { .tv_sec = 0,
					   .tv_nsec = delay_ns } };
  timerfd_settime (dpyinfo->timer_fd, 0, &spec, NULL);
}

/* Whether DPYINFO's terminal may be deleted now: no redisplay is
   active (`redisplaying_p' is already set while redisplay runs Lisp,
   so this covers waits nested inside `pre-redisplay-function'), and no
   backend operation holds display or frame references.  */

static bool
wayland_finalize_allowed (struct wayland_display_info *dpyinfo)
{
  return !redisplaying_p && dpyinfo->busy == 0;
}

static void
wayland_timer_ready (int fd, void *data)
{
  struct wayland_display_info *dpyinfo = data;
  uint64_t expirations;

  if (read (fd, &expirations, sizeof expirations) < 0)
    return;

  struct wayland_private *queued = private_of (dpyinfo);
  if (queued->pointer_work && dpyinfo->state == WAYLAND_DISPLAY_LIVE)
    {
      /* Pointer work draws: inside redisplay, as in a wait nested in
	 `pre-redisplay-function', try again shortly.  */
      if (redisplaying_p)
	wayland_arm_timer (dpyinfo, WAYLAND_FINALIZE_RETRY_NS);
      else
	{
	  /* Queued keys come before the events this work stores.  */
	  wayland_flush_with_quit (dpyinfo);
	  wayland_pointer_work (dpyinfo);
	  queued->present_queued = true;
	}
    }
  if (queued->present_queued && dpyinfo->state == WAYLAND_DISPLAY_LIVE)
    {
      queued->present_queued = false;
      wayland_present_marked (dpyinfo);
      if (wayland_flush (dpyinfo) == FLUSH_BROKEN)
	wayland_read_after_broken_flush (dpyinfo);
    }

  if (dpyinfo->state == WAYLAND_DISPLAY_DEAD && !dpyinfo->finalize_queued)
    {
      if (!wayland_finalize_allowed (dpyinfo))
	{
	  dpyinfo->metrics.finalize_deferrals++;
	  wayland_arm_timer (dpyinfo, WAYLAND_FINALIZE_RETRY_NS);
	  return;
	}
      /* Never re-queue from inside the drain: `timer_check_2' consumes
	 entries queued during the drain in the same loop and would
	 spin.  This callback runs outside it.  */
      Lisp_Object terminal;
      XSETTERMINAL (terminal, dpyinfo->terminal);
      dpyinfo->finalize_queued = true;
      pending_funcalls = Fcons (list2 (Qwayland__finalize_display, terminal),
				pending_funcalls);
    }

  struct wayland_private *priv = private_of (dpyinfo);
  if (dpyinfo->state == WAYLAND_DISPLAY_LIVE
      && priv->scale_pending && !priv->scale_queued)
    {
      if (!wayland_finalize_allowed (dpyinfo))
        {
          wayland_arm_timer (dpyinfo, WAYLAND_FINALIZE_RETRY_NS);
          return;
        }
      Lisp_Object terminal;
      XSETTERMINAL (terminal, dpyinfo->terminal);
      priv->scale_queued = true;
      pending_funcalls = Fcons (list2 (Qwayland__apply_scale, terminal),
                                pending_funcalls);
    }
}

DEFUN ("wayland--finalize-display", Fwayland__finalize_display,
       Swayland__finalize_display, 1, 1, 0,
       doc: /* Delete TERMINAL after its Wayland connection was lost.
Internal: queued on `pending_funcalls' by the backend.  */)
  (Lisp_Object terminal)
{
  struct terminal *t = TERMINALP (terminal) ? XTERMINAL (terminal) : NULL;
  struct wayland_display_info *dpyinfo;

  /* A deleted terminal has no name; its display info is gone too.  */
  if (!t || !t->name || t->type != output_wayland
      || !(dpyinfo = t->display_info.wayland))
    return Qnil;

  if (!wayland_finalize_allowed (dpyinfo))
    {
      dpyinfo->metrics.finalize_deferrals++;
      dpyinfo->finalize_queued = false;
      wayland_arm_timer (dpyinfo, WAYLAND_FINALIZE_RETRY_NS);
      return Qnil;
    }

  Fdelete_terminal (terminal, Qnoelisp);

  /* As X does in x_connection_closed: with no display left to use, a
     non-daemon Emacs exits.  */
  if (terminal_list == 0 || NILP (selected_frame))
    Fkill_emacs (make_fixnum (70), Qnil);
  return Qt;
}

/* xdg_wm_base.  */

static void
wm_base_ping (void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
  struct wayland_display_info *dpyinfo = data;

  dpyinfo->metrics.pings++;
  xdg_wm_base_pong (wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener =
  { .ping = wm_base_ping };

/* Input events.  Listeners translate Wayland input into Emacs events
   and queue them here; the input hook moves them into the keyboard
   buffer, so a quit character takes the hold_quit path.  A composed
   or committed string takes one entry, a NO_EVENT whose arg is the
   string, and becomes one event per character only in the keyboard
   buffer, so a long input-method commit cannot overflow the queue.
   mark_waylandterm marks the queued events.  */

static bool
wayland_string_event_p (union buffered_input_event *event)
{
  return event->kind == NO_EVENT && STRINGP (event->ie.arg);
}

static void
wayland_queue_event (struct wayland_display_info *dpyinfo,
		     union buffered_input_event *event)
{
  struct wayland_private *priv = private_of (dpyinfo);

  if (priv->event_count == WAYLAND_EVENT_QUEUE_SIZE)
    {
      /* Emacs drops events from a full keyboard buffer too.  */
      priv->dropped_events++;
      return;
    }
  priv->events[(priv->event_head + priv->event_count)
	       % WAYLAND_EVENT_QUEUE_SIZE] = *event;
  priv->event_count++;
}

void
wayland_queue_input_event (struct wayland_display_info *dpyinfo,
			   union buffered_input_event *event)
{
  wayland_queue_event (dpyinfo, event);
}

/* Move queued events into the keyboard buffer and return how many
   moved.  Each event leaves the queue before it is stored, because
   storing a quit character can throw (bug#52941).  */

static int
wayland_flush_events (struct wayland_display_info *dpyinfo,
		      struct input_event *hold_quit)
{
  struct wayland_private *priv = private_of (dpyinfo);
  int count = 0;

  while (priv->event_count > 0)
    {
      union buffered_input_event event = priv->events[priv->event_head];
      priv->event_head = (priv->event_head + 1) % WAYLAND_EVENT_QUEUE_SIZE;
      priv->event_count--;
      if (wayland_string_event_p (&event))
	{
	  /* Its characters, in order.  A quit character cannot be
	     composed, so none of them throws.  */
	  Lisp_Object string = event.ie.arg;
	  ptrdiff_t chars = SCHARS (string), i = 0, byte = 0;
	  while (i < chars)
	    {
	      int c = fetch_string_char_advance (string, &i, &byte);
	      event.ie.kind = (c < 0x80 ? ASCII_KEYSTROKE_EVENT
			       : MULTIBYTE_CHAR_KEYSTROKE_EVENT);
	      event.ie.code = c;
	      event.ie.arg = Qnil;
	      kbd_buffer_store_buffered_event (&event, hold_quit);
	      count++;
	    }
	  continue;
	}
      kbd_buffer_store_buffered_event (&event, hold_quit);
      count++;
    }
  if (count)
    wayland_trace (dpyinfo, WAYLAND_TRACE_DELIVER);
  return count;
}

/* Advance the display while waiting for a selection pipe.  Leave quit
   handling to the caller after it has released the pipe.  */
bool
wayland_selection_service (struct wayland_display_info *dpyinfo)
{
  struct input_event quit;
  EVENT_INIT (quit);
  quit.kind = NO_EVENT;
  wayland_service (dpyinfo, false);
  wayland_flush_events (dpyinfo, &quit);
  if (quit.kind != NO_EVENT)
    Vquit_flag = Qt;
  return dpyinfo->state == WAYLAND_DISPLAY_LIVE && quit.kind == NO_EVENT;
}


/* Drop the queued events for frame F, which is being deleted.  */

static void
wayland_purge_events (struct wayland_display_info *dpyinfo, struct frame *f)
{
  struct wayland_private *priv = private_of (dpyinfo);
  int kept = 0;

  for (int i = 0; i < priv->event_count; i++)
    {
      union buffered_input_event *event
	= &priv->events[(priv->event_head + i) % WAYLAND_EVENT_QUEUE_SIZE];
      if (FRAMEP (event->ie.frame_or_window)
	  && XFRAME (event->ie.frame_or_window) == f)
	continue;
      priv->events[(priv->event_head + kept++) % WAYLAND_EVENT_QUEUE_SIZE]
	= *event;
    }
  priv->event_count = kept;
}

/* Load the compose table for the user's locale, as foot does.  Without
   one, dead keys reach Emacs as dead-* function keys, as in PGTK.  */

static void
wayland_make_compose_state (struct wayland_seat *seat)
{
  const char *locale = getenv ("LC_ALL");

  if (!locale || !*locale)
    locale = getenv ("LC_CTYPE");
  if (!locale || !*locale)
    locale = getenv ("LANG");
  if (!locale || !*locale)
    locale = "C";
  if (!seat->xkb_context)
    return;
  seat->compose_table
    = xkb_compose_table_new_from_locale (seat->xkb_context, locale,
					 XKB_COMPOSE_COMPILE_NO_FLAGS);
  if (seat->compose_table)
    seat->compose_state
      = xkb_compose_state_new (seat->compose_table,
			       XKB_COMPOSE_STATE_NO_FLAGS);
}

/* The modifier bits `x-ctrl-keysym' and the like ask for, as in
   pgtkterm.c.  */

static void
get_modifier_values (int *mod_ctrl, int *mod_meta, int *mod_alt,
		     int *mod_hyper, int *mod_super)
{
  Lisp_Object tem;

  *mod_ctrl = ctrl_modifier;
  *mod_meta = meta_modifier;
  *mod_alt = alt_modifier;
  *mod_hyper = hyper_modifier;
  *mod_super = super_modifier;

  tem = Fget (Vx_ctrl_keysym, Qmodifier_value);
  if (INTEGERP (tem))
    *mod_ctrl = XFIXNUM (tem) & INT_MAX;
  tem = Fget (Vx_alt_keysym, Qmodifier_value);
  if (INTEGERP (tem))
    *mod_alt = XFIXNUM (tem) & INT_MAX;
  tem = Fget (Vx_meta_keysym, Qmodifier_value);
  if (INTEGERP (tem))
    *mod_meta = XFIXNUM (tem) & INT_MAX;
  tem = Fget (Vx_hyper_keysym, Qmodifier_value);
  if (INTEGERP (tem))
    *mod_hyper = XFIXNUM (tem) & INT_MAX;
  tem = Fget (Vx_super_keysym, Qmodifier_value);
  if (INTEGERP (tem))
    *mod_super = XFIXNUM (tem) & INT_MAX;
}

/* `extra-keyboard-modifiers' holds X modifier bits: 1 is Shift, 4 is
   Control and 8 is Meta (Mod1).  */

static int
wayland_extra_modifiers (void)
{
  int mod_ctrl, mod_meta, mod_alt, mod_hyper, mod_super;
  int modifiers = 0;

  get_modifier_values (&mod_ctrl, &mod_meta, &mod_alt, &mod_hyper,
		       &mod_super);
  if (extra_keyboard_modifiers & 1)
    modifiers |= shift_modifier;
  if (extra_keyboard_modifiers & 4)
    modifiers |= mod_ctrl;
  if (extra_keyboard_modifiers & 8)
    modifiers |= mod_meta;
  return modifiers;
}

static bool
wayland_mod_active (struct wayland_seat *seat, xkb_mod_index_t index)
{
  return (index != XKB_MOD_INVALID
	  && xkb_state_mod_index_is_active (seat->state, index,
					    XKB_STATE_MODS_EFFECTIVE) > 0);
}

/* The Emacs modifiers of SEAT's current state.  Like PGTK, Alt (Mod1)
   is Meta and the logo key (Mod4) is Super; Mod3, where some keymaps
   put Hyper, is Hyper.  */

static int
wayland_emacs_modifiers (struct wayland_seat *seat)
{
  int mod_ctrl, mod_meta, mod_alt, mod_hyper, mod_super;
  int modifiers = 0;

  if (!seat->state)
    return 0;
  get_modifier_values (&mod_ctrl, &mod_meta, &mod_alt, &mod_hyper,
		       &mod_super);
  if (wayland_mod_active (seat, seat->mod_shift))
    modifiers |= shift_modifier;
  if (wayland_mod_active (seat, seat->mod_ctrl))
    modifiers |= mod_ctrl;
  if (wayland_mod_active (seat, seat->mod_alt))
    modifiers |= mod_meta;
  if (wayland_mod_active (seat, seat->mod_logo))
    modifiers |= mod_super;
  if (wayland_mod_active (seat, seat->mod_mod3))
    modifiers |= mod_hyper;
  return modifiers;
}

/* Whether KEYSYM is a function key, which make_lispy_event names
   through lispy_function_keys; this follows pgtkterm.c, whose GDK
   keysyms have the same values.  */

static bool
wayland_function_keysym_p (xkb_keysym_t keysym)
{
  /* Modifier keys, and ISO_Lock to ISO_Last_Group_Lock, which act
     like Mode_switch.  */
  if ((keysym >= XKB_KEY_Shift_L && keysym <= XKB_KEY_Hyper_R)
      || (keysym >= XKB_KEY_ISO_Lock && keysym <= XKB_KEY_ISO_Last_Group_Lock))
    return false;
  return ((keysym >= XKB_KEY_BackSpace && keysym <= XKB_KEY_Escape)
	  || keysym == XKB_KEY_Delete
	  || (keysym >= XKB_KEY_ISO_Left_Tab && keysym <= XKB_KEY_ISO_Enter)
	  /* Cursor, miscellaneous function, keypad and function keys.  */
	  || (keysym >= XKB_KEY_Home && keysym < XKB_KEY_Shift_L)
	  /* Dead keys the compose table did not take.  */
	  || (keysym >= XKB_KEY_dead_grave && keysym <= XKB_KEY_dead_greek)
	  /* Vendor-specific keys.  */
	  || (keysym & (1 << 28)));
}

static void
wayland_queue_char (struct wayland_display_info *dpyinfo,
		    union buffered_input_event *event, int c)
{
  event->ie.kind = (c < 0x80 ? ASCII_KEYSTROKE_EVENT
		    : MULTIBYTE_CHAR_KEYSTROKE_EVENT);
  event->ie.code = c;
  wayland_queue_event (dpyinfo, event);
}

/* Queue the Emacs event for KEYSYM, pressed on frame F with
   MODIFIERS at TIME.  */

static void
wayland_queue_keysym (struct wayland_display_info *dpyinfo,
		      struct frame *f, xkb_keysym_t keysym, int modifiers,
		      uint32_t time)
{
  union buffered_input_event event;
  uint32_t c;

  EVENT_INIT (event.ie);
  XSETFRAME (event.ie.frame_or_window, f);
  event.ie.modifiers = modifiers;
  event.ie.timestamp = time;

  if (keysym >= 32 && keysym < 128)
    wayland_queue_char (dpyinfo, &event, keysym);
  else if (keysym >= 0x01000000 && keysym <= 0x0110FFFF)
    wayland_queue_char (dpyinfo, &event, keysym & 0xFFFFFF);
  else if (wayland_function_keysym_p (keysym))
    {
      event.ie.kind = NON_ASCII_KEYSTROKE_EVENT;
      event.ie.code = keysym;
      wayland_queue_event (dpyinfo, &event);
    }
  else if ((c = xkb_keysym_to_utf32 (keysym)))
    wayland_queue_char (dpyinfo, &event, c);
}

/* Queue the characters of a composed UTF-8 string, as one entry.  */

void
wayland_queue_utf8 (struct wayland_display_info *dpyinfo, struct frame *f,
		    const char *utf8, int modifiers, uint32_t time)
{
  union buffered_input_event event;

  EVENT_INIT (event.ie);
  event.ie.kind = NO_EVENT;
  event.ie.arg = build_string (utf8);
  XSETFRAME (event.ie.frame_or_window, f);
  event.ie.modifiers = modifiers;
  event.ie.timestamp = time;
  if (SCHARS (event.ie.arg) > 0)
    wayland_queue_event (dpyinfo, &event);
}

/* Move Emacs's notion of keyboard focus to F, or nowhere.  Only state
   changes and queued events here: listeners may run inside redisplay,
   so the cursors are redrawn by the next redisplay.  */

static void
wayland_mark_cursor (struct frame *f)
{
  if (f && FRAME_LIVE_P (f))
    {
      f->cursor_type_changed = true;
      fset_redisplay (f);
    }
}

static void
wayland_rehighlight (struct wayland_display_info *dpyinfo)
{
  struct frame *old = dpyinfo->highlight_frame;
  struct frame *focus = dpyinfo->x_focus_frame;

  dpyinfo->highlight_frame
    = (focus && FRAMEP (FRAME_FOCUS_FRAME (focus))
       && FRAME_LIVE_P (XFRAME (FRAME_FOCUS_FRAME (focus)))
       ? XFRAME (FRAME_FOCUS_FRAME (focus)) : focus);
  if (old != dpyinfo->highlight_frame)
    {
      wayland_mark_cursor (old);
      wayland_mark_cursor (dpyinfo->highlight_frame);
    }
}

static void
wayland_new_focus (struct wayland_display_info *dpyinfo, struct frame *f)
{
  struct frame *old = dpyinfo->x_focus_frame;
  union buffered_input_event event;

  if (old == f)
    return;
  wayland_stop_repeat (dpyinfo);
  dpyinfo->x_focus_frame = f;
  dpyinfo->x_focus_event_frame = f;
  wayland_im_focus_changed (dpyinfo, old, f);
  if (old && FRAME_LIVE_P (old))
    {
      EVENT_INIT (event.ie);
      event.ie.kind = FOCUS_OUT_EVENT;
      XSETFRAME (event.ie.frame_or_window, old);
      wayland_queue_event (dpyinfo, &event);
    }
  if (f)
    {
      EVENT_INIT (event.ie);
      event.ie.kind = FOCUS_IN_EVENT;
      XSETFRAME (event.ie.frame_or_window, f);
      wayland_queue_event (dpyinfo, &event);
    }
  wayland_rehighlight (dpyinfo);
}

/* Queue the events for evdev key KEY, pressed on frame F at TIME.
   COMPOSE says whether to feed the compose state; repeats don't.  */

static void
wayland_translate_key (struct wayland_display_info *dpyinfo,
		       struct frame *f, uint32_t key, uint32_t time,
		       bool compose)
{
  struct wayland_seat *seat = &private_of (dpyinfo)->seat;
  /* xkbcommon keycodes are evdev codes plus 8.  */
  xkb_keysym_t keysym = xkb_state_key_get_one_sym (seat->state, key + 8);
  int modifiers = wayland_emacs_modifiers (seat) | wayland_extra_modifiers ();

  if (compose && seat->compose_state
      && (xkb_compose_state_feed (seat->compose_state, keysym)
	  == XKB_COMPOSE_FEED_ACCEPTED))
    switch (xkb_compose_state_get_status (seat->compose_state))
      {
      case XKB_COMPOSE_COMPOSING:
	return;

      case XKB_COMPOSE_COMPOSED:
	{
	  char utf8[64];
	  xkb_keysym_t composed
	    = xkb_compose_state_get_one_sym (seat->compose_state);

	  xkb_compose_state_get_utf8 (seat->compose_state, utf8,
				      sizeof utf8);
	  xkb_compose_state_reset (seat->compose_state);
	  if (composed != XKB_KEY_NoSymbol)
	    wayland_queue_keysym (dpyinfo, f, composed, modifiers, time);
	  else if (utf8[0])
	    wayland_queue_utf8 (dpyinfo, f, utf8, modifiers, time);
	  return;
	}

      case XKB_COMPOSE_CANCELLED:
	xkb_compose_state_reset (seat->compose_state);
	return;

      case XKB_COMPOSE_NOTHING:
	break;
      }

  wayland_queue_keysym (dpyinfo, f, keysym, modifiers, time);
}

/* Key repeat is the client's job on Wayland.  A pressed key that the
   keymap says repeats arms the repeat timer for the delay, then the
   interval of `repeat_info'; a zero rate disables repeat.  Release,
   focus loss, a new keymap and losing the keyboard stop it.  */

static void
wayland_set_repeat_timer (struct wayland_display_info *dpyinfo,
			  int delay_ms, int interval_ms)
{
  struct itimerspec spec =
    {
      .it_interval = { interval_ms / 1000, (interval_ms % 1000) * 1000000 },
      .it_value = { delay_ms / 1000, (delay_ms % 1000) * 1000000 },
    };
  if (dpyinfo->repeat_fd >= 0)
    timerfd_settime (dpyinfo->repeat_fd, 0, &spec, NULL);
}

static void
wayland_stop_repeat (struct wayland_display_info *dpyinfo)
{
  private_of (dpyinfo)->seat.repeating = false;
  wayland_set_repeat_timer (dpyinfo, 0, 0);
}

static void
wayland_start_repeat (struct wayland_display_info *dpyinfo, uint32_t key,
		      uint32_t time)
{
  struct wayland_seat *seat = &private_of (dpyinfo)->seat;

  if (seat->repeat_rate <= 0 || !seat->keymap
      || !xkb_keymap_key_repeats (seat->keymap, key + 8))
    {
      /* A key that doesn't repeat still ends another key's repeat,
	 unless it is a modifier, which repeats with it.  */
      if (seat->repeat_rate <= 0)
	wayland_stop_repeat (dpyinfo);
      return;
    }
  seat->repeating = true;
  seat->repeat_key = key;
  seat->repeat_time = time;
  wayland_set_repeat_timer (dpyinfo, max (1, seat->repeat_delay),
			    max (1, 1000 / seat->repeat_rate));
}

/* Queue the repeats that are due.  A slow Emacs gets at most a few at
   once, so a backlog doesn't arrive as a burst.  */

enum { WAYLAND_MAX_REPEAT_BURST = 4 };

static void
wayland_repeat (struct wayland_display_info *dpyinfo)
{
  struct wayland_seat *seat = &private_of (dpyinfo)->seat;
  uint64_t expirations;
  struct frame *f;

  if (dpyinfo->repeat_fd < 0
      || read (dpyinfo->repeat_fd, &expirations, sizeof expirations)
	 != sizeof expirations
      || !seat->repeating || !seat->state
      || !(f = dpyinfo->x_focus_frame))
    return;
  for (uint64_t i = 0; i < min (expirations, WAYLAND_MAX_REPEAT_BURST); i++)
    {
      seat->repeat_time += max (1, 1000 / max (1, seat->repeat_rate));
      wayland_translate_key (dpyinfo, f, seat->repeat_key,
			     seat->repeat_time, false);
    }
}

static void
keyboard_keymap (void *data, struct wl_keyboard *keyboard, uint32_t format,
		 int32_t fd, uint32_t size)
{
  struct wayland_display_info *dpyinfo = data;
  struct wayland_seat *seat = &private_of (dpyinfo)->seat;
  struct xkb_keymap *keymap = NULL;
  char *map;

  if (format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1
      && (map = mmap (NULL, size, PROT_READ, MAP_PRIVATE, fd, 0))
	  != MAP_FAILED)
    {
      keymap = xkb_keymap_new_from_buffer (seat->xkb_context, map,
					   strnlen (map, size),
					   XKB_KEYMAP_FORMAT_TEXT_V1,
					   XKB_KEYMAP_COMPILE_NO_FLAGS);
      munmap (map, size);
    }
  emacs_close (fd);

  if (!keymap)
    return;
  if (seat->state)
    xkb_state_unref (seat->state);
  if (seat->keymap)
    xkb_keymap_unref (seat->keymap);
  seat->keymap = keymap;
  seat->state = xkb_state_new (keymap);
  seat->mod_shift = xkb_keymap_mod_get_index (keymap, XKB_MOD_NAME_SHIFT);
  seat->mod_ctrl = xkb_keymap_mod_get_index (keymap, XKB_MOD_NAME_CTRL);
  seat->mod_alt = xkb_keymap_mod_get_index (keymap, XKB_MOD_NAME_ALT);
  seat->mod_logo = xkb_keymap_mod_get_index (keymap, XKB_MOD_NAME_LOGO);
  seat->mod_mod3 = xkb_keymap_mod_get_index (keymap, "Mod3");
  if (seat->compose_state)
    xkb_compose_state_reset (seat->compose_state);
  wayland_stop_repeat (dpyinfo);
}

static void
keyboard_enter (void *data, struct wl_keyboard *keyboard, uint32_t serial,
		struct wl_surface *surface, struct wl_array *keys)
{
  struct wayland_display_info *dpyinfo = data;
  struct wayland_seat *seat = &private_of (dpyinfo)->seat;

  seat->keyboard_enter_serial = serial;
  seat->keyboard_focus = surface;
  if (seat->compose_state)
    xkb_compose_state_reset (seat->compose_state);
  wayland_new_focus (dpyinfo, wayland_surface_frame (dpyinfo, surface));
}

static void
keyboard_leave (void *data, struct wl_keyboard *keyboard, uint32_t serial,
		struct wl_surface *surface)
{
  struct wayland_display_info *dpyinfo = data;
  struct wayland_seat *seat = &private_of (dpyinfo)->seat;

  seat->keyboard_focus = NULL;
  if (seat->compose_state)
    xkb_compose_state_reset (seat->compose_state);
  wayland_stop_repeat (dpyinfo);
  wayland_new_focus (dpyinfo, NULL);
}

static void
keyboard_key (void *data, struct wl_keyboard *keyboard, uint32_t serial,
	      uint32_t time, uint32_t key, uint32_t state)
{
  struct wayland_display_info *dpyinfo = data;
  struct wayland_seat *seat = &private_of (dpyinfo)->seat;
  struct frame *f;

  seat->last_input_serial = serial;
  dpyinfo->metrics.raw_keys++;
  if (key < WAYLAND_RAW_KEYCODES)
    seat->raw_keycodes[key]++;

  if (state != WL_KEYBOARD_KEY_STATE_PRESSED)
    {
      if (seat->repeating && seat->repeat_key == key)
	wayland_stop_repeat (dpyinfo);
      return;
    }
  wayland_trace (dpyinfo, WAYLAND_TRACE_KEY);
  if (!seat->state
      || !(f = dpyinfo->x_focus_frame))
    return;
  wayland_translate_key (dpyinfo, f, key, time, true);
  wayland_start_repeat (dpyinfo, key, time);
}

static void
keyboard_modifiers (void *data, struct wl_keyboard *keyboard,
		    uint32_t serial, uint32_t depressed, uint32_t latched,
		    uint32_t locked, uint32_t group)
{
  struct wayland_display_info *dpyinfo = data;
  struct wayland_seat *seat = &private_of (dpyinfo)->seat;

  if (seat->state)
    xkb_state_update_mask (seat->state, depressed, latched, locked, 0, 0,
			   group);
}

static void
keyboard_repeat_info (void *data, struct wl_keyboard *keyboard,
		      int32_t rate, int32_t delay)
{
  struct wayland_display_info *dpyinfo = data;
  struct wayland_seat *seat = &private_of (dpyinfo)->seat;

  seat->repeat_rate = rate;
  seat->repeat_delay = delay;
  if (rate <= 0)
    wayland_stop_repeat (dpyinfo);
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

static const struct wl_pointer_listener pointer_listener;

static void
seat_capabilities (void *data, struct wl_seat *wl_seat, uint32_t caps)
{
  struct wayland_display_info *dpyinfo = data;
  struct wayland_seat *seat = &private_of (dpyinfo)->seat;

  seat->capabilities = caps;
  if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !seat->keyboard)
    {
      seat->keyboard = wl_seat_get_keyboard (wl_seat);
      wl_keyboard_add_listener (seat->keyboard, &keyboard_listener, dpyinfo);
    }
  else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && seat->keyboard)
    {
      /* Losing the keyboard also ends focus and key repeat.  */
      if (wl_keyboard_get_version (seat->keyboard)
	  >= WL_KEYBOARD_RELEASE_SINCE_VERSION)
	wl_keyboard_release (seat->keyboard);
      else
	wl_keyboard_destroy (seat->keyboard);
      seat->keyboard = NULL;
      seat->keyboard_focus = NULL;
      wayland_stop_repeat (dpyinfo);
      wayland_new_focus (dpyinfo, NULL);
    }

  if ((caps & WL_SEAT_CAPABILITY_POINTER) && !seat->pointer)
    {
      struct wp_cursor_shape_manager_v1 *shapes
	= private_of (dpyinfo)->globals.cursor_shape_manager;

      seat->pointer = wl_seat_get_pointer (wl_seat);
      wl_pointer_add_listener (seat->pointer, &pointer_listener, dpyinfo);
      if (shapes)
	seat->cursor_device
	  = wp_cursor_shape_manager_v1_get_pointer (shapes, seat->pointer);
    }
  else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && seat->pointer)
    {
      if (seat->cursor_device)
	wp_cursor_shape_device_v1_destroy (seat->cursor_device);
      seat->cursor_device = NULL;
      if (wl_pointer_get_version (seat->pointer)
	  >= WL_POINTER_RELEASE_SINCE_VERSION)
	wl_pointer_release (seat->pointer);
      else
	wl_pointer_destroy (seat->pointer);
      seat->pointer = NULL;
      seat->pointer_focus = NULL;
      dpyinfo->grabbed = 0;
    }
}

static void
seat_name (void *data, struct wl_seat *wl_seat, const char *name)
{
}

static const struct wl_seat_listener seat_listener =
  {
    .capabilities = seat_capabilities,
    .name = seat_name,
  };

/* Basic display dimensions.  Frame-to-output tracking is separate.  */

static void
output_geometry (void *data, struct wl_output *proxy, int32_t x, int32_t y,
		 int32_t mm_width, int32_t mm_height, int32_t subpixel,
		 const char *make, const char *model, int32_t transform)
{
  struct wayland_monitor *output = data;
  output->x = x;
  output->y = y;
  output->mm_width = mm_width;
  output->mm_height = mm_height;
  output->transform = transform;
}

static void
output_mode (void *data, struct wl_output *proxy, uint32_t flags,
	     int32_t width, int32_t height, int32_t refresh)
{
  struct wayland_monitor *output = data;
  if (flags & WL_OUTPUT_MODE_CURRENT)
    {
      output->width = width;
      output->height = height;
      if (wl_output_get_version (proxy) < WL_OUTPUT_DONE_SINCE_VERSION)
        wayland_output_changed (output, false);
    }
}

static void
output_done (void *data, struct wl_output *proxy)
{
  struct wayland_monitor *output = data;
  wayland_output_changed (output, false);
}

static void
output_scale (void *data, struct wl_output *proxy, int32_t scale)
{
  struct wayland_monitor *output = data;
  output->scale = max (1, scale);
}

static void
output_name (void *data, struct wl_output *proxy, const char *name)
{
  struct wayland_monitor *output = data;
  xfree (output->label);
  output->label = xstrdup (name);
}

static void
output_description (void *data, struct wl_output *proxy,
                    const char *description)
{
}

static const struct wl_output_listener output_listener =
  {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
    .name = output_name,
    .description = output_description,
  };

static void
xdg_output_logical_position (void *data, struct zxdg_output_v1 *proxy,
			     int32_t x, int32_t y)
{
  struct wayland_monitor *output = data;
  output->logical_x = x;
  output->logical_y = y;
}

static void
xdg_output_logical_size (void *data, struct zxdg_output_v1 *proxy,
			 int32_t width, int32_t height)
{
  struct wayland_monitor *output = data;
  output->logical_width = width;
  output->logical_height = height;
}

/* Version 3 deprecates this event in favor of wl_output.done, which
   compositors then send after the xdg-output events.  */
static void
xdg_output_done (void *data, struct zxdg_output_v1 *proxy)
{
  struct wayland_monitor *output = data;
  if (zxdg_output_v1_get_version (proxy) < 3)
    wayland_output_changed (output, false);
}

static void
xdg_output_name (void *data, struct zxdg_output_v1 *proxy,
		 const char *name)
{
}

static void
xdg_output_description (void *data, struct zxdg_output_v1 *proxy,
			const char *description)
{
}

static bool output_rotated (const struct wayland_monitor *);

static const struct zxdg_output_v1_listener xdg_output_listener =
  {
    .logical_position = xdg_output_logical_position,
    .logical_size = xdg_output_logical_size,
    .done = xdg_output_done,
    .name = xdg_output_name,
    .description = xdg_output_description,
  };

/* Ask for OUTPUT's logical geometry, if the compositor offers
   xdg-output and OUTPUT has no xdg_output yet.  */

static void
wayland_get_xdg_output (struct wayland_private *priv,
			struct wayland_monitor *output)
{
  if (!priv->globals.output_manager || output->xdg_output)
    return;
  output->xdg_output = zxdg_output_manager_v1_get_xdg_output
    (priv->globals.output_manager, output->proxy);
  if (output->xdg_output)
    zxdg_output_v1_add_listener (output->xdg_output, &xdg_output_listener,
				 output);
}

static void
wayland_destroy_output (struct wayland_monitor *output)
{
  if (output->xdg_output)
    zxdg_output_v1_destroy (output->xdg_output);
  output->xdg_output = NULL;
}

/* Store OUTPUT's geometry in the compositor's logical coordinates.
   Without xdg-output, divide the mode by wl_output.scale, which is
   exact only at whole-number scales: compositors round fractional
   scales up there.  */

static void
wayland_output_geometry (struct wayland_monitor *output,
			 Emacs_Rectangle *geometry)
{
  if (output->logical_width > 0 && output->logical_height > 0)
    {
      *geometry = (Emacs_Rectangle) {output->logical_x, output->logical_y,
				     output->logical_width,
				     output->logical_height};
      return;
    }
  bool rotated = output_rotated (output);
  *geometry = (Emacs_Rectangle)
    {output->x, output->y,
     (rotated ? output->height : output->width) / output->scale,
     (rotated ? output->width : output->height) / output->scale};
}

static bool
output_rotated (const struct wayland_monitor *output)
{
  return (output->transform == WL_OUTPUT_TRANSFORM_90
	  || output->transform == WL_OUTPUT_TRANSFORM_270
	  || output->transform == WL_OUTPUT_TRANSFORM_FLIPPED_90
	  || output->transform == WL_OUTPUT_TRANSFORM_FLIPPED_270);
}

void
wayland_display_dimensions (struct wayland_display_info *dpyinfo,
			    struct wayland_display_dimensions *dimensions)
{
  int64_t left = INT64_MAX, top = INT64_MAX;
  int64_t right = INT64_MIN, bottom = INT64_MIN;
  *dimensions = (struct wayland_display_dimensions) {0};

  for (struct wayland_monitor *output = private_of (dpyinfo)->outputs;
       output; output = output->next)
    {
      bool rotated = output_rotated (output);
      Emacs_Rectangle geometry;
      if (output->width <= 0 || output->height <= 0)
	continue;
      wayland_output_geometry (output, &geometry);
      left = min (left, geometry.x);
      top = min (top, geometry.y);
      right = max (right, (int64_t) geometry.x + geometry.width);
      bottom = max (bottom, (int64_t) geometry.y + geometry.height);
      dimensions->mm_width = rotated ? output->mm_height : output->mm_width;
      dimensions->mm_height = rotated ? output->mm_width : output->mm_height;
      dimensions->screens++;
    }
  if (dimensions->screens)
    {
      dimensions->width = right - left;
      dimensions->height = bottom - top;
    }
  if (dimensions->screens != 1)
    dimensions->mm_width = dimensions->mm_height = 0;
}

/* The registry.  */

static void
registry_global (void *data, struct wl_registry *registry, uint32_t name,
		 const char *interface, uint32_t version)
{
  struct wayland_display_info *dpyinfo = data;
  struct wayland_private *priv = private_of (dpyinfo);

  if (!strcmp (interface, wl_output_interface.name))
    {
      struct wayland_monitor *output = xzalloc (sizeof *output);
      output->dpyinfo = dpyinfo;
      output->name = name;
      output->scale = 1;
      output->proxy = wl_registry_bind (registry, name, &wl_output_interface,
					min (version, 4));
      if (!output->proxy)
	{
	  xfree (output);
	  return;
	}
      wl_output_add_listener (output->proxy, &output_listener, output);
      output->next = priv->outputs;
      priv->outputs = output;
      wayland_get_xdg_output (priv, output);
      return;
    }

  for (int i = 0; i < N_GLOBALS; i++)
    {
      const struct global_spec *spec = &global_specs[i];
      void **slot;
      uint32_t bind_version;

      if (strcmp (interface, spec->interface->name))
	continue;
      slot = (void **) ((char *) &priv->globals + spec->offset);
      if (*slot)
	return;			/* v1 uses the first of each, e.g. seats.  */
      if (version < spec->min_version)
	{
	  priv->binding.too_old[i] = version;
	  return;
	}
      bind_version = version < spec->max_version ? version : spec->max_version;
      *slot = wl_registry_bind (registry, name, spec->interface, bind_version);
      priv->binding.bound[i] = bind_version;

      if (spec->interface == &xdg_wm_base_interface)
	xdg_wm_base_add_listener (*slot, &wm_base_listener, dpyinfo);
      else if (spec->interface == &wl_seat_interface)
	{
	  priv->globals.seat_name = name;
	  wl_seat_add_listener (*slot, &seat_listener, dpyinfo);
	}
      else if (spec->interface == &zxdg_output_manager_v1_interface)
	for (struct wayland_monitor *output = priv->outputs; output;
	     output = output->next)
	  wayland_get_xdg_output (priv, output);
      return;
    }
}

static void
registry_global_remove (void *data, struct wl_registry *registry,
			uint32_t name)
{
  struct wayland_display_info *dpyinfo = data;
  struct wayland_monitor **link = &private_of (dpyinfo)->outputs;
  for (struct wayland_monitor *output; (output = *link); link = &output->next)
    if (output->name == name)
      {
	*link = output->next;
	wayland_output_changed (output, true);
	wayland_destroy_output (output);
	if (wl_output_get_version (output->proxy) >= WL_OUTPUT_RELEASE_SINCE_VERSION)
	  wl_output_release (output->proxy);
	else
	  wl_output_destroy (output->proxy);
	xfree (output->label);
	xfree (output);
	return;
      }
}

static const struct wl_registry_listener registry_listener =
  {
    .global = registry_global,
    .global_remove = registry_global_remove,
  };

/* Terminal setup and teardown.  */

static void
wayland_forget_activation (struct wayland_private *priv)
{
  if (priv->activation_token)
    xdg_activation_token_v1_destroy (priv->activation_token);
  priv->activation_token = NULL;
  priv->activation_frame = NULL;
}

static void
wayland_destroy_proxies (struct wayland_display_info *dpyinfo)
{
  struct wayland_private *priv = private_of (dpyinfo);

  if (!priv)
    return;
  wayland_forget_activation (priv);
  if (priv->debug_source)
    wl_proxy_destroy ((struct wl_proxy *) priv->debug_source);
  if (priv->debug_device)
    wl_proxy_destroy ((struct wl_proxy *) priv->debug_device);
  if (priv->debug_region)
    wl_proxy_destroy ((struct wl_proxy *) priv->debug_region);
  wayland_selection_destroy (dpyinfo);
  wayland_im_destroy (dpyinfo);
  if (priv->seat.keyboard)
    wl_keyboard_destroy (priv->seat.keyboard);
  if (priv->seat.cursor_device)
    wp_cursor_shape_device_v1_destroy (priv->seat.cursor_device);
  if (priv->seat.pointer)
    wl_pointer_destroy (priv->seat.pointer);
  if (priv->seat.cursor_surface)
    wl_surface_destroy (priv->seat.cursor_surface);
  if (priv->seat.cursor_theme)
    wl_cursor_theme_destroy (priv->seat.cursor_theme);
  while (priv->outputs)
    {
      struct wayland_monitor *output = priv->outputs;
      priv->outputs = output->next;
      wayland_destroy_output (output);
      wl_output_destroy (output->proxy);
      xfree (output->label);
      xfree (output);
    }
  for (int i = 0; i < N_GLOBALS; i++)
    {
      void **slot = (void **) ((char *) &priv->globals
			       + global_specs[i].offset);
      if (*slot)
	wl_proxy_destroy (*slot);
    }
  if (dpyinfo->registry)
    wl_registry_destroy (dpyinfo->registry);
  if (priv->seat.state)
    xkb_state_unref (priv->seat.state);
  if (priv->seat.keymap)
    xkb_keymap_unref (priv->seat.keymap);
  if (priv->seat.xkb_context)
    xkb_context_unref (priv->seat.xkb_context);
  if (priv->seat.compose_state)
    xkb_compose_state_unref (priv->seat.compose_state);
  if (priv->seat.compose_table)
    xkb_compose_table_unref (priv->seat.compose_table);
}

static void
wayland_free_display_info (struct wayland_display_info *dpyinfo)
{
  struct wayland_display_info **tail;

  for (tail = &x_display_list; *tail; tail = &(*tail)->next)
    if (*tail == dpyinfo)
      {
	*tail = dpyinfo->next;
	break;
      }

  if (dpyinfo->display)
    {
      struct wayland_presentation_feedback *feedback, *next;
      wl_list_for_each_safe (feedback, next,
			     &private_of (dpyinfo)->presentation_pending, link)
	{
	  wp_presentation_feedback_destroy (feedback->proxy);
	  wl_list_remove (&feedback->link);
	  xfree (feedback);
	}
      wayland_destroy_proxies (dpyinfo);
      wl_display_disconnect (dpyinfo->display);
    }
  if (dpyinfo->service_fd >= 0)
    emacs_close (dpyinfo->service_fd);
  if (dpyinfo->write_fd >= 0)
    emacs_close (dpyinfo->write_fd);
  if (dpyinfo->timer_fd >= 0)
    emacs_close (dpyinfo->timer_fd);
  if (dpyinfo->repeat_fd >= 0)
    emacs_close (dpyinfo->repeat_fd);
  if (dpyinfo->globals)
    xfree (private_of (dpyinfo)->trace);
  xfree (dpyinfo->globals);
  xfree (dpyinfo);
}

static void
wayland_delete_terminal (struct terminal *terminal)
{
  struct wayland_display_info *dpyinfo = terminal->display_info.wayland;

  /* Protect against recursive calls: delete_terminal deletes our frames,
     and deleting the last one calls this hook again.  */
  if (!terminal->name || !dpyinfo)
    return;

  block_input ();
  if (dpyinfo->state == WAYLAND_DISPLAY_LIVE)
    {
      /* Deleted while live, e.g. by `x-close-connection'.  */
      delete_keyboard_wait_descriptor (dpyinfo->connection);
      delete_read_fd (dpyinfo->repeat_fd);
      delete_read_fd (dpyinfo->service_fd);
      if (dpyinfo->output_state == WAYLAND_OUTPUT_PENDING)
	delete_write_fd (dpyinfo->write_fd);
    }
  delete_read_fd (dpyinfo->timer_fd);

  /* Delete the frames and unlink the terminal while the display info
     still exists, as X and PGTK do.  */
  delete_terminal (terminal);
  terminal->display_info.wayland = NULL;
  wayland_free_display_info (dpyinfo);
  unblock_input ();
}

/* Wayland has no resource database; frame parameters and Custom take
   its place.  */

static const char *
wayland_get_string_resource (void *rdb, const char *name, const char *class)
{
  return NULL;
}

static struct terminal *
wayland_create_terminal (struct wayland_display_info *dpyinfo,
			 Lisp_Object display_name)
{
  struct terminal *terminal
    = create_terminal (output_wayland, &wayland_redisplay_interface);

  terminal->display_info.wayland = dpyinfo;
  dpyinfo->terminal = terminal;
  terminal->name = xlispstrdup (display_name);
  terminal->read_socket_hook = wayland_read_socket;
  terminal->delete_terminal_hook = wayland_delete_terminal;
  terminal->get_string_resource_hook = wayland_get_string_resource;
  wayland_set_frame_hooks (terminal);
  gui_init_fringe (&wayland_redisplay_interface);
  /* No menu_show_hook, popup_dialog_hook or scroll-bar hooks.  */

  terminal->kboard = allocate_kboard (Qwayland);
  /* Don't let the initial kboard remain current longer than
     necessary.  */
  if (current_kboard == initial_kboard)
    current_kboard = terminal->kboard;
  terminal->kboard->reference_count++;
  return terminal;
}

/* Describe the required globals that are missing or too old, or
   return NULL if all are usable.  */

static char *
wayland_missing_globals (struct wayland_display_info *dpyinfo)
{
  struct wayland_private *priv = private_of (dpyinfo);
  static char message[512];
  int len = 0;

  for (int i = 0; i < N_GLOBALS; i++)
    if (global_specs[i].required && !priv->binding.bound[i])
      len += snprintf (message + len, sizeof message - len,
		       "%s%s %s", len ? ", " : "",
		       global_specs[i].interface->name,
		       priv->binding.too_old[i] ? "(too old)" : "(missing)");
  return len ? message : NULL;
}

/* Find an open Wayland connection with NAME, or return NULL.  */

struct wayland_display_info *
wayland_display_by_name (Lisp_Object name)
{
  for (struct wayland_display_info *dpyinfo = x_display_list;
       dpyinfo; dpyinfo = dpyinfo->next)
    if (dpyinfo->state == WAYLAND_DISPLAY_LIVE
	&& !NILP (Fstring_equal (name, XCAR (dpyinfo->name_list_element))))
      return dpyinfo;
  return NULL;
}

/* Connect to the Wayland display DISPLAY_NAME ("" means the one
   WAYLAND_DISPLAY or WAYLAND_SOCKET names) and create its terminal.
   On failure, signal an error.  */

struct wayland_display_info *
wayland_term_init (Lisp_Object display_name, char *resource_name)
{
  static bool baud_rate_initialized;
  struct wayland_display_info *dpyinfo;
  struct wl_display *display;
  const char *name;
  char *missing;
  int fd;
  Lisp_Object color_file, color_map;

  if (!SCHARS (display_name) && !getenv ("WAYLAND_SOCKET"))
    {
      /* libwayland consumes WAYLAND_SOCKET on the first connection.  */
      dpyinfo = wayland_display_by_name (display_name);
      if (dpyinfo)
	return dpyinfo;
      const char *env_name = getenv ("WAYLAND_DISPLAY");
      display_name = build_string (env_name && *env_name ? env_name
						       : "wayland-0");
    }
  dpyinfo = wayland_display_by_name (display_name);
  if (dpyinfo)
    return dpyinfo;

  /* Color names; read before connecting, since reading can signal.  */
  color_file = Fexpand_file_name (build_string ("rgb.txt"), Vdata_directory);
  color_map = Fx_load_color_file (color_file);

  name = SCHARS (display_name) ? SSDATA (display_name) : NULL;
  block_input ();
  display = wl_display_connect (name);
  if (!display)
    {
      unblock_input ();
      error ("Could not connect to Wayland display %s",
	     name ? name : "(default)");
    }

  dpyinfo = xzalloc (sizeof *dpyinfo);
  dpyinfo->display = display;
  dpyinfo->globals = xzalloc (sizeof (struct wayland_private));
  wl_list_init (&private_of (dpyinfo)->presentation_pending);
  dpyinfo->service_fd = dpyinfo->write_fd = dpyinfo->timer_fd = -1;
  dpyinfo->repeat_fd = -1;
  dpyinfo->name_list_element = Fcons (display_name, Qnil);
  dpyinfo->color_map = color_map;
  dpyinfo->reference_count = 0;
  dpyinfo->n_planes = 24;
  dpyinfo->color_p = 1;
  dpyinfo->icon_bitmap_id = -1;
  dpyinfo->smallest_font_height = 1;
  dpyinfo->smallest_char_width = 1;
  /* Frames take this and multiply it by their scale.  */
  dpyinfo->resx = dpyinfo->resy = 96.0 * wayland_text_scale;

  fd = wl_display_get_fd (display);
  dpyinfo->connection = fd;
  fcntl (fd, F_SETFL, fcntl (fd, F_GETFL) | O_NONBLOCK);
  wl_display_set_max_buffer_size (display, WAYLAND_MAX_BUFFER_SIZE);

  private_of (dpyinfo)->seat.xkb_context
    = xkb_context_new (XKB_CONTEXT_NO_FLAGS);
  wayland_make_compose_state (&private_of (dpyinfo)->seat);

  dpyinfo->registry = wl_display_get_registry (display);
  wl_registry_add_listener (dpyinfo->registry, &registry_listener, dpyinfo);

  /* The first roundtrip delivers the globals, the second the seat's
     capabilities, and the third the keymap of the keyboard the second
     created.  */
  if (!wayland_roundtrip (dpyinfo, WAYLAND_STARTUP_TIMEOUT_MS)
      || !wayland_roundtrip (dpyinfo, WAYLAND_STARTUP_TIMEOUT_MS)
      || !wayland_roundtrip (dpyinfo, WAYLAND_STARTUP_TIMEOUT_MS))
    {
      wayland_free_display_info (dpyinfo);
      unblock_input ();
      error ("Wayland display %s did not answer", name ? name : "(default)");
    }

  missing = wayland_missing_globals (dpyinfo);
  if (missing)
    {
      AUTO_STRING (format, "Wayland display lacks required interfaces: %s");
      Lisp_Object detail = build_string (missing);
      wayland_free_display_info (dpyinfo);
      unblock_input ();
      xsignal1 (Qerror, CALLN (Fformat, format, detail));
    }

  /* Duplicates of the socket: one for waits that leave keyboard
     descriptors out, one carrying write interest alone.  */
  dpyinfo->service_fd = fcntl (fd, F_DUPFD_CLOEXEC, 0);
  if (dpyinfo->service_fd >= 0)
    dpyinfo->write_fd = fcntl (fd, F_DUPFD_CLOEXEC, 0);
  if (dpyinfo->write_fd >= 0)
    dpyinfo->timer_fd = timerfd_create (CLOCK_MONOTONIC,
					TFD_NONBLOCK | TFD_CLOEXEC);
  if (dpyinfo->timer_fd >= 0)
    dpyinfo->repeat_fd = timerfd_create (CLOCK_MONOTONIC,
					 TFD_NONBLOCK | TFD_CLOEXEC);
  if (dpyinfo->service_fd < 0 || dpyinfo->write_fd < 0
      || dpyinfo->timer_fd < 0 || dpyinfo->repeat_fd < 0
      || fd >= FD_SETSIZE
      || dpyinfo->service_fd >= FD_SETSIZE || dpyinfo->write_fd >= FD_SETSIZE
      || dpyinfo->timer_fd >= FD_SETSIZE || dpyinfo->repeat_fd >= FD_SETSIZE)
    {
      int saved_errno = dpyinfo->repeat_fd < 0 ? errno : EMFILE;
      wayland_free_display_info (dpyinfo);
      unblock_input ();
      error ("Could not set up the Wayland connection: %s",
	     emacs_strerror (saved_errno));
    }

  /* Set the fast terminal default once, as on X and PGTK.  */
  if (!baud_rate_initialized)
    {
      baud_rate = 19200;
      baud_rate_initialized = true;
    }

  wayland_create_terminal (dpyinfo, display_name);
  wayland_im_create (dpyinfo, private_of (dpyinfo)->globals.text_input_manager,
		     private_of (dpyinfo)->globals.seat);
  struct wayland_globals *globals = &private_of (dpyinfo)->globals;
  wayland_selection_create (dpyinfo, globals->data_device_manager,
			    globals->primary_selection_manager, globals->seat);
  dpyinfo->next = x_display_list;
  x_display_list = dpyinfo;

  add_keyboard_wait_descriptor (fd);
  add_non_keyboard_read_fd (dpyinfo->repeat_fd, wayland_repeat_ready,
			    dpyinfo);
  /* Input on the socket raises SIGIO, so C-g interrupts busy Lisp at
     the next safe point, as on X and PGTK.  */
#ifdef F_SETOWN
  fcntl (fd, F_SETOWN, getpid ());
#endif
  if (interrupt_input)
    init_sigio (fd);
  add_non_keyboard_read_fd (dpyinfo->service_fd, wayland_service_ready,
			    dpyinfo);
  add_non_keyboard_read_fd (dpyinfo->timer_fd, wayland_timer_ready, dpyinfo);
  unblock_input ();
  return dpyinfo;
}

/* Frames: surfaces, buffers and presentation.

   Emacs draws straight into a free shared-memory buffer, after
   bringing it up to date from the buffer on screen, and commits it
   with only the current surface damage.  While the compositor holds
   every buffer, drawing is discarded, and the frame is redrawn in
   full once a buffer is released.  */

enum
{
  WAYLAND_BUFFER_COUNT = 3,
  WAYLAND_DAMAGE_HISTORY = 8,

  /* Damage is exact up to this many rectangles and their bounding box
     beyond that.  A region's cost grows with its rectangles: each
     union walks them, and each becomes a damage request and a copy.
     Ordinary edits stay far below the limit.  */
  WAYLAND_DAMAGE_RECTS = 64,
  WAYLAND_SCALE_UNIT = 120,
  WAYLAND_MAX_CANVAS_SIZE = 32767
};

struct wayland_buffer
{
  struct wl_buffer *buffer;
  struct wayland_pool *pool;
  unsigned char *data;
  /* The buffer's memory as a Cairo image, which Emacs draws into while
     the buffer is the surface's target.  */
  cairo_surface_t *image;
  cairo_t *cr;
  uint64_t commit, attached_ns;
  bool busy;
};

struct wayland_pool
{
  struct wayland_pool *next;
  struct wayland_surface_state *surface;
  unsigned char *data;
  size_t size;
  int width, height, stride, held, scale;
  bool retired, charged;
  struct wayland_buffer buffers[WAYLAND_BUFFER_COUNT];
};

struct wayland_output_member
{
  struct wayland_output_member *next;
  struct wayland_monitor *output;
};

struct wayland_surface_state
{
  struct frame *frame;
  struct wl_surface *surface;
  struct wl_subsurface *subsurface;
  struct wayland_surface_state *first_child, *next_sibling;
  bool children_pending, position_pending;
  int requested_x, requested_y;
  int position_x, position_y;
  int child_width, child_height, child_size_scale;
  struct xdg_surface *xdg_surface;
  struct xdg_toplevel *toplevel;
  struct zxdg_toplevel_decoration_v1 *decoration;
  struct wl_callback *frame_callback;
  struct wp_viewport *viewport;
  struct wp_fractional_scale_v1 *fractional;
  struct wayland_output_member *outputs;
  int scale, applied_scale, committed_scale;
  /* The buffer size and viewport destination of the last content
     commit; the destination is 0x0 when buffer scale is used.  */
  int committed_width, committed_height;
  int committed_destination_width, committed_destination_height;
  int preferred_fractional, preferred_integer;
  int logical_width, logical_height;
  bool changing_dpi;

  /* TARGET is the free buffer Emacs draws into, of size CANVAS_WIDTH
     x CANVAS_HEIGHT.  It is NULL after a commit until something
     draws again.  FRONT is the buffer of the last content commit.
     TARGET still lacks STALE, the changes since it was last
     committed, which come from FRONT (or the background if FRONT has
     another size) before anything draws into it.

     UNDRAWN means that drawing found no buffer to take and is being
     discarded, so no buffer has the image the glyph matrices
     describe.  Nothing is committed then.  Once the frame can have a
     buffer again, it is redrawn in full (see wayland_update_begin).
     CR is where drawing goes: TARGET's context, or a sink while
     UNDRAWN.  */
  cairo_t *cr;
  int canvas_width, canvas_height;
  struct wayland_buffer *target, *front;
  pixman_region32_t stale;
  bool undrawn;
  struct wayland_pool *pool;
  struct wayland_pool *retired_pools;
  struct wayland_pool *reserved_pool;
  bool pool_budget_blocked;
  bool fail_pool;

  /* After a full clear, the suffix of each row starting at
     CLEAR_ROW_START[Y] still has CLEAR_COLOR.  Stop tracking when
     this redraw ends or its image changes.  */
  int *clear_row_start;
  int clear_row_capacity;
  unsigned long clear_color;
  bool track_clear;

  /* The latest configure's serial, and the size of the toplevel
     configure that preceded it (0 leaves that axis to the client).  */
  bool configure_pending;
  uint32_t configure_serial;
  int pending_width, pending_height;
  uint32_t pending_states, configured_states;
  /* The toplevel size of the latest configure Emacs has handled.  */
  int configured_width, configured_height;

  /* The size Emacs chose for the frame itself, used for each axis a
     configure leaves to the client.  Recorded at the first map and by
     set_window_size; 0 until then.  */
  int client_width, client_height;

  /* Mapping state: the bufferless initial commit was sent, a configure
     arrived since, and a buffer is attached.  */
  bool initial_commit_done;
  bool configured;
  bool mapped;
  bool want_visible;
  /* A configure that arrived before the first image was acked with
     BACKGROUND_BUFFER, which stays attached until that image replaces
     it (see wayland_present_background).  */
  bool background_attached;
  struct wl_buffer *background_buffer;
  /* Background buffers attached, so dumps number every buffer the
     compositor received as mockcomp's snapshots do.  */
  unsigned long background_attaches;
  /* `show_hourglass' was called and `hide_hourglass' not yet.  */
  bool hourglass;

  /* Presentation state (see wayland_present).  */
  bool commit_wanted;
  pixman_region32_t damage;
  pixman_region32_t history[WAYLAND_DAMAGE_HISTORY];
  int cr_depth;
  int update_depth;
  bool incomplete;

  char *title;
  char *activation_token;
  char *dump_directory;
  unsigned long content_commits;
};

static struct wayland_surface_state *
surface_state (struct frame *f)
{
  return FRAME_X_OUTPUT (f)->surface;
}

struct frame *
wayland_frame_root (struct frame *f)
{
  struct frame *parent;
  while ((parent = FRAME_PARENT_FRAME (f)))
    f = parent;
  return f;
}

void
wayland_frame_root_position (struct frame *f, int *x, int *y)
{
  int64_t left = 0, top = 0;
  for (; FRAME_PARENT_FRAME (f); f = FRAME_PARENT_FRAME (f))
    {
      struct wayland_surface_state *ss = surface_state (f);
      left += ss->position_x;
      top += ss->position_y;
    }
  *x = clip_to_bounds (INT_MIN, left, INT_MAX);
  *y = clip_to_bounds (INT_MIN, top, INT_MAX);
}

static bool
wayland_ancestor_p (struct frame *ancestor, struct frame *f)
{
  for (; f; f = FRAME_PARENT_FRAME (f))
    if (f == ancestor)
      return true;
  return false;
}

/* Return logical keyboard focus to an ancestor outside F's subtree.  */
static void
wayland_drop_subtree_focus (struct frame *f)
{
  struct wayland_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);
  if (!wayland_ancestor_p (f, dpyinfo->x_focus_frame))
    return;
  struct frame *parent = FRAME_PARENT_FRAME (f);
  while (parent && (!FRAME_VISIBLE_P (parent) || FRAME_NO_ACCEPT_FOCUS (parent)))
    parent = FRAME_PARENT_FRAME (parent);
  wayland_new_focus (dpyinfo, parent);
}

void
wayland_check_parent (struct wayland_display_info *dpyinfo,
		      struct frame *f, Lisp_Object parent)
{
  if (NILP (parent))
    return;
  if (!FRAMEP (parent) || !FRAME_LIVE_P (XFRAME (parent))
      || !FRAME_WAYLAND_P (XFRAME (parent))
      || FRAME_DISPLAY_INFO (XFRAME (parent)) != dpyinfo
      || !surface_state (XFRAME (parent)))
    error ("Parent must be a live frame on the same Wayland display");
  if (!private_of (dpyinfo)->globals.subcompositor)
    error ("The Wayland compositor does not support child frames");
  for (struct frame *p = XFRAME (parent); p; p = FRAME_PARENT_FRAME (p))
    if (p == f)
      error ("A frame cannot become its own ancestor");
}

/* The frame of DPYINFO whose surface is SURFACE, or null.  */

struct frame *
wayland_surface_frame (struct wayland_display_info *dpyinfo,
		       struct wl_surface *surface)
{
  Lisp_Object tail, frame;

  if (!surface)
    return NULL;
  FOR_EACH_FRAME (tail, frame)
    {
      struct frame *f = XFRAME (frame);
      if (FRAME_WAYLAND_P (f) && FRAME_DISPLAY_INFO (f) == dpyinfo
	  && surface_state (f) && surface_state (f)->surface == surface)
	return f;
    }
  return NULL;
}

/* The pointer.

   Listeners record the pointer's state, queue click events and set the
   cursor, which needs only requests.  Mouse highlighting, help echo
   and tool bar and tab bar buttons draw, and listeners can run inside
   redisplay, so that work waits for the backend timer, whose callback
   runs from the wait loop.  */

/* Pointer cursors: the cursor-shape-v1 shape, and the cursor theme's
   name for it when that protocol is missing.  */

struct wayland_cursor
{
  uint32_t shape;
  const char *name;
};

enum wayland_cursor_index
  {
    WAYLAND_CURSOR_DEFAULT, WAYLAND_CURSOR_TEXT, WAYLAND_CURSOR_HAND,
    WAYLAND_CURSOR_WAIT, WAYLAND_CURSOR_EW, WAYLAND_CURSOR_NS,
    WAYLAND_CURSOR_W, WAYLAND_CURSOR_NW, WAYLAND_CURSOR_N,
    WAYLAND_CURSOR_NE, WAYLAND_CURSOR_E, WAYLAND_CURSOR_SE,
    WAYLAND_CURSOR_S, WAYLAND_CURSOR_SW,
  };

static struct wayland_cursor wayland_cursors[] =
  {
    [WAYLAND_CURSOR_DEFAULT]
      = { WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT, "left_ptr" },
    [WAYLAND_CURSOR_TEXT] = { WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT, "xterm" },
    [WAYLAND_CURSOR_HAND]
      = { WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER, "hand2" },
    [WAYLAND_CURSOR_WAIT] = { WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_WAIT, "watch" },
    [WAYLAND_CURSOR_EW]
      = { WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE, "sb_h_double_arrow" },
    [WAYLAND_CURSOR_NS]
      = { WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE, "sb_v_double_arrow" },
    [WAYLAND_CURSOR_W]
      = { WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE, "left_side" },
    [WAYLAND_CURSOR_NW]
      = { WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE, "top_left_corner" },
    [WAYLAND_CURSOR_N]
      = { WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE, "top_side" },
    [WAYLAND_CURSOR_NE]
      = { WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE, "top_right_corner" },
    [WAYLAND_CURSOR_E]
      = { WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE, "right_side" },
    [WAYLAND_CURSOR_SE]
      = { WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE, "bottom_right_corner" },
    [WAYLAND_CURSOR_S]
      = { WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE, "bottom_side" },
    [WAYLAND_CURSOR_SW]
      = { WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE, "bottom_left_corner" },
  };

static void
wayland_init_frame_cursors (struct frame *f)
{
  struct wayland_output *output = FRAME_X_OUTPUT (f);

  output->text_cursor = &wayland_cursors[WAYLAND_CURSOR_TEXT];
  output->nontext_cursor = &wayland_cursors[WAYLAND_CURSOR_DEFAULT];
  output->modeline_cursor = &wayland_cursors[WAYLAND_CURSOR_DEFAULT];
  output->hand_cursor = &wayland_cursors[WAYLAND_CURSOR_HAND];
  output->hourglass_cursor = &wayland_cursors[WAYLAND_CURSOR_WAIT];
  output->horizontal_drag_cursor = &wayland_cursors[WAYLAND_CURSOR_EW];
  output->vertical_drag_cursor = &wayland_cursors[WAYLAND_CURSOR_NS];
  output->left_edge_cursor = &wayland_cursors[WAYLAND_CURSOR_W];
  output->top_left_corner_cursor = &wayland_cursors[WAYLAND_CURSOR_NW];
  output->top_edge_cursor = &wayland_cursors[WAYLAND_CURSOR_N];
  output->top_right_corner_cursor = &wayland_cursors[WAYLAND_CURSOR_NE];
  output->right_edge_cursor = &wayland_cursors[WAYLAND_CURSOR_E];
  output->bottom_right_corner_cursor = &wayland_cursors[WAYLAND_CURSOR_SE];
  output->bottom_edge_cursor = &wayland_cursors[WAYLAND_CURSOR_S];
  output->bottom_left_corner_cursor = &wayland_cursors[WAYLAND_CURSOR_SW];
  output->current_cursor = output->nontext_cursor;
}

static struct frame *
wayland_pointer_frame (struct wayland_display_info *dpyinfo)
{
  return wayland_surface_frame (dpyinfo,
				private_of (dpyinfo)->seat.pointer_focus);
}

/* The last logical pixel of a child buffer can include padding.  */
static bool
wayland_pointer_inside (struct frame *f, int x, int y)
{
  return !FRAME_PARENT_FRAME (f)
    || (0 <= x && x < FRAME_PIXEL_WIDTH (f)
	&& 0 <= y && y < FRAME_PIXEL_HEIGHT (f));
}

/* Show CURSOR for the pointer, which is over frame F.  */

static void
wayland_set_pointer_cursor (struct wayland_display_info *dpyinfo,
			    struct frame *f, Emacs_Cursor cursor)
{
  struct wayland_private *priv = private_of (dpyinfo);
  struct wayland_seat *seat = &priv->seat;
  uint32_t serial = seat->pointer_enter_serial;

  if (!seat->pointer || !cursor || dpyinfo->state == WAYLAND_DISPLAY_DEAD)
    return;
  /* Each frame hides the pointer only over itself, as an X window
     does, since Emacs shows it again only through the frame under the
     pointer.  */
  if (f->pointer_invisible)
    {
      wl_pointer_set_cursor (seat->pointer, serial, NULL, 0, 0);
      return;
    }
  if (seat->cursor_device)
    {
      wp_cursor_shape_device_v1_set_shape (seat->cursor_device, serial,
					   cursor->shape);
      return;
    }

  /* Themed images, at the frame's scale rounded up, which the
     compositor scales down rather than up.  */
  const char *size_env = getenv ("XCURSOR_SIZE");
  int base = size_env ? atoi (size_env) : 0;
  int scale = ((wayland_frame_scale (f) + WAYLAND_SCALE_UNIT - 1)
	       / WAYLAND_SCALE_UNIT);
  int size = (base > 0 ? base : 24) * scale;

  if (!seat->cursor_theme || seat->cursor_theme_size != size)
    {
      if (seat->cursor_theme)
	wl_cursor_theme_destroy (seat->cursor_theme);
      seat->cursor_theme = wl_cursor_theme_load (getenv ("XCURSOR_THEME"),
						 size, priv->globals.shm);
      seat->cursor_theme_size = size;
    }
  if (!seat->cursor_theme)
    return;

  struct wl_cursor *themed
    = wl_cursor_theme_get_cursor (seat->cursor_theme, cursor->name);
  if (!themed)
    themed = wl_cursor_theme_get_cursor (seat->cursor_theme, "left_ptr");
  if (!themed || !themed->image_count)
    return;
  struct wl_cursor_image *image = themed->images[0];
  struct wl_buffer *buffer = wl_cursor_image_get_buffer (image);
  if (!buffer)
    return;
  /* A theme without the scaled size gives a smaller image; the buffer
     scale must divide its size.  */
  if (image->width % scale || image->height % scale)
    scale = 1;
  if (!seat->cursor_surface)
    seat->cursor_surface
      = wl_compositor_create_surface (priv->globals.compositor);
  if (!seat->cursor_surface)
    return;
  wl_surface_set_buffer_scale (seat->cursor_surface, scale);
  wl_surface_attach (seat->cursor_surface, buffer, 0, 0);
  wl_surface_damage_buffer (seat->cursor_surface, 0, 0, image->width,
			    image->height);
  wl_surface_commit (seat->cursor_surface);
  wl_pointer_set_cursor (seat->pointer, serial, seat->cursor_surface,
			 image->hotspot_x / scale, image->hotspot_y / scale);
}

/* The cursor frame F shows now: the hourglass while it is busy.  */

static Emacs_Cursor
wayland_frame_cursor (struct frame *f)
{
  struct wayland_output *output = FRAME_X_OUTPUT (f);

  if (surface_state (f) && surface_state (f)->hourglass)
    return output->hourglass_cursor;
  return output->current_cursor ? output->current_cursor
    : output->nontext_cursor;
}

static void
wayland_update_pointer_cursor (struct frame *f)
{
  struct wayland_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);

  if (wayland_pointer_frame (dpyinfo) == f)
    {
      wayland_set_pointer_cursor (dpyinfo, f, wayland_frame_cursor (f));
      /* Blanking and the hourglass change the cursor outside redisplay,
	 and nothing else may flush before Emacs next waits.  */
      wayland_flush_now (dpyinfo);
    }
}

static void
wayland_schedule_pointer_work (struct wayland_display_info *dpyinfo)
{
  private_of (dpyinfo)->pointer_work = true;
  wayland_arm_timer (dpyinfo, 1);
}

/* Record a pointer position on frame F, in logical surface
   coordinates, as Emacs's physical pixels.  */

static void
wayland_note_pointer (struct wayland_display_info *dpyinfo, struct frame *f,
		      wl_fixed_t sx, wl_fixed_t sy)
{
  int scale = surface_state (f)->applied_scale;

  dpyinfo->last_mouse_motion_frame = f;
  dpyinfo->last_mouse_motion_x
    = floor (wl_fixed_to_double (sx) * scale / WAYLAND_SCALE_UNIT);
  dpyinfo->last_mouse_motion_y
    = floor (wl_fixed_to_double (sy) * scale / WAYLAND_SCALE_UNIT);
  f->mouse_moved = true;
  private_of (dpyinfo)->pointer_moved = true;
  private_of (dpyinfo)->pointer_grabbed = dpyinfo->grabbed;
  wayland_schedule_pointer_work (dpyinfo);
}

static void
pointer_enter (void *data, struct wl_pointer *pointer, uint32_t serial,
	       struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy)
{
  struct wayland_display_info *dpyinfo = data;
  struct wayland_seat *seat = &private_of (dpyinfo)->seat;
  struct frame *f;

  seat->pointer_focus = surface;
  seat->pointer_enter_serial = serial;
  if (!(f = wayland_surface_frame (dpyinfo, surface)))
    return;
  wayland_note_pointer (dpyinfo, f, sx, sy);
  wayland_set_pointer_cursor (dpyinfo, f, wayland_frame_cursor (f));
}

static void
pointer_leave (void *data, struct wl_pointer *pointer, uint32_t serial,
	       struct wl_surface *surface)
{
  struct wayland_display_info *dpyinfo = data;
  struct wayland_private *priv = private_of (dpyinfo);
  struct frame *f = wayland_surface_frame (dpyinfo, surface);

  priv->seat.pointer_focus = NULL;
  if (f)
    {
      priv->pointer_left = f;
      wayland_schedule_pointer_work (dpyinfo);
    }
}

static void
pointer_motion (void *data, struct wl_pointer *pointer, uint32_t time,
		wl_fixed_t sx, wl_fixed_t sy)
{
  struct wayland_display_info *dpyinfo = data;
  struct frame *f = wayland_pointer_frame (dpyinfo);

  if (!f)
    return;
  dpyinfo->last_mouse_movement_time = time;
  wayland_note_pointer (dpyinfo, f, sx, sy);
}

/* Evdev button codes, and the X button numbers Emacs names them by,
   minus 1: left, middle, right, then back and forward as 8 and 9.  */

enum
  {
    WAYLAND_BTN_LEFT = 0x110, WAYLAND_BTN_RIGHT = 0x111,
    WAYLAND_BTN_MIDDLE = 0x112, WAYLAND_BTN_SIDE = 0x113,
    WAYLAND_BTN_EXTRA = 0x114, WAYLAND_BTN_FORWARD = 0x115,
    WAYLAND_BTN_BACK = 0x116,
  };

static int
wayland_button_code (uint32_t button)
{
  switch (button)
    {
    case WAYLAND_BTN_LEFT: return 0;
    case WAYLAND_BTN_MIDDLE: return 1;
    case WAYLAND_BTN_RIGHT: return 2;
    case WAYLAND_BTN_SIDE: case WAYLAND_BTN_BACK: return 7;
    case WAYLAND_BTN_EXTRA: case WAYLAND_BTN_FORWARD: return 8;
    default: return -1;
    }
}

static void
wayland_queue_click (struct wayland_display_info *dpyinfo, struct frame *f,
		     int code, bool down, int modifiers, int x, int y,
		     Time time, Lisp_Object arg)
{
  union buffered_input_event event;

  EVENT_INIT (event.ie);
  event.ie.kind = MOUSE_CLICK_EVENT;
  event.ie.code = code;
  event.ie.modifiers = modifiers | (down ? down_modifier : up_modifier);
  event.ie.timestamp = time;
  XSETINT (event.ie.x, x);
  XSETINT (event.ie.y, y);
  XSETFRAME (event.ie.frame_or_window, f);
  event.ie.arg = arg;
  wayland_queue_event (dpyinfo, &event);
}

/* Whether (X, Y) on frame F is on the window WINDOW, the frame's tool
   bar or tab bar window.  */

static bool
wayland_on_bar_window (struct frame *f, Lisp_Object window, int x, int y)
{
  return (WINDOWP (window) && WINDOW_TOTAL_LINES (XWINDOW (window))
	  && EQ (window_from_coordinates (f, x, y, 0, true, true, true),
		 window));
}

static void
pointer_button (void *data, struct wl_pointer *pointer, uint32_t serial,
		uint32_t time, uint32_t button, uint32_t state)
{
  struct wayland_display_info *dpyinfo = data;
  struct wayland_private *priv = private_of (dpyinfo);
  int code = wayland_button_code (button);
  bool down = state == WL_POINTER_BUTTON_STATE_PRESSED;
  int x = dpyinfo->last_mouse_motion_x, y = dpyinfo->last_mouse_motion_y;
  struct frame *f;

  if (down)
    {
      priv->seat.last_button_serial = serial;
      priv->seat.last_input_serial = serial;
    }
  if (code < 0)
    return;
  f = (gui_mouse_grabbed (dpyinfo) ? dpyinfo->last_mouse_frame
       : wayland_pointer_frame (dpyinfo));
  dpyinfo->last_mouse_glyph_frame = NULL;

  if (f && FRAME_LIVE_P (f))
    {
      if (!gui_mouse_grabbed (dpyinfo) && !wayland_pointer_inside (f, x, y))
	return;
      if (down)
	wayland_focus_frame (f, true);
      int modifiers = (wayland_emacs_modifiers (&priv->seat)
		       | wayland_extra_modifiers ());
      bool tab_bar = wayland_on_bar_window (f, f->tab_bar_window, x, y);
      bool tool_bar = (code < 3
		       && wayland_on_bar_window (f, f->tool_bar_window, x, y));

      if ((tab_bar || tool_bar)
	  && priv->bar_click_count < WAYLAND_MAX_BAR_CLICKS)
	{
	  /* These buttons draw their relief: leave them to the timer.  */
	  priv->bar_clicks[priv->bar_click_count++]
	    = (struct wayland_bar_click) { f, x, y, modifiers, down,
					   tool_bar };
	  wayland_schedule_pointer_work (dpyinfo);
	}
      else if (!tab_bar && !tool_bar)
	wayland_queue_click (dpyinfo, f, code, down, modifiers, x, y, time,
			     Qnil);
      /* Ignore earlier motion: later mouse-movement events reflect only
	 motion after this button event.  */
      f->mouse_moved = false;
    }

  if (down)
    {
      if (!dpyinfo->grabbed && f && FRAME_LIVE_P (f))
	wayland_frame_root_position (f, &priv->press_left, &priv->press_top);
      dpyinfo->grabbed |= 1 << code;
      dpyinfo->last_mouse_frame = f;
    }
  else
    dpyinfo->grabbed &= ~(1 << code);
}

/* Scrolling.  Axis events collect until wl_pointer.frame, which
   versions below 5 lack; those dispatch each axis event at once.
   Wheels give one Emacs wheel event per notch: from axis_value120,
   whose high-resolution steps accumulate to 120, from axis_discrete
   below version 8, and from the value's sign below version 5.
   Fingers and other continuous sources scroll as X does under
   Xwayland: 10 compositor pixels make a scroll unit, whatever the
   scale, and each event carries a pixel delta of its units times the
   2/3 power of the height of the window under the pointer, times
   `x-scroll-event-delta-factor', as in xterm.c's XI_Motion handling.
   While `mwheel-coalesce-scroll-events' is non-nil, an event waits for
   a whole unit and carries no line count.  Lifting the fingers
   (axis_stop) gives a touch-end event and drops the remainder, as
   Xwayland does.  Scrolling down or right gives wheel-down and
   wheel-right, from a wheel and a touchpad alike, and each notch is
   one unit, as on X.  */

/* Compositor pixels per X scroll unit, as Xwayland's
   DEFAULT_AXIS_STEP_DISTANCE.  */
#define WAYLAND_AXIS_STEP_DISTANCE 10.0

/* The devices scroll events are attributed to, or nil until used.
   Wayland identifies no input devices, but an axis frame says what
   scrolled; `wayland-device-class' maps these names to classes.  */
static Lisp_Object wayland_axis_devices;

/* Return the device of the scroll events of SEAT's current axis
   frame, for `last-event-device', or t if the frame does not say.  */

static Lisp_Object
wayland_axis_device (struct wayland_seat *seat)
{
  static const char *const names[] =
    { "Wayland Wheel Scrolling", "Wayland Finger Scrolling",
      "Wayland Continuous Scrolling" };
  int index;

  if (!seat->axis.source_known)
    return Qt;
  switch (seat->axis.source)
    {
    case WL_POINTER_AXIS_SOURCE_WHEEL:
    case WL_POINTER_AXIS_SOURCE_WHEEL_TILT:
      index = 0;
      break;
    case WL_POINTER_AXIS_SOURCE_FINGER:
      index = 1;
      break;
    case WL_POINTER_AXIS_SOURCE_CONTINUOUS:
      index = 2;
      break;
    default:
      return Qt;
    }
  if (NILP (AREF (wayland_axis_devices, index)))
    ASET (wayland_axis_devices, index, build_string (names[index]));
  return AREF (wayland_axis_devices, index);
}

static void
wayland_queue_wheel (struct wayland_display_info *dpyinfo, struct frame *f,
		     bool horizontal, bool positive, int modifiers,
		     uint32_t time, Lisp_Object arg, Lisp_Object device)
{
  union buffered_input_event event;

  EVENT_INIT (event.ie);
  event.ie.device = device;
  event.ie.kind = horizontal ? HORIZ_WHEEL_EVENT : WHEEL_EVENT;
  event.ie.modifiers = modifiers | (positive ? down_modifier : up_modifier);
  event.ie.timestamp = time;
  XSETINT (event.ie.x, dpyinfo->last_mouse_motion_x);
  XSETINT (event.ie.y, dpyinfo->last_mouse_motion_y);
  XSETFRAME (event.ie.frame_or_window, f);
  event.ie.arg = arg;
  wayland_queue_event (dpyinfo, &event);
}

/* Return the pixels one X scroll unit moves the window of F under
   the pointer.  */

static double
wayland_scroll_unit (struct wayland_display_info *dpyinfo, struct frame *f)
{
  Lisp_Object window
    = window_from_coordinates (f, dpyinfo->last_mouse_motion_x,
			       dpyinfo->last_mouse_motion_y, NULL,
			       false, false, false);
  double unit = pow (WINDOWP (window) ? XWINDOW (window)->pixel_height
		     : FRAME_PIXEL_HEIGHT (f), 2.0 / 3.0);

  if (NUMBERP (Vx_scroll_event_delta_factor))
    unit *= XFLOATINT (Vx_scroll_event_delta_factor);
  return unit;
}

/* Return the pixel delta of a scroll event that moves UNITS
   vertically or, if HORIZONTAL, horizontally.  */

static Lisp_Object
wayland_scroll_delta (struct wayland_display_info *dpyinfo,
		      struct frame *f, bool horizontal, double units)
{
  Lisp_Object delta
    = make_float (units ? -units * wayland_scroll_unit (dpyinfo, f) : 0);

  return (horizontal ? list3 (Qnil, delta, make_float (0))
	  : list3 (Qnil, make_float (0), delta));
}

/* Queue the scroll event for continuous travel of DX and DY
   compositor pixels, if it completes one.  */

static void
wayland_queue_pixel_scroll (struct wayland_display_info *dpyinfo,
			    struct frame *f, double dx, double dy,
			    int modifiers, uint32_t time, Lisp_Object device)
{
  struct wayland_seat *seat = &private_of (dpyinfo)->seat;
  double *carry = seat->pixel_carry;
  double pixels[2] = { dy, dx };
  double total[2] = { 0, 0 };
  bool found = false;

  for (int axis = 0; axis < 2; axis++)
    {
      if (!pixels[axis])
	continue;
      if (signbit (pixels[axis]) != signbit (carry[axis]))
	carry[axis] = 0;
      carry[axis] += pixels[axis];
      if (mwheel_coalesce_scroll_events
	  && fabs (carry[axis]) < WAYLAND_AXIS_STEP_DISTANCE)
	continue;
      total[axis] = carry[axis] / WAYLAND_AXIS_STEP_DISTANCE;
      carry[axis] = 0;
      found = true;
    }
  if (!found)
    return;

  double unit = wayland_scroll_unit (dpyinfo, f);
  bool vertical = fabs (total[0]) >= fabs (total[1]);
  wayland_queue_wheel (dpyinfo, f, !vertical,
		       (vertical ? total[0] : total[1]) > 0, modifiers, time,
		       list3 (Qnil,
			      make_float (total[1] ? -total[1] * unit : 0),
			      make_float (total[0] ? -total[0] * unit : 0)),
		       device);
}

/* Turn the axis events collected in SEAT->axis into Emacs events.  */

static void
wayland_dispatch_axis (struct wayland_display_info *dpyinfo)
{
  struct wayland_seat *seat = &private_of (dpyinfo)->seat;
  struct frame *f = (gui_mouse_grabbed (dpyinfo) ? dpyinfo->last_mouse_frame
		     : wayland_pointer_frame (dpyinfo));
  bool continuous;
  int modifiers;
  Lisp_Object device;

  if (!seat->axis.any)
    return;
  if (!f || !FRAME_LIVE_P (f) || !surface_state (f)
      || !wayland_pointer_inside (f, dpyinfo->last_mouse_motion_x,
				  dpyinfo->last_mouse_motion_y))
    goto done;
  modifiers = wayland_emacs_modifiers (seat) | wayland_extra_modifiers ();
  device = wayland_axis_device (seat);
  /* Only continuous sources send axis_stop, whose frame need not
     repeat the source.  */
  continuous = (seat->axis.source_known
		? (seat->axis.source != WL_POINTER_AXIS_SOURCE_WHEEL
		   && seat->axis.source != WL_POINTER_AXIS_SOURCE_WHEEL_TILT)
		: seat->axis.stop[0] || seat->axis.stop[1]);

  if (continuous)
    {
      double dy = seat->axis.value[0];
      double dx = seat->axis.value[1];

      if (dx || dy)
	wayland_queue_pixel_scroll (dpyinfo, f, dx, dy, modifiers,
				    seat->axis.time, device);
      if (seat->axis.stop[0] || seat->axis.stop[1])
	{
	  union buffered_input_event event;

	  EVENT_INIT (event.ie);
	  event.ie.device = device;
	  event.ie.kind = TOUCH_END_EVENT;
	  event.ie.timestamp = seat->axis.time;
	  XSETINT (event.ie.x, dpyinfo->last_mouse_motion_x);
	  XSETINT (event.ie.y, dpyinfo->last_mouse_motion_y);
	  XSETFRAME (event.ie.frame_or_window, f);
	  wayland_queue_event (dpyinfo, &event);
	  seat->pixel_carry[0] = seat->pixel_carry[1] = 0;
	}
      goto done;
    }

  for (int axis = 0; axis < 2; axis++)
    {
      int notches = 0;

      if (seat->axis.has_value120[axis])
	{
	  int32_t *carry = &seat->value120_carry[axis];
	  int32_t value120 = seat->axis.value120[axis];

	  /* A change of direction drops the partial step.  */
	  if ((value120 > 0) != (*carry > 0))
	    *carry = 0;
	  *carry += value120;
	  notches = *carry / 120;
	  *carry -= notches * 120;
	}
      else if (seat->axis.has_discrete[axis])
	notches = seat->axis.discrete[axis];
      else if (seat->axis.value[axis])
	notches = seat->axis.value[axis] > 0 ? 1 : -1;

      /* Each notch carries one unit's pixel delta, as on X.  */
      for (int i = 0; i < eabs (notches); i++)
	wayland_queue_wheel (dpyinfo, f, axis == 1, notches > 0, modifiers,
			     seat->axis.time,
			     wayland_scroll_delta (dpyinfo, f, axis == 1,
						   notches > 0 ? 1 : -1),
			     device);
    }

 done:
  memset (&seat->axis, 0, sizeof seat->axis);
}

static int
wayland_axis_index (uint32_t axis)
{
  return axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL;
}

static void
pointer_axis (void *data, struct wl_pointer *pointer, uint32_t time,
	      uint32_t axis, wl_fixed_t value)
{
  struct wayland_display_info *dpyinfo = data;
  struct wayland_seat *seat = &private_of (dpyinfo)->seat;

  seat->axis.any = true;
  seat->axis.time = time;
  seat->axis.value[wayland_axis_index (axis)] += wl_fixed_to_double (value);
  if (wl_pointer_get_version (pointer) < WL_POINTER_FRAME_SINCE_VERSION)
    wayland_dispatch_axis (dpyinfo);
}

static void
pointer_frame (void *data, struct wl_pointer *pointer)
{
  wayland_dispatch_axis (data);
}

static void
pointer_axis_source (void *data, struct wl_pointer *pointer,
		     uint32_t source)
{
  struct wayland_seat *seat = &private_of ((struct wayland_display_info *)
					   data)->seat;

  seat->axis.source_known = true;
  seat->axis.source = source;
}

static void
pointer_axis_stop (void *data, struct wl_pointer *pointer, uint32_t time,
		   uint32_t axis)
{
  struct wayland_seat *seat = &private_of ((struct wayland_display_info *)
					   data)->seat;

  seat->axis.any = true;
  seat->axis.time = time;
  seat->axis.stop[wayland_axis_index (axis)] = true;
}

static void
pointer_axis_discrete (void *data, struct wl_pointer *pointer,
		       uint32_t axis, int32_t discrete)
{
  struct wayland_seat *seat = &private_of ((struct wayland_display_info *)
					   data)->seat;
  int index = wayland_axis_index (axis);

  seat->axis.any = true;
  seat->axis.has_discrete[index] = true;
  seat->axis.discrete[index] += discrete;
}

static void
pointer_axis_value120 (void *data, struct wl_pointer *pointer,
		       uint32_t axis, int32_t value120)
{
  struct wayland_seat *seat = &private_of ((struct wayland_display_info *)
					   data)->seat;
  int index = wayland_axis_index (axis);

  seat->axis.any = true;
  seat->axis.has_value120[index] = true;
  seat->axis.value120[index] += value120;
}

static const struct wl_pointer_listener pointer_listener =
  {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
    .axis_value120 = pointer_axis_value120,
  };

/* The pointer work listeners leave: clearing the highlight of a frame
   the pointer left, the highlight and help echo where it moved, and
   tool bar and tab bar buttons.  This follows pgtkterm.c's
   leave_notify_event, motion_notify_event and button_event.  */

static bool any_help_event_p;

static void
wayland_pointer_left (struct wayland_display_info *dpyinfo, struct frame *f)
{
  Mouse_HLInfo *hlinfo = MOUSE_HL_INFO (f);
  Lisp_Object frame;

  if (f == hlinfo->mouse_face_mouse_frame)
    {
      /* Outside the frame, the pointer is on none of its text.  */
      clear_mouse_face (hlinfo);
      hlinfo->mouse_face_mouse_frame = 0;
    }
  if (any_help_event_p)
    {
      XSETFRAME (frame, f);
      help_echo_string = Qnil;
      gen_help_event (Qnil, frame, Qnil, Qnil, 0);
      any_help_event_p = false;
    }
  dpyinfo->last_mouse_glyph_frame = NULL;
}

static void
wayland_pointer_moved (struct wayland_display_info *dpyinfo)
{
  struct frame *f = (gui_mouse_grabbed (dpyinfo)
		     ? dpyinfo->last_mouse_frame
		     : dpyinfo->last_mouse_motion_frame);
  int x = dpyinfo->last_mouse_motion_x, y = dpyinfo->last_mouse_motion_y;
  XRectangle *r = &dpyinfo->last_mouse_glyph;
  Mouse_HLInfo *hlinfo;

  if (!f || !FRAME_LIVE_P (f) || !FRAME_WAYLAND_P (f)
      || f != dpyinfo->last_mouse_motion_frame)
    return;
  if (!wayland_pointer_inside (f, x, y))
    {
      wayland_pointer_left (dpyinfo, f);
      return;
    }
  hlinfo = MOUSE_HL_INFO (f);
  previous_help_echo_string = help_echo_string;
  help_echo_string = Qnil;

  if (hlinfo->mouse_face_hidden)
    {
      hlinfo->mouse_face_hidden = false;
      clear_mouse_face (hlinfo);
    }

  /* `mouse-autoselect-window', but never out of the minibuffer.  */
  if (!NILP (Vmouse_autoselect_window)
      && !MINI_WINDOW_P (XWINDOW (selected_window))
      && (f == XFRAME (selected_frame) || !NILP (focus_follows_mouse)))
    {
      Lisp_Object window
	= window_from_coordinates (f, x, y, 0, false, false, false);

      if (WINDOWP (window) && !EQ (window, last_mouse_window)
	  && !EQ (window, selected_window))
	{
	  union buffered_input_event event;
	  EVENT_INIT (event.ie);
	  event.ie.kind = SELECT_WINDOW_EVENT;
	  event.ie.frame_or_window = window;
	  wayland_queue_event (dpyinfo, &event);
	}
      last_mouse_window = window;
    }

  /* Only a move to another glyph changes the highlight.  */
  if (f != dpyinfo->last_mouse_glyph_frame
      || x < r->x || x >= r->x + (int) r->width
      || y < r->y || y >= r->y + (int) r->height)
    {
      dpyinfo->last_mouse_scroll_bar = NULL;
      note_mouse_highlight (f, x, y);
      remember_mouse_glyph (f, x, y, r);
      dpyinfo->last_mouse_glyph_frame = f;
    }
  else
    help_echo_string = previous_help_echo_string;

  if (!NILP (help_echo_string) || !NILP (previous_help_echo_string))
    {
      Lisp_Object frame;
      XSETFRAME (frame, f);
      any_help_event_p = true;
      gen_help_event (help_echo_string, frame, help_echo_window,
		      help_echo_object, help_echo_pos);
    }
}

static void
wayland_pointer_work (struct wayland_display_info *dpyinfo)
{
  struct wayland_private *priv = private_of (dpyinfo);
  int count = priv->bar_click_count;
  struct wayland_bar_click clicks[WAYLAND_MAX_BAR_CLICKS];
  Lisp_Object tail, frame;

  priv->pointer_work = false;
  memcpy (clicks, priv->bar_clicks, count * sizeof *clicks);
  priv->bar_click_count = 0;

  block_input ();
  if (priv->pointer_left)
    {
      struct frame *f = priv->pointer_left;
      priv->pointer_left = NULL;
      if (FRAME_LIVE_P (f))
	wayland_pointer_left (dpyinfo, f);
    }
  if (priv->pointer_moved)
    {
      /* Handle the motion as of when it happened, as X, which handles
	 each event in turn, does: with the buttons then held.  */
      int grabbed = dpyinfo->grabbed;
      priv->pointer_moved = false;
      dpyinfo->grabbed = priv->pointer_grabbed;
      if (wayland_pointer_frame (dpyinfo) || gui_mouse_grabbed (dpyinfo))
	wayland_pointer_moved (dpyinfo);
      dpyinfo->grabbed = grabbed;
    }
  for (int i = 0; i < count; i++)
    {
      struct frame *f = clicks[i].frame;

      if (!FRAME_LIVE_P (f))
	continue;
      if (clicks[i].tool_bar)
	handle_tool_bar_click (f, clicks[i].x, clicks[i].y, clicks[i].down,
			       clicks[i].modifiers);
      else
	{
	  Lisp_Object arg = handle_tab_bar_click (f, clicks[i].x, clicks[i].y,
						  clicks[i].down,
						  clicks[i].modifiers);
	  if (!NILP (arg))
	    wayland_queue_click (dpyinfo, f, 0, clicks[i].down,
				 clicks[i].modifiers, clicks[i].x,
				 clicks[i].y,
				 dpyinfo->last_mouse_movement_time, arg);
	}
    }
  unblock_input ();

  /* What that drew goes out with the next commit.  */
  FOR_EACH_FRAME (tail, frame)
    {
      struct frame *f = XFRAME (frame);
      if (FRAME_WAYLAND_P (f) && FRAME_DISPLAY_INFO (f) == dpyinfo
	  && surface_state (f)
	  && pixman_region32_not_empty (&surface_state (f)->damage))
	surface_state (f)->commit_wanted = true;
    }
}

/* Forget frame F's pointer state; it is being deleted.  */

static void
wayland_forget_pointer_frame (struct wayland_display_info *dpyinfo,
			      struct frame *f)
{
  struct wayland_private *priv = private_of (dpyinfo);
  int kept = 0;

  if (surface_state (f) && priv->seat.pointer_focus
      == surface_state (f)->surface)
    priv->seat.pointer_focus = NULL;
  if (dpyinfo->last_mouse_frame == f)
    dpyinfo->last_mouse_frame = NULL;
  if (dpyinfo->last_mouse_motion_frame == f)
    dpyinfo->last_mouse_motion_frame = NULL;
  if (dpyinfo->last_mouse_glyph_frame == f)
    dpyinfo->last_mouse_glyph_frame = NULL;
  if (priv->pointer_left == f)
    priv->pointer_left = NULL;
  for (int i = 0; i < priv->bar_click_count; i++)
    if (priv->bar_clicks[i].frame != f)
      priv->bar_clicks[kept++] = priv->bar_clicks[i];
  priv->bar_click_count = kept;
}

static struct wayland_globals *
globals_of (struct frame *f)
{
  return &private_of (FRAME_DISPLAY_INFO (f))->globals;
}

static bool
frame_display_dead (struct frame *f)
{
  return FRAME_DISPLAY_INFO (f)->state == WAYLAND_DISPLAY_DEAD;
}

/* Wayland supplies output membership without intersection areas.
   Use the lowest registry name among entered outputs for the monitor
   API.  Scale fallback uses the largest entered scale independently.  */
static struct wayland_monitor *
wayland_frame_monitor (struct wayland_surface_state *ss)
{
  ss = surface_state (wayland_frame_root (ss->frame));
  struct wayland_monitor *best = NULL;
  for (struct wayland_output_member *m = ss->outputs; m; m = m->next)
    if (m->output->width > 0 && (!best || m->output->name < best->name))
      best = m->output;
  if (best)
    return best;
  for (struct wayland_monitor *o
         = private_of (FRAME_DISPLAY_INFO (ss->frame))->outputs;
       o; o = o->next)
    if (o->width > 0 && (!best || o->name < best->name))
      best = o;
  return best;
}

DEFUN ("wayland-display-monitor-attributes-list",
       Fwayland_display_monitor_attributes_list,
       Swayland_display_monitor_attributes_list, 0, 1, 0,
       doc: /* Return the monitor attributes of Wayland DISPLAY.  */)
  (Lisp_Object display)
{
  struct wayland_display_info *dpyinfo = check_wayland_display_info (display);
  struct wayland_monitor *outputs = private_of (dpyinfo)->outputs;
  int count = 0, primary = 0;
  for (struct wayland_monitor *o = outputs; o; o = o->next)
    count++;
  if (!count)
    return Qnil;

  USE_SAFE_ALLOCA;
  struct MonitorInfo *infos = SAFE_ALLOCA (count * sizeof *infos);
  Lisp_Object frames = Fmake_vector (make_fixnum (count), Qnil);
  uint32_t first = UINT32_MAX;
  int i = 0;
  for (struct wayland_monitor *o = outputs; o; o = o->next, i++)
    {
      bool rotated = output_rotated (o);
      Emacs_Rectangle geometry;
      wayland_output_geometry (o, &geometry);
      infos[i] = (struct MonitorInfo)
        {.geom = geometry, .work = geometry, .name = o->label,
         .mm_width = rotated ? o->mm_height : o->mm_width,
         .mm_height = rotated ? o->mm_width : o->mm_height};
      if (geometry.width && o->name < first)
        {
          first = o->name;
          primary = i;
        }
    }
  Lisp_Object frame, tail;
  FOR_EACH_FRAME (tail, frame)
    {
      struct frame *f = XFRAME (frame);
      if (FRAME_WAYLAND_P (f) && FRAME_DISPLAY_INFO (f) == dpyinfo
          && surface_state (f))
        {
          struct wayland_monitor *monitor = wayland_frame_monitor
            (surface_state (f));
          i = 0;
          for (struct wayland_monitor *o = outputs; o; o = o->next, i++)
            if (o == monitor)
              ASET (frames, i, Fcons (frame, AREF (frames, i)));
        }
    }
  Lisp_Object result = make_monitor_attribute_list (infos, count, primary,
                                                    frames, "Wayland");
  SAFE_FREE ();
  return result;
}

static void
wayland_note_scale (struct wayland_surface_state *ss)
{
  if (FRAME_PARENT_FRAME (ss->frame))
    return;
  int scale = ss->preferred_fractional;
  if (!scale)
    scale = ss->preferred_integer;
  if (!scale)
    {
      scale = WAYLAND_SCALE_UNIT;
      for (struct wayland_output_member *m = ss->outputs; m; m = m->next)
        scale = max (scale, min (m->output->scale, WAYLAND_MAX_CANVAS_SIZE)
                            * WAYLAND_SCALE_UNIT);
    }
  if (scale == ss->scale)
    return;
  ss->scale = scale;

  struct wayland_display_info *dpyinfo = FRAME_DISPLAY_INFO (ss->frame);
  struct wayland_private *priv = private_of (dpyinfo);
  Lisp_Object tail, frame;
  FOR_EACH_FRAME (tail, frame)
    {
      struct frame *f = XFRAME (frame);
      if (FRAME_WAYLAND_P (f) && FRAME_DISPLAY_INFO (f) == dpyinfo
	  && f != ss->frame && wayland_frame_root (f) == ss->frame)
	surface_state (f)->scale = scale;
    }
  priv->scale_pending = true;
  if (!priv->scale_queued)
    wayland_arm_timer (dpyinfo, 1);
}

static void
wayland_leave_output (struct wayland_surface_state *ss,
                      struct wayland_monitor *output)
{
  struct wayland_output_member **link = &ss->outputs;
  while (*link && (*link)->output != output)
    link = &(*link)->next;
  if (*link)
    {
      struct wayland_output_member *member = *link;
      *link = member->next;
      xfree (member);
    }
}

static void
wayland_output_changed (struct wayland_monitor *output, bool removed)
{
  struct wayland_display_info *dpyinfo = output->dpyinfo;
  wayland_update_pool_budget (dpyinfo);
  if (!dpyinfo->terminal)
    return;
  Lisp_Object frame, tail;
  FOR_EACH_FRAME (tail, frame)
    {
      struct frame *f = XFRAME (frame);
      if (!FRAME_WAYLAND_P (f) || FRAME_DISPLAY_INFO (f) != dpyinfo
          || !surface_state (f))
        continue;
      if (removed)
        wayland_leave_output (surface_state (f), output);
      wayland_note_scale (surface_state (f));
    }

  struct input_event event;
  EVENT_INIT (event);
  event.kind = MONITORS_CHANGED_EVENT;
  XSETTERMINAL (event.arg, dpyinfo->terminal);
  kbd_buffer_store_event (&event);
}

static void
surface_enter (void *data, struct wl_surface *proxy, struct wl_output *output)
{
  struct wayland_surface_state *ss = data;
  struct wayland_monitor *monitor = wl_output_get_user_data (output);
  for (struct wayland_output_member *m = ss->outputs; m; m = m->next)
    if (m->output == monitor)
      return;
  struct wayland_output_member *member = xmalloc (sizeof *member);
  member->output = monitor;
  member->next = ss->outputs;
  ss->outputs = member;
  wayland_note_scale (ss);
}

static void
surface_leave (void *data, struct wl_surface *proxy, struct wl_output *output)
{
  struct wayland_surface_state *ss = data;
  wayland_leave_output (ss, wl_output_get_user_data (output));
  wayland_note_scale (ss);
}

static void
surface_preferred_scale (void *data, struct wl_surface *proxy, int32_t scale)
{
  struct wayland_surface_state *ss = data;
  ss->preferred_integer = clip_to_bounds (1, scale, WAYLAND_MAX_CANVAS_SIZE)
    * WAYLAND_SCALE_UNIT;
  wayland_note_scale (ss);
}

static void
surface_preferred_transform (void *data, struct wl_surface *proxy,
                             uint32_t transform)
{
}

static const struct wl_surface_listener surface_listener =
{
  .enter = surface_enter,
  .leave = surface_leave,
  .preferred_buffer_scale = surface_preferred_scale,
  .preferred_buffer_transform = surface_preferred_transform,
};

static void
fractional_preferred_scale (void *data, struct wp_fractional_scale_v1 *proxy,
                            uint32_t scale)
{
  struct wayland_surface_state *ss = data;
  ss->preferred_fractional = min (scale, WAYLAND_MAX_CANVAS_SIZE);
  wayland_note_scale (ss);
}

static const struct wp_fractional_scale_v1_listener fractional_listener =
{
  .preferred_scale = fractional_preferred_scale,
};

static int
wayland_physical_size (int logical, int scale)
{
  int64_t pixels = ((int64_t) logical * scale + WAYLAND_SCALE_UNIT / 2)
    / WAYLAND_SCALE_UNIT;
  return clip_to_bounds (1, pixels, WAYLAND_MAX_CANVAS_SIZE);
}

static int
wayland_logical_size (int physical, int scale)
{
  return max (1, ((int64_t) physical * WAYLAND_SCALE_UNIT + scale / 2)
                 / scale);
}

static int
wayland_scale_offset (int offset, int old_scale, int new_scale)
{
  int64_t value = (int64_t) offset * new_scale;
  value += offset < 0 ? -old_scale / 2 : old_scale / 2;
  return clip_to_bounds (INT_MIN, value / old_scale, INT_MAX);
}

/* F's scale in 120ths, the unit of fractional-scale-v1.  */

/* The desktop's text scaling factor, which multiplies the resolution
   fonts are sized at, as GTK's does (see `wayland--set-desktop-settings'
   in waylandfns.c).  */

double wayland_text_scale = 1.0;

/* The resolution F's fonts are sized at: 96 dpi times the text scaling
   factor and the frame's scale.  */

static double
wayland_frame_resolution (struct frame *f)
{
  return (96.0 * wayland_text_scale * wayland_frame_scale (f)
	  / WAYLAND_SCALE_UNIT);
}

int
wayland_frame_scale (struct frame *f)
{
  return surface_state (f) ? surface_state (f)->applied_scale
    : WAYLAND_SCALE_UNIT;
}

/* The whole-number factor for fixed-size elements: fringe bitmaps
   and widths, the bar cursor, underlines and border widths.  The
   frame's scale rounds to the nearest whole number, halves up, so that
   at 150% they match the 2x drawing toolkits downscale.  */

int
wayland_fixed_scale (struct frame *f)
{
  return max (1, ((wayland_frame_scale (f) + WAYLAND_SCALE_UNIT / 2)
		  / WAYLAND_SCALE_UNIT));
}

static void
wayland_resize_surface (struct wayland_surface_state *ss)
{
  struct frame *f = ss->frame;
  int width = ss->subsurface ? ss->child_width
    : wayland_physical_size (ss->logical_width, ss->applied_scale);
  int height = ss->subsurface ? ss->child_height
    : wayland_physical_size (ss->logical_height, ss->applied_scale);
  if (width != FRAME_PIXEL_WIDTH (f) || height != FRAME_PIXEL_HEIGHT (f)
      || f->new_size_p)
    change_frame_size (f, width, height, false, true, false);
}

static void
wayland_end_dpi_change (Lisp_Object frame)
{
  struct frame *f = XFRAME (frame);
  if (FRAME_LIVE_P (f) && surface_state (f))
    {
      surface_state (f)->changing_dpi = false;
      int unit = FRAME_COLUMN_WIDTH (f);
      f->fringe_cols = (FRAME_LEFT_FRINGE_WIDTH (f)
		       + FRAME_RIGHT_FRINGE_WIDTH (f) + unit - 1) / unit;
      wayland_resize_surface (surface_state (f));
      SET_FRAME_GARBAGED (f);
    }
}

DEFUN ("wayland--apply-scale", Fwayland__apply_scale, Swayland__apply_scale,
       1, 1, 0,
       doc: /* Apply pending scale changes on TERMINAL at a safe point.  */)
  (Lisp_Object terminal)
{
  struct terminal *t = TERMINALP (terminal) ? XTERMINAL (terminal) : NULL;
  if (!t || !t->name || t->type != output_wayland)
    return Qnil;
  struct wayland_display_info *dpyinfo = t->display_info.wayland;
  if (dpyinfo->state == WAYLAND_DISPLAY_DEAD)
    return Qnil;
  struct wayland_private *priv = private_of (dpyinfo);
  priv->scale_queued = false;
  if (!wayland_finalize_allowed (dpyinfo))
    {
      wayland_arm_timer (dpyinfo, WAYLAND_FINALIZE_RETRY_NS);
      return Qnil;
    }

  priv->scale_pending = false;
  Lisp_Object frame, tail;
  FOR_EACH_FRAME (tail, frame)
    {
      struct frame *f = XFRAME (frame);
      if (!FRAME_LIVE_P (f) || !FRAME_WAYLAND_P (f)
          || FRAME_DISPLAY_INFO (f) != dpyinfo || !surface_state (f))
        continue;
      struct wayland_surface_state *ss = surface_state (f);
      if (ss->scale == ss->applied_scale)
        continue;
      if (ss->subsurface)
	{
	  ss->child_width = clip_to_bounds
	    (1, wayland_scale_offset (ss->client_width, ss->child_size_scale,
				      ss->scale), WAYLAND_MAX_CANVAS_SIZE);
	  ss->child_height = clip_to_bounds
	    (1, wayland_scale_offset (ss->client_height, ss->child_size_scale,
				      ss->scale), WAYLAND_MAX_CANVAS_SIZE);
	  f->left_pos = wayland_scale_offset (ss->requested_x,
					     WAYLAND_SCALE_UNIT, ss->scale);
	  f->top_pos = wayland_scale_offset (ss->requested_y,
					    WAYLAND_SCALE_UNIT, ss->scale);
	}
      ss->applied_scale = ss->scale;
      FRAME_X_OUTPUT (f)->resx = FRAME_X_OUTPUT (f)->resy
        = wayland_frame_resolution (f);
      ss->changing_dpi = true;
      wayland_scale_frame_elements (f);
      specpdl_ref count = SPECPDL_INDEX ();
      record_unwind_protect (wayland_end_dpi_change, frame);
      Freconsider_frame_fonts (frame);
      unbind_to (count, Qnil);
    }
  return Qnil;
}

/* Size fonts on every Wayland display for the text scaling FACTOR.  */

void
wayland_set_text_scale (double factor)
{
  if (factor == wayland_text_scale)
    return;
  wayland_text_scale = factor;
  for (struct wayland_display_info *dpyinfo = x_display_list; dpyinfo;
       dpyinfo = dpyinfo->next)
    dpyinfo->resx = dpyinfo->resy = 96.0 * factor;

  Lisp_Object frame, tail;
  FOR_EACH_FRAME (tail, frame)
    {
      struct frame *f = XFRAME (frame);
      struct wayland_surface_state *ss = surface_state (f);

      if (!FRAME_WAYLAND_P (f) || !FRAME_LIVE_P (f) || frame_display_dead (f))
	continue;
      FRAME_X_OUTPUT (f)->resx = FRAME_X_OUTPUT (f)->resy
	= wayland_frame_resolution (f);
      specpdl_ref count = SPECPDL_INDEX ();
      if (ss)
	{
	  ss->changing_dpi = true;
	  record_unwind_protect (wayland_end_dpi_change, frame);
	}
      Freconsider_frame_fonts (frame);
      unbind_to (count, Qnil);
    }
}

/* Pool admission is per display.  When more room becomes available,
   schedule redisplay for frames whose replacements were blocked.  */
static void
wayland_retry_pool_waiters (struct wayland_display_info *dpyinfo)
{
  if (!dpyinfo->terminal || dpyinfo->state != WAYLAND_DISPLAY_LIVE)
    return;

  Lisp_Object tail, frame;
  FOR_EACH_FRAME (tail, frame)
    {
      struct frame *f = XFRAME (frame);
      if (FRAME_LIVE_P (f) && FRAME_WAYLAND_P (f)
	  && FRAME_DISPLAY_INFO (f) == dpyinfo && surface_state (f)
	  && surface_state (f)->pool_budget_blocked)
	SET_FRAME_GARBAGED (f);
    }
}

/* Charge full pool allocations, including unused slots when one
   buffer keeps the mapping alive.  Output removal can reduce the
   budget while old pools drain.  The next admission checks that
   limit before charging a pool.  */
static void
wayland_update_pool_budget (struct wayland_display_info *dpyinfo)
{
  struct wayland_private *priv = private_of (dpyinfo);
  uint64_t old_budget = priv->pool_budget;
  uint64_t largest = 0;
  for (struct wayland_monitor *o = priv->outputs; o; o = o->next)
    if (o->width > 0 && o->height > 0)
      {
        uint64_t pixels = (uint64_t) o->width * o->height;
        largest = max (largest,
                       min (pixels, INT32_MAX / (4 * WAYLAND_BUFFER_COUNT)));
      }
  priv->pool_budget = largest * 4 * WAYLAND_BUFFER_COUNT * 3;

  /* With no usable output, keep room for two full-HD pools so resizes
     are not throttled to the compositor's release cadence.  */
  uint64_t minimum = (uint64_t) 1920 * 1080 * 4 * WAYLAND_BUFFER_COUNT * 2;
  priv->pool_budget = max (priv->pool_budget, minimum);
  if (priv->pool_budget > old_budget)
    wayland_retry_pool_waiters (dpyinfo);
}

static void
wayland_free_pool (struct wayland_pool *pool)
{
  for (int i = 0; i < WAYLAND_BUFFER_COUNT; i++)
    {
      struct wayland_buffer *buffer = &pool->buffers[i];
      if (buffer->buffer)
	wl_buffer_destroy (buffer->buffer);
      if (buffer->cr)
	cairo_destroy (buffer->cr);
      if (buffer->image)
	cairo_surface_destroy (buffer->image);
    }
  if (pool->data)
    munmap (pool->data, pool->size);
  xfree (pool);
}

static void
wayland_destroy_pool (struct wayland_pool *pool)
{
  struct wayland_surface_state *ss = pool->surface;
  struct wayland_display_info *dpyinfo = FRAME_DISPLAY_INFO (ss->frame);
  struct wayland_private *priv = private_of (dpyinfo);
  priv->pool_bytes -= pool->size;
  if (ss->front && ss->front->pool == pool)
    ss->front = NULL;
  if (ss->target && ss->target->pool == pool)
    {
      ss->track_clear = false;
      ss->target = NULL;
      ss->cr = NULL;
    }
  if (pool->retired)
    {
      struct wayland_pool **link = &ss->retired_pools;
      while (*link != pool)
        link = &(*link)->next;
      *link = pool->next;
      priv->retired_bytes -= pool->size;
      if (ss->reserved_pool == pool)
        ss->reserved_pool = NULL;
      if (pool->charged)
	{
	  priv->charged_pool_bytes -= pool->size;
	  wayland_retry_pool_waiters (dpyinfo);
	}
    }
  wayland_free_pool (pool);
}

static void
buffer_release (void *data, struct wl_buffer *wl_buffer)
{
  struct wayland_buffer *buffer = data;
  struct wayland_pool *pool = buffer->pool;
  eassert (buffer->busy);
  /* Held counts decide when a pool's mapping is freed, so a duplicate
     release from a faulty compositor must not lower them again.  */
  if (!buffer->busy)
    return;
  buffer->busy = false;
  struct wayland_private *priv
    = private_of (FRAME_DISPLAY_INFO (pool->surface->frame));
  priv->releases++;
  priv->last_release_ns = monotonic_ns () - buffer->attached_ns;
  /* A frame whose drawing was discarded may have a buffer to take
     now.  Have redisplay find out, even if nothing else changes.  */
  if (pool->surface->undrawn)
    SET_FRAME_GARBAGED (pool->surface->frame);
  if (--pool->held == 0 && pool->retired)
    wayland_destroy_pool (pool);
}

static const struct wl_buffer_listener buffer_listener =
  { .release = buffer_release };

static struct wayland_pool *
wayland_new_pool (struct frame *f)
{
  struct wayland_surface_state *ss = surface_state (f);
  int stride = cairo_format_stride_for_width (CAIRO_FORMAT_RGB24,
					      ss->canvas_width);
  size_t buffer_size, size;
  if (ss->fail_pool || stride <= 0
      || ckd_mul (&buffer_size, (size_t) stride, (size_t) ss->canvas_height)
      || ckd_mul (&size, buffer_size, WAYLAND_BUFFER_COUNT)
      || size > INT32_MAX)
    return NULL;

  struct wayland_pool *pool = xzalloc (sizeof *pool);
  pool->surface = ss;
  pool->size = size;
  pool->width = ss->canvas_width;
  pool->height = ss->canvas_height;
  pool->scale = ss->applied_scale;
  pool->stride = stride;
  int fd = memfd_create ("emacs-wayland-pool", MFD_CLOEXEC);
  if (fd < 0)
    goto fail;
  if (posix_fallocate (fd, 0, size) != 0)
    {
      emacs_close (fd);
      goto fail;
    }
  void *data = mmap (NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (data == MAP_FAILED)
    {
      emacs_close (fd);
      goto fail;
    }
  pool->data = data;
  struct wl_shm_pool *proxy = wl_shm_create_pool (globals_of (f)->shm, fd,
                                                  size);
  emacs_close (fd);
  if (!proxy)
    goto fail;
  for (int i = 0; i < WAYLAND_BUFFER_COUNT; i++)
    {
      struct wayland_buffer *buffer = &pool->buffers[i];
      size_t offset = i * buffer_size;
      buffer->buffer = wl_shm_pool_create_buffer (proxy, offset, pool->width,
                                                  pool->height, stride,
                                                  WL_SHM_FORMAT_XRGB8888);
      if (!buffer->buffer)
        {
          wl_shm_pool_destroy (proxy);
          goto fail;
        }
      buffer->pool = pool;
      buffer->data = pool->data + offset;
      wl_buffer_add_listener (buffer->buffer, &buffer_listener, buffer);
      buffer->image = cairo_image_surface_create_for_data
	(buffer->data, CAIRO_FORMAT_RGB24, pool->width, pool->height, stride);
      buffer->cr = cairo_create (buffer->image);
      if (cairo_status (buffer->cr) != CAIRO_STATUS_SUCCESS)
	{
	  wl_shm_pool_destroy (proxy);
	  goto fail;
	}
    }
  wl_shm_pool_destroy (proxy);
  private_of (FRAME_DISPLAY_INFO (f))->pool_bytes += size;
  return pool;

 fail:
  wayland_free_pool (pool);
  return NULL;
}

static void
wayland_retire_pool (struct wayland_surface_state *ss)
{
  struct wayland_pool *pool = ss->pool;
  if (!pool)
    return;
  if (!pool->held)
    {
      wayland_destroy_pool (pool);
      return;
    }

  struct wayland_private *priv
    = private_of (FRAME_DISPLAY_INFO (ss->frame));
  if (ss->reserved_pool)
    {
      ss->reserved_pool->charged = true;
      priv->charged_pool_bytes += ss->reserved_pool->size;
    }
  ss->reserved_pool = pool;
  pool->retired = true;
  pool->next = ss->retired_pools;
  ss->retired_pools = pool;
  priv->retired_bytes += pool->size;
  for (int i = 0; i < WAYLAND_BUFFER_COUNT; i++)
    if (!pool->buffers[i].busy)
      {
        wl_buffer_destroy (pool->buffers[i].buffer);
        pool->buffers[i].buffer = NULL;
      }
}

/* Replace the live pool before asking for any of its held buffers.
   The most recently retired pool uses the frame's replacement reserve.
   Any preceding reserve becomes charged on the same transition.  */
static struct wayland_buffer *
wayland_get_buffer (struct frame *f)
{
  struct wayland_surface_state *ss = surface_state (f);
  struct wayland_pool *pool = ss->pool;
  ss->pool_budget_blocked = false;
  if (!pool || pool->width != ss->canvas_width
      || pool->height != ss->canvas_height || pool->scale != ss->applied_scale)
    {
      struct wayland_private *priv = private_of (FRAME_DISPLAY_INFO (f));
      if (pool && pool->held && ss->reserved_pool
          && priv->charged_pool_bytes + ss->reserved_pool->size
             > priv->pool_budget)
	{
	  ss->pool_budget_blocked = true;
	  return NULL;
	}
      struct wayland_pool *replacement = wayland_new_pool (f);
      if (!replacement)
        return NULL;
      wayland_retire_pool (ss);
      ss->pool = pool = replacement;
    }

  struct wayland_buffer *youngest = NULL;
  for (int i = 0; i < WAYLAND_BUFFER_COUNT; i++)
    {
      struct wayland_buffer *b = &pool->buffers[i];
      if (!b->busy && (!youngest || b->commit > youngest->commit))
        youngest = b;
    }
  return youngest;
}

static void
wayland_full_damage (struct wayland_surface_state *ss, pixman_region32_t *r)
{
  pixman_box32_t box = {0, 0, ss->canvas_width, ss->canvas_height};
  pixman_region32_reset (r, &box);
}

static void
wayland_add_damage (struct wayland_surface_state *ss,
                   int x1, int y1, int x2, int y2)
{
  x1 = max (0, x1);
  y1 = max (0, y1);
  x2 = min (ss->canvas_width, x2);
  y2 = min (ss->canvas_height, y2);
  if (x1 >= x2 || y1 >= y2)
    return;
  if (ss->track_clear)
    {
      /* The clear already damaged the entire image.  */
      for (int y = y1; y < y2; y++)
	ss->clear_row_start[y] = max (ss->clear_row_start[y], x2);
      return;
    }

  /* Most drawing falls inside its glyph string's box, which
     wayland_damage_glyph_string has already added.  */
  pixman_box32_t box = { x1, y1, x2, y2 };
  if (pixman_region32_contains_rectangle (&ss->damage, &box)
      == PIXMAN_REGION_IN)
    return;
  if (!pixman_region32_union_rect (&ss->damage, &ss->damage, x1, y1,
				   x2 - x1, y2 - y1))
    wayland_full_damage (ss, &ss->damage);
  else if (pixman_region32_n_rects (&ss->damage) > WAYLAND_DAMAGE_RECTS)
    {
      box = *pixman_region32_extents (&ss->damage);
      pixman_region32_reset (&ss->damage, &box);
    }
}

/* Damage the box of glyph string S before it is drawn.  Its fills,
   glyphs and lines then add nothing: their ink boxes start and end
   at different heights, which would split the region into several
   bands per text row.  The box is a pixel wider on each side, like
   the glyph damage, which allows for antialiased edges.  Ink outside
   the box, from overlapping glyphs, is still damaged where it is
   drawn.  */
void
wayland_damage_glyph_string (struct glyph_string *s)
{
  struct wayland_surface_state *ss = surface_state (s->f);

  /* After a full clear, damage records only what was drawn, so that
     fills in the clear color can be skipped.  */
  if (!ss->track_clear)
    wayland_add_damage (ss, s->x - s->left_overhang - 1, s->y,
			(s->x + max (s->width, s->background_width)
			 + s->right_overhang + 1),
			s->y + s->height);
}

/* Cairo drawing uses device pixels.  Intersect operation bounds with
   the current clip, rounding outward to cover antialiased edges.  */
static bool
wayland_cairo_clip_box (struct wayland_surface_state *ss, cairo_t *cr,
		       double x1, double y1, double x2, double y2,
		       pixman_box32_t *box)
{
  double left, top, right, bottom;
  cairo_clip_extents (cr, &left, &top, &right, &bottom);
  x1 = max (x1, max (0, left));
  y1 = max (y1, max (0, top));
  x2 = min (x2, min (ss->canvas_width, right));
  y2 = min (y2, min (ss->canvas_height, bottom));
  if (x1 < x2 && y1 < y2)
    {
      *box = (pixman_box32_t) { floor (x1), floor (y1),
			       ceil (x2), ceil (y2) };
      return true;
    }
  return false;
}

void
wayland_cairo_damage (struct frame *f, cairo_t *cr,
                     double x1, double y1, double x2, double y2)
{
  struct wayland_surface_state *ss = surface_state (f);
  pixman_box32_t box;
  if (wayland_cairo_clip_box (ss, cr, x1, y1, x2, y2, &box))
    wayland_add_damage (ss, box.x1, box.y1, box.x2, box.y2);
}

void
wayland_damage_fill (struct frame *f, cairo_t *cr)
{
  double x1, y1, x2, y2;
  cairo_fill_extents (cr, &x1, &y1, &x2, &y2);
  wayland_cairo_damage (f, cr, x1, y1, x2, y2);
}

void
wayland_damage_glyphs (struct frame *f, cairo_t *cr,
                      const cairo_glyph_t *glyphs, int count)
{
  if (count <= 0)
    return;
  cairo_text_extents_t extents;
  cairo_glyph_extents (cr, glyphs, count, &extents);
  double x = glyphs[0].x + extents.x_bearing;
  double y = glyphs[0].y + extents.y_bearing;
  wayland_cairo_damage (f, cr, x - 1, y - 1, x + extents.width + 1,
                        y + extents.height + 1);
}

void
wayland_stroke_path (struct frame *f, cairo_t *cr)
{
  double x1, y1, x2, y2;
  cairo_stroke_extents (cr, &x1, &y1, &x2, &y2);
  wayland_cairo_damage (f, cr, x1, y1, x2, y2);
  cairo_stroke (cr);
}

/* Set REGION to what BUFFER lacks of the last image committed: the
   damage of every commit since its own, or all of it once the history
   no longer reaches back that far.  Record the age of the selected
   buffer even when a later full clear replaces these pixels.  */
static void
wayland_stale_region (struct wayland_surface_state *ss,
		      struct wayland_buffer *buffer, pixman_region32_t *region)
{
  struct wayland_private *priv = private_of (FRAME_DISPLAY_INFO (ss->frame));
  uint64_t age = ss->content_commits - buffer->commit + 1;
  priv->last_buffer_age = buffer->commit ? age : 0;
  pixman_region32_clear (region);
  bool complete = buffer->commit && age <= WAYLAND_DAMAGE_HISTORY + 1;
  for (uint64_t i = 1; complete && i < age; i++)
    complete = pixman_region32_union
      (region, region,
       &ss->history[(ss->content_commits - i) % WAYLAND_DAMAGE_HISTORY]);
  if (!complete)
    wayland_full_damage (ss, region);
}

/* Copy REGION of SRC, an image laid out as BUFFER, into BUFFER.  */
static void
wayland_copy_region (struct wayland_surface_state *ss,
		     struct wayland_buffer *buffer, const unsigned char *src,
		     pixman_region32_t *region)
{
  struct wayland_display_info *dpyinfo = FRAME_DISPLAY_INFO (ss->frame);
  struct wayland_private *priv = private_of (dpyinfo);
  int stride = buffer->pool->stride, n;
  const pixman_box32_t *boxes = pixman_region32_rectangles (region, &n);
  if (!n)
    return;
  wayland_trace (dpyinfo, WAYLAND_TRACE_RESTORE_START);
  for (int i = 0; i < n; i++)
    {
      size_t width = (boxes[i].x2 - boxes[i].x1) * 4;
      size_t height = boxes[i].y2 - boxes[i].y1;
      size_t offset = (size_t) boxes[i].y1 * stride + boxes[i].x1 * 4;
      if (width == stride)
	memcpy (buffer->data + offset, src + offset, width * height);
      else
	for (size_t y = 0; y < height; y++, offset += stride)
	  memcpy (buffer->data + offset, src + offset, width);
      priv->repair_bytes += width * height;
    }
  if (n == 1 && boxes[0].x1 == 0 && boxes[0].y1 == 0
      && boxes[0].x2 == ss->canvas_width && boxes[0].y2 == ss->canvas_height)
    priv->full_repairs++;
  wayland_trace (dpyinfo, WAYLAND_TRACE_RESTORE);
}

/* Fill REGION of CR's image with F's background.  */
static void
wayland_fill_background (struct frame *f, cairo_t *cr,
			 pixman_region32_t *region)
{
  unsigned long bg = FRAME_BACKGROUND_COLOR (f);
  int n;
  const pixman_box32_t *boxes = pixman_region32_rectangles (region, &n);
  cairo_save (cr);
  cairo_set_source_rgb (cr, RED_FROM_ULONG (bg) / 255.0,
			GREEN_FROM_ULONG (bg) / 255.0,
			BLUE_FROM_ULONG (bg) / 255.0);
  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
  for (int i = 0; i < n; i++)
    cairo_rectangle (cr, boxes[i].x1, boxes[i].y1,
		     boxes[i].x2 - boxes[i].x1, boxes[i].y2 - boxes[i].y1);
  cairo_fill (cr);
  cairo_restore (cr);
}

/* Bring F's target up to date before drawing into it: copy what it
   lacks from the front buffer, or fill it with the background when
   there is no front buffer of this size.  */
static void
wayland_flush_stale (struct frame *f)
{
  struct wayland_surface_state *ss = surface_state (f);
  struct wayland_buffer *front = ss->front;

  if (!ss->target || !pixman_region32_not_empty (&ss->stale))
    return;
  ss->track_clear = false;
  if (front && front->commit && front != ss->target
      && front->pool->width == ss->canvas_width
      && front->pool->height == ss->canvas_height)
    {
      cairo_surface_flush (ss->target->image);
      cairo_surface_flush (front->image);
      wayland_copy_region (ss, ss->target, front->data, &ss->stale);
      cairo_surface_mark_dirty (ss->target->image);
    }
  else
    wayland_fill_background (f, ss->target->cr, &ss->stale);
  pixman_region32_clear (&ss->stale);
}

/* The canvas size F's pixel size calls for.  */

static void
wayland_canvas_size (struct frame *f, int *width, int *height)
{
  *width = clip_to_bounds (1, FRAME_PIXEL_WIDTH (f),
			   WAYLAND_MAX_CANVAS_SIZE);
  *height = clip_to_bounds (1, FRAME_PIXEL_HEIGHT (f),
			    WAYLAND_MAX_CANVAS_SIZE);
  struct wayland_surface_state *ss = surface_state (f);
  if (ss->subsurface)
    {
      int scale = ss->applied_scale;
      int logical_width = ((int64_t) *width * WAYLAND_SCALE_UNIT + scale - 1)
	/ scale;
      int logical_height
	= ((int64_t) *height * WAYLAND_SCALE_UNIT + scale - 1) / scale;
      *width = wayland_physical_size (logical_width, scale);
      *height = wayland_physical_size (logical_height, scale);
      if (scale % WAYLAND_SCALE_UNIT == 0)
	{
	  int integer = scale / WAYLAND_SCALE_UNIT;
	  *width -= *width % integer;
	  *height -= *height % integer;
	}
      return;
    }
  if (!ss->preferred_fractional)
    {
      int integer = ss->applied_scale / WAYLAND_SCALE_UNIT;
      *width = max (integer, *width - *width % integer);
      *height = max (integer, *height - *height % integer);
    }
}

/* Take a free buffer of WIDTH x HEIGHT as F's drawing target.  Return
   false, leaving F without a target and its size unchanged, if there
   is none: the compositor holds every buffer of that size, or a pool
   of a new size could not be made or admitted.  */

static bool
wayland_acquire_target (struct frame *f, int width, int height)
{
  struct wayland_surface_state *ss = surface_state (f);
  int old_width = ss->canvas_width, old_height = ss->canvas_height;

  ss->track_clear = false;
  ss->canvas_width = width;
  ss->canvas_height = height;
  struct wayland_buffer *buffer = wayland_get_buffer (f);
  if (!buffer)
    {
      ss->canvas_width = old_width;
      ss->canvas_height = old_height;
      return false;
    }
  ss->target = buffer;
  ss->cr = buffer->cr;
  wayland_stale_region (ss, buffer, &ss->stale);
  /* Drawing can change these pixels before a commit.  If a resize
     abandons the target, do not trust its old buffer age or use it
     as a copy source, even if it was the front buffer.  */
  buffer->commit = 0;
  return true;
}

/* Make sure F has a buffer of its pixel size to draw into.  A content
   commit hands the target to the compositor, so the next drawing
   takes another.  A buffer of a new size starts with the background,
   and the frame is redrawn in full.  Return false if there is no
   buffer to take.  Drawing is then discarded (see wayland_begin_cr),
   and a configure waiting for a new size stays unacked.  */

static bool
wayland_ensure_target (struct frame *f)
{
  struct wayland_surface_state *ss = surface_state (f);
  int width, height;

  wayland_canvas_size (f, &width, &height);
  bool resized = width != ss->canvas_width || height != ss->canvas_height;
  if (ss->target && !resized)
    return true;

  /* A target of the old size goes back to its pool: nothing of that
     size is committed any more.  */
  bool replacing = resized && ss->canvas_width > 0;
  ss->target = NULL;
  ss->cr = NULL;
  if (!wayland_acquire_target (f, width, height))
    return false;
  if (resized)
    {
      if (replacing)
	SET_FRAME_GARBAGED (f);
      wayland_full_damage (ss, &ss->damage);
    }
  return true;
}

static void
frame_done (void *data, struct wl_callback *callback, uint32_t time)
{
  struct wayland_surface_state *ss = data;

  wl_callback_destroy (callback);
  ss->frame_callback = NULL;
  if (ss->frame && FRAME_LIVE_P (ss->frame))
    wayland_trace (FRAME_DISPLAY_INFO (ss->frame), WAYLAND_TRACE_FRAME_DONE);
}

static const struct wl_callback_listener frame_listener =
  { .done = frame_done };

/* Write the backing image for the content commit just enqueued.  The
   mock names its attached buffer by the same surface ID and sequence.  */
static void
wayland_dump_committed_canvas (struct wayland_surface_state *ss)
{
  uint32_t id = wl_proxy_get_id ((struct wl_proxy *) ss->surface);
  char suffix[80];
  snprintf (suffix, sizeof suffix, "/surface-%" PRIu32 "-%lu.png",
	    id, ss->content_commits + ss->background_attaches);
  size_t directory_length = strlen (ss->dump_directory);
  size_t suffix_length = strlen (suffix) + 1;
  char *path = xmalloc (directory_length + suffix_length);
  memcpy (path, ss->dump_directory, directory_length);
  memcpy (path + directory_length, suffix, suffix_length);
  cairo_status_t status = cairo_surface_write_to_png (ss->target->image,
						      path);
  if (status != CAIRO_STATUS_SUCCESS)
    fprintf (stderr, "emacs: could not write %s: %s\n",
	     path, cairo_status_to_string (status));
  xfree (path);
}

/* Commit F's canvas if, and only if, it is safe and useful.  Readiness
   callbacks only mark that a commit is wanted; every content commit
   goes through here.  Return true if a commit was queued.  */

/* The viewport destination for a WIDTH x HEIGHT buffer, or 0x0 when
   the buffer scale is used instead.  */

static void
wayland_destination (struct wayland_surface_state *ss, int width,
		     int height, int *dest_width, int *dest_height)
{
  if (!ss->viewport
      || !(ss->subsurface ? ss->applied_scale % WAYLAND_SCALE_UNIT
	   : ss->preferred_fractional))
    {
      *dest_width = *dest_height = 0;
      return;
    }
  *dest_width = ss->logical_width;
  *dest_height = ss->logical_height;
  if (width != wayland_physical_size (*dest_width, ss->applied_scale))
    *dest_width = wayland_logical_size (width, ss->applied_scale);
  if (height != wayland_physical_size (*dest_height, ss->applied_scale))
    *dest_height = wayland_logical_size (height, ss->applied_scale);
}

/* Publish the pending relationship changes with the parent's state.  */
static void
wayland_commit_surface (struct wayland_surface_state *ss)
{
  if (ss->children_pending)
    {
      for (struct wayland_surface_state *child = ss->first_child;
	   child; child = child->next_sibling)
	{
	  if (!child->subsurface || !child->position_pending)
	    continue;
	  struct frame *f = child->frame;
	  child->position_x = child->requested_x;
	  child->position_y = child->requested_y;
	  wl_subsurface_set_position (child->subsurface, child->position_x,
				      child->position_y);
	  struct frame *focus = FRAME_DISPLAY_INFO (f)->x_focus_frame;
	  if (wayland_ancestor_p (f, focus))
	    wayland_mark_cursor (focus);
	  child->position_pending = false;
	  if (!child->mapped && child->commit_wanted)
	    {
	      struct wayland_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);
	      private_of (dpyinfo)->present_queued = true;
	      wayland_arm_timer (dpyinfo, 1);
	    }
	}
      ss->children_pending = false;
    }
  wl_surface_commit (ss->surface);
}

/* Publish child state on an idle, mapped parent.  Leave any pending
   configure for the parent's next content commit.  */
static bool
wayland_present_children (struct frame *f)
{
  struct wayland_surface_state *ss = surface_state (f);
  struct wayland_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);
  if (!ss || !ss->children_pending || !ss->mapped || !FRAME_VISIBLE_P (f)
      || dpyinfo->state == WAYLAND_DISPLAY_DEAD
      || dpyinfo->output_state == WAYLAND_OUTPUT_PENDING
      || ss->cr_depth || ss->update_depth || buffer_flipping_blocked_p ())
    return false;
  wayland_commit_surface (ss);
  return true;
}

static void
presentation_sync_output (void *data, struct wp_presentation_feedback *proxy,
			  struct wl_output *output)
{
}

static void
finish_presentation_feedback (struct wayland_presentation_feedback *feedback)
{
  feedback->owner->presentation_count--;
  wp_presentation_feedback_destroy (feedback->proxy);
  wl_list_remove (&feedback->link);
  xfree (feedback);
}

static void
presentation_presented (void *data, struct wp_presentation_feedback *proxy,
			uint32_t hi, uint32_t lo, uint32_t ns,
			uint32_t refresh, uint32_t seq_hi, uint32_t seq_lo,
			uint32_t flags)
{
  struct wayland_presentation_feedback *feedback = data;
  feedback->owner->presented++;
  finish_presentation_feedback (feedback);
}

static void
presentation_discarded (void *data, struct wp_presentation_feedback *proxy)
{
  struct wayland_presentation_feedback *feedback = data;
  feedback->owner->discarded++;
  finish_presentation_feedback (feedback);
}

static const struct wp_presentation_feedback_listener presentation_listener =
  {
    .sync_output = presentation_sync_output,
    .presented = presentation_presented,
    .discarded = presentation_discarded
  };

static void
wayland_request_presentation_feedback (struct wayland_private *priv,
				       struct wl_surface *surface)
{
  if (!priv->presentation_enabled)
    return;
  /* Bound the probes if the compositor never replies.  An incomplete
     series is recorded separately from compositor discards.  */
  if (priv->presentation_count >= 256)
    {
      priv->presentation_skipped++;
      return;
    }
  struct wayland_presentation_feedback *feedback = xmalloc (sizeof *feedback);
  feedback->owner = priv;
  feedback->proxy = wp_presentation_feedback (priv->globals.presentation,
					      surface);
  if (!feedback->proxy)
    {
      xfree (feedback);
      priv->presentation_skipped++;
      return;
    }
  wl_list_insert (&priv->presentation_pending, &feedback->link);
  wp_presentation_feedback_add_listener (feedback->proxy,
					 &presentation_listener, feedback);
  priv->presentation_requested++;
  priv->presentation_count++;
}

static bool
wayland_present_content (struct frame *f)
{
  struct wayland_surface_state *ss = surface_state (f);
  struct wayland_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);
  struct wayland_buffer *buffer;
  bool acking;

  if (!ss || !ss->canvas_width || dpyinfo->state == WAYLAND_DISPLAY_DEAD)
    return false;
  ss->commit_wanted = true;

  /* Mapped only after a configure; never mid-drawing or while buffer
     flips are blocked; never while output is pending.  */
  if (!ss->want_visible || !FRAME_VISIBLE_P (f) || !ss->configured
      || ss->cr_depth > 0 || ss->update_depth > 0 || ss->incomplete
      || ss->changing_dpi || ss->scale != ss->applied_scale
      || buffer_flipping_blocked_p ()
      || dpyinfo->output_state == WAYLAND_OUTPUT_PENDING)
    return false;

  /* A configure is acked only with an image drawn after its size was
     applied: no resize may still be pending, and the canvas must match
     the frame.  The frame's size can differ from the configure's, for
     example when the configure is below the frame's minimum size.  */
  int width, height;
  wayland_canvas_size (f, &width, &height);
  if (f->new_size_p || width != ss->canvas_width
      || height != ss->canvas_height)
    return false;
  acking = ss->configure_pending;
  if (!acking && ss->mapped
      && (ss->configured_states & (1 << XDG_TOPLEVEL_STATE_SUSPENDED)))
    return false;

  /* Pace content commits by frame callbacks; a configure response or
     the first commit after mapping goes out regardless, because a
     hidden surface may never get a callback.  */
  bool damaged = pixman_region32_not_empty (&ss->damage);
  bool scaling = ss->committed_scale != ss->applied_scale;
  if (ss->mapped && !acking && !scaling && (ss->frame_callback || !damaged))
    {
      if (!damaged)
	ss->commit_wanted = false;
      else
	wayland_trace (dpyinfo, WAYLAND_TRACE_DEFER);
      return false;
    }

  int dest_width, dest_height;
  wayland_destination (ss, width, height, &dest_width, &dest_height);

  /* Niri uses the position published by the parent when it maps
     a desynchronized child.  Apply the position before mapping.  */
  if (ss->subsurface && !ss->mapped && ss->position_pending
      && !wayland_present_children (FRAME_PARENT_FRAME (f)))
    return false;

  /* With no target, nothing was drawn since the last commit, or
     nothing could be: the compositor holds every buffer, and the
     drawing was discarded.  Either way, a configure that keeps the
     committed buffer size, scale and viewport is still acked, with a
     commit that attaches nothing: the buffer on screen already fits.
     Damage stays for the next content commit, so compositors that
     wait for acks (resize transactions) never wait on a release.  */
  bool fits = (ss->mapped && !scaling
	       && width == ss->committed_width
	       && height == ss->committed_height
	       && dest_width == ss->committed_destination_width
	       && dest_height == ss->committed_destination_height);
  buffer = NULL;
  if (!ss->undrawn)
    {
      /* A configure, a scale or a remap that needs a buffer attached
	 can have the image on screen in another one.  */
      if (!ss->target && !(acking && fits))
	wayland_ensure_target (f);
      if (ss->target)
	{
	  wayland_flush_stale (f);
	  buffer = ss->target;
	}
    }
  wayland_trace (dpyinfo, buffer ? WAYLAND_TRACE_BUFFER
		 : WAYLAND_TRACE_NO_BUFFER);
  if (!buffer)
    {
      if (acking && fits)
	{
	  xdg_surface_ack_configure (ss->xdg_surface, ss->configure_serial);
	  ss->configure_pending = false;
	  wayland_commit_surface (ss);
	  private_of (dpyinfo)->ack_only_commits++;
	  ss->commit_wanted = damaged;
	  return true;
	}
      return false;
    }

  if (!ss->mapped || scaling)
    wayland_full_damage (ss, &ss->damage);
  cairo_surface_flush (buffer->image);
  wayland_trace (dpyinfo, WAYLAND_TRACE_REPAIRED);

  if (acking)
    {
      xdg_surface_ack_configure (ss->xdg_surface, ss->configure_serial);
      ss->configure_pending = false;
    }
  if (dest_width)
    {
      wl_surface_set_buffer_scale (ss->surface, 1);
      wp_viewport_set_destination (ss->viewport, dest_width, dest_height);
    }
  else
    {
      if (ss->viewport && ss->committed_destination_width)
	wp_viewport_set_destination (ss->viewport, -1, -1);
      wl_surface_set_buffer_scale (ss->surface,
				   ss->applied_scale / WAYLAND_SCALE_UNIT);
    }
  if (ss->xdg_surface
      && (!ss->mapped || scaling || width != ss->committed_width
	  || height != ss->committed_height))
    {
      int scale = ss->applied_scale / WAYLAND_SCALE_UNIT;
      xdg_surface_set_window_geometry (ss->xdg_surface, 0, 0,
				       dest_width ? dest_width : width / scale,
				       dest_height ? dest_height
				       : height / scale);
    }
  wl_surface_attach (ss->surface, buffer->buffer, 0, 0);
  int n;
  const pixman_box32_t *boxes = pixman_region32_rectangles (&ss->damage, &n);
  for (int i = 0; i < n; i++)
    wl_surface_damage_buffer (ss->surface, boxes[i].x1, boxes[i].y1,
			      boxes[i].x2 - boxes[i].x1,
			      boxes[i].y2 - boxes[i].y1);
  if (!ss->frame_callback)
    {
      ss->frame_callback = wl_surface_frame (ss->surface);
      if (ss->frame_callback)
	wl_callback_add_listener (ss->frame_callback, &frame_listener, ss);
    }
  wayland_request_presentation_feedback (private_of (dpyinfo), ss->surface);
  wayland_commit_surface (ss);
  wayland_trace (dpyinfo, WAYLAND_TRACE_COMMIT);
  pixman_region32_t *history
    = &ss->history[ss->content_commits % WAYLAND_DAMAGE_HISTORY];
  if (!pixman_region32_copy (history, &ss->damage))
    wayland_full_damage (ss, history);
  pixman_region32_clear (&ss->damage);
  ss->content_commits++;
  ss->committed_scale = ss->applied_scale;
  ss->committed_width = width;
  ss->committed_height = height;
  ss->committed_destination_width = dest_width;
  ss->committed_destination_height = dest_height;
  if (ss->dump_directory)
    wayland_dump_committed_canvas (ss);
  buffer->busy = true;
  buffer->pool->held++;
  buffer->commit = ss->content_commits;
  buffer->attached_ns = monotonic_ns ();
  /* The compositor has the image now; the next drawing takes another,
     brought up to date from this front buffer.  */
  ss->track_clear = false;
  ss->front = buffer;
  ss->target = NULL;
  ss->cr = NULL;
  ss->mapped = true;
  ss->background_attached = false;
  ss->commit_wanted = false;
  if (ss->activation_token)
    {
      wayland_activate (wayland_frame_root (f), ss->activation_token);
      xfree (ss->activation_token);
      ss->activation_token = NULL;
    }
  return true;
}

static bool
wayland_present (struct frame *f)
{
  return wayland_present_content (f) || wayland_present_children (f);
}

/* Present every frame of DPYINFO whose commit was marked as wanted.
   Called after dispatch, so callbacks that only marked work get their
   commit here.  */

static bool
wayland_commits_wanted (struct wayland_display_info *dpyinfo)
{
  Lisp_Object tail, frame;

  FOR_EACH_FRAME (tail, frame)
    {
      struct frame *f = XFRAME (frame);
      if (FRAME_WAYLAND_P (f) && FRAME_DISPLAY_INFO (f) == dpyinfo
	  && surface_state (f)
	  && (surface_state (f)->commit_wanted
	      || surface_state (f)->children_pending))
	return true;
    }
  return false;
}

static void
wayland_present_marked (struct wayland_display_info *dpyinfo)
{
  Lisp_Object tail, frame;

  FOR_EACH_FRAME (tail, frame)
    {
      struct frame *f = XFRAME (frame);

      if (FRAME_WAYLAND_P (f) && FRAME_DISPLAY_INFO (f) == dpyinfo
	  && surface_state (f)
	  && (surface_state (f)->commit_wanted
	      || surface_state (f)->children_pending))
	wayland_present (f);
    }
}

static void
background_release (void *data, struct wl_buffer *buffer)
{
  struct wayland_surface_state *ss = data;
  wl_buffer_destroy (buffer);
  ss->background_buffer = NULL;
}

static const struct wl_buffer_listener background_listener =
  { .release = background_release };

/* A WIDTH x HEIGHT buffer of PIXEL for F's surface, or NULL.  */

static struct wl_buffer *
wayland_solid_buffer (struct frame *f, int width, int height, uint32_t pixel)
{
  size_t stride = (size_t) width * sizeof pixel, size;
  if (ckd_mul (&size, stride, (size_t) height) || size > INT32_MAX)
    return NULL;
  int fd = memfd_create ("emacs-wayland-background", MFD_CLOEXEC);
  if (fd < 0)
    return NULL;
  struct wl_buffer *buffer = NULL;
  void *data = MAP_FAILED;
  if (posix_fallocate (fd, 0, size) == 0)
    data = mmap (NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (data != MAP_FAILED)
    {
      uint32_t *pixels = data;
      for (size_t i = 0; i < size / sizeof pixel; i++)
	pixels[i] = pixel;
      munmap (data, size);
      struct wl_shm_pool *pool = wl_shm_create_pool (globals_of (f)->shm,
						     fd, size);
      if (pool)
	{
	  buffer = wl_shm_pool_create_buffer (pool, 0, width, height, stride,
					      WL_SHM_FORMAT_XRGB8888);
	  wl_shm_pool_destroy (pool);
	}
    }
  emacs_close (fd);
  return buffer;
}

/* Answer a configure of SS's toplevel that arrives before its first
   image with the frame's background, as X and GTK show a window while
   Lisp runs.  The initial frame maps before the init files load, and
   no redisplay runs until they finish.  With a viewport, a 1x1 buffer
   fills the configured size and follows later configures; otherwise
   one buffer of the configured size answers only configures of that
   size.  The first image replaces it in full as on any map.  Nothing
   here reads the frame's size or canvas, so the input hook may call
   it inside a resize.  */

static void
wayland_present_background (struct wayland_surface_state *ss)
{
  struct frame *f = ss->frame;
  struct wayland_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);

  if (!ss->toplevel || ss->mapped || ss->content_commits
      || !ss->configure_pending || !ss->want_visible || !FRAME_VISIBLE_P (f)
      || (ss->background_buffer && !ss->background_attached)
      || (!ss->viewport && ss->applied_scale % WAYLAND_SCALE_UNIT)
      || dpyinfo->output_state == WAYLAND_OUTPUT_PENDING)
    return;

  int scale = ss->viewport ? 1 : ss->applied_scale / WAYLAND_SCALE_UNIT;
  int64_t width = ss->viewport ? 1 : (int64_t) ss->logical_width * scale;
  int64_t height = ss->viewport ? 1 : (int64_t) ss->logical_height * scale;
  if (width <= 0 || width > WAYLAND_MAX_CANVAS_SIZE
      || height <= 0 || height > WAYLAND_MAX_CANVAS_SIZE)
    return;
  if (!ss->background_buffer)
    {
      unsigned long bg = FRAME_BACKGROUND_COLOR (f);
      uint32_t pixel = (RED_FROM_ULONG (bg) << 16 | GREEN_FROM_ULONG (bg) << 8
			| BLUE_FROM_ULONG (bg));
      ss->background_buffer = wayland_solid_buffer (f, width, height, pixel);
      if (!ss->background_buffer)
	return;
      wl_buffer_add_listener (ss->background_buffer, &background_listener, ss);
      wl_surface_attach (ss->surface, ss->background_buffer, 0, 0);
      wl_surface_damage_buffer (ss->surface, 0, 0, width, height);
      wl_surface_set_buffer_scale (ss->surface, scale);
      ss->background_attached = true;
      ss->background_attaches++;
      ss->committed_width = width;
      ss->committed_height = height;
    }
  else if (width != ss->committed_width || height != ss->committed_height)
    return;

  xdg_surface_ack_configure (ss->xdg_surface, ss->configure_serial);
  ss->configure_pending = false;
  xdg_surface_set_window_geometry (ss->xdg_surface, 0, 0, ss->logical_width,
				   ss->logical_height);
  if (ss->viewport)
    {
      wp_viewport_set_destination (ss->viewport, ss->logical_width,
				   ss->logical_height);
      /* The first image sets the buffer scale and viewport again.  */
      ss->committed_destination_width = ss->logical_width;
      ss->committed_destination_height = ss->logical_height;
    }
  wayland_commit_surface (ss);
  private_of (dpyinfo)->background_commits++;
}

/* Answer with the background every configure that the content commits
   of DPYINFO's frames left unanswered.  The input hook leaves those
   commits to the backend timer, which runs only from Emacs's wait.  */

static void
wayland_present_backgrounds (struct wayland_display_info *dpyinfo)
{
  Lisp_Object tail, frame;

  FOR_EACH_FRAME (tail, frame)
    {
      struct frame *f = XFRAME (frame);
      if (FRAME_WAYLAND_P (f) && FRAME_DISPLAY_INFO (f) == dpyinfo
	  && surface_state (f))
	wayland_present_background (surface_state (f));
    }
}

/* xdg-shell events.  */

#define TOPLEVEL_STATE(name) (UINT32_C (1) << XDG_TOPLEVEL_STATE_##name)

/* Whether the latest configure fixes the toplevel's size on one axis.
   A fullscreen, maximized or resizing toplevel, or one tiled against
   both edges of that axis, takes the size the configure names, as
   window managers refuse such resizes on X.  Emacs chooses the size
   of any other axis.  */

static bool
wayland_axis_fixed (struct wayland_surface_state *ss, bool vertical)
{
  uint32_t edges = (vertical
		    ? TOPLEVEL_STATE (TILED_TOP) | TOPLEVEL_STATE (TILED_BOTTOM)
		    : TOPLEVEL_STATE (TILED_LEFT) | TOPLEVEL_STATE (TILED_RIGHT));
  uint32_t states = ss->configured_states;

  if (ss->subsurface || !ss->configured
      || (vertical ? ss->configured_height : ss->configured_width) <= 0)
    return false;
  return ((states & (TOPLEVEL_STATE (FULLSCREEN) | TOPLEVEL_STATE (MAXIMIZED)
		     | TOPLEVEL_STATE (RESIZING)))
	  || (states & edges) == edges);
}

/* Record the toplevel's state in the `fullscreen' frame parameter as
   PGTK does, so that shared code inhibits implied resizes the
   compositor would not grant.  */

static void
wayland_note_toplevel_states (struct wayland_surface_state *ss)
{
  uint32_t states = ss->configured_states;
  uint32_t vertical = TOPLEVEL_STATE (TILED_TOP) | TOPLEVEL_STATE (TILED_BOTTOM);
  uint32_t horizontal = (TOPLEVEL_STATE (TILED_LEFT)
			 | TOPLEVEL_STATE (TILED_RIGHT));
  Lisp_Object value = Qnil;

  if (states & TOPLEVEL_STATE (FULLSCREEN))
    value = Qfullboth;
  else if (states & TOPLEVEL_STATE (MAXIMIZED))
    value = Qmaximized;
  else if ((states & vertical) == vertical)
    value = Qfullheight;
  else if ((states & horizontal) == horizontal)
    value = Qfullwidth;
  if (!EQ (get_frame_param (ss->frame, Qfullscreen), value))
    store_frame_param (ss->frame, Qfullscreen, value);
}

static void
xdg_surface_configure (void *data, struct xdg_surface *xdg_surface,
		       uint32_t serial)
{
  struct wayland_surface_state *ss = data;

  /* Configure bursts merge: only the latest is acked.  */
  ss->configure_serial = serial;
  ss->configure_pending = true;
  ss->configured = true;
  ss->configured_states = ss->pending_states;
  ss->configured_width = ss->pending_width;
  ss->configured_height = ss->pending_height;
  ss->commit_wanted = true;
  wayland_note_toplevel_states (ss);

  /* Each axis the configure leaves at 0 takes the size Emacs chose.  */
  ss->logical_width = (ss->pending_width > 0
                       ? ss->pending_width : max (1, ss->client_width));
  ss->logical_height = (ss->pending_height > 0
                        ? ss->pending_height : max (1, ss->client_height));
  wayland_resize_surface (ss);
}

static const struct xdg_surface_listener xdg_surface_listener =
  { .configure = xdg_surface_configure };

static void
toplevel_configure (void *data, struct xdg_toplevel *toplevel,
		    int32_t width, int32_t height, struct wl_array *states)
{
  struct wayland_surface_state *ss = data;

  ss->pending_width = width;
  ss->pending_height = height;
  ss->pending_states = 0;
  uint32_t *state;
  wl_array_for_each (state, states)
    if (*state < 32)
      ss->pending_states |= UINT32_C (1) << *state;
}

static void
toplevel_close (void *data, struct xdg_toplevel *toplevel)
{
  struct wayland_surface_state *ss = data;
  struct input_event ie;

  EVENT_INIT (ie);
  ie.kind = DELETE_WINDOW_EVENT;
  XSETFRAME (ie.frame_or_window, ss->frame);
  kbd_buffer_store_event (&ie);
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

/* Create F's surface and role objects.  Nothing is committed until the
   frame is made visible.  */

static void
wayland_mark_parent (struct frame *parent)
{
  struct wayland_display_info *dpyinfo = FRAME_DISPLAY_INFO (parent);
  surface_state (parent)->children_pending = true;
  private_of (dpyinfo)->present_queued = true;
  wayland_arm_timer (dpyinfo, 1);
}

static bool
wayland_attach_subsurface (struct wayland_surface_state *ss,
			    struct frame *parent)
{
  ss->subsurface = wl_subcompositor_get_subsurface
    (globals_of (ss->frame)->subcompositor, ss->surface,
     surface_state (parent)->surface);
  if (!ss->subsurface)
    return false;
  wl_subsurface_set_desync (ss->subsurface);
  ss->configured = true;
  ss->position_pending = true;
  wayland_mark_parent (parent);
  return true;
}

static struct wayland_surface_state **
wayland_child_slot (struct wayland_surface_state *parent,
		     struct wayland_surface_state *child)
{
  struct wayland_surface_state **slot = &parent->first_child;
  while (*slot && *slot != child)
    slot = &(*slot)->next_sibling;
  return slot;
}

static void
wayland_unlink_child (struct wayland_surface_state *parent,
			struct wayland_surface_state *child)
{
  struct wayland_surface_state **slot = wayland_child_slot (parent, child);
  if (*slot)
    {
      *slot = child->next_sibling;
      child->next_sibling = NULL;
    }
}

static void
wayland_link_child (struct wayland_surface_state *parent,
		      struct wayland_surface_state *child)
{
  *wayland_child_slot (parent, NULL) = child;
}

bool
wayland_restack_frame (struct frame *f, struct frame *reference, bool above)
{
  struct frame *parent = FRAME_PARENT_FRAME (f);
  if (!parent || f == reference || !FRAME_WAYLAND_P (reference)
      || (reference != parent && FRAME_PARENT_FRAME (reference) != parent)
      || frame_display_dead (f))
    return false;
  struct wayland_surface_state *ss = surface_state (f);
  struct wayland_surface_state *ps = surface_state (parent);
  struct wayland_surface_state *rs = surface_state (reference);
  block_input ();
  wayland_unlink_child (ps, ss);
  struct wayland_surface_state **slot;
  if (reference == parent)
    {
      /* The parent is the lower bound of the sibling stack.  */
      slot = &ps->first_child;
      above = true;
    }
  else
    slot = above ? &rs->next_sibling : wayland_child_slot (ps, rs);
  ss->next_sibling = *slot;
  *slot = ss;
  if (above)
    wl_subsurface_place_above (ss->subsurface, rs->surface);
  else
    wl_subsurface_place_below (ss->subsurface, rs->surface);
  wayland_mark_parent (parent);
  unblock_input ();
  return true;
}

bool
wayland_restack_child (struct frame *f, bool raise)
{
  struct frame *parent = FRAME_PARENT_FRAME (f);
  if (!parent || frame_display_dead (f))
    return false;
  struct frame *reference = parent;
  if (raise)
    for (struct wayland_surface_state *child = surface_state (parent)->first_child;
	 child; child = child->next_sibling)
      if (child->frame != f)
	reference = child->frame;
  return wayland_restack_frame (f, reference, true);
}

static struct wayland_surface_state *
wayland_new_frame_surface (struct frame *f, struct frame *parent)
{
  struct wayland_globals *globals = globals_of (f);
  struct wayland_surface_state *ss = xzalloc (sizeof *ss);

  ss->frame = f;
  ss->applied_scale = surface_state (f) ? surface_state (f)->applied_scale
    : parent ? surface_state (parent)->applied_scale : WAYLAND_SCALE_UNIT;
  ss->scale = parent ? surface_state (parent)->scale : ss->applied_scale;
  ss->logical_width = max (1, FRAME_PIXEL_WIDTH (f));
  ss->logical_height = max (1, FRAME_PIXEL_HEIGHT (f));
  if (parent)
    {
      ss->client_width = ss->child_width = ss->logical_width;
      ss->client_height = ss->child_height = ss->logical_height;
      ss->child_size_scale = ss->applied_scale;
      ss->requested_x = wayland_scale_offset
	(f->left_pos, ss->applied_scale, WAYLAND_SCALE_UNIT);
      ss->requested_y = wayland_scale_offset
	(f->top_pos, ss->applied_scale, WAYLAND_SCALE_UNIT);
    }
  pixman_region32_init (&ss->damage);
  pixman_region32_init (&ss->stale);
  for (int i = 0; i < WAYLAND_DAMAGE_HISTORY; i++)
    pixman_region32_init (&ss->history[i]);
  ss->surface = wl_compositor_create_surface (globals->compositor);
  if (ss->surface)
    wl_surface_add_listener (ss->surface, &surface_listener, ss);
  if (ss->surface && globals->viewporter && globals->fractional_scale_manager)
    {
      ss->viewport = wp_viewporter_get_viewport (globals->viewporter,
                                                 ss->surface);
      if (ss->viewport && !parent)
        {
          ss->fractional = wp_fractional_scale_manager_v1_get_fractional_scale
            (globals->fractional_scale_manager, ss->surface);
          if (ss->fractional)
            wp_fractional_scale_v1_add_listener (ss->fractional,
                                                 &fractional_listener, ss);
        }
    }
  if (ss->surface && parent)
    wayland_attach_subsurface (ss, parent);
  else if (ss->surface)
    {
      ss->xdg_surface = xdg_wm_base_get_xdg_surface (globals->wm_base,
						    ss->surface);
      if (ss->xdg_surface)
	{
	  xdg_surface_add_listener (ss->xdg_surface, &xdg_surface_listener, ss);
	  ss->toplevel = xdg_surface_get_toplevel (ss->xdg_surface);
	}
      if (ss->toplevel)
	{
	  xdg_toplevel_add_listener (ss->toplevel, &toplevel_listener, ss);
	  if (globals->decoration_manager)
	    ss->decoration = zxdg_decoration_manager_v1_get_toplevel_decoration
	      (globals->decoration_manager, ss->toplevel);
	}
    }
  return ss;
}

void
wayland_create_frame_surface (struct frame *f)
{
  struct frame *parent = FRAME_PARENT_FRAME (f);
  wayland_init_frame_cursors (f);
  block_input ();
  struct wayland_surface_state *ss = wayland_new_frame_surface (f, parent);
  FRAME_X_OUTPUT (f)->surface = ss;
  if (ss->subsurface)
    wayland_link_child (surface_state (parent), ss);
  unblock_input ();

  /* Everything here is freed by unwind_create_frame.  */
  if (!(parent ? ss->subsurface != NULL : ss->toplevel != NULL)
      || !wayland_ensure_target (f))
    memory_full (0);
}

static void
wayland_send_toplevel_mode (struct wayland_surface_state *ss)
{
  enum fullscreen_type mode = ss->frame->want_fullscreen;
  if (mode == FULLSCREEN_BOTH)
    xdg_toplevel_set_fullscreen (ss->toplevel, NULL);
  else if (ss->initial_commit_done)
    xdg_toplevel_unset_fullscreen (ss->toplevel);
  if (mode == FULLSCREEN_MAXIMIZED)
    xdg_toplevel_set_maximized (ss->toplevel);
  else if (ss->initial_commit_done)
    xdg_toplevel_unset_maximized (ss->toplevel);
}

static void
wayland_send_toplevel_state (struct wayland_surface_state *ss)
{
  xdg_toplevel_set_app_id (ss->toplevel, "emacs");
  if (ss->title)
    xdg_toplevel_set_title (ss->toplevel, ss->title);
  struct frame *f = ss->frame;
  Lisp_Object cols = get_frame_param (f, Qmin_width);
  Lisp_Object rows = get_frame_param (f, Qmin_height);
  if (RANGED_FIXNUMP (1, cols, WAYLAND_MAX_CANVAS_SIZE)
      || RANGED_FIXNUMP (1, rows, WAYLAND_MAX_CANVAS_SIZE))
    {
      int width = RANGED_FIXNUMP (1, cols, WAYLAND_MAX_CANVAS_SIZE)
        ? wayland_logical_size (XFIXNUM (cols) * FRAME_COLUMN_WIDTH (f),
                                ss->applied_scale) : 0;
      int height = RANGED_FIXNUMP (1, rows, WAYLAND_MAX_CANVAS_SIZE)
        ? wayland_logical_size (XFIXNUM (rows) * FRAME_LINE_HEIGHT (f),
                                ss->applied_scale) : 0;
      xdg_toplevel_set_min_size (ss->toplevel, width, height);
    }
  wayland_send_toplevel_mode (ss);
  /* Server-side decorations only.  */
  if (ss->decoration)
    zxdg_toplevel_decoration_v1_set_mode
      (ss->decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

void
wayland_set_frame_title (struct frame *f, const char *title)
{
  struct wayland_surface_state *ss = surface_state (f);

  if (!ss)
    return;
  xfree (ss->title);
  ss->title = xstrdup (title);
  if (ss->toplevel && !frame_display_dead (f))
    {
      xdg_toplevel_set_title (ss->toplevel, ss->title);
      wayland_flush_now (FRAME_DISPLAY_INFO (f));
    }
}

static void
wayland_update_visibility (struct frame *f)
{
  struct wayland_surface_state *ss = surface_state (f);
  struct frame *parent = FRAME_PARENT_FRAME (f);
  bool visible = ss->want_visible && (!parent || FRAME_VISIBLE_P (parent));
  if (visible == FRAME_VISIBLE_P (f))
    return;
  SET_FRAME_VISIBLE (f, visible);
  if (visible)
    {
      SET_FRAME_GARBAGED (f);
      ss->commit_wanted = true;
    }
  for (struct wayland_surface_state *child = ss->first_child;
       child; child = child->next_sibling)
    wayland_update_visibility (child->frame);
}

/* Map F: send the toplevel's state and the bufferless initial commit.
   The first buffer is attached by wayland_present once the configure
   arrives.  Unmapping discards the toplevel's attributes, so they are
   sent again on every map.  */

void
wayland_make_frame_visible (struct frame *f)
{
  struct wayland_surface_state *ss = surface_state (f);

  if (!ss || ss->want_visible || frame_display_dead (f))
    return;
  block_input ();
  ss->want_visible = true;
  if (!ss->client_width)
    {
      ss->client_width = ss->logical_width
        = wayland_logical_size (FRAME_PIXEL_WIDTH (f), ss->applied_scale);
      ss->client_height = ss->logical_height
        = wayland_logical_size (FRAME_PIXEL_HEIGHT (f), ss->applied_scale);
    }
  if (ss->toplevel && !ss->initial_commit_done)
    {
      wayland_send_toplevel_state (ss);
      wayland_commit_surface (ss);
      ss->initial_commit_done = true;
      ss->configured = false;
    }
  wayland_update_visibility (f);
  /* The initial frame maps before the init files load: send the
     commit now, so that its configure can be answered meanwhile.  */
  wayland_flush_now (FRAME_DISPLAY_INFO (f));
  unblock_input ();
}

/* Unmap F by attaching a null buffer.  Remapping takes a new initial
   commit and configure.  */

void
wayland_make_frame_invisible (struct frame *f)
{
  struct wayland_surface_state *ss = surface_state (f);

  if (!ss || !ss->want_visible)
    return;
  block_input ();
  ss->want_visible = false;
  if (!frame_display_dead (f) && (ss->initial_commit_done || ss->mapped))
    {
      wl_surface_attach (ss->surface, NULL, 0, 0);
      wl_surface_commit (ss->surface);
    }
  if (ss->frame_callback)
    {
      wl_callback_destroy (ss->frame_callback);
      ss->frame_callback = NULL;
    }
  ss->mapped = false;
  ss->background_attached = false;
  ss->initial_commit_done = false;
  ss->configured = ss->subsurface != NULL;
  ss->configure_pending = false;
  ss->pending_width = ss->pending_height = 0;
  ss->configured_width = ss->configured_height = 0;
  ss->pending_states = ss->configured_states = 0;
  wayland_drop_subtree_focus (f);
  wayland_update_visibility (f);
  wayland_flush_now (FRAME_DISPLAY_INFO (f));
  unblock_input ();
}

static void
wayland_destroy_frame_surface (struct wayland_surface_state *ss)
{
  ss->pool_budget_blocked = false;
  if (ss->pool)
    wayland_destroy_pool (ss->pool);
  while (ss->retired_pools)
    wayland_destroy_pool (ss->retired_pools);
  if (ss->background_buffer)
    wl_buffer_destroy (ss->background_buffer);
  if (ss->frame_callback)
    wl_callback_destroy (ss->frame_callback);
  if (ss->decoration)
    zxdg_toplevel_decoration_v1_destroy (ss->decoration);
  if (ss->toplevel)
    xdg_toplevel_destroy (ss->toplevel);
  if (ss->xdg_surface)
    xdg_surface_destroy (ss->xdg_surface);
  if (ss->subsurface)
    wl_subsurface_destroy (ss->subsurface);
  if (ss->fractional)
    wp_fractional_scale_v1_destroy (ss->fractional);
  if (ss->viewport)
    wp_viewport_destroy (ss->viewport);
  while (ss->outputs)
    wayland_leave_output (ss, ss->outputs->output);
  if (ss->surface)
    wl_surface_destroy (ss->surface);
  xfree (ss->title);
  xfree (ss->activation_token);
  xfree (ss->dump_directory);
  xfree (ss->clear_row_start);
  pixman_region32_fini (&ss->damage);
  pixman_region32_fini (&ss->stale);
  for (int i = 0; i < WAYLAND_DAMAGE_HISTORY; i++)
    pixman_region32_fini (&ss->history[i]);
  xfree (ss);
}

static void
wayland_detach_subsurface (struct wayland_surface_state *ss)
{
  wl_subsurface_destroy (ss->subsurface);
  ss->subsurface = NULL;
  if (ss->frame_callback)
    {
      wl_callback_destroy (ss->frame_callback);
      ss->frame_callback = NULL;
    }
  ss->mapped = false;
  ss->commit_wanted = ss->want_visible;
  SET_FRAME_GARBAGED (ss->frame);
}

/* Reattach child surfaces in stacking order after replacing their
   parent.  The surviving children keep their own descendant trees.  */
static void
wayland_rebind_children (struct wayland_surface_state *ss)
{
  struct wayland_surface_state *previous = NULL;
  for (struct wayland_surface_state *child = ss->first_child;
       child; child = child->next_sibling)
    {
      if (!wayland_attach_subsurface (child, ss->frame))
	memory_full (0);
      wl_subsurface_place_above (child->subsurface,
				previous ? previous->surface : ss->surface);
      previous = child;
    }
}

static void
wayland_inherit_scale (struct frame *f, int scale)
{
  struct wayland_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);
  struct wayland_private *priv = private_of (dpyinfo);
  Lisp_Object frame, tail;
  FOR_EACH_FRAME (tail, frame)
    {
      struct frame *descendant = XFRAME (frame);
      if (wayland_ancestor_p (f, descendant))
	surface_state (descendant)->scale = scale;
    }
  priv->scale_pending = true;
  if (!priv->scale_queued)
    wayland_arm_timer (dpyinfo, 1);
}

void
wayland_set_parent_frame (struct frame *f, Lisp_Object value,
			  Lisp_Object old_value)
{
  struct wayland_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);
  struct frame *old_parent = FRAME_PARENT_FRAME (f);
  wayland_check_parent (dpyinfo, f, value);
  struct frame *parent = NILP (value) ? NULL : XFRAME (value);
  if (parent == old_parent)
    return;

  struct wayland_surface_state *ss = surface_state (f);
  struct frame *old_root = wayland_frame_root (f);
  struct frame *new_root = parent ? wayland_frame_root (parent) : f;
  bool visible = ss->want_visible;
  block_input ();
  if (frame_display_dead (f))
    {
      if (ss->subsurface && !parent)
	{
	  wayland_unlink_child (surface_state (old_parent), ss);
	  wayland_detach_subsurface (ss);
	  fset_parent_frame (f, Qnil);
	}
      unblock_input ();
      return;
    }
  if (old_root != new_root || (parent && !FRAME_VISIBLE_P (parent)))
    wayland_drop_subtree_focus (f);

  if (old_parent && parent)
    {
      wayland_unlink_child (surface_state (old_parent), ss);
      wayland_detach_subsurface (ss);
      if (!wayland_attach_subsurface (ss, parent))
	{
	  if (wayland_attach_subsurface (ss, old_parent))
	    wayland_link_child (surface_state (old_parent), ss);
	  unblock_input ();
	  store_frame_param (f, Qparent_frame, old_value);
	  memory_full (0);
	}
      wayland_link_child (surface_state (parent), ss);
      fset_parent_frame (f, value);
      wayland_update_visibility (f);
    }
  else
    {
      /* wl_surface roles persist after their role objects are gone.
	 Allocate the replacement before changing the frame hierarchy.  */
      struct wayland_surface_state *replacement
	= wayland_new_frame_surface (f, parent);
      if (!(parent ? replacement->subsurface != NULL
	    : replacement->toplevel != NULL))
	{
	  wayland_destroy_frame_surface (replacement);
	  unblock_input ();
	  store_frame_param (f, Qparent_frame, old_value);
	  memory_full (0);
	}
      if (old_parent)
	wayland_unlink_child (surface_state (old_parent), ss);
      for (struct wayland_surface_state *child = ss->first_child;
	   child; child = child->next_sibling)
	wayland_detach_subsurface (child);

      wayland_forget_pointer_frame (dpyinfo, f);
      wayland_im_forget_frame (dpyinfo, f, ss->surface);
      if (private_of (dpyinfo)->activation_frame == f)
	wayland_forget_activation (private_of (dpyinfo));
      if (private_of (dpyinfo)->seat.keyboard_focus == ss->surface)
	private_of (dpyinfo)->seat.keyboard_focus = NULL;

      replacement->first_child = ss->first_child;
      replacement->title = ss->title;
      replacement->activation_token = ss->activation_token;
      replacement->dump_directory = ss->dump_directory;
      ss->title = ss->activation_token = ss->dump_directory = NULL;
      if (!parent && replacement->viewport
	  && replacement->applied_scale % WAYLAND_SCALE_UNIT)
	replacement->preferred_fractional = replacement->applied_scale;
      FRAME_X_OUTPUT (f)->surface = replacement;
      fset_parent_frame (f, value);
      if (parent)
	wayland_link_child (surface_state (parent), replacement);
      wayland_rebind_children (replacement);
      wayland_destroy_frame_surface (ss);

      SET_FRAME_GARBAGED (f);
      if (visible)
	wayland_make_frame_visible (f);
    }
  if (parent)
    {
      if (FRAME_Z_GROUP_BELOW (f))
	wayland_restack_child (f, false);
      wayland_inherit_scale (f, surface_state (new_root)->scale);
    }
  wayland_flush_now (dpyinfo);
  unblock_input ();
}

/* Free F's faces and its surface.  Callers move focus and highlight off
   F first, since either would draw with its faces.  */

void
wayland_free_frame_resources (struct frame *f)
{
  block_input ();
  /* As x_free_frame_resources does.  This removes F's realized
     fontsets from Vfontset_table, which would otherwise keep the dead
     frame alive, and drops its share of the image cache.  */
  free_frame_faces (f);
  if (FRAME_X_OUTPUT (f) && surface_state (f))
    {
      struct wayland_surface_state *ss = surface_state (f);
      if (ss->subsurface)
	wayland_unlink_child (surface_state (FRAME_PARENT_FRAME (f)), ss);
      FRAME_X_OUTPUT (f)->surface = NULL;
      wayland_destroy_frame_surface (ss);
    }
  unblock_input ();
}

static void
wayland_delete_frame (struct frame *f)
{
  struct wayland_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);

  if (dpyinfo)
    {
      wayland_drop_subtree_focus (f);
      if (private_of (dpyinfo)->activation_frame == f)
	wayland_forget_activation (private_of (dpyinfo));
      wayland_purge_events (dpyinfo, f);
      wayland_forget_pointer_frame (dpyinfo, f);
      wayland_im_forget_frame (dpyinfo, f,
			       surface_state (f) ? surface_state (f)->surface
			       : NULL);
      if (dpyinfo->x_focus_frame == f)
	dpyinfo->x_focus_frame = NULL;
      if (dpyinfo->x_focus_event_frame == f)
	dpyinfo->x_focus_event_frame = NULL;
      if (dpyinfo->highlight_frame == f)
	dpyinfo->highlight_frame = NULL;
      /* No leave event comes for a destroyed surface.  */
      if (MOUSE_HL_INFO (f)->mouse_face_mouse_frame == f)
	reset_mouse_highlight (MOUSE_HL_INFO (f));
      if (surface_state (f)
	  && private_of (dpyinfo)->seat.keyboard_focus
	     == surface_state (f)->surface)
	private_of (dpyinfo)->seat.keyboard_focus = NULL;
    }
  wayland_free_frame_resources (f);
  xfree (f->output_data.wayland);
  f->output_data.wayland = NULL;
  /* Send the destruction now: nothing else may flush until the next
     input or redisplay, and the compositor would keep showing the
     frame.  */
  if (dpyinfo)
    wayland_flush_now (dpyinfo);
}

/* Colors.  */

bool
wayland_defined_color (struct frame *f, const char *name,
		       Emacs_Color *color_def, bool alloc, bool makeIndex)
{
  unsigned short r, g, b;
  struct wayland_display_info *dpyinfo
    = f ? FRAME_DISPLAY_INFO (f) : x_display_list;

  if (parse_color_spec (name, &r, &g, &b))
    {
      color_def->red = r;
      color_def->green = g;
      color_def->blue = b;
    }
  else
    {
      Lisp_Object tail;

      for (tail = dpyinfo ? dpyinfo->color_map : Qnil; CONSP (tail);
	   tail = XCDR (tail))
	{
	  Lisp_Object entry = XCAR (tail);
	  if (CONSP (entry) && !xstrcasecmp (SSDATA (XCAR (entry)), name))
	    {
	      unsigned long pixel = XFIXNUM (XCDR (entry));
	      color_def->red = RED_FROM_ULONG (pixel) * 257;
	      color_def->green = GREEN_FROM_ULONG (pixel) * 257;
	      color_def->blue = BLUE_FROM_ULONG (pixel) * 257;
	      break;
	    }
	}
      if (!CONSP (tail))
	return false;
    }
  color_def->pixel = RGB_TO_ULONG (color_def->red >> 8,
				   color_def->green >> 8,
				   color_def->blue >> 8);
  return true;
}

void
wayland_query_colors (struct frame *f, Emacs_Color *colors, int ncolors)
{
  for (int i = 0; i < ncolors; i++)
    {
      unsigned long pixel = colors[i].pixel;
      colors[i].red = RED_FROM_ULONG (pixel) * 257;
      colors[i].green = GREEN_FROM_ULONG (pixel) * 257;
      colors[i].blue = BLUE_FROM_ULONG (pixel) * 257;
    }
}

static void
wayland_query_frame_background_color (struct frame *f, Emacs_Color *bgcolor)
{
  bgcolor->pixel = FRAME_BACKGROUND_COLOR (f);
  wayland_query_colors (f, bgcolor, 1);
}

static void
wayland_free_pixmap (struct frame *f, Emacs_Pixmap pixmap)
{
  if (pixmap)
    {
      xfree (pixmap->data);
      xfree (pixmap);
    }
}

/* Fonts.  */

static Lisp_Object
wayland_new_font (struct frame *f, Lisp_Object font_object, int fontset)
{
  struct font *font = XFONT_OBJECT (font_object);
  int ascent, descent, unit;

  if (fontset < 0)
    fontset = fontset_from_font (font_object);
  FRAME_FONTSET (f) = fontset;
  if (FRAME_FONT (f) == font)
    return font_object;

  FRAME_FONT (f) = font;
  FRAME_BASELINE_OFFSET (f) = font->baseline_offset;
  FRAME_COLUMN_WIDTH (f) = font->average_width;
  get_font_ascent_descent (font, &ascent, &descent);
  FRAME_LINE_HEIGHT (f) = ascent + descent;
  FRAME_TAB_BAR_HEIGHT (f) = FRAME_TAB_BAR_LINES (f) * FRAME_LINE_HEIGHT (f);

  /* No scroll bars, but keep the columns consistent for shared code.  */
  unit = FRAME_COLUMN_WIDTH (f);
  FRAME_CONFIG_SCROLL_BAR_COLS (f) = (14 + unit - 1) / unit;

  if (surface_state (f) && !surface_state (f)->changing_dpi)
    adjust_frame_size (f, FRAME_COLS (f) * FRAME_COLUMN_WIDTH (f),
		       FRAME_LINES (f) * FRAME_LINE_HEIGHT (f), 3, false,
		       Qfont);
  return font_object;
}

/* Drawing into the target buffer.  */

/* Where drawing goes while a frame has no buffer to draw into: an
   image that keeps nothing.  One pixel serves every frame.  */
static cairo_t *
wayland_sink (void)
{
  static cairo_t *cr;
  if (!cr)
    {
      cairo_surface_t *surface
	= cairo_image_surface_create (CAIRO_FORMAT_RGB24, 1, 1);
      cr = cairo_create (surface);
      cairo_surface_destroy (surface);
    }
  return cr;
}

static cairo_t *
wayland_begin_cr (struct frame *f, bool clear)
{
  struct wayland_surface_state *ss = surface_state (f);

  /* Nested glyph draws must keep the same image.  The outer scope
     also prevents terminal finalization during drawing.  */
  if (ss->cr_depth == 0)
    {
      /* With no buffer to take, discard the drawing.  The glyph
	 matrices then describe an image that no buffer has, so keep
	 discarding until the next update can take a buffer and ask
	 for a full redraw (see wayland_update_begin).  */
      if (!ss->undrawn && !wayland_ensure_target (f))
	{
	  ss->undrawn = true;
	  private_of (FRAME_DISPLAY_INFO (f))->discards++;
	}
      if (ss->undrawn)
	ss->cr = wayland_sink ();
      else
	{
	  ss->cr = ss->target->cr;
	  if (!clear)
	    wayland_flush_stale (f);
	}
      FRAME_DISPLAY_INFO (f)->busy++;
    }
  ss->cr_depth++;
  cairo_save (ss->cr);
  return ss->cr;
}

cairo_t *
wayland_begin_cr_clip (struct frame *f)
{
  return wayland_begin_cr (f, false);
}

void
wayland_end_cr_clip (struct frame *f)
{
  struct wayland_surface_state *ss = surface_state (f);

  eassert (ss->cr_depth > 0);
  cairo_restore (ss->cr);
  if (--ss->cr_depth == 0)
    FRAME_DISPLAY_INFO (f)->busy--;
}

void
wayland_set_cr_source_with_color (struct frame *f, unsigned long color,
				  bool respect_alpha_background)
{
  cairo_set_source_rgb (surface_state (f)->cr,
			RED_FROM_ULONG (color) / 255.0,
			GREEN_FROM_ULONG (color) / 255.0,
			BLUE_FROM_ULONG (color) / 255.0);
}

static bool
wayland_box_still_clear (const struct wayland_surface_state *ss,
			const pixman_box32_t *box)
{
  for (int y = box->y1; y < box->y2; y++)
    if (ss->clear_row_start[y] > box->x1)
      return false;
  return true;
}

/* Paint an opaque rectangle, unless the full clear already supplied
   its pixels.  The clip bounds include every pixel the fill can
   touch, so the row test also works with a nonrectangular clip.  */
void
wayland_fill_cairo_rectangle (struct frame *f, cairo_t *cr,
			      unsigned long color, int x, int y,
			      int width, int height)
{
  struct wayland_surface_state *ss = surface_state (f);
  cairo_rectangle (cr, x, y, width, height);
  double x1, y1, x2, y2;
  pixman_box32_t box;
  cairo_fill_extents (cr, &x1, &y1, &x2, &y2);
  if (wayland_cairo_clip_box (ss, cr, x1, y1, x2, y2, &box))
    {
      if (ss->track_clear && color == ss->clear_color
	  && wayland_box_still_clear (ss, &box))
	{
	  cairo_new_path (cr);
	  return;
	}
      wayland_add_damage (ss, box.x1, box.y1, box.x2, box.y2);
    }
  wayland_set_cr_source_with_color (f, color, false);
  cairo_fill (cr);
}

void
wayland_fill_rect (struct frame *f, unsigned long color, int x, int y,
		   int width, int height)
{
  cairo_t *cr;

  if (width <= 0 || height <= 0 || frame_display_dead (f))
    return;
  cr = wayland_begin_cr_clip (f);
  wayland_fill_cairo_rectangle (f, cr, color, x, y, width, height);
  wayland_end_cr_clip (f);
}

static void
wayland_clear_frame_area (struct frame *f, int x, int y, int width,
			  int height)
{
  wayland_fill_rect (f, FRAME_BACKGROUND_COLOR (f), x, y, width, height);
}

static void
wayland_clear_frame (struct frame *f)
{
  mark_window_cursors_off (XWINDOW (FRAME_ROOT_WINDOW (f)));
  struct wayland_surface_state *ss = surface_state (f);
  if (!ss || frame_display_dead (f))
    return;
  /* A full clear needs nothing from the front buffer.  An earlier
     draw may already have restored the target.  */
  cairo_t *cr = wayland_begin_cr (f, true);
  if (ss->undrawn)
    {
      wayland_end_cr_clip (f);
      return;
    }
  pixman_region32_clear (&ss->stale);
  wayland_trace (FRAME_DISPLAY_INFO (f), WAYLAND_TRACE_CLEAR);
  cairo_reset_clip (cr);
  ss->clear_color = FRAME_BACKGROUND_COLOR (f);
  wayland_set_cr_source_with_color (f, ss->clear_color, false);
  cairo_paint (cr);
  wayland_full_damage (ss, &ss->damage);
  wayland_end_cr_clip (f);

  if (ss->clear_row_capacity < ss->canvas_height)
    {
      ss->clear_row_start = xrealloc (ss->clear_row_start,
				     ss->canvas_height
				     * sizeof *ss->clear_row_start);
      ss->clear_row_capacity = ss->canvas_height;
    }
  memset (ss->clear_row_start, 0,
	  ss->canvas_height * sizeof *ss->clear_row_start);
  ss->track_clear = true;
}

void
wayland_clear_under_internal_border (struct frame *f)
{
  int border = FRAME_INTERNAL_BORDER_WIDTH (f);
  int width = FRAME_PIXEL_WIDTH (f), height = FRAME_PIXEL_HEIGHT (f);
  int margin = FRAME_TOP_MARGIN_HEIGHT (f);
  struct face *face = FACE_FROM_ID_OR_NULL (f, INTERNAL_BORDER_FACE_ID);
  unsigned long color = face ? face->background : FRAME_BACKGROUND_COLOR (f);

  if (border <= 0 || !surface_state (f))
    return;
  wayland_fill_rect (f, color, 0, margin, width, border);
  wayland_fill_rect (f, color, 0, 0, border, height);
  wayland_fill_rect (f, color, width - border, 0, border, height);
  wayland_fill_rect (f, color, 0, height - border, width, border);
}

/* Installed on a test terminal only.  The runner makes an event
   ready before drawing resumes, so this needs just one nonblocking
   pass through the ordinary service routine.  */
static struct redisplay_interface wayland_test_redisplay_interface;

static void
wayland_test_draw_glyph_string (struct glyph_string *s)
{
  struct wayland_display_info *dpyinfo = FRAME_DISPLAY_INFO (s->f);
  struct wayland_private *priv = private_of (dpyinfo);
  struct wayland_surface_state *ss = surface_state (s->f);

  wayland_draw_glyph_string (s);
  if (!ss || ss->update_depth <= 0 || !redisplaying_p
      || priv->debug_draws_remaining <= 0
      || --priv->debug_draws_remaining > 0)
    return;

  dpyinfo->terminal->rif = &wayland_redisplay_interface;
  if (priv->debug_interrupt_draw)
    Fthrow (Qwayland__test_interrupted_draw, Qt);

  enum wayland_display_state state = dpyinfo->state;
  uint32_t serial = ss->configure_serial;
  uint64_t releases = priv->releases;
  wayland_service (dpyinfo, false);
  if (state == WAYLAND_DISPLAY_LIVE && dpyinfo->state == WAYLAND_DISPLAY_DEAD)
    fputs ("emacs: Wayland test detected loss during glyph drawing\n", stderr);
  if (serial != ss->configure_serial && releases < priv->releases)
    fputs ("emacs: Wayland test handled release and configure"
	   " during drawing\n", stderr);
}

/* Scroll part of a window by copying rows of the target.  The copy
   is overlap-safe: rows move in the direction that never overwrites
   rows still to be copied.  */

static void
wayland_scroll_run (struct window *w, struct run *run)
{
  struct frame *f = XFRAME (WINDOW_FRAME (w));
  struct wayland_surface_state *ss = surface_state (f);
  int x, y, width, height, from_y, to_y, bottom_y;

  window_box (w, ANY_AREA, &x, &y, &width, &height);
  from_y = WINDOW_TO_FRAME_PIXEL_Y (w, run->current_y);
  to_y = WINDOW_TO_FRAME_PIXEL_Y (w, run->desired_y);
  bottom_y = y + height;
  if (to_y < from_y)
    height = (from_y + run->height > bottom_y
	      ? bottom_y - from_y : run->height);
  else
    height = (to_y + run->height > bottom_y
	      ? bottom_y - to_y : run->height);

  block_input ();
  gui_clear_cursor (w);
  /* Take the target, brought up to date, as any drawing does.  */
  wayland_begin_cr_clip (f);
  if (x < 0)
    {
      width += x;
      x = 0;
    }
  if (x + width > ss->canvas_width)
    width = ss->canvas_width - x;
  if (!ss->undrawn && width > 0 && height > 0
      && from_y >= 0 && to_y >= 0
      && from_y + height <= ss->canvas_height
      && to_y + height <= ss->canvas_height)
    {
      cairo_surface_t *image = ss->target->image;
      unsigned char *data = ss->target->data;
      int stride = ss->target->pool->stride;
      cairo_surface_flush (image);
      if (to_y < from_y)
	for (int i = 0; i < height; i++)
	  memmove (data + (to_y + i) * stride + x * 4,
		   data + (from_y + i) * stride + x * 4, width * 4);
      else
	for (int i = height - 1; i >= 0; i--)
	  memmove (data + (to_y + i) * stride + x * 4,
		   data + (from_y + i) * stride + x * 4, width * 4);
      cairo_surface_mark_dirty_rectangle (image, x, to_y, width, height);
      wayland_add_damage (ss, x, to_y, x + width, to_y + height);
      private_of (FRAME_DISPLAY_INFO (f))->scroll_bytes
        += (uint64_t) width * height * 4;
    }
  wayland_end_cr_clip (f);
  unblock_input ();
}

static void
wayland_after_update_window_line (struct window *w,
				  struct glyph_row *desired_row)
{
  if (!desired_row->mode_line_p && !w->pseudo_window_p)
    desired_row->redraw_fringe_bitmaps_p = true;
}

static void
wayland_define_frame_cursor (struct frame *f, Emacs_Cursor cursor)
{
  if (!cursor || FRAME_X_OUTPUT (f)->current_cursor == cursor)
    return;
  FRAME_X_OUTPUT (f)->current_cursor = cursor;
  wayland_update_pointer_cursor (f);
}

static void
wayland_flush_display (struct frame *f)
{
  wayland_flush_now (FRAME_DISPLAY_INFO (f));
}

static void
wayland_show_hourglass (struct frame *f)
{
  if (surface_state (f))
    {
      surface_state (f)->hourglass = true;
      wayland_update_pointer_cursor (f);
    }
}

static void
wayland_hide_hourglass (struct frame *f)
{
  if (surface_state (f))
    {
      surface_state (f)->hourglass = false;
      wayland_update_pointer_cursor (f);
    }
}

/* `make-pointer-invisible': a null cursor surface hides the pointer.  */

static void
wayland_toggle_invisible_pointer (struct frame *f, bool invisible)
{
  f->pointer_invisible = invisible;
  wayland_update_pointer_cursor (f);
}

struct redisplay_interface wayland_redisplay_interface =
  {
    wayland_frame_parm_handlers,
    gui_produce_glyphs,
    gui_write_glyphs,
    gui_insert_glyphs,
    gui_clear_end_of_line,
    wayland_scroll_run,
    wayland_after_update_window_line,
    NULL, /* update_window_begin */
    NULL, /* update_window_end */
    wayland_flush_display,
    gui_clear_window_mouse_face,
    gui_get_glyph_overhangs,
    gui_fix_overlapping_area,
    wayland_draw_fringe_bitmap,
    wayland_define_fringe_bitmap,
    wayland_destroy_fringe_bitmap,
    wayland_compute_glyph_string_overhangs,
    wayland_draw_glyph_string,
    wayland_define_frame_cursor,
    wayland_clear_frame_area,
    wayland_clear_under_internal_border,
    wayland_draw_window_cursor,
    wayland_draw_vertical_window_border,
    wayland_draw_window_divider,
    NULL, /* shift_glyphs_for_insert */
    wayland_show_hourglass,
    wayland_hide_hourglass,
    wayland_default_font_parameter,
  };

/* Terminal hooks for frames.  */

/* Drawing brackets.  update_begin marks the image incomplete; only
   frame_up_to_date marks it complete.  If buffer flips are unblocked
   while the image is still incomplete, redisplay unwound (an error, a
   quit or a throw) after drawing part of it: publish nothing and
   redraw in full.  */

static void
wayland_update_begin (struct frame *f)
{
  struct wayland_surface_state *ss = surface_state (f);

  if (!ss)
    return;
  if (frame_display_dead (f))
    return;
  /* A resize needs a full redraw, and taking the buffer of the new
     size asks for it.  So does a frame whose drawing was discarded,
     once it can have a buffer again: that buffer starts as the image
     on screen, which lacks what was discarded.  Otherwise wait until
     something actually draws before taking a pool slot.  */
  int width, height;
  wayland_canvas_size (f, &width, &height);
  if ((ss->undrawn || width != ss->canvas_width
       || height != ss->canvas_height)
      && wayland_ensure_target (f) && ss->undrawn)
    {
      ss->undrawn = false;
      SET_FRAME_GARBAGED (f);
    }
  if (ss->update_depth == 0)
    wayland_trace (FRAME_DISPLAY_INFO (f), WAYLAND_TRACE_BEGIN);
  ss->update_depth++;
  ss->incomplete = true;
  /* Highlighting waits until the matrices are current again.  */
  MOUSE_HL_INFO (f)->mouse_face_defer = true;
}

static void
wayland_update_end (struct frame *f)
{
  struct wayland_surface_state *ss = surface_state (f);

  if (ss && ss->update_depth > 0)
    ss->update_depth--;
  if (ss && ss->update_depth == 0)
    MOUSE_HL_INFO (f)->mouse_face_defer = false;
}

static void
wayland_frame_up_to_date (struct frame *f)
{
  struct wayland_surface_state *ss = surface_state (f);

  if (!ss)
    return;
  ss->update_depth = 0;
  ss->incomplete = false;
  ss->track_clear = false;
  wayland_trace (FRAME_DISPLAY_INFO (f), WAYLAND_TRACE_UP_TO_DATE);

  /* Redraw the mouse highlight the update may have removed, as
     XTframe_up_to_date does.  */
  Mouse_HLInfo *hlinfo = MOUSE_HL_INFO (f);
  hlinfo->mouse_face_defer = false;
  if (f == hlinfo->mouse_face_mouse_frame)
    {
      block_input ();
      note_mouse_highlight (f, hlinfo->mouse_face_mouse_x,
			    hlinfo->mouse_face_mouse_y);
      unblock_input ();
    }

  if (wayland_present (f))
    wayland_flush_display (f);
}

static void
wayland_buffer_flipping_unblocked (struct frame *f)
{
  struct wayland_surface_state *ss = surface_state (f);

  if (!ss)
    return;
  if (ss->incomplete)
    {
      /* Redisplay unwound after drawing part of the image.  It stays
	 incomplete, so no service or write callback can present it,
	 until frame_up_to_date marks the full redraw this requests as
	 complete.  */
      ss->update_depth = 0;
      ss->track_clear = false;
      SET_FRAME_GARBAGED (f);
      return;
    }
  /* Redisplay can queue this commit after its last flush.  Send it
     before Lisp enters a wait with no further Wayland events.  */
  if (wayland_present (f))
    wayland_flush_display (f);
}

static void
wayland_frame_visible_invisible (struct frame *f, bool visible)
{
  if (visible)
    wayland_make_frame_visible (f);
  else
    wayland_make_frame_invisible (f);
}

static void
wayland_iconify_frame (struct frame *f)
{
  struct wayland_surface_state *ss = surface_state (f);

  /* Wayland never reports a minimized state, so the frame stays
     visible as far as Emacs knows.  */
  if (ss && ss->toplevel && !frame_display_dead (f))
    {
      xdg_toplevel_set_minimized (ss->toplevel);
      wayland_flush_now (FRAME_DISPLAY_INFO (f));
    }
}

static void
wayland_set_window_size (struct frame *f, bool change_gravity, int width,
			 int height)
{
  struct wayland_surface_state *ss = surface_state (f);

  /* A floating toplevel's size is the client's choice.  An axis the
     latest configure fixes keeps the configured size, and the frame
     changes back to it.  */
  if (ss)
    {
      if (ss->changing_dpi)
        return;
      if (ss->subsurface)
	{
	  ss->client_width = ss->child_width = width;
	  ss->client_height = ss->child_height = height;
	  ss->child_size_scale = ss->applied_scale;
	}
      else
	{
	  if (!wayland_axis_fixed (ss, false))
	    ss->client_width = ss->logical_width
	      = wayland_logical_size (width, ss->applied_scale);
	  if (!wayland_axis_fixed (ss, true))
	    ss->client_height = ss->logical_height
	      = wayland_logical_size (height, ss->applied_scale);
	  width = wayland_physical_size (ss->logical_width, ss->applied_scale);
	  height = wayland_physical_size (ss->logical_height, ss->applied_scale);
	}
    }
  change_frame_size (f, width, height, false, true, false);
  SET_FRAME_GARBAGED (f);
}

static void
wayland_set_offset (struct frame *f, int xoff, int yoff, int change_gravity)
{
  struct wayland_surface_state *ss = surface_state (f);
  if (ss && ss->subsurface && !frame_display_dead (f))
    {
      f->left_pos = xoff;
      f->top_pos = yoff;
      ss->requested_x = wayland_scale_offset (xoff, ss->applied_scale,
					     WAYLAND_SCALE_UNIT);
      ss->requested_y = wayland_scale_offset (yoff, ss->applied_scale,
					     WAYLAND_SCALE_UNIT);
      ss->position_pending = true;
      wayland_mark_parent (FRAME_PARENT_FRAME (f));
    }
}

static void
wayland_mouse_position (struct frame **fp, int insist, Lisp_Object *bar_window,
			enum scroll_bar_part *part, Lisp_Object *x,
			Lisp_Object *y, Time *timestamp)
{
  struct wayland_display_info *dpyinfo = FRAME_DISPLAY_INFO (*fp);
  struct frame *f1 = dpyinfo->last_mouse_motion_frame;
  Lisp_Object tail, frame;

  block_input ();
  FOR_EACH_FRAME (tail, frame)
    if (FRAME_WAYLAND_P (XFRAME (frame))
	&& FRAME_DISPLAY_INFO (XFRAME (frame)) == dpyinfo)
      XFRAME (frame)->mouse_moved = false;
  dpyinfo->last_mouse_scroll_bar = NULL;

  /* Pointer coordinates are relative to the surface under the pointer,
     which stays the one pressed during a button grab, so the frame is
     the one of the last motion.  Outside every frame, report nothing
     unless the caller insists.  */
  if (!f1 || !FRAME_LIVE_P (f1) || !FRAME_WAYLAND_P (f1)
      || (!insist && !gui_mouse_grabbed (dpyinfo)
	  && !wayland_pointer_frame (dpyinfo)))
    {
      unblock_input ();
      return;
    }
  *fp = f1;
  XSETINT (*x, dpyinfo->last_mouse_motion_x);
  XSETINT (*y, dpyinfo->last_mouse_motion_y);
  *bar_window = Qnil;
  *part = scroll_bar_above_handle;
  *timestamp = dpyinfo->last_mouse_movement_time;
  remember_mouse_glyph (f1, dpyinfo->last_mouse_motion_x,
			dpyinfo->last_mouse_motion_y,
			&dpyinfo->last_mouse_glyph);
  dpyinfo->last_mouse_glyph_frame = f1;
  unblock_input ();
}

static Lisp_Object
wayland_get_focus_frame (struct frame *f)
{
  struct frame *focus = FRAME_DISPLAY_INFO (f)->x_focus_frame;
  Lisp_Object frame = Qnil;

  if (focus)
    XSETFRAME (frame, focus);
  return frame;
}

static void
wayland_activate (struct frame *f, const char *token)
{
  struct wayland_surface_state *ss = surface_state (f);
  if (!*token)
    return;
  if (ss->mapped)
    xdg_activation_v1_activate (globals_of (f)->activation, token, ss->surface);
  else
    {
      xfree (ss->activation_token);
      ss->activation_token = xstrdup (token);
    }
}

static void
wayland_activation_done (void *data, struct xdg_activation_token_v1 *proxy,
			 const char *token)
{
  struct wayland_private *priv = data;
  if (priv->activation_frame)
    wayland_activate (priv->activation_frame, token);
  wayland_forget_activation (priv);
}

static const struct xdg_activation_token_v1_listener activation_listener =
  {.done = wayland_activation_done};

static void
wayland_request_activation (struct frame *f)
{
  struct frame *root = wayland_frame_root (f);
  struct wayland_display_info *dpyinfo = FRAME_DISPLAY_INFO (root);
  struct wayland_private *priv = private_of (dpyinfo);
  if (!priv->globals.activation || frame_display_dead (root))
    return;
  block_input ();
  Lisp_Object token = get_frame_param (f, Qactivation_token);
  if (STRINGP (token))
    {
      wayland_forget_activation (priv);
      wayland_activate (root, SSDATA (token));
      store_frame_param (f, Qactivation_token, Qnil);
    }
  else if (priv->activation_frame != root
	   && !surface_state (root)->activation_token
	   && priv->seat.keyboard_focus != surface_state (root)->surface)
    {
      wayland_forget_activation (priv);
      struct xdg_activation_token_v1 *request
	= xdg_activation_v1_get_activation_token (priv->globals.activation);
      if (request)
	{
	  priv->activation_token = request;
	  priv->activation_frame = root;
	  xdg_activation_token_v1_add_listener (request, &activation_listener,
					       priv);
	  if (priv->seat.last_input_serial)
	    xdg_activation_token_v1_set_serial
	      (request, priv->seat.last_input_serial, priv->globals.seat);
	  if (priv->seat.keyboard_focus)
	    xdg_activation_token_v1_set_surface (request,
						 priv->seat.keyboard_focus);
	  xdg_activation_token_v1_set_app_id (request, "emacs");
	  xdg_activation_token_v1_commit (request);
	}
    }
  wayland_flush_now (dpyinfo);
  unblock_input ();
}

static void
wayland_focus_frame (struct frame *f, bool noactivate)
{
  struct wayland_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);
  struct wayland_seat *seat = &private_of (dpyinfo)->seat;
  if (FRAME_VISIBLE_P (f) && !FRAME_NO_ACCEPT_FOCUS (f)
      && wayland_surface_frame (dpyinfo, seat->keyboard_focus)
	 == wayland_frame_root (f))
    wayland_new_focus (dpyinfo, f);
  if (!noactivate && !FRAME_NO_ACCEPT_FOCUS (f))
    wayland_request_activation (f);
}

static void
wayland_frame_rehighlight (struct frame *f)
{
  wayland_rehighlight (FRAME_DISPLAY_INFO (f));
}

static void
wayland_frame_raise_lower (struct frame *f, bool raise_flag)
{
  struct frame *parent = FRAME_PARENT_FRAME (f);
  if (!parent)
    {
      if (raise_flag)
	wayland_request_activation (f);
      return;
    }
  wayland_restack_child (f, raise_flag);
}

static void
wayland_fullscreen_hook (struct frame *f)
{
  struct wayland_surface_state *ss = surface_state (f);
  if (ss && ss->initial_commit_done && !frame_display_dead (f))
    {
      wayland_send_toplevel_mode (ss);
      wayland_flush_display (f);
    }
}

static void
wayland_ring_bell (struct frame *f)
{
}

static bool
wayland_bitmap_icon (struct frame *f, Lisp_Object file)
{
  return false;
}

static void
wayland_set_frame_hooks (struct terminal *terminal)
{
  terminal->clear_frame_hook = wayland_clear_frame;
  terminal->ring_bell_hook = wayland_ring_bell;
  terminal->update_begin_hook = wayland_update_begin;
  terminal->update_end_hook = wayland_update_end;
  terminal->frame_up_to_date_hook = wayland_frame_up_to_date;
  terminal->buffer_flipping_unblocked_hook
    = wayland_buffer_flipping_unblocked;
  terminal->defined_color_hook = wayland_defined_color;
  terminal->query_frame_background_color
    = wayland_query_frame_background_color;
  terminal->query_colors = wayland_query_colors;
  terminal->free_pixmap = wayland_free_pixmap;
  terminal->mouse_position_hook = wayland_mouse_position;
  terminal->toggle_invisible_pointer_hook = wayland_toggle_invisible_pointer;
  terminal->get_focus_frame = wayland_get_focus_frame;
  terminal->focus_frame_hook = wayland_focus_frame;
  terminal->frame_rehighlight_hook = wayland_frame_rehighlight;
  terminal->frame_raise_lower_hook = wayland_frame_raise_lower;
  terminal->frame_visible_invisible_hook = wayland_frame_visible_invisible;
  terminal->fullscreen_hook = wayland_fullscreen_hook;
  terminal->iconify_frame_hook = wayland_iconify_frame;
  terminal->set_window_size_hook = wayland_set_window_size;
  terminal->set_frame_offset_hook = wayland_set_offset;
  terminal->set_new_font_hook = wayland_new_font;
  terminal->set_bitmap_icon_hook = wayland_bitmap_icon;
  terminal->implicit_set_name_hook = wayland_implicitly_set_name;
  terminal->change_tab_bar_height_hook = wayland_change_tab_bar_height;
  terminal->change_tool_bar_height_hook = wayland_change_tool_bar_height;
  terminal->delete_frame_hook = wayland_delete_frame;
}

/* Return the name of KEYSYM.  xkbcommon keysyms have the same values as
   X keysyms, so `lispy_function_keys' and this agree.  */

char *
get_keysym_name (int keysym)
{
  static char name[64];

  if (xkb_keysym_get_name (keysym, name, sizeof name) < 0)
    name[0] = '\0';
  return name;
}

/* Wayland clients cannot move the pointer, so `set-mouse-position' and
   friends do nothing.  */

void
frame_set_mouse_pixel_position (struct frame *f, int pix_x, int pix_y)
{
}

/* Lisp interface for tests and diagnosis.  */

static struct wayland_display_info *
decode_wayland_display (Lisp_Object terminal)
{
  struct terminal *t;

  if (NILP (terminal) && x_display_list)
    return x_display_list;
  t = decode_live_terminal (terminal);
  if (t->type != output_wayland || !t->display_info.wayland)
    error ("Not a Wayland display");
  return t->display_info.wayland;
}

DEFUN ("wayland--trace", Fwayland__trace, Swayland__trace, 1, 2, 0,
       doc: /* Start or collect the diagnostic timing trace of TERMINAL.
With START non-nil, discard any entries and start recording.  Otherwise
stop and return the entries as a vector of [KIND NS GCS], where KIND
names the stage, NS is CLOCK_MONOTONIC in nanoseconds and GCS the value
of `gcs-done'.  Only the latest entries are kept.  For diagnosis; the
stages may change.  */)
  (Lisp_Object terminal, Lisp_Object start)
{
  struct wayland_display_info *dpyinfo = decode_wayland_display (terminal);
  struct wayland_private *priv = private_of (dpyinfo);

  if (!NILP (start))
    {
      if (!priv->trace)
	priv->trace = xmalloc (WAYLAND_TRACE_SIZE * sizeof *priv->trace);
      priv->trace_head = priv->trace_count = 0;
      return Qnil;
    }

  Lisp_Object entries = make_nil_vector (priv->trace_count);
  for (int i = 0; i < priv->trace_count; i++)
    {
      struct wayland_trace_entry *entry
	= &priv->trace[(priv->trace_head + i) % WAYLAND_TRACE_SIZE];
      ASET (entries, i,
	    CALLN (Fvector, build_string (wayland_trace_names[entry->kind]),
		   make_int (entry->ns), make_int (entry->gcs)));
    }
  xfree (priv->trace);
  priv->trace = NULL;
  priv->trace_count = 0;
  return entries;
}

DEFUN ("wayland--presentation-feedback", Fwayland__presentation_feedback,
       Swayland__presentation_feedback, 0, 2, 0,
       doc: /* Return presentation feedback counters on TERMINAL.
With ENABLE non-nil, request feedback for future content commits.
Return [REQUESTED PRESENTED DISCARDED PENDING SKIPPED], or nil if the
compositor lacks the presentation-time protocol.  For benchmarks.  */)
  (Lisp_Object enable, Lisp_Object terminal)
{
  struct wayland_private *priv = private_of (decode_wayland_display (terminal));
  if (!priv->globals.presentation)
    return Qnil;
  if (!NILP (enable))
    priv->presentation_enabled = true;
  return CALLN (Fvector, make_uint (priv->presentation_requested),
		make_uint (priv->presented), make_uint (priv->discarded),
		make_uint (priv->presentation_count),
		make_uint (priv->presentation_skipped));
}

DEFUN ("wayland--metrics", Fwayland__metrics, Swayland__metrics, 0, 1, 0,
       doc: /* Return the Wayland backend's counters for TERMINAL as a plist.
TERMINAL defaults to the first Wayland display.  For tests and diagnosis;
the keys may change.  */)
  (Lisp_Object terminal)
{
  struct wayland_display_info *dpyinfo = decode_wayland_display (terminal);
  struct wayland_private *priv = private_of (dpyinfo);
  struct wayland_metrics *m = &dpyinfo->metrics;
  Lisp_Object globals = Qnil, keycodes = Qnil;

  for (int i = N_GLOBALS - 1; i >= 0; i--)
    if (priv->binding.bound[i])
      globals = Fcons (Fcons (build_string (global_specs[i].interface->name),
			      make_fixnum (priv->binding.bound[i])),
		       globals);
  for (int i = WAYLAND_RAW_KEYCODES - 1; i >= 0; i--)
    if (priv->seat.raw_keycodes[i])
      keycodes = Fcons (Fcons (make_fixnum (i),
			      make_uint (priv->seat.raw_keycodes[i])),
			keycodes);

  return list (QChook_calls, make_uint (m->hook_calls),
	       QCservice_calls, make_uint (m->service_calls),
	       QCwrite_callbacks, make_uint (m->write_callbacks),
	       QCpings, make_uint (m->pings),
	       QCraw_keys, make_uint (m->raw_keys),
	       QCraw_keycodes, keycodes,
	       QCflush_eagain, make_uint (m->flush_eagain),
	       QCflush_epipe, make_uint (m->flush_epipe),
	       QCoutput_resumed, make_uint (m->output_resumed),
	       QCwrite_resumes, make_uint (m->write_resumes),
	       QCfinalize_deferrals, make_uint (m->finalize_deferrals),
	       QCpool_bytes, make_uint (priv->pool_bytes),
	       QCretired_bytes, make_uint (priv->retired_bytes),
	       QCcharged_pool_bytes, make_uint (priv->charged_pool_bytes),
	       QCpool_budget, make_uint (priv->pool_budget),
	       QCrepair_bytes, make_uint (priv->repair_bytes),
	       QCfull_repairs, make_uint (priv->full_repairs),
	       QCdiscards, make_uint (priv->discards),
	       QClast_buffer_age, make_uint (priv->last_buffer_age),
	       QClast_release_ns, make_uint (priv->last_release_ns),
	       QCscroll_bytes, make_uint (priv->scroll_bytes),
	       QCack_only_commits, make_uint (priv->ack_only_commits),
	       QCbackground_commits, make_uint (priv->background_commits),
	       QCmax_hook_ns, make_int (m->max_hook_ns),
	       QCoutput, (dpyinfo->output_state == WAYLAND_OUTPUT_PENDING
			  ? Qpending : Qidle),
	       QCstate, (dpyinfo->state == WAYLAND_DISPLAY_DEAD
			 ? Qdead : Qlive),
	       QCdeath_message, (dpyinfo->death_message[0]
				 ? build_string (dpyinfo->death_message)
				 : Qnil),
	       QCkeymap, priv->seat.keymap ? Qt : Qnil,
	       QCmouse_face_active,
	       dpyinfo->mouse_highlight.mouse_face_mouse_frame ? Qt : Qnil,
	       QCdropped_events, make_uint (priv->dropped_events),
	       QCglobals, globals);
}

DEFUN ("wayland--debug-fail-pool", Fwayland__debug_fail_pool,
       Swayland__debug_fail_pool, 1, 2, 0,
       doc: /* Make FRAME's buffer pool allocations fail if FAIL is non-nil.
For tests.  With FAIL nil they work again, and a frame whose drawing
was discarded meanwhile is redrawn.  */)
  (Lisp_Object fail, Lisp_Object frame)
{
  struct frame *f = decode_window_system_frame (frame);
  struct wayland_surface_state *ss = surface_state (f);
  ss->fail_pool = !NILP (fail);
  /* Nothing else tells the frame that it can have a buffer again.  */
  if (!ss->fail_pool && ss->undrawn)
    SET_FRAME_GARBAGED (f);
  return Qnil;
}

DEFUN ("wayland--debug-request-burst", Fwayland__debug_request_burst,
       Swayland__debug_request_burst, 1, 3, 0,
       doc: /* Send COUNT requests to TERMINAL and flush once.
Return the output state afterwards, `idle' or `pending'.  For tests: with
the compositor not reading, this fills the socket and makes output
pending.

If KIND is `sync', send `wl_display.sync' requests, each of which the
compositor answers with an event.  Otherwise send `wl_region.add'
requests, which it never answers, so only the write callback can resume
output.  */)
  (Lisp_Object count, Lisp_Object kind, Lisp_Object terminal)
{
  struct wayland_display_info *dpyinfo = decode_wayland_display (terminal);
  struct wayland_private *priv = private_of (dpyinfo);
  EMACS_INT n = check_integer_range (count, 1, 1000000);

  if (dpyinfo->state == WAYLAND_DISPLAY_DEAD)
    error ("The Wayland connection is lost");
  if (EQ (kind, Qsync))
    for (EMACS_INT i = 0; i < n; i++)
      {
	static bool ignored;
	struct wl_callback *callback = wl_display_sync (dpyinfo->display);
	wl_callback_add_listener (callback, &roundtrip_listener, &ignored);
      }
  else
    {
      /* Reuse the region.  Destroying it would send a delete_id event
	 and give the service fd a chance to drive the flush.  */
      if (!priv->debug_region)
	priv->debug_region
	  = wl_compositor_create_region (priv->globals.compositor);
      if (!priv->debug_region)
	error ("Could not create a Wayland test region");
      for (EMACS_INT i = 0; i < n; i++)
	wl_region_add (priv->debug_region, 0, 0, 1, 1);
    }
  if (wayland_flush (dpyinfo) == FLUSH_BROKEN)
    wayland_read_after_broken_flush (dpyinfo);
  return (dpyinfo->output_state == WAYLAND_OUTPUT_PENDING
	  ? Qpending : Qidle);
}

static void
wayland_arm_test_draw (Lisp_Object count, Lisp_Object terminal, bool interrupt)
{
  struct wayland_display_info *dpyinfo = decode_wayland_display (terminal);
  struct wayland_private *priv = private_of (dpyinfo);
  if (dpyinfo->state == WAYLAND_DISPLAY_DEAD)
    error ("The Wayland connection is lost");
  priv->debug_draws_remaining = check_integer_range (count, 1, INT_MAX);
  priv->debug_interrupt_draw = interrupt;
  wayland_test_redisplay_interface = wayland_redisplay_interface;
  wayland_test_redisplay_interface.draw_glyph_string
    = wayland_test_draw_glyph_string;
  dpyinfo->terminal->rif = &wayland_test_redisplay_interface;
}

DEFUN ("wayland--debug-dispatch-after-draw",
       Fwayland__debug_dispatch_after_draw,
       Swayland__debug_dispatch_after_draw, 1, 2, 0,
       doc: /* Service TERMINAL once after COUNT glyph drawing calls.
For tests.  The compositor must make its event ready before the draw.  */)
  (Lisp_Object count, Lisp_Object terminal)
{
  wayland_arm_test_draw (count, terminal, false);
  return Qnil;
}

DEFUN ("wayland--debug-interrupt-after-draw",
       Fwayland__debug_interrupt_after_draw,
       Swayland__debug_interrupt_after_draw, 1, 2, 0,
       doc: /* Interrupt TERMINAL after COUNT glyph drawing calls.
For tests.  Throw to `wayland--test-interrupted-draw'.  */)
  (Lisp_Object count, Lisp_Object terminal)
{
  wayland_arm_test_draw (count, terminal, true);
  return Qnil;
}

DEFUN ("wayland--dump-frame", Fwayland__dump_frame, Swayland__dump_frame,
       1, 2, 0,
       doc: /* Dump FRAME's backing image to DIRECTORY on every content commit.
FRAME defaults to the selected frame.  DIRECTORY must already exist.
Pass nil to stop dumping.  Return the surface ID and last commit number.  */)
  (Lisp_Object directory, Lisp_Object frame)
{
  struct frame *f = decode_window_system_frame (frame);
  struct wayland_surface_state *ss = surface_state (f);
  if (!ss || frame_display_dead (f))
    error ("No live Wayland surface to dump");
  char *path = NULL;
  if (!NILP (directory))
    {
      CHECK_STRING (directory);
      path = xlispstrdup (ENCODE_FILE (Fexpand_file_name (directory, Qnil)));
    }
  xfree (ss->dump_directory);
  ss->dump_directory = path;
  uint32_t id = wl_proxy_get_id ((struct wl_proxy *) ss->surface);
  return list2 (make_uint (id), make_uint (ss->content_commits));
}

static const char debug_mime[] = "application/x-emacs-wayland-test";

static void
debug_source_target (void *data, struct wl_data_source *source,
		     const char *mime)
{
}

static void
debug_source_send (void *data, struct wl_data_source *source,
		   const char *mime, int fd)
{
  static const char payload[] = "emacs-wayland-m0";
  int flags = fcntl (fd, F_GETFL);
  /* This fixed payload fits in PIPE_BUF.  Never wait on the recipient
     or enter Lisp from dispatch.  Its reader observes either all the
     bytes or EOF, even if the Wayland socket is full.  */
  if (!strcmp (mime, debug_mime) && flags >= 0
      && fcntl (fd, F_SETFL, flags | O_NONBLOCK) == 0)
    {
      ssize_t result;
      do
	result = write (fd, payload, sizeof payload - 1);
      while (result < 0 && errno == EINTR);
    }
  emacs_close (fd);
}

static void
debug_source_cancelled (void *data, struct wl_data_source *source)
{
  struct wayland_private *priv = data;
  priv->debug_source = NULL;
  wl_data_source_destroy (source);
}

static const struct wl_data_source_listener debug_source_listener =
  {
    .target = debug_source_target,
    .send = debug_source_send,
    .cancelled = debug_source_cancelled,
  };

DEFUN ("wayland-mouse-absolute-pixel-position",
       Fwayland_mouse_absolute_pixel_position,
       Swayland_mouse_absolute_pixel_position, 0, 0, 0,
       doc: /* Return the position of the mouse pointer in pixels.
The position is a cons cell (X . Y), relative to the top left corner of
the root frame of the frame under the pointer: Wayland has no screen
coordinates.  While a button is held, it stays relative to the frame
where the button was pressed, also outside it.  */)
  (void)
{
  struct frame *sf = SELECTED_FRAME ();
  int x = 0, y = 0;

  if (FRAME_WAYLAND_P (sf))
    {
      struct wayland_display_info *dpyinfo = FRAME_DISPLAY_INFO (sf);
      struct frame *f = dpyinfo->last_mouse_motion_frame;

      if (f && FRAME_LIVE_P (f) && surface_state (f))
	{
	  /* The compositor reports the pointer relative to where it
	     showed F when it last chose the surface under the pointer:
	     at the press while buttons are held, and otherwise where F
	     was last sent.  F's own left and top change at once when
	     Lisp moves it.  */
	  struct wayland_private *priv = private_of (dpyinfo);
	  struct frame *root = wayland_frame_root (f);
	  int scale = surface_state (f)->applied_scale;
	  int root_scale = surface_state (root)->applied_scale;
	  int left, top;

	  if (dpyinfo->grabbed && f == dpyinfo->last_mouse_frame)
	    left = priv->press_left, top = priv->press_top;
	  else
	    wayland_frame_root_position (f, &left, &top);
	  x = (wayland_scale_offset (dpyinfo->last_mouse_motion_x, scale,
				     root_scale)
	       + wayland_scale_offset (left, WAYLAND_SCALE_UNIT, root_scale));
	  y = (wayland_scale_offset (dpyinfo->last_mouse_motion_y, scale,
				     root_scale)
	       + wayland_scale_offset (top, WAYLAND_SCALE_UNIT, root_scale));
	}
    }
  return Fcons (make_fixnum (x), make_fixnum (y));
}

DEFUN ("wayland--debug-sigio", Fwayland__debug_sigio,
       Swayland__debug_sigio, 1, 2, 0,
       doc: /* Turn SIGIO from TERMINAL's socket on if ENABLE, off otherwise.
For tests of the paths that must work without it: the service and write
descriptors, and loss detected by the routine under test.  */)
  (Lisp_Object enable, Lisp_Object terminal)
{
  struct wayland_display_info *dpyinfo = decode_wayland_display (terminal);
  int flags = fcntl (dpyinfo->connection, F_GETFL);

  if (flags >= 0)
    fcntl (dpyinfo->connection, F_SETFL,
	   NILP (enable) ? flags & ~O_ASYNC : flags | O_ASYNC);
  return Qnil;
}

DEFUN ("wayland--debug-selection", Fwayland__debug_selection,
       Swayland__debug_selection, 0, 1, 0,
       doc: /* Publish a fixed test payload on TERMINAL's selection.
The test compositor requests it on a pipe.  Use only with mockcomp.  */)
  (Lisp_Object terminal)
{
  struct wayland_display_info *dpyinfo = decode_wayland_display (terminal);
  struct wayland_private *priv = private_of (dpyinfo);
  struct wl_data_device_manager *manager = priv->globals.data_device_manager;
  if (dpyinfo->state == WAYLAND_DISPLAY_DEAD
      || !manager || !priv->globals.seat)
    error ("Test selection requires a live Wayland seat and data device");
  if (priv->debug_source)
    return Qnil;
  if (!priv->debug_device)
    priv->debug_device = wl_data_device_manager_get_data_device
      (manager, priv->globals.seat);
  if (!priv->debug_device)
    error ("Could not create a Wayland test data device");
  priv->debug_source = wl_data_device_manager_create_data_source (manager);
  if (!priv->debug_source)
    error ("Could not create a Wayland test selection source");
  wl_data_source_add_listener (priv->debug_source, &debug_source_listener,
			       priv);
  wl_data_source_offer (priv->debug_source, debug_mime);
  wl_data_device_set_selection (priv->debug_device, priv->debug_source,
				priv->seat.last_input_serial);
  if (wayland_flush (dpyinfo) == FLUSH_BROKEN)
    wayland_read_after_broken_flush (dpyinfo);
  return Qnil;
}

void
mark_waylandterm (void)
{
  for (struct wayland_display_info *dpyinfo = x_display_list; dpyinfo;
       dpyinfo = dpyinfo->next)
    {
      mark_object (dpyinfo->name_list_element);
      mark_object (dpyinfo->color_map);
      wayland_selection_mark (dpyinfo);

      /* Queued events, as mark_kboards marks the keyboard buffer's.
	 Strings and preedit text can stay queued across a wait.  */
      struct wayland_private *priv = private_of (dpyinfo);
      for (int i = 0; priv && i < priv->event_count; i++)
	{
	  union buffered_input_event *event
	    = &priv->events[(priv->event_head + i) % WAYLAND_EVENT_QUEUE_SIZE];
	  mark_object (event->ie.x);
	  mark_object (event->ie.y);
	  mark_object (event->ie.frame_or_window);
	  mark_object (event->ie.arg);
	  mark_object (event->ie.device);
	}
    }
}

void
syms_of_waylandterm (void)
{
  Fprovide (Qwayland, Qnil);

  wayland_axis_devices = make_nil_vector (3);
  staticpro (&wayland_axis_devices);

  DEFSYM (Qwayland__finalize_display, "wayland--finalize-display");
  DEFSYM (Qactivation_token, "activation-token");
  DEFSYM (Qmodifier_value, "modifier-value");
  DEFSYM (Qwayland__apply_scale, "wayland--apply-scale");
  DEFSYM (Qwayland__test_interrupted_draw, "wayland--test-interrupted-draw");
  DEFSYM (Qpending, "pending");
  DEFSYM (Qidle, "idle");
  DEFSYM (Qsync, "sync");
  DEFSYM (Qdead, "dead");
  DEFSYM (Qlive, "live");
  DEFSYM (QChook_calls, ":hook-calls");
  DEFSYM (QCservice_calls, ":service-calls");
  DEFSYM (QCwrite_callbacks, ":write-callbacks");
  DEFSYM (QCpings, ":pings");
  DEFSYM (QCraw_keys, ":raw-keys");
  DEFSYM (QCraw_keycodes, ":raw-keycodes");
  DEFSYM (QCflush_eagain, ":flush-eagain");
  DEFSYM (QCflush_epipe, ":flush-epipe");
  DEFSYM (QCoutput_resumed, ":output-resumed");
  DEFSYM (QCwrite_resumes, ":write-resumes");
  DEFSYM (QCfinalize_deferrals, ":finalize-deferrals");
  DEFSYM (QCpool_bytes, ":pool-bytes");
  DEFSYM (QCretired_bytes, ":retired-bytes");
  DEFSYM (QCcharged_pool_bytes, ":charged-pool-bytes");
  DEFSYM (QCpool_budget, ":pool-budget");
  DEFSYM (QCrepair_bytes, ":repair-bytes");
  DEFSYM (QCfull_repairs, ":full-repairs");
  DEFSYM (QCdiscards, ":discards");
  DEFSYM (QClast_buffer_age, ":last-buffer-age");
  DEFSYM (QClast_release_ns, ":last-release-ns");
  DEFSYM (QCscroll_bytes, ":scroll-bytes");
  DEFSYM (QCack_only_commits, ":ack-only-commits");
  DEFSYM (QCbackground_commits, ":background-commits");
  DEFSYM (QCmax_hook_ns, ":max-hook-ns");
  DEFSYM (QCoutput, ":output");
  DEFSYM (QCstate, ":state");
  DEFSYM (QCdeath_message, ":death-message");
  DEFSYM (QCkeymap, ":keymap");
  DEFSYM (QCmouse_face_active, ":mouse-face-active");
  DEFSYM (QCdropped_events, ":dropped-events");
  DEFSYM (QCglobals, ":globals");

  DEFVAR_BOOL ("x-use-underline-position-properties",
	       x_use_underline_position_properties,
     doc: /* SKIP: real doc in xterm.c.  */);
  x_use_underline_position_properties = true;

  DEFVAR_LISP ("x-ctrl-keysym", Vx_ctrl_keysym,
    doc: /* SKIP: real doc in xterm.c.  */);
  Vx_ctrl_keysym = Qnil;

  DEFVAR_LISP ("x-alt-keysym", Vx_alt_keysym,
    doc: /* SKIP: real doc in xterm.c.  */);
  Vx_alt_keysym = Qnil;

  DEFVAR_LISP ("x-hyper-keysym", Vx_hyper_keysym,
    doc: /* SKIP: real doc in xterm.c.  */);
  Vx_hyper_keysym = Qnil;

  DEFVAR_LISP ("x-meta-keysym", Vx_meta_keysym,
    doc: /* SKIP: real doc in xterm.c.  */);
  Vx_meta_keysym = Qnil;

  DEFVAR_LISP ("x-super-keysym", Vx_super_keysym,
    doc: /* SKIP: real doc in xterm.c.  */);
  Vx_super_keysym = Qnil;

  /* Continuous scrolling follows X, so honor its scale too.  */
  DEFVAR_LISP ("x-scroll-event-delta-factor", Vx_scroll_event_delta_factor,
    doc: /* SKIP: real doc in xterm.c.  */);
  Vx_scroll_event_delta_factor = make_float (1.0);

  /* Only defined so loadup.el loads scroll-bar.el, as on Android:
     there are no scroll bars, but `scroll-bar-mode' must exist.  */
  DEFVAR_LISP ("x-toolkit-scroll-bars", Vx_toolkit_scroll_bars,
    doc: /* SKIP: real doc in xterm.c.  */);
  Vx_toolkit_scroll_bars = Qnil;

  DEFVAR_BOOL ("x-underline-at-descent-line",
	       x_underline_at_descent_line,
     doc: /* SKIP: real doc in xterm.c.  */);
  x_underline_at_descent_line = false;

  defsubr (&Swayland__finalize_display);
  defsubr (&Swayland__apply_scale);
  defsubr (&Swayland_display_monitor_attributes_list);
  defsubr (&Swayland__metrics);
  defsubr (&Swayland__presentation_feedback);
  defsubr (&Swayland__trace);
  defsubr (&Swayland__debug_fail_pool);
  defsubr (&Swayland__debug_request_burst);
  defsubr (&Swayland__debug_dispatch_after_draw);
  defsubr (&Swayland__debug_interrupt_after_draw);
  defsubr (&Swayland__dump_frame);
  defsubr (&Swayland__debug_selection);
  defsubr (&Swayland__debug_sigio);
  defsubr (&Swayland_mouse_absolute_pixel_position);
}
