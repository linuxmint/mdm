/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * MDM - The MDM Display Manager
 * Copyright (C) 1999, 2000 Martin K. Petersen <mkp@mkp.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include "config.h"

#include <glib/gi18n.h>
#include <pwd.h>
#include <grp.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>

#if defined (CAN_CLEAR_ADMCHG) && defined (HAVE_USERSEC_H)
#  include <usersec.h>
#endif /* CAN_CLEAR_ADMCHG && HAVE_USERSEC_H */

#ifdef HAVE_CRYPT
#  include <crypt.h>
#endif /* HAVE_CRYPT */

#include "mdm.h"
#include "misc.h"
#include "slave.h"
#include "verify.h"
#include "errorgui.h"
#include "mdm-common.h"
#include "mdm-daemon-config.h"
#include "mdm-socket-protocol.h"
#include "mdm-log.h"

static char *selected_user = NULL;

void
mdm_verify_select_user (const char *user)
{
	g_free (selected_user);
	if (ve_string_empty (user))
		selected_user = NULL;
	else
		selected_user = g_strdup (user);
}

static void
print_cant_auth_errbox (void)
{
	gboolean is_capslock = FALSE;
	const char *basemsg;
	char *msg;
	char *ret;

	ret = mdm_slave_greeter_ctl (MDM_QUERY_CAPSLOCK, "");
	if ( ! ve_string_empty (ret))
		is_capslock = TRUE;
	g_free (ret);

	basemsg = _("\nIncorrect username or password.  "
		    "Letters must be typed in the correct "
		    "case.");
	if (is_capslock) {
		msg = g_strconcat (basemsg, "  ",
				   _("Caps Lock is on."),
				   NULL);
	} else {
		msg = g_strdup (basemsg);
	}
	mdm_slave_greeter_ctl_no_ret (MDM_ERRBOX, msg);
	g_free (msg);
}

/**
 * mdm_verify_user:
 * @username: Name of user or NULL if we should ask
 * @allow_retry: boolean.  Not used by verify-crypt.  If this code
 *               allowed the user to retry, this boolean would specify
 *               whether to enable this feature.  We only want this
 *               feature to work for normal login, not for asking for
 *               root password to call the configurator.
 *
 * Provides a communication layer between the operating system's
 * authentication functions and the mdmgreeter.
 *
 * Returns the user's login on success and NULL on failure.
 */

gchar *
mdm_verify_user (MdmDisplay *d,
		 const char *username,
		 gboolean allow_retry)
{
	gchar *login, *passwd, *ppasswd;
	struct passwd *pwent;
#if defined (HAVE_PASSWDEXPIRED) && defined (HAVE_CHPASS)	\
	|| defined (HAVE_LOGINRESTRICTIONS)
	gchar *message = NULL;
#endif
#if defined (HAVE_PASSWDEXPIRED) && defined (HAVE_CHPASS)
	gchar *info_msg = NULL, *response = NULL;
	gint reEnter, ret;
#endif

	if (d->attached && d->timed_login_ok)
		mdm_slave_greeter_ctl_no_ret (MDM_STARTTIMER, "");

	if (username == NULL) {
	authenticate_again:
		/* Ask for the user's login */
		mdm_verify_select_user (NULL);
		mdm_slave_greeter_ctl_no_ret (MDM_MSG, _("Please enter your username"));
		login = mdm_slave_greeter_ctl (MDM_PROMPT, _("Username:"));
		if (login == NULL ||
		    mdm_slave_greeter_check_interruption ()) {
			if ( ! ve_string_empty (selected_user)) {
				/* user selected */
				g_free (login);
				login = selected_user;
				selected_user = NULL;
			} else {
				/* some other interruption */
				if (d->attached)
					mdm_slave_greeter_ctl_no_ret (MDM_STOPTIMER, "");
				g_free (login);
				return NULL;
			}
		}
		mdm_slave_greeter_ctl_no_ret (MDM_MSG, "");

		if (mdm_daemon_config_get_value_bool (MDM_KEY_DISPLAY_LAST_LOGIN)) {
			char *info = mdm_get_last_info (login);
			mdm_slave_greeter_ctl_no_ret (MDM_ERRBOX, info);
			g_free (info);
		}
	} else {
		login = g_strdup (username);
	}
	mdm_slave_greeter_ctl_no_ret (MDM_SETLOGIN, login);

	pwent = getpwnam (login);

	ppasswd = (pwent == NULL) ? NULL : g_strdup (pwent->pw_passwd);

	/* Request the user's password */
	if (pwent != NULL &&
	    ve_string_empty (ppasswd)) {
		/* eeek a passwordless account */
		passwd = g_strdup ("");
	} else {
		passwd = mdm_slave_greeter_ctl (MDM_NOECHO, _("Password:"));
		if (passwd == NULL)
			passwd = g_strdup ("");
		if (mdm_slave_greeter_check_interruption ()) {
			if (d->attached)
				mdm_slave_greeter_ctl_no_ret (MDM_STOPTIMER, "");
			g_free (login);
			g_free (passwd);
			g_free (ppasswd);
			return NULL;
		}
	}

	if (d->attached)
		mdm_slave_greeter_ctl_no_ret (MDM_STOPTIMER, "");

	if (pwent == NULL) {
		mdm_sleep_no_signal (mdm_daemon_config_get_value_int (MDM_KEY_RETRY_DELAY));
		mdm_debug ("Couldn't authenticate user");

		print_cant_auth_errbox ();

		g_free (login);
		g_free (passwd);
		g_free (ppasswd);
		return NULL;
	}

	/* Check whether password is valid */
	if (ppasswd == NULL || (ppasswd[0] != '\0' &&
				strcmp (crypt (passwd, ppasswd), ppasswd) != 0)) {
		mdm_sleep_no_signal (mdm_daemon_config_get_value_int (MDM_KEY_RETRY_DELAY));
		mdm_debug ("Couldn't authenticate user");

		print_cant_auth_errbox ();

		g_free (login);
		g_free (passwd);
		g_free (ppasswd);
		return NULL;
	}

	if (( ! mdm_daemon_config_get_value_bool (MDM_KEY_ALLOW_ROOT) ||
	    ( ! d->attached)) && pwent->pw_uid == 0) {

		mdm_debug ("Root login disallowed on display '%s'", d->name);
		mdm_slave_greeter_ctl_no_ret (MDM_ERRBOX,
					      _("The system administrator "
						"is not allowed to login "
						"from this screen"));
		/*mdm_slave_greeter_ctl_no_ret (MDM_ERRDLG,
		  _("Root login disallowed"));*/
		g_free (login);
		g_free (passwd);
		g_free (ppasswd);
		return NULL;
	}

#ifdef HAVE_LOGINRESTRICTIONS

	/* Check with the 'loginrestrictions' function
	   if the user has been disallowed */
	if (loginrestrictions (login, 0, NULL, &message) != 0) {
		mdm_debug ("User not allowed to log in");
		mdm_slave_greeter_ctl_no_ret (MDM_ERRBOX,
					      _("\nThe system administrator "
						"has disabled your "
						"account."));
		g_free (login);
		g_free (passwd);
		g_free (ppasswd);
		if (message != NULL)
			free (message);
		return NULL;
	}

	if (message != NULL)
		free (message);
	message = NULL;

#else /* ! HAVE_LOGINRESTRICTIONS */

	/* check for the standard method of disallowing users */
	if (pwent->pw_shell != NULL &&
	    (strcmp (pwent->pw_shell, NOLOGIN) == 0 ||
	     strcmp (pwent->pw_shell, "/bin/true") == 0 ||
	     strcmp (pwent->pw_shell, "/bin/false") == 0)) {
		mdm_debug ("User not allowed to log in");
		mdm_slave_greeter_ctl_no_ret (MDM_ERRBOX,
					      _("\nThe system administrator "
						"has disabled your "
						"account."));
		/*mdm_slave_greeter_ctl_no_ret (MDM_ERRDLG,
		  _("Login disabled"));*/
		g_free (login);
		g_free (passwd);
		g_free (ppasswd);
		return NULL;
	}

#endif /* HAVE_LOGINRESTRICTIONS */

	g_free (passwd);
	g_free (ppasswd);

	if ( ! mdm_slave_check_user_wants_to_log_in (login)) {
		g_free (login);
		login = NULL;
		goto authenticate_again;
	}

	if ( ! mdm_setup_gids (login, pwent->pw_gid)) {
		mdm_debug ("Cannot set user group");
		mdm_slave_greeter_ctl_no_ret (MDM_ERRBOX,
					      _("\nCannot set your user group; "
						"you will not be able to log in. "
						"Please contact your system administrator."));
		g_free (login);
		return NULL;
	}

#if defined (HAVE_PASSWDEXPIRED) && defined (HAVE_CHPASS)

	switch (passwdexpired (login, &info_msg)) {
	case 1 :
		mdm_debug ("User password has expired");
		mdm_errorgui_error_box (d, GTK_MESSAGE_ERROR,
					_("You are required to change your password.\n"
					  "Please choose a new one."));
		g_free (info_msg);

		do {
			ret = chpass (login, response, &reEnter, &message);
			g_free (response);

			if (ret != 1) {
				if (ret != 0) {
					mdm_slave_greeter_ctl_no_ret (MDM_ERRBOX,
								      _("\nCannot change your password; "
									"you will not be able to log in. "
									"Please try again later or contact "
									"your system administrator."));
				} else if ((reEnter != 0) && (message)) {
					response = mdm_slave_greeter_ctl (MDM_NOECHO, message);
					if (response == NULL)
						response = g_strdup ("");
				}
			}

			g_free (message);
			message = NULL;

		} while ( ((reEnter != 0) && (ret == 0))
			  || (ret ==1) );

		g_free (response);
		g_free (message);

		if ((ret != 0) || (reEnter != 0)) {
			return NULL;
		}

#if defined (CAN_CLEAR_ADMCHG)
		/* The password is changed by root, clear the ADM_CHG
		   flag in the passwd file */
		ret = setpwdb (S_READ | S_WRITE);
		if (!ret) {
			upwd = getuserpw (login);
			if (upwd == NULL) {
				ret = -1;
			} else {
				upwd->upw_flags &= ~PW_ADMCHG;
				ret = putuserpw (upwd);
				if (!ret) {
					ret = endpwdb ();
				}
			}
		}

		if (ret) {
			mdm_errorgui_error_box (d, GTK_MESSAGE_WARNING,
						_("Your password has been changed but "
						  "you may have to change it again. "
						  "Please try again later or contact "
						  "your system administrator."));
		}

#else /* !CAN_CLEAR_ADMCHG */
		mdm_errorgui_error_box (d, GTK_MESSAGE_WARNING,
					_("Your password has been changed but you "
					  "may have to change it again. Please try again "
					  "later or contact your system administrator."));

#endif /* CAN_CLEAR_ADMCHG */

		break;

	case 2 :
		mdm_debug ("User password has expired");
		mdm_errorgui_error_box (d, GTK_MESSAGE_ERROR,
					_("Your password has expired.\n"
					  "Only a system administrator can now change it"));
		g_free (info_msg);
		return NULL;
		break;

	case -1 :
		mdm_debug ("Internal error on passwdexpired");
		mdm_errorgui_error_box (d, GTK_MESSAGE_ERROR,
					_("An internal error occurred. You will not be able to log in.\n"
					  "Please try again later or contact your system administrator."));
		g_free (info_msg);
		return NULL;
		break;

	default :
		g_free (info_msg);
		break;
	}

#endif /* HAVE_PASSWDEXPIRED && HAVE_CHPASS */

	return login;
}

/**
 * mdm_verify_setup_user:
 * @login: The name of the user
 *
 * This is used for auto loging in.  This just sets up the login
 * session for this user
 */

gboolean
mdm_verify_setup_user (MdmDisplay *d,
		       const gchar *login,
		       char **new_login)
{
	struct passwd *pwent;

	*new_login = NULL;

	pwent = getpwnam (login);
	if (pwent == NULL) {
		mdm_debug ("Cannot get passwd structure");
		return FALSE;
	}

	if ( ! mdm_setup_gids (login, pwent->pw_gid)) {
		mdm_debug ("Cannot set user group");
		mdm_errorgui_error_box (d,
					GTK_MESSAGE_ERROR,
					_("\nCannot set your user group; "
					  "you will not be able to log in. "
					  "Please contact your system administrator."));
		return FALSE;
	}

	return TRUE;
}

/**
 * mdm_verify_cleanup:
 *
 * Unregister the user's session
 */

void
mdm_verify_cleanup (MdmDisplay *d)
{
	gid_t groups[1] = { 0 };

	/* Clear the group setup */
	setgid (0);
	/* this will get rid of any suplementary groups etc... */
	setgroups (1, groups);
}

/* used in pam */
gboolean
mdm_verify_setup_env (MdmDisplay *d)
{
	return TRUE;
}
