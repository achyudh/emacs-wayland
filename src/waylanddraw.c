/* Glyph, cursor and fringe drawing for the native Wayland backend.

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

/* The drawing half of the redisplay interface.  Most of these
   functions began as copies of their pgtk_ namesakes in pgtkterm.c;
   test/wayland/pgtk-drift.py lists the originals that have changed
   upstream since.  They draw through the frame's Cairo context, which
   waylandterm.c hands out with wayland_begin_cr_clip, and report what
   they change through its damage functions.  The fringe bitmaps are
   the only state kept here.  */

#include <config.h>

#include <math.h>

#include "lisp.h"
#include "blockinput.h"
#include "composite.h"
#include "frame.h"
#include "waylandterm.h"

static bool
frame_display_dead (struct frame *f)
{
  return FRAME_DISPLAY_INFO (f)->state == WAYLAND_DISPLAY_DEAD;
}

/* The glyph renderer follows the Cairo drawing in pgtkterm.c.  */

/* Select colors for the face or the block cursor.  */
static void
wayland_set_glyph_string_gc (struct glyph_string *s)
{
  prepare_face_for_display (s->f, s->face);
  s->xgcv.background = s->face->background;
  s->xgcv.foreground = s->face->foreground;
  s->stippled_p = s->face->stipple != 0;
  if (s->hl == DRAW_CURSOR)
    {
      s->stippled_p = false;
      s->xgcv.background = FRAME_CURSOR_COLOR (s->f);
      s->xgcv.foreground = s->face->background;
      if (s->xgcv.foreground == s->xgcv.background)
	s->xgcv.foreground = s->face->foreground;
      if (s->xgcv.foreground == s->xgcv.background)
	s->xgcv.foreground = FRAME_X_OUTPUT (s->f)->cursor_foreground_color;
      if (s->xgcv.foreground == s->xgcv.background)
	s->xgcv.foreground = s->xgcv.background ^ 0xffffff;
      if (s->xgcv.background == s->face->background
	  && s->xgcv.foreground == s->face->foreground)
	{
	  s->xgcv.background = s->face->foreground;
	  s->xgcv.foreground = s->face->background;
	}
    }
}

static void
wayland_clip_rectangles (cairo_t *cr, const XRectangle *rectangles, int n)
{
  if (n > 0)
    {
      for (int i = 0; i < n; i++)
	cairo_rectangle (cr, rectangles[i].x, rectangles[i].y,
			 rectangles[i].width, rectangles[i].height);
      cairo_clip (cr);
    }
}

static void
wayland_set_glyph_string_clipping (struct glyph_string *s, cairo_t *cr)
{
  XRectangle r[2];
  int n = get_glyph_string_clip_rects (s, r, countof (r));
  wayland_clip_rectangles (cr, r, n);
}

/* Set SRC's clipping for output of glyph string DST.  This is called
   when we are drawing DST's left_overhang or right_overhang only in
   the area of SRC.  */

static void
wayland_set_glyph_string_clipping_exactly (struct glyph_string *src,
					struct glyph_string *dst, cairo_t *cr)
{
  dst->clip[0].x = src->x;
  dst->clip[0].y = src->y;
  dst->clip[0].width = src->width;
  dst->clip[0].height = src->height;
  dst->num_clips = 1;

  cairo_rectangle (cr, src->x, src->y, src->width, src->height);
  cairo_clip (cr);
}

/* RIF:
   Compute left and right overhang of glyph string S.  */

void
wayland_compute_glyph_string_overhangs (struct glyph_string *s)
{
  if (s->cmp == NULL
      && ((s->first_glyph->type == CHAR_GLYPH && s->font)
	  || s->first_glyph->type == COMPOSITE_GLYPH))
    {
      struct font_metrics metrics;

      if (s->first_glyph->type == CHAR_GLYPH)
	{
	  unsigned *code = alloca (sizeof (unsigned) * s->nchars);
	  struct font *font = s->font;
	  int i;

	  for (i = 0; i < s->nchars; i++)
	    code[i] = s->char2b[i];
	  font->driver->text_extents (font, code, s->nchars, &metrics);
	}
      else
	{
	  Lisp_Object gstring = composition_gstring_from_id (s->cmp_id);

	  composition_gstring_width (gstring, s->cmp_from, s->cmp_to,
				     &metrics);
	}
      s->right_overhang = (metrics.rbearing > metrics.width
			   ? metrics.rbearing - metrics.width : 0);
      s->left_overhang = metrics.lbearing < 0 ? -metrics.lbearing : 0;
    }
  else if (s->cmp)
    {
      s->right_overhang = s->cmp->rbearing - s->cmp->pixel_width;
      s->left_overhang = -s->cmp->lbearing;
    }
}

/* Fill rectangle X, Y, W, H with background color of glyph string
   S.  */
static void
wayland_clear_glyph_string_rect (struct glyph_string *s, int x, int y,
			      int w, int h)
{
  wayland_fill_rect (s->f, s->xgcv.background, x, y, w, h);
}

static void
wayland_fill_face_background (struct frame *f, struct face *face, int x, int y,
			 int width, int height)
{
  cairo_t *cr = wayland_begin_cr_clip (f);

  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
  cairo_rectangle (cr, x, y, width, height);
  cairo_clip (cr);

  wayland_set_cr_source_with_color (f, face->background, false);
  wayland_cairo_damage (f, cr, x, y, x + width, y + height);
  cairo_paint (cr);

  if (face->stipple != 0)
    {
      cairo_pattern_t *mask
	= FRAME_DISPLAY_INFO (f)->bitmaps[face->stipple - 1].pattern;

      wayland_set_cr_source_with_color (f, face->foreground, false);
      cairo_mask (cr, mask);
    }

  wayland_end_cr_clip (f);
}

static void
wayland_fill_glyph_background (struct glyph_string *s, int x, int y,
			       int width, int height)
{
  wayland_fill_face_background (s->f, s->face, x, y, width, height);
}

/* Draw the background of glyph_string S.  If S->background_filled_p
   is non-zero don't draw it.  FORCE_P non-zero means draw the
   background even if it wouldn't be drawn normally.  This is used
   when a string preceding S draws into the background of S, or S
   contains the first component of a composition.  */
static void
wayland_draw_glyph_string_background (struct glyph_string *s, bool force_p)
{
  /* Nothing to do if background has already been drawn or if it
     shouldn't be drawn in the first place.  */
  if (!s->background_filled_p)
    {
      int box_line_width = max (s->face->box_horizontal_line_width, 0);

      if (s->stippled_p)
	{
	  /* Fill background with a stipple pattern.  */
	  wayland_fill_glyph_background (s, s->x, s->y + box_line_width,
			   s->background_width,
			   s->height - 2 * box_line_width);
	  s->background_filled_p = true;
	}
      else if (force_p || !s->font
	       || FONT_HEIGHT (s->font) < s->height - 2 * box_line_width
	       /* When xdisp.c ignores FONT_HEIGHT, we cannot trust
	          font dimensions, since the actual glyphs might be
	          much smaller.  So in that case we always clear the
	          rectangle with background color.  */
	       || FONT_TOO_HIGH (s->font)
	       || s->font_not_found_p
	       || s->extends_to_end_of_line_p)
	{
	  wayland_clear_glyph_string_rect (s, s->x, s->y + box_line_width,
					s->background_width,
					s->height - 2 * box_line_width);
	  s->background_filled_p = true;
	}
    }
}


static void
wayland_stroke_rect (struct frame *f, unsigned long color, int x, int y,
		     int width, int height, bool respect_alpha_background)
{
  cairo_t *cr;

  cr = wayland_begin_cr_clip (f);
  wayland_set_cr_source_with_color (f, color, respect_alpha_background);
  cairo_rectangle (cr, x + 0.5, y + 0.5, width, height);
  cairo_set_line_width (cr, 1);
  wayland_stroke_path (f, cr);
  wayland_end_cr_clip (f);
}

/* Draw the foreground of glyph string S.  */
static void
wayland_draw_glyph_string_foreground (struct glyph_string *s)
{
  int i, x;

  /* If first glyph of S has a left box line, start drawing the text
     of S to the right of that box line.  */
  if (s->face->box != FACE_NO_BOX && s->first_glyph->left_box_line_p)
    x = s->x + max (s->face->box_vertical_line_width, 0);
  else
    x = s->x;

  /* Draw characters of S as rectangles if S's font could not be
     loaded.  */
  if (s->font_not_found_p)
    {
      for (i = 0; i < s->nchars; ++i)
	{
	  struct glyph *g = s->first_glyph + i;
	  wayland_stroke_rect (s->f,
			       s->face->foreground, x, s->y,
			       g->pixel_width - 1, s->height - 1,
			       false);
	  x += g->pixel_width;
	}
    }
  else
    {
      struct font *font = s->font;
      int boff = font->baseline_offset;
      int y;

      if (font->vertical_centering)
	boff = VCENTER_BASELINE_OFFSET (font, s->f) - boff;

      y = s->ybase - boff;
      if (s->for_overlaps || (s->background_filled_p && s->hl != DRAW_CURSOR))
	font->driver->draw (s, 0, s->nchars, x, y, false);
      else
	font->driver->draw (s, 0, s->nchars, x, y, true);
      if (s->face->overstrike)
	font->driver->draw (s, 0, s->nchars, x + 1, y, false);
    }
}

/* Draw the foreground of composite glyph string S.  */
static void
wayland_draw_composite_glyph_string_foreground (struct glyph_string *s)
{
  int i, j, x;
  struct font *font = s->font;

  /* If first glyph of S has a left box line, start drawing the text
     of S to the right of that box line.  */
  if (s->face && s->face->box != FACE_NO_BOX
      && s->first_glyph->left_box_line_p)
    x = s->x + max (s->face->box_vertical_line_width, 0);
  else
    x = s->x;

  /* S is a glyph string for a composition.  S->cmp_from is the index
     of the first character drawn for glyphs of this composition.
     S->cmp_from == 0 means we are drawing the very first character of
     this composition.  */

  /* Draw a rectangle for the composition if the font for the very
     first character of the composition could not be loaded.  */
  if (s->font_not_found_p)
    {
      if (s->cmp_from == 0)
	wayland_stroke_rect (s->f, s->face->foreground, x, s->y,
			     s->width - 1, s->height - 1, false);
    }
  else if (!s->first_glyph->u.cmp.automatic)
    {
      int y = s->ybase;

      for (i = 0, j = s->cmp_from; i < s->nchars; i++, j++)
	/* TAB in a composition means display glyphs with padding
	   space on the left or right.  */
	if (COMPOSITION_GLYPH (s->cmp, j) != '\t')
	  {
	    int xx = x + s->cmp->offsets[j * 2];
	    int yy = y - s->cmp->offsets[j * 2 + 1];

	    font->driver->draw (s, j, j + 1, xx, yy, false);
	    if (s->face->overstrike)
	      font->driver->draw (s, j, j + 1, xx + 1, yy, false);
	  }
    }
  else
    {
      Lisp_Object gstring = composition_gstring_from_id (s->cmp_id);
      Lisp_Object glyph;
      int y = s->ybase;
      int width = 0;

      for (i = j = s->cmp_from; i < s->cmp_to; i++)
	{
	  glyph = LGSTRING_GLYPH (gstring, i);
	  if (NILP (LGLYPH_ADJUSTMENT (glyph)))
	    width += LGLYPH_WIDTH (glyph);
	  else
	    {
	      int xoff, yoff, wadjust;

	      if (j < i)
		{
		  font->driver->draw (s, j, i, x, y, false);
		  if (s->face->overstrike)
		    font->driver->draw (s, j, i, x + 1, y, false);
		  x += width;
		}
	      xoff = LGLYPH_XOFF (glyph);
	      yoff = LGLYPH_YOFF (glyph);
	      wadjust = LGLYPH_WADJUST (glyph);
	      font->driver->draw (s, i, i + 1, x + xoff, y + yoff, false);
	      if (s->face->overstrike)
		font->driver->draw (s, i, i + 1, x + xoff + 1, y + yoff,
				    false);
	      x += wadjust;
	      j = i + 1;
	      width = 0;
	    }
	}
      if (j < i)
	{
	  font->driver->draw (s, j, i, x, y, false);
	  if (s->face->overstrike)
	    font->driver->draw (s, j, i, x + 1, y, false);
	}
    }
}


/* Draw the foreground of glyph string S for glyphless characters.  */
static void
wayland_draw_glyphless_glyph_string_foreground (struct glyph_string *s)
{
  struct glyph *glyph = s->first_glyph;
  unsigned char2b[8];
  int x, i, j;

  /* If first glyph of S has a left box line, start drawing the text
     of S to the right of that box line.  */
  if (s->face && s->face->box != FACE_NO_BOX
      && s->first_glyph->left_box_line_p)
    x = s->x + max (s->face->box_vertical_line_width, 0);
  else
    x = s->x;

  s->char2b = char2b;

  for (i = 0; i < s->nchars; i++, glyph++)
    {
#ifdef GCC_LINT
      enum
      { PACIFY_GCC_BUG_81401 = 1 };
#else
      enum
      { PACIFY_GCC_BUG_81401 = 0 };
#endif
      char buf[7 + PACIFY_GCC_BUG_81401];
      char *str = NULL;
      int len = glyph->u.glyphless.len;

      if (glyph->u.glyphless.method == GLYPHLESS_DISPLAY_ACRONYM)
	{
	  if (len > 0
	      && CHAR_TABLE_P (Vglyphless_char_display)
	      &&
	      (CHAR_TABLE_EXTRA_SLOTS (XCHAR_TABLE (Vglyphless_char_display))
	       >= 1))
	    {
	      Lisp_Object acronym
		= (!glyph->u.glyphless.for_no_font
		   ? CHAR_TABLE_REF (Vglyphless_char_display,
				     glyph->u.glyphless.ch)
		   : XCHAR_TABLE (Vglyphless_char_display)->extras[0]);
	      if (CONSP (acronym))
		acronym = XCAR (acronym);
	      if (STRINGP (acronym))
		str = SSDATA (acronym);
	    }
	}
      else if (glyph->u.glyphless.method == GLYPHLESS_DISPLAY_HEX_CODE)
	{
	  unsigned int ch = glyph->u.glyphless.ch;
	  eassume (ch <= MAX_CHAR);
	  sprintf (buf, "%0*X", ch < 0x10000 ? 4 : 6, ch);
	  str = buf;
	}

      if (str)
	{
	  int upper_len = (len + 1) / 2;

	  /* It is assured that all LEN characters in STR is ASCII.  */
	  for (j = 0; j < len; j++)
	    char2b[j]
	      = s->font->driver->encode_char (s->font, str[j]) & 0xFFFF;
	  s->font->driver->draw (s, 0, upper_len,
				 x + glyph->slice.glyphless.upper_xoff,
				 s->ybase + glyph->slice.glyphless.upper_yoff,
				 false);
	  s->font->driver->draw (s, upper_len, len,
				 x + glyph->slice.glyphless.lower_xoff,
				 s->ybase + glyph->slice.glyphless.lower_yoff,
				 false);
	}
      if (glyph->u.glyphless.method != GLYPHLESS_DISPLAY_THIN_SPACE)
	wayland_stroke_rect (s->f, s->face->foreground,
			     x, s->ybase - glyph->ascent,
			     glyph->pixel_width - 1,
			     glyph->ascent + glyph->descent - 1,
			     false);
      x += glyph->pixel_width;
    }

  /* Pacify GCC 12 even though s->char2b is not used after this
     function returns.  */
  s->char2b = NULL;
}

/* Brightness beyond which a color won't have its highlight brightness
   boosted.

   Nominally, highlight colors for `3d' faces are calculated by
   brightening an object's color by a constant scale factor, but this
   doesn't yield good results for dark colors, so for colors who's
   brightness is less than this value (on a scale of 0-65535) have an
   use an additional additive factor.

   The value here is set so that the default menu-bar/mode-line color
   (grey75) will not have its highlights changed at all.  */
#define HIGHLIGHT_COLOR_DARK_BOOST_LIMIT 48000


/* Compute a color which is lighter or darker than *PIXEL by FACTOR or
   DELTA.  Try a color with RGB values multiplied by FACTOR first.  If
   this produces the same color as PIXEL, try a color where all RGB
   values have DELTA added.  Return the computed color in *PIXEL.  F
   is the frame to act on.  */

static void
wayland_compute_lighter_color (struct frame *f, unsigned long *pixel,
			    double factor, int delta)
{
  Emacs_Color color, new;
  long bright;

  /* Get RGB color values.  */
  color.pixel = *pixel;
  wayland_query_colors (f, &color, 1);

  /* Change RGB values by specified FACTOR.  Avoid overflow!  */
  eassert (factor >= 0);
  new.red = min (0xffff, factor * color.red);
  new.green = min (0xffff, factor * color.green);
  new.blue = min (0xffff, factor * color.blue);

  /* Calculate brightness of COLOR.  */
  bright = (2 * color.red + 3 * color.green + color.blue) / 6;

  /* We only boost colors that are darker than
     HIGHLIGHT_COLOR_DARK_BOOST_LIMIT.  */
  if (bright < HIGHLIGHT_COLOR_DARK_BOOST_LIMIT)
    /* Make an additive adjustment to NEW, because it's dark enough so
       that scaling by FACTOR alone isn't enough.  */
    {
      /* How far below the limit this color is (0 - 1, 1 being darker).  */
      double dimness = 1 - (double) bright / HIGHLIGHT_COLOR_DARK_BOOST_LIMIT;
      /* The additive adjustment.  */
      int min_delta = delta * dimness * factor / 2;

      if (factor < 1)
	{
	  new.red = max (0, new.red - min_delta);
	  new.green = max (0, new.green - min_delta);
	  new.blue = max (0, new.blue - min_delta);
	}
      else
	{
	  new.red = min (0xffff, min_delta + new.red);
	  new.green = min (0xffff, min_delta + new.green);
	  new.blue = min (0xffff, min_delta + new.blue);
	}
    }

  new.pixel = (new.red >> 8 << 16
	       | new.green >> 8 << 8
	       | new.blue >> 8);

  if (new.pixel == *pixel)
    {
      /* If we end up with the same color as before, try adding
	 delta to the RGB values.  */
      new.red = min (0xffff, delta + color.red);
      new.green = min (0xffff, delta + color.green);
      new.blue = min (0xffff, delta + color.blue);
      new.pixel = (new.red >> 8 << 16
		   | new.green >> 8 << 8
		   | new.blue >> 8);
    }

  *pixel = new.pixel;
}

static void
wayland_fill_trapezoid_for_relief (struct frame *f, unsigned long color, int x,
				int y, int width, int height, int top_p)
{
  cairo_t *cr;

  cr = wayland_begin_cr_clip (f);
  wayland_set_cr_source_with_color (f, color, false);
  cairo_move_to (cr, top_p ? x : x + height, y);
  cairo_line_to (cr, x, y + height);
  cairo_line_to (cr, top_p ? x + width - height : x + width, y + height);
  cairo_line_to (cr, x + width, y);
  wayland_damage_fill (f, cr);
  cairo_fill (cr);
  wayland_end_cr_clip (f);
}

enum corners
{
  CORNER_BOTTOM_RIGHT,		/* 0 -> pi/2 */
  CORNER_BOTTOM_LEFT,		/* pi/2 -> pi */
  CORNER_TOP_LEFT,		/* pi -> 3pi/2 */
  CORNER_TOP_RIGHT,		/* 3pi/2 -> 2pi */
  CORNER_LAST
};

static void
wayland_erase_corners_for_relief (struct frame *f, unsigned long color, int x,
			       int y, int width, int height, double radius,
			       double margin, int corners)
{
  cairo_t *cr;
  int i;

  cr = wayland_begin_cr_clip (f);
  wayland_set_cr_source_with_color (f, color, false);
  for (i = 0; i < CORNER_LAST; i++)
    if (corners & (1 << i))
      {
	double xm, ym, xc, yc;

	if (i == CORNER_TOP_LEFT || i == CORNER_BOTTOM_LEFT)
	  xm = x - margin, xc = xm + radius;
	else
	  xm = x + width + margin, xc = xm - radius;
	if (i == CORNER_TOP_LEFT || i == CORNER_TOP_RIGHT)
	  ym = y - margin, yc = ym + radius;
	else
	  ym = y + height + margin, yc = ym - radius;

	cairo_move_to (cr, xm, ym);
	cairo_arc (cr, xc, yc, radius, i * M_PI_2, (i + 1) * M_PI_2);
      }
  cairo_clip (cr);
  cairo_rectangle (cr, x, y, width, height);
  wayland_damage_fill (f, cr);
  cairo_fill (cr);
  wayland_end_cr_clip (f);
}

static void
wayland_setup_relief_color (struct frame *f, struct relief *relief,
			    double factor, int delta)
{
  unsigned long pixel = FRAME_X_OUTPUT (f)->relief_background;
  wayland_compute_lighter_color (f, &pixel, factor, delta);
  relief->xgcv.foreground = relief->pixel = pixel;
}

/* Set up colors for the relief lines around glyph string S.  */
static void
wayland_setup_relief_colors (struct glyph_string *s)
{
  struct wayland_output *di = FRAME_X_OUTPUT (s->f);
  unsigned long color;

  if (s->face->use_box_color_for_shadows_p)
    color = s->face->box_color;
  else if (s->first_glyph->type == IMAGE_GLYPH
	   && s->img->pixmap
	   && !IMAGE_BACKGROUND_TRANSPARENT (s->img, s->f, 0))
    color = IMAGE_BACKGROUND (s->img, s->f, 0);
  else
    {
      /* Get the background color of the face.  */
      color = s->xgcv.background;
    }

  if (!di->relief_background_valid_p
      || di->relief_background != color)
    {
      di->relief_background_valid_p = true;
      di->relief_background = color;
      wayland_setup_relief_color (s->f, &di->white_relief, 1.2, 0x8000);
      wayland_setup_relief_color (s->f, &di->black_relief, 0.6, 0x4000);
    }
}

/* Draw a relief on frame F inside the rectangle given by LEFT_X,
   TOP_Y, RIGHT_X, and BOTTOM_Y.  WIDTH is the thickness of the relief
   to draw, it must be >= 0.  RAISED_P means draw a raised
   relief.  LEFT_P means draw a relief on the left side of
   the rectangle.  RIGHT_P means draw a relief on the right
   side of the rectangle.  CLIP_RECT is the clipping rectangle to use
   when drawing.  */

static void
wayland_draw_relief_rect (struct frame *f,
		       int left_x, int top_y, int right_x, int bottom_y,
		       int hwidth, int vwidth, bool raised_p, bool top_p,
		       bool bot_p, bool left_p, bool right_p,
		       XRectangle *clip_rect)
{
  unsigned long top_left_color, bottom_right_color;
  int corners = 0;

  cairo_t *cr = wayland_begin_cr_clip (f);

  if (raised_p)
    {
      top_left_color = FRAME_X_OUTPUT (f)->white_relief.xgcv.foreground;
      bottom_right_color = FRAME_X_OUTPUT (f)->black_relief.xgcv.foreground;
    }
  else
    {
      top_left_color = FRAME_X_OUTPUT (f)->black_relief.xgcv.foreground;
      bottom_right_color = FRAME_X_OUTPUT (f)->white_relief.xgcv.foreground;
    }

  wayland_clip_rectangles (cr, clip_rect, 1);

  if (left_p)
    {
      wayland_fill_rect (f, top_left_color, left_x, top_y,
			   vwidth, bottom_y + 1 - top_y);
      if (top_p)
	corners |= 1 << CORNER_TOP_LEFT;
      if (bot_p)
	corners |= 1 << CORNER_BOTTOM_LEFT;
    }
  if (right_p)
    {
      wayland_fill_rect (f, bottom_right_color, right_x + 1 - vwidth, top_y,
			   vwidth, bottom_y + 1 - top_y);
      if (top_p)
	corners |= 1 << CORNER_TOP_RIGHT;
      if (bot_p)
	corners |= 1 << CORNER_BOTTOM_RIGHT;
    }
  if (top_p)
    {
      if (!right_p)
	wayland_fill_rect (f, top_left_color, left_x, top_y,
			     right_x + 1 - left_x, hwidth);
      else
	wayland_fill_trapezoid_for_relief (f, top_left_color, left_x, top_y,
					right_x + 1 - left_x, hwidth, 1);
    }
  if (bot_p)
    {
      if (!left_p)
	wayland_fill_rect (f, bottom_right_color, left_x,
			     bottom_y + 1 - hwidth, right_x + 1 - left_x,
			     hwidth);
      else
	wayland_fill_trapezoid_for_relief (f, bottom_right_color,
					left_x, bottom_y + 1 - hwidth,
					right_x + 1 - left_x, hwidth, 0);
    }
  if (left_p && vwidth > 1)
    wayland_fill_rect (f, bottom_right_color, left_x, top_y,
			 1, bottom_y + 1 - top_y);
  if (top_p && hwidth > 1)
    wayland_fill_rect (f, bottom_right_color, left_x, top_y,
			 right_x + 1 - left_x, 1);
  if (corners)
    wayland_erase_corners_for_relief (f, FRAME_BACKGROUND_COLOR (f), left_x,
				   top_y, right_x - left_x + 1,
				   bottom_y - top_y + 1, 6, 1, corners);

  wayland_end_cr_clip (f);
}

/* Draw a box on frame F inside the rectangle given by LEFT_X, TOP_Y,
   RIGHT_X, and BOTTOM_Y.  WIDTH is the thickness of the lines to
   draw, it must be >= 0.  LEFT_P means draw a line on the
   left side of the rectangle.  RIGHT_P means draw a line
   on the right side of the rectangle.  CLIP_RECT is the clipping
   rectangle to use when drawing.  */

static void
wayland_draw_box_rect (struct glyph_string *s, int left_x,
		    int top_y, int right_x, int bottom_y, int hwidth,
		    int vwidth, bool left_p, bool right_p,
		    XRectangle * clip_rect)
{
  unsigned long color = s->face->box_color;
  cairo_t *cr = wayland_begin_cr_clip (s->f);
  wayland_clip_rectangles (cr, clip_rect, 1);

  /* Top.  */
  wayland_fill_rect (s->f, color, left_x, top_y,
		     right_x - left_x + 1, hwidth);

  /* Left.  */
  if (left_p)
    wayland_fill_rect (s->f, color, left_x, top_y, vwidth,
		       bottom_y - top_y + 1);

  /* Bottom.  */
  wayland_fill_rect (s->f, color, left_x, bottom_y - hwidth + 1,
		     right_x - left_x + 1, hwidth);

  /* Right.  */
  if (right_p)
    wayland_fill_rect (s->f, color, right_x - vwidth + 1, top_y,
		       vwidth, bottom_y - top_y + 1);

  wayland_end_cr_clip (s->f);
}


/* Draw a box around glyph string S.  */

static void
wayland_draw_glyph_string_box (struct glyph_string *s)
{
  int hwidth, vwidth, left_x, right_x, top_y, bottom_y, last_x;
  bool raised_p, left_p, right_p;
  struct glyph *last_glyph;
  XRectangle clip_rect;

  last_x = ((s->row->full_width_p && !s->w->pseudo_window_p)
	    ? WINDOW_RIGHT_EDGE_X (s->w) : window_box_right (s->w, s->area));

  /* The glyph that may have a right box line.  */
  last_glyph = (s->cmp || s->img
		? s->first_glyph : s->first_glyph + s->nchars - 1);

  vwidth = eabs (s->face->box_vertical_line_width);
  hwidth = eabs (s->face->box_horizontal_line_width);
  raised_p = s->face->box == FACE_RAISED_BOX;
  left_x = s->x;
  right_x = (s->row->full_width_p && s->extends_to_end_of_line_p
	     ? last_x - 1 : min (last_x, s->x + s->background_width) - 1);
  top_y = s->y;
  bottom_y = top_y + s->height - 1;

  left_p = (s->first_glyph->left_box_line_p
	    || (s->hl == DRAW_MOUSE_FACE
		&& (s->prev == NULL || s->prev->hl != s->hl)));
  right_p = (last_glyph->right_box_line_p
	     || (s->hl == DRAW_MOUSE_FACE
		 && (s->next == NULL || s->next->hl != s->hl)));

  get_glyph_string_clip_rect (s, &clip_rect);

  if (s->face->box == FACE_SIMPLE_BOX)
    wayland_draw_box_rect (s, left_x, top_y, right_x, bottom_y, hwidth,
			vwidth, left_p, right_p, &clip_rect);
  else
    {
      wayland_setup_relief_colors (s);
      wayland_draw_relief_rect (s->f, left_x, top_y, right_x, bottom_y, hwidth,
			     vwidth, raised_p, true, true, left_p, right_p,
			     &clip_rect);
    }
}

static void
wayland_draw_horizontal_wave (struct frame *f, unsigned long color,
			      int x, int y, int width, int height,
			      int wave_length)
{
  cairo_t *cr;
  double dx = wave_length, dy = height - 1;
  int xoffset, n;

  cr = wayland_begin_cr_clip (f);
  wayland_set_cr_source_with_color (f, color, false);
  cairo_rectangle (cr, x, y, width, height);
  cairo_clip (cr);

  if (x >= 0)
    {
      xoffset = x % (wave_length * 2);
      if (xoffset == 0)
	xoffset = wave_length * 2;
    }
  else
    xoffset = x % (wave_length * 2) + wave_length * 2;
  n = (width + xoffset) / wave_length + 1;
  if (xoffset > wave_length)
    {
      xoffset -= wave_length;
      --n;
      y += height - 1;
      dy = -dy;
    }

  cairo_move_to (cr, x - xoffset + 0.5, y + 0.5);
  while (--n >= 0)
    {
      cairo_rel_line_to (cr, dx, dy);
      dy = -dy;
    }
  cairo_set_line_width (cr, 1);
  wayland_stroke_path (f, cr);
  wayland_end_cr_clip (f);
}

static void
wayland_draw_underwave (struct glyph_string *s, unsigned long color,
			int thickness)
{
  int wave_height = 3 * thickness, wave_length = 2 * thickness;

  wayland_draw_horizontal_wave (s->f, color, s->x, s->ybase - wave_height + 3,
			     s->width, wave_height, wave_length);
}

/* Draw a relief around the image glyph string S.  */

static void
wayland_draw_image_relief (struct glyph_string *s)
{
  int x1, y1, thick;
  bool raised_p, top_p, bot_p, left_p, right_p;
  int extra_x, extra_y;
  XRectangle r;
  int x = s->x;
  int y = s->ybase - image_ascent (s->img, s->face, &s->slice);

  /* If first glyph of S has a left box line, start drawing it to the
     right of that line.  */
  if (s->face->box != FACE_NO_BOX
      && s->first_glyph->left_box_line_p
      && s->slice.x == 0)
    x += max (s->face->box_vertical_line_width, 0);

  /* If there is a margin around the image, adjust x- and y-position
     by that margin.  */
  if (s->slice.x == 0)
    x += s->img->hmargin;
  if (s->slice.y == 0)
    y += s->img->vmargin;

  if (s->hl == DRAW_IMAGE_SUNKEN
      || s->hl == DRAW_IMAGE_RAISED)
    {
      if (s->face->id == TAB_BAR_FACE_ID)
	thick = (tab_bar_button_relief < 0
		 ? DEFAULT_TAB_BAR_BUTTON_RELIEF
		 : min (tab_bar_button_relief, 1000000));
      else
	thick = (tool_bar_button_relief < 0
		 ? DEFAULT_TOOL_BAR_BUTTON_RELIEF
		 : min (tool_bar_button_relief, 1000000));
      raised_p = s->hl == DRAW_IMAGE_RAISED;
    }
  else
    {
      thick = eabs (s->img->relief);
      raised_p = s->img->relief > 0;
    }

  x1 = x + s->slice.width - 1;
  y1 = y + s->slice.height - 1;

  extra_x = extra_y = 0;
  if (s->face->id == TAB_BAR_FACE_ID)
    {
      if (CONSP (Vtab_bar_button_margin)
	  && FIXNUMP (XCAR (Vtab_bar_button_margin))
	  && FIXNUMP (XCDR (Vtab_bar_button_margin)))
	{
	  extra_x = XFIXNUM (XCAR (Vtab_bar_button_margin)) - thick;
	  extra_y = XFIXNUM (XCDR (Vtab_bar_button_margin)) - thick;
	}
      else if (FIXNUMP (Vtab_bar_button_margin))
	extra_x = extra_y = XFIXNUM (Vtab_bar_button_margin) - thick;
    }

  if (s->face->id == TOOL_BAR_FACE_ID)
    {
      if (CONSP (Vtool_bar_button_margin)
	  && FIXNUMP (XCAR (Vtool_bar_button_margin))
	  && FIXNUMP (XCDR (Vtool_bar_button_margin)))
	{
	  extra_x = XFIXNUM (XCAR (Vtool_bar_button_margin));
	  extra_y = XFIXNUM (XCDR (Vtool_bar_button_margin));
	}
      else if (FIXNUMP (Vtool_bar_button_margin))
	extra_x = extra_y = XFIXNUM (Vtool_bar_button_margin);
    }

  top_p = bot_p = left_p = right_p = false;

  if (s->slice.x == 0)
    x -= thick + extra_x, left_p = true;
  if (s->slice.y == 0)
    y -= thick + extra_y, top_p = true;
  if (s->slice.x + s->slice.width == s->img->width)
    x1 += thick + extra_x, right_p = true;
  if (s->slice.y + s->slice.height == s->img->height)
    y1 += thick + extra_y, bot_p = true;

  wayland_setup_relief_colors (s);
  get_glyph_string_clip_rect (s, &r);
  wayland_draw_relief_rect (s->f, x, y, x1, y1, thick, thick, raised_p,
			 top_p, bot_p, left_p, right_p, &r);
}

/* Draw part of the background of glyph string S.  X, Y, W, and H
   give the rectangle to draw.  */

static void
wayland_draw_glyph_string_bg_rect (struct glyph_string *s, int x, int y, int w,
				int h)
{
  if (s->stippled_p)
    wayland_fill_glyph_background (s, x, y, w, h);
  else
    wayland_clear_glyph_string_rect (s, x, y, w, h);
}

static void
wayland_cr_draw_image (struct frame *f, Emacs_GC *gc, cairo_pattern_t *image,
		    int src_x, int src_y, int width, int height,
		    int dest_x, int dest_y, bool overlay_p)
{
  cairo_t *cr = wayland_begin_cr_clip (f);

  wayland_cairo_damage (f, cr, dest_x, dest_y, dest_x + width,
                        dest_y + height);
  if (overlay_p)
    cairo_rectangle (cr, dest_x, dest_y, width, height);
  else
    {
      wayland_set_cr_source_with_color (f, gc->background, false);
      cairo_rectangle (cr, dest_x, dest_y, width, height);
      cairo_fill_preserve (cr);
    }

  cairo_translate (cr, dest_x - src_x, dest_y - src_y);

  cairo_surface_t *surface;
  cairo_pattern_get_surface (image, &surface);
  cairo_format_t format = cairo_image_surface_get_format (surface);
  if (format != CAIRO_FORMAT_A8 && format != CAIRO_FORMAT_A1)
    {
      cairo_set_source (cr, image);
      cairo_fill (cr);
    }
  else
    {
      wayland_set_cr_source_with_color (f, gc->foreground, false);
      cairo_clip (cr);
      cairo_mask (cr, image);
    }

  wayland_end_cr_clip (f);
}

/* Draw foreground of image glyph string S.  */

static void
wayland_draw_image_foreground (struct glyph_string *s)
{
  int x = s->x;
  int y = s->ybase - image_ascent (s->img, s->face, &s->slice);

  /* If first glyph of S has a left box line, start drawing it to the
     right of that line.  */
  if (s->face->box != FACE_NO_BOX
      && s->first_glyph->left_box_line_p
      && s->slice.x == 0)
    x += max (s->face->box_vertical_line_width, 0);

  /* If there is a margin around the image, adjust x- and y-position
     by that margin.  */
  if (s->slice.x == 0)
    x += s->img->hmargin;
  if (s->slice.y == 0)
    y += s->img->vmargin;

  if (s->img->cr_data)
    {
      cairo_t *cr = wayland_begin_cr_clip (s->f);
      wayland_set_glyph_string_clipping (s, cr);
      wayland_cr_draw_image (s->f, &s->xgcv, s->img->cr_data,
			  s->slice.x, s->slice.y,
			  s->slice.width, s->slice.height,
			  x, y, true);
      if (!s->img->mask)
	{
	  /* When the image has a mask, we can expect that at
	     least part of a mouse highlight or a block cursor will
	     be visible.  If the image doesn't have a mask, make
	     a block cursor visible by drawing a rectangle around
	     the image.  I believe it's looking better if we do
	     nothing here for mouse-face.  */
	  if (s->hl == DRAW_CURSOR)
	    {
	      int relief = eabs (s->img->relief);
	      wayland_stroke_rect (s->f, s->xgcv.foreground, x - relief,
				   y - relief, s->slice.width + relief * 2 - 1,
				   s->slice.height + relief * 2 - 1, false);
	    }
	}
      wayland_end_cr_clip (s->f);
    }
  else
    /* Draw a rectangle if image could not be loaded.  */
    wayland_stroke_rect (s->f, s->xgcv.foreground, x, y,
			 s->slice.width - 1, s->slice.height - 1, false);
}

/* Draw image glyph string S.

            s->y
   s->x      +-------------------------
	     |   s->face->box
	     |
	     |     +-------------------------
	     |     |  s->img->margin
	     |     |
	     |     |       +-------------------
	     |     |       |  the image

 */

static void
wayland_draw_image_glyph_string (struct glyph_string *s)
{
  int box_line_hwidth = max (s->face->box_vertical_line_width, 0);
  int box_line_vwidth = max (s->face->box_horizontal_line_width, 0);
  int height;

  height = s->height;
  if (s->slice.y == 0)
    height -= box_line_vwidth;
  if (s->slice.y + s->slice.height >= s->img->height)
    height -= box_line_vwidth;

  /* Fill background with face under the image.  Do it only if row is
     taller than image or if image has a clip mask to reduce
     flickering.  */
  s->stippled_p = s->face->stipple != 0;
  if (height > s->slice.height
      || s->img->hmargin
      || s->img->vmargin
      || s->img->mask
      || s->img->pixmap == 0
      || s->width != s->background_width)
    {
      int x = s->x;
      int y = s->y;
      int width = s->background_width;

      if (s->first_glyph->left_box_line_p
	  && s->slice.x == 0)
	{
	  x += box_line_hwidth;
	  width -= box_line_hwidth;
	}

      if (s->slice.y == 0)
	y += box_line_vwidth;

      wayland_draw_glyph_string_bg_rect (s, x, y, width, height);

      s->background_filled_p = true;
    }

  /* Draw the foreground.  */
  wayland_draw_image_foreground (s);

  /* If we must draw a relief around the image, do it.  */
  if (s->img->relief
      || s->hl == DRAW_IMAGE_RAISED
      || s->hl == DRAW_IMAGE_SUNKEN)
    wayland_draw_image_relief (s);
}

/* Draw stretch glyph string S.  */

static void
wayland_draw_stretch_glyph_string (struct glyph_string *s)
{
  eassert (s->first_glyph->type == STRETCH_GLYPH);

  if (s->hl == DRAW_CURSOR && !x_stretch_cursor_p)
    {
      /* If `x-stretch-cursor' is nil, don't draw a block cursor as
         wide as the stretch glyph.  */
      int width, background_width = s->background_width;
      int x = s->x;

      if (!s->row->reversed_p)
	{
	  int left_x = window_box_left_offset (s->w, TEXT_AREA);

	  if (x < left_x)
	    {
	      background_width -= left_x - x;
	      x = left_x;
	    }
	}
      else
	{
	  /* In R2L rows, draw the cursor on the right edge of the
	     stretch glyph.  */
	  int right_x = window_box_right (s->w, TEXT_AREA);

	  if (x + background_width > right_x)
	    background_width -= x - right_x;
	  x += background_width;
	}
      width = min (FRAME_COLUMN_WIDTH (s->f), background_width);
      if (s->row->reversed_p)
	x -= width;

      /* Draw cursor.  */
      wayland_draw_glyph_string_bg_rect (s, x, s->y, width, s->height);

      /* Clear rest using the GC of the original non-cursor face.  */
      if (width < background_width)
	{
	  int y = s->y;
	  int w = background_width - width, h = s->height;
	  XRectangle r;
	  unsigned long color;

	  if (!s->row->reversed_p)
	    x += width;
	  else
	    x = s->x;
	  if (s->row->mouse_face_p && cursor_in_mouse_face_p (s->w))
	    color = s->face->foreground;
	  else
	    color = s->face->background;

	  cairo_t *cr = wayland_begin_cr_clip (s->f);

	  get_glyph_string_clip_rect (s, &r);
	  wayland_clip_rectangles (cr, &r, 1);

	  if (s->face->stipple)
	    wayland_fill_glyph_background (s, x, y, w, h);
	  else
	    wayland_fill_rect (s->f, color, x, y, w, h);

	  wayland_end_cr_clip (s->f);
	}
    }
  else if (!s->background_filled_p)
    {
      int background_width = s->background_width;
      int x = s->x, text_left_x = window_box_left (s->w, TEXT_AREA);

      /* Don't draw into left fringe or scrollbar area except for
	 header line and mode line.  */
      if (s->area == TEXT_AREA
	  && x < text_left_x && !s->row->mode_line_p)
	{
	  background_width -= text_left_x - x;
	  x = text_left_x;
	}

      if (background_width > 0)
	wayland_draw_glyph_string_bg_rect (s, x, s->y,
					  background_width, s->height);
    }

  s->background_filled_p = true;
}


/* Draw a dashed underline of thickness THICKNESS and width WIDTH onto F
   at a vertical offset of OFFSET from the position of the glyph string
   S, with each segment SEGMENT pixels in length.  */

static void
wayland_draw_dash (struct frame *f, struct glyph_string *s,
		unsigned long foreground, int width,
		char segment, int offset, int thickness)
{
  cairo_t *cr;
  double cr_segment, y_center;

  cr = wayland_begin_cr_clip (s->f);
  wayland_set_cr_source_with_color (f, foreground, false);
  cr_segment = (double) segment;
  y_center = s->ybase + offset + (thickness / 2.0);

  cairo_set_dash (cr, &cr_segment, 1, s->x);
  cairo_set_line_width (cr, thickness);
  cairo_move_to (cr, s->x, y_center);
  cairo_line_to (cr, s->x + width, y_center);
  wayland_stroke_path (f, cr);
  wayland_end_cr_clip (f);
}

/* Draw an underline of STYLE onto F at an offset of POSITION from the
   baseline of the glyph string S in the color provided by FOREGROUND,
   DECORATION_WIDTH in length, and THICKNESS in height.  */

static void
wayland_fill_underline (struct frame *f, struct glyph_string *s,
		     unsigned long foreground,
		     enum face_underline_type style, int position,
		     int decoration_width, int thickness)
{
  int segment;

  segment = thickness * 3;

  switch (style)
    {
      /* FACE_UNDERLINE_DOUBLE_LINE is treated identically to SINGLE, as
	 the second line will be filled by another invocation of this
	 function.  */
    case FACE_UNDERLINE_SINGLE:
    case FACE_UNDERLINE_DOUBLE_LINE:
      wayland_fill_rect (f, foreground, s->x, s->ybase + position,
			   decoration_width, thickness);
      break;

    case FACE_UNDERLINE_DOTS:
      segment = thickness;
      FALLTHROUGH;

    case FACE_UNDERLINE_DASHES:
      wayland_draw_dash (f, s, foreground, decoration_width, segment,
		      position, thickness);
      break;

    case FACE_NO_UNDERLINE:
    case FACE_UNDERLINE_WAVE:
    default:
      emacs_abort ();
    }
}

void
wayland_draw_glyph_string (struct glyph_string *s)
{
  if (frame_display_dead (s->f))
    return;
  bool relief_drawn_p = false;

  /* Draw the backgrounds of successor strings before drawing into
     them.  Their foreground draws can then preserve the overhang.  */
  if (s->next && s->right_overhang && !s->for_overlaps)
    {
      int width;
      struct glyph_string *next;

      for (width = 0, next = s->next;
	   next && width < s->right_overhang;
	   width += next->width, next = next->next)
	if (next->first_glyph->type != IMAGE_GLYPH)
	  {
	    cairo_t *cr = wayland_begin_cr_clip (next->f);
	    wayland_set_glyph_string_gc (next);
	    wayland_set_glyph_string_clipping (next, cr);
	    if (next->first_glyph->type == STRETCH_GLYPH)
	      wayland_draw_stretch_glyph_string (next);
	    else
	      wayland_draw_glyph_string_background (next, true);
	    next->num_clips = 0;
	    wayland_end_cr_clip (next->f);
	  }
    }

  /* Set up S->gc, set clipping and draw S.  */
  wayland_set_glyph_string_gc (s);

  cairo_t *cr = wayland_begin_cr_clip (s->f);
  wayland_damage_glyph_string (s);

  /* Draw relief (if any) in advance for char/composition so that the
     glyph string can be drawn over it.  */
  if (!s->for_overlaps
      && s->face->box != FACE_NO_BOX
      && (s->first_glyph->type == CHAR_GLYPH
	  || s->first_glyph->type == COMPOSITE_GLYPH))

    {
      wayland_set_glyph_string_clipping (s, cr);
      wayland_draw_glyph_string_background (s, true);
      wayland_draw_glyph_string_box (s);
      wayland_set_glyph_string_clipping (s, cr);
      relief_drawn_p = true;
    }
  else if (!s->clip_head	/* draw_glyphs didn't specify a clip mask. */
	   && !s->clip_tail
	   && ((s->prev && s->prev->hl != s->hl && s->left_overhang)
	       || (s->next && s->next->hl != s->hl && s->right_overhang)))
    /* We must clip just this glyph.  left_overhang part has already
       drawn when s->prev was drawn, and right_overhang part will be
       drawn later when s->next is drawn. */
    wayland_set_glyph_string_clipping_exactly (s, s, cr);
  else
    wayland_set_glyph_string_clipping (s, cr);

  switch (s->first_glyph->type)
    {
    case IMAGE_GLYPH:
      wayland_draw_image_glyph_string (s);
      break;

    case XWIDGET_GLYPH:
      wayland_draw_glyph_string_background (s, true);
      break;

    case STRETCH_GLYPH:
      wayland_draw_stretch_glyph_string (s);
      break;

    case CHAR_GLYPH:
      if (s->for_overlaps)
	s->background_filled_p = true;
      else
	wayland_draw_glyph_string_background (s, false);
      wayland_draw_glyph_string_foreground (s);
      break;

    case COMPOSITE_GLYPH:
      if (s->for_overlaps || (s->cmp_from > 0
			      && !s->first_glyph->u.cmp.automatic))
	s->background_filled_p = true;
      else
	wayland_draw_glyph_string_background (s, true);
      wayland_draw_composite_glyph_string_foreground (s);
      break;

    case GLYPHLESS_GLYPH:
      if (s->for_overlaps)
	s->background_filled_p = true;
      else
	wayland_draw_glyph_string_background (s, true);
      wayland_draw_glyphless_glyph_string_foreground (s);
      break;

    default:
      emacs_abort ();
    }

  if (!s->for_overlaps)
    {
      struct font *font = font_for_underline_metrics (s);
      unsigned long scaled_thickness;

      /* Get the underline thickness.  Default is 1 pixel.  */
      if (font && font->underline_thickness > 0)
	scaled_thickness = font->underline_thickness;
      else
	scaled_thickness = 1;

      /* Draw relief if not yet drawn.  */
      if (!relief_drawn_p && s->face->box != FACE_NO_BOX)
	wayland_draw_glyph_string_box (s);

      /* Draw underline.  */
      if (s->face->underline)
	{
	  unsigned long thickness = (underline_line_scaling_flag
				     ? scaled_thickness
				     : wayland_fixed_scale (s->f));
	  if (s->face->underline == FACE_UNDERLINE_WAVE)
	    {
	      if (s->face->underline_defaulted_p)
		wayland_draw_underwave (s, s->xgcv.foreground, thickness);
	      else
		wayland_draw_underwave (s, s->face->underline_color,
					thickness);
	    }
	  else if (s->face->underline >= FACE_UNDERLINE_SINGLE)
	    {
	      unsigned long position;
	      unsigned long foreground;

	      if (s->prev
		  && (s->prev->face->underline != FACE_UNDERLINE_WAVE
		      && s->prev->face->underline >= FACE_UNDERLINE_SINGLE)
		  && (s->prev->face->underline_at_descent_line_p
		      == s->face->underline_at_descent_line_p)
		  && (s->prev->face->underline_pixels_above_descent_line
		      == s->face->underline_pixels_above_descent_line))
		{
		  /* We use the same underline style as the previous one.  */
		  thickness = s->prev->underline_thickness;
		  position = s->prev->underline_position;
		}
	      else
		{
		  if ((x_underline_at_descent_line
		       || s->face->underline_at_descent_line_p))
		    {
		      int above_descent
			= s->face->underline_pixels_above_descent_line;
		      position = (s->height - thickness
				  - (s->ybase - s->y) - above_descent);
		    }
		  else
		    {
		      /* The underline position is the signed offset in
			 pixels from the baseline to the top of the line.
			 Its default is

		         ROUND ((maximum descent) / 2), with
		         ROUND(x) = floor (x + 0.5)  */

		      if (x_use_underline_position_properties
			  && font && font->underline_position >= 0)
			position = font->underline_position;
		      else if (font)
			position = (font->descent + 1) / 2;
		      else
			position = underline_minimum_offset;
		    }

		  /* Ignore minimum_offset if the amount of pixels was
		     explicitly specified.  */
		  if (!s->face->underline_pixels_above_descent_line)
		    position = max (position, underline_minimum_offset);
		}
	      /* Check the sanity of thickness and position.  We should
	         avoid drawing underline out of the current line area.  */
	      if (s->y + s->height <= s->ybase + position)
		position = (s->height - 1) - (s->ybase - s->y);
	      if (s->y + s->height < s->ybase + position + thickness)
		thickness = (s->y + s->height) - (s->ybase + position);
	      s->underline_thickness = thickness;
	      s->underline_position = position;

	      if (s->face->underline_defaulted_p)
		foreground = s->xgcv.foreground;
	      else
		foreground = s->face->underline_color;

	      wayland_fill_underline (s->f, s, foreground, s->face->underline,
				   position, s->width, thickness);

	      /* Place a second underline above the first if this was
		 requested in the face specification.  */

	      if (s->face->underline == FACE_UNDERLINE_DOUBLE_LINE)
		{
		  /* Compute the position of the second underline.  */
		  position = position - thickness - 1;
		  wayland_fill_underline (s->f, s, foreground,
					   s->face->underline, position,
					   s->width, thickness);
		}
	    }
	}
      /* Draw overline.  */
      if (s->face->overline_p)
	{
	  unsigned long h = (overline_line_scaling_flag
			     ? scaled_thickness : 1);

	  if (s->face->overline_color_defaulted_p)
	    wayland_fill_rect (s->f, s->xgcv.foreground, s->x, s->y,
			       s->width, h);
	  else
	    wayland_fill_rect (s->f, s->face->overline_color, s->x,
			       s->y, s->width, h);
	}

      /* Draw strike-through.  */
      if (s->face->strike_through_p)
	{
	  /* Y-coordinate and height of the glyph string's first
	     glyph.  We cannot use s->y and s->height because those
	     could be larger if there are taller display elements
	     (e.g., characters displayed with a larger font) in the
	     same glyph row.  */
	  int glyph_y = s->ybase - s->first_glyph->ascent;
	  int glyph_height = s->first_glyph->ascent + s->first_glyph->descent;
	  /* Strike-through width and offset from the glyph string's
	     top edge.  */
	  unsigned long h = (strike_through_line_scaling_flag
			     ? scaled_thickness : 1);
	  unsigned long dy = (glyph_height - h) / 2;

	  if (s->face->strike_through_color_defaulted_p)
	    wayland_fill_rect (s->f, s->xgcv.foreground, s->x, glyph_y + dy,
				 s->width, h);
	  else
	    wayland_fill_rect (s->f, s->face->strike_through_color, s->x,
				 glyph_y + dy, s->width, h);
	}

      if (s->prev)
	{
	  struct glyph_string *prev;

	  for (prev = s->prev; prev; prev = prev->prev)
	    if (prev->hl != s->hl
		&& prev->x + prev->width + prev->right_overhang > s->x)
	      {
		/* As prev was drawn while clipped to its own area, we
		   must draw the right_overhang part using s->hl now.  */
		enum draw_glyphs_face save = prev->hl;

		prev->hl = s->hl;
		wayland_set_glyph_string_gc (prev);
		cairo_save (cr);
		wayland_set_glyph_string_clipping_exactly (s, prev, cr);
		if (prev->first_glyph->type == CHAR_GLYPH)
		  wayland_draw_glyph_string_foreground (prev);
		else
		  wayland_draw_composite_glyph_string_foreground (prev);
		prev->hl = save;
		prev->num_clips = 0;
		cairo_restore (cr);
	      }
	}

      if (s->next)
	{
	  struct glyph_string *next;

	  for (next = s->next; next; next = next->next)
	    if (next->hl != s->hl
		&& next->x - next->left_overhang < s->x + s->width)
	      {
		/* As next will be drawn while clipped to its own area,
		   we must draw the left_overhang part using s->hl now.  */
		enum draw_glyphs_face save = next->hl;

		next->hl = s->hl;
		wayland_set_glyph_string_gc (next);
		cairo_save (cr);
		wayland_set_glyph_string_clipping_exactly (s, next, cr);
		if (next->first_glyph->type == CHAR_GLYPH)
		  wayland_draw_glyph_string_foreground (next);
		else
		  wayland_draw_composite_glyph_string_foreground (next);
		cairo_restore (cr);
		next->hl = save;
		next->num_clips = 0;
		next->clip_head = s->next;
	      }
	}
    }

  if (!s->row->stipple_p)
    s->row->stipple_p = s->face->stipple;

  /* Reset clipping.  */
  wayland_end_cr_clip (s->f);
  s->num_clips = 0;
}


/* Cairo masks for the bitmaps owned by fringe.c.  */
static cairo_pattern_t **wayland_fringe_masks;
static ptrdiff_t wayland_fringe_mask_count;

void
wayland_destroy_fringe_bitmap (int which)
{
  if (which < wayland_fringe_mask_count && wayland_fringe_masks[which])
    {
      cairo_pattern_destroy (wayland_fringe_masks[which]);
      wayland_fringe_masks[which] = NULL;
    }
}

void
wayland_define_fringe_bitmap (int which, unsigned short *bits, int h, int wd)
{
  if (which >= wayland_fringe_mask_count)
    {
      ptrdiff_t old_count = wayland_fringe_mask_count;
      wayland_fringe_masks = xpalloc (wayland_fringe_masks,
				      &wayland_fringe_mask_count,
				      which - old_count + 1, -1,
				      sizeof *wayland_fringe_masks);
      for (ptrdiff_t i = old_count; i < wayland_fringe_mask_count; i++)
	wayland_fringe_masks[i] = NULL;
    }
  wayland_destroy_fringe_bitmap (which);
  if (h <= 0)
    return;

  cairo_surface_t *surface = cairo_image_surface_create (CAIRO_FORMAT_A1,
							       wd, h);
  if (cairo_surface_status (surface) == CAIRO_STATUS_SUCCESS)
    {
      unsigned char *data = cairo_image_surface_get_data (surface);
      int stride = cairo_image_surface_get_stride (surface);
      for (int i = 0; i < h; i++, data += stride)
	{
	  /* fringe.c puts the pixels in the first 16 bits of an A1 row.  */
	  uint32_t row = bits[i];
#ifdef WORDS_BIGENDIAN
	  row <<= 16;
#endif
	  memcpy (data, &row, sizeof row);
	}
      cairo_surface_mark_dirty (surface);
      cairo_pattern_t *mask = cairo_pattern_create_for_surface (surface);
      if (cairo_pattern_status (mask) == CAIRO_STATUS_SUCCESS)
	{
	  cairo_pattern_set_filter (mask, CAIRO_FILTER_NEAREST);
	  wayland_fringe_masks[which] = mask;
	}
      else
	cairo_pattern_destroy (mask);
    }
  cairo_surface_destroy (surface);
}

static void
wayland_clip_to_row (struct window *w, struct glyph_row *row,
		     enum glyph_row_area area, cairo_t *cr)
{
  int x, y, width;
  window_box (w, area, &x, &y, &width, NULL);
  y = max (y, WINDOW_TO_FRAME_PIXEL_Y (w, max (0, row->y)));
  cairo_rectangle (cr, x, y, width, row->visible_height);
  cairo_clip (cr);
}

void
wayland_draw_fringe_bitmap (struct window *w, struct glyph_row *row,
			    struct draw_fringe_bitmap_params *p)
{
  struct frame *f = XFRAME (WINDOW_FRAME (w));
  if (frame_display_dead (f))
    return;

  int which = p->which;
  cairo_pattern_t *mask = NULL;
  if (which && which < max_used_fringe_bitmap)
    {
      /* A bitmap defined on a tty can lack its Cairo mask.  */
      if (which >= wayland_fringe_mask_count || !wayland_fringe_masks[which])
	gui_define_fringe_bitmap (f, which);
      if (which < wayland_fringe_mask_count)
	mask = wayland_fringe_masks[which];
    }

  cairo_t *cr = wayland_begin_cr_clip (f);
  wayland_clip_to_row (w, row, ANY_AREA, cr);
  struct face *face = p->face;
  if (p->bx >= 0 && !p->overlay_p)
    wayland_fill_face_background (f, face, p->bx, p->by, p->nx, p->ny);

  if (mask)
    {
      unsigned long color = (p->cursor_p
			     ? (p->overlay_p ? face->background
				: FRAME_CURSOR_COLOR (f))
			     : face->foreground);
      if (!p->overlay_p)
	wayland_fill_rect (f, face->background, p->x, p->y, p->wd, p->h);
      wayland_cairo_damage (f, cr, p->x, p->y, p->x + p->wd, p->y + p->h);
      cairo_rectangle (cr, p->x, p->y, p->wd, p->h);
      cairo_clip (cr);
      cairo_translate (cr, p->x, p->y - p->dh);
      int scale = wayland_fixed_scale (f);
      cairo_scale (cr, scale, scale);
      wayland_set_cr_source_with_color (f, color, false);
      cairo_mask (cr, mask);
    }
  wayland_end_cr_clip (f);
}

static void
wayland_draw_hollow_cursor (struct window *w, struct glyph_row *row)
{
  struct glyph *glyph = get_phys_cursor_glyph (w);
  if (!glyph)
    return;
  struct frame *f = XFRAME (WINDOW_FRAME (w));
  int x, y, height;
  get_phys_cursor_geometry (w, row, glyph, &x, &y, &height);
  int width = w->phys_cursor_width - 1;
  if ((glyph->resolved_level & 1) != 0 && glyph->pixel_width > width)
    {
      x += glyph->pixel_width - width;
      if (width > 0)
	width--;
    }

  cairo_t *cr = wayland_begin_cr_clip (f);
  wayland_clip_to_row (w, row, TEXT_AREA, cr);
  wayland_stroke_rect (f, FRAME_CURSOR_COLOR (f), x, y, width, height - 1,
		       false);
  wayland_end_cr_clip (f);
}

static void
wayland_draw_bar_cursor (struct window *w, struct glyph_row *row, int width,
			 enum text_cursor_kinds kind)
{
  struct glyph *glyph = get_phys_cursor_glyph (w);
  if (!glyph || glyph->type == XWIDGET_GLYPH)
    return;
  if (glyph->type == IMAGE_GLYPH)
    {
      draw_phys_cursor_glyph (w, row, DRAW_CURSOR);
      return;
    }

  struct frame *f = XFRAME (WINDOW_FRAME (w));
  struct face *face = FACE_FROM_ID (f, glyph->face_id);
  unsigned long color = FRAME_CURSOR_COLOR (f);
  if (face->background == color)
    color = face->foreground;
  if (width >= 0)
    width = min ((int64_t) width * wayland_fixed_scale (f), INT_MAX);

  int x = WINDOW_TEXT_TO_FRAME_PIXEL_X (w, w->phys_cursor.x);
  int y = WINDOW_TO_FRAME_PIXEL_Y (w, w->phys_cursor.y);
  int draw_width, draw_height;
  if (kind == BAR_CURSOR)
    {
      draw_width = min (glyph->pixel_width,
			width < 0 ? FRAME_CURSOR_WIDTH (f) : width);
      draw_height = row->height;
      w->phys_cursor_width = draw_width;
    }
  else
    {
      int cursor_x, cursor_y, cursor_height;
      get_phys_cursor_geometry (w, row, glyph, &cursor_x, &cursor_y,
				&cursor_height);
      draw_width = w->phys_cursor_width - 1;
      draw_height = width < 0 ? row->height : min (row->height, width);
      y += row->height - draw_height;
    }
  if ((glyph->resolved_level & 1) != 0 && glyph->pixel_width > draw_width)
    x += glyph->pixel_width - draw_width;

  cairo_t *cr = wayland_begin_cr_clip (f);
  wayland_clip_to_row (w, row, TEXT_AREA, cr);
  wayland_fill_rect (f, color, x, y, draw_width, draw_height);
  wayland_end_cr_clip (f);
}

void
wayland_draw_window_cursor (struct window *w, struct glyph_row *glyph_row,
			    int x, int y, enum text_cursor_kinds cursor_type,
			    int cursor_width, bool on_p, bool active_p)
{
  struct frame *f = XFRAME (WINDOW_FRAME (w));

  if (!on_p || frame_display_dead (f))
    return;
  w->phys_cursor_type = cursor_type;
  w->phys_cursor_on_p = true;

  if (glyph_row->exact_window_width_line_p
      && (glyph_row->reversed_p
	  ? w->phys_cursor.hpos < 0
	  : w->phys_cursor.hpos >= glyph_row->used[TEXT_AREA]))
    {
      glyph_row->cursor_in_fringe_p = true;
      draw_fringe_bitmap (w, glyph_row, glyph_row->reversed_p);
    }
  else
    switch (cursor_type)
      {
      case NO_CURSOR:
	w->phys_cursor_width = 0;
	break;
      case FILLED_BOX_CURSOR:
	draw_phys_cursor_glyph (w, glyph_row, DRAW_CURSOR);
	break;
      case HOLLOW_BOX_CURSOR:
      case DEFAULT_CURSOR:
	wayland_draw_hollow_cursor (w, glyph_row);
	break;
      case BAR_CURSOR:
      case HBAR_CURSOR:
	wayland_draw_bar_cursor (w, glyph_row, cursor_width, cursor_type);
	break;
      }

  /* The input method places its candidate window here, as in
     pgtkterm.c.  */
  if (w == XWINDOW (f->selected_window))
    wayland_im_set_cursor_rectangle
      (f, (WINDOW_TO_FRAME_PIXEL_X (w, x) + WINDOW_LEFT_FRINGE_WIDTH (w)
	   + WINDOW_LEFT_MARGIN_WIDTH (w)),
       WINDOW_TO_FRAME_PIXEL_Y (w, y), w->phys_cursor_width,
       w->phys_cursor_height);
}

void
wayland_draw_vertical_window_border (struct window *w, int x, int y0, int y1)
{
  struct frame *f = XFRAME (WINDOW_FRAME (w));
  struct face *face = FACE_FROM_ID_OR_NULL (f, VERTICAL_BORDER_FACE_ID);
  unsigned long color = face ? face->foreground : FRAME_FOREGROUND_COLOR (f);

  wayland_fill_rect (f, color, x, y0, 1, y1 - y0);
}

void
wayland_draw_window_divider (struct window *w, int x0, int x1, int y0,
			     int y1)
{
  struct frame *f = XFRAME (WINDOW_FRAME (w));
  struct face *face = FACE_FROM_ID_OR_NULL (f, WINDOW_DIVIDER_FACE_ID);
  unsigned long color = face ? face->foreground : FRAME_FOREGROUND_COLOR (f);

  wayland_fill_rect (f, color, x0, y0, x1 - x0, y1 - y0);
}
