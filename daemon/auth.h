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

#ifndef MDM_AUTH_H
#define MDM_AUTH_H

#include "mdm.h"

gboolean mdm_auth_secure_display (MdmDisplay *d);
gboolean mdm_auth_user_add       (MdmDisplay *d, uid_t user, const char *homedir);
void     mdm_auth_user_remove    (MdmDisplay *d, uid_t user);

/* Call XSetAuthorization */
void	 mdm_auth_set_local_auth (MdmDisplay *d);

void     mdm_auth_free_auth_list (GSList *list);

#endif /* MDM_AUTH_H */

/* EOF */
