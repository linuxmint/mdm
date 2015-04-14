/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * MDM - The MDM Display Manager
 * Copyright (C) 1998, 1999, 2000 Martin K. Petersen <mkp@mkp.net>
 * Copyright (C) 2005 Sun Microsystems, Inc.
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

#ifndef _MDM_DAEMON_CONFIG_H
#define _MDM_DAEMON_CONFIG_H

#include "server.h"
#include "mdm-daemon-config-entries.h"

G_BEGIN_DECLS

const char *   mdm_daemon_config_get_string_for_id    (int id);
gboolean       mdm_daemon_config_get_bool_for_id      (int id);
int            mdm_daemon_config_get_int_for_id       (int id);

void           mdm_daemon_config_parse                (const char *config_file,
                                                       gboolean    no_console);
MdmXserver *   mdm_daemon_config_find_xserver         (const char *id);
char *         mdm_daemon_config_get_xservers         (void);
GSList *       mdm_daemon_config_get_display_list     (void);
GSList *       mdm_daemon_config_display_list_append  (MdmDisplay *display);
GSList *       mdm_daemon_config_display_list_insert  (MdmDisplay *display);
GSList *       mdm_daemon_config_display_list_remove  (MdmDisplay *display);
uid_t          mdm_daemon_config_get_mdmuid           (void);
uid_t          mdm_daemon_config_get_mdmgid           (void);
gint           mdm_daemon_config_get_high_display_num (void);
void           mdm_daemon_config_set_high_display_num (gint val);
void           mdm_daemon_config_close                (void);

/* deprecated */
char *         mdm_daemon_config_get_display_custom_config_file (const char *display);
char *         mdm_daemon_config_get_custom_config_file (void);


const char *   mdm_daemon_config_get_value_string     (const char *key);
const char **  mdm_daemon_config_get_value_string_array (const char *key);
gboolean       mdm_daemon_config_get_value_bool       (const char *key);
gint           mdm_daemon_config_get_value_int        (const char *key);
char *         mdm_daemon_config_get_value_string_per_display (const char *key,
                                                               const char *display);
gboolean       mdm_daemon_config_get_value_bool_per_display   (const char *key,
                                                               const char *display);
gint           mdm_daemon_config_get_value_int_per_display    (const char *key,
                                                               const char *display);

void           mdm_daemon_config_set_value_string     (const char *key,
                                                       const char *value);
void           mdm_daemon_config_set_value_bool       (const char *key,
                                                       gboolean value);
void           mdm_daemon_config_set_value_int        (const char *key,
                                                       gint value);


gboolean       mdm_daemon_config_key_to_string_per_display (const char *display,
                                                            const char *key,
                                                            char **retval);
gboolean       mdm_daemon_config_key_to_string        (const char *file,
                                                       const char *key,
                                                       char **retval);
gboolean       mdm_daemon_config_to_string            (const char *key,
                                                       const char *display,
                                                       char **retval);
gboolean       mdm_daemon_config_update_key           (const char *key);


int            mdm_daemon_config_compare_displays     (gconstpointer a,
                                                       gconstpointer b);
gboolean       mdm_daemon_config_is_valid_key         (const char *key);
gboolean       mdm_daemon_config_signal_terminthup_was_notified  (void);
void           mdm_daemon_config_get_user_session_lang (char **usrsess,
                                                        char **usrlang,
                                                        const char *homedir);
void	       mdm_daemon_config_set_user_session_lang (gboolean savesess,
                                                        gboolean savelang,
                                                        const char *home_dir,
                                                        const char *save_session,
                                                        const char *save_language);
char *         mdm_daemon_config_get_session_exec     (const char *session_name,
                                                       gboolean check_try_exec);
char *         mdm_daemon_config_get_session_xserver_args (const char *session_name);


G_END_DECLS

#endif /* _MDM_DAEMON_CONFIG_H */
