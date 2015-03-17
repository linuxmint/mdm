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
#include <libgnomecanvas/libgnomecanvas.h>

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

#include "greeter.h"
#include "greeter_configuration.h"
#include "greeter_parser.h"
#include "greeter_geometry.h"
#include "greeter_item_clock.h"
#include "greeter_item_pam.h"
#include "greeter_item_ulist.h"
#include "greeter_item_customlist.h"
#include "greeter_item_capslock.h"
#include "greeter_item_timed.h"
#include "greeter_events.h"
#include "greeter_session.h"
#include "greeter_system.h"

gboolean DOING_MDM_DEVELOPMENT = FALSE;

GtkWidget *window;
GtkWidget *canvas;

gboolean MDM_IS_LOCAL          = FALSE;
static gboolean ignore_buttons = FALSE;
gboolean MdmHaltFound          = FALSE;
gboolean MdmRebootFound        = FALSE;
gboolean MdmSuspendFound       = FALSE;
gboolean MdmConfiguratorFound  = FALSE;

/* FIXME: hack */
GreeterItemInfo *welcome_string_info = NULL;
GreeterItemInfo *root = NULL;

extern gboolean session_dir_whacked_out;
extern gboolean require_quarter;
extern gint mdm_timed_delay;
extern GtkButton *gtk_ok_button;
extern GtkButton *gtk_start_again_button;

gboolean greeter_probably_login_prompt = FALSE;
static gboolean first_prompt = TRUE;

extern char       *current_session;

static void process_operation (guchar opcode, const gchar *args);

void
greeter_ignore_buttons (gboolean val)
{
   ignore_buttons = val;
}

static char *get_theme_file (const char *in, char **theme_dir);

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
    GreeterItemInfo *conversation_info;
    static GnomeCanvasItem *disabled_cover = NULL;

    /* Parse opcode */
    switch (op_code) {
    case MDM_SETLOGIN:
	/* somebody is trying to fool us this is the user that
	 * wants to log in, and well, we are the gullible kind */
	
	greeter_item_pam_set_user (args);
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
	if (gtk_ok_button != NULL)
                gtk_widget_set_sensitive (GTK_WIDGET (gtk_ok_button), FALSE);

	if (gtk_start_again_button != NULL)
                gtk_widget_set_sensitive (GTK_WIDGET (gtk_start_again_button), !first_prompt);

	first_prompt = FALSE;

	greeter_ignore_buttons (FALSE);

	greeter_item_pam_prompt (tmp, PW_ENTRY_SIZE, TRUE);
	g_free (tmp);
	break;

    case MDM_NOECHO:
	tmp = ve_locale_to_utf8 (args);

	greeter_probably_login_prompt = FALSE;

	if (gtk_ok_button != NULL)
                gtk_widget_set_sensitive (GTK_WIDGET (gtk_ok_button), FALSE);

	if (gtk_start_again_button != NULL)
                gtk_widget_set_sensitive (GTK_WIDGET (gtk_start_again_button), !first_prompt);

	first_prompt = FALSE;

	greeter_ignore_buttons (FALSE);
	greeter_item_pam_prompt (tmp, PW_ENTRY_SIZE, FALSE);
	g_free (tmp);

	break;

    case MDM_MSG:
	tmp = ve_locale_to_utf8 (args);
	greeter_item_pam_message (tmp);
	g_free (tmp);
	printf ("%c\n", STX);
	fflush (stdout);
	break;

    case MDM_ERRBOX:
	tmp = ve_locale_to_utf8 (args);
	greeter_item_pam_error (tmp);
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
	/* fall thru to reset */

    case MDM_RESETOK:

	if (gtk_ok_button != NULL)
                gtk_widget_set_sensitive (GTK_WIDGET (gtk_ok_button), FALSE);
	if (gtk_start_again_button != NULL)
                gtk_widget_set_sensitive (GTK_WIDGET (gtk_start_again_button), FALSE);

	first_prompt = TRUE;

	conversation_info = greeter_lookup_id ("pam-conversation");
	
	if (conversation_info)
	  {
	    tmp = ve_locale_to_utf8 (args);
	    g_object_set (G_OBJECT (conversation_info->item),
			  "text", tmp,
			  NULL);
	    g_free (tmp);
	  }

	printf ("%c\n", STX);
	fflush (stdout);
	greeter_ignore_buttons (FALSE);
        greeter_item_ulist_enable ();

	break;

    case MDM_QUIT:
	greeter_item_timed_stop ();

	if (require_quarter) {
		/* we should be now fine for focusing new windows */
		mdm_wm_focus_new_windows (TRUE);

		dlg = hig_dialog_new (NULL /* parent */,
                                      GTK_DIALOG_MODAL /* flags */,
                                      GTK_MESSAGE_INFO,
                                      GTK_BUTTONS_OK,
                                      /* translators:  This is a nice and evil eggie text, translate
                                       * to your favourite currency */
                                      _("Please insert 25 cents "
                                        "to log in."),
                                      "");
		mdm_wm_center_window (GTK_WINDOW (dlg));

		mdm_wm_no_login_focus_push ();
		gtk_dialog_run (GTK_DIALOG (dlg));
		gtk_widget_destroy (dlg);
		mdm_wm_no_login_focus_pop ();
	}

	greeter_item_pam_leftover_messages ();

	gdk_flush ();

	if (greeter_show_only_background (root)) {
		GdkPixbuf *background;
		int width, height;

		gtk_window_get_size (GTK_WINDOW (window), &width, &height);
		background = gdk_pixbuf_get_from_drawable (NULL, gtk_widget_get_root_window(window), NULL, 0, 0, 0, 0 ,width, height);
		if (background) {
			mdm_common_set_root_background (background);
			g_object_unref (background);
		}
	}

	printf ("%c\n", STX);
	fflush (stdout);

	/* screw gtk_main_quit, we want to make sure we definately die */
	_exit (EXIT_SUCCESS);
	break;

    case MDM_STARTTIMER:
	greeter_item_timed_start ();
	
	printf ("%c\n", STX);
	fflush (stdout);
	break;

    case MDM_STOPTIMER:
	greeter_item_timed_stop ();

	printf ("%c\n", STX);
	fflush (stdout);
	break;

    case MDM_DISABLE:
	gtk_widget_set_sensitive (window, FALSE);

	if (disabled_cover == NULL)
	  {
	    disabled_cover = gnome_canvas_item_new
		    (gnome_canvas_root (GNOME_CANVAS (canvas)),
		     GNOME_TYPE_CANVAS_RECT,
		     "x1", 0.0,
		     "y1", 0.0,
		     "x2", (double)canvas->allocation.width,
		     "y2", (double)canvas->allocation.height,
		     "fill_color_rgba", (guint)0x00000088,
		     NULL);
	  }

	printf ("%c\n", STX);
	fflush (stdout);
	break;

    case MDM_ENABLE:
	gtk_widget_set_sensitive (window, TRUE);

	if (disabled_cover != NULL)
	  {
	    gtk_object_destroy (GTK_OBJECT (disabled_cover));
	    disabled_cover = NULL;
	  }

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

/*
 * The buttons with these handlers never appear in the F10 menu,
 * so they can make use of callback data.
 */
static void
greeter_ok_handler (GreeterItemInfo *info,
                    gpointer         user_data)
{
   if (ignore_buttons == FALSE)
     {
       GreeterItemInfo *entry_info = greeter_lookup_id ("user-pw-entry");
       if (entry_info && entry_info->item &&
           GNOME_IS_CANVAS_WIDGET (entry_info->item) &&
           GTK_IS_ENTRY (GNOME_CANVAS_WIDGET (entry_info->item)->widget))
         {
           GtkWidget *entry;
           entry = GNOME_CANVAS_WIDGET (entry_info->item)->widget;
           greeter_ignore_buttons (TRUE);
           greeter_item_pam_login (GTK_ENTRY (entry), entry_info);
         }
    }
}

static void
greeter_cancel_handler (GreeterItemInfo *info,
                        gpointer         user_data)
{
   if (ignore_buttons == FALSE)
     {
       greeter_item_ulist_unset_selected_user ();
       greeter_item_ulist_disable ();
       greeter_ignore_buttons (TRUE);
       printf ("%c%c%c\n", STX, BEL, MDM_INTERRUPT_CANCEL);
       fflush (stdout);
     }
}

static void
greeter_language_handler (GreeterItemInfo *info,
                          gpointer         user_data)
{
  mdm_lang_handler (user_data);
}

static void
greeter_setup_items (void)
{
  greeter_item_clock_setup ();
  greeter_item_pam_setup ();

  /* This will query the daemon for pictures through stdin/stdout! */
  greeter_item_ulist_setup ();

  greeter_item_capslock_setup (window);
  greeter_item_timed_setup ();
  greeter_item_register_action_callback ("ok_button",
					 greeter_ok_handler,
					 (gpointer) window);
  greeter_item_register_action_callback ("cancel_button",
					 greeter_cancel_handler,
					 (gpointer) window);
  greeter_item_register_action_callback ("language_button",
					 greeter_language_handler,
					 NULL);
  greeter_item_register_action_callback ("disconnect_button",
					 (ActionFunc)gtk_main_quit,
					 NULL);
  greeter_item_system_setup ();
  greeter_item_session_setup ();

  /* Setup the custom widgets */
  greeter_item_customlist_setup ();
}

static void
mdm_set_welcomemsg (void)
{
	char *welcomemsg = mdm_common_get_welcomemsg ();

	if (welcome_string_info->data.text.orig_text != NULL)
		g_free (welcome_string_info->data.text.orig_text);

	welcome_string_info->data.text.orig_text = welcomemsg;
	greeter_item_update_text (welcome_string_info);
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
	
	mdmcomm_open_connection_to_daemon ();

	/*
	 * Read all the keys at once and close sockets connection so we do
	 * not have to keep the socket open.
	 */
	mdm_config_get_string (MDM_KEY_GRAPHICAL_THEME);
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
	mdm_config_get_string (MDM_KEY_BACKGROUND_COLOR);
	mdm_config_get_string (MDM_KEY_DEFAULT_FACE);
	mdm_config_get_string (MDM_KEY_DEFAULT_SESSION);
	mdm_config_get_string (MDM_KEY_SOUND_PROGRAM);
	mdm_config_get_string (MDM_KEY_SOUND_ON_LOGIN_FILE);
	mdm_config_get_string (MDM_KEY_USE_24_CLOCK);
	mdm_config_get_string (MDM_KEY_WELCOME);
        mdm_config_get_string (MDM_KEY_RBAC_SYSTEM_COMMAND_KEYS);
        mdm_config_get_string (MDM_KEY_SYSTEM_COMMANDS_IN_MENU);

	mdm_config_get_string    (MDM_KEY_PRIMARY_MONITOR);
	mdm_config_get_int    (MDM_KEY_TIMED_LOGIN_DELAY);
	mdm_config_get_int    (MDM_KEY_FLEXI_REAP_DELAY_MINUTES);
	mdm_config_get_int    (MDM_KEY_MAX_ICON_HEIGHT);
	mdm_config_get_int    (MDM_KEY_MAX_ICON_WIDTH);
	mdm_config_get_int    (MDM_KEY_MINIMAL_UID);
	mdm_config_get_bool   (MDM_KEY_ENTRY_CIRCLES);
	mdm_config_get_bool   (MDM_KEY_ENTRY_INVISIBLE);
	mdm_config_get_bool   (MDM_KEY_INCLUDE_ALL);
	mdm_config_get_bool   (MDM_KEY_SYSTEM_MENU);
	mdm_config_get_bool   (MDM_KEY_CONFIG_AVAILABLE);
	mdm_config_get_bool   (MDM_KEY_TIMED_LOGIN_ENABLE);
	mdm_config_get_bool   (MDM_KEY_ALLOW_ROOT);
	mdm_config_get_bool   (MDM_KEY_SOUND_ON_LOGIN);
	mdm_config_get_bool   (MDM_KEY_DEFAULT_WELCOME);
	mdm_config_get_bool   (MDM_KEY_ADD_GTK_MODULES);		

	/* Keys not to include in reread_config */
	mdm_config_get_string (MDM_KEY_SESSION_DESKTOP_DIR);
	mdm_config_get_string (MDM_KEY_PRE_FETCH_PROGRAM);

	mdmcomm_close_connection_to_daemon ();
}

static gboolean
greeter_reread_config (int sig, gpointer data)
{
	gint i;
	gchar *key_string = NULL;
		
	mdmcomm_open_connection_to_daemon ();

	/* FIXME: The following is evil, we should update on the fly rather
	 * then just restarting */
	/* Also we may not need to check ALL those keys but just a few */
	if (mdm_config_reload_string (MDM_KEY_GRAPHICAL_THEME) ||	    
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
	    mdm_config_reload_string (MDM_KEY_BACKGROUND_COLOR) ||
	    mdm_config_reload_string (MDM_KEY_DEFAULT_FACE) ||
	    mdm_config_reload_string (MDM_KEY_DEFAULT_SESSION) ||
            mdm_config_reload_string (MDM_KEY_RBAC_SYSTEM_COMMAND_KEYS) ||
            mdm_config_reload_string (MDM_KEY_SYSTEM_COMMANDS_IN_MENU) ||

	    mdm_config_reload_string    (MDM_KEY_PRIMARY_MONITOR) ||
	    mdm_config_reload_int    (MDM_KEY_TIMED_LOGIN_DELAY) ||
	    mdm_config_reload_int    (MDM_KEY_FLEXI_REAP_DELAY_MINUTES) ||
	    mdm_config_reload_int    (MDM_KEY_MAX_ICON_HEIGHT) ||
	    mdm_config_reload_int    (MDM_KEY_MAX_ICON_WIDTH) ||
	    mdm_config_reload_int    (MDM_KEY_MINIMAL_UID) ||

	    mdm_config_reload_bool   (MDM_KEY_ENTRY_CIRCLES) ||
	    mdm_config_reload_bool   (MDM_KEY_ENTRY_INVISIBLE) ||
	    mdm_config_reload_bool   (MDM_KEY_INCLUDE_ALL) ||
	    mdm_config_reload_bool   (MDM_KEY_SYSTEM_MENU) ||
	    mdm_config_reload_bool   (MDM_KEY_CONFIG_AVAILABLE) ||
	    mdm_config_reload_bool   (MDM_KEY_TIMED_LOGIN_ENABLE) ||
	    mdm_config_reload_bool   (MDM_KEY_ALLOW_ROOT) ||
	    mdm_config_reload_bool   (MDM_KEY_ADD_GTK_MODULES)) {

		/* Set busy cursor */
		mdm_common_setup_cursor (GDK_WATCH);

		mdm_wm_save_wm_order ();
		mdmcomm_close_connection_to_daemon ();

		_exit (DISPLAY_RESTARTGREETER);
	}	

	mdm_config_reload_string (MDM_KEY_SOUND_PROGRAM);
	mdm_config_reload_bool   (MDM_KEY_SOUND_ON_LOGIN);
	mdm_config_reload_string (MDM_KEY_SOUND_ON_LOGIN_FILE);
	mdm_config_reload_string (MDM_KEY_USE_24_CLOCK);

	if (mdm_config_reload_string (MDM_KEY_WELCOME) ||
	    mdm_config_reload_bool   (MDM_KEY_DEFAULT_WELCOME)) {

		mdm_set_welcomemsg ();

		/* Set busy cursor */
		mdm_common_setup_cursor (GDK_WATCH);

		mdm_wm_save_wm_order ();
		mdmcomm_close_connection_to_daemon ();

		_exit (DISPLAY_RESTARTGREETER);
	}

	mdmcomm_close_connection_to_daemon ();

	return TRUE;
}

static void
greeter_done (int sig)
{
    _exit (EXIT_SUCCESS);
}


static char *
get_theme_file (const char *in, char **theme_dir)
{
  char *file, *dir, *info, *s;

  if (in == NULL)
    in = "circles";

  *theme_dir = NULL;

  if (g_path_is_absolute (in))
    {
      dir = g_strdup (in);
    }
  else
    {
      dir = NULL;
      if (DOING_MDM_DEVELOPMENT)
        {
	  if (g_access (in, F_OK) == 0)
	    {
	      dir = g_strdup (in);
	    }
	  else
	    {
              dir = g_build_filename ("themes", in, NULL);
	      if (g_access (dir, F_OK) != 0)
	        {
	          g_free (dir);
	          dir = NULL;
	        }
	    }
	}
      if (dir == NULL)
        dir = g_build_filename (mdm_config_get_string (MDM_KEY_GRAPHICAL_THEME_DIR), in, NULL);
    }

  *theme_dir = dir;

  info = g_build_filename (dir, "GdmGreeterTheme.desktop", NULL);
  if (g_access (info, R_OK) != 0) {
	  g_debug ("Could not open %s. The file either doesn't exist or is not readable", info);
	  g_free (info);
	  info = g_build_filename (dir, "GdmGreeterTheme.info", NULL);
  }
  if (g_access (info, R_OK) != 0)
    {
      char *base = g_path_get_basename (in);
      /* just guess the name, we have no info about the theme at
       * this point */
      g_debug ("Could not open %s. The file either doesn't exist or is not readable", info);
      g_free (info);
      file = g_strdup_printf ("%s/%s.xml", dir, base);
      g_free (base);
      return file;
    }

  s    = mdm_get_theme_greeter (info, in);
  file = g_build_filename (dir, s, NULL);

  g_free (info);
  g_free (s);

  return file;
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

static gboolean
mdm_event (GSignalInvocationHint *ihint,
           guint                n_param_values,
           const GValue        *param_values,
           gpointer             data)
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

		GreeterItemInfo *entry_info = greeter_lookup_id ("user-pw-entry");
		if (entry_info && entry_info->item &&
		    GNOME_IS_CANVAS_WIDGET (entry_info->item) &&
		    GTK_IS_ENTRY (GNOME_CANVAS_WIDGET (entry_info->item)->widget))
		{
			GtkWidget *entry;
			entry = GNOME_CANVAS_WIDGET (entry_info->item)->widget;
			gtk_entry_set_text (GTK_ENTRY (entry), "");
		}
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
  char *theme_file;
  char *theme_dir;
  gchar *mdm_graphical_theme;
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

  mdm_wm_screen_init (mdm_config_get_string (MDM_KEY_PRIMARY_MONITOR)); 

  /* Load the background as early as possible so MDM does not leave  */
  /* the background unfilled.   The cursor should be a watch already */
  /* but just in case */
  bg_color = mdm_config_get_string (MDM_KEY_BACKGROUND_COLOR);  
  mdm_common_setup_background_color (bg_color);
  greeter_session_init ();
  mdm_lang_initialize_model (mdm_config_get_string (MDM_KEY_LOCALE_FILE));

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

  canvas = gnome_canvas_new_aa ();
  GTK_WIDGET_UNSET_FLAGS (canvas, GTK_CAN_FOCUS);
  gnome_canvas_set_scroll_region (GNOME_CANVAS (canvas),
				  0.0, 0.0,
				  (double) mdm_wm_screen.width,
				  (double) mdm_wm_screen.height);
  
  if (g_getenv ("MDM_THEME") != NULL)
     mdm_graphical_theme = g_strdup (g_getenv ("MDM_THEME"));  
  else
     mdm_graphical_theme = mdm_config_get_string (MDM_KEY_GRAPHICAL_THEME);


  	if G_LIKELY ( ! DOING_MDM_DEVELOPMENT) {
	    gtk_window_set_decorated (GTK_WINDOW (window), FALSE);
	    gtk_window_set_default_size (GTK_WINDOW (window), mdm_wm_screen.width, mdm_wm_screen.height);
	}
	else {
	    gtk_window_set_icon_name (GTK_WINDOW (window), "mdmsetup");
	    gtk_window_set_title (GTK_WINDOW (window), mdm_graphical_theme);
	    gtk_window_maximize (GTK_WINDOW (window));
	}

  gtk_container_add (GTK_CONTAINER (window), canvas);

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
  
  
  theme_file = get_theme_file (mdm_graphical_theme, &theme_dir);
  
  error = NULL;
  root = greeter_parse (theme_file, theme_dir,
			GNOME_CANVAS (canvas), 
			mdm_wm_screen.width,
			mdm_wm_screen.height,
			&error);

    if G_UNLIKELY (root == NULL)
      {
        GtkWidget *dialog;
	char *s;
	char *tmp;

        mdm_wm_init (0);
        mdm_wm_focus_new_windows (TRUE);
    
	tmp = ve_filename_to_utf8 (ve_sure_string (mdm_graphical_theme));
	s = g_strdup_printf (_("There was an error loading the "
			       "theme %s"), tmp);
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

	if (DOING_MDM_DEVELOPMENT)
	  {
	    exit (1);
	  }
      }

  if G_UNLIKELY (error)
    g_clear_error (&error);

  /* Try circles.xml */
  if G_UNLIKELY (root == NULL)
    {
      g_free (theme_file);
      g_free (theme_dir);
      theme_file = get_theme_file ("circles", &theme_dir);
      root = greeter_parse (theme_file, theme_dir,
			    GNOME_CANVAS (canvas), 
			    mdm_wm_screen.width,
			    mdm_wm_screen.height,
			    NULL);
    }

  g_free (theme_file);

  if G_UNLIKELY (root != NULL && greeter_lookup_id ("user-pw-entry") == NULL)
    {
      GtkWidget *dialog;

      mdm_wm_init (0);
      mdm_wm_focus_new_windows (TRUE);
    
      dialog = hig_dialog_new (NULL /* parent */,
                               GTK_DIALOG_MODAL /* flags */,
                               GTK_MESSAGE_ERROR,
                               GTK_BUTTONS_OK,
                               _("The greeter theme is corrupt"),
                               _("The theme does not contain "
                                 "definition for the username/password "
                                 "entry element."));

      gtk_widget_show_all (dialog);
      mdm_wm_center_window (GTK_WINDOW (dialog));

      mdm_common_setup_cursor (GDK_LEFT_PTR);

      gtk_dialog_run (GTK_DIALOG (dialog));
      gtk_widget_destroy (dialog);

      root = NULL;
    }

  /* FIXME: beter information should be printed */
  if G_UNLIKELY (DOING_MDM_DEVELOPMENT && root == NULL)
    {
      g_warning ("No theme could be loaded");
      exit (1);
    }

  if G_UNLIKELY (root == NULL)
    {
      GtkWidget *dialog;

      mdm_wm_init (0);
      mdm_wm_focus_new_windows (TRUE);
    
      dialog = hig_dialog_new (NULL /* parent */,
                               GTK_DIALOG_MODAL /* flags */,
                               GTK_MESSAGE_ERROR,
                               GTK_BUTTONS_OK,
                               _("There was an error loading the "
                                 "theme, and the default theme "
                                 "could not be loaded. "
                                 "Attempting to start the "
                                 "standard greeter"),
                               "");
    
      gtk_widget_show_all (dialog);
      mdm_wm_center_window (GTK_WINDOW (dialog));

      mdm_common_setup_cursor (GDK_LEFT_PTR);
    
      gtk_dialog_run (GTK_DIALOG (dialog));
      gtk_widget_destroy (dialog);

      execl (LIBEXECDIR "/mdmlogin", LIBEXECDIR "/mdmlogin", NULL);
      execlp ("mdmlogin", "mdmlogin", NULL);

      dialog = hig_dialog_new (NULL /* parent */,
                               GTK_DIALOG_MODAL /* flags */,
                               GTK_MESSAGE_ERROR,
                               GTK_BUTTONS_OK,
                               _("The GTK+ greeter could not be started.  "
                                 "This display will abort and you may "
                                 "have to login another way and fix the "
                                 "installation of MDM"),
                               "");
    
      gtk_widget_show_all (dialog);
      mdm_wm_center_window (GTK_WINDOW (dialog));

      mdm_common_setup_cursor (GDK_LEFT_PTR);
    
      gtk_dialog_run (GTK_DIALOG (dialog));
      gtk_widget_destroy (dialog);

      _exit (DISPLAY_ABORT);
    }

  greeter_layout (root, GNOME_CANVAS (canvas));
  
  greeter_setup_items ();

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

  greeter_item_ulist_unset_selected_user ();
  greeter_item_ulist_enable ();
  greeter_item_ulist_check_show_userlist ();

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
      /* but don't reap nested flexis */
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
  g_signal_add_emission_hook (sid,
                              0 /* detail */,
                              mdm_event,
                              NULL /* data */,
                              NULL /* destroy_notify */);

  mdm_wm_restore_wm_order ();

  mdm_wm_show_info_msg_dialog (mdm_config_get_string (MDM_KEY_INFO_MSG_FILE),
	mdm_config_get_string (MDM_KEY_INFO_MSG_FONT));

  mdm_common_setup_cursor (GDK_LEFT_PTR);
  mdm_wm_center_cursor ();
  mdm_common_pre_fetch_launch ();
  gtk_main ();

  return 0;
}
