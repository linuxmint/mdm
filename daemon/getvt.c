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

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include "mdm.h"
#include "misc.h"
#include "getvt.h"
#include "display.h"

#include "mdm-common.h"
#include "mdm-daemon-config.h"
#include "mdm-log.h"

/*
 * Get the VT number associated with the display via the XFree86_VT
 * Atom.
 */
long
mdm_get_current_vtnum (Display *display)
{
	Atom prop;
	Atom actualtype;
	int actualformat;
	unsigned long nitems;
	unsigned long bytes_after;
	unsigned char *buf;
	unsigned long num;

	if (display == NULL)
		return -1;

	prop = XInternAtom (display, "XFree86_VT", True);
	if (prop == None) {
	        mdm_debug ("no XFree86_VT atom\n");
	        return -1;
	}
	if (XGetWindowProperty (display, DefaultRootWindow (display), prop, 0, 1,
		False, AnyPropertyType, &actualtype, &actualformat,
		&nitems, &bytes_after, &buf)) {
		mdm_debug ("no XFree86_VT property\n");
		return -1;
	}
	if (nitems != 1) {
		mdm_debug ("%lu items in XFree86_VT property!\n", nitems);
		XFree (buf);
		return -1;
	}

	switch (actualtype) {
	case XA_CARDINAL:
	case XA_INTEGER:
	case XA_WINDOW:
		switch (actualformat) {
		case  8:
			num = (*(uint8_t  *)(void *)buf);
			break;
		case 16:
			num = (*(uint16_t *)(void *)buf);
			break;
		case 32:
			num = (*(uint32_t *)(void *)buf);
			break;
		default:
			mdm_debug ("format %d in XFree86_VT property!\n", actualformat);
			XFree (buf);
			return -1;
		}
		break;
	default:
		mdm_debug ("type %lx in XFree86_VT property!\n", actualtype);
		XFree (buf);
		return -1;
	}
	XFree (buf);
	return num;
}

#if defined (MDM_USE_SYS_VT)
#include <sys/vt.h>
#elif defined (MDM_USE_CONSIO_VT)
#include <sys/consio.h>

static const char*
__itovty (int val)
{
	static char str[8];
	char* next = str + sizeof (str) - 1;

	*next = '\0';
	do {
		*--next = "0123456789abcdefghigklmnopqrstuv"[val % 32];
	} while (val /= 32);

	return next;
}
#endif


gchar *
mdm_get_vt_device (int vtno)
{
   gchar *vtname = NULL;

#if defined (MDM_USE_SYS_VT)
     vtname = g_strdup_printf ("/dev/tty%d", vtno);
#elif defined (MDM_USE_CONSIO_VT)
     vtname = g_strdup_printf ("/dev/ttyv%s", __itovty (vtno - 1));
#endif

   return vtname;
}

#if defined (MDM_USE_SYS_VT) || defined (MDM_USE_CONSIO_VT)

#define MDMCONSOLEDEVICE "/dev/tty0"

static int
open_vt (int vtno)
{
	char *vtname = NULL;
	int fd = -1;

	vtname = mdm_get_vt_device (vtno);

	do {
		errno = 0;
		fd = open (vtname, O_RDWR
#ifdef O_NOCTTY
			   |O_NOCTTY
#endif
			   , 0);
	} while G_UNLIKELY (errno == EINTR);

	g_free (vtname);

	return fd;
}

#if defined (MDM_USE_SYS_VT)

static int 
get_free_vt_sys (int *vtfd)
{
	int fd, fdv;
	int vtno;
	unsigned short vtmask;
	struct vt_stat vtstat;

	*vtfd = -1;

	do {
		errno = 0;
		fd = open (MDMCONSOLEDEVICE,
			   O_WRONLY
#ifdef O_NOCTTY
			   |O_NOCTTY
#endif
			   , 0);
	} while G_UNLIKELY (errno == EINTR);
	if (fd < 0)
		return -1;

	if (ioctl (fd, VT_GETSTATE, &vtstat) < 0) {
		VE_IGNORE_EINTR (close (fd));
		return -1;
	}

	for (vtno = mdm_daemon_config_get_value_int (MDM_KEY_FIRST_VT), vtmask = 1 << vtno;
			vtstat.v_state & vtmask; vtno++, vtmask <<= 1);
	if (!vtmask) {
		VE_IGNORE_EINTR (close (fd));
		return -1;
	}

	fdv = open_vt (vtno);
	if (fdv < 0) {
		VE_IGNORE_EINTR (close (fd));
		return -1;
	}
	*vtfd = fdv;
	return vtno;
}

#elif defined (MDM_USE_CONSIO_VT)

static int
get_free_vt_consio (int *vtfd)
{
	int fd, fdv;
	int vtno;
	GList *to_close_vts = NULL, *li;

	*vtfd = -1;

	do {
		errno = 0;
		fd = open (MDMCONSOLEDEVICE,
			   O_WRONLY
#ifdef O_NOCTTY
			   |O_NOCTTY
#endif
			   , 0);
	} while G_UNLIKELY (errno == EINTR);
	if (fd < 0)
		return -1;

	if ((ioctl (fd, VT_OPENQRY, &vtno) < 0) || (vtno == -1)) {
		VE_IGNORE_EINTR (close (fd));
		return -1;
	}

	fdv = open_vt (vtno);
	if (fdv < 0) {
		VE_IGNORE_EINTR (close (fd));
		return -1;
	}

	while (vtno < mdm_daemon_config_get_value_int (MDM_KEY_FIRST_VT)) {
		int oldvt = vtno;
		to_close_vts = g_list_prepend (to_close_vts,
					       GINT_TO_POINTER (fdv));

		if (ioctl (fd, VT_OPENQRY, &vtno) == -1) {
			vtno = -1;
			goto cleanup;
		}

		if (oldvt == vtno) {
			vtno = -1;
			goto cleanup;
		}

		fdv = open_vt (vtno);
		if (fdv < 0) {
			vtno = -1;
			goto cleanup;
		}
	}

	*vtfd = fdv;

cleanup:
	for (li = to_close_vts; li != NULL; li = li->next) {
		VE_IGNORE_EINTR (close (GPOINTER_TO_INT (li->data)));
	}
	return vtno;
}

#endif

char *
mdm_get_empty_vt_argument (int *fd, int *vt)
{
	if ( ! mdm_daemon_config_get_value_bool (MDM_KEY_VT_ALLOCATION)) {
		*fd = -1;
		return NULL;
	}

#if defined (MDM_USE_SYS_VT)
	*vt = get_free_vt_sys (fd);
#elif defined (MDM_USE_CONSIO_VT)
	*vt = get_free_vt_consio (fd);
#endif

	if (*vt < 0)
		return NULL;
	else
		return g_strdup_printf ("vt%d", *vt);
}

/* change to an existing vt */
void
mdm_change_vt (int vt)
{
	int fd;
	int rc;
	if (vt < 0)
		return;

	do {
		errno = 0;
		fd = open (MDMCONSOLEDEVICE,
			   O_WRONLY
#ifdef O_NOCTTY
			   |O_NOCTTY
#endif
			   , 0);
	} while G_UNLIKELY (errno == EINTR);
	if (fd < 0)
		return;

	rc = ioctl (fd, VT_ACTIVATE, vt);
	rc = ioctl (fd, VT_WAITACTIVE, vt);

	VE_IGNORE_EINTR (close (fd));
}

int
mdm_get_current_vt (void)
{
#if defined (MDM_USE_SYS_VT)
	struct vt_stat s;
#elif defined (MDM_USE_CONSIO_VT)
	int vtno;
#endif
	int fd;

	do {
		errno = 0;
		fd = open (MDMCONSOLEDEVICE,
			   O_WRONLY
#ifdef O_NOCTTY
			   |O_NOCTTY
#endif
			   , 0);
	} while G_UNLIKELY (errno == EINTR);
	if (fd < 0)
		return -1;
#if defined (MDM_USE_SYS_VT)
	ioctl (fd, VT_GETSTATE, &s);

	VE_IGNORE_EINTR (close (fd));

	/* debug */
	/*
	printf ("current_Active %d\n", (int)s.v_active);
	*/

	return s.v_active;
#elif defined (MDM_USE_CONSIO_VT)
	if (ioctl (fd, VT_GETACTIVE, &vtno) == -1) {
		VE_IGNORE_EINTR (close (fd));
		return -1;
	}

VE_IGNORE_EINTR (close (fd));

	/* debug */
	/*
	printf ("current_Active %d\n", vtno);
	*/

	return vtno;
#endif
}

#else /* MDM_USE_SYS_VT || MDM_USE_CONSIO_VT - Here this is just
       * a stub, we do not know how to support this on other
       * platforms
       */

char *
mdm_get_empty_vt_argument (int *fd, int *vt)
{
	*fd = -1;
	*vt = -1;
	return NULL;
}

void
mdm_change_vt (int vt)
{
	return;
}

int
mdm_get_current_vt (void)
{
	return -1;
}

#endif
