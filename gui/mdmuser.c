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
#include <locale.h>
#include <glib/gi18n.h>
#include <string.h>
#include <unistd.h>

#include <pwd.h>

#include "mdm.h"
#include "mdm-common.h"
#include "mdmcommon.h"
#include "mdmuser.h"
#include "mdmconfig.h"

#include "mdm-socket-protocol.h"
#include "mdm-daemon-config-keys.h"

static time_t time_started;

static MdmUser * 
mdm_user_alloc (const gchar *logname,
		uid_t uid,
		const gchar *homedir,
		const char *gecos,
		GdkPixbuf *defface,
		gboolean read_faces)
{
	MdmUser *user;
	GdkPixbuf *img = NULL;
	gchar buf[PIPE_SIZE];
	size_t size;
	int bufsize;
	char *p;

	user = g_new0 (MdmUser, 1);

	user->uid = uid;
	user->login = g_strdup (logname);
	if (!g_utf8_validate (gecos, -1, NULL))
		user->gecos = ve_locale_to_utf8 (gecos);
	else
		user->gecos = g_strdup (gecos);

	/* Cut up to first comma since those are ugly arguments and
	 * not the name anymore, but only if more then 1 comma is found,
	 * since otherwise it might be part of the actual comment,
	 * this is sort of "heurestic" because there seems to be no
	 * real standard, it's all optional */
	p = strchr (user->gecos, ',');
	if (p != NULL) {
		if (strchr (p+1, ',') != NULL)
			*p = '\0';
	}

	user->homedir = g_strdup (homedir);
	if (defface != NULL)
		user->picture = (GdkPixbuf *)g_object_ref (G_OBJECT (defface));

	if (ve_string_empty (logname))
		return user;

	/* don't read faces, since that requires the daemon */
	if (!read_faces)
		return user;

	/* read initial request */
	do {
		while (read (STDIN_FILENO, buf, 1) == 1)
			if (buf[0] == STX)
				break;
		size = read (STDIN_FILENO, buf, sizeof (buf));
		if (size <= 0)
			return user;
	} while (buf[0] != MDM_NEEDPIC);

	printf ("%c%s\n", STX, logname);
	fflush (stdout);

	do {
		while (read (STDIN_FILENO, buf, 1) == 1)
			if (buf[0] == STX)
				break;
		size = read (STDIN_FILENO, buf, sizeof (buf));
		if (size <= 0)
			return user;
	} while (buf[0] != MDM_READPIC);

	/* both nul terminate and wipe the trailing \n */
	buf[size-1] = '\0';

	if (size < 2) {
		img = NULL;
	} else if (sscanf (&buf[1], "buffer:%d", &bufsize) == 1) {
		unsigned char buffer[2048];
		int pos = 0;
		int n;
		GdkPixbufLoader *loader;
		/* we trust the daemon, even if it wanted to give us
		 * bogus bufsize */
		/* the daemon will now print the buffer */
		printf ("%cOK\n", STX);
		fflush (stdout);

		while (read (STDIN_FILENO, buf, 1) == 1)
			if (buf[0] == STX)
				break;

		loader = gdk_pixbuf_loader_new ();

		while ((n = read (STDIN_FILENO, buffer,
				  MIN (sizeof (buffer), bufsize-pos))) > 0) {
			gdk_pixbuf_loader_write (loader, buffer, n, NULL);
			pos += n;
			if (pos >= bufsize)
			       break;	
		}

		gdk_pixbuf_loader_close (loader, NULL);

		img = gdk_pixbuf_loader_get_pixbuf (loader);
		if (img != NULL) {
			g_object_ref (G_OBJECT (img));
			GdkPixbuf *img2;
			img2 = gdk_pixbuf_scale_simple (img, 48, 48, GDK_INTERP_BILINEAR);
			g_object_unref (img);
			img = img2;		    
		}
				
		g_object_unref (G_OBJECT (loader));

		/* read the "done" bit, but don't check */
		read (STDIN_FILENO, buf, sizeof (buf));
	} else if (g_access (&buf[1], R_OK) == 0) {
		img = mdm_common_get_face (&buf[1],
					   NULL,
					   mdm_config_get_int (MDM_KEY_MAX_ICON_WIDTH),
					   mdm_config_get_int (MDM_KEY_MAX_ICON_HEIGHT));
	} else {
		img = NULL;
	}

	/* the daemon is now free to go on */
	printf ("%c\n", STX);
	fflush (stdout);

	if (img != NULL) {
		if (user->picture != NULL)
			g_object_unref (G_OBJECT (user->picture));

		user->picture = img;
	}

	return user;
}

static gboolean
mdm_check_exclude (struct passwd *pwent, char **excludes, gboolean is_local)
{
	const char * const lockout_passes[] = { "!!", NULL };
	gint i;

        if ( ! mdm_config_get_bool (MDM_KEY_ALLOW_ROOT) && pwent->pw_uid == 0)
                return TRUE;       

	if (pwent->pw_uid < mdm_config_get_int (MDM_KEY_MINIMAL_UID))
		return TRUE;

	for (i=0 ; lockout_passes[i] != NULL ; i++)  {
		if (strcmp (lockout_passes[i], pwent->pw_passwd) == 0) {
			return TRUE;
		}
	}

	if (excludes != NULL) {
		for (i=0 ; excludes[i] != NULL ; i++)  {
			if (g_ascii_strcasecmp (excludes[i],
						pwent->pw_name) == 0) {
				return TRUE;
			}
		}
	}

	return FALSE;
}

static gboolean
mdm_check_shell (const gchar *usersh)
{
    gint found = 0;
    gchar *csh;

    if (strcmp (usersh, NOLOGIN) == 0 ||
	strcmp (usersh, "/bin/true") == 0 ||
	strcmp (usersh, "/bin/false") == 0) {
      return 0;
    }

    setusershell ();

    while ((csh = getusershell ()) != NULL)
        if (! strcmp (csh, usersh))
            found = 1;

    endusershell ();

    return (found);
}

static gint
mdm_sort_func (gpointer d1, gpointer d2)
{
    MdmUser *a = d1;
    MdmUser *b = d2;

    if (!d1 || !d2)
        return (0);

    return (strcmp (a->login, b->login));
}


static gboolean
setup_user (struct passwd *pwent,
	    GList **users,
	    GList **users_string,
	    char **excludes,
	    char *exclude_user,
	    GdkPixbuf *defface,
	    int *size_of_users,
	    gboolean is_local,
	    gboolean read_faces)
{
    MdmUser *user;
    int cnt = 0;

    if (pwent->pw_shell && 
	mdm_check_shell (pwent->pw_shell) &&
	!mdm_check_exclude (pwent, excludes, is_local) &&
	(exclude_user == NULL ||
	strcmp (ve_sure_string (exclude_user), pwent->pw_name)) != 0) {

	    user = mdm_user_alloc (pwent->pw_name,
				   pwent->pw_uid,
				   pwent->pw_dir,
				   ve_sure_string (pwent->pw_gecos),
				   defface, read_faces);

	    if ((user) &&
		(! g_list_find_custom (*users, user, (GCompareFunc) mdm_sort_func))) {
		cnt++;
		*users = g_list_insert_sorted (*users, user,
		     (GCompareFunc) mdm_sort_func);
		*users_string = g_list_prepend (*users_string, g_strdup (pwent->pw_name));

		if (user->picture != NULL) {
			*size_of_users +=
				gdk_pixbuf_get_height (user->picture) + 2;
		} else {
			*size_of_users += mdm_config_get_int (MDM_KEY_MAX_ICON_HEIGHT);
		}
	    }

	    if (cnt > 1000 || time_started + 5 <= time (NULL)) {
		*users = g_list_append (*users,
			g_strdup (_("Too many users to list here...")));
		*users_string = g_list_append (*users_string,
			g_strdup (_("Too many users to list here...")));

		return (FALSE);
	    }
    }
    return (TRUE);
}

gboolean
mdm_is_user_valid (const char *username)
{
    return (NULL != getpwnam (username));
}

gint
mdm_user_uid (const char *username)
{
    struct passwd *pwent;
    pwent = getpwnam (username);
    if (pwent != NULL)
	    return pwent->pw_uid;

    return -1;
}

const char *
get_root_user (void)
{
	static char *root_user = NULL;
	struct passwd *pwent;
	
	if (root_user != NULL)
		return root_user;
	
	pwent = getpwuid (0);
	if (pwent == NULL) /* huh? */
		root_user = g_strdup ("root");
	else
		root_user = g_strdup (pwent->pw_name);
	return root_user;
}

void 
mdm_users_init (GList **users,
		GList **users_string,
		char *exclude_user,
		GdkPixbuf *defface,
		int *size_of_users,
		gboolean is_local,
		gboolean read_faces)
{
    struct passwd *pwent;
    char **includes;
    char **excludes;
    gboolean found_include = FALSE;
    int i;

    time_started = time (NULL);
	
    includes = g_strsplit (mdm_config_get_string (MDM_KEY_INCLUDE), ",", 0);
    for (i=0 ; includes != NULL && includes[i] != NULL ; i++) {
	g_strstrip (includes[i]);
        if (includes[i] != NULL)
           found_include = TRUE;
    }

    excludes = g_strsplit (mdm_config_get_string (MDM_KEY_EXCLUDE), ",", 0);
    for (i=0 ; excludes != NULL && excludes[i] != NULL ; i++)
	g_strstrip (excludes[i]);

    if (mdm_config_get_bool (MDM_KEY_INCLUDE_ALL) == TRUE) {
	    setpwent ();
	    pwent = getpwent ();
	    while (pwent != NULL) {

		if (! setup_user (pwent, users, users_string, excludes,
			exclude_user, defface, size_of_users, is_local,
			read_faces))
			break;

		pwent = getpwent ();
	}
	endpwent ();

    } else if (found_include == TRUE) {
	for (i=0 ; includes != NULL && includes[i] != NULL ; i++) {
		pwent = getpwnam (includes[i]);

		if (pwent != NULL) {
			if (!setup_user (pwent, users, users_string, excludes,
			    exclude_user, defface, size_of_users, is_local,
			    read_faces))
			break;

		}
	}
    }

    g_strfreev (includes);
    g_strfreev (excludes);
}

