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

#ifndef MDM_ERRORGUI_H
#define MDM_ERRORGUI_H

#include "mdm.h"
#include <gtk/gtkmessagedialog.h>

void		mdm_errorgui_error_box_full	(MdmDisplay *d,
                                                 GtkMessageType type,
                                                 const char *error,
                                                 const char *details_label,
                                                 const char *details_file,
                                                 /* zero doesn't mean root,
                                                    we never wish to run as root,
                                                    zero means use the mdm user */
                                                 uid_t uid,
                                                 gid_t gid);

void		mdm_errorgui_error_box		(MdmDisplay *d,
                                                 GtkMessageType type,
                                                 const char *error);

char *		mdm_errorgui_failsafe_question	(MdmDisplay *d,
                                                 const char *question,
                                                 gboolean echo);

gboolean	mdm_errorgui_failsafe_yesno	(MdmDisplay *d,
                                                 const char *question);
int		mdm_errorgui_failsafe_ask_buttons (MdmDisplay *d,
                                                   const char *question,
                                                   char **but);

#endif /* MDM_ERRORGUI_H */

/* EOF */

