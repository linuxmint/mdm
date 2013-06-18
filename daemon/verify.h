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

#ifndef MDM_VERIFY_H
#define MDM_VERIFY_H

#include "mdm.h"
#include "display.h"

/* If username is NULL, we ask, if local is FALSE, don't start
 * the timed login timer */
gchar *mdm_verify_user			 (MdmDisplay *d,
					  const char *username,
					  gboolean allow_retry);
void   mdm_verify_cleanup		 (MdmDisplay *d);
void   mdm_verify_select_user		 (const char *user);

/* used in pam */
gboolean mdm_verify_setup_env  (MdmDisplay *d);
gboolean mdm_verify_setup_user (MdmDisplay *d,
				const gchar *login,
				char **new_login);

#endif /* MDM_VERIFY_H */

/* EOF */
