/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * MDM - The GNOME Display Manager
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

#include <signal.h>
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>
#if defined (_POSIX_PRIORITY_SCHEDULING) && defined (HAVE_SCHED_YIELD)
#include <sched.h>
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <ctype.h>
#include <pwd.h>
#include <grp.h>
#include <fcntl.h>
#include <errno.h>
#include <locale.h>
#include <dirent.h>

#ifdef HAVE_CHKAUTHATTR
#include <auth_attr.h>
#include <secdb.h>
#endif

/* This should be moved to auth.c I suppose */

#include <X11/Xauth.h>
#include <glib/gi18n.h>
#include <glib-object.h>

#include <gtk/gtk.h>

/* Needed for signal handling */
#include "mdm-common.h"

#include "mdm.h"
#include "misc.h"
#include "slave.h"
#include "server.h"
#include "verify.h"
#include "display.h"
#include "choose.h"
#include "getvt.h"
#include "mdm-net.h"
#include "cookie.h"
#include "filecheck.h"
#include "errorgui.h"

#include "mdm-socket-protocol.h"
#include "mdm-daemon-config.h"
#include "mdm-log.h"

#include "xdmcp.h"

#define DYNAMIC_ADD     0
#define DYNAMIC_RELEASE 1
#define DYNAMIC_REMOVE  2

/* Local functions */
static void mdm_handle_message (MdmConnection *conn,
				const gchar *msg,
				gpointer data);
static void mdm_handle_user_message (MdmConnection *conn,
				     const gchar *msg,
				     gpointer data);
static void mdm_daemonify (void);
static void mdm_safe_restart (void);
static void mdm_try_logout_action (MdmDisplay *disp);
static void mdm_restart_now (void);
static void handle_flexi_server (MdmConnection *conn,
				 int type,
				 const gchar *server,
				 gboolean handled,
				 gboolean chooser,
				 const gchar *xnest_disp,
				 uid_t xnest_uid,
				 const gchar *xnest_auth_file,
				 const gchar *xnest_cookie,
				 const gchar *username);
static void custom_cmd_restart (long cmd_id);
static void custom_cmd_no_restart (long cmd_id);

/* Global vars */

gint flexi_servers         = 0; /* Number of flexi servers */
pid_t extra_process = 0;        /* An extra process.  Used for quickie
                                   processes, so that they also get whacked */
static int extra_status    = 0; /* Last status from the last extra process */
pid_t mdm_main_pid         = 0; /* PID of the main daemon */

gboolean mdm_wait_for_go         = FALSE; /* wait for a GO in the fifo */
static gboolean print_version    = FALSE; /* print version number and quit */
static gboolean preserve_ld_vars = FALSE; /* Preserve the ld environment
                                             variables */
static gboolean no_daemon        = FALSE; /* Do not daemonize */
static gboolean no_console       = FALSE; /* There are no static servers, this
                                             means, don't run static servers
                                             and second, don't display info on
                                             the console */

MdmConnection *fifoconn = NULL; /* Fifo connection */
MdmConnection *pipeconn = NULL; /* slavepipe (handled just like Fifo for
                                   compatibility) connection */
MdmConnection *unixconn = NULL; /* UNIX Socket connection */
int slave_fifo_pipe_fd = -1;    /* The slavepipe connection */

unsigned char *mdm_global_cookie  = NULL;
unsigned char *mdm_global_bcookie = NULL;
char *mdm_system_locale = NULL;

gboolean mdm_first_login = TRUE;

static MdmLogoutAction safe_logout_action = MDM_LOGOUT_ACTION_NONE;

/* set in the main function */
gchar **stored_argv = NULL;
int stored_argc     = 0;

static GMainLoop *main_loop       = NULL;
static gchar *config_file         = NULL;
static gboolean mdm_restart_mode  = FALSE;
static gboolean monte_carlo_sqrt2 = FALSE;

/*
 * Lookup display number if the display number is
 * exists then clear the remove flag and return TRUE
 * otherwise return FALSE.
 */
static gboolean
mark_display_exists (int num)
{
	GSList *li;
	GSList *displays;

	displays = mdm_daemon_config_get_display_list ();

	for (li = displays; li != NULL; li = li->next) {
		MdmDisplay *disp = li->data;
		if (disp->dispnum == num) {
			disp->removeconf = FALSE;
			return TRUE;
		}
	}
	return FALSE;
}

/**
 * mdm_daemonify:
 *
 * Detach mdm daemon from the controlling terminal
 */
static void
mdm_daemonify (void)
{
	FILE *pf;
	pid_t pid;

	pid = fork ();
	if (pid > 0) {
		const char *pidfile = MDM_PID_FILE;

		errno = 0;
		if ((pf = mdm_safe_fopen_w (pidfile, 0644)) != NULL) {
			errno = 0;
			VE_IGNORE_EINTR (fprintf (pf, "%d\n", (int)pid));
			VE_IGNORE_EINTR (fclose (pf));
			if G_UNLIKELY (errno != 0) {
				/* FIXME: how to handle this? */
				mdm_fdprintf (2, _("Cannot write PID file %s: possibly out of diskspace.  Error: %s\n"),
					      pidfile, strerror (errno));
				mdm_error (_("Cannot write PID file %s: possibly out of diskspace.  Error: %s"),
					   pidfile, strerror (errno));

			}
		} else if G_UNLIKELY (errno != 0) {
			/* FIXME: how to handle this? */
			mdm_fdprintf (2, _("Cannot write PID file %s: possibly out of diskspace.  Error: %s\n"),
				      pidfile, strerror (errno));
			mdm_error (_("Cannot write PID file %s: possibly out of diskspace.  Error: %s"),
				   pidfile, strerror (errno));

		}

		exit (EXIT_SUCCESS);
	}

	mdm_main_pid = getpid ();

	if G_UNLIKELY (pid < 0)
		mdm_fail (_("%s: fork () failed!"), "mdm_daemonify");

	if G_UNLIKELY (setsid () < 0)
		mdm_fail (_("%s: setsid () failed: %s!"), "mdm_daemonify",
			  strerror (errno));

	VE_IGNORE_EINTR (g_chdir (mdm_daemon_config_get_value_string (MDM_KEY_SERV_AUTHDIR)));
	umask (022);

	VE_IGNORE_EINTR (close (0));
	VE_IGNORE_EINTR (close (1));
	VE_IGNORE_EINTR (close (2));

	mdm_open_dev_null (O_RDONLY); /* open stdin - fd 0 */
	mdm_open_dev_null (O_RDWR); /* open stdout - fd 1 */
	mdm_open_dev_null (O_RDWR); /* open stderr - fd 2 */
}

static void
mdm_start_first_unborn_local (int delay)
{
	GSList *li;
	GSList *displays;

	displays = mdm_daemon_config_get_display_list ();

	/* tickle the random stuff */
	mdm_random_tick ();

	for (li = displays; li != NULL; li = li->next) {
		MdmDisplay *d = li->data;

		if (d != NULL &&
		    d->type == TYPE_STATIC &&
		    d->dispstat == DISPLAY_UNBORN) {
			MdmXserver *svr;
			mdm_debug ("mdm_start_first_unborn_local: "
				   "Starting %s", d->name);

			/* well sleep at least 'delay' seconds
			 * before starting */
			d->sleep_before_run = delay;

			/* all static displays have
			 * timed login going on */
			d->timed_login_ok = TRUE;

			svr = mdm_server_resolve (d);

			if ( ! mdm_display_manage (d)) {
				mdm_display_unmanage (d);
				/* only the first static display where
				   we actually log in gets
				   autologged in */
				if (svr != NULL &&
				    svr->handled &&
				    ! svr->chooser)
					mdm_first_login = FALSE;
			} else {
				/* only the first static display where
				   we actually log in gets
				   autologged in */
				if (svr != NULL &&
				    svr->handled &&
				    ! svr->chooser)
					mdm_first_login = FALSE;
				break;
			}
		}
	}
}

void
mdm_final_cleanup (void)
{
	GSList *list, *li;
	const char *pidfile;
	gboolean first;
	GSList *displays;
	struct sigaction sig;

	/* Remove all signal handlers, since we are freeing structures used by the handlers */
	sig.sa_handler = SIG_DFL;
	sig.sa_flags = SA_RESTART;
	sigemptyset (&sig.sa_mask);
	sigaction (SIGTERM, &sig, NULL);
	sigaction (SIGINT,  &sig, NULL);
	sigaction (SIGHUP,  &sig, NULL);
	sigaction (SIGUSR1, &sig, NULL);
#ifdef SIGXCPU
	sigaction (SIGXCPU, &sig, NULL);
#endif
#ifdef SIGXFSZ
	sigaction (SIGXFSZ, &sig, NULL);
#endif

	displays = mdm_daemon_config_get_display_list ();

	mdm_debug ("mdm_final_cleanup");

	if (extra_process > 1) {
		/* we sigterm extra processes, and we
		 * don't wait */
		kill (-(extra_process), SIGTERM);
		extra_process = 0;
	}

	/* First off whack all XDMCP and FLEXI_XNEST
	   slaves, we'll wait for them later */
	for (li = displays; li != NULL; li = li->next) {
		MdmDisplay *d = li->data;
		if (SERVER_IS_XDMCP (d) ||
		    SERVER_IS_PROXY (d)) {
			/* set to DEAD so that we won't kill it again */
			d->dispstat = DISPLAY_DEAD;
			if (d->slavepid > 1)
				kill (d->slavepid, SIGTERM);
		}
	}

	/* Now completely unmanage the static servers */
	first = TRUE;
	list = g_slist_copy (displays);
	/* somewhat of a hack to kill last server
	 * started first.  This mostly makes things end up on
	 * the right vt */
	list = g_slist_reverse (list);
	for (li = list; li != NULL; li = li->next) {
		MdmDisplay *d = li->data;
		if (SERVER_IS_XDMCP (d) ||
		    SERVER_IS_PROXY (d))
			continue;
		/* HACK! Wait 2 seconds between killing of static servers
		 * because X is stupid and full of races and will otherwise
		 * hang my keyboard */
		if ( ! first) {
			/* there could be signals happening
			   here */
			mdm_sleep_no_signal (2);
		}
		first = FALSE;
		mdm_display_unmanage (d);
	}
	g_slist_free (list);

	/* and now kill and wait for the XDMCP and FLEXI_XNEST
	   slaves.  unmanage will not kill slaves we have already
	   killed unless a SIGTERM was sent in the meantime */

	list = g_slist_copy (displays);
	for (li = list; li != NULL; li = li->next) {
		MdmDisplay *d = li->data;
		if (SERVER_IS_XDMCP (d) ||
		    SERVER_IS_PROXY (d))
			mdm_display_unmanage (d);
	}
	g_slist_free (list);

	/* Close stuff */

	if (mdm_daemon_config_get_value_bool (MDM_KEY_XDMCP))
		mdm_xdmcp_close ();

	if (fifoconn != NULL) {
		char *path;
		mdm_connection_close (fifoconn);
		path = g_build_filename (mdm_daemon_config_get_value_string (MDM_KEY_SERV_AUTHDIR), ".mdmfifo", NULL);
		VE_IGNORE_EINTR (g_unlink (path));
		g_free (path);
		fifoconn = NULL;
	}

	if (pipeconn != NULL) {
		mdm_connection_close (pipeconn);
		pipeconn = NULL;
	}

	if (slave_fifo_pipe_fd >= 0) {
		VE_IGNORE_EINTR (close (slave_fifo_pipe_fd));
		slave_fifo_pipe_fd = -1;
	}

	if (unixconn != NULL) {
		mdm_connection_close (unixconn);
		VE_IGNORE_EINTR (g_unlink (MDM_SUP_SOCKET));
		unixconn = NULL;
	}

	pidfile = MDM_PID_FILE;
	if (pidfile != NULL) {
		VE_IGNORE_EINTR (g_unlink (pidfile));
	}

	mdm_daemon_config_close ();
}

static gboolean
deal_with_x_crashes (MdmDisplay *d)
{
	gboolean just_abort = FALSE;
	const char *failsafe = mdm_daemon_config_get_value_string (MDM_KEY_FAILSAFE_XSERVER);
	const char *keepscrashing = mdm_daemon_config_get_value_string (MDM_KEY_X_KEEPS_CRASHING);

	if ( ! d->failsafe_xserver &&
	     ! ve_string_empty (failsafe)) {
		char *bin = ve_first_word (failsafe);
		/* Yay we have a failsafe */
		if ( ! ve_string_empty (bin) &&
		     g_access (bin, X_OK) == 0) {
			mdm_info (_("%s: Trying failsafe X "
				    "server %s"),
				  "deal_with_x_crashes",
				  failsafe);
			g_free (bin);
			g_free (d->command);
			d->command = g_strdup (failsafe);
			d->failsafe_xserver = TRUE;
			return TRUE;
		}
		g_free (bin);
	}

	/* Eeek X keeps crashing, let's try the XKeepsCrashing script */
	if ( ! ve_string_empty (keepscrashing) &&
	     g_access (keepscrashing, X_OK|R_OK) == 0) {
		pid_t pid;

		mdm_info (_("%s: Running the "
			    "XKeepsCrashing script"),
			  "deal_with_x_crashes");

		extra_process = pid = fork ();
		if (pid < 0)
			extra_process = 0;

		if (pid == 0) {
			char *argv[2];
			char *xlog = mdm_make_filename (mdm_daemon_config_get_value_string (MDM_KEY_LOG_DIR), d->name, ".log");

			mdm_unset_signals ();

			/* Also make a new process group so that we may use
			 * kill -(extra_process) to kill extra process and all its
			 * possible children */
			setsid ();

			if (mdm_daemon_config_get_value_bool (MDM_KEY_XDMCP))
				mdm_xdmcp_close ();

			mdm_close_all_descriptors (0 /* from */, -1 /* except */, -1 /* except2 */);

			/* No error checking here - if it's messed the best response
			 * is to ignore & try to continue */
			mdm_open_dev_null (O_RDONLY); /* open stdin - fd 0 */
			mdm_open_dev_null (O_RDWR); /* open stdout - fd 1 */
			mdm_open_dev_null (O_RDWR); /* open stderr - fd 2 */

			argv[0] = (char *)mdm_daemon_config_get_value_string (MDM_KEY_X_KEEPS_CRASHING);
			argv[1] = NULL;

			mdm_restoreenv ();

			/* unset DISPLAY and XAUTHORITY if they exist
			 * so that gdialog (if used) doesn't get confused */
			g_unsetenv ("DISPLAY");
			g_unsetenv ("XAUTHORITY");

			/* some promised variables */
			g_setenv ("XLOG", xlog, TRUE);
			g_setenv ("BINDIR", BINDIR, TRUE);
			g_setenv ("SBINDIR", SBINDIR, TRUE);
			g_setenv ("LIBEXECDIR", LIBEXECDIR, TRUE);
			g_setenv ("SYSCONFDIR", MDMCONFDIR, TRUE);

			/* To enable gettext stuff in the script */
			g_setenv ("TEXTDOMAIN", GETTEXT_PACKAGE, TRUE);
			g_setenv ("TEXTDOMAINDIR", GNOMELOCALEDIR, TRUE);

			if ( ! mdm_ok_console_language ()) {
				g_unsetenv ("LANG");
				g_unsetenv ("LC_ALL");
				g_unsetenv ("LC_MESSAGES");
				g_setenv ("LANG", "C", TRUE);
				g_setenv ("UNSAFE_TO_TRANSLATE", "yes", TRUE);
			}

			VE_IGNORE_EINTR (execv (argv[0], argv));

			/* yaikes! */
			_exit (32);
		} else if (pid > 0) {
			int status;

			if (extra_process > 1) {
				int ret;
				int killsignal = SIGTERM;
				int storeerrno;
				errno = 0;
				ret = waitpid (extra_process, &status, WNOHANG);
				do {
					/* wait for some signal, yes this is a race */
					if (ret <= 0)
						sleep (10);
					errno = 0;
					ret = waitpid (extra_process, &status, WNOHANG);
					storeerrno = errno;
					if ((ret <= 0) && mdm_daemon_config_signal_terminthup_was_notified ()) {
						kill (-(extra_process), killsignal);
						killsignal = SIGKILL;
					}
				} while (ret == 0 || (ret < 0 && storeerrno == EINTR));
			}
			extra_process = 0;

			if (WIFEXITED (status) &&
			    WEXITSTATUS (status) == 0) {
				/* Yay, the user wants to try again, so
				 * here we go */
				return TRUE;
			} else if (WIFEXITED (status) &&
				   WEXITSTATUS (status) == 32) {
				/* We couldn't run the script, just drop through */
				;
			} else {
				/* Things went wrong. */
				just_abort = TRUE;
			}
		}

		/* if we failed to fork, or something else has happened,
		 * we fall through to the other options below */
	}

	/* if we have "open" we can talk to the user, not as user
	 * friendly as the above script, but getting there */
	if ( ! just_abort &&
	     g_access (LIBEXECDIR "/mdmopen", X_OK) == 0) {
		/* Shit if we knew what the program was to tell the user,
		 * the above script would have been defined and we'd run
		 * it for them */
		const char *error =
			C_(N_("The X server (your graphical interface) "
			      "cannot be started.  It is likely that it is not "
			      "set up correctly.  You will need to log in on a "
			      "console and rerun the X configuration "
			      "application, then restart MDM."));
		mdm_text_message_dialog (error);
	} /* else {
	   * At this point .... screw the user, we don't know how to
	   * talk to him.  He's on some 'l33t system anyway, so syslog
	   * reading will do him good
	   * } */

	mdm_error (_("Failed to start X server several times in a short time period; disabling display %s"), d->name);

	return FALSE;
}

static gboolean
try_command (const char *command)
{
	GError  *error;
	gboolean res;
	int      status;

	mdm_debug ("Running %s", command);

	error = NULL;
	res = g_spawn_command_line_sync (command, NULL, NULL, &status, &error);
	if (error != NULL) {
		mdm_debug ("Command failed %s: %s", command, error->message);
		g_error_free (error);		
	}
	else {
		if (WIFEXITED (status)) {
			if (WEXITSTATUS (status) != 0) {
				mdm_error ("Command '%s' exited with status %u", command, WEXITSTATUS (status));
				res = FALSE;
			}
		}
		else if (WIFSIGNALED (status)) {
			mdm_error ("Command '%s' was killed by signal '%s'", command, g_strsignal (WTERMSIG (status)));
			res = FALSE;
		}
	}

	return res;
}

static gboolean
try_commands (const char **array)
{
	int      i;
	gboolean ret;

	ret = FALSE;

	/* the idea here is to try the first available command and return if it succeeded */

	for (i = 0; array[i] != NULL; i++) {
		ret = try_command (array[i]);
		if (ret == TRUE)
			break;				
	}

	return ret;
}

static void
suspend_machine (void)
{
	const gchar **suspend;

	suspend = mdm_daemon_config_get_value_string_array (MDM_KEY_SUSPEND);

	mdm_info (_("Master suspending..."));

	if (suspend == NULL) {
		return;
	}

	try_commands (suspend);
}

#ifdef __linux__
static void
change_to_first_and_clear (gboolean restart)
{
	mdm_change_vt (1);
	VE_IGNORE_EINTR (close (0));
	VE_IGNORE_EINTR (close (1));
	VE_IGNORE_EINTR (close (2));
	VE_IGNORE_EINTR (open ("/dev/tty1", O_WRONLY));
	VE_IGNORE_EINTR (open ("/dev/tty1", O_WRONLY));
	VE_IGNORE_EINTR (open ("/dev/tty1", O_WRONLY));

	g_setenv ("TERM", "linux", TRUE);

	/* evil hack that will get the fonts right */
	if (g_access ("/bin/bash", X_OK) == 0)
		system ("/bin/bash -l -c /bin/true");

	/* clear screen and set to red */
	printf ("\033[H\033[J\n\n\033[1m---\n\033[1;31m ");

	if (restart)
		printf (_("System is restarting, please wait ..."));
	else
		printf (_("System is shutting down, please wait ..."));
	/* set to black */
	printf ("\033[0m\n\033[1m---\033[0m\n\n");
}
#endif /* __linux__ */

static void
halt_machine (void)
{
	const char **s;

	mdm_debug (_("Master halting..."));

	s = mdm_daemon_config_get_value_string_array (MDM_KEY_HALT);

	if (try_commands (s)) {
		/* maybe these don't run but oh well - there isn't
		   really a good way to know a priori if the command
		   will succeed. */
		mdm_final_cleanup ();
		VE_IGNORE_EINTR (g_chdir ("/"));
#ifdef __linux__
		change_to_first_and_clear (FALSE);
#endif /* __linux */

		_exit (EXIT_SUCCESS);
	}
}

static void
restart_machine (void)
{
	const char **s;

	mdm_debug (_("Restarting computer..."));

	s = mdm_daemon_config_get_value_string_array (MDM_KEY_REBOOT);

	if (try_commands (s)) {
		mdm_final_cleanup ();
		VE_IGNORE_EINTR (g_chdir ("/"));

#ifdef __linux__
		change_to_first_and_clear (TRUE);
#endif /* __linux */

	_exit (EXIT_SUCCESS);
	}
}

static void
custom_cmd (long cmd_id)
{
        gchar * key_string;

        if (cmd_id < 0 || cmd_id >= MDM_CUSTOM_COMMAND_MAX) {
		/* We are just feeling very paranoid */
		mdm_error (_("custom_cmd: Custom command index %ld outside permitted range [0,%d)"),
			   cmd_id, MDM_CUSTOM_COMMAND_MAX);
		return;
	}

        key_string = g_strdup_printf ("%s%ld=", MDM_KEY_CUSTOM_CMD_NO_RESTART_TEMPLATE, cmd_id);
        if (mdm_daemon_config_get_value_bool (key_string))
	        custom_cmd_no_restart (cmd_id);
	else
	        custom_cmd_restart (cmd_id);

	g_free(key_string);
}

static void
custom_cmd_restart (long cmd_id)
{
	gchar * key_string;
	char **argv;
	const char *s;

        mdm_debug (_("Executing custom command %ld with restart option..."), cmd_id);

	VE_IGNORE_EINTR (g_chdir ("/"));

#ifdef __linux__
	change_to_first_and_clear (TRUE);
#endif /* __linux */

	key_string = g_strdup_printf ("%s%ld=", MDM_KEY_CUSTOM_CMD_TEMPLATE, cmd_id);

	argv = NULL;
	s = mdm_daemon_config_get_value_string (key_string);
	g_free (key_string);
	if (s != NULL) {
		g_shell_parse_argv (s, NULL, &argv, NULL);
	}

	mdm_final_cleanup ();

	if (argv != NULL && argv[0] != NULL)
		VE_IGNORE_EINTR (execv (argv[0], argv));

	g_strfreev (argv);

	mdm_error (_("%s: Execution of custom command failed: %s"),
		   "mdm_child_action", strerror (errno));
}

static void
custom_cmd_no_restart (long cmd_id)
{
        pid_t pid;

        mdm_debug (_("Executing custom command %ld with no restart option ..."), cmd_id);

        pid = fork ();

	if (pid < 0) {
	        /*failed fork*/
	        mdm_error (_("custom_cmd: forking process for custom command %ld failed"), cmd_id);
		return;
	}
	else if (pid == 0) {
		/* child */
		char **argv;
		const char *s;
		gchar *key_string;

		key_string = g_strdup_printf ("%s%ld=", MDM_KEY_CUSTOM_CMD_TEMPLATE, cmd_id);

		argv = NULL;
		s = mdm_daemon_config_get_value_string (key_string);
		g_free (key_string);
		if (s != NULL) {
			g_shell_parse_argv (s, NULL, &argv, NULL);
		}

		if (argv != NULL && argv[0] != NULL)
			VE_IGNORE_EINTR (execv (argv[0], argv));

		g_strfreev (argv);

		mdm_error (_("%s: Execution of custom command failed: %s"),
			   "mdm_child_action", strerror (errno));
		_exit (0);
	}
	else {
		/* parent */
		gint exitstatus = 0, status;
		pid_t p_stat = waitpid (1, &exitstatus, WNOHANG);
		if (p_stat > 0) {
			if G_LIKELY (WIFEXITED (exitstatus)){
				status = WEXITSTATUS (exitstatus);
				mdm_debug (_("custom_cmd: child %d returned %d"), p_stat, status);
			}
			return;
		}
	}
}

static gboolean
mdm_cleanup_children (void)
{
	gint exitstatus = 0, status;
	MdmDisplay *d = NULL;
	gboolean crashed;
	gboolean sysmenu;
	pid_t pid;

	/* Pid and exit status of slave that died */
	pid = waitpid (-1, &exitstatus, WNOHANG);

	if (pid <= 0)
		return FALSE;

	if G_LIKELY (WIFEXITED (exitstatus)) {
		status = WEXITSTATUS (exitstatus);
		crashed = FALSE;
		mdm_debug ("mdm_cleanup_children: child %d returned %d", pid, status);
	} else {
		status = EXIT_SUCCESS;
		crashed = TRUE;
		if (WIFSIGNALED (exitstatus)) {
			if (WTERMSIG (exitstatus) == SIGTERM ||
			    WTERMSIG (exitstatus) == SIGINT) {
				/*
				 * We send these signals, sometimes children
				 * do not handle them
				 */
				mdm_debug ("mdm_cleanup_children: child %d died of signal %d (TERM/INT)", pid,
					   (int)WTERMSIG (exitstatus));
			} else {
				mdm_error ("mdm_cleanup_children: child %d crashed of signal %d", pid,
					   (int)WTERMSIG (exitstatus));
			}
		} else {
			mdm_error ("mdm_cleanup_children: child %d crashed", pid);
		}
	}

	if (pid == extra_process) {
		/* An extra process died, yay! */
		extra_process = 0;
		extra_status  = exitstatus;
		return TRUE;
	}

	/* Find out who this slave belongs to */
	d = mdm_display_lookup (pid);

	if (d == NULL)
		return TRUE;

	/* Whack connections about this display */
	if (unixconn != NULL)
		mdm_kill_subconnections_with_display (unixconn, d);

	if G_UNLIKELY (crashed) {
		mdm_error ("mdm_cleanup_children: Slave crashed, killing its "
			   "children");

		if (d->sesspid > 1)
			kill (-(d->sesspid), SIGTERM);
		d->sesspid = 0;
		if (d->greetpid > 1)
			kill (-(d->greetpid), SIGTERM);
		d->greetpid = 0;
		if (d->chooserpid > 1)
			kill (-(d->chooserpid), SIGTERM);
		d->chooserpid = 0;
		if (d->servpid > 1)
			kill (d->servpid, SIGTERM);
		d->servpid = 0;

		if (mdm_daemon_config_get_value_bool (MDM_KEY_DYNAMIC_XSERVERS)) {
			/* XXX - This needs to be handled better */
			mdm_server_whack_lockfile (d);
		}

		/* Race avoider */
		mdm_sleep_no_signal (1);
	}

	/* null all these, they are not valid most definately */
	d->servpid    = 0;
	d->sesspid    = 0;
	d->greetpid   = 0;
	d->chooserpid = 0;

	/* definately not logged in now */
	d->logged_in = FALSE;
	g_free (d->login);
	d->login = NULL;

	/* Declare the display dead */
	d->slavepid = 0;
	d->dispstat = DISPLAY_DEAD;

	if (status == DISPLAY_RESTARTMDM ||
	    status == DISPLAY_REBOOT     ||
	    status == DISPLAY_SUSPEND    ||
	    status == DISPLAY_HALT) {
		/*
		 * Reset status to DISPLAY_REMANAGE if it is not valid to
		 * perform the operation
		 */
		sysmenu = mdm_daemon_config_get_value_bool_per_display (
			MDM_KEY_SYSTEM_MENU, d->name);

		if (!sysmenu) {
			mdm_info (_("Restart MDM, Restart machine, Suspend, or Halt request when there is no system menu from display %s"), d->name);
			status = DISPLAY_REMANAGE;
		}


		if ( ! d->attached) {
			mdm_info (_("Restart MDM, Restart machine, Suspend or Halt request from a non-static display %s"), d->name);
			status = DISPLAY_REMANAGE;
		}

		/* checkout if we can actually do stuff */
		switch (status) {
		case DISPLAY_REBOOT:
			if (mdm_daemon_config_get_value_string_array (MDM_KEY_REBOOT) == NULL)
				status = DISPLAY_REMANAGE;
			break;
		case DISPLAY_HALT:
			if (mdm_daemon_config_get_value_string_array (MDM_KEY_HALT) == NULL)
				status = DISPLAY_REMANAGE;
			break;
		case DISPLAY_SUSPEND:
			if (mdm_daemon_config_get_value_string_array (MDM_KEY_SUSPEND) == NULL)
				status = DISPLAY_REMANAGE;
			break;
		default:
			break;
		}
	}

	if (status == DISPLAY_RUN_CHOOSER) {
		sysmenu = mdm_daemon_config_get_value_bool_per_display (
			MDM_KEY_SYSTEM_MENU, d->name);

		/* Use the chooser on the next run (but only if allowed) */
		if (sysmenu &&
		    mdm_daemon_config_get_value_bool_per_display (
			MDM_KEY_CHOOSER_BUTTON, d->name)) {
			d->use_chooser = TRUE;
		}

		status = DISPLAY_REMANAGE;
		/*
		 * Go around the display loop detection, these are short
		 * sessions, so this decreases the chances of the loop
		 * detection being hit
		 */
		d->last_loop_start_time = 0;
	}

	if (status == DISPLAY_CHOSEN) {
		/*
		 * Forget about this indirect id, since this
		 * display will be dead very soon, and we don't want it
		 * to take the indirect display with it
		 */
		d->indirect_id = 0;
		status = DISPLAY_REMANAGE;
	}

	if (status == DISPLAY_GREETERFAILED) {
		if (d->managetime + 10 >= time (NULL)) {
			d->try_different_greeter = TRUE;
		} else {
			d->try_different_greeter = FALSE;
		}
		/* Now just remanage */
		status = DISPLAY_REMANAGE;
	} else {
		d->try_different_greeter = FALSE;
	}

	/* if we crashed clear the theme */
	if (crashed) {
		g_free (d->theme_name);
		d->theme_name = NULL;
	}

 start_autopsy:

	/* Autopsy */
	switch (status) {

	case DISPLAY_ABORT:		/* Bury this display for good */
		mdm_info (_("%s: Aborting display %s"),
			  "mdm_child_action", d->name);

		mdm_try_logout_action (d);
		mdm_safe_restart ();

		mdm_display_unmanage (d);

		/* If there are some pending statics, start them now */
		mdm_start_first_unborn_local (3 /* delay */);
		break;

	case DISPLAY_REBOOT:	/* Restart machine */
		restart_machine ();

		status = DISPLAY_REMANAGE;
		goto start_autopsy;
		break;

	case DISPLAY_HALT:	/* Halt machine */
		halt_machine ();

		status = DISPLAY_REMANAGE;
		goto start_autopsy;
		break;

	case DISPLAY_SUSPEND:	/* Suspend machine */
		/* XXX: this is ugly, why should there be a suspend like this,
		 * see MDM_SOP_SUSPEND_MACHINE */
		suspend_machine ();

		status = DISPLAY_REMANAGE;
		goto start_autopsy;
		break;

	case DISPLAY_RESTARTMDM:
		mdm_restart_now ();
		break;

	case DISPLAY_XFAILED:       /* X sucks */
		mdm_debug ("X failed!");
		/* inform about error if needed */
		if (d->socket_conn != NULL) {
			MdmConnection *conn = d->socket_conn;
			d->socket_conn = NULL;
			mdm_connection_set_close_notify (conn, NULL, NULL);
			mdm_connection_write (conn, "ERROR 3 X failed\n");
		}

		mdm_try_logout_action (d);
		mdm_safe_restart ();

		/* in remote/flexi case just drop to _REMANAGE */
		if (d->type == TYPE_STATIC) {
			time_t now = time (NULL);
			d->x_faileds++;
			/* This really is likely the first time if it's been,
			   some time, say 5 minutes */
			if (now - d->last_x_failed > (5*60)) {
				/* reset */
				d->x_faileds = 1;
				d->last_x_failed = now;
				/* Sleep at least 3 seconds before starting */
				d->sleep_before_run = 3;
			} else if (d->x_faileds >= 3) {
				mdm_debug ("mdm_child_action: dealing with X crashes");
				if ( ! deal_with_x_crashes (d)) {
					mdm_debug ("mdm_child_action: Aborting display");
					/*
					 * An original way to deal with these
					 * things:
					 * "Screw you guys, I'm going home!"
					 */
					mdm_display_unmanage (d);

					/* If there are some pending statics,
					 * start them now */
					mdm_start_first_unborn_local (3 /* delay */);
					break;
				}
				mdm_debug ("mdm_child_action: Trying again");

				/* reset */
				d->x_faileds     = 0;
				d->last_x_failed = 0;
			} else {
				/* Sleep at least 3 seconds before starting */
				d->sleep_before_run = 3;
			}
			/*
			 * Go around the display loop detection, we are doing
			 * our own here
			 */
			d->last_loop_start_time = 0;
		}
		/* fall through */

	case DISPLAY_REMANAGE:	/* Remanage display */
	default:
		mdm_debug ("mdm_child_action: In remanage");

		/*
		 * If we did REMANAGE, that means that we are no longer
		 * failing.
		 */
		if (status == DISPLAY_REMANAGE) {
			/* reset */
			d->x_faileds = 0;
			d->last_x_failed = 0;
		}

		/* inform about error if needed */
		if (d->socket_conn != NULL) {
			MdmConnection *conn = d->socket_conn;
			d->socket_conn = NULL;
			mdm_connection_set_close_notify (conn, NULL, NULL);
			mdm_connection_write (conn, "ERROR 2 Startup errors\n");
		}

		mdm_try_logout_action (d);
		mdm_safe_restart ();

		/* This is a static server so we start a new slave */
		if (d->type == TYPE_STATIC) {
			if ( ! mdm_display_manage (d)) {
				mdm_display_unmanage (d);
				/* If there are some pending statics,
				 * start them now */
				mdm_start_first_unborn_local (3 /* delay */);
			}
		} else if (d->type == TYPE_FLEXI || d->type == TYPE_FLEXI_XNEST) {
			/*
			 * If this was a chooser session and we have chosen a
			 * host, then we don't want to unmanage, we want to
			 * manage and choose that host
			 */
			if (d->chosen_hostname != NULL || d->use_chooser) {
				if ( ! mdm_display_manage (d)) {
					mdm_display_unmanage (d);
				}
			} else {
				/* else, this is a one time thing */
				mdm_display_unmanage (d);
			}
			/* Remote displays will send a request to be managed */
		} else /* TYPE_XDMCP */ {
			mdm_display_unmanage (d);
		}

		break;
	}

	mdm_try_logout_action (d);
	mdm_safe_restart ();

	return TRUE;
}

static void
mdm_restart_now (void)
{
	mdm_info (_("MDM restarting ..."));
	mdm_final_cleanup ();
	mdm_restoreenv ();
	VE_IGNORE_EINTR (execvp (stored_argv[0], stored_argv));
	mdm_error (_("Failed to restart self"));
	_exit (1);
}

static void
mdm_safe_restart (void)
{
	GSList *li;
	GSList *displays;

	displays = mdm_daemon_config_get_display_list ();

	if ( ! mdm_restart_mode)
		return;

	for (li = displays; li != NULL; li = li->next) {
		MdmDisplay *d = li->data;

		if (d->logged_in)
			return;
	}

	mdm_restart_now ();
}

static void
mdm_do_logout_action (MdmLogoutAction logout_action)
{
	switch (logout_action) {
	case MDM_LOGOUT_ACTION_HALT:
		halt_machine ();
		break;

	case MDM_LOGOUT_ACTION_REBOOT:
		restart_machine ();
		break;

	case MDM_LOGOUT_ACTION_SUSPEND:
		suspend_machine ();
		break;

	default:
	        /* This is a bit ugly but its the only place we can
		   check for the range of values */
	        if (logout_action >= MDM_LOGOUT_ACTION_CUSTOM_CMD_FIRST &&
		    logout_action <= MDM_LOGOUT_ACTION_CUSTOM_CMD_LAST)
			custom_cmd (logout_action - MDM_LOGOUT_ACTION_CUSTOM_CMD_FIRST);
		break;
	}
}

static void
mdm_try_logout_action (MdmDisplay *disp)
{
	GSList *li;
	GSList *displays;

	displays = mdm_daemon_config_get_display_list ();

	if (disp != NULL &&
	    disp->logout_action != MDM_LOGOUT_ACTION_NONE &&
	    ! disp->logged_in) {
		mdm_do_logout_action (disp->logout_action);
		disp->logout_action = MDM_LOGOUT_ACTION_NONE;
		return;
	}

	if (safe_logout_action == MDM_LOGOUT_ACTION_NONE)
		return;

	for (li = displays; li != NULL; li = li->next) {
		MdmDisplay *d = li->data;

		if (d->logged_in)
			return;
	}

	mdm_do_logout_action (safe_logout_action);
	safe_logout_action = MDM_LOGOUT_ACTION_NONE;
}

static void
main_daemon_abrt (int sig)
{
	/* FIXME: note that this could mean out of memory */
	mdm_error (_("main daemon: Got SIGABRT. Something went very wrong. Going down!"));
	mdm_final_cleanup ();
	exit (EXIT_FAILURE);
}

static gboolean
mainloop_sig_callback (int sig, gpointer data)
{
	/* signals are at somewhat random times aren't they? */
	mdm_random_tick ();

	mdm_debug ("mainloop_sig_callback: Got signal %d", (int)sig);
	switch (sig)
		{
		case SIGCHLD:
			while (mdm_cleanup_children ())
				;
			break;

		case SIGINT:
		case SIGTERM:
			mdm_debug ("mainloop_sig_callback: Got TERM/INT. Going down!");
			mdm_final_cleanup ();
			exit (EXIT_SUCCESS);
			break;

#ifdef SIGXFSZ
		case SIGXFSZ:
			mdm_error ("main daemon: Hit file size rlimit, restarting!");
			mdm_restart_now ();
			break;
#endif

#ifdef SIGXCPU
		case SIGXCPU:
			mdm_error ("main daemon: Hit CPU rlimit, restarting!");
			mdm_restart_now ();
			break;
#endif

		case SIGHUP:
			mdm_restart_now ();
			break;

		case SIGUSR1:
			mdm_restart_mode = TRUE;
			mdm_safe_restart ();
			break;

		default:
			break;
		}

	return TRUE;
}

/*
 * main: The main daemon control
 */

static void
store_argv (int argc, char *argv[])
{
	int i;

	stored_argv = g_new0 (char *, argc + 1);
	for (i = 0; i < argc; i++)
		stored_argv[i] = g_strdup (argv[i]);
	stored_argv[i] = NULL;
	stored_argc = argc;
}

static void
close_notify (gpointer data)
{
	MdmConnection **conn = data;
	* conn = NULL;
}

static void
create_connections (void)
{
	int p[2];
	gchar *path;

	path = g_build_filename (mdm_daemon_config_get_value_string (MDM_KEY_SERV_AUTHDIR), ".mdmfifo", NULL);
	fifoconn = mdm_connection_open_fifo (path, 0660);
	g_free (path);

	if G_LIKELY (fifoconn != NULL) {
		mdm_connection_set_handler (fifoconn,
					    mdm_handle_message,
					    NULL /* data */,
					    NULL /* destroy_notify */);
		mdm_connection_set_close_notify (fifoconn,
						 &fifoconn,
						 close_notify);
	}

	if G_UNLIKELY (pipe (p) < 0) {
		slave_fifo_pipe_fd = -1;
		pipeconn = NULL;
	} else {
		slave_fifo_pipe_fd = p[1];
		pipeconn = mdm_connection_open_fd (p[0]);
	}

	if G_LIKELY (pipeconn != NULL) {
		mdm_connection_set_handler (pipeconn,
					    mdm_handle_message,
					    NULL /* data */,
					    NULL /* destroy_notify */);
		mdm_connection_set_close_notify (pipeconn,
						 &pipeconn,
						 close_notify);
	} else {
		VE_IGNORE_EINTR (close (p[0]));
		VE_IGNORE_EINTR (close (p[1]));
		slave_fifo_pipe_fd = -1;
	}

	unixconn = mdm_connection_open_unix (MDM_SUP_SOCKET, 0666);

	if G_LIKELY (unixconn != NULL) {
		mdm_connection_set_handler (unixconn,
					    mdm_handle_user_message,
					    NULL /* data */,
					    NULL /* destroy_notify */);
		mdm_connection_set_nonblock (unixconn, TRUE);
		mdm_connection_set_close_notify (unixconn,
						 &unixconn,
						 close_notify);
	}
}

static void
calc_sqrt2 (void)
{
	unsigned long n = 0, h = 0;
	double x;
	printf ("\n");
	for (;;) {
		x = g_random_double_range (1.0, 2.0);
		if (x*x <= 2.0)
			h++;
		n++;
		if ( ! (n & 0xfff)) {
			double sqrttwo = 1.0 + ((double)h)/(double)n;
			printf ("sqrt(2) ~~ %1.10f\t(1 + %lu/%lu) "
				"iteration: %lu \r",
				sqrttwo, h, n, n);
		}
	}
}

GOptionEntry options [] = {
	{ "nodaemon", '\0', 0, G_OPTION_ARG_NONE,
	  &no_daemon, N_("Do not fork into the background"), NULL },
	{ "no-console", '\0', 0, G_OPTION_ARG_NONE,
	  &no_console, N_("No console (static) servers to be run"), NULL },
	{ "config", '\0', 0, G_OPTION_ARG_STRING,
	  &config_file, N_("Alternative MDM System Defaults configuration file"), N_("CONFIGFILE") },
	{ "preserve-ld-vars", '\0', 0, G_OPTION_ARG_NONE,
	  &preserve_ld_vars, N_("Preserve LD_* variables"), NULL },
	{ "version", '\0', 0, G_OPTION_ARG_NONE,
	  &print_version, N_("Print MDM version"), NULL },
	{ "wait-for-go", '\0', 0, G_OPTION_ARG_NONE,
	  &mdm_wait_for_go, N_("Start the first X server but then halt until we get a GO in the fifo"), NULL },
	{ "monte-carlo-sqrt2", 0, 0, G_OPTION_ARG_NONE,
	  &monte_carlo_sqrt2, NULL, NULL },
	{ NULL }
};

static gboolean
linux_only_is_running (pid_t pid)
{
	char *resolved_self;
	char *resolved_running;
	gboolean ret;

	char *running = g_strdup_printf ("/proc/%lu/exe", (gulong)pid);

	if ((resolved_self = realpath ("/proc/self/exe", NULL)) == NULL) {
		g_free (running);
		/* probably not a linux system */
		return TRUE;
	}

	if ((resolved_running = realpath (running, NULL)) == NULL) {
		g_free (running);
		free (resolved_self);
		/* probably not a linux system */
		return TRUE;
	}

	g_free (running);

	ret = FALSE;
	if (strcmp (resolved_running, resolved_self) == 0)
		ret = TRUE;
	free (resolved_self);
	free (resolved_running);
	return ret;
}

static void
ensure_desc_012 (void)
{
	int fd;
	/* We here ensure descriptors 0, 1 and 2
	 * we of course count on the fact that open
	 * opens the lowest available descriptor */
	for (;;) {
		fd = mdm_open_dev_null (O_RDWR);
		/* Once we are up to 3, we're beyond stdin,
		 * stdout and stderr */
		if (fd >= 3) {
			VE_IGNORE_EINTR (close (fd));
			break;
		}
	}
}

/* initially if we get a TERM or INT we just want to die,
   but we want to also kill an extra process if it exists */
static void
initial_term_int (int signal)
{
	if (extra_process > 1)
		kill (-(extra_process), SIGTERM);
	_exit (EXIT_FAILURE);
}

static void
mdm_make_global_cookie (void)
{
	FILE *fp;
	char *file;

	mdm_cookie_generate ((char **)&mdm_global_cookie, (char **)&mdm_global_bcookie);

	file = g_build_filename (mdm_daemon_config_get_value_string (MDM_KEY_SERV_AUTHDIR), ".cookie", NULL);
	VE_IGNORE_EINTR (g_unlink (file));

	fp = mdm_safe_fopen_w (file, 0600);
	if G_UNLIKELY (fp == NULL) {
		mdm_error (_("Can't open %s for writing"), file);
		g_free (file);
		return;
	}

	VE_IGNORE_EINTR (fprintf (fp, "%s\n", mdm_global_cookie));

	/* FIXME: What about out of disk space errors? */
	errno = 0;
	VE_IGNORE_EINTR (fclose (fp));
	if G_UNLIKELY (errno != 0) {
		mdm_error (_("Can't write to %s: %s"), file,
			   strerror (errno));
	}

	g_free (file);
}

int
main (int argc, char *argv[])
{
	FILE *pf;
	sigset_t mask;
	struct sigaction sig, child, abrt;
	GOptionContext *ctx;
	const char *pidfile;
	int i;

	/* semi init pseudorandomness */
	mdm_random_tick ();

	mdm_main_pid = getpid ();

	/* We here ensure descriptors 0, 1 and 2 */
	ensure_desc_012 ();

	/* store all initial stuff, args, env, rlimits, runlevel */
	store_argv (argc, argv);
	mdm_saveenv ();
	mdm_get_initial_limits ();

	bindtextdomain (GETTEXT_PACKAGE, GNOMELOCALEDIR);
	textdomain (GETTEXT_PACKAGE);

	setlocale (LC_ALL, "");

	/* Initialize runtime environment */
	umask (022);

	g_type_init ();

	ctx = g_option_context_new (_("- The GNOME login manager"));
	g_option_context_add_main_entries (ctx, options, _("main options"));

	/* preprocess the arguments to support the xdm style -nodaemon
	 * option
	 */
	for (i = 0; i < argc; i++) {
		if (strcmp (argv[i], "-nodaemon") == 0)
			argv[i] = (char *) "--nodaemon";
	}

	g_option_context_parse (ctx, &argc, &argv, NULL);
	g_option_context_free (ctx);

	if (monte_carlo_sqrt2) {
		calc_sqrt2 ();
		return 0;
	}

	if (print_version) {
		printf ("MDM %s\n", VERSION);
		fflush (stdout);
		exit (0);
	}

	mdm_log_init ();
	/* Parse configuration file */
	mdm_daemon_config_parse (config_file, no_console);

	/* XDM compliant error message */
	if G_UNLIKELY (getuid () != 0) {
		/* make sure the pid file doesn't get wiped */
		mdm_error (_("Only root wants to run MDM\n"));
		exit (-1);
	}

	main_loop = g_main_loop_new (NULL, FALSE);

	mdm_system_locale = g_strdup (setlocale (LC_MESSAGES, NULL));
	/* initial TERM/INT handler */
	sig.sa_handler = initial_term_int;
	sig.sa_flags = SA_RESTART;
	sigemptyset (&sig.sa_mask);

	/*
	 * Do not call mdm_fail before calling mdm_config_parse ()
	 * since the mdm_fail function uses config data
	 */
	if G_UNLIKELY (sigaction (SIGTERM, &sig, NULL) < 0)
		mdm_fail (_("%s: Error setting up %s signal handler: %s"),
			  "main", "TERM", strerror (errno));

	if G_UNLIKELY (sigaction (SIGINT, &sig, NULL) < 0)
		mdm_fail (_("%s: Error setting up %s signal handler: %s"),
			  "main", "INT", strerror (errno));

	/* get the name of the root user */
	mdm_root_user ();

	pidfile = MDM_PID_FILE;

	/* Check if another mdm process is already running */
	if (g_access (pidfile, R_OK) == 0) {

		/* Check if the existing process is still alive. */
		gint pidv;

		pf = fopen (pidfile, "r");

		if (pf != NULL &&
		    fscanf (pf, "%d", &pidv) == 1 &&
		    kill (pidv, 0) == 0 &&
		    linux_only_is_running (pidv)) {
			/* make sure the pid file doesn't get wiped */
			VE_IGNORE_EINTR (fclose (pf));
			mdm_fail (_("MDM already running. Aborting!"));
		}

		if (pf != NULL)
			VE_IGNORE_EINTR (fclose (pf));
	}

	/* Become daemon unless started in -nodaemon mode or child of init */
	if (no_daemon || getppid () == 1) {

		/* Write pid to pidfile */
		errno = 0;
		if ((pf = mdm_safe_fopen_w (pidfile, 0644)) != NULL) {
			errno = 0;
			VE_IGNORE_EINTR (fprintf (pf, "%d\n", (int)getpid ()));
			VE_IGNORE_EINTR (fclose (pf));
			if (errno != 0) {
				/* FIXME: how to handle this? */
				mdm_fdprintf (2, _("Cannot write PID file %s: possibly out of diskspace.  Error: %s\n"),
					      pidfile, strerror (errno));
				mdm_error (_("Cannot write PID file %s: possibly out of diskspace.  Error: %s"),
					   pidfile, strerror (errno));

			}
		} else if (errno != 0) {
			/* FIXME: how to handle this? */
			mdm_fdprintf (2, _("Cannot write PID file %s: possibly out of diskspace.  Error: %s\n"),
				      pidfile, strerror (errno));
			mdm_error (_("Cannot write PID file %s: possibly out of diskspace.  Error: %s"),
				   pidfile, strerror (errno));

		}

		VE_IGNORE_EINTR (g_chdir (mdm_daemon_config_get_value_string (MDM_KEY_SERV_AUTHDIR)));
		umask (022);
	}
	else
		mdm_daemonify ();

#ifdef __sun
	{
		struct stat statbuf;
		int r;

		r = stat (MDM_DT_DIR, &statbuf);
		if (r < 0) {
			g_mkdir (MDM_DT_DIR, 0755);
		}
		
		g_remove (MDM_SDTLOGIN_DIR);
		g_mkdir (MDM_SDTLOGIN_DIR, 0700);
	}
#endif

	/* Signal handling */
	ve_signal_add (SIGCHLD, mainloop_sig_callback, NULL);
	ve_signal_add (SIGTERM, mainloop_sig_callback, NULL);
	ve_signal_add (SIGINT,  mainloop_sig_callback, NULL);
	ve_signal_add (SIGHUP,  mainloop_sig_callback, NULL);
	ve_signal_add (SIGUSR1, mainloop_sig_callback, NULL);

	sig.sa_handler = ve_signal_notify;
	sig.sa_flags = SA_RESTART;
	sigemptyset (&sig.sa_mask);

	if G_UNLIKELY (sigaction (SIGTERM, &sig, NULL) < 0)
		mdm_fail (_("%s: Error setting up %s signal handler: %s"),
			  "main", "TERM", strerror (errno));

	if G_UNLIKELY (sigaction (SIGINT, &sig, NULL) < 0)
		mdm_fail (_("%s: Error setting up %s signal handler: %s"),
			  "main", "INT", strerror (errno));

	if G_UNLIKELY (sigaction (SIGHUP, &sig, NULL) < 0)
		mdm_fail (_("%s: Error setting up %s signal handler: %s"),
			  "main", "HUP", strerror (errno));

	if G_UNLIKELY (sigaction (SIGUSR1, &sig, NULL) < 0)
		mdm_fail (_("%s: Error setting up %s signal handler: %s"),
			  "main", "USR1", strerror (errno));

	/* some process limit signals we catch and restart on,
	   note that we don't catch these in the slave, but then
	   we catch those in the main daemon as slave crashing
	   (terminated by signal), and we clean up appropriately */
#ifdef SIGXCPU
	ve_signal_add (SIGXCPU, mainloop_sig_callback, NULL);
	if G_UNLIKELY (sigaction (SIGXCPU, &sig, NULL) < 0)
		mdm_fail (_("%s: Error setting up %s signal handler: %s"),
			  "main", "XCPU", strerror (errno));
#endif
#ifdef SIGXFSZ
	ve_signal_add (SIGXFSZ, mainloop_sig_callback, NULL);
	if G_UNLIKELY (sigaction (SIGXFSZ, &sig, NULL) < 0)
		mdm_fail (_("%s: Error setting up %s signal handler: %s"),
			  "main", "XFSZ", strerror (errno));
#endif

	/* cannot use mainloop for SIGABRT, the handler can never
	   return */
	abrt.sa_handler = main_daemon_abrt;
	abrt.sa_flags = SA_RESTART;
	sigemptyset (&abrt.sa_mask);

	if G_UNLIKELY (sigaction (SIGABRT, &abrt, NULL) < 0)
		mdm_fail (_("%s: Error setting up %s signal handler: %s"),
			  "main", "ABRT", strerror (errno));

	child.sa_handler = ve_signal_notify;
	child.sa_flags = SA_RESTART|SA_NOCLDSTOP;
	sigemptyset (&child.sa_mask);
	sigaddset (&child.sa_mask, SIGCHLD);

	if G_UNLIKELY (sigaction (SIGCHLD, &child, NULL) < 0)
		mdm_fail (_("%s: Error setting up CHLD signal handler"), "mdm_main");

	sigemptyset (&mask);
	sigaddset (&mask, SIGINT);
	sigaddset (&mask, SIGTERM);
	sigaddset (&mask, SIGCHLD);
	sigaddset (&mask, SIGHUP);
	sigaddset (&mask, SIGUSR1);
	sigaddset (&mask, SIGABRT);
#ifdef SIGXCPU
	sigaddset (&mask, SIGXCPU);
#endif
#ifdef SIGXFSZ
	sigaddset (&mask, SIGXFSZ);
#endif
	sigprocmask (SIG_UNBLOCK, &mask, NULL);

	mdm_signal_ignore (SIGUSR2);
	mdm_signal_ignore (SIGPIPE);

	/* ignore power failures, up to user processes to
	 * handle things correctly */
#ifdef SIGPWR
	mdm_signal_ignore (SIGPWR);
#endif
	/* can we ever even get this one? */
#ifdef SIGLOST
	mdm_signal_ignore (SIGLOST);
#endif

	mdm_debug ("mdm_main: Here we go...");

	/* Init XDMCP if applicable */
	if (mdm_daemon_config_get_value_bool (MDM_KEY_XDMCP) && ! mdm_wait_for_go) {
		mdm_xdmcp_init ();
	}

	create_connections ();

	/* make sure things (currently /tmp/.ICE-unix and /tmp/.X11-unix)
	 * are sane */
	mdm_ensure_sanity () ;

	/* Make us a unique global cookie to authenticate */
	mdm_make_global_cookie ();

	/* Start static X servers */
	mdm_start_first_unborn_local (0 /* delay */);

	/* Accept remote connections */
	if (mdm_daemon_config_get_value_bool (MDM_KEY_XDMCP) && ! mdm_wait_for_go) {
		mdm_debug ("Accepting XDMCP connections...");
		mdm_xdmcp_run ();
	}

	/* We always exit via exit (), and sadly we need to g_main_quit ()
	 * at times not knowing if it's this main or a recursive one we're
	 * quitting.
	 */
	while (1)
		{
			g_main_loop_run (main_loop);
			mdm_debug ("main: Exited main loop");
		}

	return EXIT_SUCCESS;	/* Not reached */
}

static gboolean
order_exists (int order)
{
	GSList *li;
	GSList *displays;

	displays = mdm_daemon_config_get_display_list ();
	for (li = displays; li != NULL; li = li->next) {
		MdmDisplay *d = li->data;
		if (d->x_servers_order == order)
			return TRUE;
	}
	return FALSE;
}

static int
get_new_order (MdmDisplay *d)
{
	int order;
	GSList *li;
	GSList *displays;

	displays = mdm_daemon_config_get_display_list ();
	/* first try the position in the 'displays' list as
	 * our order */
	for (order = 0, li = displays; li != NULL; order++, li = li->next) {
		if (li->data == d)
			break;
	}
	/* next make sure it's unique */
	while (order_exists (order))
		order++;
	return order;
}

static void
write_x_servers (MdmDisplay *d)
{
	FILE *fp;
	char *file = mdm_make_filename (mdm_daemon_config_get_value_string (MDM_KEY_SERV_AUTHDIR), d->name, ".Xservers");
	int i;
	int bogusname;

	if (d->x_servers_order < 0)
		d->x_servers_order = get_new_order (d);

	fp = mdm_safe_fopen_w (file, 0644);
	if G_UNLIKELY (fp == NULL) {
		mdm_error (_("Can't open %s for writing"), file);
		g_free (file);
		return;
	}

	for (bogusname = 0, i = 0; i < d->x_servers_order; bogusname++, i++) {
		char buf[32];
		g_snprintf (buf, sizeof (buf), ":%d", bogusname);
		if (strcmp (buf, d->name) == 0)
			g_snprintf (buf, sizeof (buf), ":%d", ++bogusname);
		VE_IGNORE_EINTR (fprintf (fp, "%s local /usr/X11R6/bin/Xbogus\n", buf));
	}

	if (SERVER_IS_LOCAL (d)) {
		char    **argv;
		char     *command;
		int       argc;
		gboolean  rc;

		argc = 0;
		argv = NULL;
		rc   = mdm_server_resolve_command_line (d,
						        FALSE, /* resolve_flags */
						        NULL, /* vtarg */
						        &argc,
						        &argv);

		if (rc == FALSE) {
			g_free (file);
			return;
		}

		command = g_strjoinv (" ", argv);
		g_strfreev (argv);
		VE_IGNORE_EINTR (fprintf (fp, "%s local %s\n", d->name, command));
		g_free (command);
	} else {
		VE_IGNORE_EINTR (fprintf (fp, "%s foreign\n", d->name));
	}

	/* FIXME: What about out of disk space errors? */
	errno = 0;
	VE_IGNORE_EINTR (fclose (fp));
	if G_UNLIKELY (errno != 0) {
		mdm_error (_("Can't write to %s: %s"), file,
			   strerror (errno));
	}

	g_free (file);
}

static void
send_slave_ack_dialog_int (MdmDisplay *d, int type, int response)
{
	if (d->master_notify_fd >= 0) {
		char *not;

		not = g_strdup_printf ("%c%c%d\n", MDM_SLAVE_NOTIFY_RESPONSE, type, response);
		VE_IGNORE_EINTR (write (d->master_notify_fd, not, strlen (not)));

		g_free (not);
	}
	if (d->slavepid > 1) {
		/* now yield the CPU as the other process has more
		   useful work to do then we do */
#if defined (_POSIX_PRIORITY_SCHEDULING) && defined (HAVE_SCHED_YIELD)
		sched_yield ();
#endif
	}
}

static void
send_slave_ack_dialog_char (MdmDisplay *d, int type, const char *resp)
{
	if (d->master_notify_fd >= 0) {
		if (resp == NULL) {
			char not[3];

			not[0] = MDM_SLAVE_NOTIFY_RESPONSE;
			not[1] = type;
			not[2] = '\n';
			VE_IGNORE_EINTR (write (d->master_notify_fd, not, 3));
		} else {
			char *not = g_strdup_printf ("%c%c%s\n",
						     MDM_SLAVE_NOTIFY_RESPONSE,
						     type,
						     resp);
			VE_IGNORE_EINTR (write (d->master_notify_fd, not, strlen (not)));
			g_free (not);
		}
	}
	if (d->slavepid > 1) {
		/* now yield the CPU as the other process has more
		   useful work to do then we do */
#if defined (_POSIX_PRIORITY_SCHEDULING) && defined (HAVE_SCHED_YIELD)
		sched_yield ();
#endif
	}
}

static void
send_slave_ack (MdmDisplay *d, const char *resp)
{
	if (d->master_notify_fd >= 0) {
		if (resp == NULL) {
			char not[2];
			not[0] = MDM_SLAVE_NOTIFY_ACK;
			not[1] = '\n';
			VE_IGNORE_EINTR (write (d->master_notify_fd, not, 2));
		} else {
			char *not = g_strdup_printf ("%c%s\n",
						     MDM_SLAVE_NOTIFY_ACK,
						     resp);
			VE_IGNORE_EINTR (write (d->master_notify_fd, not, strlen (not)));
			g_free (not);
		}
	}
	if (d->slavepid > 1) {
		kill (d->slavepid, SIGUSR2);
		/* now yield the CPU as the other process has more
		   useful work to do then we do */
#if defined (_POSIX_PRIORITY_SCHEDULING) && defined (HAVE_SCHED_YIELD)
		sched_yield ();
#endif
	}
}

static void
send_slave_command (MdmDisplay *d, const char *command)
{
	if (d->master_notify_fd >= 0) {
		char *cmd = g_strdup_printf ("%c%s\n",
					     MDM_SLAVE_NOTIFY_COMMAND,
					     command);
		VE_IGNORE_EINTR (write (d->master_notify_fd, cmd, strlen (cmd)));
		g_free (cmd);
	}
	if (d->slavepid > 1) {
		kill (d->slavepid, SIGUSR2);
		/* now yield the CPU as the other process has more
		   useful work to do then we do */
#if defined (_POSIX_PRIORITY_SCHEDULING) && defined (HAVE_SCHED_YIELD)
		sched_yield ();
#endif
	}
}

static void
mdm_handle_message (MdmConnection *conn, const char *msg, gpointer data)
{
	/* Evil!, all this for debugging? */
	if G_UNLIKELY (mdm_daemon_config_get_value_bool (MDM_KEY_DEBUG)) {
		if (strncmp (msg, MDM_SOP_COOKIE " ",
			     strlen (MDM_SOP_COOKIE " ")) == 0) {
			char *s = g_strndup
				(msg, strlen (MDM_SOP_COOKIE " XXXX XX"));
			/* cut off most of the cookie for "security" */
			mdm_debug ("Handling message: '%s...'", s);
			g_free (s);
		} else if (strncmp (msg, MDM_SOP_SYSLOG " ",
				    strlen (MDM_SOP_SYSLOG " ")) != 0) {
			/* Don't print out the syslog message as it will
			 * be printed out anyway as that's the whole point
			 * of the message. */
			mdm_debug ("Handling message: '%s'", msg);
		}
	}

	if (strncmp (msg, MDM_SOP_CHOSEN " ",
		     strlen (MDM_SOP_CHOSEN " ")) == 0) {
		mdm_choose_data (msg);
	} else if (strncmp (msg, MDM_SOP_CHOSEN_LOCAL " ",
		            strlen (MDM_SOP_CHOSEN_LOCAL " ")) == 0) {
		MdmDisplay *d;
		long slave_pid;
		char *p;

		if (sscanf (msg, MDM_SOP_CHOSEN_LOCAL " %ld", &slave_pid)
		    != 1)
			return;
		p = strchr (msg, ' ');
		if (p != NULL)
			p = strchr (p+1, ' ');
		if (p == NULL)
			return;
		p++;

		/* Find out who this slave belongs to */
		d = mdm_display_lookup (slave_pid);

		if (d != NULL) {
			g_free (d->chosen_hostname);
			d->chosen_hostname = g_strdup (p);
			mdm_debug ("Got CHOSEN_LOCAL == %s", p);
			/* send ack */
			send_slave_ack (d, NULL);
		}
	} else if (strncmp (msg, MDM_SOP_XPID " ",
		            strlen (MDM_SOP_XPID " ")) == 0) {
		MdmDisplay *d;
		long slave_pid, pid;

		if (sscanf (msg, MDM_SOP_XPID " %ld %ld", &slave_pid, &pid)
		    != 2)
			return;

		/* Find out who this slave belongs to */
		d = mdm_display_lookup (slave_pid);

		if (d != NULL) {
			d->servpid = pid;
			mdm_debug ("Got XPID == %ld", (long)pid);
			/* send ack */
			send_slave_ack (d, NULL);
		}
	} else if (strncmp (msg, MDM_SOP_SESSPID " ",
		            strlen (MDM_SOP_SESSPID " ")) == 0) {
		MdmDisplay *d;
		long slave_pid, pid;

		if (sscanf (msg, MDM_SOP_SESSPID " %ld %ld", &slave_pid, &pid)
		    != 2)
			return;

		/* Find out who this slave belongs to */
		d = mdm_display_lookup (slave_pid);

		if (d != NULL) {
			d->sesspid = pid;
			mdm_debug ("Got SESSPID == %ld", (long)pid);
			/* send ack */
			send_slave_ack (d, NULL);
		}
	} else if (strncmp (msg, MDM_SOP_GREETPID " ",
		            strlen (MDM_SOP_GREETPID " ")) == 0) {
		MdmDisplay *d;
		long slave_pid, pid;

		if (sscanf (msg, MDM_SOP_GREETPID " %ld %ld", &slave_pid, &pid)
		    != 2)
			return;

		/* Find out who this slave belongs to */
		d = mdm_display_lookup (slave_pid);

		if (d != NULL) {
			d->greetpid = pid;
			mdm_debug ("Got GREETPID == %ld", (long)pid);
			/* send ack */
			send_slave_ack (d, NULL);
		}
	} else if (strncmp (msg, MDM_SOP_CHOOSERPID " ",
		            strlen (MDM_SOP_CHOOSERPID " ")) == 0) {
		MdmDisplay *d;
		long slave_pid, pid;

		if (sscanf (msg, MDM_SOP_CHOOSERPID " %ld %ld",
			    &slave_pid, &pid) != 2)
			return;

		/* Find out who this slave belongs to */
		d = mdm_display_lookup (slave_pid);

		if (d != NULL) {
			d->chooserpid = pid;
			mdm_debug ("Got CHOOSERPID == %ld", (long)pid);
			/* send ack */
			send_slave_ack (d, NULL);
		}
	} else if (strncmp (msg, MDM_SOP_LOGGED_IN " ",
		            strlen (MDM_SOP_LOGGED_IN " ")) == 0) {
		MdmDisplay *d;
		long slave_pid;
		int logged_in;
		if (sscanf (msg, MDM_SOP_LOGGED_IN " %ld %d", &slave_pid,
			    &logged_in) != 2)
			return;

		/* Find out who this slave belongs to */
		d = mdm_display_lookup (slave_pid);

		if (d != NULL) {
			d->logged_in = logged_in ? TRUE : FALSE;
			mdm_debug ("Got logged in == %s",
				   d->logged_in ? "TRUE" : "FALSE");

			/* whack connections about this display if a user
			 * just logged out since we don't want such
			 * connections persisting to be authenticated */
			if ( ! logged_in && unixconn != NULL)
				mdm_kill_subconnections_with_display (unixconn, d);

			/* if the user just logged out,
			 * let's see if it's safe to restart */
			if ( ! d->logged_in) {
				mdm_try_logout_action (d);
				mdm_safe_restart ();
			}

			/* send ack */
			send_slave_ack (d, NULL);
		}
	} else if (strncmp (msg, MDM_SOP_DISP_NUM " ",
		            strlen (MDM_SOP_DISP_NUM " ")) == 0) {
		MdmDisplay *d;
		long slave_pid;
		int disp_num;

		if (sscanf (msg, MDM_SOP_DISP_NUM " %ld %d",
			    &slave_pid, &disp_num) != 2)
			return;

		/* Find out who this slave belongs to */
		d = mdm_display_lookup (slave_pid);

		if (d != NULL) {
			g_free (d->name);
			d->name = g_strdup_printf (":%d", disp_num);
			d->dispnum = disp_num;
			mdm_debug ("Got DISP_NUM == %d", disp_num);
			/* send ack */
			send_slave_ack (d, NULL);
		}
	} else if (strncmp (msg, MDM_SOP_VT_NUM " ",
		            strlen (MDM_SOP_VT_NUM " ")) == 0) {
		MdmDisplay *d;
		long slave_pid;
		int vt_num;

		if (sscanf (msg, MDM_SOP_VT_NUM " %ld %d",
			    &slave_pid, &vt_num) != 2)
			return;

		/* Find out who this slave belongs to */
		d = mdm_display_lookup (slave_pid);

		if (d != NULL) {
			d->vt = vt_num;
			mdm_debug ("Got VT_NUM == %d", vt_num);
			/* send ack */
			send_slave_ack (d, NULL);
		}
	} else if (strncmp (msg, MDM_SOP_LOGIN " ",
		            strlen (MDM_SOP_LOGIN " ")) == 0) {
		MdmDisplay *d;
		long slave_pid;
		char *p;

		if (sscanf (msg, MDM_SOP_LOGIN " %ld",
			    &slave_pid) != 1)
			return;
		p = strchr (msg, ' ');
		if (p != NULL)
			p = strchr (p+1, ' ');
		if (p == NULL)
			return;

		p++;

		/* Find out who this slave belongs to */
		d = mdm_display_lookup (slave_pid);

		if (d != NULL) {
			g_free (d->login);
			d->login = g_strdup (p);
			mdm_debug ("Got LOGIN == %s", p);
			/* send ack */
			send_slave_ack (d, NULL);
		}
	} else if (strncmp (msg, MDM_SOP_QUERYLOGIN " ",
		            strlen (MDM_SOP_QUERYLOGIN " ")) == 0) {
		MdmDisplay *d;
		long slave_pid;
		char *p;

		if (sscanf (msg, MDM_SOP_QUERYLOGIN " %ld",
			    &slave_pid) != 1)
			return;
		p = strchr (msg, ' ');
		if (p != NULL)
			p = strchr (p+1, ' ');
		if (p == NULL)
			return;

		p++;

		/* Find out who this slave belongs to */
		d = mdm_display_lookup (slave_pid);

		if (d != NULL) {
			GString *resp = NULL;
			GSList *li;
			GSList *displays;

			displays = mdm_daemon_config_get_display_list ();
			mdm_debug ("Got QUERYLOGIN %s", p);
			for (li = displays; li != NULL; li = li->next) {
				MdmDisplay *di = li->data;
				if (di->logged_in &&
				    di->login != NULL &&
				    strcmp (di->login, p) == 0) {
					gboolean migratable = FALSE;

					if (resp == NULL)
						resp = g_string_new (NULL);
					else
						resp = g_string_append_c (resp, ',');

					g_string_append (resp, di->name);
					g_string_append_c (resp, ',');

					if (d->attached && di->attached && di->vt > 0)
						migratable = TRUE;
					else if (mdm_daemon_config_get_value_string (MDM_KEY_XDMCP_PROXY_RECONNECT) != NULL &&
						 d->type == TYPE_XDMCP_PROXY && di->type == TYPE_XDMCP_PROXY)
						migratable = TRUE;

					g_string_append_c (resp, migratable ? '1' : '0');
				}
			}

			/* send ack */
			if (resp != NULL) {
				send_slave_ack (d, resp->str);
				g_string_free (resp, TRUE);
			} else {
				send_slave_ack (d, NULL);
			}
		}
	} else if (strncmp (msg, MDM_SOP_MIGRATE " ",
		            strlen (MDM_SOP_MIGRATE " ")) == 0) {
		MdmDisplay *d;
		long slave_pid;
		char *p;
		GSList *li;
		GSList *displays;

		displays = mdm_daemon_config_get_display_list ();

		if (sscanf (msg, MDM_SOP_MIGRATE " %ld", &slave_pid) != 1)
			return;
		p = strchr (msg, ' ');
		if (p != NULL)
			p = strchr (p+1, ' ');
		if (p == NULL)
			return;

		p++;

		/* Find out who this slave belongs to */
		d = mdm_display_lookup (slave_pid);
		if (d == NULL)
			return;

		mdm_debug ("Got MIGRATE %s", p);
		for (li = displays; li != NULL; li = li->next) {
			MdmDisplay *di = li->data;
			if (di->logged_in && strcmp (di->name, p) == 0) {
				if (d->attached && di->vt > 0)
					mdm_change_vt (di->vt);
				else if (d->type == TYPE_XDMCP_PROXY && di->type == TYPE_XDMCP_PROXY)
					mdm_xdmcp_migrate (d, di);
			}
		}
		send_slave_ack (d, NULL);
	} else if (strncmp (msg, MDM_SOP_COOKIE " ",
		            strlen (MDM_SOP_COOKIE " ")) == 0) {
		MdmDisplay *d;
		long slave_pid;
		char *p;

		if (sscanf (msg, MDM_SOP_COOKIE " %ld",
			    &slave_pid) != 1)
			return;
		p = strchr (msg, ' ');
		if (p != NULL)
			p = strchr (p+1, ' ');
		if (p == NULL)
			return;

		p++;

		/* Find out who this slave belongs to */
		d = mdm_display_lookup (slave_pid);

		if (d != NULL) {
			g_free (d->cookie);
			d->cookie = g_strdup (p);
			mdm_debug ("Got COOKIE == <secret>");
			/* send ack */
			send_slave_ack (d, NULL);
		}
	} else if (strncmp (msg, MDM_SOP_AUTHFILE " ",
		            strlen (MDM_SOP_AUTHFILE " ")) == 0) {
		MdmDisplay *d;
		long slave_pid;
		char *p;

		if (sscanf (msg, MDM_SOP_AUTHFILE " %ld",
			    &slave_pid) != 1)
			return;
		p = strchr (msg, ' ');
		if (p != NULL)
			p = strchr (p+1, ' ');
		if (p == NULL)
			return;

		p++;

		/* Find out who this slave belongs to */
		d = mdm_display_lookup (slave_pid);

		if (d != NULL) {
			g_free (d->authfile);
			d->authfile = g_strdup (p);
			mdm_debug ("Got AUTHFILE == %s", d->authfile);
			/* send ack */
			send_slave_ack (d, NULL);
		}
	} else if (strncmp (msg, MDM_SOP_FLEXI_ERR " ",
		            strlen (MDM_SOP_FLEXI_ERR " ")) == 0) {
		MdmDisplay *d;
		long slave_pid;
		int err;

		if (sscanf (msg, MDM_SOP_FLEXI_ERR " %ld %d",
			    &slave_pid, &err) != 2)
			return;

		/* Find out who this slave belongs to */
		d = mdm_display_lookup (slave_pid);

		if (d != NULL) {
			char *error = NULL;
			MdmConnection *conn = d->socket_conn;
			d->socket_conn = NULL;

			if (conn != NULL)
				mdm_connection_set_close_notify (conn,
								 NULL, NULL);

			if (err == 3)
				error = "ERROR 3 X failed\n";
			else if (err == 4)
				error = "ERROR 4 X too busy\n";
			else if (err == 5)
				error = "ERROR 5 Nested display can't connect\n";
			else
				error = "ERROR 999 Unknown error\n";
			if (conn != NULL)
				mdm_connection_write (conn, error);

			mdm_debug ("Got FLEXI_ERR == %d", err);
			/* send ack */
			send_slave_ack (d, NULL);
		}
	} else if (strncmp (msg, MDM_SOP_FLEXI_OK " ",
		            strlen (MDM_SOP_FLEXI_OK " ")) == 0) {
		MdmDisplay *d;
		long slave_pid;

		if (sscanf (msg, MDM_SOP_FLEXI_OK " %ld",
			    &slave_pid) != 1)
			return;

		/* Find out who this slave belongs to */
		d = mdm_display_lookup (slave_pid);

		if (d != NULL) {
			MdmConnection *conn = d->socket_conn;
			d->socket_conn = NULL;

			if (conn != NULL) {
				mdm_connection_set_close_notify (conn,
								 NULL, NULL);
				if ( ! mdm_connection_printf (conn, "OK %s\n", d->name))
					mdm_display_unmanage (d);
			}

			mdm_debug ("Got FLEXI_OK");
			/* send ack */
			send_slave_ack (d, NULL);
		}
	} else if (strcmp (msg, MDM_SOP_SOFT_RESTART) == 0) {
		mdm_restart_mode = TRUE;
		mdm_safe_restart ();
	} else if (strcmp (msg, MDM_SOP_DIRTY_SERVERS) == 0) {
		GSList *li;
		GSList *displays;

		displays = mdm_daemon_config_get_display_list ();
		for (li = displays; li != NULL; li = li->next) {
			MdmDisplay *d = li->data;
			send_slave_command (d, MDM_NOTIFY_DIRTY_SERVERS);
		}
	} else if (strcmp (msg, MDM_SOP_SOFT_RESTART_SERVERS) == 0) {
		GSList *li;
		GSList *displays;

		displays = mdm_daemon_config_get_display_list ();
		for (li = displays; li != NULL; li = li->next) {
			MdmDisplay *d = li->data;
			send_slave_command (d, MDM_NOTIFY_SOFT_RESTART_SERVERS);
		}
	} else if (strncmp (msg, MDM_SOP_SYSLOG " ",
		            strlen (MDM_SOP_SYSLOG " ")) == 0) {
		char *p;
		long pid;
	       	int type;
		p = strchr (msg, ' ');
		if (p == NULL)
			return;
		p++;
		if (sscanf (p, "%ld", &pid) != 1)
			return;
		p = strchr (p, ' ');
		if (p == NULL)
			return;
		p++;
		if (sscanf (p, "%d", &type) != 1)
			return;

		p = strchr (p, ' ');
		if (p == NULL)
			return;
		p++;

		/* FIXME: use g_critical or mdm_debug when required */
		mdm_debug ("(child %ld) %s", pid, p);
	} else if (strcmp (msg, MDM_SOP_START_NEXT_LOCAL) == 0) {
		mdm_start_first_unborn_local (3 /* delay */);
	} else if (strcmp (msg, MDM_SOP_HUP_ALL_GREETERS) == 0) {
		/* probably shouldn't be done too often */
		GSList *li;
		GSList *displays;

		displays = mdm_daemon_config_get_display_list ();
		for (li = displays; li != NULL; li = li->next) {
			MdmDisplay *d = li->data;
			if (d->greetpid > 1)
				kill (d->greetpid, SIGHUP);
			else if (d->chooserpid > 1)
				kill (d->chooserpid, SIGHUP);
		}
	} else if (strcmp (msg, MDM_SOP_GO) == 0) {
		GSList *li;
		gboolean old_wait = mdm_wait_for_go;
		GSList *displays;

		displays = mdm_daemon_config_get_display_list ();
		mdm_wait_for_go = FALSE;
		for (li = displays; li != NULL; li = li->next) {
			MdmDisplay *d = li->data;
			send_slave_command (d, MDM_NOTIFY_GO);
		}
		/* Init XDMCP if applicable */
		if (old_wait && mdm_daemon_config_get_value_bool (MDM_KEY_XDMCP)) {
			if (mdm_xdmcp_init ()) {
				mdm_debug ("Accepting XDMCP connections...");
				mdm_xdmcp_run ();
			}
		}
	} else if (strncmp (msg, MDM_SOP_WRITE_X_SERVERS " ",
		            strlen (MDM_SOP_WRITE_X_SERVERS " ")) == 0) {
		MdmDisplay *d;
		long slave_pid;

		if (sscanf (msg, MDM_SOP_WRITE_X_SERVERS " %ld",
			    &slave_pid) != 1)
			return;

		/* Find out who this slave belongs to */
		d = mdm_display_lookup (slave_pid);

		if (d != NULL) {
			write_x_servers (d);

			/* send ack */
			send_slave_ack (d, NULL);
		}
	} else if (strncmp (msg, MDM_SOP_SUSPEND_MACHINE " ",
			    strlen (MDM_SOP_SUSPEND_MACHINE " ")) == 0) {
		MdmDisplay *d;
		long slave_pid;
		gboolean sysmenu;

		if (sscanf (msg, MDM_SOP_SUSPEND_MACHINE " %ld", &slave_pid) != 1)
			return;
		d = mdm_display_lookup (slave_pid);

		mdm_info (_("Master suspending..."));

		sysmenu = mdm_daemon_config_get_value_bool_per_display (MDM_KEY_SYSTEM_MENU, d->name);
		if (sysmenu && mdm_daemon_config_get_value_string_array (MDM_KEY_SUSPEND) != NULL) {
			suspend_machine ();
		}
	} else if (strncmp (msg, MDM_SOP_CHOSEN_THEME " ",
		            strlen (MDM_SOP_CHOSEN_THEME " ")) == 0) {
		MdmDisplay *d;
		long slave_pid;
		const char *p;

		if (sscanf (msg, MDM_SOP_CHOSEN_THEME " %ld", &slave_pid) != 1)
			return;
		d = mdm_display_lookup (slave_pid);

		if (d != NULL) {
			g_free (d->theme_name);
			d->theme_name = NULL;

			/* Syntax errors are partially OK here, if there
			   was no theme argument we just wanted to clear the
			   theme field */
			p = strchr (msg, ' ');
			if (p != NULL) {
				p = strchr (p+1, ' ');
				if (p != NULL) {
					while (*p == ' ')
						p++;
					if ( ! ve_string_empty (p))
						d->theme_name = g_strdup (p);
				}
			}

			send_slave_ack (d, NULL);
		}
	} else if (strncmp (msg, MDM_SOP_CUSTOM_CMD " ",
		            strlen (MDM_SOP_CUSTOM_CMD " ")) == 0) {
		MdmDisplay *d;
		long slave_pid;
		long cmd_id;

		if (sscanf (msg, MDM_SOP_CUSTOM_CMD " %ld %ld", &slave_pid, &cmd_id) != 2)
			return;
		d = mdm_display_lookup (slave_pid);

		if (d != NULL) {
		        custom_cmd (cmd_id);
			send_slave_ack (d, NULL);
		}
	} else if (strcmp (msg, MDM_SOP_FLEXI_XSERVER) == 0) {
		handle_flexi_server (NULL, TYPE_FLEXI, mdm_daemon_config_get_value_string (MDM_KEY_STANDARD_XSERVER),
				     TRUE /* handled */,
				     FALSE /* chooser */,
				     NULL, 0, NULL, NULL, NULL);
	} else if (strncmp (msg, "opcode="MDM_SOP_SHOW_ERROR_DIALOG,
			    strlen ("opcode="MDM_SOP_SHOW_ERROR_DIALOG)) == 0) {
		char **list;
		list = g_strsplit (msg, "$$", -1);

		if (mdm_vector_len (list) == 8) {
			MdmDisplay *d;
			GtkMessageType type;
			char *ptr;
			char *error;
			char *details_label;
			char *details_file;
			long slave_pid;
			int uid, gid;

			ptr = strchr (list[1], '=');
			slave_pid = atol (ptr + 1);

			ptr = strchr (list[2], '=');
			type = atoi (ptr + 1);

			ptr = strchr (list[3], '=');
			error = g_malloc0 (strlen (ptr));
			strcpy (error, ptr + 1);

			ptr = strchr (list[4], '=');
			details_label = g_malloc0 (strlen (ptr));
			strcpy (details_label, ptr + 1);

			ptr = strchr (list[5], '=');
			details_file = g_malloc0 (strlen (ptr));
			strcpy (details_file, ptr + 1);

			ptr = strchr (list[6], '=');
			uid = atoi (ptr + 1);

			ptr = strchr (list[7], '=');
			gid = atoi (ptr + 1);

			d = mdm_display_lookup (slave_pid);

			if (d != NULL) {
				if (MDM_AUTHFILE (d)) {
					VE_IGNORE_EINTR (
						chmod (MDM_AUTHFILE (d), 0644));
				}

				/* FIXME: this is really bad */
				mdm_errorgui_error_box_full (d, type, error,
				     details_label, details_file, 0, 0);

				if (MDM_AUTHFILE (d)) {
					VE_IGNORE_EINTR (
						chmod (MDM_AUTHFILE (d), 0640));
				}

				send_slave_ack_dialog_char (d,
					MDM_SLAVE_NOTIFY_ERROR_RESPONSE, NULL);
			}

			g_free (error);
			g_free (details_label);
			g_free (details_file);
		}
		g_strfreev (list);
	} else if (strncmp (msg, "opcode="MDM_SOP_SHOW_YESNO_DIALOG,
                            strlen ("opcode="MDM_SOP_SHOW_YESNO_DIALOG)) == 0) {
		char **list;
		list = g_strsplit (msg, "$$", -1);

		if (mdm_vector_len (list) == 3) {
			MdmDisplay *d;
			char *ptr;
			char *yesno_msg;
			long slave_pid;
			gboolean resp;

			ptr = strchr (list [1], '=');
			slave_pid = atol (ptr + 1);

			ptr = strchr (list [2], '=');
			yesno_msg = g_malloc0 (strlen (ptr));
			strcpy (yesno_msg, ptr + 1);

			d = mdm_display_lookup (slave_pid);
			if (d != NULL) {
				if (MDM_AUTHFILE (d)) {
					VE_IGNORE_EINTR (
						chmod (MDM_AUTHFILE (d), 0644));
				}

				resp = mdm_errorgui_failsafe_yesno (d,
					yesno_msg);

				send_slave_ack_dialog_int (d,
					MDM_SLAVE_NOTIFY_YESNO_RESPONSE,
					resp);

				if (MDM_AUTHFILE (d)) {
					VE_IGNORE_EINTR (
						chmod (MDM_AUTHFILE (d), 0640));
				}
			}
			g_free (yesno_msg);
		}

		g_strfreev (list);
	} else if (strncmp (msg, "opcode="MDM_SOP_SHOW_QUESTION_DIALOG,
                            strlen ("opcode="MDM_SOP_SHOW_QUESTION_DIALOG)) == 0) {
		char **list;

		list = g_strsplit (msg, "$$", -1);

		if (mdm_vector_len (list) == 4) {
			MdmDisplay *d;
			char *ptr;
			char *question_msg;
			char *resp;
			long slave_pid;
			gboolean echo;

			ptr = strchr (list [1], '=');
			slave_pid = atol (ptr + 1);

			ptr = strchr (list [2], '=');
			question_msg = g_malloc0 (strlen (ptr));
			strcpy (question_msg, ptr + 1);

			ptr = strchr (list [3], '=');
			echo = atoi (ptr + 1);

			d = mdm_display_lookup (slave_pid);
			if (d != NULL) {
				if (MDM_AUTHFILE (d)) {
					VE_IGNORE_EINTR (
						chmod (MDM_AUTHFILE (d), 0644));
				}

				resp = mdm_errorgui_failsafe_question (d,
					question_msg, echo);

				send_slave_ack_dialog_char (d,
					MDM_SLAVE_NOTIFY_QUESTION_RESPONSE,
					resp);

				if (MDM_AUTHFILE (d)) {
					VE_IGNORE_EINTR (
						chmod (MDM_AUTHFILE (d), 0640));
				}
			}

			g_free (question_msg);
		}
		g_strfreev (list);
	} else if (strncmp (msg, "opcode="MDM_SOP_SHOW_ASKBUTTONS_DIALOG,
                            strlen ("opcode="MDM_SOP_SHOW_ASKBUTTONS_DIALOG)) == 0) {
		char **list;
		list = g_strsplit (msg, "$$", -1);

		if (mdm_vector_len (list) == 7) {
			MdmDisplay *d;
			char *askbuttons_msg;
			char *ptr;
			char *options[4];
			long slave_pid;
			int i;

			int resp;
			ptr = strchr (list [1], '=');
			slave_pid = atol (ptr + 1);

			ptr = strchr (list [2], '=');
			askbuttons_msg = g_malloc0 (strlen (ptr));
			strcpy (askbuttons_msg, ptr + 1);

			ptr = strchr (list [3], '=');
			options[0] = g_malloc0 (strlen (ptr));
			strcpy (options[0], ptr + 1);

			ptr = strchr (list [4], '=');
			options[1] = g_malloc0 (strlen (ptr));
			strcpy (options[1], ptr + 1);

			ptr = strchr (list [5], '=');
			options[2] = g_malloc0 (strlen (ptr));
			strcpy (options[2], ptr + 1);

			ptr = strchr (list [6], '=');
			options[3] = g_malloc0 (strlen (ptr));
			strcpy (options[3], ptr + 1);

			d = mdm_display_lookup (slave_pid);
			if (d != NULL) {
				if (MDM_AUTHFILE (d)) {
					VE_IGNORE_EINTR (
						chmod (MDM_AUTHFILE (d), 0644));
				}

				resp = mdm_errorgui_failsafe_ask_buttons (d,
					askbuttons_msg, options);

				send_slave_ack_dialog_int (d,
					MDM_SLAVE_NOTIFY_ASKBUTTONS_RESPONSE,
					resp);

				if (MDM_AUTHFILE (d)) {
					VE_IGNORE_EINTR (
						chmod (MDM_AUTHFILE (d), 0640));
				}
			}

			g_free (askbuttons_msg);

			for (i = 0; i < 3; i ++)
				g_free (options[i]);
		}
		g_strfreev (list);
	}
}

/* extract second word and the rest of the string */
static void
extract_dispname_uid_xauthfile_cookie (const char *msg,
				       char **dispname,
				       uid_t *uid,
				       char **xauthfile,
				       char **cookie)
{
	const char *p;
	int i;
	char *pp;

	*dispname = NULL;
	*xauthfile = NULL;
	*cookie = NULL;

	/* Get dispname */
	p = strchr (msg, ' ');
	if (p == NULL)
		return;

	while (*p == ' ')
		p++;

	*dispname = g_strdup (p);
	pp = strchr (*dispname, ' ');
	if (pp != NULL)
		*pp = '\0';

	/* Get uid */
	p = strchr (p, ' ');
	if (p == NULL) {
		*dispname = NULL;
		g_free (*dispname);
		return;
	}
	while (*p == ' ')
		p++;

	if (sscanf (p, "%d", &i) != 1) {
		*dispname = NULL;
		g_free (*dispname);
		return;
	}
	*uid = i;

	/* Get cookie */
	p = strchr (p, ' ');
	if (p == NULL) {
		*dispname = NULL;
		g_free (*dispname);
		return;
	}
	while (*p == ' ')
		p++;

	*cookie = g_strdup (p);
	pp = strchr (*cookie, ' ');
	if (pp != NULL)
		*pp = '\0';

	/* Get xauthfile */
	p = strchr (p, ' ');
	if (p == NULL) {
		*cookie = NULL;
		g_free (*cookie);
		*dispname = NULL;
		g_free (*dispname);
		return;
	}

	while (*p == ' ')
		p++;

	*xauthfile = g_strstrip (g_strdup (p));

}

static void
close_conn (gpointer data)
{
	MdmDisplay *disp = data;

	/* We still weren't finished, so we want to whack this display */
	if (disp->socket_conn != NULL) {
		disp->socket_conn = NULL;
		mdm_display_unmanage (disp);
	}
}

static MdmDisplay *
find_display (const char *name)
{
	GSList *li;
	GSList *displays;

	displays = mdm_daemon_config_get_display_list ();

	for (li = displays; li != NULL; li = li->next) {
		MdmDisplay *disp = li->data;
		if (disp->name != NULL &&
		    strcmp (disp->name, name) == 0)
			return disp;
	}
	return NULL;
}

static char *
extract_dispnum (const char *addy)
{
	int num;
	char *p;

	mdm_assert (addy != NULL);

	p = strchr (addy, ':');
	if (p == NULL)
		return NULL;

	/* Whee! handles DECnet even if we don't do that */
	while (*p == ':')
		p++;

	if (sscanf (p, "%d", &num) != 1)
		return NULL;

	return g_strdup_printf ("%d", num);
}

static char *
dehex_cookie (const char *cookie, int *len)
{
	/* it should be +1 really, but I'm paranoid */
	char *bcookie = g_new0 (char, (strlen (cookie) / 2) + 2);
	int i;
	const char *p;

	*len = 0;

	for (i = 0, p = cookie;
	     *p != '\0' && *(p+1) != '\0';
	     i++, p += 2) {
		unsigned int num;
		if (sscanf (p, "%02x", &num) != 1) {
			g_free (bcookie);
			return NULL;
		}
		bcookie[i] = num;
	}
	*len = i;
	return bcookie;
}

/* This runs as the user who owns the file */
static gboolean
check_cookie (const gchar *file, const gchar *disp, const gchar *cookie)
{
	Xauth *xa;
	gchar *number;
	gchar *bcookie;
	int cookielen;
	gboolean ret = FALSE;
	int cnt = 0;

	FILE *fp = fopen (file, "r");
	if (fp == NULL)
		return FALSE;

	number = extract_dispnum (disp);
	if (number == NULL)
		return FALSE;
	bcookie = dehex_cookie (cookie, &cookielen);
	if (bcookie == NULL) {
		g_free (number);
		return FALSE;
	}

	while ((xa = XauReadAuth (fp)) != NULL) {
		if (xa->number_length == strlen (number) &&
		    strncmp (xa->number, number, xa->number_length) == 0 &&
		    xa->name_length == strlen ("MIT-MAGIC-COOKIE-1") &&
		    strncmp (xa->name, "MIT-MAGIC-COOKIE-1",
			     xa->name_length) == 0 &&
		    xa->data_length == cookielen &&
		    memcmp (xa->data, bcookie, cookielen) == 0) {
			XauDisposeAuth (xa);
			ret = TRUE;
			break;
		}
		XauDisposeAuth (xa);

		/* just being ultra anal */
		cnt++;
		if (cnt > 500)
			break;
	}

	g_free (number);
	g_free (bcookie);

	VE_IGNORE_EINTR (fclose (fp));

	return ret;
}

static void
handle_flexi_server (MdmConnection *conn,
		     int            type,
		     const char    *server,
		     gboolean       handled,
		     gboolean       chooser,
		     const char    *xnest_disp,
		     uid_t          xnest_uid,
		     const char    *xnest_auth_file,
		     const char    *xnest_cookie,
		     const char    *username)
{
	MdmDisplay *display;
	gchar *bin;
	uid_t server_uid = 0;

	mdm_debug ("flexi server: '%s'", server);

	if (mdm_wait_for_go) {
		if (conn != NULL)
			mdm_connection_write (conn,
					      "ERROR 1 No more flexi servers\n");
		return;
	}

	if (type == TYPE_FLEXI_XNEST) {
		gboolean authorized = TRUE;
		struct passwd *pw;
		gid_t oldgid = getegid ();

		pw = getpwuid (xnest_uid);
		if (pw == NULL) {
			if (conn != NULL)
				mdm_connection_write (conn,
						      "ERROR 100 Not authenticated\n");
			return;
		}

		/* paranoia */
		NEVER_FAILS_seteuid (0);

		if (setegid (pw->pw_gid) < 0)
			NEVER_FAILS_setegid (mdm_daemon_config_get_mdmgid ());

		if (seteuid (xnest_uid) < 0) {
			if (conn != NULL)
				mdm_connection_write (conn,
						      "ERROR 100 Not authenticated\n");
			return;
		}

		mdm_assert (xnest_auth_file != NULL);
		mdm_assert (xnest_disp != NULL);
		mdm_assert (xnest_cookie != NULL);

		if (authorized &&
		    ! mdm_auth_file_check ("handle_flexi_server", xnest_uid, xnest_auth_file, FALSE /* absentok */, NULL))
			authorized = FALSE;

		if (authorized &&
		    ! check_cookie (xnest_auth_file,
				    xnest_disp,
				    xnest_cookie)) {
			authorized = FALSE;
		}

		/* this must always work, thus the asserts */
		NEVER_FAILS_root_set_euid_egid (0, oldgid);

		if (! authorized) {
			/* Sorry dude, you're not doing something
			 * right */
			if (conn != NULL)
				mdm_connection_write (conn,
						      "ERROR 100 Not authenticated\n");
			return;
		}

		server_uid = xnest_uid;
	}

	if (flexi_servers >= mdm_daemon_config_get_value_int (MDM_KEY_FLEXIBLE_XSERVERS)) {
		if (conn != NULL)
			mdm_connection_write (conn,
					      "ERROR 1 No more flexi servers\n");
		return;
	}

	bin = ve_first_word (server);
	if (ve_string_empty (server) ||
	    g_access (bin, X_OK) != 0) {
		g_free (bin);
		if (conn != NULL)
			mdm_connection_write (conn,
					      "ERROR 6 No server binary\n");
		return;
	}
	g_free (bin);

	display = mdm_display_alloc (-1, server, NULL);
	if G_UNLIKELY (display == NULL) {
		if (conn != NULL)
			mdm_connection_write (conn,
					      "ERROR 2 Startup errors\n");
		return;
	}

	/* It is kind of ugly that we don't use
	   the standard resolution for this, but
	   oh well, this makes other things simpler */
	display->handled = handled;
	display->use_chooser = chooser;

	if (type == TYPE_FLEXI_XNEST) {
		MdmDisplay *parent;
		gchar *disp, *p;
		mdm_assert (xnest_disp != NULL);

		disp = g_strdup (xnest_disp);
		/* whack the screen info */
		p = strchr (disp, ':');
		if (p != NULL)
			p = strchr (p+1, '.');
		if (p != NULL)
			*p = '\0';
		/* if it's on one of the attached displays we started,
		 * it's on the console, else it's not (it could be but
		 * we aren't sure and we don't want to be fooled) */
		parent = find_display (disp);
		if (/* paranoia */xnest_disp[0] == ':' &&
		    parent != NULL &&
		    parent->attached)
			display->attached = TRUE;
		else
			display->attached = FALSE;
		g_free (disp);

		display->server_uid = server_uid;
	}

	flexi_servers++;

	display->preset_user = g_strdup (username);
	display->type = type;
	display->socket_conn = conn;
	display->parent_disp = g_strdup (xnest_disp);
	display->parent_auth_file = g_strdup (xnest_auth_file);
	if (conn != NULL)
		mdm_connection_set_close_notify (conn, display, close_conn);
	mdm_daemon_config_display_list_append (display);

	if ( ! mdm_display_manage (display)) {
		mdm_display_unmanage (display);
		if (conn != NULL)
			mdm_connection_write (conn,
					      "ERROR 2 Startup errors\n");
		return;
	}
	/* Now we wait for the server to start up (or not) */
}

static void
handle_dynamic_server (MdmConnection *conn, int type, gchar *key)
{
	MdmDisplay *disp;
	int disp_num;
	gchar *msg;
	gchar *full;
	gchar *val;

	if (!(mdm_daemon_config_get_value_bool (MDM_KEY_DYNAMIC_XSERVERS))) {
		mdm_connection_write (conn, "ERROR 200 Dynamic Displays not allowed\n");
		return;
	}

	if ( ! MDM_CONN_AUTH_GLOBAL(conn)) {
		mdm_info (_("DYNAMIC request denied: " "Not authenticated"));
		mdm_connection_write (conn, "ERROR 100 Not authenticated\n");
		return;
	}

	if (key == NULL) {
		mdm_connection_write (conn, "ERROR 1 Bad display number <NULL>\n");
		return;
	} else if ( !(isdigit (*key))) {
		msg = g_strdup_printf ("ERROR 1 Bad display number <%s>\n", key);
		mdm_connection_write (conn, msg);
		g_free (msg);
		return;
	}
	disp_num = atoi (key);

	if (type == DYNAMIC_ADD) {
		/* prime an X server for launching */

		if (mark_display_exists (disp_num)) {
			/* need to skip starting this one again */
			mdm_connection_write (conn, "ERROR 2 Existing display\n");
			return;
		}

		full = strchr (key, '=');
		if (full == NULL || *(full + 1) == 0) {
			mdm_connection_write (conn, "ERROR 3 No server string\n");
			return;
		}

		val = full + 1;
		disp = mdm_display_alloc (disp_num, val, NULL);

		if (disp == NULL) {
			mdm_connection_write (conn, "ERROR 4 Display startup failure\n");
			return;
		}

		mdm_daemon_config_display_list_insert (disp);

		disp->dispstat = DISPLAY_CONFIG;
		disp->removeconf = FALSE;

		if (disp_num > mdm_daemon_config_get_high_display_num ())
			mdm_daemon_config_set_high_display_num (disp_num);

		mdm_connection_write (conn, "OK\n");
		return;
	}

	if (type == DYNAMIC_REMOVE) {
		GSList *li;
		GSList *nli;
		GSList *displays;

		displays = mdm_daemon_config_get_display_list ();

		/* shutdown a dynamic X server */

		for (li = displays; li != NULL; li = nli) {
			disp = li->data;
			nli = li->next;
			if (disp->dispnum == disp_num) {
				disp->removeconf = TRUE;
				mdm_display_unmanage (disp);
				mdm_connection_write (conn, "OK\n");
				return;
			}
		}

		msg = g_strdup_printf ("ERROR 1 Bad display number <%d>\n", disp_num);
		mdm_connection_write (conn, msg);
		return;
	}

	if (type == DYNAMIC_RELEASE) {
		/* cause the newly configured X servers to actually run */
		GSList *li;
		GSList *nli;
		gboolean found = FALSE;
		GSList *displays;

		displays = mdm_daemon_config_get_display_list ();

		for (li = displays; li != NULL; li = nli) {
			MdmDisplay *disp = li->data;
			nli = li->next;
			if ((disp->dispnum == disp_num) &&
			    (disp->dispstat == DISPLAY_CONFIG)) {
				disp->dispstat = DISPLAY_UNBORN;

				if ( ! mdm_display_manage (disp)) {
					mdm_display_unmanage (disp);
				}
				found = TRUE;
			}
		}

		if (found)
			mdm_connection_write (conn, "OK\n");
		else {
			msg = g_strdup_printf ("ERROR 1 Bad display number <%d>\n", disp_num);
			mdm_connection_write (conn, msg);
		}

		/* Now we wait for the server to start up (or not) */
		return;
	}
}

static void
sup_handle_auth_local (MdmConnection *conn,
		       const char    *msg,
		       gpointer       data)
{
	GSList *li;
	char *cookie;
	GSList *displays;

	cookie = g_strdup (&msg[strlen (MDM_SUP_AUTH_LOCAL " ")]);

	displays = mdm_daemon_config_get_display_list ();

	g_strstrip (cookie);
	if (strlen (cookie) != 16*2) /* 16 bytes in hex form */ {
		/* evil, just whack the connection in this case */
		mdm_connection_write (conn,
				      "ERROR 100 Not authenticated\n");
		mdm_connection_close (conn);
		g_free (cookie);
		return;
	}
	/* check if cookie matches one of the attached displays */
	for (li = displays; li != NULL; li = li->next) {
		MdmDisplay *disp = li->data;
		if (disp->attached &&
		    disp->cookie != NULL &&
		    g_ascii_strcasecmp (disp->cookie, cookie) == 0) {
			g_free (cookie);
			MDM_CONNECTION_SET_USER_FLAG
				(conn, MDM_SUP_FLAG_AUTHENTICATED);
			mdm_connection_set_display (conn, disp);
			mdm_connection_write (conn, "OK\n");
			return;
		}
	}

	if (mdm_global_cookie != NULL &&
	    g_ascii_strcasecmp ((gchar *) mdm_global_cookie, cookie) == 0) {
		g_free (cookie);
		MDM_CONNECTION_SET_USER_FLAG
			(conn, MDM_SUP_FLAG_AUTH_GLOBAL);
		mdm_connection_write (conn, "OK\n");
		return;
	}

	/* Hmmm, perhaps this is better defined behaviour */
	MDM_CONNECTION_UNSET_USER_FLAG
		(conn, MDM_SUP_FLAG_AUTHENTICATED);
	MDM_CONNECTION_UNSET_USER_FLAG
		(conn, MDM_SUP_FLAG_AUTH_GLOBAL);
	mdm_connection_set_display (conn, NULL);
	mdm_connection_write (conn, "ERROR 100 Not authenticated\n");
	g_free (cookie);
}

static void
sup_handle_attached_servers (MdmConnection *conn,
			     const char    *msg,
			     gpointer       data)
{
	GString *retMsg;
	GSList  *li;
	const gchar *sep = " ";
	char    *key;
	int     msgLen=0;
	GSList *displays;

	displays = mdm_daemon_config_get_display_list ();

	if (strncmp (msg, MDM_SUP_ATTACHED_SERVERS,
		     strlen (MDM_SUP_ATTACHED_SERVERS)) == 0)
		msgLen = strlen (MDM_SUP_ATTACHED_SERVERS);
	else if (strncmp (msg, MDM_SUP_CONSOLE_SERVERS,
			  strlen (MDM_SUP_CONSOLE_SERVERS)) == 0)
		msgLen = strlen (MDM_SUP_CONSOLE_SERVERS);

	key = g_strdup (&msg[msgLen]);
	g_strstrip (key);

	retMsg = g_string_new ("OK");
	for (li = displays; li != NULL; li = li->next) {
		MdmDisplay *disp = li->data;

		if ( ! disp->attached)
			continue;
		if (!(strlen (key)) || (g_pattern_match_simple (key, disp->command))) {
			g_string_append_printf (retMsg, "%s%s,%s,", sep,
						ve_sure_string (disp->name),
						ve_sure_string (disp->login));
			sep = ";";
			if (disp->type == TYPE_FLEXI_XNEST) {
				g_string_append (retMsg, ve_sure_string (disp->parent_disp));
			} else {
				g_string_append_printf (retMsg, "%d", disp->vt);
			}
		}
	}

	g_string_append (retMsg, "\n");
	mdm_connection_write (conn, retMsg->str);
	g_free (key);
	g_string_free (retMsg, TRUE);
}

static void
sup_handle_get_server_details (MdmConnection *conn,
			       const char    *msg,
			       gpointer       data)
{
	const gchar  *server   = &msg[strlen (MDM_SUP_GET_SERVER_DETAILS " ")];
	gchar       **splitstr = g_strsplit (server, " ", 2);
	MdmXserver   *svr      = NULL;

	if (mdm_vector_len (splitstr) == 2) {
		svr = mdm_daemon_config_find_xserver ((gchar *)splitstr[0]);
	}

	if (svr != NULL) {
		if (g_strcasecmp (splitstr[1], "ID") == 0)
			mdm_connection_printf (conn, "OK %s\n",
				svr->id ? svr->id : "(null)");
		else if (g_strcasecmp (splitstr[1], "NAME") == 0)
			mdm_connection_printf (conn, "OK %s\n",
				svr->name ? svr->name : "(null)");
		else if (g_strcasecmp (splitstr[1], "COMMAND") == 0)
			mdm_connection_printf (conn, "OK %s\n",
				svr->command ? svr->command : "(null)");
		else if (g_strcasecmp (splitstr[1], "PRIORITY") == 0)
			mdm_connection_printf (conn, "OK %d\n", svr->priority);
		else if (g_strcasecmp (splitstr[1], "FLEXIBLE") == 0 &&
			 svr->flexible)
			mdm_connection_printf (conn, "OK true\n");
		else if (g_strcasecmp (splitstr[1], "FLEXIBLE") == 0 &&
			 !svr->flexible)
			mdm_connection_printf (conn, "OK false\n");
		else if (g_strcasecmp (splitstr[1], "CHOOSABLE") == 0 &&
			 svr->choosable)
			mdm_connection_printf (conn, "OK true\n");
		else if (g_strcasecmp (splitstr[1], "CHOOSABLE") == 0 &&
			 !svr->choosable)
			mdm_connection_printf (conn, "OK false\n");
		else if (g_strcasecmp (splitstr[1], "HANDLED") == 0 &&
			 svr->handled)
			mdm_connection_printf (conn, "OK true\n");
		else if (g_strcasecmp (splitstr[1], "HANDLED") == 0 &&
			 !svr->handled)
			mdm_connection_printf (conn, "OK false\n");
		else if (g_strcasecmp (splitstr[1], "CHOOSER") == 0 &&
			 svr->chooser)
			mdm_connection_printf (conn, "OK true\n");
		else if (g_strcasecmp (splitstr[1], "CHOOSER") == 0 &&
			 !svr->chooser)
			mdm_connection_printf (conn, "OK false\n");
		else
			mdm_connection_printf (conn, "ERROR 2 Key not valid\n");

	} else {
		mdm_connection_printf (conn, "ERROR 1 Server not found\n");
	}
	g_strfreev (splitstr);
}

static void
sup_handle_flexi_xserver (MdmConnection *conn,
			  const char    *msg,
			  gpointer       data)
{
	char       *name;
	const char *command = NULL;
	MdmXserver *svr;
	const char *rest;
	char       *username;
	char       *end;
	gboolean    has_user;

	has_user = strncmp (msg, MDM_SUP_FLEXI_XSERVER_USER " ", strlen (MDM_SUP_FLEXI_XSERVER_USER " ")) == 0;

	mdm_debug ("Handling flexi request has-user:%d", has_user);

	/* Only allow locally authenticated connections */
	if ( ! MDM_CONN_AUTHENTICATED (conn)) {
		mdm_info (_("%s request denied: "
			    "Not authenticated"), "FLEXI_XSERVER");
		mdm_connection_write (conn,
				      "ERROR 100 Not authenticated\n");
		return;
	}

	if (has_user) {
		rest = msg + strlen (MDM_SUP_FLEXI_XSERVER_USER " ");
		end = strchr (rest, ' ');
		if (end) {
			username = g_strndup (rest, end - rest);
			rest = end + 1;
		} else {
			username = g_strdup (rest);
			rest = rest + strlen (rest);
		}
	} else {
		rest = msg + strlen (MDM_SUP_FLEXI_XSERVER " ");
		username = NULL;
	}

	name = g_strdup (rest);
	g_strstrip (name);
	if (ve_string_empty (name)) {
		g_free (name);
		name = g_strdup (MDM_STANDARD);
	}

	svr = mdm_daemon_config_find_xserver (name);

	if G_UNLIKELY (svr == NULL) {
		/* Don't print the name to syslog as it might be
		 * long and dangerous */
		mdm_error (_("Unknown server type requested; using "
			     "standard server."));
		command = mdm_daemon_config_get_value_string (MDM_KEY_STANDARD_XSERVER);
	} else if G_UNLIKELY ( ! svr->flexible) {
		mdm_error (_("Requested server %s not allowed to be "
			     "used for flexible servers; using "
			     "standard server."), name);
		command = mdm_daemon_config_get_value_string (MDM_KEY_STANDARD_XSERVER);
	} else {
		command = svr->command;
	}
	g_free (name);

	handle_flexi_server (conn,
			     TYPE_FLEXI,
			     command,
			     /* It is kind of ugly that we don't use
				the standard resolution for this, but
				oh well, this makes other things simpler */
			     svr->handled,
			     svr->chooser,
			     NULL,
			     0,
			     NULL,
			     NULL,
			     username);
	g_free (username);
}

static void
sup_handle_flexi_xnest (MdmConnection *conn,
			const char    *msg,
			gpointer       data)
{
	char       *dispname = NULL;
	char       *xauthfile = NULL;
	char       *cookie = NULL;
	uid_t       uid;
	const char *rest;
	char       *username;
	char       *end;
	gboolean    has_user;

	has_user = strncmp (msg, MDM_SUP_FLEXI_XNEST_USER " ", strlen (MDM_SUP_FLEXI_XNEST_USER " ")) == 0;

	mdm_debug ("Handling flexi xnest request has-user:%d", has_user);

	if (has_user) {
		rest = msg + strlen (MDM_SUP_FLEXI_XNEST_USER " ");
		end = strchr (rest, ' ');
		username = g_strndup (rest, end - rest);
	} else {
		rest = msg;
		username = NULL;
	}

	extract_dispname_uid_xauthfile_cookie (rest,
					       &dispname,
					       &uid,
					       &xauthfile,
					       &cookie);

	if (dispname == NULL) {
		/* Something bogus is going on, so just whack the
		 * connection */
		g_free (xauthfile);
		mdm_connection_close (conn);
		mdm_debug ("Unable to get display name from request");
		return;
	}

	/* This is probably a pre-2.2.4.2 client */
	if (xauthfile == NULL || cookie == NULL) {
		/* evil, just whack the connection in this case */
		mdm_connection_write (conn,
				      "ERROR 100 Not authenticated\n");
		mdm_connection_close (conn);
		g_free (cookie);
		return;
	}

	handle_flexi_server (conn,
			     TYPE_FLEXI_XNEST,
			     mdm_daemon_config_get_value_string (MDM_KEY_XNEST),
			     TRUE /* handled */,
			     FALSE /* chooser */,
			     dispname,
			     uid,
			     xauthfile,
			     cookie,
			     username);

	g_free (dispname);
	g_free (xauthfile);
	g_free (username);
}

static void
sup_handle_get_config (MdmConnection *conn,
		       const char    *msg,
		       gpointer       data)
{
	const char *parms;
	char **splitstr;
	char *retval;
	static gboolean done_prefetch = FALSE;

	retval = NULL;

	parms = &msg[strlen (MDM_SUP_GET_CONFIG " ")];

	splitstr = g_strsplit (parms, " ", 2);

	if (splitstr == NULL || splitstr[0] == NULL) {
		mdm_connection_printf (conn, "ERROR 50 Unsupported key <null>\n");
		goto out;
	}

	/*
	 * It is not meaningful to manage this in a per-display
	 * fashion since the prefetch program is only run once the
	 * for the first display that requests the key.  So process
	 * this first and return "Unsupported key" for requests
	 * after the first request.
	 */
	if (strcmp (splitstr[0], MDM_KEY_PRE_FETCH_PROGRAM) == 0) {
		if (done_prefetch) {
			mdm_connection_printf (conn, "OK \n");
		} else {
			mdm_connection_printf (conn, "ERROR 50 Unsupported key <%s>\n", splitstr[0]);
			done_prefetch = TRUE;
		}
		goto out;
	}

	if (splitstr[0] != NULL) {
		gboolean res;

		/*
		 * Note passing in the display is backwards compatible
		 * since if it is NULL, it won't try to load the display
		 * value at all.
		 */

		mdm_debug ("Handling GET_CONFIG: %s for display %s", splitstr[0],
			   splitstr[1] ? splitstr[1] : "(null)");

		res = mdm_daemon_config_to_string (splitstr[0], splitstr[1], &retval);

		if (res) {
			mdm_connection_printf (conn, "OK %s\n", retval);
			g_free (retval);
		} else {
			if (mdm_daemon_config_is_valid_key ((gchar *)splitstr[0]))
				mdm_connection_printf (conn, "OK \n");
			else
				mdm_connection_printf (conn,
						       "ERROR 50 Unsupported key <%s>\n",
						       splitstr[0]);
		}
	}
 out:
	g_strfreev (splitstr);
}

static gboolean
is_action_available (MdmDisplay *disp, gchar *action)
{
	const gchar **allowsyscmd;
	const gchar **rbackeys;
	gboolean sysmenu;
	gboolean ret = FALSE;
	int i;

	allowsyscmd = mdm_daemon_config_get_value_string_array (MDM_KEY_ALLOW_LOGOUT_ACTIONS);
	rbackeys    = mdm_daemon_config_get_value_string_array (MDM_KEY_RBAC_SYSTEM_COMMAND_KEYS);
	sysmenu     = mdm_daemon_config_get_value_bool_per_display (MDM_KEY_SYSTEM_MENU, disp->name);

	if (!disp->attached || !sysmenu) {
		return FALSE;
	}

	for (i = 0; allowsyscmd[i] != NULL; i++) {
		if (strcmp (allowsyscmd[i], action) == 0) {
			ret = TRUE;
			break;
		}
	}

#ifdef HAVE_CHKAUTHATTR
	if (ret == TRUE && rbackeys) {
		for (i = 0; rbackeys[i] != NULL; i++) {
			gchar **rbackey = g_strsplit (rbackeys[i], ":", 2);

			if (mdm_vector_len (rbackey) == 2 &&
			    ! ve_string_empty (rbackey[0]) &&
			    ! ve_string_empty (rbackey[1]) &&
			    strcmp (rbackey[0], action) == 0) {

       				if (!chkauthattr (rbackey[1], disp->login)) {
					g_strfreev (rbackey);
					ret = FALSE;
					break;
				}
			
			}
			g_strfreev (rbackey);
		}
	}
#endif

	return ret;
}

static void
sup_handle_query_logout_action (MdmConnection *conn,
				const char    *msg,
				gpointer       data)
{
	MdmLogoutAction logout_action;
	MdmDisplay *disp;
	GString *reply;
	const gchar *sep = " ";
	int i;
	gchar *key_string = NULL;

	disp = mdm_connection_get_display (conn);

	/* Only allow locally authenticated connections */
        if (! MDM_CONN_AUTHENTICATED (conn) || disp == NULL) {
		mdm_info (_("%s request denied: "
			    "Not authenticated"), "QUERY_LOGOUT_ACTION");
		mdm_connection_write (conn,
				      "ERROR 100 Not authenticated\n");
		return;
	}

	reply   = g_string_new ("OK");

	logout_action = disp->logout_action;
	if (logout_action == MDM_LOGOUT_ACTION_NONE)
		logout_action = safe_logout_action;

	if (mdm_daemon_config_get_value_string_array (MDM_KEY_HALT) &&
	    is_action_available (disp, MDM_SUP_LOGOUT_ACTION_HALT)) {
		g_string_append_printf (reply, "%s%s", sep,
			MDM_SUP_LOGOUT_ACTION_HALT);
		if (logout_action == MDM_LOGOUT_ACTION_HALT)
			g_string_append (reply, "!");
		sep = ";";
	}
	if (mdm_daemon_config_get_value_string_array (MDM_KEY_REBOOT) &&
	    is_action_available (disp, MDM_SUP_LOGOUT_ACTION_REBOOT)) {
		g_string_append_printf (reply, "%s%s", sep,
			MDM_SUP_LOGOUT_ACTION_REBOOT);
		if (logout_action == MDM_LOGOUT_ACTION_REBOOT)
			g_string_append (reply, "!");
		sep = ";";
	}
	if (mdm_daemon_config_get_value_string_array (MDM_KEY_SUSPEND) &&
	    is_action_available (disp, MDM_SUP_LOGOUT_ACTION_SUSPEND)) {
		g_string_append_printf (reply, "%s%s", sep,
			MDM_SUP_LOGOUT_ACTION_SUSPEND);
		if (logout_action == MDM_LOGOUT_ACTION_SUSPEND)
			g_string_append (reply, "!");
		sep = ";";
	}

	if (is_action_available (disp, MDM_SUP_LOGOUT_ACTION_CUSTOM_CMD_TEMPLATE)) {
		for (i = 0; i < MDM_CUSTOM_COMMAND_MAX; i++) {
			key_string = g_strdup_printf ("%s%d=", MDM_KEY_CUSTOM_CMD_TEMPLATE, i);

			if (! ve_string_empty (mdm_daemon_config_get_value_string (key_string))) {

				g_free (key_string);
				key_string = g_strdup_printf("%s%d=",
					MDM_KEY_CUSTOM_CMD_IS_PERSISTENT_TEMPLATE, i);

				if (mdm_daemon_config_get_value_bool (key_string)) {
					g_string_append_printf (reply, "%s%s%d", sep,
						MDM_SUP_LOGOUT_ACTION_CUSTOM_CMD_TEMPLATE, i);
					if (logout_action == (MDM_LOGOUT_ACTION_CUSTOM_CMD_FIRST + i))
						g_string_append (reply, "!");
				sep = ";";
				}
			}
			g_free(key_string);
		}
	}

	g_string_append (reply, "\n");
	mdm_connection_write (conn, reply->str);
	g_string_free (reply, TRUE);
}

static void
sup_handle_query_custom_cmd_labels (MdmConnection *conn,
				    const char    *msg,
				    gpointer       data)
{

	MdmDisplay *disp;
	GString *reply;
	const gchar *sep = " ";
	gboolean sysmenu;
	int i;
	gchar *key_string = NULL;

	disp = mdm_connection_get_display (conn);
	sysmenu = mdm_daemon_config_get_value_bool_per_display (MDM_KEY_SYSTEM_MENU, disp->name);

	/* Only allow locally authenticated connections */
	if ( ! MDM_CONN_AUTHENTICATED (conn) ||
	     disp == NULL) {
		mdm_info (_("%s request denied: "
			    "Not authenticated"), "QUERY_CUSTOM_CMD_LABELS");
		mdm_connection_write (conn,
				      "ERROR 100 Not authenticated\n");
		return;
	}

	reply = g_string_new ("OK");

	for (i = 0; i < MDM_CUSTOM_COMMAND_MAX; i++) {
		key_string = g_strdup_printf("%s%d=", MDM_KEY_CUSTOM_CMD_TEMPLATE, i);
		if (sysmenu && disp->attached &&
		    ! ve_string_empty (mdm_daemon_config_get_value_string (key_string))) {
			g_free (key_string);
			key_string = g_strdup_printf("%s%d=", MDM_KEY_CUSTOM_CMD_IS_PERSISTENT_TEMPLATE, i);
			if (mdm_daemon_config_get_value_bool (key_string)) {
				g_free (key_string);
				key_string = g_strdup_printf("%s%d=", MDM_KEY_CUSTOM_CMD_LABEL_TEMPLATE, i);
				g_string_append_printf (reply, "%s%s",  sep, mdm_daemon_config_get_value_string (key_string));
				sep = ";";
			}
		}
		g_free(key_string);
	}

	g_string_append (reply, "\n");
	mdm_connection_write (conn, reply->str);
	g_string_free (reply, TRUE);
}

static void
sup_handle_all_servers (MdmConnection *conn,
			const char    *msg,
			gpointer       data)
{
	GString *reply;
	GSList *li;
	const gchar *sep = " ";
	GSList *displays;

	displays = mdm_daemon_config_get_display_list ();

	reply = g_string_new ("OK");
	for (li = displays; li != NULL; li = li->next) {
		MdmDisplay *disp = li->data;
		g_string_append_printf (reply, "%s%s,%s", sep,
					ve_sure_string (disp->name),
					ve_sure_string (disp->login));
		sep = ";";
	}
	g_string_append (reply, "\n");
	mdm_connection_write (conn, reply->str);
	g_string_free (reply, TRUE);
}

static void
sup_handle_get_server_list (MdmConnection *conn,
			    const char    *msg,
			    gpointer       data)
{
	gchar *retval = mdm_daemon_config_get_xservers ();

	if (retval != NULL) {
		mdm_connection_printf (conn, "OK %s\n", retval);
		g_free (retval);
	} else {
		mdm_connection_printf (conn, "ERROR 1 No servers found\n");
	}
}

static void
sup_handle_get_custom_config_file (MdmConnection *conn,
				   const char    *msg,
				   gpointer       data)
{
	gchar *ret;

	ret = mdm_daemon_config_get_custom_config_file ();
	if (ret)
		mdm_connection_printf (conn, "OK %s\n", ret);
	else
		mdm_connection_write (conn,
				      "ERROR 1 File not found\n");
}

static void
sup_handle_greeterpids (MdmConnection *conn,
			const char    *msg,
			gpointer       data)
{
	GString *reply;
	GSList *li;
	const gchar *sep = " ";
	GSList *displays;

	displays = mdm_daemon_config_get_display_list ();

	reply = g_string_new ("OK");
	for (li = displays; li != NULL; li = li->next) {
		MdmDisplay *disp = li->data;
		if (disp->greetpid > 0) {
			g_string_append_printf (reply, "%s%ld",
						sep, (long)disp->greetpid);
			sep = ";";
		}
	}
	g_string_append (reply, "\n");
	mdm_connection_write (conn, reply->str);
	g_string_free (reply, TRUE);
}

static void
sup_handle_query_custom_cmd_no_restart_status (MdmConnection *conn,
					       const char    *msg,
					       gpointer       data)
{

	MdmDisplay *disp;
	GString *reply;
	gboolean sysmenu;
	unsigned long no_restart_status_flag = 0; /* we can store up-to 32 commands this way */
	int i;
	gchar *key_string = NULL;

	disp = mdm_connection_get_display (conn);
	sysmenu = mdm_daemon_config_get_value_bool_per_display (MDM_KEY_SYSTEM_MENU, disp->name);

	/* Only allow locally authenticated connections */
	if ( ! MDM_CONN_AUTHENTICATED (conn) ||
	     disp == NULL) {
		mdm_info (_("%s request denied: "
			    "Not authenticated"), "QUERY_CUSTOM_CMD_NO_RESTART_STATUS");
		mdm_connection_write (conn,
				      "ERROR 100 Not authenticated\n");
		return;
	}

	reply = g_string_new ("OK ");

	for (i = 0; i < MDM_CUSTOM_COMMAND_MAX; i++) {
		key_string = g_strdup_printf("%s%d=", MDM_KEY_CUSTOM_CMD_TEMPLATE, i);
		if (sysmenu && disp->attached &&
		    ! ve_string_empty (mdm_daemon_config_get_value_string (key_string))) {
			g_free (key_string);
			key_string = g_strdup_printf("%s%d=", MDM_KEY_CUSTOM_CMD_IS_PERSISTENT_TEMPLATE, i);
			if (mdm_daemon_config_get_value_bool (key_string)) {
				g_free (key_string);
				key_string = g_strdup_printf("%s%d=", MDM_KEY_CUSTOM_CMD_NO_RESTART_TEMPLATE, i);
				if(mdm_daemon_config_get_value_bool (key_string))
					no_restart_status_flag |= (1 << i);
			}
		}
		g_free(key_string);
	}

	g_string_append_printf (reply, "%ld\n", no_restart_status_flag);
	mdm_connection_write (conn, reply->str);
	g_string_free (reply, TRUE);
}

static void
sup_handle_set_logout_action (MdmConnection *conn,
			      const char    *msg,
			      gpointer       data)

{
	MdmDisplay *disp;
	const gchar *action;
	gboolean was_ok = FALSE;

	action = &msg[strlen (MDM_SUP_SET_LOGOUT_ACTION " ")];
	disp   = mdm_connection_get_display (conn);

	/* Only allow locally authenticated connections */
	if ( ! MDM_CONN_AUTHENTICATED (conn) ||
	     disp == NULL ||
	     ! disp->logged_in) {
		mdm_info (_("%s request denied: "
			    "Not authenticated"), "SET_LOGOUT_ACTION");
		mdm_connection_write (conn,
				      "ERROR 100 Not authenticated\n");
		return;
	}

	if (strcmp (action, MDM_SUP_LOGOUT_ACTION_NONE) == 0) {
		disp->logout_action = MDM_LOGOUT_ACTION_NONE;
		was_ok = TRUE;
	} else if (strcmp (action, MDM_SUP_LOGOUT_ACTION_HALT) == 0 &&
		   mdm_daemon_config_get_value_string_array (MDM_KEY_HALT) &&
		   is_action_available (disp, MDM_SUP_LOGOUT_ACTION_HALT)) {
		disp->logout_action = MDM_LOGOUT_ACTION_HALT;
		was_ok = TRUE;
	} else if (strcmp (action, MDM_SUP_LOGOUT_ACTION_REBOOT) == 0 &&
		   mdm_daemon_config_get_value_string_array (MDM_KEY_REBOOT) &&
		   is_action_available (disp, MDM_SUP_LOGOUT_ACTION_REBOOT)) {
		disp->logout_action = MDM_LOGOUT_ACTION_REBOOT;
		was_ok = TRUE;
	} else if (strcmp (action, MDM_SUP_LOGOUT_ACTION_SUSPEND) == 0 &&
		   mdm_daemon_config_get_value_string_array (MDM_KEY_SUSPEND) &&
		   is_action_available (disp, MDM_SUP_LOGOUT_ACTION_SUSPEND)) {
		disp->logout_action = MDM_LOGOUT_ACTION_SUSPEND;
		was_ok = TRUE;
	}
	else if (strncmp (action, MDM_SUP_LOGOUT_ACTION_CUSTOM_CMD_TEMPLATE,
			  strlen (MDM_SUP_LOGOUT_ACTION_CUSTOM_CMD_TEMPLATE)) == 0 &&
		 is_action_available (disp, MDM_SUP_LOGOUT_ACTION_CUSTOM_CMD_TEMPLATE)) {

		int cmd_index;
		if (sscanf (action, MDM_SUP_LOGOUT_ACTION_CUSTOM_CMD_TEMPLATE "%d", &cmd_index) == 1) {
			gchar *key_string = NULL;
			key_string = g_strdup_printf ("%s%d=", MDM_KEY_CUSTOM_CMD_TEMPLATE, cmd_index);
			if (! ve_string_empty (mdm_daemon_config_get_value_string (key_string))) {
				disp->logout_action =
					MDM_LOGOUT_ACTION_CUSTOM_CMD_FIRST + cmd_index;
				was_ok = TRUE;
			}
			g_free(key_string);
		}
	}

	if (was_ok) {
		mdm_connection_write (conn, "OK\n");
		mdm_try_logout_action (disp);
	} else {
		mdm_connection_write (conn, "ERROR 7 Unknown logout action, or not available\n");
	}
}

static void
sup_handle_set_safe_logout_action (MdmConnection *conn,
				   const char    *msg,
				   gpointer       data)

{
	MdmDisplay *disp;
	const gchar *action;
	gboolean was_ok = FALSE;

	action = &msg[strlen (MDM_SUP_SET_SAFE_LOGOUT_ACTION " ")];
	disp   = mdm_connection_get_display (conn);

	/* Only allow locally authenticated connections */
	if ( ! MDM_CONN_AUTHENTICATED (conn) ||
	     disp == NULL ||
	     ! disp->logged_in) {
		mdm_info (_("%s request denied: "
			    "Not authenticated"), "SET_LOGOUT_ACTION");
		mdm_connection_write (conn,
				      "ERROR 100 Not authenticated\n");
		return;
	}

	if (strcmp (action, MDM_SUP_LOGOUT_ACTION_NONE) == 0) {
		safe_logout_action = MDM_LOGOUT_ACTION_NONE;
		was_ok = TRUE;
	} else if (strcmp (action, MDM_SUP_LOGOUT_ACTION_HALT) == 0 &&
		   mdm_daemon_config_get_value_string_array (MDM_KEY_HALT) &&
		   is_action_available (disp, MDM_SUP_LOGOUT_ACTION_HALT)) {
		safe_logout_action = MDM_LOGOUT_ACTION_HALT;
		was_ok = TRUE;
	} else if (strcmp (action, MDM_SUP_LOGOUT_ACTION_REBOOT) == 0 &&
		   mdm_daemon_config_get_value_string_array (MDM_KEY_REBOOT) &&
		   is_action_available (disp, MDM_SUP_LOGOUT_ACTION_REBOOT)) {
		safe_logout_action = MDM_LOGOUT_ACTION_REBOOT;
		was_ok = TRUE;
	} else if (strcmp (action, MDM_SUP_LOGOUT_ACTION_SUSPEND) == 0 &&
		   mdm_daemon_config_get_value_string_array (MDM_KEY_SUSPEND) &&
		   is_action_available (disp, MDM_SUP_LOGOUT_ACTION_SUSPEND)) {
		safe_logout_action = MDM_LOGOUT_ACTION_SUSPEND;
		was_ok = TRUE;
	}
	else if (strncmp (action, MDM_SUP_LOGOUT_ACTION_CUSTOM_CMD_TEMPLATE,
			  strlen (MDM_SUP_LOGOUT_ACTION_CUSTOM_CMD_TEMPLATE)) == 0 &&
		 is_action_available (disp, MDM_SUP_LOGOUT_ACTION_CUSTOM_CMD_TEMPLATE)) {

		int cmd_index;
		if (sscanf (action, MDM_SUP_LOGOUT_ACTION_CUSTOM_CMD_TEMPLATE "%d", &cmd_index) == 1) {

			gchar *key_string = NULL;
			key_string = g_strdup_printf ("%s%d=", MDM_KEY_CUSTOM_CMD_TEMPLATE, cmd_index);

			if (! ve_string_empty (mdm_daemon_config_get_value_string (key_string))) {
				safe_logout_action =
					MDM_LOGOUT_ACTION_CUSTOM_CMD_FIRST + cmd_index;
				was_ok = TRUE;
			}
			g_free(key_string);
		}
	}

	if (was_ok) {
		mdm_connection_write (conn, "OK\n");
		mdm_try_logout_action (disp);
	} else {
		mdm_connection_write (conn, "ERROR 7 Unknown logout action, or not available\n");
	}
}

static void
sup_handle_query_vt (MdmConnection *conn,
		     const char    *msg,
		     gpointer       data)

{
	int current_vt;

	/* Only allow locally authenticated connections */
	if ( ! MDM_CONN_AUTHENTICATED (conn)) {
		mdm_info (_("%s request denied: "
			    "Not authenticated"), "QUERY_VT");
		mdm_connection_write (conn,
				      "ERROR 100 Not authenticated\n");
		return;
	}

#if defined (MDM_USE_SYS_VT) || defined (MDM_USE_CONSIO_VT)
	current_vt = mdm_get_current_vt ();

	if (current_vt != -1) {
		mdm_connection_printf (conn, "OK %d\n", current_vt);
	} else {
		mdm_connection_write (conn, "ERROR 8 Virtual terminals not supported\n");
	}
#else
	mdm_connection_write (conn, "ERROR 8 Virtual terminals not supported\n");
#endif
}

static void
sup_handle_set_vt (MdmConnection *conn,
		   const char    *msg,
		   gpointer       data)
{
	int vt;
	GSList *li;
	GSList *displays;

	displays = mdm_daemon_config_get_display_list ();

	if (sscanf (msg, MDM_SUP_SET_VT " %d", &vt) != 1 ||
	    vt < 0) {
		mdm_connection_write (conn,
				      "ERROR 9 Invalid virtual terminal number\n");
		return;
	}

	/* Only allow locally authenticated connections */
	if ( ! MDM_CONN_AUTHENTICATED (conn)) {
		mdm_info (_("%s request denied: "
			    "Not authenticated"), "QUERY_VT");
		mdm_connection_write (conn,
				      "ERROR 100 Not authenticated\n");
		return;
	}

#if defined (MDM_USE_SYS_VT) || defined (MDM_USE_CONSIO_VT)
	mdm_change_vt (vt);
	for (li = displays; li != NULL; li = li->next) {
		MdmDisplay *disp = li->data;
		if (disp->vt == vt) {
			send_slave_command (disp, MDM_NOTIFY_TWIDDLE_POINTER);
			break;
		}
	}
	mdm_connection_write (conn, "OK\n");
#else
	mdm_connection_write (conn, "ERROR 8 Virtual terminals not supported\n");
#endif
}

static void
mdm_handle_user_message (MdmConnection *conn,
			 const char    *msg,
			 gpointer       data)
{

	mdm_debug ("Handling user message: '%s'", msg);

	if (mdm_connection_get_message_count (conn) > MDM_SUP_MAX_MESSAGES) {
		mdm_debug ("Closing connection, %d messages reached", MDM_SUP_MAX_MESSAGES);
		mdm_connection_write (conn, "ERROR 200 Too many messages\n");
		mdm_connection_close (conn);
		return;
	}

	if (strncmp (msg, MDM_SUP_AUTH_LOCAL " ",
		     strlen (MDM_SUP_AUTH_LOCAL " ")) == 0) {

		sup_handle_auth_local (conn, msg, data);

	} else if (strcmp (msg, MDM_SUP_FLEXI_XSERVER) == 0) {
		/* Only allow locally authenticated connections */
		if ( ! MDM_CONN_AUTHENTICATED (conn)) {
			mdm_info (_("%s request denied: "
				    "Not authenticated"), "FLEXI_XSERVER");
			mdm_connection_write (conn,
					      "ERROR 100 Not authenticated\n");
			return;
		}

		handle_flexi_server (conn,
				     TYPE_FLEXI,
				     mdm_daemon_config_get_value_string (MDM_KEY_STANDARD_XSERVER),
				     TRUE /* handled */,
				     FALSE /* chooser */,
				     NULL,
				     0,
				     NULL,
				     NULL,
				     NULL);
	} else if ((strncmp (msg, MDM_SUP_FLEXI_XSERVER_USER " ",
			     strlen (MDM_SUP_FLEXI_XSERVER_USER " ")) == 0) ||
                   (strncmp (msg, MDM_SUP_FLEXI_XSERVER " ",
			     strlen (MDM_SUP_FLEXI_XSERVER " ")) == 0)) {

		sup_handle_flexi_xserver (conn, msg, data);

	} else if ((strncmp (msg, MDM_SUP_FLEXI_XNEST_USER " ",
			     strlen (MDM_SUP_FLEXI_XNEST_USER " ")) == 0) ||
		   (strncmp (msg, MDM_SUP_FLEXI_XNEST " ",
			     strlen (MDM_SUP_FLEXI_XNEST " ")) == 0)) {

		sup_handle_flexi_xnest (conn, msg, data);

	} else if ((strncmp (msg, MDM_SUP_ATTACHED_SERVERS,
	                     strlen (MDM_SUP_ATTACHED_SERVERS)) == 0) ||
	           (strncmp (msg, MDM_SUP_CONSOLE_SERVERS,
	                     strlen (MDM_SUP_CONSOLE_SERVERS)) == 0)) {

		sup_handle_attached_servers (conn, msg, data);

	} else if (strcmp (msg, MDM_SUP_ALL_SERVERS) == 0) {

		sup_handle_all_servers (conn, msg, data);

	} else if (strcmp (msg, MDM_SUP_GET_SERVER_LIST) == 0) {

		sup_handle_get_server_list (conn, msg, data);

	} else if (strncmp (msg, MDM_SUP_GET_SERVER_DETAILS " ",
			    strlen (MDM_SUP_GET_SERVER_DETAILS " ")) == 0) {

		sup_handle_get_server_details (conn, msg, data);

	} else if (strcmp (msg, MDM_SUP_GREETERPIDS) == 0) {

		sup_handle_greeterpids (conn, msg, data);

	} else if (strncmp (msg, MDM_SUP_UPDATE_CONFIG " ",
			    strlen (MDM_SUP_UPDATE_CONFIG " ")) == 0) {
		const char *key;

		key = &msg[strlen (MDM_SUP_UPDATE_CONFIG " ")];

		if (! mdm_daemon_config_update_key ((gchar *)key))
			mdm_connection_printf (conn, "ERROR 50 Unsupported key <%s>\n", key);
		else
			mdm_connection_write (conn, "OK\n");
	} else if (strncmp (msg, MDM_SUP_GET_CONFIG " ",
			    strlen (MDM_SUP_GET_CONFIG " ")) == 0) {

		sup_handle_get_config (conn, msg, data);

	} else if (strcmp (msg, MDM_SUP_GET_CONFIG_FILE) == 0) {
		/*
		 * Value is only non-null if passed in on command line.
		 * Otherwise print compiled-in default file location.
		 */
		if (config_file == NULL) {
			mdm_connection_printf (conn, "OK %s\n",
					       MDM_DEFAULTS_CONF);
		} else {
			mdm_connection_printf (conn, "OK %s\n", config_file);
		}
	} else if (strcmp (msg, MDM_SUP_GET_CUSTOM_CONFIG_FILE) == 0) {

		sup_handle_get_custom_config_file (conn, msg, data);

	} else if (strcmp (msg, MDM_SUP_QUERY_LOGOUT_ACTION) == 0) {

		sup_handle_query_logout_action (conn, msg, data);

	} else if (strcmp (msg, MDM_SUP_QUERY_CUSTOM_CMD_LABELS) == 0) {

		sup_handle_query_custom_cmd_labels (conn, msg, data);

	} else if (strcmp (msg, MDM_SUP_QUERY_CUSTOM_CMD_NO_RESTART_STATUS) == 0) {

		sup_handle_query_custom_cmd_no_restart_status (conn, msg, data);

	} else if (strncmp (msg, MDM_SUP_SET_LOGOUT_ACTION " ",
			    strlen (MDM_SUP_SET_LOGOUT_ACTION " ")) == 0) {

		sup_handle_set_logout_action (conn, msg, data);

	} else if (strncmp (msg, MDM_SUP_SET_SAFE_LOGOUT_ACTION " ",
			    strlen (MDM_SUP_SET_SAFE_LOGOUT_ACTION " ")) == 0) {

		sup_handle_set_safe_logout_action (conn, msg, data);

	} else if (strcmp (msg, MDM_SUP_QUERY_VT) == 0) {

		sup_handle_query_vt (conn, msg, data);

	} else if (strncmp (msg, MDM_SUP_SET_VT " ",
			    strlen (MDM_SUP_SET_VT " ")) == 0) {

		sup_handle_set_vt (conn, msg, data);

	} else if (strncmp (msg, MDM_SUP_ADD_DYNAMIC_DISPLAY " ",
			    strlen (MDM_SUP_ADD_DYNAMIC_DISPLAY " ")) == 0) {
		gchar *key;

		key = g_strdup (&msg[strlen (MDM_SUP_ADD_DYNAMIC_DISPLAY " ")]);
		g_strstrip (key);
		handle_dynamic_server (conn, DYNAMIC_ADD, key);
		g_free (key);

	} else if (strncmp (msg, MDM_SUP_REMOVE_DYNAMIC_DISPLAY " ",
			    strlen (MDM_SUP_REMOVE_DYNAMIC_DISPLAY " ")) == 0) {
		gchar *key;

		key = g_strdup (&msg[strlen (MDM_SUP_REMOVE_DYNAMIC_DISPLAY " ")]);
		g_strstrip (key);
		handle_dynamic_server (conn, DYNAMIC_REMOVE, key);
		g_free (key);

	} else if (strncmp (msg, MDM_SUP_RELEASE_DYNAMIC_DISPLAYS " ",
			    strlen (MDM_SUP_RELEASE_DYNAMIC_DISPLAYS " ")) == 0) {

		gchar *key;

		key = g_strdup (&msg[strlen (MDM_SUP_RELEASE_DYNAMIC_DISPLAYS " ")]);
		g_strstrip (key);
		handle_dynamic_server (conn, DYNAMIC_RELEASE, key);
		g_free (key);

	} else if (strcmp (msg, MDM_SUP_VERSION) == 0) {
		mdm_connection_write (conn, "MDM " VERSION "\n");
	} else if (strcmp (msg, MDM_SUP_SERVER_BUSY) == 0) {
		if (mdm_connection_is_server_busy (unixconn))
	        	mdm_connection_write (conn, "OK true\n");
		else
	        	mdm_connection_write (conn, "OK false\n");
	} else if (strcmp (msg, MDM_SUP_CLOSE) == 0) {
		mdm_connection_close (conn);
	} else {
		mdm_connection_write (conn, "ERROR 0 Not implemented\n");
		mdm_connection_close (conn);
	}
}
