/* Definitions for the native Wayland backend.

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

/* The layout of the generic parts of the structures below follows
   pgtkterm.h, because shared code (image.c, xfaces.c, fringe.c, ...)
   reaches into them through the same field names.  Wayland's own
   types are only forward-declared here, so that files including this
   header need not see libwayland's headers.  */

#ifndef WAYLANDTERM_H
#define WAYLANDTERM_H

#include "dispextern.h"
#include "frame.h"
#include "character.h"
#include "font.h"
#include "sysselect.h"

#ifdef HAVE_WAYLAND

#include <cairo.h>

struct wl_display;
struct wl_registry;
struct wl_surface;
struct wl_seat;
struct wl_data_device_manager;
struct zwp_primary_selection_device_manager_v1;
struct wayland_globals;
struct wayland_seat;
struct wayland_surface_state;
struct wayland_selection;

/* A bitmap shared with image.c's Cairo code; same fields as PGTK's.  */
struct wayland_bitmap_record
{
  char *file;
  int refcount;
  int height, width, depth;
  cairo_pattern_t *pattern;
};

#define RGB_TO_ULONG(r, g, b) (((r) << 16) | ((g) << 8) | (b))
#define ARGB_TO_ULONG(a, r, g, b) \
  (((a) << 24) | ((r) << 16) | ((g) << 8) | (b))
#define ALPHA_FROM_ULONG(color) ((color) >> 24)
#define RED_FROM_ULONG(color)	(((color) >> 16) & 0xff)
#define GREEN_FROM_ULONG(color) (((color) >> 8) & 0xff)
#define BLUE_FROM_ULONG(color)	((color) & 0xff)

/* The backend has no scroll bars.  Shared code still names this type.  */
struct scroll_bar
{
  union vectorlike_header header;
  Lisp_Object window;
  Lisp_Object next, prev;
  Window x_window;
  int top, left, width, height;
  int start, end;
  int dragging;
  bool horizontal;
} GCALIGNED_STRUCT;

/* Whether the display connection is usable.  */
enum wayland_display_state
{
  WAYLAND_DISPLAY_LIVE,
  /* Connection lost: descriptors disarmed, drawing is a no-op, and
     finalization waits for a guarded safe point.  */
  WAYLAND_DISPLAY_DEAD,
};

/* Counters reported by `wayland--metrics'.  They diagnose; they are
   never compared with PGTK.  */
struct wayland_metrics
{
  unsigned long hook_calls;
  unsigned long service_calls;
  unsigned long write_callbacks;
  unsigned long pings;
  unsigned long raw_keys;
  unsigned long flush_eagain;
  unsigned long flush_epipe;
  unsigned long output_resumed;
  unsigned long write_resumes;
  unsigned long finalize_deferrals;
  /* Longest time one input-hook call took, in nanoseconds; the hook
     must never wait.  */
  long long max_hook_ns;
};

/* Display socket output state.  */
enum wayland_output_state
{
  WAYLAND_OUTPUT_IDLE,
  WAYLAND_OUTPUT_PENDING,
};

struct wayland_display_dimensions
{
  int64_t width, height;
  int mm_width, mm_height, screens;
};

struct wayland_display_info;
extern struct wayland_display_info *wayland_display_by_name (Lisp_Object);
extern void wayland_display_dimensions (struct wayland_display_info *,
					struct wayland_display_dimensions *);

struct wayland_display_info
{
  /* Chain of all wayland_display_info structures.  */
  struct wayland_display_info *next;

  /* The generic display parameters for this display.  */
  struct terminal *terminal;

  /* The compositor connection.  `display' is the name shared code
     uses for it.  */
  struct wl_display *display;
  struct wl_registry *registry;

  /* Bound globals and their versions.  */
  struct wayland_globals *globals;

  /* The seat; v1 handles one.  */
  struct wayland_seat *seat;

  /* The input method (text-input-v3) of the seat, or null when the
     compositor has none.  */
  struct wayland_im *im;
  struct wayland_selection *selection;

  /* The display socket, its duplicates for non-keyboard waits and for
     write interest, the backend timer (transfer deadlines and
     finalization retries), and the key repeat timer.  Both timers are
     in every wait, so keys repeat even while Lisp waits on a process,
     as X's server-side repeat does.  */
  int connection;
  int service_fd;
  int write_fd;
  int timer_fd;
  int repeat_fd;

  enum wayland_display_state state;
  enum wayland_output_state output_state;

  /* Re-entry guard for the input hook.  */
  bool_bf dispatching : 1;

  /* Set once `wayland--finalize-display' is queued on
     `pending_funcalls'.  */
  bool_bf finalize_queued : 1;

  /* Why the connection was lost, for the log and `wayland--metrics'.  */
  char death_message[256];

  struct wayland_metrics metrics;

  /* Backend operations in flight that hold display or frame
     references; finalization waits for zero.  */
  int busy;

  /* This is a cons cell of the form (NAME . FONT-LIST-CACHE).  */
  Lisp_Object name_list_element;

  /* Number of frames that are on this display.  */
  int reference_count;

  /* The number of fonts loaded.  */
  int n_fonts;

  /* Minimum width over all characters in all fonts in font_table.  */
  int smallest_char_width;

  /* Minimum font height over all fonts in font_table.  */
  int smallest_font_height;

  struct wayland_bitmap_record *bitmaps;
  ptrdiff_t bitmaps_size;
  ptrdiff_t bitmaps_last;

  /* Display resolution.  Frames on outputs with different scales use
     their own value.  */
  double resx, resy;

  /* Mask of things that cause the mouse to be grabbed.  */
  int grabbed;

  int n_planes;
  int color_p;

  /* Emacs bitmap-id of the default icon bitmap for this frame.
     Or -1 if none has been allocated yet.  */
  ptrdiff_t icon_bitmap_id;

  Window root_window;

  /* X resources do not exist on Wayland; kept for shared code.  */
  XrmDatabase rdb;

  /* Color names from etc/rgb.txt, as an alist of (NAME . PIXEL).  */
  Lisp_Object color_map;

  Emacs_Cursor vertical_scroll_bar_cursor;
  Emacs_Cursor horizontal_scroll_bar_cursor;

  /* Information about the range of text currently shown in
     mouse-face.  */
  Mouse_HLInfo mouse_highlight;

  struct frame *highlight_frame;
  struct frame *x_focus_frame;
  struct frame *x_focus_event_frame;
  struct frame *last_mouse_frame;
  struct frame *last_mouse_motion_frame;
  int last_mouse_motion_x;
  int last_mouse_motion_y;
  XRectangle last_mouse_glyph;
  Time last_mouse_movement_time;
  void *last_mouse_scroll_bar;
  struct frame *last_mouse_glyph_frame;

  /* The invisible cursor used for pointer blanking.  */
  Emacs_Cursor invisible_cursor;
};

/* This is a chain of structures for all the Wayland displays
   currently in use.  */
extern struct wayland_display_info *x_display_list;

struct wayland_output
{
  unsigned long foreground_color;
  unsigned long background_color;

  /* Cursors.  */
  Emacs_Cursor current_cursor;
  Emacs_Cursor text_cursor;
  Emacs_Cursor nontext_cursor;
  Emacs_Cursor modeline_cursor;
  Emacs_Cursor hand_cursor;
  Emacs_Cursor hourglass_cursor;
  Emacs_Cursor horizontal_drag_cursor;
  Emacs_Cursor vertical_drag_cursor;
  Emacs_Cursor left_edge_cursor;
  Emacs_Cursor top_left_corner_cursor;
  Emacs_Cursor top_edge_cursor;
  Emacs_Cursor top_right_corner_cursor;
  Emacs_Cursor right_edge_cursor;
  Emacs_Cursor bottom_right_corner_cursor;
  Emacs_Cursor bottom_edge_cursor;
  Emacs_Cursor bottom_left_corner_cursor;
  Emacs_Cursor current_pointer;

  unsigned long border_pixel;
  Emacs_GC cursor_xgcv;

  /* Shared code compares these with 0; Wayland has no window ids.  */
  Window window_desc, parent_desc;
  char explicit_parent;

  /* If >=0, a bitmap index.  The indicated bitmap is used for the
     icon.  */
  ptrdiff_t icon_bitmap;

  struct font *font;
  int baseline_offset;

  /* If a fontset is specified for this frame instead of font, this
     value contains an ID of the fontset, else -1.  */
  int fontset;

  unsigned long mouse_color;
  unsigned long cursor_color;
  unsigned long cursor_foreground_color;

  /* This is the Emacs structure for the display this frame is on.  */
  struct wayland_display_info *display_info;

  /* Surface, buffers, damage and presentation state; private to
     waylandterm.c.  */
  struct wayland_surface_state *surface;

  /* Relief GCs, colors etc.  */
  struct relief
  {
    Emacs_GC xgcv;
    unsigned long pixel;
  }
  black_relief, white_relief;

  /* The background for which the above relief GCs were set up.  */
  unsigned long relief_background;
  bool_bf relief_background_valid_p : 1;

  /* Per-frame resolution.  */
  double resx, resy;
};

/* Satisfy term.c.  */
struct x_output
{
  int unused;
};

#define FRAME_X_OUTPUT(f)         ((f)->output_data.wayland)
#define FRAME_OUTPUT_DATA(f)      FRAME_X_OUTPUT (f)

#define FRAME_DISPLAY_INFO(f)     (FRAME_X_OUTPUT (f)->display_info)
#define FRAME_FOREGROUND_COLOR(f) (FRAME_X_OUTPUT (f)->foreground_color)
#define FRAME_BACKGROUND_COLOR(f) (FRAME_X_OUTPUT (f)->background_color)
#define FRAME_CURSOR_COLOR(f)     (FRAME_X_OUTPUT (f)->cursor_color)
#define FRAME_POINTER_TYPE(f)     (FRAME_X_OUTPUT (f)->current_pointer)
#define FRAME_FONT(f)             (FRAME_X_OUTPUT (f)->font)
#define FRAME_FONTSET(f)          (FRAME_X_OUTPUT (f)->fontset)
#define FRAME_BASELINE_OFFSET(f)  (FRAME_X_OUTPUT (f)->baseline_offset)
#define FRAME_X_DISPLAY(f)        (FRAME_DISPLAY_INFO (f)->display)
/* Wayland has no window ids; shared code prints this.  */
#define FRAME_NATIVE_WINDOW(f)    (FRAME_X_OUTPUT (f)->window_desc)

#define BLACK_PIX_DEFAULT(f) 0x000000
#define WHITE_PIX_DEFAULT(f) 0xFFFFFF

/* Turning a lisp vector value into a pointer to a struct scroll_bar.  */
#define XSCROLL_BAR(vec) ((struct scroll_bar *) XVECTOR (vec))

#define FRAME_DEFAULT_FACE(f) FACE_FROM_ID_OR_NULL (f, DEFAULT_FACE_ID)

/* There are no scroll bars, so text always starts at column 0.  */
#define FIRST_CHAR_POSITION(f) 0

/* waylandterm.c.  */
extern struct redisplay_interface wayland_redisplay_interface;
extern void syms_of_waylandterm (void);
extern void wayland_create_frame_surface (struct frame *);
extern void wayland_check_parent (struct wayland_display_info *,
				  struct frame *, Lisp_Object);
extern struct frame *wayland_frame_root (struct frame *);
extern void wayland_frame_root_position (struct frame *, int *, int *);
extern bool wayland_restack_frame (struct frame *, struct frame *, bool);
extern bool wayland_restack_child (struct frame *, bool);
extern void wayland_set_parent_frame (struct frame *, Lisp_Object, Lisp_Object);
extern void wayland_free_frame_resources (struct frame *);
extern void wayland_make_frame_visible (struct frame *);
extern void wayland_make_frame_invisible (struct frame *);
extern void wayland_set_frame_title (struct frame *, const char *);
extern void wayland_clear_under_internal_border (struct frame *);
extern void mark_waylandterm (void);
extern struct wayland_display_info *wayland_term_init (Lisp_Object, char *);
extern cairo_t *wayland_begin_cr_clip (struct frame *);
extern void wayland_end_cr_clip (struct frame *);
extern void wayland_fill_cairo_rectangle (struct frame *, cairo_t *,
					 unsigned long, int, int, int, int);
extern void wayland_damage_glyphs (struct frame *, cairo_t *,
                                  const cairo_glyph_t *, int);
extern int wayland_fixed_scale (struct frame *);
extern double wayland_text_scale;
extern void wayland_set_text_scale (double);
extern void wayland_scale_frame_elements (struct frame *);
extern void wayland_set_cr_source_with_color (struct frame *, unsigned long,
					      bool);
extern bool wayland_defined_color (struct frame *, const char *,
				   Emacs_Color *, bool, bool);
extern void wayland_query_colors (struct frame *, Emacs_Color *, int);

/* waylandterm.c, for waylanddraw.c.  */
extern void wayland_fill_rect (struct frame *, unsigned long, int, int, int,
			       int);
extern void wayland_cairo_damage (struct frame *, cairo_t *, double, double,
				  double, double);
extern void wayland_damage_fill (struct frame *, cairo_t *);
extern void wayland_stroke_path (struct frame *, cairo_t *);
extern void wayland_damage_glyph_string (struct glyph_string *);

/* waylanddraw.c.  */
extern void wayland_compute_glyph_string_overhangs (struct glyph_string *);
extern void wayland_draw_glyph_string (struct glyph_string *);
extern void wayland_draw_fringe_bitmap (struct window *, struct glyph_row *,
					struct draw_fringe_bitmap_params *);
extern void wayland_define_fringe_bitmap (int, unsigned short *, int, int);
extern void wayland_destroy_fringe_bitmap (int);
extern void wayland_draw_window_cursor (struct window *, struct glyph_row *,
					int, int, enum text_cursor_kinds, int,
					bool, bool);
extern void wayland_draw_vertical_window_border (struct window *, int, int,
						 int);
extern void wayland_draw_window_divider (struct window *, int, int, int, int);

/* waylandfns.c.  */
extern Lisp_Object wayland_menu_choices (int);
extern frame_parm_handler wayland_frame_parm_handlers[];
extern void syms_of_waylandfns (void);
extern void wayland_default_font_parameter (struct frame *, Lisp_Object);
extern void wayland_implicitly_set_name (struct frame *, Lisp_Object,
					 Lisp_Object);
extern void wayland_change_tab_bar_height (struct frame *, int);
extern void wayland_change_tool_bar_height (struct frame *, int);
extern struct wayland_display_info *check_wayland_display_info (Lisp_Object);

/* waylandselect.c.  */
extern void syms_of_waylandselect (void);
extern void wayland_selection_create
  (struct wayland_display_info *, struct wl_data_device_manager *,
   struct zwp_primary_selection_device_manager_v1 *, struct wl_seat *);
extern void wayland_selection_destroy (struct wayland_display_info *);
extern void wayland_selection_connection_lost (struct wayland_display_info *);
extern void wayland_selection_mark (struct wayland_display_info *);
extern bool wayland_selection_focused_p (struct wayland_display_info *);
extern bool wayland_display_has_pointer (struct wayland_display_info *);
extern uint32_t wayland_selection_serial (struct wayland_display_info *);
extern void wayland_selection_flush (struct wayland_display_info *);
extern bool wayland_selection_service (struct wayland_display_info *);

/* waylandterm.c, for waylandim.c.  */
union buffered_input_event;
struct zwp_text_input_manager_v3;
extern struct frame *wayland_surface_frame (struct wayland_display_info *,
					    struct wl_surface *);
extern void wayland_queue_input_event (struct wayland_display_info *,
				       union buffered_input_event *);
extern void wayland_queue_utf8 (struct wayland_display_info *,
				struct frame *, const char *, int,
				uint32_t);
extern int wayland_frame_scale (struct frame *);

/* waylandim.c.  */
extern void syms_of_waylandim (void);
extern void wayland_im_create (struct wayland_display_info *,
			       struct zwp_text_input_manager_v3 *,
			       struct wl_seat *);
extern void wayland_im_destroy (struct wayland_display_info *);
extern void wayland_im_forget_frame (struct wayland_display_info *,
				     struct frame *, struct wl_surface *);
extern void wayland_im_focus_changed (struct wayland_display_info *,
				     struct frame *, struct frame *);
extern void wayland_im_set_cursor_rectangle (struct frame *, int, int, int,
					     int);

#endif /* HAVE_WAYLAND */
#endif /* WAYLANDTERM_H */
