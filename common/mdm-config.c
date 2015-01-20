/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2007 William Jon McCann <mccann@jhu.edu>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <locale.h>
#include <syslog.h>
#include <errno.h>
#include <sys/stat.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <glib/gi18n.h>

#include "mdm-config.h"

struct _MdmConfig
{	
	char            *default_filename;
	char            *distro_filename;
	char            *custom_filename;
	
	gboolean         default_loaded;
	gboolean         distro_loaded;
	gboolean         custom_loaded;
	
	GKeyFile        *default_key_file;
	GKeyFile        *distro_key_file;
	GKeyFile        *custom_key_file;
	
	time_t           default_mtime;
	time_t           distro_mtime;
	time_t           custom_mtime;

	GPtrArray       *entries;

	GHashTable      *value_hash;

	MdmConfigFunc    validate_func;
	gpointer         validate_func_data;
	MdmConfigFunc    notify_func;
	gpointer         notify_func_data;
};


typedef struct _MdmConfigRealValue
{
	MdmConfigValueType type;
	union {
		gboolean bool;
		int      integer;
		char    *str;
		char   **array;
	} val;
} MdmConfigRealValue;

#define REAL_VALUE(x) ((MdmConfigRealValue *)(x))

GQuark
mdm_config_error_quark (void)
{
	return g_quark_from_static_string ("mdm-config-error-quark");
}

MdmConfigEntry *
mdm_config_entry_copy (const MdmConfigEntry *src)
{
	MdmConfigEntry *dest;

	dest = g_new0 (MdmConfigEntry, 1);
	dest->group = g_strdup (src->group);
	dest->key = g_strdup (src->key);
	dest->default_value = g_strdup (src->default_value);
	dest->type = src->type;
	dest->id = src->id;

	return dest;
}

void
mdm_config_entry_free (MdmConfigEntry *entry)
{
	g_free (entry->group);
	g_free (entry->key);
	g_free (entry->default_value);

	g_free (entry);
}

MdmConfigValue *
mdm_config_value_new (MdmConfigValueType type)
{
	MdmConfigValue *value;

	g_return_val_if_fail (type != MDM_CONFIG_VALUE_INVALID, NULL);

	value = (MdmConfigValue *) g_slice_new0 (MdmConfigRealValue);
	value->type = type;

	return value;
}

void
mdm_config_value_free (MdmConfigValue *value)
{
	MdmConfigRealValue *real;

	real = REAL_VALUE (value);

	switch (real->type) {
        case MDM_CONFIG_VALUE_INVALID:
        case MDM_CONFIG_VALUE_BOOL:
        case MDM_CONFIG_VALUE_INT:
		break;
        case MDM_CONFIG_VALUE_STRING:
        case MDM_CONFIG_VALUE_LOCALE_STRING:
		g_free (real->val.str);
		break;
	case MDM_CONFIG_VALUE_STRING_ARRAY:
	case MDM_CONFIG_VALUE_LOCALE_STRING_ARRAY:
		g_strfreev (real->val.array);
		break;
	default:
		g_assert_not_reached ();
		break;
	}

	g_slice_free (MdmConfigRealValue, real);
}

static void
set_string (char      **dest,
	    const char *src)
{
	if (*dest != NULL) {
		g_free (*dest);
	}

	*dest = src ? g_strdup (src) : NULL;
}

static void
set_string_array (char      ***dest,
		  const char **src)
{
	if (*dest != NULL) {
		g_strfreev (*dest);
	}

	*dest = src ? g_strdupv ((char **)src) : NULL;
}

MdmConfigValue *
mdm_config_value_copy (const MdmConfigValue *src)
{
	MdmConfigRealValue *dest;
	MdmConfigRealValue *real;

	g_return_val_if_fail (src != NULL, NULL);

	real = REAL_VALUE (src);
	dest = REAL_VALUE (mdm_config_value_new (src->type));

	switch (real->type) {
	case MDM_CONFIG_VALUE_INT:
	case MDM_CONFIG_VALUE_BOOL:
	case MDM_CONFIG_VALUE_INVALID:
		dest->val = real->val;
		break;
	case MDM_CONFIG_VALUE_STRING:
	case MDM_CONFIG_VALUE_LOCALE_STRING:
		set_string (&dest->val.str, real->val.str);
		break;
	case MDM_CONFIG_VALUE_STRING_ARRAY:
	case MDM_CONFIG_VALUE_LOCALE_STRING_ARRAY:
		set_string_array (&dest->val.array, (const char **)real->val.array);
		break;
	default:
		g_assert_not_reached();
	}

	return (MdmConfigValue *) dest;
}

const char *
mdm_config_value_get_string (const MdmConfigValue *value)
{
	g_return_val_if_fail (value != NULL, NULL);
	g_return_val_if_fail (value->type == MDM_CONFIG_VALUE_STRING, NULL);
	return REAL_VALUE (value)->val.str;
}

const char *
mdm_config_value_get_locale_string (const MdmConfigValue *value)
{
	g_return_val_if_fail (value != NULL, NULL);
	g_return_val_if_fail (value->type == MDM_CONFIG_VALUE_LOCALE_STRING, NULL);
	return REAL_VALUE (value)->val.str;
}

const char **
mdm_config_value_get_string_array (const MdmConfigValue *value)
{
	g_return_val_if_fail (value != NULL, NULL);
	g_return_val_if_fail (value->type == MDM_CONFIG_VALUE_STRING_ARRAY, NULL);
	return (const char **)REAL_VALUE (value)->val.array;
}

gboolean
mdm_config_value_get_bool (const MdmConfigValue *value)
{
	g_return_val_if_fail (value != NULL, FALSE);
	g_return_val_if_fail (value->type == MDM_CONFIG_VALUE_BOOL, FALSE);
	return REAL_VALUE (value)->val.bool;
}

int
mdm_config_value_get_int (const MdmConfigValue *value)
{
	g_return_val_if_fail (value != NULL, 0);
	g_return_val_if_fail (value->type == MDM_CONFIG_VALUE_INT, 0);
	return REAL_VALUE (value)->val.integer;
}

static gint
safe_strcmp (const char *a,
	     const char *b)
{
	return strcmp (a ? a : "", b ? b : "");
}

/* based on code from gconf */
int
mdm_config_value_compare (const MdmConfigValue *value_a,
			  const MdmConfigValue *value_b)
{
	g_return_val_if_fail (value_a != NULL, 0);
	g_return_val_if_fail (value_b != NULL, 0);

	if (value_a->type < value_b->type) {
		return -1;
	} else if (value_a->type > value_b->type) {
		return 1;
	}

	switch (value_a->type) {
	case MDM_CONFIG_VALUE_INT:
		if (mdm_config_value_get_int (value_a) < mdm_config_value_get_int (value_b)) {
			return -1;
		} else if (mdm_config_value_get_int (value_a) > mdm_config_value_get_int (value_b)) {
			return 1;
		} else {
			return 0;
		}
	case MDM_CONFIG_VALUE_STRING:
		return safe_strcmp (mdm_config_value_get_string (value_a),
				    mdm_config_value_get_string (value_b));
	case MDM_CONFIG_VALUE_LOCALE_STRING:
		return safe_strcmp (mdm_config_value_get_locale_string (value_a),
				    mdm_config_value_get_locale_string (value_b));
	case MDM_CONFIG_VALUE_STRING_ARRAY:
	case MDM_CONFIG_VALUE_LOCALE_STRING_ARRAY:
		{
			char *str_a;
			char *str_b;
			int   res;

			str_a = mdm_config_value_to_string (value_a);
			str_b = mdm_config_value_to_string (value_b);
			res = safe_strcmp (str_a, str_b);
			g_free (str_a);
			g_free (str_b);

			return res;
		}
	case MDM_CONFIG_VALUE_BOOL:
		if (mdm_config_value_get_bool (value_a) == mdm_config_value_get_bool (value_b)) {
			return 0;
		} else if (mdm_config_value_get_bool (value_a)) {
			return 1;
		} else {
			return -1;
		}
	case MDM_CONFIG_VALUE_INVALID:
	default:
		g_assert_not_reached ();
		break;
	}

	return 0;
}

/* based on code from gconf */
MdmConfigValue *
mdm_config_value_new_from_string (MdmConfigValueType type,
				  const char        *value_str,
				  GError           **error)
{
	MdmConfigValue *value;

	g_return_val_if_fail (type != MDM_CONFIG_VALUE_INVALID, NULL);
	g_return_val_if_fail (value_str != NULL, NULL);

	value = mdm_config_value_new (type);

        switch (value->type) {
        case MDM_CONFIG_VALUE_INT:
		{
			char* endptr = NULL;
			glong result;

			errno = 0;
			result = strtol (value_str, &endptr, 10);
			if (endptr == value_str) {
				g_set_error (error,
					     MDM_CONFIG_ERROR,
					     MDM_CONFIG_ERROR_PARSE_ERROR,
					     _("Didn't understand `%s' (expected integer)"),
					     value_str);
				mdm_config_value_free (value);
				value = NULL;
			} else if (errno == ERANGE) {
				g_set_error (error,
					     MDM_CONFIG_ERROR,
					     MDM_CONFIG_ERROR_PARSE_ERROR,
					     _("Integer `%s' is too large or small"),
					     value_str);
				mdm_config_value_free (value);
				value = NULL;
			} else {
				mdm_config_value_set_int (value, result);
			}
		}
                break;
        case MDM_CONFIG_VALUE_BOOL:
		switch (*value_str) {
		case 't':
		case 'T':
		case '1':
		case 'y':
		case 'Y':
			mdm_config_value_set_bool (value, TRUE);
			break;

		case 'f':
		case 'F':
		case '0':
		case 'n':
		case 'N':
			mdm_config_value_set_bool (value, FALSE);
			break;
		default:
			g_set_error (error,
				     MDM_CONFIG_ERROR,
				     MDM_CONFIG_ERROR_PARSE_ERROR,
				     _("Didn't understand `%s' (expected true or false)"),
				     value_str);
			mdm_config_value_free (value);
			value = NULL;
			break;
		}
		break;
        case MDM_CONFIG_VALUE_STRING:
		if (! g_utf8_validate (value_str, -1, NULL)) {
			g_set_error (error,
				     MDM_CONFIG_ERROR,
				     MDM_CONFIG_ERROR_PARSE_ERROR,
				     _("Text contains invalid UTF-8"));
			mdm_config_value_free (value);
			value = NULL;
		} else {
			mdm_config_value_set_string (value, value_str);
		}
                break;
        case MDM_CONFIG_VALUE_LOCALE_STRING:
		if (! g_utf8_validate (value_str, -1, NULL)) {
			g_set_error (error,
				     MDM_CONFIG_ERROR,
				     MDM_CONFIG_ERROR_PARSE_ERROR,
				     _("Text contains invalid UTF-8"));
			mdm_config_value_free (value);
			value = NULL;
		} else {
			mdm_config_value_set_locale_string (value, value_str);
		}
		break;
        case MDM_CONFIG_VALUE_STRING_ARRAY:
		if (! g_utf8_validate (value_str, -1, NULL)) {
			g_set_error (error,
				     MDM_CONFIG_ERROR,
				     MDM_CONFIG_ERROR_PARSE_ERROR,
				     _("Text contains invalid UTF-8"));
			mdm_config_value_free (value);
			value = NULL;
		} else {
			char **split;
			split = g_strsplit (value_str, ";", -1);
			mdm_config_value_set_string_array (value, (const char **)split);
			g_strfreev (split);
		}
                break;
        case MDM_CONFIG_VALUE_LOCALE_STRING_ARRAY:
		if (! g_utf8_validate (value_str, -1, NULL)) {
			g_set_error (error,
				     MDM_CONFIG_ERROR,
				     MDM_CONFIG_ERROR_PARSE_ERROR,
				     _("Text contains invalid UTF-8"));
			mdm_config_value_free (value);
			value = NULL;
		} else {
			char **split;
			split = g_strsplit (value_str, ";", -1);
			mdm_config_value_set_locale_string_array (value, (const char **)split);
			g_strfreev (split);
		}
                break;
        case MDM_CONFIG_VALUE_INVALID:
        default:
		g_assert_not_reached ();
                break;
        }

	return value;
}

void
mdm_config_value_set_string_array (MdmConfigValue *value,
				   const char    **array)
{
	MdmConfigRealValue *real;

	g_return_if_fail (value != NULL);
	g_return_if_fail (value->type == MDM_CONFIG_VALUE_STRING_ARRAY);

	real = REAL_VALUE (value);

	g_strfreev (real->val.array);
	real->val.array = g_strdupv ((char **)array);
}

void
mdm_config_value_set_locale_string_array (MdmConfigValue *value,
					  const char    **array)
{
	MdmConfigRealValue *real;

	g_return_if_fail (value != NULL);
	g_return_if_fail (value->type == MDM_CONFIG_VALUE_LOCALE_STRING_ARRAY);

	real = REAL_VALUE (value);

	g_strfreev (real->val.array);
	real->val.array = g_strdupv ((char **)array);
}

void
mdm_config_value_set_int (MdmConfigValue *value,
			  int             integer)
{
	MdmConfigRealValue *real;

	g_return_if_fail (value != NULL);
	g_return_if_fail (value->type == MDM_CONFIG_VALUE_INT);

	real = REAL_VALUE (value);

	real->val.integer = integer;
}

void
mdm_config_value_set_bool (MdmConfigValue *value,
			   gboolean        bool)
{
	MdmConfigRealValue *real;

	g_return_if_fail (value != NULL);
	g_return_if_fail (value->type == MDM_CONFIG_VALUE_BOOL);

	real = REAL_VALUE (value);

	real->val.bool = bool;
}

void
mdm_config_value_set_string (MdmConfigValue *value,
			     const char     *str)
{
	MdmConfigRealValue *real;

	g_return_if_fail (value != NULL);
	g_return_if_fail (value->type == MDM_CONFIG_VALUE_STRING);

	real = REAL_VALUE (value);

	g_free (real->val.str);
	real->val.str = g_strdup (str);
}

void
mdm_config_value_set_locale_string (MdmConfigValue *value,
				    const char     *str)
{
	MdmConfigRealValue *real;

	g_return_if_fail (value != NULL);
	g_return_if_fail (value->type == MDM_CONFIG_VALUE_LOCALE_STRING);

	real = REAL_VALUE (value);

	g_free (real->val.str);
	real->val.str = g_strdup (str);
}

char *
mdm_config_value_to_string (const MdmConfigValue *value)
{
	MdmConfigRealValue *real;
	char               *ret;

	g_return_val_if_fail (value != NULL, NULL);

	ret = NULL;
	real = REAL_VALUE (value);

	switch (real->type) {
        case MDM_CONFIG_VALUE_INVALID:
		break;
        case MDM_CONFIG_VALUE_BOOL:
		ret = real->val.bool ? g_strdup ("true") : g_strdup ("false");
		break;
        case MDM_CONFIG_VALUE_INT:
		ret = g_strdup_printf ("%d", real->val.integer);
		break;
        case MDM_CONFIG_VALUE_STRING:
        case MDM_CONFIG_VALUE_LOCALE_STRING:
		ret = g_strdup (real->val.str);
		break;
	case MDM_CONFIG_VALUE_STRING_ARRAY:
	case MDM_CONFIG_VALUE_LOCALE_STRING_ARRAY:
		ret = g_strjoinv (";", real->val.array);
		break;
	default:
		g_assert_not_reached ();
		break;
	}
	return ret;
}

static void
mdm_config_init (MdmConfig *config)
{
	config->entries = g_ptr_array_new ();
	config->value_hash = g_hash_table_new_full (g_str_hash,
						    g_str_equal,
						    (GDestroyNotify)g_free,
						    (GDestroyNotify)mdm_config_value_free);
}

MdmConfig *
mdm_config_new (void)
{
	MdmConfig *config;

	config = g_slice_new0 (MdmConfig);
	mdm_config_init (config);

	return config;
}

/*
 * Note that this function can be called a second time while 
 * MDM is in the middle of processing this function.  This is
 * because some MDM signal handlers (such as main_daemon_abrt)
 * call mdm_final_cleanup, which ends up calling this function.
 * To fix the sort of crashing problem reported in bugzilla bug
 * #517526.  This function could probably be made more thread
 * safe.
 */
void
mdm_config_free (MdmConfig *config)
{
	MdmConfigEntry *e;
	GKeyFile       *mkf, *dkf, *ckf;
	GHashTable     *hash;

	g_return_if_fail (config != NULL);

	/*
	 * Set local variables equal to the memory that we
	 * intend to free, and set the structure variables
	 * to NULL, so if this function is called again, we
	 * do not try to free the same data structures again.
	 */
	e    = config->entries;	
	dkf  = config->default_key_file;
	mkf  = config->distro_key_file;
	ckf  = config->custom_key_file;
	hash = config->value_hash;

	config->entries            = NULL;	
	config->default_key_file   = NULL;
	config->distro_key_file    = NULL;
	config->custom_key_file    = NULL;
	config->value_hash         = NULL;
	
	g_free (config->default_filename);
	g_free (config->distro_filename);
	g_free (config->custom_filename);

	g_slice_free (MdmConfig, config);

	if (e != NULL) {
		g_ptr_array_foreach (e, (GFunc)mdm_config_entry_free, NULL);
		g_ptr_array_free (e, TRUE);
	}
	if (mkf != NULL)
		g_key_file_free (mkf);
	if (dkf != NULL)
		g_key_file_free (dkf);
	if (ckf != NULL)
		g_key_file_free (ckf);
	if (hash != NULL)
		g_hash_table_destroy (hash);
}

static void
add_server_group_once (GPtrArray *server_groups, char *group)
{
	int i;

	for (i=0; i < server_groups->len; i++) {
		if (strcmp (g_ptr_array_index (server_groups, i), group) == 0) {
			g_debug ("server group %s already exists, skipping",
				group);
			return;
		}
	}
	g_ptr_array_add (server_groups, g_strdup (group));
}

GPtrArray *
mdm_config_get_server_groups (MdmConfig *config)
{
	GPtrArray       *server_groups;
	GError          *error;
	char           **groups;
	gsize            len;
	int              i;
	
	server_groups = g_ptr_array_new ();
	
	if (config->default_key_file != NULL) {
		groups = g_key_file_get_groups (config->default_key_file, &len);

		for (i = 0; i < len; i++)
		{
			if (g_str_has_prefix (groups[i], "server-")) {
				add_server_group_once (server_groups, groups[i]);
			}
		}
		g_strfreev (groups);
	}

	if (config->distro_key_file != NULL) {
		groups = g_key_file_get_groups (config->distro_key_file, &len);

		for (i = 0; i < len; i++)
		{
			if (g_str_has_prefix (groups[i], "server-")) {
				add_server_group_once (server_groups, groups[i]);
			}
		}
		g_strfreev (groups);
	}

	if (config->custom_key_file != NULL) {
		groups = g_key_file_get_groups (config->custom_key_file, &len);

		for (i = 0; i < len; i++)
		{
			if (g_str_has_prefix (groups[i], "server-")) {
				add_server_group_once (server_groups, groups[i]);
			}
		}
		g_strfreev (groups);
	}

	return server_groups;
}

const MdmConfigEntry *
mdm_config_lookup_entry (MdmConfig  *config,
			 const char *group,
			 const char *key)
{
	int                   i;
	const MdmConfigEntry *entry;

	g_return_val_if_fail (config != NULL, NULL);
	g_return_val_if_fail (group != NULL, NULL);
	g_return_val_if_fail (key != NULL, NULL);

	entry = NULL;

	for (i = 0; i < config->entries->len; i++) {
		MdmConfigEntry *this;
		this = g_ptr_array_index (config->entries, i);
		if (strcmp (this->group, group) == 0
		    && strcmp (this->key, key) == 0) {
			entry = (const MdmConfigEntry *)this;
			break;
		}
	}

	return entry;
}

const MdmConfigEntry *
mdm_config_lookup_entry_for_id (MdmConfig  *config,
				int         id)
{
	int                   i;
	const MdmConfigEntry *entry;

	g_return_val_if_fail (config != NULL, NULL);

	entry = NULL;

	for (i = 0; i < config->entries->len; i++) {
		MdmConfigEntry *this;
		this = g_ptr_array_index (config->entries, i);
		if (this->id == id) {
			entry = (const MdmConfigEntry *)this;
			break;
		}
	}

	return entry;
}

void
mdm_config_add_entry (MdmConfig            *config,
		      const MdmConfigEntry *entry)
{
	MdmConfigEntry *new_entry;

	g_return_if_fail (config != NULL);
	g_return_if_fail (entry != NULL);

	new_entry = mdm_config_entry_copy (entry);
	g_ptr_array_add (config->entries, new_entry);
}

void
mdm_config_add_static_entries (MdmConfig            *config,
			       const MdmConfigEntry *entries)
{
	int i;

	g_return_if_fail (config != NULL);
	g_return_if_fail (entries != NULL);

	for (i = 0; entries[i].group != NULL; i++) {
		mdm_config_add_entry (config, &entries[i]);
	}
}

void
mdm_config_set_validate_func (MdmConfig       *config,
			      MdmConfigFunc    func,
			      gpointer         data)
{
	g_return_if_fail (config != NULL);

	config->validate_func = func;
	config->validate_func_data = data;
}

void
mdm_config_set_default_file (MdmConfig  *config,
			     const char *name)
{
	g_return_if_fail (config != NULL);

	g_free (config->default_filename);
	config->default_filename = g_strdup (name);
}

void
mdm_config_set_distro_file (MdmConfig  *config,
			       const char *name)
{
	g_return_if_fail (config != NULL);

	g_free (config->distro_filename);
	config->distro_filename = g_strdup (name);
}

void
mdm_config_set_custom_file (MdmConfig  *config,
			    const char *name)
{
	g_return_if_fail (config != NULL);

	g_free (config->custom_filename);
	config->custom_filename = g_strdup (name);
}

void
mdm_config_set_notify_func (MdmConfig       *config,
			    MdmConfigFunc    func,
			    gpointer         data)
{
	g_return_if_fail (config != NULL);

	config->notify_func = func;
	config->notify_func_data = data;
}

static gboolean
key_file_get_value (MdmConfig            *config,
		    GKeyFile             *key_file,
		    const char           *group,
		    const char           *key,
		    MdmConfigValueType    type,
		    MdmConfigValue      **valuep)
{
	char           *val;
	GError         *error;
	MdmConfigValue *value;
	gboolean        ret;

	ret = FALSE;
	value = NULL;

	error = NULL;
	if (type == MDM_CONFIG_VALUE_LOCALE_STRING ||
	    type == MDM_CONFIG_VALUE_LOCALE_STRING_ARRAY) {
		/* Use NULL locale to detect current locale */
		val = g_key_file_get_locale_string (key_file,
						    group,
						    key,
						    NULL,
						    &error);
		g_debug ("Loading locale string: %s %s", key, val ? val : "(null)");

		if (error != NULL) {
			g_debug ("%s", error->message);
			g_error_free (error);
			error = NULL;
		}
		if (val == NULL) {
			error = NULL;
			val = g_key_file_get_value (key_file,
						    group,
						    key,
						    &error);
			g_debug ("Loading non-locale string: %s %s", key, val ? val : "(null)");
		}
	} else {
		val = g_key_file_get_value (key_file,
					    group,
					    key,
					    &error);
	}

	if (error != NULL) {
		g_error_free (error);
		goto out;
	}

	if (val == NULL) {
		goto out;
	}

	error = NULL;
	value = mdm_config_value_new_from_string (type, val, &error);
	if (error != NULL) {
		g_warning ("%s", error->message);
		g_error_free (error);
		goto out;
	}

	ret = TRUE;

 out:
	*valuep = value;

	g_free (val);

	return ret;
}

static void
entry_get_default_value (MdmConfig            *config,
			 const MdmConfigEntry *entry,
			 MdmConfigValue      **valuep)
{
	MdmConfigValue *value;
	GError         *error;

	error = NULL;
	value = mdm_config_value_new_from_string (entry->type,
						  entry->default_value ? entry->default_value : "",
						  &error);
	if (error != NULL) {
		g_warning ("%s", error->message);
		g_error_free (error);
	}

	*valuep = value;
}

static gboolean
load_value_entry (MdmConfig            *config,
		  const MdmConfigEntry *entry,
		  MdmConfigValue      **valuep,
		  MdmConfigSourceType  *sourcep)
{
	MdmConfigValue     *value;
	MdmConfigSourceType source;
	gboolean            ret;
	gboolean            res;

	value = NULL;

	/* Look for the first occurence of the key in:
	   custom file, distro file, default file, or built-in-default
	 */
	
	if (config->custom_filename != NULL) {
		source = MDM_CONFIG_SOURCE_CUSTOM;
		res = key_file_get_value (config,
					  config->custom_key_file,
					  entry->group,
					  entry->key,
					  entry->type,
					  &value);
		if (res) {
			goto done;
		}
	}
	if (config->distro_filename != NULL) {
		source = MDM_CONFIG_SOURCE_DISTRO;
		res = key_file_get_value (config,
					  config->distro_key_file,
					  entry->group,
					  entry->key,
					  entry->type,
					  &value);
		if (res) {
			goto done;
		}
	}
	if (config->default_filename != NULL) {
		source = MDM_CONFIG_SOURCE_DEFAULT;
		res = key_file_get_value (config,
					  config->default_key_file,
					  entry->group,
					  entry->key,
					  entry->type,
					  &value);
		if (res) {
			goto done;
		}
	}


	source = MDM_CONFIG_SOURCE_BUILT_IN;
	entry_get_default_value (config, entry, &value);

 done:

	if (value != NULL) {
		ret = TRUE;
	} else {
		ret = FALSE;
	}

	*valuep = value;
	*sourcep = source;

	return ret;
}

static int
lookup_id_for_key (MdmConfig  *config,
		   const char *group,
		   const char *key)
{
	int                   id;
	const MdmConfigEntry *entry;

	id = MDM_CONFIG_INVALID_ID;
	entry = mdm_config_lookup_entry (config, group, key);
	if (entry != NULL) {
		id = entry->id;
	}

	return id;
}

static void
internal_set_value (MdmConfig          *config,
		    MdmConfigSourceType source,
		    const char         *group,
		    const char         *key,
		    MdmConfigValue     *value)
{
	char           *key_path;
	int             id;
	MdmConfigValue *v;
	gboolean        res;

	g_return_if_fail (config != NULL);

	key_path = g_strdup_printf ("%s/%s", group, key);

	v = NULL;
	res = g_hash_table_lookup_extended (config->value_hash,
					    key_path,
					    NULL,
					    (gpointer *)&v);

	if (res) {
		if (v != NULL && mdm_config_value_compare (v, value) == 0) {
			/* value is the same - don't update */
			goto out;
		}
	}

	g_hash_table_insert (config->value_hash,
			     g_strdup (key_path),
			     mdm_config_value_copy (value));

	id = lookup_id_for_key (config, group, key);

	if (config->notify_func) {
		(* config->notify_func) (config, source, group, key, value, id, config->notify_func_data);
	}
 out:
	g_free (key_path);
}

static void
store_entry_value (MdmConfig            *config,
		   const MdmConfigEntry *entry,
		   MdmConfigSourceType   source,
		   MdmConfigValue       *value)
{
	internal_set_value (config, source, entry->group, entry->key, value);
}

static gboolean
load_entry (MdmConfig            *config,
	    const MdmConfigEntry *entry)
{
	MdmConfigValue     *value;
	MdmConfigSourceType source;
	gboolean            res;

	value = NULL;
	source = MDM_CONFIG_SOURCE_INVALID;

	res = load_value_entry (config, entry, &value, &source);
	if (!res) {
		return FALSE;
	}

	res = TRUE;
	if (config->validate_func) {
		res = (* config->validate_func) (config, source, entry->group, entry->key, value, entry->id, config->validate_func_data);
	}

	if (res) {
		/* store runs notify */
		store_entry_value (config, entry, source, value);
	}

	return TRUE;
}

static void
add_keys_to_hash (GKeyFile   *key_file,
		  const char *group_name,
		  GHashTable *hash)
{
	GError     *local_error;
	gchar      **keys;
	gsize       len;
	int         i;

	local_error = NULL;
	len = 0;
	keys = g_key_file_get_keys (key_file,
				    group_name,
				    &len,
				    &local_error);
	if (local_error != NULL) {
		g_error_free (local_error);
		g_strfreev (keys);
		return;
	}

	for (i = 0; i < len; i++) {
		g_hash_table_insert (hash, g_strdup (keys[i]), GINT_TO_POINTER (1));
	}

	g_strfreev (keys);
}

static void
collect_hash_keys (const char *key,
		   gpointer    value,
		   GPtrArray **array)
{
	g_message ("Adding %s", key);
	g_ptr_array_add (*array, g_strdup (key));
}

char **
mdm_config_get_keys_for_group (MdmConfig  *config,
			       const char *group,
			       gsize      *length,
			       GError    **error)
{
	GHashTable *hash;
	gsize       len;
	GPtrArray  *array;

	hash = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);	

	if (config->default_filename != NULL) {
		add_keys_to_hash (config->default_key_file, group, hash);
	}

	if (config->distro_filename != NULL) {
		add_keys_to_hash (config->distro_key_file, group, hash);
	}

	if (config->custom_filename != NULL) {
		add_keys_to_hash (config->custom_key_file, group, hash);
	}

	len = g_hash_table_size (hash);
	array = g_ptr_array_sized_new (len);

	g_hash_table_foreach (hash, (GHFunc)collect_hash_keys, &array);
	g_ptr_array_add (array, NULL);

	g_hash_table_destroy (hash);

	if (length != NULL) {
		*length = array->len - 1;
	}

	return (char **)g_ptr_array_free (array, FALSE);
}

static gboolean
load_backend (MdmConfig  *config,
	      const char *filename,
	      GKeyFile  **key_file,
	      time_t     *mtime)
{
	GError     *local_error;
	gboolean    res;
	gboolean    ret;
	struct stat statbuf;
	GKeyFile   *kf;
	time_t      lmtime;

	if (filename == NULL) {
		return FALSE;
	}

	if (g_stat (filename, &statbuf) != 0) {
		return FALSE;
	}
	lmtime = statbuf.st_mtime;

	/* if already loaded check whether reload is necessary */
	if (*key_file != NULL) {
		if (lmtime > *mtime) {

			/* needs an update */

                        /*
                         * As in mdm-config-free, set a local
                         * variable equal to the memory to 
                         * free, and set the structure to 
                         * NULL, so if this function is 
                         * called again, we do not free the
                         * same data stucture again.  Similar
                         * to bug #517526.  Again, this could
                         * probably be made more thread safe.
                         */
			kf = *key_file;
			*key_file = NULL;
			g_key_file_free (kf);
		} else {
			/* no reload necessary so we're done */
			return TRUE;
		}
	}

	kf = g_key_file_new ();

	local_error = NULL;
	res = g_key_file_load_from_file (kf,
					 filename,
					 G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS,
					 &local_error);
	if (! res) {
		g_error_free (local_error);
		g_key_file_free (kf);
		kf = NULL;
		lmtime = 0;
		ret = FALSE;
	} else {
		ret = TRUE;
	}

	*key_file = kf;
	*mtime = lmtime;

	return ret;
}

gboolean
mdm_config_load (MdmConfig *config,
		 GError   **error)
{
	g_return_val_if_fail (config != NULL, FALSE);
	
	config->default_loaded = load_backend (config,
					       config->default_filename,
					       &config->default_key_file,
					       &config->default_mtime);
	config->distro_loaded = load_backend (config,
						 config->distro_filename,
						 &config->distro_key_file,
						 &config->distro_mtime);
	config->custom_loaded = load_backend (config,
					      config->custom_filename,
					      &config->custom_key_file,
					      &config->custom_mtime);

	return TRUE;
}

static gboolean
process_entries (MdmConfig             *config,
		 const MdmConfigEntry **entries,
		 gsize                  n_entries,
		 GError               **error)
{
	gboolean ret;
	int      i;

	ret = TRUE;

	for (i = 0; i < n_entries; i++) {
		load_entry (config, entries[i]);
	}

	return ret;
}

gboolean
mdm_config_process_entry (MdmConfig            *config,
			  const MdmConfigEntry *entry,
			  GError              **error)
{
	gboolean  ret;

	g_return_val_if_fail (config != NULL, FALSE);
	g_return_val_if_fail (entry != NULL, FALSE);

	ret = load_entry (config, entry);

	return ret;
}

gboolean
mdm_config_process_entries (MdmConfig             *config,
			    const MdmConfigEntry **entries,
			    gsize                  n_entries,
			    GError               **error)
{
	gboolean  ret;

	g_return_val_if_fail (config != NULL, FALSE);
	g_return_val_if_fail (entries != NULL, FALSE);
	g_return_val_if_fail (n_entries > 0, FALSE);

	ret = process_entries (config, entries, n_entries, error);

	return ret;
}

gboolean
mdm_config_process_all (MdmConfig *config,
			GError   **error)
{
	gboolean  ret;

	g_return_val_if_fail (config != NULL, FALSE);

	ret = process_entries (config,
			       (const MdmConfigEntry **)config->entries->pdata,
			       config->entries->len,
			       error);

	return ret;
}

gboolean
mdm_config_peek_value (MdmConfig             *config,
		       const char            *group,
		       const char            *key,
		       const MdmConfigValue **valuep)
{
	gboolean              ret;
	char                 *key_path;
	const MdmConfigValue *value;

	g_return_val_if_fail (config != NULL, FALSE);

	key_path = g_strdup_printf ("%s/%s", group, key);
	value = NULL;
	ret = g_hash_table_lookup_extended (config->value_hash,
					    key_path,
					    NULL,
					    (gpointer *)&value);
	g_free (key_path);

	if (valuep != NULL) {
		if (ret) {
			*valuep = value;
		} else {
			*valuep = NULL;
		}
	}

	return ret;
}

gboolean
mdm_config_get_value (MdmConfig       *config,
		      const char      *group,
		      const char      *key,
		      MdmConfigValue **valuep)
{
	gboolean              res;
	const MdmConfigValue *value;

	res = mdm_config_peek_value (config, group, key, &value);
	if (valuep != NULL) {
		*valuep = (value == NULL) ? NULL : mdm_config_value_copy (value);
	}

	return res;
}

gboolean
mdm_config_set_value (MdmConfig       *config,
		      const char      *group,
		      const char      *key,
		      MdmConfigValue  *value)
{
	g_return_val_if_fail (config != NULL, FALSE);
	g_return_val_if_fail (group != NULL, FALSE);
	g_return_val_if_fail (key != NULL, FALSE);
	g_return_val_if_fail (value != NULL, FALSE);

	internal_set_value (config, MDM_CONFIG_SOURCE_RUNTIME_USER, group, key, value);

	return TRUE;
}

static gboolean
mdm_config_peek_value_for_id (MdmConfig             *config,
			      int                    id,
			      const MdmConfigValue **valuep)
{
	const MdmConfigEntry *entry;

	g_return_val_if_fail (config != NULL, FALSE);

	entry = mdm_config_lookup_entry_for_id (config, id);
	if (entry == NULL) {
		return FALSE;
	}

	return mdm_config_peek_value (config, entry->group, entry->key, valuep);
}

gboolean
mdm_config_get_value_for_id (MdmConfig       *config,
			     int              id,
			     MdmConfigValue **valuep)
{
	const MdmConfigEntry *entry;

	g_return_val_if_fail (config != NULL, FALSE);

	entry = mdm_config_lookup_entry_for_id (config, id);
	if (entry == NULL) {
		return FALSE;
	}

	return mdm_config_get_value (config, entry->group, entry->key, valuep);
}

gboolean
mdm_config_set_value_for_id (MdmConfig      *config,
			     int             id,
			     MdmConfigValue *valuep)
{
	const MdmConfigEntry *entry;

	g_return_val_if_fail (config != NULL, FALSE);

	entry = mdm_config_lookup_entry_for_id (config, id);
	if (entry == NULL) {
		return FALSE;
	}

	return mdm_config_set_value (config, entry->group, entry->key, valuep);
}

gboolean
mdm_config_peek_string_for_id (MdmConfig       *config,
			       int              id,
			       const char     **strp)
{
	const MdmConfigValue *value;
	const char           *str;
	gboolean              res;

	g_return_val_if_fail (config != NULL, FALSE);

	res = mdm_config_peek_value_for_id (config, id, &value);
	if (! res) {
		return FALSE;
	}

	str = mdm_config_value_get_string (value);
	if (strp != NULL) {
		*strp = str;
	}

	return res;
}

gboolean
mdm_config_get_string_for_id (MdmConfig       *config,
			      int              id,
			      char           **strp)
{
	gboolean    res;
	const char *str;

	res = mdm_config_peek_string_for_id (config, id, &str);
	if (res && strp != NULL) {
		*strp = g_strdup (str);
	}

	return res;
}

gboolean
mdm_config_get_bool_for_id (MdmConfig       *config,
			    int              id,
			    gboolean        *boolp)
{
	MdmConfigValue *value;
	gboolean        bool;
	gboolean        res;

	g_return_val_if_fail (config != NULL, FALSE);

	res = mdm_config_get_value_for_id (config, id, &value);
	if (! res) {
		return FALSE;
	}

	bool = mdm_config_value_get_bool (value);
	if (boolp != NULL) {
		*boolp = bool;
	}

	mdm_config_value_free (value);

	return res;
}

gboolean
mdm_config_get_int_for_id (MdmConfig       *config,
			   int              id,
			   int             *integerp)
{
	MdmConfigValue *value;
	gboolean        integer;
	gboolean        res;

	g_return_val_if_fail (config != NULL, FALSE);

	res = mdm_config_get_value_for_id (config, id, &value);
	if (! res) {
		return FALSE;
	}

	integer = mdm_config_value_get_int (value);
	if (integerp != NULL) {
		*integerp = integer;
	}

	mdm_config_value_free (value);

	return res;
}

gboolean
mdm_config_set_string_for_id (MdmConfig      *config,
			      int             id,
			      char           *str)
{
	MdmConfigValue *value;
	gboolean        res;

	g_return_val_if_fail (config != NULL, FALSE);

	value = mdm_config_value_new (MDM_CONFIG_VALUE_STRING);
	mdm_config_value_set_string (value, str);

	res = mdm_config_set_value_for_id (config, id, value);
	mdm_config_value_free (value);

	return res;
}

gboolean
mdm_config_set_bool_for_id (MdmConfig      *config,
			    int             id,
			    gboolean        bool)
{
	MdmConfigValue *value;
	gboolean        res;

	g_return_val_if_fail (config != NULL, FALSE);

	value = mdm_config_value_new (MDM_CONFIG_VALUE_BOOL);
	mdm_config_value_set_bool (value, bool);

	res = mdm_config_set_value_for_id (config, id, value);
	mdm_config_value_free (value);

	return res;
}

gboolean
mdm_config_set_int_for_id (MdmConfig      *config,
			   int             id,
			   int             integer)
{
	MdmConfigValue *value;
	gboolean        res;

	g_return_val_if_fail (config != NULL, FALSE);

	value = mdm_config_value_new (MDM_CONFIG_VALUE_INT);
	mdm_config_value_set_int (value, integer);

	res = mdm_config_set_value_for_id (config, id, value);
	mdm_config_value_free (value);

	return res;
}
