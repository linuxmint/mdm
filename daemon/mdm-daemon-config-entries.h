/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2007 William Jon McCann <mccann@jhu.edu>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifndef _MDM_DAEMON_CONFIG_ENTRIES_H
#define _MDM_DAEMON_CONFIG_ENTRIES_H

#include <glib.h>

#include "mdm-config.h"

G_BEGIN_DECLS

#define MDM_CONFIG_GROUP_NONE       NULL
#define MDM_CONFIG_GROUP_DAEMON     "daemon"
#define MDM_CONFIG_GROUP_SECURITY   "security"
#define MDM_CONFIG_GROUP_GREETER    "greeter"
#define MDM_CONFIG_GROUP_GUI        "gui"
#define MDM_CONFIG_GROUP_SERVERS    "servers"
#define MDM_CONFIG_GROUP_DEBUG      "debug"

#define MDM_CONFIG_GROUP_SERVER_PREFIX "server-"

#include "mdm-daemon-config-keys.h"

typedef enum {
	MDM_ID_NONE,
	MDM_ID_DEBUG,
	MDM_ID_LIMIT_SESSION_OUTPUT,
	MDM_ID_FILTER_SESSION_OUTPUT,
	MDM_ID_DEBUG_GESTURES,
	MDM_ID_AUTOMATIC_LOGIN_ENABLE,
	MDM_ID_AUTOMATIC_LOGIN,
	MDM_ID_GREETER,
	MDM_ID_ADD_GTK_MODULES,
	MDM_ID_GTK_MODULES_LIST,
	MDM_ID_GROUP,
	MDM_ID_HALT,
	MDM_ID_DISPLAY_INIT_DIR,
	MDM_ID_KILL_INIT_CLIENTS,
	MDM_ID_LOG_DIR,
	MDM_ID_PATH,
	MDM_ID_POSTSESSION,
	MDM_ID_PRESESSION,
	MDM_ID_POSTLOGIN,
	MDM_ID_FAILSAFE_XSERVER,
	MDM_ID_X_KEEPS_CRASHING,
	MDM_ID_REBOOT ,
	MDM_ID_ROOT_PATH,
	MDM_ID_SERV_AUTHDIR,
	MDM_ID_SESSION_DESKTOP_DIR,
	MDM_ID_BASE_XSESSION,
	MDM_ID_DEFAULT_SESSION,
	MDM_ID_DEFAULT_SESSIONS,
	MDM_ID_SUSPEND,
	MDM_ID_USER_AUTHDIR,
	MDM_ID_USER_AUTHDIR_FALLBACK,
	MDM_ID_USER_AUTHFILE,
	MDM_ID_USER,
	MDM_ID_CONSOLE_NOTIFY,
	MDM_ID_DOUBLE_LOGIN_WARNING,
	MDM_ID_ALWAYS_LOGIN_CURRENT_SESSION,
	MDM_ID_DISPLAY_LAST_LOGIN,
	MDM_ID_SELECT_LAST_LOGIN,
	MDM_ID_NUMLOCK,
	MDM_ID_TIMED_LOGIN_ENABLE,
	MDM_ID_TIMED_LOGIN,
	MDM_ID_TIMED_LOGIN_DELAY,
	MDM_ID_FLEXI_REAP_DELAY_MINUTES,
	MDM_ID_STANDARD_XSERVER,
	MDM_ID_FLEXIBLE_XSERVERS,
	MDM_ID_FIRST_VT,
	MDM_ID_VT_ALLOCATION,
	MDM_ID_CONSOLE_CANNOT_HANDLE,
	MDM_ID_XSERVER_TIMEOUT,
	MDM_ID_SERVER_PREFIX,
	MDM_ID_SERVER_NAME,
	MDM_ID_SERVER_COMMAND,
	MDM_ID_SERVER_FLEXIBLE,
	MDM_ID_SERVER_CHOOSABLE,
	MDM_ID_SERVER_HANDLED,
	MDM_ID_SERVER_PRIORITY,
	MDM_ID_ALLOW_ROOT,	
	MDM_ID_USER_MAX_FILE,
	MDM_ID_RELAX_PERM,
	MDM_ID_CHECK_DIR_OWNER,
	MDM_ID_SUPPORT_AUTOMOUNT,
	MDM_ID_RETRY_DELAY,
	MDM_ID_DISALLOW_TCP,
	MDM_ID_PAM_STACK,
	MDM_ID_NEVER_PLACE_COOKIES_ON_NFS,
	MDM_ID_PASSWORD_REQUIRED,
	MDM_ID_UTMP_LINE_ATTACHED,	
	MDM_ID_UTMP_PSEUDO_DEVICE,
	MDM_ID_GTK_THEME,
	MDM_ID_GTKRC,
	MDM_ID_MAX_ICON_WIDTH,
	MDM_ID_MAX_ICON_HEIGHT,
	MDM_ID_ALLOW_GTK_THEME_CHANGE,
	MDM_ID_GTK_THEMES_TO_ALLOW,
	MDM_ID_BROWSER,
	MDM_ID_INCLUDE,
	MDM_ID_EXCLUDE,
	MDM_ID_INCLUDE_ALL,
	MDM_ID_MINIMAL_UID,
	MDM_ID_DEFAULT_FACE,
	MDM_ID_GLOBAL_FACE_DIR,
    MDM_ID_GNOME_ACCOUNTS_SERVICE_FACE_DIR,
	MDM_ID_LOCALE_FILE,		
	MDM_ID_SYSTEM_MENU,
	MDM_ID_CONFIGURATOR,
	MDM_ID_CONFIG_AVAILABLE,
	MDM_ID_DEFAULT_WELCOME,
	MDM_ID_WELCOME,
	MDM_ID_PRIMARY_MONITOR,
	MDM_ID_BACKGROUND_PROGRAM,
	MDM_ID_RUN_BACKGROUND_PROGRAM_ALWAYS,
	MDM_ID_BACKGROUND_PROGRAM_INITIAL_DELAY,
	MDM_ID_RESTART_BACKGROUND_PROGRAM,
	MDM_ID_BACKGROUND_PROGRAM_RESTART_DELAY,
	MDM_ID_BACKGROUND_IMAGE,
	MDM_ID_BACKGROUND_COLOR,
	MDM_ID_BACKGROUND_TYPE,		
	MDM_ID_USE_24_CLOCK,
	MDM_ID_ENTRY_CIRCLES,
	MDM_ID_ENTRY_INVISIBLE,
	MDM_ID_GRAPHICAL_THEME,	
	MDM_ID_GRAPHICAL_THEME_DIR,
	MDM_ID_GRAPHICAL_THEMED_COLOR,
	MDM_ID_HTML_THEME,	
	MDM_ID_INFO_MSG_FILE,
	MDM_ID_INFO_MSG_FONT,
	MDM_ID_PRE_FETCH_PROGRAM,
	MDM_ID_SOUND_ON_LOGIN,
	MDM_ID_SOUND_ON_LOGIN_SUCCESS,
	MDM_ID_SOUND_ON_LOGIN_FAILURE,
	MDM_ID_SOUND_ON_LOGIN_FILE,
	MDM_ID_SOUND_ON_LOGIN_SUCCESS_FILE,
	MDM_ID_SOUND_ON_LOGIN_FAILURE_FILE,
	MDM_ID_SOUND_PROGRAM,
	MDM_ID_SCAN_TIME,
	MDM_ID_DEFAULT_HOST_IMG,
	MDM_ID_HOST_IMAGE_DIR,
	MDM_ID_HOSTS,
	MDM_ID_MULTICAST,
	MDM_ID_MULTICAST_ADDR,
	MDM_ID_BROADCAST,
	MDM_ID_ALLOW_ADD,
	MDM_ID_SECTION_GREETER,
	MDM_ID_SECTION_SERVERS,
	MDM_ID_SYSTEM_COMMANDS_IN_MENU,
	MDM_ID_ALLOW_LOGOUT_ACTIONS,
	MDM_ID_RBAC_SYSTEM_COMMAND_KEYS,
	GDK_ID_LAST
} MdmConfigKey;


/*
 * The following section contains keys used by the MDM configuration files.
 * The key/value pairs defined in the MDM configuration files are considered
 * "stable" interface and should only change in ways that are backwards
 * compatible.  Please keep this in mind when changing MDM configuration.
 *
 * Developers who add new configuration options should ensure that they do the
 * following:
 *
 * + Add the key to config/mdm.conf.in file and specify the default value.
 *   Include comments explaining what the key does.
 *
 * + Add the key as a #define to daemon/mdm-daemon-config-keys.h with
 *   the same default value.
 *
 * + Update the MdmConfigKey enumeration and mdm_daemon_config_entries[] to
 *   add the new key.  Include some documentation about the new key,
 *   following the style of existing comments.
 *
 * + Add any validation to the validate_cb function in
 *   mdm-daemon-config.c, if validation is needed.
 *
 * + If MDM_UPDATE_CONFIG should not respond to this configuration setting,
 *   update the mdm_daemon_config_update_key function in mdmconfig.c to
 *   return FALSE for this key.  Examples include changing the ServAuthDir
 *   or  other values that MDM should not change until it is restarted.  If
 *   this is true, the next bullet can be ignored.
 *
 * + If the option should cause the greeter (mdmlogin/mdmgreeter) program to
 *   be updated immediately, update the notify_cb and lookup_notify_key
 *   functions to handle this key.
 *
 * + Add the key to the mdm_read_config and mdm_reread_config functions in
 *   gui/mdmlogin.c, and gui/greeter/greeter.c
 *   if the key is used by those programs.  Note that all MDM slaves load
 *   all their configuration data between calls to mdmcomm_comm_bulk_start()
 *   and mdmcomm_comm_bulk_stop().  This makes sure that the slave only uses
 *   a single sockets connection to get all configuration data.  If a new
 *   config value is read by a slave, make sure to load the key in this
 *   code section for best performance.
 *
 * + The gui/mdmsetup.c program should be updated to support the new option
 *   unless there's a good reason not to.
 *
 * + Currently MDM treats any key in the "gui" and "greeter" categories,
 *   and security/PamStack as available for per-display configuration.
 *   If a key is appropriate for per-display configuration, and is not
 *   in the "gui" or "greeter" categories, then it will need to be added
 *   to the mdm_config_key_to_string_per_display function.  It may make
 *   sense for some keys used by the daemon to be per-display so this
 *   will need to be coded (refer to MDM_ID_PAM_STACK for an example).
 *
 * + Update the docs/C/mdm.xml file to include information about the new
 *   option.  Include information about any other interfaces (such as
 *   ENVIRONMENT variables) that may affect the configuration option.
 *   Patches without documentation will not be accepted.
 *
 * Please do this work *before* submitting an patch.  Patches that are not
 * complete will not likely be accepted.
 */

#define MDM_DEFAULT_WELCOME_MSG "Welcome"

/* These are processed in order so debug should always be first */
static const MdmConfigEntry mdm_daemon_config_entries [] = {
	{ MDM_CONFIG_GROUP_DEBUG, "Enable", MDM_CONFIG_VALUE_BOOL, "false", MDM_ID_DEBUG },
	{ MDM_CONFIG_GROUP_DEBUG, "LimitSessionOutput", MDM_CONFIG_VALUE_BOOL, "true", MDM_ID_LIMIT_SESSION_OUTPUT },
	{ MDM_CONFIG_GROUP_DEBUG, "FilterSessionOutput", MDM_CONFIG_VALUE_BOOL, "false", MDM_ID_FILTER_SESSION_OUTPUT },
	{ MDM_CONFIG_GROUP_DEBUG, "Gestures", MDM_CONFIG_VALUE_BOOL, "false", MDM_ID_DEBUG_GESTURES },


	{ MDM_CONFIG_GROUP_DAEMON, "AutomaticLoginEnable", MDM_CONFIG_VALUE_BOOL, "false", MDM_ID_AUTOMATIC_LOGIN_ENABLE },
	{ MDM_CONFIG_GROUP_DAEMON, "AutomaticLogin", MDM_CONFIG_VALUE_STRING, "", MDM_ID_AUTOMATIC_LOGIN },

	/* The SDTLOGIN feature is Solaris specific, and causes the Xserver to be
	 * run with user permissionsinstead of as root, which adds security but,
	 * disables the AlwaysRestartServer option as highlighted in the mdm
	 * documentation */

	{ MDM_CONFIG_GROUP_DAEMON, "Greeter", MDM_CONFIG_VALUE_STRING, LIBEXECDIR "/mdmlogin", MDM_ID_GREETER },
	{ MDM_CONFIG_GROUP_DAEMON, "AddGtkModules", MDM_CONFIG_VALUE_BOOL, "false", MDM_ID_ADD_GTK_MODULES },
	{ MDM_CONFIG_GROUP_DAEMON, "GtkModulesList", MDM_CONFIG_VALUE_STRING, NULL, MDM_ID_GTK_MODULES_LIST },

	{ MDM_CONFIG_GROUP_DAEMON, "User", MDM_CONFIG_VALUE_STRING, "mdm", MDM_ID_USER },
	{ MDM_CONFIG_GROUP_DAEMON, "Group", MDM_CONFIG_VALUE_STRING, "mdm", MDM_ID_GROUP },

	{ MDM_CONFIG_GROUP_DAEMON, "HaltCommand", MDM_CONFIG_VALUE_STRING_ARRAY, HALT_COMMAND, MDM_ID_HALT },
	{ MDM_CONFIG_GROUP_DAEMON, "RebootCommand", MDM_CONFIG_VALUE_STRING_ARRAY, REBOOT_COMMAND, MDM_ID_REBOOT },
	{ MDM_CONFIG_GROUP_DAEMON, "SuspendCommand", MDM_CONFIG_VALUE_STRING_ARRAY, SUSPEND_COMMAND, MDM_ID_SUSPEND },

	{ MDM_CONFIG_GROUP_DAEMON, "DisplayInitDir", MDM_CONFIG_VALUE_STRING, MDMCONFDIR "/Init", MDM_ID_DISPLAY_INIT_DIR },
	{ MDM_CONFIG_GROUP_DAEMON, "KillInitClients", MDM_CONFIG_VALUE_BOOL, "true", MDM_ID_KILL_INIT_CLIENTS },
	{ MDM_CONFIG_GROUP_DAEMON, "LogDir", MDM_CONFIG_VALUE_STRING, LOGDIR, MDM_ID_LOG_DIR },
	{ MDM_CONFIG_GROUP_DAEMON, "DefaultPath", MDM_CONFIG_VALUE_STRING, MDM_USER_PATH, MDM_ID_PATH },
	{ MDM_CONFIG_GROUP_DAEMON, "PostSessionScriptDir", MDM_CONFIG_VALUE_STRING, MDMCONFDIR "/PostSession/", MDM_ID_POSTSESSION },
	{ MDM_CONFIG_GROUP_DAEMON, "PreSessionScriptDir", MDM_CONFIG_VALUE_STRING, MDMCONFDIR "/PreSession/", MDM_ID_PRESESSION },
	{ MDM_CONFIG_GROUP_DAEMON, "PostLoginScriptDir", MDM_CONFIG_VALUE_STRING, MDMCONFDIR "/PreSession/", MDM_ID_POSTLOGIN },
	{ MDM_CONFIG_GROUP_DAEMON, "FailsafeXServer", MDM_CONFIG_VALUE_STRING, NULL, MDM_ID_FAILSAFE_XSERVER },
	{ MDM_CONFIG_GROUP_DAEMON, "XKeepsCrashing", MDM_CONFIG_VALUE_STRING, MDMCONFDIR "/XKeepsCrashing", MDM_ID_X_KEEPS_CRASHING },
	{ MDM_CONFIG_GROUP_DAEMON, "RootPath", MDM_CONFIG_VALUE_STRING, "/sbin:/usr/sbin:" MDM_USER_PATH, MDM_ID_ROOT_PATH },
	{ MDM_CONFIG_GROUP_DAEMON, "ServAuthDir", MDM_CONFIG_VALUE_STRING, AUTHDIR, MDM_ID_SERV_AUTHDIR },
	{ MDM_CONFIG_GROUP_DAEMON, "SessionDesktopDir", MDM_CONFIG_VALUE_STRING, "/etc/X11/sessions/:" DMCONFDIR "/Sessions/:" DATADIR "/mdm/BuiltInSessions/:" DATADIR "/xsessions/", MDM_ID_SESSION_DESKTOP_DIR },
	{ MDM_CONFIG_GROUP_DAEMON, "BaseXsession", MDM_CONFIG_VALUE_STRING, MDMCONFDIR "/Xsession", MDM_ID_BASE_XSESSION },
	{ MDM_CONFIG_GROUP_DAEMON, "DefaultSession", MDM_CONFIG_VALUE_STRING, "auto", MDM_ID_DEFAULT_SESSION },
	{ MDM_CONFIG_GROUP_DAEMON, "DefaultSessions", MDM_CONFIG_VALUE_STRING, "cinnamon.desktop,mate.desktop,xfce.desktop,kde-plasma.desktop,kde.desktop", MDM_ID_DEFAULT_SESSIONS },

	{ MDM_CONFIG_GROUP_DAEMON, "UserAuthDir", MDM_CONFIG_VALUE_STRING, "", MDM_ID_USER_AUTHDIR },
	{ MDM_CONFIG_GROUP_DAEMON, "UserAuthFBDir", MDM_CONFIG_VALUE_STRING, "/tmp", MDM_ID_USER_AUTHDIR_FALLBACK },
	{ MDM_CONFIG_GROUP_DAEMON, "UserAuthFile", MDM_CONFIG_VALUE_STRING, ".Xauthority", MDM_ID_USER_AUTHFILE },
	{ MDM_CONFIG_GROUP_DAEMON, "ConsoleNotify", MDM_CONFIG_VALUE_BOOL, "true", MDM_ID_CONSOLE_NOTIFY },

	{ MDM_CONFIG_GROUP_DAEMON, "DoubleLoginWarning", MDM_CONFIG_VALUE_BOOL, "true", MDM_ID_DOUBLE_LOGIN_WARNING },
	{ MDM_CONFIG_GROUP_DAEMON, "AlwaysLoginCurrentSession", MDM_CONFIG_VALUE_BOOL, "true", MDM_ID_ALWAYS_LOGIN_CURRENT_SESSION },

	{ MDM_CONFIG_GROUP_DAEMON, "DisplayLastLogin", MDM_CONFIG_VALUE_BOOL, "false", MDM_ID_DISPLAY_LAST_LOGIN },
	{ MDM_CONFIG_GROUP_DAEMON, "SelectLastLogin", MDM_CONFIG_VALUE_BOOL, "true", MDM_ID_SELECT_LAST_LOGIN },
	{ MDM_CONFIG_GROUP_DAEMON, "EnableNumLock", MDM_CONFIG_VALUE_BOOL, "false", MDM_ID_NUMLOCK },

	{ MDM_CONFIG_GROUP_DAEMON, "TimedLoginEnable", MDM_CONFIG_VALUE_BOOL, "false", MDM_ID_TIMED_LOGIN_ENABLE },
	{ MDM_CONFIG_GROUP_DAEMON, "TimedLogin", MDM_CONFIG_VALUE_STRING, "", MDM_ID_TIMED_LOGIN },
	{ MDM_CONFIG_GROUP_DAEMON, "TimedLoginDelay", MDM_CONFIG_VALUE_INT, "30", MDM_ID_TIMED_LOGIN_DELAY },

	{ MDM_CONFIG_GROUP_DAEMON, "FlexiReapDelayMinutes", MDM_CONFIG_VALUE_INT, "0", MDM_ID_FLEXI_REAP_DELAY_MINUTES },

	{ MDM_CONFIG_GROUP_DAEMON, "StandardXServer", MDM_CONFIG_VALUE_STRING, X_SERVER, MDM_ID_STANDARD_XSERVER },
	{ MDM_CONFIG_GROUP_DAEMON, "FlexibleXServers", MDM_CONFIG_VALUE_INT, "5", MDM_ID_FLEXIBLE_XSERVERS },

	/* Keys for automatic VT allocation rather then letting it up to the X server */
	{ MDM_CONFIG_GROUP_DAEMON, "FirstVT", MDM_CONFIG_VALUE_INT, "7", MDM_ID_FIRST_VT },
	{ MDM_CONFIG_GROUP_DAEMON, "VTAllocation", MDM_CONFIG_VALUE_BOOL, "true", MDM_ID_VT_ALLOCATION },

	{ MDM_CONFIG_GROUP_DAEMON, "ConsoleCannotHandle", MDM_CONFIG_VALUE_STRING, "am,ar,az,bn,el,fa,gu,hi,ja,ko,ml,mr,pa,ta,zh", MDM_ID_CONSOLE_CANNOT_HANDLE },

	/* How long to wait before assuming an Xserver has timed out */
	{ MDM_CONFIG_GROUP_DAEMON, "MdmXserverTimeout", MDM_CONFIG_VALUE_INT, "10", MDM_ID_XSERVER_TIMEOUT },

	{ MDM_CONFIG_GROUP_DAEMON, "SystemCommandsInMenu", MDM_CONFIG_VALUE_STRING_ARRAY, "HALT;REBOOT;SUSPEND", MDM_ID_SYSTEM_COMMANDS_IN_MENU },
	{ MDM_CONFIG_GROUP_DAEMON, "AllowLogoutActions", MDM_CONFIG_VALUE_STRING_ARRAY, "HALT;REBOOT;SUSPEND", MDM_ID_ALLOW_LOGOUT_ACTIONS },
	{ MDM_CONFIG_GROUP_DAEMON, "RBACSystemCommandKeys", MDM_CONFIG_VALUE_STRING_ARRAY, MDM_RBAC_SYSCMD_KEYS, MDM_ID_RBAC_SYSTEM_COMMAND_KEYS },

	{ MDM_CONFIG_GROUP_SECURITY, "AllowRoot", MDM_CONFIG_VALUE_BOOL, "true", MDM_ID_ALLOW_ROOT },
	{ MDM_CONFIG_GROUP_SECURITY, "UserMaxFile", MDM_CONFIG_VALUE_INT, "65536", MDM_ID_USER_MAX_FILE },
	{ MDM_CONFIG_GROUP_SECURITY, "RelaxPermissions", MDM_CONFIG_VALUE_INT, "0", MDM_ID_RELAX_PERM },
	{ MDM_CONFIG_GROUP_SECURITY, "CheckDirOwner", MDM_CONFIG_VALUE_BOOL, "true", MDM_ID_CHECK_DIR_OWNER },
	{ MDM_CONFIG_GROUP_SECURITY, "SupportAutomount", MDM_CONFIG_VALUE_BOOL, "false", MDM_ID_SUPPORT_AUTOMOUNT },
	{ MDM_CONFIG_GROUP_SECURITY, "RetryDelay", MDM_CONFIG_VALUE_INT, "1", MDM_ID_RETRY_DELAY },
	{ MDM_CONFIG_GROUP_SECURITY, "DisallowTCP", MDM_CONFIG_VALUE_BOOL, "true", MDM_ID_DISALLOW_TCP },
	{ MDM_CONFIG_GROUP_SECURITY, "PamStack", MDM_CONFIG_VALUE_STRING, "mdm", MDM_ID_PAM_STACK },

	{ MDM_CONFIG_GROUP_SECURITY, "NeverPlaceCookiesOnNFS", MDM_CONFIG_VALUE_BOOL, "true", MDM_ID_NEVER_PLACE_COOKIES_ON_NFS },
	{ MDM_CONFIG_GROUP_SECURITY, "PasswordRequired", MDM_CONFIG_VALUE_BOOL, "false", MDM_ID_PASSWORD_REQUIRED },
	{ MDM_CONFIG_GROUP_SECURITY, "UtmpLineAttached", MDM_CONFIG_VALUE_STRING, "", MDM_ID_UTMP_LINE_ATTACHED },
	{ MDM_CONFIG_GROUP_SECURITY, "UtmpPseudoDevice", MDM_CONFIG_VALUE_BOOL, "", MDM_ID_UTMP_PSEUDO_DEVICE },

	{ MDM_CONFIG_GROUP_GUI, "GtkTheme", MDM_CONFIG_VALUE_STRING, "Default", MDM_ID_GTK_THEME },
	{ MDM_CONFIG_GROUP_GUI, "GtkRC", MDM_CONFIG_VALUE_STRING, DATADIR "/themes/Default/gtk-2.0/gtkrc", MDM_ID_GTKRC },
	{ MDM_CONFIG_GROUP_GUI, "MaxIconWidth", MDM_CONFIG_VALUE_INT, "128", MDM_ID_MAX_ICON_WIDTH },
	{ MDM_CONFIG_GROUP_GUI, "MaxIconHeight", MDM_CONFIG_VALUE_INT, "128", MDM_ID_MAX_ICON_HEIGHT },

	{ MDM_CONFIG_GROUP_GUI, "AllowGtkThemeChange", MDM_CONFIG_VALUE_BOOL, "true", MDM_ID_ALLOW_GTK_THEME_CHANGE },
	{ MDM_CONFIG_GROUP_GUI, "GtkThemesToAllow", MDM_CONFIG_VALUE_STRING, "all", MDM_ID_GTK_THEMES_TO_ALLOW },
	{ MDM_CONFIG_GROUP_GREETER, "Browser", MDM_CONFIG_VALUE_BOOL, "true", MDM_ID_BROWSER },

	{ MDM_CONFIG_GROUP_GREETER, "Include", MDM_CONFIG_VALUE_STRING, "", MDM_ID_INCLUDE },
	{ MDM_CONFIG_GROUP_GREETER, "Exclude", MDM_CONFIG_VALUE_STRING, "bin,daemon,adm,lp,sync,shutdown,halt,mail,news,uucp,operator,nobody,mdm,postgres,pvm,rpm,nfsnobody,pcap", MDM_ID_EXCLUDE },
	{ MDM_CONFIG_GROUP_GREETER, "IncludeAll", MDM_CONFIG_VALUE_BOOL, "false", MDM_ID_INCLUDE_ALL },
	{ MDM_CONFIG_GROUP_GREETER, "MinimalUID", MDM_CONFIG_VALUE_INT, "100", MDM_ID_MINIMAL_UID },
	{ MDM_CONFIG_GROUP_GREETER, "DefaultFace", MDM_CONFIG_VALUE_STRING, PIXMAPDIR "/nobody.png", MDM_ID_DEFAULT_FACE },
	{ MDM_CONFIG_GROUP_GREETER, "GlobalFaceDir", MDM_CONFIG_VALUE_STRING, DATADIR "/pixmaps/faces/", MDM_ID_GLOBAL_FACE_DIR },
    { MDM_CONFIG_GROUP_GREETER, "GnomeFaceDir", MDM_CONFIG_VALUE_STRING, "/var/lib/AccountsService/icons/", MDM_ID_GNOME_ACCOUNTS_SERVICE_FACE_DIR },
	{ MDM_CONFIG_GROUP_GREETER, "LocaleFile", MDM_CONFIG_VALUE_STRING, MDMLOCALEDIR "/locale.alias", MDM_ID_LOCALE_FILE },		
	{ MDM_CONFIG_GROUP_GREETER, "SystemMenu", MDM_CONFIG_VALUE_BOOL, "true", MDM_ID_SYSTEM_MENU },
	{ MDM_CONFIG_GROUP_DAEMON, "Configurator", MDM_CONFIG_VALUE_STRING, SBINDIR "/mdmsetup --disable-sound --disable-crash-dialog", MDM_ID_CONFIGURATOR },
	{ MDM_CONFIG_GROUP_GREETER, "ConfigAvailable", MDM_CONFIG_VALUE_BOOL, "true", MDM_ID_CONFIG_AVAILABLE },

	/*
	 * For backwards compatibility, do not set values for DEFAULT_WELCOME This will cause these values to always be
	 * read from the config file, and will cause them to return FALSE if
	 * no value is set in the config file.  We want the value, "FALSE" if
	 * the values don't exist in the config file.  The daemon will compare
	 * the Welcome/RemoveWelcome value with the default string and
	 * automatically translate the text if the string is the same as the
	 * default string.  We set the default values of MDM_ID_WELCOME so that the default value is returned when
	 * you run GET_CONFIG on these keys.
	 */
	{ MDM_CONFIG_GROUP_GREETER, "DefaultWelcome", MDM_CONFIG_VALUE_BOOL, "true", MDM_ID_DEFAULT_WELCOME },
	{ MDM_CONFIG_GROUP_GREETER, "Welcome", MDM_CONFIG_VALUE_LOCALE_STRING, MDM_DEFAULT_WELCOME_MSG, MDM_ID_WELCOME },
	{ MDM_CONFIG_GROUP_GREETER, "PrimaryMonitor", MDM_CONFIG_VALUE_STRING, "None", MDM_ID_PRIMARY_MONITOR },
	{ MDM_CONFIG_GROUP_GREETER, "BackgroundProgram", MDM_CONFIG_VALUE_STRING, "", MDM_ID_BACKGROUND_PROGRAM },
	{ MDM_CONFIG_GROUP_GREETER, "RunBackgroundProgramAlways", MDM_CONFIG_VALUE_BOOL, "false", MDM_ID_RUN_BACKGROUND_PROGRAM_ALWAYS },
	{ MDM_CONFIG_GROUP_GREETER, "BackgroundProgramInitialDelay", MDM_CONFIG_VALUE_INT, "30", MDM_ID_BACKGROUND_PROGRAM_INITIAL_DELAY },
	{ MDM_CONFIG_GROUP_GREETER, "RestartBackgroundProgram", MDM_CONFIG_VALUE_BOOL, "true", MDM_ID_RESTART_BACKGROUND_PROGRAM },
	{ MDM_CONFIG_GROUP_GREETER, "BackgroundProgramRestartDelay", MDM_CONFIG_VALUE_INT, "30", MDM_ID_BACKGROUND_PROGRAM_RESTART_DELAY },
	{ MDM_CONFIG_GROUP_GREETER, "BackgroundImage", MDM_CONFIG_VALUE_STRING, "", MDM_ID_BACKGROUND_IMAGE },
	{ MDM_CONFIG_GROUP_GREETER, "BackgroundColor", MDM_CONFIG_VALUE_STRING, "#000000", MDM_ID_BACKGROUND_COLOR },
	{ MDM_CONFIG_GROUP_GREETER, "BackgroundType", MDM_CONFIG_VALUE_INT, "2", MDM_ID_BACKGROUND_TYPE },		
	{ MDM_CONFIG_GROUP_GREETER, "Use24Clock", MDM_CONFIG_VALUE_STRING, "true", MDM_ID_USE_24_CLOCK },
	{ MDM_CONFIG_GROUP_GREETER, "UseCirclesInEntry", MDM_CONFIG_VALUE_BOOL, "false", MDM_ID_ENTRY_CIRCLES },
	{ MDM_CONFIG_GROUP_GREETER, "UseInvisibleInEntry", MDM_CONFIG_VALUE_BOOL, "false", MDM_ID_ENTRY_INVISIBLE },
	{ MDM_CONFIG_GROUP_GREETER, "GraphicalTheme", MDM_CONFIG_VALUE_STRING, "circles", MDM_ID_GRAPHICAL_THEME },	
	{ MDM_CONFIG_GROUP_GREETER, "GraphicalThemeDir", MDM_CONFIG_VALUE_STRING, DATADIR "/mdm/themes/", MDM_ID_GRAPHICAL_THEME_DIR },
    { MDM_CONFIG_GROUP_GREETER, "HTMLTheme", MDM_CONFIG_VALUE_STRING, "mdm", MDM_ID_HTML_THEME },	

	{ MDM_CONFIG_GROUP_GREETER, "InfoMsgFile", MDM_CONFIG_VALUE_STRING, "", MDM_ID_INFO_MSG_FILE },
	{ MDM_CONFIG_GROUP_GREETER, "InfoMsgFont", MDM_CONFIG_VALUE_STRING, "", MDM_ID_INFO_MSG_FONT },

	{ MDM_CONFIG_GROUP_GREETER, "PreFetchProgram", MDM_CONFIG_VALUE_STRING, "", MDM_ID_PRE_FETCH_PROGRAM },

	{ MDM_CONFIG_GROUP_GREETER, "SoundOnLogin", MDM_CONFIG_VALUE_BOOL, "true", MDM_ID_SOUND_ON_LOGIN },
	{ MDM_CONFIG_GROUP_GREETER, "SoundOnLoginSuccess", MDM_CONFIG_VALUE_BOOL, "false", MDM_ID_SOUND_ON_LOGIN_SUCCESS },
	{ MDM_CONFIG_GROUP_GREETER, "SoundOnLoginFailure", MDM_CONFIG_VALUE_BOOL, "false", MDM_ID_SOUND_ON_LOGIN_FAILURE },
	{ MDM_CONFIG_GROUP_GREETER, "SoundOnLoginFile", MDM_CONFIG_VALUE_STRING, "", MDM_ID_SOUND_ON_LOGIN_FILE },
	{ MDM_CONFIG_GROUP_GREETER, "SoundOnLoginSuccessFile", MDM_CONFIG_VALUE_STRING, "", MDM_ID_SOUND_ON_LOGIN_SUCCESS_FILE },
	{ MDM_CONFIG_GROUP_GREETER, "SoundOnLoginFailureFile", MDM_CONFIG_VALUE_STRING, "", MDM_ID_SOUND_ON_LOGIN_FAILURE_FILE },
	{ MDM_CONFIG_GROUP_DAEMON, "SoundProgram", MDM_CONFIG_VALUE_STRING, SOUND_PROGRAM, MDM_ID_SOUND_PROGRAM },

	{ NULL }
};

static const MdmConfigEntry mdm_daemon_server_config_entries [] = {
	/* Per server definitions */
	{ MDM_CONFIG_GROUP_NONE, "name", MDM_CONFIG_VALUE_STRING, "Standard server", MDM_ID_SERVER_NAME },
	{ MDM_CONFIG_GROUP_NONE, "command", MDM_CONFIG_VALUE_STRING, X_SERVER, MDM_ID_SERVER_COMMAND },
	/* runnable as flexi server */
	{ MDM_CONFIG_GROUP_NONE, "flexible", MDM_CONFIG_VALUE_BOOL, "true", MDM_ID_SERVER_FLEXIBLE },
	/* choosable from the login screen */
	{ MDM_CONFIG_GROUP_NONE, "choosable", MDM_CONFIG_VALUE_BOOL, "false", MDM_ID_SERVER_CHOOSABLE },
	/* Login is handled by mdm, otherwise it's a remote server */
	{ MDM_CONFIG_GROUP_NONE, "handled", MDM_CONFIG_VALUE_BOOL, "true", MDM_ID_SERVER_HANDLED },
	/* select a nice level to run the X server at */
	{ MDM_CONFIG_GROUP_NONE, "priority", MDM_CONFIG_VALUE_INT, "0", MDM_ID_SERVER_PRIORITY },
};

G_END_DECLS

#endif /* _MDM_DAEMON_CONFIG_ENTRIES_H */
