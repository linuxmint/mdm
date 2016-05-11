/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

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
#include "mdm-log.h"
#include "mdm-socket-protocol.h"
#include "mdm-daemon-config-keys.h"

#include <webkit/webkit.h>

gboolean DOING_MDM_DEVELOPMENT = FALSE;

static WebKitWebView *webView;
static gboolean webkit_ready = FALSE;
static gchar * mdm_msg = "";
static gchar *current_language;
static GtkWidget *login;
static guint err_box_clear_handler = 0;

static GdkPixbuf *defface;

/* Eew. Loads of global vars. It's hard to be event controlled while maintaining state */
static GList *users = NULL;
static GList *users_string = NULL;
static gint size_of_users = 0;

static gchar *session = NULL;

static guint timed_handler_id = 0;

extern GList *sessions;
extern GHashTable *sessnames;
extern gchar *default_session;
extern const gchar *current_session;
extern gint mdm_timed_delay;

enum {
    MDM_BACKGROUND_NONE = 0,
    MDM_BACKGROUND_IMAGE_AND_COLOR = 1,
    MDM_BACKGROUND_COLOR = 2,
    MDM_BACKGROUND_IMAGE = 3,
};

static void process_operation (guchar op_code, const gchar *args);
static gboolean mdm_login_ctrl_handler (GIOChannel *source, GIOCondition cond, gint fd);

static GHashTable *displays_hash = NULL;

static void check_for_displays (void) {
    char  *ret;
    char **vec;
    int    i;

    /*
     * Might be nice to move this call into read_config() so that it happens
     * on the same socket call as reading the configuration.
     */
    ret = mdmcomm_send_cmd_to_daemon (MDM_SUP_ATTACHED_SERVERS);
    if (ve_string_empty (ret) || strncmp (ret, "OK ", 3) != 0) {
        g_free (ret);
        return;
    }

    vec = g_strsplit (&ret[3], ";", -1);
    g_free (ret);
    if (vec == NULL) {
        return;
    }

    if (displays_hash == NULL) {
        displays_hash = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
    }

    for (i = 0; vec[i] != NULL; i++) {
        char **rvec;

        rvec = g_strsplit (vec[i], ",", -1);
        if (mdm_vector_len (rvec) != 3) {
            g_strfreev (rvec);
            continue;
        }

        g_hash_table_insert (displays_hash, g_strdup (rvec[1]), g_strdup (rvec[0]));

        g_strfreev (rvec);
    }

    g_strfreev (vec);
}

static gchar * str_replace(const char *string, const char *delimiter, const char *replacement) {
    gchar **split;
    gchar *ret;
    g_return_val_if_fail(string != NULL, NULL);
    g_return_val_if_fail(delimiter != NULL, NULL);
    g_return_val_if_fail(replacement != NULL, NULL);

    split = g_strsplit(string, delimiter, 0);
    ret = g_strjoinv(replacement, split);
    g_strfreev(split);
    return ret;
}

static char * html_encode(const char *string) {
    char * ret;
    ret = str_replace(string, "'", "&#39");
    ret = str_replace(ret, "\"", "&#34");
    ret = str_replace(ret, ";", "&#59");
    ret = str_replace(ret, "<", "&#60");
    ret = str_replace(ret, ">", "&#62");
    ret = str_replace(ret, "\n", "<br/>");
    return ret;
}

void webkit_execute_script(const gchar * function, const gchar * arguments) {
    if (webkit_ready) {
        gchar * tmp;

        if (arguments == NULL) {
            tmp = g_strdup_printf("if ((typeof %s) === 'function') { %s(); }", function, function);
        }
        else {
            tmp = g_strdup_printf("if ((typeof %s) === 'function') { %s(\"%s\"); }", function, function, str_replace(arguments, "\n", ""));
        }
        webkit_web_view_execute_script(webView, tmp);
        g_free (tmp);
    }
}

gboolean webkit_on_message(WebKitWebView *view, WebKitWebFrame *frame, gchar *message, gpointer user_data) {
    gchar ** message_parts = g_strsplit (message, "###", -1);
    gchar * command = message_parts[0];
    if (strcmp(command, "LOGIN") == 0) {
        char string[255];
        sscanf(message, "LOGIN###%255[^\n]s", string);
        printf ("%c%s\n", STX, string);
        fflush (stdout);
    }
    else if (strcmp(command, "LANGUAGE") == 0) {
        current_language = message_parts[1];
        //gchar *language = message_parts[1];
        //printf ("%c%c%c%c%s\n", STX, BEL, MDM_INTERRUPT_SELECT_LANG, 0, language);
        //fflush (stdout);
        //g_free (language);
    }
    else if (strcmp(command, "SESSION") == 0) {
        current_session = message_parts[2];
    }
    else if (strcmp(command, "SHUTDOWN") == 0) {
        if (mdm_wm_warn_dialog (_("Are you sure you want to shut down the computer?"), "", _("Shut _Down"), NULL, TRUE) == GTK_RESPONSE_YES) {
            _exit (DISPLAY_HALT);
        }
    }
    else if (strcmp(command, "SUSPEND") == 0) {
        if (mdm_wm_warn_dialog (_("Are you sure you want to suspend the computer?"), "", _("_Suspend"), NULL, TRUE) == GTK_RESPONSE_YES) {
            printf ("%c%c%c\n", STX, BEL, MDM_INTERRUPT_SUSPEND);
            fflush (stdout);
        }
    }
    else if (strcmp(command, "RESTART") == 0) {
        if (mdm_wm_warn_dialog (_("Are you sure you want to restart the computer?"), "", _("_Restart"), NULL, TRUE) == GTK_RESPONSE_YES) {
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
    else if (strcmp(command, "USER") == 0) {
        printf ("%c%c%c%s\n", STX, BEL, MDM_INTERRUPT_SELECT_USER, message_parts[1]);
        fflush (stdout);
    }
    else {
        //printf("Unknown command received from Webkit greeter: %s\n", command);
    }
    return TRUE;
}

gboolean webkit_on_console_message (WebKitWebView *web_view, gchar *message, gint line, gchar *source_id, gpointer user_data) {
    mdm_debug("webkit_on_console_message: line #%d '%s' '%s'.", line, source_id, message);
    return FALSE;
}

gboolean webkit_on_error (WebKitWebView *web_view, WebKitWebFrame *web_frame, gchar *uri, GError *error, gpointer user_data) {
    mdm_debug("webkit_on_error: '%s'.", error->message);
    return FALSE;
}

void webkit_on_resource_failed (WebKitWebView *web_view, WebKitWebFrame *web_frame, WebKitWebResource *web_resource, GError *error, gpointer user_data) {
    mdm_debug("webkit_on_resource_failed: '%s' '%s'.", webkit_web_resource_get_uri (web_resource), error->message);
}

void webkit_on_loaded(WebKitWebView *view, WebKitWebFrame *frame, gpointer user_data) {
    GIOChannel *ctrlch;
    webkit_ready = TRUE;
    mdm_common_login_sound (mdm_config_get_string (MDM_KEY_SOUND_PROGRAM), mdm_config_get_string (MDM_KEY_SOUND_ON_LOGIN_FILE), mdm_config_get_bool   (MDM_KEY_SOUND_ON_LOGIN));
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
    }

    if (!mdm_working_command_exists (mdm_config_get_string (MDM_KEY_SUSPEND)) || !mdm_common_is_action_available ("SUSPEND")) {
        webkit_execute_script("mdm_hide_suspend", NULL);
    }
    if (!mdm_working_command_exists (mdm_config_get_string (MDM_KEY_REBOOT)) || !mdm_common_is_action_available ("REBOOT")) {
        webkit_execute_script("mdm_hide_restart", NULL);
    }
    if (!mdm_working_command_exists (mdm_config_get_string (MDM_KEY_HALT)) || !mdm_common_is_action_available ("HALT")) {
        webkit_execute_script("mdm_hide_shutdown", NULL);
    }
    if (ve_string_empty (g_getenv ("MDM_FLEXI_SERVER")) && ve_string_empty (g_getenv ("MDM_IS_LOCAL"))) {
        webkit_execute_script("mdm_hide_quit", NULL);
    }

    char * current_lang = g_getenv("LANG");
    if (current_lang) {
        char *name;
        char *untranslated;
        if (mdm_common_locale_is_displayable (current_lang)) {
            name = mdm_lang_name (current_lang, FALSE, TRUE, FALSE, FALSE);

            untranslated = mdm_lang_untranslated_name (current_lang, TRUE);

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
        g_io_channel_set_flags (ctrlch, g_io_channel_get_flags (ctrlch) | G_IO_FLAG_NONBLOCK, NULL);
        g_io_add_watch (ctrlch, G_IO_IN | G_IO_PRI | G_IO_ERR | G_IO_HUP | G_IO_NVAL, (GIOFunc) mdm_login_ctrl_handler, NULL);
        g_io_channel_unref (ctrlch);
    }

    gtk_widget_show_all (GTK_WIDGET (login));
}

gboolean webkit_on_navigation_policy_decision_requested(WebKitWebView *web_view, WebKitWebFrame *frame, WebKitNetworkRequest *request, WebKitWebNavigationAction *navigation_action, WebKitWebPolicyDecision *policy_decision, gpointer user_data) {
    webkit_web_policy_decision_ignore (policy_decision);
    return TRUE;
}

static GtkWidget * hig_dialog_new (GtkWindow *parent, GtkDialogFlags flags, GtkMessageType type, GtkButtonsType buttons, const gchar *primary_message, const gchar *secondary_message) {
    GtkWidget *dialog;
    dialog = gtk_message_dialog_new (GTK_WINDOW (parent), GTK_DIALOG_DESTROY_WITH_PARENT, type, buttons, "%s", primary_message);
    gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog), "%s", secondary_message);
    gtk_window_set_title (GTK_WINDOW (dialog), "");
    gtk_container_set_border_width (GTK_CONTAINER (dialog), 5);
    gtk_box_set_spacing (GTK_BOX (GTK_DIALOG (dialog)->vbox), 14);
    return dialog;
}

static gboolean mdm_timer (gpointer data) {
    if (mdm_timed_delay <= 0) {
        printf ("%c%c%c\n", STX, BEL, MDM_INTERRUPT_TIMED_LOGIN);
        fflush (stdout);
    } else {
        gchar *autologin_msg;
        autologin_msg = mdm_common_expand_text (_("User %u will login in %t"));
        webkit_execute_script("mdm_timed", autologin_msg);
        g_free (autologin_msg);
    }
    mdm_timed_delay--;
    return TRUE;
}

 // Timed Login: On GTK events, increase delay to at least 30 seconds, or the MDM_KEY_TIMED_LOGIN_DELAY, whichever is higher.
static gboolean mdm_timer_up_delay (GSignalInvocationHint *ihint, guint n_param_values, const GValue *param_values, gpointer data) {
    if (mdm_timed_delay < 30) {
        mdm_timed_delay = 30;
    }
    if (mdm_timed_delay < mdm_config_get_int (MDM_KEY_TIMED_LOGIN_DELAY)) {
        mdm_timed_delay = mdm_config_get_int (MDM_KEY_TIMED_LOGIN_DELAY);
    }
    return TRUE;
}

// The reaping stuff
static time_t last_reap_delay = 0;

static gboolean delay_reaping (GSignalInvocationHint *ihint, guint  n_param_values, const GValue *param_values, gpointer data) {
    last_reap_delay = time (NULL);
    return TRUE;
}

static gboolean reap_flexiserver (gpointer data) {
    int reapminutes = mdm_config_get_int (MDM_KEY_FLEXI_REAP_DELAY_MINUTES);
    if (reapminutes > 0 && ((time (NULL) - last_reap_delay) / 60) > reapminutes) {
        _exit (DISPLAY_REMANAGE);
    }
    return TRUE;
}

static void mdm_login_done (int sig) {
    _exit (EXIT_SUCCESS);
}

void mdm_login_session_init () {
    GSList *sessgrp = NULL;
    GList *tmp;
    int num = 1;
    char *label;

    current_session = NULL;

    for (tmp = sessions; tmp != NULL; tmp = tmp->next) {
        MdmSession *session;
        char *file;

        file = (char *) tmp->data;
        session = g_hash_table_lookup (sessnames, file);

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
                gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (w), TRUE);
                break;
            }
            tmp = tmp->next;
        }
    }
}

void mdm_login_lang_init (gchar * locale_file) {
    GList *list, *li;
    list = mdm_lang_read_locale_file (locale_file);

    for (li = list; li != NULL; li = li->next) {
        char *lang = li->data;
        char *name;
        char *untranslated;

        li->data = NULL;

        if (!mdm_common_locale_is_displayable (lang)) {
            g_free (lang);
            continue;
        }

        name = mdm_lang_name (lang, FALSE, TRUE, FALSE, FALSE);

        untranslated = mdm_lang_untranslated_name (lang, TRUE);

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

static gboolean err_box_clear (gpointer data) {
    webkit_execute_script("mdm_error", "");
    err_box_clear_handler = 0;
    return FALSE;
}

static gboolean greeter_is_capslock_on (void) {
    XkbStateRec states;
    Display *dsp;
    dsp = GDK_DISPLAY ();
    if (XkbGetState (dsp, XkbUseCoreKbd, &states) != Success) {
      return FALSE;
    }
    return (states.locked_mods & LockMask) != 0;
}

static gboolean mdm_login_ctrl_handler (GIOChannel *source, GIOCondition cond, gint fd) {
    gchar buf[PIPE_SIZE];
    gchar *p;
    gsize len;

    /* If this is not incoming i/o then return */
    if (cond != G_IO_IN) {
        return (TRUE);
    }

    /* Read random garbage from i/o channel until STX is found */
    do {
        g_io_channel_read_chars (source, buf, 1, &len, NULL);

        if (len != 1) {
            return (TRUE);
        }
    } while (buf[0] && buf[0] != STX);

    memset (buf, '\0', sizeof (buf));

    if (g_io_channel_read_chars (source, buf, sizeof (buf) - 1, &len, NULL) != G_IO_STATUS_NORMAL) {
      return TRUE;
    }

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

void process_operation (guchar op_code, const gchar *args) {
    char *tmp;
    gint i, x, y;
    GtkWidget *dlg;
    static gboolean replace_msg = TRUE;
    static gboolean messages_to_give = FALSE;
    switch (op_code) {

        case MDM_SETLOGIN:
            webkit_execute_script("mdm_set_current_user", html_encode(args));
            printf ("%c\n", STX);
            fflush (stdout);
            break;


        case MDM_PROMPT:
            tmp = ve_locale_to_utf8 (args);
            if (tmp != NULL && strcmp (tmp, _("Username:")) == 0) {
                mdm_common_login_sound (mdm_config_get_string (MDM_KEY_SOUND_PROGRAM), mdm_config_get_string (MDM_KEY_SOUND_ON_LOGIN_FILE), mdm_config_get_bool (MDM_KEY_SOUND_ON_LOGIN));
                webkit_execute_script("mdm_prompt", _("Username:"));
            } else {
                if (tmp != NULL) {
                    webkit_execute_script("mdm_prompt", tmp);
                }
            }
            g_free (tmp);

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
                if (tmp != NULL) {
                    webkit_execute_script("mdm_noecho", tmp);
                }
            }
            g_free (tmp);

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
            if ( ! replace_msg && /* empty message is for clearing */ ! ve_string_empty (args)) {
                const char *oldtext;
                oldtext = g_strdup (mdm_msg);
                if ( ! ve_string_empty (oldtext)) {
                    char *newtext;
                    tmp = ve_locale_to_utf8 (args);
                    newtext = g_strdup_printf ("%s\n%s", oldtext, tmp);
                    g_free (tmp);
                    mdm_msg = g_strdup (newtext);
                    g_free (newtext);
                }
                else {
                    tmp = ve_locale_to_utf8 (args);
                    mdm_msg = g_strdup (tmp);
                    g_free (tmp);
                }
            }
            else {
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
            if (err_box_clear_handler > 0) {
                g_source_remove (err_box_clear_handler);
            }
            if (ve_string_empty (args)) {
                err_box_clear_handler = 0;
            }
            else {
                err_box_clear_handler = g_timeout_add (30000, err_box_clear, NULL);
            }
            printf ("%c\n", STX);
            fflush (stdout);
            break;

        case MDM_ERRDLG:
            /* we should be now fine for focusing new windows */
            mdm_wm_focus_new_windows (TRUE);
            tmp = ve_locale_to_utf8 (args);
            dlg = hig_dialog_new (NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, tmp, "");
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
            //mdm_lang_op_lang (args);
            printf ("%c%s\n", STX, current_language);
            fflush (stdout);
            break;

        case MDM_SLANG:
            //mdm_lang_op_slang (args);
            printf ("%c\n", STX);
            fflush (stdout);
            break;

        case MDM_SETSESS:
            current_session = args;
            gchar * session_file = g_strdup_printf("%s.desktop", args);
            gchar * wargs = g_strdup_printf("%s\", \"%s", mdm_session_name(session_file), session_file);
            webkit_execute_script("mdm_set_current_session", wargs);
            g_free (wargs);
            mdm_debug("mdm_verify_set_user_settings: mdm_set_current_session '%s'.", args);
            printf ("%c\n", STX);
            fflush (stdout);
            break;

        case MDM_SETLANG:
            if (args) {
                current_language = args;
                char *name;
                char *untranslated;
                if (mdm_common_locale_is_displayable (args)) {
                    name = mdm_lang_name (args, FALSE, TRUE, FALSE, FALSE);

                    untranslated = mdm_lang_untranslated_name (args, TRUE);

                    if (untranslated != NULL) {
                        gchar * wargs = g_strdup_printf("%s\", \"%s", untranslated, args);
                        webkit_execute_script("mdm_set_current_language", wargs);
                        g_free (wargs);
                    }
                    else {
                        gchar * wargs = g_strdup_printf("%s\", \"%s", name, args);
                        webkit_execute_script("mdm_set_current_language", wargs);
                        g_free (wargs);
                    }
                }
                g_free (name);
                g_free (untranslated);
            }

            printf ("%c\n", STX);
            fflush (stdout);
            break;

        case MDM_ALWAYS_RESTART:
            mdm_lang_op_always_restart (args);
            break;

        case MDM_RESET:
        case MDM_RESETOK:
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
                    dlg = hig_dialog_new (NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK, oldtext, "");
                    gtk_window_set_modal (GTK_WINDOW (dlg), TRUE);
                    mdm_wm_center_window (GTK_WINDOW (dlg));
                    mdm_wm_no_login_focus_push ();
                    gtk_dialog_run (GTK_DIALOG (dlg));
                    gtk_widget_destroy (dlg);
                    mdm_wm_no_login_focus_pop ();
                }
                messages_to_give = FALSE;
            }

            //gdk_flush ();
            printf ("%c\n", STX);
            fflush (stdout);
            _exit (EXIT_SUCCESS);
            break;

        case MDM_STARTTIMER:
            if (timed_handler_id == 0 && mdm_config_get_bool (MDM_KEY_TIMED_LOGIN_ENABLE) && ! ve_string_empty (mdm_config_get_string (MDM_KEY_TIMED_LOGIN)) && mdm_config_get_int (MDM_KEY_TIMED_LOGIN_DELAY) > 0) {
                mdm_timed_delay = mdm_config_get_int (MDM_KEY_TIMED_LOGIN_DELAY);
                timed_handler_id  = g_timeout_add (1000, mdm_timer, NULL);
            }
            printf ("%c\n", STX);
            fflush (stdout);
            break;

        case MDM_STOPTIMER:
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

        // These are handled separately so ignore them here and send back a NULL response so that the daemon quits sending them
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
            //mdm_common_setup_cursor (GDK_WATCH);
            //mdm_wm_save_wm_order ();
            //gdk_flush ();
            printf ("%c\n", STX);
            fflush (stdout);
            _exit (EXIT_SUCCESS);
            break;

        case MDM_QUERY_CAPSLOCK:
            if (greeter_is_capslock_on ()) {
                printf ("%cY\n", STX);
            }
            else {
                printf ("%c\n", STX);
            }
            fflush (stdout);
            break;

        default:
            mdm_common_fail_greeter ("Unexpected greeter command received: '%c'", op_code);
            break;
    }
}


void mdm_login_browser_populate (void) {
    check_for_displays ();

    GList *li;
    for (li = users; li != NULL; li = li->next) {
        MdmUser *usr = li->data;
        char *login, *gecos, *status, *facefile;
        facefile = mdm_common_get_facefile(usr->homedir, usr->login, usr->uid);
        login = mdm_common_text_to_escaped_utf8 (usr->login);
        gecos = mdm_common_text_to_escaped_utf8 (usr->gecos);

        if (g_hash_table_lookup (displays_hash, usr->login)) {
            status = _("Already logged in");
        }
        else {
            status = "";
        }
        gchar * args = g_strdup_printf("%s\", \"%s\", \"%s\", \"%s", login, gecos, status, facefile);
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

gboolean update_clock (void) {
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

void mdm_set_welcomemsg (void) {
    gchar *greeting;
    gchar *welcomemsg = mdm_common_get_welcomemsg ();
    greeting = mdm_common_expand_text (welcomemsg);
    webkit_execute_script("set_welcome_message", greeting);
    g_free (welcomemsg);
    g_free (greeting);
}

static gboolean key_press_event (GtkWidget *widget, GdkEventKey *key, gpointer data) {
    if (key->keyval == GDK_Escape) {
        printf ("%c%c%c\n", STX, BEL, MDM_INTERRUPT_CANCEL);
        fflush (stdout);
        return TRUE;
    }
    return FALSE;
}

static void webkit_init (void) {
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
        dialog = hig_dialog_new (NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, s, (error && error->message) ? error->message : "");
        g_free (s);

        gtk_widget_show_all (dialog);
        mdm_wm_center_window (GTK_WINDOW (dialog));

        //mdm_common_setup_cursor (GDK_LEFT_PTR);

        gtk_dialog_run (GTK_DIALOG (dialog));
        gtk_widget_destroy (dialog);

        g_free (theme_dir);
        g_free (theme_filename);

        mdm_common_fail_greeter ("mdm_webkit: There was an error loading the theme '%s'", theme_name);

        g_free (theme_name);

    }

    char lsb_description[255];
    FILE *fp = popen("lsb_release -d -s", "r");
    fgets(lsb_description, 255, fp);
    pclose(fp);

    html = str_replace(html, "$lsb_description", lsb_description);
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
    html = str_replace(html, "$session", html_encode(_("Session")));
    html = str_replace(html, "$selectsession", html_encode(_("Select a session")));
    html = str_replace(html, "$defaultsession", html_encode(_("Default session")));
    html = str_replace(html, "$selectuser", html_encode(_("Please select a user.")));
    html = str_replace(html, "$pressf1toenterusername", html_encode(_("Press F1 to enter a username.")));
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

    int scale = 0;
    gchar *out = NULL;
    g_spawn_command_line_sync ("mdm-get-monitor-scale",
                               &out,
                               NULL,
                               NULL,
                               NULL);
    if (out) {
        scale = atoi (out);
        scale = CLAMP (scale, 10, 20);
        g_free (out);
    }

    if (scale > 10) {
        g_object_set (G_OBJECT(webView), "full-content-zoom", TRUE, NULL);
        float zoom_level = (float) scale / (float) 10;
        webkit_web_view_set_zoom_level (webView, zoom_level);
    }

    webkit_web_view_set_settings (WEBKIT_WEB_VIEW(webView), settings);
    webkit_web_view_set_transparent (webView, TRUE);

    webkit_web_view_load_string(webView, html, "text/html", "UTF-8", theme_dir);

    g_signal_connect(G_OBJECT(webView), "script-alert", G_CALLBACK(webkit_on_message), NULL);
    g_signal_connect(G_OBJECT(webView), "load-finished", G_CALLBACK(webkit_on_loaded), NULL);
    g_signal_connect(G_OBJECT(webView), "load-error", G_CALLBACK(webkit_on_error), NULL);
    g_signal_connect(G_OBJECT(webView), "resource-load-failed", G_CALLBACK(webkit_on_resource_failed), NULL);
    g_signal_connect(G_OBJECT(webView), "console-message", G_CALLBACK(webkit_on_console_message), NULL);
    g_signal_connect(G_OBJECT(webView), "navigation-policy-decision-requested", G_CALLBACK(webkit_on_navigation_policy_decision_requested), NULL);
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

static void mdm_login_gui_init (void) {
    gint i;
    const gchar *theme_name;
    gchar *key_string = NULL;

    theme_name = g_getenv ("MDM_GTK_THEME");
    if (ve_string_empty (theme_name)) {
        theme_name = mdm_config_get_string (MDM_KEY_GTK_THEME);
    }

    if ( ! ve_string_empty (mdm_config_get_string (MDM_KEY_GTKRC))) {
        gtk_rc_parse (mdm_config_get_string (MDM_KEY_GTKRC));
    }

    if ( ! ve_string_empty (theme_name)) {
        mdm_set_theme (theme_name);
    }

    login = gtk_window_new (GTK_WINDOW_TOPLEVEL);

    GdkColor color;
    color.red = 0;
    color.green = 0;
    color.blue = 0;
    gtk_widget_modify_bg(login, GTK_STATE_NORMAL, &color);

    if G_LIKELY ( ! DOING_MDM_DEVELOPMENT) {
        gtk_window_set_decorated (GTK_WINDOW (login), FALSE);
        gtk_window_set_default_size (GTK_WINDOW (login), mdm_wm_screen.width, mdm_wm_screen.height);
    }
    else {
        gtk_window_set_icon_name (GTK_WINDOW (login), "mdmsetup");
        gtk_window_set_title (GTK_WINDOW (login), theme_name);
        gtk_window_maximize (GTK_WINDOW (login));
    }

    g_object_ref (login);
    g_object_set_data_full (G_OBJECT (login), "login", login, (GDestroyNotify) g_object_unref);

    gtk_widget_set_events (login, GDK_ALL_EVENTS_MASK);

    g_signal_connect (G_OBJECT (login), "key_press_event", G_CALLBACK (key_press_event), NULL);


    GtkWidget *scrolled = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

    gtk_container_add (GTK_CONTAINER (scrolled), webView);
    gtk_container_add (GTK_CONTAINER (login), scrolled);

    int height;

    height = size_of_users + 4 /* some padding */;
    if (height > mdm_wm_screen.height * 0.25) {
        height = mdm_wm_screen.height * 0.25;
    }

    /* cursor blinking is evil on remote displays, don't do it forever */
    mdm_common_setup_blinking ();

    gtk_widget_grab_focus (webView);
    gtk_window_set_focus (GTK_WINDOW (login), webView);
    g_object_set (G_OBJECT (login), "allow_grow", TRUE, "allow_shrink", TRUE, "resizable", TRUE, NULL);

    mdm_wm_center_window (GTK_WINDOW (login));
}

// This function does nothing for mdmlogin, but mdmgreeter does do extra work in this callback function.
void lang_set_custom_callback (gchar *language) {
}

int main (int argc, char *argv[]) {
    struct sigaction hup;
    struct sigaction term;
    sigset_t mask;
    guint sid;

    if (g_getenv ("DOING_MDM_DEVELOPMENT") != NULL) {
        DOING_MDM_DEVELOPMENT = TRUE;
    }

    bindtextdomain (GETTEXT_PACKAGE, "/usr/share/mdm/locale");
    bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
    textdomain (GETTEXT_PACKAGE);

    gtk_init (&argc, &argv);

    mdm_common_log_init ();
    mdm_common_log_set_debug (mdm_config_get_bool (MDM_KEY_DEBUG));

    setlocale (LC_ALL, "");

    mdm_wm_screen_init (mdm_config_get_string (MDM_KEY_PRIMARY_MONITOR));

    setup_background();

    current_language = g_getenv("LANG");

    defface = mdm_common_get_face (NULL, mdm_config_get_string (MDM_KEY_DEFAULT_FACE), mdm_config_get_int (MDM_KEY_MAX_ICON_WIDTH), mdm_config_get_int (MDM_KEY_MAX_ICON_HEIGHT));

    if (! defface) {
        mdm_common_warning ("mdmwebkit: Could not open DefaultFace: %s!", mdm_config_get_string (MDM_KEY_DEFAULT_FACE));
    }

    mdm_session_list_init ();
    mdm_users_init (&users, &users_string, NULL, defface, &size_of_users, TRUE, !DOING_MDM_DEVELOPMENT);

    webkit_init();

    mdm_login_gui_init ();

    hup.sa_handler = ve_signal_notify;
    hup.sa_flags = 0;
    sigemptyset (&hup.sa_mask);
    sigaddset (&hup.sa_mask, SIGCHLD);

    if G_UNLIKELY (sigaction (SIGHUP, &hup, NULL) < 0) {
        mdm_common_fail_greeter ("mdmwebkit: Error setting up HUP signal handler: %s", strerror (errno));
    }

    term.sa_handler = mdm_login_done;
    term.sa_flags = 0;
    sigemptyset (&term.sa_mask);
    sigaddset (&term.sa_mask, SIGCHLD);

    if G_UNLIKELY (sigaction (SIGINT, &term, NULL) < 0) {
        mdm_common_fail_greeter ("mdmwebkit: Error setting up INT signal handler: %s", strerror (errno));
    }

    if G_UNLIKELY (sigaction (SIGTERM, &term, NULL) < 0) {
        mdm_common_fail_greeter ("mdmwebkit: Error setting up TERM signal handler: %s", strerror (errno));
    }

    sigemptyset (&mask);
    sigaddset (&mask, SIGTERM);
    sigaddset (&mask, SIGHUP);
    sigaddset (&mask, SIGINT);

    if G_UNLIKELY (sigprocmask (SIG_UNBLOCK, &mask, NULL) == -1) {
        mdm_common_fail_greeter ("Could not set signal mask!");
    }

    /* if in timed mode, delay timeout on keyboard or menu
     * activity */
    if (mdm_config_get_bool (MDM_KEY_TIMED_LOGIN_ENABLE) && ! ve_string_empty (mdm_config_get_string (MDM_KEY_TIMED_LOGIN))) {
        sid = g_signal_lookup ("activate", GTK_TYPE_MENU_ITEM);
        g_signal_add_emission_hook (sid, 0, mdm_timer_up_delay, NULL, NULL);

        sid = g_signal_lookup ("key_release_event", GTK_TYPE_WIDGET);
        g_signal_add_emission_hook (sid, 0, mdm_timer_up_delay, NULL, NULL);

        sid = g_signal_lookup ("button_press_event", GTK_TYPE_WIDGET);
        g_signal_add_emission_hook (sid, 0, mdm_timer_up_delay, NULL, NULL);
    }

    /* if a flexiserver, reap self after some time */
    if (mdm_config_get_int (MDM_KEY_FLEXI_REAP_DELAY_MINUTES) > 0 && ! ve_string_empty (g_getenv ("MDM_FLEXI_SERVER")) ) {
        sid = g_signal_lookup ("activate", GTK_TYPE_MENU_ITEM);
        g_signal_add_emission_hook (sid, 0, delay_reaping, NULL, NULL);

        sid = g_signal_lookup ("key_release_event", GTK_TYPE_WIDGET);
        g_signal_add_emission_hook (sid, 0, delay_reaping, NULL, NULL);

        sid = g_signal_lookup ("button_press_event", GTK_TYPE_WIDGET);
        g_signal_add_emission_hook (sid, 0, delay_reaping, NULL, NULL);

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

    mdm_common_setup_cursor (GDK_LEFT_PTR);

    mdm_wm_center_cursor ();

    gtk_main ();

    return EXIT_SUCCESS;
}
