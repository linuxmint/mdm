/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2006 William Jon McCann <mccann@jhu.edu>
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


#ifndef __MDM_XDMCP_MANAGER_H
#define __MDM_XDMCP_MANAGER_H

#include <glib-object.h>

G_BEGIN_DECLS

#define MDM_TYPE_XDMCP_MANAGER         (mdm_xdmcp_manager_get_type ())
#define MDM_XDMCP_MANAGER(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), MDM_TYPE_XDMCP_MANAGER, MdmXdmcpManager))
#define MDM_XDMCP_MANAGER_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), MDM_TYPE_XDMCP_MANAGER, MdmXdmcpManagerClass))
#define MDM_IS_XDMCP_MANAGER(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), MDM_TYPE_XDMCP_MANAGER))
#define MDM_IS_XDMCP_MANAGER_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), MDM_TYPE_XDMCP_MANAGER))
#define MDM_XDMCP_MANAGER_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), MDM_TYPE_XDMCP_MANAGER, MdmXdmcpManagerClass))

typedef struct MdmXdmcpManagerPrivate MdmXdmcpManagerPrivate;

typedef struct
{
	GObject		        parent;
	MdmXdmcpManagerPrivate *priv;
} MdmXdmcpManager;

typedef struct
{
	GObjectClass   parent_class;
} MdmXdmcpManagerClass;

typedef enum
{
	 MDM_XDMCP_MANAGER_ERROR_GENERAL
} MdmXdmcpManagerError;

#define MDM_XDMCP_MANAGER_ERROR mdm_xdmcp_manager_error_quark ()

GQuark		    mdm_xdmcp_manager_error_quark	      (void);
GType		    mdm_xdmcp_manager_get_type		      (void);

MdmXdmcpManager *   mdm_xdmcp_manager_new		      (void);

void                mdm_xdmcp_manager_set_port                (MdmXdmcpManager *manager,
							       guint            port);
gboolean            mdm_xdmcp_manager_start                   (MdmXdmcpManager *manager,
							       GError         **error);

G_END_DECLS

#endif /* __MDM_XDMCP_MANAGER_H */
