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

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <signal.h>
#include <dirent.h>
#include <locale.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/XKBlib.h>

#include <glib/gi18n.h>
#include <gdk/gdkx.h>
#include <gdk/gdkkeysyms.h>

#if HAVE_PAM
#include <security/pam_appl.h>
#define PW_ENTRY_SIZE PAM_MAX_RESP_SIZE
#else
#define PW_ENTRY_SIZE MDM_MAX_PASS
#endif

#include "mdm.h"
#include "mdmuser.h"
#include "mdmcomm.h"
#include "mdmcommon.h"
#include "mdmsession.h"
#include "mdmlanguages.h"
#include "mdmwm.h"
#include "mdmconfig.h"
#include "misc.h"

#include "mdm-common.h"
#include "mdm-socket-protocol.h"
#include "mdm-daemon-config-keys.h"

/*
 * Set the DOING_MDM_DEVELOPMENT env variable if you aren't running
 * within the protocol
 */
gboolean DOING_MDM_DEVELOPMENT              = FALSE;
gboolean MdmConfiguratorFound               = FALSE;
gboolean MdmSuspendFound                    = FALSE;
gboolean MdmHaltFound                       = FALSE;
gboolean MdmRebootFound                     = FALSE;
static gboolean disable_system_menu_buttons = FALSE;

#define GTK_KEY "gtk-2.0"

enum {
	GREETER_ULIST_ICON_COLUMN = 0,
	GREETER_ULIST_LABEL_COLUMN,
	GREETER_ULIST_LOGIN_COLUMN
};

enum {
	MDM_BACKGROUND_NONE = 0,
	MDM_BACKGROUND_IMAGE_AND_COLOR = 1,
	MDM_BACKGROUND_COLOR = 2,
	MDM_BACKGROUND_IMAGE = 3,
};

static GtkWidget *login;
static GtkWidget *table = NULL;
static GtkWidget *welcome;
static GtkWidget *label;
static GtkWidget *icon_button = NULL;
static GtkWidget *title_box = NULL;
static GtkWidget *clock_label = NULL;
static GtkWidget *entry;
static GtkWidget *ok_button;
static GtkWidget *start_again_button;
static GtkWidget *msg;
static GtkWidget *auto_timed_msg;
static GtkWidget *err_box;
static guint err_box_clear_handler = 0;
static GtkWidget *icon_win = NULL;
static GtkWidget *sessmenu;
static GtkWidget *langmenu;

static gboolean login_is_local = FALSE;

static GtkWidget *browser;
static GtkTreeModel *browser_model;
static GdkPixbuf *defface;

/* Eew. Loads of global vars. It's hard to be event controlled while maintaining state */
static GList *users = NULL;
static GList *users_string = NULL;
static gint size_of_users = 0;

static gchar *curuser = NULL;
static gchar *session = NULL;

/* back_prog_timeout_event_id: event of the timer.
 * back_prog_watcher_event_id: event of the background program watcher.
 * back_prog_pid: 	       process ID of the background program.
 * back_prog_has_run:	       true if the background program has run
 *  			       at least once.
 * back_prog_watching_events:  true if we are watching for user events.
 * back_prog_delayed: 	       true if the execution of the program has
 *                             been delayed.
 */
static gint back_prog_timeout_event_id = -1;
static gint back_prog_watcher_event_id = -1;
static gint back_prog_pid = -1;
static gboolean back_prog_has_run = FALSE;
static gboolean back_prog_watching_events = FALSE;
static gboolean back_prog_delayed = FALSE;

static guint timed_handler_id = 0;

#if FIXME
static char *selected_browser_user = NULL;
#endif
static gboolean  selecting_user = TRUE;
static gchar    *selected_user = NULL;

extern GList *sessions;
extern GHashTable *sessnames;
extern gchar *default_session;
extern const gchar *current_session;
extern gboolean session_dir_whacked_out;
extern gint mdm_timed_delay;

static gboolean first_prompt = TRUE;

static void login_window_resize (gboolean force);

/* Background program logic */
static void back_prog_on_exit (GPid pid, gint status, gpointer data);
static gboolean back_prog_on_timeout (gpointer data);
static gboolean back_prog_delay_timeout (GSignalInvocationHint *ihint, 
					 guint n_param_values, 
					 const GValue *param_values, 
					 gpointer data);
static void back_prog_watch_events (void);
static gchar * back_prog_get_path (void);
static void back_prog_launch_after_timeout (void);
static void back_prog_run (void);
static void back_prog_stop (void);

static void process_operation (guchar op_code, const gchar *args);

/* 
 * This function is called when the background program exits.
 * It will add a timer to restart the program after the
 * restart delay has elapsed, if this is enabled.
 */
static void 
back_prog_on_exit (GPid pid, gint status, gpointer data)
{
	g_assert (back_prog_timeout_event_id == -1);
	
	back_prog_watcher_event_id = -1;
	back_prog_pid = -1;
	
	back_prog_launch_after_timeout ();
}

/* 
 * This function starts the background program (if any) when
 * the background program timer triggers, unless the execution
 * has been delayed.
 */
static gboolean 
back_prog_on_timeout (gpointer data)
{
	g_assert (back_prog_watcher_event_id == -1);
	g_assert (back_prog_pid == -1);
	
	back_prog_timeout_event_id = -1;
	
	if (back_prog_delayed) {
	 	back_prog_launch_after_timeout ();
	} else {
		back_prog_run ();
	}
	
	return FALSE;
}

/*
 * This function is called to delay the execution of the background
 * program when the user is doing something (when we detect an event).
 */
static gboolean
back_prog_delay_timeout (GSignalInvocationHint *ihint,
	       		 guint n_param_values,
	       		 const GValue *param_values,
	       		 gpointer data)
{
	back_prog_delayed = TRUE;
	return TRUE;
}

/*
 * This function creates signal listeners to catch user events.
 * That allows us to avoid spawning the background program
 * when the user is doing something.
 */
static void
back_prog_watch_events (void)
	{
	guint sid;
	
	if (back_prog_watching_events)
		return;
	
	back_prog_watching_events = TRUE;
	
	sid = g_signal_lookup ("activate", GTK_TYPE_MENU_ITEM);
	g_signal_add_emission_hook (sid, 0, back_prog_delay_timeout, 
				    NULL, NULL);

	sid = g_signal_lookup ("key_release_event", GTK_TYPE_WIDGET);
	g_signal_add_emission_hook (sid, 0, back_prog_delay_timeout, 
				    NULL, NULL);

	sid = g_signal_lookup ("button_press_event", GTK_TYPE_WIDGET);
	g_signal_add_emission_hook (sid, 0, back_prog_delay_timeout, 
				    NULL, NULL);
	}

/*
 * This function returns the path of the background program
 * if there is one. Otherwise, NULL is returned.
 */
static gchar *
back_prog_get_path (void)
{
	gchar *backprog = mdm_config_get_string (MDM_KEY_BACKGROUND_PROGRAM);

	if ((mdm_config_get_int (MDM_KEY_BACKGROUND_TYPE) == MDM_BACKGROUND_NONE ||
	     mdm_config_get_bool (MDM_KEY_RUN_BACKGROUND_PROGRAM_ALWAYS)) &&
	    ! ve_string_empty (backprog)) {
		return backprog;
	} else 
		return NULL;
}

/* 
 * This function creates a timer to start the background 
 * program after the requested delay (in seconds) has elapsed.
 */ 
static void 
back_prog_launch_after_timeout ()
{
	int timeout;
	
	g_assert (back_prog_timeout_event_id == -1);
	g_assert (back_prog_watcher_event_id == -1);
	g_assert (back_prog_pid == -1);
	
	/* No program to run. */
	if (! back_prog_get_path ())
		return;
	
	/* First time. */
	if (! back_prog_has_run) {
		timeout = mdm_config_get_int (MDM_KEY_BACKGROUND_PROGRAM_INITIAL_DELAY);
		
	/* Already run, but we are allowed to restart it. */
	} else if (mdm_config_get_bool (MDM_KEY_RESTART_BACKGROUND_PROGRAM)) {
		timeout = mdm_config_get_int (MDM_KEY_BACKGROUND_PROGRAM_RESTART_DELAY);
	
	/* Already run, but we are not allowed to restart it. */
	} else {
		return;
	}
	
	back_prog_delayed = FALSE;
	back_prog_watch_events ();
	back_prog_timeout_event_id = g_timeout_add (timeout * 1000,
						    back_prog_on_timeout,
						    NULL);
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

/*
 * This function starts the background program (if any).
 */
static void
back_prog_run (void)
{
	GPid pid = -1;
	GError *error = NULL;
	gchar *command = NULL;
	gchar **back_prog_argv = NULL;
	
	g_assert (back_prog_timeout_event_id == -1);
	g_assert (back_prog_watcher_event_id == -1);
	g_assert (back_prog_pid == -1);
	
	command = back_prog_get_path ();
	if (! command)
		return;

        mdm_common_debug ("Running background program <%s>", command);
	
	/* Focus new windows. We want to give focus to the background program. */
	mdm_wm_focus_new_windows (TRUE);

	back_prog_argv = NULL;
	g_shell_parse_argv (command, NULL, &back_prog_argv, NULL);

	/* Don't reap child automatically: we want to catch the event. */
	if (! g_spawn_async (".", 
			     back_prog_argv, 
			     NULL, 
			     (GSpawnFlags) (G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD), 
			     NULL, 
			     NULL, 
			     &pid, 
			     &error)) {
			    
		GtkWidget *dialog;
		gchar *msg;
		
                mdm_common_debug ("Cannot run background program %s : %s", command, error->message);
		msg = g_strdup_printf (_("Cannot run command '%s': %s."),
		                       command,
		                       error->message);
					    
		dialog = hig_dialog_new (NULL,
					 GTK_DIALOG_MODAL,
					 GTK_MESSAGE_ERROR,
					 GTK_BUTTONS_OK,
					 _("Cannot start background application"),
					 msg);
		g_free (msg);
		
		gtk_widget_show_all (dialog);
		mdm_wm_center_window (GTK_WINDOW (dialog));

		gtk_dialog_run (GTK_DIALOG (dialog));
		gtk_widget_destroy (dialog);
		g_error_free (error);
		g_strfreev (back_prog_argv);
		
		return;
	}
	
	g_strfreev (back_prog_argv);
	back_prog_watcher_event_id = g_child_watch_add (pid, 
							back_prog_on_exit,
							NULL);
	back_prog_pid = pid;
	back_prog_has_run = TRUE;
}

/*
 * This function stops the background program if it is running,
 * and removes any associated timer or watcher.
 */
static void 
back_prog_stop (void)
{
	if (back_prog_timeout_event_id != -1) {
		GSource *source = g_main_context_find_source_by_id
					(NULL, back_prog_timeout_event_id);
		if (source != NULL)
			g_source_destroy (source);
			
		back_prog_timeout_event_id = -1;
	}
	
	if (back_prog_watcher_event_id != -1) {
		GSource *source = g_main_context_find_source_by_id
					(NULL, back_prog_watcher_event_id);
		if (source != NULL)
			g_source_destroy (source);
			
		back_prog_watcher_event_id = -1;
	}
	
	if (back_prog_pid != -1) {		
		if (kill (back_prog_pid, SIGTERM) == 0) {
			waitpid (back_prog_pid, NULL, 0);
		}

		back_prog_pid = -1;
	}
}

/*
 * Timed Login: Timer
 */
static gboolean
mdm_timer (gpointer data)
{
	if (mdm_timed_delay <= 0) {
		/* timed interruption */
		printf ("%c%c%c\n", STX, BEL, MDM_INTERRUPT_TIMED_LOGIN);
		fflush (stdout);
	} else {
		gchar *autologin_msg;

		/*
		 * Note that this message is not handled the same way as in
		 * the greeter, we don't parse it through the enriched text.
		 */
		autologin_msg = mdm_common_expand_text (
			_("User %u will login in %t"));
		gtk_label_set_text (GTK_LABEL (auto_timed_msg), autologin_msg);
		gtk_widget_show (GTK_WIDGET (auto_timed_msg));
		g_free (autologin_msg);
		login_window_resize (FALSE /* force */);
	}

	mdm_timed_delay--;
	return TRUE;
}

/*
 * Timed Login: On GTK events, increase delay to at least 30
 * seconds, or the MDM_KEY_TIMED_LOGIN_DELAY, whichever is higher
 */
static gboolean
mdm_timer_up_delay (GSignalInvocationHint *ihint,
		    guint	           n_param_values,
		    const GValue	  *param_values,
		    gpointer		   data)
{
	if (mdm_timed_delay < 30)
		mdm_timed_delay = 30;
	if (mdm_timed_delay < mdm_config_get_int (MDM_KEY_TIMED_LOGIN_DELAY))
		mdm_timed_delay = mdm_config_get_int (MDM_KEY_TIMED_LOGIN_DELAY);
	return TRUE;
}

/* The reaping stuff */
static time_t last_reap_delay = 0;

static gboolean
delay_reaping (GSignalInvocationHint *ihint,
	       guint	           n_param_values,
	       const GValue	  *param_values,
	       gpointer		   data)
{
	last_reap_delay = time (NULL);
	return TRUE;
}      

static void
mdm_kill_thingies (void)
{
	back_prog_stop ();
}

static gboolean
reap_flexiserver (gpointer data)
{
	int reapminutes = mdm_config_get_int (MDM_KEY_FLEXI_REAP_DELAY_MINUTES);

	if (reapminutes > 0 &&
	    ((time (NULL) - last_reap_delay) / 60) > reapminutes) {
		mdm_kill_thingies ();
		_exit (DISPLAY_REMANAGE);
	}
	return TRUE;
}


static gboolean
mdm_event (GSignalInvocationHint *ihint,
	   guint		n_param_values,
	   const GValue	       *param_values,
	   gpointer		data)
{
	GdkEvent *event;

	/* HAAAAAAAAAAAAAAAAACK */
	/* Since the user has not logged in yet and may have left/right
	 * mouse buttons switched, we just translate every right mouse click
	 * to a left mouse click */
	if (n_param_values != 2 ||
	    !G_VALUE_HOLDS (&param_values[1], GDK_TYPE_EVENT))
	  return FALSE;
	
	event = g_value_get_boxed (&param_values[1]);
	if ((event->type == GDK_BUTTON_PRESS ||
	     event->type == GDK_2BUTTON_PRESS ||
	     event->type == GDK_3BUTTON_PRESS ||
	     event->type == GDK_BUTTON_RELEASE)
	    && event->button.button == 3)
		event->button.button = 1;

	/* Support Ctrl-U for blanking the username/password entry */
	if (event->type == GDK_KEY_PRESS &&
	    (event->key.state & GDK_CONTROL_MASK) &&
	    (event->key.keyval == GDK_u ||
	     event->key.keyval == GDK_U)) {

		gtk_entry_set_text (GTK_ENTRY (entry), "");
	}

	return TRUE;
}      

static void
mdm_login_done (int sig)
{
	mdm_kill_thingies ();
	_exit (EXIT_SUCCESS);
}

static guint set_pos_id = 0;

static gboolean
set_pos_idle (gpointer data)
{
	mdm_wm_center_window (GTK_WINDOW (login));
	
	set_pos_id = 0;
	return FALSE;
}

static void
login_window_resize (gboolean force)
{
	/* allow opt out if we don't really need
	 * a resize */
	if ( ! force) {
		GtkRequisition req;
		int width, height;

		gtk_window_get_size (GTK_WINDOW (login), &width, &height);
		gtk_widget_size_request (login, &req);

		if (req.width <= width && req.height <= height)
			return;
	}

	GTK_WINDOW (login)->need_default_size = TRUE;
	gtk_container_check_resize (GTK_CONTAINER (login));

	if (set_pos_id == 0)
		set_pos_id = g_idle_add (set_pos_idle, NULL);
}


typedef struct _CursorOffset {
	int x;
	int y;
} CursorOffset;

static void
mdm_run_mdmconfig (GtkWidget *w, gpointer data)
{
	gtk_widget_set_sensitive (browser, FALSE);

	/* Make sure to unselect the user */
	if (selected_user != NULL)
		g_free (selected_user);
	selected_user = NULL;

	/* we should be now fine for focusing new windows */
	mdm_wm_focus_new_windows (TRUE);

	/* configure interruption */
	printf ("%c%c%c\n", STX, BEL, MDM_INTERRUPT_CONFIGURE);
	fflush (stdout);
}

static void
mdm_login_restart_handler (void)
{
	if (mdm_wm_warn_dialog (
	    _("Are you sure you want to restart the computer?"), "",
	    _("_Restart"), NULL, TRUE) == GTK_RESPONSE_YES) {

		mdm_kill_thingies ();
		_exit (DISPLAY_REBOOT);
	}
}

static void
mdm_login_halt_handler (void)
{
	if (mdm_wm_warn_dialog (
	    _("Are you sure you want to Shut Down the computer?"), "",
	    _("Shut _Down"), NULL, TRUE) == GTK_RESPONSE_YES) {

		mdm_kill_thingies ();
		_exit (DISPLAY_HALT);
	}
}

static void
mdm_login_suspend_handler (void)
{
	if (mdm_wm_warn_dialog (
	    _("Are you sure you want to suspend the computer?"), "",
	    _("_Suspend"), NULL, TRUE) == GTK_RESPONSE_YES) {

		/* suspend interruption */
		printf ("%c%c%c\n", STX, BEL, MDM_INTERRUPT_SUSPEND);
		fflush (stdout);
	}
}

static void
mdm_login_enter (GtkWidget *entry)
{
	const char *login_string;
	const char *str;
	char *tmp;

	if (entry == NULL)
		return;

	gtk_widget_set_sensitive (entry, FALSE);
	gtk_widget_set_sensitive (ok_button, FALSE);
	gtk_widget_set_sensitive (start_again_button, FALSE);

	login_string = gtk_entry_get_text (GTK_ENTRY (entry));

	str = gtk_label_get_text (GTK_LABEL (label));
	if (str != NULL &&
	    (strcmp (str, _("Username:")) == 0 ||
	     strcmp (str, _("_Username:")) == 0) &&
	    /* If in timed login mode, and if this is the login
	     * entry.  Then an enter by itself is sort of like I want to
	     * log in as the timed user, really.  */
	    ve_string_empty (login_string) &&
	    timed_handler_id != 0) {
		/* timed interruption */
		printf ("%c%c%c\n", STX, BEL, MDM_INTERRUPT_TIMED_LOGIN);
		fflush (stdout);
		return;
	}	

	/* clear the err_box */
	if (err_box_clear_handler > 0)
		g_source_remove (err_box_clear_handler);
	err_box_clear_handler = 0;
	gtk_label_set_text (GTK_LABEL (err_box), "");

	tmp = ve_locale_from_utf8 (gtk_entry_get_text (GTK_ENTRY (entry)));
	printf ("%c%s\n", STX, tmp);
	fflush (stdout);
	g_free (tmp);
}

static void
mdm_login_ok_button_press (GtkButton *button, GtkWidget *entry)
{
	mdm_login_enter (entry);
}

static void
mdm_login_start_again_button_press (GtkButton *button, GtkWidget *entry)
{
	GtkTreeSelection *selection;

	if (browser != NULL) {
		selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (browser));
		gtk_tree_selection_unselect_all (selection);
	}

	if (selected_user != NULL)
		g_free (selected_user);
	selected_user = NULL;

	printf ("%c%c%c\n", STX, BEL,
		MDM_INTERRUPT_CANCEL);
	fflush (stdout);
}

static gboolean
mdm_login_focus_in (GtkWidget *widget, GdkEventFocus *event)
{
	if (title_box != NULL)
		gtk_widget_set_state (title_box, GTK_STATE_SELECTED);

	if (icon_button != NULL)
		gtk_widget_set_state (icon_button, GTK_STATE_NORMAL);

	return FALSE;
}

static gboolean
mdm_login_focus_out (GtkWidget *widget, GdkEventFocus *event)
{
	if (title_box != NULL)
		gtk_widget_set_state (title_box, GTK_STATE_NORMAL);

	return FALSE;
}

static void 
mdm_login_session_handler (GtkWidget *widget) 
{
    gchar *s;

    current_session = g_object_get_data (G_OBJECT (widget), SESSION_NAME);

    s = g_strdup_printf (_("%s session selected"), mdm_session_name (current_session));

    gtk_label_set_text (GTK_LABEL (msg), s);
    g_free (s);

    login_window_resize (FALSE /* force */);
}

static void 
mdm_login_session_init (GtkWidget *menu)
{
    GSList *sessgrp = NULL;
    GList *tmp;
    GtkWidget *item;
    char *label;

    current_session = NULL;

    mdm_session_list_init ();

    for (tmp = sessions; tmp != NULL; tmp = tmp->next) {
	    MdmSession *session;
	    char *file;

	    file = (char *) tmp->data;
	    session = g_hash_table_lookup (sessnames, file);
		label = g_strdup (session->name);

	    item = gtk_radio_menu_item_new_with_mnemonic (sessgrp, label);
	    g_free (label);
	    g_object_set_data_full (G_OBJECT (item), SESSION_NAME,
		 g_strdup (file), (GDestroyNotify) g_free);

	    sessgrp = gtk_radio_menu_item_get_group (GTK_RADIO_MENU_ITEM (item));
	    gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
	    g_signal_connect (G_OBJECT (item), "activate",
		G_CALLBACK (mdm_login_session_handler), NULL);
	    gtk_widget_show (GTK_WIDGET (item));
    }

    /* Select the proper session */
    {
            GSList *tmp;
            
            tmp = sessgrp;
            while (tmp != NULL) {
                    GtkWidget *w = tmp->data;
                    const char *n;

                    n = g_object_get_data (G_OBJECT (w), SESSION_NAME);
                    
                    if (n && strcmp (n, current_session) == 0) {
                            gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (w),
                                                            TRUE);
                            break;
                    }
                    
                    tmp = tmp->next;
            }
    }
}


static void 
mdm_login_language_handler (GtkMenuItem *menu_item, gpointer user_data) 
{
    mdm_lang_handler (user_data);
}


static GtkWidget *
mdm_login_language_menu_new (void)
{
    GtkWidget *menu;
    GtkWidget *item;

    mdm_lang_initialize_model (mdm_config_get_string (MDM_KEY_LOCALE_FILE));

    menu = gtk_menu_new ();

    item = gtk_menu_item_new_with_mnemonic (_("Select _Language..."));
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
    g_signal_connect (G_OBJECT (item), "activate", 
		      G_CALLBACK (mdm_login_language_handler), 
		      NULL);
    gtk_widget_show (GTK_WIDGET (item));

    return menu;
}

static gboolean
err_box_clear (gpointer data)
{
	if (err_box != NULL)
		gtk_label_set_text (GTK_LABEL (err_box), "");

	err_box_clear_handler = 0;
	return FALSE;
}

static void
browser_set_user (const char *user)
{
  gboolean old_selecting_user = selecting_user;
  GtkTreeSelection *selection;
  GtkTreeIter iter = {0};
  GtkTreeModel *tm = NULL;

  if (browser == NULL)
    return;

  selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (browser));
  gtk_tree_selection_unselect_all (selection);

  if (ve_string_empty (user))
    return;

  selecting_user = FALSE;

  tm = gtk_tree_view_get_model (GTK_TREE_VIEW (browser));

  if (gtk_tree_model_get_iter_first (tm, &iter))
    {
      do
        {
          char *login;
	  gtk_tree_model_get (tm, &iter, GREETER_ULIST_LOGIN_COLUMN,
			      &login, -1);
	  if (login != NULL && strcmp (user, login) == 0)
	    {
	      GtkTreePath *path = gtk_tree_model_get_path (tm, &iter);
	      gtk_tree_selection_select_iter (selection, &iter);
	      gtk_tree_view_scroll_to_cell (GTK_TREE_VIEW (browser),
					    path, NULL,
					    FALSE, 0.0, 0.0);
	      gtk_tree_path_free (path);
	      break;
	    }
	  
        }
      while (gtk_tree_model_iter_next (tm, &iter));
    }
  selecting_user = old_selecting_user;
}

static Display *
get_parent_display (void)
{
  static gboolean tested = FALSE;
  static Display *dsp = NULL;

  if (tested)
    return dsp;

  tested = TRUE;

  if (g_getenv ("MDM_PARENT_DISPLAY") != NULL)
    {
      char *old_xauth = g_strdup (g_getenv ("XAUTHORITY"));
      if (g_getenv ("MDM_PARENT_XAUTHORITY") != NULL)
        {
	  g_setenv ("XAUTHORITY",
		     g_getenv ("MDM_PARENT_XAUTHORITY"), TRUE);
	}
      dsp = XOpenDisplay (g_getenv ("MDM_PARENT_DISPLAY"));
      if (old_xauth != NULL)
        g_setenv ("XAUTHORITY", old_xauth, TRUE);
      else
        g_unsetenv ("XAUTHORITY");
      g_free (old_xauth);
    }

  return dsp;
}

static gboolean
greeter_is_capslock_on (void)
{
  XkbStateRec states;
  Display *dsp;

  /* HACK! incredible hack, if MDM_PARENT_DISPLAY is set we get
   * indicator state from the parent display, since we must be inside a nested display */
  dsp = get_parent_display ();
  if (dsp == NULL)
    dsp = GDK_DISPLAY ();

  if (XkbGetState (dsp, XkbUseCoreKbd, &states) != Success)
      return FALSE;

  return (states.locked_mods & LockMask) != 0;
}

static void
face_browser_select_user (gchar *login)
{
        printf ("%c%c%c%s\n", STX, BEL,
                MDM_INTERRUPT_SELECT_USER, login);

        fflush (stdout);
}

static gboolean
mdm_login_ctrl_handler (GIOChannel *source, GIOCondition cond, gint fd)
{
    gchar buf[PIPE_SIZE];
    gchar *p;
    gsize len;

    /* If this is not incoming i/o then return */
    if (cond != G_IO_IN) 
	return (TRUE);

    /* Read random garbage from i/o channel until STX is found */
    do {
	g_io_channel_read_chars (source, buf, 1, &len, NULL);

	if (len != 1)
	    return (TRUE);
    } while (buf[0] && buf[0] != STX);

    memset (buf, '\0', sizeof (buf));
    if (g_io_channel_read_chars (source, buf, sizeof (buf) - 1, &len, NULL) !=
	G_IO_STATUS_NORMAL)
      return TRUE;

    p = memchr (buf, STX, len);
    if (p != NULL) {
      len = p - buf;
      g_io_channel_seek_position (source, -((sizeof (buf) - 1) - len), G_SEEK_CUR, NULL);
      memset (buf + len, '\0', (sizeof (buf) - 1) - len);
    }
    buf[len - 1] = '\0';  
 
    process_operation ((guchar) buf[0], buf + 1);

    return TRUE;
}

static void
process_operation (guchar       op_code,
		   const gchar *args)
{
    char *tmp;
    gint i, x, y;
    GtkWidget *dlg;
    static gboolean replace_msg = TRUE;
    static gboolean messages_to_give = FALSE;

    /* Parse opcode */
    switch (op_code) {
    case MDM_SETLOGIN:
	/* somebody is trying to fool us this is the user that
	 * wants to log in, and well, we are the gullible kind */
	g_free (curuser);
	curuser = g_strdup (args);
	if (mdm_config_get_bool (MDM_KEY_BROWSER)) {
    	browser_set_user (curuser);
    }
	printf ("%c\n", STX);
	fflush (stdout);
	break;

    case MDM_PROMPT:
	tmp = ve_locale_to_utf8 (args);
	if (tmp != NULL && strcmp (tmp, _("Username:")) == 0) {
		mdm_common_login_sound (mdm_config_get_string (MDM_KEY_SOUND_PROGRAM),
					mdm_config_get_string (MDM_KEY_SOUND_ON_LOGIN_FILE),
					mdm_config_get_bool   (MDM_KEY_SOUND_ON_LOGIN));
		gtk_label_set_text_with_mnemonic (GTK_LABEL (label), _("_Username:"));
	} else {
		if (tmp != NULL)
			gtk_label_set_text (GTK_LABEL (label), tmp);
	}
	g_free (tmp);

	gtk_widget_set_sensitive (GTK_WIDGET (start_again_button), !first_prompt);
	first_prompt = FALSE;

	gtk_widget_show (GTK_WIDGET (label));
	gtk_entry_set_text (GTK_ENTRY (entry), "");
	gtk_entry_set_max_length (GTK_ENTRY (entry), PW_ENTRY_SIZE);
	gtk_entry_set_visibility (GTK_ENTRY (entry), TRUE);
	gtk_widget_set_sensitive (entry, TRUE);
	gtk_widget_set_sensitive (ok_button, FALSE);
	gtk_widget_grab_focus (entry);	
	gtk_window_set_focus (GTK_WINDOW (login), entry);	
	gtk_widget_show (entry);

	/* replace rather then append next message string */
	replace_msg = TRUE;

	/* the user has seen messages */
	messages_to_give = FALSE;

	login_window_resize (FALSE /* force */);
	break;

    case MDM_NOECHO:
	tmp = ve_locale_to_utf8 (args);
	if (tmp != NULL && strcmp (tmp, _("Password:")) == 0) {
		gtk_label_set_text_with_mnemonic (GTK_LABEL (label), _("_Password:"));
	} else {
		if (tmp != NULL)
			gtk_label_set_text (GTK_LABEL (label), tmp);
	}
	g_free (tmp);

	gtk_widget_set_sensitive (GTK_WIDGET (start_again_button), !first_prompt);
	first_prompt = FALSE;

	gtk_widget_show (GTK_WIDGET (label));
	gtk_entry_set_text (GTK_ENTRY (entry), "");
	gtk_entry_set_max_length (GTK_ENTRY (entry), PW_ENTRY_SIZE);
	gtk_entry_set_visibility (GTK_ENTRY (entry), FALSE);
	gtk_widget_set_sensitive (entry, TRUE);
	gtk_widget_set_sensitive (ok_button, FALSE);
	gtk_widget_grab_focus (entry);	
	gtk_window_set_focus (GTK_WINDOW (login), entry);	
	gtk_widget_show (entry);

	/* replace rather then append next message string */
	replace_msg = TRUE;

	/* the user has seen messages */
	messages_to_give = FALSE;

	login_window_resize (FALSE /* force */);
	break;

    case MDM_MSG:
	/* the user has not yet seen messages */
	messages_to_give = TRUE;

	/* HAAAAAAACK.  Sometimes pam sends many many messages, SO
	 * we try to collect them until the next prompt or reset or
	 * whatnot */
	if ( ! replace_msg &&
	   /* empty message is for clearing */
	   ! ve_string_empty (args)) {
		const char *oldtext;
		oldtext = gtk_label_get_text (GTK_LABEL (msg));
		if ( ! ve_string_empty (oldtext)) {
			char *newtext;
			tmp = ve_locale_to_utf8 (args);
			newtext = g_strdup_printf ("%s\n%s", oldtext, tmp);
			g_free (tmp);
			gtk_label_set_text (GTK_LABEL (msg), newtext);
			g_free (newtext);
		} else {
			tmp = ve_locale_to_utf8 (args);
			gtk_label_set_text (GTK_LABEL (msg), tmp);
			g_free (tmp);
		}
	} else {
		tmp = ve_locale_to_utf8 (args);
		gtk_label_set_text (GTK_LABEL (msg), tmp);
		g_free (tmp);
	}
	replace_msg = FALSE;

	gtk_widget_show (GTK_WIDGET (msg));
	printf ("%c\n", STX);
	fflush (stdout);

	login_window_resize (FALSE /* force */);

	break;

    case MDM_ERRBOX:
	tmp = ve_locale_to_utf8 (args);
	gtk_label_set_text (GTK_LABEL (err_box), tmp);
	g_free (tmp);
	if (err_box_clear_handler > 0)
		g_source_remove (err_box_clear_handler);
	if (ve_string_empty (args))
		err_box_clear_handler = 0;
	else
		err_box_clear_handler = g_timeout_add (30000,
						       err_box_clear,
						       NULL);
	printf ("%c\n", STX);
	fflush (stdout);

	login_window_resize (FALSE /* force */);
	break;

    case MDM_ERRDLG:
	/* we should be now fine for focusing new windows */
	mdm_wm_focus_new_windows (TRUE);

	tmp = ve_locale_to_utf8 (args);
	dlg = hig_dialog_new (NULL /* parent */,
			      GTK_DIALOG_MODAL /* flags */,
			      GTK_MESSAGE_ERROR,
			      GTK_BUTTONS_OK,
			      tmp,
			      "");
	g_free (tmp);

	mdm_wm_center_window (GTK_WINDOW (dlg));

	mdm_wm_no_login_focus_push ();
	gtk_dialog_run (GTK_DIALOG (dlg));
	gtk_widget_destroy (dlg);
	mdm_wm_no_login_focus_pop ();

	printf ("%c\n", STX);
	fflush (stdout);
	break;

    case MDM_SESS:
	printf ("%c%s\n", STX, current_session);
	fflush (stdout);
	break;

    case MDM_LANG:
	mdm_lang_op_lang (args);
	break;

    case MDM_SLANG:
	mdm_lang_op_slang (args);
	break;

    case MDM_SETSESS:
    	current_session = args;
    	printf ("%c\n", STX);
    	fflush (stdout);
    	break;

    case MDM_SETLANG:
	mdm_lang_op_setlang (args);
	break;

    case MDM_ALWAYS_RESTART:
	mdm_lang_op_always_restart (args);
	break;

    case MDM_RESET:
	if (login->window != NULL &&
	    icon_win == NULL &&
	    GTK_WIDGET_VISIBLE (login)) {
		Window lw = GDK_WINDOW_XWINDOW (login->window);

		mdm_wm_get_window_pos (lw, &x, &y);

		for (i = 32 ; i > 0 ; i = i/4) {
			mdm_wm_move_window_now (lw, i+x, y);
			usleep (200);
			mdm_wm_move_window_now (lw, x, y);
			usleep (200);
			mdm_wm_move_window_now (lw, -i+x, y);
			usleep (200);
			mdm_wm_move_window_now (lw, x, y);
			usleep (200);
		}
	}
	/* fall thru to reset */

    case MDM_RESETOK:
	if (curuser != NULL) {
	    g_free (curuser);
	    curuser = NULL;
	}

	first_prompt = TRUE;

	gtk_widget_set_sensitive (entry, TRUE);
	gtk_widget_set_sensitive (ok_button, FALSE);
	gtk_widget_set_sensitive (start_again_button, FALSE);
	if (mdm_config_get_bool (MDM_KEY_BROWSER)) {
    	gtk_widget_set_sensitive (GTK_WIDGET (browser), TRUE);
    }

	tmp = ve_locale_to_utf8 (args);
	gtk_label_set_text (GTK_LABEL (msg), tmp);
	g_free (tmp);
	gtk_widget_show (GTK_WIDGET (msg));

	printf ("%c\n", STX);
	fflush (stdout);

	login_window_resize (FALSE /* force */);
	break;

    case MDM_QUIT:
	if (timed_handler_id != 0) {
		g_source_remove (timed_handler_id);
		timed_handler_id = 0;
	}	

	/* Hide the login window now */
	gtk_widget_hide (login);

	if (messages_to_give) {
		const char *oldtext;
		oldtext = gtk_label_get_text (GTK_LABEL (msg));

		if ( ! ve_string_empty (oldtext)) {
			/* we should be now fine for focusing new windows */
			mdm_wm_focus_new_windows (TRUE);

			dlg = hig_dialog_new (NULL /* parent */,
					      GTK_DIALOG_MODAL /* flags */,
					      GTK_MESSAGE_INFO,
					      GTK_BUTTONS_OK,
					      oldtext,
					      "");
			gtk_window_set_modal (GTK_WINDOW (dlg), TRUE);
			mdm_wm_center_window (GTK_WINDOW (dlg));

			mdm_wm_no_login_focus_push ();
			gtk_dialog_run (GTK_DIALOG (dlg));
			gtk_widget_destroy (dlg);
			mdm_wm_no_login_focus_pop ();
		}
		messages_to_give = FALSE;
	}

	mdm_kill_thingies ();

	gdk_flush ();

	printf ("%c\n", STX);
	fflush (stdout);

	/* screw gtk_main_quit, we want to make sure we definately die */
	_exit (EXIT_SUCCESS);
	break;

    case MDM_STARTTIMER:
	/*
	 * Timed Login: Start Timer Loop
	 */

	if (timed_handler_id == 0 &&
	    mdm_config_get_bool (MDM_KEY_TIMED_LOGIN_ENABLE) &&
	    ! ve_string_empty (mdm_config_get_string (MDM_KEY_TIMED_LOGIN)) &&
	    mdm_config_get_int (MDM_KEY_TIMED_LOGIN_DELAY) > 0) {
		mdm_timed_delay = mdm_config_get_int (MDM_KEY_TIMED_LOGIN_DELAY);
		timed_handler_id  = g_timeout_add (1000, mdm_timer, NULL);
	}
	printf ("%c\n", STX);
	fflush (stdout);
	break;

    case MDM_STOPTIMER:
	/*
	 * Timed Login: Stop Timer Loop
	 */

	if (timed_handler_id != 0) {
		g_source_remove (timed_handler_id);
		timed_handler_id = 0;
	}
	printf ("%c\n", STX);
	fflush (stdout);
	break;

    case MDM_DISABLE:
	if (clock_label != NULL)
		GTK_WIDGET_SET_FLAGS (clock_label->parent, GTK_SENSITIVE);
	gtk_widget_set_sensitive (login, FALSE);
	printf ("%c\n", STX);
	fflush (stdout);
	break;

    case MDM_ENABLE:
	gtk_widget_set_sensitive (login, TRUE);
	if (clock_label != NULL)
		GTK_WIDGET_UNSET_FLAGS (clock_label->parent, GTK_SENSITIVE);
	printf ("%c\n", STX);
	fflush (stdout);
	break;

    /* These are handled separately so ignore them here and send
     * back a NULL response so that the daemon quits sending them */
    case MDM_NEEDPIC:
    case MDM_READPIC:
	printf ("%c\n", STX);
	fflush (stdout);
	break;

    case MDM_NOFOCUS:
	mdm_wm_no_login_focus_push ();
	
	printf ("%c\n", STX);
	fflush (stdout);
	break;

    case MDM_FOCUS:
	mdm_wm_no_login_focus_pop ();
	
	printf ("%c\n", STX);
	fflush (stdout);
	break;

    case MDM_SAVEDIE:
	/* Set busy cursor */
	mdm_common_setup_cursor (GDK_WATCH);

	mdm_wm_save_wm_order ();

	mdm_kill_thingies ();
	gdk_flush ();

	printf ("%c\n", STX);
	fflush (stdout);

	_exit (EXIT_SUCCESS);

    case MDM_QUERY_CAPSLOCK:
	if (greeter_is_capslock_on ())
	    printf ("%cY\n", STX);
	else
	    printf ("%c\n", STX);
	fflush (stdout);

	break;
	
    default:
	mdm_kill_thingies ();
	mdm_common_fail_greeter ("Unexpected greeter command received: '%c'", op_code);
	break;
    }
}


static void
mdm_login_browser_populate (void)
{
    GList *li;

    for (li = users; li != NULL; li = li->next) {
	    MdmUser *usr = li->data;
	    GtkTreeIter iter = {0};
	    char *label;
	    char *login, *gecos;

	    login = mdm_common_text_to_escaped_utf8 (usr->login);
	    gecos = mdm_common_text_to_escaped_utf8 (usr->gecos);

	    label = g_strdup_printf ("<b>%s</b>\n%s",
				     login,
				     gecos);

	    g_free (login);
	    g_free (gecos);
	    gtk_list_store_append (GTK_LIST_STORE (browser_model), &iter);
	    gtk_list_store_set (GTK_LIST_STORE (browser_model), &iter,
				GREETER_ULIST_ICON_COLUMN, usr->picture,
				GREETER_ULIST_LOGIN_COLUMN, usr->login,
				GREETER_ULIST_LABEL_COLUMN, label,
				-1);
	    g_free (label);
    }
    return;
}

static void
user_selected (GtkTreeSelection *selection, gpointer data)
{
  GtkTreeModel *tm = NULL;
  GtkTreeIter iter = {0};

#ifdef FIXME
  g_free (selected_browser_user);
  selected_browser_user = NULL;
#endif

  if (gtk_tree_selection_get_selected (selection, &tm, &iter)) {
	char *login = NULL;
	gtk_tree_model_get (tm, &iter, GREETER_ULIST_LOGIN_COLUMN,
			      &login, -1);
	if (login != NULL) {
		const char *str = gtk_label_get_text (GTK_LABEL (label));

		if (selecting_user &&
		    str != NULL &&
		    (strcmp (str, _("Username:")) == 0 ||
		     strcmp (str, _("_Username:")) == 0)) {
			gtk_entry_set_text (GTK_ENTRY (entry), login);
		}
#ifdef FIXME
		selected_browser_user = g_strdup (login);
#endif
		if (selecting_user) {
			face_browser_select_user (login);
			if (selected_user != NULL)
				g_free (selected_user);
			selected_user = g_strdup (login);
  		}
  	}
  }
}

static void
browser_change_focus (GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    gtk_widget_grab_focus (entry);	
}

static gboolean
update_clock (void)
{
        struct tm *the_tm;
	gchar *str;
        gint time_til_next_min;

	if (clock_label == NULL)
		return FALSE;

	str = mdm_common_get_clock (&the_tm);
	gtk_label_set_text (GTK_LABEL (clock_label), str);
	g_free (str);

	/* account for leap seconds */
	time_til_next_min = 60 - the_tm->tm_sec;
	time_til_next_min = (time_til_next_min>=0?time_til_next_min:0);

	g_timeout_add (time_til_next_min*1000, (GSourceFunc)update_clock, NULL);
	return FALSE;
}

/* doesn't check for executability, just for existence */
static gboolean
bin_exists (const char *command)
{
	char *bin;

	if (ve_string_empty (command))
		return FALSE;

	/* Note, check only for existence, not for executability */
	bin = ve_first_word (command);
	if (bin != NULL &&
	    g_access (bin, F_OK) == 0) {
		g_free (bin);
		return TRUE;
	} else {
		g_free (bin);
		return FALSE;
	}
}

static gboolean
window_browser_event (GtkWidget *window, GdkEvent *event, gpointer data)
{
	switch (event->type) {
		/* FIXME: Fix fingering cuz it's cool */
#ifdef FIXME
	case GDK_KEY_PRESS:
		if ((event->key.state & GDK_CONTROL_MASK) &&
		    (event->key.keyval == GDK_f ||
		     event->key.keyval == GDK_F) &&
		    selected_browser_user != NULL) {
			GtkWidget *d, *less;
			char *command;
			d = gtk_dialog_new_with_buttons (_("Finger"),
							 NULL /* parent */,
							 0 /* flags */,
							 GTK_STOCK_OK,
							 GTK_RESPONSE_OK,
							 NULL);
			gtk_dialog_set_has_separator (GTK_DIALOG (d), FALSE);
			less = gnome_less_new ();
			gtk_widget_show (less);
			gtk_box_pack_start (GTK_BOX (GTK_DIALOG (d)->vbox),
					    less,
					    TRUE,
					    TRUE,
					    0);

			/* hack to make this be the size of a terminal */
			gnome_less_set_fixed_font (GNOME_LESS (less), TRUE);
			{
				int i;
				char buf[82];
				GtkWidget *text = GTK_WIDGET (GNOME_LESS (less)->text);
				GdkFont *font = GNOME_LESS (less)->font;
				for (i = 0; i < 81; i++)
					buf[i] = 'X';
				buf[i] = '\0';
				gtk_widget_set_size_request
					(text,
					 gdk_string_width (font, buf) + 30,
					 gdk_string_height (font, buf)*24+30);
			}

			command = g_strconcat ("finger ",
					       selected_browser_user,
					       NULL);
			gnome_less_show_command (GNOME_LESS (less), command);

			gtk_widget_grab_focus (GTK_WIDGET (less));

			gtk_window_set_modal (GTK_WINDOW (d), TRUE);
			mdm_wm_center_window (GTK_WINDOW (d));

			mdm_wm_no_login_focus_push ();
			gtk_dialog_run (GTK_DIALOG (d));
			gtk_widget_destroy (d);
			mdm_wm_no_login_focus_pop ();
		}
		break;
#endif
	default:
		break;
	}

	return FALSE;
}

static gboolean
key_release_event (GtkWidget *entry, GdkEventKey *event, gpointer data)
{
	const char *login_string;

	/*
	 * Allow tab to trigger enter; but not <modifier>-tab
	 */
	if ((event->keyval == GDK_Tab ||
	     event->keyval == GDK_KP_Tab) &&
	    (event->state & (GDK_CONTROL_MASK|GDK_MOD1_MASK|GDK_SHIFT_MASK)) == 0) {
		mdm_login_enter (entry);
		return TRUE;
	}

	/*
	 * Set ok button to sensitive only if there are characters in
	 * the entry field
	 */
	login_string = gtk_entry_get_text (GTK_ENTRY (entry));
        if (login_string != NULL && login_string[0] != '\0')
		gtk_widget_set_sensitive (ok_button, TRUE);
	else
		gtk_widget_set_sensitive (ok_button, FALSE);

	return FALSE;
}

static void
mdm_set_welcomemsg (void)
{
	gchar *greeting;
	gchar *welcomemsg     = mdm_common_get_welcomemsg ();
	gchar *fullwelcomemsg = g_strdup_printf (
		"<big><big><big>%s</big></big></big>", welcomemsg);

	greeting = mdm_common_expand_text (fullwelcomemsg);
	gtk_label_set_markup (GTK_LABEL (welcome), greeting);

	g_free (fullwelcomemsg);
	g_free (welcomemsg);
	g_free (greeting);
}

static gboolean
key_press_event (GtkWidget *widget, GdkEventKey *key, gpointer data)
{
  if (key->keyval == GDK_Escape)
    {
      printf ("%c%c%c\n", STX, BEL, MDM_INTERRUPT_CANCEL);
      fflush (stdout);

      return TRUE;
    }
  
  return FALSE;
}

static void
mdm_login_gui_init (void)
{
    GtkTreeSelection *selection;
    GtkWidget *frame1, *frame2;
    GtkWidget *mbox, *menu, *menubar, *item;
    GtkWidget *stack, *hline1, *hline2;
    GtkWidget *bbox = NULL;
    GtkWidget /**help_button,*/ *button_box;
    gint i;        
    const gchar *theme_name;
    gchar *key_string = NULL;

    theme_name = g_getenv ("MDM_GTK_THEME");
    if (ve_string_empty (theme_name))
	    theme_name = mdm_config_get_string (MDM_KEY_GTK_THEME);

    if ( ! ve_string_empty (mdm_config_get_string (MDM_KEY_GTKRC)))
	    gtk_rc_parse (mdm_config_get_string (MDM_KEY_GTKRC));

    if ( ! ve_string_empty (theme_name)) {
	    mdm_set_theme (theme_name);
    }

    login = gtk_window_new (GTK_WINDOW_TOPLEVEL);
    g_object_ref (login);
    g_object_set_data_full (G_OBJECT (login), "login", login,
			    (GDestroyNotify) g_object_unref);

    gtk_widget_set_events (login, GDK_ALL_EVENTS_MASK);

    g_signal_connect (G_OBJECT (login), "key_press_event",
                      G_CALLBACK (key_press_event), NULL);

    if G_LIKELY ( ! DOING_MDM_DEVELOPMENT) {
    	gtk_window_set_title (GTK_WINDOW (login), _("MDM Login"));
    }
    else {    	
    	gtk_window_set_icon_name (GTK_WINDOW (login), "mdmsetup");
        gtk_window_set_title (GTK_WINDOW (login), ("GTK"));
        gtk_window_set_default_size (GTK_WINDOW (login), 640, 400);
    }
    
    /* connect for fingering */    
    if (mdm_config_get_bool (MDM_KEY_BROWSER)) {
		g_signal_connect (G_OBJECT (login), "event", G_CALLBACK (window_browser_event), NULL);
	}

    frame1 = gtk_frame_new (NULL);
    gtk_frame_set_shadow_type (GTK_FRAME (frame1), GTK_SHADOW_OUT);
    gtk_container_set_border_width (GTK_CONTAINER (frame1), 0);
    gtk_container_add (GTK_CONTAINER (login), frame1);
    g_object_set_data_full (G_OBJECT (login), "frame1", frame1,
			    (GDestroyNotify) gtk_widget_unref);
    gtk_widget_ref (GTK_WIDGET (frame1));
    gtk_widget_show (GTK_WIDGET (frame1));

    frame2 = gtk_frame_new (NULL);
    gtk_frame_set_shadow_type (GTK_FRAME (frame2), GTK_SHADOW_IN);
    gtk_container_set_border_width (GTK_CONTAINER (frame2), 2);
    gtk_container_add (GTK_CONTAINER (frame1), frame2);
    g_object_set_data_full (G_OBJECT (login), "frame2", frame2,
			    (GDestroyNotify) gtk_widget_unref);
    gtk_widget_ref (GTK_WIDGET (frame2));
    gtk_widget_show (GTK_WIDGET (frame2));

    mbox = gtk_vbox_new (FALSE, 0);
    gtk_widget_ref (mbox);
    g_object_set_data_full (G_OBJECT (login), "mbox", mbox,
			    (GDestroyNotify) gtk_widget_unref);
    gtk_widget_show (mbox);
    gtk_container_add (GTK_CONTAINER (frame2), mbox);
    
    menubar = gtk_menu_bar_new ();
    gtk_widget_ref (GTK_WIDGET (menubar));
    gtk_box_pack_start (GTK_BOX (mbox), menubar, FALSE, FALSE, 0);

    menu = gtk_menu_new ();
    mdm_login_session_init (menu);
    sessmenu = gtk_menu_item_new_with_mnemonic (_("S_ession"));
    gtk_menu_shell_append (GTK_MENU_SHELL (menubar), sessmenu);
    gtk_menu_item_set_submenu (GTK_MENU_ITEM (sessmenu), menu);
    gtk_widget_show (GTK_WIDGET (sessmenu));

    menu = mdm_login_language_menu_new ();
    if (menu != NULL) {
	langmenu = gtk_menu_item_new_with_mnemonic (_("_Language"));
	gtk_menu_shell_append (GTK_MENU_SHELL (menubar), langmenu);
	gtk_menu_item_set_submenu (GTK_MENU_ITEM (langmenu), menu);
	gtk_widget_show (GTK_WIDGET (langmenu));
    }

    if (disable_system_menu_buttons == FALSE &&
        mdm_config_get_bool (MDM_KEY_SYSTEM_MENU)) {

        gboolean got_anything = FALSE;

	menu = gtk_menu_new ();	

	/*
	 * Disable Configuration if using accessibility (AddGtkModules) since
	 * using it with accessibility causes a hang.
	 */
	if (mdm_config_get_bool (MDM_KEY_CONFIG_AVAILABLE) &&
	    !mdm_config_get_bool (MDM_KEY_ADD_GTK_MODULES) &&
	    bin_exists (mdm_config_get_string (MDM_KEY_CONFIGURATOR))) {
		item = gtk_menu_item_new_with_mnemonic (_("_Configure Login Manager..."));
		gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
		g_signal_connect (G_OBJECT (item), "activate",
				  G_CALLBACK (mdm_run_mdmconfig),
				  NULL);
		gtk_widget_show (item);
		got_anything = TRUE;
	}

	if (mdm_working_command_exists (mdm_config_get_string (MDM_KEY_REBOOT)) &&
	    mdm_common_is_action_available ("REBOOT")) {
		item = gtk_menu_item_new_with_mnemonic (_("_Restart"));
		gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
		g_signal_connect (G_OBJECT (item), "activate",
				  G_CALLBACK (mdm_login_restart_handler), 
				  NULL);
		gtk_widget_show (GTK_WIDGET (item));
		got_anything = TRUE;
	}
	
	if (mdm_working_command_exists (mdm_config_get_string (MDM_KEY_HALT)) &&
	    mdm_common_is_action_available ("HALT")) {
		item = gtk_menu_item_new_with_mnemonic (_("Shut _Down"));
		gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
		g_signal_connect (G_OBJECT (item), "activate",
				  G_CALLBACK (mdm_login_halt_handler), 
				  NULL);
		gtk_widget_show (GTK_WIDGET (item));
		got_anything = TRUE;
	}

	if (mdm_working_command_exists (mdm_config_get_string (MDM_KEY_SUSPEND)) &&
	    mdm_common_is_action_available ("SUSPEND")) {
		item = gtk_menu_item_new_with_mnemonic (_("_Suspend"));
		gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
		g_signal_connect (G_OBJECT (item), "activate",
				  G_CALLBACK (mdm_login_suspend_handler), 
				  NULL);
		gtk_widget_show (GTK_WIDGET (item));
		got_anything = TRUE;
	}	

	if (got_anything) {
		item = gtk_menu_item_new_with_mnemonic (_("_Actions"));
		gtk_menu_shell_append (GTK_MENU_SHELL (menubar), item);
		gtk_menu_item_set_submenu (GTK_MENU_ITEM (item), menu);
		gtk_widget_show (GTK_WIDGET (item));
	}
    }  

    /* The clock */
    clock_label = gtk_label_new ("");
    gtk_widget_show (clock_label);
    item = gtk_menu_item_new ();
    gtk_container_add (GTK_CONTAINER (item), clock_label);
    gtk_widget_show (item);
    gtk_menu_shell_append (GTK_MENU_SHELL (menubar), item);
    gtk_menu_item_set_right_justified (GTK_MENU_ITEM (item), TRUE);
    GTK_WIDGET_UNSET_FLAGS (item, GTK_SENSITIVE);

    g_signal_connect (G_OBJECT (clock_label), "destroy",
		      G_CALLBACK (gtk_widget_destroyed),
		      &clock_label);

    update_clock (); 

    table = gtk_table_new (1, 2, FALSE);
    gtk_widget_ref (table);
    g_object_set_data_full (G_OBJECT (login), "table", table,
			    (GDestroyNotify) gtk_widget_unref);
    gtk_widget_show (table);
    gtk_box_pack_start (GTK_BOX (mbox), table, TRUE, TRUE, 0);
    gtk_container_set_border_width (GTK_CONTAINER (table), 10);
    gtk_table_set_row_spacings (GTK_TABLE (table), 10);
    gtk_table_set_col_spacings (GTK_TABLE (table), 10);
    
    if (mdm_config_get_bool (MDM_KEY_BROWSER)) {
	    int height;
	    GtkTreeViewColumn *column;

	    browser = gtk_tree_view_new ();
	    gtk_tree_view_set_rules_hint (GTK_TREE_VIEW (browser), FALSE);
	    gtk_tree_view_set_headers_visible (GTK_TREE_VIEW (browser),
	                       FALSE);
	    gtk_tree_view_set_enable_search (GTK_TREE_VIEW (browser),
	                     FALSE);
	    selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (browser));
	    gtk_tree_selection_set_mode (selection, GTK_SELECTION_SINGLE);

	    g_signal_connect (selection, "changed",
	              G_CALLBACK (user_selected),
	              NULL);

	    g_signal_connect (browser, "button_release_event",
	              G_CALLBACK (browser_change_focus),
	              NULL);

	    browser_model = (GtkTreeModel *)gtk_list_store_new (3,
	             GDK_TYPE_PIXBUF, G_TYPE_STRING, G_TYPE_STRING);

	    gtk_tree_view_set_model (GTK_TREE_VIEW (browser), browser_model);
	    column = gtk_tree_view_column_new_with_attributes
	        (_("Icon"),
	         gtk_cell_renderer_pixbuf_new (),
	         "pixbuf", GREETER_ULIST_ICON_COLUMN,
	         NULL);
	    gtk_tree_view_append_column (GTK_TREE_VIEW (browser), column);
	  
	    column = gtk_tree_view_column_new_with_attributes
	        (_("Username"),
	         gtk_cell_renderer_text_new (),
	         "markup", GREETER_ULIST_LABEL_COLUMN,
	         NULL);
	    gtk_tree_view_append_column (GTK_TREE_VIEW (browser), column);
	   
	    bbox = gtk_scrolled_window_new (NULL, NULL);
	    gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (bbox),
	                     GTK_SHADOW_IN);
	    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (bbox),
	                    GTK_POLICY_NEVER,
	                    GTK_POLICY_AUTOMATIC);
	    gtk_container_add (GTK_CONTAINER (bbox), browser);

	    height = size_of_users + 4 /* some padding */;
	    if (height > mdm_wm_screen.height * 0.25)
	        height = mdm_wm_screen.height * 0.25;

	    gtk_widget_set_size_request (GTK_WIDGET (bbox), -1, height);   
	} 
               
    stack = gtk_table_new (7, 1, FALSE);
    gtk_widget_ref (stack);
    g_object_set_data_full (G_OBJECT (login), "stack", stack,
			    (GDestroyNotify) gtk_widget_unref);
    gtk_widget_show (stack);

    /* Welcome msg */
    welcome = gtk_label_new (NULL);
    mdm_set_welcomemsg ();
    gtk_widget_set_name (welcome, _("Welcome"));
    gtk_widget_ref (welcome);
    g_object_set_data_full (G_OBJECT (login), "welcome", welcome,
			    (GDestroyNotify) gtk_widget_unref);
    gtk_widget_show (welcome);
    gtk_table_attach (GTK_TABLE (stack), welcome, 0, 1, 0, 1,
		      (GtkAttachOptions) (GTK_EXPAND | GTK_FILL),
		      (GtkAttachOptions) (GTK_EXPAND | GTK_FILL), 0, 15);

    /* Put in error box here */
    err_box = gtk_label_new (NULL);
    gtk_widget_set_name (err_box, "Error box");
    g_signal_connect (G_OBJECT (err_box), "destroy",
		      G_CALLBACK (gtk_widget_destroyed),
		      &err_box);
    gtk_label_set_line_wrap (GTK_LABEL (err_box), TRUE);
    gtk_table_attach (GTK_TABLE (stack), err_box, 0, 1, 1, 2,
		      (GtkAttachOptions) (GTK_EXPAND | GTK_FILL),
		      (GtkAttachOptions) (GTK_FILL), 0, 0);


    hline1 = gtk_hseparator_new ();
    gtk_widget_ref (hline1);
    g_object_set_data_full (G_OBJECT (login), "hline1", hline1,
			    (GDestroyNotify) gtk_widget_unref);
    gtk_widget_show (hline1);
    gtk_table_attach (GTK_TABLE (stack), hline1, 0, 1, 2, 3,
		      (GtkAttachOptions) (GTK_EXPAND | GTK_FILL),
		      (GtkAttachOptions) (GTK_FILL), 0, 6);
    
    label = gtk_label_new_with_mnemonic (_("_Username:"));
    gtk_widget_ref (label);
    g_object_set_data_full (G_OBJECT (login), "label", label,
			    (GDestroyNotify) gtk_widget_unref);
    gtk_widget_show (label);
    gtk_table_attach (GTK_TABLE (stack), label, 0, 1, 3, 4,
		      (GtkAttachOptions) (GTK_EXPAND | GTK_FILL),
		      (GtkAttachOptions) (0), 0, 0);
    gtk_label_set_justify (GTK_LABEL (label), GTK_JUSTIFY_LEFT);
    gtk_misc_set_alignment (GTK_MISC (label), 0, 0.5);
    gtk_misc_set_padding (GTK_MISC (label), 10, 5);
    gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
    
    entry = gtk_entry_new ();
    /*
     * Connect this on key release so we can make the OK button 
     * sensitive based on whether the entry field has data.
     */
    g_signal_connect (G_OBJECT (entry), "key_release_event",
		      G_CALLBACK (key_release_event), NULL);

    if (mdm_config_get_bool (MDM_KEY_ENTRY_INVISIBLE))
	    gtk_entry_set_invisible_char (GTK_ENTRY (entry), 0);
    else if (mdm_config_get_bool (MDM_KEY_ENTRY_CIRCLES))
	    gtk_entry_set_invisible_char (GTK_ENTRY (entry), 0x25cf);

    gtk_entry_set_max_length (GTK_ENTRY (entry), PW_ENTRY_SIZE);
    gtk_widget_set_size_request (entry, 250, -1);
    gtk_widget_ref (entry);
    g_object_set_data_full (G_OBJECT (login), "entry", entry,
			    (GDestroyNotify) gtk_widget_unref);
    gtk_entry_set_text (GTK_ENTRY (entry), "");
    gtk_widget_show (entry);
    gtk_table_attach (GTK_TABLE (stack), entry, 0, 1, 4, 5,
		      (GtkAttachOptions) (GTK_EXPAND | GTK_FILL),
		      (GtkAttachOptions) (0), 10, 0);
    g_signal_connect (G_OBJECT(entry), "activate", 
		      G_CALLBACK (mdm_login_enter),
		      NULL);

    /* cursor blinking is evil on remote displays, don't do it forever */
    mdm_common_setup_blinking ();
    mdm_common_setup_blinking_entry (entry);
    
    hline2 = gtk_hseparator_new ();
    gtk_widget_ref (hline2);
    g_object_set_data_full (G_OBJECT (login), "hline2", hline2,
			    (GDestroyNotify) gtk_widget_unref);
    gtk_widget_show (hline2);
    gtk_table_attach (GTK_TABLE (stack), hline2, 0, 1, 5, 6,
		      (GtkAttachOptions) (GTK_EXPAND | GTK_FILL),
		      (GtkAttachOptions) (GTK_FILL), 0, 10);

    msg = gtk_label_new (NULL);
    gtk_widget_set_name (msg, "Message");
    gtk_label_set_line_wrap (GTK_LABEL (msg), TRUE);
    gtk_label_set_justify (GTK_LABEL (msg), GTK_JUSTIFY_LEFT);
    gtk_table_attach (GTK_TABLE (stack), msg, 0, 1, 6, 7,
		      (GtkAttachOptions) (GTK_EXPAND | GTK_FILL),
		      (GtkAttachOptions) (GTK_FILL), 0, 10);

    gtk_widget_ref (msg);
    g_object_set_data_full (G_OBJECT (login), "msg", msg,
			    (GDestroyNotify) gtk_widget_unref);
    gtk_widget_show (msg);

    auto_timed_msg = gtk_label_new ("");
    gtk_widget_set_name (auto_timed_msg, "Message");
    gtk_label_set_line_wrap (GTK_LABEL (auto_timed_msg), TRUE);
    gtk_label_set_justify (GTK_LABEL (auto_timed_msg), GTK_JUSTIFY_LEFT);
    gtk_table_attach (GTK_TABLE (stack), auto_timed_msg, 0, 1, 7, 8,
		      (GtkAttachOptions) (GTK_EXPAND | GTK_FILL),
		      (GtkAttachOptions) (GTK_FILL), 0, 10);
    gtk_widget_set_size_request (auto_timed_msg, -1, 15);
    
    gtk_widget_ref (auto_timed_msg);
    gtk_widget_show (auto_timed_msg);

    /* FIXME: No Documentation yet.... */
    /*help_button = gtk_button_new_from_stock (GTK_STOCK_OK);
    gtk_widget_show (help_button);*/

    ok_button = gtk_button_new_from_stock (GTK_STOCK_OK);
    gtk_widget_set_sensitive (ok_button, FALSE);
    g_signal_connect (G_OBJECT (ok_button), "clicked",
		      G_CALLBACK (mdm_login_ok_button_press),
		      entry);
    gtk_widget_show (ok_button);

    start_again_button = gtk_button_new_with_mnemonic (_("_Start Again"));
    g_signal_connect (G_OBJECT (start_again_button), "clicked",
		      G_CALLBACK (mdm_login_start_again_button_press),
		      entry);
    gtk_widget_show (start_again_button);

    button_box = gtk_hbutton_box_new ();
    gtk_button_box_set_layout (GTK_BUTTON_BOX (button_box),
                               GTK_BUTTONBOX_END);
    gtk_box_set_spacing (GTK_BOX (button_box), 10);

#if 0
    gtk_box_pack_end (GTK_BOX (button_box), GTK_WIDGET (help_button),
                      FALSE, TRUE, 0);
    gtk_button_box_set_child_secondary (GTK_BUTTON_BOX (button_box), help_button, TRUE);
#endif

    gtk_box_pack_end (GTK_BOX (button_box), GTK_WIDGET (start_again_button),
		      FALSE, TRUE, 0);
    gtk_box_pack_end (GTK_BOX (button_box), GTK_WIDGET (ok_button),
		      FALSE, TRUE, 0);
    gtk_widget_show (button_box);
    
    gtk_table_attach (GTK_TABLE (stack), button_box, 0, 1, 8, 9,
		      (GtkAttachOptions) (GTK_FILL),
		      (GtkAttachOptions) (GTK_FILL), 10, 10);

    /* Put it nicely together */
    gtk_table_attach (GTK_TABLE (table), bbox, 0, 1, 1, 2,
              (GtkAttachOptions) (GTK_FILL),
              (GtkAttachOptions) (GTK_FILL), 0, 0);
    gtk_table_attach (GTK_TABLE (table), stack, 1, 2, 1, 2,
              (GtkAttachOptions) (GTK_EXPAND | GTK_FILL),
              (GtkAttachOptions) (GTK_FILL), 0, 0);
    
    gtk_widget_grab_focus (entry);	
    gtk_window_set_focus (GTK_WINDOW (login), entry);	
    g_object_set (G_OBJECT (login),
		  "allow_grow", TRUE,
		  "allow_shrink", TRUE,
		  "resizable", TRUE,
		  NULL);
    
    mdm_wm_center_window (GTK_WINDOW (login));    

    g_signal_connect (G_OBJECT (login), "focus_in_event", 
		      G_CALLBACK (mdm_login_focus_in),
		      NULL);
    g_signal_connect (G_OBJECT (login), "focus_out_event", 
		      G_CALLBACK (mdm_login_focus_out),
		      NULL);

    gtk_label_set_mnemonic_widget (GTK_LABEL (label), entry);

    /* normally disable the prompt first */
    if ( ! DOING_MDM_DEVELOPMENT) {
	    gtk_widget_set_sensitive (entry, FALSE);
	    gtk_widget_set_sensitive (ok_button, FALSE);
	    gtk_widget_set_sensitive (start_again_button, FALSE);
    }

    gtk_widget_show_all (GTK_WIDGET (login));    

    if (mdm_config_get_bool (MDM_KEY_BROWSER)) {
	    /* Make sure userlist has no users selected */
	    selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (browser));
	    gtk_tree_selection_unselect_all (selection);
	    if (selected_user)
	    g_free (selected_user);
	    selected_user = NULL;
	}
}

static GdkPixbuf *
render_scaled_back (const GdkPixbuf *pb)
{
	int i;
	int width, height;

	GdkPixbuf *back = gdk_pixbuf_new (GDK_COLORSPACE_RGB,
					  gdk_pixbuf_get_has_alpha (pb),
					  8,
					  gdk_screen_width (),
					  gdk_screen_height ());

	width = gdk_pixbuf_get_width (pb);
	height = gdk_pixbuf_get_height (pb);

	for (i = 0; i < mdm_wm_num_monitors; i++) {
		gdk_pixbuf_scale (pb, back,
				  mdm_wm_all_monitors[i].x,
				  mdm_wm_all_monitors[i].y,
				  mdm_wm_all_monitors[i].width,
				  mdm_wm_all_monitors[i].height,
				  mdm_wm_all_monitors[i].x /* offset_x */,
				  mdm_wm_all_monitors[i].y /* offset_y */,
				  (double) mdm_wm_all_monitors[i].width / width,
				  (double) mdm_wm_all_monitors[i].height / height,
				  GDK_INTERP_BILINEAR);
	}

	return back;
}

static void
add_color_to_pb (GdkPixbuf *pb, GdkColor *color)
{
	int width = gdk_pixbuf_get_width (pb);
	int height = gdk_pixbuf_get_height (pb);
	int rowstride = gdk_pixbuf_get_rowstride (pb);
	guchar *pixels = gdk_pixbuf_get_pixels (pb);
	gboolean has_alpha = gdk_pixbuf_get_has_alpha (pb);
	int i;
	int cr = color->red >> 8;
	int cg = color->green >> 8;
	int cb = color->blue >> 8;

	if ( ! has_alpha)
		return;

	for (i = 0; i < height; i++) {
		int ii;
		guchar *p = pixels + (rowstride * i);
		for (ii = 0; ii < width; ii++) {
			int r = p[0];
			int g = p[1];
			int b = p[2];
			int a = p[3];

			p[0] = (r * a + cr * (255 - a)) >> 8;
			p[1] = (g * a + cg * (255 - a)) >> 8;
			p[2] = (b * a + cb * (255 - a)) >> 8;
			p[3] = 255;

			p += 4;
		}
	}
}

/* setup background color/image */
static void
setup_background (void)
{
	GdkColor color;
	GdkPixbuf *pb = NULL;
	gchar *bg_color = mdm_config_get_string (MDM_KEY_BACKGROUND_COLOR);
	gchar *bg_image = mdm_config_get_string (MDM_KEY_BACKGROUND_IMAGE);
	gint   bg_type  = mdm_config_get_int    (MDM_KEY_BACKGROUND_TYPE); 

	if ((bg_type == MDM_BACKGROUND_IMAGE ||
	     bg_type == MDM_BACKGROUND_IMAGE_AND_COLOR) &&
	    ! ve_string_empty (bg_image))
		pb = gdk_pixbuf_new_from_file (bg_image, NULL);

	/* Load background image */
	if (pb != NULL) {
		if (gdk_pixbuf_get_has_alpha (pb)) {
			if (bg_type == MDM_BACKGROUND_IMAGE_AND_COLOR) {
				if (bg_color == NULL ||
				    bg_color[0] == '\0' ||
				    ! gdk_color_parse (bg_color,
					       &color)) {
					gdk_color_parse ("#000000", &color);
				}
				add_color_to_pb (pb, &color);
			}
		}
		
		GdkPixbuf *spb = render_scaled_back (pb);
		g_object_unref (G_OBJECT (pb));
		pb = spb;		

		/* paranoia */
		if (pb != NULL) {
			mdm_common_set_root_background (pb);
			g_object_unref (G_OBJECT (pb));
		}
	/* Load background color */
	} else if (bg_type != MDM_BACKGROUND_NONE &&
	           bg_type != MDM_BACKGROUND_IMAGE) {
		mdm_common_setup_background_color (bg_color);
	/* Load default background */
	} else {
		gchar *blank_color = g_strdup ("#000000");
		mdm_common_setup_background_color (blank_color);
	}
}

enum {
	RESPONSE_RESTART,
	RESPONSE_REBOOT,
	RESPONSE_CLOSE
};

/* 
 * If new configuration keys are added to this program, make sure to add the
 * key to the mdm_read_config and mdm_reread_config functions.  Note if the
 * number of configuration values used by mdmlogin is greater than 
 * MDM_SUP_MAX_MESSAGES defined in daemon/mdm.h (currently defined to be 80),
 * consider bumping that number so that all the config can be read in one
 * socket connection.
 */
static void
mdm_read_config (void)
{
	gint i;
	gchar *key_string = NULL;
		
	mdmcomm_open_connection_to_daemon ();

	/*
	 * Read all the keys at once and close sockets connection so we do
	 * not have to keep the socket open. 
	 */
	mdm_config_get_string (MDM_KEY_BACKGROUND_COLOR);
	mdm_config_get_string (MDM_KEY_BACKGROUND_IMAGE);
	mdm_config_get_string (MDM_KEY_BACKGROUND_PROGRAM);
	mdm_config_get_string (MDM_KEY_CONFIGURATOR);
	mdm_config_get_string (MDM_KEY_DEFAULT_FACE);
	mdm_config_get_string (MDM_KEY_DEFAULT_SESSION);
	mdm_config_get_string (MDM_KEY_EXCLUDE);
	mdm_config_get_string (MDM_KEY_GTK_THEME);
	mdm_config_get_string (MDM_KEY_GTK_THEMES_TO_ALLOW);
	mdm_config_get_string (MDM_KEY_GTKRC);
	mdm_config_get_string (MDM_KEY_HALT);
	mdm_config_get_string (MDM_KEY_INCLUDE);
	mdm_config_get_string (MDM_KEY_INFO_MSG_FILE);
	mdm_config_get_string (MDM_KEY_INFO_MSG_FONT);
	mdm_config_get_string (MDM_KEY_LOCALE_FILE);	
	mdm_config_get_string (MDM_KEY_REBOOT);	
	mdm_config_get_string (MDM_KEY_SESSION_DESKTOP_DIR);
	mdm_config_get_string (MDM_KEY_SOUND_PROGRAM);
	mdm_config_get_string (MDM_KEY_SOUND_ON_LOGIN_FILE);
	mdm_config_get_string (MDM_KEY_SUSPEND);
	mdm_config_get_string (MDM_KEY_TIMED_LOGIN);
	mdm_config_get_string (MDM_KEY_USE_24_CLOCK);
	mdm_config_get_string (MDM_KEY_WELCOME);
	mdm_config_get_string (MDM_KEY_RBAC_SYSTEM_COMMAND_KEYS);
	mdm_config_get_string (MDM_KEY_SYSTEM_COMMANDS_IN_MENU);	

	mdm_config_get_int    (MDM_KEY_BACKGROUND_TYPE);
	mdm_config_get_int    (MDM_KEY_BACKGROUND_PROGRAM_INITIAL_DELAY);
	mdm_config_get_int    (MDM_KEY_BACKGROUND_PROGRAM_RESTART_DELAY);
	mdm_config_get_int    (MDM_KEY_FLEXI_REAP_DELAY_MINUTES);
	mdm_config_get_int    (MDM_KEY_MAX_ICON_HEIGHT);
	mdm_config_get_int    (MDM_KEY_MAX_ICON_WIDTH);
	mdm_config_get_int    (MDM_KEY_MINIMAL_UID);
	mdm_config_get_int    (MDM_KEY_TIMED_LOGIN_DELAY);
	mdm_config_get_string    (MDM_KEY_PRIMARY_MONITOR);

	mdm_config_get_bool   (MDM_KEY_ALLOW_GTK_THEME_CHANGE);
	mdm_config_get_bool   (MDM_KEY_ALLOW_ROOT);	
	mdm_config_get_bool   (MDM_KEY_BROWSER);
	mdm_config_get_bool   (MDM_KEY_CONFIG_AVAILABLE);
	mdm_config_get_bool   (MDM_KEY_DEFAULT_WELCOME);
	mdm_config_get_bool   (MDM_KEY_ENTRY_CIRCLES);
	mdm_config_get_bool   (MDM_KEY_ENTRY_INVISIBLE);
	mdm_config_get_bool   (MDM_KEY_INCLUDE_ALL);	
	mdm_config_get_bool   (MDM_KEY_RUN_BACKGROUND_PROGRAM_ALWAYS);
	mdm_config_get_bool   (MDM_KEY_RESTART_BACKGROUND_PROGRAM);
	mdm_config_get_bool   (MDM_KEY_SOUND_ON_LOGIN);
	mdm_config_get_bool   (MDM_KEY_SYSTEM_MENU);
	mdm_config_get_bool   (MDM_KEY_TIMED_LOGIN_ENABLE);	
	mdm_config_get_bool   (MDM_KEY_ADD_GTK_MODULES);

	/* Keys not to include in reread_config */	
	mdm_config_get_string (MDM_KEY_PRE_FETCH_PROGRAM);	

	mdmcomm_close_connection_to_daemon ();
}

static gboolean
mdm_reread_config (int sig, gpointer data)
{
	gboolean resize = FALSE;
	gint i;
	gchar *key_string = NULL;
	
	mdmcomm_open_connection_to_daemon ();

	/* reparse config stuff here.  At least the ones we care about */
	/* FIXME: We should update these on the fly rather than just
         * restarting */
	/* Also we may not need to check ALL those keys but just a few */

	if (mdm_config_reload_string (MDM_KEY_BACKGROUND_PROGRAM) ||
	    mdm_config_reload_string (MDM_KEY_CONFIGURATOR) ||
	    mdm_config_reload_string (MDM_KEY_DEFAULT_FACE) ||
	    mdm_config_reload_string (MDM_KEY_DEFAULT_SESSION) ||
	    mdm_config_reload_string (MDM_KEY_EXCLUDE) ||
	    mdm_config_reload_string (MDM_KEY_GTKRC) ||
	    mdm_config_reload_string (MDM_KEY_GTK_THEME) ||
	    mdm_config_reload_string (MDM_KEY_GTK_THEMES_TO_ALLOW) ||
	    mdm_config_reload_string (MDM_KEY_HALT) ||
	    mdm_config_reload_string (MDM_KEY_INCLUDE) ||
	    mdm_config_reload_string (MDM_KEY_INFO_MSG_FILE) ||
	    mdm_config_reload_string (MDM_KEY_INFO_MSG_FONT) ||
	    mdm_config_reload_string (MDM_KEY_LOCALE_FILE) ||
	    mdm_config_reload_string (MDM_KEY_REBOOT) ||
	    mdm_config_reload_string (MDM_KEY_SESSION_DESKTOP_DIR) ||
	    mdm_config_reload_string (MDM_KEY_SUSPEND) ||
	    mdm_config_reload_string (MDM_KEY_TIMED_LOGIN) ||
	    mdm_config_reload_string (MDM_KEY_RBAC_SYSTEM_COMMAND_KEYS) ||
	    mdm_config_reload_string (MDM_KEY_SYSTEM_COMMANDS_IN_MENU) ||

	    mdm_config_reload_int    (MDM_KEY_BACKGROUND_PROGRAM_INITIAL_DELAY) ||
	    mdm_config_reload_int    (MDM_KEY_BACKGROUND_PROGRAM_RESTART_DELAY) ||
	    mdm_config_reload_int    (MDM_KEY_MAX_ICON_WIDTH) ||
	    mdm_config_reload_int    (MDM_KEY_MAX_ICON_HEIGHT) ||
	    mdm_config_reload_int    (MDM_KEY_MINIMAL_UID) ||
	    mdm_config_reload_int    (MDM_KEY_TIMED_LOGIN_DELAY) ||
	    mdm_config_reload_string    (MDM_KEY_PRIMARY_MONITOR) ||

	    mdm_config_reload_bool   (MDM_KEY_ALLOW_GTK_THEME_CHANGE) ||
	    mdm_config_reload_bool   (MDM_KEY_ALLOW_ROOT) ||
	    mdm_config_reload_bool   (MDM_KEY_BROWSER) ||	    
	    mdm_config_reload_bool   (MDM_KEY_CONFIG_AVAILABLE) ||
	    mdm_config_reload_bool   (MDM_KEY_ENTRY_CIRCLES) ||
	    mdm_config_reload_bool   (MDM_KEY_ENTRY_INVISIBLE) ||
	    mdm_config_reload_bool   (MDM_KEY_INCLUDE_ALL) ||	    
	    mdm_config_reload_bool   (MDM_KEY_RESTART_BACKGROUND_PROGRAM) ||
	    mdm_config_reload_bool   (MDM_KEY_RUN_BACKGROUND_PROGRAM_ALWAYS) ||
	    mdm_config_reload_bool   (MDM_KEY_SYSTEM_MENU) ||
	    mdm_config_reload_bool   (MDM_KEY_TIMED_LOGIN_ENABLE) ||	    
	    mdm_config_reload_bool   (MDM_KEY_ADD_GTK_MODULES)) {

		/* Set busy cursor */
		mdm_common_setup_cursor (GDK_WATCH);

		mdm_wm_save_wm_order ();
		mdm_kill_thingies ();
		mdmcomm_close_connection_to_daemon ();

		_exit (DISPLAY_RESTARTGREETER);
		return TRUE;
	}

	if (mdm_config_reload_string (MDM_KEY_BACKGROUND_IMAGE) ||
	    mdm_config_reload_string (MDM_KEY_BACKGROUND_COLOR) ||
	    mdm_config_reload_int    (MDM_KEY_BACKGROUND_TYPE)) {

		mdm_kill_thingies ();
		setup_background ();
		back_prog_launch_after_timeout ();
	}	

	mdm_config_reload_string (MDM_KEY_SOUND_PROGRAM);
	mdm_config_reload_bool   (MDM_KEY_SOUND_ON_LOGIN);
	mdm_config_reload_string (MDM_KEY_SOUND_ON_LOGIN_FILE);
	mdm_config_reload_string (MDM_KEY_USE_24_CLOCK);
	update_clock ();
	
	if (mdm_config_reload_string (MDM_KEY_WELCOME) ||
            mdm_config_reload_bool   (MDM_KEY_DEFAULT_WELCOME)) {

		mdm_set_welcomemsg ();
	}

	if (resize)
		login_window_resize (TRUE /* force */);

	mdmcomm_close_connection_to_daemon ();

	return TRUE;
}

/*
 * This function does nothing for mdmlogin, but mdmgreeter does do extra
 * work in this callback function.
 */
void
lang_set_custom_callback (gchar *language)
{
}

int 
main (int argc, char *argv[])
{
    struct sigaction hup;
    struct sigaction term;
    sigset_t mask;
    GIOChannel *ctrlch;
    guint sid;

    if (g_getenv ("DOING_MDM_DEVELOPMENT") != NULL)
	    DOING_MDM_DEVELOPMENT = TRUE;

    bindtextdomain (GETTEXT_PACKAGE, GNOMELOCALEDIR);
    bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
    textdomain (GETTEXT_PACKAGE);

    /*
     * mdm_common_atspi_launch () needs gdk initialized.
     * We cannot start gtk before the registry is running 
     * because the atk-bridge will crash.
     */
    gdk_init (&argc, &argv);
    if ( ! DOING_MDM_DEVELOPMENT) {
       mdm_common_atspi_launch ();
    }

    gtk_init (&argc, &argv);

    if (ve_string_empty (g_getenv ("MDM_IS_LOCAL")))
	disable_system_menu_buttons = TRUE;

    mdm_common_log_init ();
    mdm_common_log_set_debug (mdm_config_get_bool (MDM_KEY_DEBUG));

    mdm_common_setup_builtin_icons ();

    /* Read all configuration at once, so the values get cached */
    mdm_read_config ();
    
    setlocale (LC_ALL, "");

    mdm_wm_screen_init (mdm_config_get_string (MDM_KEY_PRIMARY_MONITOR));   

    /* Load the background as early as possible so MDM does not leave  */
    /* the background unfilled.   The cursor should be a watch already */
    /* but just in case */
    setup_background ();
    mdm_common_setup_cursor (GDK_WATCH);   

    defface = mdm_common_get_face (NULL,
                   mdm_config_get_string (MDM_KEY_DEFAULT_FACE),
                   mdm_config_get_int (MDM_KEY_MAX_ICON_WIDTH),
                   mdm_config_get_int (MDM_KEY_MAX_ICON_HEIGHT));

    if (! defface) {
        mdm_common_warning ("Could not open DefaultFace: %s!", mdm_config_get_string (MDM_KEY_DEFAULT_FACE));
    }

    if (mdm_config_get_bool (MDM_KEY_BROWSER)) {
    	mdm_users_init (&users, &users_string, NULL, defface, &size_of_users, login_is_local, !DOING_MDM_DEVELOPMENT);
    }

    mdm_login_gui_init ();

    if (mdm_config_get_bool (MDM_KEY_BROWSER)) {
		mdm_login_browser_populate ();
	}

    ve_signal_add (SIGHUP, mdm_reread_config, NULL);

    hup.sa_handler = ve_signal_notify;
    hup.sa_flags = 0;
    sigemptyset (&hup.sa_mask);
    sigaddset (&hup.sa_mask, SIGCHLD);

    if G_UNLIKELY (sigaction (SIGHUP, &hup, NULL) < 0) {
	    mdm_kill_thingies ();
	    mdm_common_fail_greeter (_("%s: Error setting up %s signal handler: %s"), "main",
		"HUP", strerror (errno));
    }

    term.sa_handler = mdm_login_done;
    term.sa_flags = 0;
    sigemptyset (&term.sa_mask);
    sigaddset (&term.sa_mask, SIGCHLD);

    if G_UNLIKELY (sigaction (SIGINT, &term, NULL) < 0) {
	    mdm_kill_thingies ();
	    mdm_common_fail_greeter (_("%s: Error setting up %s signal handler: %s"), "main",
		"INT", strerror (errno));
    }

    if G_UNLIKELY (sigaction (SIGTERM, &term, NULL) < 0) {
	    mdm_kill_thingies ();
	    mdm_common_fail_greeter (_("%s: Error setting up %s signal handler: %s"), "main",
		"TERM", strerror (errno));
    }

    sigemptyset (&mask);
    sigaddset (&mask, SIGTERM);
    sigaddset (&mask, SIGHUP);
    sigaddset (&mask, SIGINT);
    
    if G_UNLIKELY (sigprocmask (SIG_UNBLOCK, &mask, NULL) == -1) {
	    mdm_kill_thingies ();
	    mdm_common_fail_greeter (_("Could not set signal mask!"));
    }

    g_atexit (mdm_kill_thingies);
    back_prog_launch_after_timeout ();

    if G_LIKELY ( ! DOING_MDM_DEVELOPMENT) {
	    ctrlch = g_io_channel_unix_new (STDIN_FILENO);
	    g_io_channel_set_encoding (ctrlch, NULL, NULL);
	    g_io_channel_set_buffered (ctrlch, TRUE);
	    g_io_channel_set_flags (ctrlch, 
				    g_io_channel_get_flags (ctrlch) | G_IO_FLAG_NONBLOCK,
				    NULL);
	    g_io_add_watch (ctrlch, 
			    G_IO_IN | G_IO_PRI | G_IO_ERR | G_IO_HUP | G_IO_NVAL,
			    (GIOFunc) mdm_login_ctrl_handler,
			    NULL);
	    g_io_channel_unref (ctrlch);
    }

    /* if in timed mode, delay timeout on keyboard or menu
     * activity */
    if (mdm_config_get_bool (MDM_KEY_TIMED_LOGIN_ENABLE) &&
        ! ve_string_empty (mdm_config_get_string (MDM_KEY_TIMED_LOGIN))) {
	    sid = g_signal_lookup ("activate",
				   GTK_TYPE_MENU_ITEM);
	    g_signal_add_emission_hook (sid,
					0 /* detail */,
					mdm_timer_up_delay,
					NULL /* data */,
					NULL /* destroy_notify */);

	    sid = g_signal_lookup ("key_release_event",
				   GTK_TYPE_WIDGET);
	    g_signal_add_emission_hook (sid,
					0 /* detail */,
					mdm_timer_up_delay,
					NULL /* data */,
					NULL /* destroy_notify */);

	    sid = g_signal_lookup ("button_press_event",
				   GTK_TYPE_WIDGET);
	    g_signal_add_emission_hook (sid,
					0 /* detail */,
					mdm_timer_up_delay,
					NULL /* data */,
					NULL /* destroy_notify */);
    }

    /* if a flexiserver, reap self after some time */
    if (mdm_config_get_int (MDM_KEY_FLEXI_REAP_DELAY_MINUTES) > 0 &&
	! ve_string_empty (g_getenv ("MDM_FLEXI_SERVER")) &&
	/* but don't reap nested flexis */
	ve_string_empty (g_getenv ("MDM_PARENT_DISPLAY"))) {
	    sid = g_signal_lookup ("activate",
				   GTK_TYPE_MENU_ITEM);
	    g_signal_add_emission_hook (sid,
					0 /* detail */,
					delay_reaping,
					NULL /* data */,
					NULL /* destroy_notify */);

	    sid = g_signal_lookup ("key_release_event",
				   GTK_TYPE_WIDGET);
	    g_signal_add_emission_hook (sid,
					0 /* detail */,
					delay_reaping,
					NULL /* data */,
					NULL /* destroy_notify */);

	    sid = g_signal_lookup ("button_press_event",
				   GTK_TYPE_WIDGET);
	    g_signal_add_emission_hook (sid,
					0 /* detail */,
					delay_reaping,
					NULL /* data */,
					NULL /* destroy_notify */);

	    last_reap_delay = time (NULL);
	    g_timeout_add (60*1000, reap_flexiserver, NULL);
    }

    sid = g_signal_lookup ("event",
                           GTK_TYPE_WIDGET);
    g_signal_add_emission_hook (sid,
				0 /* detail */,
				mdm_event,
				NULL /* data */,
				NULL /* destroy_notify */);

    gtk_widget_queue_resize (login);
    gtk_widget_show_now (login);

    mdm_wm_center_window (GTK_WINDOW (login));    

    /* can it ever happen that it'd be NULL here ??? */
    if G_UNLIKELY (login->window != NULL) {
	    mdm_wm_init (GDK_WINDOW_XWINDOW (login->window));

	    /* Run the focus, note that this will work no matter what
	     * since mdm_wm_init will set the display to the gdk one
	     * if it fails */
	    mdm_wm_focus_window (GDK_WINDOW_XWINDOW (login->window));
    }

    if G_UNLIKELY (session_dir_whacked_out) {
	    GtkWidget *dialog;

	    mdm_wm_focus_new_windows (TRUE);

	    dialog = hig_dialog_new (NULL /* parent */,
				     GTK_DIALOG_MODAL /* flags */,
				     GTK_MESSAGE_ERROR,
				     GTK_BUTTONS_OK,
				     _("Session directory is missing"),
				     _("Your session directory is missing or empty!  "
				       "There are two available sessions you can use, but "
				       "you should log in and correct the MDM configuration."));
	    gtk_widget_show_all (dialog);
	    mdm_wm_center_window (GTK_WINDOW (dialog));

	    mdm_common_setup_cursor (GDK_LEFT_PTR);

	    mdm_wm_no_login_focus_push ();
	    gtk_dialog_run (GTK_DIALOG (dialog));
	    gtk_widget_destroy (dialog);
	    mdm_wm_no_login_focus_pop ();
    }

    if G_UNLIKELY (g_getenv ("MDM_WHACKED_GREETER_CONFIG") != NULL) {
	    GtkWidget *dialog;

	    mdm_wm_focus_new_windows (TRUE);

	    dialog = hig_dialog_new (NULL /* parent */,
				     GTK_DIALOG_MODAL /* flags */,
				     GTK_MESSAGE_ERROR,
				     GTK_BUTTONS_OK,
				     _("Configuration is not correct"),
				     _("The configuration file contains an invalid command "
				       "line for the login dialog, so running the "
				       "default command.  Please fix your configuration."));
	    gtk_widget_show_all (dialog);
	    mdm_wm_center_window (GTK_WINDOW (dialog));

	    mdm_common_setup_cursor (GDK_LEFT_PTR);

	    mdm_wm_no_login_focus_push ();
	    gtk_dialog_run (GTK_DIALOG (dialog));
	    gtk_widget_destroy (dialog);
	    mdm_wm_no_login_focus_pop ();
    }

    mdm_wm_restore_wm_order ();

    mdm_wm_show_info_msg_dialog (mdm_config_get_string (MDM_KEY_INFO_MSG_FILE),
       mdm_config_get_string (MDM_KEY_INFO_MSG_FONT));

    /* Only setup the cursor now since it will be a WATCH from before */
    mdm_common_setup_cursor (GDK_LEFT_PTR);
	mdm_wm_center_cursor ();
    mdm_common_pre_fetch_launch ();
    gtk_main ();

    mdm_kill_thingies ();

    return EXIT_SUCCESS;
}

/* EOF */
