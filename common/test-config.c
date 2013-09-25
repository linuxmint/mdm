/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2007 William Jon McCann <mccann@jhu.edu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 */

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <pwd.h>
#include <string.h>
#include <errno.h>

#include <locale.h>

#include <glib.h>

#include "mdm-common.h"

#include "../daemon/mdm-daemon-config-entries.h"

static const char *
source_to_name (MdmConfigSourceType source)
{
        const char *name;

        switch (source) {
        case MDM_CONFIG_SOURCE_DEFAULT:
                name = "default";
                break;
        case MDM_CONFIG_SOURCE_DISTRO:
                name = "distro";
                break;
        case MDM_CONFIG_SOURCE_CUSTOM:
                name = "custom";
                break;
        case MDM_CONFIG_SOURCE_BUILT_IN:
                name = "built-in";
                break;
        case MDM_CONFIG_SOURCE_RUNTIME_USER:
                name = "runtime-user";
                break;
        case MDM_CONFIG_SOURCE_INVALID:
                name = "Invalid";
                break;
        default:
                name = "Unknown";
                break;
        }

        return name;
}

static const char *
type_to_name (MdmConfigValueType type)
{
        const char *name;

        switch (type) {
        case MDM_CONFIG_VALUE_INT:
                name = "int";
                break;
        case MDM_CONFIG_VALUE_BOOL:
                name = "boolean";
                break;
        case MDM_CONFIG_VALUE_STRING:
                name = "string";
                break;
        case MDM_CONFIG_VALUE_LOCALE_STRING:
                name = "locale-string";
                break;
        case MDM_CONFIG_VALUE_STRING_ARRAY:
                name = "string-array";
                break;
        case MDM_CONFIG_VALUE_LOCALE_STRING_ARRAY:
                name = "locale-string-array";
                break;
        case MDM_CONFIG_VALUE_INVALID:
                name = "invalid";
                break;
        default:
                name = "unknown";
                break;
        }

        return name;
}

static gboolean
notify_cb (MdmConfig          *config,
           MdmConfigSourceType source,
           const char         *group,
           const char         *key,
           MdmConfigValue     *value,
           int                 id,
           gpointer            data)
{
        char *str;

        if (value == NULL) {
                return FALSE;
        }

        str = mdm_config_value_to_string (value);

        g_print ("SOURCE=%s GROUP=%s KEY=%s ID=%d TYPE=%s VALUE=%s\n", source_to_name (source), group, key, id, type_to_name (value->type), str);     

        g_free (str);
        return TRUE;
}

static gboolean
validate_cb (MdmConfig          *config,
             MdmConfigSourceType source,
             const char         *group,
             const char         *key,
             MdmConfigValue     *value,
             int                 id,
             gpointer            data)
{
        /* Here you can do validation or override the values */

        switch (id) {
        case MDM_ID_SOUND_PROGRAM:
                mdm_config_value_set_string (value, "NONE");
                break;
        case MDM_ID_NONE:
        default:
                /* doesn't have an ID : match group/key */
                break;
        }

        return TRUE;
}

static void
load_servers_group (MdmConfig *config)
{
        gchar    **keys;
        gsize      len;
        int        i;

        keys = mdm_config_get_keys_for_group (config, MDM_CONFIG_GROUP_SERVERS, &len, NULL);
        g_message ("Got %d keys for group %s", (int)len, MDM_CONFIG_GROUP_SERVERS);

        /* now construct entries for these groups */
        for (i = 0; i < len; i++) {
                MdmConfigEntry  entry;
                MdmConfigValue *value;
                char           *new_group;
                gboolean        res;
                int             j;

                entry.group = MDM_CONFIG_GROUP_SERVERS;
                entry.key = keys[i];
                entry.type = MDM_CONFIG_VALUE_STRING;
                entry.default_value = NULL;
                entry.id = MDM_CONFIG_INVALID_ID;

                mdm_config_add_entry (config, &entry);
                mdm_config_process_entry (config, &entry, NULL);

                res = mdm_config_get_value (config, entry.group, entry.key, &value);
                if (! res) {
                        continue;
                }

                new_group = g_strdup_printf ("server-%s", mdm_config_value_get_string (value));
                mdm_config_value_free (value);

                for (j = 0; j < G_N_ELEMENTS (mdm_daemon_server_config_entries); j++) {
                        MdmConfigEntry *srv_entry;
                        if (mdm_daemon_server_config_entries[j].key == NULL) {
                                continue;
                        }
                        srv_entry = mdm_config_entry_copy (&mdm_daemon_server_config_entries[j]);
                        g_free (srv_entry->group);
                        srv_entry->group = g_strdup (new_group);
                        mdm_config_process_entry (config, srv_entry, NULL);
                        mdm_config_entry_free (srv_entry);
                }
                g_free (new_group);
        }
	g_ptr_array_free ((GPtrArray*) keys, TRUE);
}

static void
test_config (void)
{
        MdmConfig *config;
        GError    *error;
        int        i;

        config = mdm_config_new ();

        mdm_config_set_notify_func (config, notify_cb, NULL);
        mdm_config_set_validate_func (config, validate_cb, NULL);

        mdm_config_add_static_entries (config, mdm_daemon_config_entries);

        /* At first try loading with only defaults */
        mdm_config_set_default_file (config, DATADIR "/mdm/defaults.conf");

        g_message ("Loading configuration: Default source only");

        /* load the data files */
        error = NULL;
        mdm_config_load (config, &error);
        if (error != NULL) {
                g_warning ("Unable to load configuration: %s", error->message);
                g_error_free (error);
        } else {
                /* populate the database with all specified entries */
                mdm_config_process_all (config, &error);
        }

        g_message ("Getting all standard values");
        /* now test retrieving these values */
        for (i = 0; mdm_daemon_config_entries [i].group != NULL; i++) {
                MdmConfigValue       *value;
                const MdmConfigEntry *entry;
                gboolean              res;
                char                 *str;

                entry = &mdm_daemon_config_entries [i];

                res = mdm_config_get_value (config, entry->group, entry->key, &value);
                if (! res) {
                        g_warning ("Unable to lookup entry g=%s k=%s", entry->group, entry->key);
                        continue;
                }

                str = mdm_config_value_to_string (value);

                g_print ("Got g=%s k=%s: %s\n", entry->group, entry->key, str);

                g_free (str);
                mdm_config_value_free (value);
        }

        g_message ("Setting values");
        /* now test setting a few values */
        {
                MdmConfigValue *value;
                value = mdm_config_value_new_from_string  (MDM_CONFIG_VALUE_BOOL, "false", NULL);
                mdm_config_set_value (config, "greeter", "ShowLastSession", value);
                /* should only see one notification */
                mdm_config_set_value (config, "greeter", "ShowLastSession", value);
                mdm_config_value_free (value);
        }

        g_message ("Loading the server entries");
        load_servers_group (config);

        g_message ("Loading configuration: Default and Custom sources");
        /* Now try adding a custom config */
        mdm_config_set_custom_file (config, MDMCONFDIR "/custom.conf");
        /* load the data files */
        error = NULL;
        mdm_config_load (config, &error);
        if (error != NULL) {
                g_warning ("Unable to load configuration: %s", error->message);
                g_error_free (error);
        } else {
                /* populate the database with all specified entries */
                mdm_config_process_all (config, &error);
        }


        /* Test translated keys */

        mdm_config_free (config);
}

int
main (int argc, char **argv)
{

        test_config ();

	return 0;
}
