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

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <grp.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <security/pam_appl.h>
#include <pwd.h>

#define MDM_PAM_QUAL const
 
#include <glib/gi18n.h>

#include "mdm.h"
#include "misc.h"
#include "slave.h"
#include "verify.h"
#include "errorgui.h"
#include "getvt.h"

#include "mdm-common.h"
#include "mdm-log.h"
#include "mdm-daemon-config.h"

#include "mdm-socket-protocol.h"

#define  AU_FAILED 0
#define  AU_SUCCESS 1
#ifdef HAVE_LIBAUDIT
#include <libaudit.h>
#else
#define log_to_audit_system(l,h,d,s)	do { ; } while (0)
#endif

/* Evil, but this way these things are passed to the child session */
static pam_handle_t *pamh = NULL;

static MdmDisplay *cur_mdm_disp = NULL;

/* Hack. Used so user does not need to select username in face
 * browser again if pw was wrong. Not used if username was typed
 * manually */
static char* prev_user;
static unsigned auth_retries;

static gboolean do_we_need_to_preset_the_username = TRUE;
static gboolean did_we_ask_for_password = FALSE;

static char *selected_user = NULL;

static gboolean opened_session = FALSE;
static gboolean did_setcred    = FALSE;

extern char *mdm_ack_question_response;

gboolean mdm_verify_check_selectable_user (const char * user) {		
	
	// Return if the username is null or empty
	if (user == NULL || strcmp (user, "") == 0) {
		mdm_debug("mdm_verify_check_selectable_user: invalid username.");
		return FALSE;
	}

	// Return if the username is "oem"
	if (strcmp (user, "oem") == 0) {
		mdm_debug("mdm_verify_check_selectable_user: username is 'oem'.");
		return FALSE;
	}

	char * home_dir = g_strdup_printf("/home/%s", user);
	char * accounts_service = g_strdup_printf("/var/lib/AccountsService/users/%s", user);

	// Return if the user doesn't exist (check /home and AccountsService)
	if ( !(g_file_test(home_dir, G_FILE_TEST_EXISTS) || g_file_test(accounts_service, G_FILE_TEST_EXISTS)) ) {
		mdm_debug("mdm_verify_check_selectable_user: user '%s' doesn't exist.", user);
		g_free (home_dir);
		g_free (accounts_service);
		return FALSE;
	}

	// Return if the user belongs to the nopasswdlogin group
	char result[255];
	char * command = g_strdup_printf("id '%s' 2>/dev/null | grep nopasswdlogin | wc -l", user);
	FILE *fp = popen(command, "r");
	fscanf(fp, "%s", result);
	pclose(fp);
	if (strcmp(result, "0") != 0) {
		mdm_debug("mdm_verify_check_selectable_user: user '%s' is part of the nopasswdlogin group.", user);
		g_free (home_dir);
		g_free (accounts_service);
		g_free (command);
		return FALSE;
	}

	g_free (home_dir);
	g_free (accounts_service);
	g_free (command);
	return TRUE;
}

void mdm_verify_set_user_settings (const char *user) {
	char * session;
	char * language;

	if (mdm_verify_check_selectable_user(user)) {
		mdm_debug("mdm_verify_set_user_settings: Checking settings for '%s'", user);
		char * home_dir = g_strdup_printf("/home/%s", user);
		if ( !(g_file_test(home_dir, G_FILE_TEST_EXISTS))) {
			mdm_debug("mdm_verify_set_user_settings: user '%s' doesn't exist.", user);
			g_free (home_dir);
			return;
		}
		mdm_daemon_config_get_user_session_lang (&session, &language, home_dir);
		if (!ve_string_empty(session)) {
			mdm_debug("mdm_verify_set_user_settings: Found session '%s'.", session);
			mdm_slave_greeter_ctl_no_ret (MDM_SETSESS, session);
		}
		else {
			mdm_debug("mdm_verify_set_user_settings: No session found, setting to default '%s'.", "default");
			mdm_slave_greeter_ctl_no_ret (MDM_SETSESS, "default"); // Do not translate "default" here, it's a value
		}
		if (!ve_string_empty(language)) {
			mdm_debug("mdm_verify_set_user_settings: Found language '%s'.", language);
			mdm_slave_greeter_ctl_no_ret (MDM_SETLANG, language);
		}
		else {
			const char * mdmlang = g_getenv ("LANG");
			if (mdmlang) {
				mdm_debug("mdm_verify_set_user_settings: No language found, setting to default '%s'.", mdmlang);
				mdm_slave_greeter_ctl_no_ret (MDM_SETLANG, mdmlang);
			}
		}

		g_free (home_dir);
	}
}

void
mdm_verify_select_user (const char *user)
{
	g_free (selected_user);
	if (ve_string_empty (user))
		selected_user = NULL;
	else {
		selected_user = g_strdup (user);
		mdm_verify_set_user_settings (user);		
	}
}

static const char *
perhaps_translate_message (const char *msg)
{
	char *s;
	const char *ret;
	static GHashTable *hash = NULL;
	static char *locale = NULL;

	/* if locale changes out from under us then rebuild hash table
	 */
	if ((locale != NULL) &&
	    (strcmp (locale, setlocale (LC_ALL, NULL)) != 0)) {
		g_assert (hash != NULL);
		g_hash_table_destroy (hash);
		hash = NULL;
	}

	if (hash == NULL) {
		g_free (locale);
		locale = g_strdup (setlocale (LC_ALL, NULL));

		/* Here we come with some fairly standard messages so that
		   we have as much as possible translated.  Should really be
		   translated in pam I suppose.  This way we can "change"
		   some of these messages to be more sane. */
		hash = g_hash_table_new (g_str_hash, g_str_equal);
		/* login: is whacked always translate to Username: */
		g_hash_table_insert (hash, "login:", _("Username:"));
		g_hash_table_insert (hash, "Username:", _("Username:"));
		g_hash_table_insert (hash, "username:", _("Username:"));
		g_hash_table_insert (hash, "Password:", _("Password:"));
		g_hash_table_insert (hash, "password:", _("Password:"));
		g_hash_table_insert (hash, "You are required to change your password immediately (password aged)", _("You are required to change your password immediately (password aged)"));
		g_hash_table_insert (hash, "You are required to change your password immediately (root enforced)", _("You are required to change your password immediately (root enforced)"));
		g_hash_table_insert (hash, "Your account has expired; please contact your system administrator", _("Your account has expired; please contact your system administrator"));
		g_hash_table_insert (hash, "No password supplied", _("No password supplied"));
		g_hash_table_insert (hash, "Password unchanged", _("Password unchanged"));
		g_hash_table_insert (hash, "Can not get username", _("Can not get username"));
		g_hash_table_insert (hash, "Retype new UNIX password:", _("Retype new UNIX password:"));
		g_hash_table_insert (hash, "Enter new UNIX password:", _("Enter new UNIX password:"));
		g_hash_table_insert (hash, "(current) UNIX password:", _("(current) UNIX password:"));
		g_hash_table_insert (hash, "Error while changing NIS password.", _("Error while changing NIS password."));
		g_hash_table_insert (hash, "You must choose a longer password", _("You must choose a longer password"));
		g_hash_table_insert (hash, "Password has been already used. Choose another.", _("Password has been already used. Choose another."));
		g_hash_table_insert (hash, "You must wait longer to change your password", _("You must wait longer to change your password"));
		g_hash_table_insert (hash, "Sorry, passwords do not match", _("Sorry, passwords do not match"));
		/* FIXME: what about messages which have some variables in them, perhaps try to do those as well */
	}
	s = g_strstrip (g_strdup (msg));
	ret = g_hash_table_lookup (hash, s);
	g_free (s);
	if (ret != NULL)
		return ret;
	else
		return msg;
}

/* Internal PAM conversation function. Interfaces between the PAM
 * authentication system and the actual greeter program */

static int
mdm_verify_pam_conv (int num_msg,
		     MDM_PAM_QUAL struct pam_message **msg,
		     struct pam_response **resp,
		     void *appdata_ptr)
{
	int replies = 0;
	int i;
	char *s = NULL;
	struct pam_response *reply = NULL;
	MDM_PAM_QUAL void *p;
	const char *login;

	if (pamh == NULL)
		return PAM_CONV_ERR;

	/* Should never happen unless PAM is on crack and keeps asking questions
	   after we told it to go away.  So tell it to go away again and
	   maybe it will listen */
	/* well, it actually happens if there are multiple pam modules
	 * with conversations */
	if ( ! mdm_slave_action_pending () || selected_user)
		return PAM_CONV_ERR;

	reply = malloc (sizeof (struct pam_response) * num_msg);

	if (reply == NULL)
		return PAM_CONV_ERR;

	memset (reply, 0, sizeof (struct pam_response) * num_msg);

	/* Here we set the login if it wasn't already set,
	 * this is kind of anal, but this way we guarantee that
	 * the greeter always is up to date on the login */
	if (pam_get_item (pamh, PAM_USER, &p) == PAM_SUCCESS) {
		login = (const char *)p;
		mdm_slave_greeter_ctl_no_ret (MDM_SETLOGIN, login);
	}

	/* Workaround to avoid mdm messages being logged as PAM_pwdb */
	mdm_log_shutdown ();
	mdm_log_init ();

	for (replies = 0; replies < num_msg; replies++) {
		const char *m = (*msg)[replies].msg;
		m = perhaps_translate_message (m);

		switch ((*msg)[replies].msg_style) {

			/* PAM requested textual input with echo on */
		case PAM_PROMPT_ECHO_ON:
			if (strcmp (m, _("Username:")) == 0) {
				if (ve_string_empty (selected_user)) {
					/* this is an evil hack, but really there is no way we'll
					   know this is a username prompt.  However we SHOULD NOT
					   rely on this working.  The pam modules can set their
					   prompt to whatever they wish to */
					mdm_slave_greeter_ctl_no_ret
						(MDM_MSG, _("Please enter your username"));
					s = mdm_slave_greeter_ctl (MDM_PROMPT, m);
					/* this will clear the message */
					mdm_slave_greeter_ctl_no_ret (MDM_MSG, "");
				}
			} else {
				s = mdm_slave_greeter_ctl (MDM_PROMPT, m);
			}

			if (mdm_slave_greeter_check_interruption ()) {
				g_free (s);
				for (i = 0; i < replies; i++)
					if (reply[replies].resp != NULL)
						free (reply[replies].resp);
				free (reply);
				return PAM_CONV_ERR;
			}

			reply[replies].resp_retcode = PAM_SUCCESS;
			reply[replies].resp = strdup (ve_sure_string (s));
			g_free (s);
			break;

		case PAM_PROMPT_ECHO_OFF:
			if (strcmp (m, _("Password:")) == 0)
				did_we_ask_for_password = TRUE;
			/* PAM requested textual input with echo off */
			s = mdm_slave_greeter_ctl (MDM_NOECHO, m);
			if (mdm_slave_greeter_check_interruption ()) {
				g_free (s);
				for (i = 0; i < replies; i++)
					if (reply[replies].resp != NULL)
						free (reply[replies].resp);
				free (reply);
				return PAM_CONV_ERR;
			}
			reply[replies].resp_retcode = PAM_SUCCESS;
			reply[replies].resp = strdup (ve_sure_string (s));
			g_free (s);
			break;

		case PAM_ERROR_MSG:
			/* PAM sent a message that should displayed to the user */
			mdm_slave_greeter_ctl_no_ret (MDM_ERRDLG, m);
			reply[replies].resp_retcode = PAM_SUCCESS;
			reply[replies].resp = NULL;
			break;

		case PAM_TEXT_INFO:
			/* PAM sent a message that should displayed to the user */
			mdm_slave_greeter_ctl_no_ret (MDM_MSG, m);
			reply[replies].resp_retcode = PAM_SUCCESS;
			reply[replies].resp = NULL;
			break;

		default:
			/* PAM has been smoking serious crack */
			for (i = 0; i < replies; i++)
				if (reply[replies].resp != NULL)
					free (reply[replies].resp);
			free (reply);
			return PAM_CONV_ERR;
		}

	}

	*resp = reply;
	return PAM_SUCCESS;
}

static struct pam_conv pamc = {
	&mdm_verify_pam_conv,
	NULL
};

/* Extra message to give on queries */
static char *extra_standalone_message = NULL;

static int
mdm_verify_standalone_pam_conv (int num_msg,
				MDM_PAM_QUAL struct pam_message **msg,
				struct pam_response **resp,
				void *appdata_ptr)
{
	int replies = 0;
	int i;
        char *text;
        char *question_msg;
	struct pam_response *reply = NULL;

	if (pamh == NULL)
		return PAM_CONV_ERR;

	reply = malloc (sizeof (struct pam_response) * num_msg);

	if (reply == NULL)
		return PAM_CONV_ERR;

	memset (reply, 0, sizeof (struct pam_response) * num_msg);

	for (replies = 0; replies < num_msg; replies++) {
		const char *m = (*msg)[replies].msg;
		m = perhaps_translate_message (m);
		switch ((*msg)[replies].msg_style) {

		case PAM_PROMPT_ECHO_ON:
			if (extra_standalone_message != NULL)
				text = g_strdup_printf
					("%s%s", extra_standalone_message,
					 m);
			else
				text = g_strdup (m);

			/* PAM requested textual input with echo on */
			question_msg = g_strdup_printf ("question_msg=%s$$echo=%d", text, TRUE);

			mdm_slave_send_string (MDM_SOP_SHOW_QUESTION_DIALOG, question_msg);

			g_free (question_msg);
			g_free (text);

			reply[replies].resp_retcode = PAM_SUCCESS;
			if (mdm_ack_question_response) {
				reply[replies].resp = strdup (ve_sure_string (mdm_ack_question_response));
				g_free (mdm_ack_question_response);
				mdm_ack_question_response = NULL;
			} else
				reply[replies].resp = NULL;

			break;

		case PAM_PROMPT_ECHO_OFF:
			if (extra_standalone_message != NULL)
				text = g_strdup_printf
					("%s%s", extra_standalone_message,
					 m);
			else
				text = g_strdup (m);

			/* PAM requested textual input with echo off */
			question_msg = g_strdup_printf ("question_msg=%s$$echo=%d", text, FALSE);

			mdm_slave_send_string (MDM_SOP_SHOW_QUESTION_DIALOG, question_msg);

			g_free (question_msg);
			g_free (text);

			reply[replies].resp_retcode = PAM_SUCCESS;
			if (mdm_ack_question_response) {
				reply[replies].resp = strdup (ve_sure_string (mdm_ack_question_response));
				g_free (mdm_ack_question_response);
				mdm_ack_question_response = NULL;
			} else
				reply[replies].resp = NULL;

			break;

		case PAM_ERROR_MSG:
			/* PAM sent a message that should displayed to the user */
			mdm_errorgui_error_box (cur_mdm_disp,
						GTK_MESSAGE_ERROR,
						m);
			reply[replies].resp_retcode = PAM_SUCCESS;
			reply[replies].resp = NULL;
			break;

		case PAM_TEXT_INFO:
			/* PAM sent a message that should displayed to the user */
			mdm_errorgui_error_box (cur_mdm_disp,
						GTK_MESSAGE_INFO,
						m);
			reply[replies].resp_retcode = PAM_SUCCESS;
			reply[replies].resp = NULL;
			break;

		default:
			/* PAM has been smoking serious crack */
			for (i = 0; i < replies; i++)
				if (reply[replies].resp != NULL)
					free (reply[replies].resp);
			free (reply);
			return PAM_CONV_ERR;
		}

	}

	*resp = reply;
	return PAM_SUCCESS;
}

/* Preselects the last logged in user */
static void mdm_preselect_user (int *pamerr) {
	
	// Return if user preselection isn't enabled
	if (!mdm_daemon_config_get_value_bool (MDM_KEY_SELECT_LAST_LOGIN)) {
		return;
	}
	
	// Return if we're using automatic or timed login
	if (mdm_daemon_config_get_value_bool (MDM_KEY_AUTOMATIC_LOGIN_ENABLE) || mdm_daemon_config_get_value_bool (MDM_KEY_TIMED_LOGIN_ENABLE)) {
		mdm_debug("mdm_preselect_user: Automatic/Timed login detected, not presetting user.");
		return;
	}

        // Return if the user list is disabled (relevant to mdmlogin)
        if (!mdm_daemon_config_get_value_bool (MDM_KEY_BROWSER)) {
		mdm_debug("mdm_preselect_user: User list disabled, not presetting user.");
                return;
        }

	// Find the name of the last logged in user
	char last_username[255];
	FILE *fp = popen("last -w | grep tty | head -1 | awk {'print $1;'}", "r");
	fscanf(fp, "%s", last_username);
	pclose(fp);

	// If for any reason the user isn't selectable, don't preset.
	if (!mdm_verify_check_selectable_user(last_username)) {
		return;
	}	

	// Preset the user
	mdm_debug("mdm_verify_user: presetting user to '%s'", last_username);
	if ((*pamerr = pam_set_item (pamh, PAM_USER, last_username)) != PAM_SUCCESS) {		
		if (mdm_slave_action_pending ()) {
			mdm_error ("Can't set PAM_USER='%s'", last_username);
		}
	}

	mdm_verify_set_user_settings (last_username);

}

static struct pam_conv standalone_pamc = {
	&mdm_verify_standalone_pam_conv,
	NULL
};

/* Creates a pam handle for the auto login */
static gboolean
create_pamh (MdmDisplay *d,
	     const char *service,
	     const char *login,
	     struct pam_conv *conv,
	     const char *display,
	     int *pamerr)
{

	if (display == NULL) {
		mdm_error ("Cannot setup pam handle with null display");
		return FALSE;
	}

	if (pamh != NULL) {
		mdm_error ("create_pamh: Stale pamh around, cleaning up");
		pam_end (pamh, PAM_SUCCESS);
	}
	/* init things */
	pamh = NULL;
	opened_session = FALSE;
	did_setcred = FALSE;

	/* Initialize a PAM session for the user */
	if ((*pamerr = pam_start (service, login, conv, &pamh)) != PAM_SUCCESS) {
		pamh = NULL; /* be anal */
		if (mdm_slave_action_pending ())
			mdm_error ("Unable to establish service %s: %s\n", service, pam_strerror (NULL, *pamerr));
		return FALSE;
	}

	/* Inform PAM of the user's tty */
		if ((*pamerr = pam_set_item (pamh, PAM_TTY, display)) != PAM_SUCCESS) {
			if (mdm_slave_action_pending ())
				mdm_error ("Can't set PAM_TTY=%s", display);
			return FALSE;
		}

	if ( ! d->attached) {
		/* Only set RHOST if host is remote */
		/* From the host of the display */
		if ((*pamerr = pam_set_item (pamh, PAM_RHOST,
					     d->hostname)) != PAM_SUCCESS) {
			if (mdm_slave_action_pending ())
				mdm_error ("Can't set PAM_RHOST=%s", d->hostname);
			return FALSE;
		}
	}

	// Preselect the previous user
	if (do_we_need_to_preset_the_username) {		
		do_we_need_to_preset_the_username = FALSE;
		mdm_preselect_user(pamerr);
	}

	return TRUE;
}

/**
 * log_to_audit_system:
 * @login: Name of user
 * @hostname: Name of host machine
 * @tty: Name of display 
 * @success: 1 for success, 0 for failure
 *
 * Logs the success or failure of the login attempt with the linux kernel
 * audit system. The intent is to capture failed events where the user
 * fails authentication or otherwise is not permitted to login. There are
 * many other places where pam could potentially fail and cause login to 
 * fail, but these are system failures rather than the signs of an account
 * being hacked.
 *
 * Returns nothing.
 */

#ifdef HAVE_LIBAUDIT
static void 
log_to_audit_system(const char *login,
		    const char *hostname,
		    const char *tty,
		    gboolean success)
{
	struct passwd *pw;
	char buf[64];
	int audit_fd;

	audit_fd = audit_open ();
	if (login)
		pw = getpwnam (login);
	else {
		login = "unknown";
		pw = NULL;
	}
	if (pw) {
		snprintf (buf, sizeof (buf), "uid=%d", pw->pw_uid);
		audit_log_user_message (audit_fd, AUDIT_USER_LOGIN,
				        buf, hostname, NULL, tty, (int)success);
	} else {
		snprintf (buf, sizeof (buf), "acct=%s", login);
		audit_log_user_message (audit_fd, AUDIT_USER_LOGIN,
				        buf, hostname, NULL, tty, (int)success);
	}
	close (audit_fd);
}
#endif

/**
 * mdm_verify_user:
 * @username: Name of user or NULL if we should ask
 * @allow_retry: boolean if we should allow retry logic to be enabled.
 *               We only want this to work for normal login, not for
 *               asking for the root password to cal the configurator.
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
	gint pamerr = 0;
	struct passwd *pwent = NULL;
	char *login, *passreq;
	char *pam_stack = NULL;
	MDM_PAM_QUAL void *p;
	int null_tok = 0;
	gboolean credentials_set = FALSE;
	gboolean error_msg_given = FALSE;
	gboolean started_timer   = FALSE;

    verify_user_again:

	pamerr = 0;
	login = NULL;
	error_msg_given = FALSE;
	credentials_set = FALSE;
	started_timer = FALSE;
	null_tok = 0;

	/* Don't start a timed login if we've already entered a username */
	if (username != NULL) {
		login = g_strdup (username);
		mdm_slave_greeter_ctl_no_ret (MDM_SETLOGIN, login);
	} else {
		/* start the timer for timed logins */
		if ( ! ve_string_empty (mdm_daemon_config_get_value_string (MDM_KEY_TIMED_LOGIN)) &&
		    d->timed_login_ok && (d->attached)) {
			mdm_slave_greeter_ctl_no_ret (MDM_STARTTIMER, "");
			started_timer = TRUE;
		}
	}

	cur_mdm_disp = d;

 authenticate_again:

	if (prev_user && !login) {
		login = g_strdup (prev_user);
	} else if (login && !prev_user) {
		prev_user = g_strdup (login);
		auth_retries = 0;
	} else if (login && prev_user && strcmp (login, prev_user)) {
		g_free (prev_user);
		prev_user = g_strdup (login);
		auth_retries = 0;
	}

	/*
	 * Initialize a PAM session for the user...
	 * Get value per-display so different displays can use different
	 * PAM Stacks, in case one display should use a different
	 * authentication mechanism than another display.
	 */
	pam_stack = mdm_daemon_config_get_value_string_per_display (MDM_KEY_PAM_STACK,
		(char *)d->name);

	if ( ! create_pamh (d, pam_stack, login, &pamc, d->name, &pamerr)) {
		if (started_timer)
			mdm_slave_greeter_ctl_no_ret (MDM_STOPTIMER, "");
		g_free (pam_stack);
		goto pamerr;
	}

	g_free (pam_stack);

	/*
	 * have to unset login otherwise there is no chance to ever enter
	 * a different user
	 */
	g_free (login);
	login = NULL;

	pam_set_item (pamh, PAM_USER_PROMPT, _("Username:"));

#if 0
	/* FIXME: this makes things wait at the wrong places! such as
	   when running the configurator.  We wish to ourselves cancel logins
	   without a delay, so ... evil */
#ifdef PAM_FAIL_DELAY
	pam_fail_delay (pamh, mdm_daemon_config_get_value_int (MDM_KEY_RETRY_DELAY) * 1000000);
#endif /* PAM_FAIL_DELAY */
#endif

	passreq = mdm_read_default ("PASSREQ=");

	if (mdm_daemon_config_get_value_bool (MDM_KEY_PASSWORD_REQUIRED) ||
            ((passreq != NULL) && g_ascii_strcasecmp (passreq, "YES") == 0))
		null_tok |= PAM_DISALLOW_NULL_AUTHTOK;

	mdm_verify_select_user (NULL);

	/* Start authentication session */
	did_we_ask_for_password = FALSE;
	if ((pamerr = pam_authenticate (pamh, null_tok)) != PAM_SUCCESS) {
		if ( ! ve_string_empty (selected_user)) {
			pam_handle_t *tmp_pamh;

			/* Face browser was used to select a user,
			   just completely rewhack everything since it
			   seems various PAM implementations are
			   having goats with just setting PAM_USER
			   and trying to pam_authenticate again */

			g_free (login);
			login = selected_user;
			selected_user = NULL;

			mdm_sigterm_block_push ();
			mdm_sigchld_block_push ();
			tmp_pamh = pamh;
			pamh     = NULL;
			mdm_sigchld_block_pop ();
			mdm_sigterm_block_pop ();

			/* FIXME: what about errors */
			/* really this has been a sucess, not a failure */
			pam_end (tmp_pamh, pamerr);

			g_free (prev_user);
			prev_user    = NULL;
			auth_retries = 0;

			mdm_slave_greeter_ctl_no_ret (MDM_SETLOGIN, login);

			goto authenticate_again;
		}

		if (started_timer)
			mdm_slave_greeter_ctl_no_ret (MDM_STOPTIMER, "");

		if (mdm_slave_action_pending ()) {
			/* FIXME: see note above about PAM_FAIL_DELAY */
			/* #ifndef PAM_FAIL_DELAY */
			mdm_sleep_no_signal (mdm_daemon_config_get_value_int (MDM_KEY_RETRY_DELAY));
			/* wait up to 100ms randomly */
			usleep (g_random_int_range (0, 100000));
			/* #endif */ /* PAM_FAIL_DELAY */
			mdm_error ("Couldn't authenticate user");

			if (prev_user) {

				unsigned max_auth_retries = 3;
				char *val = mdm_read_default ("LOGIN_RETRIES=");

				if (val) {
					max_auth_retries = atoi (val);
					g_free (val);
				}

				if (allow_retry == FALSE || pamerr == PAM_MAXTRIES ||
				    ++auth_retries >= max_auth_retries) {

					g_free (prev_user);
					prev_user    = NULL;
					auth_retries = 0;
				}
			}
		} else {
			/* cancel, configurator etc pressed */
			g_free (prev_user);
			prev_user    = NULL;
			auth_retries = 0;
		}

		goto pamerr;
	}

	/* stop the timer for timed logins */
	if (started_timer)
		mdm_slave_greeter_ctl_no_ret (MDM_STOPTIMER, "");

	g_free (login);
	login = NULL;
	g_free (prev_user);
	prev_user = NULL;

	if ((pamerr = pam_get_item (pamh, PAM_USER, &p)) != PAM_SUCCESS) {
		login = NULL;
		/* is not really an auth problem, but it will
		   pretty much look as such, it shouldn't really
		   happen */
		if (mdm_slave_action_pending ())
			mdm_error ("Couldn't authenticate user");
		goto pamerr;
	}

	login = g_strdup ((const char *)p);
	/* kind of anal, the greeter likely already knows, but it could have
	   been changed */
	mdm_slave_greeter_ctl_no_ret (MDM_SETLOGIN, login);

	if ( ! mdm_slave_check_user_wants_to_log_in (login)) {
		/* cleanup stuff */
		mdm_slave_greeter_ctl_no_ret (MDM_SETLOGIN, "");
		g_free (login);
		login = NULL;
		mdm_slave_greeter_ctl_no_ret (MDM_RESETOK, "");

		mdm_verify_cleanup (d);

		goto verify_user_again;
	}

	/* Check if user is root and is allowed to log in */

	pwent = getpwnam (login);
	if (( ! mdm_daemon_config_get_value_bool (MDM_KEY_ALLOW_ROOT) ||
            ( ! d->attached )) &&
            (pwent != NULL && pwent->pw_uid == 0)) {
		mdm_error ("Root login disallowed on display '%s'", d->name);
		mdm_slave_greeter_ctl_no_ret (MDM_ERRBOX,
					      _("\nThe system administrator "
						"is not allowed to login "
						"from this screen"));
		/*mdm_slave_greeter_ctl_no_ret (MDM_ERRDLG,
		  _("Root login disallowed"));*/
		error_msg_given = TRUE;

		goto pamerr;
	}

	if (mdm_daemon_config_get_value_bool (MDM_KEY_DISPLAY_LAST_LOGIN)) {
		char *info = mdm_get_last_info (login);
		mdm_slave_greeter_ctl_no_ret (MDM_MSG, info);
		g_free (info);
	}

	/* Check if the user's account is healthy. */
	pamerr = pam_acct_mgmt (pamh, null_tok);
	switch (pamerr) {
	case PAM_SUCCESS :
		break;
	case PAM_NEW_AUTHTOK_REQD :
		if ((pamerr = pam_chauthtok (pamh, PAM_CHANGE_EXPIRED_AUTHTOK)) != PAM_SUCCESS) {
			mdm_error ("Authentication token change failed for user %s", login);
			mdm_slave_greeter_ctl_no_ret (MDM_ERRBOX,
						      _("\nThe change of the authentication token failed. "
							"Please try again later or contact the system administrator."));
			error_msg_given = TRUE;
			goto pamerr;
		}
		break;
	case PAM_ACCT_EXPIRED :
		mdm_error ("User %s no longer permitted to access the system", login);
		mdm_slave_greeter_ctl_no_ret (MDM_ERRBOX,
					      _("\nThe system administrator has disabled your account."));
		error_msg_given = TRUE;
		goto pamerr;
	case PAM_PERM_DENIED :
		mdm_error ("User %s not permitted to gain access at this time", login);
		mdm_slave_greeter_ctl_no_ret (MDM_ERRBOX,
					      _("\nThe system administrator has disabled access to the system temporarily."));
		error_msg_given = TRUE;
		goto pamerr;
	default :
		if (mdm_slave_action_pending ())
			mdm_error ("Couldn't set acct. mgmt for %s", login);
		goto pamerr;
	}

	pwent = getpwnam (login);
	if (/* paranoia */ pwent == NULL ||
	    ! mdm_setup_gids (login, pwent->pw_gid)) {
		mdm_error ("Cannot set user group for %s", login);
		mdm_slave_greeter_ctl_no_ret (MDM_ERRBOX,
					      _("\nCannot set your user group; "
						"you will not be able to log in. "
						"Please contact your system administrator."));
		goto pamerr;
	}

	did_setcred = TRUE;

	/* Set credentials */
	pamerr = pam_setcred (pamh, PAM_ESTABLISH_CRED);
	if (pamerr != PAM_SUCCESS) {
		did_setcred = FALSE;
		if (mdm_slave_action_pending ())
			mdm_error ("Couldn't set credentials for %s", login);
		goto pamerr;
	}

	credentials_set = TRUE;
	opened_session  = TRUE;

	/* Register the session */
	pamerr = pam_open_session (pamh, 0);
	if (pamerr != PAM_SUCCESS) {
		opened_session = FALSE;
		/* we handle this above */
		did_setcred = FALSE;
		if (mdm_slave_action_pending ())
			mdm_error ("Couldn't open session for %s", login);
		goto pamerr;
	}

	/* Workaround to avoid mdm messages being logged as PAM_pwdb */
	mdm_log_shutdown ();
	mdm_log_init ();

	cur_mdm_disp = NULL;

	/*
	 * Login succeeded.
	 * This function is a no-op if libaudit is not present.
	 */
	log_to_audit_system(login, d->hostname, d->name, AU_SUCCESS);

	return login;

 pamerr:
	/*
	 * Take care of situation where we get here before setting pwent.
	 * Since login can be passed in as NULL, get the actual value if
	 * possible.
	 */
	if ((pam_get_item (pamh, PAM_USER, &p)) == PAM_SUCCESS) {
		g_free (login);
		login = g_strdup ((const char *)p);
	}
	if (pwent == NULL && login != NULL) {
		pwent = getpwnam (login);
	}

	/*
	 * Log the failed login attempt.
	 * This function is a no-op if libaudit is not present.
	 */
	log_to_audit_system(login, d->hostname, d->name, AU_FAILED);

	/* The verbose authentication is turned on, output the error
	 * message from the PAM subsystem */
	if ( ! error_msg_given &&
	     mdm_slave_action_pending ()) {
		mdm_slave_write_utmp_wtmp_record (d,
					MDM_SESSION_RECORD_TYPE_FAILED_ATTEMPT,
					login, getpid ());

		/*
		 * I'm not sure yet if I should display this message for any
		 * other issues - heeten
		 * Adding AUTHINFO_UNAVAIL to the list - its what an unknown
		 * user is.
		 */
		if (pamerr == PAM_AUTH_ERR ||
		    pamerr == PAM_USER_UNKNOWN ||
		    pamerr == PAM_AUTHINFO_UNAVAIL) {
			gboolean is_capslock = FALSE;
			const char *basemsg;
			char *msg;
			char *ret;

			ret = mdm_slave_greeter_ctl (MDM_QUERY_CAPSLOCK, "");
			if ( ! ve_string_empty (ret))
				is_capslock = TRUE;
			g_free (ret);

			/* Only give this message if we actually asked for
			   password, otherwise it would be silly to say that
			   the password may have been wrong */
			if (did_we_ask_for_password) {
				basemsg = _("\nIncorrect username or password.  "
					    "Letters must be typed in the correct "
					    "case.");
			} else {
				basemsg = _("\nAuthentication failed.  "
					    "Letters must be typed in the correct "
					    "case.");
			}
			if (is_capslock) {
				msg = g_strconcat (basemsg, "  ",
						   _("Caps Lock is on."),
						   NULL);
			} else {
				msg = g_strdup (basemsg);
			}
			mdm_slave_greeter_ctl_no_ret (MDM_ERRBOX, msg);
			g_free (msg);
		} else {
			mdm_slave_greeter_ctl_no_ret (MDM_ERRDLG, _("Authentication failed"));
		}
	}

	did_setcred = FALSE;
	opened_session = FALSE;

	if (pamh != NULL) {
		pam_handle_t *tmp_pamh;
		mdm_sigterm_block_push ();
		mdm_sigchld_block_push ();
		tmp_pamh = pamh;
		pamh = NULL;
		mdm_sigchld_block_pop ();
		mdm_sigterm_block_pop ();

		/* Throw away the credentials */
		if (credentials_set)
			pam_setcred (tmp_pamh, PAM_DELETE_CRED);
		pam_end (tmp_pamh, pamerr);
	}
	pamh = NULL;

	/* Workaround to avoid mdm messages being logged as PAM_pwdb */
	mdm_log_shutdown ();
	mdm_log_init ();

	g_free (login);

	cur_mdm_disp = NULL;

	return NULL;
}

/**
 * mdm_verify_setup_user:
 * @login: The name of the user
 *
 * This is used for auto loging in.  This just sets up the login
 * session for this user
 */

gboolean
mdm_verify_setup_user (MdmDisplay *d, const gchar *login, char **new_login)
{
	gint pamerr = 0;
	struct passwd *pwent = NULL;
	MDM_PAM_QUAL void *p;
	char *passreq;
	char *pam_stack = NULL;
	char *pam_service_name = NULL;
	int null_tok = 0;
	gboolean credentials_set;
	const char *after_login;

	credentials_set = FALSE;

	*new_login = NULL;

	if (login == NULL)
		return FALSE;

	cur_mdm_disp = d;

	g_free (extra_standalone_message);
	extra_standalone_message = g_strdup_printf ("%s (%s)",
						    _("Automatic login"),
						    login);

	/*
	 * Initialize a PAM session for the user...
	 * Get value per-display so different displays can use different
	 * PAM Stacks, in case one display should use a different
	 * authentication mechanism than another display.
	 */
	pam_stack = mdm_daemon_config_get_value_string_per_display (MDM_KEY_PAM_STACK,
		(char *)d->name);
	pam_service_name = g_strdup_printf ("%s-autologin", pam_stack);

	if ( ! create_pamh (d, pam_service_name, login, &standalone_pamc,
			    d->name, &pamerr)) {
		g_free (pam_stack);
		g_free (pam_service_name);
		goto setup_pamerr;
	}
	g_free (pam_stack);
	g_free (pam_service_name);

	passreq = mdm_read_default ("PASSREQ=");

	if (mdm_daemon_config_get_value_bool (MDM_KEY_PASSWORD_REQUIRED) ||
            ((passreq != NULL) && g_ascii_strcasecmp (passreq, "YES") == 0))
		null_tok |= PAM_DISALLOW_NULL_AUTHTOK;

	/* Start authentication session */
	did_we_ask_for_password = FALSE;
	if ((pamerr = pam_authenticate (pamh, null_tok)) != PAM_SUCCESS) {
		if (mdm_slave_action_pending ()) {
			mdm_error ("Couldn't authenticate user");
			mdm_errorgui_error_box (cur_mdm_disp,
						GTK_MESSAGE_ERROR,
						_("Authentication failed"));
		}
		goto setup_pamerr;
	}

	if ((pamerr = pam_get_item (pamh, PAM_USER, &p)) != PAM_SUCCESS) {
		/* is not really an auth problem, but it will
		   pretty much look as such, it shouldn't really
		   happen */
		mdm_error ("Couldn't authenticate user");
		mdm_errorgui_error_box (cur_mdm_disp,
					GTK_MESSAGE_ERROR,
					_("Authentication failed"));
		goto setup_pamerr;
	}
	after_login = p;

	if (after_login != NULL /* should never be */ &&
	    strcmp (after_login, login) != 0) {
		*new_login = g_strdup (after_login);
	}

	/* Check if the user's account is healthy. */
	pamerr = pam_acct_mgmt (pamh, null_tok);
	switch (pamerr) {
	case PAM_SUCCESS :
		break;
	case PAM_NEW_AUTHTOK_REQD :
		/* XXX: this is for automatic and timed logins,
		 * we shouldn't be asking for new pw since we never
		 * authenticated the user.  I suppose just ignoring
		 * this would be OK */
#if	0	/* don't change password */
		if ((pamerr = pam_chauthtok (pamh, PAM_CHANGE_EXPIRED_AUTHTOK)) != PAM_SUCCESS) {
			mdm_error ("Authentication token change failed for user %s", login);
			mdm_errorgui_error_box (cur_mdm_disp,
						GTK_MESSAGE_ERROR,
						_("\nThe change of the authentication token failed. "
						  "Please try again later or contact the system administrator."));

			goto setup_pamerr;
		}

#endif	/* 0 */
		break;
	case PAM_ACCT_EXPIRED :
		mdm_error ("User %s no longer permitted to access the system", login);
		mdm_errorgui_error_box (cur_mdm_disp,
					GTK_MESSAGE_ERROR,
					_("The system administrator has disabled your account."));
		goto setup_pamerr;
	case PAM_PERM_DENIED :
		mdm_error ("User %s not permitted to gain access at this time", login);
		mdm_errorgui_error_box (cur_mdm_disp,
					GTK_MESSAGE_ERROR,
					_("The system administrator has disabled your access to the system temporarily."));
		goto setup_pamerr;
	default :
		if (mdm_slave_action_pending ())
			mdm_error ("Couldn't set acct. mgmt for %s", login);
		goto setup_pamerr;
	}

	pwent = getpwnam (login);
	if (/* paranoia */ pwent == NULL ||
	    ! mdm_setup_gids (login, pwent->pw_gid)) {
		mdm_error ("Cannot set user group for %s", login);
		mdm_errorgui_error_box (cur_mdm_disp,
					GTK_MESSAGE_ERROR,
					_("Cannot set your user group; "
					  "you will not be able to log in. "
					  "Please contact your system administrator."));

		goto setup_pamerr;
	}

	did_setcred = TRUE;

	/* Set credentials */
	pamerr = pam_setcred (pamh, PAM_ESTABLISH_CRED);
	if (pamerr != PAM_SUCCESS) {
		did_setcred = FALSE;
		if (mdm_slave_action_pending ())
			mdm_error ("Couldn't set credentials for %s", login);
		goto setup_pamerr;
	}

	credentials_set = TRUE;
	opened_session  = TRUE;

	/* Register the session */
	pamerr = pam_open_session (pamh, 0);
	if (pamerr != PAM_SUCCESS) {
		did_setcred = FALSE;
		opened_session = FALSE;
		/* Throw away the credentials */
		pam_setcred (pamh, PAM_DELETE_CRED);

		if (mdm_slave_action_pending ())
			mdm_error ("Couldn't open session for %s", login);
		goto setup_pamerr;
	}

	/* Workaround to avoid mdm messages being logged as PAM_pwdb */
	mdm_log_shutdown ();
	mdm_log_init ();

	cur_mdm_disp = NULL;

	g_free (extra_standalone_message);
	extra_standalone_message = NULL;

	/*
	 * Login succeeded.
	 * This function is a no-op if libaudit is not present
	 */
	log_to_audit_system(login, d->hostname, d->name, AU_SUCCESS);

	return TRUE;

 setup_pamerr:
	/*
	 * Take care of situation where we get here before setting pwent.
	 * Note login is never NULL when this function is called.
	 */
	if (pwent == NULL) {
		pwent = getpwnam (login);
	}

	/*
	 * Log the failed login attempt.
	 * This function is a no-op if libaudit is not present
	 */
	log_to_audit_system(login, d->hostname, d->name, AU_FAILED);

	did_setcred = FALSE;
	opened_session = FALSE;
	if (pamh != NULL) {
		pam_handle_t *tmp_pamh;

		mdm_sigterm_block_push ();
		mdm_sigchld_block_push ();
		tmp_pamh = pamh;
		pamh = NULL;
		mdm_sigchld_block_pop ();
		mdm_sigterm_block_pop ();

		/* Throw away the credentials */
		if (credentials_set)
			pam_setcred (tmp_pamh, PAM_DELETE_CRED);
		pam_end (tmp_pamh, pamerr);
	}
	pamh = NULL;

	/* Workaround to avoid mdm messages being logged as PAM_pwdb */
	mdm_log_shutdown ();
	mdm_log_init ();

	cur_mdm_disp = NULL;

	g_free (extra_standalone_message);
	extra_standalone_message = NULL;

	return FALSE;
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
	cur_mdm_disp = d;

	if (pamh != NULL) {
		gint pamerr;
		pam_handle_t *tmp_pamh;
		gboolean old_opened_session;
		gboolean old_did_setcred;

		mdm_debug ("Running mdm_verify_cleanup and pamh != NULL");

		mdm_sigterm_block_push ();
		mdm_sigchld_block_push ();
		tmp_pamh = pamh;
		pamh = NULL;
		old_opened_session = opened_session;
		opened_session = FALSE;
		old_did_setcred = did_setcred;
		did_setcred = FALSE;
		mdm_sigchld_block_pop ();
		mdm_sigterm_block_pop ();

		pamerr = PAM_SUCCESS;

		/* Close the users session */
		if (old_opened_session) {
			mdm_debug ("Running pam_close_session");
			pamerr = pam_close_session (tmp_pamh, 0);
		}

		/* Throw away the credentials */
		if (old_did_setcred) {
			mdm_debug ("Running pam_setcred with PAM_DELETE_CRED");
			pamerr = pam_setcred (tmp_pamh, PAM_DELETE_CRED);
		}

		pam_end (tmp_pamh, pamerr);

		/* Workaround to avoid mdm messages being logged as PAM_pwdb */
                mdm_log_shutdown ();
                mdm_log_init ();
	}

	/* Clear the group setup */
	setgid (0);
	/* this will get rid of any suplementary groups etc... */
	setgroups (1, groups);

	cur_mdm_disp = NULL;

	/* reset limits */
	mdm_reset_limits ();
}

/* used in pam */
gboolean
mdm_verify_setup_env (MdmDisplay *d)
{
	gchar **pamenv;

	if (pamh == NULL)
		return FALSE;

	/* Migrate any PAM env. variables to the user's environment */
	/* This leaks, oh well */
	if ((pamenv = pam_getenvlist (pamh))) {
		gint i;

		for (i = 0 ; pamenv[i] ; i++) {
			putenv (g_strdup (pamenv[i]));
		}
	}

	return TRUE;
}
