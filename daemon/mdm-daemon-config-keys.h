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

#ifndef _MDM_DAEMON_CONFIG_KEYS_H
#define _MDM_DAEMON_CONFIG_KEYS_H

#include <glib.h>

#include "mdm-config.h"

G_BEGIN_DECLS

/*
 * For backwards compatibility, do not set values for DEFAULT_WELCOME This will cause these values to always be
 * read from the config file, and will cause them to return FALSE if
 * no value is set in the config file.  We want the value "FALSE" if
 * the values don't exist in the config file.  The daemon will compare
 * the Welcome/RemoveWelcome value with the default string and
 * automatically translate the text if the string is the same as the
 * default string.  We set the default values of MDM_KEY_WELCOME so that the default value is returned when
 * you run GET_CONFIG on these keys.
 */
#define MDM_DEFAULT_WELCOME_MSG "Welcome"
#define MDM_DEFAULT_WELCOME_TRANSLATED_MSG N_("Welcome")

/* BEGIN LEGACY KEYS */
#define MDM_KEY_AUTOMATIC_LOGIN_ENABLE "daemon/AutomaticLoginEnable=false"
#define MDM_KEY_AUTOMATIC_LOGIN "daemon/AutomaticLogin="
#define MDM_KEY_GREETER "daemon/Greeter=" LIBEXECDIR "/mdmlogin"
#define MDM_KEY_ADD_GTK_MODULES "daemon/AddGtkModules=false"
#define MDM_KEY_GTK_MODULES_LIST "daemon/GtkModulesList="
#define MDM_KEY_GROUP "daemon/Group=mdm"
#define MDM_KEY_HALT "daemon/HaltCommand=" HALT_COMMAND
#define MDM_KEY_DISPLAY_INIT_DIR "daemon/DisplayInitDir=" MDMCONFDIR "/Init"
#define MDM_KEY_KILL_INIT_CLIENTS "daemon/KillInitClients=true"
#define MDM_KEY_LOG_DIR "daemon/LogDir=" LOGDIR
#define MDM_KEY_PATH "daemon/DefaultPath=" MDM_USER_PATH
#define MDM_KEY_PID_FILE "daemon/PidFile=" MDM_PID_FILE
#define MDM_KEY_POSTSESSION "daemon/PostSessionScriptDir=" MDMCONFDIR "/PostSession/"
#define MDM_KEY_PRESESSION "daemon/PreSessionScriptDir=" MDMCONFDIR "/PreSession/"
#define MDM_KEY_POSTLOGIN "daemon/PostLoginScriptDir=" MDMCONFDIR "/PreSession/"
#define MDM_KEY_FAILSAFE_XSERVER "daemon/FailsafeXServer="
#define MDM_KEY_X_KEEPS_CRASHING "daemon/XKeepsCrashing=" MDMCONFDIR "/XKeepsCrashing"
#define MDM_KEY_REBOOT  "daemon/RebootCommand=" REBOOT_COMMAND
#define MDM_KEY_ROOT_PATH "daemon/RootPath=/sbin:/usr/sbin:" MDM_USER_PATH
#define MDM_KEY_SERV_AUTHDIR "daemon/ServAuthDir=" AUTHDIR
#define MDM_KEY_SESSION_DESKTOP_DIR "daemon/SessionDesktopDir=/etc/X11/sessions/:" DMCONFDIR "/Sessions/:" DATADIR "/mdm/BuiltInSessions/:" DATADIR "/xsessions/"
#define MDM_KEY_DEFAULT_SESSIONS "daemon/DefaultSessions=cinnamon.desktop,mate.desktop,xfce.desktop,kde-plasma.desktop,kde.desktop"
#define MDM_KEY_BASE_XSESSION "daemon/BaseXsession=" MDMCONFDIR "/Xsession"
#define MDM_KEY_DEFAULT_SESSION "daemon/DefaultSession=auto"
#define MDM_KEY_SUSPEND "daemon/SuspendCommand=" SUSPEND_COMMAND
#define MDM_KEY_USER_AUTHDIR "daemon/UserAuthDir="
#define MDM_KEY_USER_AUTHDIR_FALLBACK "daemon/UserAuthFBDir=/tmp"
#define MDM_KEY_USER_AUTHFILE "daemon/UserAuthFile=.Xauthority"
#define MDM_KEY_USER "daemon/User=mdm"
#define MDM_KEY_CONSOLE_NOTIFY "daemon/ConsoleNotify=true"
#define MDM_KEY_DOUBLE_LOGIN_WARNING "daemon/DoubleLoginWarning=true"
#define MDM_KEY_ALWAYS_LOGIN_CURRENT_SESSION "daemon/AlwaysLoginCurrentSession=true"
#define MDM_KEY_DISPLAY_LAST_LOGIN "daemon/DisplayLastLogin=false"
#define MDM_KEY_SELECT_LAST_LOGIN "daemon/SelectLastLogin=true"
#define MDM_KEY_NUMLOCK "daemon/EnableNumLock=false"
#define MDM_KEY_TIMED_LOGIN_ENABLE "daemon/TimedLoginEnable=false"
#define MDM_KEY_TIMED_LOGIN "daemon/TimedLogin="
#define MDM_KEY_TIMED_LOGIN_DELAY "daemon/TimedLoginDelay=30"
#define MDM_KEY_FLEXI_REAP_DELAY_MINUTES "daemon/FlexiReapDelayMinutes=0"
#define MDM_KEY_STANDARD_XSERVER "daemon/StandardXServer=" X_SERVER
#define MDM_KEY_FLEXIBLE_XSERVERS "daemon/FlexibleXServers=5"
#define MDM_KEY_FIRST_VT "daemon/FirstVT=7"
#define MDM_KEY_VT_ALLOCATION "daemon/VTAllocation=true"
#define MDM_KEY_CONSOLE_CANNOT_HANDLE "daemon/ConsoleCannotHandle=am,ar,az,bn,el,fa,gu,hi,ja,ko,ml,mr,pa,ta,zh"
#define MDM_KEY_XSERVER_TIMEOUT "daemon/MdmXserverTimeout=10"
#define MDM_KEY_SYSTEM_COMMANDS_IN_MENU "daemon/SystemCommandsInMenu=HALT;REBOOT;SUSPEND"
#define MDM_KEY_ALLOW_LOGOUT_ACTIONS "daemon/AllowLogoutActions=HALT;REBOOT;SUSPEND"
#define MDM_KEY_RBAC_SYSTEM_COMMAND_KEYS "daemon/RBACSystemCommandKeys=" MDM_RBAC_SYSCMD_KEYS

#define MDM_KEY_SERVER_PREFIX "server-"
#define MDM_KEY_SERVER_NAME "name=Standard server"
#define MDM_KEY_SERVER_COMMAND "command=" X_SERVER
#define MDM_KEY_SERVER_FLEXIBLE "flexible=true"
#define MDM_KEY_SERVER_CHOOSABLE "choosable=false"
#define MDM_KEY_SERVER_HANDLED "handled=true"
#define MDM_KEY_SERVER_PRIORITY "priority=0"

#define MDM_KEY_ALLOW_ROOT "security/AllowRoot=true"
#define MDM_KEY_USER_MAX_FILE "security/UserMaxFile=65536"
#define MDM_KEY_RELAX_PERM "security/RelaxPermissions=0"
#define MDM_KEY_CHECK_DIR_OWNER "security/CheckDirOwner=true"
#define MDM_KEY_SUPPORT_AUTOMOUNT "security/SupportAutomount=false"
#define MDM_KEY_RETRY_DELAY "security/RetryDelay=1"
#define MDM_KEY_DISALLOW_TCP "security/DisallowTCP=true"
#define MDM_KEY_PAM_STACK "security/PamStack=mdm"
#define MDM_KEY_NEVER_PLACE_COOKIES_ON_NFS "security/NeverPlaceCookiesOnNFS=true"
#define MDM_KEY_PASSWORD_REQUIRED "security/PasswordRequired=false"
#define MDM_KEY_UTMP_LINE_ATTACHED "security/UtmpLineAttached="
#define MDM_KEY_UTMP_PSEUDO_DEVICE "security/UtmpPseudoDevice=true"
#define MDM_KEY_GTK_THEME "gui/GtkTheme=Default"
#define MDM_KEY_GTKRC "gui/GtkRC=" DATADIR "/themes/Default/gtk-2.0/gtkrc"
#define MDM_KEY_MAX_ICON_WIDTH "gui/MaxIconWidth=128"
#define MDM_KEY_MAX_ICON_HEIGHT "gui/MaxIconHeight=128"
#define MDM_KEY_ALLOW_GTK_THEME_CHANGE "gui/AllowGtkThemeChange=true"
#define MDM_KEY_GTK_THEMES_TO_ALLOW "gui/GtkThemesToAllow=all"
#define MDM_KEY_BROWSER "greeter/Browser=true"
#define MDM_KEY_INCLUDE "greeter/Include="
#define MDM_KEY_EXCLUDE "greeter/Exclude=bin,daemon,adm,lp,sync,shutdown,halt,mail,news,uucp,operator,nobody,mdm,postgres,pvm,rpm,nfsnobody,pcap"
#define MDM_KEY_INCLUDE_ALL "greeter/IncludeAll=false"
#define MDM_KEY_MINIMAL_UID "greeter/MinimalUID=100"
#define MDM_KEY_DEFAULT_FACE "greeter/DefaultFace=" PIXMAPDIR "/nobody.png"
#define MDM_KEY_GLOBAL_FACE_DIR "greeter/GlobalFaceDir=" DATADIR "/pixmaps/faces/"
#define MDM_KEY_GNOME_ACCOUNTS_SERVICE_FACE_DIR "greeter/GnomeFaceDir=/var/lib/AccountsService/icons/"
#define MDM_KEY_LOCALE_FILE "greeter/LocaleFile=" MDMLOCALEDIR "/locale.alias"
#define MDM_KEY_SYSTEM_MENU "greeter/SystemMenu=true"
#define MDM_KEY_CONFIGURATOR "daemon/Configurator=" SBINDIR "/mdmsetup --disable-sound --disable-crash-dialog"
#define MDM_KEY_CONFIG_AVAILABLE "greeter/ConfigAvailable=true"
#define MDM_KEY_DEFAULT_WELCOME "greeter/DefaultWelcome=true"
#define MDM_KEY_WELCOME "greeter/Welcome=" MDM_DEFAULT_WELCOME_MSG
#define MDM_KEY_PRIMARY_MONITOR "greeter/PrimaryMonitor=0"
#define MDM_KEY_BACKGROUND_PROGRAM "greeter/BackgroundProgram="
#define MDM_KEY_RUN_BACKGROUND_PROGRAM_ALWAYS "greeter/RunBackgroundProgramAlways=false"
#define MDM_KEY_BACKGROUND_PROGRAM_INITIAL_DELAY "greeter/BackgroundProgramInitialDelay=30"
#define MDM_KEY_RESTART_BACKGROUND_PROGRAM "greeter/RestartBackgroundProgram=true"
#define MDM_KEY_BACKGROUND_PROGRAM_RESTART_DELAY "greeter/BackgroundProgramRestartDelay=30"
#define MDM_KEY_BACKGROUND_IMAGE "greeter/BackgroundImage="
#define MDM_KEY_BACKGROUND_COLOR "greeter/BackgroundColor=#000000"
#define MDM_KEY_BACKGROUND_TYPE "greeter/BackgroundType=2"
#define MDM_KEY_USE_24_CLOCK "greeter/Use24Clock=true"
#define MDM_KEY_ENTRY_CIRCLES "greeter/UseCirclesInEntry=false"
#define MDM_KEY_ENTRY_INVISIBLE "greeter/UseInvisibleInEntry=false"
#define MDM_KEY_GRAPHICAL_THEME "greeter/GraphicalTheme=circles"
#define MDM_KEY_GRAPHICAL_THEME_DIR "greeter/GraphicalThemeDir=" DATADIR "/mdm/themes/"
#define MDM_KEY_HTML_THEME "greeter/HTMLTheme=mdm"
#define MDM_KEY_INFO_MSG_FILE "greeter/InfoMsgFile="
#define MDM_KEY_INFO_MSG_FONT "greeter/InfoMsgFont="
#define MDM_KEY_PRE_FETCH_PROGRAM "greeter/PreFetchProgram="
#define MDM_KEY_SOUND_ON_LOGIN "greeter/SoundOnLogin=true"
#define MDM_KEY_SOUND_ON_LOGIN_SUCCESS "greeter/SoundOnLoginSuccess=false"
#define MDM_KEY_SOUND_ON_LOGIN_FAILURE "greeter/SoundOnLoginFailure=false"
#define MDM_KEY_SOUND_ON_LOGIN_FILE "greeter/SoundOnLoginFile="
#define MDM_KEY_SOUND_ON_LOGIN_SUCCESS_FILE "greeter/SoundOnLoginSuccessFile="
#define MDM_KEY_SOUND_ON_LOGIN_FAILURE_FILE "greeter/SoundOnLoginFailureFile="
#define MDM_KEY_SOUND_PROGRAM "daemon/SoundProgram=" SOUND_PROGRAM
#define MDM_KEY_DEBUG "debug/Enable=false"
#define MDM_KEY_LIMIT_SESSION_OUTPUT "debug/LimitSessionOutput=true"
#define MDM_KEY_FILTER_SESSION_OUTPUT "debug/FilterSessionOutput=false"
#define MDM_KEY_DEBUG_GESTURES "debug/Gestures=false"
#define MDM_KEY_SECTION_GREETER "greeter"
#define MDM_KEY_SECTION_SERVERS "servers"
/* END LEGACY KEYS */

/* Notification protocol */
/* keys */
#define MDM_NOTIFY_ALLOW_ROOT "AllowRoot" /* <true/false as int> */
#define MDM_NOTIFY_SYSTEM_MENU "SystemMenu" /* <true/false as int> */
#define MDM_NOTIFY_CONFIG_AVAILABLE "ConfigAvailable" /* <true/false as int> */
#define MDM_NOTIFY_RETRY_DELAY "RetryDelay" /* <seconds> */
#define MDM_NOTIFY_GREETER "Greeter" /* <greeter binary> */
#define MDM_NOTIFY_TIMED_LOGIN "TimedLogin" /* <login> */
#define MDM_NOTIFY_TIMED_LOGIN_DELAY "TimedLoginDelay" /* <seconds> */
#define MDM_NOTIFY_TIMED_LOGIN_ENABLE "TimedLoginEnable" /* <true/false as int> */
#define MDM_NOTIFY_DISALLOW_TCP "DisallowTCP" /* <true/false as int> */
#define MDM_NOTIFY_SOUND_ON_LOGIN_FILE "SoundOnLoginFile" /* <sound file> */
#define MDM_NOTIFY_SOUND_ON_LOGIN_SUCCESS_FILE "SoundOnLoginSuccessFile" /* <sound file> */
#define MDM_NOTIFY_SOUND_ON_LOGIN_FAILURE_FILE "SoundOnLoginFailureFile" /* <sound file> */
#define MDM_NOTIFY_ADD_GTK_MODULES "AddGtkModules" /* <true/false as int> */
#define MDM_NOTIFY_GTK_MODULES_LIST "GtkModulesList" /* <modules list> */

/* commands, seel MDM_SLAVE_NOTIFY_COMMAND */
#define MDM_NOTIFY_DIRTY_SERVERS "DIRTY_SERVERS"
#define MDM_NOTIFY_SOFT_RESTART_SERVERS "SOFT_RESTART_SERVERS"
#define MDM_NOTIFY_GO "GO"
#define MDM_NOTIFY_TWIDDLE_POINTER "TWIDDLE_POINTER"

G_END_DECLS

#endif /* _MDM_DAEMON_CONFIG_KEYS_H */
