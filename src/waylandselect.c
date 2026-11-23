/* Clipboard and primary selection for the native Wayland backend.

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

#include <config.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "lisp.h"
#include "blockinput.h"
#include "frame.h"
#include "process.h"
#include "waylandterm.h"

#include <wayland-client.h>
#if __GNUC__ >= 15
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wtrailing-whitespace"
#endif
#include "primary-selection-unstable-v1-client-protocol.h"
#if __GNUC__ >= 15
# pragma GCC diagnostic pop
#endif

enum { MAX_SELECTION_TRANSFERS = 32 };

struct selection_source
{
  struct wayland_selection *selection;
  struct wl_data_source *clipboard;
  struct zwp_primary_selection_source_v1 *primary;
  struct wl_callback *barrier;
  struct selection_source *next;
  Lisp_Object payloads;
};

struct selection_mime
{
  struct selection_mime *next;
  char name[FLEXIBLE_ARRAY_MEMBER];
};

struct selection_offer
{
  struct wl_data_offer *clipboard;
  struct zwp_primary_selection_offer_v1 *primary;
  struct selection_mime *mimes;
};

struct selection_slot
{
  struct selection_source *source;
  struct selection_offer *offer, *announced;
};

struct selection_transfer
{
  int fd;
  bool watching;
  int64_t deadline;
  ptrdiff_t written;
  Lisp_Object bytes;
};

struct wayland_selection
{
  struct wayland_display_info *display;
  struct wl_data_device_manager *clipboard_manager;
  struct zwp_primary_selection_device_manager_v1 *primary_manager;
  struct wl_data_device *clipboard;
  struct zwp_primary_selection_device_v1 *primary;
  struct selection_slot slots[2];
  struct selection_source *retired;
  struct selection_transfer transfers[MAX_SELECTION_TRANSFERS];
  int timer;
};

static int64_t
selection_now (void)
{
  struct timespec now;
  clock_gettime (CLOCK_MONOTONIC, &now);
  int64_t seconds = now.tv_sec;
  return seconds * 1000 + now.tv_nsec / 1000000;
}

static int64_t
selection_deadline (void)
{
  return selection_now () + clip_to_bounds (1, wayland_selection_timeout,
					     INT_MAX);
}

static void
close_transfer (struct selection_transfer *transfer)
{
  if (transfer->watching)
    delete_write_fd (transfer->fd);
  emacs_close (transfer->fd);
  transfer->fd = -1;
  transfer->watching = false;
  transfer->bytes = Qnil;
}

static void
selection_write_ready (int fd, void *data)
{
  struct selection_transfer *transfer = data;
  ptrdiff_t size = SBYTES (transfer->bytes);
  if (transfer->deadline > selection_now () && transfer->written < size)
    {
      ssize_t n;
      do
	n = write (fd, SDATA (transfer->bytes) + transfer->written,
		   size - transfer->written);
      while (n < 0 && errno == EINTR);
      if (n < 0 && errno == EAGAIN)
	return;
      if (n > 0 && (transfer->written += n) < size)
	{
	  /* The timer already wakes at the old deadline and will rearm
	     itself using this one.  */
	  transfer->deadline = selection_deadline ();
	  return;
	}
    }
  close_transfer (transfer);
}

/* Start new sends and expire stalled ones independently of the input
   queue and of other display wakeups.  */
static void
selection_timer (int fd, void *data)
{
  struct wayland_selection *selection = data;
  uint64_t count;
  if (read (fd, &count, sizeof count) != sizeof count)
    return;
  int64_t now = selection_now (), next = INT64_MAX;
  for (int i = 0; i < MAX_SELECTION_TRANSFERS; i++)
    {
      struct selection_transfer *transfer = &selection->transfers[i];
      if (transfer->fd < 0)
	continue;
      if (transfer->deadline <= now)
	{
	  close_transfer (transfer);
	  continue;
	}
      if (!transfer->watching)
	{
	  selection_write_ready (transfer->fd, transfer);
	  if (transfer->fd < 0)
	    continue;
	  transfer->watching = true;
	  add_write_fd (transfer->fd, selection_write_ready, transfer);
	}
      next = min (next, transfer->deadline);
    }

  struct itimerspec deadline = {0};
  if (next != INT64_MAX)
    deadline.it_value = (struct timespec) {next / 1000,
					  next % 1000 * 1000000};
  timerfd_settime (fd, TFD_TIMER_ABSTIME, &deadline, NULL);
}

static Lisp_Object
selection_payload (struct selection_source *source, const char *mime)
{
  for (Lisp_Object alist = source->payloads; CONSP (alist); alist = XCDR (alist))
    {
      Lisp_Object pair = XCAR (alist);
      if (!strcmp (SSDATA (XCAR (pair)), mime))
	return XCDR (pair);
    }
  return Qnil;
}

static void
queue_send (struct selection_source *source, const char *mime, int fd)
{
  struct wayland_selection *selection = source->selection;
  Lisp_Object bytes = selection_payload (source, mime);
  int flags = fcntl (fd, F_GETFL);
  if (!NILP (bytes) && selection->timer >= 0 && fd < FD_SETSIZE
      && flags >= 0 && fcntl (fd, F_SETFL, flags | O_NONBLOCK) == 0)
    for (int i = 0; i < MAX_SELECTION_TRANSFERS; i++)
      {
	struct selection_transfer *transfer = &selection->transfers[i];
	if (transfer->fd >= 0)
	  continue;
	*transfer = (struct selection_transfer)
	  {.fd = fd, .deadline = selection_deadline (), .bytes = bytes};
	struct itimerspec tick = {.it_value = {0, 1}};
	timerfd_settime (selection->timer, 0, &tick, NULL);
	return;
      }
  emacs_close (fd);
}

static void
destroy_source (struct selection_source *source)
{
  if (!source)
    return;
  struct wayland_selection *s = source->selection;
  for (int i = 0; i < 2; i++)
    if (s->slots[i].source == source)
      s->slots[i].source = NULL;
  struct selection_source **retired = &s->retired;
  while (*retired && *retired != source)
    retired = &(*retired)->next;
  if (*retired)
    *retired = source->next;
  if (source->barrier)
    wl_callback_destroy (source->barrier);
  if (source->clipboard)
    wl_data_source_destroy (source->clipboard);
  if (source->primary)
    zwp_primary_selection_source_v1_destroy (source->primary);
  xfree (source);
}

static void
selection_replaced (void *data, struct wl_callback *callback, uint32_t serial)
{
  destroy_source (data);
}

static const struct wl_callback_listener selection_barrier_listener =
  {.done = selection_replaced};

/* Keep the old source until earlier send events have been dispatched.
   The write itself refers to the payload independently of the source.  */
static void
retire_source (struct selection_source *source)
{
  if (!source)
    return;
  if (!(source->clipboard || source->primary))
    {
      destroy_source (source);
      return;
    }
  struct wayland_selection *s = source->selection;
  source->barrier = wl_display_sync (s->display->display);
  if (!source->barrier)
    {
      destroy_source (source);
      return;
    }
  source->next = s->retired;
  s->retired = source;
  wl_callback_add_listener (source->barrier, &selection_barrier_listener, source);
}

static void
clipboard_send (void *data, struct wl_data_source *proxy,
		const char *mime, int32_t fd)
{
  queue_send (data, mime, fd);
}

static void
clipboard_cancelled (void *data, struct wl_data_source *proxy)
{
  destroy_source (data);
}

static void
clipboard_target (void *data, struct wl_data_source *proxy, const char *mime)
{
}

static const struct wl_data_source_listener clipboard_source_listener =
  {.target = clipboard_target, .send = clipboard_send,
   .cancelled = clipboard_cancelled};

static void
primary_send (void *data, struct zwp_primary_selection_source_v1 *proxy,
	      const char *mime, int32_t fd)
{
  queue_send (data, mime, fd);
}

static void
primary_cancelled (void *data, struct zwp_primary_selection_source_v1 *proxy)
{
  destroy_source (data);
}

static const struct zwp_primary_selection_source_v1_listener
primary_source_listener =
  {.send = primary_send, .cancelled = primary_cancelled};

static void
destroy_offer (struct selection_offer *offer)
{
  if (!offer)
    return;
  if (offer->clipboard)
    wl_data_offer_destroy (offer->clipboard);
  if (offer->primary)
    zwp_primary_selection_offer_v1_destroy (offer->primary);
  while (offer->mimes)
    {
      struct selection_mime *mime = offer->mimes;
      offer->mimes = mime->next;
      xfree (mime);
    }
  xfree (offer);
}

static void
add_mime (struct selection_offer *offer, const char *name)
{
  size_t size = strlen (name) + 1;
  struct selection_mime *mime = xmalloc (sizeof *mime + size);
  memcpy (mime->name, name, size);
  mime->next = offer->mimes;
  offer->mimes = mime;
}

static void
clipboard_mime (void *data, struct wl_data_offer *proxy, const char *name)
{
  add_mime (data, name);
}

static void
clipboard_actions (void *data, struct wl_data_offer *proxy, uint32_t actions)
{
}

static const struct wl_data_offer_listener clipboard_offer_listener =
  {.offer = clipboard_mime, .source_actions = clipboard_actions,
   .action = clipboard_actions};

static void
clipboard_announced (void *data, struct wl_data_device *device,
		     struct wl_data_offer *proxy)
{
  struct selection_slot *slot = data;
  destroy_offer (slot->announced);
  slot->announced = xzalloc (sizeof *slot->announced);
  slot->announced->clipboard = proxy;
  wl_data_offer_add_listener (proxy, &clipboard_offer_listener, slot->announced);
}

static void
clipboard_selected (void *data, struct wl_data_device *device,
		    struct wl_data_offer *proxy)
{
  struct selection_slot *slot = data;
  destroy_offer (slot->offer);
  slot->offer = proxy ? wl_data_offer_get_user_data (proxy) : NULL;
  if (slot->offer && slot->offer == slot->announced)
    slot->announced = NULL;
}

/* Drag and drop is a separate use of wl_data_device.  Decline it.  */
static void
clipboard_drag_enter (void *data, struct wl_data_device *device,
		      uint32_t serial, struct wl_surface *surface,
		      wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *proxy)
{
  if (!proxy)
    return;
  struct selection_slot *slot = data;
  struct selection_offer *offer = wl_data_offer_get_user_data (proxy);
  if (offer == slot->announced)
    slot->announced = NULL;
  destroy_offer (offer);
}

static void
clipboard_drag_motion (void *data, struct wl_data_device *device,
		       uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
}

static void
clipboard_drag_end (void *data, struct wl_data_device *device)
{
}

static const struct wl_data_device_listener clipboard_device_listener =
  {.data_offer = clipboard_announced, .selection = clipboard_selected,
   .enter = clipboard_drag_enter, .leave = clipboard_drag_end,
   .motion = clipboard_drag_motion, .drop = clipboard_drag_end};

static void
primary_mime (void *data, struct zwp_primary_selection_offer_v1 *proxy,
	      const char *name)
{
  add_mime (data, name);
}

static const struct zwp_primary_selection_offer_v1_listener
primary_offer_listener = {.offer = primary_mime};

static void
primary_announced (void *data, struct zwp_primary_selection_device_v1 *device,
		   struct zwp_primary_selection_offer_v1 *proxy)
{
  struct selection_slot *slot = data;
  destroy_offer (slot->announced);
  slot->announced = xzalloc (sizeof *slot->announced);
  slot->announced->primary = proxy;
  zwp_primary_selection_offer_v1_add_listener (proxy, &primary_offer_listener,
					      slot->announced);
}

static void
primary_selected (void *data, struct zwp_primary_selection_device_v1 *device,
		  struct zwp_primary_selection_offer_v1 *proxy)
{
  struct selection_slot *slot = data;
  destroy_offer (slot->offer);
  slot->offer = proxy ? zwp_primary_selection_offer_v1_get_user_data (proxy)
    : NULL;
  if (slot->offer && slot->offer == slot->announced)
    slot->announced = NULL;
}

static const struct zwp_primary_selection_device_v1_listener
primary_device_listener =
  {.data_offer = primary_announced, .selection = primary_selected};

void
wayland_selection_create
  (struct wayland_display_info *dpyinfo,
   struct wl_data_device_manager *clipboard,
   struct zwp_primary_selection_device_manager_v1 *primary, struct wl_seat *seat)
{
  struct wayland_selection *selection = xzalloc (sizeof *selection);
  dpyinfo->selection = selection;
  selection->display = dpyinfo;
  selection->clipboard_manager = clipboard;
  selection->primary_manager = primary;
  for (int i = 0; i < MAX_SELECTION_TRANSFERS; i++)
    selection->transfers[i].fd = -1;
  selection->timer = timerfd_create (CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (selection->timer >= FD_SETSIZE)
    {
      emacs_close (selection->timer);
      selection->timer = -1;
    }
  if (selection->timer >= 0)
    add_non_keyboard_read_fd (selection->timer, selection_timer, selection);
  selection->clipboard = wl_data_device_manager_get_data_device (clipboard, seat);
  if (selection->clipboard)
    wl_data_device_add_listener (selection->clipboard, &clipboard_device_listener,
				 &selection->slots[0]);
  if (primary)
    {
      selection->primary
	= zwp_primary_selection_device_manager_v1_get_device (primary, seat);
      if (selection->primary)
	zwp_primary_selection_device_v1_add_listener
	  (selection->primary, &primary_device_listener, &selection->slots[1]);
    }
}

void
wayland_selection_connection_lost (struct wayland_display_info *dpyinfo)
{
  struct wayland_selection *selection = dpyinfo->selection;
  if (!selection)
    return;
  for (int i = 0; i < MAX_SELECTION_TRANSFERS; i++)
    if (selection->transfers[i].fd >= 0)
      close_transfer (&selection->transfers[i]);
  if (selection->timer >= 0)
    {
      delete_read_fd (selection->timer);
      emacs_close (selection->timer);
      selection->timer = -1;
    }
}

void
wayland_selection_destroy (struct wayland_display_info *dpyinfo)
{
  struct wayland_selection *selection = dpyinfo->selection;
  if (!selection)
    return;
  wayland_selection_connection_lost (dpyinfo);
  for (int i = 0; i < 2; i++)
    {
      destroy_source (selection->slots[i].source);
      destroy_offer (selection->slots[i].offer);
      destroy_offer (selection->slots[i].announced);
    }
  while (selection->retired)
    destroy_source (selection->retired);
  if (selection->clipboard)
    {
      if (wl_data_device_get_version (selection->clipboard)
	  >= WL_DATA_DEVICE_RELEASE_SINCE_VERSION)
	wl_data_device_release (selection->clipboard);
      else
	wl_data_device_destroy (selection->clipboard);
    }
  if (selection->primary)
    zwp_primary_selection_device_v1_destroy (selection->primary);
  dpyinfo->selection = NULL;
  xfree (selection);
}

void
wayland_selection_mark (struct wayland_display_info *dpyinfo)
{
  struct wayland_selection *selection = dpyinfo->selection;
  if (!selection)
    return;
  for (int i = 0; i < 2; i++)
    if (selection->slots[i].source)
      mark_object (selection->slots[i].source->payloads);
  for (struct selection_source *source = selection->retired; source;
       source = source->next)
    mark_object (source->payloads);
  for (int i = 0; i < MAX_SELECTION_TRANSFERS; i++)
    if (selection->transfers[i].fd >= 0)
      mark_object (selection->transfers[i].bytes);
}

static struct wayland_selection *
selection_for_frame (Lisp_Object frame)
{
  struct frame *f = decode_live_frame (frame);
  if (!FRAME_WAYLAND_P (f)
      || FRAME_DISPLAY_INFO (f)->state == WAYLAND_DISPLAY_DEAD)
    return NULL;
  return FRAME_DISPLAY_INFO (f)->selection;
}

static bool
can_publish (struct wayland_selection *s, bool primary)
{
  return s && ((primary && !s->primary)
	       || ((primary || s->clipboard)
		   && wayland_selection_serial (s->display)));
}

DEFUN ("wayland--selection-publishable-p", Fwayland__selection_publishable_p,
       Swayland__selection_publishable_p, 1, 2, 0,
       doc: /* Return non-nil if FRAME can claim the clipboard or PRIMARY.  */)
  (Lisp_Object primary, Lisp_Object frame)
{
  return can_publish (selection_for_frame (frame), !NILP (primary)) ? Qt : Qnil;
}

DEFUN ("wayland--set-selection", Fwayland__set_selection,
       Swayland__set_selection, 2, 3, 0,
       doc: /* Claim the clipboard or PRIMARY with MIME PAYLOADS on FRAME.
PAYLOADS is an alist of MIME type strings and unibyte strings.
Nil disowns the selection.  */)
  (Lisp_Object primary, Lisp_Object payloads, Lisp_Object frame)
{
  struct wayland_selection *s = selection_for_frame (frame);
  bool p = !NILP (primary);
  if (!s || (!NILP (payloads) && !can_publish (s, p)))
    return Qnil;
  struct selection_slot *slot = &s->slots[p];
  CHECK_LIST (payloads);
  for (Lisp_Object list = payloads; CONSP (list); list = XCDR (list))
    {
      Lisp_Object pair = XCAR (list);
      CHECK_CONS (pair);
      CHECK_STRING_NULL_BYTES (XCAR (pair));
      CHECK_STRING (XCDR (pair));
      if (STRING_MULTIBYTE (XCDR (pair)))
	error ("Wayland selection payload must be unibyte");
    }

  block_input ();
  if (NILP (payloads))
    {
      destroy_source (slot->source);
      wayland_selection_flush (s->display);
      unblock_input ();
      return Qnil;
    }
  struct selection_source *source = xzalloc (sizeof *source);
  source->selection = s;
  source->payloads = payloads;
  if (p && s->primary)
    source->primary
      = zwp_primary_selection_device_manager_v1_create_source (s->primary_manager);
  else if (!p)
    source->clipboard
      = wl_data_device_manager_create_data_source (s->clipboard_manager);
  if ((!p && !source->clipboard) || (p && s->primary && !source->primary))
    {
      xfree (source);
      unblock_input ();
      memory_full (0);
    }
  if (source->clipboard)
    wl_data_source_add_listener (source->clipboard, &clipboard_source_listener,
				 source);
  if (source->primary)
    zwp_primary_selection_source_v1_add_listener (source->primary,
						 &primary_source_listener,
						 source);
  for (Lisp_Object list = payloads; CONSP (list); list = XCDR (list))
    {
      const char *mime = SSDATA (XCAR (XCAR (list)));
      if (source->clipboard)
	wl_data_source_offer (source->clipboard, mime);
      if (source->primary)
	zwp_primary_selection_source_v1_offer (source->primary, mime);
    }
  uint32_t serial = wayland_selection_serial (s->display);
  if (source->clipboard)
    wl_data_device_set_selection (s->clipboard, source->clipboard, serial);
  if (source->primary)
    zwp_primary_selection_device_v1_set_selection (s->primary, source->primary,
						   serial);
  struct selection_source *previous = slot->source;
  slot->source = source;
  retire_source (previous);
  wayland_selection_flush (s->display);
  unblock_input ();
  return Qt;
}

DEFUN ("wayland--selection-owner-p", Fwayland__selection_owner_p,
       Swayland__selection_owner_p, 1, 2, 0,
       doc: /* Return non-nil if FRAME's seat owns the clipboard or PRIMARY.  */)
  (Lisp_Object primary, Lisp_Object frame)
{
  struct wayland_selection *s = selection_for_frame (frame);
  return s && s->slots[!NILP (primary)].source ? Qt : Qnil;
}

DEFUN ("wayland--selection-targets", Fwayland__selection_targets,
       Swayland__selection_targets, 1, 2, 0,
       doc: /* Return MIME type names for the clipboard or PRIMARY on FRAME.  */)
  (Lisp_Object primary, Lisp_Object frame)
{
  struct wayland_selection *s = selection_for_frame (frame);
  if (!s)
    return Qnil;
  struct selection_slot *slot = &s->slots[!NILP (primary)];
  Lisp_Object mimes = Qnil;
  block_input ();
  if (slot->source)
    for (Lisp_Object list = slot->source->payloads; CONSP (list);
	 list = XCDR (list))
      mimes = Fcons (Fcopy_sequence (XCAR (XCAR (list))), mimes);
  else if (slot->offer && wayland_selection_focused_p (s->display))
    for (struct selection_mime *mime = slot->offer->mimes; mime; mime = mime->next)
      mimes = Fcons (build_string (mime->name), mimes);
  unblock_input ();
  return mimes;
}

struct selection_read
{
  int fd;
  char *data;
  struct wayland_display_info *display;
};

static void
finish_read (void *data)
{
  struct selection_read *read_state = data;
  emacs_close (read_state->fd);
  xfree (read_state->data);
  read_state->display->busy--;
}

static Lisp_Object
read_offer (struct wayland_selection *s, struct selection_offer *offer,
	    const char *mime)
{
  int fds[2];
  if (emacs_pipe (fds) < 0)
    return Qnil;
  if (fcntl (fds[0], F_SETFL, O_NONBLOCK) < 0)
    {
      emacs_close (fds[0]);
      emacs_close (fds[1]);
      return Qnil;
    }

  struct wayland_display_info *dpyinfo = s->display;
  struct selection_read state = {.fd = fds[0], .display = dpyinfo};
  specpdl_ref count = SPECPDL_INDEX ();
  dpyinfo->busy++;
  record_unwind_protect_ptr (finish_read, &state);
  if (offer->clipboard)
    wl_data_offer_receive (offer->clipboard, mime, fds[1]);
  else
    zwp_primary_selection_offer_v1_receive (offer->primary, mime, fds[1]);
  emacs_close (fds[1]);
  int64_t deadline = selection_deadline ();
  ptrdiff_t length = 0, capacity = 0;
  bool complete = false;
  while (wayland_selection_service (dpyinfo) && NILP (Vquit_flag))
    {
      int64_t remaining = deadline - selection_now ();
      if (remaining <= 0)
	break;
      struct pollfd pollfds[] = {
	{.fd = fds[0], .events = POLLIN},
	{.fd = dpyinfo->connection,
	 .events = POLLIN | (dpyinfo->output_state == WAYLAND_OUTPUT_PENDING
			     ? POLLOUT : 0)}
      };
      int ready = poll (pollfds, 2, remaining);
      if (ready < 0 && errno == EINTR)
	continue;
      if (ready <= 0)
	break;
      if (pollfds[1].revents && !wayland_selection_service (dpyinfo))
	break;
      if (pollfds[0].revents)
	{
	  if (capacity == length)
	    state.data = xpalloc (state.data, &capacity, 4096,
				  STRING_BYTES_BOUND, 1);
	  ssize_t n = read (fds[0], state.data + length, capacity - length);
	  if (!n)
	    {
	      complete = true;
	      break;
	    }
	  if (n > 0)
	    length += n;
	  else if (errno != EINTR && errno != EAGAIN)
	    break;
	}
    }
  Lisp_Object result = complete ? make_unibyte_string (state.data, length) : Qnil;
  return unbind_to (count, result);
}

DEFUN ("wayland--get-selection-mime", Fwayland__get_selection_mime,
       Swayland__get_selection_mime, 2, 3, 0,
       doc: /* Read MIME bytes from the clipboard or PRIMARY on FRAME.  */)
  (Lisp_Object primary, Lisp_Object mime, Lisp_Object frame)
{
  struct wayland_selection *s = selection_for_frame (frame);
  CHECK_STRING_NULL_BYTES (mime);
  if (!s)
    return Qnil;
  specpdl_ref count = SPECPDL_INDEX ();
  block_input ();
  record_unwind_protect_void (unblock_input);
  struct selection_slot *slot = &s->slots[!NILP (primary)];
  Lisp_Object result = Qnil;
  if (slot->source)
    result = Fcopy_sequence (selection_payload (slot->source, SSDATA (mime)));
  else if (slot->offer && wayland_selection_focused_p (s->display))
    for (struct selection_mime *type = slot->offer->mimes; type; type = type->next)
      if (!strcmp (type->name, SSDATA (mime)))
	{
	  result = read_offer (s, slot->offer, type->name);
	  break;
	}
  result = unbind_to (count, result);
  maybe_quit ();
  return result;
}

void
syms_of_waylandselect (void)
{
  DEFVAR_INT ("wayland-selection-timeout", wayland_selection_timeout,
	      doc: /* Timeout in milliseconds for Wayland selection transfers.
Incoming reads have this long to finish.  Outgoing writes time out
after this long without making progress.  */);
  wayland_selection_timeout = 5000;

  defsubr (&Swayland__selection_publishable_p);
  defsubr (&Swayland__set_selection);
  defsubr (&Swayland__selection_owner_p);
  defsubr (&Swayland__selection_targets);
  defsubr (&Swayland__get_selection_mime);
}
