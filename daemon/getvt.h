/* MDM - The MDM Display Manager
 * Copyright (C) 2002 Queen of England
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

#ifndef GETVT_H
#define GETVT_H

/* Virtual terminals only supported on Linux, FreeBSD, DragonFly, or Solaris */
#if defined (__linux__)
/* Must check HAVE_SYS_VT since older Solaris doesn't support this. */
#ifdef HAVE_SYS_VT_H
#define MDM_USE_SYS_VT
#endif
#endif

#if defined (__FreeBSD__) || defined (__DragonFly__)
#define MDM_USE_CONSIO_VT
#endif

/* gets an argument we should pass to the X server, on
 * linux for example we get the first empty vt (higher than
 * or equal to MDM_KEY_FIRST_VT) and then return vt<number>
 * (e.g. "vt7") as a newly allocated string.
 * Can return NULL if we can't figure out what to do
 * or if MDM_KEY_VT_ALLOCATION is false. */
/* fd is opened so that we are saying we have opened this
 * vt.  This should be closed after the server has started.
 * This is to avoid race with other stuff openning this vt.
 * It can be set to -1 if nothing could be opened. */
char *	mdm_get_empty_vt_argument	(int *fd,
					 int *vt);

/* Change to the specified virtual terminal */
void	mdm_change_vt			(int vt);

/* Get the current virtual terminal number or -1 if we can't */
int	mdm_get_current_vt		(void);
long	mdm_get_current_vtnum		(Display *display);
gchar * mdm_get_vt_device		(int vtno);

#endif /* GETVT_H */
