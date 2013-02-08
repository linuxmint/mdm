/* MDM - The GNOME Display Manager
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

#include <libintl.h>
#include <locale.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>

#if HAVE_PAM
#include <security/pam_appl.h>
#define PW_ENTRY_SIZE PAM_MAX_RESP_SIZE
#else
#define PW_ENTRY_SIZE MDM_MAX_PASS
#endif

#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>

#include "mdm-common.h"
#include "mdm-socket-protocol.h"
#include "mdm-daemon-config-keys.h"

#include "mdm.h"
#include "mdmwm.h"
#include "mdmcomm.h"
#include "mdmcommon.h"
#include "mdmconfig.h"
#include "mdmsession.h"
#include "mdmlanguages.h"

#include <webkit/webkit.h>

gboolean DOING_MDM_DEVELOPMENT = FALSE;

GtkWidget *window;
WebKitWebView *webView;

gboolean MDM_IS_LOCAL          = FALSE;
gboolean MdmHaltFound          = FALSE;
gboolean MdmRebootFound        = FALSE;
gboolean *MdmCustomCmdsFound   = NULL;
gboolean MdmAnyCustomCmdsFound = FALSE;
gboolean MdmSuspendFound       = FALSE;
gboolean MdmConfiguratorFound  = FALSE;

extern gboolean session_dir_whacked_out;
extern gint mdm_timed_delay;

gboolean greeter_probably_login_prompt = FALSE;
static gboolean first_prompt = TRUE;

static void process_operation (guchar opcode, const gchar *args);

gboolean webkit_on_message(WebKitWebView* view, WebKitWebFrame* frame, const gchar* message)
{    
    gchar ** message_parts = g_strsplit (message, "###", -1);
    gchar * command = message_parts[0];
    if (strcmp(command, "LOGIN") == 0) {
		// Perform the login
		
		// If the username/password is incorrect	
		webkit_web_view_execute_script(view, "reinit()");
		webkit_web_view_execute_script(view, g_strdup_printf("show_error('%s')", _("Wrong username or password")));		
	}
	else {		
		printf("Unknown command received from Webkit greeter: %s\n", command);
	}    
    return TRUE;
}

void webkit_on_loaded(WebKitWebView* view, WebKitWebFrame* frame)
{    
    webkit_web_view_execute_script(view, "init()");
    webkit_web_view_execute_script(view, "clear_errors()");   
    mdm_common_login_sound (mdm_config_get_string (MDM_KEY_SOUND_PROGRAM),
					mdm_config_get_string (MDM_KEY_SOUND_ON_LOGIN_FILE),
					mdm_config_get_bool   (MDM_KEY_SOUND_ON_LOGIN));
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


static gboolean
greeter_ctrl_handler (GIOChannel *source,
		      GIOCondition cond,
		      gint fd)
{
    gchar buf[PIPE_SIZE];
    gchar *p;
    gsize len;

    /* If this is not incoming i/o then return */
    if (cond != G_IO_IN) 
      return TRUE;

    /* Read random garbage from i/o channel until first STX is found */
    do {
      g_io_channel_read_chars (source, buf, 1, &len, NULL);
      
      if (len != 1)
	return TRUE;
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
process_operation (guchar       op_code,
		   const gchar *args)
{		
    GtkWidget *dlg;
    char *tmp;
    char *session;    
    gint lookup_status = SESSION_LOOKUP_SUCCESS;
    gchar *firstmsg = NULL;
    gchar *secondmsg = NULL;
    gint dont_save_session = GTK_RESPONSE_YES;

    /* Parse opcode */
    switch (op_code) {
    case MDM_SETLOGIN:
	/* somebody is trying to fool us this is the user that
	 * wants to log in, and well, we are the gullible kind */
	printf ("%c\n", STX);
	fflush (stdout);
	break;

    case MDM_PROMPT:
	tmp = ve_locale_to_utf8 (args);
	if (tmp != NULL && strcmp (tmp, _("Username:")) == 0) {
		mdm_common_login_sound (mdm_config_get_string (MDM_KEY_SOUND_PROGRAM),
					mdm_config_get_string (MDM_KEY_SOUND_ON_LOGIN_FILE),
					mdm_config_get_bool (MDM_KEY_SOUND_ON_LOGIN));
		greeter_probably_login_prompt = TRUE;
	}	

	first_prompt = FALSE;

	g_free (tmp);
	break;

    case MDM_NOECHO:
	tmp = ve_locale_to_utf8 (args);

	greeter_probably_login_prompt = FALSE;
	
	first_prompt = FALSE;
	
	g_free (tmp);

	break;

    case MDM_MSG:
	tmp = ve_locale_to_utf8 (args);	
	g_free (tmp);
	printf ("%c\n", STX);
	fflush (stdout);
	break;

    case MDM_ERRBOX:
	tmp = ve_locale_to_utf8 (args);	
	g_free (tmp);
	
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
	g_free (session);
	break;
    
    case MDM_SSESS:
	if (mdm_get_save_session () == GTK_RESPONSE_NO)
	  printf ("%cY\n", STX);
	else
	  printf ("%c\n", STX);
	fflush (stdout);
	
	break;

    case MDM_RESET:
	/* fall thru to reset */

    case MDM_RESETOK:
	
	first_prompt = TRUE;

	printf ("%c\n", STX);
	fflush (stdout);	
	break;

    case MDM_QUIT:
	gdk_flush ();
	
	printf ("%c\n", STX);
	fflush (stdout);

	/* screw gtk_main_quit, we want to make sure we definately die */
	_exit (0);
	break;

    case MDM_STARTTIMER:	
	printf ("%c\n", STX);
	fflush (stdout);
	break;

    case MDM_STOPTIMER:
	printf ("%c\n", STX);
	fflush (stdout);
	break;

    case MDM_DISABLE:
	gtk_widget_set_sensitive (window, FALSE);	
	printf ("%c\n", STX);
	fflush (stdout);
	break;

    case MDM_ENABLE:
	gtk_widget_set_sensitive (window, TRUE);	

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

	_exit (0);

    case MDM_QUERY_CAPSLOCK:	
	fflush (stdout);

	break;
	
    default:
	mdm_common_fail_greeter ("Unexpected greeter command received: '%c'", op_code);
	break;
    }
}

static gboolean
key_press_event (GtkWidget *widget, GdkEventKey *key, gpointer data)
{
  if (key->keyval == GDK_Escape)
    {
      if (DOING_MDM_DEVELOPMENT)
        process_operation (MDM_QUIT, NULL);
      else
      {
        printf ("%c%c%c\n", STX, BEL, MDM_INTERRUPT_CANCEL);
        fflush (stdout);
      }

      return TRUE;
    }
  
  return FALSE;
}

enum {
	RESPONSE_RESTART,
	RESPONSE_REBOOT,
	RESPONSE_CLOSE
};

static int
verify_mdm_version (void)
{
  const char *mdm_version;
  const char *mdm_protocol_version;
  
  mdm_version = g_getenv ("MDM_VERSION");
  mdm_protocol_version = g_getenv ("MDM_GREETER_PROTOCOL_VERSION");

  if (! DOING_MDM_DEVELOPMENT &&
      ((mdm_protocol_version != NULL &&
	strcmp (mdm_protocol_version, MDM_GREETER_PROTOCOL_VERSION) != 0) ||
       (mdm_protocol_version == NULL &&
	(mdm_version == NULL ||
	 strcmp (mdm_version, VERSION) != 0))) &&
      (g_getenv ("MDM_IS_LOCAL") != NULL))
    {
      GtkWidget *dialog;
      gchar *msg;
    
      mdm_wm_init (0);
      mdm_wm_focus_new_windows (TRUE);
    
      msg =  g_strdup_printf (_("The greeter version (%s) does not match the daemon "
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
      gtk_widget_destroy (dialog);
    
      return 0;
    }
  
  if (! DOING_MDM_DEVELOPMENT &&
      mdm_protocol_version == NULL &&
      mdm_version == NULL)
    {
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
			      _("Restart Machine"),
			      RESPONSE_REBOOT,
			      GTK_STOCK_CLOSE,
			      RESPONSE_CLOSE,
			      NULL);
    
      gtk_widget_show_all (dialog);
      mdm_wm_center_window (GTK_WINDOW (dialog));
      
      mdm_common_setup_cursor (GDK_LEFT_PTR);

      switch (gtk_dialog_run (GTK_DIALOG (dialog)))
	{
	case RESPONSE_REBOOT:
	  gtk_widget_destroy (dialog);
	  return DISPLAY_REBOOT;
	default:
	  gtk_widget_destroy (dialog);
	  return DISPLAY_ABORT;
	}
    }
  
  if (! DOING_MDM_DEVELOPMENT &&
      ((mdm_protocol_version != NULL &&
	strcmp (mdm_protocol_version, MDM_GREETER_PROTOCOL_VERSION) != 0) ||
       (mdm_protocol_version == NULL &&
	strcmp (mdm_version, VERSION) != 0)))
    {
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
			      _("Restart Machine"),
			      RESPONSE_REBOOT,
			      GTK_STOCK_CLOSE,
			      RESPONSE_CLOSE,
			      NULL);
      
      
      gtk_widget_show_all (dialog);
      mdm_wm_center_window (GTK_WINDOW (dialog));
    
      gtk_dialog_set_default_response (GTK_DIALOG (dialog), RESPONSE_RESTART);
      
      mdm_common_setup_cursor (GDK_LEFT_PTR);

      switch (gtk_dialog_run (GTK_DIALOG (dialog)))
	{
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
  
  return 0;
}

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
	mdm_config_get_string (MDM_KEY_GRAPHICAL_THEME);
	mdm_config_get_string (MDM_KEY_GRAPHICAL_THEMES);
	mdm_config_get_string (MDM_KEY_GRAPHICAL_THEME_DIR);
	mdm_config_get_string (MDM_KEY_GTKRC);
	mdm_config_get_string (MDM_KEY_GTK_THEME);
	mdm_config_get_string (MDM_KEY_INCLUDE);
	mdm_config_get_string (MDM_KEY_EXCLUDE);
	mdm_config_get_string (MDM_KEY_SESSION_DESKTOP_DIR);
	mdm_config_get_string (MDM_KEY_LOCALE_FILE);
	mdm_config_get_string (MDM_KEY_HALT);
	mdm_config_get_string (MDM_KEY_REBOOT);
	mdm_config_get_string (MDM_KEY_SUSPEND);
	mdm_config_get_string (MDM_KEY_CONFIGURATOR);
	mdm_config_get_string (MDM_KEY_INFO_MSG_FILE);
	mdm_config_get_string (MDM_KEY_INFO_MSG_FONT);
	mdm_config_get_string (MDM_KEY_TIMED_LOGIN);
	mdm_config_get_string (MDM_KEY_GRAPHICAL_THEMED_COLOR);
	mdm_config_get_string (MDM_KEY_BACKGROUND_COLOR);
	mdm_config_get_string (MDM_KEY_DEFAULT_FACE);
	mdm_config_get_string (MDM_KEY_DEFAULT_SESSION);
	mdm_config_get_string (MDM_KEY_SOUND_PROGRAM);
	mdm_config_get_string (MDM_KEY_SOUND_ON_LOGIN_FILE);
	mdm_config_get_string (MDM_KEY_USE_24_CLOCK);
	mdm_config_get_string (MDM_KEY_WELCOME);
	mdm_config_get_string (MDM_KEY_REMOTE_WELCOME);
        mdm_config_get_string (MDM_KEY_RBAC_SYSTEM_COMMAND_KEYS);
        mdm_config_get_string (MDM_KEY_SYSTEM_COMMANDS_IN_MENU);

	mdm_config_get_int    (MDM_KEY_XINERAMA_SCREEN);
	mdm_config_get_int    (MDM_KEY_TIMED_LOGIN_DELAY);
	mdm_config_get_int    (MDM_KEY_FLEXI_REAP_DELAY_MINUTES);
	mdm_config_get_int    (MDM_KEY_MAX_ICON_HEIGHT);
	mdm_config_get_int    (MDM_KEY_MAX_ICON_WIDTH);
	mdm_config_get_int    (MDM_KEY_MINIMAL_UID);
	mdm_config_get_bool   (MDM_KEY_ENTRY_CIRCLES);
	mdm_config_get_bool   (MDM_KEY_ENTRY_INVISIBLE);
	mdm_config_get_bool   (MDM_KEY_SHOW_XTERM_FAILSAFE);
	mdm_config_get_bool   (MDM_KEY_SHOW_GNOME_FAILSAFE);
	mdm_config_get_bool   (MDM_KEY_INCLUDE_ALL);
	mdm_config_get_bool   (MDM_KEY_SYSTEM_MENU);
	mdm_config_get_bool   (MDM_KEY_CONFIG_AVAILABLE);
	mdm_config_get_bool   (MDM_KEY_CHOOSER_BUTTON);
	mdm_config_get_bool   (MDM_KEY_TIMED_LOGIN_ENABLE);
	mdm_config_get_bool   (MDM_KEY_GRAPHICAL_THEME_RAND);
	mdm_config_get_bool   (MDM_KEY_SHOW_LAST_SESSION);
	mdm_config_get_bool   (MDM_KEY_ALLOW_ROOT);
	mdm_config_get_bool   (MDM_KEY_ALLOW_REMOTE_ROOT);
	mdm_config_get_bool   (MDM_KEY_SOUND_ON_LOGIN);
	mdm_config_get_bool   (MDM_KEY_DEFAULT_WELCOME);
	mdm_config_get_bool   (MDM_KEY_DEFAULT_REMOTE_WELCOME);
	mdm_config_get_bool   (MDM_KEY_ADD_GTK_MODULES);	

	/* Keys for custom commands */
	for (i = 0; i < MDM_CUSTOM_COMMAND_MAX; i++) {		
		key_string = g_strdup_printf("%s%d=", MDM_KEY_CUSTOM_CMD_TEMPLATE, i);
		mdm_config_get_string (key_string);
		g_free (key_string);
		
		key_string = g_strdup_printf("%s%d=", MDM_KEY_CUSTOM_CMD_LABEL_TEMPLATE, i);
		mdm_config_get_string (key_string);
		g_free (key_string);
		
		key_string = g_strdup_printf("%s%d=", MDM_KEY_CUSTOM_CMD_LR_LABEL_TEMPLATE, i);
		mdm_config_get_string (key_string);
		g_free (key_string);
		
		key_string = g_strdup_printf("%s%d=", MDM_KEY_CUSTOM_CMD_TEXT_TEMPLATE, i);
		mdm_config_get_string (key_string);
		g_free (key_string);
		
		key_string = g_strdup_printf("%s%d=", MDM_KEY_CUSTOM_CMD_TOOLTIP_TEMPLATE, i);
		mdm_config_get_string (key_string);
		g_free (key_string);
	}     

	/* Keys not to include in reread_config */
	mdm_config_get_string (MDM_KEY_SESSION_DESKTOP_DIR);
	mdm_config_get_string (MDM_KEY_PRE_FETCH_PROGRAM);

	mdmcomm_comm_bulk_stop ();
}

static gboolean
greeter_reread_config (int sig, gpointer data)
{
	gint i;
	gboolean custom_changed = FALSE;
	gchar *key_string = NULL;
	
	/* Read config data in bulk */
	mdmcomm_comm_bulk_start ();

	/* FIXME: The following is evil, we should update on the fly rather
	 * then just restarting */
	/* Also we may not need to check ALL those keys but just a few */
	if (mdm_config_reload_string (MDM_KEY_GRAPHICAL_THEME) ||
	    mdm_config_reload_string (MDM_KEY_GRAPHICAL_THEMES) ||
	    mdm_config_reload_string (MDM_KEY_GRAPHICAL_THEME_DIR) ||
	    mdm_config_reload_string (MDM_KEY_GTKRC) ||
	    mdm_config_reload_string (MDM_KEY_GTK_THEME) ||
	    mdm_config_reload_string (MDM_KEY_INCLUDE) ||
	    mdm_config_reload_string (MDM_KEY_EXCLUDE) ||
	    mdm_config_reload_string (MDM_KEY_SESSION_DESKTOP_DIR) ||
	    mdm_config_reload_string (MDM_KEY_LOCALE_FILE) ||
	    mdm_config_reload_string (MDM_KEY_HALT) ||
	    mdm_config_reload_string (MDM_KEY_REBOOT) ||
	    mdm_config_reload_string (MDM_KEY_SUSPEND) ||
	    mdm_config_reload_string (MDM_KEY_CONFIGURATOR) ||
	    mdm_config_reload_string (MDM_KEY_INFO_MSG_FILE) ||
	    mdm_config_reload_string (MDM_KEY_INFO_MSG_FONT) ||
	    mdm_config_reload_string (MDM_KEY_TIMED_LOGIN) ||
	    mdm_config_reload_string (MDM_KEY_GRAPHICAL_THEMED_COLOR) ||
	    mdm_config_reload_string (MDM_KEY_BACKGROUND_COLOR) ||
	    mdm_config_reload_string (MDM_KEY_DEFAULT_FACE) ||
	    mdm_config_reload_string (MDM_KEY_DEFAULT_SESSION) ||
            mdm_config_reload_string (MDM_KEY_RBAC_SYSTEM_COMMAND_KEYS) ||
            mdm_config_reload_string (MDM_KEY_SYSTEM_COMMANDS_IN_MENU) ||

	    mdm_config_reload_int    (MDM_KEY_XINERAMA_SCREEN) ||
	    mdm_config_reload_int    (MDM_KEY_TIMED_LOGIN_DELAY) ||
	    mdm_config_reload_int    (MDM_KEY_FLEXI_REAP_DELAY_MINUTES) ||
	    mdm_config_reload_int    (MDM_KEY_MAX_ICON_HEIGHT) ||
	    mdm_config_reload_int    (MDM_KEY_MAX_ICON_WIDTH) ||
	    mdm_config_reload_int    (MDM_KEY_MINIMAL_UID) ||

	    mdm_config_reload_bool   (MDM_KEY_ENTRY_CIRCLES) ||
	    mdm_config_reload_bool   (MDM_KEY_ENTRY_INVISIBLE) ||
	    mdm_config_reload_bool   (MDM_KEY_SHOW_XTERM_FAILSAFE) ||
	    mdm_config_reload_bool   (MDM_KEY_SHOW_GNOME_FAILSAFE) ||
	    mdm_config_reload_bool   (MDM_KEY_INCLUDE_ALL) ||
	    mdm_config_reload_bool   (MDM_KEY_SYSTEM_MENU) ||
	    mdm_config_reload_bool   (MDM_KEY_CONFIG_AVAILABLE) ||
	    mdm_config_reload_bool   (MDM_KEY_CHOOSER_BUTTON) ||
	    mdm_config_reload_bool   (MDM_KEY_TIMED_LOGIN_ENABLE) ||
	    mdm_config_reload_bool   (MDM_KEY_GRAPHICAL_THEME_RAND) ||
	    mdm_config_reload_bool   (MDM_KEY_SHOW_LAST_SESSION) ||
	    mdm_config_reload_bool   (MDM_KEY_ALLOW_ROOT) ||
	    mdm_config_reload_bool   (MDM_KEY_ALLOW_REMOTE_ROOT) ||
	    mdm_config_reload_bool   (MDM_KEY_ADD_GTK_MODULES)) {

		/* Set busy cursor */
		mdm_common_setup_cursor (GDK_WATCH);

		mdm_wm_save_wm_order ();
		mdmcomm_comm_bulk_stop ();

		_exit (DISPLAY_RESTARTGREETER);
	}

	for (i = 0; i < MDM_CUSTOM_COMMAND_MAX; i++) {		
		key_string = g_strdup_printf("%s%d=", MDM_KEY_CUSTOM_CMD_TEMPLATE, i);
		if (mdm_config_reload_string (key_string))
			custom_changed = TRUE;
		g_free (key_string);
		
		key_string = g_strdup_printf("%s%d=", MDM_KEY_CUSTOM_CMD_LABEL_TEMPLATE, i);
		if (mdm_config_reload_string (key_string))
			custom_changed = TRUE;
		g_free (key_string);
		
		key_string = g_strdup_printf("%s%d=", MDM_KEY_CUSTOM_CMD_LR_LABEL_TEMPLATE, i);
		if (mdm_config_reload_string (key_string))
			custom_changed = TRUE;
		g_free (key_string);
		
		key_string = g_strdup_printf("%s%d=", MDM_KEY_CUSTOM_CMD_TEXT_TEMPLATE, i);
		if (mdm_config_reload_string (key_string))
			custom_changed = TRUE;
		g_free (key_string);
		
		key_string = g_strdup_printf("%s%d=", MDM_KEY_CUSTOM_CMD_TOOLTIP_TEMPLATE, i);
		if (mdm_config_reload_string (key_string))
			custom_changed = TRUE;
		g_free (key_string);
	}     	
	
	if(custom_changed){
		/* Set busy cursor */
		mdm_common_setup_cursor (GDK_WATCH);
		
		mdm_wm_save_wm_order ();
		mdmcomm_comm_bulk_stop ();
		
		_exit (DISPLAY_RESTARTGREETER);
	}

	mdm_config_reload_string (MDM_KEY_SOUND_PROGRAM);
	mdm_config_reload_bool   (MDM_KEY_SOUND_ON_LOGIN);
	mdm_config_reload_string (MDM_KEY_SOUND_ON_LOGIN_FILE);
	mdm_config_reload_string (MDM_KEY_USE_24_CLOCK);

	if (mdm_config_reload_string (MDM_KEY_WELCOME) ||
	    mdm_config_reload_bool   (MDM_KEY_DEFAULT_WELCOME) ||
	    mdm_config_reload_string (MDM_KEY_REMOTE_WELCOME) ||
	    mdm_config_reload_bool   (MDM_KEY_DEFAULT_REMOTE_WELCOME)) {
		
		/* Set busy cursor */
		mdm_common_setup_cursor (GDK_WATCH);

		mdm_wm_save_wm_order ();
		mdmcomm_comm_bulk_stop ();

		_exit (DISPLAY_RESTARTGREETER);
	}

	mdmcomm_comm_bulk_stop ();

	return TRUE;
}

static void
greeter_done (int sig)
{
    _exit (0);
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

int
main (int argc, char *argv[])
{
  char *bg_color;
  struct sigaction hup;
  struct sigaction term;
  sigset_t mask;
  GIOChannel *ctrlch;
  GError *error;  
  const char *mdm_gtk_theme;
  guint sid;
  int r;
  gint i;
  gchar *key_string = NULL;

  if (g_getenv ("DOING_MDM_DEVELOPMENT") != NULL)
    DOING_MDM_DEVELOPMENT = TRUE;

  bindtextdomain (GETTEXT_PACKAGE, GNOMELOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  setlocale (LC_ALL, "");

  if (ve_string_empty (g_getenv ("MDM_IS_LOCAL")))
	   MDM_IS_LOCAL = FALSE;
  else
	   MDM_IS_LOCAL = TRUE;

  /*
   * mdm_common_atspi_launch () needs gdk initialized.
   * We cannot start gtk before the registry is running 
   * because the atk-bridge will crash.
   */
  gdk_init (&argc, &argv);
  if (! DOING_MDM_DEVELOPMENT) {
       mdm_common_atspi_launch ();
  }

  gtk_init (&argc, &argv);

  mdm_common_setup_cursor (GDK_WATCH);

  mdm_common_log_init ();
  mdm_common_log_set_debug (mdm_config_get_bool (MDM_KEY_DEBUG));

  mdm_common_setup_builtin_icons ();

  /* Read all configuration at once, so the values get cached */
  mdm_read_config ();

  if ( ! ve_string_empty (mdm_config_get_string (MDM_KEY_GTKRC)))
	  gtk_rc_parse (mdm_config_get_string (MDM_KEY_GTKRC));

  mdm_gtk_theme = g_getenv ("MDM_GTK_THEME");
  if (ve_string_empty (mdm_gtk_theme))
	  mdm_gtk_theme = mdm_config_get_string (MDM_KEY_GTK_THEME);

  if ( ! ve_string_empty (mdm_gtk_theme)) {
	  mdm_set_theme (mdm_gtk_theme);
  }

  mdm_wm_screen_init (mdm_config_get_int (MDM_KEY_XINERAMA_SCREEN));
  
  r = verify_mdm_version ();
  if (r != 0)
    return r;

  /* Load the background as early as possible so MDM does not leave  */
  /* the background unfilled.   The cursor should be a watch already */
  /* but just in case */
  bg_color = mdm_config_get_string (MDM_KEY_GRAPHICAL_THEMED_COLOR);
  /* If a graphical theme color does not exist fallback to the plain color */
  if (ve_string_empty (bg_color)) {
    bg_color = mdm_config_get_string (MDM_KEY_BACKGROUND_COLOR);
  }
  mdm_common_setup_background_color (bg_color);
  //mdm_lang_initialize_model (mdm_config_get_string (MDM_KEY_LOCALE_FILE));

  ve_signal_add (SIGHUP, greeter_reread_config, NULL);

  hup.sa_handler = ve_signal_notify;
  hup.sa_flags = 0;
  sigemptyset (&hup.sa_mask);
  sigaddset (&hup.sa_mask, SIGCHLD);
  
  if (sigaction (SIGHUP, &hup, NULL) < 0) {
    mdm_common_fail_greeter ("%s: Error setting up %s signal handler: %s", "main",
		"HUP", strerror (errno));
  }

  term.sa_handler = greeter_done;
  term.sa_flags = 0;
  sigemptyset (&term.sa_mask);
  sigaddset (&term.sa_mask, SIGCHLD);
  
  if G_UNLIKELY (sigaction (SIGINT, &term, NULL) < 0) {
    mdm_common_fail_greeter ("%s: Error setting up %s signal handler: %s", "main",
	"INT", strerror (errno));
  }
  
  if G_UNLIKELY (sigaction (SIGTERM, &term, NULL) < 0) {
    mdm_common_fail_greeter ("%s: Error setting up %s signal handler: %s", "main",
	"TERM", strerror (errno));
  }
  
  sigemptyset (&mask);
  sigaddset (&mask, SIGTERM);
  sigaddset (&mask, SIGHUP);
  sigaddset (&mask, SIGINT);

  if G_UNLIKELY (sigprocmask (SIG_UNBLOCK, &mask, NULL) == -1) {
	  mdm_common_fail_greeter ("Could not set signal mask!");
  }

  /* ignore SIGCHLD */
  sigemptyset (&mask);
  sigaddset (&mask, SIGCHLD);

  if G_UNLIKELY (sigprocmask (SIG_BLOCK, &mask, NULL) == -1) {
	  mdm_common_fail_greeter ("Could not set signal mask!");
  }
  
  window = gtk_window_new (GTK_WINDOW_TOPLEVEL);

  g_signal_connect (G_OBJECT (window), "key_press_event",
                    G_CALLBACK (key_press_event), NULL);

  webView = WEBKIT_WEB_VIEW(webkit_web_view_new());  
  //GTK_WIDGET_UNSET_FLAGS (GTK_WIDGET (webView), GTK_CAN_FOCUS); 
  gtk_window_set_decorated (GTK_WINDOW (window), FALSE);
  gtk_window_set_default_size (GTK_WINDOW (window), 
			       mdm_wm_screen.width, 
			       mdm_wm_screen.height);
  gtk_container_add (GTK_CONTAINER (window), webView);

 /*
  * Initialize the value with the default value so the first time it
  * is displayed it doesn't show as 0.  Also determine if the Halt,
  * Reboot, Suspend and Configurator commands work.
  */
  mdm_timed_delay         = mdm_config_get_int (MDM_KEY_TIMED_LOGIN_DELAY);
  MdmHaltFound            = mdm_working_command_exists (mdm_config_get_string (MDM_KEY_HALT));
  MdmRebootFound          = mdm_working_command_exists (mdm_config_get_string (MDM_KEY_REBOOT));
  MdmSuspendFound         = mdm_working_command_exists (mdm_config_get_string (MDM_KEY_SUSPEND));
  MdmConfiguratorFound    = mdm_working_command_exists (mdm_config_get_string (MDM_KEY_CONFIGURATOR));

  MdmCustomCmdsFound = g_new0 (gboolean, MDM_CUSTOM_COMMAND_MAX);
  for (i = 0; i < MDM_CUSTOM_COMMAND_MAX; i++) {
	  /*  For each possible custom command */      
	  key_string = g_strdup_printf("%s%d=", MDM_KEY_CUSTOM_CMD_TEMPLATE, i);
	  MdmCustomCmdsFound[i] = mdm_working_command_exists (mdm_config_get_string (key_string));
	  if (MdmCustomCmdsFound[i])
		  MdmAnyCustomCmdsFound = TRUE;

	  g_free (key_string);
  }
  
	char *html;
	gsize file_length;
	g_file_get_contents ("/usr/share/mdm/html-themes/mdm/index.html", &html, &file_length, NULL);    

	html = str_replace(html, "$username_label", _("Username"));
	html = str_replace(html, "$password_label", _("Password"));
	html = str_replace(html, "$login_label", _("Login"));
	html = str_replace(html, "$ok_label", _("OK"));
	html = str_replace(html, "$cancel_label", _("Cancel"));
	html = str_replace(html, "$enter_your_username_label", _("Please enter your username"));
	html = str_replace(html, "$enter_your_password_label", _("Please enter your password"));
	
	html = str_replace(html, "$hostname", g_get_host_name());

	// Load a web page into the browser instance
	webkit_web_view_load_string(webView, html, "text/html", "UTF-8", "file:///usr/share/mdm/html-themes/mdm/");

	g_signal_connect(G_OBJECT(webView), "script-alert", G_CALLBACK(webkit_on_message), 0);
	g_signal_connect(G_OBJECT(webView), "load-finished", G_CALLBACK(webkit_on_loaded), 0);
           
  if G_LIKELY (! DOING_MDM_DEVELOPMENT) {
    ctrlch = g_io_channel_unix_new (STDIN_FILENO);
    g_io_channel_set_encoding (ctrlch, NULL, NULL);
    g_io_channel_set_buffered (ctrlch, TRUE);
    g_io_channel_set_flags (ctrlch, 
			    g_io_channel_get_flags (ctrlch) | G_IO_FLAG_NONBLOCK,
			    NULL);
    g_io_add_watch (ctrlch, 
		    G_IO_IN | G_IO_PRI | G_IO_ERR | G_IO_HUP | G_IO_NVAL,
		    (GIOFunc) greeter_ctrl_handler,
		    NULL);
    g_io_channel_unref (ctrlch);
  }

  mdm_common_setup_blinking ();

  gtk_widget_show_all (window);
  gtk_window_move (GTK_WINDOW (window), mdm_wm_screen.x, mdm_wm_screen.y);
  gtk_widget_show_now (window);
  
  /* can it ever happen that it'd be NULL here ??? */
  if G_UNLIKELY (window->window != NULL)
    {
      mdm_wm_init (GDK_WINDOW_XWINDOW (window->window));

      /* Run the focus, note that this will work no matter what
       * since mdm_wm_init will set the display to the gdk one
       * if it fails */
      mdm_wm_focus_window (GDK_WINDOW_XWINDOW (window->window));
    }

  if G_UNLIKELY (session_dir_whacked_out)
    {
      GtkWidget *dialog;

      mdm_wm_focus_new_windows (TRUE);

      dialog = hig_dialog_new (NULL /* parent */,
                               GTK_DIALOG_MODAL /* flags */,
                               GTK_MESSAGE_ERROR,
                               GTK_BUTTONS_OK,
                               _("Session directory is missing"),
                               _("Your session directory is missing or empty!  "
                                 "There are two available sessions you can use, but "
                                 "you should log in and correct the mdm configuration."));
      gtk_widget_show_all (dialog);
      mdm_wm_center_window (GTK_WINDOW (dialog));

      mdm_common_setup_cursor (GDK_LEFT_PTR);

      mdm_wm_no_login_focus_push ();
      gtk_dialog_run (GTK_DIALOG (dialog));
      gtk_widget_destroy (dialog);
      mdm_wm_no_login_focus_pop ();
    }

  if G_UNLIKELY (g_getenv ("MDM_WHACKED_GREETER_CONFIG") != NULL)
    {
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

  /* if a flexiserver, reap self after some time */
  if (mdm_config_get_int (MDM_KEY_FLEXI_REAP_DELAY_MINUTES) > 0 &&
      ! ve_string_empty (g_getenv ("MDM_FLEXI_SERVER")) &&
      /* but don't reap Xnest flexis */
      ve_string_empty (g_getenv ("MDM_PARENT_DISPLAY")))
    {
      sid = g_signal_lookup ("activate",
			     GTK_TYPE_MENU_ITEM);
      g_signal_add_emission_hook (sid,
				  0 /* detail */,
				  delay_reaping,
				  NULL /* data */,
				  NULL /* destroy_notify */);

      sid = g_signal_lookup ("key_press_event",
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

  mdm_wm_restore_wm_order ();

  mdm_wm_show_info_msg_dialog (mdm_config_get_string (MDM_KEY_INFO_MSG_FILE),
	mdm_config_get_string (MDM_KEY_INFO_MSG_FONT));

  mdm_common_setup_cursor (GDK_LEFT_PTR);
  mdm_wm_center_cursor ();
  mdm_common_pre_fetch_launch ();
  gtk_main ();

  return 0;
}
