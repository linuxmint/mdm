/* MDM - The MDM Display Manager
 * Copyright (C) 1999, 2000 Martin K. Petersen <mkp@mkp.net>
 *
 * This file Copyright (c) 2001 George Lebl
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

#ifndef MDM_WM_H
#define MDM_WM_H

#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <X11/X.h>
#include <X11/Xlib.h>

/*
 * Login window will be given focus every time a window
 * is killed
 */
void	mdm_wm_init			(Window login_window);

/*
 * By default new windows aren't given focus, you have to
 * call this function with a TRUE
 */
void	mdm_wm_focus_new_windows	(gboolean focus);

void	mdm_wm_focus_window		(Window window);

/* Movement for the impatient */
void	mdm_wm_move_window_now		(Window window,
					 int x,
					 int y);
void	mdm_wm_get_window_pos		(Window window,
					 int *xp,
					 int *yp);

/* Refuse to focus the login window, poor mans modal dialogs */
void	mdm_wm_no_login_focus_push	(void);
void	mdm_wm_no_login_focus_pop	(void);

/*
 * Xinerama support stuff
 */
void	mdm_wm_screen_init		(gchar *current_monitor_num);

/*
 * Not really a WM function, center a gtk window on current screen
 * by setting uposition
 */
void	mdm_wm_center_window		(GtkWindow *cw);

/* Center mouse pointer
 */
void mdm_wm_center_cursor (void);

/*
 * Save and restore stacking order, useful for restarting
 * the greeter
 */
void	mdm_wm_save_wm_order		(void);
void	mdm_wm_restore_wm_order		(void);

/* Dialogs */
gint    mdm_wm_query_dialog             (const gchar *primary_message,
                                         const gchar *secondary_message,
                                         const char *posbutton,
                                         const char *negbutton,
                                         gboolean has_cancel);
gint    mdm_wm_warn_dialog              (const gchar *primary_message,
                                         const gchar *secondary_message,
                                         const char *posbutton,
                                         const char *negbutton,
                                         gboolean has_cancel);
void    mdm_wm_show_info_msg_dialog     (const gchar *msg_file,
                                         const gchar *msg_font);
void    mdm_wm_message_dialog           (const gchar *primary_message,
                                         const gchar *secondary_message);

/* Access to the screen structures */
extern GdkRectangle *mdm_wm_all_monitors;
extern int mdm_wm_num_monitors;
extern GdkRectangle mdm_wm_screen;

#endif /* MDM_WM_H */

/* EOF */
