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

#ifndef MDM_FILECHECK_H
#define MDM_FILECHECK_H

gboolean mdm_file_check (const gchar *caller, uid_t user, const gchar *dir,
			 const gchar *file, gboolean absentok,
			 gboolean absentdirok, gint maxsize,
			 gint perms);

/* more paranoid on the file itself, doesn't check directory (for all we know
   it could be /tmp) */
gboolean mdm_auth_file_check (const gchar *caller, uid_t user, const gchar *authfile, gboolean absentok, struct stat *s);

#endif /* MDM_FILECHECK_H */

/* EOF */
