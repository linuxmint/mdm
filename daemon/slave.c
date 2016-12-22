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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/* This is the mdm slave process. mdmslave runs the greeter
 * and the user's session scripts. */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <unistd.h>
#include <utime.h>
#if defined (_POSIX_PRIORITY_SCHEDULING) && defined (HAVE_SCHED_YIELD)
#include <sched.h>
#endif
#ifdef HAVE_LOGINCAP
#include <login_cap.h>
#endif
#include <fcntl.h>
#if defined(HAVE_SYS_PARAM_H)
#include <sys/param.h>
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <strings.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#if defined(HAVE_UTMPX_H)
#include <utmpx.h>
#endif
#if defined(HAVE_UTMP_H)
#include <utmp.h>
#endif
#if defined(HAVE_LIBUTIL_H)
#include <libutil.h>
#endif

#if !defined (MAXPATHLEN) && defined (PATH_MAX)
#define MAXPATHLEN PATH_MAX
#endif

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#ifdef HAVE_XFREE_XINERAMA
#include <X11/extensions/Xinerama.h>
#elif HAVE_SOLARIS_XINERAMA
#include <X11/extensions/xinerama.h>
#endif

#if defined (CAN_USE_SETPENV) && defined (HAVE_USERSEC_H)
#include <usersec.h>
#endif

#include <signal.h>
#include <pwd.h>
#include <grp.h>
#include <errno.h>
#include <time.h>

#ifdef HAVE_SELINUX
#include <selinux/selinux.h>
#include <selinux/get_context_list.h>
#endif /* HAVE_SELINUX */

#include <glib/gi18n.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>

#include "mdm.h"
#include "slave.h"
#include "misc.h"
#include "verify.h"
#include "filecheck.h"
#include "auth.h"
#include "server.h"
#include "getvt.h"
#include "errorgui.h"
#include "cookie.h"
#include "display.h"

#include "mdm-common.h"
#include "mdm-log.h"
#include "mdm-daemon-config.h"

#include "mdm-socket-protocol.h"

#ifdef WITH_CONSOLE_KIT
#include "mdmconsolekit.h"
#endif

#ifndef MDM_BAD_RECORDS_FILE
#define MDM_BAD_RECORDS_FILE "/var/log/btmp"
#endif

#ifndef MDM_NEW_RECORDS_FILE
#define MDM_NEW_RECORDS_FILE "/var/log/wtmp"
#endif

/* Per-slave globals */

static MdmDisplay *d                   = 0;
static gchar *login_user               = NULL;
static gboolean greet                  = FALSE;
static gboolean configurator           = FALSE;
static gboolean remanage_asap          = FALSE;
static gboolean got_xfsz_signal        = FALSE;
static gboolean do_timed_login         = FALSE; /* If this is true, login the
                                                   timed login */
static gboolean do_configurator        = FALSE; /* If this is true, login as 
					         * root and start the
                                                 * configurator */
static gboolean do_cancel              = FALSE; /* If this is true, go back to
                                                   username entry & unselect
                                                   face browser (if present) */
static gboolean do_restart_greeter     = FALSE; /* If this is true, whack the
					           greeter and try again */
static gboolean restart_greeter_now    = FALSE; /* Restart_greeter_when the
                                                   SIGCHLD hits */
static gboolean always_restart_greeter = FALSE; /* Always restart greeter when
                                                   the user accepts restarts. */
static gboolean mdm_wait_for_ack       = TRUE;  /* Wait for ack on all messages
                                                   to the daemon */
static int in_session_stop             = 0;
static int in_usr2_signal              = 0;
static gboolean need_to_quit_after_session_stop = FALSE;
static int exit_code_to_use            = DISPLAY_REMANAGE;
static gboolean session_started        = FALSE;
static gboolean greeter_disabled       = FALSE;
static gboolean greeter_no_focus       = FALSE;

static uid_t logged_in_uid             = -1;
static gid_t logged_in_gid             = -1;

static int greeter_fd_out              = -1;
static int greeter_fd_in               = -1;

static gboolean interrupted            = FALSE;
static gchar *ParsedAutomaticLogin     = NULL;
static gchar *ParsedTimedLogin         = NULL;

static int mdm_in_signal               = 0;
static int mdm_normal_runlevel         = -1;
static pid_t extra_process             = 0;
static int extra_status                = 0;

static int slave_waitpid_r             = -1;
static int slave_waitpid_w             = -1;
static GSList *slave_waitpids          = NULL;

extern gboolean mdm_first_login;

/* The slavepipe, this is the write end */
extern int slave_fifo_pipe_fd;

/* wait for a GO in the SOP protocol */
extern gboolean mdm_wait_for_go;

extern char *mdm_system_locale;

typedef struct {
	pid_t pid;
} MdmWaitPid;

/* Local prototypes */
static gint   mdm_slave_xerror_handler (Display *disp, XErrorEvent *evt);
static gint   mdm_slave_xioerror_handler (Display *disp);
static void   mdm_slave_run (MdmDisplay *display);
static void   mdm_slave_wait_for_login (void);
static void   mdm_slave_greeter (void);
static void   mdm_slave_session_start (void);
static void   mdm_slave_session_stop (gboolean run_post_session,
					gboolean no_shutdown_check);
static void   mdm_slave_term_handler (int sig);
static void   mdm_slave_usr2_handler (int sig);
static void   mdm_slave_quick_exit (gint status);
static void   mdm_slave_exit (gint status, const gchar *format, ...) G_GNUC_PRINTF (2, 3);
static void   mdm_child_exit (gint status, const gchar *format, ...) G_GNUC_PRINTF (2, 3);
static gint   mdm_slave_exec_script (MdmDisplay *d, const gchar *dir,
				       const char *login, struct passwd *pwent,
				       gboolean pass_stdout);
static gchar *mdm_slave_parse_enriched_login (MdmDisplay *d, const gchar *s);
static void   mdm_slave_handle_usr2_message (void);
static void   mdm_slave_handle_notify (const char *msg);
static void   check_notifies_now (void);
static void   restart_the_greeter (void);

gboolean mdm_is_user_valid (const char *username);

/* Yay thread unsafety */
static gboolean x_error_occurred = FALSE;
static gboolean mdm_got_ack = FALSE;
static char * mdm_ack_response = NULL;
char * mdm_ack_question_response = NULL;
static GList *unhandled_notifies = NULL;

/* for signals that want to exit */
static Jmp_buf slave_start_jmp;
static gboolean return_to_slave_start_jmp = FALSE;
static gboolean already_in_slave_start_jmp = FALSE;
static char *slave_start_jmp_error_to_print = NULL;
enum {
	JMP_FIRST_RUN = 0,
	JMP_SESSION_STOP_AND_QUIT = 1,
	JMP_JUST_QUIT_QUICKLY = 2
};
#define DEFAULT_LANGUAGE "Default"
#define SIGNAL_EXIT_WITH_JMP(d,how) \
   {											\
	if ((d)->slavepid == getpid () && return_to_slave_start_jmp) {			\
		already_in_slave_start_jmp = TRUE;					\
		Longjmp (slave_start_jmp, how);						\
	} else {									\
		/* evil! how this this happen */					\
		if (slave_start_jmp_error_to_print != NULL)				\
			mdm_error (slave_start_jmp_error_to_print);			\
		mdm_error ("Bad (very very VERY bad!) things happening in signal");	\
		_exit (DISPLAY_REMANAGE);						\
	}										\
   }

/* Notify all waitpids, make waitpids check notifies */
static void
slave_waitpid_notify (void)
{
	/* we're in no slave waitpids */
	if (slave_waitpids == NULL)
		return;

	mdm_sigchld_block_push ();

	if (slave_waitpid_w >= 0)
		VE_IGNORE_EINTR (write (slave_waitpid_w, "N", 1));

	mdm_sigchld_block_pop ();
}

/* Make sure to wrap this call with sigchld blocks */
static MdmWaitPid *
slave_waitpid_setpid (pid_t pid)
{
	int p[2];
	MdmWaitPid *wp;

	if G_UNLIKELY (pid <= 1)
		return NULL;

	wp = g_new0 (MdmWaitPid, 1);
	wp->pid = pid;

	if (slave_waitpid_r < 0) {
		if G_UNLIKELY (pipe (p) < 0) {
			mdm_error ("slave_waitpid_setpid: cannot create pipe, trying to wing it");
		} else {
			slave_waitpid_r = p[0];
			slave_waitpid_w = p[1];
		}
	}

	slave_waitpids = g_slist_prepend (slave_waitpids, wp);
	return wp;
}

static void
run_session_output (gboolean read_until_eof)
{
	char buf[256];
	int r, written;
	uid_t old;
	gid_t oldg;

	old = geteuid ();
	oldg = getegid ();

	/* make sure we can set the gid */
	NEVER_FAILS_seteuid (0);

	/* make sure we are the user when we do this,
	   for purposes of file limits and all that kind of
	   stuff */
	if G_LIKELY (logged_in_gid >= 0) {
		if G_UNLIKELY (setegid (logged_in_gid) != 0) {
			mdm_error ("Can't set EGID to user GID");
			NEVER_FAILS_root_set_euid_egid (old, oldg);
			return;
		}
	}

	if G_LIKELY (logged_in_uid >= 0) {
		if G_UNLIKELY (seteuid (logged_in_uid) != 0) {
			mdm_error ("Can't set EUID to user UID");
			NEVER_FAILS_root_set_euid_egid (old, oldg);
			return;
		}
	}

	gboolean limit_output = mdm_daemon_config_get_value_bool (MDM_KEY_LIMIT_SESSION_OUTPUT);
	gboolean filter_output = mdm_daemon_config_get_value_bool (MDM_KEY_FILTER_SESSION_OUTPUT);

	/* the fd is non-blocking */
	for (;;) {
		VE_IGNORE_EINTR (r = read (d->session_output_fd, buf, sizeof (buf)));		

		/* EOF */
		if G_UNLIKELY (r == 0) {
			VE_IGNORE_EINTR (close (d->session_output_fd));
			d->session_output_fd = -1;
			VE_IGNORE_EINTR (close (d->xsession_errors_fd));
			d->xsession_errors_fd = -1;
			break;
		}

		/* Nothing to read */
		if (r < 0 && errno == EAGAIN)
			break;

		/* some evil error */
		if G_UNLIKELY (r < 0) {
			mdm_error ("error reading from session output, closing the pipe");
			VE_IGNORE_EINTR (close (d->session_output_fd));
			d->session_output_fd = -1;
			VE_IGNORE_EINTR (close (d->xsession_errors_fd));
			d->xsession_errors_fd = -1;
			break;
		}

		if G_UNLIKELY (limit_output && d->xsession_errors_bytes >= MAX_XSESSION_ERRORS_BYTES || got_xfsz_signal) {
			continue;
		}

		if G_UNLIKELY (filter_output &&
			(  g_strrstr(buf, "Gtk-WARNING") != NULL 
			|| g_strrstr(buf, "Gtk-CRITICAL") != NULL
			|| g_strrstr(buf, "Clutter-WARNING") != NULL 
			|| g_strrstr(buf, "Clutter-CRITICAL") != NULL 
			|| g_strrstr(buf, "GLib-GObject-WARNING") != NULL
			|| g_strrstr(buf, "GLib-GObject-CRITICAL") != NULL
			|| g_strrstr(buf, "GLib-GIO-WARNING") != NULL
			|| g_strrstr(buf, "GLib-GIO-CRITICAL") != NULL
			|| g_strrstr(buf, "libglade-WARNING") != NULL
			|| g_strrstr(buf, "libglade-CRITICAL") != NULL
			|| g_strrstr(buf, "GStreamer-WARNING") != NULL
			|| g_strrstr(buf, "GStreamer-CRITICAL") != NULL)) {
		 	continue;
		}

		/* write until we succeed in writing something */
		VE_IGNORE_EINTR (written = write (d->xsession_errors_fd, buf, r));
		if G_UNLIKELY (written < 0 || got_xfsz_signal) {
			/* evil! */
			break;
		}		

		/* write until we succeed in writing everything */
		while G_UNLIKELY (written < r) {
			int n;
			VE_IGNORE_EINTR (n = write (d->xsession_errors_fd, &buf[written], r-written));
			if G_UNLIKELY (n < 0 || got_xfsz_signal) {
				/* evil! */
				break;
			}
			written += n;
		}

		d->xsession_errors_bytes += r;

		if G_UNLIKELY (limit_output && d->xsession_errors_bytes >= MAX_XSESSION_ERRORS_BYTES && ! got_xfsz_signal) {
			VE_IGNORE_EINTR (write (d->xsession_errors_fd,
				        "\n\n --- MDM: .xsession-errors output limit reached. No more output will be written. ---\n --- Set 'LimitSessionOutput=false' in the [debug] section of /etc/mdm/mdm.conf to disable this limit. ---\n\n",
				strlen ("\n\n --- MDM: .xsession-errors output limit reached. No more output will be written. ---\n --- Set 'LimitSessionOutput=false' in the [debug] section of /etc/mdm/mdm.conf to disable this limit. ---\n\n")));
		}

		/* there wasn't more then buf available, so no need to try reading
		 * again, unless we really want to */
		if (r < sizeof (buf) && ! read_until_eof)
			break;
	}

	NEVER_FAILS_root_set_euid_egid (old, oldg);
}

#define TIME_UNSET_P(tv) ((tv)->tv_sec == 0 && (tv)->tv_usec == 0)

/* Try to touch an authfb auth file every 12 hours.  That way if it's
 * in /tmp it doesn't get whacked by tmpwatch */
#define TRY_TO_TOUCH_TIME (60*60*12)

static struct timeval *
min_time_to_wait (struct timeval *tv)
{
	if (d->authfb) {
		time_t ct = time (NULL);
		time_t sec_to_wait;

		if (d->last_auth_touch + TRY_TO_TOUCH_TIME + 5 <= ct)
			sec_to_wait = 5;
		else
			sec_to_wait = (d->last_auth_touch + TRY_TO_TOUCH_TIME) - ct;

		if (TIME_UNSET_P (tv) ||
		    sec_to_wait < tv->tv_sec)
			tv->tv_sec = sec_to_wait;
	}
	if (TIME_UNSET_P (tv))
		return NULL;    
	else 
		return tv;    
}

static void
try_to_touch_fb_userauth (void)
{
	if (d->authfb && d->userauth != NULL && logged_in_uid >= 0) {
		time_t ct = time (NULL);

		if (d->last_auth_touch + TRY_TO_TOUCH_TIME <= ct) {
			uid_t old;
			gid_t oldg;

			old = geteuid ();
			oldg = getegid ();

			NEVER_FAILS_seteuid (0);

			/* make sure we are the user when we do this,
			   for purposes of file limits and all that kind of
			   stuff */
			if G_LIKELY (logged_in_gid >= 0) {
				if G_UNLIKELY (setegid (logged_in_gid) != 0) {
					mdm_error ("Can't set GID to user GID");
					NEVER_FAILS_root_set_euid_egid (old, oldg);
					return;
				}
			}

			if G_LIKELY (logged_in_uid >= 0) {
				if G_UNLIKELY (seteuid (logged_in_uid) != 0) {
					mdm_error ("Can't set UID to user UID");
					NEVER_FAILS_root_set_euid_egid (old, oldg);
					return;
				}
			}

			/* This will "touch" the file */
			utime (d->userauth, NULL);

			NEVER_FAILS_root_set_euid_egid (old, oldg);

			d->last_auth_touch = ct;
		}
	}
}

/* must call slave_waitpid_setpid before calling this */
static void
slave_waitpid (MdmWaitPid *wp)
{
	if G_UNLIKELY (wp == NULL)
		return;

	mdm_debug ("slave_waitpid: waiting on %d", (int)wp->pid);

	if G_UNLIKELY (slave_waitpid_r < 0) {
		mdm_error ("slave_waitpid: no pipe, trying to wing it");

		/* This is a real stupid fallback for a real stupid case */
		while (wp->pid > 1) {
			struct timeval tv;
			/* Wait 5 seconds. */
			tv.tv_sec = 5;
			tv.tv_usec = 0;
			select (0, NULL, NULL, NULL, min_time_to_wait (&tv));
			
			/* try to touch an fb auth file */
			try_to_touch_fb_userauth ();

			if (d->session_output_fd >= 0)
				run_session_output (FALSE /* read_until_eof */);			
			check_notifies_now ();
		}
		check_notifies_now ();
	} else {
		gboolean read_session_output = TRUE;

		do {
            // mdm_debug ("slave_waitpid: start loop");
			char buf[1];
			fd_set rfds;
			int ret;
			struct timeval tv;
			int maxfd;

			FD_ZERO (&rfds);
			FD_SET (slave_waitpid_r, &rfds);
			if (read_session_output &&
			    d->session_output_fd >= 0) {
				FD_SET (d->session_output_fd, &rfds);
                // mdm_debug ("slave_waitpid: no session");
            }			

			/* unset time */
			tv.tv_sec = 0;
			tv.tv_usec = 0;
			maxfd = MAX (slave_waitpid_r, d->session_output_fd);
            
            struct timeval * timetowait = min_time_to_wait (&tv);

            // mdm_debug ("slave_waitpid: ret = %d", (int) ret);
            // if (timetowait != NULL) {
            //     mdm_debug ("slave_waitpid: timetowait = %d, %d", (int) timetowait->tv_sec, (int) timetowait->tv_usec);
            // }
            // else {
            //     mdm_debug ("slave_waitpid: timetowait = NULL");
            // }
            // mdm_debug ("slave_waitpid: slave_waitpid_r = %d", (int) slave_waitpid_r);
            // mdm_debug ("slave_waitpid: d->session_output_fd = %d", (int) d->session_output_fd);
            // mdm_debug ("slave_waitpid: MAX = %d", (int) maxfd);                

			ret = select (maxfd + 1, &rfds, NULL, NULL, timetowait);

            // mdm_debug ("slave_waitpid: ret = %d", (int) ret);

			/* try to touch an fb auth file */
			try_to_touch_fb_userauth ();
                                    
			if (ret > 0) {
			       	if (FD_ISSET (slave_waitpid_r, &rfds)) {
					VE_IGNORE_EINTR (read (slave_waitpid_r, buf, 1));
				}
				if (d->session_output_fd >= 0 &&
				    FD_ISSET (d->session_output_fd, &rfds)) {
					run_session_output (FALSE /* read_until_eof */);
				}				
			} else if (errno == EBADF) {
				read_session_output = FALSE;
                mdm_debug ("slave_waitpid: errno = EBADF");
			} else if (errno == EINTR) {				
                mdm_debug ("slave_waitpid: errno = EINTR");   
            } else if (errno == EINVAL) {				
                mdm_debug ("slave_waitpid: errno = EINVAL");
            }
            else {
                mdm_debug ("slave_waitpid: errno = unknown error");
            }
			check_notifies_now ();
		} while (wp->pid > 1);
		check_notifies_now ();
	}

	mdm_sigchld_block_push ();

	wp->pid = -1;

	slave_waitpids = g_slist_remove (slave_waitpids, wp);
	g_free (wp);

	mdm_sigchld_block_pop ();

	mdm_debug ("slave_waitpid: done_waiting");
}

static void
check_notifies_now (void)
{
	GList *list, *li;

	if (restart_greeter_now &&
	    do_restart_greeter) {
		do_restart_greeter = FALSE;
		restart_the_greeter ();
	}

	while (unhandled_notifies != NULL) {
		mdm_sigusr2_block_push ();
		list = unhandled_notifies;
		unhandled_notifies = NULL;
		mdm_sigusr2_block_pop ();

		for (li = list; li != NULL; li = li->next) {
			char *s = li->data;
			li->data = NULL;

			mdm_slave_handle_notify (s);

			g_free (s);
		}
		g_list_free (list);
	}

	if (restart_greeter_now &&
	    do_restart_greeter) {
		do_restart_greeter = FALSE;
		restart_the_greeter ();
	}
}

static void
mdm_slave_desensitize_config (void)
{
	if (configurator &&
	    d->dsp != NULL) {
		gulong foo = 1;
		Atom atom = XInternAtom (d->dsp,
					 "_MDM_SETUP_INSENSITIVE",
					 False);
		XChangeProperty (d->dsp,
				 DefaultRootWindow (d->dsp),
				 atom,
				 XA_CARDINAL, 32, PropModeReplace,
				 (unsigned char *) &foo, 1);
		XSync (d->dsp, False);
	}

}

static void
mdm_slave_sensitize_config (void)
{
	if (d->dsp != NULL) {
		XDeleteProperty (d->dsp,
				 DefaultRootWindow (d->dsp),
				 XInternAtom (d->dsp,
					      "_MDM_SETUP_INSENSITIVE",
					      False));
		XSync (d->dsp, False);
	}
}

/* ignore handlers */
static int
ignore_xerror_handler (Display *disp, XErrorEvent *evt)
{
	x_error_occurred = TRUE;
	return 0;
}

static void
whack_greeter_fds (void)
{
	if (greeter_fd_out > 0)
		VE_IGNORE_EINTR (close (greeter_fd_out));
	greeter_fd_out = -1;
	if (greeter_fd_in > 0)
		VE_IGNORE_EINTR (close (greeter_fd_in));
	greeter_fd_in = -1;
}

static void
term_session_stop_and_quit (void)
{
	mdm_in_signal = 0;
	already_in_slave_start_jmp = TRUE;
	mdm_wait_for_ack = FALSE;
	need_to_quit_after_session_stop = TRUE;

	if (slave_start_jmp_error_to_print != NULL)
		mdm_error (slave_start_jmp_error_to_print);
	slave_start_jmp_error_to_print = NULL;

	/* only if we're not hanging in session stop and getting a
	   TERM signal again */
	if (in_session_stop == 0 && session_started)
		mdm_slave_session_stop (d->logged_in && login_user != NULL,
					TRUE /* no_shutdown_check */);

	mdm_debug ("term_session_stop_and_quit: Final cleanup");

	/* Well now we're just going to kill
	 * everything including the X server,
	 * so no need doing XCloseDisplay which
	 * may just get us an XIOError */
	d->dsp = NULL;

	//mdm_slave_quick_exit (exit_code_to_use);
}

static void
term_quit (void)
{
	mdm_in_signal = 0;
	already_in_slave_start_jmp = TRUE;
	mdm_wait_for_ack = FALSE;
	need_to_quit_after_session_stop = TRUE;

	if (slave_start_jmp_error_to_print != NULL)
		mdm_error (slave_start_jmp_error_to_print);
	slave_start_jmp_error_to_print = NULL;

	mdm_debug ("term_quit: Final cleanup");

	/* Well now we're just going to kill
	 * everything including the X server,
	 * so no need doing XCloseDisplay which
	 * may just get us an XIOError */
	d->dsp = NULL;

	mdm_slave_quick_exit (exit_code_to_use);
}

static gboolean
parent_exists (void)
{
	pid_t ppid = getppid ();
	static gboolean parent_dead = FALSE; /* once dead, always dead */

	if G_UNLIKELY (parent_dead ||
		       ppid <= 1 ||
		       kill (ppid, 0) < 0) {
		parent_dead = TRUE;
		return FALSE;
	}
	return TRUE;
}

#ifdef SIGXFSZ
static void
mdm_slave_xfsz_handler (int signal)
{
	mdm_in_signal++;

	/* in places where we care we can check
	 * and stop writing */
	got_xfsz_signal = TRUE;

	/* whack self ASAP */
	remanage_asap = TRUE;

	mdm_in_signal--;
}
#endif /* SIGXFSZ */


static int
get_runlevel (void)
{
	int rl;

	rl = -1;
#ifdef __linux__
	/* on linux we get our current runlevel, for use later
	 * to detect a shutdown going on, and not mess up. */
	if (g_access ("/sbin/runlevel", X_OK) == 0) {
		char ign;
		int rnl;
		FILE *fp = popen ("/sbin/runlevel", "r");
		if (fp != NULL) {
			if (fscanf (fp, "%c %d", &ign, &rnl) == 2) {
				rl = rnl;
			}
			pclose (fp);
		}
	}
#endif /* __linux__ */
	return rl;
}

void
mdm_slave_start (MdmDisplay *display)
{
	time_t first_time;
	int death_count;
	struct sigaction alrm, term, child, usr2;
#ifdef SIGXFSZ
	struct sigaction xfsz;
#endif /* SIGXFSZ */
	sigset_t mask;

	/*
	 * Set d global to display before setting signal handlers,
	 * since the signal handlers use the d value.  Avoids a 
	 * race condition.  It is also set again in mdm_slave_run
	 * since it is called in a loop.
	 */
	d = display;

	mdm_normal_runlevel = get_runlevel ();

	/* Ignore SIGUSR1/SIGPIPE, and especially ignore it
	   before the Setjmp */
	mdm_signal_ignore (SIGUSR1);
	mdm_signal_ignore (SIGPIPE);

	/* ignore power failures, up to user processes to
	 * handle things correctly */
#ifdef SIGPWR
	mdm_signal_ignore (SIGPWR);
#endif

	/* The signals we wish to listen to */
	sigemptyset (&mask);
	sigaddset (&mask, SIGINT);
	sigaddset (&mask, SIGTERM);
	sigaddset (&mask, SIGCHLD);
	sigaddset (&mask, SIGUSR2);
	sigaddset (&mask, SIGUSR1); /* normally we ignore USR1 */

	/* must set signal mask before the Setjmp as it will be
	   restored, and we're only interested in catching the above signals */
	sigprocmask (SIG_UNBLOCK, &mask, NULL);


	if G_UNLIKELY (display == NULL) {
		/* saaay ... what? */
		_exit (DISPLAY_REMANAGE);
	}

	mdm_debug ("mdm_slave_start: Starting slave process for %s", display->name);

	switch (Setjmp (slave_start_jmp)) {
	case JMP_FIRST_RUN:
		return_to_slave_start_jmp = TRUE;
		break;
	case JMP_SESSION_STOP_AND_QUIT:
		term_session_stop_and_quit ();
		/* huh? should never get here */
		_exit (DISPLAY_REMANAGE);
	default:
	case JMP_JUST_QUIT_QUICKLY:
		term_quit ();
		/* huh? should never get here */
		_exit (DISPLAY_REMANAGE);
	}
	
	/* Handle a INT/TERM signals from mdm master */
	term.sa_handler = mdm_slave_term_handler;
	term.sa_flags = SA_RESTART;
	sigemptyset (&term.sa_mask);
	sigaddset (&term.sa_mask, SIGTERM);
	sigaddset (&term.sa_mask, SIGINT);

	if G_UNLIKELY ((sigaction (SIGTERM, &term, NULL) < 0) ||
		       (sigaction (SIGINT, &term, NULL) < 0))
		mdm_slave_exit (DISPLAY_ABORT,
				_("%s: Error setting up %s signal handler: %s"),
				"mdm_slave_start", "TERM/INT", strerror (errno));

	/* Child handler. Keeps an eye on greeter/session */
	child.sa_handler = mdm_slave_child_handler;
	child.sa_flags = SA_RESTART|SA_NOCLDSTOP;
	sigemptyset (&child.sa_mask);
	sigaddset (&child.sa_mask, SIGCHLD);

	if G_UNLIKELY (sigaction (SIGCHLD, &child, NULL) < 0)
		mdm_slave_exit (DISPLAY_ABORT, _("%s: Error setting up %s signal handler: %s"),
				"mdm_slave_start", "CHLD", strerror (errno));

	/* Handle a USR2 which is ack from master that it received a message */
	usr2.sa_handler = mdm_slave_usr2_handler;
	usr2.sa_flags = SA_RESTART;
	sigemptyset (&usr2.sa_mask);
	sigaddset (&usr2.sa_mask, SIGUSR2);

	if G_UNLIKELY (sigaction (SIGUSR2, &usr2, NULL) < 0)
		mdm_slave_exit (DISPLAY_ABORT, _("%s: Error setting up %s signal handler: %s"),
				"mdm_slave_start", "USR2", strerror (errno));

#ifdef SIGXFSZ
	/* handle the filesize signal */
	xfsz.sa_handler = mdm_slave_xfsz_handler;
	xfsz.sa_flags = SA_RESTART;
	sigemptyset (&xfsz.sa_mask);
	sigaddset (&xfsz.sa_mask, SIGXFSZ);

	if G_UNLIKELY (sigaction (SIGXFSZ, &xfsz, NULL) < 0)
		mdm_slave_exit (DISPLAY_ABORT,
				_("%s: Error setting up %s signal handler: %s"),
				"mdm_slave_start", "XFSZ", strerror (errno));
#endif /* SIGXFSZ */

	first_time = time (NULL);
	death_count = 0;

	for (;;) {
		time_t the_time;

		check_notifies_now ();

		mdm_debug ("mdm_slave_start: Loop Thingie");
		mdm_slave_run (display);

		/* flexi only run once */
		if (display->type != TYPE_STATIC ||
		    ! parent_exists ()) {
			mdm_server_stop (display);
			mdm_slave_send_num (MDM_SOP_XPID, 0);
			mdm_slave_quick_exit (DISPLAY_REMANAGE);
		}

		the_time = time (NULL);

		death_count++;

		if ((the_time - first_time) <= 0 ||
		    (the_time - first_time) > 60) {
			first_time = the_time;
			death_count = 0;
		} else if G_UNLIKELY (death_count > 6) {
			mdm_slave_quick_exit (DISPLAY_ABORT);
		}

		mdm_debug ("mdm_slave_start: Reinitializing things");

		/* Whack the server because we want to restart it next
		 * time we run mdm_slave_run */
		mdm_server_stop (display);
		mdm_slave_send_num (MDM_SOP_XPID, 0);
	}
	/* very very very evil, should never break, we can't return from
	   here sanely */
	_exit (DISPLAY_ABORT);
}

static gboolean
setup_automatic_session (MdmDisplay *display, const char *name)
{
	char *new_login;
	g_free (login_user);
	login_user = g_strdup (name);

	greet = FALSE;
	mdm_debug ("setup_automatic_session: Automatic login: %s", login_user);

	/* Run the init script. mdmslave suspends until script
	 * has terminated */
	mdm_slave_exec_script (display, "/etc/mdm/SuperInit", "root", getpwnam("root"), FALSE /* pass_stdout */);
	mdm_slave_exec_script (display, mdm_daemon_config_get_value_string (MDM_KEY_DISPLAY_INIT_DIR), NULL, NULL, FALSE /* pass_stdout */);

	mdm_debug ("setup_automatic_session: DisplayInit script finished");

	new_login = NULL;
	if ( ! mdm_verify_setup_user (display, login_user, &new_login))
		return FALSE;

	if (new_login != NULL) {
		g_free (login_user);
		login_user = g_strdup (new_login);
	}

	mdm_debug ("setup_automatic_session: Automatic login successful");

	return TRUE;
}

static void
mdm_screen_init (MdmDisplay *display)
{
#ifdef HAVE_XFREE_XINERAMA
	int (* old_xerror_handler) (Display *, XErrorEvent *);
	gboolean have_xinerama = FALSE;

	x_error_occurred = FALSE;
	old_xerror_handler = XSetErrorHandler (ignore_xerror_handler);

	have_xinerama = XineramaIsActive (display->dsp);

	XSync (display->dsp, False);
	XSetErrorHandler (old_xerror_handler);

	if (x_error_occurred)
		have_xinerama = FALSE;

	if (have_xinerama) {
		int screen_num;
		int xineramascreen;
		XineramaScreenInfo *xscreens =
			XineramaQueryScreens (display->dsp,
					      &screen_num);


		if G_UNLIKELY (screen_num <= 0)
			mdm_fail ("Xinerama active, but <= 0 screens?");

		
		xineramascreen = 0;

		display->screenx = xscreens[xineramascreen].x_org;
		display->screeny = xscreens[xineramascreen].y_org;
		display->screenwidth = xscreens[xineramascreen].width;
		display->screenheight = xscreens[xineramascreen].height;

		display->lrh_offsetx =
			DisplayWidth (display->dsp,
				      DefaultScreen (display->dsp))
			- (display->screenx + display->screenwidth);
		display->lrh_offsety =
			DisplayHeight (display->dsp,
				       DefaultScreen (display->dsp))
			- (display->screeny + display->screenheight);

		XFree (xscreens);
	} else
#elif HAVE_SOLARIS_XINERAMA
		/* This code from GDK, Copyright (C) 2002 Sun Microsystems, Inc. */
		int opcode;
	int firstevent;
	int firsterror;
	int n_monitors = 0;

	gboolean have_xinerama = FALSE;
	have_xinerama = XQueryExtension (display->dsp,
					 "XINERAMA",
					 &opcode,
					 &firstevent,
					 &firsterror);

	if (have_xinerama) {

		int result;
		XRectangle monitors[MAXFRAMEBUFFERS];
		unsigned char  hints[16];
		int xineramascreen;

		result = XineramaGetInfo (display->dsp, 0, monitors, hints, &n_monitors);
		/* Yes I know it should be Success but the current implementation
		 * returns the num of monitor
		 */
		if G_UNLIKELY (result <= 0)
			mdm_fail ("Xinerama active, but <= 0 screens?");
		
		xineramascreen = 0;
		display->screenx = monitors[xineramascreen].x;
		display->screeny = monitors[xineramascreen].y;
		display->screenwidth = monitors[xineramascreen].width;
		display->screenheight = monitors[xineramascreen].height;

		display->lrh_offsetx =
			DisplayWidth (display->dsp,
				      DefaultScreen (display->dsp))
			- (display->screenx + display->screenwidth);
		display->lrh_offsety =
			DisplayHeight (display->dsp,
				       DefaultScreen (display->dsp))
			- (display->screeny + display->screenheight);

	} else
#endif
		{
			display->screenx = 0;
			display->screeny = 0;
			display->screenwidth = 0; /* we'll use the gdk size */
			display->screenheight = 0;

			display->lrh_offsetx = 0;
			display->lrh_offsety = 0;
		}
}

static void
mdm_window_path (MdmDisplay *d)
{
	/* setting WINDOWPATH for clients */
	const char *windowpath;
	char *newwindowpath;
	char nums[10];
	int numn;

	if (d->vtnum != -1) {
		windowpath = getenv ("WINDOWPATH");
		numn = snprintf (nums, sizeof (nums), "%d", d->vtnum);
		if (!windowpath) {
			newwindowpath = malloc (numn + 1);
			sprintf (newwindowpath, "%s", nums);
		} else {
			newwindowpath = malloc (strlen (windowpath) + 1 + numn + 1);
			sprintf (newwindowpath, "%s:%s", windowpath, nums);
		}
		free (d->windowpath);
		d->windowpath = newwindowpath;
	}
}

static void
mdm_slave_whack_greeter (void)
{
	MdmWaitPid *wp;

	mdm_sigchld_block_push ();

	/* do what you do when you quit, this will hang until the
	 * greeter decides to print an STX\n and die, meaning it can do some
	 * last minute cleanup */
	mdm_slave_greeter_ctl_no_ret (MDM_QUIT, "");

	greet = FALSE;

	wp = slave_waitpid_setpid (d->greetpid);
	mdm_sigchld_block_pop ();

	slave_waitpid (wp);

	d->greetpid = 0;

	whack_greeter_fds ();

	mdm_slave_send_num (MDM_SOP_GREETPID, 0);

}

static int
ask_migrate (const char *migrate_to)
{
	int   r;
	char *msg;
	char *but[4];
	char *askbuttons_msg;

	/*
	 * If migratable and ALWAYS_LOGIN_CURRENT_SESSION is true, then avoid 
	 * the dialog.
	 */
	if (migrate_to != NULL &&
	    mdm_daemon_config_get_value_bool (MDM_KEY_ALWAYS_LOGIN_CURRENT_SESSION)) {
		return 1;
	}

	/*
	 * Avoid dialog if DOUBLE_LOGIN_WARNING is false.  In this case
	 * ALWAYS_LOGIN_CURRENT_SESSION is false, so assume new session.
	 */
	if (!mdm_daemon_config_get_value_bool (MDM_KEY_DOUBLE_LOGIN_WARNING)) {
		return 0;
	}

	but[0] = _("Log in anyway");
	if (migrate_to != NULL) {
		msg = _("You are already logged in.  "
			"You can log in anyway, return to your "
			"previous login session, or abort this "
			"login");
		but[1] = _("Return to previous login");
		but[2] = _("Abort login");
		but[3] = "NIL";
	} else {
		msg = _("You are already logged in.  "
			"You can log in anyway or abort this "
			"login");
		but[1] = _("Abort login");
		but[2] = "NIL";
		but[3] = "NIL";
	}

	if (greet)
		mdm_slave_greeter_ctl_no_ret (MDM_DISABLE, "");

	askbuttons_msg = g_strdup_printf ("askbuttons_msg=%s$$options_msg1=%s$$options_msg2=%s$$options_msg3=%s$$options_msg4=%s", msg, but[0], but[1], but[2], but[3]);


	mdm_slave_send_string (MDM_SOP_SHOW_ASKBUTTONS_DIALOG, askbuttons_msg);

	r = atoi (mdm_ack_response);

	g_free (askbuttons_msg);
	g_free (mdm_ack_response);
	mdm_ack_response = NULL;

	if (greet)
		mdm_slave_greeter_ctl_no_ret (MDM_ENABLE, "");

	return r;
}

gboolean
mdm_slave_check_user_wants_to_log_in (const char *user)
{
	gboolean loggedin = FALSE;
	int i;
	char **vec;
	char *migrate_to = NULL;

	/* always ignore root here, this is mostly a special case
	 * since a root login may not be a real login, such as the
	 * config stuff, and people shouldn't log in as root anyway
	 */
	if (strcmp (user, mdm_root_user ()) == 0)
		return TRUE;

	mdm_slave_send_string (MDM_SOP_QUERYLOGIN, user);
	if G_LIKELY (ve_string_empty (mdm_ack_response))
		return TRUE;
	vec = g_strsplit (mdm_ack_response, ",", -1);
	if (vec == NULL)
		return TRUE;

	mdm_debug ("QUERYLOGIN response: %s\n", mdm_ack_response);

	for (i = 0; vec[i] != NULL && vec[i+1] != NULL; i += 2) {
		int ii;
		loggedin = TRUE;
		if (sscanf (vec[i+1], "%d", &ii) == 1 && ii == 1) {
			migrate_to = g_strdup (vec[i]);
			break;
		}
	}

	g_strfreev (vec);

	if ( ! loggedin)
		return TRUE;

	int r;

	r = ask_migrate (migrate_to);

	if (r <= 0) {
		g_free (migrate_to);
		return TRUE;
	}

	/*
	 * migrate_to should never be NULL here, since
	 * ask_migrate will always return 0 or 1 if migrate_to
	 * is NULL.
	 */
	if (migrate_to == NULL ||
	    (migrate_to != NULL && r == 2)) {
		g_free (migrate_to);
		return FALSE;
	}

	/* Must be that r == 1, that is return to previous login */

#ifdef WITH_CONSOLE_KIT
	// Unlock the session with consolekit
	unlock_ck_session (user, migrate_to);
#endif

	// Unlock the session with logind
	char * unlock_logind_command = g_strdup_printf ("/usr/bin/mdm-unlock-logind %s %s &", user, migrate_to);
	system(unlock_logind_command);
	g_free(unlock_logind_command);

	if (d->type == TYPE_FLEXI) {
		mdm_slave_whack_greeter ();
		mdm_server_stop (d);
		mdm_slave_send_num (MDM_SOP_XPID, 0);
	}

	mdm_slave_send_string (MDM_SOP_MIGRATE, migrate_to);
	g_free (migrate_to);

	if (d->type == TYPE_FLEXI) {
		/* we are no longer needed so just die.
		   REMANAGE == ABORT here really */
		mdm_slave_quick_exit (DISPLAY_REMANAGE);
	}

	/* abort this login attempt */
	return FALSE;
}

static gboolean do_xfailed_on_xio_error = FALSE;

static gboolean
plymouth_is_running (void)
{
    int status;
    status = system ("/bin/plymouth --ping");
    return WIFEXITED (status) && WEXITSTATUS (status) == 0;
}

static void
plymouth_quit_without_transition (void) {
    system ("/bin/plymouth quit");
}

static void
mdm_slave_run (MdmDisplay *display)
{	
	gint openretries = 0;
	gint maxtries = 0;

	mdm_reset_locale ();

	/* Reset d since mdm_slave_run is called in a loop */
	d = display;

	mdm_random_tick ();

	if (d->sleep_before_run > 0) {
		mdm_debug ("mdm_slave_run: Sleeping %d seconds before server start", d->sleep_before_run);
		mdm_sleep_no_signal (d->sleep_before_run);
		d->sleep_before_run = 0;

		check_notifies_now ();
	}

	/*
	 * Set it before we run the server, it may be that we're using
	 * the XOpenDisplay to find out if a server is ready (as with
	 * nested display)
	 */
	d->dsp = NULL;
	
	if (plymouth_is_running ()) {
		g_warning("Plymouth is running, asking it to stop...");
		plymouth_quit_without_transition ();
		g_warning("Plymouth stopped");
	}

	/* if this is local display start a server if one doesn't
	 * exist */
	if (SERVER_IS_LOCAL (d) &&
	    d->servpid <= 0) {
		if G_UNLIKELY ( ! mdm_server_start (d,
						    TRUE /* try_again_if_busy */,
						    FALSE /* treat_as_flexi */,
						    20 /* min_flexi_disp */,
						    5 /* flexi_retries */)) {
			/* We're really not sure what is going on,
			 * so we throw up our hands and tell the user
			 * that we've given up.  The error is likely something
			 * internal. */
			mdm_text_message_dialog
				(C_(N_("Could not start the X\n"
				       "server (your graphical environment)\n"
				       "due to some internal error.\n"
				       "Please contact your system administrator\n"
				       "or check your syslog to diagnose.\n"
				       "In the meantime this display will be\n"
				       "disabled.  Please restart MDM when\n"
				       "the problem is corrected.")));
			if (plymouth_is_running ()) {
				g_warning("Plymouth is running, asking it to stop...");
				plymouth_quit_without_transition ();
				g_warning("Plymouth stopped");
			}
			mdm_slave_quick_exit (DISPLAY_ABORT);
		}
		mdm_slave_send_num (MDM_SOP_XPID, d->servpid);

		check_notifies_now ();
	}

	/* We can use d->handled from now on on this display,
	 * since the lookup was done in server start */

	g_setenv ("DISPLAY", d->name, TRUE);
	if (d->windowpath)
		g_setenv ("WINDOWPATH", d->windowpath, TRUE);
	g_unsetenv ("XAUTHORITY"); /* just in case it's set */

	mdm_auth_set_local_auth (d);

	if (d->handled) {
		/* Now the display name and hostname is final */

		const char *automaticlogin = mdm_daemon_config_get_value_string (MDM_KEY_AUTOMATIC_LOGIN);
		const char *timedlogin     = mdm_daemon_config_get_value_string (MDM_KEY_TIMED_LOGIN);

		if (mdm_daemon_config_get_value_bool (MDM_KEY_AUTOMATIC_LOGIN_ENABLE) &&
		    ! ve_string_empty (automaticlogin)) {
			g_free (ParsedAutomaticLogin);
			ParsedAutomaticLogin = mdm_slave_parse_enriched_login (display,
									       automaticlogin);
		}

		if (mdm_daemon_config_get_value_bool (MDM_KEY_TIMED_LOGIN_ENABLE) &&
		    ! ve_string_empty (timedlogin)) {
			g_free (ParsedTimedLogin);
			ParsedTimedLogin = mdm_slave_parse_enriched_login (display,
									   timedlogin);
		}
	}

	/* X error handlers to avoid the default one (i.e. exit (1)) */
	do_xfailed_on_xio_error = TRUE;
	XSetErrorHandler (mdm_slave_xerror_handler);
	XSetIOErrorHandler (mdm_slave_xioerror_handler);

	/* We keep our own (windowless) connection (dsp) open to avoid the
	 * X server resetting due to lack of active connections. */

	mdm_debug ("mdm_slave_run: Opening display %s", d->name);

	/* if local then the the server should be ready for openning, so
	 * don't try so long before killing it and trying again */
	if (SERVER_IS_LOCAL (d))
		maxtries = 2;
	else
		maxtries = 10;

	while (d->handled &&
	       openretries < maxtries &&
	       d->dsp == NULL &&
	       ( ! SERVER_IS_LOCAL (d) || d->servpid > 1)) {

		mdm_sigchld_block_push ();
		d->dsp = XOpenDisplay (d->name);
		mdm_sigchld_block_pop ();

		if G_UNLIKELY (d->dsp == NULL) {
			mdm_debug ("mdm_slave_run: Sleeping %d on a retry", 1+openretries*2);
			mdm_sleep_no_signal (1+openretries*2);
			openretries++;
		}
	}

	/* Really this will only be useful for the first local server,
	   since that's the only time this can really be on */
	while G_UNLIKELY (mdm_wait_for_go) {
		struct timeval tv;
		/* Wait 1 second. */
		tv.tv_sec = 1;
		tv.tv_usec = 0;
		select (0, NULL, NULL, NULL, &tv);
		
		check_notifies_now ();
	}

	/* Set the busy cursor */
	if (d->dsp != NULL) {
		Cursor xcursor = XCreateFontCursor (d->dsp, GDK_WATCH);
		XDefineCursor (d->dsp,
			       DefaultRootWindow (d->dsp),
			       xcursor);
		XFreeCursor (d->dsp, xcursor);
		XSync (d->dsp, False);
	}

	/* Just a race avoiding sleep, probably not necessary though,
	 * but doesn't hurt anything */
	if ( ! d->handled)
		mdm_sleep_no_signal (1);

	if (SERVER_IS_LOCAL (d)) {
		mdm_slave_send (MDM_SOP_START_NEXT_LOCAL, FALSE);
	}

	check_notifies_now ();

	/* something may have gone wrong, try xfailed, if local (non-flexi),
	 * the toplevel loop of death will handle us */ 
	if G_UNLIKELY (d->handled && d->dsp == NULL) {
		if (d->type == TYPE_STATIC)
			mdm_slave_quick_exit (DISPLAY_XFAILED);
		else
			mdm_slave_quick_exit (DISPLAY_ABORT);
	}

	/* OK from now on it's really the user whacking us most likely,
	 * we have already started up well */
	do_xfailed_on_xio_error = FALSE;	

	/* checkout xinerama */
	if (d->handled)
		mdm_screen_init (d);

	/*
	 * Find out the VT number of the display.  VT's could be started by some
	 * other mechanism than by running mdmflexiserver, so need to check the
	 * Atom.  Do this before starting the greeter so that the user does not
	 * have the ability to modify the atom.
	 */
	d->vtnum = mdm_get_current_vtnum (d->dsp);

	/* checkout window number */
	mdm_window_path (d);

	/* check log stuff for the server, this is done here
	 * because it's really a race */
	if (SERVER_IS_LOCAL (d))
		mdm_server_checklog (d);

	if ( ! d->handled) {
		/* yay, we now wait for the server to die */
		while (d->servpid > 0) {
			pause ();
		}
		mdm_slave_quick_exit (DISPLAY_REMANAGE);	
	} else if (d->type == TYPE_STATIC &&
		   mdm_first_login &&
		   ! ve_string_empty (ParsedAutomaticLogin) &&
		   strcmp (ParsedAutomaticLogin, mdm_root_user ()) != 0) {
		mdm_first_login = FALSE;

		d->logged_in = TRUE;
		mdm_slave_send_num (MDM_SOP_LOGGED_IN, TRUE);
		mdm_slave_send_string (MDM_SOP_LOGIN, ParsedAutomaticLogin);

		if (setup_automatic_session (d, ParsedAutomaticLogin)) {
			mdm_slave_session_start ();
		}

		mdm_slave_send_num (MDM_SOP_LOGGED_IN, FALSE);
		d->logged_in = FALSE;
		mdm_slave_send_string (MDM_SOP_LOGIN, "");
		logged_in_uid = -1;
		logged_in_gid = -1;

		mdm_debug ("mdm_slave_run: Automatic login done");

		if (remanage_asap) {
			mdm_slave_quick_exit (DISPLAY_REMANAGE);
		}

		/* return to mdm_slave_start so that the server
		 * can be reinitted and all that kind of fun stuff. */
		return;
	}

	if (mdm_first_login)
		mdm_first_login = FALSE;

	do {
		check_notifies_now ();

		if ( ! greet) {
			mdm_slave_greeter ();  /* Start the greeter */
			greeter_no_focus = FALSE;
			greeter_disabled = FALSE;
		}

		mdm_slave_wait_for_login (); /* wait for a password */

		d->logged_in = TRUE;
		mdm_slave_send_num (MDM_SOP_LOGGED_IN, TRUE);

		if (do_timed_login) {
			/* timed out into a timed login */
			do_timed_login = FALSE;
			if (setup_automatic_session (d, ParsedTimedLogin)) {
				mdm_slave_send_string (MDM_SOP_LOGIN,
						       ParsedTimedLogin);
				mdm_slave_session_start ();
			}
		} else {
			mdm_slave_send_string (MDM_SOP_LOGIN, login_user);
			mdm_slave_session_start ();
		}

		mdm_slave_send_num (MDM_SOP_LOGGED_IN, FALSE);
		d->logged_in = FALSE;
		mdm_slave_send_string (MDM_SOP_LOGIN, "");
		logged_in_uid = -1;
		logged_in_gid = -1;

		if (remanage_asap) {
			mdm_slave_quick_exit (DISPLAY_REMANAGE);
		}

		if (greet) {
			greeter_no_focus = FALSE;
			mdm_slave_greeter_ctl_no_ret (MDM_FOCUS, "");
			greeter_disabled = FALSE;
			mdm_slave_greeter_ctl_no_ret (MDM_ENABLE, "");
			mdm_slave_greeter_ctl_no_ret (MDM_RESETOK, "");
		}
		/* Note that greet is only true if the above was no 'login',
		 * so no need to reinit the server nor rebake cookies
		 * nor such nonsense */
	} while (greet);
	
}

static void
run_config (MdmDisplay *display, struct passwd *pwent)
{
	pid_t pid;

	/* Lets check if custom.conf exists. If not there
	   is no point in launching mdmsetup as it will fail.
	   We don't need to worry about defaults.conf as
	   the daemon wont start without it
	*/
	if (mdm_daemon_config_get_custom_config_file () == NULL) {
		mdm_errorgui_error_box (d,
			       GTK_MESSAGE_ERROR,
			       _("Could not access configuration file (custom.conf). "
				 "Make sure that the file exists before launching "
				 " login manager config utility."));
		return;
	}

	/* Set the busy cursor */
	if (d->dsp != NULL) {
		Cursor xcursor = XCreateFontCursor (d->dsp, GDK_WATCH);
		XDefineCursor (d->dsp,
			       DefaultRootWindow (d->dsp),
			       xcursor);
		XFreeCursor (d->dsp, xcursor);
		XSync (d->dsp, False);
	}

	mdm_debug ("Forking MDM configuration process");

	mdm_sigchld_block_push ();
	mdm_sigterm_block_push ();
	pid = d->sesspid = fork ();
	if (pid == 0)
		mdm_unset_signals ();
	mdm_sigterm_block_pop ();
	mdm_sigchld_block_pop ();

	if G_UNLIKELY (pid < 0) {
		/* Return left pointer */
		Cursor xcursor;

		/* Can't fork */
		display->sesspid = 0;

		xcursor = XCreateFontCursor (d->dsp, GDK_LEFT_PTR);
		XDefineCursor (d->dsp,
			       DefaultRootWindow (d->dsp),
			       xcursor);
		XFreeCursor (d->dsp, xcursor);
		XSync (d->dsp, False);

		return;
	}

	if (pid == 0) {
		char **argv = NULL;
		const char *s;

		/* child */

		setsid ();

		mdm_unset_signals ();

		setuid (0);
		setgid (0);
		mdm_desetuid ();

		/* setup environment */
		mdm_restoreenv ();
		mdm_reset_locale ();

		/* root here */
		g_setenv ("XAUTHORITY", MDM_AUTHFILE (display), TRUE);
		g_setenv ("DISPLAY", display->name, TRUE);
		if (d->windowpath)
			g_setenv ("WINDOWPATH", d->windowpath, TRUE);
		g_setenv ("LOGNAME", pwent->pw_name, TRUE);
		g_setenv ("USER", pwent->pw_name, TRUE);
		g_setenv ("USERNAME", pwent->pw_name, TRUE);
		g_setenv ("HOME", pwent->pw_dir, TRUE);
		g_setenv ("SHELL", pwent->pw_shell, TRUE);
		g_setenv ("PATH", mdm_daemon_config_get_value_string (MDM_KEY_ROOT_PATH), TRUE);
		g_setenv ("RUNNING_UNDER_MDM", "true", TRUE);
		if ( ! ve_string_empty (display->theme_name))
			g_setenv ("MDM_GTK_THEME", display->theme_name, TRUE);

		mdm_log_shutdown ();

		/* Debian changes */
#if 0
		/* upstream version */
		mdm_close_all_descriptors (0 /* from */, slave_fifo_pipe_fd /* except */, d->slave_notify_fd /* except2 */);

		/* No error checking here - if it's messed the best response
		 * is to ignore & try to continue */
		mdm_open_dev_null (O_RDONLY); /* open stdin - fd 0 */
		mdm_open_dev_null (O_RDWR); /* open stdout - fd 1 */
		mdm_open_dev_null (O_RDWR); /* open stderr - fd 2 */
#endif
		/* Leave stderr open to the log */
		VE_IGNORE_EINTR (close (0));
		VE_IGNORE_EINTR (close (1));
		mdm_close_all_descriptors (3 /* from */, slave_fifo_pipe_fd /* except */, d->slave_notify_fd /* except2 */);

		/* No error checking here - if it's messed the best response
		 * is to ignore & try to continue */
		mdm_open_dev_null (O_RDONLY); /* open stdin - fd 0 */
		mdm_open_dev_null (O_RDWR); /* open stdout - fd 1 */
		/* End of Debian changes */

		mdm_log_init ();

		VE_IGNORE_EINTR (g_chdir (pwent->pw_dir));
		if G_UNLIKELY (errno != 0)
			VE_IGNORE_EINTR (g_chdir ("/"));

		/* exec the configurator */
		s = mdm_daemon_config_get_value_string (MDM_KEY_CONFIGURATOR);
		if (s != NULL) {
			g_shell_parse_argv (s, NULL, &argv, NULL);
		}

		if G_LIKELY (argv != NULL &&
			     argv[0] != NULL &&
			     g_access (argv[0], X_OK) == 0) {
			VE_IGNORE_EINTR (execv (argv[0], argv));
		}

		g_strfreev (argv);

		mdm_errorgui_error_box (d,
					GTK_MESSAGE_ERROR,
					_("Could not execute the configuration "
					  "application.  Make sure its path is set "
					  "correctly in the configuration file.  "
					  "Attempting to start it from the default "
					  "location."));
		s = LIBEXECDIR "/mdmsetup --disable-sound --disable-crash-dialog";
		argv = NULL;
		g_shell_parse_argv (s, NULL, &argv, NULL);

		if (g_access (argv[0], X_OK) == 0) {
			VE_IGNORE_EINTR (execv (argv[0], argv));
		}

		g_strfreev (argv);

		mdm_errorgui_error_box (d,
					GTK_MESSAGE_ERROR,
					_("Could not execute the configuration "
					  "application.  Make sure its path is set "
					  "correctly in the configuration file."));

		_exit (0);
	} else {
		MdmWaitPid *wp;

		configurator = TRUE;

		mdm_sigchld_block_push ();
		wp = slave_waitpid_setpid (display->sesspid);
		mdm_sigchld_block_pop ();

		slave_waitpid (wp);

		display->sesspid = 0;
		configurator = FALSE;

		/* this will clean up the sensitivity property */
		mdm_slave_sensitize_config ();
	}
}

static void
restart_the_greeter (void)
{
	do_restart_greeter = FALSE;

	mdm_slave_desensitize_config ();

	/* no login */
	g_free (login_user);
	login_user = NULL;

	/* Now restart it */
	if (greet) {
		MdmWaitPid *wp;

		mdm_sigchld_block_push ();

		mdm_slave_greeter_ctl_no_ret (MDM_SAVEDIE, "");

		greet = FALSE;

		wp = slave_waitpid_setpid (d->greetpid);

		mdm_sigchld_block_pop ();

		slave_waitpid (wp);

		d->greetpid = 0;

		whack_greeter_fds ();

		mdm_slave_send_num (MDM_SOP_GREETPID, 0);
	}
	mdm_slave_greeter ();

	if (greeter_disabled)
		mdm_slave_greeter_ctl_no_ret (MDM_DISABLE, "");

	if (greeter_no_focus)
		mdm_slave_greeter_ctl_no_ret (MDM_NOFOCUS, "");

	mdm_slave_sensitize_config ();
}

static gboolean
play_login_sound (const char *sound_file)
{
	const char *soundprogram = mdm_daemon_config_get_value_string (MDM_KEY_SOUND_PROGRAM);	

	if (ve_string_empty (soundprogram) ||
	    ve_string_empty (sound_file) ||
	    g_access (soundprogram, X_OK) != 0 ||
	    g_access (sound_file, F_OK) != 0)
		return FALSE;

	mdm_debug ("play_login_sound: Launching %s", soundprogram);	
	char * command = g_strdup_printf ("%s \"%s\" &", soundprogram, sound_file);
	mdm_debug ("play_login_sound: Executing %s", command);
	system(command);    

	return TRUE;
}

static void
mdm_slave_wait_for_login (void)
{
	const char *successsound;
	char *username;
	g_free (login_user);
	login_user = NULL;

	/* Chat with greeter */
	while (login_user == NULL) {
		/* init to a sane value */
		do_timed_login = FALSE;
		do_configurator = FALSE;
		do_cancel = FALSE;

		if G_UNLIKELY (do_restart_greeter) {
			do_restart_greeter = FALSE;
			restart_the_greeter ();
		}

		/* We are NOT interrupted yet */
		interrupted = FALSE;

		check_notifies_now ();

		/* just for paranoia's sake */
		NEVER_FAILS_root_set_euid_egid (0, 0);

		mdm_debug ("mdm_slave_wait_for_login: In loop");
		username = d->preset_user;
		d->preset_user = NULL;
		login_user = mdm_verify_user (d /* the display */,
					      username /* username */,
					      TRUE /* allow retry */);
		g_free (username);

		mdm_debug ("mdm_slave_wait_for_login: end verify for '%s'",
			   ve_sure_string (login_user));

		/* Complex, make sure to always handle the do_configurator
		 * do_timed_login and do_restart_greeter after any call
		 * to mdm_verify_user */

		if G_UNLIKELY (do_restart_greeter) {
			g_free (login_user);
			login_user = NULL;
			do_restart_greeter = FALSE;
			restart_the_greeter ();
			continue;
		}

		check_notifies_now ();

		if G_UNLIKELY (do_configurator) {
			struct passwd *pwent;
			gboolean oldAllowRoot;

			do_configurator = FALSE;
			g_free (login_user);
			login_user = NULL;
			/* clear any error */
			mdm_slave_greeter_ctl_no_ret (MDM_ERRBOX, "");
			mdm_slave_greeter_ctl_no_ret
				(MDM_MSG,
				 _("You must authenticate as root to run configuration."));

			/* we always allow root for this */
			oldAllowRoot = mdm_daemon_config_get_value_bool (MDM_KEY_ALLOW_ROOT);
			mdm_daemon_config_set_value_bool (MDM_KEY_ALLOW_ROOT, TRUE);

			pwent = getpwuid (0);
			if G_UNLIKELY (pwent == NULL) {
				/* what? no "root" ?? */
				mdm_slave_greeter_ctl_no_ret (MDM_RESET, "");
				continue;
			}

			mdm_slave_greeter_ctl_no_ret (MDM_SETLOGIN, pwent->pw_name);
			login_user = mdm_verify_user (d,
						      pwent->pw_name,
						      FALSE);
			mdm_daemon_config_set_value_bool (MDM_KEY_ALLOW_ROOT, oldAllowRoot);

			/* Clear message */
			mdm_slave_greeter_ctl_no_ret (MDM_MSG, "");

			if G_UNLIKELY (do_restart_greeter) {
				g_free (login_user);
				login_user = NULL;
				do_restart_greeter = FALSE;
				restart_the_greeter ();
				continue;
			}

			check_notifies_now ();

			/* The user can't remember his password */
			if (login_user == NULL) {
				mdm_debug ("mdm_slave_wait_for_login: No login/Bad login");
				mdm_slave_greeter_ctl_no_ret (MDM_RESET, "");
				continue;
			}

			/* Wipe the login */
			g_free (login_user);
			login_user = NULL;

			/* Note that this can still fall through to
			 * the timed login if the user doesn't type in the
			 * password fast enough and there is timed login
			 * enabled */
			if (do_timed_login) {
				break;
			}

			if G_UNLIKELY (do_configurator) {
				do_configurator = FALSE;
				mdm_slave_greeter_ctl_no_ret (MDM_RESET, "");
				continue;
			}

			/* Now running as root */

			/* Get the root pwent */
			pwent = getpwuid (0);

			if G_UNLIKELY (pwent == NULL) {
				/* What?  No "root" ??  This is not possible
				 * since we logged in, but I'm paranoid */
				mdm_slave_greeter_ctl_no_ret (MDM_RESET, "");
				continue;
			}

			d->logged_in = TRUE;
			logged_in_uid = 0;
			logged_in_gid = 0;
			mdm_slave_send_num (MDM_SOP_LOGGED_IN, TRUE);
			/* Note: nobody really logged in */
			mdm_slave_send_string (MDM_SOP_LOGIN, "");

			/* Disable the login screen, we don't want people to
			 * log in in the meantime */
			mdm_slave_greeter_ctl_no_ret (MDM_DISABLE, "");
			greeter_disabled = TRUE;

			/* Make the login screen not focusable */
			mdm_slave_greeter_ctl_no_ret (MDM_NOFOCUS, "");
			greeter_no_focus = TRUE;

			check_notifies_now ();
			restart_greeter_now = TRUE;

			mdm_debug ("mdm_slave_wait_for_login: Running MDM Configurator ...");
			run_config (d, pwent);
			mdm_debug ("mdm_slave_wait_for_login: MDM Configurator finished ...");

			restart_greeter_now = FALSE;

			mdm_verify_cleanup (d);

			mdm_slave_send_num (MDM_SOP_LOGGED_IN, FALSE);
			d->logged_in = FALSE;
			logged_in_uid = -1;
			logged_in_gid = -1;

			if (remanage_asap) {
				mdm_slave_quick_exit (DISPLAY_REMANAGE);
			}

			greeter_no_focus = FALSE;
			mdm_slave_greeter_ctl_no_ret (MDM_FOCUS, "");

			greeter_disabled = FALSE;
			mdm_slave_greeter_ctl_no_ret (MDM_ENABLE, "");
			mdm_slave_greeter_ctl_no_ret (MDM_RESETOK, "");
			continue;
		}

		/* The user timed out into a timed login during the
		 * conversation */
		if (do_timed_login) {
			break;
		}

		if (login_user == NULL) {

			const char *failuresound = mdm_daemon_config_get_value_string (MDM_KEY_SOUND_ON_LOGIN_FAILURE_FILE);

			mdm_debug ("mdm_slave_wait_for_login: No login/Bad login");
			mdm_slave_greeter_ctl_no_ret (MDM_RESET, "");

			/* Play sounds if specified for a failed login */
			if (d->attached && failuresound &&
			    mdm_daemon_config_get_value_bool (MDM_KEY_SOUND_ON_LOGIN_FAILURE) &&
			    ! play_login_sound (failuresound)) {
				mdm_error ("Login sound requested on non-local display or the play software cannot be run or the sound does not exist.");
			}
		}
	}

	/* The user timed out into a timed login during the conversation */
	if (do_timed_login) {
		g_free (login_user);
		login_user = NULL;
		/* timed login is automatic, thus no need for greeter,
		 * we'll take default values */
		mdm_slave_whack_greeter ();

		mdm_debug ("mdm_slave_wait_for_login: Timed Login");
	}

	successsound = mdm_daemon_config_get_value_string (MDM_KEY_SOUND_ON_LOGIN_SUCCESS_FILE);
	/* Play sounds if specified for a successful login */
	if (login_user != NULL && successsound &&
	    mdm_daemon_config_get_value_bool (MDM_KEY_SOUND_ON_LOGIN_SUCCESS) &&
	    d->attached &&
	    ! play_login_sound (successsound)) {
		mdm_error ("Login sound requested on non-local display or the play software cannot be run or the sound does not exist.");
	}

	mdm_debug ("mdm_slave_wait_for_login: got_login for '%s'",
		   ve_sure_string (login_user));


}

/* This is VERY evil! */
static void
run_pictures (void)
{
	char *response;
	int max_write;
	char buf[1024];
	size_t bytes;
	struct passwd *pwent;
	char *picfile;
	FILE *fp;

	response = NULL;
	for (;;) {
		struct stat s;
		char *tmp, *ret;
		int i, r;

		g_free (response);
		response = mdm_slave_greeter_ctl (MDM_NEEDPIC, "");
		if (ve_string_empty (response)) {
			g_free (response);
			return;
		}

		pwent = getpwnam (response);
		if G_UNLIKELY (pwent == NULL) {
			mdm_slave_greeter_ctl_no_ret (MDM_READPIC, "");
			continue;
		}

		picfile = NULL;

		NEVER_FAILS_seteuid (0);
		if G_UNLIKELY (setegid (pwent->pw_gid) != 0 ||
			       seteuid (pwent->pw_uid) != 0) {
			NEVER_FAILS_root_set_euid_egid (0, mdm_daemon_config_get_mdmgid ());
			mdm_slave_greeter_ctl_no_ret (MDM_READPIC, "");
			continue;
		}

		picfile = mdm_common_get_facefile (pwent->pw_dir, pwent->pw_name, pwent->pw_uid);

		if (! picfile) {
			NEVER_FAILS_root_set_euid_egid (0, mdm_daemon_config_get_mdmgid ());
			mdm_slave_greeter_ctl_no_ret (MDM_READPIC, "");
			continue;
		}

		VE_IGNORE_EINTR (r = g_stat (picfile, &s));
		if G_UNLIKELY (r < 0 || s.st_size > mdm_daemon_config_get_value_int (MDM_KEY_USER_MAX_FILE)) {
			NEVER_FAILS_root_set_euid_egid (0, mdm_daemon_config_get_mdmgid ());

			mdm_slave_greeter_ctl_no_ret (MDM_READPIC, "");
			continue;
		}

		VE_IGNORE_EINTR (fp = fopen (picfile, "r"));
		g_free (picfile);
		if G_UNLIKELY (fp == NULL) {
			NEVER_FAILS_root_set_euid_egid (0, mdm_daemon_config_get_mdmgid ());

			mdm_slave_greeter_ctl_no_ret (MDM_READPIC, "");
			continue;
		}

		tmp = g_strdup_printf ("buffer:%d", (int)s.st_size);
		ret = mdm_slave_greeter_ctl (MDM_READPIC, tmp);
		g_free (tmp);

		if G_UNLIKELY (ret == NULL || strcmp (ret, "OK") != 0) {
			VE_IGNORE_EINTR (fclose (fp));
			g_free (ret);

			NEVER_FAILS_root_set_euid_egid (0, mdm_daemon_config_get_mdmgid ());

			continue;
		}
		g_free (ret);

		mdm_fdprintf (greeter_fd_out, "%c", STX);

#ifdef PIPE_BUF
		max_write = MIN (PIPE_BUF, sizeof (buf));
#else
		/* apparently Hurd doesn't have PIPE_BUF */
		max_write = fpathconf (greeter_fd_out, _PC_PIPE_BUF);
		/* could return -1 if no limit */
		if (max_write > 0)
			max_write = MIN (max_write, sizeof (buf));
		else
			max_write = sizeof (buf);
#endif

		i = 0;
		while (i < s.st_size) {
			int written;

			VE_IGNORE_EINTR (bytes = fread (buf, sizeof (char),
							max_write, fp));

			if (bytes <= 0)
				break;

			if G_UNLIKELY (i + bytes > s.st_size)
				bytes = s.st_size - i;

			/* write until we succeed in writing something */
			VE_IGNORE_EINTR (written = write (greeter_fd_out, buf, bytes));
			if G_UNLIKELY (written < 0 &&
				       (errno == EPIPE || errno == EBADF)) {
				/* something very, very bad has happened */
				mdm_slave_quick_exit (DISPLAY_REMANAGE);
			}

			if G_UNLIKELY (written < 0)
				written = 0;

			/* write until we succeed in writing everything */
			while (written < bytes) {
				int n;
				VE_IGNORE_EINTR (n = write (greeter_fd_out, &buf[written], bytes-written));
				if G_UNLIKELY (n < 0 &&
					       (errno == EPIPE || errno == EBADF)) {
					/* something very, very bad has happened */
					mdm_slave_quick_exit (DISPLAY_REMANAGE);
				} else if G_LIKELY (n > 0) {
					written += n;
				}
			}

			/* we have written bytes bytes if it likes it or not */
			i += bytes;
		}

		VE_IGNORE_EINTR (fclose (fp));

		/* eek, this "could" happen, so just send some garbage */
		while G_UNLIKELY (i < s.st_size) {
			bytes = MIN (sizeof (buf), s.st_size - i);
			errno = 0;
			bytes = write (greeter_fd_out, buf, bytes);
			if G_UNLIKELY (bytes < 0 && (errno == EPIPE || errno == EBADF)) {
				/* something very, very bad has happened */
				mdm_slave_quick_exit (DISPLAY_REMANAGE);
			}
			if (bytes > 0)
				i += bytes;
		}

		mdm_slave_greeter_ctl_no_ret (MDM_READPIC, "done");

		NEVER_FAILS_root_set_euid_egid (0, mdm_daemon_config_get_mdmgid ());
	}
	g_free (response); /* not reached */
}

static void
exec_command (const char *command, const char *extra_arg)
{
	char **argv;
	int argc;

	if (! g_shell_parse_argv (command, &argc, &argv, NULL)) {
		return;
	}

	if (argv == NULL ||
	    ve_string_empty (argv[0]))
		return;

	if (g_access (argv[0], X_OK) != 0)
		return;

	if (extra_arg != NULL) {

		argv           = g_renew (char *, argv, argc + 2);
		argv[argc]     = g_strdup (extra_arg);
		argv[argc + 1] = NULL;
	}

	VE_IGNORE_EINTR (execv (argv[0], argv));
	g_strfreev (argv);
}

static void
mdm_slave_greeter (void)
{
	gint pipe1[2], pipe2[2];
	struct passwd *pwent;
	pid_t pid;
	const char *command;
	const char *defaultpath;
	const char *mdmuser;
	const char *moduleslist;
	const char *mdmlang;

	mdm_debug ("mdm_slave_greeter: Running greeter on %s", d->name);

	/* Run the init script. mdmslave suspends until script has terminated */
	mdm_slave_exec_script (d, "/etc/mdm/SuperInit", "root", getpwnam("root"), FALSE /* pass_stdout */);
	mdm_slave_exec_script (d, mdm_daemon_config_get_value_string (MDM_KEY_DISPLAY_INIT_DIR), NULL, NULL, FALSE /* pass_stdout */);

	/* Open a pipe for greeter communications */
	if G_UNLIKELY (pipe (pipe1) < 0)
		mdm_slave_exit (DISPLAY_REMANAGE, _("%s: Can't init pipe to mdmgreeter"),
				"mdm_slave_greeter");
	if G_UNLIKELY (pipe (pipe2) < 0) {
		VE_IGNORE_EINTR (close (pipe1[0]));
		VE_IGNORE_EINTR (close (pipe1[1]));
		mdm_slave_exit (DISPLAY_REMANAGE, _("%s: Can't init pipe to mdmgreeter"),
				"mdm_slave_greeter");
	}	

	command = mdm_daemon_config_get_value_string (MDM_KEY_GREETER);	

	mdm_debug ("Forking greeter process: %s", command);

	/* Fork. Parent is mdmslave, child is greeter process. */
	mdm_sigchld_block_push ();
	mdm_sigterm_block_push ();
	greet = TRUE;
	pid = d->greetpid = fork ();
	if (pid == 0)
		mdm_unset_signals ();
	mdm_sigterm_block_pop ();
	mdm_sigchld_block_pop ();

	switch (pid) {

	case 0:
		setsid ();

		mdm_unset_signals ();

		/* Plumbing */
		VE_IGNORE_EINTR (close (pipe1[1]));
		VE_IGNORE_EINTR (close (pipe2[0]));

		VE_IGNORE_EINTR (dup2 (pipe1[0], STDIN_FILENO));
		VE_IGNORE_EINTR (dup2 (pipe2[1], STDOUT_FILENO));

		mdm_log_shutdown ();

		mdm_close_all_descriptors (2 /* from */, slave_fifo_pipe_fd/* except */, d->slave_notify_fd/* except2 */);

		mdm_open_dev_null (O_RDWR); /* open stderr - fd 2 */

		mdm_log_init ();

		if G_UNLIKELY (setgid (mdm_daemon_config_get_mdmgid ()) < 0)
			mdm_child_exit (DISPLAY_ABORT,
					_("%s: Couldn't set groupid to %d"),
					"mdm_slave_greeter",
					mdm_daemon_config_get_mdmgid ());

		mdmuser = mdm_daemon_config_get_value_string (MDM_KEY_USER);
		if G_UNLIKELY (initgroups (mdmuser, mdm_daemon_config_get_mdmgid ()) < 0)
			mdm_child_exit (DISPLAY_ABORT,
					_("%s: initgroups () failed for %s"),
					"mdm_slave_greeter",
					mdmuser ? mdmuser : "(null)");

		if G_UNLIKELY (setuid (mdm_daemon_config_get_mdmuid ()) < 0)
			mdm_child_exit (DISPLAY_ABORT,
					_("%s: Couldn't set userid to %d"),
					"mdm_slave_greeter",
					mdm_daemon_config_get_mdmuid ());

		mdm_restoreenv ();
		mdm_reset_locale ();

		g_setenv ("XAUTHORITY", MDM_AUTHFILE (d), TRUE);
		g_setenv ("DISPLAY", d->name, TRUE);
		if (d->windowpath)
			g_setenv ("WINDOWPATH", d->windowpath, TRUE);		

		g_setenv ("LOGNAME", mdmuser, TRUE);
		g_setenv ("USER", mdmuser, TRUE);
		g_setenv ("USERNAME", mdmuser, TRUE);
		g_setenv ("MDM_GREETER_PROTOCOL_VERSION",
			  MDM_GREETER_PROTOCOL_VERSION, TRUE);
		g_setenv ("MDM_VERSION", VERSION, TRUE);

		pwent = getpwnam (mdmuser);
		if G_LIKELY (pwent != NULL) {
			/* Note that usually this doesn't exist */
			if (pwent->pw_dir != NULL &&
			    g_file_test (pwent->pw_dir, G_FILE_TEST_EXISTS))
				g_setenv ("HOME", pwent->pw_dir, TRUE);
			else
				g_setenv ("HOME",
					  ve_sure_string (mdm_daemon_config_get_value_string (MDM_KEY_SERV_AUTHDIR)),
					  TRUE); /* Hack */
			g_setenv ("SHELL", pwent->pw_shell, TRUE);
		} else {
			g_setenv ("HOME",
				  ve_sure_string (mdm_daemon_config_get_value_string (MDM_KEY_SERV_AUTHDIR)),
				  TRUE); /* Hack */
			g_setenv ("SHELL", "/bin/sh", TRUE);
		}

		defaultpath = mdm_daemon_config_get_value_string (MDM_KEY_PATH);
		if (ve_string_empty (g_getenv ("PATH"))) {
			g_setenv ("PATH", defaultpath, TRUE);
		} else if ( ! ve_string_empty (defaultpath)) {
			gchar *temp_string = g_strconcat (g_getenv ("PATH"),
							  ":", defaultpath, NULL);
			g_setenv ("PATH", temp_string, TRUE);
			g_free (temp_string);
		}
		g_setenv ("RUNNING_UNDER_MDM", "true", TRUE);
		if ( ! ve_string_empty (d->theme_name))
			g_setenv ("MDM_GTK_THEME", d->theme_name, TRUE);

		if (mdm_daemon_config_get_value_bool (MDM_KEY_DEBUG_GESTURES)) {
			g_setenv ("MDM_DEBUG_GESTURES", "true", TRUE);
		}

		/* Note that this is just informative, the slave will not listen to
		 * the greeter even if it does something it shouldn't on a non-local
		 * display so it's not a security risk */
		if (d->attached) {
			g_setenv ("MDM_IS_LOCAL", "yes", TRUE);
		} else {
			g_unsetenv ("MDM_IS_LOCAL");
		}

		/* this is again informal only, if the greeter does time out it will
		 * not actually login a user if it's not enabled for this display */
		if (d->timed_login_ok) {
			if (ParsedTimedLogin == NULL)
				g_setenv ("MDM_TIMED_LOGIN_OK", " ", TRUE);
			else
				g_setenv ("MDM_TIMED_LOGIN_OK", ParsedTimedLogin, TRUE);
		} else {
			g_unsetenv ("MDM_TIMED_LOGIN_OK");
		}

		if (SERVER_IS_FLEXI (d)) {
			g_setenv ("MDM_FLEXI_SERVER", "yes", TRUE);
		} else {
			g_unsetenv ("MDM_FLEXI_SERVER");
		}

		if G_UNLIKELY (d->is_emergency_server) {
			mdm_errorgui_error_box (d,
						GTK_MESSAGE_ERROR,
						_("No servers were defined in the "
						  "configuration file.  This can only be a "
						  "configuration error.  MDM has started "
						  "a single server for you.  You should "
						  "log in and fix the configuration.  "
						  "Note that automatic and timed logins "
						  "are disabled now."));
			g_unsetenv ("MDM_TIMED_LOGIN_OK");
		}

		if G_UNLIKELY (d->failsafe_xserver) {
			mdm_errorgui_error_box (d,
						GTK_MESSAGE_ERROR,
						_("Could not start the regular X "
						  "server (your graphical environment) "
						  "and so this is a failsafe X server.  "
						  "You should log in and properly "
						  "configure the X server."));
		}

		if G_UNLIKELY (d->busy_display) {
			char *msg = g_strdup_printf
				(_("The specified display number was busy, so "
				   "this server was started on display %s."),
				 d->name);
			mdm_errorgui_error_box (d, GTK_MESSAGE_ERROR, msg);
			g_free (msg);
		}

		if G_UNLIKELY (d->try_different_greeter) {
			/* FIXME: we should also really be able to do standalone failsafe
			   login, but that requires some work and is perhaps an overkill. */
			/* This should handle mostly the case where mdmgreeter is crashing
			   and we'd want to start mdmlogin for the user so that at least
			   something works instead of a flickering screen */
			mdm_error ("mdm_slave_greeter: Failed to run '%s', trying another greeter", command);
			if (strstr (command, "mdmlogin") != NULL) {
				/* in case it is mdmlogin that's crashing
				   try the themed greeter for luck */
				command = LIBEXECDIR "/mdmgreeter";
			} else {
				/* in all other cases, try the mdmlogin (standard greeter)
				   proggie */
				command = LIBEXECDIR "/mdmlogin";
			}
		}

		moduleslist = mdm_daemon_config_get_value_string (MDM_KEY_GTK_MODULES_LIST);

		if (mdm_daemon_config_get_value_bool (MDM_KEY_ADD_GTK_MODULES) &&
		    ! ve_string_empty (moduleslist) &&
		    /* don't add modules if we're trying to prevent crashes,
		       perhaps it's the modules causing the problem in the first place */
		    ! d->try_different_greeter) {
			gchar *modules = g_strdup_printf ("--gtk-module=%s", moduleslist);
			exec_command (command, modules);
			/* Something went wrong */
			mdm_error ("mdm_slave_greeter: Cannot start greeter with gtk modules: %s. Trying without modules", moduleslist);
			g_free (modules);
		}

		if (mdm_daemon_config_get_value_bool (MDM_KEY_NUMLOCK)) {
			if (g_file_test ("/usr/bin/numlockx", G_FILE_TEST_IS_EXECUTABLE)) {
				mdm_debug("mdm_slave_greeter: Enabling NumLock");
				system("/usr/bin/numlockx on");
			}
		}

		mdm_debug ("mdm_slave_greeter: Launching greeter '%s'", command);

		exec_command (command, NULL);

		mdm_error ("mdm_slave_greeter: Cannot start greeter trying default: %s", LIBEXECDIR "/mdmlogin");

		g_setenv ("MDM_WHACKED_GREETER_CONFIG", "true", TRUE);

		exec_command (LIBEXECDIR "/mdmlogin", NULL);

		VE_IGNORE_EINTR (execl (LIBEXECDIR "/mdmlogin", LIBEXECDIR "/mdmlogin", NULL));

		mdm_errorgui_error_box (d,
			       GTK_MESSAGE_ERROR,
			       _("Cannot start the greeter application; "
				 "you will not be able to log in.  "
				 "This display will be disabled.  "
				 "Try logging in by other means and "
				 "editing the configuration file"));

		/* If no greeter we really have to disable the display */
		mdm_child_exit (DISPLAY_ABORT,
				_("%s: Error starting greeter on display %s"),
				"mdm_slave_greeter",
				d->name ? d->name : "(null)");

	case -1:
		d->greetpid = 0;
		mdm_slave_exit (DISPLAY_REMANAGE, _("%s: Can't fork mdmgreeter process"), "mdm_slave_greeter");

	default:
		VE_IGNORE_EINTR (close (pipe1[0]));
		VE_IGNORE_EINTR (close (pipe2[1]));

		whack_greeter_fds ();

		greeter_fd_out = pipe1[1];
		greeter_fd_in = pipe2[0];

		mdm_debug ("mdm_slave_greeter: Greeter on pid %d", (int)pid);

		mdm_slave_send_num (MDM_SOP_GREETPID, d->greetpid);

		// Append pictures to greeter (except for mdmwebkit)
		run_pictures ();
		
		if (always_restart_greeter)
			mdm_slave_greeter_ctl_no_ret (MDM_ALWAYS_RESTART, "Y");
		else
			mdm_slave_greeter_ctl_no_ret (MDM_ALWAYS_RESTART, "N");
		mdmlang = g_getenv ("MDM_LANG");
		if (mdmlang)
			mdm_slave_greeter_ctl_no_ret (MDM_SETLANG, mdmlang);


		check_notifies_now ();
		break;
	}
}

/* This should not call anything that could cause a syslog in case we
 * are in a signal */
void
mdm_slave_send (const char *str, gboolean wait_for_ack)
{
	int i;
	uid_t old;

	if ( ! mdm_wait_for_ack)
		wait_for_ack = FALSE;

	if (wait_for_ack) {
		mdm_got_ack = FALSE;
		g_free (mdm_ack_response);
		mdm_ack_response = NULL;
	}

	mdm_fdprintf (slave_fifo_pipe_fd, "\n%s\n", str);	

#if defined (_POSIX_PRIORITY_SCHEDULING) && defined (HAVE_SCHED_YIELD)
	if (wait_for_ack && ! mdm_got_ack) {
		/* let the other process do its stuff */
		sched_yield ();
	}
#endif

	/* Wait till you get a response from the daemon */
	if (strncmp (str, "opcode="MDM_SOP_SHOW_ERROR_DIALOG,
		     strlen ("opcode="MDM_SOP_SHOW_ERROR_DIALOG)) == 0 ||
	    strncmp (str, "opcode="MDM_SOP_SHOW_YESNO_DIALOG,
		     strlen ("opcode="MDM_SOP_SHOW_YESNO_DIALOG)) == 0 ||
	    strncmp (str, "opcode="MDM_SOP_SHOW_QUESTION_DIALOG,
		     strlen ("opcode="MDM_SOP_SHOW_QUESTION_DIALOG)) == 0 ||
	    strncmp (str, "opcode="MDM_SOP_SHOW_ASKBUTTONS_DIALOG,
		     strlen ("opcode="MDM_SOP_SHOW_ASKBUTTONS_DIALOG)) == 0) {

		for (; wait_for_ack && !mdm_got_ack ; ) {
			fd_set rfds;

			FD_ZERO (&rfds);
			FD_SET (d->slave_notify_fd, &rfds);

			if (select (d->slave_notify_fd+1, &rfds, NULL, NULL, NULL) > 0) {
				mdm_slave_handle_usr2_message ();
			}
		}
        } else {
		for (i = 0;
		     wait_for_ack &&
			     ! mdm_got_ack &&
			     parent_exists () &&
			     i < 10;
		     i++) {
			if (in_usr2_signal > 0) {
				fd_set rfds;
				struct timeval tv;

				FD_ZERO (&rfds);
				FD_SET (d->slave_notify_fd, &rfds);

				/* Wait up to 1 second. */
				tv.tv_sec = 1;
				tv.tv_usec = 0;

				if (select (d->slave_notify_fd+1, &rfds, NULL, NULL, &tv) > 0) {
					mdm_slave_handle_usr2_message ();
				}
			} else {
				struct timeval tv;
				/* Wait 1 second. */
				tv.tv_sec = 1;
				tv.tv_usec = 0;
				select (0, NULL, NULL, NULL, &tv);
			
			}
		}
	}

	if G_UNLIKELY (wait_for_ack  &&
		       ! mdm_got_ack &&
		       mdm_in_signal == 0) {
		if (strncmp (str, MDM_SOP_COOKIE " ",
			     strlen (MDM_SOP_COOKIE " ")) == 0) {
			char *s = g_strndup
				(str, strlen (MDM_SOP_COOKIE " XXXX XX"));
			/* cut off most of the cookie for "security" */
			mdm_debug ("Timeout occurred for sending message %s...", s);
			g_free (s);
		} else {
			mdm_debug ("Timeout occurred for sending message %s", str);
		}
	}
}

void
mdm_slave_send_num (const char *opcode, long num)
{
	char *msg;

	if (mdm_in_signal == 0)
		mdm_debug ("Sending %s == %ld for slave %ld",
			   opcode,
			   (long)num,
			   (long)getpid ());

	msg = g_strdup_printf ("%s %ld %ld", opcode,
			       (long)getpid (), (long)num);

	mdm_slave_send (msg, TRUE);

	g_free (msg);
}

void
mdm_slave_send_string (const char *opcode, const char *str)
{
	char *msg;

	if G_UNLIKELY (mdm_daemon_config_get_value_bool (MDM_KEY_DEBUG) && mdm_in_signal == 0) {
		mdm_debug ("Sending %s == <secret> for slave %ld",
			   opcode,
			   (long)getpid ());
	}

	if (strcmp (opcode, MDM_SOP_SHOW_ERROR_DIALOG) == 0 ||
	    strcmp (opcode, MDM_SOP_SHOW_YESNO_DIALOG) == 0 ||
	    strcmp (opcode, MDM_SOP_SHOW_QUESTION_DIALOG) == 0 ||
	    strcmp (opcode, MDM_SOP_SHOW_ASKBUTTONS_DIALOG) == 0) {
		msg = g_strdup_printf ("opcode=%s$$pid=%ld$$%s", opcode,
				       (long)d->slavepid, ve_sure_string (str));
	} else {
		msg = g_strdup_printf ("%s %ld %s", opcode,
				       (long)getpid (), ve_sure_string (str));
	}

	mdm_slave_send (msg, TRUE);

	g_free (msg);
}

static gboolean
is_session_valid (const char *session_name)
{
	// Empty sessions aren't valid
	if (ve_string_empty(session_name)) {
		mdm_debug ("is_session_valid: Rejecting empty session");
		return FALSE;
	}

	// Autodetect session, by definition, isn't valid (used to delegate selection to session detection)
	if (strcmp (session_name, MDM_SESSION_AUTO) == 0) {
		mdm_debug ("is_session_valid: Rejecting auto session");
		return FALSE;
	}

	// For other sessions we check the session file to see if Exec and Try_Exec are in the path
	gboolean valid = FALSE;
	char * exec = mdm_daemon_config_get_session_exec (session_name, TRUE /* check_try_exec */);
	if (exec != NULL) {
		valid = TRUE;
	}
	g_free (exec);
	return valid;
}

static char *
find_a_session (void)
{
	char *session;
	const char *default_session = mdm_daemon_config_get_value_string (MDM_KEY_DEFAULT_SESSION);
	if (is_session_valid (default_session)) {
		mdm_debug ("find_a_session: Applied default session '%s'", default_session);
		session = g_strdup (default_session);
	}
	else {
		char ** default_sessions = g_strsplit (mdm_daemon_config_get_value_string (MDM_KEY_DEFAULT_SESSIONS), ",", -1);
		int i;
		for (i = 0; default_sessions != NULL && default_sessions[i] != NULL; i++) {
			if (is_session_valid (default_sessions[i])) {
				mdm_debug ("find_a_session: Detected and applied valid session '%s'", default_sessions[i]);
				session = g_strdup (default_sessions[i]);
				break;
			}
		}
		g_strfreev (default_sessions);
	}
	return session;
}

static gboolean
wipe_xsession_errors (struct passwd *pwent,
		      const char *home_dir,
		      gboolean home_dir_ok)
{
	gboolean wiped_something = FALSE;
	DIR *dir;
	struct dirent *ent;
	uid_t old = geteuid ();
	uid_t oldg = getegid ();

	seteuid (0);
	if G_UNLIKELY (setegid (pwent->pw_gid) != 0 ||
		       seteuid (pwent->pw_uid) != 0) {
		NEVER_FAILS_root_set_euid_egid (old, oldg);
		return FALSE;
	}

	if G_LIKELY (home_dir_ok) {
		char *filename = g_build_filename (home_dir,
						   ".xsession-errors",
						   NULL);
		if (g_access (filename, F_OK) == 0) {
			wiped_something = TRUE;
			VE_IGNORE_EINTR (g_unlink (filename));
		}
		g_free (filename);
	}

	VE_IGNORE_EINTR (dir = opendir ("/tmp"));
	if G_LIKELY (dir != NULL) {
		char *prefix = g_strdup_printf ("xses-%s.", pwent->pw_name);
		int prefixlen = strlen (prefix);
		VE_IGNORE_EINTR (ent = readdir (dir));
		while (ent != NULL) {
			if (strncmp (ent->d_name, prefix, prefixlen) == 0) {
				char *filename = g_strdup_printf ("/tmp/%s",
								  ent->d_name);
				wiped_something = TRUE;
				VE_IGNORE_EINTR (g_unlink (filename));
				g_free (filename);
			}
			VE_IGNORE_EINTR (ent = readdir (dir));
		}
		VE_IGNORE_EINTR (closedir (dir));
		g_free (prefix);
	}

	NEVER_FAILS_root_set_euid_egid (old, oldg);

	return wiped_something;
}

static int
open_xsession_errors (struct passwd *pwent,
		      const char *home_dir,
		      gboolean home_dir_ok)
{
	int logfd = -1;

	g_free (d->xsession_errors_filename);
	d->xsession_errors_filename = NULL;

        /* Log all output from session programs to a file,
	 * unless in failsafe mode which needs to work when there is
	 * no diskspace as well */
	if G_LIKELY (home_dir_ok) {
		char *filename = g_build_filename (home_dir,
						   ".xsession-errors",
						   NULL);
		uid_t old = geteuid ();
		uid_t oldg = getegid ();

		seteuid (0);
		if G_LIKELY (setegid (pwent->pw_gid) == 0 &&
			     seteuid (pwent->pw_uid) == 0) {
			/* unlink to be anal */
			VE_IGNORE_EINTR (g_unlink (filename));
			VE_IGNORE_EINTR (logfd = open (filename, O_EXCL|O_CREAT|O_TRUNC|O_WRONLY, 0644));
		}
		NEVER_FAILS_root_set_euid_egid (old, oldg);

		if G_UNLIKELY (logfd < 0) {
			mdm_error ("run_session_child: Could not open ~/.xsession-errors");
			g_free (filename);
		} else {
			d->xsession_errors_filename = filename;
		}
	}

	/* let's try an alternative */
	if G_UNLIKELY (logfd < 0) {
		mode_t oldmode;

		char *filename = g_strdup_printf ("/tmp/xses-%s.XXXXXX",
						  pwent->pw_name);
		uid_t old = geteuid ();
		uid_t oldg = getegid ();

		seteuid (0);
		if G_LIKELY (setegid (pwent->pw_gid) == 0 &&
			     seteuid (pwent->pw_uid) == 0) {
			oldmode = umask (077);
			logfd = mkstemp (filename);
			umask (oldmode);
		}

		NEVER_FAILS_root_set_euid_egid (old, oldg);

		if G_LIKELY (logfd >= 0) {
			d->xsession_errors_filename = filename;
		} else {
			g_free (filename);
		}
	}

	return logfd;
}

#ifdef HAVE_SELINUX
/* This should be run just before we exec the user session */
static gboolean
mdm_selinux_setup (const char *login)
{
	security_context_t scontext;
	int ret=-1;
	char *seuser=NULL;
	char *level=NULL;

	/* If selinux is not enabled, then we don't do anything */
	if (is_selinux_enabled () <= 0)
		return TRUE;

	if (getseuserbyname(login, &seuser, &level) == 0) {
		ret=get_default_context_with_level(seuser, level, 0, &scontext);
		free(seuser);
		free(level);
	}

	if (ret < 0) {
		mdm_error ("SELinux mdm login: unable to obtain default security context for %s.", login);
		/* note that this will be run when the .xsession-errors
		   is already being logged, so we can use stderr */
		mdm_fdprintf (2, "SELinux mdm login: unable to obtain default security context for %s.", login);
 		return (security_getenforce()==0);
	}

	mdm_assert (scontext != NULL);

	if (setexeccon (scontext) != 0) {
		mdm_error ("SELinux mdm login: unable to set executable context %s.",
			   (char *)scontext);
		mdm_fdprintf (2, "SELinux mdm login: unable to set executable context %s.",
			      (char *)scontext);
		freecon (scontext);
		return (security_getenforce()==0);
	}

	freecon (scontext);

	return TRUE;
}
#endif /* HAVE_SELINUX */

static void
session_child_run (struct passwd *pwent,
		   int logfd,
		   const char *home_dir,
		   gboolean home_dir_ok,
#ifdef WITH_CONSOLE_KIT
		   const char *ck_session_cookie,
#endif
		   const char *session,
		   const char *save_session,
		   const char *language,
		   const char *gnome_session,
		   gboolean usrcfgok,
		   gboolean savesess,
		   gboolean savelang)
{
	char *sessionexec = NULL;
	GString *fullexec = NULL;
	const char *shell = NULL;
	const char *greeter;
	gint result;
	gchar **argv = NULL;

#ifdef CAN_USE_SETPENV
	extern char **newenv;
	int i;
#endif

	mdm_unset_signals ();
	if G_UNLIKELY (setsid () < 0)
		/* should never happen */
		mdm_error ("session_child_run: setsid () failed: %s!", strerror (errno));

	g_setenv ("XAUTHORITY", MDM_AUTHFILE (d), TRUE);

	/* Here we setup our 0,1,2 descriptors, we do it here
	 * nowdays rather then later on so that we get errors even
	 * from the PreSession script */
	if G_LIKELY (logfd >= 0) {
		VE_IGNORE_EINTR (dup2 (logfd, 1));
		VE_IGNORE_EINTR (dup2 (logfd, 2));
		VE_IGNORE_EINTR (close (logfd));
	} else {
		VE_IGNORE_EINTR (close (1));
		VE_IGNORE_EINTR (close (2));
		mdm_open_dev_null (O_RDWR); /* open stdout - fd 1 */
		mdm_open_dev_null (O_RDWR); /* open stderr - fd 2 */
	}

	VE_IGNORE_EINTR (close (0));
	mdm_open_dev_null (O_RDONLY); /* open stdin - fd 0 */

	/* Set this for the PreSession script */
	/* compatibility */
	g_setenv ("MDMSESSION", session, TRUE);
	g_setenv ("GDMSESSION", session, TRUE);

	g_setenv ("DESKTOP_SESSION", session, TRUE);

	/* Determine default greeter type so the PreSession */
	/* script can set the appropriate background color. */
	greeter = mdm_daemon_config_get_value_string (MDM_KEY_GREETER);	

	if (strstr (greeter, "mdmlogin") != NULL) {
		g_setenv ("MDM_GREETER_TYPE", "PLAIN", TRUE);
	} else if (strstr (greeter, "mdmgreeter") != NULL) {
		g_setenv ("MDM_GREETER_TYPE", "THEMED", TRUE);
	} else if (strstr (greeter, "mdmwebkit") != NULL) {
		g_setenv ("MDM_GREETER_TYPE", "HTML", TRUE);
	} else {
		/* huh? */
		g_setenv ("MDM_GREETER_TYPE", "unknown", TRUE);
	}

	/* Run the PreSession script */
	if G_UNLIKELY (mdm_slave_exec_script (d, mdm_daemon_config_get_value_string (MDM_KEY_PRESESSION),
                                              pwent->pw_name, pwent,
					      TRUE /* pass_stdout */) != EXIT_SUCCESS)
		/* If script fails reset X server and restart greeter */
		mdm_child_exit (DISPLAY_REMANAGE,
				_("%s: Execution of PreSession script returned > 0. Aborting."),
				"session_child_run");

	ve_clearenv ();

	/* Prepare user session */
	g_setenv ("XAUTHORITY", d->userauth, TRUE);
	g_setenv ("DISPLAY", d->name, TRUE);
	if (d->windowpath)
		g_setenv ("WINDOWPATH", d->windowpath, TRUE);
	g_setenv ("LOGNAME", pwent->pw_name, TRUE);
	g_setenv ("USER", pwent->pw_name, TRUE);
	g_setenv ("USERNAME", pwent->pw_name, TRUE);
	g_setenv ("HOME", home_dir, TRUE);
#ifdef WITH_CONSOLE_KIT
	if (ck_session_cookie != NULL) {
		g_setenv ("XDG_SESSION_COOKIE", ck_session_cookie, TRUE);
	}
#endif
	g_setenv ("PWD", home_dir, TRUE);
	g_setenv ("MDMSESSION", session, TRUE);
	g_setenv ("GDMSESSION", session, TRUE);
	g_setenv ("DESKTOP_SESSION", session, TRUE);
	g_setenv ("SHELL", pwent->pw_shell, TRUE);

    g_setenv ("XDG_SESSION_DESKTOP", session, TRUE);

	if (d->type == TYPE_STATIC) {
		g_setenv ("MDM_XSERVER_LOCATION", "local", TRUE);
		g_setenv ("GDM_XSERVER_LOCATION", "local", TRUE);	
	} else if (d->type == TYPE_FLEXI) {
		g_setenv ("MDM_XSERVER_LOCATION", "flexi", TRUE);
		g_setenv ("GDM_XSERVER_LOCATION", "flexi", TRUE);	
	} else {
		/* huh? */
		g_setenv ("MDM_XSERVER_LOCATION", "unknown", TRUE);
		g_setenv ("GDM_XSERVER_LOCATION", "unknown", TRUE);
	}

	if (gnome_session != NULL)
		g_setenv ("MDM_GNOME_SESSION", gnome_session, TRUE);

	/* Special PATH for root */
	if (pwent->pw_uid == 0)
		g_setenv ("PATH", mdm_daemon_config_get_value_string (MDM_KEY_ROOT_PATH), TRUE);
	else
		g_setenv ("PATH", mdm_daemon_config_get_value_string (MDM_KEY_PATH), TRUE);

	/* Now still as root make the system authfile not readable by others,
	   and therefore not by the mdm user */
	VE_IGNORE_EINTR (g_chmod (MDM_AUTHFILE (d), 0640));

	setpgid (0, 0);

	umask (022);

	/* setup the verify env vars */
	if G_UNLIKELY ( ! mdm_verify_setup_env (d))
		mdm_child_exit (DISPLAY_REMANAGE,
				_("%s: Could not setup environment for %s. "
				  "Aborting."),
				"session_child_run",
				login_user ? login_user : "(null)");

        /* setup euid/egid to the correct user,
         * not to leave the egid around.  It's
         * ok to mdm_fail here */
        NEVER_FAILS_root_set_euid_egid (pwent->pw_uid, pwent->pw_gid);

	VE_IGNORE_EINTR (result = g_chdir (home_dir));
	if G_UNLIKELY (result != 0) {
		VE_IGNORE_EINTR (g_chdir ("/"));
		NEVER_FAILS_root_set_euid_egid (0, 0);
	} else if (pwent->pw_uid != 0) {
                /* sanitize .ICEauthority to be of the correct
                 * permissions, if it exists */
                struct stat s0, s1, s2;
                gint        s0_ret, s1_ret, s2_ret;
                gint        iceauth_fd;

		NEVER_FAILS_root_set_euid_egid (0, 0);

                iceauth_fd = open (".ICEauthority", O_RDONLY);

                s0_ret = stat (home_dir, &s0);
                s1_ret = lstat (".ICEauthority", &s1);
                s2_ret = fstat (iceauth_fd, &s2);

                if (iceauth_fd >= 0 &&
                    s0_ret == 0 &&
                    s0.st_uid == pwent->pw_uid &&
                    s1_ret == 0 &&
                    s2_ret == 0 &&
                    S_ISREG (s1.st_mode) &&
                    s1.st_ino == s2.st_ino &&
                    s1.st_dev == s2.st_dev &&
                    s1.st_uid == s2.st_uid &&
                    s1.st_gid == s2.st_gid &&
                    s1.st_mode == s2.st_mode &&
                    (s1.st_uid != pwent->pw_uid ||
                     s1.st_gid != pwent->pw_gid ||
                     (s1.st_mode & (S_IRWXG|S_IRWXO)) ||
                     !(s1.st_mode & S_IRWXU))) {
                        /* This may not work on NFS, but oh well, there
                         * this is beyond our help, but it's unlikely
                         * that it got screwed up when NFS was used
                         * in the first place */

                        /* only if we own the current directory */
                        fchown (iceauth_fd,
                                pwent->pw_uid,
                                pwent->pw_gid);
                        fchmod (iceauth_fd, S_IRUSR | S_IWUSR);
                }

                if (iceauth_fd >= 0)
                        close (iceauth_fd);
        }

	NEVER_FAILS_setegid (pwent->pw_gid);
#ifdef HAVE_LOGINCAP
	if (setusercontext (NULL, pwent, pwent->pw_uid,
			    LOGIN_SETLOGIN | LOGIN_SETPATH |
			    LOGIN_SETPRIORITY | LOGIN_SETRESOURCES |
			    LOGIN_SETUMASK | LOGIN_SETUSER |
			    LOGIN_SETENV) < 0)
		mdm_child_exit (DISPLAY_REMANAGE,
				_("%s: setusercontext () failed for %s. "
				  "Aborting."), "session_child_run",
				login ? login : "(null)");
#else
	if G_UNLIKELY (setuid (pwent->pw_uid) < 0)
		mdm_child_exit (DISPLAY_REMANAGE,
				_("%s: Could not become %s. Aborting."),
				"session_child_run",
				login_user ? login_user : "(null)");
#endif

	/* Only force MDM_LANG to something if there is other then
	 * system default selected.  Else let the session do whatever it
	 * does since we're using sys default */
	if ( ! ve_string_empty (language)) {
		g_setenv ("LANG", language, TRUE);
		g_setenv ("MDM_LANG", language, TRUE);
	}

	/* just in case there is some weirdness going on */
	VE_IGNORE_EINTR (g_chdir (home_dir));

        if (usrcfgok && home_dir_ok)
		mdm_daemon_config_set_user_session_lang (savesess, savelang, home_dir, save_session, language);

	mdm_log_shutdown ();

	mdm_close_all_descriptors (3 /* from */, slave_fifo_pipe_fd /* except */, d->slave_notify_fd /* except2 */);

	mdm_log_init ();

	sessionexec = mdm_daemon_config_get_session_exec (session, FALSE /* check_try_exec */);

	fullexec = g_string_new (NULL);

	if (sessionexec != NULL) {
		const char *basexsession = mdm_daemon_config_get_value_string (MDM_KEY_BASE_XSESSION);
		char **bxvec = g_strsplit (basexsession, " ", -1);

		if G_UNLIKELY (bxvec == NULL || g_access (bxvec[0], X_OK) != 0) {
			mdm_error ("session_child_run: Cannot find or run the base Xsession script.");
		}


		/*
		 * This is where the session is OK, and note that
		 * we really DON'T care about leaks, we are going to
		 * exec in just a bit
		 */
		g_string_append (fullexec, bxvec[0]);
		g_string_append (fullexec, " ");

		//If we find space chars in the session Exec, and there's no quotes around them, add quotes.
		if ((strchr (sessionexec, ' ') != NULL) && (strchr (sessionexec, '"') == NULL)) {
			// Exec line doesn't contain quotes, let's add them
			mdm_debug ("Warning, session Exec line did not contain quotes: %s", sessionexec);
			g_string_append (fullexec, "\"");
			g_string_append (fullexec, sessionexec);
			g_string_append (fullexec, "\"");
		}
		else {
			g_string_append (fullexec, sessionexec);
		}

		g_strfreev (bxvec);
	}

	mdm_debug ("Running %s for %s on %s", fullexec->str, login_user, d->name);

	if ( ! ve_string_empty (pwent->pw_shell)) {
		shell = pwent->pw_shell;
	} else {
		shell = "/bin/sh";
	}

	/* just a stupid test */
	if (strcmp (shell, NOLOGIN) == 0 ||
	    strcmp (shell, "/bin/false") == 0 ||
	    strcmp (shell, "/bin/true") == 0) {
		mdm_error ("session_child_run: User not allowed to log in");
		mdm_errorgui_error_box (d, GTK_MESSAGE_ERROR,
			       _("The system administrator has "
				 "disabled your account."));
		/* ends as if nothing bad happened */
		/* 66 means no "session crashed" examine .xsession-errors
		   dialog */
		_exit (66);
	}

#ifdef CAN_USE_SETPENV
	/* Call the function setpenv which instanciates the extern variable "newenv" */
	setpenv (login, (PENV_INIT | PENV_NOEXEC), NULL, NULL);

	/* Add the content of the "newenv" variable to the environment */
	for (i=0; newenv != NULL && newenv[i] != NULL; i++) {
		char *env_str = g_strdup (newenv[i]);
		char *p = strchr (env_str, '=');
		if (p != NULL) {
			/* Add a NULL byte to terminate the variable name */
			p[0] = '\0';
			/* Add the variable to the env */
			g_setenv (env_str, &p[1], TRUE);
		}
		g_free (env_str);
	}
#endif

#ifdef HAVE_SELINUX
	if ( ! mdm_selinux_setup (pwent->pw_name)) {
		/* 66 means no "session crashed" examine .xsession-errors
		   dialog */
		mdm_errorgui_error_box (d, GTK_MESSAGE_ERROR,
			       _("Error! Unable to set executable context."));
		_exit (66);
	}
#endif

        g_shell_parse_argv (fullexec->str, NULL, &argv, NULL);
	VE_IGNORE_EINTR (execv (argv[0], argv));
	g_strfreev (argv);

	/* will go to .xsession-errors */
	fprintf (stderr, "session_child_run: Could not exec %s", fullexec->str);
	mdm_error ("session_child_run: Could not exec %s", fullexec->str);
	g_string_free (fullexec, TRUE);

	/* if we can't read and exec the session, then make a nice
	 * error dialog */
	mdm_errorgui_error_box
		(d, GTK_MESSAGE_ERROR,
		 /* we can't really be any more specific */
		 _("Cannot start the session due to some "
		   "internal error."));

	/* ends as if nothing bad happened */
	_exit (0);
}

static void
finish_session_output (gboolean do_read)
{
	if G_LIKELY (d->session_output_fd >= 0)  {
		if (do_read)
			run_session_output (TRUE /* read_until_eof */);
		if (d->session_output_fd >= 0)  {
			VE_IGNORE_EINTR (close (d->session_output_fd));
			d->session_output_fd = -1;
		}
		if (d->xsession_errors_fd >= 0)  {
			VE_IGNORE_EINTR (close (d->xsession_errors_fd));
			d->xsession_errors_fd = -1;
		}
	}
}

static GString *
mdm_slave_parse_enriched_string (MdmDisplay *d, const gchar *s)
{
	GString *str = g_string_new (NULL);
	gchar cmd;

	while (s[0] != '\0') {

		if (s[0] == '%' && s[1] != 0) {
			cmd = s[1];
			s++;

			switch (cmd) {

			case 'h':
				g_string_append (str, d->hostname);
				break;

			case 'd':
				g_string_append (str, d->name);
				break;

			case '%':
				g_string_append_c (str, '%');
				break;

			default:
				break;
			};
		} else {
			g_string_append_c (str, *s);
		}
		s++;
	}
	return (str);
}

static gchar *
mdm_slave_update_pseudo_device (MdmDisplay *d, const char *device_in)
{
	GString *str;
	struct stat st;
	gchar *device;

	/* If not a valid device, then do not update */
	if (device_in == NULL || strncmp (device_in, "/dev/", 5) != 0) {
		mdm_debug ("Warning, invalid device %s", device_in);
		return NULL;
	}

	/* Parse the string, changing %d to display or %h to hostname, etc. */
	str = mdm_slave_parse_enriched_string (d, device_in);
	device = g_strdup (str->str);
	g_string_free (str, TRUE);

	/* If using pseudo-devices, setup symlink if it does not exist */
	if (device != NULL &&
	    mdm_daemon_config_get_value_bool (MDM_KEY_UTMP_PSEUDO_DEVICE)) {
		gchar *buf;


		if (stat (device, &st) != 0) {
			mdm_debug ("Creating pseudo-device %s", device);
			symlink ("/dev/null", device);
		} else if ((buf = realpath (device, NULL)) != NULL) {
			if (strcmp (buf, "/dev/null") == 0) {
				/* Touch symlink */
				struct utimbuf  timebuf;

				timebuf.modtime = time ((time_t *) 0);
				timebuf.actime  = timebuf.modtime;

				if ((utime (device, &timebuf)) != 0)
					mdm_debug ("Problem updating access time of pseudo-device %s", device);
				else
					mdm_debug ("Touching pseudo-device %s",
						device);
			} else {
				mdm_debug ("Device %s points to %s", device, buf);
			}
			free (buf);
		} else {
			mdm_debug ("Device %s is not a symlink", device);
		}
	}
	return (device);
}

gchar *
mdm_slave_get_display_device (MdmDisplay *d)
{
	gchar *device_name = NULL;
	
	if (d->vtnum != -1)
		device_name = mdm_get_vt_device (d->vtnum);

	/*
	 * Default to the value in the MDM configuration for the
	 * display number.  Allow pseudo devices.
	 */
	if (device_name == NULL && d->device_name != NULL) {
		device_name = mdm_slave_update_pseudo_device (d,
			d->device_name);
	}

	/* If not VT, then use default local value from configuration */
	if (device_name == NULL) {
		const char *dev_local =
			mdm_daemon_config_get_value_string (MDM_KEY_UTMP_LINE_ATTACHED);

		if (dev_local != NULL) {
			device_name = mdm_slave_update_pseudo_device (d,
				dev_local);
		}
	}

	mdm_debug ("Display device is %s for display %s", device_name, d->name);
	return (device_name);
}
	
void
mdm_slave_write_utmp_wtmp_record (MdmDisplay *d,
			MdmSessionRecordType record_type,
			const gchar *username,
			GPid  pid)
{
#if defined(HAVE_UTMPX_H)
	struct utmpx record = { 0 };
	struct utmpx *u = NULL;
#else
	struct utmp record = { 0 };
#endif
	GTimeVal now = { 0 };
	gchar *device_name = NULL;
	gchar *host;

	device_name = mdm_slave_get_display_device (d);

	mdm_debug ("Writing %s utmp-wtmp record",
	       record_type == MDM_SESSION_RECORD_TYPE_LOGIN ? "session" :
	       record_type == MDM_SESSION_RECORD_TYPE_LOGOUT ?  "logout" :
	       "failed session attempt");

	if (record_type != MDM_SESSION_RECORD_TYPE_LOGOUT) {
		/*
		 * It is possible that PAM failed before
		 * it mapped the user input into a valid username
		 * so we fallback to try using "(unknown)".  We
		 * don't ever log user input directly, because
		 * we don't want passwords entered into the 
		 * username entry to accidently get logged.
		 */
		if (username != NULL) {
#if defined(HAVE_UT_UT_USER)
			strncpy (record.ut_user,
				 username, 
				 sizeof (record.ut_user));
#elif defined(HAVE_UT_UT_NAME)
			strncpy (record.ut_name,
				 username,
				 sizeof (record.ut_name));
#endif
		} else {
			g_assert (record_type == MDM_SESSION_RECORD_TYPE_FAILED_ATTEMPT);
#if defined(HAVE_UT_UT_USER)
			strncpy (record.ut_user,
				 "(unknown)",
				 sizeof (record.ut_user));
#elif defined(HAVE_UT_UT_NAME)
			strncpy (record.ut_name,
				 "(unknown)",
				 sizeof (record.ut_name));
#endif
		}

#if defined(HAVE_UT_UT_USER)
		mdm_debug ("utmp-wtmp: Using username %*s",
			   (int) sizeof (record.ut_user),
			   record.ut_user);
#elif defined(HAVE_UT_UT_NAME)
		mdm_debug ("utmp-wtmp: Using username %*s",
			   (int) sizeof (record.ut_name),
			   record.ut_name);
#endif
	}

#if defined(HAVE_UT_UT_TYPE)
	if (record_type == MDM_SESSION_RECORD_TYPE_LOGOUT) {
		record.ut_type = DEAD_PROCESS;
		mdm_debug ("utmp-wtmp: Using type DEAD_PROCESS"); 
	} else  {
		record.ut_type = USER_PROCESS;
		mdm_debug ("utmp-wtmp: Using type USER_PROCESS"); 
	}
#endif

#if defined(HAVE_UT_UT_PID)
	record.ut_pid = pid;
	mdm_debug ("utmp-wtmp: Using pid %d", (gint)record.ut_pid);
#endif

#if defined(HAVE_UT_UT_TV)
	g_get_current_time (&now);
	record.ut_tv.tv_sec = now.tv_sec;
	mdm_debug ("utmp-wtmp: Using time %ld", (glong) record.ut_tv.tv_sec);
#elif defined(HAVE_UT_UT_TIME)
	time (&record.ut_time);
#endif

#if defined(HAVE_UT_UT_ID)
	strncpy (record.ut_id, d->name, sizeof (record.ut_id));
	mdm_debug ("utmp-wtmp: Using id %*s",
	       (int) sizeof (record.ut_id),
	       record.ut_id);
#endif

	if (device_name != NULL) {
		g_assert (g_str_has_prefix (device_name, "/dev/"));
		strncpy (record.ut_line, device_name + strlen ("/dev/"),
			 sizeof (record.ut_line));
		g_free (device_name);
		device_name = NULL;
	}

	mdm_debug ("utmp-wtmp: Using line %*s",
	       (int) sizeof (record.ut_line),
	       record.ut_line);

#if defined(HAVE_UT_UT_HOST)
	host = NULL;
	if (! d->attached && g_str_has_prefix (d->name, ":")) {
		host = g_strdup_printf ("%s%s",
					d->hostname,
					d->name);
	} else {
		host = g_strdup (d->name);
	}

	if (host) {
		strncpy (record.ut_host, host, sizeof (record.ut_host));
		g_free (host);

		mdm_debug ("utmp-wtmp: Using hostname %*s",
		   (int) sizeof (record.ut_host),
		   record.ut_host);

#ifdef HAVE_UT_SYSLEN
		record.ut_syslen = MIN (strlen (host), sizeof (record.ut_host));
#endif
	} 
#endif

	switch (record_type)
	{
	case MDM_SESSION_RECORD_TYPE_LOGIN:
		mdm_debug ("Login utmp/wtmp record");
#if defined(HAVE_UPDWTMPX)
		updwtmpx (MDM_NEW_RECORDS_FILE, &record);
#elif defined(HAVE_LOGWTMP) && defined(HAVE_UT_UT_HOST) && !defined(HAVE_LOGIN)
#if defined(HAVE_UT_UT_USER)
		logwtmp (record.ut_line, record.ut_user, record.ut_host);
#elif defined(HAVE_UT_UT_NAME)
		logwtmp (record.ut_line, record.ut_name, record.ut_host);
#endif
#endif

#if defined(HAVE_GETUTXENT)
		/* Update if entry already exists */
		while ((u = getutxent ()) != NULL) {
			if (u->ut_type == USER_PROCESS &&
			   (record.ut_line != NULL &&
			   (strncmp (u->ut_line, record.ut_line,
				     sizeof (u->ut_line)) == 0 ||
			    u->ut_pid == record.ut_pid))) {

				mdm_debug ("Updating existing utmp record");
				pututxline (&record);
				break;
			}
		}
		endutxent ();

		/* Add new entry if update did not work */
		if (u == (struct utmpx *)NULL) {
			mdm_debug ("Adding new utmp record");
			pututxline (&record);
		}
#elif defined(HAVE_LOGIN)
		login (&record);
#endif

		break;

	case MDM_SESSION_RECORD_TYPE_LOGOUT: 
		mdm_debug ("Logout utmp/wtmp record");

#if defined(HAVE_UPDWTMPX)
		updwtmpx (MDM_NEW_RECORDS_FILE, &record);
#elif defined(HAVE_LOGWTMP)
		logwtmp (record.ut_line, "", "");
#endif

#if defined(HAVE_GETUTXENT)
		setutxent ();

		while ((u = getutxent ()) != NULL &&
		       (u = getutxid (&record)) != NULL) {

			mdm_debug ("Removing utmp record");
			if (u->ut_pid == pid &&
			    u->ut_type == DEAD_PROCESS) {
				/* Already done */
				break;
			}

			u->ut_type = DEAD_PROCESS;
			u->ut_tv.tv_sec = record.ut_tv.tv_sec;
			u->ut_exit.e_termination = 0;
			u->ut_exit.e_exit = 0;

			pututxline (u);

			break;
		}

		endutxent ();
#elif defined(HAVE_LOGOUT)
		logout (record.ut_line);
#endif
		break;

	case MDM_SESSION_RECORD_TYPE_FAILED_ATTEMPT:
#if defined(HAVE_UPDWTMPX)
		mdm_debug ("Writing failed session attempt record to " 
			   MDM_BAD_RECORDS_FILE);
		updwtmpx (MDM_BAD_RECORDS_FILE, &record);
#endif
		break;
	}
}

static void
mdm_slave_session_start (void)
{
	struct passwd *pwent;
	const char *home_dir = NULL;
	char *save_session = NULL, *session = NULL, *language = NULL, *usrsess, *usrlang;
	char *gnome_session = NULL;
#ifdef WITH_CONSOLE_KIT
	char *ck_session_cookie;
#endif
	char *tmp;
	gboolean savesess = FALSE, savelang = FALSE;
	gboolean usrcfgok = FALSE, authok = FALSE;
	gboolean home_dir_ok = FALSE;
	time_t session_start_time, end_time; 
	pid_t pid;
	MdmWaitPid *wp;
	uid_t uid;
	gid_t gid;
	int logpipe[2];
	int logfilefd;

	mdm_debug ("mdm_slave_session_start: Attempting session for user '%s'",
		   login_user);

	pwent = getpwnam (login_user);

	if G_UNLIKELY (pwent == NULL)  {
		/* This is sort of an "assert", this should NEVER happen */
		if (greet)
			mdm_slave_whack_greeter ();
		mdm_slave_exit (DISPLAY_REMANAGE,
				_("%s: User passed auth but getpwnam (%s) failed!"), "mdm_slave_session_start", login_user);
	}

	logged_in_uid = uid = pwent->pw_uid;
	logged_in_gid = gid = pwent->pw_gid;

	/* Run the PostLogin script */
	if G_UNLIKELY (mdm_slave_exec_script (d, mdm_daemon_config_get_value_string (MDM_KEY_POSTLOGIN),
					      login_user, pwent,
					      TRUE /* pass_stdout */) != EXIT_SUCCESS) {
		mdm_verify_cleanup (d);
		mdm_error ("mdm_slave_session_start: Execution of PostLogin script returned > 0. Aborting.");
		/* script failed so just try again */
		return;
	}

	/*
	 * Set euid, gid to user before testing for user's $HOME since root
	 * does not always have access to the user's $HOME directory.
	 */
	if G_UNLIKELY (setegid (pwent->pw_gid) != 0 ||
		       seteuid (pwent->pw_uid) != 0) {
		mdm_error ("Cannot set effective user/group id");
		mdm_verify_cleanup (d);
		session_started = FALSE;
		return;
	}

	if G_UNLIKELY (pwent->pw_dir == NULL ||
		       ! g_file_test (pwent->pw_dir, G_FILE_TEST_IS_DIR)) {
		char *yesno_msg;
		char *msg = g_strdup_printf (
					     _("Your home directory is listed as: '%s' "
					       "but it does not appear to exist.  "
					       "Do you want to log in with the / (root) "
					       "directory as your home directory? "
					       "It is unlikely anything will work unless "
					       "you use a failsafe session."),
					     ve_sure_string (pwent->pw_dir));

		/* Set euid, egid to root:mdm to manage user interaction */
		seteuid (0);
		setegid (mdm_daemon_config_get_mdmgid ());

		mdm_error ("mdm_slave_session_start: Home directory for %s: '%s' does not exist!", login_user, ve_sure_string (pwent->pw_dir));

		/* Check what the user wants to do */
		yesno_msg = g_strdup_printf ("yesno_msg=%s", msg);
		mdm_slave_send_string (MDM_SOP_SHOW_YESNO_DIALOG, yesno_msg);

		g_free (yesno_msg);

		if (strcmp (mdm_ack_response, "no") == 0) {
			mdm_verify_cleanup (d);
			session_started = FALSE;

			g_free (msg);
			g_free (mdm_ack_response);
			mdm_ack_response = NULL;
			return;
		}

		g_free (msg);
		g_free (mdm_ack_response);
		mdm_ack_response = NULL;

		/* Reset euid, egid back to user */
		if G_UNLIKELY (setegid (pwent->pw_gid) != 0 ||
			       seteuid (pwent->pw_uid) != 0) {
			mdm_error ("Cannot set effective user/group id");
			mdm_verify_cleanup (d);
			session_started = FALSE;
			return;
		}

		home_dir_ok = FALSE;
		home_dir = "/";
	} else {
		home_dir_ok = TRUE;
		home_dir = pwent->pw_dir;
	}

	if G_LIKELY (home_dir_ok) {
		/* Sanity check on ~user/.dmrc */
		usrcfgok = mdm_file_check ("mdm_slave_session_start", pwent->pw_uid,
					   home_dir, ".dmrc", TRUE, FALSE,
					   mdm_daemon_config_get_value_int (MDM_KEY_USER_MAX_FILE),
					   mdm_daemon_config_get_value_int (MDM_KEY_RELAX_PERM));
	} else {
		usrcfgok = FALSE;
	}

	if G_LIKELY (usrcfgok) {
		mdm_daemon_config_get_user_session_lang (&usrsess, &usrlang, home_dir);
	} else {
		/* This won't get displayed if the .dmrc file simply doesn't
		 * exist since we pass absentok=TRUE when we call mdm_file_check
		 */
		mdm_errorgui_error_box (d,
			       GTK_MESSAGE_WARNING,
			       _("User's $HOME/.dmrc file is being ignored.  "
				 "This prevents the default session "
				 "and language from being saved.  File "
				 "should be owned by user and have 644 "
				 "permissions.  User's $HOME directory "
				 "must be owned by user and not writable "
				 "by other users."));
		usrsess = g_strdup ("");
		usrlang = g_strdup ("");
	}

	mdm_debug ("mdm_slave_session_start: .dmrc SESSION == '%s'", usrsess);

	if (! is_session_valid (usrsess) ) {
		g_free (usrsess);
		usrsess = find_a_session ();
	}

	NEVER_FAILS_root_set_euid_egid (0, mdm_daemon_config_get_mdmgid ());

	if (greet) {
		tmp = mdm_ensure_extension (usrsess, ".desktop");
		char * greeter_session = mdm_slave_greeter_ctl (MDM_SESS, tmp);
		if (is_session_valid(greeter_session) && strcmp(greeter_session, "default.desktop") != 0) {
			// If the greeter chooses a valid session (not default.desktop.. in that case we use the session we detected already)
			session = g_strdup (greeter_session);
			mdm_debug ("mdm_slave_session_start: Greeter set SESSION to '%s'", session);
		}
		else {
			session = g_strdup (usrsess);
		}
		g_free (tmp);
		g_free (greeter_session);

		if (session != NULL &&
		    strcmp (session, MDM_RESPONSE_CANCEL) == 0) {
			mdm_debug ("User canceled login");
			mdm_verify_cleanup (d);
			session_started = FALSE;
			g_free (usrlang);
			return;
		}

		language = mdm_slave_greeter_ctl (MDM_LANG, usrlang);
		if (language != NULL &&
		    strcmp (language, MDM_RESPONSE_CANCEL) == 0) {
			mdm_debug ("User canceled login");
			mdm_verify_cleanup (d);
			session_started = FALSE;
			g_free (usrlang);
			return;
		}
	} else {
		session = g_strdup (usrsess);
		language = g_strdup (usrlang);
	}

	if (ve_string_empty(session)) {
		g_free (session);
		mdm_debug ("Session is NULL, setting it to default.desktop");
		session = g_strdup ("default.desktop");
	}

	tmp = mdm_strip_extension (session, ".desktop");
	g_free (session);
	session = tmp;

	if G_LIKELY (ve_string_empty (language)) {
		g_free (language);
		language = NULL;
	}

	g_free (usrsess);

	mdm_debug ("Initial setting: session: '%s' language: '%s'\n", session, ve_sure_string (language));

	/* save this session as the users session */
	save_session = g_strdup (session);

	if (greet) {
		savesess = TRUE;
		char *ret = mdm_slave_greeter_ctl (MDM_SLANG, "");
		if ( ! ve_string_empty (ret))
			savelang = TRUE;
		g_free (ret);

		mdm_debug ("mdm_slave_session_start: Authentication completed. Whacking greeter");
		mdm_slave_whack_greeter ();
	}

	if (mdm_daemon_config_get_value_bool (MDM_KEY_KILL_INIT_CLIENTS))
		mdm_server_whack_clients (d->dsp);

	/*
	 * If the desktop file specifies that there are special Xserver
	 * arguments to use, then restart the Xserver with them.
	 */
	d->xserver_session_args = mdm_daemon_config_get_session_xserver_args (session);
	if (d->xserver_session_args) {
		mdm_server_stop (d);
		mdm_slave_send_num (MDM_SOP_XPID, 0);
		mdm_server_start (d, TRUE, FALSE, 20, 5);
		mdm_slave_send_num (MDM_SOP_XPID, d->servpid);
		g_free (d->xserver_session_args);
		d->xserver_session_args = NULL;
	}

	/* Now that we will set up the user authorization we will
	   need to run session_stop to whack it */
	session_started = TRUE;

	/* Setup cookie -- We need this information during cleanup, thus
	 * cookie handling is done before fork()ing */

	if G_UNLIKELY (setegid (pwent->pw_gid) != 0 ||
		       seteuid (pwent->pw_uid) != 0) {
		mdm_error ("Cannot set effective user/group id");
		mdm_slave_quick_exit (DISPLAY_REMANAGE);
	}

	authok = mdm_auth_user_add (d, pwent->pw_uid,
				    /* Only pass the home_dir if
				     * it was ok */
				    home_dir_ok ? home_dir : NULL);

	/* FIXME: this should be smarter and only do this on out-of-diskspace
	 * errors */
	if G_UNLIKELY ( ! authok && home_dir_ok) {
		/* try wiping the .xsession-errors file (and perhaps other things)
		   in an attempt to gain disk space */
		if (wipe_xsession_errors (pwent, home_dir, home_dir_ok)) {
			mdm_error ("Tried wiping some old user session errors files "
				   "to make disk space and will try adding user auth "
				   "files again");
			/* Try again */
			authok = mdm_auth_user_add (d, pwent->pw_uid,
						    /* Only pass the home_dir if
						     * it was ok */
						    home_dir_ok ? home_dir : NULL);
		}
	}

	NEVER_FAILS_root_set_euid_egid (0, mdm_daemon_config_get_mdmgid ());

	if G_UNLIKELY ( ! authok) {
		mdm_debug ("mdm_slave_session_start: Auth not OK");

		mdm_errorgui_error_box (d,
			       GTK_MESSAGE_ERROR,
			       _("MDM could not write to your authorization "
				 "file.  This could mean that you are out of "
				 "disk space or that your home directory could "
				 "not be opened for writing.  In any case, it "
				 "is not possible to log in.  Please contact "
				 "your system administrator"));

		mdm_slave_session_stop (FALSE /* run_post_session */,
					FALSE /* no_shutdown_check */);

		mdm_slave_quick_exit (DISPLAY_REMANAGE);
	}

	/* Write out the Xservers file */
	mdm_slave_send_num (MDM_SOP_WRITE_X_SERVERS, 0 /* bogus */);

	if G_LIKELY (d->dsp != NULL) {
		Cursor xcursor;

		XSetInputFocus (d->dsp, PointerRoot,
				RevertToPointerRoot, CurrentTime);

		/* return left pointer */
		xcursor = XCreateFontCursor (d->dsp, GDK_LEFT_PTR);
		XDefineCursor (d->dsp,
			       DefaultRootWindow (d->dsp),
			       xcursor);
		XFreeCursor (d->dsp, xcursor);
		XSync (d->dsp, False);
	}

	/* Init the ~/.xsession-errors stuff */
	d->xsession_errors_bytes = 0;
	d->xsession_errors_fd = -1;
	d->session_output_fd = -1;

	logfilefd = open_xsession_errors (pwent,
					  home_dir,
					  home_dir_ok);
	if G_UNLIKELY (logfilefd < 0 ||
		       pipe (logpipe) != 0) {
		if (logfilefd >= 0)
			VE_IGNORE_EINTR (close (logfilefd));
		logfilefd = -1;
	}

	/* don't completely rely on this, the user
	 * could reset time or do other crazy things */
	session_start_time = time (NULL);

#ifdef WITH_CONSOLE_KIT
	ck_session_cookie = open_ck_session (pwent, d, session);
#endif

	mdm_debug ("Forking user session %s", session);
	
	/* Start user process */
	mdm_sigchld_block_push ();
	mdm_sigterm_block_push ();
	pid = d->sesspid = fork ();
	if (pid == 0)
		mdm_unset_signals ();
	mdm_sigterm_block_pop ();
	mdm_sigchld_block_pop ();

	switch (pid) {

	case -1:
		mdm_slave_exit (DISPLAY_REMANAGE, _("%s: Error forking user session"), "mdm_slave_session_start");

	case 0:
		{
			const char *lang;
			gboolean    has_language;

			has_language = (language != NULL) && (language[0] != '\0');

			if ((mdm_system_locale != NULL) && (!has_language)) {
				lang = mdm_system_locale;
			} else {
				lang = language;
			}

			if G_LIKELY (logfilefd >= 0) {
				VE_IGNORE_EINTR (close (logpipe[0]));
			}
			/* Never returns */
			session_child_run (pwent,
					   logpipe[1],
					   home_dir,
					   home_dir_ok,
#ifdef WITH_CONSOLE_KIT
					   ck_session_cookie,
#endif
					   session,
					   save_session,
					   lang,
					   gnome_session,
					   usrcfgok,
					   savesess,
					   savelang);
			g_assert_not_reached ();
		}

	default:
		always_restart_greeter = FALSE;
		if (!savelang && language && strcmp (usrlang, language)) {
			if (mdm_system_locale != NULL) {
				g_setenv ("LANG", mdm_system_locale, TRUE);
				setlocale (LC_ALL, "");
				g_unsetenv ("MDM_LANG");
				/* for "MDM_LANG" */
				mdm_clearenv_no_lang ();
				mdm_saveenv ();
			}
			mdm_slave_greeter_ctl_no_ret (MDM_SETLANG, DEFAULT_LANGUAGE);
		}
		break;
	}

	/* this clears internal cache */
	mdm_daemon_config_get_session_exec (NULL, FALSE);

	if G_LIKELY (logfilefd >= 0)  {
		d->xsession_errors_fd = logfilefd;
		d->session_output_fd = logpipe[0];
		/* make the output read fd non-blocking */
		fcntl (d->session_output_fd, F_SETFL, O_NONBLOCK);
		VE_IGNORE_EINTR (close (logpipe[1]));
	}

	/* We must be root for this, and we are, but just to make sure */
	NEVER_FAILS_root_set_euid_egid (0, mdm_daemon_config_get_mdmgid ());
	/* Reset all the process limits, pam may have set some up for our process and that
	   is quite evil.  But pam is generally evil, so this is to be expected. */
	mdm_reset_limits ();

	g_free (session);
	g_free (save_session);
	g_free (language);
	g_free (gnome_session);

	mdm_slave_write_utmp_wtmp_record (d,
				MDM_SESSION_RECORD_TYPE_LOGIN,
				pwent->pw_name,
				pid);

	mdm_slave_send_num (MDM_SOP_SESSPID, pid);

	mdm_sigchld_block_push ();
	wp = slave_waitpid_setpid (d->sesspid);
	mdm_sigchld_block_pop ();

	slave_waitpid (wp);

	d->sesspid = 0;

	/* finish reading the session output if any of it is still there */
	finish_session_output (TRUE);

	/* Now still as root make the system authfile readable by others,
	   and therefore by the mdm user */
	VE_IGNORE_EINTR (g_chmod (MDM_AUTHFILE (d), 0644));

	end_time = time (NULL);

	mdm_debug ("Session: start_time: %ld end_time: %ld",
		   (long)session_start_time, (long)end_time);

	/* Sync to get notified in the case the X server died
	 */
	XSync (d->dsp, False);

	/* 66 is a very magical number signifying failure in MDM */
	if G_UNLIKELY ((d->last_sess_status != 66) &&
		       (/* sanity */ end_time >= session_start_time) &&
		       (end_time - 10 <= session_start_time) &&
		       /* only if the X server still exist! */
		       d->servpid > 1) {
		char *msg_string;
		char *error_msg =
			_("Your session only lasted less than "
			  "10 seconds.  If you have not logged out "
			  "yourself, this could mean that there is "
			  "some installation problem or that you may "
			  "be out of diskspace.  Try logging in with "
			  "one of the failsafe sessions to see if you "
			  "can fix this problem.");

		/* FIXME: perhaps do some checking to display a better error,
		 * such as cinnamon-session missing and such things. */
		mdm_debug ("Session less than 10 seconds!");
		msg_string = g_strdup_printf ("type=%d$$error_msg=%s$$details_label=%s$$details_file=%s$$uid=%d$$gid=%d",
					      GTK_MESSAGE_WARNING,error_msg, 
					      (d->xsession_errors_filename != NULL) ?
					      _("View details (~/.xsession-errors file)") :
					      NULL,
					      d->xsession_errors_filename,
					      0, 0);

		mdm_slave_send_string (MDM_SOP_SHOW_ERROR_DIALOG, msg_string);

		g_free (msg_string);

	}

#ifdef WITH_CONSOLE_KIT
	if (ck_session_cookie != NULL) {
		close_ck_session (ck_session_cookie);
		g_free (ck_session_cookie);
	}
#endif

	if ((pid != 0) && (d->last_sess_status != -1)) {
		mdm_debug ("session '%d' exited with status '%d', recording logout",
		pid, d->last_sess_status);
		mdm_slave_write_utmp_wtmp_record (d,
					MDM_SESSION_RECORD_TYPE_LOGOUT,
					pwent->pw_name,
					pid);
	}

	mdm_slave_session_stop (pid != 0 /* run_post_session */,
				FALSE /* no_shutdown_check */);

	mdm_debug ("mdm_slave_session_start: Session ended OK (now all finished)");

}


/* Stop any in progress sessions */
static void
mdm_slave_session_stop (gboolean run_post_session,
			gboolean no_shutdown_check)
{
	struct passwd *pwent;
	char *x_servers_file;
	char *local_login;

	in_session_stop++;

	session_started = FALSE;

	local_login = login_user;
	login_user  = NULL;

	/* don't use NEVER_FAILS_ here this can be called from places
	   kind of exiting and it's ok if this doesn't work (when shouldn't
	   it work anyway? */
	seteuid (0);
	setegid (0);

	mdm_slave_send_num (MDM_SOP_SESSPID, 0);

	/* Now still as root make the system authfile not readable by others,
	   and therefore not by the mdm user */
	if (MDM_AUTHFILE (d) != NULL) {
		VE_IGNORE_EINTR (g_chmod (MDM_AUTHFILE (d), 0640));
	}

	mdm_debug ("mdm_slave_session_stop: %s on %s", local_login, d->name);

	/* Note we use the info in the structure here since if we get passed
	 * a 0 that means the process is already dead.
	 * FIXME: Maybe we should waitpid here, note make sure this will
	 * not create a hang! */
	mdm_sigchld_block_push ();
	if (d->sesspid > 1)
		kill (- (d->sesspid), SIGTERM);
	mdm_sigchld_block_pop ();

	finish_session_output (run_post_session /* do_read */);

	if (local_login == NULL)
		pwent = NULL;
	else
		pwent = getpwnam (local_login);	/* PAM overwrites our pwent */

	x_servers_file = mdm_make_filename (mdm_daemon_config_get_value_string (MDM_KEY_SERV_AUTHDIR),
					    d->name, ".Xservers");

	/* if there was a session that ran, run the PostSession script */
	if (run_post_session) {
		/* Execute post session script */
		mdm_debug ("mdm_slave_session_stop: Running post session script");
		mdm_slave_exec_script (d, mdm_daemon_config_get_value_string (MDM_KEY_POSTSESSION), local_login, pwent,
				       FALSE /* pass_stdout */);
	}

	VE_IGNORE_EINTR (g_unlink (x_servers_file));
	g_free (x_servers_file);

	g_free (local_login);

	if (pwent != NULL) {
		seteuid (0); /* paranoia */
		/* Remove display from ~user/.Xauthority */
		if G_LIKELY (setegid (pwent->pw_gid) == 0 &&
			     seteuid (pwent->pw_uid) == 0) {
			mdm_auth_user_remove (d, pwent->pw_uid);
		}

		/* don't use NEVER_FAILS_ here this can be called from places
		   kind of exiting and it's ok if this doesn't work (when shouldn't
		   it work anyway? */
		seteuid (0);
		setegid (0);
	}

	logged_in_uid = -1;
	logged_in_gid = -1;

	/* things are going to be killed, so ignore errors */
	XSetErrorHandler (ignore_xerror_handler);

	mdm_verify_cleanup (d);
	mdm_slave_quick_exit (DISPLAY_REMANAGE); // Clem. Fix for 2nd login in Petra - Force slave to exit after logout

	in_session_stop --;

	if (need_to_quit_after_session_stop) {
		mdm_debug ("mdm_slave_session_stop: Final cleanup");

		mdm_slave_quick_exit (exit_code_to_use);
	}

#ifdef __linux__
	/* If on Linux and the runlevel is 0 or 6 and not the runlevel that
	   we were started in, then we are restarting or halting the machine.
	   Probably the user selected halt or restart from the logout
	   menu.  In this case we can really just sleep for a few seconds and
	   basically wait to be killed.  I will set the default for 30 seconds
	   and let people yell at me if this breaks something.  It shouldn't.
	   In fact it should fix things so that the login screen is not brought
	   up again and then whacked.  Waiting is safer then DISPLAY_ABORT,
	   since if we really do get this wrong, then at the worst case the
	   user will wait for a few moments. */
	if ( ! need_to_quit_after_session_stop &&
	     ! no_shutdown_check &&
	     g_access ("/sbin/runlevel", X_OK) == 0) {
		int rnl = get_runlevel ();
		if ((rnl == 0 || rnl == 6) && rnl != mdm_normal_runlevel) {
			/* this is a stupid loop, but we may be getting signals,
			   so we don't want to just do sleep (30) */
			time_t c = time (NULL);
			mdm_info ("MDM detected a halt or restart in progress.");
			while (c + 30 >= time (NULL)) {
				struct timeval tv;
				/* Wait 30 seconds. */
				tv.tv_sec = 30;
				tv.tv_usec = 0;
				select (0, NULL, NULL, NULL, &tv);
				
			}
			/* hmm, didn't get TERM, weird */
		}
	}
#endif /* __linux__ */
}

static void
mdm_slave_term_handler (int sig)
{
	static gboolean got_term_before = FALSE;

	mdm_in_signal++;
	mdm_wait_for_ack = FALSE;

	exit_code_to_use = DISPLAY_ABORT;
	need_to_quit_after_session_stop = TRUE;

	if (already_in_slave_start_jmp ||
	    (got_term_before && in_session_stop > 0)) {
		mdm_sigchld_block_push ();
		/* be very very very nasty to the extra process if the user is really
		   trying to get rid of us */
		if (extra_process > 1)
			kill (-(extra_process), SIGKILL);
		/* also be very nasty to the X server at this stage */
		if (d->servpid > 1)
			kill (d->servpid, SIGKILL);
		mdm_sigchld_block_pop ();
		mdm_in_signal--;
		got_term_before = TRUE;
		/* we're already quitting, just a matter of killing all the processes */
		return;
	}
	got_term_before = TRUE;

	/* just in case this was set to something else, like during
	 * server reinit */
	XSetIOErrorHandler (mdm_slave_xioerror_handler);

	if (in_session_stop > 0) {
		/* the need_to_quit_after_session_stop is now set so things will
		   work out right */
		mdm_in_signal--;
		return;
	}

	if (session_started) {
		SIGNAL_EXIT_WITH_JMP (d, JMP_SESSION_STOP_AND_QUIT);
	} else {
		SIGNAL_EXIT_WITH_JMP (d, JMP_JUST_QUIT_QUICKLY);
	}

	/* never reached */
	mdm_in_signal--;
}

/* Called on every SIGCHLD */
void
mdm_slave_child_handler (int sig)
{

	mdm_debug("mdm_slave_child_handler: Handling children...");

	gint status;
	pid_t pid;
	uid_t old;

	if G_UNLIKELY (already_in_slave_start_jmp)
		return;

	mdm_in_signal++;

	old = geteuid ();
	if (old != 0)
		seteuid (0);

	while ((pid = waitpid (-1, &status, WNOHANG)) > 0) {
		GSList *li;

		for (li = slave_waitpids; li != NULL; li = li->next) {
			MdmWaitPid *wp = li->data;
			if (wp->pid == pid) {
				wp->pid = -1;
				if (slave_waitpid_w >= 0) {
					VE_IGNORE_EINTR (write (slave_waitpid_w, "!", 1));
				}
			}
		}

		if (pid == d->greetpid && greet) {
			if (WIFEXITED (status) &&
			    WEXITSTATUS (status) == DISPLAY_RESTARTGREETER) {
				/* FIXME: shouldn't do this from
				   a signal handler */
				/*mdm_slave_desensitize_config ();*/

				greet = FALSE;
				d->greetpid = 0;
				whack_greeter_fds ();
				mdm_slave_send_num (MDM_SOP_GREETPID, 0);

				do_restart_greeter = TRUE;
				if (restart_greeter_now) {
					slave_waitpid_notify ();
				} else {
					interrupted = TRUE;
				}
				continue;
			}

			whack_greeter_fds ();

			/* if greet is TRUE, then the greeter died outside of our
			 * control really, so clean up and die, something is wrong
			 * The greeter is only allowed to pass back these
			 * exit codes, else we'll just remanage */
			if (WIFEXITED (status) &&
			    (WEXITSTATUS (status) == DISPLAY_ABORT ||
			     WEXITSTATUS (status) == DISPLAY_REBOOT ||
			     WEXITSTATUS (status) == DISPLAY_HALT ||
			     WEXITSTATUS (status) == DISPLAY_SUSPEND ||
			     WEXITSTATUS (status) == DISPLAY_RESTARTMDM ||
			     WEXITSTATUS (status) == DISPLAY_GREETERFAILED)) {
				exit_code_to_use = WEXITSTATUS (status);
				SIGNAL_EXIT_WITH_JMP (d, JMP_JUST_QUIT_QUICKLY);
			} else {
				if (WIFSIGNALED (status) &&
				    (WTERMSIG (status) == SIGSEGV ||
				     WTERMSIG (status) == SIGABRT ||
				     WTERMSIG (status) == SIGPIPE ||
				     WTERMSIG (status) == SIGBUS)) {
					exit_code_to_use = DISPLAY_GREETERFAILED;
					SIGNAL_EXIT_WITH_JMP (d, JMP_JUST_QUIT_QUICKLY);
				} else {
					/* weird error return, interpret as failure */
					if (WIFEXITED (status) &&
					    WEXITSTATUS (status) == 1)
						exit_code_to_use = DISPLAY_GREETERFAILED;
					SIGNAL_EXIT_WITH_JMP (d, JMP_JUST_QUIT_QUICKLY);
				}
			}
		} else if (pid != 0 && pid == d->sesspid) {
			d->sesspid = 0;
			if (WIFEXITED (status))
				d->last_sess_status = WEXITSTATUS (status);
			else
				d->last_sess_status = -1;
		} else if (pid != 0 && pid == d->servpid) {
			if (d->servstat == SERVER_RUNNING)
				mdm_server_whack_lockfile (d);
			d->servstat = SERVER_DEAD;
			d->servpid = 0;
			mdm_server_wipe_cookies (d);

			mdm_slave_send_num (MDM_SOP_XPID, 0);

			/* whack the session good */
			if (d->sesspid > 1) {
				mdm_slave_send_num (MDM_SOP_SESSPID, 0);
				kill (- (d->sesspid), SIGTERM);
			}
			if (d->greetpid > 1) {
				mdm_slave_send_num (MDM_SOP_GREETPID, 0);
				kill (d->greetpid, SIGTERM);
			}			

			/* just in case we restart again wait at least
			   one sec to avoid races */
			if (d->sleep_before_run < 1)
				d->sleep_before_run = 1;
		} else if (pid == extra_process) {
			/* an extra process died, yay! */
			extra_process = 0;
			extra_status = status;
		}
	}
	if (old != 0)
		seteuid (old);

	mdm_in_signal--;
}

static void
mdm_slave_handle_usr2_message (void)
{
	char buf[256];
	ssize_t count;
	char **vec;
	int i;

	VE_IGNORE_EINTR (count = read (d->slave_notify_fd, buf, sizeof (buf) -1));
	if (count <= 0) {
		return;
	}

	buf[count] = '\0';

	vec = g_strsplit (buf, "\n", -1);
	if (vec == NULL) {
		return;
	}

	for (i = 0; vec[i] != NULL; i++) {
		char *s = vec[i];
		if (s[0] == MDM_SLAVE_NOTIFY_ACK) {
			mdm_got_ack = TRUE;
			g_free (mdm_ack_response);
			if (s[1] != '\0')
				mdm_ack_response = g_strdup (&s[1]);
			else
				mdm_ack_response = NULL;
		} else if (s[0] == MDM_SLAVE_NOTIFY_KEY) {
			slave_waitpid_notify ();
			unhandled_notifies =
				g_list_append (unhandled_notifies,
					       g_strdup (&s[1]));
		} else if (s[0] == MDM_SLAVE_NOTIFY_COMMAND) {
			if (strcmp (&s[1], MDM_NOTIFY_DIRTY_SERVERS) == 0) {
				/* never restart flexi servers
				 * they whack themselves */
				if (!SERVER_IS_FLEXI (d))
					remanage_asap = TRUE;
			} else if (strcmp (&s[1], MDM_NOTIFY_SOFT_RESTART_SERVERS) == 0) {
				/* never restart flexi servers,
				 * they whack themselves */
				/* FIXME: here we should handle actual
				 * restarts of flexi servers, but it probably
				 * doesn't matter */
				if (!SERVER_IS_FLEXI (d)) {
					if ( ! d->logged_in) {
						if (mdm_in_signal > 0) {
							exit_code_to_use = DISPLAY_REMANAGE;
							SIGNAL_EXIT_WITH_JMP (d, JMP_JUST_QUIT_QUICKLY);
						} else {
							/* FIXME: are we ever not in signal here? */
							mdm_slave_quick_exit (DISPLAY_REMANAGE);
						}
					} else {
						remanage_asap = TRUE;
					}
				}
			} else if (strcmp (&s[1], MDM_NOTIFY_GO) == 0) {
				mdm_wait_for_go = FALSE;
			} else if (strcmp (&s[1], MDM_NOTIFY_TWIDDLE_POINTER) == 0) {
				mdm_twiddle_pointer (d);
			}
		} else if (s[0] == MDM_SLAVE_NOTIFY_RESPONSE) {
			mdm_got_ack = TRUE;
			if (mdm_ack_response)
				g_free (mdm_ack_response);

			if (s[1] == MDM_SLAVE_NOTIFY_YESNO_RESPONSE) {
				if (s[2] == '0') {
					mdm_ack_response =  g_strdup ("no");
				} else {
					mdm_ack_response =  g_strdup ("yes");
				}
			} else if (s[1] == MDM_SLAVE_NOTIFY_ASKBUTTONS_RESPONSE) {
				mdm_ack_response = g_strdup (&s[2]);
			} else if (s[1] == MDM_SLAVE_NOTIFY_QUESTION_RESPONSE) {
				mdm_ack_question_response = g_strdup (&s[2]);
			} else if (s[1] == MDM_SLAVE_NOTIFY_ERROR_RESPONSE) {
				if (s[2] != '\0') {
					mdm_ack_response = g_strdup (&s[2]);
				} else {
					mdm_ack_response = NULL;
				}
			}
		}
	}

	g_strfreev (vec);
}

static void
mdm_slave_usr2_handler (int sig)
{
	mdm_in_signal++;
	in_usr2_signal++;

	mdm_slave_handle_usr2_message ();

	in_usr2_signal--;
	mdm_in_signal--;
}

/* Minor X faults */
static gint
mdm_slave_xerror_handler (Display *disp, XErrorEvent *evt)
{
	mdm_debug ("mdm_slave_xerror_handler: X error - display doesn't respond");
	return (0);
}

/* We usually respond to fatal errors by restarting the display */
static gint
mdm_slave_xioerror_handler (Display *disp)
{
	if (already_in_slave_start_jmp) {
		/* eki eki eki, this is not good,
		   should only happen if we get some io error after
		   we have gotten a SIGTERM */
		SIGNAL_EXIT_WITH_JMP (d, JMP_JUST_QUIT_QUICKLY);
	}

	mdm_in_signal++;

	/* Display is all gone */
	d->dsp = NULL;

	if ((d->type == TYPE_STATIC ||
	     d->type == TYPE_FLEXI) &&
	    (do_xfailed_on_xio_error ||
	     d->starttime + 5 >= time (NULL))) {
		exit_code_to_use = DISPLAY_XFAILED;
	} else {
		exit_code_to_use = DISPLAY_REMANAGE;
	}

	slave_start_jmp_error_to_print =
		g_strdup_printf (_("%s: Fatal X error - Restarting %s"), 
				 "mdm_slave_xioerror_handler", d->name);

	need_to_quit_after_session_stop = TRUE;

	if (session_started) {
		SIGNAL_EXIT_WITH_JMP (d, JMP_SESSION_STOP_AND_QUIT);
	} else {
		SIGNAL_EXIT_WITH_JMP (d, JMP_JUST_QUIT_QUICKLY);
	}

	/* never reached */
	mdm_in_signal--;

	return 0;
}

/* return true for "there was an interruption received",
   and interrupted will be TRUE if we are actually interrupted from doing what
   we want.  If FALSE is returned, just continue on as we would normally */
static gboolean
check_for_interruption (const char *msg)
{
	/* Hell yeah we were interrupted, the greeter died */
	if (msg == NULL) {
		interrupted = TRUE;
		return TRUE;
	}

	if (msg[0] == BEL) {
		/* Different interruptions come here */
		/* Note that we don't want to actually do anything.  We want
		 * to just set some flag and go on and schedule it after we
		 * dump out of the login in the main login checking loop */
		switch (msg[1]) {
		case MDM_INTERRUPT_TIMED_LOGIN:
			/* only allow timed login if display is local,
			 * it is allowed for this display (it's only allowed
			 * for the first local display) and if it's set up
			 * correctly */
			if (d->attached && d->timed_login_ok &&
			    ! ve_string_empty (ParsedTimedLogin) &&
                            strcmp (ParsedTimedLogin, mdm_root_user ()) != 0 &&
			    mdm_daemon_config_get_value_int (MDM_KEY_TIMED_LOGIN_DELAY) > 0) {
				do_timed_login = TRUE;
			}
			break;
		case MDM_INTERRUPT_CONFIGURE:
			if (d->attached &&
			    mdm_daemon_config_get_value_bool_per_display (MDM_KEY_CONFIG_AVAILABLE, d->name) &&
			    mdm_daemon_config_get_value_bool_per_display (MDM_KEY_SYSTEM_MENU, d->name) &&
			    ! ve_string_empty (mdm_daemon_config_get_value_string (MDM_KEY_CONFIGURATOR))) {
				do_configurator = TRUE;
			}
			break;
		case MDM_INTERRUPT_SUSPEND:
			if (d->attached &&
			    mdm_daemon_config_get_value_bool_per_display (MDM_KEY_SYSTEM_MENU, d->name) &&
			    ! ve_string_empty (mdm_daemon_config_get_value_string_array (MDM_KEY_SUSPEND))) {
			    	gchar *msg = g_strdup_printf ("%s %ld", 
							      MDM_SOP_SUSPEND_MACHINE,
							      (long)getpid ());

				mdm_slave_send (msg, FALSE /* wait_for_ack */);
				g_free (msg);
			}
			/* Not interrupted, continue reading input,
			 * just proxy this to the master server */
			return TRUE;
		case MDM_INTERRUPT_LOGIN_SOUND:
			if (d->attached &&
			    ! play_login_sound (mdm_daemon_config_get_value_string (MDM_KEY_SOUND_ON_LOGIN_FILE))) {
				mdm_error ("Login sound requested on non-local display or the play software cannot be run or the sound does not exist");
			}
			return TRUE;
		case MDM_INTERRUPT_SELECT_USER:
			mdm_verify_select_user (&msg[2]);
			break;
		case MDM_INTERRUPT_CANCEL:
			do_cancel = TRUE;
			break;		
		case MDM_INTERRUPT_THEME:
			g_free (d->theme_name);
			d->theme_name = NULL;
			if ( ! ve_string_empty (&msg[2]))
				d->theme_name = g_strdup (&msg[2]);
			mdm_slave_send_string (MDM_SOP_CHOSEN_THEME, &msg[2]);
			return TRUE;
		case MDM_INTERRUPT_SELECT_LANG:
			if (msg + 2) {
				const char *locale;

				locale = (gchar*)(msg + 3);

				always_restart_greeter = (gboolean)(*(msg + 2));
				ve_clearenv ();
				if (!strcmp (locale, DEFAULT_LANGUAGE)) {
					locale = mdm_system_locale;
				}
				/*
				 * Do not lose DISPLAY.  It should always be
				 * available for use, PAM modules use it for
				 * example.
				 */
				g_setenv ("DISPLAY", d->name, TRUE);
				g_setenv ("MDM_LANG", locale, TRUE);
				g_setenv ("LANG", locale, TRUE);
				g_unsetenv ("LC_ALL");
				g_unsetenv ("LC_MESSAGES");
				setlocale (LC_ALL, "");
				setlocale (LC_MESSAGES, "");
				mdm_saveenv ();

				do_restart_greeter = TRUE;
			}
			break;
		default:
			break;
		}

		/* this was an interruption, if it wasn't
		 * handled then the user will just get an error as if he
		 * entered an invalid login or passward.  Seriously BEL
		 * cannot be part of a login/password really */
		interrupted = TRUE;
		return TRUE;
	}
	return FALSE;
}


char *
mdm_slave_greeter_ctl (char cmd, const char *str)
{
	char *buf = NULL;
	int c;

	/* There is no spoon^H^H^H^H^Hgreeter */
	if G_UNLIKELY ( ! greet)
		return NULL;

	check_notifies_now ();

	if ( ! ve_string_empty (str)) {
		mdm_fdprintf (greeter_fd_out, "%c%c%s\n", STX, cmd, str);
	} else {
		mdm_fdprintf (greeter_fd_out, "%c%c\n", STX, cmd);
	}

#if defined (_POSIX_PRIORITY_SCHEDULING) && defined (HAVE_SCHED_YIELD)
	/* let the other process (greeter) do its stuff */
	sched_yield ();
#endif

	do {
		g_free (buf);
		buf = NULL;
		/* Skip random junk that might have accumulated */
		do {
			c = mdm_fdgetc (greeter_fd_in);
		} while (c != EOF && c != STX);

		if (c == EOF ||
		    (buf = mdm_fdgets (greeter_fd_in)) == NULL) {
			interrupted = TRUE;
			/* things don't seem well with the greeter, it probably died */
			return NULL;
		}
	} while (check_for_interruption (buf) && ! interrupted);

	/* user responses take kind of random amount of time */
	mdm_random_tick ();

	if ( ! ve_string_empty (buf)) {
		return buf;
	} else {
		g_free (buf);
		return NULL;
	}
}

void
mdm_slave_greeter_ctl_no_ret (char cmd, const char *str)
{
	g_free (mdm_slave_greeter_ctl (cmd, str));
}

static void
mdm_slave_quick_exit (gint status)
{
	/* just for paranoia's sake */
	/* don't use NEVER_FAILS_ here this can be called from places
	   kind of exiting and it's ok if this doesn't work (when shouldn't
	   it work anyway? */
	seteuid (0);
	setegid (0);

	if (d != NULL) {
		mdm_debug ("mdm_slave_quick_exit: Will kill everything from the display");

		/* just in case we do get the XIOError,
		   don't run session_stop since we've
		   requested a quick exit */
		session_started = FALSE;

		/* No need to send the PIDS to the daemon
		 * since we'll just exit cleanly */

		/* Push and never pop */
		mdm_sigchld_block_push ();

		/* Kill children where applicable */
		if (d->greetpid > 1)
			kill (d->greetpid, SIGTERM);
		d->greetpid = 0;		

		if (d->sesspid > 1)
			kill (-(d->sesspid), SIGTERM);
		d->sesspid = 0;

		if (extra_process > 1)
			kill (-(extra_process), SIGTERM);
		extra_process = 0;

		mdm_verify_cleanup (d);
		mdm_server_stop (d);

		if (d->servpid > 1)
			kill (d->servpid, SIGTERM);
		d->servpid = 0;

		mdm_debug ("mdm_slave_quick_exit: Killed everything from the display");
	}

	_exit (status);
}

static void
mdm_slave_exit (gint status, const gchar *format, ...)
{
	va_list args;
	gchar *s;

	va_start (args, format);
	s = g_strdup_vprintf (format, args);
	va_end (args);

	mdm_error ("%s", s);

	g_free (s);

	mdm_slave_quick_exit (status);
}

static void
mdm_child_exit (gint status, const gchar *format, ...)
{
	va_list args;
	gchar *s;

	va_start (args, format);
	s = g_strdup_vprintf (format, args);
	va_end (args);

	g_error ("%s", s);

	g_free (s);

	_exit (status);
}

static gint
mdm_slave_exec_script (MdmDisplay *d,
		       const gchar *dir,
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
	char *x_servers_file;

	if G_UNLIKELY (!d || ve_string_empty (dir))
		return EXIT_SUCCESS;

	script = g_build_filename (dir, d->name, NULL);
	if (g_access (script, R_OK|X_OK) != 0) {
		g_free (script);
		script = NULL;
	}
	if (script == NULL &&
	    ! ve_string_empty (d->hostname)) {
		script = g_build_filename (dir, d->hostname, NULL);
		if (g_access (script, R_OK|X_OK) != 0) {
			g_free (script);
			script = NULL;
		}
	}	
	if (script == NULL &&
	    SERVER_IS_FLEXI (d)) {
		script = g_build_filename (dir, "Flexi", NULL);
		if (g_access (script, R_OK|X_OK) != 0) {
			g_free (script);
			script = NULL;
		}
	}
	if (script == NULL) {
		script = g_build_filename (dir, "Default", NULL);
		if (g_access (script, R_OK|X_OK) != 0) {
			g_free (script);
			script = NULL;
		}
	}

	if (script == NULL) {
		return EXIT_SUCCESS;
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

	extra_process = pid = mdm_fork_extra ();

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

		/* some env for use with the Pre and Post scripts */
		x_servers_file = mdm_make_filename (mdm_daemon_config_get_value_string (MDM_KEY_SERV_AUTHDIR),
						    d->name, ".Xservers");
		g_setenv ("X_SERVERS", x_servers_file, TRUE);
		g_free (x_servers_file);		

		/* Runs as root */
		if (MDM_AUTHFILE (d) != NULL)
			g_setenv ("XAUTHORITY", MDM_AUTHFILE (d), TRUE);
		else
			g_unsetenv ("XAUTHORITY");
		g_setenv ("DISPLAY", d->name, TRUE);
		if (d->windowpath)
			g_setenv ("WINDOWPATH", d->windowpath, TRUE);
		g_setenv ("PATH", mdm_daemon_config_get_value_string (MDM_KEY_ROOT_PATH), TRUE);
		g_setenv ("RUNNING_UNDER_MDM", "true", TRUE);
		if ( ! ve_string_empty (d->theme_name))
			g_setenv ("MDM_GTK_THEME", d->theme_name, TRUE);

		g_shell_parse_argv (script, NULL, &argv, NULL);

		VE_IGNORE_EINTR (execv (argv[0], argv));
		g_strfreev (argv);
		g_error (_("%s: Failed starting: %s"),
			 "mdm_slave_exec_script",
			 script);
		_exit (EXIT_SUCCESS);

	case -1:
		g_free (script);
		g_error (_("%s: Can't fork script process!"), "mdm_slave_exec_script");

		setgid (save_gid);
		setegid (save_egid);

		return EXIT_SUCCESS;

	default:
		mdm_wait_for_extra (extra_process, &status);
		g_free (script);

		setgid (save_gid);
		setegid (save_egid);

		if (WIFEXITED (status))
			return WEXITSTATUS (status);
		else
			return EXIT_SUCCESS;
	}
}

gboolean
mdm_slave_greeter_check_interruption (void)
{
	if (interrupted) {
		/* no longer interrupted */
		interrupted = FALSE;
		return TRUE;
	} else {
		return FALSE;
	}
}

gboolean
mdm_slave_action_pending (void)
{
	if (do_timed_login ||
	    do_configurator ||
	    do_restart_greeter ||
	    do_cancel)
		return FALSE;
	return TRUE;
}

/* The user name for automatic/timed login may be parameterized by
   host/display. */

static gchar *
mdm_slave_parse_enriched_login (MdmDisplay *d, const gchar *s)
{
	GString *str;
	gchar in_buffer[20];
	gint pipe1[2], in_buffer_len;
	gchar **argv = NULL;
	pid_t pid;

	if (s == NULL)
		return (NULL);

	str = mdm_slave_parse_enriched_string (d, s);

	/* Sometimes it is not convenient to use the display or hostname as
	   user name. A script may be used to generate the automatic/timed
	   login name based on the display/host by ending the name with the
	   pipe symbol '|'. */

	if (str->len > 0 && str->str[str->len - 1] == '|') {
		g_string_truncate (str, str->len - 1);
		if G_UNLIKELY (pipe (pipe1) < 0) {
			mdm_error ("mdm_slave_parse_enriched_login: Failed creating pipe");
		} else {
			mdm_debug ("Forking extra process: %s", str->str);

			extra_process = pid = mdm_fork_extra ();

			switch (pid) {
			case 0:
				/* The child will write the username to stdout based on the DISPLAY
				   environment variable. */

				VE_IGNORE_EINTR (close (pipe1[0]));
				if G_LIKELY (pipe1[1] != STDOUT_FILENO)  {
					VE_IGNORE_EINTR (dup2 (pipe1[1], STDOUT_FILENO));
				}

				mdm_log_shutdown ();

				mdm_close_all_descriptors (3 /* from */, pipe1[1] /* except */, -1 /* except2 */);

				mdm_log_init ();

				/* runs as root */
				if (MDM_AUTHFILE (d) != NULL)
					g_setenv ("XAUTHORITY", MDM_AUTHFILE (d), TRUE);
				else
					g_unsetenv ("XAUTHORITY");
				g_setenv ("DISPLAY", d->name, TRUE);
				if (d->windowpath)
					g_setenv ("WINDOWPATH", d->windowpath, TRUE);
				
				g_setenv ("PATH", mdm_daemon_config_get_value_string (MDM_KEY_ROOT_PATH), TRUE);
				g_setenv ("SHELL", "/bin/sh", TRUE);
				g_setenv ("RUNNING_UNDER_MDM", "true", TRUE);
				if ( ! ve_string_empty (d->theme_name))
					g_setenv ("MDM_GTK_THEME", d->theme_name, TRUE);

				g_shell_parse_argv (str->str, NULL, &argv, NULL);

				VE_IGNORE_EINTR (execv (argv[0], argv));
				g_strfreev (argv);
				mdm_error ("mdm_slave_parse_enriched_login: Failed executing: %s", str->str);
				_exit (EXIT_SUCCESS);

			case -1:
				mdm_error ("mdm_slave_parse_enriched_login: Can't fork script process!");
				VE_IGNORE_EINTR (close (pipe1[0]));
				VE_IGNORE_EINTR (close (pipe1[1]));
				break;

			default:
				/* The parent reads username from the pipe a chunk at a time */
				VE_IGNORE_EINTR (close (pipe1[1]));
				g_string_truncate (str, 0);
				do {
					VE_IGNORE_EINTR (in_buffer_len = read (pipe1[0], in_buffer,
									       sizeof (in_buffer) - 1));
					if (in_buffer_len > 0) {
						in_buffer[in_buffer_len] = '\0';
						g_string_append (str, in_buffer);
					}
				} while (in_buffer_len > 0);

				if (str->len > 0 && str->str[str->len - 1] == '\n')
					g_string_truncate (str, str->len - 1);

				VE_IGNORE_EINTR (close (pipe1[0]));

				mdm_wait_for_extra (extra_process, NULL);
			}
		}
	}

	if (!ve_string_empty(str->str) && mdm_is_user_valid(str->str))
		return g_string_free (str, FALSE);
	else
		{
			/* "If an empty or otherwise invalid username is returned [by the script]
			 *  automatic login [and timed login] is not performed." -- MDM manual 
			 */
			/* fixme: also turn off automatic login */
			mdm_daemon_config_set_value_bool(MDM_KEY_TIMED_LOGIN_ENABLE, FALSE);
			d->timed_login_ok = FALSE;
			do_timed_login = FALSE;
			g_string_free (str, TRUE);
			return NULL;
		}
}

static void
mdm_slave_handle_notify (const char *msg)
{
	int val;

	mdm_debug ("Handling slave notify: '%s'", msg);

	if (sscanf (msg, MDM_NOTIFY_ALLOW_ROOT " %d", &val) == 1) {
		mdm_daemon_config_set_value_bool (MDM_KEY_ALLOW_ROOT, val);	
	} else if (sscanf (msg, MDM_NOTIFY_SYSTEM_MENU " %d", &val) == 1) {
		mdm_daemon_config_set_value_bool (MDM_KEY_SYSTEM_MENU, val);
		if (d->greetpid > 1)
			kill (d->greetpid, SIGHUP);
	} else if (sscanf (msg, MDM_NOTIFY_CONFIG_AVAILABLE " %d", &val) == 1) {
		mdm_daemon_config_set_value_bool (MDM_KEY_CONFIG_AVAILABLE, val);
		if (d->greetpid > 1)
			kill (d->greetpid, SIGHUP);
	} else if (sscanf (msg, MDM_NOTIFY_RETRY_DELAY " %d", &val) == 1) {
		mdm_daemon_config_set_value_int (MDM_KEY_RETRY_DELAY, val);
	} else if (sscanf (msg, MDM_NOTIFY_DISALLOW_TCP " %d", &val) == 1) {
		mdm_daemon_config_set_value_bool (MDM_KEY_DISALLOW_TCP, val);
		remanage_asap = TRUE;
	} else if (strncmp (msg, MDM_NOTIFY_GREETER " ",
			    strlen (MDM_NOTIFY_GREETER) + 1) == 0) {
		mdm_daemon_config_set_value_string (MDM_KEY_GREETER, ((gchar *)&msg[strlen (MDM_NOTIFY_GREETER) + 1]));

		if (d->attached) {
			do_restart_greeter = TRUE;
			if (restart_greeter_now) {
				; /* will get restarted later */
			} else if (d->type == TYPE_STATIC) {
				/* FIXME: can't handle flexi servers like this
				 * without going all cranky */
				if ( ! d->logged_in) {
					mdm_slave_quick_exit (DISPLAY_REMANAGE);
				} else {
					remanage_asap = TRUE;
				}
			}
		}	
	} else if ((strncmp (msg, MDM_NOTIFY_TIMED_LOGIN " ",
			     strlen (MDM_NOTIFY_TIMED_LOGIN) + 1) == 0) ||
	           (strncmp (msg, MDM_NOTIFY_TIMED_LOGIN_DELAY " ",
			     strlen (MDM_NOTIFY_TIMED_LOGIN_DELAY) + 1) == 0) ||
	           (strncmp (msg, MDM_NOTIFY_TIMED_LOGIN_ENABLE " ",
			     strlen (MDM_NOTIFY_TIMED_LOGIN_ENABLE) + 1) == 0)) {
		do_restart_greeter = TRUE;
		/* FIXME: this is fairly nasty, we should handle this nicer   */
		/* FIXME: can't handle flexi servers without going all cranky */
		if (d->type == TYPE_STATIC) {
			if ( ! d->logged_in) {
				mdm_slave_quick_exit (DISPLAY_REMANAGE);
			} else {
				remanage_asap = TRUE;
			}
		}
	} else if (strncmp (msg, MDM_NOTIFY_SOUND_ON_LOGIN_FILE " ",
			    strlen (MDM_NOTIFY_SOUND_ON_LOGIN_FILE) + 1) == 0) {
		mdm_daemon_config_set_value_string (MDM_KEY_SOUND_ON_LOGIN_FILE,
						    (gchar *)(&msg[strlen (MDM_NOTIFY_SOUND_ON_LOGIN_FILE) + 1]));
		if (d->greetpid > 1)
			kill (d->greetpid, SIGHUP);
	} else if (strncmp (msg, MDM_NOTIFY_SOUND_ON_LOGIN_SUCCESS_FILE " ",
			    strlen (MDM_NOTIFY_SOUND_ON_LOGIN_SUCCESS_FILE) + 1) == 0) {
		mdm_daemon_config_set_value_string (MDM_KEY_SOUND_ON_LOGIN_SUCCESS_FILE,
						    (gchar *)(&msg[strlen (MDM_NOTIFY_SOUND_ON_LOGIN_SUCCESS_FILE) + 1]));
		if (d->greetpid > 1)
			kill (d->greetpid, SIGHUP);
	} else if (strncmp (msg, MDM_NOTIFY_SOUND_ON_LOGIN_FAILURE_FILE " ",
			    strlen (MDM_NOTIFY_SOUND_ON_LOGIN_FAILURE_FILE) + 1) == 0) {
		mdm_daemon_config_set_value_string (MDM_KEY_SOUND_ON_LOGIN_FAILURE_FILE,
						    (gchar *)(&msg[strlen (MDM_NOTIFY_SOUND_ON_LOGIN_FAILURE_FILE) + 1]));
		if (d->greetpid > 1)
			kill (d->greetpid, SIGHUP);
	} else if (strncmp (msg, MDM_NOTIFY_GTK_MODULES_LIST " ",
			    strlen (MDM_NOTIFY_GTK_MODULES_LIST) + 1) == 0) {
		mdm_daemon_config_set_value_string (MDM_KEY_GTK_MODULES_LIST,
						    (gchar *)(&msg[strlen (MDM_NOTIFY_GTK_MODULES_LIST) + 1]));

		if (mdm_daemon_config_get_value_bool (MDM_KEY_ADD_GTK_MODULES)) {
			do_restart_greeter = TRUE;
			if (restart_greeter_now) {
				; /* will get restarted later */
			} else if (d->type == TYPE_STATIC) {
				/* FIXME: can't handle flexi servers like this
				 * without going all cranky */
				if ( ! d->logged_in) {
					mdm_slave_quick_exit (DISPLAY_REMANAGE);
				} else {
					remanage_asap = TRUE;
				}
			}
		}
	} else if (sscanf (msg, MDM_NOTIFY_ADD_GTK_MODULES " %d", &val) == 1) {
		mdm_daemon_config_set_value_bool (MDM_KEY_ADD_GTK_MODULES, val);

		do_restart_greeter = TRUE;
		if (restart_greeter_now) {
			; /* will get restarted later */
		} else if (d->type == TYPE_STATIC) {
			/* FIXME: can't handle flexi servers like this
			 * without going all cranky */
			if ( ! d->logged_in) {
				mdm_slave_quick_exit (DISPLAY_REMANAGE);
			} else {
				remanage_asap = TRUE;
			}
		}
	}
}

/* do cleanup but only if we are a slave, if we're not a slave, just
 * return FALSE */
gboolean
mdm_slave_final_cleanup (void)
{
	if (getpid () != d->slavepid)
		return FALSE;
	mdm_debug ("slave killing self");
	mdm_slave_term_handler (SIGTERM);
	return TRUE;
}

/* mdm_is_user_valid() mostly copied from gui/mdmuser.c */
gboolean
mdm_is_user_valid (const char *username)
{
	return (NULL != getpwnam (username));
}

