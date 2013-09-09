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

static char     *MdmSoundProgram = NULL;
static gchar    *MdmExclude      = NULL;
static gchar    *MdmInclude      = NULL;
static gint      MdmIconMaxHeight;
static gint      MdmIconMaxWidth;
static gboolean  MdmIncludeAll;
static gboolean  MdmUserChangesUnsaved;
static gint      last_selected_command;

/* set the DOING_MDM_DEVELOPMENT env variable if you want to
 * search for the glade file in the current dir and not the system
 * install dir, better then something you have to change
 * in the source and recompile
 */

static gboolean  DOING_MDM_DEVELOPMENT = FALSE;
static gboolean  RUNNING_UNDER_MDM     = FALSE;
static gboolean  mdm_running           = FALSE;
static GladeXML  *xml;
static GladeXML  *xml_add_users;
static GladeXML  *xml_add_xservers;
static GladeXML  *xml_xservers;
static GladeXML  *xml_commands;
static GtkWidget *setup_notebook;
static GList     *timeout_widgets = NULL;
static gchar     *last_theme_installed = NULL;
static gchar     *last_html_theme_installed = NULL;
static char      *selected_theme  = NULL;
static char      *selected_html_theme  = NULL;
static gchar     *config_file;
static gchar     *custom_config_file;
static GSList    *xservers;

/* This is used to store changes made to all
   possible fields of custom/normal commands */
static GHashTable  *MdmCommandChangesUnsaved = NULL;

/* Used to store all available sessions */
static GList *sessions = NULL;

enum {
	XSERVER_COLUMN_VT,
	XSERVER_COLUMN_SERVER,
	XSERVER_COLUMN_OPTIONS,
	XSERVER_NUM_COLUMNS
};

enum {
	THEME_COLUMN_SELECTED,	
	THEME_COLUMN_DIR,
	THEME_COLUMN_FILE,
	THEME_COLUMN_SCREENSHOT,
	THEME_COLUMN_MARKUP,
	THEME_COLUMN_NAME,
	THEME_COLUMN_DESCRIPTION,	
	THEME_NUM_COLUMNS
};

enum {
	USERLIST_NAME,
	USERLIST_NUM_COLUMNS
};

enum {
	LOCAL_TAB,
	GENERAL_TAB,
	SECURITY_TAB,
	USERS_TAB
};

enum {
	CLOCK_AUTO,
	CLOCK_YES,
	CLOCK_NO
};

enum {
	HALT_CMD,
	REBOOT_CMD,
	SUSPEND_CMD,
	CUSTOM_CMD
};

enum {
	LOCAL_PLAIN,	
	LOCAL_THEMED,
	LOCAL_HTML	
};

enum {
	XSERVER_LAUNCH_GREETER,
	XSERVER_LAUNCH_CHOOSER
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
	mdm_running = mdmcomm_check (FALSE);

	if ( ! mdm_running)
		return;

	ret = mdmcomm_call_mdm (MDM_SUP_GREETERPIDS,
				NULL /* auth_cookie */,
				"1.0.0.0",
				5);
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
	mdm_running = mdmcomm_check (FALSE);

	if (mdm_running) {
		char *ret;
		char *s = g_strdup_printf ("%s %s", MDM_SUP_UPDATE_CONFIG,
					   key);
		ret = mdmcomm_call_mdm (s,
					NULL /* auth_cookie */,
					"1.0.0.0",
					5);
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
radiogroup_timeout (GtkWidget *toggle)
{	
	const char *key = g_object_get_data (G_OBJECT (toggle), "key");
	GSList *radio_group = gtk_radio_button_get_group (GTK_RADIO_BUTTON (toggle));
		
	if (strcmp (ve_sure_string (key), MDM_KEY_RELAX_PERM) == 0) {
		GSList *tmp;
		gint val;
		gint selected = 0;
		gint i = 0;
		gint list_size;
		
		val = mdm_config_get_int ((gchar *)key);
		list_size = g_slist_length (radio_group) - 1;
		
		for (tmp = radio_group; tmp != NULL; tmp = tmp->next, i++) {
			GtkWidget *radio_button;
			radio_button = (GtkWidget *) tmp->data;
			if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (radio_button)))
				selected = list_size - i;
		}
		
		if (val != selected)
			mdm_setup_config_set_int (key, selected);
			
	}
	return FALSE;	
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

	if (strcmp (ve_sure_string (key), MDM_KEY_GLOBAL_FACE_DIR) == 0) {
		/* Once enabled write the curently selected item
		   in the filechooser widget, otherwise disable
		   the config entry, i.e. write an empty string */
		if (GTK_TOGGLE_BUTTON (toggle)->active == TRUE) {
			gchar *filename;
			GtkWidget *file_chooser;

			file_chooser = glade_xml_get_widget (xml, "global_face_dir_filechooser");
			filename  = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (file_chooser));
			if (strcmp (ve_sure_string (mdm_config_get_string ((char*)key)),
				    ve_sure_string (filename)) != 0)
				mdm_setup_config_set_string (key, ve_sure_string (filename));

			g_free (filename);
		}
		else
			mdm_setup_config_set_string (key, "");		    
	}	
	else if (strcmp (ve_sure_string (key), MDM_KEY_DEFAULT_FACE) == 0) {
		/* Once enabled write the curently selected item
		   in the filechooser widget, otherwise disable
		   the config entry, i.e. write an empty string */
		if (GTK_TOGGLE_BUTTON (toggle)->active == TRUE) {
			gchar *filename;
			GtkWidget *file_chooser;
			
			file_chooser = glade_xml_get_widget (xml, "default_face_filechooser");

			filename  = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (file_chooser));
			
			if (strcmp (ve_sure_string (mdm_config_get_string ((char*)key)), 
				    ve_sure_string (filename)) != 0)				
				mdm_setup_config_set_string (key, ve_sure_string (filename));
			
			g_free (filename);
		}
		else
			mdm_setup_config_set_string (key, "");		    
	}
	else if (strcmp (ve_sure_string (key), MDM_KEY_GTKRC) == 0) {
		/* Once enabled write the curently selected item
		   in the filechooser widget, otherwise disable
		   the config entry, i.e. write an empty string */
		if (GTK_TOGGLE_BUTTON (toggle)->active == TRUE) {
			gchar *filename;
			GtkWidget *file_chooser;
			
			file_chooser = glade_xml_get_widget (xml, "gtkrc_chooserbutton");

			filename  = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (file_chooser));
			
			if (strcmp (ve_sure_string (mdm_config_get_string ((char*)key)), 
				    ve_sure_string (filename)) != 0)				
				mdm_setup_config_set_string (key, ve_sure_string (filename));
			
			g_free (filename);
		}
		else
			mdm_setup_config_set_string (key, "");		    
	}
	else if (strcmp (ve_sure_string (key), MDM_KEY_DEFAULT_SESSION) == 0) {
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

static gboolean
command_toggle_timeout (GtkWidget *toggle)
{
	const char *key = g_object_get_data (G_OBJECT (toggle), "key");
	
	if (strcmp (ve_sure_string (key), MDM_KEY_CUSTOM_CMD_IS_PERSISTENT_TEMPLATE) == 0 ||
	    strcmp (ve_sure_string (key), MDM_KEY_CUSTOM_CMD_NO_RESTART_TEMPLATE) == 0) {
		/* This only applies to custom commands
		   First find which command has been ticked on/off then put the new value 
		   together with the corresponding key into the command changed hash. 
		   Enable apply command changes button. If command is equal to the existing
		   config value remove it from the hash and disable the apply command changes 
		   button (if applicable) */
		
		gchar *key_string;
		gboolean old_val = FALSE;
		gboolean val;
		GtkWidget *apply_cmd_changes;
		GtkWidget *command_combobox;
		gint selected, i;
		
		val = GTK_TOGGLE_BUTTON (toggle)->active;
		apply_cmd_changes = glade_xml_get_widget (xml_commands, "command_apply_button");
		command_combobox = glade_xml_get_widget (xml_commands, "cmd_type_combobox");
		
		selected = gtk_combo_box_get_active (GTK_COMBO_BOX (command_combobox));
		
		i = selected - CUSTOM_CMD;
		key_string = g_strdup_printf("%s%d=", ve_sure_string (key), i); 
		old_val = mdm_config_get_bool (key_string);
		
		if (val != old_val) {	
			gboolean *p_val = g_new0 (gboolean, 1);
			*p_val = val;
			g_hash_table_insert (MdmCommandChangesUnsaved, g_strdup (key_string), p_val);
		}
		else if (g_hash_table_lookup (MdmCommandChangesUnsaved, key_string) != NULL) {
			g_hash_table_remove (MdmCommandChangesUnsaved, key_string);
		}
		
		g_free (key_string);
		
		if (g_hash_table_size (MdmCommandChangesUnsaved) == 0)
			gtk_widget_set_sensitive (apply_cmd_changes, FALSE);
		else 
			gtk_widget_set_sensitive (apply_cmd_changes, TRUE);
	}		
	
	return FALSE;
}

/* Forward declarations */
static void
setup_user_combobox_list (const char *name, const char *key);
static char *
strings_list_add (char *strings_list, const char *string, const char *sep);
static char *
strings_list_remove (char *strings_list, const char *string, const char *sep);

static gboolean
intspin_timeout (GtkWidget *spin)
{
	const char *key = g_object_get_data (G_OBJECT (spin), "key");
	int new_val = gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (spin));
	int val;
	gboolean greeters_need_update = FALSE;

	val = mdm_config_get_int ((gchar *)key);

	if (strcmp (ve_sure_string (key), MDM_KEY_MINIMAL_UID) == 0){
		/* We have changed MinimalUID, so we need to go through
		   the list of existing users in the Include list and remove
		   the entries that do not match the criteria anymore. If there 
		   are any user is informed about the changes. Auto login and 
		   timed login comboboxes are adjusted and greeters restarted */
		char **list;
		char *removed = NULL;	
		int i;
		gchar *autologon_user;
		gchar *timedlogon_user;
		
		
		list = g_strsplit (MdmInclude, ",", 0);
		for (i=0; list != NULL && list[i] != NULL; i++) {
			if (mdm_user_uid (list[i]) >= new_val) 
				continue;					
			
			MdmInclude = strings_list_remove (MdmInclude, list[i], ",");
			removed = strings_list_add(removed, list[i], ",");
			
		}
		g_strfreev (list);

		//Now if there were items to remove then
		if (removed != NULL) {
			gboolean  valid;
			gchar *text;
			GtkWidget *include_treeview;
			GtkTreeModel *include_model;			
			GtkWidget *dlg;	
			GtkTreeIter iter;			
			GtkWidget *setup_dialog;

			setup_dialog = glade_xml_get_widget(xml, "setup_dialog");

			//Inform user about the change and its implications
			dlg = hig_dialog_new (GTK_WINDOW (setup_dialog),
					      GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
					      GTK_MESSAGE_WARNING,
					      GTK_BUTTONS_OK,
					      _("Users include list modification"),
					      _("Some of the users in the Include list "
						"(Users tab) now have uid lower than "
						"MinimalUID and will be removed."));
			gtk_dialog_run (GTK_DIALOG (dlg));
			gtk_widget_destroy (dlg);						

			include_treeview  = glade_xml_get_widget (xml, "fb_include_treeview");

			include_model = gtk_tree_view_get_model (GTK_TREE_VIEW (include_treeview));

			valid = gtk_tree_model_get_iter_first (include_model, &iter);
			while (valid) {
				gtk_tree_model_get (include_model, &iter, USERLIST_NAME,
						    &text, -1);				
				
				if (strstr (removed, text) != NULL) {					
					valid = gtk_list_store_remove (GTK_LIST_STORE (include_model), &iter);	
				}
				else {				
					valid = gtk_tree_model_iter_next (include_model, &iter);
				}
			}
			g_free (text);
			/* Now we need to save updated list, toggle the 
			   automatic and timed loggon comboboxes and update greeters */
			mdm_setup_config_set_string (MDM_KEY_INCLUDE, MdmInclude);				
			
			greeters_need_update = TRUE;

		}
		
		g_free (removed);

		/* We also need to check if user (if any) in the
		   autologon/timed logon still match the criteria */
		autologon_user = mdm_config_get_string (MDM_KEY_AUTOMATIC_LOGIN);
		timedlogon_user = mdm_config_get_string (MDM_KEY_TIMED_LOGIN);

		if(!ve_string_empty (autologon_user)) {
			if (mdm_is_user_valid (autologon_user) && mdm_user_uid (autologon_user) < new_val) {
				mdm_setup_config_set_string (MDM_KEY_AUTOMATIC_LOGIN, "");			
				greeters_need_update = TRUE;
			}
		}

		if(!ve_string_empty (timedlogon_user)) {
			if (mdm_is_user_valid (timedlogon_user) && mdm_user_uid (timedlogon_user) < new_val) {
				mdm_setup_config_set_string (MDM_KEY_TIMED_LOGIN, "");						
				greeters_need_update = TRUE;
			}
		}

		g_free (autologon_user);
		g_free (timedlogon_user);

	}

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

static gint
display_sort_func (gpointer d1, gpointer d2)
{
   return (strcmp (ve_sure_string ((gchar *)d1), ve_sure_string ((gchar *)d2)));
}

static GSList *displays          = NULL;
static GSList *displays_inactive = NULL;
static GHashTable *dispval_hash  = NULL;

static void
mdm_load_displays (GKeyFile *cfg,
		   char    **keys)
{
	GSList *li2;
	int i;

	if (keys == NULL)
		return;

	for (i = 0; keys[i] != NULL; i++) {
		const gchar *key = keys[i];

		if (isdigit (*key)) {
			gchar *fullkey;
			gchar *dispval;
			int keynum = atoi (key);
			gboolean skip_entry = FALSE;

			fullkey = g_strdup_printf ("%s/%s", MDM_KEY_SECTION_SERVERS, key);
			mdm_common_config_get_string (cfg, fullkey, &dispval, NULL);
			g_free (fullkey);

			/* Do not add if already in the list */
			for (li2 = displays; li2 != NULL; li2 = li2->next) {
				gchar *disp = li2->data;
				if (atoi (disp) == keynum) {
					skip_entry = TRUE;
					break;
				}
			}

			/* Do not add if this display was marked as inactive already */
			for (li2 = displays_inactive; li2 != NULL; li2 = li2->next) {
				gchar *disp = li2->data;
				if (atoi (disp) == keynum) {
					skip_entry = TRUE;
					break;
				}
			}

			if (skip_entry == TRUE) {
				g_free (dispval);
				continue;
			}

			if (g_ascii_strcasecmp (ve_sure_string (dispval), "inactive") == 0) {
				displays_inactive = g_slist_append (displays_inactive, g_strdup (key));
			} else {
				if (dispval_hash == NULL)
					dispval_hash = g_hash_table_new (g_str_hash, g_str_equal);            

				displays = g_slist_insert_sorted (displays, g_strdup (key), (GCompareFunc) display_sort_func);
				g_hash_table_insert (dispval_hash, g_strdup (key), g_strdup (dispval));
			}

			g_free (dispval);
		}
	}
}

static char *
ve_rest (const char *s)
{
	const char *p;
	gboolean single_quot = FALSE;
	gboolean double_quot = FALSE;
	gboolean escape = FALSE;

	if (s == NULL)
		return NULL;

	for (p = s; *p != '\0'; p++) {
		if (single_quot) {
			if (*p == '\'') {
				single_quot = FALSE;
			}
		} else if (escape) {
			escape = FALSE;
		} else if (double_quot) {
			if (*p == '"') {
				double_quot = FALSE;
			} else if (*p == '\\') {
				escape = TRUE;
			}
		} else if (*p == '\'') {
			single_quot = TRUE;
		} else if (*p == '"') {
			double_quot = TRUE;
		} else if (*p == '\\') {
			escape = TRUE;
		} else if (*p == ' ' || *p == '\t') {
			while (*p == ' ' || *p == '\t')
				p++;
			return g_strdup (p);
		}
	}

	return NULL;
}

static void
xservers_get_displays (GtkListStore *store)
{
	/* Find server definitions */
	GKeyFile *custom_cfg;
	GKeyFile *cfg;
	char    **keys;
	GSList *li;
	gchar *server, *options;

	custom_cfg = mdm_common_config_load (custom_config_file, NULL);
        cfg = mdm_common_config_load (config_file, NULL);

	/* Fill list with all the active displays */
	if (custom_cfg != NULL) {
		keys = g_key_file_get_keys (custom_cfg, MDM_KEY_SECTION_SERVERS, NULL, NULL);
		mdm_load_displays (custom_cfg, keys);
		g_strfreev (keys);
	}

	keys = g_key_file_get_keys (cfg, MDM_KEY_SECTION_SERVERS, NULL, NULL);
	mdm_load_displays (cfg, keys);
	g_strfreev (keys);

	for (li = displays; li != NULL; li = li->next) {
		GtkTreeIter iter;
		gchar *key = li->data;
		int vt = atoi (key);
		server = ve_first_word (g_hash_table_lookup (dispval_hash, key));
		options = ve_rest (g_hash_table_lookup (dispval_hash, key));

		gtk_list_store_append (store, &iter);
		gtk_list_store_set (store, &iter,
				    XSERVER_COLUMN_VT, vt,
				    XSERVER_COLUMN_SERVER, server,
				    XSERVER_COLUMN_OPTIONS, options,
				    -1);
		g_free (server);
	}
	for (li = displays; li != NULL; li = li->next) {
		gchar *disp = li->data;
		g_free (disp);
	}
	g_slist_free (displays);
        displays = NULL;
	for (li = displays_inactive; li != NULL; li = li->next) {
		gchar *disp = li->data;
		g_free (disp);
	}
	g_slist_free (displays_inactive);
        displays_inactive = NULL;
        if (dispval_hash) {
           g_hash_table_destroy (dispval_hash);
           dispval_hash = NULL;
        }
}

static void
xserver_update_delete_sensitivity ()
{
	GtkWidget *modify_combobox, *delete_button;
	GtkListStore *store;
	GtkTreeIter iter;
	MdmXserver *xserver;
	gchar *text;
	gchar *selected;
	gboolean valid;
	gint i;

	modify_combobox = glade_xml_get_widget (xml_xservers, "xserver_mod_combobox");
	delete_button   = glade_xml_get_widget (xml_xservers, "xserver_deletebutton");

	/* Get list of servers that are set to start */
	store = gtk_list_store_new (XSERVER_NUM_COLUMNS,
	                            G_TYPE_INT    /* virtual terminal */,
	                            G_TYPE_STRING /* server type */,
	                            G_TYPE_STRING /* options */);

	/* Get list of servers and determine which one was selected */
	xservers_get_displays (store);

	i = gtk_combo_box_get_active (GTK_COMBO_BOX (modify_combobox));
	if (i < 0) {
		gtk_widget_set_sensitive(delete_button, FALSE);
	} else {
		/* Get the xserver selected */
		xserver = g_slist_nth_data (xservers, i);
	
		/* Sensitivity of delete_button */
		if (g_slist_length (xservers) <= 1) {
			/* Can't delete the last server */
			gtk_widget_set_sensitive (delete_button, FALSE);
		} else {
			gtk_widget_set_sensitive (delete_button, TRUE);
			valid = gtk_tree_model_get_iter_first (GTK_TREE_MODEL (store),
			                                       &iter);
			selected = gtk_combo_box_get_active_text (
			              GTK_COMBO_BOX (modify_combobox));

			/* Can't delete servers currently in use */
			while (valid) {
				gtk_tree_model_get (GTK_TREE_MODEL (store), &iter,
				                    XSERVER_COLUMN_SERVER, &text, -1);
				if (strcmp (ve_sure_string (text), ve_sure_string (selected)) == 0) {
					gtk_widget_set_sensitive(delete_button, FALSE);
					break;
				}
				valid = gtk_tree_model_iter_next (GTK_TREE_MODEL (store),
				                                  &iter);
			}
		}
	}
}

static
void init_servers_combobox (int index)
{
	GtkWidget *mod_combobox;
	GtkWidget *name_entry;
	GtkWidget *command_entry;
	GtkWidget *style_combobox;
	GtkWidget *handled_checkbutton;
	GtkWidget *flexible_checkbutton;
	GtkWidget *priority_spinbutton;
	GtkListStore *store;
	MdmXserver *xserver;

	mod_combobox = glade_xml_get_widget (xml_xservers, "xserver_mod_combobox");
	name_entry = glade_xml_get_widget (xml_xservers, "xserver_name_entry");
	command_entry = glade_xml_get_widget (xml_xservers, "xserver_command_entry");
	style_combobox = glade_xml_get_widget (xml_xservers, "xserver_style_combobox");
	handled_checkbutton = glade_xml_get_widget (xml_xservers, "xserver_handled_checkbutton");
	flexible_checkbutton = glade_xml_get_widget (xml_xservers, "xserver_flexible_checkbutton");
	priority_spinbutton = glade_xml_get_widget(xml_xservers, "xserv_priority_spinbutton");

	/* Get list of servers that are set to start */
	store = gtk_list_store_new (XSERVER_NUM_COLUMNS,
	                            G_TYPE_INT    /* virtual terminal */,
	                            G_TYPE_STRING /* server type */,
	                            G_TYPE_STRING /* options */);
	xservers_get_displays (store);

	xserver = g_slist_nth_data (xservers, index);

	gtk_combo_box_set_active (GTK_COMBO_BOX (mod_combobox), index);
	gtk_entry_set_text (GTK_ENTRY (name_entry), xserver->name);
	gtk_entry_set_text (GTK_ENTRY (command_entry), xserver->command);

	if (!xserver->chooser) {
		gtk_combo_box_set_active (GTK_COMBO_BOX (style_combobox), XSERVER_LAUNCH_GREETER);
	}
	else {
		gtk_combo_box_set_active (GTK_COMBO_BOX (style_combobox), XSERVER_LAUNCH_CHOOSER);
	}
	
	gtk_spin_button_set_value (GTK_SPIN_BUTTON (priority_spinbutton), 
				   xserver->priority);
	gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (handled_checkbutton),
	                              xserver->handled);
	gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (flexible_checkbutton),
	                              xserver->flexible);

	xserver_update_delete_sensitivity ();
}

/*
 * We probably should check the server definition in the defaults.conf file
 * and just erase the section if the values are the same, like we do for the
 * displays section and the normal configuration sections.
 */
static void
update_xserver (gchar *section, MdmXserver *svr)
{
	GKeyFile *custom_cfg;
	gchar *real_section;
	gchar *key;

	custom_cfg = mdm_common_config_load (custom_config_file, NULL);
	real_section = g_strdup_printf ("%s%s", MDM_KEY_SERVER_PREFIX, section);

	key = g_strconcat (real_section, "/" MDM_KEY_SERVER_NAME, NULL);
	mdm_common_config_set_string (custom_cfg, key, svr->name);
	g_free (key);

	key = g_strconcat (real_section, "/" MDM_KEY_SERVER_COMMAND, NULL);
	mdm_common_config_set_string (custom_cfg, key, svr->command);
	g_free (key);

	key = g_strconcat (real_section, "/", MDM_KEY_SERVER_CHOOSER, NULL);
	mdm_common_config_set_boolean (custom_cfg, key, svr->chooser);
	g_free (key);

	key = g_strconcat (real_section, "/" MDM_KEY_SERVER_HANDLED, NULL);
	mdm_common_config_set_boolean (custom_cfg, key, svr->handled);
	g_free (key);

	key = g_strconcat (real_section, "/" MDM_KEY_SERVER_FLEXIBLE, NULL);
	mdm_common_config_set_boolean (custom_cfg, key, svr->flexible);
	g_free (key);

	key = g_strconcat (real_section, "/" MDM_KEY_SERVER_PRIORITY, NULL);
	mdm_common_config_set_int (custom_cfg, key, svr->priority);
	g_free (key);

        g_free (real_section);
	mdm_common_config_save (custom_cfg, custom_config_file, NULL);

	g_key_file_free (custom_cfg);

	update_key ("xservers/PARAMETERS");
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
						str = g_strdup_printf (_("The \"%s\" user UID is lower than allowed MinimalUID."), new_val);				
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
	/* Style combobox */
	else if (strcmp (ve_sure_string (key), MDM_KEY_SERVER_CHOOSER) == 0) {
		GtkWidget *mod_combobox;
		GtkWidget *style_combobox;
		GSList *li;
		gchar *section;
		gboolean val_old, val_new;

		mod_combobox    = glade_xml_get_widget (xml_xservers, "xserver_mod_combobox");
		style_combobox  = glade_xml_get_widget (xml_xservers, "xserver_style_combobox");

		/* Get xserver section to update */
		section = gtk_combo_box_get_active_text (GTK_COMBO_BOX (mod_combobox));

		for (li = xservers; li != NULL; li = li->next) {
			MdmXserver *svr = li->data;
			if (strcmp (ve_sure_string (svr->id), ve_sure_string (section)) == 0) {

				val_old = svr->chooser;
				val_new = (gtk_combo_box_get_active (GTK_COMBO_BOX (style_combobox)) != 0);

				/* Update this servers configuration */
				if (! bool_equal (val_old, val_new)) {
					svr->chooser = val_new;
					update_xserver (section, svr);
				}
				break;
			}
		}
		g_free (section);
	}
	/* Use 24 clock combobox */
	else if (strcmp (ve_sure_string (key), MDM_KEY_USE_24_CLOCK) == 0) {		
		gchar *val;		
		gchar *new_val;
		
		new_val = gtk_combo_box_get_active_text (GTK_COMBO_BOX (combo_box));
		val     = mdm_config_get_string ((gchar *)key);

		if (new_val) {
                    if (strcmp (_(new_val), _("auto"))) {
                       if (strcasecmp (ve_sure_string (val), "auto") != 0)
                          mdm_setup_config_set_string (key, "auto");
                    }
                    else if (strcmp (_(new_val), _("yes"))) {
                       if (strcasecmp (ve_sure_string (val), "true") != 0 &&
                           strcasecmp (ve_sure_string (val), "yes") != 0)
                          mdm_setup_config_set_string (key, "true");
                    }
                    else {
                       if (strcasecmp (ve_sure_string (val), "false") != 0 &&
                           strcasecmp (ve_sure_string (val), "no") != 0) 
                           mdm_setup_config_set_string (key, "false");
		    }
		}
		g_free (new_val);
		g_free (val);
	}
	/* Commands combobox */
	else if (strcmp (ve_sure_string (key), "command_chooser_combobox") == 0) {
		/* We need to set the data according to selected command */
		GtkWidget *hrs_cmd_entry = NULL;
		GtkWidget *cust_cmd_entry = NULL;
		GtkWidget *cust_cmd_label_entry = NULL;
		GtkWidget *cust_cmd_lrlabel_entry = NULL;
		GtkWidget *cust_cmd_text_entry = NULL;
		GtkWidget *cust_cmd_tooltip_entry = NULL;
		GtkWidget *cust_cmd_persistent_checkbox = NULL;
		GtkWidget *cust_cmd_norestart_checkbox = NULL;
		GtkWidget *status_label;
		gchar *val = NULL;		
		gboolean bool_val = FALSE;		
		gboolean enabled_command = FALSE;
		
		hrs_cmd_entry = glade_xml_get_widget (xml_commands, "hrs_cmd_path_entry");
		cust_cmd_entry  = glade_xml_get_widget (xml_commands, "custom_cmd_path_entry");
		status_label = glade_xml_get_widget (xml_commands, "command_status_label");

		if (selected == HALT_CMD) {
			val = mdm_config_get_string (MDM_KEY_HALT);		
			gtk_entry_set_text (GTK_ENTRY (hrs_cmd_entry), ve_sure_string (val));
			enabled_command = !ve_string_empty (val);
		}
		else if (selected == REBOOT_CMD) {
			val = mdm_config_get_string (MDM_KEY_REBOOT);			
			gtk_entry_set_text (GTK_ENTRY (hrs_cmd_entry), ve_sure_string (val));
			enabled_command = !ve_string_empty (val);
		}
		else if (selected == SUSPEND_CMD) {
			val = mdm_config_get_string (MDM_KEY_SUSPEND);			
			gtk_entry_set_text (GTK_ENTRY (hrs_cmd_entry), ve_sure_string (val));
			enabled_command = !ve_string_empty (val);
		}
		else {
			gchar *key_string = NULL;

			gint i = selected - CUSTOM_CMD;
			/* Here we are going to deal with custom commands */
			key_string = g_strdup_printf("%s%d=", MDM_KEY_CUSTOM_CMD_TEMPLATE, i);
			val = mdm_config_get_string (key_string);
			gtk_entry_set_text (GTK_ENTRY (cust_cmd_entry), ve_sure_string (val));
			enabled_command = !ve_string_empty (val);
			g_free (key_string);
			g_free (val);
			
			key_string = g_strdup_printf("%s%d=", MDM_KEY_CUSTOM_CMD_LABEL_TEMPLATE, i);
			cust_cmd_label_entry  = glade_xml_get_widget (xml_commands, "custom_cmd_label_entry");
			val = mdm_config_get_string (key_string);
			gtk_entry_set_text (GTK_ENTRY (cust_cmd_label_entry), ve_sure_string (val));
			g_free (key_string);
			g_free (val);

			key_string = g_strdup_printf("%s%d=", MDM_KEY_CUSTOM_CMD_LR_LABEL_TEMPLATE, i);
			cust_cmd_lrlabel_entry  = glade_xml_get_widget (xml_commands, "custom_cmd_lrlabel_entry");
			val = mdm_config_get_string (key_string);
			gtk_entry_set_text (GTK_ENTRY (cust_cmd_lrlabel_entry), ve_sure_string (val));
			g_free (key_string);
			g_free (val);

			key_string = g_strdup_printf("%s%d=", MDM_KEY_CUSTOM_CMD_TEXT_TEMPLATE, i);
			cust_cmd_text_entry  = glade_xml_get_widget (xml_commands, "custom_cmd_text_entry");
			val = mdm_config_get_string (key_string);
			gtk_entry_set_text (GTK_ENTRY (cust_cmd_text_entry), ve_sure_string (val));
			g_free (key_string);
			g_free (val);
			
			key_string = g_strdup_printf("%s%d=", MDM_KEY_CUSTOM_CMD_TOOLTIP_TEMPLATE, i);
			cust_cmd_tooltip_entry  = glade_xml_get_widget (xml_commands, "custom_cmd_tooltip_entry");
			val = mdm_config_get_string (key_string);
			gtk_entry_set_text (GTK_ENTRY (cust_cmd_tooltip_entry), ve_sure_string (val));
			g_free (key_string);
			
			key_string = g_strdup_printf("%s%d=", MDM_KEY_CUSTOM_CMD_NO_RESTART_TEMPLATE, i);
			cust_cmd_norestart_checkbox  = glade_xml_get_widget (xml_commands, "custom_cmd_norestart_checkbutton");
			bool_val = mdm_config_get_bool (key_string);
			gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (cust_cmd_norestart_checkbox), bool_val);
			g_free (key_string);
			
			key_string = g_strdup_printf("%s%d=", MDM_KEY_CUSTOM_CMD_IS_PERSISTENT_TEMPLATE, i);
			cust_cmd_persistent_checkbox  = glade_xml_get_widget (xml_commands, "custom_cmd_persistent_checkbutton");
			bool_val = mdm_config_get_bool (key_string);
			gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (cust_cmd_persistent_checkbox), bool_val);
			g_free (key_string);

		}
		g_free (val);

		if (enabled_command)
			gtk_label_set_text (GTK_LABEL (status_label), _("(Enabled)"));			
		else 
			gtk_label_set_text (GTK_LABEL (status_label), _("(Disabled)"));	
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
	return FALSE;
}

static void
toggle_toggled (GtkWidget *toggle)
{
	run_timeout (toggle, 200, toggle_timeout);
}

static void
command_toggle_toggled (GtkWidget *toggle)
{
	run_timeout (toggle, 200, command_toggle_timeout);
}

static void
radiogroup_toggled (GtkWidget *toggle)
{
	run_timeout (toggle, 200, radiogroup_timeout);
}

static void
list_selection_toggled (GtkWidget *toggle, gpointer data)
{
	GtkWidget *widget = data;
	GtkWidget *include_treeview;
	GtkTreeSelection *selection;
	GtkTreeModel *include_model;
	GtkTreeIter iter;
	gboolean val;

	val = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (toggle));

	include_treeview  = glade_xml_get_widget (xml, "fb_include_treeview");
	include_model = gtk_tree_view_get_model (GTK_TREE_VIEW (include_treeview));
	
	selection = gtk_tree_view_get_selection (
	            GTK_TREE_VIEW (include_treeview));

	if ((val == FALSE) && (gtk_tree_selection_get_selected (selection, &(include_model), &iter))) {
		gtk_widget_set_sensitive (widget, TRUE);
	} else {
		gtk_widget_set_sensitive (widget, FALSE);
	}

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
	else if (strcmp (ve_sure_string (key), MDM_KEY_SERVER_PREFIX) == 0 ) {
		init_servers_combobox (gtk_combo_box_get_active (GTK_COMBO_BOX (combobox)));
	}
	else if (strcmp (ve_sure_string (key), "command_chooser_combobox") == 0) {
		gint selected;
		GtkWidget *hrs_cmd_vbox;
		GtkWidget *custom_cmd_vbox;
		
		selected = gtk_combo_box_get_active (GTK_COMBO_BOX (combobox));			

		/* First of all we need to check if we had made any changes
		   to any of the command fields. If so user gets reminded and
		   given an option to save, or discard */
		if (g_hash_table_size (MdmCommandChangesUnsaved) != 0) {
			GtkWidget *prompt;
			GtkWidget *setup_dialog;
			GtkWidget *apply_command;
			gint response;
			
			setup_dialog = glade_xml_get_widget (xml, "setup_dialog");
			
			prompt = hig_dialog_new (GTK_WINDOW (setup_dialog),
						 GTK_DIALOG_MODAL |
						 GTK_DIALOG_DESTROY_WITH_PARENT,
						 GTK_MESSAGE_WARNING,
						 GTK_BUTTONS_NONE,
						 _("Apply changes to the modified command?"),
						 _("If you don't apply, the changes "
						   "will be discarded."));
			
			gtk_dialog_add_button (GTK_DIALOG (prompt), "gtk-cancel", GTK_RESPONSE_CANCEL); 
			gtk_dialog_add_button (GTK_DIALOG (prompt), "gtk-apply", GTK_RESPONSE_APPLY);
			
			response = gtk_dialog_run (GTK_DIALOG (prompt));
			gtk_widget_destroy (prompt);	       
			
			apply_command = glade_xml_get_widget (xml_commands, "command_apply_button");
			if (response == GTK_RESPONSE_APPLY)				
				g_signal_emit_by_name (G_OBJECT (apply_command), "clicked");
			
			else {
				g_hash_table_remove_all (MdmCommandChangesUnsaved);				
				gtk_widget_set_sensitive (apply_command, FALSE);	
			}
		}
		
		last_selected_command = selected;

		hrs_cmd_vbox = glade_xml_get_widget (xml_commands, "hrs_command_vbox");
		custom_cmd_vbox = glade_xml_get_widget (xml_commands, "custom_command_vbox");
		if (selected > SUSPEND_CMD) {
			/* We are dealing with custom commands */							
			gtk_widget_show (custom_cmd_vbox);
			gtk_widget_hide (hrs_cmd_vbox);					
		}
		else {
			/* We are dealing with hrs (Halt, Reboot, Shutdown) commands */
			gtk_widget_hide (custom_cmd_vbox);
			gtk_widget_show (hrs_cmd_vbox);	
		}

		/* We dont want default timeout for this one so we
		   are going to bail out now */
		run_timeout (combobox, 10, combobox_timeout);		
		return;
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

static void
toggle_toggled_sensitivity_negative (GtkWidget *toggle, GtkWidget *depend)
{
	gtk_widget_set_sensitive (depend, !gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (toggle)));
}

static void
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

	if (strcmp (ve_sure_string (name), "sysmenu") == 0) {
	
		GtkWidget *config_available;
		GtkWidget *chooser_button;
		
		config_available = glade_xml_get_widget (xml, "config_available");
		chooser_button = glade_xml_get_widget (xml, "chooser_button");

		gtk_widget_set_sensitive (config_available, val);
		gtk_widget_set_sensitive (chooser_button, val);
		
		g_signal_connect (G_OBJECT (toggle), "toggled",
		                  G_CALLBACK (toggle_toggled), toggle);	
		g_signal_connect (G_OBJECT (toggle), "toggled",
		                  G_CALLBACK (toggle_toggled_sensitivity_positive), config_available);
		g_signal_connect (G_OBJECT (toggle), "toggled",
		                  G_CALLBACK (toggle_toggled_sensitivity_positive), chooser_button);
	}
	else if (strcmp ("autologin", ve_sure_string (name)) == 0) {

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
	else if (strcmp ("hide_vis_feedback_passwd_checkbox", ve_sure_string (name)) == 0) {
		gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (toggle), val);
		
		g_signal_connect (G_OBJECT (toggle), "toggled",
		                  G_CALLBACK (toggle_toggled), toggle);		
	}
	else if (strcmp ("disallow_tcp", ve_sure_string (name)) == 0) {
		GtkWidget *nfs_cookies;
		
		nfs_cookies = glade_xml_get_widget (xml, "never_cookies_NFS_checkbutton");

		gtk_widget_set_sensitive (nfs_cookies, !val);

		g_signal_connect (G_OBJECT (toggle), "toggled",
		                  G_CALLBACK (toggle_toggled), toggle);	
		g_signal_connect (G_OBJECT (toggle), "toggled",
		                  G_CALLBACK (toggle_toggled_sensitivity_negative), nfs_cookies);
		
	}
	else {
		g_signal_connect (G_OBJECT (toggle), "toggled",
		                  G_CALLBACK (toggle_toggled), NULL);
	}
}

static gboolean
commands_entry_timeout (GtkWidget *entry)
{
	GtkWidget *apply_cmd_changes;
	GtkWidget *command_combobox;
	gint selected;

	const char  *key  = g_object_get_data (G_OBJECT (entry), "key");
	const gchar *val  = gtk_entry_get_text (GTK_ENTRY (entry));
	
	apply_cmd_changes = glade_xml_get_widget (xml_commands, "command_apply_button");
	command_combobox = glade_xml_get_widget (xml_commands, "cmd_type_combobox");
	
	selected = gtk_combo_box_get_active (GTK_COMBO_BOX (command_combobox));	

	/* All hrs (Halt, Shutdown, Suspend) commands will fall into this category */
	if (strcmp ("hrs_custom_cmd", ve_sure_string (key)) == 0) {
		gchar *old_val = NULL;
		gchar *cmd_key = NULL;

		if (selected == HALT_CMD)
			cmd_key = g_strdup (MDM_KEY_HALT);			
		else if (selected == REBOOT_CMD)
			cmd_key = g_strdup (MDM_KEY_REBOOT);
		else if (selected == SUSPEND_CMD)
			cmd_key = g_strdup (MDM_KEY_SUSPEND);
		
		old_val = mdm_config_get_string (cmd_key);		
			
		if (strcmp (ve_sure_string (val), ve_sure_string (old_val)) != 0) 			
			g_hash_table_insert (MdmCommandChangesUnsaved, g_strdup (cmd_key), g_strdup (val));

		else if (g_hash_table_lookup (MdmCommandChangesUnsaved, cmd_key) != NULL)			
			g_hash_table_remove (MdmCommandChangesUnsaved, cmd_key);		
		
		g_free (old_val);
		g_free (cmd_key);
	}
	/* All the custom commands will fall into this category */
	else {
		gchar *key_string = NULL;
		gchar *old_val = NULL;
		gint i;			

		i = selected - CUSTOM_CMD;
		key_string = g_strdup_printf("%s%d=", ve_sure_string (key), i); 
		old_val = mdm_config_get_string (key_string);				
	
		
		if (strcmp (ve_sure_string (val), ve_sure_string (old_val)) != 0)
			g_hash_table_insert (MdmCommandChangesUnsaved, g_strdup (key_string), g_strdup (val));
		
		else if (g_hash_table_lookup (MdmCommandChangesUnsaved, key_string) != NULL)
			g_hash_table_remove (MdmCommandChangesUnsaved, key_string);
		
		g_free (old_val);
		g_free (key_string);
	}	
	
	if (g_hash_table_size (MdmCommandChangesUnsaved) == 0)
		gtk_widget_set_sensitive (apply_cmd_changes, FALSE);
	else 
		gtk_widget_set_sensitive (apply_cmd_changes, TRUE);
	
	return FALSE;
}

static void
commands_entry_changed (GtkWidget *entry)
{
	run_timeout (entry, 100, commands_entry_timeout);
}

static void
setup_commands_text_entry (const char *name,
                           const char *key)
{
	GtkWidget *entry;

	entry = glade_xml_get_widget (xml_commands, name);
	
	g_object_set_data_full (G_OBJECT (entry),
	                        "key", g_strdup (key),
	                        (GDestroyNotify) g_free);

	g_signal_connect (G_OBJECT (entry), "changed",
		          G_CALLBACK (commands_entry_changed), NULL);
}

static void
setup_commands_notify_toggle (const char *name,
			      const char *key)
{
	GtkWidget *toggle;
	
	toggle = glade_xml_get_widget (xml_commands, name);
	
	g_object_set_data_full (G_OBJECT (toggle),
	                        "key", g_strdup (key),
	                        (GDestroyNotify) g_free);
	
	g_signal_connect (G_OBJECT (toggle), "toggled",
		          G_CALLBACK (command_toggle_toggled), NULL);
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
	users_string = g_list_append (users_string, g_strdup (""));

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
		if (strcmp (li->data, ve_sure_string (selected_user)) == 0)
			selected=cnt;
		gtk_list_store_append (combobox_store, &iter);
		gtk_list_store_set(combobox_store, &iter, USERLIST_NAME, li->data, -1);
		cnt++;
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

static GtkListStore *
setup_include_exclude (GtkWidget *treeview, const char *key)
{
	GtkListStore *face_store = gtk_list_store_new (USERLIST_NUM_COLUMNS,
		G_TYPE_STRING);
	GtkTreeIter iter;
	GtkTreeViewColumn *column;
	GtkCellRenderer *renderer;
	char **list;
	int i;

	column = gtk_tree_view_column_new ();

	renderer = gtk_cell_renderer_text_new ();
	gtk_tree_view_column_pack_start (column, renderer, TRUE);

	gtk_tree_view_column_set_attributes (column, renderer,
		"text", USERLIST_NAME, NULL);

	gtk_tree_view_append_column (GTK_TREE_VIEW (treeview), column);
	gtk_tree_view_set_model (GTK_TREE_VIEW(treeview),
		(GTK_TREE_MODEL (face_store)));

	if ((! ve_string_empty (MdmInclude)) && 
            (strcmp (ve_sure_string (key), MDM_KEY_INCLUDE) == 0))
		list = g_strsplit (MdmInclude, ",", 0);
	else if ((! ve_string_empty (MdmExclude)) &&
                 (strcmp (ve_sure_string (key), MDM_KEY_EXCLUDE) == 0))
		list = g_strsplit (MdmExclude, ",", 0);
	else
		list = NULL;

	for (i=0; list != NULL && list[i] != NULL; i++) {
		gtk_list_store_append (face_store, &iter);
		gtk_list_store_set(face_store, &iter, USERLIST_NAME, list[i], -1);
	}
	g_strfreev (list);

	return (face_store);
}

typedef enum {
	INCLUDE,
	EXCLUDE
} FaceType;

typedef struct _FaceCommon {
	GtkWidget *apply;
	GtkWidget *include_treeview;
	GtkWidget *exclude_treeview;
	GtkListStore *include_store;
	GtkListStore *exclude_store;
	GtkTreeModel *include_model;
	GtkTreeModel *exclude_model;
	GtkWidget *include_add;
	GtkWidget *exclude_add;
	GtkWidget *include_del;
	GtkWidget *exclude_del;
	GtkWidget *to_include_button;
	GtkWidget *to_exclude_button;
	GtkWidget *allusers;
} FaceCommon;

typedef struct _FaceData {
	FaceCommon *fc;
	FaceType type;
} FaceData;

typedef struct _FaceApply {
	FaceData *exclude;
	FaceData *include;
} FaceApply;

static void
face_add (FaceData *fd)
{
	GtkWidget *user_entry;
	const char *text = NULL;
	const char *model_text;
	GtkTreeIter iter;
	gboolean valid;

	user_entry = glade_xml_get_widget (xml_add_users, "fb_addentry");
	text = gtk_entry_get_text (GTK_ENTRY (user_entry));

	if (mdm_is_user_valid (text)) {
		valid = gtk_tree_model_get_iter_first (fd->fc->include_model, &iter);
		while (valid) {
			gtk_tree_model_get (fd->fc->include_model, &iter, USERLIST_NAME,
				 &model_text, -1);
			if (strcmp (ve_sure_string (text), ve_sure_string (model_text)) == 0) {
				GtkWidget *setup_dialog;
				GtkWidget *dialog;
				gchar *str;
				
				str = g_strdup_printf (_("The \"%s\" user already exists in the include list."), text); 
				
				setup_dialog = glade_xml_get_widget (xml_add_users, "add_user_dialog");
				dialog = hig_dialog_new (GTK_WINDOW (setup_dialog),
							 GTK_DIALOG_MODAL | 
							 GTK_DIALOG_DESTROY_WITH_PARENT,
							 GTK_MESSAGE_ERROR,
							 GTK_BUTTONS_OK,
							 _("Cannot add user"),
							 str);
				gtk_dialog_run (GTK_DIALOG (dialog));
				gtk_widget_destroy (dialog);
				g_free (str);				
				return;
			}

			valid = gtk_tree_model_iter_next (fd->fc->include_model, &iter);
		}

		valid = gtk_tree_model_get_iter_first (fd->fc->exclude_model, &iter);
		while (valid) {
			gtk_tree_model_get (fd->fc->exclude_model, &iter, USERLIST_NAME,
				 &model_text, -1);
			if (strcmp (ve_sure_string (text), ve_sure_string (model_text)) == 0) {
				GtkWidget *setup_dialog;
				GtkWidget *dialog;
				gchar *str;
				
				str = g_strdup_printf (_("The \"%s\" user already exists in the exclude list."), text); 
				
				setup_dialog = glade_xml_get_widget (xml_add_users, "add_user_dialog");
				dialog = hig_dialog_new (GTK_WINDOW (setup_dialog),
							 GTK_DIALOG_MODAL | 
							 GTK_DIALOG_DESTROY_WITH_PARENT,
							 GTK_MESSAGE_ERROR,
							 GTK_BUTTONS_OK,
							 _("Cannot add user"),
							 str);
				gtk_dialog_run (GTK_DIALOG (dialog));
				gtk_widget_destroy (dialog);
				g_free (str);	
				return;
			}

			valid = gtk_tree_model_iter_next (fd->fc->exclude_model, &iter);
		}

		if (fd->type == INCLUDE) {
			/* Now the user is valid but his/hers UID might be smaller than the MinimalUID */
			gint user_uid = mdm_user_uid (text);
			if (user_uid < mdm_config_get_int (MDM_KEY_MINIMAL_UID)) {
				GtkWidget *setup_dialog;
				GtkWidget *dialog;
				gchar *str;
				
				str = g_strdup_printf (_("The \"%s\" user UID is lower than allowed MinimalUID."), text); 
				
				setup_dialog = glade_xml_get_widget (xml_add_users, "add_user_dialog");
				dialog = hig_dialog_new (GTK_WINDOW (setup_dialog),
							 GTK_DIALOG_MODAL | 
							 GTK_DIALOG_DESTROY_WITH_PARENT,
							 GTK_MESSAGE_ERROR,
							 GTK_BUTTONS_OK,
							 _("Cannot add user"),
							 str);
				gtk_dialog_run (GTK_DIALOG (dialog));
				gtk_widget_destroy (dialog);
				g_free (str);
				return;
			}
			gtk_list_store_append (fd->fc->include_store, &iter);
			gtk_list_store_set (fd->fc->include_store, &iter,
				USERLIST_NAME, text, -1);
		} else if (fd->type == EXCLUDE) {
			gtk_list_store_append (fd->fc->exclude_store, &iter);
			gtk_list_store_set (fd->fc->exclude_store, &iter,
				USERLIST_NAME, text, -1);
		}
		gtk_widget_set_sensitive (fd->fc->apply, TRUE);
		MdmUserChangesUnsaved = TRUE;
	} else {
		GtkWidget *setup_dialog;
		GtkWidget *dialog;
		gchar *str;
		
		str = g_strdup_printf (_("The \"%s\" user does not exist."), text); 
			
		setup_dialog = glade_xml_get_widget (xml_add_users, "add_user_dialog");
		dialog = hig_dialog_new (GTK_WINDOW (setup_dialog),
					 GTK_DIALOG_MODAL | 
					 GTK_DIALOG_DESTROY_WITH_PARENT,
					 GTK_MESSAGE_ERROR,
					 GTK_BUTTONS_OK,
					 _("Cannot add user"),
					 str);
		gtk_dialog_run (GTK_DIALOG (dialog));
		gtk_widget_destroy (dialog);
		g_free (str);
	}
}

static void
face_del (GtkWidget *button, gpointer data)
{
	FaceData *fd = data;
	GtkTreeSelection *selection;
	GtkTreeIter iter;

	if (fd->type == INCLUDE) { 
		selection = gtk_tree_view_get_selection (
			GTK_TREE_VIEW (fd->fc->include_treeview));

		if (gtk_tree_selection_get_selected (selection, &(fd->fc->include_model), &iter)) {
			gtk_list_store_remove (fd->fc->include_store, &iter);
			gtk_widget_set_sensitive (fd->fc->apply, TRUE);
			MdmUserChangesUnsaved = TRUE;
		}
	} else if (fd->type == EXCLUDE) {
		selection = gtk_tree_view_get_selection (
			GTK_TREE_VIEW (fd->fc->exclude_treeview));

		if (gtk_tree_selection_get_selected (selection, &(fd->fc->exclude_model), &iter)) {
			gtk_list_store_remove (fd->fc->exclude_store, &iter);
			gtk_widget_set_sensitive (fd->fc->apply, TRUE);
			MdmUserChangesUnsaved = TRUE;
		}
	}
}

static void
browser_move (GtkWidget *button, gpointer data)
{
	FaceData *fd = data;
	GtkTreeSelection *selection = NULL;
	GtkTreeIter iter;
	GtkTreeModel *model = NULL;
	char *text;

	/* The fd->type value passed in corresponds with the list moving to */
	if (fd->type == INCLUDE) {
		model = fd->fc->exclude_model;
		selection = gtk_tree_view_get_selection (
			GTK_TREE_VIEW (fd->fc->exclude_treeview));
	} else if (fd->type == EXCLUDE) {
		model = fd->fc->include_model;
		selection = gtk_tree_view_get_selection (
			GTK_TREE_VIEW (fd->fc->include_treeview));
	}

	if (gtk_tree_selection_get_selected (selection, &model, &iter)) {
	        gtk_tree_model_get (model, &iter, USERLIST_NAME, &text, -1);
		if (fd->type == INCLUDE) {
			/* We cant move the users that have uid smaller that MinimalUID */
			gint user_uid = mdm_user_uid (text);
			if (user_uid < mdm_config_get_int (MDM_KEY_MINIMAL_UID)) {
				GtkWidget *setup_dialog;
				GtkWidget *dialog;
				gchar *str;
				
				str = g_strdup_printf (_("The \"%s\" user UID is lower than allowed MinimalUID."), text); 
				
				setup_dialog = glade_xml_get_widget (xml_add_users, "add_user_dialog");
				dialog = hig_dialog_new (GTK_WINDOW (setup_dialog),
							 GTK_DIALOG_MODAL | 
							 GTK_DIALOG_DESTROY_WITH_PARENT,
							 GTK_MESSAGE_ERROR,
							 GTK_BUTTONS_OK,
							 _("Cannot add user"),
							 str);
				gtk_dialog_run (GTK_DIALOG (dialog));
				gtk_widget_destroy (dialog);
				g_free (str);
				return;
			}
			gtk_list_store_remove (fd->fc->exclude_store, &iter);
			gtk_list_store_append (fd->fc->include_store, &iter);
			gtk_list_store_set (fd->fc->include_store, &iter,
				USERLIST_NAME, text, -1);
		} else if (fd->type == EXCLUDE) {
			gtk_list_store_remove (fd->fc->include_store, &iter);
			gtk_list_store_append (fd->fc->exclude_store, &iter);
			gtk_list_store_set (fd->fc->exclude_store, &iter,
				USERLIST_NAME, text, -1);
		}
		gtk_widget_set_sensitive (fd->fc->apply, TRUE);
		MdmUserChangesUnsaved = TRUE;
	}
}

/* Hash key happens to be equal to the config entry key,
   se we save the value under the key and clean up
*/
static gboolean
unsaved_data_from_hash_table_func (gpointer key, gpointer value, gpointer user_data)
{
	gchar *c_key = key;
        if (strncmp (c_key, MDM_KEY_CUSTOM_CMD_NO_RESTART_TEMPLATE, 
		     strlen (MDM_KEY_CUSTOM_CMD_NO_RESTART_TEMPLATE)) == 0 ||
	    strncmp (c_key, MDM_KEY_CUSTOM_CMD_IS_PERSISTENT_TEMPLATE,
		     strlen (MDM_KEY_CUSTOM_CMD_IS_PERSISTENT_TEMPLATE)) == 0) {
		gboolean *p_val = (gboolean*)value;
		mdm_setup_config_set_bool (c_key, *p_val);
	}
	else
		mdm_setup_config_set_string (c_key, (gchar*)value);	
	
	/* And final cleanup */
	g_free (value);
	g_free (key);

	return TRUE;
}

/* Go thru and remove each of the hash entries
   then clean the hash (if not already empty)
   just in case, then de-sensitise the apply 
   command changes button
*/
static void
command_apply (GtkWidget *button, gpointer data)
{
	const gchar *command = NULL;
	GtkWidget *cmd_path_entry = NULL;
	GtkWidget *command_combobox = (GtkWidget*)data;
	gboolean command_exists = FALSE;
	gint selected;
	
	selected = gtk_combo_box_get_active (GTK_COMBO_BOX (command_combobox));
	
	if (last_selected_command < CUSTOM_CMD)
		cmd_path_entry = glade_xml_get_widget (xml_commands, "hrs_cmd_path_entry");
	else
		cmd_path_entry = glade_xml_get_widget (xml_commands, "custom_cmd_path_entry");

	command = gtk_entry_get_text (GTK_ENTRY (cmd_path_entry));
	
	command_exists = ve_string_empty (command) || mdm_working_command_exists (command);
	
	if(command_exists)
		g_hash_table_foreach_remove (MdmCommandChangesUnsaved,
					     (GHRFunc) unsaved_data_from_hash_table_func, NULL);
	
	else {
		GtkWidget *parent = glade_xml_get_widget (xml_commands, "commands_dialog");

		GtkWidget *dialog = hig_dialog_new (GTK_WINDOW (parent),
						    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
						    GTK_MESSAGE_ERROR,
						    GTK_BUTTONS_OK,
						    _("Invalid command path"),
						    _("The path you provided for this "
						      "command is not valid. The changes "
						      "will not be saved."));
		gtk_dialog_run (GTK_DIALOG (dialog));
		gtk_widget_destroy (dialog);
	}
	
	/* Just to make sure */
	if (g_hash_table_size (MdmCommandChangesUnsaved) != 0)
		g_hash_table_remove_all (MdmCommandChangesUnsaved);

	gtk_widget_set_sensitive (button, FALSE);
	
	if (selected == last_selected_command)
		g_signal_emit_by_name (G_OBJECT (command_combobox), "changed");
	
	if(command_exists)
		update_greeters ();
}

static void
browser_apply (GtkWidget *button, gpointer data)
{
	FaceCommon *fc = data;
	GString *userlist = g_string_new (NULL);
	const char *model_text;
	char *val;
	GtkTreeIter iter;
	gboolean valid;
	gboolean update_greet = FALSE;
	char *sep = "";
	gint minimalUID = -1;
	gboolean any_removed = FALSE;
	
	minimalUID = mdm_config_get_int (MDM_KEY_MINIMAL_UID);
	
	valid = gtk_tree_model_get_iter_first (fc->include_model, &iter);
	while (valid) {
		gtk_tree_model_get (fc->include_model, &iter, USERLIST_NAME,
			 &model_text, -1);

		/* We need to take check that during the time between adding
		   a user and clicking on the apply button UID has not changed 
		   If so then the offending users should be removed
		*/
		if (mdm_user_uid (model_text) < minimalUID) {
			valid = gtk_list_store_remove (fc->include_store, &iter);
			any_removed = TRUE;
		}
		else {
			g_string_append (userlist, sep);
			sep = ",";
			g_string_append (userlist, model_text);		
			valid = gtk_tree_model_iter_next (fc->include_model, &iter);
		}
	}

	if (any_removed) {
		GtkWidget *setup_dialog;
		GtkWidget *dlg;
		
		setup_dialog = glade_xml_get_widget(xml, "setup_dialog");
		
		//Inform user about the change
		dlg = hig_dialog_new (GTK_WINDOW (setup_dialog),
				      GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
				      GTK_MESSAGE_WARNING,
				      GTK_BUTTONS_OK,
				      _("Users include list modification"),
				      _("Some of the users had uid lower than "
					"MinimalUID (Security tab) and "
					"could not be added."));
		gtk_dialog_run (GTK_DIALOG (dlg));
		gtk_widget_destroy (dlg);						
		
	}

	val = mdm_config_get_string (MDM_KEY_INCLUDE);

	if (strcmp (ve_sure_string (val),
		    ve_sure_string (userlist->str)) != 0) {
		mdm_setup_config_set_string (MDM_KEY_INCLUDE, userlist->str);
		update_greet = TRUE;
	}

	g_string_free (userlist, TRUE);

	userlist = g_string_new (NULL);
	sep = "";
	valid = gtk_tree_model_get_iter_first (fc->exclude_model, &iter);
	while (valid) {
		gtk_tree_model_get (fc->exclude_model, &iter, USERLIST_NAME,
			 &model_text, -1);

		g_string_append (userlist, sep);
		sep = ",";
		g_string_append (userlist, model_text);

		valid = gtk_tree_model_iter_next (fc->exclude_model, &iter);
	}

	val = mdm_config_get_string (MDM_KEY_EXCLUDE);

	if (strcmp (ve_sure_string (val),
		    ve_sure_string (userlist->str)) != 0) {
		mdm_setup_config_set_string (MDM_KEY_EXCLUDE, userlist->str);
		update_greet = TRUE;
	}

	if (update_greet)
		update_greeters ();

	/* Re-initialize combox with updated userlist. */
	MdmInclude = mdm_config_get_string (MDM_KEY_INCLUDE);
	MdmExclude = mdm_config_get_string (MDM_KEY_EXCLUDE);
	setup_user_combobox_list ("autologin_combo",
			  MDM_KEY_AUTOMATIC_LOGIN);
	setup_user_combobox_list ("timedlogin_combo",
			  MDM_KEY_TIMED_LOGIN);
	gtk_widget_set_sensitive (button, FALSE);

	MdmUserChangesUnsaved = FALSE;
	g_string_free (userlist, TRUE);
}


static void
face_rowdel (GtkTreeModel *treemodel, GtkTreePath *arg1, gpointer data)
{
	FaceCommon *fc = data;
	GtkTreeIter iter;
	GtkTreeSelection *selection;

	selection = gtk_tree_view_get_selection (
		GTK_TREE_VIEW (fc->include_treeview));
	if (gtk_tree_selection_get_selected (selection, &(fc->include_model), &iter)) {
		gtk_widget_set_sensitive (fc->to_exclude_button, TRUE);
		gtk_widget_set_sensitive (fc->include_del, TRUE);
	} else {
		gtk_widget_set_sensitive (fc->to_exclude_button, FALSE);
		gtk_widget_set_sensitive (fc->include_del, FALSE);
	}

	selection = gtk_tree_view_get_selection (
		GTK_TREE_VIEW (fc->exclude_treeview));
	if (gtk_tree_selection_get_selected (selection, &(fc->exclude_model), &iter)) {
		gtk_widget_set_sensitive (fc->to_include_button, TRUE);
		gtk_widget_set_sensitive (fc->exclude_del, TRUE);
	} else {
		gtk_widget_set_sensitive (fc->to_include_button, FALSE);
		gtk_widget_set_sensitive (fc->exclude_del, FALSE);
	}
}

static void
face_selection_changed (GtkTreeSelection *selection, gpointer data)
{
	FaceData *fd = data;
	GtkTreeIter iter;

	if (fd->type == INCLUDE) {
		if (gtk_tree_selection_get_selected (selection, &(fd->fc->include_model), &iter)) {
			gtk_widget_set_sensitive (fd->fc->to_exclude_button, TRUE);
			gtk_widget_set_sensitive (fd->fc->include_del, TRUE);
		} else {
			gtk_widget_set_sensitive (fd->fc->to_exclude_button, FALSE);
			gtk_widget_set_sensitive (fd->fc->include_del, FALSE);
		}
	} else if (fd->type == EXCLUDE) {
		if (gtk_tree_selection_get_selected (selection, &(fd->fc->exclude_model), &iter)) {
			gtk_widget_set_sensitive (fd->fc->to_include_button, TRUE);
			gtk_widget_set_sensitive (fd->fc->exclude_del, TRUE);
		} else {
			gtk_widget_set_sensitive (fd->fc->to_include_button, FALSE);
			gtk_widget_set_sensitive (fd->fc->exclude_del, FALSE);
		}
	}
}

static void
users_add_button_clicked (GtkWidget *button, gpointer data)
{
	static GtkWidget *dialog = NULL;
	FaceData *fd = data;
	GtkWidget *user_entry;
	GtkWidget *parent;
	
	if (dialog == NULL) {
		parent = glade_xml_get_widget (xml, "setup_dialog");
		dialog = glade_xml_get_widget (xml_add_users, "add_user_dialog");

		gtk_window_set_transient_for (GTK_WINDOW (dialog), GTK_WINDOW (parent));
		gtk_window_set_destroy_with_parent (GTK_WINDOW (dialog), TRUE);
	}

	user_entry = glade_xml_get_widget (xml_add_users, "fb_addentry");

	gtk_widget_grab_focus (user_entry);
	
	if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_OK) {
		face_add (fd);
	}
	gtk_widget_hide (dialog);
}

static void
setup_face (void)
{
	static FaceCommon fc;
	static FaceData fd_include;
	static FaceData fd_exclude;
	static FaceApply face_apply;

	GtkTreeSelection *selection;

	fc.include_add       = glade_xml_get_widget (xml, "fb_includeadd");
	fc.include_del       = glade_xml_get_widget (xml, "fb_includedelete");
	fc.exclude_add       = glade_xml_get_widget (xml, "fb_excludeadd");
	fc.exclude_del       = glade_xml_get_widget (xml, "fb_excludedelete");
	fc.to_include_button = glade_xml_get_widget (xml, "fb_toinclude");
	fc.to_exclude_button = glade_xml_get_widget (xml, "fb_toexclude");
	fc.apply             = glade_xml_get_widget (xml, "fb_faceapply");
	fc.include_treeview  = glade_xml_get_widget (xml, "fb_include_treeview");
	fc.exclude_treeview  = glade_xml_get_widget (xml, "fb_exclude_treeview");
	fc.allusers          = glade_xml_get_widget (xml, "fb_allusers");

	fc.include_store = setup_include_exclude (fc.include_treeview,
	                                          MDM_KEY_INCLUDE);
	fc.exclude_store = setup_include_exclude (fc.exclude_treeview,
	                                          MDM_KEY_EXCLUDE);

	fc.include_model = gtk_tree_view_get_model (
	                   GTK_TREE_VIEW (fc.include_treeview));
	fc.exclude_model = gtk_tree_view_get_model (
	                   GTK_TREE_VIEW (fc.exclude_treeview));

	fd_include.fc = &fc;
	fd_include.type = INCLUDE;

	fd_exclude.fc = &fc;
	fd_exclude.type = EXCLUDE;

	gtk_widget_set_sensitive (fc.include_del, FALSE);
	gtk_widget_set_sensitive (fc.exclude_del, FALSE);
	gtk_widget_set_sensitive (fc.to_include_button, FALSE);
	gtk_widget_set_sensitive (fc.to_exclude_button, FALSE);
	gtk_widget_set_sensitive (fc.apply, FALSE);

	face_apply.include = &fd_include;
	face_apply.exclude = &fd_exclude;

	xml_add_users = glade_xml_new (MDM_GLADE_DIR "/mdmsetup.glade", "add_user_dialog", NULL);

	g_signal_connect (G_OBJECT (fc.include_add), "clicked",
	                  G_CALLBACK (users_add_button_clicked), &fd_include);
	g_signal_connect (fc.exclude_add, "clicked",
	                  G_CALLBACK (users_add_button_clicked), &fd_exclude);
	g_signal_connect (fc.include_del, "clicked",
	                  G_CALLBACK (face_del), &fd_include);
	g_signal_connect (fc.exclude_del, "clicked",
	                  G_CALLBACK (face_del), &fd_exclude);

	g_signal_connect (fc.include_model, "row-deleted",
	                  G_CALLBACK (face_rowdel), &fc);
	g_signal_connect (fc.exclude_model, "row-deleted",
	                  G_CALLBACK (face_rowdel), &fc);

	selection = gtk_tree_view_get_selection (
	            GTK_TREE_VIEW (fc.include_treeview));
	gtk_tree_selection_set_mode (selection, GTK_SELECTION_SINGLE);
	g_signal_connect (selection, "changed",
	                  G_CALLBACK (face_selection_changed), &fd_include);
	selection = gtk_tree_view_get_selection (
	            GTK_TREE_VIEW (fc.exclude_treeview));
	gtk_tree_selection_set_mode (selection, GTK_SELECTION_SINGLE);
	g_signal_connect (selection, "changed",
	                  G_CALLBACK (face_selection_changed), &fd_exclude);

	g_signal_connect (fc.to_include_button, "clicked",
	                  G_CALLBACK (browser_move), &fd_include);
	g_signal_connect (fc.to_exclude_button, "clicked",
	                  G_CALLBACK (browser_move), &fd_exclude);

	g_signal_connect (fc.apply, "clicked",
	                  G_CALLBACK (browser_apply), &fc);
}

static void
include_all_toggle (GtkWidget *toggle)
{
	if (GTK_TOGGLE_BUTTON (toggle)->active)
		MdmIncludeAll = TRUE;
	else
		MdmIncludeAll = FALSE;

	setup_user_combobox_list ("autologin_combo",
			  MDM_KEY_AUTOMATIC_LOGIN);
	setup_user_combobox_list ("timedlogin_combo",
			  MDM_KEY_TIMED_LOGIN);
}

static gboolean
greeter_toggle_timeout (GtkWidget *toggle)
{
	const char *key = g_object_get_data (G_OBJECT (toggle), "key");
	gboolean val = mdm_config_get_bool ((gchar *)key);

	if ( ! bool_equal (val, GTK_TOGGLE_BUTTON (toggle)->active)) {
        
		mdm_setup_config_set_bool (key, GTK_TOGGLE_BUTTON (toggle)->active);
		update_greeters ();

		if (strcmp (ve_sure_string (key), MDM_KEY_INCLUDE_ALL) == 0) {
			include_all_toggle (toggle);
		}
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
	
	} else if (strcmp ("fb_allusers", ve_sure_string (name)) == 0) {

		GtkWidget *fb_includetree = glade_xml_get_widget (xml, "fb_include_treeview");
		GtkWidget *fb_buttonbox = glade_xml_get_widget (xml, "UsersButtonBox");
		GtkWidget *fb_includeadd = glade_xml_get_widget (xml, "fb_includeadd");
		GtkWidget *fb_includeremove = glade_xml_get_widget (xml, "fb_includedelete");
		GtkWidget *fb_includelabel = glade_xml_get_widget (xml, "fb_includelabel");

		gtk_widget_set_sensitive (fb_buttonbox, !val);
		gtk_widget_set_sensitive (fb_includetree, !val);
		gtk_widget_set_sensitive (fb_includelabel, !val);

		g_signal_connect (G_OBJECT (toggle), "toggled",	
			G_CALLBACK (sensitive_entry_toggled), fb_buttonbox);
		g_signal_connect (G_OBJECT (toggle), "toggled",	
			G_CALLBACK (sensitive_entry_toggled), fb_includetree);
		g_signal_connect (G_OBJECT (toggle), "toggled",	
			G_CALLBACK (sensitive_entry_toggled), fb_includeadd);
		g_signal_connect (G_OBJECT (toggle), "toggled",	
			G_CALLBACK (list_selection_toggled), fb_includeremove);
		g_signal_connect (G_OBJECT (toggle), "toggled",	
			G_CALLBACK (sensitive_entry_toggled), fb_includelabel);
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
	int         i;
	char       *prefix;
	char      **keys;

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
command_response (GtkWidget *button, gpointer data)
{

	GtkWidget *chooser = NULL;
	GtkWidget *setup_dialog;
	gint response;
	gchar *filename;
	
	const gchar *key;
	gchar *value;
	GtkWidget *command_combobox;
	GtkWidget *command_entry = NULL;
	gint selected;
	
	setup_dialog = glade_xml_get_widget (xml_commands, "commands_dialog");
	
	/* first get the file */
	chooser = gtk_file_chooser_dialog_new (_("Select Command"),
					       GTK_WINDOW (setup_dialog),
					       GTK_FILE_CHOOSER_ACTION_OPEN,
					       GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
					       GTK_STOCK_OK, GTK_RESPONSE_OK,
					       NULL);	

	response = gtk_dialog_run (GTK_DIALOG (chooser));
	
	if (response != GTK_RESPONSE_OK) {
		gtk_widget_destroy (chooser);
		return;
	}

	filename = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (chooser));
	gtk_widget_destroy (chooser);

	if (filename == NULL) {
		
		GtkWidget *dialog;
		
		dialog = hig_dialog_new (GTK_WINDOW (setup_dialog),
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

	key = g_object_get_data (G_OBJECT (button), "key");	
	
	/* Then according to the selected command
	   append the chosen filepath onto the existing
	   string */
	
	command_combobox = glade_xml_get_widget (xml_commands, "cmd_type_combobox");

	selected = gtk_combo_box_get_active (GTK_COMBO_BOX (command_combobox));
	
	if (selected == HALT_CMD)
		value = mdm_config_get_string (MDM_KEY_HALT);			
	else if (selected == REBOOT_CMD)
		value = mdm_config_get_string (MDM_KEY_REBOOT);		
	else if (selected == SUSPEND_CMD) 
		value = mdm_config_get_string (MDM_KEY_SUSPEND);
	else {
		gchar *key_string;
		gint i;

		i = selected - CUSTOM_CMD;
		
		key_string = g_strdup_printf("%s%d=", MDM_KEY_CUSTOM_CMD_TEMPLATE, i); 
		value = mdm_config_get_string (key_string);		
		g_free (key_string);
	}
	
	value = strings_list_add (value, filename, ";");
	
	if (strcmp (ve_sure_string (key), "add_hrs_cmd_button") == 0)
		command_entry = glade_xml_get_widget (xml_commands, "hrs_cmd_path_entry");
	else if (strcmp (ve_sure_string (key), "add_custom_cmd_button") == 0)
		command_entry = glade_xml_get_widget (xml_commands, "custom_cmd_path_entry");

	gtk_entry_set_text (GTK_ENTRY (command_entry), ve_sure_string (value));

	g_free (value);
	g_free (filename);
}

static void
default_filechooser_response (GtkWidget *file_chooser, gpointer data)
{
	gchar *filename;
	gchar *key;
	gchar *value;
		
	filename  = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (file_chooser));
	key = g_object_get_data (G_OBJECT (file_chooser), "key");	
	value     = mdm_config_get_string (key);
	
	if (strcmp (ve_sure_string (key), MDM_KEY_GLOBAL_FACE_DIR) == 0) {
		/* we need to append trailing / so it matches the default
		   config  values. This is not really necessary but makes
		   things neater */
		gchar *corr_filename;
		
		corr_filename = g_strdup_printf("%s/", ve_sure_string (filename));
		
		if (strcmp (ve_sure_string (value), corr_filename) != 0)
			mdm_setup_config_set_string (key, corr_filename);	
		
		g_free (corr_filename);
	}
	else {
		/* All other cases */
		if (strcmp (ve_sure_string (value), ve_sure_string (filename)) != 0)
			mdm_setup_config_set_string (key, ve_sure_string (filename));		
	}
	
	g_free (filename);
	g_free (value);
}

static void
setup_general_command_buttons (const char *name,
			       const char *key)
{
	GtkWidget *button = glade_xml_get_widget (xml_commands, name);
	
	g_object_set_data_full (G_OBJECT (button),
				"key", g_strdup (key),
				(GDestroyNotify) g_free);
	g_signal_connect (G_OBJECT (button), "clicked",
			  G_CALLBACK (command_response),
			  NULL);
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

static void
apply_command_changes (GObject *object, gint response, gpointer command_data)
{
	GtkWidget *dialog = command_data;
	
	if (g_hash_table_size (MdmCommandChangesUnsaved) != 0 && 
	    response != GTK_RESPONSE_HELP) {

		GtkWidget *prompt;
		gint response;

		prompt = hig_dialog_new (GTK_WINDOW (dialog),
					 GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
					 GTK_MESSAGE_WARNING,
					 GTK_BUTTONS_NONE,
					 _("Apply the changes to commands before closing?"),
					 _("If you don't apply, the changes made "
					   "will be disregarded."));
		
		gtk_dialog_add_button (GTK_DIALOG (prompt), _("Close _without Applying"), GTK_RESPONSE_CLOSE);
		gtk_dialog_add_button (GTK_DIALOG (prompt), "gtk-apply", GTK_RESPONSE_APPLY);
		
		response = gtk_dialog_run (GTK_DIALOG (prompt));
		gtk_widget_destroy (prompt);
		
		if (response == GTK_RESPONSE_APPLY) {
			GtkWidget *apply_button;

			apply_button = glade_xml_get_widget (xml_commands, "command_apply_button");
			g_signal_emit_by_name (G_OBJECT (apply_button), "clicked");
		}
		else {
			/* Just to make sure */
			if (g_hash_table_size (MdmCommandChangesUnsaved) != 0)
				g_hash_table_remove_all (MdmCommandChangesUnsaved);
		}
	}
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

static void
command_button_clicked (void)
{
	static GtkWidget *dialog = NULL;
	GtkWidget *command_chooser = NULL;
	gint selected = -1;
	gint response;

	if (dialog == NULL) {

		GtkWidget *parent;
		GtkWidget *apply_command_changes_button;
		gint i;

		xml_commands = glade_xml_new (MDM_GLADE_DIR "/mdmsetup.glade", "commands_dialog", NULL);

		command_chooser = glade_xml_get_widget (xml_commands, "cmd_type_combobox");

		glade_helper_tagify_label (xml_commands, "custom_cmd_note_label", "i");
		glade_helper_tagify_label (xml_commands, "custom_cmd_note_label", "small");

		glade_helper_tagify_label (xml_commands, "hrs_path_label", "i");
		glade_helper_tagify_label (xml_commands, "hrs_path_label", "small");
		glade_helper_tagify_label (xml_commands, "custom_path_label", "i");
		glade_helper_tagify_label (xml_commands, "custom_path_label", "small");
		glade_helper_tagify_label (xml_commands, "label_label", "i");
		glade_helper_tagify_label (xml_commands, "label_label", "small");
		glade_helper_tagify_label (xml_commands, "lrlabel_label", "i");
		glade_helper_tagify_label (xml_commands, "lrlabel_label", "small");		
		glade_helper_tagify_label (xml_commands, "text_label", "i");
		glade_helper_tagify_label (xml_commands, "text_label", "small");
		glade_helper_tagify_label (xml_commands, "tooltip_label", "i");
		glade_helper_tagify_label (xml_commands, "tooltip_label", "small");
		glade_helper_tagify_label (xml_commands, "persistent_label", "i");
		glade_helper_tagify_label (xml_commands, "persistent_label", "small");
		glade_helper_tagify_label (xml_commands, "norestart_label", "i");
		glade_helper_tagify_label (xml_commands, "norestart_label", "small");
	
		parent = glade_xml_get_widget (xml, "setup_dialog");
		dialog = glade_xml_get_widget (xml_commands, "commands_dialog");
		
		gtk_window_set_transient_for (GTK_WINDOW (dialog), GTK_WINDOW (parent));
		gtk_window_set_destroy_with_parent (GTK_WINDOW (dialog), TRUE);

		/* Set up unsaved changes storage container */
		MdmCommandChangesUnsaved = g_hash_table_new (g_str_hash, g_str_equal);
				
		
		/* Add halt, reboot and suspend commands */
		gtk_combo_box_append_text (GTK_COMBO_BOX (command_chooser), _("Halt command"));
		gtk_combo_box_append_text (GTK_COMBO_BOX (command_chooser), _("Reboot command"));
		gtk_combo_box_append_text (GTK_COMBO_BOX (command_chooser), _("Suspend command"));
		
		/* Add all the custom commands */
		for (i = 0; i < MDM_CUSTOM_COMMAND_MAX; i++) {
			gchar *label = g_strdup_printf("Custom command %d", i);
			gtk_combo_box_append_text (GTK_COMBO_BOX (command_chooser), label);
			g_free (label);
		}
		
		g_object_set_data_full (G_OBJECT (command_chooser), "key",
					"command_chooser_combobox", (GDestroyNotify) g_free);
		g_signal_connect (G_OBJECT (command_chooser), "changed",
				  G_CALLBACK (combobox_changed), NULL);			
		
		/* Lets setup handlers for all the entries 
		   They will be assigned exactly the same key and handler
		   as their only functionality would be to notify about changes */
		
		setup_commands_text_entry ("hrs_cmd_path_entry", "hrs_custom_cmd");
		setup_commands_text_entry ("custom_cmd_path_entry", MDM_KEY_CUSTOM_CMD_TEMPLATE);
		setup_commands_text_entry ("custom_cmd_label_entry", MDM_KEY_CUSTOM_CMD_LABEL_TEMPLATE);
		setup_commands_text_entry ("custom_cmd_lrlabel_entry", MDM_KEY_CUSTOM_CMD_LR_LABEL_TEMPLATE);
		setup_commands_text_entry ("custom_cmd_text_entry", MDM_KEY_CUSTOM_CMD_TEXT_TEMPLATE);
		setup_commands_text_entry ("custom_cmd_tooltip_entry", MDM_KEY_CUSTOM_CMD_TOOLTIP_TEMPLATE);
		
		setup_commands_notify_toggle ("custom_cmd_persistent_checkbutton", MDM_KEY_CUSTOM_CMD_IS_PERSISTENT_TEMPLATE);
		setup_commands_notify_toggle ("custom_cmd_norestart_checkbutton", MDM_KEY_CUSTOM_CMD_NO_RESTART_TEMPLATE);	
		
		/* Set up append command buttons */
		setup_general_command_buttons("hrs_command_add", "add_hrs_cmd_button");
		setup_general_command_buttons("custom_command_add", "add_custom_cmd_button");
		
		/* set up apply command changes button */
		apply_command_changes_button = glade_xml_get_widget (xml_commands, "command_apply_button");
		g_object_set_data_full (G_OBJECT (apply_command_changes_button), "key",
					g_strdup ("apply_command_changes"), (GDestroyNotify) g_free);
		g_signal_connect (G_OBJECT (apply_command_changes_button), "clicked",
				  G_CALLBACK (command_apply), command_chooser);
	
		gtk_widget_set_sensitive (apply_command_changes_button, FALSE);
		
						
		g_signal_connect (G_OBJECT (dialog), "response",
				  G_CALLBACK (apply_command_changes), dialog);
	}
	else {
		command_chooser = glade_xml_get_widget (xml_commands, "cmd_type_combobox");

		selected = gtk_combo_box_get_active (GTK_COMBO_BOX (command_chooser));
		
	}

	/* Finally lets set our default choice */
	gtk_combo_box_set_active (GTK_COMBO_BOX (command_chooser), HALT_CMD); 
	if (selected == last_selected_command)
		g_signal_emit_by_name (G_OBJECT (command_chooser), "changed");

	do {
		response = gtk_dialog_run (GTK_DIALOG (dialog));
		if (response == GTK_RESPONSE_HELP) {
			g_spawn_command_line_sync ("gnome-open ghelp:mdm", NULL, NULL,
						   NULL, NULL);
		}
	} while (response != GTK_RESPONSE_CLOSE &&
                 response != GTK_RESPONSE_DELETE_EVENT);
	
	gtk_widget_hide (dialog);
}

static void
vt_spinbutton_activate (GtkWidget * widget,
                        gpointer data)
{
	GtkDialog * dialog = data;
	gtk_dialog_response (dialog, GTK_RESPONSE_OK);
}

static void
setup_greeter_combobox (const char *name,
                        const char *key)
{
	GtkWidget *combobox = glade_xml_get_widget (xml, name);
	char *greetval      = g_strdup (mdm_config_get_string ((gchar *)key));

	if (greetval != NULL &&
	    strcmp (ve_sure_string (greetval),
	    LIBEXECDIR "/mdmlogin --disable-sound --disable-crash-dialog") == 0) {
		g_free (greetval);
		greetval = g_strdup (LIBEXECDIR "/mdmlogin");
	}

	/* Set initial state of local style combo box. */
	if (strcmp (ve_sure_string (key), MDM_KEY_GREETER) == 0) {
		GtkWidget *local_plain_vbox;
		GtkWidget *local_themed_vbox;			
		GtkWidget *local_html_vbox;

		local_plain_vbox = glade_xml_get_widget (xml, "local_plain_properties_vbox");
		local_themed_vbox = glade_xml_get_widget (xml, "local_themed_properties_vbox");
		local_html_vbox = glade_xml_get_widget (xml, "local_html_properties_vbox");
		
		if (strstr (greetval, "/mdmgreeter") != NULL) {						
			gtk_combo_box_set_active (GTK_COMBO_BOX (combobox), LOCAL_THEMED);
			gtk_widget_hide (local_plain_vbox);
			gtk_widget_show (local_themed_vbox);
			gtk_widget_hide (local_html_vbox);
		}
		else if (strstr (greetval, "/mdmwebkit") != NULL) {			
            gtk_combo_box_set_active (GTK_COMBO_BOX (combobox), LOCAL_HTML);				
			gtk_widget_hide (local_plain_vbox);
			gtk_widget_hide (local_themed_vbox);
			gtk_widget_show (local_html_vbox);
		}
		else {						
			gtk_combo_box_set_active (GTK_COMBO_BOX (combobox), LOCAL_PLAIN);			
			gtk_widget_show (local_plain_vbox);
			gtk_widget_hide (local_themed_vbox);
			gtk_widget_hide (local_html_vbox);
		}		
	}
	

	g_object_set_data_full (G_OBJECT (combobox), "key",
	                        g_strdup (key), (GDestroyNotify) g_free);
	g_signal_connect (G_OBJECT (combobox), "changed",
	                  G_CALLBACK (combobox_changed), NULL);

	g_free (greetval);
}

/* This function concatenates *string onto *strings_list with the addition
   of *sep as a deliminator inbetween the strings_list and string, then
   returns a copy of the new strings_list. */
static char *
strings_list_add (char *strings_list, const char *string, const char *sep)
{
	char *n;
	if (ve_string_empty (strings_list))
		n = g_strdup (string);
	else
		n = g_strconcat (strings_list, sep, string, NULL);
	g_free (strings_list);
	return n;
}

/* This function removes *string with the addition of *sep
   as a postfix deliminator the string from *strings_list, then
   returns a copy of the new strings_list. */
static char *
strings_list_remove (char *strings_list, const char *string, const char *sep)
{
    char **actions;   
    gint i;
    GString *msg;
    const char *separator = "";
    char *n;

    if (ve_string_empty (strings_list))
        return strings_list;
    
    msg = g_string_new ("");

    actions = g_strsplit (strings_list, sep, -1);
    g_assert (actions != NULL);
    for (i = 0; actions[i] != NULL; i++) {
        if (strncmp (actions[i], string, strlen (string)) == 0)
            continue;
        g_string_append_printf (msg, "%s%s", separator, actions[i]);
        separator = sep;
    }
    g_strfreev (actions);
    n = g_strdup (msg->str);
    g_string_free (msg, TRUE);
    g_free (strings_list);
    return n;
}

static void
setup_users_tab (void)
{
	GtkFileFilter *filter;
	GtkWidget *default_face_filechooser;
	GtkWidget *default_face_checkbox;
	GtkWidget *global_face_dir_filechooser;
	GtkWidget *global_face_dir_checkbox;
	gchar *filename;

	setup_greeter_toggle ("fb_allusers",
			      MDM_KEY_INCLUDE_ALL);
	setup_face ();

	/* Setup default face */
	default_face_filechooser = glade_xml_get_widget (xml, "default_face_filechooser");

	filter = gtk_file_filter_new ();
        gtk_file_filter_set_name (filter, _("Images"));
        gtk_file_filter_add_pixbuf_formats (filter);
        gtk_file_chooser_add_filter (GTK_FILE_CHOOSER (default_face_filechooser), filter);

	filename = mdm_config_get_string (MDM_KEY_DEFAULT_FACE);

	default_face_checkbox = glade_xml_get_widget (xml, "default_face_checkbutton");

	if (!ve_string_empty (filename) && access (filename, R_OK|X_OK)) {
		gtk_file_chooser_set_filename (GTK_FILE_CHOOSER (default_face_filechooser),
					       filename);
		
		gtk_widget_set_sensitive (default_face_filechooser, TRUE);
		gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (default_face_checkbox), TRUE);
		
	}	
	else {
		gtk_widget_set_sensitive (default_face_filechooser, FALSE);
		gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (default_face_checkbox), FALSE);
	}
	
	g_object_set_data_full (G_OBJECT (default_face_filechooser),
				"key", g_strdup (MDM_KEY_DEFAULT_FACE),
				(GDestroyNotify) g_free);
	g_signal_connect (G_OBJECT (default_face_filechooser), "selection-changed",
			  G_CALLBACK (default_filechooser_response),
			  NULL);	
	
	g_object_set_data_full (G_OBJECT (default_face_checkbox),
				"key", g_strdup (MDM_KEY_DEFAULT_FACE),
				(GDestroyNotify) g_free);       	
	g_signal_connect (G_OBJECT (default_face_checkbox), "toggled",
			  G_CALLBACK (toggle_toggled), default_face_checkbox);	
	g_signal_connect (G_OBJECT (default_face_checkbox), "toggled",
			  G_CALLBACK (toggle_toggled_sensitivity_positive), default_face_filechooser);			
	
	/* Setup global face dir */
	g_free (filename);		
	
	global_face_dir_filechooser = glade_xml_get_widget (xml, "global_face_dir_filechooser");

	filename = mdm_config_get_string (MDM_KEY_GLOBAL_FACE_DIR);

	global_face_dir_checkbox = glade_xml_get_widget (xml, "global_face_dir_checkbutton");

	if (!ve_string_empty (filename) && access (filename, R_OK|X_OK) == 0) {
		gtk_file_chooser_set_filename (GTK_FILE_CHOOSER (global_face_dir_filechooser),
					       filename);
		gtk_widget_set_sensitive (global_face_dir_filechooser, TRUE);
		gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (global_face_dir_checkbox), TRUE);
	}
	else {
		gtk_widget_set_sensitive (global_face_dir_filechooser, FALSE);
		gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (global_face_dir_checkbox), FALSE);
	}
       

	g_object_set_data_full (G_OBJECT (global_face_dir_filechooser),
				"key", g_strdup (MDM_KEY_GLOBAL_FACE_DIR),
				(GDestroyNotify) g_free);		
	g_signal_connect (G_OBJECT (global_face_dir_filechooser), "selection-changed",
			  G_CALLBACK (default_filechooser_response), NULL);
			
	g_object_set_data_full (G_OBJECT (global_face_dir_checkbox),
				"key", g_strdup (MDM_KEY_GLOBAL_FACE_DIR),
				(GDestroyNotify) g_free);       	
	g_signal_connect (G_OBJECT (global_face_dir_checkbox), "toggled",
			  G_CALLBACK (toggle_toggled), global_face_dir_checkbox);	
	g_signal_connect (G_OBJECT (global_face_dir_checkbox), "toggled",
			  G_CALLBACK (toggle_toggled_sensitivity_positive), global_face_dir_filechooser);				
	g_free (filename);
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
		theme_dir = g_strdup (DATADIR "/mdm/themes/");
	}

	return theme_dir;
}

static void
textview_set_buffer (GtkTextView *view, const char *text)
{
	GtkTextBuffer *buffer = gtk_text_view_get_buffer (view);
	gtk_text_buffer_set_text (buffer, text, -1);
}

/* Sets up the preview section of Themed Greeter page
   after a theme has been selected */
static void
gg_selection_changed (GtkTreeSelection *selection, gpointer data)
{
	GtkWidget *theme_list;		
	GtkWidget *delete_button;	
	GtkTreeModel *model;
	GtkTreeIter iter;	
	GValue value  = {0, };			

	delete_button = glade_xml_get_widget (xml, "gg_delete_theme");	

	if ( !gtk_tree_selection_get_selected (selection, &model, &iter)) {
		gtk_widget_set_sensitive (delete_button, FALSE);		
		return;
	}

	/* Default to allow deleting of themes */	
	gtk_widget_set_sensitive (delete_button, TRUE);
			
	theme_list = glade_xml_get_widget (xml, "gg_theme_list");	
	model = gtk_tree_view_get_model (GTK_TREE_VIEW (theme_list));			
	
	/* Determine if the theme selected is currently active */	
	gtk_tree_model_get_value (model, &iter, THEME_COLUMN_SELECTED, &value);
	
	/* Do not allow deleting of active themes */
	if (g_value_get_boolean (&value)) {
		gtk_widget_set_sensitive (delete_button, FALSE);
	}
	g_value_unset (&value);
}

static GtkTreeIter *
read_themes (GtkListStore *store, const char *theme_dir, DIR *dir,
	     const char *select_item)
{
	struct dirent *dent;
	GtkTreeIter *select_iter = NULL;
	GdkPixbuf *pb = NULL;
	gchar *markup = NULL;	
	
	while ((dent = readdir (dir)) != NULL) {
		char *n, *file, *name, *desc, *ss;
		char *full;
		GtkTreeIter iter;
		gboolean sel_theme;
		gboolean sel_themes;
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

		if (selected_theme != NULL &&
		    strcmp (ve_sure_string (dent->d_name), ve_sure_string (selected_theme)) == 0)
			sel_theme = TRUE;
		else
			sel_theme = FALSE;

		sel_themes = FALSE;

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
					   
		markup = g_markup_printf_escaped ("<b>%s</b>\n<small>%s</small>",
                   name ? name : "(null)",
                   desc ? desc : "(null)");
		gtk_list_store_append (store, &iter);
		gtk_list_store_set (store, &iter,
				    THEME_COLUMN_SELECTED, sel_theme,			
				    THEME_COLUMN_DIR, dent->d_name,
				    THEME_COLUMN_FILE, file,
				    THEME_COLUMN_SCREENSHOT, pb,
				    THEME_COLUMN_MARKUP, markup,
				    THEME_COLUMN_NAME, name,
				    THEME_COLUMN_DESCRIPTION, desc,				    
				    -1);

		if (select_item != NULL &&
		    strcmp (ve_sure_string (dent->d_name), ve_sure_string (select_item)) == 0) {
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

	return select_iter;
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
	
	/* Determine if the theme selected is currently active */	
	gtk_tree_model_get_value (model, &iter, THEME_COLUMN_SELECTED, &value);
	
	/* Do not allow deleting of active themes */
	if (g_value_get_boolean (&value)) {
		gtk_widget_set_sensitive (delete_button, FALSE);
	}
	g_value_unset (&value);
}

static GtkTreeIter *
read_html_themes (GtkListStore *store, const char *theme_dir, DIR *dir,
	     const char *select_item)
{
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
		
		if (selected_html_theme != NULL &&
		    strcmp (ve_sure_string (dent->d_name), ve_sure_string (selected_html_theme)) == 0)
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
					   
		markup = g_markup_printf_escaped ("<b>%s</b>\n<small>%s</small>",
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
				    -1);

		if (select_item != NULL &&
		    strcmp (ve_sure_string (dent->d_name), ve_sure_string (select_item)) == 0) {
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

	return select_iter;
}

static gboolean
greeter_theme_timeout (GtkWidget *toggle)
{
	char *theme;	

	theme  = mdm_config_get_string (MDM_KEY_GRAPHICAL_THEME);	
	
	/* If themes have changed from the custom_config file, update it. */
	if (strcmp (ve_sure_string (theme),
		ve_sure_string (selected_theme)) != 0) {

		mdm_setup_config_set_string (MDM_KEY_GRAPHICAL_THEME,
			selected_theme);
		update_greeters ();
	}
	
	return FALSE;
}

static gboolean
html_greeter_theme_timeout (GtkWidget *toggle)
{
	char *theme;	

	theme  = mdm_config_get_string (MDM_KEY_HTML_THEME);	
	
	/* If themes have changed from the custom_config file, update it. */
	if (strcmp (ve_sure_string (theme),
		ve_sure_string (selected_html_theme)) != 0) {

		mdm_setup_config_set_string (MDM_KEY_HTML_THEME,
			selected_html_theme);
		update_greeters ();
	}
	
	return FALSE;
}

static void
selected_toggled (GtkCellRendererToggle *cell,
		  char                  *path_str,
		  gpointer               data)
{
	gchar *theme_name   = NULL;
	GtkTreeModel *model = GTK_TREE_MODEL (data);
	GtkTreeIter selected_iter;
	GtkTreeIter iter;
	GtkTreePath *path;
	GtkTreePath *sel_path = gtk_tree_path_new_from_string (path_str);
	GtkWidget *theme_list = glade_xml_get_widget (xml, "gg_theme_list");
	GtkWidget *del_button = glade_xml_get_widget (xml, "gg_delete_theme");	

	gtk_tree_model_get_iter (model, &selected_iter, sel_path);
	path     = gtk_tree_path_new_first ();
	
	/* Clear list of all selected themes */
	g_free (selected_theme);

	/* Get the new selected theme */
	gtk_tree_model_get (model, &selected_iter,
				THEME_COLUMN_DIR, &selected_theme, -1);

	/* Loop through all themes in list */
	while (gtk_tree_model_get_iter (model, &iter, path)) {
		/* If this toggle was just toggled */
		if (gtk_tree_path_compare (path, sel_path) == 0) {
			gtk_list_store_set (GTK_LIST_STORE (model), &iter,
				THEME_COLUMN_SELECTED, TRUE,
				-1); /* Toggle ON */
			gtk_widget_set_sensitive (del_button, FALSE);			
		} else {
			gtk_list_store_set (GTK_LIST_STORE (model), &iter,
				THEME_COLUMN_SELECTED, FALSE,
				-1); /* Toggle OFF */
		}

		gtk_tree_path_next (path);
	}

	gtk_tree_path_free (path);
	gtk_tree_path_free (sel_path);

	run_timeout (theme_list, 500, greeter_theme_timeout);
}

static void
selected_html_toggled (GtkCellRendererToggle *cell,
		  char                  *path_str,
		  gpointer               data)
{
	gchar *theme_name   = NULL;
	GtkTreeModel *model = GTK_TREE_MODEL (data);
	GtkTreeIter selected_iter;
	GtkTreeIter iter;
	GtkTreePath *path;
	GtkTreePath *sel_path = gtk_tree_path_new_from_string (path_str);
	GtkWidget *theme_list = glade_xml_get_widget (xml, "gg_html_theme_list");
	GtkWidget *del_button = glade_xml_get_widget (xml, "gg_delete_html_theme");		

	gtk_tree_model_get_iter (model, &selected_iter, sel_path);
	path     = gtk_tree_path_new_first ();	
		
	/* Clear list of all selected themes */
	g_free (selected_html_theme);

	/* Get the new selected theme */
	gtk_tree_model_get (model, &selected_iter,
				THEME_COLUMN_DIR, &selected_html_theme, -1);

	/* Loop through all themes in list */
	while (gtk_tree_model_get_iter (model, &iter, path)) {
		/* If this toggle was just toggled */
		if (gtk_tree_path_compare (path, sel_path) == 0) {
			gtk_list_store_set (GTK_LIST_STORE (model), &iter,
				THEME_COLUMN_SELECTED, TRUE,
				-1); /* Toggle ON */
			gtk_widget_set_sensitive (del_button, FALSE);			
		} else {
			gtk_list_store_set (GTK_LIST_STORE (model), &iter,
				THEME_COLUMN_SELECTED, FALSE,
				-1); /* Toggle OFF */
		}

		gtk_tree_path_next (path);
	}	

	gtk_tree_path_free (path);
	gtk_tree_path_free (sel_path);

	run_timeout (theme_list, 500, html_greeter_theme_timeout);
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

/* sense the right unzip program */
static char *
find_unzip (gchar *filename)
{
	char *prog;
	char *tryg[] = {
		"/bin/gunzip",
		"/usr/bin/gunzip",
		NULL };
	char *tryb[] = {
		"/bin/bunzip2",
		"/usr/bin/bunzip2",
		NULL };
	int i;

	if (is_ext (filename, ".bz2")) {
		prog = g_find_program_in_path ("bunzip2");
		if (prog != NULL)
			return prog;

		for (i = 0; tryb[i] != NULL; i++) {
			if (g_access (tryb[i], X_OK) == 0)
				return g_strdup (tryb[i]);
		}
	}

	prog = g_find_program_in_path ("gunzip");
	if (prog != NULL)
		return prog;

	for (i = 0; tryg[i] != NULL; i++) {
		if (g_access (tryg[i], X_OK) == 0)
			return g_strdup (tryg[i]);
	}
	/* Hmmm, fallback */
	return g_strdup ("/bin/gunzip");
}

static char *
find_tar (void)
{
	char *tar_prog;
	char *try[] = {
		"/bin/gtar",
		"/bin/tar",
		"/usr/bin/gtar",
		"/usr/bin/tar",
		NULL };
	int i;

	tar_prog = g_find_program_in_path ("gtar");
	if (tar_prog != NULL)
		return tar_prog;

	tar_prog = g_find_program_in_path ("tar");
	if (tar_prog != NULL)
		return tar_prog;

	for (i = 0; try[i] != NULL; i++) {
		if (g_access (try[i], X_OK) == 0)
			return g_strdup (try[i]);
	}
	/* Hmmm, fallback */
	return g_strdup ("/bin/tar");
}

static char *
find_chmod (void)
{
	char *chmod_prog;
	char *try[] = {
		"/bin/chmod",
		"/sbin/chmod",
		"/usr/bin/chmod",
		"/usr/sbin/chmod",
		NULL };
	int i;

	chmod_prog = g_find_program_in_path ("chmod");
	if (chmod_prog != NULL)
		return chmod_prog;

	for (i = 0; try[i] != NULL; i++) {
		if (g_access (try[i], X_OK) == 0)
			return g_strdup (try[i]);
	}
	/* Hmmm, fallback */
	return g_strdup ("/bin/chmod");
}

static char *
find_chown (void)
{
	char *chown_prog;
	char *try[] = {
		"/bin/chown",
		"/sbin/chown",
		"/usr/bin/chown",
		"/usr/sbin/chown",
		NULL };
	int i;

	chown_prog = g_find_program_in_path ("chown");
	if (chown_prog != NULL)
		return chown_prog;

	for (i = 0; try[i] != NULL; i++) {
		if (g_access (try[i], X_OK) == 0)
			return g_strdup (try[i]);
	}
	/* Hmmm, fallback */
	return g_strdup ("/bin/chown");
}


static char *
get_the_dir (FILE *fp, char **error)
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
				*error =
					_("Archive is not of a subdirectory");

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
			if (strcmp (ve_sure_string (buf), ve_sure_string (s)) == 0)
				got_info = TRUE;
			g_free (s);
		}

		if ( ! got_info) {
			s = g_strconcat (dir, "/GdmGreeterTheme.desktop", NULL);
			if (strcmp (ve_sure_string (buf), ve_sure_string (s)) == 0)
				got_info = TRUE;
			g_free (s);
		}
		
		if ( ! got_info) {
			s = g_strconcat (dir, "/theme.info", NULL);
			if (strcmp (ve_sure_string (buf), ve_sure_string (s)) == 0)
				got_info = TRUE;
			g_free (s);
		}
	}

	if (got_info)
		return dir;

	if ( ! read_a_line)
		*error = _("File not a tar.gz or tar archive");
	else
		*error = _("Archive does not include a "
			   "GdmGreeterTheme.info file");

	g_free (dir);
	return NULL;
}

static char *
get_archive_dir (gchar *filename, char **untar_cmd, char **error)
{
	char *quoted;
	char *tar;
	char *unzip;
	char *cmd;
	char *dir;
	FILE *fp;

	*untar_cmd = NULL;

	*error = NULL;

	if (g_access (filename, F_OK) != 0) {
		*error = _("File does not exist");
		return NULL;
	}

	quoted = g_shell_quote (filename);
	tar = find_tar ();
	unzip = find_unzip (filename);

	cmd = g_strdup_printf ("%s -c %s | %s -tf -", unzip, quoted, tar);
	fp = popen (cmd, "r");
	g_free (cmd);
	if (fp != NULL) {
		int ret;
		dir = get_the_dir (fp, error);
		ret = pclose (fp);
		if (ret == 0 && dir != NULL) {
			*untar_cmd = g_strdup_printf ("%s -c %s | %s -xf -",
						      unzip, quoted, tar);
			g_free (tar);
			g_free (unzip);
			g_free (quoted);
			return dir;
		} else {
			*error = NULL;
		}
		g_free (dir);
	}

	/* error due to command failing */
	if (*error == NULL) {
		/* Try uncompressed? */
		cmd = g_strdup_printf ("%s -tf %s", tar, quoted);
		fp = popen (cmd, "r");
		g_free (cmd);
		if (fp != NULL) {
			int ret;
			dir = get_the_dir (fp, error);
			ret = pclose (fp);
			if (ret == 0 && dir != NULL) {
				*untar_cmd = g_strdup_printf ("%s -xf %s",
							      tar, quoted);
				g_free (tar);
				g_free (unzip);
				g_free (quoted);
				return dir;
			} else {
				*error = NULL;
			}
			g_free (dir);
		}
	}

	if (*error == NULL)
		*error = _("File not a tar.gz or tar archive");

	g_free (tar);
	g_free (unzip);
	g_free (quoted);

	return NULL;
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

static void
install_theme_file (gchar *filename, GtkListStore *store, GtkWindow *parent)
{
	GtkTreeSelection *selection;
	GtkTreeIter *select_iter = NULL;
	GtkWidget *theme_list;
	DIR *dp;
	gchar *cwd;
	gchar *dir;
	gchar *error;
	gchar *theme_dir;
	gchar *untar_cmd;
	gboolean success = FALSE;

	theme_list = glade_xml_get_widget (xml, "gg_theme_list");

	cwd = g_get_current_dir ();
	theme_dir = get_theme_dir ();

	if ( !g_path_is_absolute (filename)) {

		gchar *temp;
		
		temp = g_build_filename (cwd, filename, NULL);
		g_free (filename);
		filename = temp;
	}
	
	dir = get_archive_dir (filename, &untar_cmd, &error);

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
		gchar *chown;
		gchar *chmod;

		quoted = g_strconcat ("./", dir, NULL);
		chown = find_chown ();
		chmod = find_chmod ();
		success = TRUE;

		/* HACK! */
		argv[0] = chown;
		argv[1] = "-R";
		argv[2] = "root:root";
		argv[3] = quoted;
		argv[4] = NULL;
		simple_spawn_sync (argv);

		argv[0] = chmod;
		argv[1] = "-R";
		argv[2] = "a+r";
		argv[3] = quoted;
		argv[4] = NULL;
		simple_spawn_sync (argv);

		argv[0] = chmod;
		argv[1] = "a+x";
		argv[2] = quoted;
		argv[3] = NULL;
		simple_spawn_sync (argv);

		g_free (quoted);
		g_free (chown);
		g_free (chmod);
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

	gtk_list_store_clear (store);

	dp = opendir (theme_dir);

	if (dp != NULL) {
		select_iter = read_themes (store, theme_dir, dp, dir);
		closedir (dp);
	}

	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (theme_list));

	if (select_iter != NULL) {
		gtk_tree_selection_select_iter (selection, select_iter);
		g_free (select_iter);
	}
	
	g_free (untar_cmd);
	g_free (theme_dir);
	g_free (dir);
	g_free (cwd);
}

static void
html_install_theme_file (gchar *filename, GtkListStore *store, GtkWindow *parent)
{
	GtkTreeSelection *selection;
	GtkTreeIter *select_iter = NULL;
	GtkWidget *theme_list;
	DIR *dp;
	gchar *cwd;
	gchar *dir;
	gchar *error;
	gchar *theme_dir;
	gchar *untar_cmd;
	gboolean success = FALSE;

	theme_list = glade_xml_get_widget (xml, "gg_html_theme_list");

	cwd = g_get_current_dir ();
	theme_dir = "/usr/share/mdm/html-themes";

	if ( !g_path_is_absolute (filename)) {

		gchar *temp;
		
		temp = g_build_filename (cwd, filename, NULL);
		g_free (filename);
		filename = temp;
	}
	
	dir = get_archive_dir (filename, &untar_cmd, &error);
		
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
		gchar *chown;
		gchar *chmod;

		quoted = g_strconcat ("./", dir, NULL);
		chown = find_chown ();
		chmod = find_chmod ();
		success = TRUE;

		/* HACK! */
		argv[0] = chown;
		argv[1] = "-R";
		argv[2] = "root:root";
		argv[3] = quoted;
		argv[4] = NULL;
		simple_spawn_sync (argv);

		argv[0] = chmod;
		argv[1] = "-R";
		argv[2] = "a+r";
		argv[3] = quoted;
		argv[4] = NULL;
		simple_spawn_sync (argv);

		argv[0] = chmod;
		argv[1] = "a+x";
		argv[2] = quoted;
		argv[3] = NULL;
		simple_spawn_sync (argv);

		g_free (quoted);
		g_free (chown);
		g_free (chmod);
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

	gtk_list_store_clear (store);
		
	dp = opendir (theme_dir);
		
	if (dp != NULL) {
		select_iter = read_html_themes (store, theme_dir, dp, dir);
		closedir (dp);
	}
		
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
theme_install_response (GtkWidget *chooser, gint response, gpointer data)
{
	GtkListStore *store = data;
	gchar *filename;

	if (response != GTK_RESPONSE_OK) {
		gtk_widget_destroy (chooser);
		return;
	}

	if (last_theme_installed != NULL) {
		g_free (last_theme_installed);
	}

	filename = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (chooser));	
	last_theme_installed = g_strdup (filename);
	
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

	install_theme_file (filename, store, GTK_WINDOW (chooser));
	gtk_widget_destroy (chooser);
	g_free (filename);
}

static void
install_new_theme (GtkWidget *button, gpointer data)
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

	g_signal_connect (G_OBJECT (chooser), "destroy",
			  G_CALLBACK (gtk_widget_destroyed), &chooser);
	g_signal_connect (G_OBJECT (chooser), "response",
			  G_CALLBACK (theme_install_response), store);

	if (last_theme_installed != NULL) {
		gtk_file_chooser_set_filename (GTK_FILE_CHOOSER (chooser),
		                               last_theme_installed);
	}
	gtk_widget_show (chooser);
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

	if (last_html_theme_installed != NULL) {
		g_free (last_html_theme_installed);
	}

	filename = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (chooser));	
	last_html_theme_installed = g_strdup (filename);
	
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

	g_signal_connect (G_OBJECT (chooser), "destroy",
			  G_CALLBACK (gtk_widget_destroyed), &chooser);
	g_signal_connect (G_OBJECT (chooser), "response",
			  G_CALLBACK (html_theme_install_response), store);

	if (last_html_theme_installed != NULL) {
		gtk_file_chooser_set_filename (GTK_FILE_CHOOSER (chooser),
		                               last_html_theme_installed);
	}
	gtk_widget_show (chooser);
}

static void
delete_theme (GtkWidget *button, gpointer data)
{
	GtkListStore *store = data;
	GtkWidget *theme_list;	
	GtkWidget *setup_dialog;
	GtkWidget *del_button;		
	GtkTreeSelection *selection;
    char *dir, *name;
	GtkTreeModel *model;
	GtkTreeIter iter;
	GValue value = {0, };
	GtkWidget *dlg;
	char *s;	

	setup_dialog = glade_xml_get_widget (xml, "setup_dialog");
	theme_list = glade_xml_get_widget (xml, "gg_theme_list");	
	del_button = glade_xml_get_widget (xml, "gg_delete_theme");	

	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (theme_list));	

	if ( ! gtk_tree_selection_get_selected (selection, &model, &iter)) {
		/* should never get here since the button shuld not be
		 * enabled */
		return;
	}

	gtk_tree_model_get_value (model, &iter,
							  THEME_COLUMN_SELECTED,
							  &value);
						
	/* Do not allow deleting of selected theme */
	if (g_value_get_boolean (&value)) {
		/* should never get here since the button shuld not be
		 * enabled */
		g_value_unset (&value);
		return;
	}
	g_value_unset (&value);

	gtk_tree_model_get_value (model, &iter,
				  THEME_COLUMN_NAME,
				  &value);
	name = g_strdup (g_value_get_string (&value));
	g_value_unset (&value);

	gtk_tree_model_get_value (model, &iter,
				  THEME_COLUMN_DIR,
				  &value);
	dir = g_strdup (g_value_get_string (&value));
	g_value_unset (&value);

	s = g_strdup_printf (_("Remove the \"%s\" theme?"),
			     name);
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
	
	gtk_dialog_set_default_response (GTK_DIALOG (dlg),
					 GTK_RESPONSE_YES);

	if (gtk_dialog_run (GTK_DIALOG (dlg)) == GTK_RESPONSE_YES) {
		char *theme_dir = get_theme_dir ();
		char *cwd = g_get_current_dir ();
		if (g_chdir (theme_dir) == 0 &&
		    /* this is a security sanity check, since we're doing rm -fR */
		    strchr (dir, '/') == NULL) {
			/* HACK! */
			DIR *dp;
			char *argv[4];
			GtkTreeIter *select_iter = NULL;
			argv[0] = "/bin/rm";
			argv[1] = "-fR";
			argv[2] = g_strconcat ("./", dir, NULL);
			argv[3] = NULL;
			simple_spawn_sync (argv);
			g_free (argv[2]);

			/* Update the list */
			gtk_list_store_clear (store);

			dp = opendir (theme_dir);

			if (dp != NULL) {
				select_iter = read_themes (store, theme_dir, dp, 
							   selected_theme);
				closedir (dp);
			}

			if (select_iter != NULL) {
				gtk_tree_selection_select_iter (selection, select_iter);
				g_free (select_iter);
			}

		}
		g_chdir (cwd);
		g_free (cwd);
		g_free (theme_dir);
	}
	gtk_widget_destroy (dlg);

	g_free (name);
	g_free (dir);
}

static void
delete_html_theme (GtkWidget *button, gpointer data)
{
	GtkListStore *store = data;
	GtkWidget *theme_list;	
	GtkWidget *setup_dialog;
	GtkWidget *del_button;		
	GtkTreeSelection *selection;
    char *dir, *name;
	GtkTreeModel *model;
	GtkTreeIter iter;
	GValue value = {0, };
	GtkWidget *dlg;
	char *s;	

	setup_dialog = glade_xml_get_widget (xml, "setup_dialog");
	theme_list = glade_xml_get_widget (xml, "gg_html_theme_list");	
	del_button = glade_xml_get_widget (xml, "gg_delete_html_theme");	

	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (theme_list));	

	if ( ! gtk_tree_selection_get_selected (selection, &model, &iter)) {
		/* should never get here since the button shuld not be
		 * enabled */
		return;
	}

	gtk_tree_model_get_value (model, &iter,
							  THEME_COLUMN_SELECTED,
							  &value);
						
	/* Do not allow deleting of selected theme */
	if (g_value_get_boolean (&value)) {
		/* should never get here since the button shuld not be
		 * enabled */
		g_value_unset (&value);
		return;
	}
	g_value_unset (&value);

	gtk_tree_model_get_value (model, &iter,
				  THEME_COLUMN_NAME,
				  &value);
	name = g_strdup (g_value_get_string (&value));
	g_value_unset (&value);

	gtk_tree_model_get_value (model, &iter,
				  THEME_COLUMN_DIR,
				  &value);
	dir = g_strdup (g_value_get_string (&value));
	g_value_unset (&value);

	s = g_strdup_printf (_("Remove the \"%s\" theme?"),
			     name);
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
	
	gtk_dialog_set_default_response (GTK_DIALOG (dlg),
					 GTK_RESPONSE_YES);

	if (gtk_dialog_run (GTK_DIALOG (dlg)) == GTK_RESPONSE_YES) {		
		char *cwd = g_get_current_dir ();
		if (g_chdir ("/usr/share/mdm/html-themes") == 0 &&
		    /* this is a security sanity check, since we're doing rm -fR */
		    strchr (dir, '/') == NULL) {
			/* HACK! */
			DIR *dp;
			char *argv[4];
			GtkTreeIter *select_iter = NULL;
			argv[0] = "/bin/rm";
			argv[1] = "-fR";
			argv[2] = g_strconcat ("./", dir, NULL);
			printf (argv[2]);
			argv[3] = NULL;
			simple_spawn_sync (argv);
			g_free (argv[2]);

			/* Update the list */
			gtk_list_store_clear (store);

			dp = opendir ("/usr/share/mdm/html-themes");

			if (dp != NULL) {
				select_iter = read_html_themes (store, "/usr/share/mdm/html-themes", dp, 
							   selected_html_theme);
				closedir (dp);
			}

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

static gboolean
xserver_entry_timeout (GtkWidget *entry)
{
	GtkWidget *mod_combobox;
	GSList *li;
	const char *key  = g_object_get_data (G_OBJECT (entry), "key");
	const char *text = gtk_entry_get_text (GTK_ENTRY (entry));
	gchar *string_old = NULL;
	gchar *section;

	mod_combobox    = glade_xml_get_widget (xml_xservers, "xserver_mod_combobox");

	/* Get xserver section to update */
	section = gtk_combo_box_get_active_text (GTK_COMBO_BOX (mod_combobox));

	for (li = xservers; li != NULL; li = li->next) {
		MdmXserver *svr = li->data;
		if (strcmp (ve_sure_string (svr->id), ve_sure_string (section)) == 0) {

			if (strcmp (ve_sure_string (key),
                            ve_sure_string (MDM_KEY_SERVER_NAME)) == 0)
				string_old = svr->name;
			else if (strcmp (ve_sure_string (key),
                                 ve_sure_string (MDM_KEY_SERVER_COMMAND)) == 0)
				string_old = svr->command;

			/* Update this servers configuration */
			if (strcmp (ve_sure_string (string_old),
                            ve_sure_string (text)) != 0) {
				if (strcmp (ve_sure_string (key),
                                    ve_sure_string (MDM_KEY_SERVER_NAME)) == 0) {
					if (svr->name)
						g_free (svr->name);
					svr->name = g_strdup (text);
				} else if (strcmp (ve_sure_string (key),
                                           ve_sure_string (MDM_KEY_SERVER_COMMAND)) == 0) {
					if (svr->command)
						g_free (svr->command);
					svr->command = g_strdup (text);;
				}
				update_xserver (section, svr);
			}
			break;
		}
	}
	g_free (section);

	return FALSE;
}

static gboolean
xserver_priority_timeout (GtkWidget *entry)
{
	GtkWidget *mod_combobox;
	GSList *li;
	const char *key  = g_object_get_data (G_OBJECT (entry), "key");
	gint value = 0;
	gchar *section;

	mod_combobox    = glade_xml_get_widget (xml_xservers, "xserver_mod_combobox");

	/* Get xserver section to update */
	section = gtk_combo_box_get_active_text (GTK_COMBO_BOX (mod_combobox));
	
	for (li = xservers; li != NULL; li = li->next) {
		MdmXserver *svr = li->data;
		if (strcmp (ve_sure_string (svr->id), ve_sure_string (section)) == 0) {
			gint new_value;
			
			if (strcmp (ve_sure_string (key),
				    ve_sure_string (MDM_KEY_SERVER_PRIORITY)) == 0)
				value = svr->priority;
			
			/* Update this servers configuration */
			new_value = gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (entry));
			if (new_value != value) {
				svr->priority = new_value;				
				update_xserver (section, svr);			
			}
			break;
		}
	}
	g_free (section);

	return FALSE;
}

static gboolean
xserver_toggle_timeout (GtkWidget *toggle)
{
	GtkWidget *mod_combobox;
	const char *key = g_object_get_data (G_OBJECT (toggle), "key");
	GSList     *li;
	gboolean   val = FALSE;
	gchar      *section;

	mod_combobox    = glade_xml_get_widget (xml_xservers, "xserver_mod_combobox");

	/* Get xserver section to update */
	section = gtk_combo_box_get_active_text (GTK_COMBO_BOX (mod_combobox));

	/* Locate this server's section */
	for (li = xservers; li != NULL; li = li->next) {
		MdmXserver *svr = li->data;
		if (strcmp (ve_sure_string (svr->id), ve_sure_string (section)) == 0) {

			if (strcmp (ve_sure_string (key),
                            ve_sure_string (MDM_KEY_SERVER_HANDLED)) == 0) {
				val = svr->handled;
			} else if (strcmp (ve_sure_string (key),
                                   ve_sure_string (MDM_KEY_SERVER_FLEXIBLE)) == 0) {
				val = svr->flexible;
			}

			/* Update this servers configuration */
			if ( ! bool_equal (val, GTK_TOGGLE_BUTTON (toggle)->active)) {
				gboolean new_val = GTK_TOGGLE_BUTTON (toggle)->active;

				if (strcmp (ve_sure_string (key),
                                    ve_sure_string (MDM_KEY_SERVER_HANDLED)) == 0)
					svr->handled = new_val;
				else if (strcmp (ve_sure_string (key),
                                         ve_sure_string (MDM_KEY_SERVER_FLEXIBLE)) == 0)
					svr->flexible = new_val;

				update_xserver (section, svr);
			}
			break;
		}
	}
	g_free (section);

	return FALSE;
}

static void
xserver_toggle_toggled (GtkWidget *toggle)
{
	run_timeout (toggle, 500, xserver_toggle_timeout);
}

static void
xserver_entry_changed (GtkWidget *entry)
{
	run_timeout (entry, 500, xserver_entry_timeout);
}

static void
xserver_priority_changed (GtkWidget *entry)
{
	run_timeout (entry, 500, xserver_priority_timeout);
}

static void
xserver_append_combobox (MdmXserver *xserver, GtkComboBox *combobox)
{
	gtk_combo_box_append_text (combobox, (xserver->id));
}

static void
xserver_populate_combobox (GtkComboBox* combobox)
{
	gint i,j;

	/* Get number of items in combobox */
	i = gtk_tree_model_iter_n_children(
	        gtk_combo_box_get_model (GTK_COMBO_BOX (combobox)), NULL);

	/* Delete all items from combobox */
	for (j = 0; j < i; j++) {
		gtk_combo_box_remove_text(combobox,0);
	}

	/* Populate combobox with list of current servers */
	g_slist_foreach (xservers, (GFunc) xserver_append_combobox, combobox);
}

static void
xserver_init_server_list ()
{
	/* Get Widgets from glade */
	GtkWidget *treeview = glade_xml_get_widget (xml_xservers, "xserver_tree_view");
	GtkWidget *remove_button = glade_xml_get_widget (xml_xservers, "xserver_remove_button");

	/* create list store */
	GtkListStore *store = gtk_list_store_new (XSERVER_NUM_COLUMNS,
	                            G_TYPE_INT    /* virtual terminal */,
	                            G_TYPE_STRING /* server type */,
	                            G_TYPE_STRING /* options */);

	/* Read all xservers to start from configuration */
	xservers_get_displays (store);
	gtk_tree_view_set_model (GTK_TREE_VIEW (treeview),
	                         GTK_TREE_MODEL (store));
	gtk_tree_view_set_rules_hint (GTK_TREE_VIEW (treeview), TRUE);
	gtk_widget_set_sensitive (remove_button, FALSE);
}

static void
xserver_init_servers ()
{
    GtkWidget *remove_button;

    /* Init widget states */
    xserver_init_server_list();

    remove_button = glade_xml_get_widget (xml_xservers, "xserver_remove_button");
    gtk_widget_set_sensitive (remove_button, FALSE);
}

static void
xserver_row_selected(GtkTreeSelection *selection, gpointer data)
{
    GtkWidget *remove_button;
    
    remove_button = glade_xml_get_widget (xml_xservers, "xserver_remove_button");
    gtk_widget_set_sensitive (remove_button, TRUE);
}

/*
 * Remove a server from the list of servers to start (not the same as
 * deleting a server definition)
 */
static void
xserver_remove_display (gpointer data)
{
	GtkWidget *treeview, *combo;
	GtkTreeSelection *selection;
	GtkTreeIter iter;
	GtkTreeModel *model;
	gint vt;
        char vt_value[3];

        treeview = glade_xml_get_widget (xml_xservers, "xserver_tree_view");

        selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (treeview));

	if (gtk_tree_selection_get_selected (selection, &model, &iter)) {
	        GKeyFile *cfg;
		GKeyFile *custom_cfg;
		gchar *defaultval;
		gchar *key;

		cfg = mdm_common_config_load (config_file, NULL);
		custom_cfg = mdm_common_config_load (custom_config_file, NULL);

		combo = glade_xml_get_widget (xml_add_xservers, "xserver_server_combobox");

		/* Update config */
		gtk_tree_model_get (model, &iter, XSERVER_COLUMN_VT, &vt, -1);

		g_snprintf (vt_value,  sizeof (vt_value), "%d", vt);
		key = g_strconcat (MDM_KEY_SECTION_SERVERS, "/", vt_value, "=", NULL);

		defaultval = NULL;
		mdm_common_config_get_string (cfg, key, &defaultval, NULL);

		/*
		 * If the value is in the default config file, set it to inactive in
		 * the custom config file, else delete it
		 */
		if (! ve_string_empty (defaultval)) {
			mdm_common_config_set_string (custom_cfg, key, "inactive");
		} else {
			mdm_common_config_remove_key (custom_cfg, key, NULL);
		}
		g_free (defaultval);

		mdm_common_config_save (custom_cfg, custom_config_file, NULL);
		g_key_file_free (custom_cfg);
		g_key_file_free (cfg);

		/* Update mdmsetup */
		xserver_init_server_list ();
		xserver_update_delete_sensitivity ();
	}
}

/* Add a display to the list of displays to start */
static void
xserver_add_display (gpointer data)
{
        GKeyFile *cfg;
        GKeyFile *custom_cfg;
	GtkWidget *spinner, *combo, *entry, *button;
	gchar *string;
	gchar *defaultval;
	char spinner_value[3], *key;

        cfg = mdm_common_config_load (config_file, NULL);
	custom_cfg = mdm_common_config_load (custom_config_file, NULL);

	/* Get Widgets from glade */
	spinner  = glade_xml_get_widget (xml_add_xservers, "xserver_spin_button");
	entry    = glade_xml_get_widget (xml_add_xservers, "xserver_options_entry");
	combo    = glade_xml_get_widget (xml_add_xservers, "xserver_server_combobox");
	button   = glade_xml_get_widget (xml_xservers, "xserver_add_button");

	/* String to add to config */
	g_snprintf (spinner_value,  sizeof (spinner_value), "%d",
	            gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (spinner)));

	key = g_strconcat (MDM_KEY_SECTION_SERVERS, "/", spinner_value, "=", NULL);
	if (! ve_string_empty (gtk_entry_get_text (GTK_ENTRY (entry)))) {
		string = g_strconcat (gtk_combo_box_get_active_text (GTK_COMBO_BOX (combo)),
		                      " ", gtk_entry_get_text (GTK_ENTRY (entry)),
		                      NULL);
	} else {
		string = g_strdup (gtk_combo_box_get_active_text (GTK_COMBO_BOX (combo)));
	}

	defaultval = NULL;
	mdm_common_config_get_string (cfg, key, &defaultval, NULL);

	/* Add to config */
	if (strcmp (ve_sure_string (defaultval), ve_sure_string (string)) == 0) {
		mdm_common_config_remove_key (custom_cfg, key, NULL);
	} else {
		mdm_common_config_set_string (custom_cfg, key, ve_sure_string(string));
	}

	mdm_common_config_save (custom_cfg, custom_config_file, NULL);
	g_key_file_free (custom_cfg);
	g_key_file_free (cfg);

	/* Reinitialize mdmsetup */
	xserver_init_servers ();
	xserver_update_delete_sensitivity ();

	/* Free memory */
	g_free (defaultval);
	g_free (string);
	g_free (key);
}

static void
xserver_add_button_clicked (void)
{
	static GtkWidget *dialog = NULL;
	GtkWidget *options_entry;
	GtkWidget *server_combobox;
	GtkWidget *vt_spinbutton;
	GtkWidget *parent;
	GtkWidget *treeview;
	GtkTreeSelection *selection;
	GtkTreeModel *treeview_model;
	GtkTreeIter treeview_iter;
	guint activate_signal_id;
	gboolean res;
		
	if (dialog == NULL) {
		parent = glade_xml_get_widget (xml_xservers, "xserver_dialog");
		dialog = glade_xml_get_widget (xml_add_xservers, "add_xserver_dialog");

		gtk_window_set_transient_for (GTK_WINDOW (dialog), GTK_WINDOW (parent));
		gtk_window_set_destroy_with_parent (GTK_WINDOW (dialog), TRUE);
	}

	vt_spinbutton = glade_xml_get_widget (xml_add_xservers, "xserver_spin_button");
	server_combobox = glade_xml_get_widget (xml_add_xservers, "xserver_server_combobox");
	options_entry = glade_xml_get_widget (xml_add_xservers, "xserver_options_entry");

	activate_signal_id = g_signal_connect (G_OBJECT (vt_spinbutton), "activate",
	                                       G_CALLBACK (vt_spinbutton_activate),
	                                       (gpointer) dialog);
	
	xserver_populate_combobox (GTK_COMBO_BOX (server_combobox));
	
	gtk_widget_grab_focus (vt_spinbutton);
		
	treeview = glade_xml_get_widget (xml_xservers, "xserver_tree_view");
	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (treeview));

	/* set default values */
	if (gtk_tree_selection_get_selected (selection, &treeview_model, &treeview_iter)) {

		GtkTreeModel *combobox_model;
		GtkTreeIter combobox_iter;
		gchar *label;
		gchar *server;
		gint vt;

		gtk_tree_model_get (treeview_model, &treeview_iter, XSERVER_COLUMN_VT, &vt, -1);	
		gtk_spin_button_set_value (GTK_SPIN_BUTTON (vt_spinbutton), vt);

		gtk_tree_model_get (GTK_TREE_MODEL (treeview_model), &treeview_iter,
				    XSERVER_COLUMN_SERVER, &server, -1);
		combobox_model = gtk_combo_box_get_model (GTK_COMBO_BOX (server_combobox));

		for (res = gtk_tree_model_get_iter_first (combobox_model, &combobox_iter); res; res = gtk_tree_model_iter_next (combobox_model, &combobox_iter)) {
	      		gtk_tree_model_get (combobox_model, &combobox_iter, 0, &label, -1);
	      		if (strcmp (ve_sure_string (label), ve_sure_string (server)) == 0) {
				gtk_combo_box_set_active_iter (GTK_COMBO_BOX (server_combobox), &combobox_iter);
      			}
      			g_free (label);
    		}

		gtk_tree_model_get (GTK_TREE_MODEL (treeview_model), &treeview_iter,
				    XSERVER_COLUMN_OPTIONS, &server, -1);
		if (server != NULL)
			gtk_entry_set_text (GTK_ENTRY (options_entry), server);
	} else {
		gint high_value = 0;
		gint vt;

		for (res = gtk_tree_model_get_iter_first (treeview_model, &treeview_iter); res; res = gtk_tree_model_iter_next (treeview_model, &treeview_iter)) {
	      		gtk_tree_model_get (treeview_model, &treeview_iter, XSERVER_COLUMN_VT, &vt, -1);
	      		if (high_value < vt) {
				high_value = vt;
      			}
    		}
		
		gtk_spin_button_set_value (GTK_SPIN_BUTTON (vt_spinbutton), ++high_value);
		gtk_combo_box_set_active (GTK_COMBO_BOX (server_combobox), 0);
		gtk_entry_set_text (GTK_ENTRY (options_entry), "");
	}
	
	if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_OK) {
		xserver_add_display (NULL);
	}
	g_signal_handler_disconnect (vt_spinbutton, activate_signal_id);
	gtk_widget_hide (dialog);
}

/*
 * TODO: This section needs a little work until it is ready (mainly config
 * section modifications) 
 * Create a server definition (not the same as removing a server
 * from the list of servers to start)
 */
#ifdef MDM_TODO_CODE
static void
xserver_create (gpointer data)
{
	/* VeConfig *cfg; */
	gboolean success;

	/* Init Widgets */
	GtkWidget *frame, *modify_combobox;
	GtkWidget *name_entry, *command_entry;
	GtkWidget *handled_check, *flexible_check;
	GtkWidget *greeter_radio, *chooser_radio;
	GtkWidget *create_button, *delete_button;
	GtkWidget *priority_spinbutton;

	/* Get Widgets from glade */
	frame           = glade_xml_get_widget (xml, "xserver_modify_frame");
	name_entry      = glade_xml_get_widget (xml, "xserver_name_entry");
	command_entry   = glade_xml_get_widget (xml, "xserver_command_entry");
	priority_spinbutton = glade_xml_get_widget(xml, "xserv_priority_spinbutton");
	handled_check   = glade_xml_get_widget (xml, "xserver_handled_checkbutton");
	flexible_check  = glade_xml_get_widget (xml, "xserver_flexible_checkbutton");
	greeter_radio   = glade_xml_get_widget (xml, "xserver_greeter_radiobutton");
	chooser_radio   = glade_xml_get_widget (xml, "xserver_chooser_radiobutton");
	modify_combobox = glade_xml_get_widget (xml, "xserver_mod_combobox");
	create_button   = glade_xml_get_widget (xml, "xserver_create_button");
	delete_button   = glade_xml_get_widget (xml, "xserver_delete_button");

	gtk_combo_box_append_text (GTK_COMBO_BOX (modify_combobox),
	                           "New Server");

	/* TODO: Create a new section for this server */
	/* TODO: Write this value to the config and update xservers list */
	/* cfg = mdm_common_config_load (custom_config_file, NULL); */
	success = FALSE;
	/* success = ve_config_add_section (cfg, SECTION_NAME); */

	if (success)
	{
		gint i;

		/* Update settings for new server */
		gtk_widget_set_sensitive (frame, TRUE);
		gtk_widget_set_sensitive (delete_button, TRUE);
		gtk_widget_grab_focus (name_entry);
		gtk_entry_set_text (GTK_ENTRY (name_entry), "New Server");
		gtk_editable_select_region (GTK_EDITABLE (name_entry), 0, -1);
		gtk_entry_set_text (GTK_ENTRY (command_entry), X_SERVER);
		gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (greeter_radio),
		                              TRUE);
		gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (chooser_radio),
		                              FALSE);
		gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (handled_check),
		                              TRUE);
		gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (flexible_check),
		                              FALSE);

		/* Select the new server in the combobox */
		i = gtk_tree_model_iter_n_children (
		   gtk_combo_box_get_model (GTK_COMBO_BOX (modify_combobox)), NULL) - 1;
		gtk_combo_box_set_active (GTK_COMBO_BOX (modify_combobox), i);
	}
}
#endif

static void
xserver_init_definitions ()
{
	GtkWidget *style_combobox;
	GtkWidget *modify_combobox;

	style_combobox  = glade_xml_get_widget (xml_xservers, "xserver_style_combobox");
	modify_combobox = glade_xml_get_widget (xml_xservers, "xserver_mod_combobox");

	xserver_populate_combobox (GTK_COMBO_BOX (modify_combobox));

	gtk_combo_box_set_active (GTK_COMBO_BOX (style_combobox), 0);	
	init_servers_combobox (gtk_combo_box_get_active (GTK_COMBO_BOX (style_combobox)));
}

/*
 * Deletes a server definition (not the same as removing a server
 * from the list of servers to start)
 *
 * NOTE, now that we have the %{datadir}/mdm/defaults.conf and
 * %{etc}/mdm/custom.conf files, this will need to work like the displays.
 * So if you want to delete something that is defaults.conf you will need
 * to write a new value to custom.conf section for this xserver like
 * "inactive=true".  For this to work, daemon/mdmconfig.c will also need
 * to be modified so that it doesn't bother loading xservers that are
 * marked as inactive in the custom.conf file.  As I said, this
 * is the same way the displays already work so the code should be
 * similar.  Or perhaps it makes more sense to just not allow
 * deleting of server-foo sections as defined in the defaults.conf
 * file.  If the user doesn't want to use them, they can always
 * create new server-foo sections in custom.conf and define their
 * displays to only use the ones they define. 
 */
#ifdef MDM_UNUSED_CODE
static void
xserver_delete (gpointer data)
{
	gchar temp_string;
	/* Get xserver section to delete */
	GtkWidget *combobox;
	gchar *section;
	/* Delete xserver section */
	VeConfig *custom_cfg;

	combobox = glade_xml_get_widget (xml_xservers, "xserver_mod_combobox");
	section = gtk_combo_box_get_active_text ( GTK_COMBO_BOX (combobox));
	custom_cfg = ve_config_get (custom_config_file);

	temp_string = g_strconcat (MDM_KEY_SERVER_PREFIX, section, NULL);
	ve_config_delete_section (custom_cfg, temp_string);
	g_free (temp_string);

	/* Reinitialize definitions */
	xserver_init_definitions();
}
#endif

static void
setup_xserver_support (GladeXML *xml_xservers)
{
	GtkWidget *command_entry;
	GtkWidget *priority_spinbutton;
	GtkWidget *name_entry;
	GtkWidget *handled_check;
	GtkWidget *flexible_check;
	GtkWidget *create_button;
	GtkWidget *delete_button;
	GtkWidget *remove_button;
	GtkWidget *servers_combobox;
	GtkWidget *style_combobox;
	GtkWidget *treeview;
	GtkCellRenderer *renderer;
	GtkTreeSelection *selection;
	GtkTreeViewColumn *column;

	/* Initialize the xserver settings */
	xserver_init_definitions();
	xserver_init_servers();

	/* TODO: In the future, resolution/refresh rate configuration */
	/* setup_xrandr_support (); */

	/* Get Widgets from glade */
	treeview        = glade_xml_get_widget (xml_xservers, "xserver_tree_view");
	name_entry      = glade_xml_get_widget (xml_xservers, "xserver_name_entry");
	command_entry   = glade_xml_get_widget (xml_xservers, "xserver_command_entry");
	priority_spinbutton = glade_xml_get_widget(xml_xservers, "xserv_priority_spinbutton");
	handled_check   = glade_xml_get_widget (xml_xservers, "xserver_handled_checkbutton");
	flexible_check  = glade_xml_get_widget (xml_xservers, "xserver_flexible_checkbutton");
	style_combobox  = glade_xml_get_widget (xml_xservers, "xserver_style_combobox");
	servers_combobox = glade_xml_get_widget (xml_xservers, "xserver_mod_combobox");
	create_button   = glade_xml_get_widget (xml_xservers, "xserver_createbutton");
	delete_button   = glade_xml_get_widget (xml_xservers, "xserver_deletebutton");
	remove_button   = glade_xml_get_widget (xml_xservers, "xserver_remove_button");

	glade_helper_tagify_label (xml_xservers, "xserver_informationlabel", "i");
	glade_helper_tagify_label (xml_xservers, "xserver_informationlabel", "small");
	glade_helper_tagify_label (xml_xservers, "server_to_start_label", "b");
	glade_helper_tagify_label (xml_xservers, "server_settings_label", "b");
	
	/* Setup Virtual terminal column in servers to start frame */
	column = gtk_tree_view_column_new ();
	renderer = gtk_cell_renderer_text_new ();
	gtk_tree_view_column_pack_start (column, renderer, TRUE);
	gtk_tree_view_column_set_title (column, "VT");
	gtk_tree_view_column_set_attributes (column, renderer,
	                                    "text", XSERVER_COLUMN_VT,
	                                     NULL);
	gtk_tree_view_append_column (GTK_TREE_VIEW (treeview), column);

	/* Setup Server column in servers to start frame */
	column = gtk_tree_view_column_new ();
	renderer = gtk_cell_renderer_text_new ();
	gtk_tree_view_column_pack_start (column, renderer, TRUE);
	gtk_tree_view_column_set_title (column, "Server");
	gtk_tree_view_column_set_attributes (column, renderer,
	                                     "text", XSERVER_COLUMN_SERVER,
	                                     NULL);
	gtk_tree_view_append_column (GTK_TREE_VIEW (treeview), column);

	/* Setup Options column in servers to start frame*/
	column   = gtk_tree_view_column_new ();
	renderer = gtk_cell_renderer_text_new ();
	gtk_tree_view_column_pack_start (column, renderer, TRUE);
	gtk_tree_view_column_set_title (column, "Options");
	gtk_tree_view_column_set_attributes (column, renderer,
	                                     "text", XSERVER_COLUMN_OPTIONS,
	                                     NULL);
	gtk_tree_view_append_column (GTK_TREE_VIEW (treeview), column);

	/* Setup tree selections */
	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (treeview));
	gtk_tree_selection_set_mode (selection, GTK_SELECTION_SINGLE);

	/* Register these items with keys */
	g_object_set_data_full (G_OBJECT (servers_combobox), "key",
	                        g_strdup (MDM_KEY_SERVER_PREFIX),
	                        (GDestroyNotify) g_free);
	g_object_set_data_full (G_OBJECT (name_entry), "key",
	                        g_strdup (MDM_KEY_SERVER_NAME),
	                        (GDestroyNotify) g_free);
	g_object_set_data_full (G_OBJECT (command_entry), "key",
	                        g_strdup (MDM_KEY_SERVER_COMMAND),
	                        (GDestroyNotify) g_free);
	g_object_set_data_full (G_OBJECT (handled_check), "key",
	                        g_strdup (MDM_KEY_SERVER_HANDLED),
	                        (GDestroyNotify) g_free);
	g_object_set_data_full (G_OBJECT (flexible_check), "key",
	                        g_strdup (MDM_KEY_SERVER_FLEXIBLE),
	                        (GDestroyNotify) g_free);
	g_object_set_data_full (G_OBJECT (style_combobox), "key",
	                        g_strdup (MDM_KEY_SERVER_CHOOSER),
	                        (GDestroyNotify) g_free);
	g_object_set_data_full (G_OBJECT (priority_spinbutton), "key",
				g_strdup (MDM_KEY_SERVER_PRIORITY),
	                        (GDestroyNotify) g_free);
	/* Signals Handlers */
    	g_signal_connect (G_OBJECT (name_entry), "changed",
	                  G_CALLBACK (xserver_entry_changed),NULL);
    	g_signal_connect (G_OBJECT (command_entry), "changed",
	                  G_CALLBACK (xserver_entry_changed), NULL);
	g_signal_connect (G_OBJECT (handled_check), "toggled",
	                  G_CALLBACK (xserver_toggle_toggled), NULL);
	g_signal_connect (G_OBJECT (flexible_check), "toggled",
	                  G_CALLBACK (xserver_toggle_toggled), NULL);
	g_signal_connect (G_OBJECT (servers_combobox), "changed",
	                  G_CALLBACK (combobox_changed), NULL);
	g_signal_connect (G_OBJECT (style_combobox), "changed",
	                  G_CALLBACK (combobox_changed), NULL);
	g_signal_connect (G_OBJECT (remove_button), "clicked",
	                  G_CALLBACK (xserver_remove_display), NULL);
	g_signal_connect (G_OBJECT (selection), "changed",
	                  G_CALLBACK (xserver_row_selected), NULL);
	g_signal_connect (G_OBJECT (priority_spinbutton), "value_changed",
	                  G_CALLBACK (xserver_priority_changed), NULL);
	
	/* TODO: In the future, allow creation & delection of servers
	g_signal_connect (create_button, "clicked",
			  G_CALLBACK (xserver_create), NULL);
  	g_signal_connect (delete_button, "clicked",
	                  G_CALLBACK (xserver_delete), NULL);
	*/
}

static void
xserver_button_clicked (void)
{
	static GtkWidget *dialog = NULL;
	int response;

	if (dialog == NULL) {

		GtkWidget *parent;
		GtkWidget *button;
	
		xml_xservers = glade_xml_new (MDM_GLADE_DIR "/mdmsetup.glade", "xserver_dialog", NULL);

		xml_add_xservers = glade_xml_new (MDM_GLADE_DIR "/mdmsetup.glade", "add_xserver_dialog", NULL);

		parent = glade_xml_get_widget (xml, "setup_dialog");
		dialog = glade_xml_get_widget (xml_xservers, "xserver_dialog");
		button = glade_xml_get_widget (xml_xservers, "xserver_add_button");

		gtk_window_set_transient_for (GTK_WINDOW (dialog), GTK_WINDOW (parent));
		gtk_window_set_destroy_with_parent (GTK_WINDOW (dialog), TRUE);

		g_signal_connect (G_OBJECT (button), "clicked",
		                  G_CALLBACK (xserver_add_button_clicked), NULL);

		setup_xserver_support (xml_xservers);
	}

	do {
		response = gtk_dialog_run (GTK_DIALOG (dialog));
		if (response == GTK_RESPONSE_HELP) {
			g_spawn_command_line_sync ("gnome-open ghelp:mdm", NULL, NULL,
							NULL, NULL);
		}
	} while (response != GTK_RESPONSE_CLOSE &&
                 response != GTK_RESPONSE_DELETE_EVENT);

	gtk_widget_hide (dialog);
}

static void
setup_radio_group (const gchar *name,
		   const gchar *key, gint position)
{
	GtkWidget *radio;
	gint val;
	
	radio = glade_xml_get_widget (xml, name);
	val   = mdm_config_get_int ((gchar *)key);
	
	if (val == position)
		gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (radio), TRUE);
	else
		gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (radio), FALSE);
	
	g_object_set_data_full (G_OBJECT (radio), "key", g_strdup (key),
	                        (GDestroyNotify) g_free);
			
    	g_signal_connect (G_OBJECT (radio), "toggled",
			  G_CALLBACK (radiogroup_toggled), NULL);	
}

static void
setup_security_tab (void)
{
	GtkWidget *checkbox;
	GtkWidget *label;

	/* Setup Local administrator login setttings */
	setup_notify_toggle ("allowroot", MDM_KEY_ALLOW_ROOT);

	/* Setup Enable debug message to system log */
	setup_notify_toggle ("enable_debug", MDM_KEY_DEBUG);

	/* Setup Deny TCP connections to Xserver */
	setup_notify_toggle ("disallow_tcp", MDM_KEY_DISALLOW_TCP);

	/* Setup never place cookies on NFS */
	setup_notify_toggle ("never_cookies_NFS_checkbutton", MDM_KEY_NEVER_PLACE_COOKIES_ON_NFS);

	/* Setup Retry delay */
	setup_intspin ("retry_delay", MDM_KEY_RETRY_DELAY);

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

	/* Setup check dir owner */
	setup_notify_toggle ("check_dir_owner_checkbutton", MDM_KEY_CHECK_DIR_OWNER);	
	
	/* Setup Relax permissions */
	setup_radio_group ("relax_permissions0_radiobutton", MDM_KEY_RELAX_PERM, 0);
	setup_radio_group ("relax_permissions1_radiobutton", MDM_KEY_RELAX_PERM, 1);
	setup_radio_group ("relax_permissions2_radiobutton", MDM_KEY_RELAX_PERM, 2);
	
	/* Setup MinimalUID */
	setup_intspin ("minimal_uid_spinbutton", MDM_KEY_MINIMAL_UID);
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
theme_list_drag_data_received  (GtkWidget        *widget,
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
	theme_list = glade_xml_get_widget (xml, "gg_theme_list");
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
		g_free (mesg);

		if (response == GTK_RESPONSE_OK) {
			install_theme_file (list->data, store, GTK_WINDOW (parent));
		}
	}
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
		g_free (mesg);

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
setup_local_themed_settings (void)
{	
	DIR *dir;
	GtkListStore *store;
	GtkCellRenderer *renderer;
	GtkTreeViewColumn *column;
	GtkTreeSelection *selection;
	GtkTreeIter *select_iter = NULL;
	GtkWidget *color_colorbutton;
	GtkWidget *style_label;
	GtkWidget *theme_label;
	GtkSizeGroup *size_group;
	char *theme_dir;
	
	GtkWidget *theme_list = glade_xml_get_widget (xml, "gg_theme_list");
	GtkWidget *button = glade_xml_get_widget (xml, "gg_install_new_theme");
	GtkWidget *del_button = glade_xml_get_widget (xml, "gg_delete_theme");	

	style_label = glade_xml_get_widget (xml, "local_stylelabel");
	theme_label = glade_xml_get_widget (xml, "local_theme_label");
	size_group = gtk_size_group_new (GTK_SIZE_GROUP_HORIZONTAL);
	gtk_size_group_add_widget (size_group, style_label);
	gtk_size_group_add_widget (size_group, theme_label);
	
	color_colorbutton = glade_xml_get_widget (xml, "local_background_theme_colorbutton");

	g_object_set_data (G_OBJECT (color_colorbutton), "key",
	                   MDM_KEY_GRAPHICAL_THEMED_COLOR);

	setup_greeter_color ("local_background_theme_colorbutton", 
	                     MDM_KEY_GRAPHICAL_THEMED_COLOR);

	theme_dir = get_theme_dir ();

	gtk_tree_view_set_rules_hint (GTK_TREE_VIEW (theme_list), TRUE);

	selected_theme  = mdm_config_get_string (MDM_KEY_GRAPHICAL_THEME);	

	/* FIXME: If a theme directory contains the string MDM_DELIMITER_THEMES
		  in the name, then this theme won't work when trying to load as it
		  will be perceived as two different themes seperated by
		  MDM_DELIMITER_THEMES.  This can be fixed by setting up an escape
		  character for it, but I'm not sure if directories can have the
		  slash (/) character in them, so I just made MDM_DELIMITER_THEMES
		  equal to "/:" instead. */
	
	/* create list store */
	store = gtk_list_store_new (THEME_NUM_COLUMNS,
				    G_TYPE_BOOLEAN /* selected theme */,				  
				    G_TYPE_STRING /* dir */,
				    G_TYPE_STRING /* file */,
				    GDK_TYPE_PIXBUF /* preview */,
				    G_TYPE_STRING /* markup */,
				    G_TYPE_STRING /* name */,
				    G_TYPE_STRING /* desc */);
		
	g_signal_connect (button, "clicked",
			  G_CALLBACK (install_new_theme), store);
	g_signal_connect (del_button, "clicked",
			  G_CALLBACK (delete_theme), store);

	/* Init controls */
	gtk_widget_set_sensitive (del_button, FALSE);	

	/* Read all Themes from directory and store in tree */
	dir = opendir (theme_dir);
	if (dir != NULL) {
		select_iter = read_themes (store, theme_dir, dir,
					   selected_theme);
		closedir (dir);
	}
	g_free (theme_dir);
	gtk_tree_view_set_model (GTK_TREE_VIEW (theme_list), 
				 GTK_TREE_MODEL (store));

	/* The radio toggle column */
	column = gtk_tree_view_column_new ();
	renderer = gtk_cell_renderer_toggle_new ();
	gtk_cell_renderer_toggle_set_radio (GTK_CELL_RENDERER_TOGGLE (renderer),
					    TRUE);
	g_signal_connect (G_OBJECT (renderer), "toggled",
			  G_CALLBACK (selected_toggled), store);
	gtk_tree_view_column_pack_start (column, renderer, FALSE);
	gtk_tree_view_column_set_attributes (column, renderer,
		"active", THEME_COLUMN_SELECTED, NULL);
	gtk_tree_view_append_column (GTK_TREE_VIEW (theme_list), column);
	gtk_tree_view_column_set_visible(column, TRUE);
	
	/* The preview column */
	column   = gtk_tree_view_column_new ();
	renderer = gtk_cell_renderer_pixbuf_new ();
	gtk_tree_view_column_pack_start (column, renderer, FALSE);
        gtk_tree_view_column_set_attributes (column, renderer,
                                             "pixbuf", THEME_COLUMN_SCREENSHOT,
                                             NULL);
	/* The markup column */
	renderer = gtk_cell_renderer_text_new ();
	gtk_tree_view_column_pack_start (column, renderer, FALSE);
	gtk_tree_view_column_set_attributes (column, renderer,
		"markup", THEME_COLUMN_MARKUP, NULL);
	gtk_tree_view_column_set_spacing (column, 6);
	gtk_tree_view_append_column (GTK_TREE_VIEW (theme_list), column);

	gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (store),
	                                      THEME_COLUMN_MARKUP, GTK_SORT_ASCENDING);

	gtk_tree_view_set_search_equal_func (GTK_TREE_VIEW (theme_list),
	                                     theme_list_equal_func, NULL, NULL);

	/* Selection setup */
	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (theme_list));
	gtk_tree_selection_set_mode (selection, GTK_SELECTION_SINGLE);
	g_signal_connect (selection, "changed",
		G_CALLBACK (gg_selection_changed), NULL);

	gtk_drag_dest_set (theme_list,
			   GTK_DEST_DEFAULT_ALL,
			   target_table, n_targets,
			   GDK_ACTION_COPY);
			   
	g_signal_connect (theme_list, "drag_data_received",
		G_CALLBACK (theme_list_drag_data_received), NULL);

	if (select_iter != NULL) {
		gtk_tree_selection_select_iter (selection, select_iter);
		g_free (select_iter);
	}
}

static void
setup_local_html_themed_settings (void)
{		
	DIR *dir;
	GtkListStore *store;
	GtkCellRenderer *renderer;
	GtkTreeViewColumn *column;
	GtkTreeSelection *selection;
	GtkTreeIter *select_iter = NULL;	
			
	GtkWidget *theme_list = glade_xml_get_widget (xml, "gg_html_theme_list");
	GtkWidget *button = glade_xml_get_widget (xml, "gg_install_new_html_theme");
	GtkWidget *del_button = glade_xml_get_widget (xml, "gg_delete_html_theme");				
	
	gtk_tree_view_set_rules_hint (GTK_TREE_VIEW (theme_list), TRUE);

	selected_html_theme  = mdm_config_get_string (MDM_KEY_HTML_THEME);	
		
	/* create list store */
	store = gtk_list_store_new (THEME_NUM_COLUMNS,
				    G_TYPE_BOOLEAN /* selected theme */,				  
				    G_TYPE_STRING /* dir */,
				    G_TYPE_STRING /* file */,
				    GDK_TYPE_PIXBUF /* preview */,
				    G_TYPE_STRING /* markup */,
				    G_TYPE_STRING /* name */,
				    G_TYPE_STRING /* desc */);
		
	g_signal_connect (button, "clicked",
			  G_CALLBACK (install_new_html_theme), store);
	g_signal_connect (del_button, "clicked",
			  G_CALLBACK (delete_html_theme), store);

	/* Init controls */
	gtk_widget_set_sensitive (del_button, FALSE);	

	/* Read all Themes from directory and store in tree */
	dir = opendir ("/usr/share/mdm/html-themes");
	if (dir != NULL) {		
		select_iter = read_html_themes (store, "/usr/share/mdm/html-themes", dir,
					   selected_html_theme);
		closedir (dir);
	}	
	gtk_tree_view_set_model (GTK_TREE_VIEW (theme_list), 
				 GTK_TREE_MODEL (store));

	/* The radio toggle column */
	column = gtk_tree_view_column_new ();
	renderer = gtk_cell_renderer_toggle_new ();
	gtk_cell_renderer_toggle_set_radio (GTK_CELL_RENDERER_TOGGLE (renderer),
					    TRUE);
	g_signal_connect (G_OBJECT (renderer), "toggled",
			  G_CALLBACK (selected_html_toggled), store);
	gtk_tree_view_column_pack_start (column, renderer, FALSE);
	gtk_tree_view_column_set_attributes (column, renderer,
		"active", THEME_COLUMN_SELECTED, NULL);
	gtk_tree_view_append_column (GTK_TREE_VIEW (theme_list), column);
	gtk_tree_view_column_set_visible(column, TRUE);
	
	/* The preview column */
	column   = gtk_tree_view_column_new ();
	renderer = gtk_cell_renderer_pixbuf_new ();
	gtk_tree_view_column_pack_start (column, renderer, FALSE);
        gtk_tree_view_column_set_attributes (column, renderer,
                                             "pixbuf", THEME_COLUMN_SCREENSHOT,
                                             NULL);
	/* The markup column */
	renderer = gtk_cell_renderer_text_new ();
	gtk_tree_view_column_pack_start (column, renderer, FALSE);
	gtk_tree_view_column_set_attributes (column, renderer,
		"markup", THEME_COLUMN_MARKUP, NULL);
	gtk_tree_view_column_set_spacing (column, 6);
	gtk_tree_view_append_column (GTK_TREE_VIEW (theme_list), column);

	gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (store),
	                                      THEME_COLUMN_MARKUP, GTK_SORT_ASCENDING);

	gtk_tree_view_set_search_equal_func (GTK_TREE_VIEW (theme_list),
	                                     theme_list_equal_func, NULL, NULL);

	/* Selection setup */
	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (theme_list));
	gtk_tree_selection_set_mode (selection, GTK_SELECTION_SINGLE);
	g_signal_connect (selection, "changed",
		G_CALLBACK (gg_html_selection_changed), NULL);

	gtk_drag_dest_set (theme_list,
			   GTK_DEST_DEFAULT_ALL,
			   target_table, n_targets,
			   GDK_ACTION_COPY);
			   
	g_signal_connect (theme_list, "drag_data_received",
		G_CALLBACK (html_theme_list_drag_data_received), NULL);

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
dialog_response (GtkWidget *dlg, int response, gpointer data)
{
	if (response == GTK_RESPONSE_CLOSE) {
		timeout_remove_all ();
		gtk_main_quit ();
	} else if (response == GTK_RESPONSE_HELP) {
		GtkWidget *setup_dialog = glade_xml_get_widget (xml, "setup_dialog");
		static GtkWidget *dlg = NULL;

		if (dlg != NULL) {
			gtk_window_present (GTK_WINDOW (dlg));
			return;
		}

		if ( ! RUNNING_UNDER_MDM) {
			gint exit_status;
			if (g_spawn_command_line_sync ("gnome-open ghelp:mdm", NULL, NULL,
							&exit_status, NULL) && exit_status == 0)
				return;
		}

		/* fallback help dialogue */
	
		/* HIG compliance? */
		dlg = gtk_message_dialog_new
			(GTK_WINDOW (setup_dialog),
			 GTK_DIALOG_DESTROY_WITH_PARENT,
			 GTK_MESSAGE_INFO,
			 GTK_BUTTONS_OK,
			 /* This is the temporary help dialog */
			 _("This configuration window changes settings "
			   "for the MDM daemon, which is the graphical "
			   "login screen for GNOME.  Changes that you make "
			   "will take effect immediately.\n\n"
			   "Note that not all configuration options "
			   "are listed here.  You may want to edit %s "
			   "if you cannot find what you are looking for.\n\n"
			   "For complete documentation see the GNOME help browser "
			   "under the \"Desktop\" category."),
			 custom_config_file);
		gtk_dialog_set_has_separator (GTK_DIALOG (dlg), FALSE);
		g_signal_connect (G_OBJECT (dlg), "destroy",
				  G_CALLBACK (gtk_widget_destroyed),
				  &dlg);
		g_signal_connect_swapped (G_OBJECT (dlg), "response",
					  G_CALLBACK (gtk_widget_destroy),
					  dlg);
		gtk_widget_show (dlg);
	}
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

			GdkPixbufFormat *info = NULL;
			gint width;
			gint height;

			info = gdk_pixbuf_get_file_info (file, &width, &height);
			
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
setup_plain_menubar (void)
{
	/* Initialize and hookup callbacks for plain menu bar settings */
	setup_notify_toggle ("sysmenu", MDM_KEY_SYSTEM_MENU);
	setup_notify_toggle ("config_available", MDM_KEY_CONFIG_AVAILABLE);
	setup_notify_toggle ("chooser_button", MDM_KEY_CHOOSER_BUTTON);
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
	/* Style setting */
	setup_greeter_combobox ("local_greeter",
	                        MDM_KEY_GREETER);
	
	/* Plain background settings */
	hookup_plain_background ();
		
	/* Plain menu bar settings */
	setup_plain_menubar ();
	
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
		
		if (!ve_string_empty (session->clearname)) {
			gtk_combo_box_append_text (GTK_COMBO_BOX (default_session_combobox), 
						   session->clearname);
			sessions = g_list_prepend (sessions, file);
		}
		/* This is a sort of safety fallback
		   if session does not have the clearname defined
		   we will use name instead*/		
		else if (!ve_string_empty (session->name)) {
			gtk_combo_box_append_text (GTK_COMBO_BOX (default_session_combobox), 
						   session->name);
			sessions = g_list_prepend (sessions, file);
		}
		
	}

	sessions = g_list_reverse (sessions);	

	/* some cleanup */	
	g_list_free (org_sessions);
	g_hash_table_remove_all (sessnames);
	
	if (!ve_string_empty (org_val)) {
		gtk_widget_set_sensitive (default_session_combobox, TRUE);		
		gtk_combo_box_set_active (GTK_COMBO_BOX (default_session_combobox), active);
	}
	else
		gtk_widget_set_sensitive (default_session_combobox, FALSE);
	
	g_object_set_data_full (G_OBJECT (default_session_combobox), "key",
	                        g_strdup (MDM_KEY_DEFAULT_SESSION),
				(GDestroyNotify) g_free);
	
	g_signal_connect (default_session_combobox, "changed",
		          G_CALLBACK (combobox_changed), NULL);
	
	default_session_checkbox = glade_xml_get_widget (xml, "default_session_checkbutton");

	gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (default_session_checkbox), !ve_string_empty (org_val));
	
	g_object_set_data_full (G_OBJECT (default_session_checkbox),
				"key", g_strdup (MDM_KEY_DEFAULT_SESSION),
				(GDestroyNotify) g_free);       
	
	g_signal_connect (G_OBJECT (default_session_checkbox), "toggled",
			  G_CALLBACK (toggle_toggled), default_session_checkbox);
	g_signal_connect (G_OBJECT (default_session_checkbox), "toggled",
			  G_CALLBACK (toggle_toggled_sensitivity_positive), default_session_combobox);
	
	g_free (org_val);
	
}

static void
setup_general_tab (void)
{
	GtkWidget *gtkrc_filechooser;
	GtkWidget *gtkrc_checkbox;
	GtkWidget *clock_type_chooser;	
	GtkWidget *commands_button;
	gchar *gtkrc_filename;
	gchar *user_24hr_clock;

	
	/* Setup use visual feedback in the passwotrd entry */
	setup_notify_toggle ("hide_vis_feedback_passwd_checkbox", MDM_KEY_ENTRY_INVISIBLE);

	/* Setup always login current session entry */
	setup_notify_toggle ("a_login_curr_session_checkbutton", MDM_KEY_ALWAYS_LOGIN_CURRENT_SESSION);

	/* Setup default session */
	setup_default_session ();
	
	/* Setup GtkRC file path */
	gtkrc_filechooser = glade_xml_get_widget (xml, "gtkrc_chooserbutton");

	gtkrc_filename = mdm_config_get_string (MDM_KEY_GTKRC);

	gtkrc_checkbox = glade_xml_get_widget (xml, "gtkrc_checkbutton");

	if (!ve_string_empty (gtkrc_filename) && access (gtkrc_filename, R_OK) == 0) {
		gtk_file_chooser_set_filename (GTK_FILE_CHOOSER (gtkrc_filechooser),
					       gtkrc_filename);
		
		gtk_widget_set_sensitive (gtkrc_filechooser, TRUE);
		gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (gtkrc_checkbox), TRUE);
		
	}
	else {
		gtk_widget_set_sensitive (gtkrc_filechooser, FALSE);
		gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (gtkrc_checkbox), FALSE);	
	}
		
	g_object_set_data_full (G_OBJECT (gtkrc_filechooser),
				"key", g_strdup (MDM_KEY_GTKRC),
				(GDestroyNotify) g_free);

	g_signal_connect (G_OBJECT (gtkrc_filechooser), "selection-changed",
			  G_CALLBACK (default_filechooser_response),
			  NULL);
	
	
	g_object_set_data_full (G_OBJECT (gtkrc_checkbox),
				"key", g_strdup (MDM_KEY_GTKRC),
				(GDestroyNotify) g_free);       
	
	g_signal_connect (G_OBJECT (gtkrc_checkbox), "toggled",
			  G_CALLBACK (toggle_toggled), gtkrc_checkbox);	
	g_signal_connect (G_OBJECT (gtkrc_checkbox), "toggled",
			  G_CALLBACK (toggle_toggled_sensitivity_positive), gtkrc_filechooser);
	
	g_free (gtkrc_filename);

	/* Setup user 24Hr Clock */
	clock_type_chooser = glade_xml_get_widget (xml, "use_24hr_clock_combobox");

	user_24hr_clock = mdm_config_get_string (MDM_KEY_USE_24_CLOCK);
	if (!ve_string_empty (user_24hr_clock)) {
		if (strcasecmp (ve_sure_string (user_24hr_clock), "auto") == 0) {
			gtk_combo_box_set_active (GTK_COMBO_BOX (clock_type_chooser), CLOCK_AUTO);
		}
		else if (strcasecmp (ve_sure_string (user_24hr_clock), "yes") == 0 ||
		         strcasecmp (ve_sure_string (user_24hr_clock), "true") == 0) {
			gtk_combo_box_set_active (GTK_COMBO_BOX (clock_type_chooser), CLOCK_YES);
		}
		else if (strcasecmp (ve_sure_string (user_24hr_clock), "no") == 0 ||
		         strcasecmp (ve_sure_string (user_24hr_clock), "true") == 0) {
			gtk_combo_box_set_active (GTK_COMBO_BOX (clock_type_chooser), CLOCK_NO);
		}
	}
	
	g_object_set_data_full (G_OBJECT (clock_type_chooser), "key",
	                        g_strdup (MDM_KEY_USE_24_CLOCK), (GDestroyNotify) g_free);
	g_signal_connect (G_OBJECT (clock_type_chooser), "changed",
	                  G_CALLBACK (combobox_changed), NULL);			
	
	commands_button = glade_xml_get_widget (xml, "configure_commands_button");
	g_signal_connect (G_OBJECT (commands_button), "clicked",
	                  G_CALLBACK (command_button_clicked), NULL);
      	
}

static void
setup_local_tab (void)
{
	setup_local_plain_settings ();
	setup_local_themed_settings ();
	setup_local_html_themed_settings();
}

static GtkWidget *
setup_gui (void)
{
	GtkWidget *dialog;

	xml = glade_xml_new (MDM_GLADE_DIR "/mdmsetup.glade", "setup_dialog", NULL);

	dialog = glade_xml_get_widget (xml, "setup_dialog");

	g_signal_connect (G_OBJECT (dialog), "delete_event",
			  G_CALLBACK (delete_event), NULL);
	g_signal_connect (G_OBJECT (dialog), "response",
			  G_CALLBACK (dialog_response), NULL);

	setup_notebook = glade_xml_get_widget (xml, "setup_notebook");

	/* Markup glade labels */
	glade_helper_tagify_label (xml, "themes_label", "b");
	glade_helper_tagify_label (xml, "local_background_label", "b");	
	glade_helper_tagify_label (xml, "local_menubar_label", "b");
	glade_helper_tagify_label (xml, "local_welcome_message_label", "b");
	glade_helper_tagify_label (xml, "label_welcome_note", "i");
	glade_helper_tagify_label (xml, "label_welcome_note", "small");		
	glade_helper_tagify_label (xml, "autologin", "b");
	glade_helper_tagify_label (xml, "timedlogin", "b");
	glade_helper_tagify_label (xml, "security_label", "b");
	glade_helper_tagify_label (xml, "fb_informationlabel", "i");
	glade_helper_tagify_label (xml, "fb_informationlabel", "small");
	
	/* Setup preference tabs */
	setup_general_tab ();
	setup_local_tab (); 
	setup_security_tab ();
	setup_users_tab ();

	return (dialog);
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

static void
apply_user_changes (GObject *object, gint arg1, gpointer user_data)
{
	GtkWidget *dialog = user_data;
	
	if (MdmUserChangesUnsaved == TRUE) {

		GtkWidget *prompt;
		gint response;

		prompt = hig_dialog_new (GTK_WINDOW (dialog),
					 GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
					 GTK_MESSAGE_WARNING,
					 GTK_BUTTONS_NONE,
					 _("Apply the changes to users before closing?"),
					 _("If you don't apply, the changes made on "
					   "the Users tab will be disregarded."));

		gtk_dialog_add_button (GTK_DIALOG (prompt), _("Close _without Applying"), GTK_RESPONSE_CLOSE);
		gtk_dialog_add_button (GTK_DIALOG (prompt), "gtk-cancel", GTK_RESPONSE_CANCEL); 
		gtk_dialog_add_button (GTK_DIALOG (prompt), "gtk-apply", GTK_RESPONSE_APPLY);

		response = gtk_dialog_run (GTK_DIALOG (prompt));
		gtk_widget_destroy (prompt);

		if (response == GTK_RESPONSE_APPLY) {
			GtkWidget *apply_button;

			apply_button = glade_xml_get_widget (xml, "fb_faceapply");
			g_signal_emit_by_name (G_OBJECT (apply_button), "clicked");
		}
		
		gtk_main_quit ();

		if (response == GTK_RESPONSE_CANCEL) {
			gtk_main ();
		}
	}
}

int 
main (int argc, char *argv[])
{
	GtkWidget *dialog;
	char **list;
	gint MdmMinimalUID;
	int i;

	mdm_config_never_cache (TRUE);

	if (g_getenv ("DOING_MDM_DEVELOPMENT") != NULL)
		DOING_MDM_DEVELOPMENT = TRUE;
	if (g_getenv ("RUNNING_UNDER_MDM") != NULL)
		RUNNING_UNDER_MDM = TRUE;

	bindtextdomain (GETTEXT_PACKAGE, GNOMELOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);

	gtk_init (&argc, &argv);

	mdm_log_init ();
	mdm_log_set_debug (FALSE);

	/* Lets check if mdm daemon is running
	   if no there is no point in continuing
	*/
	mdm_running = mdmcomm_check (TRUE);
	if (mdm_running == FALSE)
		exit (EXIT_FAILURE);

	gtk_window_set_default_icon_name ("mdmsetup");	
	glade_init();
	
	/* Start using socket */
	mdmcomm_comm_bulk_start ();
	
	config_file = mdm_common_get_config_file ();
	if (config_file == NULL) {
		GtkWidget *dialog;

		/* Done using socket */
		mdmcomm_comm_bulk_stop ();
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
		mdmcomm_comm_bulk_stop ();
		dialog = hig_dialog_new (NULL /* parent */,
					 GTK_DIALOG_MODAL /* flags */,
					 GTK_MESSAGE_ERROR,
					 GTK_BUTTONS_OK,
					 _("Could not access configuration file (custom.conf)"),
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
		mdmcomm_comm_bulk_stop ();

		fatal_error = hig_dialog_new (NULL /* parent */,
					      GTK_DIALOG_MODAL /* flags */,
					      GTK_MESSAGE_ERROR,
					      GTK_BUTTONS_OK,
					      _("You must be the root user to configure MDM."),
					      "");
		if (RUNNING_UNDER_MDM)
			setup_cursor (GDK_LEFT_PTR);
		gtk_dialog_run (GTK_DIALOG (fatal_error));
		exit (EXIT_FAILURE);
	}

	/*
         * XXX: the setup proggie using a greeter config var for it's
	 * ui?  Say it ain't so.  Our config sections are SUCH A MESS
         */
	MdmIconMaxHeight   = mdm_config_get_int (MDM_KEY_MAX_ICON_HEIGHT);
	MdmIconMaxWidth    = mdm_config_get_int (MDM_KEY_MAX_ICON_WIDTH);
	MdmIncludeAll      = mdm_config_get_bool (MDM_KEY_INCLUDE_ALL);
	MdmInclude         = mdm_config_get_string (MDM_KEY_INCLUDE);
	
	/* We need to make sure that the users in the include list exist
	   and have uid that are higher than MinimalUID. This protects us
	   from invalid data obtained from the config file */
	MdmMinimalUID = mdm_config_get_int (MDM_KEY_MINIMAL_UID);
	list = g_strsplit (MdmInclude, ",", 0);
	for (i=0; list != NULL && list[i] != NULL; i++) {
		if (mdm_is_user_valid (list[i]) && mdm_user_uid (list[i]) >= MdmMinimalUID)
			continue;
		
		MdmInclude = strings_list_remove (MdmInclude, list[i], ",");
	}
	g_strfreev (list);

	MdmExclude         = mdm_config_get_string (MDM_KEY_EXCLUDE);
	MdmSoundProgram    = mdm_config_get_string (MDM_KEY_SOUND_PROGRAM);

	if (ve_string_empty (MdmSoundProgram) ||
            g_access (MdmSoundProgram, X_OK) != 0) {
		MdmSoundProgram = NULL;
	}

        xservers = mdm_config_get_xservers (FALSE);

	/* Done using socket */
	mdmcomm_comm_bulk_stop ();

	/* Once we corrected the include list we need to save it if
	   it was modified */
	if ( strcmp (ve_sure_string (mdm_config_get_string (MDM_KEY_INCLUDE)), ve_sure_string (MdmInclude)) != 0)
		mdm_setup_config_set_string (MDM_KEY_INCLUDE, MdmInclude);
	
	dialog = setup_gui ();

	g_signal_connect (G_OBJECT (dialog), "response",
			  G_CALLBACK (apply_user_changes), dialog);
	gtk_widget_show (dialog);

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
