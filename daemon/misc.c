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

#include <unistd.h>
#include <stdlib.h>
#include <dirent.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <sys/types.h>
#ifdef HAVE_SYS_SOCKIO_H
#include <sys/sockio.h>
#endif
#ifdef HAVE_DEFOPEN
#include <deflt.h>
#endif

#include <X11/Xlib.h>

#include <glib/gi18n.h>

#include "mdm.h"
#include "misc.h"
#include "slave.h"

#include "mdm-common.h"
#include "mdm-log.h"
#include "mdm-daemon-config.h"

extern char **environ;
extern pid_t mdm_main_pid;
extern pid_t extra_process;

#ifdef ENABLE_IPV6

static gboolean
have_ipv6 (void)
{
	int s;
        static gboolean has_ipv6 = -1;

        if (has_ipv6 != -1) return has_ipv6;

        s = socket (AF_INET6, SOCK_STREAM, 0);  
        if (s < 0) {
                  has_ipv6 = FALSE;
                  return FALSE;
	}

       VE_IGNORE_EINTR (close (s));            
       return has_ipv6;
}
#endif

void
mdm_fdprintf (int fd, const gchar *format, ...)
{
	va_list args;
	gchar *s;
	int written, len;

	va_start (args, format);
	s = g_strdup_vprintf (format, args);
	va_end (args);

	len = strlen (s);

	if (len == 0) {
		g_free (s);
		return;
	}

	written = 0;
	while (written < len) {
		int w;
		VE_IGNORE_EINTR (w = write (fd, &s[written], len - written));
		if (w < 0)
			/* evil! */
			break;
		written += w;
	}

	g_free (s);
}

/*
 * Clear environment, but keep the i18n ones,
 * note that this leaks memory so only use before exec
 * Also do not clear DISPLAY since we want to ensure that
 * this environment variable is always available.
 */
void
mdm_clearenv_no_lang (void)
{
	int i;
	GList *li, *envs = NULL;

	for (i = 0; environ[i] != NULL; i++) {
		char *env = environ[i];
		if (strncmp (env, "LC_", 3) == 0 ||
		    strncmp (env, "LANG", 4) == 0 ||
		    strncmp (env, "LINGUAS", 7) == 0 ||
		    strncmp (env, "DISPLAY", 7) == 0) {
			envs = g_list_prepend (envs, g_strdup (env));
		}
	}

	ve_clearenv ();

	for (li = envs; li != NULL; li = li->next) {
		putenv (li->data);
	}

	g_list_free (envs);
}

static GList *stored_env = NULL;

void
mdm_saveenv (void)
{
	int i;

	g_list_foreach (stored_env, (GFunc)g_free, NULL);
	g_list_free (stored_env);
	stored_env = NULL;

	for (i = 0; environ[i] != NULL; i++) {
		char *env = environ[i];
		stored_env = g_list_prepend (stored_env, g_strdup (env));
	}
}

const char *
mdm_saved_getenv (const char *var)
{
	int len;
	GList *li;

	len = strlen (var);

	for (li = stored_env; li != NULL; li = li->next) {
		const char *e = li->data;
		if (strncmp (var, e, len) == 0 &&
		    e[len] == '=') {
			return &(e[len+1]);
		}
	}
	return NULL;
}

/* leaks */
void
mdm_restoreenv (void)
{
	GList *li;

	ve_clearenv ();

	for (li = stored_env; li != NULL; li = li->next) {
		putenv (g_strdup (li->data));
	}
}

/* Evil function to figure out which display number is free */
int
mdm_get_free_display (int start, uid_t server_uid)
{
	int sock;
	int i;
	struct sockaddr_in serv_addr = { 0 };

	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = htonl (INADDR_LOOPBACK);

	/*
         * Cap this at 3000, I'm not sure we can ever seriously
	 * go that far
         */
	for (i = start; i < 3000; i++) {
		FILE *fp;
		GSList *li;
		GSList *displays;
		struct stat s;
		char buf[256];
		int r;
		gboolean try_ipv4 = TRUE;

                displays = mdm_daemon_config_get_display_list ();

		for (li = displays; li != NULL; li = li->next) {
			MdmDisplay *dsp = li->data;
			if (SERVER_IS_LOCAL (dsp) &&
			    dsp->dispnum == i)
				break;
		}
		if (li != NULL) {
			/* found one */
			continue;
		}

#ifdef ENABLE_IPV6
		if (have_ipv6 ()) {
			struct sockaddr_in6 serv6_addr = { 0 };

			sock = socket (AF_INET6, SOCK_STREAM,0);

			serv6_addr.sin6_family = AF_INET6;
			serv6_addr.sin6_addr = in6addr_loopback;
			serv6_addr.sin6_port = htons (6000 + i);
			errno = 0;
			VE_IGNORE_EINTR (connect (sock,
                                      (struct sockaddr *)&serv6_addr,
                                      sizeof (serv6_addr)));

			/*
			 * If IPv6 returns the network is unreachable,
			 * then try fallbacking to IPv4.  In all other
			 * cases, do not fallback.  This problem can
			 * happen if IPv6 is enabled, but the
			 * administrator has disabled it.
			 */
			if (errno != ENETUNREACH)
				try_ipv4 = FALSE;
		}
#endif

		if (try_ipv4)
		{
			sock = socket (AF_INET, SOCK_STREAM, 0);

			serv_addr.sin_port = htons (6000 + i);

			errno = 0;
			VE_IGNORE_EINTR (connect (sock,
				       (struct sockaddr *)&serv_addr,
				       sizeof (serv_addr)));
		}
		if (errno != 0 && errno != ECONNREFUSED) {
			VE_IGNORE_EINTR (close (sock));
			continue;
		}
		VE_IGNORE_EINTR (close (sock));

		/* if lock file exists and the process exists */
		g_snprintf (buf, sizeof (buf), "/tmp/.X%d-lock", i);
		VE_IGNORE_EINTR (r = g_stat (buf, &s));
		if (r == 0 &&
		    ! S_ISREG (s.st_mode)) {
			/*
                         * Eeeek! not a regular file?  Perhaps someone
			 * is trying to play tricks on us
                         */
			continue;
		}
		VE_IGNORE_EINTR (fp = fopen (buf, "r"));
		if (fp != NULL) {
			char buf2[100];
			char *getsret;
			VE_IGNORE_EINTR (getsret = fgets (buf2, sizeof (buf2), fp));
			if (getsret != NULL) {
				gulong pid;
				if (sscanf (buf2, "%lu", &pid) == 1 &&
				    kill (pid, 0) == 0) {
					VE_IGNORE_EINTR (fclose (fp));
					continue;
				}

			}
			VE_IGNORE_EINTR (fclose (fp));

			/* whack the file, it's a stale lock file */
			VE_IGNORE_EINTR (g_unlink (buf));
		}

		/* If starting as root, we'll be able to overwrite any
		 * stale sockets or lock files, but a user may not be
		 * able to */
		if (server_uid > 0) {
			g_snprintf (buf, sizeof (buf),
				    "/tmp/.X11-unix/X%d", i);
			VE_IGNORE_EINTR (r = g_stat (buf, &s));
			if (r == 0 &&
			    s.st_uid != server_uid) {
				continue;
			}

			g_snprintf (buf, sizeof (buf),
				    "/tmp/.X%d-lock", i);
			VE_IGNORE_EINTR (r = g_stat (buf, &s));
			if (r == 0 &&
			    s.st_uid != server_uid) {
				continue;
			}
		}

		return i;
	}

	return -1;
}

gboolean
mdm_text_message_dialog (const char *msg)
{
	char *dialog; /* do we have dialog? */
	char *msg_quoted;

    if ( ! mdm_daemon_config_get_value_bool (MDM_KEY_CONSOLE_NOTIFY))
		return FALSE;

	if (g_access (LIBEXECDIR "/mdmopen", X_OK) != 0)
		return FALSE;

	if (msg[0] == '-') {
		char *tmp = g_strconcat (" ", msg, NULL);
		msg_quoted = g_shell_quote (tmp);
		g_free (tmp);
	} else {
		msg_quoted = g_shell_quote (msg);
	}
	
	dialog = g_find_program_in_path ("dialog");
	if (dialog == NULL)
		dialog = g_find_program_in_path ("whiptail");
	if (dialog != NULL) {
		char *argv[6];

		if ( ! mdm_ok_console_language ()) {
			g_unsetenv ("LANG");
			g_unsetenv ("LC_ALL");
			g_unsetenv ("LC_MESSAGES");
			g_setenv ("LANG", "C", TRUE);
			g_setenv ("UNSAFE_TO_TRANSLATE", "yes", TRUE);
		}
		
		argv[0] = LIBEXECDIR "/mdmopen";
		argv[1] = "-l";
		argv[2] = "/bin/sh";
		argv[3] = "-c";
		argv[4] = g_strdup_printf ("%s --msgbox %s 16 70",
					   dialog, msg_quoted);
		argv[5] = NULL;

		/* Make sure gdialog wouldn't get confused */
		if (mdm_exec_wait (argv, TRUE /* no display */,
				   TRUE /* de_setuid */) < 0) {
			g_free (dialog);
			g_free (msg_quoted);
			g_free (argv[4]);
			return FALSE;
		}

		g_free (dialog);
		g_free (argv[4]);
	} else {
		char *argv[6];

		argv[0] = LIBEXECDIR "/mdmopen";
		argv[1] = "-l";
		argv[2] = "/bin/sh";
		argv[3] = "-c";
		argv[4] = g_strdup_printf
			("clear ; "
			 "echo %s ; read ; clear",
			 msg_quoted);
		argv[5] = NULL;

		if (mdm_exec_wait (argv, TRUE /* no display */,
				   TRUE /* de_setuid */) < 0) {
			g_free (argv[4]);
			g_free (msg_quoted);
			return FALSE;
		}
		g_free (argv[4]);
	}
	g_free (msg_quoted);
	return TRUE;
}

gboolean
mdm_text_yesno_dialog (const char *msg, gboolean *ret)
{
	char *dialog; /* do we have dialog? */
	char *msg_quoted;

    if ( ! mdm_daemon_config_get_value_bool (MDM_KEY_CONSOLE_NOTIFY))
		return FALSE;
	
	if (g_access (LIBEXECDIR "/mdmopen", X_OK) != 0)
		return FALSE;

	if (ret != NULL)
		*ret = FALSE;

	if (msg[0] == '-') {
		char *tmp = g_strconcat (" ", msg, NULL);
		msg_quoted = g_shell_quote (tmp);
		g_free (tmp);
	} else {
		msg_quoted = g_shell_quote (msg);
	}
	
	dialog = g_find_program_in_path ("dialog");
	if (dialog == NULL)
		dialog = g_find_program_in_path ("whiptail");
	if (dialog != NULL) {
		char *argv[6];
		int retint;

		if ( ! mdm_ok_console_language ()) {
			g_unsetenv ("LANG");
			g_unsetenv ("LC_ALL");
			g_unsetenv ("LC_MESSAGES");
			g_setenv ("LANG", "C", TRUE);
			g_setenv ("UNSAFE_TO_TRANSLATE", "yes", TRUE);
		}

		argv[0] = LIBEXECDIR "/mdmopen";
		argv[1] = "-l";
		argv[2] = "/bin/sh";
		argv[3] = "-c";
		argv[4] = g_strdup_printf ("%s --yesno %s 16 70",
					   dialog, msg_quoted);
		argv[5] = NULL;

		/*
                 * Will unset DISPLAY and XAUTHORITY if they exist
		 * so that gdialog (if used) doesn't get confused
                 */
		retint = mdm_exec_wait (argv, TRUE /* no display */,
					TRUE /* de_setuid */);
		if (retint < 0) {
			g_free (argv[4]);
			g_free (dialog);
			g_free (msg_quoted);
			return FALSE;
		}

		if (ret != NULL)
			*ret = (retint == 0) ? TRUE : FALSE;

		g_free (dialog);
		g_free (msg_quoted);
		g_free (argv[4]);

		return TRUE;
	} else {
		char tempname[] = "/tmp/mdm-yesno-XXXXXX";
		int tempfd;
		FILE *fp;
		char buf[256];
		char *argv[6];

		tempfd = g_mkstemp (tempname);
		if (tempfd < 0) {
			g_free (msg_quoted);
			return FALSE;
		}

		VE_IGNORE_EINTR (close (tempfd));

		argv[0] = LIBEXECDIR "/mdmopen";
		argv[1] = "-l";
		argv[2] = "/bin/sh";
		argv[3] = "-c";
		argv[4] = g_strdup_printf
			("clear ; "
			 "echo %s ; echo ; echo \"%s\" ; "
			 "read RETURN ; echo $RETURN > %s ; clear'",
			 msg_quoted,
			 /* Translators, don't translate the 'y' and 'n' */
			 _("y = Yes or n = No? >"),
			 tempname);
		argv[5] = NULL;

		if (mdm_exec_wait (argv, TRUE /* no display */,
				   TRUE /* de_setuid */) < 0) {
			g_free (argv[4]);
			g_free (msg_quoted);
			return FALSE;
		}
		g_free (argv[4]);

		if (ret != NULL) {
			VE_IGNORE_EINTR (fp = fopen (tempname, "r"));
			if (fp != NULL) {
				if (fgets (buf, sizeof (buf), fp) != NULL &&
				    (buf[0] == 'y' || buf[0] == 'Y'))
					*ret = TRUE;
				VE_IGNORE_EINTR (fclose (fp));
			} else {
				g_free (msg_quoted);
				return FALSE;
			}
		}

		VE_IGNORE_EINTR (g_unlink (tempname));

		g_free (msg_quoted);
		return TRUE;
	}
}

int
mdm_exec_wait (char * const *argv,
	       gboolean no_display,
	       gboolean de_setuid)
{
	int status;
	pid_t pid;

	if (argv == NULL ||
	    argv[0] == NULL ||
	    g_access (argv[0], X_OK) != 0)
		return -1;

	mdm_debug ("Forking extra process: %s", argv[0]);

	pid = mdm_fork_extra ();
	if (pid == 0) {
		mdm_log_shutdown ();

		mdm_close_all_descriptors (0 /* from */, -1 /* except */, -1 /* except2 */);

		/*
                 * No error checking here - if it's messed the best response
		 * is to ignore & try to continue
                 */
		mdm_open_dev_null (O_RDONLY); /* open stdin - fd 0 */
		mdm_open_dev_null (O_RDWR);   /* open stdout - fd 1 */
		mdm_open_dev_null (O_RDWR);   /* open stderr - fd 2 */

		if (de_setuid) {
			mdm_desetuid ();
		}

		mdm_log_init ();

		if (no_display) {
			g_unsetenv ("DISPLAY");
			g_unsetenv ("XAUTHORITY");
		}

		VE_IGNORE_EINTR (execv (argv[0], argv));

		_exit (-1);
	}

	if (pid < 0)
		return -1;

	mdm_wait_for_extra (pid, &status);

	if (WIFEXITED (status))
		return WEXITSTATUS (status);
	else
		return -1;
}

static int sigchld_blocked = 0;
static sigset_t sigchldblock_mask, sigchldblock_oldmask;

static int sigterm_blocked = 0;
static sigset_t sigtermblock_mask, sigtermblock_oldmask;

static int sigusr2_blocked = 0;
static sigset_t sigusr2block_mask, sigusr2block_oldmask;

void
mdm_sigchld_block_push (void)
{
	sigchld_blocked++;

	if (sigchld_blocked == 1) {
		/* Set signal mask */
		sigemptyset (&sigchldblock_mask);
		sigaddset (&sigchldblock_mask, SIGCHLD);
		sigprocmask (SIG_BLOCK, &sigchldblock_mask, &sigchldblock_oldmask);
	}
}

void
mdm_sigchld_block_pop (void)
{
	sigchld_blocked --;

	if (sigchld_blocked == 0) {
		/* Reset signal mask back */
		sigprocmask (SIG_SETMASK, &sigchldblock_oldmask, NULL);
	}
}

void
mdm_sigterm_block_push (void)
{
	sigterm_blocked++;

	if (sigterm_blocked == 1) {
		/* Set signal mask */
		sigemptyset (&sigtermblock_mask);
		sigaddset (&sigtermblock_mask, SIGTERM);
		sigaddset (&sigtermblock_mask, SIGINT);
		sigaddset (&sigtermblock_mask, SIGHUP);
		sigprocmask (SIG_BLOCK, &sigtermblock_mask, &sigtermblock_oldmask);
	}
}

void
mdm_sigterm_block_pop (void)
{
	sigterm_blocked --;

	if (sigterm_blocked == 0) {
		/* Reset signal mask back */
		sigprocmask (SIG_SETMASK, &sigtermblock_oldmask, NULL);
	}
}

void
mdm_sigusr2_block_push (void)
{
	sigset_t oldmask;

	if (sigusr2_blocked == 0) {
		/* Set signal mask */
		sigemptyset (&sigusr2block_mask);
		sigaddset (&sigusr2block_mask, SIGUSR2);
		sigprocmask (SIG_BLOCK, &sigusr2block_mask, &oldmask);
	}

	sigusr2_blocked++;

	sigusr2block_oldmask = oldmask;
}

void
mdm_sigusr2_block_pop (void)
{
	sigset_t oldmask;

	oldmask = sigusr2block_oldmask;

	sigusr2_blocked--;

	if (sigusr2_blocked == 0) {
	        /* Reset signal mask back */
	        sigprocmask (SIG_SETMASK, &sigusr2block_oldmask, NULL);
	}
}

pid_t
mdm_fork_extra (void)
{
	pid_t pid;

	mdm_sigchld_block_push ();
	mdm_sigterm_block_push ();

	pid = fork ();
	if (pid < 0)
		return 0;
	else if (pid == 0)
		/*
                 * Unset signals here, and yet again
		 * later as the block_pop will whack
		 * our signal mask
                 */
		mdm_unset_signals ();

	mdm_sigterm_block_pop ();
	mdm_sigchld_block_pop ();

	if (pid == 0) {
		/*
                 * In the child setup empty mask and set all signals to
		 * default values
                 */
		mdm_unset_signals ();

		/*
                 * Also make a new process group so that we may use
		 * kill -(extra_process) to kill extra process and all it's
		 * possible children
                 */
		setsid ();
		
	}

	return pid;
}

void
mdm_wait_for_extra (pid_t pid,
		    int  *statusp)
{
	int status;

	mdm_sigchld_block_push ();

	if (pid > 1) {
		ve_waitpid_no_signal (pid, &status, 0);
	}

	if (statusp != NULL)
		*statusp = status;

	mdm_sigchld_block_pop ();
}

static void
ensure_tmp_socket_dir (const char *dir)
{
	mode_t old_umask;

	/*
         * The /tmp/.ICE-unix / .X11-unix check, note that we do
	 * ignore errors, since it's not deadly to run
	 * if we can't perform this task :)
         */
	old_umask = umask (0);

        if G_UNLIKELY (g_mkdir (dir, 01777) != 0) {
		/*
                 * If we can't create it, perhaps it
		 * already exists, in which case ensure the
		 * correct permissions
                 */
		struct stat s;
		int r;
		VE_IGNORE_EINTR (r = g_lstat (dir, &s));
		if G_LIKELY (r == 0 && S_ISDIR (s.st_mode)) {
			/* Make sure it is root and sticky */
			VE_IGNORE_EINTR (chown (dir, 0, 0));
			VE_IGNORE_EINTR (g_chmod (dir, 01777));
		} else {
			/*
                         * There is a file/link/whatever of the same name?
			 * whack and try mkdir
                         */
			VE_IGNORE_EINTR (g_remove (dir));
			g_mkdir (dir, 01777);
		}
	}

	umask (old_umask);
}

/*
 * Done on startup and when running display_manage
 * This can do some sanity ensuring, one of the things it does now is make
 * sure /tmp/.ICE-unix and /tmp/.X11-unix exist and have the correct
 * permissions.
 *
 * Do nothing on Solaris since this logic breaks Trusted Extensions, and
 * the Solaris X permissions model (socket & pipe directories only writable
 * by gid-root), and it ignores the Solaris /tmp/.X11-pipe directory.
 */
void
mdm_ensure_sanity (void)
{
	uid_t old_euid;
	gid_t old_egid;

	old_euid = geteuid ();
	old_egid = getegid ();

	NEVER_FAILS_root_set_euid_egid (0, 0);

	ensure_tmp_socket_dir ("/tmp/.ICE-unix");
	ensure_tmp_socket_dir ("/tmp/.X11-unix");

	NEVER_FAILS_root_set_euid_egid (old_euid, old_egid);
}

const GList *
mdm_address_peek_local_list (void)
{
	static GList *the_list = NULL;
	static time_t last_time = 0;
	char hostbuf[BUFSIZ];
	struct addrinfo hints;  
	struct addrinfo *result;
	struct addrinfo *res;

	/* Don't check more then every 5 seconds */
	if (last_time + 5 > time (NULL)) {
		return the_list;
	}

	g_list_foreach (the_list, (GFunc)g_free, NULL);
	g_list_free (the_list);
	the_list = NULL;

	last_time = time (NULL);

	hostbuf[BUFSIZ-1] = '\0';
	if (gethostname (hostbuf, BUFSIZ-1) != 0) {
		mdm_debug ("%s: Could not get server hostname, using localhost", "mdm_peek_local_address_list");
		snprintf (hostbuf, BUFSIZ-1, "localhost");
	}

	memset (&hints, 0, sizeof (hints)); 

	hints.ai_family = AF_INET;

#ifdef ENABLE_IPV6
	hints.ai_family = AF_INET6;
#endif  

#ifdef ENABLE_IPV6 
	if (getaddrinfo (hostbuf, NULL, &hints, &result) != 0) {
		hints.ai_family = AF_INET;
#endif

	if (getaddrinfo (hostbuf, NULL, &hints, &result) != 0) {
		mdm_debug ("%s: Could not get address from hostname!", "mdm_peek_local_address_list");

		return NULL;
	}

#ifdef ENABLE_IPV6        
	}
#endif    
	for (res = result; res != NULL; res = res->ai_next) {
		struct sockaddr_storage *sa;

		sa = g_memdup (res->ai_addr, res->ai_addrlen);
		the_list = g_list_append (the_list, sa);
	}

	if (result) {
		freeaddrinfo (result);
		result = NULL;
	}

	return the_list;
}


gboolean
mdm_address_is_local (struct sockaddr_storage *sa)
{
	const GList *list;

	if (mdm_address_is_loopback (sa)) {
		return TRUE;
	}

	list = mdm_address_peek_local_list ();

	while (list != NULL) {
		struct sockaddr_storage *addr = list->data;

		if (mdm_address_equal (sa, addr)) {
			return TRUE;
		}

		list = list->next;
	}

	return FALSE;
}

gboolean
mdm_setup_gids (const char *login, gid_t gid)
{
	/*
         * FIXME: perhaps for *BSD there should be setusercontext
	 * stuff here
         */
	if G_UNLIKELY (setgid (gid) < 0)  {
		mdm_error ("Could not setgid %d. Aborting.", (int)gid);
		return FALSE;
	}

	if G_UNLIKELY (initgroups (login, gid) < 0) {
		mdm_error ("initgroups () failed for %s. Aborting.", login);
		return FALSE;
	}

	return TRUE;
}

void
mdm_desetuid (void)
{
	uid_t uid = getuid (); 
	gid_t gid = getgid (); 

#ifdef HAVE_SETRESUID
	{
		int setresuid (uid_t ruid, uid_t euid, uid_t suid);
		int setresgid (gid_t rgid, gid_t egid, gid_t sgid);
		setresgid (gid, gid, gid);
		setresuid (uid, uid, uid);
	}
#else
	setegid (getgid ());
	seteuid (getuid ());
#endif
}

gboolean
mdm_test_opt (const char *cmd, const char *help, const char *option)
{
	char *q;
	char *full;
	char buf[1024];
	FILE *fp;
	static GString *cache = NULL;
	static char *cached_cmd = NULL;
	gboolean got_it;

	if (cached_cmd != NULL &&
	    strcmp (cached_cmd, cmd) == 0) {
		char *p = strstr (ve_sure_string (cache->str), option);
		char end;
		if (p == NULL)
			return FALSE;
		/* Must be a full word */
		end = *(p + strlen (option));
		if ((end >= 'a' && end <= 'z') ||
		    (end >= 'A' && end <= 'Z') ||
		    (end >= '0' && end <= '9') ||
		    end == '_')
			return FALSE;
		return TRUE;
	}

	g_free (cached_cmd);
	cached_cmd = g_strdup (cmd);
	if (cache != NULL)
		g_string_assign (cache, "");
	else
		cache = g_string_new (NULL);

	q = g_shell_quote (cmd);

	full = g_strdup_printf ("%s %s 2>&1", q, help);
	g_free (q);

	fp = popen (full, "r");
	g_free (full);

	if (fp == NULL)
		return FALSE;

	got_it = FALSE;

	while (fgets (buf, sizeof (buf), fp) != NULL) {
		char *p;
		char end;

		g_string_append (cache, buf);

		if (got_it)
			continue;

		p = strstr (buf, option);
		if (p == NULL)
			continue;
		/* Must be a full word */
		end = *(p + strlen (option));
		if ((end >= 'a' && end <= 'z') ||
		    (end >= 'A' && end <= 'Z') ||
		    (end >= '0' && end <= '9') ||
		    end == '_')
			continue;

		got_it = TRUE;
	}
	VE_IGNORE_EINTR (fclose (fp));
	return got_it;
}

int
mdm_fdgetc (int fd)
{
	unsigned char buf[1];
	int bytes;

	/*
	 * Must used an unsigned char buffer here because the GUI sends
	 * username/password data as utf8 and the daemon will interpret
	 * any character sent with its high bit set as EOF unless we 
	 * used unsigned here.
	 */
	VE_IGNORE_EINTR (bytes = read (fd, buf, 1));
	if (bytes != 1)
		return EOF;
	else
		return (int)buf[0];
}

char *
mdm_fdgets (int fd)
{
	int c;
	int bytes = 0;
	GString *gs = g_string_new (NULL);
	for (;;) {
		c = mdm_fdgetc (fd);
		if (c == '\n')
			return g_string_free (gs, FALSE);
		/* on EOF */
		if (c < 0) {
			if (bytes == 0) {
				g_string_free (gs, TRUE);
				return NULL;
			} else {
				return g_string_free (gs, FALSE);
			}
		} else {
			bytes++;
			g_string_append_c (gs, c);
		}
	}
}

void
mdm_close_all_descriptors (int from, int except, int except2)
{
	DIR *dir;
	struct dirent *ent;
	GSList *openfds = NULL;

	/*
         * Evil, but less evil then going to _SC_OPEN_MAX
	 * which can be very VERY large
         */
	dir = opendir ("/proc/self/fd/");   /* This is the Linux dir */
	if (dir == NULL)
		dir = opendir ("/dev/fd/"); /* This is the FreeBSD dir */
	if G_LIKELY (dir != NULL) {
		GSList *li;
		while ((ent = readdir (dir)) != NULL) {
			int fd;
			if (ent->d_name[0] == '.')
				continue;
			fd = atoi (ent->d_name);
			if (fd >= from && fd != except && fd != except2)
				openfds = g_slist_prepend (openfds, GINT_TO_POINTER (fd));
		}
		closedir (dir);
		for (li = openfds; li != NULL; li = li->next) {
			int fd = GPOINTER_TO_INT (li->data); 
			VE_IGNORE_EINTR (close (fd));
		}
		g_slist_free (openfds);
	} else {
		int i;
		int max = sysconf (_SC_OPEN_MAX);
		/*
                 * Don't go higher then this.  This is
		 * a safety measure to not hang on crazy
		 * systems
                 */
		if G_UNLIKELY (max > 4096) {
			/* FIXME: warn about this perhaps */
			/*
                         * Try an open, in case we're really
			 * leaking fds somewhere badly, this
			 * should be very high
                         */
			i = mdm_open_dev_null (O_RDONLY);
			max = MAX (i+1, 4096);
		}
		for (i = from; i < max; i++) {
			if G_LIKELY (i != except && i != except2)
				VE_IGNORE_EINTR (close (i));
		}
	}
}

int
mdm_open_dev_null (mode_t mode)
{
	int ret;
	VE_IGNORE_EINTR (ret = open ("/dev/null", mode));
	if G_UNLIKELY (ret < 0) {
		/*
                 * Never output anything, we're likely in some
		 * strange state right now
                 */
		mdm_signal_ignore (SIGPIPE);
		VE_IGNORE_EINTR (close (2));
		mdm_fail ("Cannot open /dev/null, system on crack!");
	}

	return ret;
}

void
mdm_unset_signals (void)
{
	sigset_t mask;

	sigemptyset (&mask);
	sigprocmask (SIG_SETMASK, &mask, NULL);

	mdm_signal_default (SIGUSR1);
	mdm_signal_default (SIGUSR2);
	mdm_signal_default (SIGCHLD);
	mdm_signal_default (SIGTERM);
	mdm_signal_default (SIGINT);
	mdm_signal_default (SIGPIPE);
	mdm_signal_default (SIGALRM);
	mdm_signal_default (SIGHUP);
	mdm_signal_default (SIGABRT);
#ifdef SIGXFSZ
	mdm_signal_default (SIGXFSZ);
#endif
#ifdef SIGXCPU
	mdm_signal_default (SIGXCPU);
#endif
}

void
mdm_signal_ignore (int signal)
{
	struct sigaction ign_signal;

	ign_signal.sa_handler = SIG_IGN;
	ign_signal.sa_flags = SA_RESTART;
	sigemptyset (&ign_signal.sa_mask);

	if G_UNLIKELY (sigaction (signal, &ign_signal, NULL) < 0)
		mdm_error ("mdm_signal_ignore: Error setting signal %d to %s", signal, "SIG_IGN");
}

void
mdm_signal_default (int signal)
{
	struct sigaction def_signal;

	def_signal.sa_handler = SIG_DFL;
	def_signal.sa_flags = SA_RESTART;
	sigemptyset (&def_signal.sa_mask);

	if G_UNLIKELY (sigaction (signal, &def_signal, NULL) < 0)
		mdm_error ("mdm_signal_ignore: Error setting signal %d to %s", signal, "SIG_DFL");
}

static struct sigaction oldterm, oldint, oldhup;

/*
 * This sets up interruptes to be proxied and the
 * gethostbyname/addr to be whacked using longjmp,
 * in case INT/TERM/HUP was gotten in which case
 * we no longer care for the result of the
 * resolution.
 */
#define SETUP_INTERRUPTS_FOR_TERM_DECLS \
    struct sigaction term;

#define SETUP_INTERRUPTS_FOR_TERM_SETUP \
    									\
    term.sa_handler = jumpback_sighandler;				\
    term.sa_flags = SA_RESTART;						\
    sigemptyset (&term.sa_mask);					\
									\
    if G_UNLIKELY (sigaction (SIGTERM, &term, &oldterm) < 0) 		\
	mdm_fail ("SETUP_INTERRUPTS_FOR_TERM: Error setting up %s signal handler: %s", "TERM", strerror (errno)); \
									\
    if G_UNLIKELY (sigaction (SIGINT, &term, &oldint) < 0)		\
	mdm_fail ("SETUP_INTERRUPTS_FOR_TERM: Error setting up %s signal handler: %s", "INT", strerror (errno)); \
									\
    if G_UNLIKELY (sigaction (SIGHUP, &term, &oldhup) < 0) 		\
	mdm_fail ("SETUP_INTERRUPTS_FOR_TERM: Error setting up %s signal handler: %s", "HUP", strerror (errno)); \

#define SETUP_INTERRUPTS_FOR_TERM_TEARDOWN \
									\
    if G_UNLIKELY (sigaction (SIGTERM, &oldterm, NULL) < 0) 		\
	mdm_fail ("SETUP_INTERRUPTS_FOR_TERM: Error setting up %s signal handler: %s", "TERM", strerror (errno)); \
									\
    if G_UNLIKELY (sigaction (SIGINT, &oldint, NULL) < 0) 		\
	mdm_fail ("SETUP_INTERRUPTS_FOR_TERM: Error setting up %s signal handler: %s", "INT", strerror (errno)); \
									\
    if G_UNLIKELY (sigaction (SIGHUP, &oldhup, NULL) < 0) 		\
	mdm_fail ("SETUP_INTERRUPTS_FOR_TERM: Error setting up %s signal handler: %s", "HUP", strerror (errno));

/* Like fopen with "w" */
FILE *
mdm_safe_fopen_w (const char *file, mode_t perm)
{
	int fd;
	FILE *ret;
	VE_IGNORE_EINTR (g_unlink (file));
	do {
		errno = 0;
		fd = open (file, O_EXCL|O_CREAT|O_TRUNC|O_WRONLY
#ifdef O_NOCTTY
			   |O_NOCTTY
#endif
#ifdef O_NOFOLLOW
			   |O_NOFOLLOW
#endif
			   , perm);
	} while G_UNLIKELY (errno == EINTR);
	if (fd < 0)
		return NULL;
	VE_IGNORE_EINTR (ret = fdopen (fd, "w"));
	return ret;
}

/* Like fopen with "a+" */
FILE *
mdm_safe_fopen_ap (const char *file, mode_t perm)
{
	int fd;
	FILE *ret;

	if (g_access (file, F_OK) == 0) {
		do {
			errno = 0;
			fd = open (file, O_APPEND|O_RDWR
#ifdef O_NOCTTY
				   |O_NOCTTY
#endif
#ifdef O_NOFOLLOW
				   |O_NOFOLLOW
#endif
				  );
		} while G_UNLIKELY (errno == EINTR);
	} else {
		/* Doesn't exist, open with O_EXCL */
		do {
			errno = 0;
			fd = open (file, O_EXCL|O_CREAT|O_RDWR
#ifdef O_NOCTTY
				   |O_NOCTTY
#endif
#ifdef O_NOFOLLOW
				   |O_NOFOLLOW
#endif
				   , perm);
		} while G_UNLIKELY (errno == EINTR);
	}
	if (fd < 0)
		return NULL;
	VE_IGNORE_EINTR (ret = fdopen (fd, "a+"));
	return ret;
}


#define CHECK_LC(value, category) \
    (g_str_has_prefix (line->str, value "=")) \
      { \
	character = g_utf8_get_char (line->str + strlen (value "=")); \
\
	if ((character == '\'') || (character == '\"')) \
	  { \
	    q = g_utf8_find_prev_char (line->str, line->str + line->len); \
\
	    if ((q == NULL) || (g_utf8_get_char (q) != character)) \
	      { \
		g_string_set_size (line, 0); \
		continue; \
	      } \
\
	    g_string_set_size (line, line->len - 1); \
	    g_setenv (value, line->str + strlen (value "=") + 1, TRUE); \
	    if (category) \
	      setlocale ((category), line->str + strlen (value "=") + 1); \
	  } \
	else \
	  { \
	    g_setenv (value, line->str + strlen (value "="), TRUE); \
	    if (category) \
	      setlocale ((category), line->str + strlen (value "=")); \
	  } \
\
        g_string_set_size (line, 0); \
	continue; \
      }

void
mdm_reset_locale (void)
{
    char *i18n_file_contents;
    gsize i18n_file_length, i;
    GString *line;
    const gchar *p, *q;
    const gchar *mdmlang = g_getenv ("MDM_LANG");

    if (mdmlang)
      {
	g_setenv ("LANG", mdmlang, TRUE);
	g_unsetenv ("LC_ALL");
	g_unsetenv ("LC_MESSAGES");
	setlocale (LC_ALL, "");
	setlocale (LC_MESSAGES, "");
	return;
      }

    i18n_file_contents = NULL;
    line = NULL; 
    p = NULL;
    if (!g_file_get_contents (LANG_CONFIG_FILE, &i18n_file_contents,
			      &i18n_file_length, NULL))
      goto out;

    if (!g_utf8_validate (i18n_file_contents, i18n_file_length, NULL))
      goto out;

    line = g_string_new ("");
    p = i18n_file_contents;
    for (i = 0; i < i18n_file_length; 
	 p = g_utf8_next_char (p), i = p - i18n_file_contents) 
      {
	gunichar character;
	character = g_utf8_get_char (p);

	if ((character != '\n') && (character != '\0')) 
	  {
	    g_string_append_unichar (line, character);
	    continue;
	  }

	if CHECK_LC("LC_ALL", LC_ALL)
	else if CHECK_LC("LC_COLLATE", LC_COLLATE)
	else if CHECK_LC("LC_MESSAGES", LC_MESSAGES)
	else if CHECK_LC("LC_MONETARY", LC_MONETARY)
	else if CHECK_LC("LC_NUMERIC", LC_NUMERIC)
	else if CHECK_LC("LC_TIME", LC_TIME)
	else if CHECK_LC("LANG", 0)

        g_string_set_size (line, 0);
      }

    g_string_free (line, TRUE);

    setlocale (LC_ALL, "");

  out:
    g_free (i18n_file_contents);
}

#undef CHECK_LC

#ifdef RLIM_NLIMITS
#define NUM_OF_LIMITS RLIM_NLIMITS
#else /* ! RLIM_NLIMITS */
#ifdef RLIMIT_NLIMITS
#define NUM_OF_LIMITS RLIMIT_NLIMITS
#endif /* RLIMIT_NLIMITS */
#endif /* RLIM_NLIMITS */

/* If we can count limits then the reset code is simple */
#ifdef NUM_OF_LIMITS

static struct rlimit limits[NUM_OF_LIMITS];

void
mdm_get_initial_limits (void)
{
	int i;

	for (i = 0; i < NUM_OF_LIMITS; i++) {
		/* Some sane defaults */
		limits[i].rlim_cur = RLIM_INFINITY;
		limits[i].rlim_max = RLIM_INFINITY;
		/* Get the limits */
		getrlimit (i, &(limits[i]));
	}
}


void
mdm_reset_limits (void)
{
	int i;

	for (i = 0; i < NUM_OF_LIMITS; i++) {
		/* Get the limits */
		setrlimit (i, &(limits[i]));
	}
}


#else /* ! NUM_OF_LIMITS */
/* We have to go one by one here */

#ifdef RLIMIT_CPU
static struct rlimit limit_cpu = { RLIM_INFINITY, RLIM_INFINITY };
#endif
#ifdef RLIMIT_DATA
static struct rlimit limit_data = { RLIM_INFINITY, RLIM_INFINITY };
#endif
#ifdef RLIMIT_FSIZE
static struct rlimit limit_fsize = { RLIM_INFINITY, RLIM_INFINITY };
#endif
#ifdef RLIMIT_LOCKS
static struct rlimit limit_locks = { RLIM_INFINITY, RLIM_INFINITY };
#endif
#ifdef RLIMIT_MEMLOCK
static struct rlimit limit_memlock = { RLIM_INFINITY, RLIM_INFINITY };
#endif
#ifdef RLIMIT_NOFILE
static struct rlimit limit_nofile = { RLIM_INFINITY, RLIM_INFINITY };
#endif
#ifdef RLIMIT_OFILE
static struct rlimit limit_ofile = { RLIM_INFINITY, RLIM_INFINITY };
#endif
#ifdef RLIMIT_NPROC
static struct rlimit limit_nproc = { RLIM_INFINITY, RLIM_INFINITY };
#endif
#ifdef RLIMIT_RSS
static struct rlimit limit_rss = { RLIM_INFINITY, RLIM_INFINITY };
#endif
#ifdef RLIMIT_STACK
static struct rlimit limit_stack = { RLIM_INFINITY, RLIM_INFINITY };
#endif
#ifdef RLIMIT_CORE
static struct rlimit limit_core = { RLIM_INFINITY, RLIM_INFINITY };
#endif
#ifdef RLIMIT_AS
static struct rlimit limit_as = { RLIM_INFINITY, RLIM_INFINITY };
#endif
#ifdef RLIMIT_VMEM
static struct rlimit limit_vmem = { RLIM_INFINITY, RLIM_INFINITY };
#endif
#ifdef RLIMIT_PTHREAD
static struct rlimit limit_pthread = { RLIM_INFINITY, RLIM_INFINITY };
#endif

void
mdm_get_initial_limits (void)
{
	/* Note: I don't really know which ones are really very standard
	   and which ones are not, so I just test for them all one by one */

#ifdef RLIMIT_CPU
	getrlimit (RLIMIT_CPU, &limit_cpu);
#endif
#ifdef RLIMIT_DATA
	getrlimit (RLIMIT_DATA, &limit_data);
#endif
#ifdef RLIMIT_FSIZE
	getrlimit (RLIMIT_FSIZE, &limit_fsize);
#endif
#ifdef RLIMIT_LOCKS
	getrlimit (RLIMIT_LOCKS, &limit_locks);
#endif
#ifdef RLIMIT_MEMLOCK
	getrlimit (RLIMIT_MEMLOCK, &limit_memlock);
#endif
#ifdef RLIMIT_NOFILE
	getrlimit (RLIMIT_NOFILE, &limit_nofile);
#endif
#ifdef RLIMIT_OFILE
	getrlimit (RLIMIT_OFILE, &limit_ofile);
#endif
#ifdef RLIMIT_NPROC
	getrlimit (RLIMIT_NPROC, &limit_nproc);
#endif
#ifdef RLIMIT_RSS
	getrlimit (RLIMIT_RSS, &limit_rss);
#endif
#ifdef RLIMIT_STACK
	getrlimit (RLIMIT_STACK, &limit_stack);
#endif
#ifdef RLIMIT_CORE
	getrlimit (RLIMIT_CORE, &limit_core);
#endif
#ifdef RLIMIT_AS
	getrlimit (RLIMIT_AS, &limit_as);
#endif
#ifdef RLIMIT_VMEM
	getrlimit (RLIMIT_VMEM, &limit_vmem);
#endif
#ifdef RLIMIT_PTHREAD
	getrlimit (RLIMIT_PTHREAD, &limit_pthread);
#endif
}

void
mdm_reset_limits (void)
{
	/* Note: I don't really know which ones are really very standard
	   and which ones are not, so I just test for them all one by one */

#ifdef RLIMIT_CPU
	setrlimit (RLIMIT_CPU, &limit_cpu);
#endif
#ifdef RLIMIT_DATA
	setrlimit (RLIMIT_DATA, &limit_data);
#endif
#ifdef RLIMIT_FSIZE
	setrlimit (RLIMIT_FSIZE, &limit_fsize);
#endif
#ifdef RLIMIT_LOCKS
	setrlimit (RLIMIT_LOCKS, &limit_locks);
#endif
#ifdef RLIMIT_MEMLOCK
	setrlimit (RLIMIT_MEMLOCK, &limit_memlock);
#endif
#ifdef RLIMIT_NOFILE
	setrlimit (RLIMIT_NOFILE, &limit_nofile);
#endif
#ifdef RLIMIT_OFILE
	setrlimit (RLIMIT_OFILE, &limit_ofile);
#endif
#ifdef RLIMIT_NPROC
	setrlimit (RLIMIT_NPROC, &limit_nproc);
#endif
#ifdef RLIMIT_RSS
	setrlimit (RLIMIT_RSS, &limit_rss);
#endif
#ifdef RLIMIT_STACK
	setrlimit (RLIMIT_STACK, &limit_stack);
#endif
#ifdef RLIMIT_CORE
	setrlimit (RLIMIT_CORE, &limit_core);
#endif
#ifdef RLIMIT_AS
	setrlimit (RLIMIT_AS, &limit_as);
#endif
#ifdef RLIMIT_VMEM
	setrlimit (RLIMIT_VMEM, &limit_vmem);
#endif
#ifdef RLIMIT_PTHREAD
	setrlimit (RLIMIT_PTHREAD, &limit_pthread);
#endif
}

#endif /* NUM_OF_LIMITS */

const char *
mdm_root_user (void)
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
mdm_sleep_no_signal (int secs)
{
	time_t endtime = time (NULL)+secs;

	while (secs > 0) {
		struct timeval tv;
		tv.tv_sec = secs;
		tv.tv_usec = 0;
		select (0, NULL, NULL, NULL, &tv);
		/* Don't want to use sleep since we're using alarm
		   for pinging */
		secs = endtime - time (NULL);
	}
}

char *
mdm_make_filename (const char *dir, const char *name, const char *extension)
{
	char *base = g_strconcat (name, extension, NULL);
	char *full = g_build_filename (dir, base, NULL);
	g_free (base);
	return full;
}

char *
mdm_ensure_extension (const char *name, const char *extension)
{
	const char *p;

	if (ve_string_empty (name))
		return g_strdup (name);

	p = strrchr (name, '.');
	if (p != NULL &&
	    strcmp (p, extension) == 0) {
		return g_strdup (name);
	} else {
		return g_strconcat (name, extension, NULL);
	}
}

char *
mdm_strip_extension (const char *name, const char *extension)
{
	const char *p = strrchr (name, '.');
	if (p != NULL &&
	    strcmp (p, extension) == 0) {
		char *r = g_strdup (name);
		char *rp = strrchr (r, '.');
		*rp = '\0';
		return r;
	} else {
		return g_strdup (name);
	}
}

void
mdm_twiddle_pointer (MdmDisplay *disp)
{
	if (disp == NULL ||
	    disp->dsp == NULL)
		return;

	XWarpPointer (disp->dsp,
		      None /* src_w */,
		      None /* dest_w */,
		      0 /* src_x */,
		      0 /* src_y */,
		      0 /* src_width */,
		      0 /* src_height */,
		      1 /* dest_x */,
		      1 /* dest_y */);
	XSync (disp->dsp, False);
	XWarpPointer (disp->dsp,
		      None /* src_w */,
		      None /* dest_w */,
		      0 /* src_x */,
		      0 /* src_y */,
		      0 /* src_width */,
		      0 /* src_height */,
		      -1 /* dest_x */,
		      -1 /* dest_y */);
	XSync (disp->dsp, False);
}

static char *
compress_string (const char *s)
{
	GString *gs = g_string_new (NULL);
	const char *p;
	gboolean in_whitespace = TRUE;

	for (p = s; *p != '\0'; p++) {
		if (*p == ' ' || *p == '\t') {
			if ( ! in_whitespace)
				g_string_append_c (gs, *p);
			in_whitespace = TRUE;
		} else {
			g_string_append_c (gs, *p);
			in_whitespace = FALSE;
		}
	}

	return g_string_free (gs, FALSE);
}


char *
mdm_get_last_info (const char *username)
{
	char *info = NULL;
	const char *cmd = NULL;

	if G_LIKELY (g_access ("/usr/bin/last", X_OK) == 0)
		cmd = "/usr/bin/last";
	else if (g_access ("/bin/last", X_OK) == 0)
		cmd = "/bin/last";

	if G_LIKELY (cmd != NULL) {
		char *user_quoted = g_shell_quote (username);
		char *newcmd;
		FILE *fp;

		newcmd = g_strdup_printf ("%s %s", cmd, user_quoted);

		VE_IGNORE_EINTR (fp = popen (newcmd, "r"));

		g_free (user_quoted);
		g_free (newcmd);

		if G_LIKELY (fp != NULL) {
			char buf[256];
			char *r;
			VE_IGNORE_EINTR (r = fgets (buf, sizeof (buf), fp));
			if G_LIKELY (r != NULL) {
				char *s = compress_string (buf);
				if ( ! ve_string_empty (s))
					info = g_strdup_printf (_("Last login:\n%s"), s);
				g_free (s);
			}
			VE_IGNORE_EINTR (pclose (fp));
		}
	}

	return info;
}

gboolean
mdm_ok_console_language (void)
{
	int i;
	char **v;
	static gboolean cached = FALSE;
	static gboolean is_ok;
	const char *loc;
	const char *consolecannothandle = mdm_daemon_config_get_value_string (MDM_KEY_CONSOLE_CANNOT_HANDLE);

	if (cached)
		return is_ok;

	/* So far we should be paranoid, we're not set yet */
	if (consolecannothandle == NULL)
		return FALSE;

	cached = TRUE;

	loc = setlocale (LC_MESSAGES, NULL);
	if (loc == NULL) {
		is_ok = TRUE;
		return TRUE;
	}

	is_ok = TRUE;

	v = g_strsplit (consolecannothandle, ",", -1);
	for (i = 0; v != NULL && v[i] != NULL; i++) {
		if ( ! ve_string_empty (v[i]) &&
		    strncmp (v[i], loc, strlen (v[i])) == 0) {
			is_ok = FALSE;
			break;
		}
	}
	if (v != NULL)
		g_strfreev (v);

	return is_ok;
}

const char *
mdm_console_translate (const char *str)
{
	if (mdm_ok_console_language ())
		return _(str);
	else
		return str;
}

/*
 * mdm_read_default
 *
 * This function is used to support systems that have the /etc/default/login
 * interface to control programs that affect security.  This is a Solaris
 * thing, though some users on other systems may find it useful.
 */
gchar *
mdm_read_default (gchar *key)
{
#ifdef HAVE_DEFOPEN
    gchar *retval = NULL;

    if (defopen ("/etc/default/login") == 0) {
       int flags = defcntl (DC_GETFLAGS, 0);

       TURNOFF (flags, DC_CASE);
       (void) defcntl (DC_SETFLAGS, flags);  /* ignore case */
       retval = g_strdup (defread (key));
       (void) defopen ((char *)NULL);
    }
    return retval;
#else
    return NULL;
#endif
}

/**
 * mdm_fail:
 * @format: printf style format string
 * @...: Optional arguments
 *
 * Logs fatal error condition and aborts master daemon.  Also sleeps
 * for 30 seconds to avoid looping if mdm is started by init.
 */
void
mdm_fail (const gchar *format, ...)
{
    va_list args;
    char *s;

    va_start (args, format);
    s = g_strdup_vprintf (format, args);
    va_end (args);

    /* Log to both syslog and stderr */
    mdm_error (s);
    if (getpid () == mdm_main_pid) {
	    mdm_fdprintf (2, "%s\n", s);
    }

    g_free (s);

    /* If main process do final cleanup to kill all processes */
    if (getpid () == mdm_main_pid) {
            mdm_final_cleanup ();
    } else if ( ! mdm_slave_final_cleanup ()) {
            /* If we weren't even a slave do some random cleanup only */
            /* FIXME: is this all fine? */
            mdm_sigchld_block_push ();
            if (extra_process > 1 && extra_process != getpid ()) {
                    /* we sigterm extra processes, and we don't wait */
                    kill (-(extra_process), SIGTERM);
                    extra_process = 0;
            }
            mdm_sigchld_block_pop ();
    }

    /* Slow down respawning if we're started from init */
    if (getppid () == 1)
        sleep (30);

    exit (EXIT_FAILURE);
}

/* EOF */
