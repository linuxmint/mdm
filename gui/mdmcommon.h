/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * MDM - The MDM Display Manager
 * Copyright (C) 1999, 2000 Martin K. Petersen <mkp@mkp.net>
 *
 * This file Copyright (c) 2003 George Lebl
 * - Common routines for the greeters.
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

#ifndef MDM_COMMON_H
#define MDM_COMMON_H

#include <gtk/gtk.h>

#include "misc.h"

/* Handle error messages */
void      mdm_common_log_init               (void);
void      mdm_common_log_set_debug          (gboolean enable);
void	  mdm_common_fail_exit		    (const gchar *format, ...)
					     G_GNUC_PRINTF (1, 2);
void	  mdm_common_fail_greeter	    (const gchar *format, ...)
					     G_GNUC_PRINTF (1, 2);
void	  mdm_common_info		    (const gchar *format, ...)
					     G_GNUC_PRINTF (1, 2);
void	  mdm_common_error		    (const gchar *format, ...)
					     G_GNUC_PRINTF (1, 2);
void	  mdm_common_warning		    (const gchar *format, ...)
					     G_GNUC_PRINTF (1, 2);
void	  mdm_common_debug		    (const gchar *format, ...)
					     G_GNUC_PRINTF (1, 2);

/* Misc. Common Functions */
void	  mdm_common_setup_cursor	    (GdkCursorType type);

void      mdm_common_setup_builtin_icons    (void);

void      mdm_common_login_sound            (const gchar *MdmSoundProgram,
                                             const gchar *MdmSoundOnLoginReadyFile,
                                             gboolean     MdmSoundOnLoginReady);

void	  mdm_common_setup_blinking	    (void);
void	  mdm_common_setup_blinking_entry   (GtkWidget *entry);

GdkPixbuf *mdm_common_get_face              (const char *filename,
                                             const char *fallback_filename,
                                             guint       max_width,
                                             guint       max_height);

gchar*	  mdm_common_text_to_escaped_utf8   (const char *text);
gchar*	  mdm_common_get_config_file	    (void);
gchar*	  mdm_common_get_custom_config_file (void);
gboolean  mdm_common_select_time_format	    (void);
void	  mdm_common_setup_background_color (gchar *bg_color);
void      mdm_common_set_root_background    (GdkPixbuf *pb);
gchar*	  mdm_common_get_welcomemsg	    (void);
void	  mdm_common_pre_fetch_launch       (void);
void      mdm_common_atspi_launch           (void);
gchar*    mdm_common_expand_text            (const gchar *text);
gchar*    mdm_common_get_clock              (struct tm **the_tm);
gboolean  mdm_common_locale_is_displayable  (const gchar *locale);
gboolean  mdm_common_is_action_available    (gchar *action);
#endif /* MDM_COMMON_H */
