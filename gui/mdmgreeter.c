/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * MDM - The MDM Display Manager
 * Copyright (C) 1999, 2000 Martin K. Petersen <mkp@mkp.net>
 *
 * This file Copyright (c) 2003 George Lebl
 * - Common routines for the greeters.
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

#include <string.h>

#include "mdmgreeter.h"

#include <glib/gi18n.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>

#include "mdm.h"
#include "mdm-common.h"
#include "mdmconfig.h"
#include "mdm-daemon-config-keys.h"

/*
 * This file is for functions that are only used by mdmlogin,
 * mdmgreeter, and mdmsetup
 */

gboolean
mdm_common_is_action_available (gchar *action)
{
	gchar **allowsyscmd = NULL;
	const gchar *allowsyscmdval;
	gboolean ret = FALSE;
	int i;

	allowsyscmdval = mdm_config_get_string (MDM_KEY_SYSTEM_COMMANDS_IN_MENU);
	if (allowsyscmdval)
		allowsyscmd = g_strsplit (allowsyscmdval, ";", 0);

	if (allowsyscmd) {
		for (i = 0; allowsyscmd[i] != NULL; i++) {
			if (strcmp (allowsyscmd[i], action) == 0) {
				ret = TRUE;
				break;
			}
		}
	}

	g_strfreev (allowsyscmd);

	return ret;
}

