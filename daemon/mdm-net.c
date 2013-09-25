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

#include <strings.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/un.h>
#include <netdb.h>
#include <ctype.h>
#include <fcntl.h>
#include <errno.h>

#include <glib/gi18n.h>

#include "mdm.h"
#include "misc.h"
#include "mdm-net.h"

#include "mdm-common.h"
#include "mdm-log.h"
#include "mdm-daemon-config.h"

/*
 * Kind of a weird setup, new connections whack old connections.
 *
 * We may want to allow tuning of the max connections, since
 * this number means that only a certain number of slaves can
 * talk to the daemon at once.  Note that since new connections
 * whack old connections, this tends to cause traffic problems
 * if the daemon is really being hammered.
 *
 * This is because the slaves retry a failed connection 5 times,
 * though they are at least smart enough to sleep 1 second
 * between retries if the connection failed on the connect()
 * call.  But, this means that throwing off a connection causes
 * that slave to come back in another second and try to
 * connect again.  So if the daemon is really being hammered,
 * this just causes more traffic problems.  It's really faster
 * to let each connection finish.
 *
 * This may cause problems for some setups (perhaps terminal
 * servers) where lots of connections may hit the server at once
 * and 15 connections may not be enough (especially since the
 * console login screen may also be using one of them).  Perhaps
 * this number should be in configuration file so it can be
 * tuned by the end user?
 *
 * If, when you turn on debug, you notice messages like this
 * in the log, "Closing connection, x subconnections reached"
 * and some slaves are not working properly, then bumping this
 * number up is probably a reasonable fix if you can't simply
 * reduce the socket load the daemon must handle.
 */
#define MAX_CONNECTIONS 15

struct _MdmConnection {
	int fd;
	guint source;
	gboolean writable;

	GString *buffer;

	int message_count;

	gboolean nonblock;

	int close_level; /* 0 - normal
			    1 - no close, when called raise to 2
			    2 - close was requested */

	char *filename; /* unix socket or fifo filename */

	guint32 user_flags;

	MdmConnectionHandler handler;
	gpointer data;
	GDestroyNotify destroy_notify;

	gpointer close_data;
	GDestroyNotify close_notify;

	MdmConnection *parent;

	GList *subconnections;
	int n_subconnections;

	MdmDisplay *disp;
};

int 
mdm_connection_is_server_busy (MdmConnection *conn) {
	int max_connections = MAX_CONNECTIONS;

	if (conn->n_subconnections >= (max_connections / 2)) {
		mdm_debug ("Connections is %d, max is %d, busy TRUE",
			conn->n_subconnections, max_connections);
		return TRUE;
	} else {
		mdm_debug ("Connections is %d, max is %d, busy FALSE",
			conn->n_subconnections, max_connections);
		return FALSE;
	}
}

static gboolean
close_if_needed (MdmConnection *conn, GIOCondition cond, gboolean error)
{
	/* non-subconnections are never closed */
	if (conn->parent == NULL)
		return TRUE;

	if (cond & G_IO_ERR ||
	    cond & G_IO_HUP || error) {
	        if (cond & G_IO_ERR)
			mdm_debug ("close_if_needed: Got G_IO_ERR on %d", conn->fd);
	        if (cond & G_IO_HUP)
			mdm_debug ("close_if_needed: Got G_IO_HUP on %d", conn->fd);
		if (error)
			mdm_debug ("close_if_needed: Got error on %d", conn->fd);
		conn->source = 0;
		mdm_connection_close (conn);
		return FALSE;
	}
	return TRUE;
}

static gboolean
mdm_connection_handler (GIOChannel *source,
		        GIOCondition cond,
		        gpointer data)
{
	MdmConnection *conn = data;
	char buf[PIPE_SIZE];
	char *p;
	size_t len;

	if ( ! (cond & G_IO_IN))
		return close_if_needed (conn, cond, FALSE);

	VE_IGNORE_EINTR (len = read (conn->fd, buf, sizeof (buf) -1));
	if (len <= 0)
		return close_if_needed (conn, cond, TRUE);

	buf[len] = '\0';

	if (conn->buffer == NULL)
		conn->buffer = g_string_new (NULL);

	for (p = buf; *p != '\0'; p++) {
		if (*p == '\r' ||
		    (*p == '\n' &&
		     ve_string_empty (conn->buffer->str)))
			/*ignore \r or empty lines*/
			continue;
		if (*p == '\n' || 
		    /* cut lines short at 4096 to prevent DoS attacks */
		    conn->buffer->len > 4096) {
			conn->close_level = 1;
			conn->message_count++;
			conn->handler (conn, conn->buffer->str,
				       conn->data);
			if (conn->close_level == 2) {
				conn->close_level = 0;
				conn->source = 0;
				mdm_connection_close (conn);
				return FALSE;
			}
			conn->close_level = 0;
			g_string_truncate (conn->buffer, 0);
		} else {
			g_string_append_c (conn->buffer, *p);
		}
	}

	return close_if_needed (conn, cond, FALSE);
}

gboolean
mdm_connection_is_writable (MdmConnection *conn)
{
	g_return_val_if_fail (conn != NULL, FALSE);

	return conn->writable;
}

gboolean
mdm_connection_write (MdmConnection *conn, const char *str)
{
	int ret;
	int save_errno;
	int flags = 0;
#ifndef MSG_NOSIGNAL
	void (*old_handler)(int);
#endif

	g_return_val_if_fail (conn != NULL, FALSE);
	g_return_val_if_fail (str != NULL, FALSE);

	if G_UNLIKELY ( ! conn->writable)
		return FALSE;

#ifdef MSG_DONTWAIT
	if (conn->nonblock)
		flags |= MSG_DONTWAIT;
#endif

#ifdef MSG_NOSIGNAL
	VE_IGNORE_EINTR (ret = send (conn->fd, str, strlen (str), MSG_NOSIGNAL | flags));
	save_errno = errno;
#else
	old_handler = signal (SIGPIPE, SIG_IGN);
	VE_IGNORE_EINTR (ret = send (conn->fd, str, strlen (str), flags));
	save_errno = errno;
	signal (SIGPIPE, old_handler);
#endif

	/* just so that 'signal' doesn't whack it */
	errno = save_errno;

	if G_UNLIKELY (ret < 0)
		return FALSE;
	else
		return TRUE;
}

static gboolean
mdm_socket_handler (GIOChannel *source,
		    GIOCondition cond,
		    gpointer data)
{
	GIOChannel *unixchan;
	MdmConnection *conn = data;
	MdmConnection *newconn;
	struct sockaddr_un addr;
	socklen_t addr_size = sizeof (addr);
	int fd;
	int max_connections;

	if ( ! (cond & G_IO_IN))
		return TRUE;

	VE_IGNORE_EINTR (fd = accept (conn->fd,
				   (struct sockaddr *)&addr,
				   &addr_size));
	if G_UNLIKELY (fd < 0) {
		mdm_debug ("mdm_socket_handler: Rejecting connection");
		return TRUE;
	}

	mdm_debug ("mdm_socket_handler: Accepting new connection fd %d", fd);

	newconn = g_new0 (MdmConnection, 1);
	newconn->disp = NULL;
	newconn->message_count = 0;
	newconn->nonblock = conn->nonblock;
	newconn->close_level = 0;
	newconn->fd = fd;
	newconn->writable = TRUE;
	newconn->filename = NULL;
	newconn->user_flags = 0;
	newconn->buffer = NULL;
	newconn->parent = conn;
	newconn->subconnections = NULL;
	newconn->n_subconnections = 0;
	newconn->handler = conn->handler;
	newconn->data = conn->data;
	newconn->destroy_notify = NULL; /* the data belongs to
					   parent connection */

	conn->subconnections = g_list_append (conn->subconnections, newconn);
	conn->n_subconnections++;
	
	max_connections = MAX_CONNECTIONS;
             
	if (conn->n_subconnections > max_connections) {
		MdmConnection *old;
		mdm_debug ("Closing connection, %d subconnections reached",
			max_connections);
		old = conn->subconnections->data;
		conn->subconnections =
			g_list_remove (conn->subconnections, old);
		mdm_connection_close (old);
	}

	unixchan = g_io_channel_unix_new (newconn->fd);
	g_io_channel_set_encoding (unixchan, NULL, NULL);
	g_io_channel_set_buffered (unixchan, FALSE);

	newconn->source = g_io_add_watch_full
		(unixchan, G_PRIORITY_DEFAULT,
		 G_IO_IN|G_IO_PRI|G_IO_ERR|G_IO_HUP|G_IO_NVAL,
		 mdm_connection_handler, newconn, NULL);
	g_io_channel_unref (unixchan);

	return TRUE;
}

MdmConnection *
mdm_connection_open_unix (const char *sockname, mode_t mode)
{
	GIOChannel *unixchan;
	MdmConnection *conn;
	struct sockaddr_un addr;
	int fd;
	int try_again_attempts = 1000;

	fd = socket (AF_UNIX, SOCK_STREAM, 0);
	if G_UNLIKELY (fd < 0) {
		mdm_error ("mdm_connection_open_unix: Could not make socket");
		return NULL;
	}

try_again:
	/* this is all for creating sockets in /tmp/ safely */
	VE_IGNORE_EINTR (g_unlink (sockname));
	if G_UNLIKELY (errno == EISDIR ||
		       errno == EPERM) {
		/* likely a directory, someone's playing tricks on us */
		char *newname = NULL;
		do {
			g_free (newname);
			newname = g_strdup_printf ("%s-renamed-%u",
						   sockname,
						   (guint)g_random_int ());
		} while (g_access (newname, F_OK) == 0);
		VE_IGNORE_EINTR (g_rename (sockname, newname));
		if G_UNLIKELY (errno != 0)
			try_again_attempts = 0;
		g_free (newname);
	} else if G_UNLIKELY (errno != 0) {
		try_again_attempts = 0;
	}

	memset (&addr, 0, sizeof (addr));
	strcpy (addr.sun_path, sockname);
	addr.sun_family = AF_UNIX;
	if G_UNLIKELY (bind (fd,
			     (struct sockaddr *) &addr, sizeof (addr)) < 0) {
		mdm_error ("mdm_connection_open_unix: Could not bind socket");
		try_again_attempts --;
		/* someone is being evil on us */
		if (errno == EADDRINUSE && try_again_attempts >= 0)
			goto try_again;
		VE_IGNORE_EINTR (close (fd));
		return NULL;
	}

	VE_IGNORE_EINTR (g_chmod (sockname, mode));

	conn = g_new0 (MdmConnection, 1);
	conn->disp = NULL;
	conn->message_count = 0;
	conn->nonblock = FALSE;
	conn->close_level = 0;
	conn->fd = fd;
	conn->writable = FALSE;
	conn->buffer = NULL;
	conn->filename = g_strdup (sockname);
	conn->user_flags = 0;
	conn->parent = NULL;
	conn->subconnections = NULL;
	conn->n_subconnections = 0;

	unixchan = g_io_channel_unix_new (conn->fd);
	g_io_channel_set_encoding (unixchan, NULL, NULL);
	g_io_channel_set_buffered (unixchan, FALSE);

	conn->source = g_io_add_watch_full
		(unixchan, G_PRIORITY_DEFAULT,
		 G_IO_IN|G_IO_PRI|G_IO_ERR|G_IO_HUP|G_IO_NVAL,
		 mdm_socket_handler, conn, NULL);
	g_io_channel_unref (unixchan);

	listen (fd, 5);

	return conn;
}

MdmConnection *
mdm_connection_open_fd (int fd)
{
	GIOChannel *unixchan;
	MdmConnection *conn;

	g_return_val_if_fail (fd >= 0, NULL);

	conn = g_new0 (MdmConnection, 1);
	conn->disp = NULL;
	conn->message_count = 0;
	conn->nonblock = FALSE;
	conn->close_level = 0;
	conn->fd = fd;
	conn->writable = FALSE;
	conn->buffer = NULL;
	conn->filename = NULL;
	conn->user_flags = 0;
	conn->parent = NULL;
	conn->subconnections = NULL;
	conn->n_subconnections = 0;

	unixchan = g_io_channel_unix_new (conn->fd);
	g_io_channel_set_encoding (unixchan, NULL, NULL);
	g_io_channel_set_buffered (unixchan, FALSE);

	conn->source = g_io_add_watch_full
		(unixchan, G_PRIORITY_DEFAULT,
		 G_IO_IN|G_IO_PRI|G_IO_ERR|G_IO_HUP|G_IO_NVAL,
		 mdm_connection_handler, conn, NULL);
	g_io_channel_unref (unixchan);

	return conn;
}

MdmConnection *
mdm_connection_open_fifo (const char *fifo, mode_t mode)
{
	GIOChannel *fifochan;
	MdmConnection *conn;
	int fd;

	VE_IGNORE_EINTR (g_unlink (fifo));

	if G_UNLIKELY (mkfifo (fifo, 0660) < 0) {
		mdm_error ("mdm_connection_open_fifo: Could not make FIFO");
		return NULL;
	}

	fd = open (fifo, O_RDWR); /* Open with write to avoid EOF */

	if G_UNLIKELY (fd < 0) {
		mdm_error ("mdm_connection_open_fifo: Could not open FIFO");
		return NULL;
	}

	VE_IGNORE_EINTR (g_chmod (fifo, mode));

	conn = g_new0 (MdmConnection, 1);
	conn->disp = NULL;
	conn->message_count = 0;
	conn->nonblock = FALSE;
	conn->close_level = 0;
	conn->fd = fd;
	conn->writable = FALSE;
	conn->buffer = NULL;
	conn->filename = g_strdup (fifo);
	conn->user_flags = 0;
	conn->parent = NULL;
	conn->subconnections = NULL;
	conn->n_subconnections = 0;

	fifochan = g_io_channel_unix_new (conn->fd);
	g_io_channel_set_encoding (fifochan, NULL, NULL);
	g_io_channel_set_buffered (fifochan, FALSE);

	conn->source = g_io_add_watch_full
		(fifochan, G_PRIORITY_DEFAULT,
		 G_IO_IN|G_IO_PRI|G_IO_ERR|G_IO_HUP|G_IO_NVAL,
		 mdm_connection_handler, conn, NULL);
	g_io_channel_unref (fifochan);

	return conn;
}

void
mdm_connection_set_handler (MdmConnection *conn,
			    MdmConnectionHandler handler,
			    gpointer data,
			    GDestroyNotify destroy_notify)
{
	g_return_if_fail (conn != NULL);

	if (conn->destroy_notify != NULL)
		conn->destroy_notify (conn->data);

	conn->handler = handler;
	conn->data = data;
	conn->destroy_notify = destroy_notify;
}

guint32
mdm_connection_get_user_flags (MdmConnection *conn)
{
	g_return_val_if_fail (conn != NULL, 0);

	return conn->user_flags;
}

void
mdm_connection_set_user_flags (MdmConnection *conn,
			       guint32 flags)
{
	g_return_if_fail (conn != NULL);

	conn->user_flags = flags;
}

void
mdm_connection_close (MdmConnection *conn)
{
	GList *list;

	g_return_if_fail (conn != NULL);

	if (conn->close_level > 0) {
		/* flag that close was requested */
		conn->close_level = 2;
		return;
	}

	if (conn->close_notify != NULL) {
		conn->close_notify (conn->close_data);
		conn->close_notify = NULL;
	}
	conn->close_data = NULL;

	if (conn->buffer != NULL) {
		g_string_free (conn->buffer, TRUE);
		conn->buffer = NULL;
	}

	if (conn->parent != NULL) {
		conn->parent->subconnections =
			g_list_remove (conn->parent->subconnections, conn);
		/* This is evil a bit, but safe, whereas -- would not be */
		conn->parent->n_subconnections =
			g_list_length (conn->parent->subconnections);
		conn->parent = NULL;
	}

	list = conn->subconnections;
	conn->subconnections = NULL;
	g_list_foreach (list, (GFunc) mdm_connection_close, NULL);
	g_list_free (list);

	if (conn->destroy_notify != NULL) {
		conn->destroy_notify (conn->data);
		conn->destroy_notify = NULL;
	}
	conn->data = NULL;

	if (conn->source > 0) {
		g_source_remove (conn->source);
		conn->source = 0;
	}

	if (conn->fd > 0) {
		VE_IGNORE_EINTR (close (conn->fd));
		conn->fd = -1;
	}

	g_free (conn->filename);
	conn->filename = NULL;

	g_free (conn);
}

void
mdm_connection_set_close_notify (MdmConnection *conn,
				 gpointer close_data,
				 GDestroyNotify close_notify)
{
	g_return_if_fail (conn != NULL);

	if (conn->close_notify != NULL)
		conn->close_notify (conn->close_data);

	conn->close_data = close_data;
	conn->close_notify = close_notify;
}

gboolean 
mdm_connection_printf (MdmConnection *conn, const gchar *format, ...)
{
	va_list args;
	gboolean ret;
	gchar *s;

	va_start (args, format);
	s = g_strdup_vprintf (format, args);
	va_end (args);

	ret = mdm_connection_write (conn, s);

	g_free (s);

	return ret;
}

int
mdm_connection_get_message_count (MdmConnection *conn)
{
	g_return_val_if_fail (conn != NULL, -1);
	return conn->message_count;
}

gboolean
mdm_connection_get_nonblock (MdmConnection *conn)
{
	g_return_val_if_fail (conn != NULL, FALSE);
	return conn->nonblock;
}

void
mdm_connection_set_nonblock (MdmConnection *conn,
			     gboolean nonblock)
{
	g_return_if_fail (conn != NULL);
	conn->nonblock = nonblock;
}

MdmDisplay *
mdm_connection_get_display (MdmConnection *conn)
{
	g_return_val_if_fail (conn != NULL, NULL);
	return conn->disp;
}

void
mdm_connection_set_display (MdmConnection *conn,
			    MdmDisplay *disp)
{
	g_return_if_fail (conn != NULL);
	conn->disp = disp;
}

void
mdm_kill_subconnections_with_display (MdmConnection *conn,
				      MdmDisplay *disp)
{
	GList *subs;

	g_return_if_fail (conn != NULL);
	g_return_if_fail (disp != NULL);

	subs = conn->subconnections;
	while (subs != NULL) {
		MdmConnection *subcon = subs->data;
		if (subcon->disp == disp) {
			subcon->disp = NULL;
			conn->subconnections =
				g_list_remove (conn->subconnections, subcon);
			mdm_connection_close (subcon);
			subs = conn->subconnections;
		} else {
			subs = subs->next;
		}
	}
}

/* EOF */
