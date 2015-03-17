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

#include "config.h"

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <dirent.h>
#include <gtk/gtk.h>
#include <glib/gi18n.h>

#include "mdm.h"
#include "mdmsession.h"
#include "mdmcommon.h"
#include "mdmconfig.h"

#include "mdm-common.h"
#include "mdm-daemon-config-keys.h"

GHashTable *sessnames        = NULL;
gchar *default_session       = NULL;
const gchar *current_session = NULL;
GList *sessions              = NULL;

/* This is true if session dir doesn't exist or is whacked out
 * in some way or another */
gboolean session_dir_whacked_out = FALSE;

gint
mdm_session_sort_func (const char *a, const char *b)
{
        /* Put default session at the top */
        if (strcmp (a, "default.desktop") == 0)
                return -1;

        if (strcmp (b, "default.desktop") == 0)
                return 1;

        /* put everything else in the middle in alphabetical order */
                return strcmp (a, b);
}

const char *
mdm_session_name (const char *name)
{
        MdmSession *session;

        /* eek */
        if G_UNLIKELY (name == NULL)
                return "(null)";

        session = g_hash_table_lookup (sessnames, name);
        if (session != NULL && !ve_string_empty (session->name))
                return session->name;
        else
		return name;
}

void
mdm_session_list_from_hash_table_func (const char *key, const char *value, GList **sessions)
{
    *sessions = g_list_prepend (*sessions, g_strdup (key));
}

/* Just a wrapper to ensure compatibility with the
   existing code */
void
mdm_session_list_init ()
{
	_mdm_session_list_init (&sessnames, &sessions, &default_session, &current_session);
}

/* The real mdm_session_list_init */
void
_mdm_session_list_init (GHashTable **sessnames, GList **sessions, gchar **default_session, const gchar **current_session)
{

    MdmSession *session = NULL;
    gboolean some_dir_exists = FALSE;
    gboolean searching_for_default = TRUE;
    struct dirent *dent;
    char **vec;
    char *name;
    DIR *sessdir;
    int i;

    *sessnames = g_hash_table_new (g_str_hash, g_str_equal);

    vec = g_strsplit (mdm_config_get_string (MDM_KEY_SESSION_DESKTOP_DIR),
	 ":", -1);
    for (i = 0; vec != NULL && vec[i] != NULL; i++) {
	    const char *dir = vec[i];

	    /* Check that session dir is readable */
	    if G_UNLIKELY (dir == NULL || access (dir, R_OK|X_OK) != 0)
		    continue;

	    some_dir_exists = TRUE;

	    /* Read directory entries in session dir */
	    sessdir = opendir (dir);

	    if G_LIKELY (sessdir != NULL)
		    dent = readdir (sessdir);
	    else
		    dent = NULL;

	    while (dent != NULL) {
		    GKeyFile *cfg;
		    char *exec;
		    char *comment;
		    char *s;
		    char *tryexec;
		    char *ext;
		    gboolean hidden;

		    /* ignore everything but the .desktop files */
		    ext = strstr (dent->d_name, ".desktop");
		    if (ext == NULL ||
			strcmp (ext, ".desktop") != 0) {
			    dent = readdir (sessdir);
			    continue;
		    }

		    /* already found this session, ignore */
		    if (g_hash_table_lookup (*sessnames, dent->d_name) != NULL) {
			    dent = readdir (sessdir);
			    continue;
		    }

		    s = g_strconcat (dir, "/", dent->d_name, NULL);
		    cfg = mdm_common_config_load (s, NULL);
		    g_free (s);

		    hidden = FALSE;
		    mdm_common_config_get_boolean (cfg, "Desktop Entry/Hidden=false", &hidden, NULL);
		    if (hidden) {
			    g_key_file_free (cfg);
			    dent = readdir (sessdir);
			    continue;
		    }

		    tryexec = NULL;
		    mdm_common_config_get_string (cfg, "Desktop Entry/TryExec", &tryexec, NULL);
		    if ( ! ve_string_empty (tryexec)) {
			    char **tryexecvec = g_strsplit (tryexec, " ", -1);
			    char *full = NULL;

			    /* Do not pass any arguments to g_find_program_in_path */
			    if (tryexecvec != NULL)
				full = g_find_program_in_path (tryexecvec[0]);

			    if (full == NULL) {
				    session = g_new0 (MdmSession, 1);
				    session->name      = g_strdup (dent->d_name);
				    g_hash_table_insert (*sessnames, g_strdup (dent->d_name),
					 session);
				    g_free (tryexec);
				    g_key_file_free (cfg);
				    dent = readdir (sessdir);
				    continue;
			    }
			    g_strfreev (tryexecvec);
			    g_free (full);
		    }
		    g_free (tryexec);

		    exec = NULL;
		    name = NULL;
		    comment = NULL;
		    mdm_common_config_get_string (cfg, "Desktop Entry/Exec", &exec, NULL);
		    mdm_common_config_get_translated_string (cfg, "Desktop Entry/Name", &name, NULL);
		    mdm_common_config_get_translated_string (cfg, "Desktop Entry/Comment", &comment, NULL);
		    g_key_file_free (cfg);

		    if G_UNLIKELY (ve_string_empty (exec) || ve_string_empty (name)) {
			    session = g_new0 (MdmSession, 1);
			    session->name      = g_strdup (dent->d_name);
			    g_hash_table_insert (*sessnames, g_strdup (dent->d_name), session);
			    g_free (exec);
			    g_free (name);
			    g_free (comment);
			    dent = readdir (sessdir);
			    continue;
		    }

		    /* if we found the default session */
		    if (default_session != NULL) {
			    if ( ! ve_string_empty (mdm_config_get_string (MDM_KEY_DEFAULT_SESSION)) &&
				 strcmp (dent->d_name, mdm_config_get_string (MDM_KEY_DEFAULT_SESSION)) == 0) {
				    g_free (*default_session);
				    *default_session = g_strdup (dent->d_name);
				    searching_for_default = FALSE;
			    }

			    /* if there is a session called Default */
			    if (searching_for_default &&
				g_ascii_strcasecmp (dent->d_name, "default.desktop") == 0) {
				    g_free (*default_session);
				    *default_session = g_strdup (dent->d_name);
			    }
		    }

		    session = g_new0 (MdmSession, 1);
		    session->name      = g_strdup (name);
		    session->comment   = g_strdup (comment);
		    g_hash_table_insert (*sessnames, g_strdup (dent->d_name), session);
		    g_free (exec);
		    g_free (comment);
		    dent = readdir (sessdir);
	    }

	    if G_LIKELY (sessdir != NULL)
		    closedir (sessdir);
    }

    g_strfreev (vec);

    /* Check that session dir is readable */
    if G_UNLIKELY ( ! some_dir_exists) {
	   mdm_common_error ("%s: Session directory <%s> not found!", "mdm_session_list_init", ve_sure_string (mdm_config_get_string (MDM_KEY_SESSION_DESKTOP_DIR)));
	   session_dir_whacked_out = TRUE;
    }

    /* Convert to list (which is unsorted) */
    g_hash_table_foreach (*sessnames, (GHFunc) mdm_session_list_from_hash_table_func, sessions);

    /* Prioritize and sort the list */
    *sessions = g_list_sort (*sessions, (GCompareFunc) mdm_session_sort_func);

    if (current_session != NULL && default_session != NULL) {
	    if (*current_session == NULL) {
		    *current_session = *default_session;
        }
    }
}

const char* mdm_get_default_session (void) {
    return default_session;
}
