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

#include <webkit/webkit.h>

/*
 * Set the DOING_MDM_DEVELOPMENT env variable if you aren't running
 * within the protocol
 */
gboolean DOING_MDM_DEVELOPMENT              = FALSE;
gboolean MdmConfiguratorFound               = FALSE;
gboolean *MdmCustomCmdsFound                = NULL;
gboolean MdmSuspendFound                    = FALSE;
gboolean MdmHaltFound                       = FALSE;
gboolean MdmRebootFound                     = FALSE;
gboolean MdmAnyCustomCmdsFound              = FALSE;

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

static WebKitWebView *webView;
static gboolean webkit_ready = FALSE;
static gchar * mdm_msg = "";

static GtkWidget *login;
static guint err_box_clear_handler = 0;
static GtkWidget *icon_win = NULL;
static GtkWidget *sessmenu;
static GtkWidget *langmenu;

static gboolean login_is_local = FALSE;

static GdkPixbuf *defface;

/* Eew. Loads of global vars. It's hard to be event controlled while maintaining state */
static GList *users = NULL;
static GList *users_string = NULL;
static gint size_of_users = 0;

static gchar *curuser = NULL;
static gchar *session = NULL;

static guint timed_handler_id = 0;

extern GList *sessions;
extern GHashTable *sessnames;
extern gchar *default_session;
extern const gchar *current_session;
extern gboolean session_dir_whacked_out;
extern gint mdm_timed_delay;

static gboolean first_prompt = TRUE;

static void process_operation (guchar op_code, const gchar *args);
static gboolean mdm_login_ctrl_handler (GIOChannel *source, GIOCondition cond, gint fd);

static GHashTable *displays_hash = NULL;

static void
check_for_displays (void)
{
	char  *ret;
	char **vec;
	char  *auth_cookie = NULL;
	int    i;

	/*
	 * Might be nice to move this call into read_config() so that it happens
	 * on the same socket call as reading the configuration.
	 */
	ret = mdmcomm_call_mdm (MDM_SUP_ATTACHED_SERVERS, auth_cookie, "1.0.0.0", 5);
	if (ve_string_empty (ret) || strncmp (ret, "OK ", 3) != 0) {
		g_free (ret);
		return;
	}

	vec = g_strsplit (&ret[3], ";", -1);
	g_free (ret);
	if (vec == NULL)
		return;

	if (displays_hash == NULL)
		displays_hash = g_hash_table_new_full (g_str_hash,
						       g_str_equal,
						       g_free,
						       g_free);

	for (i = 0; vec[i] != NULL; i++) {
		char **rvec;

		rvec = g_strsplit (vec[i], ",", -1);
		if (mdm_vector_len (rvec) != 3) {
			g_strfreev (rvec);
			continue;
		}

		g_hash_table_insert (displays_hash,
				     g_strdup (rvec[1]),
				     g_strdup (rvec[0]));

		g_strfreev (rvec);
	}

	g_strfreev (vec);
}

static gchar * 
str_replace(const char *string, const char *delimiter, const char *replacement)
{
	gchar **split;
	gchar *ret;
	g_return_val_if_fail(string      != NULL, NULL);
	g_return_val_if_fail(delimiter   != NULL, NULL);
	g_return_val_if_fail(replacement != NULL, NULL);	

	split = g_strsplit(string, delimiter, 0);
	ret = g_strjoinv(replacement, split);
	g_strfreev(split);
	return ret;
}

static char * 
html_encode(const char *string)
{	
	char * ret;	
	ret = str_replace(string, "'", "&#39");
	ret = str_replace(ret, "\"", "&#34");
	ret = str_replace(ret, ";", "&#59");
	ret = str_replace(ret, "<", "&#60");
	ret = str_replace(ret, ">", "&#62");
	return ret;
}

void webkit_execute_script(const gchar * function, const gchar * arguments) 
{
	if (webkit_ready) {		
		if (arguments == NULL) {
			webkit_web_view_execute_script(webView, function);
		}
		else {
			gchar * tmp;
			tmp = g_strdup_printf("%s(\"%s\")", function, str_replace(arguments, "\n", ""));
			webkit_web_view_execute_script(webView, tmp);		
			g_free (tmp);
		}				
	}
}

gboolean webkit_on_message(WebKitWebView* view, WebKitWebFrame* frame, const gchar* message)
{    
    gchar ** message_parts = g_strsplit (message, "###", -1);
    gchar * command = message_parts[0];
    if (strcmp(command, "LOGIN") == 0) {		
		printf ("%c%s\n", STX, message_parts[1]);
		fflush (stdout);		
	}	
	else if (strcmp(command, "LANGUAGE") == 0) {
		gchar *language = message_parts[1];
		printf ("%c%c%c%c%s\n", STX, BEL, MDM_INTERRUPT_SELECT_LANG, 1, language);
		fflush (stdout);		
		g_free (language);
	}
	else if (strcmp(command, "SESSION") == 0) {
		//gchar *s;
		current_session = message_parts[2];
		//s = g_strdup_printf (_("%s session selected"), mdm_session_name (current_session));
		//webkit_execute_script("mdm_msg",  s);
		//g_free (s);
	}	
	else if (strcmp(command, "SHUTDOWN") == 0) {
		if (mdm_wm_warn_dialog (
			_("Are you sure you want to Shut Down the computer?"), "",
			_("Shut _Down"), NULL, TRUE) == GTK_RESPONSE_YES) {

			_exit (DISPLAY_HALT);
		}
	}
	else if (strcmp(command, "SUSPEND") == 0) {
		if (mdm_wm_warn_dialog (
			_("Are you sure you want to suspend the computer?"), "",
			_("_Suspend"), NULL, TRUE) == GTK_RESPONSE_YES) {

			/* suspend interruption */
			printf ("%c%c%c\n", STX, BEL, MDM_INTERRUPT_SUSPEND);
			fflush (stdout);
		}
	}
	else if (strcmp(command, "RESTART") == 0) {
		if (mdm_wm_warn_dialog (
			_("Are you sure you want to restart the computer?"), "",
			_("_Restart"), NULL, TRUE) == GTK_RESPONSE_YES) {

			_exit (DISPLAY_REBOOT);
		}
	}
	else if (strcmp(command, "FORCE-SHUTDOWN") == 0) {
		_exit (DISPLAY_HALT);
	}
	else if (strcmp(command, "FORCE-SUSPEND") == 0) {
		printf ("%c%c%c\n", STX, BEL, MDM_INTERRUPT_SUSPEND);
		fflush (stdout);
	}
	else if (strcmp(command, "FORCE-RESTART") == 0) {
		_exit (DISPLAY_REBOOT);		
	}
	else if (strcmp(command, "QUIT") == 0) {
		gtk_main_quit();
	}
	else if (strcmp(command, "XDMCP") == 0) {
		_exit (DISPLAY_RUN_CHOOSER);
	}
	else if (strcmp(command, "USER") == 0) {
		printf ("%c%c%c%s\n", STX, BEL, MDM_INTERRUPT_SELECT_USER, message_parts[1]);
        fflush (stdout);
	}
	else {		
		printf("Unknown command received from Webkit greeter: %s\n", command);
	}    
    return TRUE;
}

void webkit_on_loaded(WebKitWebView* view, WebKitWebFrame* frame)
{    
	GIOChannel *ctrlch;
	
	webkit_ready = TRUE;
    mdm_common_login_sound (mdm_config_get_string (MDM_KEY_SOUND_PROGRAM),
					mdm_config_get_string (MDM_KEY_SOUND_ON_LOGIN_FILE),
					mdm_config_get_bool   (MDM_KEY_SOUND_ON_LOGIN));
	mdm_set_welcomemsg ();
	update_clock (); 
	
	mdm_login_browser_populate ();
	mdm_login_lang_init (mdm_config_get_string (MDM_KEY_LOCALE_FILE));  
	mdm_login_session_init ();
	
	if ( ve_string_empty (g_getenv ("MDM_IS_LOCAL")) || !mdm_config_get_bool (MDM_KEY_SYSTEM_MENU)) {
		webkit_execute_script("mdm_hide_suspend", NULL);			
		webkit_execute_script("mdm_hide_restart", NULL);			
		webkit_execute_script("mdm_hide_shutdown", NULL);
		webkit_execute_script("mdm_hide_quit", NULL);
		webkit_execute_script("mdm_hide_xdmcp", NULL);
    }
	
	if (!mdm_working_command_exists (mdm_config_get_string (MDM_KEY_SUSPEND)) ||
	    !mdm_common_is_action_available ("SUSPEND")) {
		webkit_execute_script("mdm_hide_suspend", NULL);
	}
	if (!mdm_working_command_exists (mdm_config_get_string (MDM_KEY_REBOOT)) ||
	    !mdm_common_is_action_available ("REBOOT")) {
		webkit_execute_script("mdm_hide_restart", NULL);
	}	
	if (!mdm_working_command_exists (mdm_config_get_string (MDM_KEY_HALT)) ||
	    !mdm_common_is_action_available ("HALT")) {
		webkit_execute_script("mdm_hide_shutdown", NULL);
	}	
	if (ve_string_empty (g_getenv ("MDM_FLEXI_SERVER")) && ve_string_empty (g_getenv ("MDM_IS_LOCAL"))) {
		webkit_execute_script("mdm_hide_quit", NULL);
	}
	if (!mdm_config_get_bool (MDM_KEY_CHOOSER_BUTTON)) {
		webkit_execute_script("mdm_hide_xdmcp", NULL);
	}

	char * current_lang = g_getenv("LANG");
	if (current_lang) {	
		char *name;
		char *untranslated;
		if (mdm_common_locale_is_displayable (current_lang)) {
			name = mdm_lang_name (current_lang,
			    FALSE /* never_encoding */,
			    TRUE /* no_group */,
			    FALSE /* untranslated */,
			    FALSE /* markup */);

			untranslated = mdm_lang_untranslated_name (current_lang,
						 TRUE /* markup */);

			if (untranslated != NULL) {
				gchar * args = g_strdup_printf("%s\", \"%s", untranslated, current_lang);
				webkit_execute_script("mdm_set_current_language", args);
				g_free (args);
			}
			else {
				gchar * args = g_strdup_printf("%s\", \"%s", name, current_lang);
				webkit_execute_script("mdm_set_current_language", args);
				g_free (args);
			}	 	
		}
		g_free (name);
		g_free (untranslated);
	}	
	
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
    
    gtk_widget_show_all (GTK_WIDGET (login));    
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
		webkit_execute_script("mdm_timed", autologin_msg);		
		g_free (autologin_msg);
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

static gboolean
reap_flexiserver (gpointer data)
{
	int reapminutes = mdm_config_get_int (MDM_KEY_FLEXI_REAP_DELAY_MINUTES);

	if (reapminutes > 0 &&
	    ((time (NULL) - last_reap_delay) / 60) > reapminutes) {
		_exit (DISPLAY_REMANAGE);
	}
	return TRUE;
}


static void
mdm_login_done (int sig)
{
	_exit (EXIT_SUCCESS);
}

void 
mdm_login_session_init ()
{
    GSList *sessgrp = NULL;
    GList *tmp;    
    int num = 1;
    char *label;

    current_session = NULL;
    
    if (mdm_config_get_bool (MDM_KEY_SHOW_LAST_SESSION)) {
            current_session = LAST_SESSION;            
			gchar * args = g_strdup_printf("%s\", \"%s", _("Last"), LAST_SESSION);
			webkit_execute_script("mdm_add_session", args);
			g_free (args);            
    }

    mdm_session_list_init ();

    for (tmp = sessions; tmp != NULL; tmp = tmp->next) {
	    MdmSession *session;
	    char *file;

	    file = (char *) tmp->data;
	    session = g_hash_table_lookup (sessnames, file);

	    //if (num < 10 && 
	    //   (strcmp (file, MDM_SESSION_FAILSAFE_GNOME) != 0) &&
	    //   (strcmp (file, MDM_SESSION_FAILSAFE_XTERM) != 0))
		//    label = g_strdup_printf ("_%d. %s", num, session->name);
	    //else
			label = g_strdup (session->name);
	    num++;
	    
	    gchar * args = g_strdup_printf("%s\", \"%s", label, file);
	    webkit_execute_script("mdm_add_session", args);
	    g_free (args);
	    g_free (label);
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

void
mdm_login_lang_init (gchar * locale_file)
{
  GList *list, *li;

  list = mdm_lang_read_locale_file (locale_file);



  /*gtk_list_store_append (lang_model, &iter);
  gtk_list_store_set (lang_model, &iter,
		      TRANSLATED_NAME_COLUMN, _("Last language"),
		      UNTRANSLATED_NAME_COLUMN, NULL,
		      LOCALE_COLUMN, LAST_LANGUAGE,
		      -1);

  gtk_list_store_append (lang_model, &iter);
  gtk_list_store_set (lang_model, &iter,
		      TRANSLATED_NAME_COLUMN, _("System Default"),
		      UNTRANSLATED_NAME_COLUMN, NULL,
		      LOCALE_COLUMN, DEFAULT_LANGUAGE,
		      -1);
		      */

  for (li = list; li != NULL; li = li->next)
    {
      char *lang = li->data;
      char *name;
      char *untranslated;

      li->data = NULL;

      if (!mdm_common_locale_is_displayable (lang)) {
        g_free (lang);
        continue;
      }

      name = mdm_lang_name (lang,
			    FALSE /* never_encoding */,
			    TRUE /* no_group */,
			    FALSE /* untranslated */,
			    FALSE /* markup */);

      untranslated = mdm_lang_untranslated_name (lang,
						 TRUE /* markup */);

      	if (untranslated != NULL) {
			gchar * args = g_strdup_printf("%s\", \"%s", untranslated, lang);
	  		webkit_execute_script("mdm_add_language", args);
	  		g_free (args);
      	}
      	else {
			gchar * args = g_strdup_printf("%s\", \"%s", name, lang);
	  		webkit_execute_script("mdm_add_language", args);
	  		g_free (args);
      	}	  
     
      g_free (name);
      g_free (untranslated);
      g_free (lang);
    }
  g_list_free (list);
}

static gboolean
err_box_clear (gpointer data)
{	
	webkit_execute_script("mdm_error", "");
	err_box_clear_handler = 0;
	return FALSE;
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
   * indicator state from the parent display, since we must be inside an
   * Xnest */
  dsp = get_parent_display ();
  if (dsp == NULL)
    dsp = GDK_DISPLAY ();

  if (XkbGetState (dsp, XkbUseCoreKbd, &states) != Success)
      return FALSE;

  return (states.locked_mods & LockMask) != 0;
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

void
process_operation (guchar       op_code,
		   const gchar *args)
{
    char *tmp;
    gint i, x, y;
    GtkWidget *dlg;
    static gboolean replace_msg = TRUE;
    static gboolean messages_to_give = FALSE;
    gint lookup_status = SESSION_LOOKUP_SUCCESS;
    gchar *firstmsg = NULL;
    gchar *secondmsg = NULL;
    gint dont_save_session = GTK_RESPONSE_YES;
    
    /* Parse opcode */
    switch (op_code) {
    case MDM_SETLOGIN:
	/* somebody is trying to fool us this is the user that
	 * wants to log in, and well, we are the gullible kind */
	g_free (curuser);
	curuser = g_strdup (args);	
    // WEBKIT TODO: SELECT THE USER curuser IN THE WEBKIT USER LIST
	printf ("%c\n", STX);
	fflush (stdout);
	break;

    case MDM_PROMPT:
	tmp = ve_locale_to_utf8 (args);
	if (tmp != NULL && strcmp (tmp, _("Username:")) == 0) {
		mdm_common_login_sound (mdm_config_get_string (MDM_KEY_SOUND_PROGRAM),
					mdm_config_get_string (MDM_KEY_SOUND_ON_LOGIN_FILE),
					mdm_config_get_bool   (MDM_KEY_SOUND_ON_LOGIN));		
		webkit_execute_script("mdm_prompt", _("Username:"));
	} else {
		if (tmp != NULL)			
			webkit_execute_script("mdm_prompt", tmp);
	}
	g_free (tmp);

	first_prompt = FALSE;
	
	/* replace rather then append next message string */
	replace_msg = TRUE;

	/* the user has seen messages */
	messages_to_give = FALSE;

	break;

    case MDM_NOECHO:
	tmp = ve_locale_to_utf8 (args);
	if (tmp != NULL && strcmp (tmp, _("Password:")) == 0) {		
		webkit_execute_script("mdm_noecho", _("Password:"));
	} else {
		if (tmp != NULL)
			webkit_execute_script("mdm_noecho", tmp);
	}
	g_free (tmp);
	
	first_prompt = FALSE;
	
	/* replace rather then append next message string */
	replace_msg = TRUE;

	/* the user has seen messages */
	messages_to_give = FALSE;

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
		oldtext = g_strdup (mdm_msg);
		if ( ! ve_string_empty (oldtext)) {
			char *newtext;
			tmp = ve_locale_to_utf8 (args);
			newtext = g_strdup_printf ("%s\n%s", oldtext, tmp);
			g_free (tmp);
			mdm_msg = g_strdup (newtext);	
			g_free (newtext);
		} else {
			tmp = ve_locale_to_utf8 (args);			
			mdm_msg = g_strdup (tmp);
			g_free (tmp);
		}
	} else {
		tmp = ve_locale_to_utf8 (args);
		mdm_msg = g_strdup (tmp);		
		g_free (tmp);
	}
	replace_msg = FALSE;
		
	webkit_execute_script("mdm_msg", mdm_msg);
	
	printf ("%c\n", STX);
	fflush (stdout);

	break;

    case MDM_ERRBOX:
	tmp = ve_locale_to_utf8 (args);	
	webkit_execute_script("mdm_error", tmp);
	
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
	tmp = ve_locale_to_utf8 (args);
	session = mdm_session_lookup (tmp, &lookup_status);
	if (lookup_status != SESSION_LOOKUP_SUCCESS) {				
		switch (lookup_status) {
		case SESSION_LOOKUP_PREFERRED_MISSING:
			firstmsg = g_strdup_printf (_("Do you wish to make %s the default for "
						      "future sessions?"),
						      mdm_session_name (mdm_get_default_session ()));
			secondmsg = g_strdup_printf (_("Your preferred session type %s is not "
						       "installed on this computer."),
						       mdm_session_name (tmp));	    
			dont_save_session = mdm_wm_query_dialog (firstmsg, secondmsg,
							_("Just _Log In"), _("Make _Default"), TRUE);              
			g_free (firstmsg);
			g_free (secondmsg);
			mdm_set_save_session (dont_save_session);			
			break;
			
		case SESSION_LOOKUP_DEFAULT_MISMATCH:
			firstmsg = g_strdup_printf (_("Do you wish to make %s the default for "
						      "future sessions?"),
						    mdm_session_name (session));
			secondmsg = g_strdup_printf (_("You have chosen %s for this "
						       "session, but your default "
						       "setting is %s."),
						     mdm_session_name (session),
						     mdm_session_name (tmp));
			dont_save_session = mdm_wm_query_dialog (firstmsg, secondmsg,
							    _("Just For _This Session"), _("Make _Default"), TRUE);
			
			g_free (firstmsg);
			g_free (secondmsg);
			mdm_set_save_session (dont_save_session);			
			break;
		case SESSION_LOOKUP_USE_SWITCHDESK:
			firstmsg = g_strdup_printf (_("You have chosen %s for this "
						      "session"),
						    mdm_session_name (session));
			secondmsg = g_strdup_printf (_("If you wish to make %s "
						       "the default for future sessions, "
						       "run the 'switchdesk' utility "
						       "(System->Desktop Switching Tool from "
						       "the panel menu)."),
						     mdm_session_name (session));			 
			mdm_wm_message_dialog (firstmsg, secondmsg);			 
			g_free (firstmsg);
			g_free (secondmsg);
			break;
			
		default:
			break;
		}	
	}
	g_free (tmp);
	if (mdm_get_save_session () == GTK_RESPONSE_CANCEL) {
	    printf ("%c%s\n", STX, MDM_RESPONSE_CANCEL);
	} else {
	    tmp = ve_locale_from_utf8 (session);
	    printf ("%c%s\n", STX, tmp);
	    g_free (tmp);
	}
	fflush (stdout);
	break;

    case MDM_LANG:
	mdm_lang_op_lang (args);
	break;

    case MDM_SSESS:
	if (mdm_get_save_session () == GTK_RESPONSE_NO)
	    printf ("%cY\n", STX);
	else
	    printf ("%c\n", STX);
	fflush (stdout);
	
	break;

    case MDM_SLANG:
	mdm_lang_op_slang (args);
	break;

    case MDM_SETLANG:
		//mdm_lang_op_setlang (args);
    	webkit_execute_script("mdm_set_current_language", args);
		printf ("%c\n", STX);
  		fflush (stdout);
	break;

    case MDM_ALWAYS_RESTART:
	mdm_lang_op_always_restart (args);
	break;

    case MDM_RESET:
	/* fall thru to reset */

    case MDM_RESETOK:
	if (curuser != NULL) {
	    g_free (curuser);
	    curuser = NULL;
	}

	first_prompt = TRUE;
	
	tmp = ve_locale_to_utf8 (args);
	mdm_msg = g_strdup (tmp);		
	webkit_execute_script("mdm_msg", mdm_msg);
	g_free (tmp);	

	printf ("%c\n", STX);
	fflush (stdout);

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
		oldtext = g_strdup (mdm_msg);

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
	gtk_widget_set_sensitive (login, FALSE);
	webkit_execute_script("mdm_disable", NULL);
	printf ("%c\n", STX);
	fflush (stdout);
	break;

    case MDM_ENABLE:
	gtk_widget_set_sensitive (login, TRUE);	
	webkit_execute_script("mdm_enable", NULL);
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
	mdm_common_fail_greeter ("Unexpected greeter command received: '%c'", op_code);
	break;
    }
}


void
mdm_login_browser_populate (void)
{
	check_for_displays ();

    GList *li;	
    for (li = users; li != NULL; li = li->next) {
	    MdmUser *usr = li->data;	    	    
	    char *login, *gecos, *status;
	    login = mdm_common_text_to_escaped_utf8 (usr->login);
	    gecos = mdm_common_text_to_escaped_utf8 (usr->gecos);	    				   
	    if (g_hash_table_lookup (displays_hash, usr->login)) {
	    	status = _("Already logged in");
		}
		else {
			status = "";
		}
		gchar * args = g_strdup_printf("%s\", \"%s\", \"%s", login, gecos, status);
		webkit_execute_script("mdm_add_user", args);
		g_free (args);
	    g_free (login);
	    g_free (gecos);	    	    
    }

    /* we are done with the hash */
	g_hash_table_destroy (displays_hash);
	displays_hash = NULL;
    return;
}

gboolean
update_clock (void)
{
        struct tm *the_tm;
	gchar *str;
        gint time_til_next_min;
	
	str = mdm_common_get_clock (&the_tm);
	webkit_execute_script("set_clock", str);			
	g_free (str);

	/* account for leap seconds */
	time_til_next_min = 60 - the_tm->tm_sec;
	time_til_next_min = (time_til_next_min>=0?time_til_next_min:0);

	g_timeout_add (time_til_next_min*1000, (GSourceFunc)update_clock, NULL);
	return FALSE;
}

gboolean
check_webkit (void)
{
	g_timeout_add (1000, (GSourceFunc)check_webkit, NULL);
	return FALSE;
}

void
mdm_set_welcomemsg (void)
{
	gchar *greeting;
	gchar *welcomemsg     = mdm_common_get_welcomemsg ();	
	greeting = mdm_common_expand_text (welcomemsg);
	webkit_execute_script("set_welcome_message", greeting);		
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
webkit_init (void) {
	
	GError *error;
	char *html;
	gsize file_length;
	gchar * theme_name = mdm_config_get_string (MDM_KEY_HTML_THEME);
	gchar * theme_dir = g_strdup_printf("file:///usr/share/mdm/html-themes/%s/", theme_name);
	gchar * theme_filename = g_strdup_printf("/usr/share/mdm/html-themes/%s/index.html", theme_name);			  
			
	if (!g_file_get_contents (theme_filename, &html, &file_length, error)) {    
		GtkWidget *dialog;
		char *s;
		char *tmp;

		mdm_wm_init (0);
		mdm_wm_focus_new_windows (TRUE);

		tmp = ve_filename_to_utf8 (ve_sure_string (theme_name));
		s = g_strdup_printf (_("There was an error loading the theme %s"), tmp);
		g_free (tmp);
		dialog = hig_dialog_new (NULL /* parent */,
								 GTK_DIALOG_MODAL /* flags */,
								 GTK_MESSAGE_ERROR,
								 GTK_BUTTONS_OK,
								 s,
								 (error && error->message) ? error->message : "");
		g_free (s);

		gtk_widget_show_all (dialog);
		mdm_wm_center_window (GTK_WINDOW (dialog));

		mdm_common_setup_cursor (GDK_LEFT_PTR);

		gtk_dialog_run (GTK_DIALOG (dialog));
		gtk_widget_destroy (dialog);
		
		g_free (theme_name);
		g_free (theme_dir);
		g_free (theme_filename);
		theme_name = "mdm";
		theme_dir = g_strdup_printf("file:///usr/share/mdm/html-themes/%s/", theme_name);
		theme_filename = g_strdup_printf("/usr/share/mdm/html-themes/%s/index.html", theme_name);
		g_file_get_contents (theme_filename, &html, &file_length, NULL);
			
	}
	
	html = str_replace(html, "$login_label", html_encode(_("Login")));
	html = str_replace(html, "$ok_label", html_encode(_("OK")));
	html = str_replace(html, "$cancel_label", html_encode(_("Cancel")));
	html = str_replace(html, "$enter_your_username_label", html_encode(_("Please enter your username")));
	html = str_replace(html, "$enter_your_password_label", html_encode(_("Please enter your password")));
	html = str_replace(html, "$hostname", g_get_host_name());
	
	html = str_replace(html, "$shutdown", html_encode(_("Shutdown")));
	html = str_replace(html, "$suspend", html_encode(_("Suspend")));
	html = str_replace(html, "$quit", html_encode(_("Quit")));
	html = str_replace(html, "$restart", html_encode(_("Restart")));	
	html = str_replace(html, "$remoteloginviaxdmcp", html_encode(_("Remote Login via XDMCP...")));
	
	html = str_replace(html, "$session", html_encode(_("Session")));
	html = str_replace(html, "$selectsession", html_encode(_("Select a session")));
	html = str_replace(html, "$defaultsession", html_encode(_("Default session")));
	
	html = str_replace(html, "$language", html_encode(_("Language")));
	html = str_replace(html, "$selectlanguage", html_encode(_("Select a language")));

	html = str_replace(html, "$areyousuretoquit", html_encode(_("Are you sure you want to quit?")));
	html = str_replace(html, "$close", html_encode(_("Close")));
	
	html = str_replace(html, "$locale", g_strdup (setlocale (LC_MESSAGES, NULL)));
		
	webView = WEBKIT_WEB_VIEW(webkit_web_view_new());
	
	WebKitWebSettings *settings = webkit_web_settings_new ();
	g_object_set (G_OBJECT(settings), "enable-default-context-menu", FALSE, NULL);	
	g_object_set (G_OBJECT(settings), "enable-scripts", TRUE, NULL);	
	g_object_set (G_OBJECT(settings), "enable-webgl", TRUE, NULL);	
	g_object_set (G_OBJECT(settings), "enable-universal-access-from-file-uris", TRUE, NULL);	
	g_object_set (G_OBJECT(settings), "enable-developer-extras", TRUE, NULL);	
	
	webkit_web_view_set_settings (WEBKIT_WEB_VIEW(webView), settings);	
	webkit_web_view_set_transparent (webView, TRUE);
	
	webkit_web_view_load_string(webView, html, "text/html", "UTF-8", theme_dir);

	g_signal_connect(G_OBJECT(webView), "script-alert", G_CALLBACK(webkit_on_message), 0);
	g_signal_connect(G_OBJECT(webView), "load-finished", G_CALLBACK(webkit_on_loaded), 0);
}

static void
mdm_login_gui_init (void)
{      
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
    gtk_window_set_decorated (GTK_WINDOW (login), FALSE);
    gtk_window_set_default_size (GTK_WINDOW (login), 
			       mdm_wm_screen.width, 
			       mdm_wm_screen.height);
    
    g_object_ref (login);
    g_object_set_data_full (G_OBJECT (login), "login", login,
			    (GDestroyNotify) g_object_unref);

    gtk_widget_set_events (login, GDK_ALL_EVENTS_MASK);

    g_signal_connect (G_OBJECT (login), "key_press_event",
                      G_CALLBACK (key_press_event), NULL);
                         
     
    GtkWidget *scrolled = gtk_scrolled_window_new (NULL, NULL);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	
	gtk_container_add (GTK_CONTAINER (scrolled), webView);	         
	gtk_container_add (GTK_CONTAINER (login), scrolled);
           
    int height;
    
    height = size_of_users + 4 /* some padding */;
    if (height > mdm_wm_screen.height * 0.25)
        height = mdm_wm_screen.height * 0.25;    
                                      
    /* cursor blinking is evil on remote displays, don't do it forever */
    mdm_common_setup_blinking ();    
                             
    gtk_widget_grab_focus (webView);	
    gtk_window_set_focus (GTK_WINDOW (login), webView);	
    g_object_set (G_OBJECT (login),
		  "allow_grow", TRUE,
		  "allow_shrink", TRUE,
		  "resizable", TRUE,
		  NULL);
    
    mdm_wm_center_window (GTK_WINDOW (login));    
    
    
    
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
	
	/* Read config data in bulk */
	mdmcomm_comm_bulk_start ();

	/*
	 * Read all the keys at once and close sockets connection so we do
	 * not have to keep the socket open. 
	 */
	mdm_config_get_string (MDM_KEY_HTML_THEME);
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
	mdm_config_get_string (MDM_KEY_REMOTE_WELCOME);
	mdm_config_get_string (MDM_KEY_SESSION_DESKTOP_DIR);
	mdm_config_get_string (MDM_KEY_SOUND_PROGRAM);
	mdm_config_get_string (MDM_KEY_SOUND_ON_LOGIN_FILE);
	mdm_config_get_string (MDM_KEY_SUSPEND);
	mdm_config_get_string (MDM_KEY_TIMED_LOGIN);
	mdm_config_get_string (MDM_KEY_USE_24_CLOCK);
	mdm_config_get_string (MDM_KEY_WELCOME);
	mdm_config_get_string (MDM_KEY_RBAC_SYSTEM_COMMAND_KEYS);
	mdm_config_get_string (MDM_KEY_SYSTEM_COMMANDS_IN_MENU);

	/* String keys for custom commands */	
	for (i = 0; i < MDM_CUSTOM_COMMAND_MAX; i++) {				
		key_string = g_strdup_printf ("%s%d=", MDM_KEY_CUSTOM_CMD_TEMPLATE, i);
		mdm_config_get_string (key_string);
		g_free (key_string);

		key_string = g_strdup_printf ("%s%d=", MDM_KEY_CUSTOM_CMD_LABEL_TEMPLATE, i);
		mdm_config_get_string (key_string);
		g_free (key_string);
		
		key_string = g_strdup_printf ("%s%d=", MDM_KEY_CUSTOM_CMD_LR_LABEL_TEMPLATE, i);
		mdm_config_get_string (key_string);
		g_free (key_string);

		key_string = g_strdup_printf ("%s%d=", MDM_KEY_CUSTOM_CMD_TEXT_TEMPLATE, i);
		mdm_config_get_string (key_string);
		g_free (key_string);

		key_string = g_strdup_printf ("%s%d=", MDM_KEY_CUSTOM_CMD_TOOLTIP_TEMPLATE, i);
		mdm_config_get_string (key_string);
		g_free (key_string);
	}     

	mdm_config_get_int    (MDM_KEY_BACKGROUND_TYPE);
	mdm_config_get_int    (MDM_KEY_BACKGROUND_PROGRAM_INITIAL_DELAY);
	mdm_config_get_int    (MDM_KEY_BACKGROUND_PROGRAM_RESTART_DELAY);
	mdm_config_get_int    (MDM_KEY_FLEXI_REAP_DELAY_MINUTES);
	mdm_config_get_int    (MDM_KEY_MAX_ICON_HEIGHT);
	mdm_config_get_int    (MDM_KEY_MAX_ICON_WIDTH);
	mdm_config_get_int    (MDM_KEY_MINIMAL_UID);
	mdm_config_get_int    (MDM_KEY_TIMED_LOGIN_DELAY);
	mdm_config_get_int    (MDM_KEY_XINERAMA_SCREEN);

	mdm_config_get_bool   (MDM_KEY_ALLOW_GTK_THEME_CHANGE);
	mdm_config_get_bool   (MDM_KEY_ALLOW_REMOTE_ROOT);
	mdm_config_get_bool   (MDM_KEY_ALLOW_ROOT);	
	mdm_config_get_bool   (MDM_KEY_CHOOSER_BUTTON);
	mdm_config_get_bool   (MDM_KEY_CONFIG_AVAILABLE);
	mdm_config_get_bool   (MDM_KEY_DEFAULT_REMOTE_WELCOME);
	mdm_config_get_bool   (MDM_KEY_DEFAULT_WELCOME);
	mdm_config_get_bool   (MDM_KEY_ENTRY_CIRCLES);
	mdm_config_get_bool   (MDM_KEY_ENTRY_INVISIBLE);
	mdm_config_get_bool   (MDM_KEY_INCLUDE_ALL);	
	mdm_config_get_bool   (MDM_KEY_RUN_BACKGROUND_PROGRAM_ALWAYS);
	mdm_config_get_bool   (MDM_KEY_RESTART_BACKGROUND_PROGRAM);
	mdm_config_get_bool   (MDM_KEY_SHOW_GNOME_FAILSAFE);
	mdm_config_get_bool   (MDM_KEY_SHOW_LAST_SESSION);
	mdm_config_get_bool   (MDM_KEY_SHOW_XTERM_FAILSAFE);
	mdm_config_get_bool   (MDM_KEY_SOUND_ON_LOGIN);
	mdm_config_get_bool   (MDM_KEY_SYSTEM_MENU);
	mdm_config_get_bool   (MDM_KEY_TIMED_LOGIN_ENABLE);	
	mdm_config_get_bool   (MDM_KEY_ADD_GTK_MODULES);

	/* Keys not to include in reread_config */	
	mdm_config_get_string (MDM_KEY_PRE_FETCH_PROGRAM);	

	mdmcomm_comm_bulk_stop ();
}

static gboolean
mdm_reread_config (int sig, gpointer data)
{
	gboolean resize = FALSE;
	gboolean custom_changed = FALSE;
	gint i;
	gchar *key_string = NULL;

	/* Read config data in bulk */
	mdmcomm_comm_bulk_start ();

	/* reparse config stuff here.  At least the ones we care about */
	/* FIXME: We should update these on the fly rather than just
         * restarting */
	/* Also we may not need to check ALL those keys but just a few */

	if (mdm_config_reload_string (MDM_KEY_BACKGROUND_PROGRAM) ||
	    mdm_config_get_string (MDM_KEY_HTML_THEME) || 
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
	    mdm_config_reload_int    (MDM_KEY_XINERAMA_SCREEN) ||

	    mdm_config_reload_bool   (MDM_KEY_ALLOW_GTK_THEME_CHANGE) ||
	    mdm_config_reload_bool   (MDM_KEY_ALLOW_ROOT) ||
	    mdm_config_reload_bool   (MDM_KEY_ALLOW_REMOTE_ROOT) ||	    
	    mdm_config_reload_bool   (MDM_KEY_CHOOSER_BUTTON) ||
	    mdm_config_reload_bool   (MDM_KEY_CONFIG_AVAILABLE) ||
	    mdm_config_reload_bool   (MDM_KEY_ENTRY_CIRCLES) ||
	    mdm_config_reload_bool   (MDM_KEY_ENTRY_INVISIBLE) ||
	    mdm_config_reload_bool   (MDM_KEY_INCLUDE_ALL) ||	    
	    mdm_config_reload_bool   (MDM_KEY_RESTART_BACKGROUND_PROGRAM) ||
	    mdm_config_reload_bool   (MDM_KEY_RUN_BACKGROUND_PROGRAM_ALWAYS) ||
	    mdm_config_reload_bool   (MDM_KEY_SHOW_GNOME_FAILSAFE) ||
	    mdm_config_reload_bool   (MDM_KEY_SHOW_LAST_SESSION) ||
	    mdm_config_reload_bool   (MDM_KEY_SHOW_XTERM_FAILSAFE) ||
	    mdm_config_reload_bool   (MDM_KEY_SYSTEM_MENU) ||
	    mdm_config_reload_bool   (MDM_KEY_TIMED_LOGIN_ENABLE) ||	    
	    mdm_config_reload_bool   (MDM_KEY_ADD_GTK_MODULES)) {

		/* Set busy cursor */
		mdm_common_setup_cursor (GDK_WATCH);

		mdm_wm_save_wm_order ();
		mdmcomm_comm_bulk_stop ();

		_exit (DISPLAY_RESTARTGREETER);
		return TRUE;
	}

	/* Keys for custom commands */
	for (i = 0; i < MDM_CUSTOM_COMMAND_MAX; i++) {		
		key_string = g_strdup_printf ("%s%d=", MDM_KEY_CUSTOM_CMD_TEMPLATE, i);
		if(mdm_config_reload_string (key_string))
			custom_changed = TRUE;
		g_free (key_string);

		key_string = g_strdup_printf ("%s%d=", MDM_KEY_CUSTOM_CMD_LABEL_TEMPLATE, i);
		if(mdm_config_reload_string (key_string))
			custom_changed = TRUE;
		g_free (key_string);

		key_string = g_strdup_printf ("%s%d=", MDM_KEY_CUSTOM_CMD_LR_LABEL_TEMPLATE, i);
		if(mdm_config_reload_string (key_string))
			custom_changed = TRUE;
		g_free (key_string);

		key_string = g_strdup_printf ("%s%d=", MDM_KEY_CUSTOM_CMD_TEXT_TEMPLATE, i);
		if(mdm_config_reload_string (key_string))
			custom_changed = TRUE;
		g_free (key_string);

		key_string = g_strdup_printf ("%s%d=", MDM_KEY_CUSTOM_CMD_TOOLTIP_TEMPLATE, i);
		if(mdm_config_reload_string (key_string))
			custom_changed = TRUE;
		g_free (key_string);
	}     

	if(custom_changed){
		/* Set busy cursor */
		mdm_common_setup_cursor (GDK_WATCH);

		mdm_wm_save_wm_order ();
		mdmcomm_comm_bulk_stop ();

		_exit (DISPLAY_RESTARTGREETER);
		return TRUE;		
	}

	mdm_config_reload_string (MDM_KEY_SOUND_PROGRAM);
	mdm_config_reload_bool   (MDM_KEY_SOUND_ON_LOGIN);
	mdm_config_reload_string (MDM_KEY_SOUND_ON_LOGIN_FILE);
	mdm_config_reload_string (MDM_KEY_USE_24_CLOCK);
	update_clock ();
	
	if (mdm_config_reload_string (MDM_KEY_WELCOME) ||
            mdm_config_reload_bool   (MDM_KEY_DEFAULT_WELCOME) ||
            mdm_config_reload_string (MDM_KEY_REMOTE_WELCOME) ||
            mdm_config_reload_bool   (MDM_KEY_DEFAULT_REMOTE_WELCOME)) {

		mdm_set_welcomemsg ();
	}

	mdmcomm_comm_bulk_stop ();

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
    const char *mdm_version;
    const char *mdm_protocol_version;
    guint sid;
    char *bg_color;

    if (g_getenv ("DOING_MDM_DEVELOPMENT") != NULL)
	    DOING_MDM_DEVELOPMENT = TRUE;

    bindtextdomain ("mdm", "/usr/share/mdm/locale/");
    bind_textdomain_codeset ("mdm", "UTF-8");
    textdomain ("mdm");

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

    mdm_common_log_init ();
    mdm_common_log_set_debug (mdm_config_get_bool (MDM_KEY_DEBUG));

    mdm_common_setup_builtin_icons ();

    /* Read all configuration at once, so the values get cached */
    mdm_read_config ();
    
    setlocale (LC_ALL, "");

    mdm_wm_screen_init (mdm_config_get_int (MDM_KEY_XINERAMA_SCREEN));

    mdm_version = g_getenv ("MDM_VERSION");
    mdm_protocol_version = g_getenv ("MDM_GREETER_PROTOCOL_VERSION");

    /* Load the background as early as possible so MDM does not leave  */
    /* the background unfilled.   The cursor should be a watch already */
    /* but just in case */
    bg_color = mdm_config_get_string (MDM_KEY_GRAPHICAL_THEMED_COLOR);
	/* If a graphical theme color does not exist fallback to the plain color */
	if (ve_string_empty (bg_color)) {
		bg_color = mdm_config_get_string (MDM_KEY_BACKGROUND_COLOR);
	}
	mdm_common_setup_background_color (bg_color);
    mdm_common_setup_cursor (GDK_WATCH);

    if ( ! DOING_MDM_DEVELOPMENT &&
	 ((mdm_protocol_version != NULL &&
	   strcmp (mdm_protocol_version, MDM_GREETER_PROTOCOL_VERSION) != 0) ||
	  (mdm_protocol_version == NULL &&
	   (mdm_version == NULL ||
	    strcmp (mdm_version, VERSION) != 0))) &&
	        ve_string_empty (g_getenv ("MDM_IS_LOCAL"))) {
	    GtkWidget *dialog;
	    gchar *msg;

	    mdm_wm_init (0);

	    mdm_wm_focus_new_windows (TRUE);
	    
	    msg = g_strdup_printf (_("The greeter version (%s) does not match the daemon "
				     "version. "
				     "You have probably just upgraded MDM. "
				     "Please restart the MDM daemon or the computer."),
				   VERSION);

	    dialog = hig_dialog_new (NULL /* parent */,
				     GTK_DIALOG_MODAL /* flags */,
				     GTK_MESSAGE_ERROR,
				     GTK_BUTTONS_OK,
				     _("Cannot start the greeter"),
				     msg);
	    g_free (msg);

	    gtk_widget_show_all (dialog);
	    mdm_wm_center_window (GTK_WINDOW (dialog));

	    mdm_common_setup_cursor (GDK_LEFT_PTR);

	    gtk_dialog_run (GTK_DIALOG (dialog));

	    return EXIT_SUCCESS;
    }

    if ( ! DOING_MDM_DEVELOPMENT &&
	mdm_protocol_version == NULL &&
	mdm_version == NULL) {
	    GtkWidget *dialog;
	    gchar *msg;

	    mdm_wm_init (0);

	    mdm_wm_focus_new_windows (TRUE);
	    
	    msg = g_strdup_printf (_("The greeter version (%s) does not match the daemon "
	                             "version. "
	                             "You have probably just upgraded MDM. "
	                             "Please restart the MDM daemon or the computer."),
	                           VERSION);

	    dialog = hig_dialog_new (NULL /* parent */,
				     GTK_DIALOG_MODAL /* flags */,
				     GTK_MESSAGE_WARNING,
				     GTK_BUTTONS_NONE,
				     _("Cannot start the greeter"),
				     msg);
	    g_free (msg);

	    gtk_dialog_add_buttons (GTK_DIALOG (dialog),
				    _("Restart"),
				    RESPONSE_REBOOT,
				    GTK_STOCK_CLOSE,
				    RESPONSE_CLOSE,
				    NULL);

	    gtk_widget_show_all (dialog);
	    mdm_wm_center_window (GTK_WINDOW (dialog));

	    mdm_common_setup_cursor (GDK_LEFT_PTR);

	    switch (gtk_dialog_run (GTK_DIALOG (dialog))) {
	    case RESPONSE_REBOOT:
		    gtk_widget_destroy (dialog);
		    return DISPLAY_REBOOT;
	    default:
		    gtk_widget_destroy (dialog);
		    return DISPLAY_ABORT;
	    }
    }

    if ( ! DOING_MDM_DEVELOPMENT &&
	 ((mdm_protocol_version != NULL &&
	   strcmp (mdm_protocol_version, MDM_GREETER_PROTOCOL_VERSION) != 0) ||
	  (mdm_protocol_version == NULL &&
	   strcmp (mdm_version, VERSION) != 0))) {
	    GtkWidget *dialog;
	    gchar *msg;

	    mdm_wm_init (0);

	    mdm_wm_focus_new_windows (TRUE);
	    
	    msg = g_strdup_printf (_("The greeter version (%s) does not match the daemon "
	                             "version (%s).  "
	                             "You have probably just upgraded MDM.  "
	                             "Please restart the MDM daemon or the computer."),
	                           VERSION, mdm_version);

	    dialog = hig_dialog_new (NULL /* parent */,
				     GTK_DIALOG_MODAL /* flags */,
				     GTK_MESSAGE_WARNING,
				     GTK_BUTTONS_NONE,
				     _("Cannot start the greeter"),
				     msg);
	    g_free (msg);

	    gtk_dialog_add_buttons (GTK_DIALOG (dialog),
				    _("Restart MDM"),
				    RESPONSE_RESTART,
				    _("Restart computer"),
				    RESPONSE_REBOOT,
				    GTK_STOCK_CLOSE,
				    RESPONSE_CLOSE,
				    NULL);


	    gtk_widget_show_all (dialog);
	    mdm_wm_center_window (GTK_WINDOW (dialog));

	    gtk_dialog_set_default_response (GTK_DIALOG (dialog), RESPONSE_RESTART);

	    mdm_common_setup_cursor (GDK_LEFT_PTR);

	    switch (gtk_dialog_run (GTK_DIALOG (dialog))) {
	    case RESPONSE_RESTART:
		    gtk_widget_destroy (dialog);
		    return DISPLAY_RESTARTMDM;
	    case RESPONSE_REBOOT:
		    gtk_widget_destroy (dialog);
		    return DISPLAY_REBOOT;
	    default:
		    gtk_widget_destroy (dialog);
		    return DISPLAY_ABORT;
	    }
    }
    
    defface = mdm_common_get_face (NULL,
                   mdm_config_get_string (MDM_KEY_DEFAULT_FACE),
                   mdm_config_get_int (MDM_KEY_MAX_ICON_WIDTH),
                   mdm_config_get_int (MDM_KEY_MAX_ICON_HEIGHT));

    if (! defface) {
        mdm_common_warning ("Could not open DefaultImage: %s.  Suspending face browser!",
            mdm_config_get_string (MDM_KEY_DEFAULT_FACE));
    } else  {
        mdm_users_init (&users, &users_string, NULL, defface,
                &size_of_users, login_is_local, !DOING_MDM_DEVELOPMENT);
    }

	webkit_init();
			
    mdm_login_gui_init ();
	

    ve_signal_add (SIGHUP, mdm_reread_config, NULL);

    hup.sa_handler = ve_signal_notify;
    hup.sa_flags = 0;
    sigemptyset (&hup.sa_mask);
    sigaddset (&hup.sa_mask, SIGCHLD);

    if G_UNLIKELY (sigaction (SIGHUP, &hup, NULL) < 0) {
	    mdm_common_fail_greeter (_("%s: Error setting up %s signal handler: %s"), "main",
		"HUP", strerror (errno));
    }

    term.sa_handler = mdm_login_done;
    term.sa_flags = 0;
    sigemptyset (&term.sa_mask);
    sigaddset (&term.sa_mask, SIGCHLD);

    if G_UNLIKELY (sigaction (SIGINT, &term, NULL) < 0) {
	    mdm_common_fail_greeter (_("%s: Error setting up %s signal handler: %s"), "main",
		"INT", strerror (errno));
    }

    if G_UNLIKELY (sigaction (SIGTERM, &term, NULL) < 0) {
	    mdm_common_fail_greeter (_("%s: Error setting up %s signal handler: %s"), "main",
		"TERM", strerror (errno));
    }

    sigemptyset (&mask);
    sigaddset (&mask, SIGTERM);
    sigaddset (&mask, SIGHUP);
    sigaddset (&mask, SIGINT);
    
    if G_UNLIKELY (sigprocmask (SIG_UNBLOCK, &mask, NULL) == -1) {
	    mdm_common_fail_greeter (_("Could not set signal mask!"));
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
	/* but don't reap Xnest flexis */
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

    mdm_common_pre_fetch_launch ();
    gtk_main ();

    return EXIT_SUCCESS;
}

/* EOF */
