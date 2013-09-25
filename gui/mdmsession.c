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
static gint dont_save_session     = GTK_RESPONSE_YES;


/* This is true if session dir doesn't exist or is whacked out
 * in some way or another */
gboolean session_dir_whacked_out = FALSE;

gint
mdm_session_sort_func (const char *a, const char *b)
{
        /* Put default and GNOME sessions at the top */
        if (strcmp (a, ve_sure_string (mdm_config_get_string (MDM_KEY_DEFAULT_SESSION))) == 0)
                return -1;

        if (strcmp (b, ve_sure_string (mdm_config_get_string (MDM_KEY_DEFAULT_SESSION))) == 0)
                return 1;

        if (strcmp (a, "default.desktop") == 0)
                return -1;

        if (strcmp (b, "default.desktop") == 0)
                return 1;

        if (strcmp (a, "gnome.desktop") == 0)
                return -1;

        if (strcmp (b, "gnome.desktop") == 0)
                return 1;

        /* put failsafe sessions on the bottom */
        if (strcmp (b, MDM_SESSION_FAILSAFE_XTERM) == 0)
                return -1;

        if (strcmp (a, MDM_SESSION_FAILSAFE_XTERM) == 0)
                return 1;

        if (strcmp (b, MDM_SESSION_FAILSAFE_GNOME) == 0)
                return -1;

        if (strcmp (a, MDM_SESSION_FAILSAFE_GNOME) == 0)
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
mdm_session_list_from_hash_table_func (const char *key, const char *value,
         GList **sessions)
{
        *sessions = g_list_prepend (*sessions, g_strdup (key));
}

/* Just a wrapper to ensure compatibility with the
   existing code */
void
mdm_session_list_init ()
{
	_mdm_session_list_init (&sessnames, &sessions, 
				&default_session, &current_session);	
}

/* The real mdm_session_list_init */
void
_mdm_session_list_init (GHashTable **sessnames, GList **sessions, 
			gchar **default_session, const gchar **current_session)
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

    if (mdm_config_get_bool (MDM_KEY_SHOW_GNOME_FAILSAFE)) {
	session = g_new0 (MdmSession, 1);
	session->name = g_strdup (_("Failsafe _GNOME"));
	session->clearname = g_strdup (_("Failsafe GNOME"));
	session->comment = g_strdup (_("This is a failsafe session that will log you "
		"into GNOME. No startup scripts will be read "
		"and it is only to be used when you can't log "
		"in otherwise.  GNOME will use the 'Default' "
		"session."));
	g_hash_table_insert (*sessnames, g_strdup (MDM_SESSION_FAILSAFE_GNOME), session);
    }

    if (mdm_config_get_bool (MDM_KEY_SHOW_XTERM_FAILSAFE)) {
	/* Valgrind complains that the below is leaked */
	session = g_new0 (MdmSession, 1);
	session->name = g_strdup (_("Failsafe _Terminal"));
	session->clearname = g_strdup (_("Failsafe Terminal"));
	session->comment = g_strdup (_("This is a failsafe session that will log you "
		"into a terminal.  No startup scripts will be read "
		"and it is only to be used when you can't log "
		"in otherwise.  To exit the terminal, "
		"type 'exit'."));
	g_hash_table_insert (*sessnames, g_strdup (MDM_SESSION_FAILSAFE_XTERM),
		session);
    }

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
				    session->clearname = NULL;
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
			    session->clearname = NULL;
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

			    if (searching_for_default &&
				g_ascii_strcasecmp (dent->d_name, "gnome.desktop") == 0) {
				    /* Just in case there is no default session and
				     * no default link, make gnome the default */
				    if (*default_session == NULL)
					    *default_session = g_strdup (dent->d_name);

			    }
		    }

		    session = g_new0 (MdmSession, 1);
		    session->name      = g_strdup (name);
		    session->clearname = NULL;
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
	mdm_common_error ("%s: Session directory <%s> not found!",
		"mdm_session_list_init", ve_sure_string
		 (mdm_config_get_string (MDM_KEY_SESSION_DESKTOP_DIR)));
	session_dir_whacked_out = TRUE;
    }

    if G_UNLIKELY (g_hash_table_size (*sessnames) == 0) {
	    mdm_common_warning ("Error, no sessions found in the session directory <%s>.",
		ve_sure_string (mdm_config_get_string (MDM_KEY_SESSION_DESKTOP_DIR)));

	    session_dir_whacked_out = TRUE;
	    if (default_session != NULL)
		    *default_session = g_strdup (MDM_SESSION_FAILSAFE_GNOME);
    }


    if (mdm_config_get_bool (MDM_KEY_SHOW_GNOME_FAILSAFE)) {
	    session            = g_new0 (MdmSession, 1);
	    session->name      = g_strdup (_("Failsafe _GNOME"));
	    session->clearname = g_strdup (_("Failsafe GNOME"));
	    session->comment   = g_strdup (_("This is a failsafe session that will log you "
				    "into GNOME. No startup scripts will be read "
                                    "and it is only to be used when you can't log "
                                    "in otherwise.  GNOME will use the 'Default' "
				    "session."));
	    g_hash_table_insert (*sessnames,
		g_strdup (MDM_SESSION_FAILSAFE_GNOME), session);
    }

    if (mdm_config_get_bool (MDM_KEY_SHOW_XTERM_FAILSAFE)) {
	    session            = g_new0 (MdmSession, 1);
	    session->name      = g_strdup (_("Failsafe _Terminal"));
	    session->clearname = g_strdup (_("Failsafe Terminal"));
	    session->comment   = g_strdup (_("This is a failsafe session that will log you "
				    "into a terminal.  No startup scripts will be read "
				    "and it is only to be used when you can't log "
				    "in otherwise.  To exit the terminal, "
				    "type 'exit'."));
	    g_hash_table_insert (*sessnames,
		g_strdup (MDM_SESSION_FAILSAFE_XTERM), session);
    }

    /* Convert to list (which is unsorted) */
    g_hash_table_foreach (*sessnames,
	(GHFunc) mdm_session_list_from_hash_table_func, sessions);

    /* Prioritize and sort the list */
    *sessions = g_list_sort (*sessions, (GCompareFunc) mdm_session_sort_func);

    if (default_session != NULL)
	    if G_UNLIKELY (*default_session == NULL) {
		    *default_session = g_strdup (MDM_SESSION_FAILSAFE_GNOME);
		    mdm_common_warning ("No default session link found. Using Failsafe GNOME.");
	    }
    
    if (current_session != NULL &&
	default_session != NULL) {
	    if (*current_session == NULL)
		    *current_session = *default_session;
    }
}

static gboolean
mdm_login_list_lookup (GList *l, const gchar *data)
{
    GList *list = l;

    if (list == NULL || data == NULL)
        return FALSE;

    /* FIXME: Hack, will support these builtin types later */
    if (strcmp (data, MDM_SESSION_DEFAULT ".desktop") == 0 ||
        strcmp (data, MDM_SESSION_CUSTOM ".desktop") == 0 ||
        strcmp (data, MDM_SESSION_FAILSAFE ".desktop") == 0) {
            return TRUE;
    }

    while (list) {

        if (strcmp (list->data, data) == 0)
            return TRUE;

        list = list->next;
    }

    return FALSE;
}

char *
mdm_session_lookup (const char *saved_session, gint *lookup_status)
{
  gchar *session = NULL;
  
  /* Assume that the lookup will go well */
  *lookup_status = SESSION_LOOKUP_SUCCESS;

  /* Don't save session unless told otherwise */
  dont_save_session = GTK_RESPONSE_YES;

  /* Previously saved session not found in ~/.dmrc */
  if ( ! (saved_session != NULL &&
	  strcmp ("(null)", saved_session) != 0 &&
	  saved_session[0] != '\0')) {
    /* If "Last" is chosen run default,
     * else run user's current selection */
    if (current_session == NULL || strcmp (current_session, LAST_SESSION) == 0)
      session = g_strdup (default_session);
    else
      session = g_strdup (current_session);
    
    dont_save_session = GTK_RESPONSE_NO;
    return session;
  }

  /* If "Last" session is selected */
  if (current_session == NULL ||
      strcmp (current_session, LAST_SESSION) == 0)
    { 
      session = g_strdup (saved_session);
      
      /* Check if user's saved session exists on this box */
      if (!mdm_login_list_lookup (sessions, session))
	{
		
          g_free (session);
	  session = g_strdup (default_session);
	  *lookup_status = SESSION_LOOKUP_PREFERRED_MISSING;
	}
    }
  else /* One of the other available session types is selected */
    { 
      session = g_strdup (current_session);
    
      /* User's saved session is not the chosen one */
      if (strcmp (session, MDM_SESSION_FAILSAFE_GNOME) == 0 ||
	  strcmp (session, MDM_SESSION_FAILSAFE_XTERM) == 0 ||
	  g_ascii_strcasecmp (session, MDM_SESSION_FAILSAFE ".desktop") == 0 ||
	  g_ascii_strcasecmp (session, MDM_SESSION_FAILSAFE) == 0)
	{
          /*
           * Never save failsafe sessions as the default session.
           * These are intended to be used for debugging or temporary 
           * purposes.
           */
	  dont_save_session = GTK_RESPONSE_YES;
	}
      else if (strcmp (saved_session, session) != 0)
	{	 	  
	  if (mdm_config_get_bool (MDM_KEY_SHOW_LAST_SESSION))
	    {
		    *lookup_status = SESSION_LOOKUP_DEFAULT_MISMATCH;
	    }
	  else if (strcmp (session, default_session) != 0 &&
		   strcmp (session, saved_session) != 0 &&
		   strcmp (session, LAST_SESSION) != 0)
	    {
	      /*
	       * If (! MDM_KEY_SHOW_LAST_SESSION) then our saved session is
	       * irrelevant, we are in "switchdesk mode" and the relevant
	       * thing is the saved session in .Xclients
	       */
	      if (g_access ("/usr/bin/switchdesk", F_OK) == 0)
	        {
			*lookup_status = SESSION_LOOKUP_USE_SWITCHDESK;
		}
	      dont_save_session = GTK_RESPONSE_YES;
	    }
	}
    }

  return session;
}

gint
mdm_get_save_session (void)
{
  return dont_save_session;
}

void
mdm_set_save_session (const gint session)
{
	dont_save_session = session;
}

const char*
mdm_get_default_session (void)
{
	return default_session;
}
