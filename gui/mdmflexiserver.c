/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

// The MDM daemon process is launched as root by daemon/mdm.c.
// It sets up a UNIX socket in /var/run/gdm_socket (MDM_SUP_SOCKET) and writes its pid in /var/run/mdm.pid (MDM_PID_FILE).
// MDMFlexiServer is like a remote control... it can be used to send commands with the --command option, or to ask for a new greeter.
// It uses mdmcomm.c to talk with MDM via the socket:
// If --command is used, it sends that command to MDM.
// Otherwise, it asks MDM for a list of already running greeters, with the command "MDM_SUP_ATTACHED_SERVERS".
// If some greeters are already running, it picks the first one, locks the screen and switches the screen to its VT.
// If none are running, it locks the screen and sends MDM the command "MDM_SUP_FLEXI_XSERVER" to start a new greeter.

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <errno.h>
#include <pwd.h>

#include <glib/gi18n.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>

#include <X11/Xauth.h>

#include "mdm.h"
#include "mdmcomm.h"
#include "mdmcommon.h"
#include "mdmconfig.h"

#include "mdm-common.h"
#include "mdm-log.h"
#include "mdm-socket-protocol.h"
#include "mdm-daemon-config-keys.h"
#include "server.h"

static const char *send_command  = NULL;
static char *auth_cookie         = NULL;
static gboolean got_standard     = FALSE;
static gboolean debug_in         = FALSE;
static gboolean authenticate     = FALSE;
static gboolean no_lock          = FALSE;
static gchar **args_remaining    = NULL; 

GOptionEntry options [] = {
	{ "command", 'c', 0, G_OPTION_ARG_STRING, &send_command, N_("Send the specified protocol command to MDM"), N_("COMMAND") },
	{ "no-lock", 'l', 0, G_OPTION_ARG_NONE, &no_lock, N_("Do not lock current screen"), NULL },
	{ "debug", 'd', 0, G_OPTION_ARG_NONE, &debug_in, N_("Debugging output"), NULL },
	{ "authenticate", 'a', 0, G_OPTION_ARG_NONE, &authenticate, N_("Authenticate before running --command"), NULL },
	{ G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_STRING_ARRAY, &args_remaining, NULL, NULL },
	{ NULL }
};

static int
get_cur_vt (void)
{
	char           *result;
	static int      cur_vt;
	static gboolean checked = FALSE;
	char           *ret = NULL;

	result = NULL;

	if (checked) {
		return cur_vt;
	}

	ret = mdmcomm_send_cmd_to_daemon_with_args (MDM_SUP_QUERY_VT, auth_cookie, 5);
	if (ve_string_empty (ret) || strncmp (ret, "OK ", 3) != 0) {
		goto out;
	}

	if (sscanf (ret, "OK %d", &cur_vt) != 1) {
		cur_vt = -1;
	}

	checked = TRUE;

 out:
	g_free (ret);

	return cur_vt;
}

static GtkWidget *
hig_dialog_new (GtkWindow      *parent,
		GtkDialogFlags flags,
		GtkMessageType type,
		GtkButtonsType buttons,
		const gchar    *primary_message,
		const gchar    *secondary_message)
{
	GtkWidget *dialog;

	dialog = gtk_message_dialog_new (GTK_WINDOW (parent),
		                         GTK_DIALOG_DESTROY_WITH_PARENT,
		                         type,
		                         buttons,
		                         "%s", primary_message);

	gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog),
		                                  "%s", secondary_message);

	gtk_window_set_title (GTK_WINDOW (dialog), "");
	gtk_container_set_border_width (GTK_CONTAINER (dialog), 5);
	gtk_box_set_spacing (GTK_BOX (GTK_DIALOG (dialog)->vbox), 14);

  	return dialog;
}

/* change to an existing vt */
static void
change_vt (int vt)
{
	char *cmd;
	char *ret;

	cmd = g_strdup_printf (MDM_SUP_SET_VT " %d", vt);
	ret = mdmcomm_send_cmd_to_daemon_with_args (cmd, auth_cookie, 5);
	g_free (cmd);

	if (ve_string_empty (ret) ||
	    strcmp (ret, "OK") != 0) {
		GtkWidget *dialog;
		const char *message = mdmcomm_get_error_message (ret);

		dialog = hig_dialog_new (NULL /* parent */,
					 GTK_DIALOG_MODAL /* flags */,
					 GTK_MESSAGE_ERROR,
					 GTK_BUTTONS_OK,
					 _("Cannot change display"),
					 message);
		gtk_widget_show_all (dialog);
		gtk_dialog_run (GTK_DIALOG (dialog));
		gtk_widget_destroy (dialog);
	}
	g_free (ret);
}

static int
get_vt_num (char **vec, char *vtpart, int depth)
{
	int i;

	if (ve_string_empty (vtpart) || depth <= 0)
		return -1;

	if (strchr (vtpart, ':') == NULL)
		return atoi (vtpart);

	for (i = 0; vec[i] != NULL; i++) {
		char **rvec;
		rvec = g_strsplit (vec[i], ",", -1);
		if (mdm_vector_len (rvec) != 3) {
			g_strfreev (rvec);
			continue;
		}

		if (strcmp (rvec[0], vtpart) == 0) {
			/* could be nested? */
			int r = get_vt_num (vec, rvec[2], depth-1);
			g_strfreev (rvec);
			return r;

		}

		g_strfreev (rvec);
	}
	return -1;
}

static gboolean
is_program_in_path (const char *program)
{
	char *tmp = g_find_program_in_path (program);
	if (tmp != NULL) {
		g_free (tmp);
		return TRUE;
	} else {
		return FALSE;
	}
}

static void
maybe_lock_screen (void)
{	
	GError    *error            = NULL;
	char      *command;
	GdkScreen *screen;
	
	screen = gdk_screen_get_default ();

	const char * desktop = g_getenv ("XDG_CURRENT_DESKTOP") ? g_getenv ("XDG_CURRENT_DESKTOP") : "NONE";

	if (is_program_in_path ("cinnamon-screensaver-command")
		&& (g_strcmp0 (desktop, "X-Cinnamon") == 0 || g_strcmp0 (desktop, "Cinnamon") == 0)) {
		printf ("Locking cinnamon-screensaver\n");
		command = g_strdup ("cinnamon-screensaver-command --lock");
	}
	else if (is_program_in_path ("mate-screensaver-command") && g_strcmp0 (desktop, "MATE") == 0) {
		printf ("Locking mate-screensaver\n");
		command = g_strdup ("mate-screensaver-command --lock");
	}
	else if (is_program_in_path ("xscreensaver-command")) {
		printf ("Locking xscreensaver\n");
		command = g_strdup ("xscreensaver-command -lock");
	}
    else if( access( "/usr/lib/kde4/libexec/kscreenlocker_greet", X_OK ) != -1 ) {
		printf ("Locking kscreenlocker\n");
        command = g_strdup ("/usr/lib/kde4/libexec/kscreenlocker_greet");
    }
    else if (is_program_in_path ("gnome-screensaver-command")) {
		printf ("Locking gnome-screensaver\n");
		command = g_strdup ("gnome-screensaver-command --lock");
	}	
	else {		
		return;	
	}

	if (! gdk_spawn_command_line_on_screen (screen, command, &error)) {		
		g_warning ("Cannot lock screen: %s", error->message);
		g_error_free (error);
	}

	g_free (command);
}

static void
check_for_users (void)
{
	char *result_string;
	char **servers;
	int i;

	// Return if we're not on a VT
	if (auth_cookie == NULL || get_cur_vt () < 0) {
		return;
	}
		
    // Get the list of running servers from the daemon
	result_string = mdmcomm_send_cmd_to_daemon_with_args (MDM_SUP_ATTACHED_SERVERS, auth_cookie, 5);

	// Return if the daemon didn't send us the list
	if (ve_string_empty (result_string) || strncmp (result_string, "OK ", 3) != 0) {
		g_free (result_string);
		return;
	}

	// Place the servers into the servers variable	
	servers = g_strsplit (&result_string[3], ";", -1);
	g_free (result_string);

	// Return if there are no servers running
	if (servers == NULL)
		return;

	// Each server is composed of 3 parts: [display, user, tty]
	for (i = 0; servers[i] != NULL; i++) {
		char **server;
		int vt;
		server = g_strsplit (servers[i], ",", -1);
		if (mdm_vector_len (server) != 3) {
			g_strfreev (server);
			continue;
		}

		// Get the VT of the server
		vt = get_vt_num (servers, server[2], 5);

		// If the server's username is empty, this is a greeter and we want to switch to it
		if (strcmp (server[1], "") == 0 && vt >= 0) {			
			// lock the screen
			if ( ! no_lock && vt != get_cur_vt () && vt >= 0) {
				maybe_lock_screen ();
			}
			// Switch VT
			change_vt (vt);	
			printf ("Switching to MDM server on VT #%d\n", vt);
			exit (0);
		}

		g_strfreev (server);
	}
	
	printf ("Found no MDM server, ordering a new one\n");
	g_strfreev (servers);
}

/**
 * is_key
 *
 * Since MDM keys sometimes have default values defined in the mdm.h header
 * file (e.g. key=value), this function strips off the "=value" from both
 * keys passed in to do a comparison.
 */
static gboolean
is_key (const gchar *key1, const gchar *key2)
{
   gchar *key1d, *key2d, *p;

   key1d = g_strdup (key1);
   key2d = g_strdup (key2);

   g_strstrip (key1d);
   p = strchr (key1d, '=');
   if (p != NULL)
      *p = '\0';

   g_strstrip (key2d);
   p = strchr (key2d, '=');
   if (p != NULL)
      *p = '\0';

   if (strcmp (key1d, key2d) == 0) {
      g_free (key1d);
      g_free (key2d);
      return TRUE;
   } else {
      g_free (key1d);
      g_free (key2d);
      return FALSE;
   }
}

int
main (int argc, char *argv[])
{
	GtkWidget *dialog;
	char *ret;
	const char *message;
	GOptionContext *ctx;

	bindtextdomain (GETTEXT_PACKAGE, GNOMELOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);

	/* Option parsing */
	ctx = g_option_context_new ("- New mdm login");
	g_option_context_add_main_entries (ctx, options, _("main options"));
	g_option_context_parse (ctx, &argc, &argv, NULL);
	g_option_context_free (ctx);

	mdm_log_init ();
	mdm_log_set_debug (debug_in);

	if (send_command != NULL) {
		if ( ! mdmcomm_is_daemon_running (FALSE)) {
			mdm_common_error (_("Error: MDM (MDM Display Manager) is not running."));
			mdm_common_error (_("You might be using a different display manager."));
			return 1;
		}
	} else {
		/*
		 * The --command argument does not display anything, so avoid
		 * running gtk_init until it finishes.  Sometimes the
		 * --command argument is used when there is no display so it
		 * will fail and cause the program to exit, complaining about
		 * "no display".
		 */
		gtk_init (&argc, &argv);

		if ( ! mdmcomm_is_daemon_running (TRUE)) {
			return 1;
		}
	}
	
	mdmcomm_open_connection_to_daemon ();

	/* Process --command option */

	if (send_command != NULL) {

		/* gdk_init is needed for cookie code to get display */
		gdk_init (&argc, &argv);
		if (authenticate)
			auth_cookie = mdmcomm_get_auth_cookie ();

		/*
		 * If asking for a translatable config value, then try to get
		 * the translated value first.  If this fails, then go ahead
		 * and call the normal sockets command.
		 */
		if (strncmp (send_command, MDM_SUP_GET_CONFIG " ",
		    strlen (MDM_SUP_GET_CONFIG " ")) == 0) {
			gchar *value = NULL;
			const char *key = &send_command[strlen (MDM_SUP_GET_CONFIG " ")];

			if (is_key (MDM_KEY_WELCOME, key)) {
				value = mdm_config_get_translated_string ((gchar *)key);
				if (value != NULL) {
					ret = g_strdup_printf ("OK %s", value);
				}
			}

			/*
			 * If the above didn't return a value, then must be a
			 * different key, so call the daemon.
			 */
			if (value == NULL)
				ret = mdmcomm_send_cmd_to_daemon_with_args (send_command, auth_cookie, 5);
		} else {
			ret = mdmcomm_send_cmd_to_daemon_with_args (send_command, auth_cookie, 5);
		}

		/* At this point we are done using the socket, so close it */
		mdmcomm_close_connection_to_daemon ();

		if (ret != NULL) {
			g_print ("%s\n", ret);
			return 0;
		} else {
			dialog = hig_dialog_new (NULL /* parent */,
						 GTK_DIALOG_MODAL /* flags */,
						 GTK_MESSAGE_ERROR,
						 GTK_BUTTONS_OK,
						 _("Cannot communicate with MDM "
						   "(The MDM Display Manager)"),
						 _("Perhaps you have an old version "
						   "of MDM running."));
			gtk_widget_show_all (dialog);
			gtk_dialog_run (GTK_DIALOG (dialog));
			gtk_widget_destroy (dialog);
			return 1;
		}
	}

	/*
	 * Always attempt to get cookie and authenticate.  On remote
	 * servers
	 */
	auth_cookie = mdmcomm_get_auth_cookie ();
	
	/* check for other displays/logged in users */
	check_for_users ();

	if (auth_cookie == NULL) {

		/* At this point we are done using the socket, so close it */
		mdmcomm_close_connection_to_daemon ();

		dialog = hig_dialog_new (NULL /* parent */,
					 GTK_DIALOG_MODAL /* flags */,
					 GTK_MESSAGE_ERROR,
					 GTK_BUTTONS_OK,
					 _("You do not seem to be logged in on the "
					   "console"),
					 _("Starting a new login only "
					   "works correctly on the console."));
		gtk_dialog_set_has_separator (GTK_DIALOG (dialog),
					      FALSE);
		gtk_widget_show_all (dialog);
		gtk_dialog_run (GTK_DIALOG (dialog));
		gtk_widget_destroy (dialog);
		return 1;
	}
	
	ret = mdmcomm_send_cmd_to_daemon_with_args (MDM_SUP_FLEXI_XSERVER, auth_cookie, 5);

	g_free (auth_cookie);
	g_strfreev (args_remaining);

	/* At this point we are done using the socket, so close it */
	mdmcomm_close_connection_to_daemon ();

	if (ret != NULL &&
	    strncmp (ret, "OK ", 3) == 0) {

		/* if we switched to a different screen as a result of this,
		 * lock the current screen */
		if ( ! no_lock ) {
			maybe_lock_screen ();
		}

		/* all fine and dandy */
		g_free (ret);
		return 0;
	}

	message = mdmcomm_get_error_message (ret);

	dialog = hig_dialog_new (NULL /* parent */,
				 GTK_DIALOG_MODAL /* flags */,
				 GTK_MESSAGE_ERROR,
				 GTK_BUTTONS_OK,
				 _("Cannot start new display"),
				 message);

	gtk_widget_show_all (dialog);
	gtk_dialog_run (GTK_DIALOG (dialog));
	gtk_widget_destroy (dialog);
	g_free (ret);

	return 1;
}


