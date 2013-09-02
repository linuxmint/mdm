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

#ifndef _MDM_CONFIG_H
#define _MDM_CONFIG_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct _MdmConfig MdmConfig;

typedef enum {
        MDM_CONFIG_VALUE_INVALID,
        MDM_CONFIG_VALUE_BOOL,
        MDM_CONFIG_VALUE_INT,
        MDM_CONFIG_VALUE_STRING,
        MDM_CONFIG_VALUE_LOCALE_STRING,
        MDM_CONFIG_VALUE_STRING_ARRAY,
        MDM_CONFIG_VALUE_LOCALE_STRING_ARRAY,
} MdmConfigValueType;

typedef enum {
        MDM_CONFIG_SOURCE_INVALID,
        MDM_CONFIG_SOURCE_DISTRO,
        MDM_CONFIG_SOURCE_DEFAULT,
        MDM_CONFIG_SOURCE_CUSTOM,
        MDM_CONFIG_SOURCE_BUILT_IN,
        MDM_CONFIG_SOURCE_RUNTIME_USER,
} MdmConfigSourceType;

#define MDM_CONFIG_INVALID_ID -1

struct _MdmConfigValue
{
	MdmConfigValueType type;
};

typedef struct _MdmConfigValue MdmConfigValue;

typedef gboolean (* MdmConfigFunc) (MdmConfig          *config,
				    MdmConfigSourceType source,
				    const char         *group,
				    const char         *key,
				    MdmConfigValue     *value,
				    int                 id,
				    gpointer            data);

typedef struct {
	char              *group;
	char              *key;
	MdmConfigValueType type;
	char              *default_value;
	int                id;
} MdmConfigEntry;

#define MDM_CONFIG_ERROR (mdm_config_error_quark ())

typedef enum
{
	MDM_CONFIG_ERROR_UNKNOWN_OPTION,
	MDM_CONFIG_ERROR_BAD_VALUE,
	MDM_CONFIG_ERROR_PARSE_ERROR,
	MDM_CONFIG_ERROR_FAILED
} MdmConfigError;

GQuark                 mdm_config_error_quark            (void);

MdmConfig *            mdm_config_new                    (void);
void                   mdm_config_free                   (MdmConfig       *config);

void                   mdm_config_set_validate_func      (MdmConfig       *config,
							  MdmConfigFunc    func,
							  gpointer         data);
void                   mdm_config_set_notify_func        (MdmConfig       *config,
							  MdmConfigFunc    func,
							  gpointer         data);
void                   mdm_config_set_default_file       (MdmConfig       *config,
							  const char      *name);
void                   mdm_config_set_distro_file     (MdmConfig       *config,
							  const char      *name);
void                   mdm_config_set_custom_file        (MdmConfig       *config,
							  const char      *name);
void                   mdm_config_add_entry              (MdmConfig            *config,
							  const MdmConfigEntry *entry);
void                   mdm_config_add_static_entries     (MdmConfig            *config,
							  const MdmConfigEntry *entries);
const MdmConfigEntry * mdm_config_lookup_entry           (MdmConfig            *config,
							  const char           *group,
							  const char           *key);
const MdmConfigEntry * mdm_config_lookup_entry_for_id    (MdmConfig            *config,
							  int                   id);

gboolean               mdm_config_load                   (MdmConfig             *config,
							  GError               **error);
gboolean               mdm_config_process_all            (MdmConfig             *config,
							  GError               **error);
gboolean               mdm_config_process_entry          (MdmConfig             *config,
							  const MdmConfigEntry  *entry,
							  GError               **error);
gboolean               mdm_config_process_entries        (MdmConfig             *config,
							  const MdmConfigEntry **entries,
							  gsize                  n_entries,
							  GError               **error);

gboolean               mdm_config_save_custom_file       (MdmConfig       *config,
							  GError         **error);
char **                mdm_config_get_keys_for_group     (MdmConfig       *config,
							  const gchar     *group_name,
							  gsize           *length,
							  GError         **error);
GPtrArray *            mdm_config_get_server_groups      (MdmConfig       *config);

gboolean               mdm_config_peek_value             (MdmConfig             *config,
							  const char            *group,
							  const char            *key,
							  const MdmConfigValue **value);
gboolean               mdm_config_get_value              (MdmConfig       *config,
							  const char      *group,
							  const char      *key,
							  MdmConfigValue **value);
gboolean               mdm_config_set_value              (MdmConfig       *config,
							  const char      *group,
							  const char      *key,
							  MdmConfigValue  *value);

/* convenience functions */
gboolean               mdm_config_get_value_for_id       (MdmConfig       *config,
							  int              id,
							  MdmConfigValue **value);
gboolean               mdm_config_set_value_for_id       (MdmConfig       *config,
							  int              id,
							  MdmConfigValue  *value);

gboolean               mdm_config_peek_string_for_id     (MdmConfig       *config,
							  int              id,
							  const char     **str);
gboolean               mdm_config_get_string_for_id      (MdmConfig       *config,
							  int              id,
							  char           **str);
gboolean               mdm_config_get_bool_for_id        (MdmConfig       *config,
							  int              id,
							  gboolean        *bool);
gboolean               mdm_config_get_int_for_id         (MdmConfig       *config,
							  int              id,
							  int             *integer);
gboolean               mdm_config_set_string_for_id      (MdmConfig       *config,
							  int              id,
							  char            *str);
gboolean               mdm_config_set_bool_for_id        (MdmConfig       *config,
							  int              id,
							  gboolean         bool);
gboolean               mdm_config_set_int_for_id         (MdmConfig       *config,
							  int              id,
							  int              integer);

/* Config Values */

MdmConfigValue *     mdm_config_value_new              (MdmConfigValueType    type);
void                 mdm_config_value_free             (MdmConfigValue       *value);
MdmConfigValue *     mdm_config_value_copy             (const MdmConfigValue *value);
int                  mdm_config_value_compare          (const MdmConfigValue *value_a,
							const MdmConfigValue *value_b);

MdmConfigValue *     mdm_config_value_new_from_string  (MdmConfigValueType    type,
							const char           *str,
							GError              **error);
const char *         mdm_config_value_get_string       (const MdmConfigValue *value);
const char *         mdm_config_value_get_locale_string       (const MdmConfigValue *value);
const char **        mdm_config_value_get_string_array (const MdmConfigValue *value);

int                  mdm_config_value_get_int          (const MdmConfigValue *value);
gboolean             mdm_config_value_get_bool         (const MdmConfigValue *value);

void                 mdm_config_value_set_string       (MdmConfigValue  *value,
							const char      *str);
void                 mdm_config_value_set_locale_string       (MdmConfigValue  *value,
							       const char      *str);

void                 mdm_config_value_set_string_array (MdmConfigValue  *value,
							const char     **array);
void                 mdm_config_value_set_locale_string_array (MdmConfigValue  *value,
							       const char     **array);
void                 mdm_config_value_set_int          (MdmConfigValue  *value,
							int              integer);
void                 mdm_config_value_set_bool         (MdmConfigValue  *value,
							gboolean         bool);
char *               mdm_config_value_to_string        (const MdmConfigValue *value);

/* Config Entries */
MdmConfigEntry *     mdm_config_entry_copy             (const MdmConfigEntry *entry);
void                 mdm_config_entry_free             (MdmConfigEntry       *entry);

G_END_DECLS

#endif /* _MDM_CONFIG_H */
