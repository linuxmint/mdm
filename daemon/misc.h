/* MDM - The MDM Display Manager
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

#ifndef MDM_MISC_H
#define MDM_MISC_H

#include <stdio.h>
#include <sys/types.h>

#include "mdm.h"
#include "display.h"

void mdm_fail   (const gchar *format, ...) G_GNUC_PRINTF (1, 2);

void mdm_fdprintf  (int fd, const gchar *format, ...) G_GNUC_PRINTF (2, 3);
int mdm_fdgetc     (int fd);
char *mdm_fdgets   (int fd);

/* clear environment, but keep the i18n ones (LANG, LC_ALL, etc...),
 * note that this leak memory so only use before exec */
void mdm_clearenv_no_lang (void);

int mdm_get_free_display (int start, uid_t server_uid);

gboolean mdm_text_message_dialog (const char *msg);
gboolean mdm_text_yesno_dialog (const char *msg, gboolean *ret);
int	mdm_exec_wait (char * const *argv, gboolean no_display,
		       gboolean de_setuid);

/* done before each login.  This can do so sanity ensuring,
 * one of the things it does now is make sure /tmp/.ICE-unix
 * exists and has the correct permissions */
void	mdm_ensure_sanity	(void);

/* This is for race free forks */
void	mdm_sigchld_block_push (void);
void	mdm_sigchld_block_pop (void);
void	mdm_sigterm_block_push (void);
void	mdm_sigterm_block_pop (void);
void	mdm_sigusr2_block_push (void);
void	mdm_sigusr2_block_pop (void);

pid_t	mdm_fork_extra (void);
void	mdm_wait_for_extra (pid_t pid, int *status);

const GList * mdm_address_peek_local_list (void);
gboolean      mdm_address_is_local        (struct sockaddr_storage *sa);

gboolean mdm_setup_gids (const char *login, gid_t gid);

void mdm_desetuid (void);

gboolean mdm_test_opt (const char *cmd, const char *help, const char *option);

void mdm_close_all_descriptors (int from, int except, int except2);

int mdm_open_dev_null (mode_t mode);

void mdm_unset_signals (void);

void mdm_saveenv (void);
const char * mdm_saved_getenv (const char *var);
/* leaks */
void mdm_restoreenv (void);

/* like fopen with "w" but unlinks and uses O_EXCL */
FILE * mdm_safe_fopen_w (const char *file, mode_t perm);
/* like fopen with "a+" and uses O_EXCL and O_NOFOLLOW */
FILE * mdm_safe_fopen_ap (const char *file, mode_t perm);

/* first must get initial limits before attempting to ever reset those
   limits */
void mdm_get_initial_limits (void);
void mdm_reset_limits (void);
void mdm_reset_locale (void);

const char *mdm_root_user (void);

#include <setjmp.h>

/* stolen from xdm sources */
#if defined(X_NOT_POSIX) || defined(__EMX__) || defined(__NetBSD__) && defined(__sparc__)
#define Setjmp(e)	setjmp(e)
#define Longjmp(e,v)	longjmp(e,v)
#define Jmp_buf		jmp_buf
#else
#define Setjmp(e)   sigsetjmp(e,1)
#define Longjmp(e,v)	siglongjmp(e,v)
#define Jmp_buf		sigjmp_buf
#endif

void mdm_signal_ignore (int signal);
void mdm_signal_default (int signal);

void mdm_sleep_no_signal (int secs);

/* somewhat like g_build_filename, but does somet hing like
 * <dir> "/" <name> <extension>
 */
char * mdm_make_filename (const char *dir, const char *name, const char *extension);
char * mdm_ensure_extension (const char *name, const char *extension);
char * mdm_strip_extension (const char *name, const char *extension);

void mdm_twiddle_pointer (MdmDisplay *disp);

char * mdm_get_last_info (const char *username);

gboolean mdm_ok_console_language (void);
const char * mdm_console_translate (const char *str);
/* Use with C_(N_("foo")) to make gettext work it out right */
#define C_(x) (mdm_console_translate(x))

gchar * mdm_read_default (gchar *key);

#define NEVER_FAILS_seteuid(uid) \
	{ int r = 0; \
	  if (geteuid () != uid) \
	    r = seteuid (uid); \
	  if G_UNLIKELY (r != 0) \
        mdm_fail ("MDM file %s: line %d (%s): Cannot run seteuid to %d: %s", \
		  __FILE__,						\
		  __LINE__,						\
		  G_GNUC_PRETTY_FUNCTION,					\
                  (int)uid,						\
		  strerror (errno));			}
#define NEVER_FAILS_setegid(gid) \
	{ int r = 0; \
	  if (getegid () != gid) \
	    r = setegid (gid); \
	  if G_UNLIKELY (r != 0) \
        mdm_fail ("MDM file %s: line %d (%s): Cannot run setegid to %d: %s", \
		  __FILE__,						\
		  __LINE__,						\
		  G_GNUC_PRETTY_FUNCTION,					\
                  (int)gid,						\
		  strerror (errno));			}

/* first goes to euid-root and then sets the egid and euid, to make sure
 * this succeeds */
#define NEVER_FAILS_root_set_euid_egid(uid,gid) \
	{ NEVER_FAILS_seteuid (0); \
	  NEVER_FAILS_setegid (gid); \
	  if (uid != 0) { NEVER_FAILS_seteuid (uid); } }

#endif /* MDM_MISC_H */
