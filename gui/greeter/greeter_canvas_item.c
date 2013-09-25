/* MDM - The MDM Display Manager
 * Copyright (C) 1998, 1999, 2000 Martin K. Petersen <mkp@mkp.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "config.h"

#include <math.h>
#include <string.h>
#include <gtk/gtk.h>
#include <glib/gi18n.h>
#include <librsvg/rsvg.h>

#include "mdm.h"
#include "mdmcommon.h"
#include "mdmconfig.h"

#include "mdm-common.h"
#include "mdm-daemon-config-keys.h"

#include "greeter.h"
#include "greeter_item.h"
#include "greeter_events.h"
#include "greeter_system.h"
#include "greeter_canvas_item.h"
#include "greeter_configuration.h"
#include "greeter_canvas_text.h"
#include "greeter_parser.h"

/* Keep track of buttons so they can be set sensitive/insensitive */
GtkButton *gtk_ok_button = NULL;
GtkButton *gtk_start_again_button = NULL;

static void
apply_tint (GdkPixbuf *pixbuf, guint32 tint_color)
{
  guchar *pixels;
  guint r, g, b;
  gboolean has_alpha;
  guint w, h, stride;
  guint pixel_stride;
  guchar *line;
  int i;
  
  pixels = gdk_pixbuf_get_pixels (pixbuf);
  has_alpha = gdk_pixbuf_get_has_alpha (pixbuf);
  
  r = (tint_color & 0xff0000) >> 16;
  g = (tint_color & 0x00ff00) >> 8;
  b = (tint_color & 0x0000ff);

  w = gdk_pixbuf_get_width (pixbuf);
  h = gdk_pixbuf_get_height (pixbuf);
  stride = gdk_pixbuf_get_rowstride (pixbuf);

  pixel_stride = (has_alpha) ? 4 : 3;

  while (h-->0)
    {
      line = pixels;

      for (i = 0; i < w; i++)
	{
	  line[0] = line[0] * r / 0xff;
	  line[1] = line[1] * g / 0xff;
	  line[2] = line[2] * b / 0xff;
	  line += pixel_stride;
	}

      pixels += stride;
    }
}

static GdkPixbuf *
transform_pixbuf (GdkPixbuf *orig,
		  gboolean has_tint, guint32 tint_color,
		  double alpha, gint width, gint height)
{
  GdkPixbuf *scaled;
  gint p_width, p_height;

  p_width = gdk_pixbuf_get_width (orig);
  p_height = gdk_pixbuf_get_height (orig);
  
  if (p_width != width ||
      p_height != height ||
      alpha < 1.0 ||
      has_tint)
    {
      int alpha_i;
      
      if (alpha >= 1.0)
	alpha_i = 0xff;
      else if (alpha <= 0.0)
	alpha_i = 0;
      else
	alpha_i = (guint) floor (0xff*alpha);
      if (alpha != 0xff)
	{
	  scaled = gdk_pixbuf_new (GDK_COLORSPACE_RGB, TRUE, 8, width, height);
	  gdk_pixbuf_fill (scaled, 0);
	  gdk_pixbuf_composite (orig, scaled, 0, 0, width, height,
				0, 0, (double)width/p_width, (double)height/p_height,
				GDK_INTERP_BILINEAR, alpha_i);
	}
      else
	scaled = gdk_pixbuf_scale_simple (orig, width, height, GDK_INTERP_BILINEAR);
    }
  else
    scaled = g_object_ref (orig);
  
  if (has_tint)
    apply_tint (scaled, tint_color);

  return scaled;
}

static void
activate_button (GtkWidget *widget, gpointer data)
{
	const char *id = data;
	if (id != NULL)
		greeter_item_run_action_callback (id);
}

static void 
menubar_done (GtkMenuShell *menushell, gpointer data)
{
	GreeterItemInfo *entry_info = greeter_lookup_id ("user-pw-entry");
	GtkWidget *entry = GNOME_CANVAS_WIDGET (entry_info->item)->widget;

	gtk_widget_grab_focus (entry);
}

static GtkWidget *
make_menubar (void)
{
	GtkWidget *w, *menu;
	GtkWidget *menubar = gtk_menu_bar_new ();

	/* FIXME: add translatable string here */
	w = gtk_menu_item_new_with_label ("Menu");
	gtk_menu_shell_append (GTK_MENU_SHELL (menubar), w);
	gtk_widget_show (GTK_WIDGET (w));

	menu = gtk_menu_new ();
	gtk_menu_item_set_submenu (GTK_MENU_ITEM (w), menu);

	w = gtk_image_menu_item_new_with_mnemonic (_("Select _Language..."));
	gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (w), 
		gtk_image_new_from_icon_name ("preferences-desktop-locale", GTK_ICON_SIZE_MENU));

	gtk_menu_shell_append (GTK_MENU_SHELL (menu), w);
	gtk_widget_show (GTK_WIDGET (w));
	g_signal_connect (G_OBJECT (w), "activate",
			  G_CALLBACK (activate_button),
			  "language_button");

	w = gtk_image_menu_item_new_with_mnemonic (_("Select _Session..."));
	gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (w), 
		gtk_image_new_from_icon_name ("user-desktop", GTK_ICON_SIZE_MENU));

	gtk_menu_shell_append (GTK_MENU_SHELL (menu), w);
	gtk_widget_show (GTK_WIDGET (w));
	g_signal_connect (G_OBJECT (w), "activate",
			  G_CALLBACK (activate_button),
			  "session_button");

	greeter_system_append_system_menu (menu);

	/* Add a quit/disconnect item when in xdmcp mode or flexi mode */
	/* Do note that the order is important, we always want "Quit" for
	 * flexi, even if not local.  and Disconnect
	 * only for xdmcp */
	if ( ! ve_string_empty (g_getenv ("MDM_FLEXI_SERVER"))) {
		w = gtk_image_menu_item_new_with_mnemonic (_("_Quit"));
		gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (w), 
			gtk_image_new_from_icon_name ("system-log-out", GTK_ICON_SIZE_MENU));
	} else if (ve_string_empty (g_getenv ("MDM_IS_LOCAL"))) {
		w = gtk_image_menu_item_new_with_mnemonic (_("D_isconnect"));
		gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (w), 
			gtk_image_new_from_icon_name ("system-log-out", GTK_ICON_SIZE_MENU));
	} else {
		w = NULL;
	}
	if (w != NULL) {
		GtkWidget *sep;
		/* add separator before the quit */
		sep = gtk_separator_menu_item_new ();
		gtk_menu_shell_append (GTK_MENU_SHELL (menu), sep);
		gtk_widget_show (GTK_WIDGET (sep));

		gtk_menu_shell_append (GTK_MENU_SHELL (menu), w);
		gtk_widget_show (GTK_WIDGET (w));
		g_signal_connect (G_OBJECT (w), "activate",
				  G_CALLBACK (gtk_main_quit), NULL);
	}

	g_signal_connect (G_OBJECT(gtk_menu_item_get_submenu(
                        gtk_container_get_children(GTK_CONTAINER(menubar))->data)), "selection-done", G_CALLBACK (menubar_done), NULL);

	return menubar;
}

static void
get_gdk_color_from_rgb (GdkColor *c, guint32 rgb)
{
	c->red = ((rgb & 0xff0000) >> 16) * 0x101;
	c->green = ((rgb & 0xff00) >> 8) * 0x101;
	c->blue = (rgb & 0xff) * 0x101;
	c->pixel = 0;
}

static void
menu_position_func (GtkMenu           *menu,
                    int               *x,
                    int               *y,
                    gboolean          *push_in,
                    GreeterItemInfo *item)
{
	GtkAllocation rect;
	GtkRequisition  requisition;

	rect = item->allocation; 
	gtk_widget_size_request (GTK_WIDGET (menu), &requisition);
	*x = rect.x;
	if (requisition.height <= rect.y)
		*y = rect.y - requisition.height;
	else
		*y = rect.y + rect.height;

	*push_in = TRUE;
}

/*
 * The button with this handler never appears in the F10 menu, so
 * it can make use of callback data.
 */
static void
greeter_options_handler (GreeterItemInfo *item, GtkWidget *menubar)
{
	gtk_menu_popup (GTK_MENU(gtk_menu_item_get_submenu(
		        gtk_container_get_children(GTK_CONTAINER(menubar))->data)), 
			NULL, NULL, (GtkMenuPositionFunc)menu_position_func, 
			item, 0, gtk_get_current_event_time());
}

static void
greeter_item_run_button_action_callback (GtkButton *button, const char *id)
{
	GreeterItemInfo *entry_info = greeter_lookup_id ("user-pw-entry");
	GtkWidget *entry = GNOME_CANVAS_WIDGET (entry_info->item)->widget;

        greeter_item_run_action_callback (id);
	gtk_widget_grab_focus (entry);
}

void
greeter_item_create_canvas_item (GreeterItemInfo *item)
{
  GnomeCanvasGroup *group;
  GtkJustification just;
  GtkWidget *entry;
  GtkWidget *gtkbutton;
  GtkWidget *list;
  GtkWidget *swin;
  double x1, y1, x2, y2;
  int i;
  GtkAllocation rect;
  char *text;
  GtkTooltips *tooltips;
  char *num_locale;
  GdkColor c;

  if (item->item != NULL)
    return;

  if ( ! greeter_item_is_visible (item))
    return;

  g_assert (item->parent->group_item);
  
  if (item->fixed_children != NULL ||
      item->box_children != NULL)
    {
      item->group_item =
	(GnomeCanvasGroup *)gnome_canvas_item_new (item->parent->group_item,
						   GNOME_TYPE_CANVAS_GROUP,
						   "x", (gdouble) 0.0,
						   "y", (gdouble) 0.0,
						   NULL);
      group = item->group_item;
    }
  else
    group = item->parent->group_item;

  rect = item->allocation;

  
  x1 = (gdouble) rect.x;
  y1 = (gdouble) rect.y;
  x2 = (gdouble) rect.x + rect.width;
  y2 = (gdouble) rect.y + rect.height;

  switch (item->item_type) {
  case GREETER_ITEM_TYPE_RECT:
    item->item = gnome_canvas_item_new (group,
					GNOME_TYPE_CANVAS_RECT,
					"x1", x1,
					"y1", y1,
					"x2", x2,
					"y2", y2,
					"fill_color_rgba", item->data.rect.colors[GREETER_ITEM_STATE_NORMAL],
					NULL);
    break;
  case GREETER_ITEM_TYPE_SVG:
    num_locale = g_strdup (setlocale (LC_NUMERIC, NULL));
    setlocale (LC_NUMERIC, "C");
    for (i = 0; i < GREETER_ITEM_STATE_MAX; i++)
      {
	if (item->data.pixmap.files[i] != NULL)
	  {
	    if (i > 0 &&
		item->data.pixmap.files[0] != NULL &&
		item->data.pixmap.pixbufs[0] != NULL &&
		strcmp (item->data.pixmap.files[0], item->data.pixmap.files[i]) == 0)
	      item->data.pixmap.pixbufs[i] = g_object_ref (item->data.pixmap.pixbufs[0]);
	    else
	      item->data.pixmap.pixbufs[i] =
                  gdk_pixbuf_new_from_file_at_size (item->data.pixmap.files[i], rect.width, rect.height, NULL);
	  }
	else
	  item->data.pixmap.pixbufs[i] = NULL;
      }
    setlocale (LC_NUMERIC, num_locale);
    g_free (num_locale);
    num_locale = NULL;

    /* Fall through */
  case GREETER_ITEM_TYPE_PIXMAP:
    for (i = 0; i < GREETER_ITEM_STATE_MAX; i++)
      {
	GdkPixbuf *pb = item->data.pixmap.pixbufs[i];
	if (pb != NULL)
	  {
	    item->data.pixmap.pixbufs[i] =
	      transform_pixbuf (pb,
				(item->data.pixmap.have_tint & (1<<i)), item->data.pixmap.tints[i],
				(double)item->data.pixmap.alphas[i] / 256.0, rect.width, rect.height);
	    g_object_unref (pb);
	  }
      }
    
    if (item->data.pixmap.pixbufs[GREETER_ITEM_STATE_NORMAL] != NULL)
      item->item = gnome_canvas_item_new (group,
					  GNOME_TYPE_CANVAS_PIXBUF,
					  "x", (gdouble) x1,
					  "y", (gdouble) y1,
					  "pixbuf", item->data.pixmap.pixbufs[GREETER_ITEM_STATE_NORMAL],
					  NULL);
    break;
  case GREETER_ITEM_TYPE_LABEL:
    text = mdm_common_expand_text (item->data.text.orig_text);

    /* Justification is taken from the anchor */
    if (item->anchor == GTK_ANCHOR_NORTH_WEST ||
	item->anchor == GTK_ANCHOR_SOUTH_WEST ||
	item->anchor == GTK_ANCHOR_WEST)
	    just = GTK_JUSTIFY_LEFT;
    else if (item->anchor == GTK_ANCHOR_NORTH_EAST ||
	     item->anchor == GTK_ANCHOR_SOUTH_EAST ||
	     item->anchor == GTK_ANCHOR_EAST)
	    just = GTK_JUSTIFY_RIGHT;
    else
	    just = GTK_JUSTIFY_CENTER;

    item->item = gnome_canvas_item_new (group,
					GREETER_TYPE_CANVAS_TEXT,
					"text", "",
					"x", x1,
					"y", y1,
					"anchor", item->anchor,
					"font_desc", item->data.text.fonts[GREETER_ITEM_STATE_NORMAL],
					"fill_color_rgba", item->data.text.colors[GREETER_ITEM_STATE_NORMAL],
					"justification", just,
					NULL);

    greeter_canvas_item_break_set_string (item,
					  text,
					  TRUE /* markup */,
					  item->data.text.real_max_width,
					  NULL /* width */,
					  NULL /* height */,
					  NULL /* canvas */,
					  item->item);
    g_free (text);

    /* if there is an accelerator we do an INCREDIBLE hack */
    if (strchr (item->data.text.orig_text, '_') != NULL)
      {
	GreeterItemInfo *button;
	GtkWidget *fake_button = gtk_button_new_with_mnemonic (item->data.text.orig_text);
	gtk_widget_show (fake_button);
	GTK_WIDGET_UNSET_FLAGS (fake_button, GTK_CAN_FOCUS);
	gnome_canvas_item_new (gnome_canvas_root (GNOME_CANVAS (canvas)),
			       GNOME_TYPE_CANVAS_WIDGET,
			       "widget", fake_button,
			       "x", (double)-999.0,
			       "y", (double)-999.0,
			       "height", (double)20.0,
			       "width", (double)20.0,
			       NULL);
	button = item->my_button;
	if (button == NULL)
	  button = item;
	g_signal_connect_data (G_OBJECT (fake_button), "clicked",
			       G_CALLBACK (activate_button),
			       g_strdup (button->id),
			       (GClosureNotify)g_free,
			       0 /* connect_flags */);
      }
    
    break;
    
  case GREETER_ITEM_TYPE_BUTTON:
    gtkbutton = gtk_button_new_with_mnemonic (item->data.text.orig_text);
    gtk_widget_set_name (gtkbutton, item->id);
    if (strcmp (ve_sure_string (item->id), "ok_button") == 0) {
       gtk_ok_button = GTK_BUTTON (gtkbutton);
    } else if (strcmp (ve_sure_string (item->id), "cancel_button") == 0) {
       gtk_start_again_button = GTK_BUTTON (gtkbutton);
    }

    g_signal_connect (G_OBJECT (gtkbutton), "clicked",
                      G_CALLBACK (greeter_item_run_button_action_callback),
                      item->id);

    item->item = gnome_canvas_item_new (group,
					GNOME_TYPE_CANVAS_WIDGET,
					"widget", gtkbutton,
					"x", x1,
					"y", y1,
					"height", (double)rect.height,
					"width", (double)rect.width,
					NULL);

    break;

  case GREETER_ITEM_TYPE_ENTRY:
    entry = gtk_entry_new ();
    gtk_widget_set_name (entry, "user-pw-entry");
    gtk_entry_set_has_frame (GTK_ENTRY (entry), FALSE);
    if (item->background != NULL && item->background == TRUE) {
    	gtk_entry_set_has_frame (GTK_ENTRY (entry), TRUE);
    }

    if (mdm_config_get_bool (MDM_KEY_ENTRY_INVISIBLE))
      gtk_entry_set_invisible_char (GTK_ENTRY (entry), 0);
    else if (mdm_config_get_bool (MDM_KEY_ENTRY_CIRCLES))
      gtk_entry_set_invisible_char (GTK_ENTRY (entry), 0x25cf);

    gtk_widget_modify_font (entry, item->data.text.fonts[GREETER_ITEM_STATE_NORMAL]);

    get_gdk_color_from_rgb (&c, item->data.text.colors[GREETER_ITEM_STATE_NORMAL]);
    gtk_widget_modify_text (entry, GTK_STATE_NORMAL, &c);
    
    if (item->id != NULL && strcmp (item->id, "user-pw-entry") == 0) {
	    /* HACK! Add a menubar, this is kind of evil isn't it,
	     * should probably be done in the pam item setup thingie.
	     * but this is really widget kind of thing.  I dunno where
	     * this belongs but it's a hack here. */
	    item->data.text.menubar = make_menubar ();

	    gnome_canvas_item_new (gnome_canvas_root (GNOME_CANVAS (canvas)),
				   GNOME_TYPE_CANVAS_WIDGET,
				   "widget", item->data.text.menubar,
				   "x", (double)x1,
				   "y", (double)y1,
				   "height", (double)rect.height,
				   "width", (double)rect.width,
				   NULL);

	    greeter_item_register_action_callback ("options_button",
						   (ActionFunc)greeter_options_handler,
						   item->data.text.menubar);
	    
	    /* Here add a tooltip, so that the user knows about F10 */
	    tooltips = gtk_tooltips_new ();
	    gtk_tooltips_set_tip (tooltips, GTK_WIDGET (entry),
				  _("Answer questions here and press Enter "
				    "when done.  For a menu press F10."),
				  NULL);

	    /* FIXME: how to make this accessible??? */
    }

    item->item = gnome_canvas_item_new (group,
					GNOME_TYPE_CANVAS_WIDGET,
					"widget", entry,
					"x", x1,
					"y", y1,
					"height", (double)rect.height,
					"width", (double)rect.width,
					NULL);

    /* cursor blinking is evil on remote displays, don't do it forever */
    mdm_common_setup_blinking_entry (entry);

    break;

  case GREETER_ITEM_TYPE_LIST:
    /* Note a list type must be setup later and we will add the list store
     * to it then, depending on the type.  Likely userlist is the
     * only type we support */

    if (item->data.list.combo_type) {
       list = gtk_combo_box_new_text ();

       item->item = gnome_canvas_item_new (group,
					   GNOME_TYPE_CANVAS_WIDGET,
					   "widget", list,
					   "x", x1,
					   "y", y1,
					   "height", (double)rect.height,
					   "width", (double)rect.width,
					   NULL);
    } else {
       list = gtk_tree_view_new ();

       swin = gtk_scrolled_window_new (NULL, NULL);
       gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (swin),
    					GTK_SHADOW_NONE);
       gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (swin),
    				GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
       gtk_container_add (GTK_CONTAINER (swin), list);

       item->item = gnome_canvas_item_new (group,
					   GNOME_TYPE_CANVAS_WIDGET,
					   "widget", swin,
					   "x", x1,
					   "y", y1,
					   "height", (double)rect.height,
					   "width", (double)rect.width,
					   NULL);
    }

    break;
  }

  if (item->item_type == GREETER_ITEM_TYPE_RECT ||
      item->item_type == GREETER_ITEM_TYPE_SVG ||
      item->item_type == GREETER_ITEM_TYPE_PIXMAP ||
      item->item_type == GREETER_ITEM_TYPE_LABEL)
    g_signal_connect (G_OBJECT (item->item), "event",
		      G_CALLBACK (greeter_item_event_handler),
		      item);
}

/* This is so evil it hurts */
static char *
make_ugly_long_string_with_line_breaks (const char *orig)
{
	const char *p;
	int n_chars, i;
	GString *foo = g_string_new (NULL);
	gboolean in_element = FALSE;

	n_chars = g_utf8_strlen (orig, -1);
	p = orig;

	i = 0;
	while (i < n_chars) {
		gunichar ch = g_utf8_get_char (p);
		if (ch == '<')
			in_element = TRUE;
		else if (ch == '>')
			in_element = FALSE;
		g_string_append_unichar (foo, ch);
		if ( ! in_element)
		g_string_append_unichar (foo, '\n');
		p = g_utf8_next_char (p);
		i++;
	}

	return g_string_free (foo, FALSE);
}

/* This function is very VERY evil */
static gboolean
append_word (GString *str, GString *line, GString *word, const char *after, int max_width, const char *textattr, GnomeCanvasItem *canvas_item)
{
	int width, height;
	char *post = make_ugly_long_string_with_line_breaks (after);
	char *try = g_strconcat (str->str, word->str, "\n", post, NULL);
	g_free (post);
	gnome_canvas_item_set (GNOME_CANVAS_ITEM (canvas_item), textattr, try, NULL);
	g_free (try);

	pango_layout_get_pixel_size (GNOME_CANVAS_TEXT (canvas_item)->layout, &width, &height);

	if (width > max_width) {
		if ( ! ve_string_empty (line->str)) {
			if (str->len > 0 &&
			    str->str[str->len-1] == ' ') {
				g_string_truncate (str, str->len-1);
			}
			g_string_append_unichar (str, '\n');
		}
		g_string_assign (line, word->str);
		g_string_append (str, word->str);
		g_string_assign (word, "");

		return TRUE;
	} else {
		g_string_append (line, word->str);
		g_string_append (str, word->str);
		g_string_assign (word, "");

		return FALSE;
	}
}

/* This function is very VERY evil */
/* Note that it should just use pango and do all the right things rather then
   the utter hacks it tries to do.  But I couldn't figure out how to do this
   simply with pango and how to interact with markup properly (it seems easy
   to do if there is no markup */
void
greeter_canvas_item_break_set_string (GreeterItemInfo *info,
				      const char *orig,
				      gboolean markup,
				      int max_width,
				      int *width,
				      int *height,
				      GnomeCanvas *canvas,
				      GnomeCanvasItem *real_item)
{
	PangoLogAttr *attrs;
	int n_chars;
	GString *str;
	GString *word;
	GString *line;
	int i;
	int n_attrs;
	int ia;
	const char *p;
	int in_current_row;
	GnomeCanvasItem *canvas_item;
	const char *textattr;
	int lwidth, lheight;

	str = g_string_new (NULL);
	word = g_string_new (NULL);
	line = g_string_new (NULL);

	/* avoid errors and even possible crashes */
	if (markup && ! pango_parse_markup (orig, -1, 0, NULL, NULL, NULL, NULL)) {
		markup = FALSE;
	}

	textattr = markup ? "markup" : "text";

	/* A gross hack */
	if (real_item != NULL)
		canvas_item = real_item;
	else
		canvas_item = gnome_canvas_item_new (gnome_canvas_root (canvas),
						     GREETER_TYPE_CANVAS_TEXT,
						     textattr, "",
						     "x", 0.0,
						     "y", 0.0,
						     "font_desc", info->data.text.fonts[GREETER_ITEM_STATE_NORMAL],
						     NULL);

	if (max_width == 0) {
		gnome_canvas_item_set (GNOME_CANVAS_ITEM (canvas_item), textattr, orig, NULL);
		pango_layout_get_pixel_size (GNOME_CANVAS_TEXT (canvas_item)->layout, width, height);

		if (real_item != canvas_item) {
			gtk_object_destroy (GTK_OBJECT (canvas_item));
			g_string_free (line, TRUE);
			g_string_free (word, TRUE);
			g_string_free (str, TRUE);
		}
		return;
	}

	n_chars = g_utf8_strlen (orig, -1);

	gnome_canvas_item_set (GNOME_CANVAS_ITEM (canvas_item), textattr, orig, NULL);
	pango_layout_get_pixel_size (GNOME_CANVAS_TEXT (canvas_item)->layout, &lwidth, &lheight);
	if (lwidth <= max_width) {
		if (width != NULL)
			*width = lwidth;
		if (height != NULL)
			*height = lheight;
		if (real_item != canvas_item) {
			gtk_object_destroy (GTK_OBJECT (canvas_item));
			g_string_free (line, TRUE);
			g_string_free (word, TRUE);
			g_string_free (str, TRUE);
		}
		return;
	}

	pango_layout_get_log_attrs (GNOME_CANVAS_TEXT (canvas_item)->layout, &attrs, &n_attrs);

	i = 0;
	ia = 0;
	in_current_row = 0;
	p = orig;
	while (i < n_chars) {
		gunichar ch;

		ch = g_utf8_get_char (p);

		if (markup && ch == '<') {
			while (i < n_chars) {
				g_string_append_unichar (word, ch);
				p = g_utf8_next_char (p);
				i++;

				if (ch == '>') {
					ch = g_utf8_get_char (p);
					break;
				} else {
					ch = g_utf8_get_char (p);
				}
			}
			if (i >= n_chars)
				break;
		}	

		if (attrs[ia].is_line_break && in_current_row > 0) {
			if (append_word (str, line, word, p, max_width, textattr, canvas_item))
				in_current_row = 0;
		}

		in_current_row++;
		g_string_append_unichar (word, ch);

		p = g_utf8_next_char (p);
		i++;
		ia++;

		/* eeek! */
		if (ia >= n_attrs) {
			while (i < n_chars) {
				ch = g_utf8_get_char (p);
				g_string_append_unichar (word, ch);
				p = g_utf8_next_char (p);
				i++;
			}
			break;
		}
	}

	if ( ! ve_string_empty (word->str))
		append_word (str, line, word, "", max_width, textattr, canvas_item);

	gnome_canvas_item_set (GNOME_CANVAS_ITEM (canvas_item), textattr, str->str, NULL);
	pango_layout_get_pixel_size (GNOME_CANVAS_TEXT (canvas_item)->layout, width, height);

	if (real_item != canvas_item)
		gtk_object_destroy (GTK_OBJECT (canvas_item));
	g_free (attrs);

	g_string_free (line, TRUE);
	g_string_free (word, TRUE);

	g_string_free (str, TRUE);
}

