/*
 *  MDM - THe MDM Display Manager
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

#ifndef _MDMCONFIG_H
#define _MDMCONFIG_H

#include "glib.h"

void		mdm_config_never_cache			(gboolean never_cache);
void		mdm_config_set_comm_retries		(int tries);
gchar *		mdm_config_get_string			(const gchar *key);
gchar *		mdm_config_get_translated_string	(const gchar *key);
gint		mdm_config_get_int     			(const gchar *key);
gboolean	mdm_config_get_bool			(const gchar *key);
gboolean	mdm_config_reload_string		(const gchar *key);
gboolean	mdm_config_reload_int			(const gchar *key);
gboolean	mdm_config_reload_bool			(const gchar *key);
GSList *	mdm_config_get_xservers			(gboolean flexible);

void		mdm_save_customlist_data		(const gchar *file,
							 const gchar *key,
							 const gchar *id);
char *		mdm_get_theme_greeter			(const gchar *file,
							 const char *fallback);

#endif /* _MDMCONFIG_H */
