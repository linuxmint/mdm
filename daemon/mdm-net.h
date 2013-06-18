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

#ifndef MDM_NET_H
#define MDM_NET_H

#include <glib.h>

typedef struct _MdmConnection MdmConnection;

#include "display.h"

/* Macros to check authentication level */
#define MDM_CONN_AUTHENTICATED(conn) \
	((mdm_connection_get_user_flags (conn) & MDM_SUP_FLAG_AUTHENTICATED) || \
	 (mdm_connection_get_user_flags (conn) & MDM_SUP_FLAG_AUTH_GLOBAL))

#define MDM_CONN_AUTH_GLOBAL(conn) \
	 (mdm_connection_get_user_flags (conn) & MDM_SUP_FLAG_AUTH_GLOBAL)


/* Something that will get stuff line by line */
typedef void (* MdmConnectionHandler) (MdmConnection *conn,
				       const char *str,
				       gpointer data);

gboolean	mdm_connection_is_writable (MdmConnection *conn);
gboolean	mdm_connection_write (MdmConnection *conn,
		                      const char *str);
gboolean	mdm_connection_printf (MdmConnection *conn,
				       const gchar *format, ...)
				       G_GNUC_PRINTF (2, 3);

MdmConnection *	mdm_connection_open_unix (const char *sockname,
					  mode_t mode);
MdmConnection * mdm_connection_open_fd (int fd);
MdmConnection *	mdm_connection_open_fifo (const char *fifo,
					  mode_t mode);

void		mdm_connection_set_close_notify (MdmConnection *conn,
						 gpointer close_data,
						 GDestroyNotify close_notify);

void		mdm_connection_set_handler (MdmConnection *conn,
					    MdmConnectionHandler handler,
					    gpointer data,
					    GDestroyNotify destroy_notify);

gboolean	mdm_connection_get_nonblock   (MdmConnection *conn);
void		mdm_connection_set_nonblock   (MdmConnection *conn,
					       gboolean nonblock);

guint32		mdm_connection_get_user_flags (MdmConnection *conn);
void		mdm_connection_set_user_flags (MdmConnection *conn,
					       guint32 flags);
#define		MDM_CONNECTION_SET_USER_FLAG(conn,flag) {			\
			guint32 _flags = mdm_connection_get_user_flags (conn);	\
			_flags |= flag;						\
			mdm_connection_set_user_flags (conn, _flags);		\
		}
#define		MDM_CONNECTION_UNSET_USER_FLAG(conn,flag) {			\
			guint32 _flags = mdm_connection_get_user_flags (conn);	\
			_flags &= ~flag;					\
			mdm_connection_set_user_flags (conn, _flags);		\
		}

MdmDisplay *	mdm_connection_get_display            (MdmConnection *conn);
void		mdm_connection_set_display            (MdmConnection *conn,
					               MdmDisplay *disp);
int		mdm_connection_is_server_busy         (MdmConnection *conn);
void		mdm_kill_subconnections_with_display  (MdmConnection *conn,
						       MdmDisplay *disp);

int		mdm_connection_get_message_count      (MdmConnection *conn);


void		mdm_connection_close                  (MdmConnection *conn);

#endif /* MDM_NET_H */

/* EOF */
