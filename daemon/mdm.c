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
#include <syslog.h>

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
#include "getvt.h"
#include "mdm-net.h"
#include "cookie.h"
#include "filecheck.h"
#include "errorgui.h"

#include "mdm-socket-protocol.h"
#include "mdm-daemon-config.h"
#include "mdm-log.h"

/* Local functions */
static void mdm_handle_message (MdmConnection *conn, const gchar *msg, gpointer data);
static void mdm_handle_user_message (MdmConnection *conn, const gchar *msg, gpointer data);
static void mdm_daemonify (void);
static void mdm_safe_restart (void);
static void mdm_try_logout_action (MdmDisplay *disp);
static void mdm_restart_now (void);
static void handle_flexi_server (MdmConnection *conn, int type, const gchar *server, gboolean handled, const gchar *username);

/* Global vars */

gint flexi_servers         = 0; /* Number of flexi servers */
pid_t extra_process = 0;        /* An extra process.  Used for quickie
                                   processes, so that they also get whacked */
static int extra_status    = 0; /* Last status from the last extra process */
pid_t mdm_main_pid         = 0; /* PID of the main daemon */

gboolean another_mdm_is_running  = FALSE;

gboolean mdm_wait_for_go         = FALSE; /* wait for a GO in the fifo */
static gboolean print_version    = FALSE; /* print version number and quit */
static gboolean preserve_ld_vars = FALSE; /* Preserve the ld environment
                                             variables */
static gboolean no_daemon        = FALSE; /* Do not daemonize */
static gboolean no_console       = FALSE; /* There are no static servers, this
                                             means, don't run static servers
                                             and second, don't display info on
                                             the console */

MdmConnection *pipeconn = NULL; /* slavepipe connection */
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
				mdm_error ("Cannot write PID file %s: possibly out of diskspace.  Error: %s", pidfile, strerror (errno));

			}
		} else if G_UNLIKELY (errno != 0) {
			/* FIXME: how to handle this? */
			mdm_fdprintf (2, _("Cannot write PID file %s: possibly out of diskspace.  Error: %s\n"),
				      pidfile, strerror (errno));
			mdm_error ("Cannot write PID file %s: possibly out of diskspace.  Error: %s", pidfile, strerror (errno));

		}

		exit (EXIT_SUCCESS);
	}

	mdm_main_pid = getpid ();

	if G_UNLIKELY (pid < 0)
		mdm_fail ("mdm_daemonify: fork () failed!");

	if G_UNLIKELY (setsid () < 0)
		mdm_fail ("mdm_daemonify: setsid () failed: %s!", strerror (errno));

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
				    svr->handled)
					mdm_first_login = FALSE;
			} else {
				/* only the first static display where
				   we actually log in gets
				   autologged in */
				if (svr != NULL &&
				    svr->handled)
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

	if (another_mdm_is_running) {
		mdm_debug ("mdm_final_cleanup: Another MDM is already running. Leaving displays alone.");		
	}
	else {
		/* Now completely unmanage the static servers */
		first = TRUE;
		list = g_slist_copy (displays);
		/* somewhat of a hack to kill last server
		 * started first.  This mostly makes things end up on
		 * the right vt */
		list = g_slist_reverse (list);
		for (li = list; li != NULL; li = li->next) {
			MdmDisplay *d = li->data;		
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
	}	
	
	/* Close stuff */	

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

	if (another_mdm_is_running) {
		mdm_debug ("mdm_final_cleanup: Another MDM is already running. Leaving %s alone.", MDM_PID_FILE);		
	}
	else {
		mdm_debug ("mdm_final_cleanup: Removing %s", MDM_PID_FILE);
		pidfile = MDM_PID_FILE;
		if (pidfile != NULL) {
			VE_IGNORE_EINTR (g_unlink (pidfile));
		}
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
			mdm_info ("deal_with_x_crashes: Trying failsafe X server %s", failsafe);
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

		mdm_info ("deal_with_x_crashes: Running the XKeepsCrashing script");

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
					if (ret <= 0) {
						mdm_debug("mdm deal_with_x_crashes: sleeping for 10 seconds");
						sleep (10);
					}
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

	mdm_error ("Failed to start X server several times in a short time period; disabling display %s", d->name);

	return FALSE;
}

static gboolean
try_command (const char *command)
{
	gboolean res;
	int      status;

	mdm_debug ("Running %s", command);

	res = TRUE;
	
	status = system (command);
	
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

	mdm_info ("Master suspending...");

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

	mdm_debug ("Master halting...");

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

	mdm_debug ("Restarting computer...");

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

static gint
mdm_exec_script (const gchar *dir,
           const char *login,
           struct passwd *pwent,
           gboolean pass_stdout)
{
  pid_t pid;
  gid_t save_gid;
  gid_t save_egid;
  char *script;
  gchar **argv = NULL;
  gint status;

  if G_UNLIKELY (ve_string_empty (dir))
    return 0;

  script = g_build_filename (dir, "Default", NULL);
  if (g_access (script, R_OK|X_OK) != 0) {
    g_free (script);
    script = NULL;
  }

  if (script == NULL) {
    return 0;
  }

  /*
   * Make sure that gid/egid are set to 0 when running the scripts, so
   * that the scripts are run with standard permisions.  Reset gid/egid
   * back to their original values after running the script.
   */
  save_egid = getegid ();
  save_gid  = getgid ();
  setegid (0);
  setgid (0);

  mdm_debug ("Forking extra process: %s", script);

  pid = mdm_fork_extra ();

  switch (pid) {
  case 0:
    mdm_log_shutdown ();

    VE_IGNORE_EINTR (close (0));
    mdm_open_dev_null (O_RDONLY); /* open stdin - fd 0 */

    if ( ! pass_stdout) {
      VE_IGNORE_EINTR (close (1));
      VE_IGNORE_EINTR (close (2));
      /* No error checking here - if it's messed the best response
       * is to ignore & try to continue */
      mdm_open_dev_null (O_RDWR); /* open stdout - fd 1 */
      mdm_open_dev_null (O_RDWR); /* open stderr - fd 2 */
    }

    mdm_close_all_descriptors (3 /* from */, -1 /* except */, -1 /* except2 */);

    mdm_log_init ();

    if (login != NULL) {
      g_setenv ("LOGNAME", login, TRUE);
      g_setenv ("USER", login, TRUE);
      g_setenv ("USERNAME", login, TRUE);
    } else {
      const char *mdmuser = mdm_daemon_config_get_value_string (MDM_KEY_USER);
      g_setenv ("LOGNAME", mdmuser, TRUE);
      g_setenv ("USER", mdmuser, TRUE);
      g_setenv ("USERNAME", mdmuser, TRUE);
    }
    if (pwent != NULL) {
      if (ve_string_empty (pwent->pw_dir)) {
        g_setenv ("HOME", "/", TRUE);
        g_setenv ("PWD", "/", TRUE);
        VE_IGNORE_EINTR (g_chdir ("/"));
      } else {
        g_setenv ("HOME", pwent->pw_dir, TRUE);
        g_setenv ("PWD", pwent->pw_dir, TRUE);
        VE_IGNORE_EINTR (g_chdir (pwent->pw_dir));
        if (errno != 0) {
          VE_IGNORE_EINTR (g_chdir ("/"));
          g_setenv ("PWD", "/", TRUE);
        }
      }
      g_setenv ("SHELL", pwent->pw_shell, TRUE);
    } else {
      g_setenv ("HOME", "/", TRUE);
      g_setenv ("PWD", "/", TRUE);
      VE_IGNORE_EINTR (g_chdir ("/"));
      g_setenv ("SHELL", "/bin/sh", TRUE);
    }   

    g_unsetenv ("XAUTHORITY");
    g_setenv ("PATH", mdm_daemon_config_get_value_string (MDM_KEY_ROOT_PATH), TRUE);
    g_setenv ("RUNNING_UNDER_MDM", "true", TRUE);
    g_shell_parse_argv (script, NULL, &argv, NULL);

    VE_IGNORE_EINTR (execv (argv[0], argv));
    g_strfreev (argv);
    g_error (_("%s: Failed starting: %s"),
       "mdm_exec_script",
       script);
    _exit (0);

  case -1:
    g_free (script);
    g_error (_("%s: Can't fork script process!"), "mdm_exec_script");

    setgid (save_gid);
    setegid (save_egid);

    return 0;

  default:
    mdm_wait_for_extra (pid, &status);
    g_free (script);

    setgid (save_gid);
    setegid (save_egid);

    if (WIFEXITED (status))
      return WEXITSTATUS (status);
    else
      return 0;
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
		if (d->servpid > 1)
			kill (d->servpid, SIGTERM);
		d->servpid = 0;	

		/* Race avoider */
		mdm_sleep_no_signal (1);
	}

	/* null all these, they are not valid most definately */
	d->servpid    = 0;
	d->sesspid    = 0;
	d->greetpid   = 0;

	/* definately not logged in now */
	d->logged_in = FALSE;
	g_free (d->login);
	d->login = NULL;

	/* Declare the display dead */
	d->slavepid = 0;
	d->dispstat = DISPLAY_DEAD;

	/* Run SuperPost script */
	mdm_exec_script ("/etc/mdm/SuperPost", "root", getpwnam("root"), FALSE /* pass_stdout */);

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
			mdm_info ("Restart MDM, Restart machine, Suspend, or Halt request when there is no system menu from display %s", d->name);
			status = DISPLAY_REMANAGE;
		}


		if ( ! d->attached) {
			mdm_info ("Restart MDM, Restart machine, Suspend or Halt request from a non-static display %s", d->name);
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
		mdm_info ("mdm_child_action: Aborting display %s", d->name);

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
			mdm_debug ("mdm_child_action: Static server died, starting a new slave");
			if ( ! mdm_display_manage (d)) {
				mdm_display_unmanage (d);
				/* If there are some pending statics,
				 * start them now */
				mdm_start_first_unborn_local (3 /* delay */);
			}
		} else if (d->type == TYPE_FLEXI) {
			/* A flexi server is dying, scan for a greeter.
			 * If there's one, chvt() into it			 
			 */			
			mdm_debug ("mdm_child_action: Flexible server died, scanning for a greeter");
			GSList *li;
			for (li = mdm_daemon_config_get_display_list (); li != NULL; li = li->next) {
				MdmDisplay *disp = li->data;
				if (disp->greetpid > 0) {
					mdm_debug ("mdm_child_action: Found a greeter on %d, changing VT.", disp->vt);
					mdm_change_vt (disp->vt);							
					break;
				}
			}

			/*
			 * If this was a chooser session and we have chosen a
			 * host, then we don't want to unmanage, we want to
			 * manage and choose that host
			 */
			if (d->chosen_hostname != NULL) {
				if ( ! mdm_display_manage (d)) {
					mdm_display_unmanage (d);
				}
			} else {
				/* else, this is a one time thing */
				mdm_display_unmanage (d);
			}
			/* Remote displays will send a request to be managed */
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
	mdm_info ("MDM restarting ...");
	mdm_final_cleanup ();
	mdm_restoreenv ();
	VE_IGNORE_EINTR (execvp (stored_argv[0], stored_argv));
	mdm_error ("Failed to restart self");
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
	mdm_error ("main daemon: Got SIGABRT. Something went very wrong. Going down!");
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
			mdm_debug ("mainloop_sig_callback: Got SIGCHLD!");
			while (mdm_cleanup_children ())
				;
			break;

		case SIGINT:
		case SIGTERM:
			mdm_debug ("mainloop_sig_callback: Got SIGTERM/SIGINT. Going down!");
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
			mdm_debug ("mainloop_sig_callback: Got SIGHUP!");
			mdm_restart_now ();
			break;

		case SIGUSR1:
			mdm_debug ("mainloop_sig_callback: Got SIGUSR1!");
			mdm_restart_mode = TRUE;
			mdm_safe_restart ();
			break;

		default:
			mdm_debug ("mainloop_sig_callback: Unkown signal!");
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
		mdm_error ("Can't open %s for writing", file);
		g_free (file);
		return;
	}

	VE_IGNORE_EINTR (fprintf (fp, "%s\n", mdm_global_cookie));

	/* FIXME: What about out of disk space errors? */
	errno = 0;
	VE_IGNORE_EINTR (fclose (fp));
	if G_UNLIKELY (errno != 0) {
		mdm_error ("Can't write to %s: %s", file,
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

	ctx = g_option_context_new (_("- The MDM login manager"));
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

	if (print_version) {
		printf ("MDM %s\n", VERSION);
		fflush (stdout);
		exit (0);
	}

	/* XDM compliant error message */
	if G_UNLIKELY (getuid () != 0) {
		/* make sure the pid file doesn't get wiped */
		mdm_error ("Only root wants to run MDM\n");
		exit (-1);
	}

	// If another MDM is already running, leave immediately!
	openlog ("mdm", LOG_CONS | LOG_PID | LOG_NDELAY, LOG_LOCAL1);
	char * command = g_strdup_printf ("pidof -s mdm -o %d", getpid());
    if(system(command) == 0) {
    	int otherpid;
		FILE *fp = popen(command, "r");
		fscanf(fp, "%d", &otherpid);
		pclose(fp);
		syslog (LOG_INFO, "Another MDM is running under process ID: %d", otherpid);
		if (getpid() > otherpid) {
			syslog(LOG_INFO, "Exiting...");
			exit(1);
		}
    }

    syslog(LOG_INFO, "Starting mdm...");
	closelog ();	

	mdm_log_init ();
	/* Parse configuration file */
	mdm_daemon_config_parse (config_file, no_console);

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
		mdm_fail ("main: Error setting up %s signal handler: %s", "TERM", strerror (errno));

	if G_UNLIKELY (sigaction (SIGINT, &sig, NULL) < 0)
		mdm_fail ("main: Error setting up %s signal handler: %s", "INT", strerror (errno));

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
			another_mdm_is_running = TRUE;
			mdm_fail ("MDM already running. Aborting!");			
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
				mdm_error ("Cannot write PID file %s: possibly out of diskspace.  Error: %s", pidfile, strerror (errno));

			}
		} else if (errno != 0) {
			/* FIXME: how to handle this? */
			mdm_fdprintf (2, _("Cannot write PID file %s: possibly out of diskspace.  Error: %s\n"),
				      pidfile, strerror (errno));
			mdm_error ("Cannot write PID file %s: possibly out of diskspace.  Error: %s", pidfile, strerror (errno));

		}

		VE_IGNORE_EINTR (g_chdir (mdm_daemon_config_get_value_string (MDM_KEY_SERV_AUTHDIR)));
		umask (022);
	}
	else
		mdm_daemonify ();

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
		mdm_fail ("main: Error setting up %s signal handler: %s", "TERM", strerror (errno));

	if G_UNLIKELY (sigaction (SIGINT, &sig, NULL) < 0)
		mdm_fail ("main: Error setting up %s signal handler: %s", "INT", strerror (errno));

	if G_UNLIKELY (sigaction (SIGHUP, &sig, NULL) < 0)
		mdm_fail ("main: Error setting up %s signal handler: %s", "HUP", strerror (errno));

	if G_UNLIKELY (sigaction (SIGUSR1, &sig, NULL) < 0)
		mdm_fail ("main: Error setting up %s signal handler: %s", "USR1", strerror (errno));

	/* some process limit signals we catch and restart on,
	   note that we don't catch these in the slave, but then
	   we catch those in the main daemon as slave crashing
	   (terminated by signal), and we clean up appropriately */
#ifdef SIGXCPU
	ve_signal_add (SIGXCPU, mainloop_sig_callback, NULL);
	if G_UNLIKELY (sigaction (SIGXCPU, &sig, NULL) < 0)
		mdm_fail ("main: Error setting up %s signal handler: %s", "XCPU", strerror (errno));
#endif
#ifdef SIGXFSZ
	ve_signal_add (SIGXFSZ, mainloop_sig_callback, NULL);
	if G_UNLIKELY (sigaction (SIGXFSZ, &sig, NULL) < 0)
		mdm_fail ("main: Error setting up %s signal handler: %s", "XFSZ", strerror (errno));
#endif

	/* cannot use mainloop for SIGABRT, the handler can never
	   return */
	abrt.sa_handler = main_daemon_abrt;
	abrt.sa_flags = SA_RESTART;
	sigemptyset (&abrt.sa_mask);

	if G_UNLIKELY (sigaction (SIGABRT, &abrt, NULL) < 0)
		mdm_fail ("main: Error setting up %s signal handler: %s", "ABRT", strerror (errno));

	child.sa_handler = ve_signal_notify;
	child.sa_flags = SA_RESTART|SA_NOCLDSTOP;
	sigemptyset (&child.sa_mask);
	sigaddset (&child.sa_mask, SIGCHLD);

	if G_UNLIKELY (sigaction (SIGCHLD, &child, NULL) < 0)
		mdm_fail ("mdm_main: Error setting up CHLD signal handler");

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

	create_connections ();

	/* make sure things (currently /tmp/.ICE-unix and /tmp/.X11-unix)
	 * are sane */
	mdm_ensure_sanity () ;

	/* Make us a unique global cookie to authenticate */
	mdm_make_global_cookie ();

	/* Start static X servers */
	mdm_start_first_unborn_local (0 /* delay */);	

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
		mdm_error ("Can't open %s for writing", file);
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
		mdm_error ("Can't write to %s: %s", file, strerror (errno));
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
		}
	}

	if (strncmp (msg, MDM_SOP_XPID " ",
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
	} else if (strcmp (msg, MDM_SOP_START_NEXT_LOCAL) == 0) {
		mdm_start_first_unborn_local (3 /* delay */);
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

		mdm_info ("Master suspending...");

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

static void
handle_flexi_server (MdmConnection *conn, int type, const char *server, gboolean handled, const char *username)
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

	flexi_servers++;

	display->preset_user = g_strdup (username);
	display->type = type;
	display->socket_conn = conn;
		
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
			g_string_append_printf (retMsg, "%d", disp->vt);			
		}
	}

	g_string_append (retMsg, "\n");
	mdm_connection_write (conn, retMsg->str);
	g_free (key);
	g_string_free (retMsg, TRUE);
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
		mdm_info ("%s request denied: Not authenticated", "QUERY_LOGOUT_ACTION");
		mdm_connection_write (conn, "ERROR 100 Not authenticated\n");
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

	g_string_append (reply, "\n");
	mdm_connection_write (conn, reply->str);
	g_string_free (reply, TRUE);
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
		mdm_info ("%s request denied: Not authenticated", "SET_LOGOUT_ACTION");
		mdm_connection_write (conn, "ERROR 100 Not authenticated\n");
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
		mdm_info ("%s request denied: Not authenticated", "SET_LOGOUT_ACTION");
		mdm_connection_write (conn, "ERROR 100 Not authenticated\n");
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
		mdm_info ("%s request denied: Not authenticated", "QUERY_VT");
		mdm_connection_write (conn,"ERROR 100 Not authenticated\n");
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
		mdm_info ("%s request denied: Not authenticated", "QUERY_VT");
		mdm_connection_write (conn, "ERROR 100 Not authenticated\n");
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
			mdm_info ("%s request denied: Not authenticated", "FLEXI_XSERVER");
			mdm_connection_write (conn, "ERROR 100 Not authenticated\n");
			return;
		}

		handle_flexi_server (conn, TYPE_FLEXI, mdm_daemon_config_get_value_string (MDM_KEY_STANDARD_XSERVER), TRUE, NULL);

	} else if ((strncmp (msg, MDM_SUP_ATTACHED_SERVERS,
	                     strlen (MDM_SUP_ATTACHED_SERVERS)) == 0)) {

		sup_handle_attached_servers (conn, msg, data);

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
	} else if (strcmp (msg, MDM_SUP_VERSION) == 0) {
		mdm_connection_write (conn, "MDM " VERSION "\n");
	} else if (strcmp (msg, MDM_SUP_CLOSE) == 0) {
		mdm_connection_close (conn);
	} else {
		mdm_connection_write (conn, "ERROR 0 Not implemented\n");
		mdm_connection_close (conn);
	}
}
