/*
 *  MDM - THe GNOME Display Manager
 *  Copyright (C) 2001 Queen of England, (c)2002 George Lebl
 *    
 *  MDMcommunication routines
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *   
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *   
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 * 
 */

#ifndef MDMCOMM_H
#define MDMCOMM_H

void		mdmcomm_set_debug (gboolean enable);
void		mdmcomm_set_quiet_errors (gboolean enable);
char *		mdmcomm_call_mdm (const char *command,
				  const char *auth_cookie,
				  const char *min_version,
				  int tries);
gboolean	mdmcomm_did_connection_fail (void);
void		mdmcomm_set_allow_sleep (gboolean val);
void		mdmcomm_comm_bulk_start (void);
void		mdmcomm_comm_bulk_stop (void);
const char *	mdmcomm_get_display (void);

/* This just gets a cookie of MIT-MAGIC-COOKIE-1 type */
char *		mdmcomm_get_a_cookie (gboolean binary);

/* get the mdm auth cookie */
char *		mdmcomm_get_auth_cookie (void);

gboolean	mdmcomm_check (gboolean show_dialog);
const char *	mdmcomm_get_error_message (const char *ret,
					   gboolean use_xnest);

#endif /* MDMCOMM_H */
