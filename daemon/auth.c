/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * MDM - The MDM Display Manager
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

/* Code for cookie handling. This really needs to be modularized to
 * support other XAuth types and possibly DECnet... */

#include "config.h"

#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <errno.h>

#include <X11/Xauth.h>
#include <glib/gi18n.h>

#include "mdm.h"
#include "cookie.h"
#include "misc.h"
#include "filecheck.h"
#include "auth.h"

#include "mdm-common.h"
#include "mdm-log.h"
#include "mdm-daemon-config.h"

/* Ensure we know about FamilyInternetV6 even if what we're compiling
   against doesn't */
#ifdef ENABLE_IPV6
#ifndef FamilyInternetV6
#define FamilyInternetV6	6
#endif /* ! FamilyInternetV6 */
#endif /* ENABLE_IPV6 */

/* Local prototypes */
static FILE *mdm_auth_purge (MdmDisplay *d, FILE *af, gboolean remove_when_empty);

static void
display_add_error (MdmDisplay *d)
{
	if (errno != 0)
		mdm_error ("add_auth_entry: Could not write new authorization entry: %s", strerror (errno));
	else
		mdm_error ("add_auth_entry: Could not write new authorization entry. Possibly out of diskspace");
	if (d->attached) {
		char *s = g_strdup_printf
			(C_(N_("MDM could not write a new authorization "
			       "entry to disk.  Possibly out of diskspace.%s%s")),
			 errno != 0 ? "  Error: " : "",
			 errno != 0 ? strerror (errno) : "");
		mdm_text_message_dialog (s);
		g_free (s);
	}
}

static gboolean
add_auth_entry (MdmDisplay *d,
		GSList **authlist,
		FILE *af,
		FILE *af2,
		unsigned short family,
		const char *addr,
		int addrlen)
{
	Xauth *xa;
	gchar *dispnum;

	if G_UNLIKELY (!d)
		return FALSE;

	xa = malloc (sizeof (Xauth));

	if G_UNLIKELY (xa == NULL)
		return FALSE;

	xa->family = family;
	if (addrlen == 0) {
		xa->address = NULL;
		xa->address_length = 0;
	} else {
		xa->address = malloc (addrlen);
		if G_UNLIKELY (xa->address == NULL) {
			free (xa);
			return FALSE;
		}

		memcpy (xa->address, addr, addrlen);
		xa->address_length = addrlen;
	}

	dispnum = g_strdup_printf ("%d", d->dispnum);
	xa->number = strdup (dispnum);
	xa->number_length = strlen (dispnum);
	g_free (dispnum);

	xa->name = strdup ("MIT-MAGIC-COOKIE-1");
	xa->name_length = strlen ("MIT-MAGIC-COOKIE-1");
	xa->data = malloc (16);
	if G_UNLIKELY (xa->data == NULL) {
		free (xa->number);
		free (xa->name);
		free (xa->address);
		free (xa);
		return FALSE;
	}
	memcpy (xa->data, d->bcookie, 16);
	xa->data_length = 16;

	if (af != NULL) {
		errno = 0;
		if G_UNLIKELY ( ! XauWriteAuth (af, xa)) {
			free (xa->data);
			free (xa->number);
			free (xa->name);
			free (xa->address);
			free (xa);
			display_add_error (d);
			return FALSE;
		}

		if (af2 != NULL) {
			errno = 0;
			if G_UNLIKELY ( ! XauWriteAuth (af2, xa)) {
				free (xa->data);
				free (xa->number);
				free (xa->name);
				free (xa->address);
				free (xa);
				display_add_error (d);
				return FALSE;
			}
		}
	}

	*authlist = g_slist_append (*authlist, xa);

	return TRUE;
}

/**
 * mdm_auth_secure_display:
 * @d: Pointer to a MdmDisplay struct
 * 
 * Create authentication cookies for local and remote displays.
 *
 * Returns TRUE on success and FALSE on error.
 */

gboolean
mdm_auth_secure_display (MdmDisplay *d)
{
	FILE *af, *af_mdm;
	int closeret;

	if G_UNLIKELY (!d)
		return FALSE;

	umask (022);

	mdm_debug ("mdm_auth_secure_display: Setting up access for %s", d->name);

	g_free (d->authfile);
	d->authfile = NULL;
	g_free (d->authfile_mdm);
	d->authfile_mdm = NULL;

	if (d->server_uid != 0) {
		int authfd;

		/* Note, nested display can't use the MDM_KEY_SERV_AUTHDIR unless 
		 * running as root, which is rare anyway. */

		d->authfile = g_build_filename (mdm_daemon_config_get_value_string (MDM_KEY_USER_AUTHDIR_FALLBACK), ".mdmXXXXXX", NULL);

		umask (077);
		authfd = g_mkstemp (d->authfile);
		umask (022);

		if G_UNLIKELY (authfd == -1) {
			mdm_error ("mdm_auth_secure_display: Could not make new cookie file in %s", mdm_daemon_config_get_value_string (MDM_KEY_USER_AUTHDIR_FALLBACK));
			g_free (d->authfile);
			d->authfile = NULL;
			return FALSE;
		}

		/* Make it owned by the user that nested display is started as */
		fchown (authfd, d->server_uid, -1);

		VE_IGNORE_EINTR (af = fdopen (authfd, "w"));

		if G_UNLIKELY (af == NULL) {
			g_free (d->authfile);
			d->authfile = NULL;
			return FALSE;
		}

		/* Make another authfile since the greeter can't read the server/user
		 * readable file */
		d->authfile_mdm = mdm_make_filename (mdm_daemon_config_get_value_string (MDM_KEY_SERV_AUTHDIR), d->name, ".Xauth");
		af_mdm = mdm_safe_fopen_w (d->authfile_mdm, 0644);

		if G_UNLIKELY (af_mdm == NULL) {
			mdm_error ("mdm_auth_secure_display: Cannot safely open %s", d->authfile_mdm);

			g_free (d->authfile_mdm);
			d->authfile_mdm = NULL;
			g_free (d->authfile);
			d->authfile = NULL;
			VE_IGNORE_EINTR (fclose (af));
			return FALSE;
		}
	} else {
		/* mdm and xserver authfile can be the same, server will run as root */
		d->authfile = mdm_make_filename (mdm_daemon_config_get_value_string (MDM_KEY_SERV_AUTHDIR), d->name, ".Xauth");
		af = mdm_safe_fopen_w (d->authfile, 0644);

		if G_UNLIKELY (af == NULL) {
			mdm_error ("mdm_auth_secure_display: Cannot safely open %s", d->authfile);

			g_free (d->authfile);
			d->authfile = NULL;
			return FALSE;
		}

		af_mdm = NULL;
	}

	/* If this is a local display the struct hasn't changed and we
	 * have to eat up old authentication cookies before baking new
	 * ones... */
	if (SERVER_IS_LOCAL (d) && d->auths) {
		mdm_auth_free_auth_list (d->auths);
		d->auths = NULL;

		g_free (d->cookie);
		d->cookie = NULL;
		g_free (d->bcookie);
		d->bcookie = NULL;
	}

	/* Create new random cookie */
	mdm_cookie_generate (&d->cookie, &d->bcookie);

	/* reget local host if local as it may have changed */
	if (SERVER_IS_LOCAL (d)) {
		char hostname[1024];

		hostname[1023] = '\0';
		if G_LIKELY (gethostname (hostname, 1023) == 0) {
			g_free (d->hostname);
			d->hostname = g_strdup (hostname);
		}
	}

	if ( ! add_auth_entry (d, &(d->auths), af, af_mdm, FamilyWild, NULL, 0))
		return FALSE;

	mdm_debug ("mdm_auth_secure_display: Setting up access");

	VE_IGNORE_EINTR (closeret = fclose (af));
	if G_UNLIKELY (closeret < 0) {
		display_add_error (d);
		return FALSE;
	}
	if (af_mdm != NULL) {
		VE_IGNORE_EINTR (closeret = fclose (af_mdm));
		if G_UNLIKELY (closeret < 0) {
			display_add_error (d);
			return FALSE;
		}
	}
	g_setenv ("XAUTHORITY", MDM_AUTHFILE (d), TRUE);

	if G_UNLIKELY (mdm_daemon_config_get_value_bool (MDM_KEY_DEBUG))
		mdm_debug ("mdm_auth_secure_display: Setting up access for %s - %d entries", 
			   d->name, g_slist_length (d->auths));

	return TRUE;
}

#define SA(__s)	   ((struct sockaddr *) __s)
#define SIN(__s)   ((struct sockaddr_in *) __s)
#define SIN6(__s)  ((struct sockaddr_in6 *) __s)

static gboolean
add_auth_entry_for_addr (MdmDisplay              *d,
			 GSList                 **authlist,
			 struct sockaddr_storage *ss)
{
	const char    *addr;
	int            len;
	unsigned short family;

	switch (ss->ss_family) {
#ifdef ENABLE_IPV6
	case AF_INET6:
		family = FamilyInternetV6;
		addr = (const char *) &SIN6 (ss)->sin6_addr;
		len = sizeof (struct in6_addr);
		break;
#endif
	case AF_INET:
	default:
		family = FamilyInternet;
		addr = (const char *) &SIN (ss)->sin_addr;
		len = sizeof (struct in_addr);
		break;
	}

	return add_auth_entry (d, authlist, NULL, NULL, family, addr, len);
}

static GSList *
get_local_auths (MdmDisplay *d)
{
	gboolean is_local = FALSE;
	const char lo[] = {127,0,0,1};
	guint i;
	const GList *local_addys = NULL;
	gboolean added_lo = FALSE;
	GSList *auths = NULL;

	if G_UNLIKELY (!d)
		return NULL;

	if (SERVER_IS_LOCAL (d)) {
		char hostname[1024];

		/* reget local host if local as it may have changed */
		hostname[1023] = '\0';
		if G_LIKELY (gethostname (hostname, 1023) == 0) {
			g_free (d->hostname);
			d->hostname = g_strdup (hostname);
		}
		if ( ! d->tcp_disallowed)
			local_addys = mdm_address_peek_local_list ();

		is_local = TRUE;
	} else  {
		is_local = FALSE;

		if (mdm_address_is_local (&(d->addr))) {
			is_local = TRUE;
		}

		for (i = 0; ! is_local && i < d->addr_count; i++) {
			if (mdm_address_is_local (&d->addrs[i])) {
				is_local = TRUE;
				break;
			}
		}
	}

	/* Local access also in case the host is very local */
	if (is_local) {
		mdm_debug ("get_local_auths: Setting up socket access");

		if ( ! add_auth_entry (d, &auths, NULL, NULL, FamilyLocal,
				       d->hostname, strlen (d->hostname)))
			goto get_local_auth_error;

		/* local machine but not local if you get my meaning, add
		 * the host gotten by gethostname as well if it's different
		 * since the above is probably localhost */
		if ( ! SERVER_IS_LOCAL (d)) {
			char hostname[1024];

			hostname[1023] = '\0';
			if (gethostname (hostname, 1023) == 0 &&
			    strcmp (hostname, d->hostname) != 0) {
				if ( ! add_auth_entry (d, &auths, NULL, NULL, FamilyLocal,
						       hostname,
						       strlen (hostname)))
					goto get_local_auth_error;
			}
		} else {
			/* local machine, perhaps we haven't added
			 * localhost.localdomain to socket access */
			const char *localhost = "localhost.localdomain";
			if (strcmp (localhost, d->hostname) != 0) {
				if ( ! add_auth_entry (d, &auths, NULL, NULL, FamilyLocal,
						       localhost,
						       strlen (localhost))) {
					goto get_local_auth_error;
				}
			}
		}
	}

	mdm_debug ("get_local_auths: Setting up network access");

	if ( ! SERVER_IS_LOCAL (d)) {
		/* we should write out an entry for d->addr since
		   possibly it is not in d->addrs */

		if (! add_auth_entry_for_addr (d, &auths, &d->addr)) {
			goto get_local_auth_error;
		}

		if (mdm_address_is_loopback (&(d->addr))) {
			added_lo = TRUE;
		}
	}

	/* Network access: Write out an authentication entry for each of
	 * this host's official addresses */
	for (i = 0; i < d->addr_count; i++) {
		struct sockaddr_storage *sa;

		sa = &d->addrs[i];
		if (mdm_address_equal (sa, &d->addr)) {
			continue;
		}

		if (! add_auth_entry_for_addr (d, &auths, sa)) {
			goto get_local_auth_error;
		}

		if (mdm_address_is_loopback (sa)) {
			added_lo = TRUE;
		}
	}

	/* Network access: Write out an authentication entry for each of
	 * this host's local addresses if any */
	for (; local_addys != NULL; local_addys = local_addys->next) {
		struct sockaddr_storage *ia = local_addys->data;

		if (ia == NULL)
			break;

		if (! add_auth_entry_for_addr (d, &auths, ia)) {
			goto get_local_auth_error;
		}

		if (mdm_address_is_loopback (ia)) {
			added_lo = TRUE;
		}
	}

	/* If local server, then add loopback */
	if (SERVER_IS_LOCAL (d) && ! added_lo && ! d->tcp_disallowed) {
		if (! add_auth_entry (d, &auths, NULL, NULL, FamilyInternet,
		      lo, sizeof (struct in_addr))) {
			goto get_local_auth_error;
		}
	}

	if G_UNLIKELY (mdm_daemon_config_get_value_bool (MDM_KEY_DEBUG))
		mdm_debug ("get_local_auths: Setting up access for %s - %d entries",
			   d->name, g_slist_length (auths));

	return auths;

 get_local_auth_error:

	mdm_auth_free_auth_list (auths);

	return NULL;
}

static gboolean
try_open_append (const char *file)
{
	FILE *fp;

	VE_IGNORE_EINTR (fp = fopen (file, "a+"));
	if G_LIKELY (fp != NULL) {
		VE_IGNORE_EINTR (fclose (fp));
		return TRUE;
	} else {
		return FALSE;
	}
}

static gboolean
try_open_read_as_root (const char *file)
{
	int fd;
	uid_t oldeuid = geteuid ();
	uid_t oldegid = getegid ();
	NEVER_FAILS_root_set_euid_egid (0, 0);

	VE_IGNORE_EINTR (fd = open (file, O_RDONLY));
	if G_UNLIKELY (fd < 0) {
		NEVER_FAILS_root_set_euid_egid (oldeuid, oldegid);
		return FALSE;
	} else {
		VE_IGNORE_EINTR (close (fd));
		NEVER_FAILS_root_set_euid_egid (oldeuid, oldegid);
		return TRUE;
	}
}

/**
 * mdm_auth_user_add:
 * @d: Pointer to a MdmDisplay struct
 * @user: Userid of the user whose cookie file to add entries to
 * @homedir: The user's home directory
 *
 * Remove all cookies referring to this display from user's cookie
 * file and append the ones specified in the display's authlist.
 *
 * Returns TRUE on success and FALSE on error.
 */

gboolean
mdm_auth_user_add (MdmDisplay *d, uid_t user, const char *homedir)
{
	char *authdir;
	gint authfd;
	FILE *af;
	GSList *auths = NULL;
	const gchar *userauthdir;
	const gchar *userauthfile;
	gboolean ret = TRUE;
	gboolean automatic_tmp_dir = FALSE;
	gboolean authdir_is_tmp_dir = FALSE;
	gboolean locked;
	gboolean user_auth_exists;
	int closeret;

	if (!d)
		return FALSE;

	if (d->local_auths != NULL) {
		mdm_auth_free_auth_list (d->local_auths);
		d->local_auths = NULL;
	}

	d->local_auths = get_local_auths (d);

	if (d->local_auths == NULL) {
		mdm_error ("Can't make cookies");
		return FALSE;
	}

	mdm_debug ("mdm_auth_user_add: Adding cookie for %d", user);

	userauthdir  = mdm_daemon_config_get_value_string (MDM_KEY_USER_AUTHDIR);
	userauthfile = mdm_daemon_config_get_value_string (MDM_KEY_USER_AUTHFILE);

	/* Determine whether UserAuthDir is specified. Otherwise ~user is used */
	if ( ! ve_string_empty (userauthdir) &&
	     strcmp (userauthdir, "~") != 0) {
		if (strncmp (userauthdir, "~/", 2) == 0) {
			authdir = g_build_filename (homedir, &userauthdir[2], NULL);
		} else {
			authdir = g_strdup (userauthdir);
			automatic_tmp_dir = TRUE;
			authdir_is_tmp_dir = TRUE;
		}
	} else {
		authdir = g_strdup (homedir);
	}

 try_user_add_again:

	locked = FALSE;

	umask (077);

	if (authdir == NULL)
		d->userauth = NULL;
	else
		d->userauth = g_build_filename (authdir, userauthfile, NULL);

	user_auth_exists = (d->userauth != NULL &&
			    g_access (d->userauth, F_OK) == 0);

	/* Find out if the Xauthority file passes the paranoia check */
	/* Note that this is not very efficient, we stat the files over
	   and over, but we don't care, we don't do this too often */
	if (automatic_tmp_dir ||
	    authdir == NULL ||

	    /* first the standard paranoia check (this checks the home dir
	     * too which is useful here) */
	    ! mdm_file_check ("mdm_auth_user_add", user, authdir, userauthfile, 
			      TRUE, FALSE, mdm_daemon_config_get_value_int (MDM_KEY_USER_MAX_FILE),
			      mdm_daemon_config_get_value_int (MDM_KEY_RELAX_PERM)) ||

	    /* now the auth file checking routine */
	    ! mdm_auth_file_check ("mdm_auth_user_add", user, d->userauth, TRUE /* absentok */, NULL) ||

	    /* now see if we can actually append this file */
	    ! try_open_append (d->userauth) ||

	    /* try opening as root, if we can't open as root,
	       then this is a NFS mounted directory with root squashing,
	       and we don't want to write cookies over NFS */
	    (mdm_daemon_config_get_value_bool (MDM_KEY_NEVER_PLACE_COOKIES_ON_NFS) &&
	     ! try_open_read_as_root (d->userauth))) {

		/* if the userauth file didn't exist and we were looking at it,
		   it likely exists now but empty, so just whack it
		   (it may not exist if the file didn't exist and the directory
		   was of wrong permissions, but more likely this is
		   file on NFS dir with root-squashing enabled) */
		if ( ! user_auth_exists && d->userauth != NULL)
			g_remove (d->userauth);

		/* No go. Let's create a fallback file in MDM_KEY_USER_AUTHDIR_FALLBACK (/tmp)
		 * or perhaps userauthfile directory (usually would be /tmp) */
		d->authfb = TRUE;
		g_free (d->userauth);
		if (authdir_is_tmp_dir && authdir != NULL)
			d->userauth = g_build_filename (authdir, ".mdmXXXXXX", NULL);
		else
			d->userauth = g_build_filename (mdm_daemon_config_get_value_string (MDM_KEY_USER_AUTHDIR_FALLBACK), ".mdmXXXXXX", NULL);
		authfd = g_mkstemp (d->userauth);

		if G_UNLIKELY (authfd < 0 && authdir_is_tmp_dir) {
			g_free (d->userauth);
			d->userauth = NULL;

			authdir_is_tmp_dir = FALSE;
			goto try_user_add_again;
		}

		if G_UNLIKELY (authfd < 0) {
			mdm_error ("mdm_auth_user_add: Could not open cookie file %s", d->userauth);
			g_free (d->userauth);
			d->userauth = NULL;

			umask (022);

			g_free (authdir);
			return FALSE;
		}

		d->last_auth_touch = time (NULL);

		VE_IGNORE_EINTR (af = fdopen (authfd, "w"));
	} else { /* User's Xauthority file is ok */
		d->authfb = FALSE;

		/* FIXME: Better implement my own locking. The libXau one is not kosher */
		if G_UNLIKELY (XauLockAuth (d->userauth, 3, 3, 0) != LOCK_SUCCESS) {
			mdm_error ("mdm_auth_user_add: Could not lock cookie file %s", d->userauth);
			g_free (d->userauth);
			d->userauth = NULL;

			automatic_tmp_dir = TRUE;
			goto try_user_add_again;
		}

		locked = TRUE;

		af = mdm_safe_fopen_ap (d->userauth, 0600);
	}

	/* Set to NULL, because can goto try_user_add_again. */
	g_free (authdir);
	authdir = NULL;

	if G_UNLIKELY (af == NULL) {
		/* Really no need to clean up here - this process is a goner anyway */
		mdm_error ("mdm_auth_user_add: Could not open cookie file %s", d->userauth);
		if (locked)
			XauUnlockAuth (d->userauth);
		g_free (d->userauth);
		d->userauth = NULL;

		if ( ! d->authfb) {
			automatic_tmp_dir = TRUE;
			goto try_user_add_again;
		}

		umask (022);
		return FALSE; 
	}

	mdm_debug ("mdm_auth_user_add: Using %s for cookies", d->userauth);

	/* If not a fallback file, nuke any existing cookies for this display */
	if (! d->authfb)
		af = mdm_auth_purge (d, af, FALSE /* remove when empty */);

	/* Append the authlist for this display to the cookie file */
	auths = d->local_auths;

	while (auths) {
		if G_UNLIKELY ( ! XauWriteAuth (af, auths->data)) {
			mdm_error ("mdm_auth_user_add: Could not write cookie");

			if ( ! d->authfb) {
				VE_IGNORE_EINTR (fclose (af));
				if (locked)
					XauUnlockAuth (d->userauth);
				g_free (d->userauth);
				d->userauth = NULL;
				automatic_tmp_dir = TRUE;
				goto try_user_add_again;
			}

			ret = FALSE;
			break;
		}

		auths = auths->next;
	}

	VE_IGNORE_EINTR (closeret = fclose (af));
	if G_UNLIKELY (closeret < 0) {
		mdm_error ("mdm_auth_user_add: Could not write cookie");

		if ( ! d->authfb) {
			if (locked)
				XauUnlockAuth (d->userauth);
			g_free (d->userauth);
			d->userauth = NULL;
			automatic_tmp_dir = TRUE;
			goto try_user_add_again;
		}

		ret = FALSE;
	}

	if (locked)
		XauUnlockAuth (d->userauth);

	mdm_debug ("mdm_auth_user_add: Done");

	umask (022);
	return ret;
}


/**
 * mdm_auth_user_remove:
 * @d: Pointer to a MdmDisplay struct
 * @user: Userid of the user whose cookie file to remove entries from
 * 
 * Remove all cookies referring to this display from user's cookie
 * file.
 */

void 
mdm_auth_user_remove (MdmDisplay *d, uid_t user)
{
	FILE *af;
	gchar *authfile;
	gchar *authdir;

	if G_UNLIKELY (!d || !d->userauth)
		return;

	mdm_debug ("mdm_auth_user_remove: Removing cookie from %s (%d)", d->userauth, d->authfb);

	/* If we are using the fallback cookie location, simply nuke the
	 * cookie file */
	if (d->authfb) {
		VE_IGNORE_EINTR (g_remove (d->userauth));
		g_free (d->userauth);
		d->userauth = NULL;
		return;
	}

	/* if the file doesn't exist, oh well, just ignore this then */
	if G_UNLIKELY (g_access (d->userauth, F_OK) != 0) {
		g_free (d->userauth);
		d->userauth = NULL;
		return;
	}

	authfile = g_path_get_basename (d->userauth);
	authdir = g_path_get_dirname (d->userauth);

	if (ve_string_empty (authfile) ||
	    ve_string_empty (authdir)) {
		g_free (authdir);
		g_free (authfile);
		return;
	}

	/* Now, the cookie file could be owned by a malicious user who
	 * decided to concatenate something like his entire MP3 collection
	 * to it. So we better play it safe... */

	if G_UNLIKELY ( ! mdm_file_check ("mdm_auth_user_remove", user, authdir, authfile, 
					  TRUE, FALSE, mdm_daemon_config_get_value_int (MDM_KEY_USER_MAX_FILE),
					  mdm_daemon_config_get_value_int (MDM_KEY_RELAX_PERM)) ||
			/* be even paranoider with permissions */
			! mdm_auth_file_check ("mdm_auth_user_remove", user, d->userauth, FALSE /* absentok */, NULL)) {
		g_free (authdir);
		g_free (authfile);
		mdm_error ("mdm_auth_user_remove: Ignoring suspiciously looking cookie file %s", d->userauth);

		return; 
	}

	g_free (authdir);
	g_free (authfile);

	/* Lock user's cookie jar and open it for writing */
	if G_UNLIKELY (XauLockAuth (d->userauth, 3, 3, 0) != LOCK_SUCCESS) {
		g_free (d->userauth);
		d->userauth = NULL;
		return;
	}

	af = mdm_safe_fopen_ap (d->userauth, 0600);

	if G_UNLIKELY (af == NULL) {
		XauUnlockAuth (d->userauth);

		mdm_error ("mdm_auth_user_remove: Cannot safely open %s", d->userauth);

		g_free (d->userauth);
		d->userauth = NULL;

		return;
	}

	/* Purge entries for this display from the cookie jar */
	af = mdm_auth_purge (d, af, TRUE /* remove when empty */);

	/* Close the file and unlock it */
	if (af != NULL) {
		/* FIXME: what about out of diskspace errors on errors close */
		errno = 0;
		VE_IGNORE_EINTR (fclose (af));
		if G_UNLIKELY (errno != 0) {
			mdm_error ("Can't write to %s: %s", d->userauth, strerror (errno));
		}
	}

	XauUnlockAuth (d->userauth);

	g_free (d->userauth);
	d->userauth = NULL;
}

static gboolean
memory_same (const char *sa, int lena, const char *sb, int lenb)
{
	if (lena == lenb) {
		if (lena == 0)
			return TRUE;
		/* sanity */
		if G_UNLIKELY (sa == NULL || sb == NULL)
			return FALSE;
		return memcmp (sa, sb, lena) == 0;
	} else {
		return FALSE;
	}
}

static gboolean
auth_same_except_data (Xauth *xa, Xauth *xb)
{
	if (xa->family == xb->family &&
	    memory_same (xa->number, xa->number_length,
			 xb->number, xb->number_length) &&
	    memory_same (xa->name, xa->name_length,
			 xb->name, xb->name_length) &&
	    memory_same (xa->address, xa->address_length,
			 xb->address, xb->address_length))
		return TRUE;
	else
		return FALSE;
}


/**
 * mdm_auth_purge:
 * @d: Pointer to a MdmDisplay struct
 * @af: File handle to a cookie file
 * @remove_when_empty: remove the file when empty
 *
 * Remove all cookies referring to this display a cookie file.
 */

static FILE *
mdm_auth_purge (MdmDisplay *d, FILE *af, gboolean remove_when_empty)
{
	Xauth *xa;
	GSList *keep = NULL, *li;
	int cnt;

	if G_UNLIKELY (!d || !af)
		return af;

	mdm_debug ("mdm_auth_purge: %s", d->name);

	fseek (af, 0L, SEEK_SET);

	/* Read the user's entire Xauth file into memory to avoid
	 * temporary file issues. Then remove any instance of this display
	 * in the cookie jar... */

	cnt = 0;

	while ( (xa = XauReadAuth (af)) != NULL ) {
		GSList *li;
		/* We look at the current auths, but those may
		   have different cookies then what is in the file,
		   so don't compare those, but we wish to purge all
		   the entries that we'd normally write */
		for (li = d->local_auths; li != NULL; li = li->next) {
			Xauth *xb = li->data;
			if (auth_same_except_data (xa, xb)) {
				XauDisposeAuth (xa);
				xa = NULL;
				break;
			}
		}
		if (xa != NULL)
			keep = g_slist_append (keep, xa);

		/* just being ultra anal */
		cnt++;
		if (cnt > 500)
			break;
	}

	VE_IGNORE_EINTR (fclose (af));

	if (remove_when_empty &&
	    keep == NULL) {
		VE_IGNORE_EINTR (g_remove (d->userauth));
		return NULL;
	}

	af = mdm_safe_fopen_w (d->userauth, 0600);

	/* Write out remaining entries */
	for (li = keep; li != NULL; li = li->next) {
		/* FIXME: is this correct, if we can't open
		 * this is quite bad isn't it ... */
		if G_LIKELY (af != NULL)
			XauWriteAuth (af, li->data);
		/* FIXME: what about errors? */
		XauDisposeAuth (li->data);
		li->data = NULL;
	}

	g_slist_free (keep);

	return af;
}

void
mdm_auth_set_local_auth (MdmDisplay *d)
{
	XSetAuthorization ((char *)"MIT-MAGIC-COOKIE-1",
			   (int) strlen ("MIT-MAGIC-COOKIE-1"),
			   (char *)d->bcookie,
			   (int) 16);
}

void
mdm_auth_free_auth_list (GSList *list)
{
	GSList *li;

	for (li = list; li != NULL; li = li->next) {
		XauDisposeAuth ((Xauth *) li->data);
		li->data = NULL;
	}

	g_slist_free (list);
}
