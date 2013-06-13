/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 *    MDMflexiserver - run a flexible server
 *    (c)2001 Queen of England
 *
 *    This program is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program; if not, write to the Free Software
 *    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

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

static GSList *xservers          = NULL;
static const char *send_command  = NULL;
static const char *server        = NULL;
static const char *chosen_server = NULL;
static char *auth_cookie         = NULL;
static gboolean got_standard     = FALSE;
static gboolean use_xnest        = FALSE;
static gboolean debug_in         = FALSE;
static gboolean authenticate     = FALSE;
static gboolean no_lock          = FALSE;
static gboolean monte_carlo_pi   = FALSE;
static gboolean startnew         = FALSE;
static gchar **args_remaining    = NULL; 

GOptionEntry options [] = {
	{ "command", 'c', 0, G_OPTION_ARG_STRING, &send_command, N_("Send the specified protocol command to MDM"), N_("COMMAND") },
	{ "xnest", 'n', 0, G_OPTION_ARG_NONE, &use_xnest, N_("Xnest mode"), NULL },
	{ "no-lock", 'l', 0, G_OPTION_ARG_NONE, &no_lock, N_("Do not lock current screen"), NULL },
	{ "debug", 'd', 0, G_OPTION_ARG_NONE, &debug_in, N_("Debugging output"), NULL },
	{ "authenticate", 'a', 0, G_OPTION_ARG_NONE, &authenticate, N_("Authenticate before running --command"), NULL },
	{ "startnew", 's', 0, G_OPTION_ARG_NONE, &startnew, N_("Start new flexible session; do not show popup"), NULL },
	{ "monte-carlo-pi", 0, 0, G_OPTION_ARG_NONE, &monte_carlo_pi, NULL, NULL },
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

	ret = mdmcomm_call_mdm ("QUERY_VT", auth_cookie, "1.0.0.0", 5);
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
	ret = mdmcomm_call_mdm (cmd, auth_cookie, "1.0.0.0", 5);
	g_free (cmd);

	if (ve_string_empty (ret) ||
	    strcmp (ret, "OK") != 0) {
		GtkWidget *dialog;
		const char *message = mdmcomm_get_error_message (ret, use_xnest);

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

enum {
	COLUMN_LOGIN /* human string */,
	COLUMN_DISPLAY /* human string */,
	COLUMN_VT /* vt number */,
	COLUMN_NUM
};

static GtkTreeModel *
create_model (char **vec)
{
	int i;
	GtkListStore *store;
	GtkTreeIter iter;

	/* create list store */
	store = gtk_list_store_new (COLUMN_NUM,
				    G_TYPE_STRING /* login */,
				    G_TYPE_STRING /* display */,
				    G_TYPE_INT /* vt */);

	for (i = 0; vec[i] != NULL; i++) {
		char **rvec;
		int vt;
		rvec = g_strsplit (vec[i], ",", -1);
		if (mdm_vector_len (rvec) != 3) {
			g_strfreev (rvec);
			continue;
		}

		vt = get_vt_num (vec, rvec[2], 5);

		if (strcmp (rvec[0], mdmcomm_get_display ()) != 0 &&
		    vt >= 0) {
			char *user;
			char *disp;

			if (ve_string_empty (rvec[1])) {
				user = g_strdup (_("Nobody"));
			} else {
				struct passwd *pw = getpwnam (rvec[1]);
				if (pw == NULL ||
				    ve_string_empty (pw->pw_gecos)) {
					char *login;
					login = g_markup_escape_text (rvec[1], -1);

					user = g_strdup_printf ("<b>%s</b>",
								login);

					g_free (login);
				} else {
					char *utf8gecos;
					char *gecos, *login;
					login = g_markup_escape_text (rvec[1], -1);
					if ( ! g_utf8_validate (pw->pw_gecos, -1, NULL))
						utf8gecos = ve_locale_to_utf8 (pw->pw_gecos);
					else
						utf8gecos = g_strdup (pw->pw_gecos);

					gecos = g_markup_escape_text (utf8gecos, -1);

					user = g_strdup_printf ("<b>%s</b>\n%s",
								login,
								gecos);

					g_free (login);
					g_free (gecos);
					g_free (utf8gecos);
				}
			}

			if (strchr (rvec[2], ':') == NULL) {
				disp = g_strdup_printf
					(_("Display %s on virtual "
					   "terminal %d"),
					 rvec[0], vt);
			} else {
				disp = g_strdup_printf
					(_("Nested display %s on virtual "
					   "terminal %d"),
					 rvec[0], vt);
			}

			/* this is not the current display */
			gtk_list_store_append (store, &iter);
			gtk_list_store_set (store, &iter,
					    COLUMN_LOGIN, user,
					    COLUMN_DISPLAY, disp,
					    COLUMN_VT, vt,
					    -1);

			g_free (user);
			g_free (disp);
		}

		g_strfreev (rvec);
	}

	return GTK_TREE_MODEL (store);
}

static void
add_columns (GtkTreeView *treeview)
{
	GtkCellRenderer *renderer;
	GtkTreeViewColumn *column;

	renderer = gtk_cell_renderer_text_new ();
	column = gtk_tree_view_column_new_with_attributes (_("Username"),
							   renderer,
							   "markup",
							   COLUMN_LOGIN,
							   NULL);
	gtk_tree_view_column_set_sort_column_id (column, COLUMN_LOGIN);
	gtk_tree_view_append_column (treeview, column);

	renderer = gtk_cell_renderer_text_new ();
	column = gtk_tree_view_column_new_with_attributes (_("Display"),
							   renderer,
							   "text",
							   COLUMN_DISPLAY,
							   NULL);
	gtk_tree_view_column_set_sort_column_id (column, COLUMN_DISPLAY);
	gtk_tree_view_append_column (treeview, column);
}

enum {
	RESPONSE_OPEN_NEW_DISPLAY,
	RESPONSE_OPEN_EXISTING_DISPLAY
};

static void
selection_changed (GtkTreeSelection *selection, gpointer data)
{
	GtkWidget *dialog = data;
	GtkTreeIter iter;

	if (gtk_tree_selection_get_selected (selection, NULL, &iter)) {
		gtk_dialog_set_response_sensitive (GTK_DIALOG (dialog),
						   RESPONSE_OPEN_EXISTING_DISPLAY,
						   TRUE);
	} else {
		gtk_dialog_set_response_sensitive (GTK_DIALOG (dialog),
						   RESPONSE_OPEN_EXISTING_DISPLAY,
						   FALSE);
	}
}

static void
row_activated (GtkTreeView *view, GtkTreePath *path, GtkTreeViewColumn *col, gpointer data)
{
	GtkWidget *dialog = data;
	GtkTreeSelection *selection;

	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (view));
	gtk_tree_selection_select_path (selection, path);
	gtk_dialog_response (GTK_DIALOG (dialog), RESPONSE_OPEN_EXISTING_DISPLAY);
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
	gboolean   use_xscreensaver = FALSE;		
	gboolean   call_multiple_screensaver = FALSE;		
	GError    *error            = NULL;
	char      *command;
	GdkScreen *screen;
	
	screen = gdk_screen_get_default ();
	
	if (is_program_in_path ("gnome-screensaver-command") || is_program_in_path ("mate-screensaver-command")) {
		command = g_strdup ("gnome-screensaver-command --lock || mate-screensaver-command --lock");
		call_multiple_screensaver = TRUE;
	}
	if (is_program_in_path ("gnome-screensaver-command")) {
		command = g_strdup ("gnome-screensaver-command --lock");
	}
	else if (is_program_in_path ("mate-screensaver-command")) {
		command = g_strdup ("mate-screensaver-command --lock");
	}
	else if (is_program_in_path ("xscreensaver-command")) {
		command = g_strdup ("xscreensaver-command -lock");
		use_xscreensaver = TRUE;
	}
     else if( access( "/usr/lib/kde4/libexec/kscreenlocker_greet", X_OK ) != -1 ) { 
          command = g_strdup ("/usr/lib/kde4/libexec/kscreenlocker_greet");
     }	
	else {		
		return;	
	}

	if (! gdk_spawn_command_line_on_screen (screen, command, &error)) {
		if (! call_multiple_screensaver) {
			g_warning ("Cannot lock screen: %s", error->message);
		}
		g_error_free (error);
	}

	g_free (command);

	if (use_xscreensaver) {
		command = g_strdup ("xscreensaver-command -throttle");
		if (! gdk_spawn_command_line_on_screen (screen, command, &error)) {
			g_warning ("Cannot disable screensaver engines: %s", error->message);
			g_error_free (error);
		}

		g_free (command);
	}
}

static void
run_logged_in_dialogue (char **vec)
{
	GtkWidget *dialog;
	GtkWidget *vbox;
	GtkWidget *w;
	GtkWidget *sw;
	GtkTreeModel *model;
	GtkWidget *treeview;
	GtkTreeIter iter;
	GtkTreeSelection *selection;
	gint response;

	if (startnew == TRUE) {
		/* Just return if the user doesn't want to see the dialog */
		return;
	}

	dialog = gtk_dialog_new_with_buttons (_("Open Displays"),
					      NULL /* parent */,
					      0 /* flags */,
					      _("_Open New Display"),
					      RESPONSE_OPEN_NEW_DISPLAY,
					      _("Change to _Existing Display"),
					      RESPONSE_OPEN_EXISTING_DISPLAY,
					      GTK_STOCK_CANCEL,
					      GTK_RESPONSE_CANCEL,
					      NULL);
	gtk_dialog_set_has_separator (GTK_DIALOG (dialog), FALSE);
	vbox = GTK_DIALOG (dialog)->vbox;

	w = gtk_label_new (_("There are some displays already open.  You can select "
		     "one from the list below or open a new one."));
	gtk_box_pack_start (GTK_BOX (vbox), w, FALSE, FALSE, 0);

	sw = gtk_scrolled_window_new (NULL, NULL);
	gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (sw),
					     GTK_SHADOW_ETCHED_IN);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sw),
					GTK_POLICY_NEVER,
					GTK_POLICY_AUTOMATIC);
	gtk_box_pack_start (GTK_BOX (vbox), sw, TRUE, TRUE, 0);

	/* create tree model */
	model = create_model (vec);

	/* create tree view */
	treeview = gtk_tree_view_new_with_model (model);
	gtk_tree_view_set_rules_hint (GTK_TREE_VIEW (treeview), TRUE);

	g_object_unref (model);

	gtk_container_add (GTK_CONTAINER (sw), treeview);

	/* add columns to the tree view */
	add_columns (GTK_TREE_VIEW (treeview));

	/* finish & show */
	gtk_window_set_default_size (GTK_WINDOW (dialog), 280, 250);

        g_signal_connect (G_OBJECT (treeview), "row_activated",
			  G_CALLBACK (row_activated),
			  dialog);

	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (treeview));

	g_signal_connect (selection, "changed",
			  G_CALLBACK (selection_changed),
			  dialog);

	gtk_dialog_set_response_sensitive (GTK_DIALOG (dialog),
				   RESPONSE_OPEN_EXISTING_DISPLAY,
				   FALSE);

	gtk_widget_show_all (dialog);
	response = gtk_dialog_run (GTK_DIALOG (dialog));

run_again:
	switch (response) {
	case RESPONSE_OPEN_NEW_DISPLAY:
		gtk_widget_destroy (dialog);

		/* just continue what you are doing */
		return;

	case RESPONSE_OPEN_EXISTING_DISPLAY:
		if (gtk_tree_selection_get_selected (selection, NULL, &iter)) {
			GValue value = {0};
			int vt;
			gtk_tree_model_get_value (model, &iter,
						  COLUMN_VT,
						  &value);
			vt = g_value_get_int (&value);
			g_value_unset (&value);

			/* we switched to a different screen as a result of this,
			 * lock the current screen */
			if ( ! no_lock && vt != get_cur_vt () && vt >= 0) {
				maybe_lock_screen ();
			}

			change_vt (vt);

			/* FIXME: wait + disturb the pointer (need SUP?), 
			 * perhaps part of the sup command to CHVT ?? */

			exit (0);
		} else {
			/* EEK */
			goto run_again;
		}
		break;

	default:
		gtk_widget_destroy (dialog);
		/* cancel, or close */
		exit (0);
		break;
	}
}

static void
check_for_users (void)
{
	char *ret;
	char **vec;
	int i;
	int extra;

	/* only for console logins on vt supporting systems */
	if (auth_cookie == NULL ||
	    get_cur_vt () < 0)
		return;

	ret = mdmcomm_call_mdm ("CONSOLE_SERVERS", auth_cookie, "1.0.0.0", 5);
	if (ve_string_empty (ret) ||
	    strncmp (ret, "OK ", 3) != 0) {
		g_free (ret);
		return;
	}

	vec = g_strsplit (&ret[3], ";", -1);
	g_free (ret);
	if (vec == NULL)
		return;

	extra = 0;

	for (i = 0; vec[i] != NULL; i++) {
		char **rvec;
		int vt;
		rvec = g_strsplit (vec[i], ",", -1);
		if (mdm_vector_len (rvec) != 3) {
			g_strfreev (rvec);
			continue;
		}

		vt = get_vt_num (vec, rvec[2], 5);

		if (strcmp (rvec[0], mdmcomm_get_display ()) != 0 &&
		    vt >= 0) {
			/* this is not the current display */
			extra++;
		}

		g_strfreev (rvec);
	}

	if (extra == 0) {
		g_strfreev (vec);
		return;
	}

	run_logged_in_dialogue (vec);

	g_strfreev (vec);
}

static void
read_servers (void)
{
	GSList *li;

        xservers = mdm_config_get_xservers (TRUE);

	for (li = xservers; li != NULL; li = li->next) {
		MdmXserver *svr = li->data;

		if (strcmp (svr->id, MDM_STANDARD) == 0)
			got_standard = TRUE;

		if (server != NULL &&
		    strcmp (svr->id, server) == 0)
			chosen_server = g_strdup (svr->id);
	}
}

static char *
choose_server (void)
{
	GtkWidget *dialog, *vbox;
	GtkWidget *w;
	GSList *group = NULL;
	GSList *li;

	if (chosen_server != NULL)
		return g_strdup (chosen_server);

	if (xservers == NULL)
		return NULL;

	if (xservers->next == NULL &&
	    got_standard)
		return g_strdup (MDM_STANDARD);

	dialog = gtk_dialog_new_with_buttons (_("Choose server"),
					      NULL /* parent */,
					      0 /* flags */,
					      GTK_STOCK_OK,
					      GTK_RESPONSE_OK,
					      GTK_STOCK_CANCEL,
					      GTK_RESPONSE_CANCEL,
					      NULL);
	gtk_dialog_set_has_separator (GTK_DIALOG (dialog), FALSE);
	vbox = GTK_DIALOG (dialog)->vbox;

	w = gtk_label_new (_("Choose the X server to start"));
	gtk_box_pack_start (GTK_BOX (vbox), w, FALSE, FALSE, 0);

	group = NULL;
	if ( ! got_standard) {
		w = gtk_radio_button_new_with_label (group,
						     _("Standard server"));
		gtk_box_pack_start (GTK_BOX (vbox), w, FALSE, FALSE, 0);
		gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (w), TRUE);
		group = gtk_radio_button_get_group (GTK_RADIO_BUTTON (w));
	}

	for (li = xservers; li != NULL; li = li->next) {
		MdmXserver *svr = li->data;
		w = gtk_radio_button_new_with_label
			(group, svr->name ? svr->name : svr->id);
		gtk_box_pack_start (GTK_BOX (vbox), w, FALSE, FALSE, 0);
		if (got_standard &&
		    strcmp (svr->id, MDM_STANDARD) == 0)
			gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (w),
						      TRUE);
		g_object_set_data (G_OBJECT (w), "ServerID", svr->id);
		group = gtk_radio_button_get_group (GTK_RADIO_BUTTON (w));
	}

	gtk_widget_show_all (dialog);

	switch (gtk_dialog_run (GTK_DIALOG (dialog))) {
	case GTK_RESPONSE_OK:	
		/* OK */
		break;
	default:
		gtk_widget_destroy (dialog);
		/* cancel, or close */
		exit (0);
		break;
	}

	for (li = group; li != NULL; li = li->next) {
		GtkWidget *w = li->data;
		char *name = g_object_get_data (G_OBJECT (w), "ServerID");
		if (GTK_TOGGLE_BUTTON (w)->active) {
			gtk_widget_destroy (dialog);
			return g_strdup (name);
		}
	}

	gtk_widget_destroy (dialog);

	/* should never get here really */
	return NULL;
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

static void
calc_pi (void)
{
	unsigned long n = 0, h = 0;
	double x, y;
	printf ("\n");
	for (;;) {
		x = g_random_double ();
		y = g_random_double ();
		if (x*x + y*y <= 1)
			h++;
		n++;
		if ( ! (n & 0xfff))
			printf ("pi ~~ %1.10f\t(%lu/%lu * 4) iteration: %lu \r",
				((double)h)/(double)n * 4.0, h, n, n);
	}
}

int
main (int argc, char *argv[])
{
	GtkWidget *dialog;
	char *command;
	char *version;
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

	if (monte_carlo_pi) {
		calc_pi ();
		return 0;
	}

	mdm_log_init ();
	mdm_log_set_debug (debug_in);

	if (args_remaining != NULL && args_remaining[0] != NULL)
		server = args_remaining[0];

	if (send_command != NULL) {
		if ( ! mdmcomm_check (FALSE)) {
			mdm_common_error (_("Error: MDM (GNOME Display Manager) is not running."));
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

		if ( ! mdmcomm_check (TRUE)) {
			return 1;
		}
	}

	/* Start reading config data in bulk */
	mdmcomm_comm_bulk_start ();

	/* Process --command option */

	g_type_init ();

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

			if (is_key (MDM_KEY_WELCOME, key) ||
			    is_key (MDM_KEY_REMOTE_WELCOME, key)) {
				value = mdm_config_get_translated_string ((gchar *)key);
				if (value != NULL) {
					ret = g_strdup_printf ("OK %s", value);
				}
			}

			/*
			 * If the above didn't return a value, then must be a
			 * different key, so call mdmcomm_call_mdm.
			 */
			if (value == NULL)
				ret = mdmcomm_call_mdm (send_command, auth_cookie,
							"1.0.0.0", 5);
		} else {
			ret = mdmcomm_call_mdm (send_command, auth_cookie,
						"1.0.0.0", 5);
		}

		/* At this point we are done using the socket, so close it */
		mdmcomm_comm_bulk_stop ();

		if (ret != NULL) {
			g_print ("%s\n", ret);
			return 0;
		} else {
			dialog = hig_dialog_new (NULL /* parent */,
						 GTK_DIALOG_MODAL /* flags */,
						 GTK_MESSAGE_ERROR,
						 GTK_BUTTONS_OK,
						 _("Cannot communicate with MDM "
						   "(The GNOME Display Manager)"),
						 _("Perhaps you have an old version "
						   "of MDM running."));
			gtk_widget_show_all (dialog);
			gtk_dialog_run (GTK_DIALOG (dialog));
			gtk_widget_destroy (dialog);
			return 1;
		}
	}

	/*
	 * Now process what mdmflexiserver is more frequently used to
	 * do, start VT (Virtual Terminal) sesions - at least on
	 * systems where it is supported.  On systems where it is not
	 * supporteed VT stands for "Very Tight" and will mess up your
	 * display if you use it.  Tight!  So do not use it.
	 *
	 * I would accept a patch to disable it on such systems, but it
	 * is easy to avoid not using it as long as your distro does not
	 * put the menu choice in the application launch button on the
	 * panel (don't ship the desktop file).
	 */

	/*
	 * Always attempt to get cookie and authenticate.  On remote
	 * servers
	 */
	auth_cookie = mdmcomm_get_auth_cookie ();

	if (use_xnest) {
		char *cookie = mdmcomm_get_a_cookie (FALSE /* binary */);

		if (cookie == NULL) {

			/* At this point we are done using the socket, so close it */
			mdmcomm_comm_bulk_stop ();

			dialog = hig_dialog_new (NULL /* parent */,
						 GTK_DIALOG_MODAL /* flags */,
						 GTK_MESSAGE_ERROR,
						 GTK_BUTTONS_OK,
						 _("You do not seem to have the "
						   "authentication needed for this "
						   "operation"),
						 _("Perhaps your .Xauthority "
						   "file is not set up correctly."));
			gtk_widget_show_all (dialog);
			gtk_dialog_run (GTK_DIALOG (dialog));
			gtk_widget_destroy (dialog);
			return 1;
		}
		command = g_strdup_printf (MDM_SUP_FLEXI_XNEST " %s %d %s %s",
					   mdmcomm_get_display (),
					   (int)getuid (),
					   cookie,
					   XauFileName ());
		g_free (cookie);
		version = "1.0.0.0";
		auth_cookie = NULL;
	} else {

		/* check for other displays/logged in users */
		check_for_users ();

		if (auth_cookie == NULL) {

			/* At this point we are done using the socket, so close it */
			mdmcomm_comm_bulk_stop ();

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

		read_servers ();
		server = choose_server ();
		if (server == NULL)
			command = g_strdup (MDM_SUP_FLEXI_XSERVER);
		else
			command = g_strdup_printf (MDM_SUP_FLEXI_XSERVER " %s",
						   server);
		version = "1.0.0.0";
	}

	ret = mdmcomm_call_mdm (command, auth_cookie, version, 5);
	g_free (command);
	g_free (auth_cookie);
	g_strfreev (args_remaining);

	/* At this point we are done using the socket, so close it */
	mdmcomm_comm_bulk_stop ();

	if (ret != NULL &&
	    strncmp (ret, "OK ", 3) == 0) {

		/* if we switched to a different screen as a result of this,
		 * lock the current screen */
		if ( ! no_lock && ! use_xnest) {
			maybe_lock_screen ();
		}

		/* all fine and dandy */
		g_free (ret);
		return 0;
	}

	message = mdmcomm_get_error_message (ret, use_xnest);

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

/* Used for torture testing the socket */
#if 0
static void
torture (void)
{
	struct sockaddr_un addr;
	int fd;
	int i;
	int times;

	srand (getpid () * time (NULL));

	fd = socket (AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		return;
	}

	strcpy (addr.sun_path, "/tmp/.mdm_socket");
	addr.sun_family = AF_UNIX;

	if (connect (fd, &addr, sizeof (addr)) < 0) {
		close (fd);
		return;
	}

	g_print ("OPEN ");

	times = rand () % 500;
	for (i = 0; i < rand () % 500; i++) {
		int len = rand () % 5000;
		char *buf = g_new (char, len);
		int ii;
		for (ii = 0; ii < len; ii++)
			buf[ii] = rand () % 256;
		write (fd, buf, len); 
		g_free (buf);
		g_print ("SENT(%d) ", len);
	}

	close (fd);
}

static void
torture_test (void)
{
	int i;

	srand (getpid () * time (NULL));

	for (i = 0; i < 500; i++) {
		if (fork () == 0) {
			torture ();
			_exit (0);
		}
		usleep (1000);
	}
}
#endif
