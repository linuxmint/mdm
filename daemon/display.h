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

#ifndef _MDM_DISPLAY_H
#define _MDM_DISPLAY_H

#include <X11/Xlib.h> /* for Display */
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h> /* for in_addr */

typedef struct _MdmDisplay MdmDisplay;

#include "mdm-net.h" /* for MdmConnection */

#include "mdm.h"

#define TYPE_STATIC 1		/* X server defined in MDM configuration */
#define TYPE_FLEXI 2		/* Local Flexi X server */

#define SERVER_IS_LOCAL(d) ((d)->type == TYPE_STATIC || (d)->type == TYPE_FLEXI)
#define SERVER_IS_FLEXI(d) ((d)->type == TYPE_FLEXI)

/* Use this to get the right authfile name */
#define MDM_AUTHFILE(display) \
	(display->authfile_mdm != NULL ? display->authfile_mdm : display->authfile)

typedef enum {
	MDM_LOGOUT_ACTION_NONE = 0,
	MDM_LOGOUT_ACTION_HALT,
	MDM_LOGOUT_ACTION_REBOOT,
	MDM_LOGOUT_ACTION_SUSPEND,
	MDM_LOGOUT_ACTION_LAST
} MdmLogoutAction;

struct _MdmDisplay
{
	/* ALL DISPLAY TYPES */

	guint8 type;
	Display *dsp;

	gchar *name;     /* value of DISPLAY */
	gchar *hostname; /* remote hostname */

	char *windowpath; /* path to server "window" */

	guint8 dispstat;
	guint16 dispnum;

	gboolean logged_in; /* TRUE if someone is logged in */
	char *login;

	gboolean attached;  /* Display is physically attached to the machine. */

	gboolean handled;
	gboolean tcp_disallowed;
	int priority;

	gboolean timed_login_ok;

	gboolean try_different_greeter;
	char *theme_name;

	time_t managetime; /* time the display was managed */

	/* loop check stuff */
	time_t last_start_time;
	time_t last_loop_start_time;
	gint retry_count;
	int sleep_before_run;

	gchar *cookie;
	gchar *bcookie;

	gchar *authfile;     /* authfile for the server */
	gchar *authfile_mdm; /* authfile readable by mdm user
				if necessary */
	GSList *auths;
	GSList *local_auths;
	gchar *userauth;
	gboolean authfb;
	time_t last_auth_touch;

	int screenx;
	int screeny;
	int screenwidth; /* Note 0 means use the gdk size */
	int screenheight;
	int lrh_offsetx; /* lower right hand corner x offset */
	int lrh_offsety; /* lower right hand corner y offset */

	pid_t slavepid;
	pid_t greetpid;
	pid_t sesspid;
	pid_t fbconsolepid;
	int last_sess_status; /* status returned by last session */

	/* Notification connection */
	int master_notify_fd;  /* write part of the connection */
	int slave_notify_fd; /* read part of the connection */
	/* The xsession-errors connection */
	int xsession_errors_fd; /* write to the file */
	int session_output_fd; /* read from the session */
	int xsession_errors_bytes;
#define MAX_XSESSION_ERRORS_BYTES (80*2500)  /* maximum number of bytes in
						the ~/.xsession-errors file */
	char *xsession_errors_filename; /* if NULL then there is no .xsession-errors
					   file */

	/* chooser stuff */
	pid_t chooserpid;
	gboolean use_chooser; /* run chooser instead of greeter */
	gchar *chosen_hostname; /* locally chosen hostname if not NULL,
				   "-query chosen_hostname" is appened to server command line */
	int chooser_output_fd; /* from the chooser */
	char *chooser_last_line;
	guint indirect_id;

	gboolean is_emergency_server;
	gboolean failsafe_xserver;

        gchar *xserver_session_args;

	/*
	 * The device associated with the display, if specified in the
	 * configuration file
	 */
	gchar *device_name;

	/* Only set in the main daemon as that's the only place that cares */
	MdmLogoutAction logout_action;

	/* XDMCP TYPE */
	
	struct sockaddr_storage addr;
	struct sockaddr_storage *addrs; /* array of addresses */
	int addr_count; /* number of addresses in array */
	/* Note that the above may in fact be empty even though
	   addr is set, these are just extra addresses
	   (it could also contain addr for all we know) */


	/* ALL LOCAL TYPE (static, flexi) */

	int vt;     /* The VT number used when starting via MDM */
	int vtnum;  /* The VT number of the display */
	pid_t servpid;
	guint8 servstat;
	gchar *command;
	time_t starttime;
	/* order in the Xservers file for sessreg, -1 if unset yet */
	int x_servers_order;

	/* STATIC TYPE */

	gboolean busy_display; /* only needed on static displays since flexi try another */
	time_t last_x_failed;
	int x_faileds;

	/* FLEXI TYPE */

	char *preset_user;
	uid_t server_uid;
	MdmConnection *socket_conn;

};

MdmDisplay *mdm_display_alloc    (gint id, const gchar *command, const gchar *device);
gboolean    mdm_display_manage   (MdmDisplay *d);
void        mdm_display_dispose  (MdmDisplay *d);
void        mdm_display_unmanage (MdmDisplay *d);
MdmDisplay *mdm_display_lookup   (pid_t pid);

#endif /* _MDM_DISPLAY_H */

