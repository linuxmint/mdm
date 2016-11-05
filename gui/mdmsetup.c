/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * MDMSetup
 * Copyright (C) 2002, George Lebl
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

#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <popt.h>
#include <ctype.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pwd.h>
#include <X11/Xmd.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <glade/glade.h>
#include <glib/gi18n.h>

#include "mdm.h"
#include "mdmcommon.h"
#include "misc.h"
#include "mdmcomm.h"
#include "mdmuser.h"
#include "mdmsession.h"
#include "mdmconfig.h"

#include "mdm-common.h"
#include "mdm-socket-protocol.h"
#include "mdm-daemon-config-keys.h"
#include "mdm-log.h"

#include "server.h"

static gboolean  DOING_MDM_DEVELOPMENT = FALSE;
static gboolean  RUNNING_UNDER_MDM     = FALSE;
static gboolean  mdm_running           = FALSE;
static GladeXML  *xml;
static GtkWidget *setup_notebook;
static GList     *timeout_widgets = NULL;
static char      *selected_theme  = NULL;
static char      *selected_html_theme  = NULL;
static gchar     *config_file;
static gchar     *custom_config_file;

/* Used to store all available sessions */
static GList *sessions = NULL;

/* Used to store all available monitors */
static GList *monitors = NULL;

enum {
	THEME_COLUMN_SELECTED,	
	THEME_COLUMN_DIR,
	THEME_COLUMN_FILE,
	THEME_COLUMN_SCREENSHOT,
	THEME_COLUMN_MARKUP,
	THEME_COLUMN_NAME,
	THEME_COLUMN_DESCRIPTION,
	THEME_COLUMN_TYPE,
	THEME_NUM_COLUMNS
};

enum {
	ICONS_ID,
	ICONS_NAME,	
	ICONS_PIC,
	ICONS_NUM
};

enum {
	THEME_TYPE_GTK,	
	THEME_TYPE_HTML,
	THEME_TYPE_GDM
};

enum {
	USERLIST_NAME,
	USERLIST_NUM_COLUMNS
};

enum {
	CLOCK_AUTO,
	CLOCK_YES,
	CLOCK_NO
};

enum {
	LOCAL_PLAIN,	
	LOCAL_THEMED,
	LOCAL_HTML	
};

enum {
	BACKGROUND_NONE,
	BACKGROUND_IMAGE_AND_COLOR,
	BACKGROUND_COLOR,
	BACKGROUND_IMAGE
};

static void combobox_changed (GtkWidget *combobox);


static GtkTargetEntry target_table[] = {
	{ "text/uri-list", 0, 0 }
};

static guint n_targets = sizeof (target_table) / sizeof (target_table[0]);

static void
simple_spawn_sync (char **argv)
{
	g_spawn_sync (NULL /* working_directory */,
	              argv,
	              NULL /* envp */,
	              G_SPAWN_SEARCH_PATH |
	              G_SPAWN_STDOUT_TO_DEV_NULL |
	              G_SPAWN_STDERR_TO_DEV_NULL,
	              NULL /* child_setup */,
	              NULL /* user_data */,
	              NULL /* stdout */,
	              NULL /* stderr */,
	              NULL /* exit status */,
	              NULL /* error */);
}

static void
setup_cursor (GdkCursorType type)
{
	GdkCursor *cursor;

	cursor = gdk_cursor_new (type);
	gdk_window_set_cursor (gdk_get_default_root_window (), cursor);
	gdk_cursor_unref (cursor);
}

static void
setup_window_cursor (GdkCursorType type)
{
	GdkCursor *cursor;
	GtkWidget *setup_dialog;

	cursor = gdk_cursor_new (type);
	setup_dialog = glade_xml_get_widget (xml, "setup_dialog");
	if (setup_dialog->window)
		gdk_window_set_cursor (setup_dialog->window, cursor);
	gdk_cursor_unref (cursor);
}

static void
unsetup_window_cursor (void)
{
	GtkWidget *setup_dialog;

	setup_dialog = glade_xml_get_widget (xml, "setup_dialog");
	if (setup_dialog->window)
		gdk_window_set_cursor (setup_dialog->window, NULL);
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

static void
update_greeters (void)
{
	char *p, *ret;
	long pid;
	static gboolean shown_error = FALSE;
	gboolean have_error = FALSE;

	/* recheck for mdm */
	mdm_running = mdmcomm_is_daemon_running (FALSE);

	if ( ! mdm_running)
		return;

	ret = mdmcomm_send_cmd_to_daemon (MDM_SUP_GREETERPIDS);
	if (ret == NULL)
		return;
	p = strchr (ret, ' ');
	if (p == NULL) {
		g_free (ret);
		return;
	}
	p++;

	for (;;) {
		if (sscanf (p, "%ld", &pid) != 1) {
			g_free (ret);
			goto check_update_error;
		}

		/* sanity */
		if (pid <= 0)
			continue;

		if (kill (pid, SIGHUP) != 0)
			have_error = TRUE;
		p = strchr (p, ';');
		if (p == NULL) {
			g_free (ret);
			goto check_update_error;
		}
		p++;
	}

check_update_error:
	if ( ! shown_error && have_error) {
		GtkWidget *setup_dialog;
		GtkWidget *dlg;
		setup_dialog = glade_xml_get_widget (xml, "setup_dialog");
		dlg = hig_dialog_new (GTK_WINDOW (setup_dialog),
				      GTK_DIALOG_MODAL |
				      GTK_DIALOG_DESTROY_WITH_PARENT,
				      GTK_MESSAGE_ERROR,
				      GTK_BUTTONS_OK,
				      _("An error occurred while "
					"trying to contact the "
					"login screens.  Not all "
					"updates may have taken "
					"effect."),
				      "");
		gtk_dialog_set_has_separator (GTK_DIALOG (dlg), FALSE);
		gtk_dialog_run (GTK_DIALOG (dlg));
		gtk_widget_destroy (dlg);
		shown_error = TRUE;
	}
}

static gboolean
the_timeout (gpointer data)
{
	GtkWidget *widget = data;
	gboolean (*func) (GtkWidget *);

	func = g_object_get_data (G_OBJECT (widget), "timeout_func");

	if ( ! (*func) (widget)) {
		g_object_set_data (G_OBJECT (widget), "change_timeout", NULL);
		g_object_set_data (G_OBJECT (widget), "timeout_func", NULL);
		timeout_widgets = g_list_remove (timeout_widgets, widget);
		return FALSE;
	} else {
		return TRUE;
	}
}

static void
run_timeout (GtkWidget *widget, guint tm, gboolean (*func) (GtkWidget *))
{
	guint id = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (widget),
							"change_timeout"));
	if (id != 0) {
		g_source_remove (id);
	} else {
		timeout_widgets = g_list_prepend (timeout_widgets, widget);
	}

	id = g_timeout_add (tm, the_timeout, widget);
	g_object_set_data (G_OBJECT (widget), "timeout_func", func);

	g_object_set_data (G_OBJECT (widget), "change_timeout",
			   GUINT_TO_POINTER (id));
}

static void
update_key (const char *key)
{
	if (key == NULL)
	       return;

	/* recheck for mdm */
	mdm_running = mdmcomm_is_daemon_running (FALSE);

	if (mdm_running) {
		char *ret;
		char *s = g_strdup_printf ("%s %s", MDM_SUP_UPDATE_CONFIG,
					   key);
		ret = mdmcomm_send_cmd_to_daemon (s);
		g_free (s);
		g_free (ret);
	}
}

static void
mdm_setup_config_set_bool (const char *key, gboolean val)
{	
	GKeyFile *custom_cfg;        
	custom_cfg = mdm_common_config_load (custom_config_file, NULL);	    
	mdm_common_config_set_boolean (custom_cfg, key, val);
	mdm_common_config_save (custom_cfg, custom_config_file, NULL);	
	g_key_file_free (custom_cfg);
	update_key (key);
}

static void
mdm_setup_config_set_int (const char *key, int val)
{
	GKeyFile *custom_cfg;        
	custom_cfg = mdm_common_config_load (custom_config_file, NULL);	
	mdm_common_config_set_int (custom_cfg, key, val);	
	mdm_common_config_save (custom_cfg, custom_config_file, NULL);
	g_key_file_free (custom_cfg);
	update_key (key);
}

static void
mdm_setup_config_set_string (const char *key, gchar *val)
{	
	GKeyFile *custom_cfg;
 	custom_cfg = mdm_common_config_load (custom_config_file, NULL);	
	mdm_common_config_set_string (custom_cfg, key, val);
	mdm_common_config_save (custom_cfg, custom_config_file, NULL);
	g_key_file_free (custom_cfg);
	update_key (key);
}

static gboolean
bool_equal (gboolean a, gboolean b)
{
	if ((a && b) || (!a && !b))
		return TRUE;
	else
		return FALSE;
}

static gboolean
toggle_timeout (GtkWidget *toggle)
{
	const char *key = g_object_get_data (G_OBJECT (toggle), "key");
	gboolean    val = mdm_config_get_bool ((gchar *)key);

	if (strcmp (ve_sure_string (key), MDM_KEY_DEFAULT_SESSION) == 0) {
		/* Once enabled write the curently selected item
		   in the combobox widget, otherwise disable
		   the config entry, i.e. write an empty string */		

		if (GTK_TOGGLE_BUTTON (toggle)->active == TRUE) {
			gint selected;
			gchar *value;
			gchar *new_val = NULL;			
			GtkWidget *default_session_combobox;
			
			default_session_combobox = glade_xml_get_widget (xml, "default_session_combobox");

			selected = gtk_combo_box_get_active (GTK_COMBO_BOX (default_session_combobox));
			
			value = mdm_config_get_string ((gchar *)key);
									
			new_val = g_strdup ((gchar*) g_list_nth_data (sessions, selected));					
			
			if (strcmp (ve_sure_string (value), ve_sure_string (new_val)) != 0)				
				mdm_setup_config_set_string (key, ve_sure_string (new_val));
			
			g_free (value);
			g_free (new_val);
		}
		else
			mdm_setup_config_set_string (key, "");		    
	}	
	else {
		/* All other cases */
		if ( ! bool_equal (val, GTK_TOGGLE_BUTTON (toggle)->active)) {
			mdm_setup_config_set_bool (key, GTK_TOGGLE_BUTTON (toggle)->active);
		}
	}	

	return FALSE;
}

/* Forward declarations */
static void
setup_user_combobox_list (const char *name, const char *key);

static gboolean
intspin_timeout (GtkWidget *spin)
{
	const char *key = g_object_get_data (G_OBJECT (spin), "key");
	int new_val = gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (spin));
	int val;
	gboolean greeters_need_update = FALSE;

	val = mdm_config_get_int ((gchar *)key);	

	if (val != new_val)
		mdm_setup_config_set_int (key, new_val);

	if (greeters_need_update) {
		setup_user_combobox_list ("autologin_combo",
					  MDM_KEY_AUTOMATIC_LOGIN);
		setup_user_combobox_list ("timedlogin_combo",
					  MDM_KEY_TIMED_LOGIN);
		update_greeters ();
	}

	return FALSE;
}

static gboolean
combobox_timeout (GtkWidget *combo_box)
{
	const char *key = g_object_get_data (G_OBJECT (combo_box), "key");
	int selected = gtk_combo_box_get_active (GTK_COMBO_BOX (combo_box));

	/* Local Greeter Comboboxes */
	if (strcmp (ve_sure_string (key), MDM_KEY_GREETER) == 0) {

		gchar *old_key_val;
		gchar *new_key_val;		

		old_key_val = mdm_config_get_string ((gchar *)key);
		new_key_val = NULL;

		if (selected == LOCAL_THEMED) {
			new_key_val = g_strdup (LIBEXECDIR "/mdmgreeter");			
		}
		else if (selected == LOCAL_HTML) {
			new_key_val = g_strdup (LIBEXECDIR "/mdmwebkit");
		}
		else {
			new_key_val = g_strdup (LIBEXECDIR "/mdmlogin");			
		}		
		
		if (new_key_val && 
		    strcmp (ve_sure_string (old_key_val), ve_sure_string (new_key_val)) != 0) {	
		    
			mdm_setup_config_set_string (key, new_key_val);			
		}
		
		update_greeters ();
				
		g_free (new_key_val);
	}	

	/* Automatic Login Combobox */
	else if (strcmp (ve_sure_string (key), MDM_KEY_AUTOMATIC_LOGIN) == 0 ||
	           strcmp (ve_sure_string (key), MDM_KEY_TIMED_LOGIN) == 0) {

		GtkTreeIter iter;
		char *new_val = NULL;
		gchar *val;
		
		if (gtk_combo_box_get_active_iter (GTK_COMBO_BOX (combo_box), 
		    &iter)) {
			gtk_tree_model_get (gtk_combo_box_get_model (
				GTK_COMBO_BOX (combo_box)), &iter,
				0, &new_val, -1);
		}
		else {
			/* The selection was typed so there are two possibilities
			   1. The typed value is a user
			   2. The typed value is a script, garbage, ete (anything but an
			   existing user)
			   If its case 1. then we check if the user matches the MinimalUID 
			   criteria or is not root. If its case 2. we do not do any checking */
			
			new_val = gtk_combo_box_get_active_text (GTK_COMBO_BOX (combo_box));
			if (mdm_is_user_valid (ve_sure_string (new_val))) {
				gint user_uid = mdm_user_uid (new_val);
				gint MdmMinimalUID = mdm_config_get_int (MDM_KEY_MINIMAL_UID);
				
				if (user_uid == 0 || user_uid < MdmMinimalUID) {
					/* we can't accept users that have uid lower
					   than minimal uid, or uid = 0 (root) */
					gchar *str;
					GtkWidget *dialog;
					GtkWidget *setup_dialog;
					
					if (mdm_user_uid (new_val) == 0)
						str = g_strdup (_("Autologin or timed login to the root account is forbidden."));
					else 
						str = g_strdup_printf (_("The %s user UID is lower than 'MinimalUID'."), new_val);				
					setup_dialog = glade_xml_get_widget(xml, "setup_dialog");
										
					dialog = hig_dialog_new (GTK_WINDOW (setup_dialog),
								 GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
								 GTK_MESSAGE_ERROR,
								 GTK_BUTTONS_OK,
								 _("User not allowed"),
								 str);
					gtk_dialog_run (GTK_DIALOG (dialog));
					gtk_widget_destroy (dialog);
					g_free (str);
					
					gtk_combo_box_set_active (GTK_COMBO_BOX (combo_box), 0);
					return TRUE;
				}
			}
		}


		val = mdm_config_get_string ((gchar *)key);
		if (new_val &&
		    strcmp (ve_sure_string (val), ve_sure_string (new_val)) != 0) {

			mdm_setup_config_set_string (key, new_val);
		}
		g_free (new_val);
	}	
	/* Default session combobox*/
	else if (strcmp (ve_sure_string (key), MDM_KEY_DEFAULT_SESSION) == 0) {
		/* First we get the selected index. Next we lookup the actual
		   filename in the List of sessions */
		gchar *val;
		gchar *new_val = NULL;
		
		val = mdm_config_get_string ((gchar *)key);
		new_val = g_strdup ((gchar*) g_list_nth_data (sessions, selected));
		
		if (strcmp (ve_sure_string (val), ve_sure_string (new_val)) != 0)
			mdm_setup_config_set_string (key,  ve_sure_string (new_val));

		g_free (new_val);
		g_free (val);		
	}
	/* Primary monitor combobox*/
	else if (strcmp (ve_sure_string (key), MDM_KEY_PRIMARY_MONITOR) == 0) {
		/* First we get the selected index. Next we lookup the actual
		   filename in the List of sessions */
		gchar *val;
		gchar *new_val = NULL;
		
		val = mdm_config_get_string ((gchar *)key);
		new_val = g_strdup ((gchar*) g_list_nth_data (monitors, selected-1));

		if (new_val == NULL || new_val == " ") {
			g_free (new_val);
			new_val = g_strdup ("None");
		}

		if (strcmp (ve_sure_string (val), ve_sure_string (new_val)) != 0)
			mdm_setup_config_set_string (key,  ve_sure_string (new_val));

		g_free (new_val);
		g_free (val);
	}
	return FALSE;
}

static void
toggle_toggled (GtkWidget *toggle)
{
	run_timeout (toggle, 200, toggle_timeout);
}

static void
intspin_changed (GtkWidget *spin)
{
	run_timeout (spin, 500, intspin_timeout);
}

static void
combobox_changed (GtkWidget *combobox)
{
	const char *key = g_object_get_data (G_OBJECT (combobox), "key");
	
	if (strcmp (ve_sure_string (key), MDM_KEY_GREETER) == 0) {

		GtkWidget *local_plain_vbox;
		GtkWidget *local_themed_vbox;
		GtkWidget *local_html_vbox;
		gint selected;
		
		local_plain_vbox = glade_xml_get_widget (xml, "local_plain_properties_vbox");
		local_themed_vbox = glade_xml_get_widget (xml, "local_themed_properties_vbox");
		local_html_vbox = glade_xml_get_widget (xml, "local_html_properties_vbox");

		selected = gtk_combo_box_get_active (GTK_COMBO_BOX (combobox));

		if (selected == LOCAL_THEMED) {						
			gtk_widget_hide (local_plain_vbox);			
			gtk_widget_show (local_themed_vbox);
			gtk_widget_hide (local_html_vbox);
		}
		else if (selected == LOCAL_HTML) {						
			gtk_widget_hide (local_plain_vbox);			
			gtk_widget_hide (local_themed_vbox);
			gtk_widget_show (local_html_vbox);
		}
		else {  /* Plain */
			gtk_widget_show (local_plain_vbox);
			gtk_widget_hide (local_themed_vbox);
			gtk_widget_hide (local_html_vbox);
		}
	}		
		
	run_timeout (combobox, 500, combobox_timeout);
}

static void
timeout_remove (GtkWidget *widget)
{
	gboolean (*func) (GtkWidget *);
	guint id = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (widget),
							"change_timeout"));
	if (id != 0) {
		g_source_remove (id);
		g_object_set_data (G_OBJECT (widget), "change_timeout", NULL);
	}

	func = g_object_get_data (G_OBJECT (widget), "timeout_func");
	if (func != NULL) {
		(*func) (widget);
		g_object_set_data (G_OBJECT (widget), "timeout_func", NULL);
	}
}

static void
timeout_remove_all (void)
{
	GList *li, *list;

	list = timeout_widgets;
	timeout_widgets = NULL;

	for (li = list; li != NULL; li = li->next) {
		timeout_remove (li->data);
		li->data = NULL;
	}
	g_list_free (list);
}

static void
toggle_toggled_sensitivity_positive (GtkWidget *toggle, GtkWidget *depend)
{
	gtk_widget_set_sensitive (depend, gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (toggle)));
}

static GtkWidget *
setup_notify_toggle (const char *name,
		     const char *key)
{
	GtkWidget *toggle;
	gboolean val;

	toggle = glade_xml_get_widget (xml, name);
	val    = mdm_config_get_bool ((gchar *)key);

	gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (toggle), val);

	g_object_set_data_full (G_OBJECT (toggle),
	                        "key", g_strdup (key),
	                        (GDestroyNotify) g_free);

	if (strcmp ("autologin", ve_sure_string (name)) == 0) {

		GtkWidget *autologin_label;
		GtkWidget *autologin_combo;

		autologin_label = glade_xml_get_widget (xml, "autologin_label");
		autologin_combo = glade_xml_get_widget (xml, "autologin_combo");

		gtk_widget_set_sensitive (autologin_label, val);
		gtk_widget_set_sensitive (autologin_combo, val);

		g_signal_connect (G_OBJECT (toggle), "toggled",	
		                  G_CALLBACK (toggle_toggled_sensitivity_positive), 
		                  autologin_label);
		g_signal_connect (G_OBJECT (toggle), "toggled",	
		                  G_CALLBACK (toggle_toggled_sensitivity_positive), 
		                  autologin_combo);
		g_signal_connect (G_OBJECT (toggle), "toggled",
		                  G_CALLBACK (toggle_toggled), toggle);
	}
	else if (strcmp ("timedlogin", ve_sure_string (name)) == 0) {

		GtkWidget *timedlogin_label;
		GtkWidget *timedlogin_combo;
		GtkWidget *timedlogin_seconds_label;
		GtkWidget *timedlogin_seconds_spin_button;
		GtkWidget *timedlogin_seconds_units;
		
		timedlogin_label = glade_xml_get_widget (xml, "timed_login_label");
		timedlogin_combo = glade_xml_get_widget (xml, "timedlogin_combo");
		timedlogin_seconds_label = glade_xml_get_widget (xml, "timedlogin_seconds_label");
		timedlogin_seconds_spin_button = glade_xml_get_widget (xml,"timedlogin_seconds");
		timedlogin_seconds_units = glade_xml_get_widget (xml,"timedlogin_seconds_units");

		gtk_widget_set_sensitive (timedlogin_label, val);
		gtk_widget_set_sensitive (timedlogin_combo, val);
		gtk_widget_set_sensitive (timedlogin_seconds_label, val);
		gtk_widget_set_sensitive (timedlogin_seconds_spin_button, val);
		gtk_widget_set_sensitive (timedlogin_seconds_units, val);		

		g_signal_connect (G_OBJECT (toggle), "toggled",
		                  G_CALLBACK (toggle_toggled), toggle);
		g_signal_connect (G_OBJECT (toggle), "toggled",	
		                  G_CALLBACK (toggle_toggled_sensitivity_positive), timedlogin_label);
		g_signal_connect (G_OBJECT (toggle), "toggled",	
		                  G_CALLBACK (toggle_toggled_sensitivity_positive), timedlogin_combo);
		g_signal_connect (G_OBJECT (toggle), "toggled",	
		                  G_CALLBACK (toggle_toggled_sensitivity_positive), timedlogin_seconds_label);
		g_signal_connect (G_OBJECT (toggle), "toggled",	
		                  G_CALLBACK (toggle_toggled_sensitivity_positive), timedlogin_seconds_spin_button);
		g_signal_connect (G_OBJECT (toggle), "toggled",	
		                  G_CALLBACK (toggle_toggled_sensitivity_positive), timedlogin_seconds_units);		
	}	
	else {
		g_signal_connect (G_OBJECT (toggle), "toggled",
		                  G_CALLBACK (toggle_toggled), NULL);
	}

	return toggle;
}

static void
root_not_allowed (GtkWidget *combo_box)
{
	static gboolean warned = FALSE;
	const char *text = NULL;
	GtkTreeIter iter;

	if (warned)
		return;

	if (gtk_combo_box_get_active_iter (GTK_COMBO_BOX (combo_box), 
	    &iter)) {
		gtk_tree_model_get (gtk_combo_box_get_model (
			GTK_COMBO_BOX (combo_box)), &iter,
				0, &text, -1);
		}

	if ( ! ve_string_empty (text) &&
	    strcmp (text, get_root_user ()) == 0) {
		GtkWidget *dlg = hig_dialog_new (NULL /* parent */,
						 GTK_DIALOG_MODAL /* flags */,
						 GTK_MESSAGE_ERROR,
						 GTK_BUTTONS_OK,
						 _("Autologin or timed login to the root account is not allowed."),
						 "");
		if (RUNNING_UNDER_MDM)
			setup_cursor (GDK_LEFT_PTR);
		gtk_dialog_run (GTK_DIALOG (dlg));
		gtk_widget_destroy (dlg);
		warned = TRUE;
	}
}

static gint
users_string_compare_func (gconstpointer a, gconstpointer b)
{
	return strcmp(a, b);
}

static gboolean
dir_exists (const char *parent, const char *dir)
{
	DIR *dp = opendir (parent);
	struct dirent *dent;
	
	if (dp == NULL)
		return FALSE;

	while ((dent = readdir (dp)) != NULL) {
		if (strcmp (ve_sure_string (dent->d_name), ve_sure_string (dir)) == 0) {
			closedir (dp);
			return TRUE;
		}
	}
	closedir (dp);
	return FALSE;
}

/* Sets up Automatic Login Username and Timed Login User entry comboboxes
 * from the general configuration tab. */
static void
setup_user_combobox_list (const char *name, const char *key)
{
	GtkListStore *combobox_store = NULL;
	GtkWidget    *combobox_entry = glade_xml_get_widget (xml, name);
	GtkTreeIter iter;
	GList *users = NULL;
	GList *users_string = NULL;
	GList *li;
	static gboolean MDM_IS_LOCAL = FALSE;
	char *selected_user;
	gint size_of_users = 0;
	int selected = -1;
	int cnt;

	combobox_store = gtk_list_store_new (USERLIST_NUM_COLUMNS, G_TYPE_STRING);
	selected_user  = mdm_config_get_string ((gchar *)key);

	/* normally empty */
	//users_string = g_list_append (users_string, g_strdup (""));

	if ( ! ve_string_empty (selected_user))
		users_string = g_list_append (users_string, g_strdup (selected_user));

	if (ve_string_empty (g_getenv ("MDM_IS_LOCAL")))
		MDM_IS_LOCAL = FALSE;
	else
		MDM_IS_LOCAL = TRUE;

	mdm_users_init (&users, &users_string, selected_user, NULL,
	                &size_of_users, MDM_IS_LOCAL, FALSE);

	users_string = g_list_sort (users_string, users_string_compare_func);

	cnt=0;
	for (li = users_string; li != NULL; li = li->next) {
		if (!dir_exists ("/home/.ecryptfs", li->data)) {
			if (strcmp (li->data, ve_sure_string (selected_user)) == 0)
				selected=cnt;
			gtk_list_store_append (combobox_store, &iter);
			gtk_list_store_set(combobox_store, &iter, USERLIST_NAME, li->data, -1);
			cnt++;
		}
	}

	gtk_combo_box_set_model (GTK_COMBO_BOX (combobox_entry),
		GTK_TREE_MODEL (combobox_store));

	if (selected != -1)
		gtk_combo_box_set_active (GTK_COMBO_BOX (combobox_entry), selected);

	g_list_foreach (users, (GFunc)g_free, NULL);
	g_list_free (users);
	g_list_foreach (users_string, (GFunc)g_free, NULL);
	g_list_free (users_string);
}

static void
setup_user_combobox (const char *name, const char *key)
{
	GtkWidget *combobox_entry = glade_xml_get_widget (xml, name);
	setup_user_combobox_list (name, key);
	g_object_set_data_full (G_OBJECT (combobox_entry), "key",
	                        g_strdup (key), (GDestroyNotify) g_free);
	g_signal_connect (G_OBJECT (combobox_entry), "changed",
	                  G_CALLBACK (combobox_changed), NULL);
	g_signal_connect (G_OBJECT (combobox_entry), "changed",
	                  G_CALLBACK (root_not_allowed), NULL);
}

static void
setup_intspin (const char *name,
	       const char *key)
{
	GtkWidget *spin = glade_xml_get_widget (xml, name);
	int val = mdm_config_get_int ((gchar *)key);

	g_object_set_data_full (G_OBJECT (spin),
				"key", g_strdup (key),
				(GDestroyNotify) g_free);

	gtk_spin_button_set_value (GTK_SPIN_BUTTON (spin), val);

	g_signal_connect (G_OBJECT (spin), "value_changed",
			  G_CALLBACK (intspin_changed), NULL);
}

static gboolean
greeter_toggle_timeout (GtkWidget *toggle)
{
	const char *key = g_object_get_data (G_OBJECT (toggle), "key");
	gboolean val = mdm_config_get_bool ((gchar *)key);

	if ( ! bool_equal (val, GTK_TOGGLE_BUTTON (toggle)->active)) {
        
		mdm_setup_config_set_bool (key, GTK_TOGGLE_BUTTON (toggle)->active);
		update_greeters ();		
	}

	return FALSE;
}

static void
greeter_toggle_toggled (GtkWidget *toggle)
{
	run_timeout (toggle, 500, greeter_toggle_timeout);
}

static void
sensitive_entry_toggled (GtkWidget *toggle, gpointer data)
{
	GtkWidget *widget = data;
	gboolean val;

	val = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (toggle));

	if (val == FALSE) {
		gtk_widget_set_sensitive (widget, TRUE);
	} else {
		gtk_widget_set_sensitive (widget, FALSE);
	}

	run_timeout (toggle, 500, greeter_toggle_timeout);
}

static gboolean
local_background_type_toggle_timeout (GtkWidget *toggle)
{
	GtkWidget *color_radiobutton;
	GtkWidget *image_radiobutton;
	gboolean image_value;
	gboolean color_value;

	image_radiobutton = glade_xml_get_widget (xml, "local_background_image_checkbutton");
	color_radiobutton = glade_xml_get_widget (xml, "local_background_color_checkbutton");

	image_value = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (image_radiobutton));
	color_value = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (color_radiobutton));
		
	if (image_value == TRUE && color_value == TRUE) {		
		/* Image & color */
                mdm_setup_config_set_int (MDM_KEY_BACKGROUND_TYPE, 1);
	}
	else if (image_value == FALSE && color_value == TRUE) {
		/* Color only */
		mdm_setup_config_set_int (MDM_KEY_BACKGROUND_TYPE, 2);
	}
	else if (image_value == TRUE && color_value == FALSE) {
		/* Image only*/
		mdm_setup_config_set_int (MDM_KEY_BACKGROUND_TYPE, 3);
	}
	else {
		/* No Background */
		mdm_setup_config_set_int (MDM_KEY_BACKGROUND_TYPE, 0);
	}
		
	update_greeters ();
	return FALSE;
}

static void
local_background_type_toggled (GtkWidget *toggle)
{
	run_timeout (toggle, 200, local_background_type_toggle_timeout);
}

static void
setup_greeter_toggle (const char *name,
		      const char *key)
{
	GtkWidget *toggle = glade_xml_get_widget (xml, name);
	gboolean val = mdm_config_get_bool ((gchar *)key);

	g_object_set_data_full (G_OBJECT (toggle), "key", g_strdup (key),
		(GDestroyNotify) g_free);

	gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (toggle), val);

	if (strcmp ("sg_defaultwelcome", ve_sure_string (name)) == 0) {
		GtkWidget *welcome = glade_xml_get_widget (xml, "welcome");
		GtkWidget *custom = glade_xml_get_widget (xml, "sg_customwelcome");

		gtk_widget_set_sensitive (welcome, !val);
		gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (custom), !val);

		g_signal_connect (G_OBJECT (toggle), "toggled",	
			G_CALLBACK (sensitive_entry_toggled), welcome);
	
	}
		
	g_signal_connect (G_OBJECT (toggle), "toggled",
		G_CALLBACK (greeter_toggle_toggled), NULL);
}

static gboolean
greeter_color_timeout (GtkWidget *picker)
{
	const char *key = g_object_get_data (G_OBJECT (picker), "key");
	GdkColor color_val;
	char *val, *color;

	gtk_color_button_get_color (GTK_COLOR_BUTTON (picker), &color_val);		
	
	color = g_strdup_printf ("#%02x%02x%02x",
	                         (guint16)color_val.red / 256, 
	                         (guint16)color_val.green / 256, 
	                         (guint16)color_val.blue / 256);

	val = mdm_config_get_string ((gchar *)key);

	if (strcmp (ve_sure_string (val), ve_sure_string (color)) != 0) {
		mdm_setup_config_set_string (key, ve_sure_string (color));
		update_greeters ();
	}

	g_free (color);

	return FALSE;
}

static void
greeter_color_changed (GtkWidget *picker,
		       guint r, guint g, guint b, guint a)
{
	run_timeout (picker, 500, greeter_color_timeout);
}

static void
setup_greeter_color (const char *name,
		     const char *key)
{
	GtkWidget *picker = glade_xml_get_widget (xml, name);
	char *val = mdm_config_get_string ((gchar *)key);

	g_object_set_data_full (G_OBJECT (picker),
				"key", g_strdup (key),
				(GDestroyNotify) g_free);

        if (val != NULL) {
		GdkColor color;

		if (gdk_color_parse (val, &color)) {
			gtk_color_button_set_color (GTK_COLOR_BUTTON (picker), &color);
		}
	}

	g_signal_connect (G_OBJECT (picker), "color_set",
			  G_CALLBACK (greeter_color_changed), NULL);
}

typedef enum {
	BACKIMAGE,
	LOGO
} ImageType;

typedef struct _ImageData {
	GtkWidget *image;
	gchar *filename;
	gchar *key;
} ImageData;

static gboolean
greeter_entry_untranslate_timeout (GtkWidget *entry)
{
	GKeyFile   *custom_cfg;
	const char *key;
	const char *text;
	char       *config_group;
	char       *config_key;

	custom_cfg = mdm_common_config_load (custom_config_file, NULL);
        key = g_object_get_data (G_OBJECT (entry), "key");

	text = gtk_entry_get_text (GTK_ENTRY (entry));

	config_group = config_key = NULL;
	if (! mdm_common_config_parse_key_string (key, &config_group, &config_key, NULL, NULL)) {
		goto out;
	}

	mdm_setup_config_set_string (key, (char *)ve_sure_string (text));
	update_greeters ();

 out:
	g_key_file_free (custom_cfg);

	return FALSE;
}


static void
greeter_entry_untranslate_changed (GtkWidget *entry)
{
	run_timeout (entry, 500, greeter_entry_untranslate_timeout);
}

static void
setup_greeter_untranslate_entry (const char *name,
				 const char *key)
{
	GtkWidget *entry = glade_xml_get_widget (xml, name);
	char *val;

	val = mdm_config_get_translated_string ((gchar *)key);

	g_object_set_data_full (G_OBJECT (entry),
				"key", g_strdup (key),
				(GDestroyNotify) g_free);

	gtk_entry_set_text (GTK_ENTRY (entry), ve_sure_string (val));

	g_signal_connect (G_OBJECT (entry), "changed",
			  G_CALLBACK (greeter_entry_untranslate_changed),
			  NULL);

	g_free (val);
}

/* Make label surrounded by tag (if tag = "tag" add <tag>text</tag>) */
static void
glade_helper_tagify_label (GladeXML *xml,
			   const char *name,
			   const char *tag)
{
	const char *lbl;
	char *s;
	GtkWidget *label;

	label = glade_xml_get_widget (xml, name);
	if (GTK_IS_BUTTON (label)) {
		label = GTK_BIN(label)->child;
	}

	lbl = gtk_label_get_label (GTK_LABEL (label));
	s = g_strdup_printf ("<%s>%s</%s>", tag, lbl, tag);
	gtk_label_set_use_markup (GTK_LABEL (label), TRUE);
	gtk_label_set_label (GTK_LABEL (label), s);
	g_free (s);
}

static char *
get_theme_dir (void)
{
	/*
	 * We always want to free the value returned from this function, since
	 * we use strdup to build a reasonable value if the configuration value
	 * is not good.  So use g_strdup here.
	 */
	char *theme_dir = g_strdup (mdm_config_get_string (MDM_KEY_GRAPHICAL_THEME_DIR));

	if (theme_dir == NULL ||
	    theme_dir[0] == '\0' ||
	    g_access (theme_dir, R_OK) != 0) {
		g_free (theme_dir);
		theme_dir = g_strdup (DATADIR "/mdm/themes/");
	}

	return theme_dir;
}

static GtkTreeIter *
read_themes (GtkListStore *store)
{
	gtk_list_store_clear (store);

	GdkPixbuf *pb = NULL;
	GtkTreeIter iter;
	gchar *markup = NULL;
	gboolean selected = FALSE;

	char * active_greeter = mdm_config_get_string(MDM_KEY_GREETER);	

	pb = gdk_pixbuf_new_from_file ("/usr/share/mdm/gtk-preview.png", NULL);
	if (pb != NULL) {
		if (gdk_pixbuf_get_width (pb) > 64 ||
		    gdk_pixbuf_get_height (pb) > 50) {
			GdkPixbuf *pb2;
			pb2 = gdk_pixbuf_scale_simple
				(pb, 64, 50,
				 GDK_INTERP_BILINEAR);
			g_object_unref (G_OBJECT (pb));
			pb = pb2;
		}
	} 

	markup = g_markup_printf_escaped ("<b>GTK</b> <sup><span fgcolor='#5C5C5C' font_size='x-small'>GTK</span></sup>\n<small>%s</small>", _("No theme, just pure GTK"));
	
	if (g_strcmp0 (active_greeter, LIBEXECDIR "/mdmlogin") == 0) {
		selected = TRUE;
	}

	gtk_list_store_append (store, &iter);
	gtk_list_store_set (store, &iter,
			    THEME_COLUMN_SELECTED, selected,
			    THEME_COLUMN_DIR, "",
			    THEME_COLUMN_FILE, "",
			    THEME_COLUMN_SCREENSHOT, pb,
			    THEME_COLUMN_MARKUP, markup,
			    THEME_COLUMN_NAME, "gtk",
			    THEME_COLUMN_DESCRIPTION, "gtk",
			    THEME_COLUMN_TYPE, THEME_TYPE_GTK,
			    -1);	
	
	g_free(markup);

	char *theme_dir = get_theme_dir();
	DIR * dir = opendir(theme_dir);

	if (dir != NULL) {

		struct dirent *dent;
		GtkTreeIter *select_iter = NULL;
		GdkPixbuf *pb = NULL;
		gchar *markup = NULL;	
		
		while ((dent = readdir (dir)) != NULL) {
			char *n, *file, *name, *desc, *ss;
			char *full;
			GtkTreeIter iter;
			gboolean sel_theme;
			GKeyFile *theme_file;

			if (dent->d_name[0] == '.')
				continue;
			n = g_strconcat (theme_dir, "/", dent->d_name,
					 "/GdmGreeterTheme.desktop", NULL);
			if (g_access (n, R_OK) != 0) {
				g_free (n);
				n = g_strconcat (theme_dir, "/", dent->d_name,
						 "/GdmGreeterTheme.info", NULL);
			}
			if (g_access (n, R_OK) != 0) {
				g_free (n);
				continue;
			}

			file = mdm_get_theme_greeter (n, dent->d_name);
			full = g_strconcat (theme_dir, "/", dent->d_name,
					    "/", file, NULL);
			if (g_access (full, R_OK) != 0) {
				g_free (file);
				g_free (full);
				g_free (n);
				continue;
			}
			g_free (full);

			if (g_strcmp0(active_greeter, LIBEXECDIR "/mdmgreeter") == 0 &&
				g_strcmp0 (ve_sure_string (dent->d_name), ve_sure_string (selected_theme)) == 0)
				sel_theme = TRUE;
			else
				sel_theme = FALSE;

			theme_file = mdm_common_config_load (n, NULL);
			name = NULL;
			mdm_common_config_get_translated_string (theme_file, "GdmGreeterTheme/Name", &name, NULL);
			if (ve_string_empty (name)) {
				g_free (name);
				name = g_strdup (dent->d_name);
			}

			desc = ss = NULL;
			mdm_common_config_get_translated_string (theme_file, "GdmGreeterTheme/Description", &desc, NULL);		
			mdm_common_config_get_translated_string (theme_file, "GdmGreeterTheme/Screenshot", &ss, NULL);

			g_key_file_free (theme_file);

			if (ss != NULL)
				full = g_strconcat (theme_dir, "/", dent->d_name,
						    "/", ss, NULL);
			else
				full = NULL;

			if ( ! ve_string_empty (full) &&
			    g_access (full, R_OK) == 0) {

				pb = gdk_pixbuf_new_from_file (full, NULL);
				if (pb != NULL) {
					if (gdk_pixbuf_get_width (pb) > 64 ||
					    gdk_pixbuf_get_height (pb) > 50) {
						GdkPixbuf *pb2;
						pb2 = gdk_pixbuf_scale_simple
							(pb, 64, 50,
							 GDK_INTERP_BILINEAR);
						g_object_unref (G_OBJECT (pb));
						pb = pb2;
					}
				}
			}			   
						   
			markup = g_markup_printf_escaped ("<b>%s</b> <sup><span fgcolor='#5C5C5C' font_size='x-small'>GDM</span></sup>\n<small>%s</small>", name ? name : "(null)", desc ? desc : "(null)");
			gtk_list_store_append (store, &iter);
			gtk_list_store_set (store, &iter,
					    THEME_COLUMN_SELECTED, sel_theme,			
					    THEME_COLUMN_DIR, dent->d_name,
					    THEME_COLUMN_FILE, file,
					    THEME_COLUMN_SCREENSHOT, pb,
					    THEME_COLUMN_MARKUP, markup,
					    THEME_COLUMN_NAME, name,
					    THEME_COLUMN_DESCRIPTION, desc,
					    THEME_COLUMN_TYPE, THEME_TYPE_GDM,
					    -1);

			if (mdm_config_get_string (MDM_KEY_GRAPHICAL_THEME) != NULL &&
			    strcmp (ve_sure_string (dent->d_name), ve_sure_string (mdm_config_get_string (MDM_KEY_GRAPHICAL_THEME))) == 0) {
				/* anality */ g_free (select_iter);
				select_iter = g_new0 (GtkTreeIter, 1);
				*select_iter = iter;
			}

			g_free (file);
			g_free (name);
			g_free (markup);
			g_free (desc);		
			g_free (ss);
			g_free (full);
			g_free (n);
		}	
		closedir(dir);
		//return select_iter;
	}

	theme_dir = "/usr/share/mdm/html-themes";
	dir = opendir(theme_dir);

	if (dir != NULL) {

		struct dirent *dent;
		GtkTreeIter *select_iter = NULL;
		GdkPixbuf *pb = NULL;
		gchar *markup = NULL;	
		
		while ((dent = readdir (dir)) != NULL) {
			char *n, *name, *desc, *ss;		
			GtkTreeIter iter;
			gboolean sel_theme;
			GKeyFile *theme_file;
			gchar * full;
			
			if (dent->d_name[0] == '.')
				continue;
			n = g_strconcat (theme_dir, "/", dent->d_name,
					 "/theme.info", NULL);		
					 		
			if (g_access (n, R_OK) != 0) {
				g_free (n);
				continue;
			}			
			
			if (g_strcmp0(active_greeter, LIBEXECDIR "/mdmwebkit") == 0 &&
			    g_strcmp0(ve_sure_string (dent->d_name), ve_sure_string (selected_html_theme)) == 0)
				sel_theme = TRUE;
			else
				sel_theme = FALSE;
			
			theme_file = mdm_common_config_load (n, NULL);
			name = NULL;
			mdm_common_config_get_translated_string (theme_file, "Theme/Name", &name, NULL);
			if (ve_string_empty (name)) {
				g_free (name);
				name = g_strdup (dent->d_name);
			}		

			desc = ss = NULL;
			mdm_common_config_get_translated_string (theme_file, "Theme/Description", &desc, NULL);		
			mdm_common_config_get_translated_string (theme_file, "Theme/Screenshot", &ss, NULL);

			g_key_file_free (theme_file);

			if (ss != NULL)
				full = g_strconcat (theme_dir, "/", dent->d_name,
						    "/", ss, NULL);
			else
				full = NULL;

			if ( ! ve_string_empty (full) &&
			    g_access (full, R_OK) == 0) {

				pb = gdk_pixbuf_new_from_file (full, NULL);
				if (pb != NULL) {
					if (gdk_pixbuf_get_width (pb) > 64 ||
					    gdk_pixbuf_get_height (pb) > 50) {
						GdkPixbuf *pb2;
						pb2 = gdk_pixbuf_scale_simple
							(pb, 64, 50,
							 GDK_INTERP_BILINEAR);
						g_object_unref (G_OBJECT (pb));
						pb = pb2;
					}
				}
			}			   
						   
			markup = g_markup_printf_escaped ("<b>%s</b> <sup><span fgcolor='#5C5C5C' font_size='x-small'>HTML</span></sup>\n<small>%s</small>",
	                   name ? name : "(null)",
	                   desc ? desc : "(null)");
	                                 
			gtk_list_store_append (store, &iter);
			gtk_list_store_set (store, &iter,
					    THEME_COLUMN_SELECTED, sel_theme,			
					    THEME_COLUMN_DIR, dent->d_name,
					    THEME_COLUMN_FILE, "",
					    THEME_COLUMN_SCREENSHOT, pb,
					    THEME_COLUMN_MARKUP, markup,
					    THEME_COLUMN_NAME, name,
					    THEME_COLUMN_DESCRIPTION, desc,
					    THEME_COLUMN_TYPE, THEME_TYPE_HTML,
					    -1);

			if (mdm_config_get_string (MDM_KEY_HTML_THEME) != NULL && strcmp (ve_sure_string (dent->d_name), ve_sure_string (mdm_config_get_string (MDM_KEY_HTML_THEME))) == 0) {
				/* anality */ g_free (select_iter);
				select_iter = g_new0 (GtkTreeIter, 1);
				*select_iter = iter;
			}

			g_free (name);
			g_free (markup);
			g_free (desc);		
			g_free (ss);
			g_free (n);
			g_free (full);
		}	
		closedir(dir);
	}



}

/* Enable/disable the delete button when an HTML theme is selected */
static void
gg_html_selection_changed (GtkTreeSelection *selection, gpointer data)
{	
	GtkWidget *theme_list;		
	GtkWidget *delete_button;	
	GtkTreeModel *model;
	GtkTreeIter iter;	
	GValue value  = {0, };			

	delete_button = glade_xml_get_widget (xml, "gg_delete_html_theme");	

	if ( !gtk_tree_selection_get_selected (selection, &model, &iter)) {
		gtk_widget_set_sensitive (delete_button, FALSE);		
		return;
	}

	/* Default to allow deleting of themes */	
	gtk_widget_set_sensitive (delete_button, TRUE);
	
		
	theme_list = glade_xml_get_widget (xml, "gg_html_theme_list");	
	model = gtk_tree_view_get_model (GTK_TREE_VIEW (theme_list));			
		
	/* Do not allow deleting of active themes */
	gtk_tree_model_get_value (model, &iter, THEME_COLUMN_SELECTED, &value);
	if (g_value_get_boolean (&value)) {
		gtk_widget_set_sensitive (delete_button, FALSE);
	}
	g_value_unset (&value);

	/* Do not allow deleting the GTK greeter */
	gtk_tree_model_get_value (model, &iter, THEME_COLUMN_TYPE, &value);
	if (g_value_get_int (&value) == THEME_TYPE_GTK) {
		gtk_widget_set_sensitive (delete_button, FALSE);
	}
	g_value_unset (&value);
}

static void
selected_html_toggled (GtkCellRendererToggle *cell, char *path_str, gpointer data)
{
	GtkTreeModel *model = GTK_TREE_MODEL (data);
	GtkTreeIter selected_iter;
	GtkTreeIter iter;
	GtkTreePath *path;
	GtkTreePath *sel_path = gtk_tree_path_new_from_string (path_str);
	GtkWidget *del_button = glade_xml_get_widget (xml, "gg_delete_html_theme");		

	gtk_tree_model_get_iter (model, &selected_iter, sel_path);
	path = gtk_tree_path_new_first ();	
		
	/* Clear list of all selected themes */
	g_free (selected_html_theme);

	/* Get the new selected theme */
	gtk_tree_model_get (model, &selected_iter, THEME_COLUMN_DIR, &selected_html_theme, -1);

	/* Loop through all themes in list */
	while (gtk_tree_model_get_iter (model, &iter, path)) {
		/* If this toggle was just toggled */
		if (gtk_tree_path_compare (path, sel_path) == 0) {
			gtk_list_store_set (GTK_LIST_STORE (model), &iter, THEME_COLUMN_SELECTED, TRUE, -1); /* Toggle ON */
			gtk_widget_set_sensitive (del_button, FALSE);
		} else {
			gtk_list_store_set (GTK_LIST_STORE (model), &iter, THEME_COLUMN_SELECTED, FALSE, -1); /* Toggle OFF */
		}
		gtk_tree_path_next (path);
	}
	gtk_tree_path_free (path);
	gtk_tree_path_free (sel_path);
	

	GValue value  = {0, };

	gtk_tree_model_get_value (model, &selected_iter, THEME_COLUMN_TYPE, &value);
	int greeter_type = g_value_get_int (&value);
	g_value_unset (&value);

	switch (greeter_type) {
		case THEME_TYPE_GTK:
			mdm_setup_config_set_string (MDM_KEY_GREETER, g_strdup (LIBEXECDIR "/mdmlogin"));
			break;
		case THEME_TYPE_HTML:
			mdm_setup_config_set_string (MDM_KEY_GREETER, g_strdup (LIBEXECDIR "/mdmwebkit"));
			gtk_tree_model_get_value (model, &selected_iter, THEME_COLUMN_DIR, &value);
			mdm_setup_config_set_string (MDM_KEY_HTML_THEME, g_value_get_string (&value));
			g_value_unset (&value);
			break;
		case THEME_TYPE_GDM:
			mdm_setup_config_set_string (MDM_KEY_GREETER, g_strdup (LIBEXECDIR "/mdmgreeter"));
			gtk_tree_model_get_value (model, &selected_iter, THEME_COLUMN_DIR, &value);
			mdm_setup_config_set_string (MDM_KEY_GRAPHICAL_THEME, g_value_get_string (&value));
			g_value_unset (&value);
			break;
	}
			
	update_greeters ();	
}

static gboolean
is_ext (gchar *filename, const char *ext)
{
	const char *dot;

	dot = strrchr (filename, '.');
	if (dot == NULL)
		return FALSE;

	if (strcmp (ve_sure_string (dot), ve_sure_string (ext)) == 0)
		return TRUE;
	else
		return FALSE;
}

static char *
get_the_dir (FILE *fp, char **destination_dir, char **error)
{
	char buf[2048];
	char *dir = NULL;
	int dirlen = 0;
	gboolean got_info = FALSE;
	gboolean read_a_line = FALSE;

	while (fgets (buf, sizeof (buf), fp) != NULL) {
		char *p, *s;

		read_a_line = TRUE;

		p = strchr (buf, '\n');
		if (p != NULL)
			*p = '\0';
		if (dir == NULL) {
			p = strchr (buf, '/');
			if (p != NULL)
				*p = '\0';
			dir = g_strdup (buf);
			if (p != NULL)
				*p = '/';
			dirlen = strlen (dir);

			if (dirlen < 1) {
				*error = _("Archive is not of a subdirectory");
				g_free (dir);
				return NULL;
			}
		}

		if (strncmp (ve_sure_string (buf), ve_sure_string (dir), dirlen) != 0) {
			*error = _("Archive is not of a single subdirectory");
			g_free (dir);
			return NULL;
		}
		
		if ( ! got_info) {
			s = g_strconcat (dir, "/GdmGreeterTheme.info", NULL);
			if (strcmp (ve_sure_string (buf), ve_sure_string (s)) == 0) {
				got_info = TRUE;
				*destination_dir = get_theme_dir();
			}
			g_free (s);
		}

		if ( ! got_info) {
			s = g_strconcat (dir, "/GdmGreeterTheme.desktop", NULL);
			if (strcmp (ve_sure_string (buf), ve_sure_string (s)) == 0)
				got_info = TRUE;
				*destination_dir = get_theme_dir();
			g_free (s);
		}
		
		if ( ! got_info) {
			s = g_strconcat (dir, "/theme.info", NULL);
			if (strcmp (ve_sure_string (buf), ve_sure_string (s)) == 0)
				got_info = TRUE;
				*destination_dir = "/usr/share/mdm/html-themes";
			g_free (s);
		}
	}


	if (got_info)
		return dir;

	if ( ! read_a_line) {
		*error = _("File not a tar.gz or tar archive");
	}
	else
	{
		*error = _("Archive does not include a GdmGreeterTheme.info file");
	}

	g_free (dir);
	return NULL;
}

static char *
get_archive_dir (gchar *filename, char **untar_cmd, char ** destination_dir, char **error)
{
	char *quoted;
	char *unzip;
	char *cmd;
	char *dir;
	FILE *fp;

	*untar_cmd = NULL;
	*destination_dir = NULL;

	*error = NULL;

	if (g_access (filename, F_OK) != 0) {
		*error = _("File does not exist");
		return NULL;
	}

	quoted = g_shell_quote (filename);	

	if (is_ext (filename, ".bz2")) {
		unzip = g_strdup("bunzip2");
	}
	else {
		unzip = g_strdup("gunzip");
	}
	
	cmd = g_strdup_printf ("%s -c %s | tar -tf -", unzip, quoted);	
	fp = popen (cmd, "r");
	printf ("%s\n", cmd);
	g_free (cmd);
	if (fp != NULL) {
		int ret;
		dir = get_the_dir (fp, destination_dir, error);		
		ret = pclose (fp);		
		if (ret == 0 && dir != NULL) {
			*untar_cmd = g_strdup_printf ("%s -c %s | tar -xf -", unzip, quoted);
			g_free (unzip);
			g_free (quoted);
			return dir;
		} 
		g_free (dir);
	}

	/* error due to command failing */
	if (*error != NULL) {
		/* Try uncompressed? */
		cmd = g_strdup_printf ("tar -tf %s", quoted);
		fp = popen (cmd, "r");
		g_free (cmd);
		if (fp != NULL) {
			int ret;
			dir = get_the_dir (fp, destination_dir, error);
			ret = pclose (fp);
			if (ret == 0 && dir != NULL) {
				*untar_cmd = g_strdup_printf ("tar -xf %s", quoted);
				g_free (unzip);
				g_free (quoted);
				return dir;
			} 
			g_free (dir);
		}
	}

	if (*error == NULL)
		*error = _("File not a tar.gz or tar archive");

	g_free (unzip);
	g_free (quoted);

	return NULL;
}

static void
html_install_theme_file (gchar *filename, GtkListStore *store, GtkWindow *parent)
{
	GtkTreeSelection *selection;
	GtkTreeIter *select_iter = NULL;
	GtkWidget *theme_list;	
	gchar *cwd;
	gchar *dir;
	gchar *error;
	gchar *theme_dir;
	gchar *untar_cmd;	
	gboolean success = FALSE;	

	theme_list = glade_xml_get_widget (xml, "gg_html_theme_list");

	cwd = g_get_current_dir ();	

	if ( !g_path_is_absolute (filename)) {

		gchar *temp;
		
		temp = g_build_filename (cwd, filename, NULL);
		g_free (filename);
		filename = temp;
	}
	
	dir = get_archive_dir (filename, &untar_cmd, &theme_dir, &error);
	
	/* FIXME: perhaps do a little bit more sanity checking of
	 * the archive */

	if (dir == NULL) {

		GtkWidget *dialog;
		gchar *msg;

		msg = g_strdup_printf (_("%s"), error);

		dialog = hig_dialog_new (GTK_WINDOW (parent),
					 GTK_DIALOG_MODAL | 
					 GTK_DIALOG_DESTROY_WITH_PARENT,
					 GTK_MESSAGE_ERROR,
					 GTK_BUTTONS_OK,
					 _("Not a theme archive"),
					 msg);
		gtk_dialog_run (GTK_DIALOG (dialog));
		gtk_widget_destroy (dialog);
		g_free (theme_dir);
		g_free (untar_cmd);
		g_free (cwd);
		g_free (msg);
		return;
	}

	if (dir_exists (theme_dir, dir)) {

		GtkWidget *button;
		GtkWidget *dialog;
		gchar *fname;
		gchar *s;

		fname = ve_filename_to_utf8 (dir);

		/* FIXME: if exists already perhaps we could also have an
		 * option to change the dir name */
		s = g_strdup_printf (_("Theme directory '%s' seems to be already "
				       "installed. Install again anyway?"),
				     fname);
		
		dialog = hig_dialog_new (GTK_WINDOW (parent),
					 GTK_DIALOG_MODAL | 
					 GTK_DIALOG_DESTROY_WITH_PARENT,
					 GTK_MESSAGE_QUESTION,
					 GTK_BUTTONS_NONE,
					 s,
					 "");
		g_free (fname);
		g_free (s);

		button = gtk_button_new_from_stock (GTK_STOCK_CANCEL);
		gtk_dialog_add_action_widget (GTK_DIALOG (dialog), button, GTK_RESPONSE_NO);
		GTK_WIDGET_SET_FLAGS (button, GTK_CAN_DEFAULT);
		gtk_widget_show (button);

		button = gtk_button_new_from_stock ("_Install Anyway");
		gtk_dialog_add_action_widget (GTK_DIALOG (dialog), button, GTK_RESPONSE_YES);
		GTK_WIDGET_SET_FLAGS (button, GTK_CAN_DEFAULT);
		gtk_widget_show (button);

		gtk_dialog_set_default_response (GTK_DIALOG (dialog),
						 GTK_RESPONSE_YES);

		gtk_dialog_set_has_separator (GTK_DIALOG (dialog), FALSE);

		if (gtk_dialog_run (GTK_DIALOG (dialog)) != GTK_RESPONSE_YES) {
			gtk_widget_destroy (dialog);
			g_free (theme_dir);
			g_free (untar_cmd);
			g_free (cwd);
			g_free (dir);
			return;
		}
		gtk_widget_destroy (dialog);
	}
	
	g_assert (untar_cmd != NULL);

	if (g_chdir (theme_dir) == 0 &&
	    /* this is a security sanity check */
	    strchr (dir, '/') == NULL &&
	    system (untar_cmd) == 0) {

		gchar *argv[5];
		gchar *quoted;		

		quoted = g_strconcat ("./", dir, NULL);		
		success = TRUE;

		/* HACK! */
		argv[0] = "chown";
		argv[1] = "-R";
		argv[2] = "root:root";
		argv[3] = quoted;
		argv[4] = NULL;
		simple_spawn_sync (argv);

		argv[0] = "chmod";
		argv[1] = "-R";
		argv[2] = "a+r";
		argv[3] = quoted;
		argv[4] = NULL;
		simple_spawn_sync (argv);

		argv[0] = "chmod";
		argv[1] = "a+x";
		argv[2] = quoted;
		argv[3] = NULL;
		simple_spawn_sync (argv);

		g_free (quoted);		
	}
	
	if (!success) {
	
		GtkWidget *dialog;
		
		dialog = hig_dialog_new (GTK_WINDOW (parent),
					 GTK_DIALOG_MODAL | 
					 GTK_DIALOG_DESTROY_WITH_PARENT,
					 GTK_MESSAGE_ERROR,
					 GTK_BUTTONS_OK,
					 _("Some error occurred when "
					   "installing the theme"),
					 "");
		gtk_dialog_run (GTK_DIALOG (dialog));
		gtk_widget_destroy (dialog);
	}

	select_iter = read_themes (store);
		
	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (theme_list));

	if (select_iter != NULL) {
		gtk_tree_selection_select_iter (selection, select_iter);
		g_free (select_iter);
	}
	
	g_free (untar_cmd);	
	g_free (dir);
	g_free (cwd);
}

static void
html_theme_install_response (GtkWidget *chooser, gint response, gpointer data)
{
	GtkListStore *store = data;
	gchar *filename;

	if (response != GTK_RESPONSE_OK) {
		gtk_widget_destroy (chooser);
		return;
	}	

	filename = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (chooser));	
	
	if (filename == NULL) {
	
		GtkWidget *dialog;

		dialog = hig_dialog_new (GTK_WINDOW (chooser),
					 GTK_DIALOG_MODAL | 
					 GTK_DIALOG_DESTROY_WITH_PARENT,
					 GTK_MESSAGE_ERROR,
					 GTK_BUTTONS_OK,
					 _("No file selected"),
					 "");
		gtk_dialog_run (GTK_DIALOG (dialog));
		gtk_widget_destroy (dialog);
		return;
	}

	html_install_theme_file (filename, store, GTK_WINDOW (chooser));
	gtk_widget_destroy (chooser);
	g_free (filename);
}

static void
install_new_html_theme (GtkWidget *button, gpointer data)
{
	GtkListStore *store = data;
	static GtkWidget *chooser = NULL;
	GtkWidget *setup_dialog;
	GtkFileFilter *filter;
	
	setup_dialog = glade_xml_get_widget (xml, "setup_dialog");
	
	chooser = gtk_file_chooser_dialog_new (_("Select Theme Archive"),
					       GTK_WINDOW (setup_dialog),
					       GTK_FILE_CHOOSER_ACTION_OPEN,
					       GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
					       _("_Install"), GTK_RESPONSE_OK,
					       NULL);
	
	filter = gtk_file_filter_new ();
	gtk_file_filter_set_name (filter, _("Theme archives"));
	gtk_file_filter_add_mime_type (filter, "application/x-tar");
	gtk_file_filter_add_mime_type (filter, "application/x-compressed-tar");
	
	gtk_file_chooser_add_filter (GTK_FILE_CHOOSER (chooser), filter);
	
	gtk_file_chooser_set_show_hidden (GTK_FILE_CHOOSER (chooser), FALSE);

	g_signal_connect (G_OBJECT (chooser), "destroy", G_CALLBACK (gtk_widget_destroyed), &chooser);
	g_signal_connect (G_OBJECT (chooser), "response", G_CALLBACK (html_theme_install_response), store);
	
	gtk_widget_show (chooser);
}

static void
preview_mdm (GtkWidget *button)
{	
	gchar * command = g_strdup_printf ("DOING_MDM_DEVELOPMENT=1 %s &", mdm_config_get_string(MDM_KEY_GREETER));		
	system (command);
	g_free(command);
}

static void
delete_html_theme (GtkWidget *button, gpointer data)
{
	GtkListStore *store = data;
	GtkWidget *theme_list;
	GtkWidget *setup_dialog;
	GtkTreeSelection *selection;
    char *dir, *name, * theme_dir;
    gboolean selected;
	int type;
	GtkTreeModel *model;
	GtkTreeIter iter;
	GValue value = {0, };
	GtkWidget *dlg;
	char *s;	

	setup_dialog = glade_xml_get_widget (xml, "setup_dialog");
	theme_list = glade_xml_get_widget (xml, "gg_html_theme_list");	

	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (theme_list));
	gtk_tree_selection_get_selected (selection, &model, &iter);			

	gtk_tree_model_get_value (model, &iter, THEME_COLUMN_SELECTED, &value);
	selected = g_value_get_boolean (&value);
	g_value_unset (&value);

	gtk_tree_model_get_value (model, &iter, THEME_COLUMN_TYPE, &value);
	type = g_value_get_int (&value);
	g_value_unset (&value);

	gtk_tree_model_get_value (model, &iter, THEME_COLUMN_NAME, &value);
	name = g_strdup (g_value_get_string (&value));
	g_value_unset (&value);

	gtk_tree_model_get_value (model, &iter, THEME_COLUMN_DIR, &value);
	dir = g_strdup (g_value_get_string (&value));
	g_value_unset (&value);

	if (selected || type == THEME_TYPE_GTK) {
		// Don't allow the deletion of GTK greeter or the active theme
		g_free (name);
		g_free (dir);
		return;
	}

	s = g_strdup_printf (_("Remove the \"%s\" theme?"), name);
	dlg = hig_dialog_new (GTK_WINDOW (setup_dialog),
			      GTK_DIALOG_MODAL | 
			      GTK_DIALOG_DESTROY_WITH_PARENT,
			      GTK_MESSAGE_WARNING,
			      GTK_BUTTONS_NONE,
			      s,
		 _("If you choose to remove the theme, it will be permanently lost."));
	g_free (s);

	button = gtk_button_new_from_stock (GTK_STOCK_CANCEL);
	gtk_dialog_add_action_widget (GTK_DIALOG (dlg), button, GTK_RESPONSE_NO);
	GTK_WIDGET_SET_FLAGS (button, GTK_CAN_DEFAULT);
	gtk_widget_show (button);

	button = gtk_button_new_from_stock (_("_Remove Theme"));
	gtk_dialog_add_action_widget (GTK_DIALOG (dlg), button, GTK_RESPONSE_YES);
	GTK_WIDGET_SET_FLAGS (button, GTK_CAN_DEFAULT);
	gtk_widget_show (button);
	
	gtk_dialog_set_default_response (GTK_DIALOG (dlg), GTK_RESPONSE_YES);

	if (gtk_dialog_run (GTK_DIALOG (dlg)) == GTK_RESPONSE_YES) {		
		char *cwd = g_get_current_dir ();

		if (type == THEME_TYPE_HTML) {
			theme_dir = "/usr/share/mdm/html-themes";
		}
		else {
			theme_dir = get_theme_dir();
		}

		if (g_chdir (theme_dir) == 0 &&
		    /* this is a security sanity check, since we're doing rm -fR */
		    strchr (dir, '/') == NULL) {
			/* HACK! */			
			char *argv[4];
			GtkTreeIter *select_iter = NULL;
			argv[0] = "/bin/rm";
			argv[1] = "-fR";
			argv[2] = g_strconcat ("./", dir, NULL);			
			argv[3] = NULL;
			simple_spawn_sync (argv);
			g_free (argv[2]);			

			select_iter = read_themes (store);
			
			if (select_iter != NULL) {
				gtk_tree_selection_select_iter (selection, select_iter);
				g_free (select_iter);
			}

		}
		g_chdir (cwd);
		g_free (cwd);		
	}
	gtk_widget_destroy (dlg);

	g_free (name);
	g_free (dir);
}

static GList *
get_file_list_from_uri_list (gchar *uri_list)
{
	GList *list = NULL;
	gchar **uris = NULL;
	gint index;

	if (uri_list == NULL) {
		return NULL;
	}
	
	uris = g_uri_list_extract_uris (uri_list);
	
	for (index = 0; uris[index] != NULL; index++) {
		
		gchar *filename;

		if (g_path_is_absolute (uris[index]) == TRUE) {
			filename = g_strdup (uris[index]);
		}
		else {
			gchar *host = NULL;
			
			filename = g_filename_from_uri (uris[index], &host, NULL);
			
			/* Sorry, we can only accept local files. */
			if (host != NULL) {
				g_free (filename);
				g_free (host);
				filename = NULL;
			}
		}

		if (filename != NULL) {
			list = g_list_prepend (list, filename);
		}
	}
	g_strfreev (uris);
	return g_list_reverse (list);
}

static void  
html_theme_list_drag_data_received  (GtkWidget        *widget,
                                GdkDragContext   *context,
                                gint              x,
                                gint              y,
                                GtkSelectionData *data,
                                guint             info,
                                guint             time,
                                gpointer          extra_data)
{
	GtkWidget *parent;
	GtkWidget *theme_list;
	GtkListStore *store;
	GList *list;
	
	parent = glade_xml_get_widget (xml, "setup_dialog");
	theme_list = glade_xml_get_widget (xml, "gg_html_theme_list");
	store = GTK_LIST_STORE (gtk_tree_view_get_model (GTK_TREE_VIEW (theme_list)));

	gtk_drag_finish (context, TRUE, FALSE, time);

	for (list = get_file_list_from_uri_list ((gchar *)data->data); list != NULL; list = list-> next) {

		GtkWidget *prompt;
		gchar *base;
		gchar *mesg;
		gchar *detail;
		gint response;

		base = g_path_get_basename ((gchar *)list->data);
		mesg = g_strdup_printf (_("Install the theme from '%s'?"), base);
		detail = g_strdup_printf (_("Select install to add the theme from the file '%s'."), (gchar *)list->data); 
		
		prompt = hig_dialog_new (GTK_WINDOW (parent),
					 GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
					 GTK_MESSAGE_QUESTION,
					 GTK_BUTTONS_NONE,
					 mesg,
					 detail);

		gtk_dialog_add_button (GTK_DIALOG (prompt), "gtk-cancel", GTK_RESPONSE_CANCEL); 
		gtk_dialog_add_button (GTK_DIALOG (prompt), _("_Install"), GTK_RESPONSE_OK);

		response = gtk_dialog_run (GTK_DIALOG (prompt));
		gtk_widget_destroy (prompt);
		g_free (base);
		g_free (mesg);
		g_free (detail);

		if (response == GTK_RESPONSE_OK) {
			html_install_theme_file (list->data, store, GTK_WINDOW (parent));
		}
	}
}

static gboolean
theme_list_equal_func (GtkTreeModel * model,
                       gint column,
                       const gchar * key,
                       GtkTreeIter * iter,
                       gpointer search_data)
{
	gboolean results = TRUE;
	gchar *name;

	gtk_tree_model_get (model, iter, THEME_COLUMN_MARKUP, &name, -1);

	if (name != NULL) {
		gchar * casefold_key;
		gchar * casefold_name;
	
		casefold_key = g_utf8_casefold (key, -1);
		casefold_name = g_utf8_casefold (name, -1);

		if ((casefold_key != NULL) &&
		    (casefold_name != NULL) && 
		    (strstr (casefold_name, casefold_key) != NULL)) {
			results = FALSE;
		}
		g_free (casefold_key);
		g_free (casefold_name);
		g_free (name);
	}
	return results;
}

static void
setup_themes (void)
{		
	GtkListStore *store;
	GtkCellRenderer *renderer;
	GtkTreeViewColumn *column;
	GtkTreeSelection *selection;
	GtkTreeIter *select_iter = NULL;	
			
	GtkWidget *theme_list = glade_xml_get_widget (xml, "gg_html_theme_list");
	GtkWidget *button = glade_xml_get_widget (xml, "gg_install_new_html_theme");
	GtkWidget *del_button = glade_xml_get_widget (xml, "gg_delete_html_theme");				
	GtkWidget *preview_button = glade_xml_get_widget (xml, "button_preview");	
	
	gtk_tree_view_set_rules_hint (GTK_TREE_VIEW (theme_list), TRUE);

	selected_html_theme  = mdm_config_get_string (MDM_KEY_HTML_THEME);	
	selected_theme  = mdm_config_get_string (MDM_KEY_GRAPHICAL_THEME);
		
	/* create list store */
	store = gtk_list_store_new (THEME_NUM_COLUMNS,
				    G_TYPE_BOOLEAN /* selected theme */,				  
				    G_TYPE_STRING /* dir */,
				    G_TYPE_STRING /* file */,
				    GDK_TYPE_PIXBUF /* preview */,
				    G_TYPE_STRING /* markup */,
				    G_TYPE_STRING /* name */,
				    G_TYPE_STRING /* desc */,
					G_TYPE_INT /* theme type */
		);
		
	g_signal_connect (button, "clicked", G_CALLBACK (install_new_html_theme), store);
	g_signal_connect (del_button, "clicked", G_CALLBACK (delete_html_theme), store);
	g_signal_connect (preview_button, "clicked", G_CALLBACK (preview_mdm), NULL);

	/* Init controls */
	gtk_widget_set_sensitive (del_button, FALSE);		

	/* Add themes */	
	select_iter = read_themes (store);
	
	gtk_tree_view_set_model (GTK_TREE_VIEW (theme_list), GTK_TREE_MODEL (store));

	/* The radio toggle column */
	column = gtk_tree_view_column_new ();
	renderer = gtk_cell_renderer_toggle_new ();
	gtk_cell_renderer_toggle_set_radio (GTK_CELL_RENDERER_TOGGLE (renderer), TRUE);
	g_signal_connect (G_OBJECT (renderer), "toggled", G_CALLBACK (selected_html_toggled), store);
	gtk_tree_view_column_pack_start (column, renderer, FALSE);
	gtk_tree_view_column_set_attributes (column, renderer, "active", THEME_COLUMN_SELECTED, NULL);
	gtk_tree_view_append_column (GTK_TREE_VIEW (theme_list), column);
	gtk_tree_view_column_set_visible(column, TRUE);
	
	/* The preview column */
	column   = gtk_tree_view_column_new ();
	renderer = gtk_cell_renderer_pixbuf_new ();
	gtk_tree_view_column_pack_start (column, renderer, FALSE);
    gtk_tree_view_column_set_attributes (column, renderer, "pixbuf", THEME_COLUMN_SCREENSHOT, NULL);

	/* The markup column */
	renderer = gtk_cell_renderer_text_new ();
	gtk_tree_view_column_pack_start (column, renderer, FALSE);
	gtk_tree_view_column_set_attributes (column, renderer, "markup", THEME_COLUMN_MARKUP, NULL);
	gtk_tree_view_column_set_spacing (column, 6);
	gtk_tree_view_append_column (GTK_TREE_VIEW (theme_list), column);

	gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (store), THEME_COLUMN_MARKUP, GTK_SORT_ASCENDING);
	gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (store), THEME_COLUMN_TYPE, GTK_SORT_ASCENDING);

	gtk_tree_view_set_search_equal_func (GTK_TREE_VIEW (theme_list), theme_list_equal_func, NULL, NULL);

	/* Selection setup */
	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (theme_list));
	gtk_tree_selection_set_mode (selection, GTK_SELECTION_SINGLE);
	g_signal_connect (selection, "changed", G_CALLBACK (gg_html_selection_changed), NULL);

	gtk_drag_dest_set (theme_list, GTK_DEST_DEFAULT_ALL, target_table, n_targets, GDK_ACTION_COPY);
			   
	g_signal_connect (theme_list, "drag_data_received", G_CALLBACK (html_theme_list_drag_data_received), NULL);

	if (select_iter != NULL) {
		gtk_tree_selection_select_iter (selection, select_iter);
		g_free (select_iter);
	}
}

static gboolean
delete_event (GtkWidget *w)
{
	timeout_remove_all ();
	gtk_main_quit ();
	return FALSE;
}

static void
background_filechooser_response (GtkWidget *file_chooser, gpointer data)
{
	gchar *filename = NULL;
	gchar *key;
	gchar *value;
				     		
	filename = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (file_chooser));
	key      = g_object_get_data (G_OBJECT (file_chooser), "key");	
	value    = mdm_config_get_string (key);

	/*
	 * File_name should never be NULL, but something about this GUI causes
	 * this function to get called on startup and filename=NULL even
	 * though we set the filename in hookup_*_background.  Resetting the
	 * value to the default in this case seems to work around this.
	 */
	if (filename == NULL && !ve_string_empty (value))
		filename = g_strdup (value);

	if (filename != NULL &&
	   (strcmp (ve_sure_string (value), ve_sure_string (filename)) != 0)) {
		gchar *old_filename;
		old_filename = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (file_chooser));

		if (strcmp (old_filename, filename) == 0)
			gtk_file_chooser_set_filename (GTK_FILE_CHOOSER (file_chooser),
						       filename);
		g_free (old_filename);

		if (strcmp (ve_sure_string (value), ve_sure_string (filename)) != 0) {
			mdm_setup_config_set_string (key, (char *)ve_sure_string (filename));
			update_greeters ();
		}
	}
	g_free (filename);
}

static GdkPixbuf *
create_preview_pixbuf (gchar *uri) 
{
	GdkPixbuf *pixbuf = NULL;
	
	if ((uri != NULL) && (uri[0] != '\0')) {
    
		gchar *file = NULL;
		
		if (g_path_is_absolute (uri) == TRUE) {
			file = g_strdup (uri);
		}
		else {
			/* URIs are local, because gtk_file_chooser_get_local_only() is true. */
			file = g_filename_from_uri (uri, NULL, NULL);	
		}
		
		if (file != NULL) {
			gint width;
			gint height;
			
			gdk_pixbuf_get_file_info (file, &width, &height);

			if (width > 128 || height > 128) {
				pixbuf = gdk_pixbuf_new_from_file_at_size (file, 128, 128, NULL);
			}
			else {
				pixbuf = gdk_pixbuf_new_from_file (file, NULL);
			}
			g_free (file);
		}
	}				
	return pixbuf;
}

static void 
update_image_preview (GtkFileChooser *chooser) 
{
	GtkWidget *image;
	gchar *uri;

	image = gtk_file_chooser_get_preview_widget (GTK_FILE_CHOOSER (chooser));
	uri = gtk_file_chooser_get_preview_uri (chooser);
  
	if (uri != NULL) {
  
		GdkPixbuf *pixbuf = NULL;
    
		pixbuf = create_preview_pixbuf (uri);

		if (pixbuf != NULL) {
			gtk_image_set_from_pixbuf (GTK_IMAGE (image), pixbuf);
			g_object_unref (pixbuf);
		}
		else {
			gtk_image_set_from_stock (GTK_IMAGE (image),
			                          "gtk-dialog-question",
			                          GTK_ICON_SIZE_DIALOG);
		}
	}		
	gtk_file_chooser_set_preview_widget_active (chooser, TRUE);
}

static void
hookup_plain_background (void)
{	
	/* Initialize and hookup callbacks for plain background settings */
	GtkFileFilter *filter;
	GtkWidget *color_radiobutton;
	GtkWidget *color_colorbutton;
	GtkWidget *image_radiobutton;
	GtkWidget *image_filechooser;	
	GtkWidget *image_preview;
	gchar *background_filename;

	color_radiobutton = glade_xml_get_widget (xml, "local_background_color_checkbutton");
	color_colorbutton = glade_xml_get_widget (xml, "local_background_colorbutton");
	image_radiobutton = glade_xml_get_widget (xml, "local_background_image_checkbutton");
	image_filechooser = glade_xml_get_widget (xml, "local_background_image_chooserbutton");	

	setup_greeter_color ("local_background_colorbutton", 
	                     MDM_KEY_BACKGROUND_COLOR);
	
        filter = gtk_file_filter_new ();
        gtk_file_filter_set_name (filter, _("Images"));
        gtk_file_filter_add_pixbuf_formats (filter);
        gtk_file_chooser_add_filter (GTK_FILE_CHOOSER (image_filechooser), filter);

        filter = gtk_file_filter_new ();
        gtk_file_filter_set_name (filter, _("All Files"));
        gtk_file_filter_add_pattern(filter, "*");
        gtk_file_chooser_add_filter (GTK_FILE_CHOOSER (image_filechooser), filter);

	background_filename = mdm_config_get_string (MDM_KEY_BACKGROUND_IMAGE);

        if (ve_string_empty (background_filename)) {
                gtk_file_chooser_set_current_folder (GTK_FILE_CHOOSER (image_filechooser),
                        PIXMAPDIR);
	} else {
                gtk_file_chooser_set_filename (GTK_FILE_CHOOSER (image_filechooser),
			background_filename);
	}

	switch (mdm_config_get_int (MDM_KEY_BACKGROUND_TYPE)) {
	
		case BACKGROUND_IMAGE_AND_COLOR: {
			/* Image & Color background type */
			gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (image_radiobutton), TRUE);
			gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (color_radiobutton), TRUE);			
			gtk_widget_set_sensitive (image_filechooser, TRUE);
			gtk_widget_set_sensitive (color_colorbutton, TRUE);
			
			break;
		}
		case BACKGROUND_COLOR: {
			/* Color background type */
			gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (image_radiobutton), FALSE);
			gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (color_radiobutton), TRUE);			
			gtk_widget_set_sensitive (image_filechooser, FALSE);
			gtk_widget_set_sensitive (color_colorbutton, TRUE);

			break;
		}
		case BACKGROUND_IMAGE: {
			/* Image background type */
			gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (image_radiobutton), TRUE);
			gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (color_radiobutton), FALSE);			
			gtk_widget_set_sensitive (image_filechooser, TRUE);
			gtk_widget_set_sensitive (color_colorbutton, FALSE);
			
			break;
		}
		default: {
			/* No background type */
			gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (image_radiobutton), FALSE);
			gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (color_radiobutton), FALSE);
			gtk_widget_set_sensitive (color_colorbutton, FALSE);			
			gtk_widget_set_sensitive (image_filechooser, FALSE);
		}
	}

	gtk_file_chooser_set_use_preview_label (GTK_FILE_CHOOSER (image_filechooser),
					        FALSE);
	image_preview = gtk_image_new ();
	if (!ve_string_empty (background_filename)) {
		gtk_image_set_from_pixbuf (GTK_IMAGE (image_preview),
			create_preview_pixbuf (background_filename));
	}
	gtk_file_chooser_set_preview_widget (GTK_FILE_CHOOSER (image_filechooser),
	                                     image_preview);
	gtk_widget_set_size_request (image_preview, 128, -1);  
	gtk_widget_show (image_preview); 

	g_object_set_data (G_OBJECT (color_radiobutton), "key",
	                   MDM_KEY_BACKGROUND_TYPE);
	g_object_set_data (G_OBJECT (color_colorbutton), "key",
	                   MDM_KEY_BACKGROUND_COLOR);
	g_object_set_data (G_OBJECT (image_radiobutton), "key",
	                   MDM_KEY_BACKGROUND_TYPE);
	g_object_set_data (G_OBJECT (image_filechooser), "key",
	                   MDM_KEY_BACKGROUND_IMAGE);			   	

	g_signal_connect (G_OBJECT (color_radiobutton), "toggled",
	                  G_CALLBACK (local_background_type_toggled), NULL);
	g_signal_connect (G_OBJECT (color_radiobutton), "toggled",
	                  G_CALLBACK (toggle_toggled_sensitivity_positive), color_colorbutton);
	g_signal_connect (G_OBJECT (image_radiobutton), "toggled",
	                  G_CALLBACK (local_background_type_toggled), NULL);
	g_signal_connect (G_OBJECT (image_radiobutton), "toggled",
	                  G_CALLBACK (toggle_toggled_sensitivity_positive), image_filechooser);	
        g_signal_connect (G_OBJECT (image_filechooser), "selection-changed",
                          G_CALLBACK (background_filechooser_response), image_filechooser);
        g_signal_connect (G_OBJECT (image_filechooser), "update-preview",
			  G_CALLBACK (update_image_preview), NULL);
}

static void
setup_local_welcome_message (void)	
{
	/* Initialize and hookup callbacks for local welcome message settings */ 
	setup_greeter_toggle ("sg_defaultwelcome", MDM_KEY_DEFAULT_WELCOME);
 	setup_greeter_untranslate_entry ("welcome", MDM_KEY_WELCOME);
}

static void
setup_local_plain_settings (void)
{		
	/* Plain background settings */
	hookup_plain_background ();	
	
	/* Local welcome message settings */
	setup_local_welcome_message  ();
}

static void
setup_default_session (void)
{
	GtkWidget  *default_session_combobox;
	GtkWidget  *default_session_checkbox;
	GHashTable *sessnames        = NULL;
	GList      *org_sessions     = NULL;
	GList      *tmp;
	gint       i = 0;
	gint       active = 0;
	gchar      *org_val;

	_mdm_session_list_init (&sessnames, &org_sessions, NULL, NULL);

	default_session_combobox = glade_xml_get_widget (xml, "default_session_combobox");

	org_val = mdm_config_get_string (MDM_KEY_DEFAULT_SESSION);

	for (tmp = org_sessions; tmp != NULL; tmp = tmp->next, i++) {
		MdmSession *session;
		gchar *file;

		file = (gchar *) tmp->data;
		if (strcmp (ve_sure_string (org_val), ve_sure_string (file)) == 0)		
			active = i;

		session = g_hash_table_lookup (sessnames, file);

		if (!ve_string_empty (session->name)) {
			if ( strcmp(file, "default.desktop") == 0 ){
				gtk_combo_box_append_text (GTK_COMBO_BOX (default_session_combobox), _("Automatically detected"));
				sessions = g_list_prepend (sessions, "auto");
			}
			else {
				gtk_combo_box_append_text (GTK_COMBO_BOX (default_session_combobox), session->name);
				sessions = g_list_prepend (sessions, file);
			}
		}

	}

	sessions = g_list_reverse (sessions);

	/* some cleanup */
	g_list_free (org_sessions);
	g_hash_table_remove_all (sessnames);

	if (!ve_string_empty (org_val)) {
		gtk_combo_box_set_active (GTK_COMBO_BOX (default_session_combobox), active);
	}

	g_object_set_data_full (G_OBJECT (default_session_combobox), "key",
	                        g_strdup (MDM_KEY_DEFAULT_SESSION),
				(GDestroyNotify) g_free);

	g_signal_connect (default_session_combobox, "changed",
		          G_CALLBACK (combobox_changed), NULL);

	g_free (org_val);
}

static void
setup_monitors (void)
{
	GtkWidget  *primary_monitor_combobox = glade_xml_get_widget (xml, "primary_monitor_combobox");
	GtkWidget  *primary_monitor_label = glade_xml_get_widget (xml, "primary_monitor_label");

	gtk_widget_set_sensitive (primary_monitor_combobox, TRUE);

	GdkScreen *screen = gdk_screen_get_default ();

	int mdm_wm_num_monitors = gdk_screen_get_n_monitors (screen);

	printf("Number of monitors: %d\n", mdm_wm_num_monitors);

	if (mdm_wm_num_monitors > 1) {
		gint active = 0;
		int i;
		gchar * org_val = mdm_config_get_string (MDM_KEY_PRIMARY_MONITOR);
		printf("Monitor found in configuration: '%s'\n", org_val);
		for (i = 0; i < mdm_wm_num_monitors; i++) {
			GdkRectangle geometry;
			gdk_screen_get_monitor_geometry (screen, i, &geometry);
			gchar * plugname = gdk_screen_get_monitor_plug_name (screen, i);
			gchar * monitor_index = g_strdup_printf ("%d", i);
			printf("Found monitor #%s with plug name: '%s'\n", monitor_index, plugname);
			gchar * monitor_label;
			gchar * monitor_id;
			if (ve_string_empty (plugname)) {
				monitor_id = g_strdup (monitor_index);
				monitor_label = g_strdup_printf ("#%s - %dx%d", monitor_index, geometry.width, geometry.height);
			}
			else {
				monitor_id = g_strdup (plugname);
				monitor_label = g_strdup_printf ("#%s - %s - %dx%d", monitor_index, plugname, geometry.width, geometry.height);
			}

			gtk_combo_box_append_text (GTK_COMBO_BOX (primary_monitor_combobox), monitor_label);
			monitors = g_list_prepend (monitors, monitor_id);

			if (g_strcmp0(monitor_id, org_val) == 0) {
				active = i + 1;
			}

			g_free (plugname);
			g_free (monitor_index);
		}
		if (active > 0) {
			gtk_combo_box_set_active (GTK_COMBO_BOX (primary_monitor_combobox), active);
		}
		monitors = g_list_reverse (monitors);
		g_object_set_data_full (G_OBJECT (primary_monitor_combobox), "key", g_strdup (MDM_KEY_PRIMARY_MONITOR), (GDestroyNotify) g_free);
		g_signal_connect (primary_monitor_combobox, "changed", G_CALLBACK (combobox_changed), NULL);
		g_free (org_val);
	}
	else {
		gtk_widget_hide (primary_monitor_combobox);
		gtk_widget_hide (primary_monitor_label);
	}
}

static void
icon_selected (GtkWidget *iconview)
{
	GtkTreeModel *model;
	GList *list;
	int len;
	GtkTreeIter iter;
	int * id;

	model = gtk_icon_view_get_model (iconview);
	list = gtk_icon_view_get_selected_items (iconview);
	if (list == NULL) {
		return;
	}

	len = g_list_length (list);
	if (len >= 1) {
		gtk_tree_model_get_iter (model, &iter, list->data);
		gtk_tree_model_get (model, &iter, ICONS_ID, &id, -1);
		gtk_notebook_set_page(setup_notebook, id);
	}

	g_list_foreach (list, (GFunc)gtk_tree_path_free, NULL);
	g_list_free (list);	
}

static GtkWidget *
setup_gui (void)
{
	GtkWidget *window;

	xml = glade_xml_new (MDM_GLADE_DIR "/mdmsetup.glade", "main_window", NULL);

	window = glade_xml_get_widget (xml, "main_window");

	g_signal_connect (G_OBJECT (window), "delete_event", G_CALLBACK (delete_event), NULL);
	
	setup_notebook = glade_xml_get_widget (xml, "setup_notebook");

	GtkWidget *iconview = glade_xml_get_widget (xml, "iconview");
	GtkListStore *iconstore = gtk_list_store_new (ICONS_NUM, GTK_TYPE_INT, GTK_TYPE_STRING, GDK_TYPE_PIXBUF);
	
	GtkIconTheme *theme = gtk_icon_theme_get_default();
    GtkTreeIter iter;
	
	gtk_list_store_append (iconstore, &iter);
	gtk_list_store_set(iconstore, &iter,
		ICONS_ID, 0,
		ICONS_NAME, _("Theme"),
		ICONS_PIC, gtk_icon_theme_load_icon (theme, "preferences-desktop-theme", 36, GTK_ICON_LOOKUP_NO_SVG, NULL), 
		-1);

	gtk_list_store_append (iconstore, &iter);
	gtk_list_store_set(iconstore, &iter,
		ICONS_ID, 1,
		ICONS_NAME, _("Auto login"),
		ICONS_PIC, gtk_icon_theme_load_icon (theme, "gnome-logout", 36, GTK_ICON_LOOKUP_NO_SVG, NULL), 
		-1);

	gtk_list_store_append (iconstore, &iter);
	gtk_list_store_set(iconstore, &iter,
		ICONS_ID, 2,
		ICONS_NAME, _("Options"),
		ICONS_PIC, gtk_icon_theme_load_icon (theme, "preferences-desktop", 36, GTK_ICON_LOOKUP_NO_SVG, NULL), 
		-1);

	gtk_icon_view_set_text_column(iconview, ICONS_NAME);
	gtk_icon_view_set_pixbuf_column(iconview, ICONS_PIC);
    gtk_icon_view_set_model(iconview, iconstore);
    gtk_icon_view_select_path(iconview, gtk_tree_path_new_first());
    g_signal_connect (G_OBJECT (iconview), "selection_changed",	G_CALLBACK (icon_selected), NULL);

	gtk_widget_show_all (iconview);

	/* Markup glade labels */	
	glade_helper_tagify_label (xml, "themes_label", "b");
	glade_helper_tagify_label (xml, "local_style_label", "b");	
	glade_helper_tagify_label (xml, "local_background_label", "b");	
	glade_helper_tagify_label (xml, "local_menubar_label", "b");
	glade_helper_tagify_label (xml, "local_welcome_message_label", "b");
	glade_helper_tagify_label (xml, "label_welcome_note", "i");
	glade_helper_tagify_label (xml, "label_welcome_note", "small");		
	glade_helper_tagify_label (xml, "autologin", "b");
	glade_helper_tagify_label (xml, "timedlogin", "b");	

	gtk_widget_set_tooltip_text (glade_xml_get_widget (xml, "gg_install_new_html_theme"), _("Add a new theme"));
	gtk_widget_set_tooltip_text (glade_xml_get_widget (xml, "gg_delete_html_theme"), _("Remove the selected theme"));
	gtk_widget_set_tooltip_text (glade_xml_get_widget (xml, "button_preview"), _("Launch the login screen to preview the theme"));
	
	/* Setup preference tabs */
	setup_default_session();	
	setup_monitors();
	setup_local_plain_settings ();
	setup_themes();	
	
	GtkWidget *checkbox;
	GtkWidget *label;

	/* Setup user preselection */
	setup_notify_toggle ("select_last_login", MDM_KEY_SELECT_LAST_LOGIN);

	/* Setup Enable NumLock */
	GtkWidget *numlock = setup_notify_toggle ("enable_numlock", MDM_KEY_NUMLOCK);
	if (!g_file_test ("/usr/bin/numlockx", G_FILE_TEST_IS_EXECUTABLE)) {
		gtk_widget_set_sensitive (numlock, FALSE);
		gtk_widget_set_tooltip_text (numlock, _("Please install 'numlockx' to enable this feature"));
	}

	/* Setup Local administrator login setttings */
	setup_notify_toggle ("allowroot", MDM_KEY_ALLOW_ROOT);

	/* Setup Enable debug message to system log */
	setup_notify_toggle ("enable_debug", MDM_KEY_DEBUG);

	/* Setup session output limit */
	setup_notify_toggle ("limit_session_output", MDM_KEY_LIMIT_SESSION_OUTPUT);

	/* Setup session output filtering */
	setup_notify_toggle ("filter_session_output", MDM_KEY_FILTER_SESSION_OUTPUT);

	/* Setup 24h clock */
	setup_notify_toggle ("enable_24h_clock", MDM_KEY_USE_24_CLOCK);

	/* Bold the Enable automatic login label */
	checkbox = glade_xml_get_widget (xml, "autologin");
	label = gtk_bin_get_child (GTK_BIN (checkbox));
	g_object_set (G_OBJECT (label), "use_markup", TRUE, NULL);

	/* Bold the Enable timed login label */
	checkbox = glade_xml_get_widget (xml, "timedlogin");
	label = gtk_bin_get_child (GTK_BIN (checkbox));
	g_object_set (G_OBJECT (label), "use_markup", TRUE, NULL);

	/* Setup Enable automatic login */
 	setup_user_combobox ("autologin_combo",
			     MDM_KEY_AUTOMATIC_LOGIN);
	setup_notify_toggle ("autologin", MDM_KEY_AUTOMATIC_LOGIN_ENABLE);

	/* Setup Enable timed login */
 	setup_user_combobox ("timedlogin_combo",
			     MDM_KEY_TIMED_LOGIN);
	setup_intspin ("timedlogin_seconds", MDM_KEY_TIMED_LOGIN_DELAY);
	setup_notify_toggle ("timedlogin", MDM_KEY_TIMED_LOGIN_ENABLE);	

	return (window);
}

static gboolean
get_sensitivity (void)
{
	static Atom atom = 0;
	Display *disp = gdk_x11_get_default_xdisplay ();
	Window root = gdk_x11_get_default_root_xwindow ();
	unsigned char *datac;
	gulong *data;
	gulong nitems_return;
	gulong bytes_after_return;
	Atom type_returned;
	int format_returned;

	if (atom == 0)
		atom = XInternAtom (disp, "_MDM_SETUP_INSENSITIVE", False);

	if (XGetWindowProperty (disp,
				root,
				atom,
				0, 1,
				False,
				XA_CARDINAL,
				&type_returned, &format_returned,
				&nitems_return,
				&bytes_after_return,
				&datac) != Success)
		return TRUE;

	data = (gulong *)datac;

	if (format_returned != 32 ||
	    data[0] == 0) {
		XFree (data);
		return TRUE;
	} else {
		XFree (data);
		return FALSE;
	}
}

static void
update_sensitivity (void)
{
	gboolean sensitive = get_sensitivity ();
	GtkWidget *setup_dialog = glade_xml_get_widget (xml, "setup_dialog");
	gtk_widget_set_sensitive (setup_dialog, sensitive);
	if (sensitive)
		unsetup_window_cursor ();
	else
		setup_window_cursor (GDK_WATCH);
}

static GdkFilterReturn
root_window_filter (GdkXEvent *gdk_xevent,
		    GdkEvent *event,
		    gpointer data)
{
	XEvent *xevent = (XEvent *)gdk_xevent;

	if (xevent->type == PropertyNotify)
		update_sensitivity ();

	return GDK_FILTER_CONTINUE;
}

static void
setup_disable_handler (void)
{
	XWindowAttributes attribs = { 0, };
	Display *disp = gdk_x11_get_default_xdisplay ();
	Window root = gdk_x11_get_default_root_xwindow ();

	update_sensitivity ();

	/* set event mask for events on root window */
	XGetWindowAttributes (disp, root, &attribs);
	XSelectInput (disp, root,
		      attribs.your_event_mask |
		      PropertyChangeMask);

	gdk_window_add_filter (gdk_get_default_root_window (),
			       root_window_filter, NULL);
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

	return TRUE;
}      

int 
main (int argc, char *argv[])
{

	GtkWidget *window;

	mdm_config_never_cache (TRUE);

	if (g_getenv ("DOING_MDM_DEVELOPMENT") != NULL)
		DOING_MDM_DEVELOPMENT = TRUE;
	if (g_getenv ("RUNNING_UNDER_MDM") != NULL)
		RUNNING_UNDER_MDM = TRUE;

	bindtextdomain (GETTEXT_PACKAGE, "/usr/share/mdm/locale");
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);

	gtk_init (&argc, &argv);

	mdm_log_init ();
	mdm_log_set_debug (FALSE);

	/* Lets check if mdm daemon is running
	   if no there is no point in continuing
	*/
	mdm_running = mdmcomm_is_daemon_running (TRUE);
	if (mdm_running == FALSE)
		exit (EXIT_FAILURE);

	gtk_window_set_default_icon_name ("mdmsetup");	
	glade_init();
		
	mdmcomm_open_connection_to_daemon ();
	
	config_file = mdm_common_get_config_file ();
	if (config_file == NULL) {
		GtkWidget *dialog;

		/* Done using socket */
		mdmcomm_close_connection_to_daemon ();
		dialog = hig_dialog_new (NULL /* parent */,
					 GTK_DIALOG_MODAL /* flags */,
					 GTK_MESSAGE_ERROR,
					 GTK_BUTTONS_OK,
					 _("Could not access configuration file (defaults.conf)"),
					 _("Make sure that the file exists before launching login manager config utility."));
		
		gtk_dialog_run (GTK_DIALOG (dialog));
		gtk_widget_destroy (dialog);
		exit (EXIT_FAILURE);
	}
	custom_config_file = mdm_common_get_custom_config_file ();
	if (custom_config_file == NULL) {
		GtkWidget *dialog;

		/* Done using socket */
		mdmcomm_close_connection_to_daemon ();
		dialog = hig_dialog_new (NULL /* parent */,
					 GTK_DIALOG_MODAL /* flags */,
					 GTK_MESSAGE_ERROR,
					 GTK_BUTTONS_OK,
					 _("Could not access configuration file"),
					 _("Make sure that the file exists before launching login manager config utility."));

		gtk_dialog_run (GTK_DIALOG (dialog));
		gtk_widget_destroy (dialog);
		exit (EXIT_FAILURE);
	}	

	if (RUNNING_UNDER_MDM) {
		char *gtkrc;
		char *theme_name;

		/* Set busy cursor */
		setup_cursor (GDK_WATCH);

		/* parse the given gtk rc first */
		gtkrc = mdm_config_get_string (MDM_KEY_GTKRC);
		if ( ! ve_string_empty (gtkrc))
			gtk_rc_parse (gtkrc);

		theme_name = g_strdup (g_getenv ("MDM_GTK_THEME"));
		if (ve_string_empty (theme_name)) {
			g_free (theme_name);
			theme_name = mdm_config_get_string (MDM_KEY_GTK_THEME);
			mdm_set_theme (theme_name);
		} else {
			mdm_set_theme (theme_name);
		}

		/* evil, but oh well */
		g_type_class_ref (GTK_TYPE_WIDGET);	
	}

	/* Make sure the user is root. If not, they shouldn't be messing with 
	 * MDM's configuration.
	 */

	if ( ! DOING_MDM_DEVELOPMENT &&
	     geteuid() != 0) {
		GtkWidget *fatal_error;

		/* Done using socket */
		mdmcomm_close_connection_to_daemon ();

		fatal_error = hig_dialog_new (NULL /* parent */,
					      GTK_DIALOG_MODAL /* flags */,
					      GTK_MESSAGE_ERROR,
					      GTK_BUTTONS_OK,
					      _("You must be root to configure MDM."),
					      "");
		if (RUNNING_UNDER_MDM)
			setup_cursor (GDK_LEFT_PTR);
		gtk_dialog_run (GTK_DIALOG (fatal_error));
		exit (EXIT_FAILURE);
	}
	
	/* Done using socket */
	mdmcomm_close_connection_to_daemon ();	
	
	window = setup_gui ();
	
	gtk_widget_show (window);

	if (RUNNING_UNDER_MDM) {
		guint sid;

		/* also setup third button to work as first to work
		   in reverse situations transparently */
		sid = g_signal_lookup ("event",
				       GTK_TYPE_WIDGET);
		g_signal_add_emission_hook (sid,
					    0 /* detail */,
					    mdm_event,
					    NULL /* data */,
					    NULL /* destroy_notify */);

		setup_disable_handler ();

		setup_cursor (GDK_LEFT_PTR);
	}

	gtk_main ();

	return 0;
}

/* EOF */
