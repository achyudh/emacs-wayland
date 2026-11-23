/* Frame and display functions for the native Wayland backend.

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

/* Frame creation follows haikufns.c, and the parameter handlers follow
   androidfns.c, the closest existing backend: no external menu bar,
   no scroll bars, and Emacs-drawn tool and tab bars.  */

#include <config.h>

#include "lisp.h"
#include "blockinput.h"
#include "buffer.h"
#include "coding.h"
#include "fontset.h"
#include "keyboard.h"
#include "menu.h"
#include "termhooks.h"
#include "window.h"
#include "waylandterm.h"

struct wayland_display_info *
check_wayland_display_info (Lisp_Object object)
{
  struct terminal *t;

  if (NILP (object))
    {
      struct frame *sf = XFRAME (selected_frame);

      if (FRAME_WAYLAND_P (sf) && FRAME_LIVE_P (sf))
	return FRAME_DISPLAY_INFO (sf);
      if (x_display_list)
	return x_display_list;
      error ("No Wayland display is open");
    }
  if (FRAMEP (object))
    {
      struct frame *f = decode_window_system_frame (object);
      return FRAME_DISPLAY_INFO (f);
    }
  if (STRINGP (object))
    {
      struct wayland_display_info *dpyinfo = wayland_display_by_name (object);
      if (dpyinfo)
	return dpyinfo;
      error ("No Wayland display named %s is open", SSDATA (object));
    }
  t = decode_live_terminal (object);
  if (t->type != output_wayland)
    error ("Terminal %d is not a Wayland display", t->id);
  return t->display_info.wayland;
}

/* The name shared code (frame.c) uses.  */

Display_Info *
check_x_display_info (Lisp_Object object)
{
  return check_wayland_display_info (object);
}

/* Colors.  */

static unsigned long
wayland_decode_color (struct frame *f, Lisp_Object color_name,
		      unsigned long fallback)
{
  Emacs_Color color;

  CHECK_STRING (color_name);
  if (wayland_defined_color (f, SSDATA (color_name), &color, true, false))
    return color.pixel;
  signal_error ("Undefined color", color_name);
  return fallback;
}

static void
wayland_set_background_color (struct frame *f, Lisp_Object arg,
			      Lisp_Object oldval)
{
  FRAME_BACKGROUND_COLOR (f)
    = wayland_decode_color (f, arg, WHITE_PIX_DEFAULT (f));
  if (FRAME_X_OUTPUT (f)->surface)
    {
      update_face_from_frame_parameter (f, Qbackground_color, arg);
      if (FRAME_VISIBLE_P (f))
	redraw_frame (f);
    }
}

static void
wayland_set_foreground_color (struct frame *f, Lisp_Object arg,
			      Lisp_Object oldval)
{
  unsigned long old = FRAME_FOREGROUND_COLOR (f);

  FRAME_FOREGROUND_COLOR (f)
    = wayland_decode_color (f, arg, BLACK_PIX_DEFAULT (f));
  if (FRAME_CURSOR_COLOR (f) == old)
    FRAME_CURSOR_COLOR (f) = FRAME_FOREGROUND_COLOR (f);
  if (FRAME_X_OUTPUT (f)->surface)
    {
      update_face_from_frame_parameter (f, Qforeground_color, arg);
      if (FRAME_VISIBLE_P (f))
	redraw_frame (f);
    }
}

static void
wayland_set_cursor_color (struct frame *f, Lisp_Object arg,
			  Lisp_Object oldval)
{
  unsigned long pixel = wayland_decode_color (f, arg, BLACK_PIX_DEFAULT (f));

  /* Make sure the cursor differs from the background.  */
  if (pixel == FRAME_BACKGROUND_COLOR (f))
    pixel = FRAME_FOREGROUND_COLOR (f);
  FRAME_CURSOR_COLOR (f) = pixel;
  FRAME_X_OUTPUT (f)->cursor_foreground_color = FRAME_BACKGROUND_COLOR (f);

  if (FRAME_X_OUTPUT (f)->surface && FRAME_VISIBLE_P (f))
    {
      gui_update_cursor (f, false);
      gui_update_cursor (f, true);
    }
  update_face_from_frame_parameter (f, Qcursor_color, arg);
}

static void
wayland_set_cursor_type (struct frame *f, Lisp_Object arg,
			 Lisp_Object oldval)
{
  set_frame_cursor_types (f, arg);
}

static void
wayland_set_mouse_color (struct frame *f, Lisp_Object arg,
			 Lisp_Object oldval)
{
  /* Keep the color for faces.  Pointer shapes are chosen separately.  */
  FRAME_X_OUTPUT (f)->mouse_color
    = wayland_decode_color (f, arg, BLACK_PIX_DEFAULT (f));
  update_face_from_frame_parameter (f, Qmouse_color, arg);
}

/* Borders and bars.  */

static void
wayland_set_internal_border_width (struct frame *f, Lisp_Object arg,
				   Lisp_Object oldval)
{
  int border = min ((int64_t) check_int_nonnegative (arg)
		    * wayland_fixed_scale (f), INT_MAX / 4);

  if (border != FRAME_INTERNAL_BORDER_WIDTH (f))
    {
      f->internal_border_width = border;
      if (FRAME_X_OUTPUT (f)->surface)
	{
	  adjust_frame_size (f, -1, -1, 3, false, Qinternal_border_width);
	  wayland_clear_under_internal_border (f);
	}
    }
}

static int
wayland_frame_fringe_width (struct frame *f, Lisp_Object width)
{
  int value = (RANGED_FIXNUMP (-INT_MAX, width, INT_MAX)
	       ? eabs (XFIXNUM (width)) : 8);
  return min ((int64_t) value * wayland_fixed_scale (f), INT_MAX / 4);
}

static void
wayland_set_left_fringe (struct frame *f, Lisp_Object arg, Lisp_Object oldval)
{
  gui_set_left_fringe (f, make_fixnum (wayland_frame_fringe_width (f, arg)),
		      oldval);
}

static void
wayland_set_right_fringe (struct frame *f, Lisp_Object arg, Lisp_Object oldval)
{
  gui_set_right_fringe (f, make_fixnum (wayland_frame_fringe_width (f, arg)),
		       oldval);
}

/* Recompute defaults from the stored logical parameters.  The scale
   change applies the new frame size after the font is selected.  */
void
wayland_scale_frame_elements (struct frame *f)
{
  f->left_fringe_width = wayland_frame_fringe_width
    (f, get_frame_param (f, Qleft_fringe));
  f->right_fringe_width = wayland_frame_fringe_width
    (f, get_frame_param (f, Qright_fringe));
  Lisp_Object border = get_frame_param (f, Qinternal_border_width);
  if (RANGED_FIXNUMP (0, border, INT_MAX))
    f->internal_border_width = min (XFIXNUM (border)
				    * wayland_fixed_scale (f), INT_MAX / 4);
}

static void
wayland_set_child_frame_border_width (struct frame *f, Lisp_Object arg,
				      Lisp_Object oldval)
{
  int border;

  if (NILP (arg))
    border = -1;
  else if (RANGED_FIXNUMP (0, arg, INT_MAX))
    border = XFIXNAT (arg);
  else
    signal_error ("Invalid child frame border width", arg);

  if (border != FRAME_CHILD_FRAME_BORDER_WIDTH (f))
    {
      f->child_frame_border_width = border;
      if (FRAME_X_OUTPUT (f)->surface)
	{
	  adjust_frame_size (f, -1, -1, 3, false, Qchild_frame_border_width);
	  wayland_clear_under_internal_border (f);
	}
    }
}

/* The menu bar is drawn by Emacs in the frame (HAVE_EXT_MENU_BAR is
   undefined), as without an X toolkit.  */

static void
wayland_set_menu_bar_lines (struct frame *f, Lisp_Object value,
			    Lisp_Object oldval)
{
  int nlines;
  int olines = FRAME_MENU_BAR_LINES (f);

  if (FRAME_MINIBUF_ONLY_P (f) || FRAME_PARENT_FRAME (f))
    return;

  nlines = TYPE_RANGED_FIXNUMP (int, value) ? XFIXNUM (value) : 0;

  fset_redisplay (f);
  FRAME_MENU_BAR_LINES (f) = nlines;
  FRAME_MENU_BAR_HEIGHT (f) = nlines * FRAME_LINE_HEIGHT (f);
  if (nlines != olines)
    {
      adjust_frame_size (f, -1, -1, 3, true, Qmenu_bar_lines);
      if (nlines == 0 && WINDOWP (f->menu_bar_window))
	clear_glyph_matrix (XWINDOW (f->menu_bar_window)->current_matrix);
    }
  adjust_frame_glyphs (f);
  if (FRAME_X_OUTPUT (f)->surface)
    SET_FRAME_GARBAGED (f);
}

void
wayland_change_tool_bar_height (struct frame *f, int height)
{
  int unit = FRAME_LINE_HEIGHT (f);
  int old_height = FRAME_TOOL_BAR_HEIGHT (f);
  int lines = (height + unit - 1) / unit;
  Lisp_Object fullscreen = get_frame_param (f, Qfullscreen);

  fset_redisplay (f);
  FRAME_TOOL_BAR_HEIGHT (f) = height;
  FRAME_TOOL_BAR_LINES (f) = lines;
  store_frame_param (f, Qtool_bar_lines, make_fixnum (lines));

  if (FRAME_X_OUTPUT (f)->surface && FRAME_TOOL_BAR_HEIGHT (f) == 0)
    {
      clear_frame (f);
      clear_current_matrices (f);
    }

  if (height < old_height && WINDOWP (f->tool_bar_window))
    clear_glyph_matrix (XWINDOW (f->tool_bar_window)->current_matrix);

  if (!f->tool_bar_resized)
    {
      if (NILP (fullscreen) || EQ (fullscreen, Qfullwidth))
	adjust_frame_size (f, FRAME_TEXT_WIDTH (f), FRAME_TEXT_HEIGHT (f),
			   1, false, Qtool_bar_lines);
      else
	adjust_frame_size (f, -1, -1, 4, false, Qtool_bar_lines);
      f->tool_bar_resized = f->tool_bar_redisplayed;
    }
  else
    adjust_frame_size (f, -1, -1, 3, false, Qtool_bar_lines);

  adjust_frame_glyphs (f);
  SET_FRAME_GARBAGED (f);
}

static void
wayland_set_tool_bar_lines (struct frame *f, Lisp_Object value,
			    Lisp_Object oldval)
{
  if (FRAME_MINIBUF_ONLY_P (f))
    return;
  wayland_change_tool_bar_height (f, (RANGED_FIXNUMP (0, value, INT_MAX)
				      ? XFIXNAT (value) : 0)
				  * FRAME_LINE_HEIGHT (f));
}

static void
wayland_set_tool_bar_position (struct frame *f, Lisp_Object new_value,
			       Lisp_Object old_value)
{
  if (!EQ (new_value, Qtop) && !EQ (new_value, Qbottom))
    error ("Tool bar position must be either `top' or `bottom'");
  if (EQ (new_value, old_value))
    return;
  fset_tool_bar_position (f, new_value);
  adjust_frame_size (f, -1, -1, 3, false, Qtool_bar_position);
  adjust_frame_glyphs (f);
  SET_FRAME_GARBAGED (f);
  if (FRAME_X_OUTPUT (f)->surface)
    wayland_clear_under_internal_border (f);
}

void
wayland_change_tab_bar_height (struct frame *f, int height)
{
  int unit = FRAME_LINE_HEIGHT (f);
  int old_height = FRAME_TAB_BAR_HEIGHT (f);
  /* Not rounded up, unlike the tool bar (bug#59285).  */
  int lines = height / unit;
  Lisp_Object fullscreen = get_frame_param (f, Qfullscreen);

  if (lines == 0 && height != 0)
    lines = 1;

  fset_redisplay (f);
  FRAME_TAB_BAR_HEIGHT (f) = height;
  FRAME_TAB_BAR_LINES (f) = lines;
  store_frame_param (f, Qtab_bar_lines, make_fixnum (lines));

  if (FRAME_X_OUTPUT (f)->surface && FRAME_TAB_BAR_HEIGHT (f) == 0)
    {
      clear_frame (f);
      clear_current_matrices (f);
    }

  if (height < old_height && WINDOWP (f->tab_bar_window))
    clear_glyph_matrix (XWINDOW (f->tab_bar_window)->current_matrix);

  if (!f->tab_bar_resized)
    {
      if (NILP (fullscreen) || EQ (fullscreen, Qfullwidth))
	adjust_frame_size (f, FRAME_TEXT_WIDTH (f), FRAME_TEXT_HEIGHT (f),
			   1, false, Qtab_bar_lines);
      else
	adjust_frame_size (f, -1, -1, 4, false, Qtab_bar_lines);
      f->tab_bar_resized = f->tab_bar_redisplayed;
    }
  else
    adjust_frame_size (f, -1, -1, 3, false, Qtab_bar_lines);

  adjust_frame_glyphs (f);
  SET_FRAME_GARBAGED (f);
}

static void
wayland_set_tab_bar_lines (struct frame *f, Lisp_Object value,
			   Lisp_Object oldval)
{
  int olines = FRAME_TAB_BAR_LINES (f);
  int nlines;

  if (FRAME_MINIBUF_ONLY_P (f))
    return;
  nlines = RANGED_FIXNUMP (0, value, INT_MAX) ? XFIXNAT (value) : 0;
  if (nlines != olines && (olines == 0 || nlines == 0))
    wayland_change_tab_bar_height (f, nlines * FRAME_LINE_HEIGHT (f));
}

/* Names and titles.  */

static void
wayland_set_name_internal (struct frame *f, Lisp_Object name)
{
  if (FRAME_X_OUTPUT (f)->surface)
    {
      Lisp_Object encoded = ENCODE_UTF_8 (name);
      wayland_set_frame_title (f, SSDATA (encoded));
    }
}

static void
wayland_set_name (struct frame *f, Lisp_Object name, bool explicit)
{
  if (explicit)
    {
      if (f->explicit_name && NILP (name))
	update_mode_lines = 37;
      f->explicit_name = !NILP (name);
    }
  else if (f->explicit_name)
    return;

  if (NILP (name))
    name = Vinvocation_name;
  else
    CHECK_STRING (name);

  if (!NILP (Fstring_equal (name, f->name)))
    return;
  fset_name (f, name);

  /* The title parameter overrides the name.  */
  if (!NILP (f->title))
    name = f->title;
  wayland_set_name_internal (f, name);
}

static void
wayland_explicitly_set_name (struct frame *f, Lisp_Object arg,
			     Lisp_Object oldval)
{
  wayland_set_name (f, arg, true);
}

void
wayland_implicitly_set_name (struct frame *f, Lisp_Object arg,
			     Lisp_Object oldval)
{
  wayland_set_name (f, arg, false);
}

static void
wayland_set_title (struct frame *f, Lisp_Object name, Lisp_Object old_name)
{
  if (EQ (name, f->title))
    return;
  update_mode_lines = 38;
  fset_title (f, name);
  if (NILP (name))
    name = f->name;
  else
    CHECK_STRING (name);
  wayland_set_name_internal (f, name);
}

static void
wayland_set_no_accept_focus (struct frame *f, Lisp_Object value,
			     Lisp_Object old_value)
{
  FRAME_NO_ACCEPT_FOCUS (f) = !NILP (value);
}

static void
wayland_set_z_group (struct frame *f, Lisp_Object value, Lisp_Object old_value)
{
  if (NILP (value) && FRAME_Z_GROUP_NONE (f))
    return;
  if (wayland_restack_child (f, !EQ (value, Qbelow)))
    FRAME_Z_GROUP (f) = EQ (value, Qbelow) ? z_group_below
      : EQ (value, Qabove) ? z_group_above : z_group_none;
}

/* The handler table, one entry per entry of frame.c's frame_parms.
   NULL means the parameter is stored but has no effect.  */

frame_parm_handler wayland_frame_parm_handlers[] =
  {
    gui_set_autoraise,
    gui_set_autolower,
    wayland_set_background_color,
    NULL, /* border-color: the compositor draws borders */
    gui_set_border_width,
    wayland_set_cursor_color,
    wayland_set_cursor_type,
    gui_set_font,
    wayland_set_foreground_color,
    NULL, /* icon-name */
    NULL, /* icon-type */
    wayland_set_child_frame_border_width,
    wayland_set_internal_border_width,
    gui_set_right_divider_width,
    gui_set_bottom_divider_width,
    wayland_set_menu_bar_lines,
    wayland_set_mouse_color,
    wayland_explicitly_set_name,
    NULL, /* scroll-bar-width: no scroll bars */
    NULL, /* scroll-bar-height */
    wayland_set_title,
    gui_set_unsplittable,
    NULL, /* vertical-scroll-bars */
    NULL, /* horizontal-scroll-bars */
    gui_set_visibility,
    wayland_set_tab_bar_lines,
    wayland_set_tool_bar_lines,
    NULL, /* scroll-bar-foreground */
    NULL, /* scroll-bar-background */
    gui_set_screen_gamma,
    gui_set_line_spacing,
    wayland_set_left_fringe,
    wayland_set_right_fringe,
    NULL, /* wait-for-wm */
    gui_set_fullscreen,
    gui_set_font_backend,
    gui_set_alpha,
    NULL, /* sticky */
    wayland_set_tool_bar_position,
    NULL, /* inhibit-double-buffering: always double-buffered */
    NULL, /* undecorated: server-side decorations only */
    wayland_set_parent_frame,
    NULL, /* skip-taskbar */
    NULL, /* no-focus-on-map */
    wayland_set_no_accept_focus,
    wayland_set_z_group,
    NULL, /* override-redirect */
    gui_set_no_special_glyphs,
    gui_set_alpha_background,
    gui_set_borders_respect_alpha_background,
    NULL, /* use-frame-synchronization */
    NULL, /* display */
    NULL, /* left */
    NULL, /* top */
    NULL, /* width */
    NULL, /* height */
    NULL, /* parent-id */
    NULL, /* window-id */
    NULL, /* outer-window-id */
    NULL, /* explicit-name */
    NULL, /* modeline */
    NULL, /* buffer-list */
    NULL, /* buried-buffer-list */
  };

/* Fonts.  */

/* The desktop's monospace and interface fonts, as names like "Adwaita
   Mono 11", or nil.  wayland-win.el reads them from the settings
   portal and passes them to `wayland--set-desktop-settings'.  */

static Lisp_Object system_monospace_font, system_normal_font;

void
wayland_default_font_parameter (struct frame *f, Lisp_Object parms)
{
  struct wayland_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);
  Lisp_Object font = gui_display_get_arg (dpyinfo, parms, Qfont, "font",
					  "Font", RES_TYPE_STRING);

  if (BASE_EQ (font, Qunbound))
    font = Qnil;
  /* The system font comes before the fallbacks whether or not
     `font-use-system-font' is set, as on X and PGTK: the init file
     may not have been read yet.  */
  if (!FONTP (font) && !STRINGP (font) && STRINGP (system_monospace_font))
    font = font_open_by_name (f, system_monospace_font);
  if (!FONTP (font) && !STRINGP (font))
    {
      static const char *const names[] =
	{ "Monospace-10", "DejaVu Sans Mono-10", "Noto Sans Mono-10",
	  "monospace", NULL };

      for (int i = 0; names[i]; i++)
	{
	  font = font_open_by_name (f, build_unibyte_string (names[i]));
	  if (!NILP (font))
	    break;
	}
      if (NILP (font))
	error ("No suitable font was found");
    }
  gui_default_parameter (f, parms, Qfont, font, "font", "Font",
			 RES_TYPE_STRING);
}

/* Frame creation.  */

static Lisp_Object
unwind_create_frame (Lisp_Object frame)
{
  struct frame *f = XFRAME (frame);

  /* Nothing to do if the frame is dead or already official.  */
  if (!FRAME_LIVE_P (f) || !NILP (Fmemq (frame, Vframe_list)))
    return Qnil;
  wayland_free_frame_resources (f);
  free_glyphs (f);
  xfree (f->output_data.wayland);
  f->output_data.wayland = NULL;
  return Qt;
}

static void
do_unwind_create_frame (Lisp_Object frame)
{
  unwind_create_frame (frame);
}

DEFUN ("x-create-frame", Fx_create_frame, Sx_create_frame, 1, 1, 0,
       doc: /* SKIP: real doc in xfns.c.  */)
  (Lisp_Object parms)
{
  struct frame *f;
  Lisp_Object frame, tem, name, display, visibility, parent;
  bool minibuffer_only = false;
  specpdl_ref count = SPECPDL_INDEX ();
  struct wayland_display_info *dpyinfo;
  struct kboard *kb;

  parms = Fcopy_alist (parms);
  Vx_resource_name = Vinvocation_name;

  display = gui_display_get_arg (NULL, parms, Qterminal, 0, 0,
				 RES_TYPE_STRING);
  if (BASE_EQ (display, Qunbound) || NILP (display))
    {
      display = gui_display_get_arg (NULL, parms, Qdisplay, 0, 0,
				     RES_TYPE_STRING);
      if (BASE_EQ (display, Qunbound))
	display = Qnil;
      dpyinfo = STRINGP (display)
	? wayland_term_init (display, NULL)
	: check_wayland_display_info (display);
    }
  else
    dpyinfo = check_wayland_display_info (display);
  XSETTERMINAL (display, dpyinfo->terminal);
  kb = dpyinfo->terminal->kboard;

  if (!dpyinfo->terminal->name)
    error ("Terminal is not live, can't create new frames on it");
  if (dpyinfo->state == WAYLAND_DISPLAY_DEAD)
    error ("The Wayland connection is lost");

  parent = gui_display_get_arg (dpyinfo, parms, Qparent_frame, NULL, NULL,
				RES_TYPE_SYMBOL);
  if (BASE_EQ (parent, Qunbound))
    parent = Qnil;
  wayland_check_parent (dpyinfo, NULL, parent);

  name = gui_display_get_arg (dpyinfo, parms, Qname, 0, 0, RES_TYPE_STRING);
  if (!STRINGP (name) && !BASE_EQ (name, Qunbound) && !NILP (name))
    error ("Invalid frame name--not a string or nil");
  if (STRINGP (name))
    Vx_resource_name = name;

  tem = gui_display_get_arg (dpyinfo, parms, Qminibuffer, "minibuffer",
			     "Minibuffer", RES_TYPE_SYMBOL);
  if (EQ (tem, Qnone) || NILP (tem))
    f = make_frame_without_minibuffer (Qnil, kb, display);
  else if (EQ (tem, Qonly))
    {
      f = make_minibuffer_frame ();
      minibuffer_only = true;
    }
  else if (WINDOWP (tem))
    f = make_frame_without_minibuffer (tem, kb, display);
  else
    f = make_frame (true);

  XSETFRAME (frame, f);
  frame_set_id_from_params (f, parms);

  f->terminal = dpyinfo->terminal;
  f->output_method = output_wayland;
  f->output_data.wayland = xzalloc (sizeof *f->output_data.wayland);
  FRAME_FONTSET (f) = -1;
  f->output_data.wayland->relief_background = -1;
  f->output_data.wayland->icon_bitmap = -1;
  f->output_data.wayland->resx = dpyinfo->resx;
  f->output_data.wayland->resy = dpyinfo->resy;
  fset_parent_frame (f, parent);
  if (!NILP (parent))
    {
      f->output_data.wayland->resx = FRAME_X_OUTPUT (XFRAME (parent))->resx;
      f->output_data.wayland->resy = FRAME_X_OUTPUT (XFRAME (parent))->resy;
      store_frame_param (f, Qparent_frame, parent);
    }

  fset_icon_name (f, gui_display_get_arg (dpyinfo, parms, Qicon_name,
					  "iconName", "Title",
					  RES_TYPE_STRING));
  if (!STRINGP (f->icon_name))
    fset_icon_name (f, Qnil);

  FRAME_DISPLAY_INFO (f) = dpyinfo;

  /* With FRAME_DISPLAY_INFO set up, this unwind-protect is safe.  */
  record_unwind_protect (do_unwind_create_frame, frame);

  if (BASE_EQ (name, Qunbound) || NILP (name) || !STRINGP (name))
    {
      fset_name (f, Vinvocation_name);
      f->explicit_name = false;
    }
  else
    {
      fset_name (f, name);
      f->explicit_name = true;
      specbind (Qx_resource_name, name);
    }

  register_font_driver (&ftcrfont_driver, f);
#ifdef HAVE_HARFBUZZ
  register_font_driver (&ftcrhbfont_driver, f);
#endif

  gui_default_parameter (f, parms, Qfont_backend, Qnil, "fontBackend",
			 "FontBackend", RES_TYPE_STRING);
  wayland_default_font_parameter (f, parms);
  if (!FRAME_FONT (f))
    {
      delete_frame (frame, Qnoelisp);
      error ("Invalid frame font");
    }

  gui_default_parameter (f, parms, Qborder_width, make_fixnum (0),
			 "borderwidth", "BorderWidth", RES_TYPE_NUMBER);
  gui_default_parameter (f, parms, Qinternal_border_width, make_fixnum (0),
			 "internalBorderWidth", "InternalBorderWidth",
			 RES_TYPE_NUMBER);
  gui_default_parameter (f, parms, Qchild_frame_border_width, Qnil,
			 "childFrameBorderWidth", "childFrameBorderWidth",
			 RES_TYPE_NUMBER);
  gui_default_parameter (f, parms, Qright_divider_width, make_fixnum (0),
			 NULL, NULL, RES_TYPE_NUMBER);
  gui_default_parameter (f, parms, Qbottom_divider_width, make_fixnum (0),
			 NULL, NULL, RES_TYPE_NUMBER);
  /* No scroll bars.  */
  gui_default_parameter (f, parms, Qvertical_scroll_bars, Qnil,
			 "verticalScrollBars", "ScrollBars", RES_TYPE_SYMBOL);
  gui_default_parameter (f, parms, Qhorizontal_scroll_bars, Qnil,
			 "horizontalScrollBars", "ScrollBars",
			 RES_TYPE_SYMBOL);
  gui_default_parameter (f, parms, Qforeground_color, build_string ("black"),
			 "foreground", "Foreground", RES_TYPE_STRING);
  gui_default_parameter (f, parms, Qbackground_color, build_string ("white"),
			 "background", "Background", RES_TYPE_STRING);
  gui_default_parameter (f, parms, Qmouse_color, build_string ("black"),
			 "pointerColor", "Foreground", RES_TYPE_STRING);
  gui_default_parameter (f, parms, Qcursor_color, build_string ("black"),
			 "cursorColor", "Foreground", RES_TYPE_STRING);
  gui_default_parameter (f, parms, Qline_spacing, Qnil, "lineSpacing",
			 "LineSpacing", RES_TYPE_NUMBER);
  gui_default_parameter (f, parms, Qleft_fringe, Qnil, "leftFringe",
			 "LeftFringe", RES_TYPE_NUMBER);
  gui_default_parameter (f, parms, Qright_fringe, Qnil, "rightFringe",
			 "RightFringe", RES_TYPE_NUMBER);
  gui_default_parameter (f, parms, Qno_special_glyphs, Qnil, NULL, NULL,
			 RES_TYPE_BOOLEAN);

  init_frame_faces (f);

  /* Read comment about this code in corresponding place in xfns.c.  */
  tem = gui_display_get_arg (dpyinfo, parms, Qmin_width, NULL, NULL,
			     RES_TYPE_NUMBER);
  if (FIXNUMP (tem))
    store_frame_param (f, Qmin_width, tem);
  tem = gui_display_get_arg (dpyinfo, parms, Qmin_height, NULL, NULL,
			     RES_TYPE_NUMBER);
  if (FIXNUMP (tem))
    store_frame_param (f, Qmin_height, tem);

  adjust_frame_size (f, FRAME_COLS (f) * FRAME_COLUMN_WIDTH (f),
		     FRAME_LINES (f) * FRAME_LINE_HEIGHT (f), 5, true,
		     Qx_create_frame_1);

  /* The resources controlling the menu-bar, tool-bar and tab-bar are
     processed specially at startup and reflected in the mode
     variables; ignore them here.  */
  gui_default_parameter (f, parms, Qmenu_bar_lines,
			 NILP (Vmenu_bar_mode) ? make_fixnum (0)
			 : make_fixnum (1), NULL, NULL, RES_TYPE_NUMBER);
  gui_default_parameter (f, parms, Qtab_bar_lines,
			 NILP (Vtab_bar_mode) ? make_fixnum (0)
			 : make_fixnum (1), NULL, NULL, RES_TYPE_NUMBER);
  gui_default_parameter (f, parms, Qtool_bar_lines,
			 NILP (Vtool_bar_mode) ? make_fixnum (0)
			 : make_fixnum (1), NULL, NULL, RES_TYPE_NUMBER);
  gui_default_parameter (f, parms, Qbuffer_predicate, Qnil,
			 "bufferPredicate", "BufferPredicate",
			 RES_TYPE_SYMBOL);
  gui_default_parameter (f, parms, Qtitle, Qnil, "title", "Title",
			 RES_TYPE_STRING);
  gui_default_parameter (f, parms, Qno_accept_focus, Qnil, NULL, NULL,
			 RES_TYPE_BOOLEAN);

  gui_figure_window_size (f, parms, false, true);

  tem = gui_display_get_arg (dpyinfo, parms, Qunsplittable, 0, 0,
			     RES_TYPE_BOOLEAN);
  f->no_split = minibuffer_only || (!BASE_EQ (tem, Qunbound) && !NILP (tem));

  /* Create the surface and its role objects; nothing is committed until
     the frame is made visible.  The name was set before the surface
     existed, so send it now.  */
  wayland_create_frame_surface (f);
  wayland_set_name_internal (f, NILP (f->title) ? f->name : f->title);

  /* Nothing between here and Vframe_list may signal, or the terminal's
     reference count would leak.  */
  f->terminal->reference_count++;
  Vframe_list = Fcons (frame, Vframe_list);

  gui_default_parameter (f, parms, Qz_group, Qnil, NULL, NULL, RES_TYPE_SYMBOL);
  gui_default_parameter (f, parms, Qicon_type, Qnil, "bitmapIcon",
			 "BitmapIcon", RES_TYPE_SYMBOL);
  gui_default_parameter (f, parms, Qauto_raise, Qnil, "autoRaise",
			 "AutoRaiseLower", RES_TYPE_BOOLEAN);
  gui_default_parameter (f, parms, Qauto_lower, Qnil, "autoLower",
			 "AutoLower", RES_TYPE_BOOLEAN);
  gui_default_parameter (f, parms, Qcursor_type, Qbox, "cursorType",
			 "CursorType", RES_TYPE_SYMBOL);
  gui_default_parameter (f, parms, Qalpha, Qnil, "alpha", "Alpha",
			 RES_TYPE_NUMBER);
  gui_default_parameter (f, parms, Qalpha_background, Qnil,
			 "alphaBackground", "AlphaBackground",
			 RES_TYPE_NUMBER);
  gui_default_parameter (f, parms, Qfullscreen, Qnil, "fullscreen",
			 "Fullscreen", RES_TYPE_SYMBOL);

  f->can_set_window_size = true;
  adjust_frame_size (f, FRAME_TEXT_WIDTH (f), FRAME_TEXT_HEIGHT (f), 0,
		     true, Qx_create_frame_2);

  visibility = gui_display_get_arg (dpyinfo, parms, Qvisibility, 0, 0,
				    RES_TYPE_SYMBOL);
  if (BASE_EQ (visibility, Qunbound))
    visibility = Qt;
  if (!NILP (visibility))
    wayland_make_frame_visible (f);
  else
    f->was_invisible = true;

  if (FRAME_HAS_MINIBUF_P (f)
      && (!FRAMEP (KVAR (kb, Vdefault_minibuffer_frame))
	  || !FRAME_LIVE_P (XFRAME (KVAR (kb, Vdefault_minibuffer_frame)))))
    kset_default_minibuffer_frame (kb, frame);

  for (tem = parms; CONSP (tem); tem = XCDR (tem))
    if (CONSP (XCAR (tem)) && !NILP (XCAR (XCAR (tem))))
      fset_param_alist (f, Fcons (XCAR (tem), f->param_alist));

  /* Make sure windows on this frame appear in calls to next-window
     and similar functions.  */
  Vwindow_list = Qnil;

  return unbind_to (count, frame);
}

/* Connections.  */

/* Unwind function for `x-open-connection' with MUST-SUCCEED.  */

static void
wayland_fatal_connection_failure (void)
{
  fatal ("Could not open the Wayland display");
}

DEFUN ("x-open-connection", Fx_open_connection, Sx_open_connection,
       1, 3, 0,
       doc: /* SKIP: real doc in xfns.c.  */)
  (Lisp_Object display, Lisp_Object resource_string, Lisp_Object must_succeed)
{
  CHECK_STRING (display);
  if (!NILP (resource_string))
    CHECK_STRING (resource_string);

  /* "" means the display WAYLAND_DISPLAY or WAYLAND_SOCKET names.  With
     MUST-SUCCEED, a failure is fatal.  */
  if (NILP (must_succeed))
    wayland_term_init (display, NULL);
  else
    {
      specpdl_ref count = SPECPDL_INDEX ();
      record_unwind_protect_void (wayland_fatal_connection_failure);
      wayland_term_init (display, NULL);
      clear_unwind_protect (count);
      unbind_to (count, Qnil);
    }
  return Qnil;
}

DEFUN ("x-close-connection", Fx_close_connection, Sx_close_connection,
       1, 1, 0,
       doc: /* SKIP: real doc in xfns.c.  */)
  (Lisp_Object terminal)
{
  struct wayland_display_info *dpyinfo = check_wayland_display_info (terminal);
  XSETTERMINAL (terminal, dpyinfo->terminal);
  Fdelete_terminal (terminal, Qnil);
  return Qnil;
}

DEFUN ("x-display-list", Fx_display_list, Sx_display_list, 0, 0, 0,
       doc: /* Return a list of the open Wayland display names.  */)
  (void)
{
  Lisp_Object displays = Qnil;
  for (struct wayland_display_info *dpyinfo = x_display_list;
       dpyinfo; dpyinfo = dpyinfo->next)
    if (dpyinfo->state == WAYLAND_DISPLAY_LIVE)
      displays = Fcons (XCAR (dpyinfo->name_list_element), displays);
  return Fnreverse (displays);
}

/* Color primitives used by the shared face code.  */

DEFUN ("xw-color-defined-p", Fxw_color_defined_p, Sxw_color_defined_p, 1, 2, 0,
       doc: /* SKIP: real doc in xfns.c.  */)
  (Lisp_Object color, Lisp_Object frame)
{
  struct frame *f = decode_window_system_frame (frame);
  Emacs_Color rgb;
  CHECK_STRING (color);
  return wayland_defined_color (f, SSDATA (color), &rgb, false, false)
    ? Qt : Qnil;
}

DEFUN ("xw-color-values", Fxw_color_values, Sxw_color_values, 1, 2, 0,
       doc: /* SKIP: real doc in xfns.c.  */)
  (Lisp_Object color, Lisp_Object frame)
{
  struct frame *f = decode_window_system_frame (frame);
  Emacs_Color rgb;
  CHECK_STRING (color);
  return wayland_defined_color (f, SSDATA (color), &rgb, false, false)
    ? list3i (rgb.red, rgb.green, rgb.blue) : Qnil;
}

DEFUN ("xw-display-color-p", Fxw_display_color_p, Sxw_display_color_p, 0, 1, 0,
       doc: /* SKIP: real doc in xfns.c.  */)
  (Lisp_Object terminal)
{
  return Qt;
}

DEFUN ("x-display-grayscale-p", Fx_display_grayscale_p,
       Sx_display_grayscale_p, 0, 1, 0,
       doc: /* SKIP: real doc in xfns.c.  */)
  (Lisp_Object terminal)
{
  /* Frames are drawn in 24-bit color.  */
  return Qt;
}

DEFUN ("font-get-system-font", Ffont_get_system_font, Sfont_get_system_font,
       0, 0, 0,
       doc: /* SKIP: real doc in xsettings.c.  */)
  (void)
{
  return system_monospace_font;
}

DEFUN ("font-get-system-normal-font", Ffont_get_system_normal_font,
       Sfont_get_system_normal_font, 0, 0, 0,
       doc: /* SKIP: real doc in xsettings.c.  */)
  (void)
{
  return system_normal_font;
}

DEFUN ("wayland--set-desktop-settings", Fwayland__set_desktop_settings,
       Swayland__set_desktop_settings, 3, 3, 0,
       doc: /* Record the desktop's font names and text scaling factor.
MONOSPACE and NORMAL name the monospace and interface fonts, and
TEXT-SCALE is the factor.  Each is nil when the desktop does not say.  TEXT-SCALE multiplies the
resolution fonts are sized at, as GNOME's text scaling factor does for
GTK; changing it resizes the fonts of every Wayland frame.  For
wayland-win.el.  */)
  (Lisp_Object monospace, Lisp_Object normal, Lisp_Object text_scale)
{
  if (!NILP (monospace))
    CHECK_STRING (monospace);
  if (!NILP (normal))
    CHECK_STRING (normal);
  double factor = 1;
  if (!NILP (text_scale))
    {
      CHECK_NUMBER (text_scale);
      /* GNOME's own range.  */
      factor = XFLOATINT (text_scale);
      factor = !(factor >= 0.5) ? 0.5 : min (factor, 3.0);
    }
  system_monospace_font = monospace;
  system_normal_font = normal;
  wayland_set_text_scale (factor);
  return Qnil;
}

DEFUN ("x-display-planes", Fx_display_planes, Sx_display_planes, 0, 1, 0,
       doc: /* SKIP: real doc in xfns.c.  */)
  (Lisp_Object terminal)
{
  /* Buffers are 32-bit XRGB.  */
  return make_fixnum (24);
}

DEFUN ("x-display-pixel-width", Fx_display_pixel_width,
       Sx_display_pixel_width, 0, 1, 0,
       doc: /* Return the width of TERMINAL in Wayland surface coordinates.  */)
  (Lisp_Object terminal)
{
  struct wayland_display_dimensions dimensions;
  wayland_display_dimensions (check_wayland_display_info (terminal),
			      &dimensions);
  return make_int (dimensions.width);
}

DEFUN ("x-display-pixel-height", Fx_display_pixel_height,
       Sx_display_pixel_height, 0, 1, 0,
       doc: /* Return the height of TERMINAL in Wayland surface coordinates.  */)
  (Lisp_Object terminal)
{
  struct wayland_display_dimensions dimensions;
  wayland_display_dimensions (check_wayland_display_info (terminal),
			      &dimensions);
  return make_int (dimensions.height);
}

DEFUN ("x-display-mm-width", Fx_display_mm_width, Sx_display_mm_width, 0, 1, 0,
       doc: /* Return TERMINAL's display width in millimeters, or nil.  */)
  (Lisp_Object terminal)
{
  struct wayland_display_dimensions dimensions;
  wayland_display_dimensions (check_wayland_display_info (terminal),
			      &dimensions);
  return dimensions.mm_width > 0 ? make_fixnum (dimensions.mm_width) : Qnil;
}

DEFUN ("x-display-mm-height", Fx_display_mm_height, Sx_display_mm_height, 0, 1, 0,
       doc: /* Return TERMINAL's display height in millimeters, or nil.  */)
  (Lisp_Object terminal)
{
  struct wayland_display_dimensions dimensions;
  wayland_display_dimensions (check_wayland_display_info (terminal),
			      &dimensions);
  return dimensions.mm_height > 0 ? make_fixnum (dimensions.mm_height) : Qnil;
}

DEFUN ("x-display-screens", Fx_display_screens, Sx_display_screens, 0, 1, 0,
       doc: /* Return the number of active outputs on TERMINAL.  */)
  (Lisp_Object terminal)
{
  struct wayland_display_dimensions dimensions;
  wayland_display_dimensions (check_wayland_display_info (terminal),
			      &dimensions);
  return make_fixnum (dimensions.screens);
}

DEFUN ("wayland-display-mouse-p", Fwayland_display_mouse_p,
       Swayland_display_mouse_p, 0, 1, 0,
       doc: /* Return non-nil if DISPLAY's seat has a pointer.  */)
  (Lisp_Object display)
{
  return wayland_display_has_pointer (check_wayland_display_info (display))
    ? Qt : Qnil;
}

DEFUN ("wayland-frame-geometry", Fwayland_frame_geometry,
       Swayland_frame_geometry, 1, 2, 0,
       doc: /* Return FRAME's geometry in surface-local coordinates.
When EDGES is non-nil, return the edges of that type instead.  */)
  (Lisp_Object frame, Lisp_Object edges)
{
  struct frame *f = decode_window_system_frame (frame);
  if (!NILP (edges) && !EQ (edges, Qouter_edges) && !EQ (edges, Qinner_edges))
    edges = Qnative_edges;
  return frame_geometry_without_wm
    (f, edges, FRAME_PARENT_FRAME (f) ? f->left_pos : 0,
     FRAME_PARENT_FRAME (f) ? f->top_pos : 0);
}

DEFUN ("wayland-frame-restack", Fwayland_frame_restack,
       Swayland_frame_restack, 2, 3, 0,
       doc: /* Place child FRAME below its sibling REFERENCE.
If ABOVE is non-nil, place FRAME above that sibling.
A parent REFERENCE places FRAME at the bottom of the sibling stack.  */)
  (Lisp_Object frame, Lisp_Object reference, Lisp_Object above)
{
  struct frame *f = decode_window_system_frame (frame);
  struct frame *r = decode_window_system_frame (reference);
  if (!wayland_restack_frame (f, r, !NILP (above)))
    error ("Can only restack a Wayland child relative to its parent or sibling");
  return Qt;
}

DEFUN ("x-hide-tip", Fx_hide_tip, Sx_hide_tip, 0, 0, 0,
       doc: /* SKIP: real doc in xfns.c.  */)
  (void)
{
  /* Tooltips arrive after v1, as child frames on subsurfaces.  */
  return Qnil;
}

/* Popup menus.  The backend has none of its own: `x-popup-menu' reads
   the choice in the minibuffer through `wayland--popup-menu'.

   Convert the parsed menu to completion labels and the values that a
   graphical menu would return.  Parsing already evaluated the menu
   items' filters, visibility and enable forms.  A label carries its
   item's keyboard equivalent, if any, as `wayland-menu-keys'.  */
Lisp_Object
wayland_menu_choices (int flags)
{
  AUTO_STRING (separator, " > ");
  bool pane_titles = !(flags & MENU_KEYMAPS) && menu_items_n_panes > 1;
  Lisp_Object labels = Qnil, prefix = Qnil, parent = Qnil, pane = Qnil;
  Lisp_Object name = Qnil, entry = Qnil, choices = Qnil;
  for (int i = 0; i < menu_items_used;)
    {
      Lisp_Object item = AREF (menu_items, i);
      if (NILP (item))
	{
	  labels = Fcons (list3 (prefix, parent, pane), labels);
	  prefix = entry;
	  parent = name;
	  pane = parent;
	  i++;
	}
      else if (EQ (item, Qlambda))
	{
	  Lisp_Object saved = XCAR (labels);
	  prefix = XCAR (saved);
	  parent = XCAR (XCDR (saved));
	  pane = XCAR (XCDR (XCDR (saved)));
	  labels = XCDR (labels);
	  i++;
	}
      else if (EQ (item, Qt))
	{
	  Lisp_Object title = AREF (menu_items, i + MENU_ITEMS_PANE_NAME);
	  prefix = AREF (menu_items, i + MENU_ITEMS_PANE_PREFIX);
	  if (STRINGP (title) && SCHARS (title) && SREF (title, 0) == '@')
	    title = Fsubstring (title, make_fixnum (1), Qnil);
	  pane = parent;
	  if (pane_titles && STRINGP (title) && SCHARS (title))
	    pane = NILP (parent) ? title : concat3 (parent, separator, title);
	  i += MENU_ITEMS_PANE_LENGTH;
	}
      else if (EQ (item, Qquote))
	i++;
      else
	{
	  name = NILP (pane) ? item : concat3 (pane, separator, item);
	  entry = AREF (menu_items, i + MENU_ITEMS_ITEM_VALUE);
	  Lisp_Object keys = AREF (menu_items, i + MENU_ITEMS_ITEM_EQUIV_KEY);
	  bool enabled = !NILP (AREF (menu_items, i + MENU_ITEMS_ITEM_ENABLE));
	  Lisp_Object def = AREF (menu_items, i + MENU_ITEMS_ITEM_DEFINITION);
	  i += MENU_ITEMS_ITEM_LENGTH;
	  if (!enabled || (NILP (def) && menu_separator_name_p (SSDATA (item)))
	      || (i < menu_items_used && NILP (AREF (menu_items, i))))
	    continue;
	  Lisp_Object value = entry;
	  if (flags & MENU_KEYMAPS)
	    {
	      value = list1 (entry);
	      if (!NILP (prefix))
		value = Fcons (prefix, value);
	      for (Lisp_Object tail = labels; CONSP (tail); tail = XCDR (tail))
		if (!NILP (XCAR (XCAR (tail))))
		  value = Fcons (XCAR (XCAR (tail)), value);
	    }
	  if (STRINGP (keys) && SCHARS (keys))
	    {
	      name = Fcopy_sequence (name);
	      Fput_text_property (make_fixnum (0), make_fixnum (SCHARS (name)),
				  Qwayland_menu_keys, keys, name);
	    }
	  choices = Fcons (Fcons (name, value), choices);
	}
    }
  return Fnreverse (choices);
}

void
syms_of_waylandfns (void)
{
  DEFSYM (Qwayland__popup_menu, "wayland--popup-menu");
  DEFSYM (Qwayland_menu_keys, "wayland-menu-keys");

  defsubr (&Sx_create_frame);
  defsubr (&Sx_open_connection);
  defsubr (&Sx_close_connection);
  defsubr (&Sx_display_list);
  defsubr (&Sxw_color_defined_p);
  defsubr (&Sxw_color_values);
  defsubr (&Sxw_display_color_p);
  defsubr (&Sx_display_grayscale_p);
  defsubr (&Sx_display_planes);
  defsubr (&Sx_display_pixel_width);
  defsubr (&Sx_display_pixel_height);
  defsubr (&Sx_display_mm_width);
  defsubr (&Sx_display_mm_height);
  defsubr (&Sx_display_screens);
  defsubr (&Swayland_display_mouse_p);
  defsubr (&Swayland_frame_geometry);
  defsubr (&Swayland_frame_restack);
  defsubr (&Sx_hide_tip);
  defsubr (&Sfont_get_system_font);
  defsubr (&Sfont_get_system_normal_font);
  defsubr (&Swayland__set_desktop_settings);

  staticpro (&system_monospace_font);
  system_monospace_font = Qnil;
  staticpro (&system_normal_font);
  system_normal_font = Qnil;

  DEFVAR_BOOL ("font-use-system-font", use_system_font,
    doc: /* SKIP: real doc in xsettings.c.  */);
  use_system_font = false;

  /* Loads dynamic-setting.el, whose `font-setting-change-default-font'
     wayland-win.el uses when the system font changes.  */
  DEFSYM (Qdynamic_setting, "dynamic-setting");
  Fprovide (Qdynamic_setting, Qnil);
}
