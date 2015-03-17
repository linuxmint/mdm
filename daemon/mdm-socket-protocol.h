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

#ifndef _MDM_SOCKET_PROTOCOL_H
#define _MDM_SOCKET_PROTOCOL_H

#include <glib.h>

#define STX 0x2			/* Start of txt */
#define BEL 0x7			/* Bell, used to interrupt login for
				 * say timed login or something similar */

/*
 * Opcodes for the highly sophisticated protocol used for
 * daemon<->greeter communications
 */

/* This will change if there are incompatible
 * protocol changes */
#define MDM_GREETER_PROTOCOL_VERSION "3"

#define MDM_MSG        'D'
#define MDM_NOECHO     'U'
#define MDM_PROMPT     'N'
#define MDM_SESS       'G'
#define MDM_LANG       '&'
#define MDM_SLANG      'R'
#define MDM_SETLANG    'L'
#define MDM_SETSESS    'Z'
#define MDM_RESET      'A'
#define MDM_QUIT       'P'
/* Well these aren't as nice as above, oh well */
#define MDM_STARTTIMER 's'
#define MDM_STOPTIMER  'S'
#define MDM_SETLOGIN   'l' /* this just sets the login to be this, just for
			      the greeters knowledge */
#define MDM_DISABLE    '-' /* disable the login screen */
#define MDM_ENABLE     '+' /* enable the login screen */
#define MDM_RESETOK    'r' /* reset but don't shake */
#define MDM_NEEDPIC    '#' /* need a user picture?, sent after greeter
			    *  is started */
#define MDM_READPIC    '%' /* Send a user picture in a temp file */
#define MDM_ERRBOX     'e' /* Puts string in the error box */
#define MDM_ERRDLG     'E' /* Puts string up in an error dialog */
#define MDM_NOFOCUS    'f' /* Don't focus the login window (optional) */
#define MDM_FOCUS      'F' /* Allow focus on the login window again (optional) */
#define MDM_SAVEDIE    '!' /* Save wm order and die (and set busy cursor) */
#define MDM_QUERY_CAPSLOCK 'Q' /* Is capslock on? */
#define MDM_ALWAYS_RESTART 'W' /* Retart greeter when the user accepts restarts */

/* Different login interruptions */
#define MDM_INTERRUPT_TIMED_LOGIN 'T'
#define MDM_INTERRUPT_CONFIGURE   'C'
#define MDM_INTERRUPT_SUSPEND     'S'
#define MDM_INTERRUPT_SELECT_USER 'U'
#define MDM_INTERRUPT_LOGIN_SOUND 'L'
#define MDM_INTERRUPT_THEME       'H'
#define MDM_INTERRUPT_CANCEL      'X'
#define MDM_INTERRUPT_SELECT_LANG 'O'

/* List delimiter for config file lists */
#define MDM_DELIMITER_MODULES ":"
#define MDM_DELIMITER_THEMES "/:"


/*
 * primitive protocol for controlling the daemon from slave
 * or mdmconfig or whatnot
 */

/* The ones that pass a <slave pid> must be from a valid slave, and
 * the slave will be sent a SIGUSR2.  Nowdays there is a pipe that is
 * used from inside slaves, so those messages may stop being processed
 * by the fifo at some point perhaps.  */
/* The fifo protocol, used only by mdm internally */
#define MDM_SOP_XPID         "XPID" /* <slave pid> <xpid> */
#define MDM_SOP_SESSPID      "SESSPID" /* <slave pid> <sesspid> */
#define MDM_SOP_GREETPID     "GREETPID" /* <slave pid> <greetpid> */
#define MDM_SOP_LOGGED_IN    "LOGGED_IN" /* <slave pid> <logged_in as int> */
#define MDM_SOP_LOGIN        "LOGIN" /* <slave pid> <username> */
#define MDM_SOP_COOKIE       "COOKIE" /* <slave pid> <cookie> */
#define MDM_SOP_AUTHFILE     "AUTHFILE" /* <slave pid> <authfile> */
#define MDM_SOP_QUERYLOGIN   "QUERYLOGIN" /* <slave pid> <username> */
/* if user already logged in somewhere, the ack response will be
   <display>,<migratable>,<display>,<migratable>,... */
#define MDM_SOP_MIGRATE      "MIGRATE" /* <slave pid> <display> */
#define MDM_SOP_DISP_NUM     "DISP_NUM" /* <slave pid> <display as int> */
/* For Linux only currently */
#define MDM_SOP_VT_NUM       "VT_NUM" /* <slave pid> <vt as int> */
#define MDM_SOP_FLEXI_ERR    "FLEXI_ERR" /* <slave pid> <error num> */
	/* 3 = X failed */
	/* 4 = X too busy */
	/* 5 = Nest display can't connect */
#define MDM_SOP_FLEXI_OK     "FLEXI_OK" /* <slave pid> */
#define MDM_SOP_START_NEXT_LOCAL "START_NEXT_LOCAL" /* no arguments */

/* write out a sessreg (xdm) compatible Xservers file
 * in the ServAuthDir as <name>.Xservers */
#define MDM_SOP_WRITE_X_SERVERS "WRITE_X_SERVERS" /* <slave pid> */

/* Suspend the machine if it is even allowed */
#define MDM_SOP_SUSPEND_MACHINE "SUSPEND_MACHINE"  /* no arguments */
#define MDM_SOP_CHOSEN_THEME "CHOSEN_THEME"  /* <slave pid> <theme name> */

#define MDM_SOP_SHOW_ERROR_DIALOG "SHOW_ERROR_DIALOG"  /* show the error dialog from daemon */
#define MDM_SOP_SHOW_YESNO_DIALOG "SHOW_YESNO_DIALOG"  /* show the yesno dialog from daemon */
#define MDM_SOP_SHOW_QUESTION_DIALOG "SHOW_QUESTION_DIALOG"  /* show the question dialog from daemon */
#define MDM_SOP_SHOW_ASKBUTTONS_DIALOG "SHOW_ASKBUTTON_DIALOG"  /* show the askbutton dialog from daemon */

/* Ack for a slave message */
/* Note that an extra response can follow an 'ack' */
#define MDM_SLAVE_NOTIFY_ACK 'A'
/* Update this key */
#define MDM_SLAVE_NOTIFY_KEY '!'
/* notify a command */
#define MDM_SLAVE_NOTIFY_COMMAND '#'
/* send the response */
#define MDM_SLAVE_NOTIFY_RESPONSE 'R'
/* send the error dialog response */
#define MDM_SLAVE_NOTIFY_ERROR_RESPONSE 'E'
/* send the yesno dialog response */
#define MDM_SLAVE_NOTIFY_YESNO_RESPONSE 'Y'
/* send the askbuttons dialog response */
#define MDM_SLAVE_NOTIFY_ASKBUTTONS_RESPONSE 'B'
/* send the question dialog response */
#define MDM_SLAVE_NOTIFY_QUESTION_RESPONSE 'Q'

/*
 * Maximum number of messages allowed over the sockets protocol.  This
 * is set to 80 since the mdmlogin/mdmgreeter programs have ~60 config
 * values that are pulled over the socket connection so it allows them
 * all to be grabbed in one pull.
 */
#define MDM_SUP_MAX_MESSAGES 80
#define MDM_SUP_SOCKET "/var/run/gdm_socket"

/*
 * The user socket protocol.  Each command is given on a separate line
 *
 * A user should first send a VERSION\n after connecting and only do
 * anything else if mdm responds with the correct response.  The version
 * is the mdm version and not a "protocol" revision, so you can't check
 * against a single version but check if the version is higher then some
 * value.
 *
 * You can only send a few commands at a time, so if you keep getting error
 * 200 try opening a new socket for every command you send.
 *
 *
 */
/* The user protocol, using /tmp/.mdm_socket */

#define MDM_SUP_VERSION "VERSION"
#define MDM_SUP_AUTH_LOCAL "AUTH_LOCAL"
#define MDM_SUP_FLEXI_XSERVER "FLEXI_XSERVER"
#define MDM_SUP_ATTACHED_SERVERS "ATTACHED_SERVERS"
#define MDM_SUP_GET_CONFIG "GET_CONFIG"
#define MDM_SUP_GET_CONFIG_FILE  "GET_CONFIG_FILE"
#define MDM_SUP_GET_CUSTOM_CONFIG_FILE  "GET_CUSTOM_CONFIG_FILE"
#define MDM_SUP_UPDATE_CONFIG "UPDATE_CONFIG"
#define MDM_SUP_GREETERPIDS  "GREETERPIDS"
#define MDM_SUP_QUERY_LOGOUT_ACTION "QUERY_LOGOUT_ACTION"
#define MDM_SUP_SET_LOGOUT_ACTION "SET_LOGOUT_ACTION"
#define MDM_SUP_SET_SAFE_LOGOUT_ACTION "SET_SAFE_LOGOUT_ACTION"
#define MDM_SUP_LOGOUT_ACTION_NONE	          "NONE"
#define MDM_SUP_LOGOUT_ACTION_HALT	          "HALT"
#define MDM_SUP_LOGOUT_ACTION_REBOOT	          "REBOOT"
#define MDM_SUP_LOGOUT_ACTION_SUSPEND	          "SUSPEND"
#define MDM_SUP_QUERY_VT "QUERY_VT"
#define MDM_SUP_SET_VT "SET_VT"
#define MDM_SUP_CLOSE        "CLOSE"

/* User flags for the SUP protocol */
enum {
	MDM_SUP_FLAG_AUTHENTICATED = 0x1, /* authenticated as a local user,
					  * from a local display we started */
	MDM_SUP_FLAG_AUTH_GLOBAL = 0x2 /* authenticated with global cookie */
};

#endif /* _MDM_SOCKET_PROTOCOL_H */
