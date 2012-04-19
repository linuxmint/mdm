/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 1998, 1999, 2000 Martin K. Petersen <mkp@mkp.net>
 * Copyright (C) 2007 William Jon McCann <mccann@jhu.edu>
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
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 */

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>

#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <net/if.h>
#ifdef HAVE_SYS_SOCKIO_H
#include <sys/sockio.h>
#endif
#include <sys/ioctl.h>

#include <errno.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <glib-object.h>

#include <X11/Xlib.h>
#include <X11/Xmd.h>
#include <X11/Xdmcp.h>

#include "mdm-common.h"
#include "mdm-xdmcp-manager.h"

#include "misc.h"
#include "auth.h"
#include "cookie.h"
#include "choose.h"
#include "mdm-daemon-config.h"
#include "mdm-log.h"

/*
 * On Sun, we need to define allow_severity and deny_severity to link
 * against libwrap.
 */
#ifdef __sun
#include <syslog.h>
int allow_severity = LOG_INFO;
int deny_severity = LOG_WARNING;
#endif

#define MDM_XDMCP_MANAGER_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), MDM_TYPE_XDMCP_MANAGER, MdmXdmcpManagerPrivate))

#define DEFAULT_PORT                  177
#define DEFAULT_USE_MULTICAST         FALSE
#define DEFAULT_MULTICAST_ADDRESS     "ff02::1"
#define DEFAULT_HONOR_INDIRECT        TRUE
#define DEFAULT_MAX_DISPLAYS_PER_HOST 2
#define DEFAULT_MAX_DISPLAYS          16
#define DEFAULT_MAX_PENDING_DISPLAYS  4
#define DEFAULT_MAX_WAIT              15

#define MDM_MAX_FORWARD_QUERIES 10
#define MDM_FORWARD_QUERY_TIMEOUT 30
#define MANAGED_FORWARD_INTERVAL 1500 /* 1.5 seconds */

/* some extra XDMCP opcodes that xdm will happily ignore since they'll be
 * the wrong XDMCP version anyway */
#define MDM_XDMCP_PROTOCOL_VERSION 1001
enum {
	MDM_XDMCP_FIRST_OPCODE = 1000, /*just a marker, not an opcode */

	MDM_XDMCP_MANAGED_FORWARD = 1000,
		/* manager (master) -> manager
		 * A packet with MANAGED_FORWARD is sent to the
		 * manager that sent the forward query from the manager to
		 * which forward query was sent.  It indicates that the forward
		 * was fully processed and that the client now has either
		 * a managed session, or has been sent denial, refuse or failed.
		 * (if the denial gets lost then client gets dumped into the
		 * chooser again).  This should be resent a few times
		 * until some (short) timeout or until GOT_MANAGED_FORWARD
		 * is sent.  MDM sends at most 3 packates with 1.5 seconds
		 * between each.
		 *
		 * Argument is ARRAY8 with the address of the originating host */
	MDM_XDMCP_GOT_MANAGED_FORWARD,
		/* manager -> manager (master)
		 * A single packet with GOT_MANAGED_FORWARD is sent to indicate
		 * that we did receive the MANAGED_FORWARD packet.  The argument
		 * must match the MANAGED_FORWARD one or it will just be ignored.
		 *
		 * Argument is ARRAY8 with the address of the originating host */
	MDM_XDMCP_LAST_OPCODE /*just a marker, not an opcode */
};

/*
 * We don't support XDM-AUTHENTICATION-1 and XDM-AUTHORIZATION-1.
 *
 * The latter would be quite useful to avoid sending unencrypted
 * cookies over the wire. Unfortunately it isn't supported without
 * XDM-AUTHENTICATION-1 which requires a key database with private
 * keys from all X terminals on your LAN. Fun, fun, fun.
 *
 * Furthermore user passwords go over the wire in cleartext anyway,
 * so protecting cookies is not that important.
 */

typedef struct _XdmAuth {
	ARRAY8 authentication;
	ARRAY8 authorization;
} XdmAuthRec, *XdmAuthPtr;

static XdmAuthRec serv_authlist = {
	{ (CARD16) 0, (CARD8 *) 0 },
	{ (CARD16) 0, (CARD8 *) 0 }
};

/* NOTE: Timeout and max are hardcoded */
typedef struct _MdmForwardQuery {
	time_t                   acctime;
	struct sockaddr_storage *dsp_sa;
	struct sockaddr_storage *from_sa;
} MdmForwardQuery;

typedef struct {
	int                     times;
	guint                   handler;
	struct sockaddr_storage manager;
	struct sockaddr_storage origin;
	MdmXdmcpManager        *xdmcp_manager;
} ManagedForward;

struct MdmXdmcpManagerPrivate
{
	GSList          *forward_queries;
	GSList          *managed_forwards;

	int              socket_fd;
	gint32		 session_serial;
	guint            socket_watch_id;
	XdmcpBuffer      buf;

	guint            num_sessions;
	guint            num_pending_sessions;

	char            *sysid;
	char            *hostname;
	ARRAY8           servhost;

	/* configuration */
	guint            port;
	gboolean         use_multicast;
	char            *multicast_address;
	gboolean         honor_indirect;
	char            *willing_script;
	guint            max_displays_per_host;
	guint            max_displays;
	guint            max_pending_displays;
	guint            max_wait;
};

enum {
	DISPLAY_ADDED,
	DISPLAY_REMOVED,
	LAST_SIGNAL
};

enum {
	PROP_0,
	PROP_PORT,
	PROP_USE_MULTICAST,
	PROP_MULTICAST_ADDRESS,
	PROP_HONOR_INDIRECT,
	PROP_WILLING_SCRIPT,
	PROP_MAX_DISPLAYS_PER_HOST,
	PROP_MAX_DISPLAYS,
	PROP_MAX_PENDING_DISPLAYS,
	PROP_MAX_WAIT,
};

static void	mdm_xdmcp_manager_class_init	(MdmXdmcpManagerClass *klass);
static void	mdm_xdmcp_manager_init	        (MdmXdmcpManager      *manager);
static void	mdm_xdmcp_manager_finalize	(GObject	      *object);

static gpointer xdmcp_manager_object = NULL;

G_DEFINE_TYPE (MdmXdmcpManager, mdm_xdmcp_manager, G_TYPE_OBJECT)

/* Theory of operation:
 *
 * Process idles waiting for UDP packets on port 177.
 * Incoming packets are decoded and checked against tcp_wrapper.
 *
 * A typical session looks like this:
 *
 * Display sends Query/BroadcastQuery to Manager.
 *
 * Manager selects an appropriate authentication scheme from the
 * display's list of supported ones and sends Willing/Unwilling.
 *
 * Assuming the display accepts the auth. scheme it sends back a
 * Request.
 *
 * If the manager accepts to service the display (i.e. loadavg is low)
 * it sends back an Accept containing a unique SessionID. The
 * SessionID is stored in an accept queue by the Manager. Should the
 * manager refuse to start a session a Decline is sent to the display.
 *
 * The display returns a Manage request containing the supplied
 * SessionID. The manager will then start a session on the display. In
 * case the SessionID is not on the accept queue the manager returns
 * Refuse. If the manager fails to open the display for connections
 * Failed is returned.
 *
 * During the session the display periodically sends KeepAlive packets
 * to the manager. The manager responds with Alive.
 *
 * Similarly the manager xpings the display once in a while and shuts
 * down the connection on failure.
 *
 */

GQuark
mdm_xdmcp_manager_error_quark (void)
{
	static GQuark ret = 0;
	if (ret == 0) {
		ret = g_quark_from_static_string ("mdm_xdmcp_manager_error");
	}

	return ret;
}

static gint32
get_next_session_serial (MdmXdmcpManager *manager)
{
	gint32 serial;

 again:
	if (manager->priv->session_serial != G_MAXINT32) {
		serial = manager->priv->session_serial++;
	} else {
		serial = g_random_int ();
	}

	if (serial == 0) {
		goto again;
	}

	return serial;
}

/* for debugging */
static const char *
ai_family_str (struct addrinfo *ai)
{
	const char *str;
	switch (ai->ai_family) {
	case AF_INET:
		str = "inet";
		break;
	case AF_INET6:
		str = "inet6";
		break;
	case AF_UNIX:
		str = "unix";
		break;
	case AF_UNSPEC:
		str = "unspecified";
		break;
	default:
		str = "unknown";
		break;
	}
	return str;
}

/* for debugging */
static const char *
ai_type_str (struct addrinfo *ai)
{
	const char *str;
	switch (ai->ai_socktype) {
	case SOCK_STREAM:
		str = "stream";
		break;
	case SOCK_DGRAM:
		str = "datagram";
		break;
	case SOCK_SEQPACKET:
		str = "seqpacket";
		break;
	case SOCK_RAW:
		str = "raw";
		break;
	default:
		str = "unknown";
		break;
	}
	return str;
}

/* for debugging */
static const char *
ai_protocol_str (struct addrinfo *ai)
{
	const char *str;
	switch (ai->ai_protocol) {
	case 0:
		str = "default";
		break;
	case IPPROTO_TCP:
		str = "TCP";
		break;
	case IPPROTO_UDP:
		str = "UDP";
		break;
	case IPPROTO_RAW:
		str = "raw";
		break;
	default:
		str = "unknown";
		break;
	}

	return str;
}

/* for debugging */
static char *
ai_flags_str (struct addrinfo *ai)
{
	GString *str;

	str = g_string_new ("");
	if (ai->ai_flags == 0) {
		g_string_append (str, "none");
	} else {
		if (ai->ai_flags & AI_PASSIVE) {
			g_string_append (str, "passive ");
		}
		if (ai->ai_flags & AI_CANONNAME) {
			g_string_append (str, "canon ");
		}
		if (ai->ai_flags & AI_NUMERICHOST) {
			g_string_append (str, "numhost ");
		}
		if (ai->ai_flags & AI_NUMERICSERV) {
			g_string_append (str, "numserv ");
		}
		if (ai->ai_flags & AI_V4MAPPED) {
			g_string_append (str, "v4mapped ");
		}
		if (ai->ai_flags & AI_ALL) {
			g_string_append (str, "all ");
		}
	}
	return g_string_free (str, FALSE);
}

/* for debugging */
static void
debug_addrinfo (struct addrinfo *ai)
{
	char *str;
	str = ai_flags_str (ai);
	mdm_debug ("XDMCP: addrinfo family=%s type=%s proto=%s flags=%s",
		   ai_family_str (ai),
		   ai_type_str (ai),
		   ai_protocol_str (ai),
		   str);
	g_free (str);
}

static int
create_socket (struct addrinfo *ai)
{
	int sock;
	int zero = 0;

	sock = socket (ai->ai_family, ai->ai_socktype, ai->ai_protocol);
	if (sock < 0) {
		mdm_error ("socket: %s", g_strerror (errno));
		return sock;
	}

	if (ai->ai_family == AF_INET6) {
		if (setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero) < 0) {
			mdm_error("setsockopt(IPV6_V6ONLY): %s\n", g_strerror(errno));
			close(sock);
			return -1;
		}
	}

	if (bind (sock, ai->ai_addr, ai->ai_addrlen) < 0) {
		mdm_error ("bind: %s", g_strerror (errno));
		close (sock);
		return -1;
	}

	return sock;
}

static int
do_bind (guint                     port,
	 int                       family,
	 struct sockaddr_storage * hostaddr)
{
	struct addrinfo  hints;
	struct addrinfo *ai_list;
	struct addrinfo *ai;
	char             strport[NI_MAXSERV];
	int              gaierr;
	int              sock;

	sock = -1;

	memset (&hints, 0, sizeof (hints));
	hints.ai_family = family;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_flags = AI_PASSIVE;

	snprintf (strport, sizeof (strport), "%u", port);
	if ((gaierr = getaddrinfo (NULL, strport, &hints, &ai_list)) != 0) {
		g_error ("Unable to connect to socket: %s", gai_strerror (gaierr));
		return -1;
	}

	/* should only be one but.. */
	for (ai = ai_list; ai != NULL; ai = ai->ai_next) {
		if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6) {
			continue;
		}

		debug_addrinfo (ai);

		if (sock < 0) {
			char *host;
			char *serv;

			mdm_address_get_info ((struct sockaddr_storage *)ai->ai_addr, &host, &serv);
			mdm_debug ("XDMCP: Attempting to bind to host %s port %s", host, serv);
			g_free (host);
			g_free (serv);
			sock = create_socket (ai);
			if (sock >= 0) {
				if (hostaddr != NULL) {
					memcpy (hostaddr, ai->ai_addr, ai->ai_addrlen);
				}
			}
		}
	}

	freeaddrinfo (ai_list);

	return sock;
}

static void
setup_multicast (MdmXdmcpManager *manager)
{
#ifdef ENABLE_IPV6
	/* Checking and Setting Multicast options */
	{
		/*
		 * socktemp is a temporary socket for getting info about
		 * available interfaces
		 */
		int              socktemp;
		int              i;
		int              num;
		char            *buf;
		struct ipv6_mreq mreq;

		/* For interfaces' list */
		struct ifconf    ifc;
		struct ifreq    *ifr;

		socktemp = socket (AF_INET, SOCK_DGRAM, 0);
#ifdef SIOCGIFNUM
		if (ioctl (socktemp, SIOCGIFNUM, &num) < 0) {
			num = 64;
		}
#else
		num = 64;
#endif /* SIOCGIFNUM */
		ifc.ifc_len = sizeof (struct ifreq) * num;
		ifc.ifc_buf = buf = malloc (ifc.ifc_len);

		if (ioctl (socktemp, SIOCGIFCONF, &ifc) >= 0) {
			ifr = ifc.ifc_req;
			num = ifc.ifc_len / sizeof (struct ifreq); /* No of interfaces */

			/* Joining multicast group with all interfaces */
			for (i = 0 ; i < num ; i++) {
				struct ifreq ifreq;
				int          ifindex;

				memset (&ifreq, 0, sizeof (ifreq));
				strncpy (ifreq.ifr_name, ifr[i].ifr_name, sizeof (ifreq.ifr_name));
				/* paranoia */
				ifreq.ifr_name[sizeof (ifreq.ifr_name) - 1] = '\0';

				if (ioctl (socktemp, SIOCGIFFLAGS, &ifreq) < 0) {
					mdm_debug ("XDMCP: Could not get SIOCGIFFLAGS for %s",
						   ifr[i].ifr_name);
				}

				ifindex = if_nametoindex (ifr[i].ifr_name);

				if ((!(ifreq.ifr_flags & IFF_UP) ||
				     (ifreq.ifr_flags & IFF_LOOPBACK)) ||
				    ((ifindex == 0 ) && (errno == ENXIO))) {
					/* Not a valid interface or loopback interface*/
					continue;
				}

				mreq.ipv6mr_interface = ifindex;
				inet_pton (AF_INET6,
					   manager->priv->multicast_address,
					   &mreq.ipv6mr_multiaddr);

				setsockopt (manager->priv->socket_fd,
					    IPPROTO_IPV6,
					    IPV6_JOIN_GROUP,
					    &mreq,
					    sizeof (mreq));
			}
		}
		g_free (buf);
		close (socktemp);
	}
#endif /* ENABLE_IPV6 */
}

static gboolean
open_port (MdmXdmcpManager *manager)
{
	struct sockaddr_storage serv_sa = { 0 };

	mdm_debug ("XDMCP: Start up on host %s, port %d",
		   manager->priv->hostname,
		 manager->priv->port);

	/* Open socket for communications */
#ifdef ENABLE_IPV6
	manager->priv->socket_fd = do_bind (manager->priv->port, AF_INET6, &serv_sa);
	if (manager->priv->socket_fd < 0)
#endif
		manager->priv->socket_fd = do_bind (manager->priv->port, AF_INET, &serv_sa);

	if G_UNLIKELY (manager->priv->socket_fd < 0) {
		mdm_debug ("Could not create socket!");
		return FALSE;
	}

	if (manager->priv->use_multicast) {
		setup_multicast (manager);
	}

	return TRUE;
}

static gboolean
mdm_xdmcp_host_allow (struct sockaddr_storage *clnt_sa)
{
#ifdef HAVE_TCPWRAPPERS

	/*
	 * Avoids a warning, my tcpd.h file doesn't include this prototype, even
	 * though the library does include the function and the manpage mentions it
	 */
	extern int hosts_ctl (char *daemon,
			      char *client_name,
			      char *client_addr,
			      char *client_user);

	MdmHostent *client_he;
	char       *client;
	gboolean    ret;
	char       *host;

	/* Find client hostname */
	client_he = mdm_gethostbyaddr (clnt_sa);

	if (client_he->not_found) {
		client = "unknown";
	} else {
		mdm_debug ("mdm_xdmcp_host_allow: client->hostname is %s\n",
			   client_he->hostname);
		client = client_he->hostname;
	}

	/* Check with tcp_wrappers if client is allowed to access */
	host = NULL;
	mdm_address_get_info (clnt_sa, &host, NULL);
	ret = hosts_ctl ("mdm", client, host, "");
	g_free (host);

	mdm_hostent_free (client_he);

	return ret;
#else /* HAVE_TCPWRAPPERS */
	return (TRUE);
#endif /* HAVE_TCPWRAPPERS */
}

static int
mdm_xdmcp_num_displays_from_host (MdmXdmcpManager         *manager,
				  struct sockaddr_storage *addr)
{
	GSList *li;
	int     count = 0;
	GSList *displays;

	displays = mdm_daemon_config_get_display_list ();

	for (li = displays; li != NULL; li = li->next) {
		MdmDisplay *disp = li->data;
		if (SERVER_IS_XDMCP (disp)) {
			if (mdm_address_equal (&disp->addr, addr)) {
				count++;
			}
		}
	}
	return count;
}

static MdmDisplay *
mdm_xdmcp_display_lookup_by_host (MdmXdmcpManager         *manager,
				  struct sockaddr_storage *addr,
				  int                      dspnum)
{
	GSList *li;
	GSList *displays;

	displays = mdm_daemon_config_get_display_list ();

	for (li = displays; li != NULL; li = li->next) {
		MdmDisplay *disp = li->data;
		if (SERVER_IS_XDMCP (disp)) {

			if (mdm_address_equal (&disp->addr, addr)
			    && disp->xdmcp_dispnum == dspnum) {
				return disp;
			}
		}
	}

	return NULL;
}

static char *
get_willing_output (MdmXdmcpManager *manager)
{
	char  *output;
	char **argv;
	FILE  *fd;
	char   buf[256];

	output = NULL;
	buf[0] = '\0';

	if (manager->priv->willing_script == NULL) {
		goto out;
	}

	argv = NULL;
	if (! g_shell_parse_argv (manager->priv->willing_script, NULL, &argv, NULL)) {
		goto out;
	}

	if (argv == NULL ||
	    argv[0] == NULL ||
	    g_access (argv[0], X_OK) != 0) {
		goto out;
	}

	fd = popen (manager->priv->willing_script, "r");
	if (fd == NULL) {
		goto out;
	}

	if (fgets (buf, sizeof (buf), fd) == NULL) {
		pclose (fd);
		goto out;
	}

	pclose (fd);

	output = g_strdup (buf);

 out:
	return output;
}

static void
mdm_xdmcp_send_willing (MdmXdmcpManager         *manager,
			struct sockaddr_storage *clnt_sa)
{
	ARRAY8        status;
	XdmcpHeader   header;
	static char  *last_status = NULL;
	static time_t last_willing = 0;
	char         *host;

	mdm_address_get_info (clnt_sa, &host, NULL);
	mdm_debug ("XDMCP: Sending WILLING to %s", host);
	g_free (host);

	if (last_willing == 0 || time (NULL) - 3 > last_willing) {
		char *s;

		g_free (last_status);

		s = get_willing_output (manager);
		if (s != NULL) {
			last_status = s;
		} else {
			last_status = g_strdup (manager->priv->sysid);
		}
	}

	if (! mdm_address_is_local (clnt_sa) &&
	    mdm_xdmcp_num_displays_from_host (manager, clnt_sa) >= manager->priv->max_displays_per_host) {
		/*
		 * Don't translate, this goes over the wire to servers where we
		 * don't know the charset or language, so it must be ascii
		 */
		status.data = (CARD8 *) g_strdup_printf ("%s (Server is busy)",
							 last_status);
	} else {
		status.data = (CARD8 *) g_strdup (last_status);
	}

	status.length = strlen ((char *) status.data);

	header.opcode   = (CARD16) WILLING;
	header.length   = 6 + serv_authlist.authentication.length;
	header.length  += manager->priv->servhost.length + status.length;
	header.version  = XDM_PROTOCOL_VERSION;
	XdmcpWriteHeader (&manager->priv->buf, &header);

	/* Hardcoded authentication */
	XdmcpWriteARRAY8 (&manager->priv->buf, &serv_authlist.authentication);
	XdmcpWriteARRAY8 (&manager->priv->buf, &manager->priv->servhost);
	XdmcpWriteARRAY8 (&manager->priv->buf, &status);

	XdmcpFlush (manager->priv->socket_fd,
		    &manager->priv->buf,
		    (XdmcpNetaddr)clnt_sa,
		    (int)mdm_sockaddr_len(clnt_sa));

	g_free (status.data);
}

static void
mdm_xdmcp_send_unwilling (MdmXdmcpManager         *manager,
			  struct sockaddr_storage *clnt_sa,
			  int                      type)
{
	ARRAY8        status;
	XdmcpHeader   header;
	static time_t last_time = 0;
	char         *host;

	/* only send at most one packet per second,
	   no harm done if we don't send it at all */
	if (last_time + 1 >= time (NULL)) {
		return;
	}

	mdm_address_get_info (clnt_sa, &host, NULL);
	mdm_debug ("XDMCP: Sending UNWILLING to %s", host);
	mdm_debug ("Denied XDMCP query from host %s", host);
	g_free (host);

	/*
	 * Don't translate, this goes over the wire to servers where we
	 * don't know the charset or language, so it must be ascii
	 */
	status.data = (CARD8 *) "Display not authorized to connect";
	status.length = strlen ((char *) status.data);

	header.opcode = (CARD16) UNWILLING;
	header.length = 4 + manager->priv->servhost.length + status.length;
	header.version = XDM_PROTOCOL_VERSION;
	XdmcpWriteHeader (&manager->priv->buf, &header);

	XdmcpWriteARRAY8 (&manager->priv->buf, &manager->priv->servhost);
	XdmcpWriteARRAY8 (&manager->priv->buf, &status);
	XdmcpFlush (manager->priv->socket_fd,
		    &manager->priv->buf,
		    (XdmcpNetaddr)clnt_sa,
		    (int)mdm_sockaddr_len(clnt_sa));

	last_time = time (NULL);
}

#define SIN(__s)   ((struct sockaddr_in *) __s)
#define SIN6(__s)  ((struct sockaddr_in6 *) __s)

static void
set_port_for_request (struct sockaddr_storage *ss,
		      ARRAY8                  *port)
{
	/* we depend on this being 2 elsewhere as well */
	port->length = 2;

	switch (ss->ss_family) {
	case AF_INET:
		port->data = (CARD8 *)g_memdup (&(SIN (ss)->sin_port), port->length);
		break;
	case AF_INET6:
		port->data = (CARD8 *)g_memdup (&(SIN6 (ss)->sin6_port), port->length);
		break;
	default:
		port->data = NULL;
		break;
	}
}

static void
set_address_for_request (struct sockaddr_storage *ss,
			 ARRAY8                  *address)
{

	switch (ss->ss_family) {
	case AF_INET:
		address->length = sizeof (struct in_addr);
		address->data = g_memdup (&SIN (ss)->sin_addr, address->length);
		break;
	case AF_INET6:
		address->length = sizeof (struct in6_addr);
		address->data = g_memdup (&SIN6 (ss)->sin6_addr, address->length);
		break;
	default:
		address->length = 0;
		address->data = NULL;
		break;
	}

}

static void
mdm_xdmcp_send_forward_query (MdmXdmcpManager         *manager,
			      MdmIndirectDisplay      *id,
			      struct sockaddr_storage *clnt_sa,
			      struct sockaddr_storage *display_addr,
			      ARRAYofARRAY8Ptr         authlist)
{
	struct sockaddr_storage *sa;
	XdmcpHeader              header;
	int                      i;
	ARRAY8                   address;
	ARRAY8                   port;
	char                    *host;
	char                    *serv;

	g_assert (id != NULL);
	g_assert (id->chosen_host != NULL);

	mdm_address_get_info (id->chosen_host, &host, NULL);
	mdm_debug ("XDMCP: Sending forward query to %s",
		    host);
	g_free (host);

	mdm_address_get_info (display_addr, &host, &serv);
	mdm_debug ("mdm_xdmcp_send_forward_query: Query contains %s:%s",
		   host, serv);
	g_free (host);
	g_free (serv);

	set_port_for_request (clnt_sa, &port);
	set_address_for_request (display_addr, &address);

	sa = g_memdup (id->chosen_host, sizeof (id->chosen_host));

	header.version = XDM_PROTOCOL_VERSION;
	header.opcode = (CARD16) FORWARD_QUERY;
	header.length = 0;
	header.length += 2 + address.length;
	header.length += 2 + port.length;
	header.length += 1;
	for (i = 0; i < authlist->length; i++) {
		header.length += 2 + authlist->data[i].length;
	}

	XdmcpWriteHeader (&manager->priv->buf, &header);
	XdmcpWriteARRAY8 (&manager->priv->buf, &address);
	XdmcpWriteARRAY8 (&manager->priv->buf, &port);
	XdmcpWriteARRAYofARRAY8 (&manager->priv->buf, authlist);

	XdmcpFlush (manager->priv->socket_fd,
		    &manager->priv->buf,
		    (XdmcpNetaddr) sa,
		    (int)mdm_sockaddr_len(clnt_sa));

	g_free (port.data);
	g_free (address.data);
	g_free (sa);
}

static void
handle_any_query (MdmXdmcpManager         *manager,
		  struct sockaddr_storage *clnt_sa,
		  ARRAYofARRAY8Ptr         authentication_names,
		  int                      type)
{
	mdm_xdmcp_send_willing (manager, clnt_sa);
}

static void
handle_direct_query (MdmXdmcpManager         *manager,
		     struct sockaddr_storage *clnt_sa,
		     int                      len,
		     int                      type)
{
	ARRAYofARRAY8 clnt_authlist;
	int           expected_len;
	int           i;
	int           res;

	res = XdmcpReadARRAYofARRAY8 (&manager->priv->buf, &clnt_authlist);
	if G_UNLIKELY (! res) {
		mdm_debug ("Could not extract authlist from packet");
		return;
	}

	expected_len = 1;

	for (i = 0 ; i < clnt_authlist.length ; i++) {
		expected_len += 2 + clnt_authlist.data[i].length;
	}

	if (len == expected_len) {
		handle_any_query (manager, clnt_sa, &clnt_authlist, type);
	} else {
		mdm_debug ("Error in checksum");
	}

	XdmcpDisposeARRAYofARRAY8 (&clnt_authlist);
}

static void
mdm_xdmcp_handle_broadcast_query (MdmXdmcpManager         *manager,
				  struct sockaddr_storage *clnt_sa,
				  int                      len)
{
	if (mdm_xdmcp_host_allow (clnt_sa)) {
		handle_direct_query (manager, clnt_sa, len, BROADCAST_QUERY);
	} else {
		/* just ignore it */
	}
}

static void
mdm_xdmcp_handle_query (MdmXdmcpManager         *manager,
			struct sockaddr_storage *clnt_sa,
			int                      len)
{
	if (mdm_xdmcp_host_allow (clnt_sa)) {
		handle_direct_query (manager, clnt_sa, len, QUERY);
	} else {
		mdm_xdmcp_send_unwilling (manager, clnt_sa, QUERY);
	}
}

static void
mdm_xdmcp_handle_indirect_query (MdmXdmcpManager         *manager,
				 struct sockaddr_storage *clnt_sa,
				 int                      len)
{
	ARRAYofARRAY8 clnt_authlist;
	int           expected_len;
	int           i;
	int           res;

	if (! mdm_xdmcp_host_allow (clnt_sa)) {
		/* ignore the request */
		return;
	}

	if (! manager->priv->honor_indirect) {
		/* ignore it */
		return;
	}

	res = XdmcpReadARRAYofARRAY8 (&manager->priv->buf, &clnt_authlist);
	if G_UNLIKELY (! res) {
		mdm_debug ("Could not extract authlist from packet");
		return;
	}

	expected_len = 1;

	for (i = 0 ; i < clnt_authlist.length ; i++) {
		expected_len += 2 + clnt_authlist.data[i].length;
	}

	/* Try to look up the display in
	 * the pending list. If found send a FORWARD_QUERY to the
	 * chosen manager. Otherwise alloc a new indirect display. */

	if (len == expected_len) {
		MdmIndirectDisplay *id;

		id = mdm_choose_indirect_lookup (clnt_sa);

		if (id != NULL && id->chosen_host != NULL) {
			/* if user chose us, then just send willing */
			if (mdm_address_is_local (id->chosen_host)) {
				/* get rid of indirect, so that we don't get
				 * the chooser */
				mdm_choose_indirect_dispose (id);
				mdm_xdmcp_send_willing (manager, clnt_sa);
			} else if (mdm_address_is_loopback (clnt_sa)) {
				/* woohoo! fun, I have no clue how to get
				 * the correct ip, SO I just send forward
				 * queries with all the different IPs */
				const GList *list = mdm_address_peek_local_list ();

				while (list != NULL) {
					struct sockaddr_storage *saddr = list->data;

					if (! mdm_address_is_loopback (saddr)) {
						/* forward query to * chosen host */
						mdm_xdmcp_send_forward_query (manager,
									      id,
									      clnt_sa,
									      saddr,
									      &clnt_authlist);
					}

					list = list->next;
				}
			} else {
				/* or send forward query to chosen host */
				mdm_xdmcp_send_forward_query (manager,
							      id,
							      clnt_sa,
							      clnt_sa,
							      &clnt_authlist);
			}
		} else if (id == NULL) {
			id = mdm_choose_indirect_alloc (clnt_sa);
			if (id != NULL) {
				mdm_xdmcp_send_willing (manager, clnt_sa);
			}
		} else  {
			mdm_xdmcp_send_willing (manager, clnt_sa);
		}

	} else {
		mdm_debug ("Error in checksum");
	}

	XdmcpDisposeARRAYofARRAY8 (&clnt_authlist);
}

static void
mdm_forward_query_dispose (MdmXdmcpManager *manager,
			   MdmForwardQuery *q)
{
	if (q == NULL) {
		return;
	}

	manager->priv->forward_queries = g_slist_remove (manager->priv->forward_queries, q);

	q->acctime = 0;

	{
		char *host;

		mdm_address_get_info (q->dsp_sa, &host, NULL);
		mdm_debug ("mdm_forward_query_dispose: Disposing %s", host);
		g_free (host);
	}

	g_free (q->dsp_sa);
	q->dsp_sa = NULL;
	g_free (q->from_sa);
	q->from_sa = NULL;

	g_free (q);
}

static gboolean
remove_oldest_forward (MdmXdmcpManager *manager)
{
	GSList          *li;
	MdmForwardQuery *oldest = NULL;

	for (li = manager->priv->forward_queries; li != NULL; li = li->next) {
		MdmForwardQuery *query = li->data;

		if (oldest == NULL || query->acctime < oldest->acctime) {
			oldest = query;
		}
	}

	if (oldest != NULL) {
		mdm_forward_query_dispose (manager, oldest);
		return TRUE;
	} else {
		return FALSE;
	}
}

static MdmForwardQuery *
mdm_forward_query_alloc (MdmXdmcpManager         *manager,
			 struct sockaddr_storage *mgr_sa,
			 struct sockaddr_storage *dsp_sa)
{
	MdmForwardQuery *q;
	int              count;

	count = g_slist_length (manager->priv->forward_queries);

	while (count > MDM_MAX_FORWARD_QUERIES && remove_oldest_forward (manager)) {
		count--;
	}

	q = g_new0 (MdmForwardQuery, 1);
	q->dsp_sa = g_memdup (dsp_sa, sizeof (struct sockaddr_storage));
	q->from_sa = g_memdup (mgr_sa, sizeof (struct sockaddr_storage));

	manager->priv->forward_queries = g_slist_prepend (manager->priv->forward_queries, q);

	return q;
}

static MdmForwardQuery *
mdm_forward_query_lookup (MdmXdmcpManager         *manager,
			  struct sockaddr_storage *clnt_sa)
{
	GSList          *li;
	GSList          *qlist;
	MdmForwardQuery *q;
	time_t           curtime;

	curtime = time (NULL);

	qlist = g_slist_copy (manager->priv->forward_queries);

	for (li = qlist; li != NULL; li = li->next) {
		q = (MdmForwardQuery *) li->data;

		if (q == NULL)
			continue;

		if (mdm_address_equal (q->dsp_sa, clnt_sa)) {
			g_slist_free (qlist);
			return q;
		}

		if (q->acctime > 0 &&  curtime > q->acctime + MDM_FORWARD_QUERY_TIMEOUT) {
			char *host;
			char *serv;

			mdm_address_get_info (q->dsp_sa, &host, &serv);

			mdm_debug ("mdm_forward_query_lookup: Disposing stale forward query from %s:%s",
				   host, serv);
			g_free (host);
			g_free (serv);

			mdm_forward_query_dispose (manager, q);
			continue;
		}
	}

	g_slist_free (qlist);

	{
		char *host;

		mdm_address_get_info (clnt_sa, &host, NULL);
		mdm_debug ("mdm_forward_query_lookup: Host %s not found",
			   host);
		g_free (host);
	}

	return NULL;
}

static gboolean
create_sa_from_request (ARRAY8 *req_addr,
			ARRAY8 *req_port,
			int    family,
			struct sockaddr_storage **sap)
{
	uint16_t         port;
	char             host_buf [NI_MAXHOST];
	char             serv_buf [NI_MAXSERV];
	char            *serv;
	const char      *host;
	struct addrinfo  hints;
	struct addrinfo *ai_list;
	struct addrinfo *ai;
	int              gaierr;
	gboolean         found;

	if (sap != NULL) {
		*sap = NULL;
	}

	if (req_addr == NULL) {
		return FALSE;
	}

	serv = NULL;
	if (req_port != NULL) {
		/* port must always be length 2 */
		if (req_port->length != 2) {
			return FALSE;
		}

		memcpy (&port, req_port->data, 2);
		snprintf (serv_buf, sizeof (serv_buf), "%d", ntohs (port));
		serv = serv_buf;
	} else {
		/* assume XDM_UDP_PORT */
		snprintf (serv_buf, sizeof (serv_buf), "%d", XDM_UDP_PORT);
		serv = serv_buf;
	}

	host = NULL;
	if (req_addr->length == 4) {
		host = inet_ntop (AF_INET,
				  (const void *)req_addr->data,
				  host_buf,
				  sizeof (host_buf));
	} else if (req_addr->length == 16) {
		host = inet_ntop (AF_INET6,
				  (const void *)req_addr->data,
				  host_buf,
				  sizeof (host_buf));
	}

	if (host == NULL) {
		mdm_debug ("Bad address");
		return FALSE;
	}

	memset (&hints, 0, sizeof (hints));
	hints.ai_family = family;
	hints.ai_flags = AI_V4MAPPED; /* this should convert IPv4 address to IPv6 if needed */
	if ((gaierr = getaddrinfo (host, serv, &hints, &ai_list)) != 0) {
		mdm_debug ("Unable get address: %s", gai_strerror (gaierr));
		return FALSE;
	}

	/* just take the first one */
	ai = ai_list;

	found = FALSE;
	if (ai != NULL) {
		found = TRUE;
		if (sap != NULL) {
			*sap = g_memdup (ai->ai_addr, ai->ai_addrlen);
		}
	}

	freeaddrinfo (ai_list);

	return found;
}

static void
mdm_xdmcp_whack_queued_managed_forwards (MdmXdmcpManager         *manager,
					 struct sockaddr_storage *clnt_sa,
					 struct sockaddr_storage *origin)
{
	GSList *li;

	for (li = manager->priv->managed_forwards; li != NULL; li = li->next) {
		ManagedForward *mf = li->data;

		if (mdm_address_equal (&mf->manager, clnt_sa) &&
		    mdm_address_equal (&mf->origin, origin)) {
			manager->priv->managed_forwards = g_slist_remove_link (manager->priv->managed_forwards, li);
			g_slist_free_1 (li);
			g_source_remove (mf->handler);
			/* mf freed by glib */
			return;
		}
	}
}

static void
mdm_xdmcp_handle_forward_query (MdmXdmcpManager         *manager,
				struct sockaddr_storage *clnt_sa,
				int                      len)
{
	ARRAY8                   clnt_addr;
	ARRAY8                   clnt_port;
	ARRAYofARRAY8            clnt_authlist;
	int                      i;
	int                      explen;
	struct sockaddr_storage *disp_sa;
	char                    *host;
	char                    *serv;

	disp_sa = NULL;

	/* Check with tcp_wrappers if client is allowed to access */
	if (! mdm_xdmcp_host_allow (clnt_sa)) {
		char *host;

		mdm_address_get_info (clnt_sa, &host, NULL);

		mdm_debug ("%s: Got FORWARD_QUERY from banned host %s",
			   "mdm_xdmcp_handle_forward query",
			   host);
		g_free (host);
		return;
	}

	/* Read display address */
	if G_UNLIKELY (! XdmcpReadARRAY8 (&manager->priv->buf, &clnt_addr)) {
		mdm_debug ("%s: Could not read display address",
			   "mdm_xdmcp_handle_forward_query");
		return;
	}

	/* Read display port */
	if G_UNLIKELY (! XdmcpReadARRAY8 (&manager->priv->buf, &clnt_port)) {
		XdmcpDisposeARRAY8 (&clnt_addr);
		mdm_debug ("%s: Could not read display port number",
			   "mdm_xdmcp_handle_forward_query");
		return;
	}

	/* Extract array of authentication names from Xdmcp packet */
	if G_UNLIKELY (! XdmcpReadARRAYofARRAY8 (&manager->priv->buf, &clnt_authlist)) {
		XdmcpDisposeARRAY8 (&clnt_addr);
		XdmcpDisposeARRAY8 (&clnt_port);
		mdm_debug ("%s: Could not extract authlist from packet",
			   "mdm_xdmcp_handle_forward_query");
		return;
	}

	/* Crude checksumming */
	explen = 1;
	explen += 2 + clnt_addr.length;
	explen += 2 + clnt_port.length;

	for (i = 0 ; i < clnt_authlist.length ; i++) {
		char *s = g_strndup ((char *) clnt_authlist.data[i].data,
				     clnt_authlist.length);
		mdm_debug ("mdm_xdmcp_handle_forward_query: authlist: %s", s);
		g_free (s);

		explen += 2 + clnt_authlist.data[i].length;
	}

	if G_UNLIKELY (len != explen) {
		mdm_debug ("%s: Error in checksum",
			   "mdm_xdmcp_handle_forward_query");
		goto out;
	}

	if (! create_sa_from_request (&clnt_addr, &clnt_port, clnt_sa->ss_family, &disp_sa)) {
		mdm_debug ("Unable to parse address for request");
		goto out;
	}

	mdm_xdmcp_whack_queued_managed_forwards (manager,
						 clnt_sa,
						 disp_sa);

	mdm_address_get_info (disp_sa, &host, &serv);
	mdm_debug ("mdm_xdmcp_handle_forward_query: Got FORWARD_QUERY for display: %s, port %s",
		   host, serv);
	g_free (host);
	g_free (serv);

	/* Check with tcp_wrappers if display is allowed to access */
	if (mdm_xdmcp_host_allow (disp_sa)) {
		MdmForwardQuery *q;

		q = mdm_forward_query_lookup (manager, disp_sa);
		if (q != NULL)
			mdm_forward_query_dispose (manager, q);

		mdm_forward_query_alloc (manager, clnt_sa, disp_sa);

		mdm_xdmcp_send_willing (manager, disp_sa);
	}

 out:

	g_free (disp_sa);
	XdmcpDisposeARRAYofARRAY8 (&clnt_authlist);
	XdmcpDisposeARRAY8 (&clnt_port);
	XdmcpDisposeARRAY8 (&clnt_addr);
}

static void
mdm_xdmcp_really_send_managed_forward (MdmXdmcpManager         *manager,
				       struct sockaddr_storage *clnt_sa,
				       struct sockaddr_storage *origin)
{
	ARRAY8      address;
	XdmcpHeader header;
	char       *host;

	mdm_address_get_info (clnt_sa, &host, NULL);
	mdm_debug ("XDMCP: Sending MANAGED_FORWARD to %s", host);
	g_free (host);

	set_address_for_request (origin, &address);

	header.opcode = (CARD16) MDM_XDMCP_MANAGED_FORWARD;
	header.length = 4 + address.length;
	header.version = MDM_XDMCP_PROTOCOL_VERSION;
	XdmcpWriteHeader (&manager->priv->buf, &header);

	XdmcpWriteARRAY8 (&manager->priv->buf, &address);
	XdmcpFlush (manager->priv->socket_fd,
		    &manager->priv->buf,
		    (XdmcpNetaddr)clnt_sa,
		    (int)mdm_sockaddr_len(clnt_sa));

	g_free (address.data);
}

static gboolean
managed_forward_handler (ManagedForward *mf)
{
	if (mf->xdmcp_manager->priv->socket_fd > 0) {
		mdm_xdmcp_really_send_managed_forward (mf->xdmcp_manager,
						       &(mf->manager),
						       &(mf->origin));
	}

	mf->times++;
	if (mf->xdmcp_manager->priv->socket_fd <= 0 || mf->times >= 2) {
		mf->xdmcp_manager->priv->managed_forwards = g_slist_remove (mf->xdmcp_manager->priv->managed_forwards, mf);
		mf->handler = 0;
		/* mf freed by glib */
		return FALSE;
	}
	return TRUE;
}

static void
mdm_xdmcp_send_managed_forward (MdmXdmcpManager         *manager,
				struct sockaddr_storage *clnt_sa,
				struct sockaddr_storage *origin)
{
	ManagedForward *mf;

	mdm_xdmcp_really_send_managed_forward (manager, clnt_sa, origin);

	mf = g_new0 (ManagedForward, 1);
	mf->times = 0;
	mf->xdmcp_manager = manager;

	memcpy (&(mf->manager), clnt_sa, sizeof (struct sockaddr_storage));
	memcpy (&(mf->origin), origin, sizeof (struct sockaddr_storage));

	mf->handler = g_timeout_add_full (G_PRIORITY_DEFAULT,
					  MANAGED_FORWARD_INTERVAL,
					  (GSourceFunc)managed_forward_handler,
					  mf,
					  (GDestroyNotify) g_free);
	manager->priv->managed_forwards = g_slist_prepend (manager->priv->managed_forwards, mf);
}

static void
mdm_xdmcp_send_got_managed_forward (MdmXdmcpManager         *manager,
				    struct sockaddr_storage *clnt_sa,
				    struct sockaddr_storage *origin)
{
	ARRAY8      address;
	XdmcpHeader header;
	char       *host;

	mdm_address_get_info (clnt_sa, &host, NULL);
	mdm_debug ("XDMCP: Sending GOT_MANAGED_FORWARD to %s", host);
	g_free (host);

	set_address_for_request (origin, &address);

	header.opcode = (CARD16) MDM_XDMCP_GOT_MANAGED_FORWARD;
	header.length = 4 + address.length;
	header.version = MDM_XDMCP_PROTOCOL_VERSION;
	XdmcpWriteHeader (&manager->priv->buf, &header);

	XdmcpWriteARRAY8 (&manager->priv->buf, &address);
	XdmcpFlush (manager->priv->socket_fd,
		    &manager->priv->buf,
		    (XdmcpNetaddr)clnt_sa,
		    (int)mdm_sockaddr_len(clnt_sa));
}

static void
mdm_xdmcp_recount_sessions (MdmXdmcpManager *manager)
{
       GSList *li;
       GSList *displays;

       displays = mdm_daemon_config_get_display_list ();

       manager->priv->num_sessions = 0;
       manager->priv->num_pending_sessions = 0;

       for (li = displays; li != NULL; li = li->next) {
               MdmDisplay *d = li->data;
	       if (SERVER_IS_XDMCP (d)) {
		       if (d->dispstat == XDMCP_MANAGED)
			       manager->priv->num_sessions++;
		       else if (d->dispstat == XDMCP_PENDING)
			       manager->priv->num_pending_sessions++;
	       }
       }
}

static void
mdm_xdmcp_displays_purge (MdmXdmcpManager *manager)
{
	GSList *dlist;
	time_t curtime = time (NULL);
	GSList *displays;
	gboolean sess_dirty = FALSE;

	displays = mdm_daemon_config_get_display_list ();

	dlist = displays;
	while (dlist != NULL) {
		MdmDisplay *d = dlist->data;

		if (d != NULL &&
		    SERVER_IS_XDMCP (d) &&
		    d->dispstat == XDMCP_PENDING &&
		    curtime > d->acctime + manager->priv->max_wait) {
			mdm_debug ("mdm_xdmcp_displays_purge: Disposing session id %ld",
				   (long)d->sessionid);
			mdm_display_dispose (d);
			sess_dirty = TRUE;

			/* restart as the list is now broken */
			dlist = displays;
		} else {
			/* just go on */
			dlist = dlist->next;
		}
	}

	/* Recount sessions only if dirty */ 
	if (sess_dirty) {
		mdm_xdmcp_recount_sessions (manager);
	}
}

static void
mdm_xdmcp_display_dispose_check (MdmXdmcpManager *manager,
				 const char      *hostname,
				 int              dspnum)
{
	GSList *dlist;
	GSList *displays;
	gboolean sess_dirty = FALSE;

	if (hostname == NULL) {
		return;
	}

	mdm_debug ("mdm_xdmcp_display_dispose_check (%s:%d)", hostname, dspnum);

	displays = mdm_daemon_config_get_display_list ();

	dlist = displays;
	while (dlist != NULL) {
		MdmDisplay *d = dlist->data;

		if (d != NULL &&
		    SERVER_IS_XDMCP (d) &&
		    d->xdmcp_dispnum == dspnum &&
		    strcmp (d->hostname, hostname) == 0) {

			if (d->dispstat == XDMCP_MANAGED) {
				mdm_display_unmanage (d);
			} else {
				mdm_display_dispose (d);
				sess_dirty = TRUE;
			}

			/* restart as the list is now broken */
			dlist = displays;
		} else {
			/* just go on */
			dlist = dlist->next;
		}
	}

	/* Recount sessions only if dirty */ 
	if (sess_dirty) {
		mdm_xdmcp_recount_sessions (manager);
	}
}

static void
mdm_xdmcp_send_decline (MdmXdmcpManager         *manager,
			struct sockaddr_storage *clnt_sa,
			const char              *reason)
{
	XdmcpHeader      header;
	ARRAY8           authentype;
	ARRAY8           authendata;
	ARRAY8           status;
	MdmForwardQuery *fq;
	char            *host;

	mdm_address_get_info (clnt_sa, &host, NULL);
	mdm_debug ("XDMCP: Sending DECLINE to %s", host);
	g_free (host);

	authentype.data   = (CARD8 *) 0;
	authentype.length = (CARD16)  0;

	authendata.data   = (CARD8 *) 0;
	authendata.length = (CARD16)  0;

	status.data       = (CARD8 *) reason;
	status.length     = strlen ((char *) status.data);

	header.version    = XDM_PROTOCOL_VERSION;
	header.opcode     = (CARD16) DECLINE;
	header.length     = 2 + status.length;
	header.length    += 2 + authentype.length;
	header.length    += 2 + authendata.length;

	XdmcpWriteHeader (&manager->priv->buf, &header);
	XdmcpWriteARRAY8 (&manager->priv->buf, &status);
	XdmcpWriteARRAY8 (&manager->priv->buf, &authentype);
	XdmcpWriteARRAY8 (&manager->priv->buf, &authendata);

	XdmcpFlush (manager->priv->socket_fd,
		    &manager->priv->buf,
		    (XdmcpNetaddr)clnt_sa,
		    (int)mdm_sockaddr_len(clnt_sa));

	/* Send MANAGED_FORWARD to indicate that the connection
	 * reached some sort of resolution */
	fq = mdm_forward_query_lookup (manager, clnt_sa);
	if (fq != NULL) {
		mdm_xdmcp_send_managed_forward (manager, fq->from_sa, clnt_sa);
		mdm_forward_query_dispose (manager, fq);
	}
}

static MdmDisplay *
mdm_xdmcp_display_alloc (MdmXdmcpManager         *manager,
			 struct sockaddr_storage *addr,
			 MdmHostent              *he /* eaten and freed */,
			 int                      displaynum)
{
	MdmDisplay *d  = NULL;
	const char *proxycmd;
	gboolean    use_proxy = FALSE;

	proxycmd = NULL;
	use_proxy = FALSE;
#if 0
	proxycmd = mdm_daemon_config_get_value_string (MDM_KEY_XDMCP_PROXY_XSERVER);
	use_proxy = FALSE;
#endif
	d = g_new0 (MdmDisplay, 1);

	if (use_proxy && proxycmd != NULL) {
		d->type = TYPE_XDMCP_PROXY;
		d->command = g_strdup (proxycmd);
		mdm_debug ("Using proxy server for XDMCP: %s\n", d->command);
	} else {
		d->type = TYPE_XDMCP;
	}

	d->logout_action    = MDM_LOGOUT_ACTION_NONE;
	d->authfile         = NULL;
	d->auths            = NULL;
	d->userauth         = NULL;
	d->greetpid         = 0;
	d->servpid          = 0;
	d->servstat         = 0;
	d->sesspid          = 0;
	d->slavepid         = 0;
	d->attached         = FALSE;
	d->dispstat         = XDMCP_PENDING;
	d->sessionid        = get_next_session_serial (manager);

	d->acctime          = time (NULL);
	d->dispnum          = displaynum;
	d->xdmcp_dispnum    = displaynum;

	d->handled          = TRUE;
	d->tcp_disallowed   = FALSE;
	d->vt               = -1;
	d->vtnum            = -1;
	d->x_servers_order  = -1;
	d->logged_in        = FALSE;
	d->login            = NULL;
	d->sleep_before_run = 0;

	if (mdm_daemon_config_get_value_bool (MDM_KEY_ALLOW_REMOTE_AUTOLOGIN) &&
	    ! ve_string_empty (mdm_daemon_config_get_value_string (MDM_KEY_TIMED_LOGIN))) {
		d->timed_login_ok = TRUE;
	} else {
		d->timed_login_ok = FALSE;
	}

	d->name = g_strdup_printf ("%s:%d",
				   he->hostname,
				   displaynum);

	memcpy (&d->addr, addr, sizeof (struct sockaddr_storage));

	d->hostname              = he->hostname;
	he->hostname             = NULL;
	d->addrs                 = he->addrs;
	he->addrs                = NULL;
	d->addr_count            = he->addr_count;
	he->addr_count           = 0;

	mdm_hostent_free (he);

	d->windowpath            = NULL;
	d->slave_notify_fd       = -1;
	d->master_notify_fd      = -1;
	d->xsession_errors_bytes = 0;
	d->xsession_errors_fd    = -1;
	d->session_output_fd     = -1;
	d->chooser_output_fd     = -1;
	d->chooser_last_line     = NULL;
	d->theme_name            = NULL;

	/* Secure display with cookie */
	if G_UNLIKELY (! mdm_auth_secure_display (d)) {
		mdm_debug ("mdm_xdmcp_display_alloc: Error setting up cookies for %s",
			   d->name);
	}

	if (d->type == TYPE_XDMCP_PROXY) {
		d->parent_disp      = d->name;
		d->name             = g_strdup (":-1");
		d->dispnum          = -1;
		d->server_uid       = mdm_daemon_config_get_mdmuid ();
		d->parent_auth_file = d->authfile;
		d->authfile         = NULL;
	}

	mdm_daemon_config_display_list_append (d);

	manager->priv->num_pending_sessions++;

	mdm_debug ("mdm_xdmcp_display_alloc: display=%s, session id=%ld, xdmcp_pending=%d",
		   d->name, (long)d->sessionid, manager->priv->num_pending_sessions);

	return d;
}

static void
mdm_xdmcp_send_accept (MdmXdmcpManager         *manager,
		       struct sockaddr_storage *clnt_sa,
		       CARD32                   session_id,
		       ARRAY8Ptr                authentication_name,
		       ARRAY8Ptr                authentication_data,
		       ARRAY8Ptr                authorization_name,
		       ARRAY8Ptr                authorization_data)
{
	XdmcpHeader header;
	char       *host;

	header.version    = XDM_PROTOCOL_VERSION;
	header.opcode     = (CARD16) ACCEPT;
	header.length     = 4;
	header.length    += 2 + authentication_name->length;
	header.length    += 2 + authentication_data->length;
	header.length    += 2 + authorization_name->length;
	header.length    += 2 + authorization_data->length;

	XdmcpWriteHeader (&manager->priv->buf, &header);
	XdmcpWriteCARD32 (&manager->priv->buf, session_id);
	XdmcpWriteARRAY8 (&manager->priv->buf, authentication_name);
	XdmcpWriteARRAY8 (&manager->priv->buf, authentication_data);
	XdmcpWriteARRAY8 (&manager->priv->buf, authorization_name);
	XdmcpWriteARRAY8 (&manager->priv->buf, authorization_data);

	XdmcpFlush (manager->priv->socket_fd,
		    &manager->priv->buf,
		    (XdmcpNetaddr)clnt_sa,
		    (int)mdm_sockaddr_len(clnt_sa));

	mdm_address_get_info (clnt_sa, &host, NULL);
	mdm_debug ("XDMCP: Sending ACCEPT to %s with SessionID=%ld",
		   host,
		   (long)session_id);
	g_free (host);
}

static void
mdm_xdmcp_handle_request (MdmXdmcpManager         *manager,
			  struct sockaddr_storage *clnt_sa,
			  int                      len)
{
	CARD16        clnt_dspnum;
	ARRAY16       clnt_conntyp;
	ARRAYofARRAY8 clnt_addr;
	ARRAY8        clnt_authname;
	ARRAY8        clnt_authdata;
	ARRAYofARRAY8 clnt_authorization;
	ARRAY8        clnt_manufacturer;
	int           explen;
	int           i;
	gboolean      mitauth;
	gboolean      entered;
	char         *host;

	mitauth = FALSE;
	entered = FALSE;

	mdm_address_get_info (clnt_sa, &host, NULL);
	mdm_debug ("mdm_xdmcp_handle_request: Got REQUEST from %s", host);

	/* Check with tcp_wrappers if client is allowed to access */
	if (! mdm_xdmcp_host_allow (clnt_sa)) {
		mdm_debug ("%s: Got REQUEST from banned host %s",
			   "mdm_xdmcp_handle_request",
			   host);
		g_free (host);
		return;
	}
	g_free (host);

	mdm_xdmcp_displays_purge (manager); /* Purge pending displays */

	/* Update num_sessions only if the length of the list that contain
 	 * them is smaller */ 
	if (g_slist_length (mdm_daemon_config_get_display_list()) < manager->priv->num_sessions ) {
		mdm_xdmcp_recount_sessions (manager);
	}

	/* Remote display number */
	if G_UNLIKELY (! XdmcpReadCARD16 (&manager->priv->buf, &clnt_dspnum)) {
		mdm_debug ("%s: Could not read Display Number",
			   "mdm_xdmcp_handle_request");
		return;
	}

	/* We don't care about connection type. Address says it all */
	if G_UNLIKELY (! XdmcpReadARRAY16 (&manager->priv->buf, &clnt_conntyp)) {
		mdm_debug ("%s: Could not read Connection Type",
			   "mdm_xdmcp_handle_request");
		return;
	}

	/* This is TCP/IP - we don't care */
	if G_UNLIKELY (! XdmcpReadARRAYofARRAY8 (&manager->priv->buf, &clnt_addr)) {
		mdm_debug ("%s: Could not read Client Address",
			   "mdm_xdmcp_handle_request");
		XdmcpDisposeARRAY16 (&clnt_conntyp);
		return;
	}

	/* Read authentication type */
	if G_UNLIKELY (! XdmcpReadARRAY8 (&manager->priv->buf, &clnt_authname)) {
		mdm_debug ("%s: Could not read Authentication Names",
			   "mdm_xdmcp_handle_request");
		XdmcpDisposeARRAYofARRAY8 (&clnt_addr);
		XdmcpDisposeARRAY16 (&clnt_conntyp);
		return;
	}

	/* Read authentication data */
	if G_UNLIKELY (! XdmcpReadARRAY8 (&manager->priv->buf, &clnt_authdata)) {
		mdm_debug ("%s: Could not read Authentication Data",
			   "mdm_xdmcp_handle_request");
		XdmcpDisposeARRAYofARRAY8 (&clnt_addr);
		XdmcpDisposeARRAY16 (&clnt_conntyp);
		XdmcpDisposeARRAY8 (&clnt_authname);
		return;
	}

	/* Read and select from supported authorization list */
	if G_UNLIKELY (! XdmcpReadARRAYofARRAY8 (&manager->priv->buf, &clnt_authorization)) {
		mdm_debug ("%s: Could not read Authorization List",
			   "mdm_xdmcp_handle_request");
		XdmcpDisposeARRAY8 (&clnt_authdata);
		XdmcpDisposeARRAYofARRAY8 (&clnt_addr);
		XdmcpDisposeARRAY16 (&clnt_conntyp);
		XdmcpDisposeARRAY8 (&clnt_authname);
		return;
	}

	/* libXdmcp doesn't terminate strings properly so we cheat and use strncmp () */
	for (i = 0 ; i < clnt_authorization.length ; i++)
		if (clnt_authorization.data[i].length == 18 &&
		    strncmp ((char *) clnt_authorization.data[i].data,
			     "MIT-MAGIC-COOKIE-1", 18) == 0)
			mitauth = TRUE;

	/* Manufacturer ID */
	if G_UNLIKELY (! XdmcpReadARRAY8 (&manager->priv->buf, &clnt_manufacturer)) {
		mdm_debug ("%s: Could not read Manufacturer ID",
			   "mdm_xdmcp_handle_request");
		XdmcpDisposeARRAY8 (&clnt_authname);
		XdmcpDisposeARRAY8 (&clnt_authdata);
		XdmcpDisposeARRAYofARRAY8 (&clnt_addr);
		XdmcpDisposeARRAYofARRAY8 (&clnt_authorization);
		XdmcpDisposeARRAY16 (&clnt_conntyp);
		return;
	}

	/* Crude checksumming */
	explen = 2;		    /* Display Number */
	explen += 1 + 2 * clnt_conntyp.length; /* Connection Type */
	explen += 1;		    /* Connection Address */
	for (i = 0 ; i < clnt_addr.length ; i++)
		explen += 2 + clnt_addr.data[i].length;
	explen += 2 + clnt_authname.length; /* Authentication Name */
	explen += 2 + clnt_authdata.length; /* Authentication Data */
	explen += 1;		    /* Authorization Names */
	for (i = 0 ; i < clnt_authorization.length ; i++)
		explen += 2 + clnt_authorization.data[i].length;
	explen += 2 + clnt_manufacturer.length;

	if G_UNLIKELY (explen != len) {
		mdm_address_get_info (clnt_sa, &host, NULL);
		mdm_debug ("%s: Failed checksum from %s",
			   "mdm_xdmcp_handle_request",
			   host);
		g_free (host);

		XdmcpDisposeARRAY8 (&clnt_authname);
		XdmcpDisposeARRAY8 (&clnt_authdata);
		XdmcpDisposeARRAY8 (&clnt_manufacturer);
		XdmcpDisposeARRAYofARRAY8 (&clnt_addr);
		XdmcpDisposeARRAYofARRAY8 (&clnt_authorization);
		XdmcpDisposeARRAY16 (&clnt_conntyp);
		return;
	}

	{
		char *s = g_strndup ((char *) clnt_manufacturer.data, clnt_manufacturer.length);
		mdm_debug ("mdm_xdmcp_handle_request: xdmcp_pending=%d, MaxPending=%d, xdmcp_sessions=%d, MaxSessions=%d, ManufacturerID=%s",
			   manager->priv->num_pending_sessions,
			   manager->priv->max_pending_displays,
			   manager->priv->num_sessions,
			   manager->priv->max_displays,
			   ve_sure_string (s));
		g_free (s);
	}

	/* Check if ok to manage display */
	if (mitauth &&
	    manager->priv->num_sessions < manager->priv->max_displays &&
	    (mdm_address_is_local (clnt_sa) ||
	     mdm_xdmcp_num_displays_from_host (manager, clnt_sa) < manager->priv->max_displays_per_host)) {
		entered = TRUE;
	}

	if (entered) {
		MdmHostent *he;
		he = mdm_gethostbyaddr (clnt_sa);

		/* Check if we are already talking to this host */
		mdm_xdmcp_display_dispose_check (manager, he->hostname, clnt_dspnum);

		if (manager->priv->num_pending_sessions >= manager->priv->max_pending_displays) {
			mdm_debug ("mdm_xdmcp_handle_request: maximum pending");
			/* Don't translate, this goes over the wire to servers where we
			 * don't know the charset or language, so it must be ascii */
			mdm_xdmcp_send_decline (manager, clnt_sa, "Maximum pending servers");
			mdm_hostent_free (he);
		} else {
			MdmDisplay *d;

			d = mdm_xdmcp_display_alloc (manager,
						     clnt_sa,
						     he /* eaten and freed */,
						     clnt_dspnum);
			if (d != NULL) {
				ARRAY8 authentication_name;
				ARRAY8 authentication_data;
				ARRAY8 authorization_name;
				ARRAY8 authorization_data;

				authentication_name.data   = NULL;
				authentication_name.length = 0;
				authentication_data.data   = NULL;
				authentication_data.length = 0;

				authorization_name.data     = (CARD8 *) "MIT-MAGIC-COOKIE-1";
				authorization_name.length   = strlen ((char *) authorization_name.data);

				authorization_data.data     = (CARD8 *) d->bcookie;
				authorization_data.length   = 16;

				/* the addrs are NOT copied */
				mdm_xdmcp_send_accept (manager,
						       clnt_sa,
						       d->sessionid,
						       &authentication_name,
						       &authentication_data,
						       &authorization_name,
						       &authorization_data);
			}
		}
	} else {
		/* Don't translate, this goes over the wire to servers where we
		 * don't know the charset or language, so it must be ascii */
		if ( ! mitauth) {
			mdm_xdmcp_send_decline (manager,
						clnt_sa,
						"Only MIT-MAGIC-COOKIE-1 supported");
		} else if (manager->priv->num_sessions >= manager->priv->max_displays) {
			mdm_debug ("Maximum number of open XDMCP sessions reached");
			mdm_xdmcp_send_decline (manager,
						clnt_sa,
						"Maximum number of open sessions reached");
		} else {
			mdm_debug ("Maximum number of open XDMCP sessions from host %s reached",
				   host);
			mdm_xdmcp_send_decline (manager,
						clnt_sa,
						"Maximum number of open sessions from your host reached");
		}
	}

	XdmcpDisposeARRAY8 (&clnt_authname);
	XdmcpDisposeARRAY8 (&clnt_authdata);
	XdmcpDisposeARRAY8 (&clnt_manufacturer);
	XdmcpDisposeARRAYofARRAY8 (&clnt_addr);
	XdmcpDisposeARRAYofARRAY8 (&clnt_authorization);
	XdmcpDisposeARRAY16 (&clnt_conntyp);
}

static MdmDisplay *
mdm_xdmcp_display_lookup (MdmXdmcpManager *manager,
			  CARD32           sessid)
{
	GSList     *l;
	MdmDisplay *d;
	GSList *displays;

	if (sessid == 0) {
		return (NULL);
	}

	displays = mdm_daemon_config_get_display_list ();

	l = displays;
	while (l != NULL) {
		d = (MdmDisplay *) l->data;

		if (d && d->sessionid == sessid) {
			return (d);
		}

		l = l->next;
	}

	return (NULL);
}

static void
mdm_xdmcp_send_failed (MdmXdmcpManager         *manager,
		       struct sockaddr_storage *clnt_sa,
		       CARD32                   sessid)
{
	XdmcpHeader header;
	ARRAY8      status;

	mdm_debug ("XDMCP: Sending FAILED to %ld", (long)sessid);

	/*
	 * Don't translate, this goes over the wire to servers where we
	 * don't know the charset or language, so it must be ascii
	 */
	status.data    = (CARD8 *) "Failed to start session";
	status.length  = strlen ((char *) status.data);

	header.version = XDM_PROTOCOL_VERSION;
	header.opcode  = (CARD16) FAILED;
	header.length  = 6+status.length;

	XdmcpWriteHeader (&manager->priv->buf, &header);
	XdmcpWriteCARD32 (&manager->priv->buf, sessid);
	XdmcpWriteARRAY8 (&manager->priv->buf, &status);

	XdmcpFlush (manager->priv->socket_fd,
		    &manager->priv->buf,
		    (XdmcpNetaddr)clnt_sa,
		    (int)mdm_sockaddr_len(clnt_sa));
}

static void
mdm_xdmcp_send_refuse (MdmXdmcpManager         *manager,
		       struct sockaddr_storage *clnt_sa,
		       CARD32                   sessid)
{
	XdmcpHeader      header;
	MdmForwardQuery *fq;

	mdm_debug ("XDMCP: Sending REFUSE to %ld",
		   (long)sessid);

	header.version = XDM_PROTOCOL_VERSION;
	header.opcode  = (CARD16) REFUSE;
	header.length  = 4;

	XdmcpWriteHeader (&manager->priv->buf, &header);
	XdmcpWriteCARD32 (&manager->priv->buf, sessid);

	XdmcpFlush (manager->priv->socket_fd,
		    &manager->priv->buf,
		    (XdmcpNetaddr)clnt_sa,
		    (int)mdm_sockaddr_len(clnt_sa));

	/*
	 * This was from a forwarded query quite apparently so
	 * send MANAGED_FORWARD
	 */
	fq = mdm_forward_query_lookup (manager, clnt_sa);
	if (fq != NULL) {
		mdm_xdmcp_send_managed_forward (manager, fq->from_sa, clnt_sa);
		mdm_forward_query_dispose (manager, fq);
	}
}

static void
mdm_xdmcp_handle_manage (MdmXdmcpManager         *manager,
			 struct sockaddr_storage *clnt_sa,
			 int                      len)
{
	CARD32              clnt_sessid;
	CARD16              clnt_dspnum;
	ARRAY8              clnt_dspclass;
	MdmDisplay         *d;
	MdmIndirectDisplay *id;
	MdmForwardQuery    *fq;
	char               *host;

	mdm_address_get_info (clnt_sa, &host, NULL);
	mdm_debug ("mdm_xdmcp_handle_manage: Got MANAGE from %s", host);

	/* Check with tcp_wrappers if client is allowed to access */
	if (! mdm_xdmcp_host_allow (clnt_sa)) {
		mdm_debug ("%s: Got Manage from banned host %s",
			   "mdm_xdmcp_handle_manage",
			   host);
		g_free (host);
		return;
	}

	/* SessionID */
	if G_UNLIKELY (! XdmcpReadCARD32 (&manager->priv->buf, &clnt_sessid)) {
		mdm_debug ("%s: Could not read Session ID",
			   "mdm_xdmcp_handle_manage");
		g_free (host);
		return;
	}

	/* Remote display number */
	if G_UNLIKELY (! XdmcpReadCARD16 (&manager->priv->buf, &clnt_dspnum)) {
		mdm_debug ("%s: Could not read Display Number",
			   "mdm_xdmcp_handle_manage");
		g_free (host);
		return;
	}

	/* Display Class */
	if G_UNLIKELY (! XdmcpReadARRAY8 (&manager->priv->buf, &clnt_dspclass)) {
		mdm_debug ("%s: Could not read Display Class",
			   "mdm_xdmcp_handle_manage");
		g_free (host);
		return;
	}

	if G_UNLIKELY (mdm_daemon_config_get_value_bool (MDM_KEY_DEBUG)) {
		char *s = g_strndup ((char *) clnt_dspclass.data, clnt_dspclass.length);
		mdm_debug ("mdm_xdmcp-handle_manage: Got display=%d, SessionID=%ld Class=%s from %s",
			   (int)clnt_dspnum, (long)clnt_sessid, ve_sure_string (s), host);
		g_free (s);
	}
	g_free (host);

	d = mdm_xdmcp_display_lookup (manager, clnt_sessid);
	if (d != NULL &&
	    d->dispstat == XDMCP_PENDING) {

		mdm_debug ("mdm_xdmcp_handle_manage: Looked up %s", d->name);

		if (manager->priv->honor_indirect) {
			id = mdm_choose_indirect_lookup (clnt_sa);

			/* This was an indirect thingie and nothing was yet chosen,
			 * use a chooser */
			if (d->dispstat == XDMCP_PENDING &&
			    id != NULL &&
			    id->chosen_host == NULL) {
				d->use_chooser = TRUE;
				d->indirect_id = id->id;
			} else {
				d->indirect_id = 0;
				d->use_chooser = FALSE;
				if (id != NULL)
					mdm_choose_indirect_dispose (id);
			}
		} else {
			d->indirect_id = 0;
			d->use_chooser = FALSE;
		}

		/* this was from a forwarded query quite apparently so
		 * send MANAGED_FORWARD */
		fq = mdm_forward_query_lookup (manager, clnt_sa);
		if (fq != NULL) {
			mdm_xdmcp_send_managed_forward (manager, fq->from_sa, clnt_sa);
			mdm_forward_query_dispose (manager, fq);
		}

		d->dispstat = XDMCP_MANAGED;
		manager->priv->num_sessions++;
		manager->priv->num_pending_sessions--;

		/* Start greeter/session */
		if G_UNLIKELY (!mdm_display_manage (d)) {
			mdm_xdmcp_send_failed (manager, clnt_sa, clnt_sessid);
			XdmcpDisposeARRAY8 (&clnt_dspclass);
			return;
		}
	} else if G_UNLIKELY (d != NULL && d->dispstat == XDMCP_MANAGED) {
		mdm_debug ("mdm_xdmcp_handle_manage: Session id %ld already managed",
			   (long)clnt_sessid);
	} else {
		mdm_debug ("mdm_xdmcp_handle_manage: Failed to look up session id %ld",
			   (long)clnt_sessid);
		mdm_xdmcp_send_refuse (manager, clnt_sa, clnt_sessid);
	}

	XdmcpDisposeARRAY8 (&clnt_dspclass);
}

static void
mdm_xdmcp_handle_managed_forward (MdmXdmcpManager         *manager,
				  struct sockaddr_storage *clnt_sa,
				  int                      len)
{
	ARRAY8 clnt_address;
	MdmIndirectDisplay *id;
	char *host;
	struct sockaddr_storage *disp_sa;

	mdm_address_get_info (clnt_sa, &host, NULL);
	mdm_debug ("mdm_xdmcp_handle_managed_forward: Got MANAGED_FORWARD from %s",
		   host);

	/* Check with tcp_wrappers if client is allowed to access */
	if (! mdm_xdmcp_host_allow (clnt_sa)) {
		mdm_debug ("%s: Got MANAGED_FORWARD from banned host %s",
			   "mdm_xdmcp_handle_request", host);
		g_free (host);
		return;
	}
	g_free (host);

	/* Hostname */
	if G_UNLIKELY ( ! XdmcpReadARRAY8 (&manager->priv->buf, &clnt_address)) {
		mdm_debug ("%s: Could not read address",
			   "mdm_xdmcp_handle_managed_forward");
		return;
	}

	disp_sa = NULL;
	if (! create_sa_from_request (&clnt_address, NULL, clnt_sa->ss_family, &disp_sa)) {
		mdm_debug ("Unable to parse address for request");
		XdmcpDisposeARRAY8 (&clnt_address);
		return;
	}

	id = mdm_choose_indirect_lookup_by_chosen (clnt_sa, disp_sa);
	if (id != NULL) {
		mdm_choose_indirect_dispose (id);
	}

	/* Note: we send GOT even on not found, just in case our previous
	 * didn't get through and this was a second managed forward */
	mdm_xdmcp_send_got_managed_forward (manager, clnt_sa, disp_sa);

	XdmcpDisposeARRAY8 (&clnt_address);
}

static void
mdm_xdmcp_handle_got_managed_forward (MdmXdmcpManager         *manager,
				      struct sockaddr_storage *clnt_sa,
				      int                      len)
{
	struct sockaddr_storage *disp_sa;
	ARRAY8 clnt_address;
	char  *host;

	mdm_address_get_info (clnt_sa, &host, NULL);
	mdm_debug ("mdm_xdmcp_handle_got_managed_forward: Got MANAGED_FORWARD from %s",
		   host);

	if (! mdm_xdmcp_host_allow (clnt_sa)) {
		mdm_debug ("%s: Got GOT_MANAGED_FORWARD from banned host %s",
			   "mdm_xdmcp_handle_request", host);
		g_free (host);
		return;
	}
	g_free (host);

	/* Hostname */
	if G_UNLIKELY ( ! XdmcpReadARRAY8 (&manager->priv->buf, &clnt_address)) {
		mdm_debug ("%s: Could not read address",
			   "mdm_xdmcp_handle_got_managed_forward");
		return;
	}

	if (! create_sa_from_request (&clnt_address, NULL, clnt_sa->ss_family, &disp_sa)) {
		mdm_debug ("%s: Could not read address",
			   "mdm_xdmcp_handle_got_managed_forward");
		XdmcpDisposeARRAY8 (&clnt_address);
		return;
	}

	mdm_xdmcp_whack_queued_managed_forwards (manager, clnt_sa, disp_sa);

	XdmcpDisposeARRAY8 (&clnt_address);
}

static void
mdm_xdmcp_send_alive (MdmXdmcpManager         *manager,
		      struct sockaddr_storage *clnt_sa,
                      CARD16                   dspnum,
		      CARD32                   sessid)
{
	XdmcpHeader header;
	MdmDisplay *d;
	int send_running = 0;
	CARD32 send_sessid = 0;

	d = mdm_xdmcp_display_lookup (manager, sessid);
	if (d == NULL) {
		d = mdm_xdmcp_display_lookup_by_host (manager, clnt_sa, dspnum);
	}

	if (d != NULL) {
		send_sessid = d->sessionid;
		if (d->dispstat == XDMCP_MANAGED) {
			send_running = 1;
		}
	}

	mdm_debug ("XDMCP: Sending ALIVE to %ld (running %d, sessid %ld)",
		   (long)sessid,
		   send_running,
		   (long)send_sessid);

	header.version = XDM_PROTOCOL_VERSION;
	header.opcode = (CARD16) ALIVE;
	header.length = 5;

	XdmcpWriteHeader (&manager->priv->buf, &header);
	XdmcpWriteCARD8 (&manager->priv->buf, send_running);
	XdmcpWriteCARD32 (&manager->priv->buf, send_sessid);

	XdmcpFlush (manager->priv->socket_fd,
		    &manager->priv->buf,
		    (XdmcpNetaddr)clnt_sa,
		    (int)mdm_sockaddr_len(clnt_sa));
}

static void
mdm_xdmcp_handle_keepalive (MdmXdmcpManager         *manager,
			    struct sockaddr_storage *clnt_sa,
			    int                      len)
{
	CARD16 clnt_dspnum;
	CARD32 clnt_sessid;
	char *host;

	mdm_address_get_info (clnt_sa, &host, NULL);
	mdm_debug ("XDMCP: Got KEEPALIVE from %s", host);

	/* Check with tcp_wrappers if client is allowed to access */
	if (! mdm_xdmcp_host_allow (clnt_sa)) {
		mdm_debug ("%s: Got KEEPALIVE from banned host %s",
			   "mdm_xdmcp_handle_keepalive",
			   host);
		g_free (host);
		return;
	}
	g_free (host);

	/* Remote display number */
	if G_UNLIKELY (! XdmcpReadCARD16 (&manager->priv->buf, &clnt_dspnum)) {
		mdm_debug ("%s: Could not read Display Number",
			   "mdm_xdmcp_handle_keepalive");
		return;
	}

	/* SessionID */
	if G_UNLIKELY (! XdmcpReadCARD32 (&manager->priv->buf, &clnt_sessid)) {
		mdm_debug ("%s: Could not read Session ID",
			   "mdm_xdmcp_handle_keepalive");
		return;
	}

	mdm_xdmcp_send_alive (manager, clnt_sa, clnt_dspnum, clnt_sessid);
}

static const char *
opcode_string (int opcode)
{
	static const char * const opcode_names[] = {
		NULL,
		"BROADCAST_QUERY",
		"QUERY",
		"INDIRECT_QUERY",
		"FORWARD_QUERY",
		"WILLING",
		"UNWILLING",
		"REQUEST",
		"ACCEPT",
		"DECLINE",
		"MANAGE",
		"REFUSE",
		"FAILED",
		"KEEPALIVE",
		"ALIVE"
	};
	static const char * const mdm_opcode_names[] = {
		"MANAGED_FORWARD",
		"GOT_MANAGED_FORWARD"
	};


	if (opcode < G_N_ELEMENTS (opcode_names)) {
		return opcode_names [opcode];
	} else if (opcode >= MDM_XDMCP_FIRST_OPCODE &&
		   opcode < MDM_XDMCP_LAST_OPCODE) {
		return mdm_opcode_names [opcode - MDM_XDMCP_FIRST_OPCODE];
	} else {
		return "UNKNOWN";
	}
}

static gboolean
decode_packet (GIOChannel      *source,
	       GIOCondition     cond,
	       MdmXdmcpManager *manager)
{
	struct sockaddr_storage clnt_sa;
	gint                    sa_len;
	XdmcpHeader             header;
	char                   *host;
	char                   *port;
	int                     res;

	sa_len = sizeof (clnt_sa);

	mdm_debug ("decode_packet: GIOCondition %d", (int)cond);

	if ( ! (cond & G_IO_IN)) {
		return TRUE;
	}

	res = XdmcpFill (manager->priv->socket_fd, &manager->priv->buf, (XdmcpNetaddr)&clnt_sa, &sa_len);
	if G_UNLIKELY (! res) {
		mdm_debug (_("XDMCP: Could not create XDMCP buffer!"));
		return TRUE;
	}

	res = XdmcpReadHeader (&manager->priv->buf, &header);
	if G_UNLIKELY (! res) {
		mdm_debug ("XDMCP: Could not read XDMCP header!");
		return TRUE;
	}

	if G_UNLIKELY (header.version != XDM_PROTOCOL_VERSION &&
		       header.version != MDM_XDMCP_PROTOCOL_VERSION) {
		mdm_debug ("XDMCP: Incorrect XDMCP version!");
		return TRUE;
	}

	mdm_address_get_info (&clnt_sa, &host, &port);

	mdm_debug ("XDMCP: Received opcode %s from client %s : %s",
		 opcode_string (header.opcode),
		 host,
		 port);

	switch (header.opcode) {
	case BROADCAST_QUERY:
		mdm_xdmcp_handle_broadcast_query (manager, &clnt_sa, header.length);
		break;

	case QUERY:
		mdm_xdmcp_handle_query (manager, &clnt_sa, header.length);
		break;

	case INDIRECT_QUERY:
		mdm_xdmcp_handle_indirect_query (manager, &clnt_sa, header.length);
		break;

	case FORWARD_QUERY:
		mdm_xdmcp_handle_forward_query (manager, &clnt_sa, header.length);
		break;

	case REQUEST:
		mdm_xdmcp_handle_request (manager, &clnt_sa, header.length);
		break;

	case MANAGE:
		mdm_xdmcp_handle_manage (manager, &clnt_sa, header.length);
		break;

	case KEEPALIVE:
		mdm_xdmcp_handle_keepalive (manager, &clnt_sa, header.length);
		break;

	case MDM_XDMCP_MANAGED_FORWARD:
		mdm_xdmcp_handle_managed_forward (manager, &clnt_sa, header.length);
		break;

	case MDM_XDMCP_GOT_MANAGED_FORWARD:
		mdm_xdmcp_handle_got_managed_forward (manager, &clnt_sa, header.length);
		break;

	default:
		mdm_debug ("XDMCP: Unknown opcode from client %s : %s",
			   host,
			   port);

		break;
	}

	g_free (host);
	g_free (port);

	return TRUE;
}

gboolean
mdm_xdmcp_manager_start (MdmXdmcpManager *manager,
			 GError         **error)
{
	gboolean    ret;
	GIOChannel *ioc;

	g_return_val_if_fail (MDM_IS_XDMCP_MANAGER (manager), FALSE);
	g_return_val_if_fail (manager->priv->socket_fd == -1, FALSE);

        if (manager->priv->multicast_address)
		g_free (manager->priv->multicast_address);
        if (manager->priv->willing_script)
		g_free (manager->priv->willing_script);

	/* read configuration */
	manager->priv->port = mdm_daemon_config_get_value_int (MDM_KEY_UDP_PORT);
	manager->priv->use_multicast = mdm_daemon_config_get_value_bool (MDM_KEY_MULTICAST);
	manager->priv->multicast_address = g_strdup(mdm_daemon_config_get_value_string (MDM_KEY_MULTICAST_ADDR));
	manager->priv->honor_indirect = mdm_daemon_config_get_value_bool (MDM_KEY_INDIRECT);
	manager->priv->max_displays_per_host = mdm_daemon_config_get_value_int (MDM_KEY_DISPLAYS_PER_HOST);
	manager->priv->max_displays = mdm_daemon_config_get_value_int (MDM_KEY_MAX_SESSIONS);
	manager->priv->max_pending_displays = mdm_daemon_config_get_value_int (MDM_KEY_MAX_PENDING);
	manager->priv->max_wait = mdm_daemon_config_get_value_int (MDM_KEY_MAX_WAIT);
	manager->priv->willing_script = g_strdup (mdm_daemon_config_get_value_string (MDM_KEY_WILLING));

	ret = open_port (manager);
	if (! ret) {
		return ret;
	}

	mdm_debug ("XDMCP: Starting to listen on XDMCP port");

	ioc = g_io_channel_unix_new (manager->priv->socket_fd);

	g_io_channel_set_encoding (ioc, NULL, NULL);
	g_io_channel_set_buffered (ioc, FALSE);

	manager->priv->socket_watch_id = g_io_add_watch_full (ioc,
							      G_PRIORITY_DEFAULT,
							      G_IO_IN | G_IO_PRI | G_IO_ERR | G_IO_HUP | G_IO_NVAL,
							      (GIOFunc)decode_packet,
							      manager,
							      NULL);
	g_io_channel_unref (ioc);

	return ret;
}

void
mdm_xdmcp_manager_set_port (MdmXdmcpManager *manager,
			    guint            port)
{
	g_return_if_fail (MDM_IS_XDMCP_MANAGER (manager));

	manager->priv->port = port;
}

static void
mdm_xdmcp_manager_set_use_multicast (MdmXdmcpManager *manager,
				     gboolean         use_multicast)
{
	g_return_if_fail (MDM_IS_XDMCP_MANAGER (manager));

	manager->priv->use_multicast = use_multicast;
}

static void
mdm_xdmcp_manager_set_multicast_address (MdmXdmcpManager *manager,
					 const char      *address)
{
	g_return_if_fail (MDM_IS_XDMCP_MANAGER (manager));

	g_free (manager->priv->multicast_address);
	manager->priv->multicast_address = g_strdup (address);
}

static void
mdm_xdmcp_manager_set_honor_indirect (MdmXdmcpManager *manager,
				      gboolean         honor_indirect)
{
	g_return_if_fail (MDM_IS_XDMCP_MANAGER (manager));

	manager->priv->honor_indirect = honor_indirect;
}

static void
mdm_xdmcp_manager_set_max_displays_per_host (MdmXdmcpManager *manager,
					     guint            num)
{
	g_return_if_fail (MDM_IS_XDMCP_MANAGER (manager));

	manager->priv->max_displays_per_host = num;
}

static void
mdm_xdmcp_manager_set_max_displays (MdmXdmcpManager *manager,
				    guint            num)
{
	g_return_if_fail (MDM_IS_XDMCP_MANAGER (manager));

	manager->priv->max_displays = num;
}

static void
mdm_xdmcp_manager_set_max_pending_displays (MdmXdmcpManager *manager,
					    guint            num)
{
	g_return_if_fail (MDM_IS_XDMCP_MANAGER (manager));

	manager->priv->max_pending_displays = num;
}

static void
mdm_xdmcp_manager_set_max_wait (MdmXdmcpManager *manager,
				guint            num)
{
	g_return_if_fail (MDM_IS_XDMCP_MANAGER (manager));

	manager->priv->max_wait = num;
}

static void
mdm_xdmcp_manager_set_willing_script (MdmXdmcpManager *manager,
				      const char      *script)
{
	g_return_if_fail (MDM_IS_XDMCP_MANAGER (manager));

	g_free (manager->priv->willing_script);
	manager->priv->willing_script = g_strdup (script);
}

static void
mdm_xdmcp_manager_set_property (GObject	      *object,
				guint	       prop_id,
				const GValue  *value,
				GParamSpec    *pspec)
{
	MdmXdmcpManager *self;

	self = MDM_XDMCP_MANAGER (object);

	switch (prop_id) {
	case PROP_PORT:
		mdm_xdmcp_manager_set_port (self, g_value_get_uint (value));
		break;
	case PROP_USE_MULTICAST:
		mdm_xdmcp_manager_set_use_multicast (self, g_value_get_boolean (value));
		break;
	case PROP_MULTICAST_ADDRESS:
		mdm_xdmcp_manager_set_multicast_address (self, g_value_get_string (value));
		break;
	case PROP_HONOR_INDIRECT:
		mdm_xdmcp_manager_set_honor_indirect (self, g_value_get_boolean (value));
		break;
	case PROP_MAX_DISPLAYS_PER_HOST:
		mdm_xdmcp_manager_set_max_displays_per_host (self, g_value_get_uint (value));
		break;
	case PROP_MAX_DISPLAYS:
		mdm_xdmcp_manager_set_max_displays (self, g_value_get_uint (value));
		break;
	case PROP_MAX_PENDING_DISPLAYS:
		mdm_xdmcp_manager_set_max_pending_displays (self, g_value_get_uint (value));
		break;
	case PROP_MAX_WAIT:
		mdm_xdmcp_manager_set_max_wait (self, g_value_get_uint (value));
		break;
	case PROP_WILLING_SCRIPT:
		mdm_xdmcp_manager_set_willing_script (self, g_value_get_string (value));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
mdm_xdmcp_manager_get_property (GObject	   *object,
				guint  	    prop_id,
				GValue	   *value,
				GParamSpec *pspec)
{
	MdmXdmcpManager *self;

	self = MDM_XDMCP_MANAGER (object);

	switch (prop_id) {
	case PROP_PORT:
		g_value_set_uint (value, self->priv->port);
		break;
	case PROP_USE_MULTICAST:
		g_value_set_boolean (value, self->priv->use_multicast);
		break;
	case PROP_MULTICAST_ADDRESS:
		g_value_set_string (value, self->priv->multicast_address);
		break;
	case PROP_HONOR_INDIRECT:
		g_value_set_boolean (value, self->priv->honor_indirect);
		break;
	case PROP_MAX_DISPLAYS_PER_HOST:
		g_value_set_uint (value, self->priv->max_displays_per_host);
		break;
	case PROP_MAX_DISPLAYS:
		g_value_set_uint (value, self->priv->max_displays);
		break;
	case PROP_MAX_PENDING_DISPLAYS:
		g_value_set_uint (value, self->priv->max_pending_displays);
		break;
	case PROP_MAX_WAIT:
		g_value_set_uint (value, self->priv->max_wait);
		break;
	case PROP_WILLING_SCRIPT:
		g_value_set_string (value, self->priv->willing_script);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
mdm_xdmcp_manager_class_init (MdmXdmcpManagerClass *klass)
{
	GObjectClass   *object_class = G_OBJECT_CLASS (klass);

	object_class->get_property = mdm_xdmcp_manager_get_property;
	object_class->set_property = mdm_xdmcp_manager_set_property;
	object_class->finalize = mdm_xdmcp_manager_finalize;

        g_object_class_install_property (object_class,
                                         PROP_PORT,
                                         g_param_spec_uint ("port",
                                                            "UDP port",
                                                            "UDP port",
                                                            0,
                                                            G_MAXINT,
                                                            DEFAULT_PORT,
                                                            G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
        g_object_class_install_property (object_class,
                                         PROP_USE_MULTICAST,
                                         g_param_spec_boolean ("use-multicast",
                                                               NULL,
                                                               NULL,
                                                               DEFAULT_USE_MULTICAST,
                                                               G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
        g_object_class_install_property (object_class,
                                         PROP_MULTICAST_ADDRESS,
                                         g_param_spec_string ("multicast-address",
                                                              "multicast-address",
                                                              "multicast-address",
                                                              DEFAULT_MULTICAST_ADDRESS,
                                                              G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
        g_object_class_install_property (object_class,
                                         PROP_HONOR_INDIRECT,
                                         g_param_spec_boolean ("honor-indirect",
                                                               NULL,
                                                               NULL,
                                                               DEFAULT_HONOR_INDIRECT,
                                                               G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
        g_object_class_install_property (object_class,
                                         PROP_WILLING_SCRIPT,
                                         g_param_spec_string ("willing-script",
                                                              "willing-script",
                                                              "willing-script",
							      NULL,
                                                              G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
        g_object_class_install_property (object_class,
                                         PROP_MAX_DISPLAYS_PER_HOST,
                                         g_param_spec_uint ("max-displays-per-host",
                                                            "max-displays-per-host",
                                                            "max-displays-per-host",
                                                            0,
                                                            G_MAXINT,
                                                            DEFAULT_MAX_DISPLAYS_PER_HOST,
                                                            G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
        g_object_class_install_property (object_class,
                                         PROP_MAX_DISPLAYS,
                                         g_param_spec_uint ("max-displays",
                                                            "max-displays",
                                                            "max-displays",
                                                            0,
                                                            G_MAXINT,
                                                            DEFAULT_MAX_DISPLAYS,
                                                            G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
        g_object_class_install_property (object_class,
                                         PROP_MAX_PENDING_DISPLAYS,
                                         g_param_spec_uint ("max-pending-displays",
                                                            "max-pending-displays",
                                                            "max-pending-displays",
                                                            0,
                                                            G_MAXINT,
                                                            DEFAULT_MAX_PENDING_DISPLAYS,
                                                            G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
        g_object_class_install_property (object_class,
                                         PROP_MAX_WAIT,
                                         g_param_spec_uint ("max-wait",
                                                            "max-wait",
                                                            "max-wait",
                                                            0,
                                                            G_MAXINT,
                                                            DEFAULT_MAX_WAIT,
                                                            G_PARAM_READWRITE | G_PARAM_CONSTRUCT));

	g_type_class_add_private (klass, sizeof (MdmXdmcpManagerPrivate));
}

static void
mdm_xdmcp_manager_init (MdmXdmcpManager *manager)
{
	char           hostbuf[1024];
	struct utsname name;

	manager->priv = MDM_XDMCP_MANAGER_GET_PRIVATE (manager);

	manager->priv->socket_fd = -1;

	manager->priv->session_serial = g_random_int ();

	/* Fetch and store local hostname in XDMCP friendly format */
	hostbuf[1023] = '\0';
	if G_UNLIKELY (gethostname (hostbuf, 1023) != 0) {
		mdm_debug ("Could not get server hostname: %s!", g_strerror (errno));
		strcpy (hostbuf, "localhost.localdomain");
	}

	uname (&name);
	manager->priv->sysid = g_strconcat (name.sysname,
					    " ",
					    name.release,
					    NULL);

	manager->priv->hostname = g_strdup (hostbuf);

	manager->priv->servhost.data   = (CARD8 *) g_strdup (hostbuf);
	manager->priv->servhost.length = strlen ((char *) manager->priv->servhost.data);
}

static void
mdm_xdmcp_manager_finalize (GObject *object)
{
	MdmXdmcpManager *manager;

	g_return_if_fail (object != NULL);
	g_return_if_fail (MDM_IS_XDMCP_MANAGER (object));

	manager = MDM_XDMCP_MANAGER (object);

	g_return_if_fail (manager->priv != NULL);

	if (manager->priv->socket_watch_id > 0) {
		g_source_remove (manager->priv->socket_watch_id);
	}

	if (manager->priv->socket_fd > 0) {
		VE_IGNORE_EINTR (close (manager->priv->socket_fd));
		manager->priv->socket_fd = -1;
	}

	g_slist_free (manager->priv->forward_queries);
	g_slist_free (manager->priv->managed_forwards);

	g_free (manager->priv->sysid);
	g_free (manager->priv->hostname);
	g_free (manager->priv->multicast_address);
	g_free (manager->priv->willing_script);

	/* FIXME: Free servhost */

	G_OBJECT_CLASS (mdm_xdmcp_manager_parent_class)->finalize (object);
}

MdmXdmcpManager *
mdm_xdmcp_manager_new (void)
{
	if (xdmcp_manager_object != NULL) {
		g_object_ref (xdmcp_manager_object);
	} else {
		xdmcp_manager_object = g_object_new (MDM_TYPE_XDMCP_MANAGER, NULL);
		g_object_add_weak_pointer (xdmcp_manager_object,
					   (gpointer *) &xdmcp_manager_object);
	}

	return MDM_XDMCP_MANAGER (xdmcp_manager_object);
}
