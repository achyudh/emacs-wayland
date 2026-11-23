/* Input methods (text-input-v3) for the native Wayland backend.

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

/* This matches what pgtkim.c does with GtkIMContext, using
   text-input-unstable-v3 directly:
   committed text becomes key-string input, preedit text becomes the
   PREEDIT_TEXT_EVENT that `wayland-preedit-text' shows, and the cursor
   rectangle comes from the same redisplay point.  Surrounding text is
   not sent, as in PGTK.

   Preedit, delete and commit events are double-buffered and take
   effect on done, in the order the protocol gives.  Every done applies
   its edits, so committed text is never lost; but after a done whose
   serial does not count all our commits, the cursor rectangle and
   content type are held back until a done that does, as the protocol
   requires.

   A batch belongs to the frame focused when its first event arrived.
   If focus moves before its done, committed text still goes to that
   frame, as with a GtkIMContext per frame, and its preedit is
   discarded.  */

#include <config.h>

#include "lisp.h"
#include "blockinput.h"
#include "frame.h"
#include "keyboard.h"
#include "termhooks.h"
#include "waylandterm.h"

#include <wayland-client.h>
#if __GNUC__ >= 15
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wtrailing-whitespace"
#endif
#include "text-input-unstable-v3-client-protocol.h"
#if __GNUC__ >= 15
# pragma GCC diagnostic pop
#endif

struct wayland_im
{
  struct wayland_display_info *dpyinfo;
  struct zwp_text_input_v3 *text_input;

  /* The surface the input method entered, or null; and the frame that
     last had it, which gets edits arriving after it left.  */
  struct wl_surface *focus;
  struct frame *last_frame;
  bool enabled;

  /* The frame the pending batch belongs to, or null before its first
     event.  Frames here and in preedit_frame are cleared when deleted
     (wayland_im_forget_frame), so they never dangle.  */
  struct frame *batch_frame;

  /* Commit requests sent, and whether the last done counted them all,
     so state may be sent.  */
  uint32_t commits;
  bool synced;

  /* The pending, double-buffered events.  */
  char *preedit, *commit;

  /* The frame showing a non-empty preedit, or null.  */
  struct frame *preedit_frame;

  /* The cursor rectangle, in logical surface coordinates: the last one
     sent, and the one wanted when it could not be sent.  Both belong
     to rect_surface.  */
  int rect[4], wanted[4];
  bool rect_sent, rect_wanted;
  struct wl_surface *rect_surface;
};

/* Forget the cursor rectangles unless they belong to SURFACE.  */

static void
wayland_im_own_rect (struct wayland_im *im, struct wl_surface *surface)
{
  if (im->rect_surface != surface)
    {
      im->rect_sent = im->rect_wanted = false;
      im->rect_surface = surface;
    }
}

static void
wayland_im_send_state (struct wayland_im *im)
{
  if (im->rect_wanted)
    {
      memcpy (im->rect, im->wanted, sizeof im->rect);
      im->rect_wanted = false;
      im->rect_sent = true;
    }
  if (im->rect_sent)
    zwp_text_input_v3_set_cursor_rectangle (im->text_input, im->rect[0],
					    im->rect[1], im->rect[2],
					    im->rect[3]);
  zwp_text_input_v3_commit (im->text_input);
  im->commits++;
}

static void
wayland_im_queue_preedit (struct wayland_im *im, struct frame *f,
			  Lisp_Object preedit)
{
  union buffered_input_event event;

  EVENT_INIT (event.ie);
  event.ie.kind = PREEDIT_TEXT_EVENT;
  event.ie.arg = preedit;
  XSETFRAME (event.ie.frame_or_window, f);
  wayland_queue_input_event (im->dpyinfo, &event);
  im->preedit_frame = NILP (preedit) ? NULL : f;
}

static struct frame *
wayland_im_target (struct wayland_im *im)
{
  struct frame *root = wayland_surface_frame (im->dpyinfo, im->focus);
  struct frame *focus = im->dpyinfo->x_focus_frame;
  if (root && focus && wayland_frame_root (focus) == root)
    return focus;
  return root;
}

void
wayland_im_focus_changed (struct wayland_display_info *dpyinfo,
			  struct frame *old, struct frame *new)
{
  struct wayland_im *im = dpyinfo->im;
  if (!im || !im->focus)
    return;
  struct frame *root = wayland_surface_frame (dpyinfo, im->focus);
  if (root && new && wayland_frame_root (new) == root)
    {
      im->last_frame = new;
      im->rect_sent = im->rect_wanted = false;
    }
  if (im->preedit_frame && im->preedit_frame == old && old != new)
    wayland_im_queue_preedit (im, old, Qnil);
}

/* Note the destination of the batch an event belongs to.  */

static void
wayland_im_open_batch (struct wayland_im *im)
{
  if (!im->batch_frame)
    {
      struct frame *f = wayland_im_target (im);
      im->batch_frame = f ? f : im->last_frame;
    }
}

static void
text_input_enter (void *data, struct zwp_text_input_v3 *text_input,
		  struct wl_surface *surface)
{
  struct wayland_im *im = data;

  im->focus = surface;
  im->last_frame = wayland_im_target (im);
  im->enabled = wayland_use_input_method;
  if (!im->enabled)
    return;
  /* The protocol resets all state on enter, so send it all again; but
     a rectangle in another surface is meaningless here.  */
  wayland_im_own_rect (im, surface);
  zwp_text_input_v3_enable (text_input);
  zwp_text_input_v3_set_content_type
    (text_input, ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE,
     ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL);
  im->synced = true;
  wayland_im_send_state (im);
}

static void
text_input_leave (void *data, struct zwp_text_input_v3 *text_input,
		  struct wl_surface *surface)
{
  struct wayland_im *im = data;
  struct frame *f = wayland_im_target (im);

  if (im->focus != surface)
    return;
  im->focus = NULL;
  if (im->enabled)
    {
      zwp_text_input_v3_disable (text_input);
      zwp_text_input_v3_commit (text_input);
      im->commits++;
      im->enabled = false;
    }
  /* Leaving clears the preedit.  */
  if (f && im->preedit_frame == f)
    wayland_im_queue_preedit (im, f, Qnil);
}

static void
text_input_preedit_string (void *data, struct zwp_text_input_v3 *text_input,
			   const char *text, int32_t cursor_begin,
			   int32_t cursor_end)
{
  struct wayland_im *im = data;

  wayland_im_open_batch (im);
  xfree (im->preedit);
  im->preedit = text ? xstrdup (text) : NULL;
}

static void
text_input_commit_string (void *data, struct zwp_text_input_v3 *text_input,
			  const char *text)
{
  struct wayland_im *im = data;

  wayland_im_open_batch (im);
  xfree (im->commit);
  im->commit = text ? xstrdup (text) : NULL;
}

static void
text_input_delete_surrounding_text (void *data,
				    struct zwp_text_input_v3 *text_input,
				    uint32_t before_length,
				    uint32_t after_length)
{
  /* No surrounding text is sent, so none can be deleted.  */
}

static void
text_input_done (void *data, struct zwp_text_input_v3 *text_input,
		 uint32_t serial)
{
  struct wayland_im *im = data;
  struct frame *f = im->batch_frame;
  struct frame *focused = wayland_im_target (im);

  if (f && !FRAME_LIVE_P (f))
    f = NULL;

  /* 1. Replace the old preedit; 3. insert the commit string; 5. insert
     the new preedit.  A batch without a preedit string ends any
     preedit, and so does one whose frame lost focus before its done.  */
  bool preedit = f && f == focused && im->preedit && *im->preedit;
  if (im->preedit_frame
      && (im->commit || !preedit || im->preedit_frame != f))
    {
      if (FRAME_LIVE_P (im->preedit_frame))
	wayland_im_queue_preedit (im, im->preedit_frame, Qnil);
      im->preedit_frame = NULL;
    }
  if (f && im->commit && *im->commit)
    wayland_queue_utf8 (im->dpyinfo, f, im->commit, 0, 0);
  if (preedit)
    /* As pgtkim.c: a list of (TEXT ATTRIBUTES...) parts.  */
    wayland_im_queue_preedit
      (im, f, list1 (list2 (build_string (im->preedit), Fcons (Qul, Qt))));
  xfree (im->preedit);
  xfree (im->commit);
  im->preedit = im->commit = NULL;
  im->batch_frame = NULL;

  im->synced = serial == im->commits;
  if (im->synced && im->enabled && im->rect_wanted)
    wayland_im_send_state (im);
}

static const struct zwp_text_input_v3_listener text_input_listener =
  {
    .enter = text_input_enter,
    .leave = text_input_leave,
    .preedit_string = text_input_preedit_string,
    .commit_string = text_input_commit_string,
    .delete_surrounding_text = text_input_delete_surrounding_text,
    .done = text_input_done,
  };

void
wayland_im_create (struct wayland_display_info *dpyinfo,
		   struct zwp_text_input_manager_v3 *manager,
		   struct wl_seat *seat)
{
  struct wayland_im *im;

  if (dpyinfo->im || !manager || !seat)
    return;
  im = xzalloc (sizeof *im);
  im->dpyinfo = dpyinfo;
  im->text_input = zwp_text_input_manager_v3_get_text_input (manager, seat);
  if (!im->text_input)
    {
      xfree (im);
      return;
    }
  zwp_text_input_v3_add_listener (im->text_input, &text_input_listener, im);
  dpyinfo->im = im;
}

void
wayland_im_destroy (struct wayland_display_info *dpyinfo)
{
  struct wayland_im *im = dpyinfo->im;

  if (!im)
    return;
  zwp_text_input_v3_destroy (im->text_input);
  xfree (im->preedit);
  xfree (im->commit);
  xfree (im);
  dpyinfo->im = NULL;
}

/* Frame F, whose surface is SURFACE or null, is being deleted.  It is
   already off the frame list, so it cannot be found from SURFACE.
   Drop every reference to it, with its pending batch and cursor
   rectangles.  */

void
wayland_im_forget_frame (struct wayland_display_info *dpyinfo,
			 struct frame *f, struct wl_surface *surface)
{
  struct wayland_im *im = dpyinfo->im;

  if (!im)
    return;
  if (surface && im->focus == surface)
    im->focus = NULL;
  if (im->last_frame == f)
    im->last_frame = NULL;
  if (im->preedit_frame == f)
    im->preedit_frame = NULL;
  if (im->batch_frame == f)
    {
      xfree (im->preedit);
      xfree (im->commit);
      im->preedit = im->commit = NULL;
      im->batch_frame = NULL;
    }
  if (surface && im->rect_surface == surface)
    wayland_im_own_rect (im, NULL);
}

/* Called by redisplay when it draws frame F's cursor at X, Y, WIDTH
   by HEIGHT in F's pixels, as pgtk_im_set_cursor_location is.  */

void
wayland_im_set_cursor_rectangle (struct frame *f, int x, int y, int width,
				 int height)
{
  struct wayland_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);
  struct wayland_im *im = dpyinfo->im;
  int scale, rect[4];

  if (!im || !im->enabled || dpyinfo->state == WAYLAND_DISPLAY_DEAD
      || wayland_im_target (im) != f)
    return;

  /* The protocol's rectangle is in logical surface coordinates.  */
  scale = wayland_frame_scale (f);
  int left, top;
  wayland_frame_root_position (f, &left, &top);
  rect[0] = clip_to_bounds (INT_MIN, (int64_t) left + x * 120 / scale, INT_MAX);
  rect[1] = clip_to_bounds (INT_MIN, (int64_t) top + y * 120 / scale, INT_MAX);
  rect[2] = max (1, width * 120 / scale);
  rect[3] = max (1, height * 120 / scale);
  wayland_im_own_rect (im, im->focus);
  if (im->rect_sent && !memcmp (rect, im->rect, sizeof rect))
    {
      /* Back where the input method already has it: any rectangle
	 held back since is obsolete.  */
      im->rect_wanted = false;
      return;
    }
  memcpy (im->wanted, rect, sizeof rect);
  im->rect_wanted = true;
  if (im->synced)
    wayland_im_send_state (im);
}

void
syms_of_waylandim (void)
{
  DEFSYM (Qul, "ul");

  DEFVAR_BOOL ("wayland-use-input-method", wayland_use_input_method,
    doc: /* Non-nil means to use the compositor's input method.
The input method composes text, as for Chinese, Japanese or Korean, and
shows its preedit text at point.  A change takes effect the next time
an Emacs frame gets keyboard focus.  This corresponds to
`pgtk-use-im-context' in PGTK builds.  */);
  wayland_use_input_method = true;
}
