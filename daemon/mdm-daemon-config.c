/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * MDM - The MDM Display Manager
 * Copyright (C) 1998, 1999, 2000 Martin K. Petersen <mkp@mkp.net>
 * Copyright (C) 2005 Sun Microsystems, Inc.
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

/*
 * mdm-daemon-config.c isolates most logic that interacts with MDM
 * configuration into a single file and provides a mechanism for
 * interacting with MDM configuration optins via access functions for
 * getting/setting values.  This logic also ensures that the same
 * configuration validation happens when loading the values initially
 * or setting them via the MDM_UPDATE_CONFIG socket command.
 *
 * When adding a new configuration option, simply add the new option
 * to mdm-daemon-config-entries.h.  Any validation for the
 * configuration option should be placed in the validate_cb function.
 */

#include "config.h"

#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <fcntl.h>
#include <string.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <signal.h>
#include <pwd.h>
#include <grp.h>

#include <glib.h>
#include <glib/gi18n.h>

#include "mdm.h"
#include "verify.h"
#include "mdm-net.h"
#include "misc.h"
#include "server.h"
#include "filecheck.h"
#include "slave.h"

#include "mdm-common.h"
#include "mdm-config.h"
#include "mdm-log.h"
#include "mdm-daemon-config.h"

#include "mdm-socket-protocol.h"

static MdmConfig *daemon_config = NULL;

static GSList *displays = NULL;
static GSList *xservers = NULL;

static gint high_display_num = 0;
static const char *default_config_file = NULL;
static char *custom_config_file = NULL;

static uid_t MdmUserId;   /* Userid  under which mdm should run */
static gid_t MdmGroupId;  /* Gruopid under which mdm should run */

/**
 * is_key
 *
 * Since MDM keys sometimes have default values defined in the mdm.h header
 * file (e.g. key=value), this function strips off the "=value" from both 
 * keys passed and compares them, returning TRUE if they are the same, 
 * FALSE otherwise.
 */
static gboolean
is_key (const gchar *key1, const gchar *key2)
{
	gchar *key1d, *key2d, *p;

	key1d = g_strdup (key1);
	key2d = g_strdup (key2);

	g_strstrip (key1d);
	p = strchr (key1d, '=');
	if (p != NULL)
		*p = '\0';

	g_strstrip (key2d);
	p = strchr (key2d, '=');
	if (p != NULL)
		*p = '\0';

	if (strcmp (ve_sure_string (key1d), ve_sure_string (key2d)) == 0) {
		g_free (key1d);
		g_free (key2d);
		return TRUE;
	} else {
		g_free (key1d);
		g_free (key2d);
		return FALSE;
	}
}

/**
 * mdm_daemon_config_get_per_display_custom_config_file
 *
 * Returns the per-display config file for a given display
 * This is always the custom config file name with the display
 * appended, and never mdm.conf.
 */
static gchar *
mdm_daemon_config_get_per_display_custom_config_file (const gchar *display)
{
	return g_strdup_printf ("%s%s", custom_config_file, display);
}

/**
 * mdm_daemon_config_get_custom_config_file
 *
 * Returns the custom config file being used.
 */
gchar *
mdm_daemon_config_get_custom_config_file (void)
{
	return custom_config_file;
}

/**
 * mdm_daemon_config_get_display_list
 *
 * Returns the list of displays being used.
 */
GSList *
mdm_daemon_config_get_display_list (void)
{
	return displays;
}

GSList *
mdm_daemon_config_display_list_append (MdmDisplay *display)
{
	displays = g_slist_append (displays, display);
	return displays;
}

GSList *
mdm_daemon_config_display_list_insert (MdmDisplay *display)
{
        displays = g_slist_insert_sorted (displays,
                                          display,
                                          mdm_daemon_config_compare_displays);
	return displays;
}

GSList *
mdm_daemon_config_display_list_remove (MdmDisplay *display)
{
	displays = g_slist_remove (displays, display);

	return displays;
}

/**
 * mdm_daemon_config_get_value_int
 *
 * Gets an integer configuration option by key.  The option must
 * first be loaded, say, by calling mdm_config_parse.
 */
gint
mdm_daemon_config_get_value_int (const char *keystring)
{
	gboolean res;
	MdmConfigValue *value;
	char *group;
	char *key;
	int   result;

	result = 0;

	res = mdm_common_config_parse_key_string (keystring,
						  &group,
						  &key,
						  NULL,
						  NULL);
	if (! res) {
		mdm_error ("Could not parse configuration key %s", keystring);
		goto out;
	}

	res = mdm_config_get_value (daemon_config,
				    group,
				    key,
				    &value);
	if (! res) {
		mdm_error ("Request for invalid configuration key %s", keystring);
		goto out;
	}

	if (value->type != MDM_CONFIG_VALUE_INT) {
		mdm_error ("Request for configuration key %s, but not type INT", keystring);
		goto out;
	}

	result = mdm_config_value_get_int (value);
 out:
	g_free (group);
	g_free (key);

	return result;
}

/**
 * mdm_daemon_config_get_value_string
 *
 * Gets a string configuration option by key.  The option must
 * first be loaded, say, by calling mdm_daemon_config_parse.
 */
const char *
mdm_daemon_config_get_value_string (const char *keystring)
{
	gboolean res;
	MdmConfigValue *value;
	char *group;
	char *key;
	const char *result;

	result = NULL;

	res = mdm_common_config_parse_key_string (keystring,
						  &group,
						  &key,
						  NULL,
						  NULL);
	if (! res) {
		mdm_error ("Could not parse configuration key %s", keystring);
		goto out;
	}

	res = mdm_config_get_value (daemon_config,
				    group,
				    key,
				    &value);
	if (! res) {
		mdm_error ("Request for invalid configuration key %s", keystring);
		goto out;
	}

	if (value->type != MDM_CONFIG_VALUE_STRING) {
		mdm_error ("Request for configuration key %s, but not type STRING", keystring);
		goto out;
	}

	result = mdm_config_value_get_string (value);
 out:
	g_free (group);
	g_free (key);

	return result;
}

/**
 * mdm_daemon_config_get_value_string_array
 *
 * Gets a string configuration option by key.  The option must
 * first be loaded, say, by calling mdm_daemon_config_parse.
 */
const char **
mdm_daemon_config_get_value_string_array (const char *keystring)
{
	gboolean res;
	MdmConfigValue *value;
	char *group;
	char *key;
	const char **result;

	result = NULL;

	res = mdm_common_config_parse_key_string (keystring,
						  &group,
						  &key,
						  NULL,
						  NULL);
	if (! res) {
		mdm_error ("Could not parse configuration key %s", keystring);
		goto out;
	}

	res = mdm_config_get_value (daemon_config,
				    group,
				    key,
				    &value);
	if (! res) {
		mdm_error ("Request for invalid configuration key %s", keystring);
		goto out;
	}

	if (value->type != MDM_CONFIG_VALUE_STRING_ARRAY) {
		mdm_error ("Request for configuration key %s, but not type STRING-ARRAY", keystring);
		goto out;
	}

	result = mdm_config_value_get_string_array (value);
 out:
	g_free (group);
	g_free (key);

	return result;
}

/**
 * mdm_daemon_config_get_bool_for_id
 *
 * Gets a boolean configuration option by ID.  The option must
 * first be loaded, say, by calling mdm_daemon_config_parse.
 */
gboolean
mdm_daemon_config_get_bool_for_id (int id)
{
	gboolean val;

	val = FALSE;
	mdm_config_get_bool_for_id (daemon_config, id, &val);

	return val;
}

/**
 * mdm_daemon_config_get_int_for_id
 *
 * Gets a integer configuration option by ID.  The option must
 * first be loaded, say, by calling mdm_daemon_config_parse.
 */
int
mdm_daemon_config_get_int_for_id (int id)
{
	int val;

	val = -1;
	mdm_config_get_int_for_id (daemon_config, id, &val);

	return val;
}

/**
 * mdm_daemon_config_get_string_for_id
 *
 * Gets a string configuration option by ID.  The option must
 * first be loaded, say, by calling mdm_daemon_config_parse.
 */
const char *
mdm_daemon_config_get_string_for_id (int id)
{
	const char *val;

	val = NULL;
	mdm_config_peek_string_for_id (daemon_config, id, &val);

	return val;
}

/**
 * mdm_daemon_config_get_value_bool
 *
 * Gets a boolean configuration option by key.  The option must
 * first be loaded, say, by calling mdm_daemon_config_parse.
 */
gboolean
mdm_daemon_config_get_value_bool (const char *keystring)
{
	gboolean res;
	MdmConfigValue *value;
	char *group;
	char *key;
	gboolean result;

	result = FALSE;

	res = mdm_common_config_parse_key_string (keystring,
						  &group,
						  &key,
						  NULL,
						  NULL);
	if (! res) {
		mdm_error ("Could not parse configuration key %s", keystring);
		goto out;
	}

	res = mdm_config_get_value (daemon_config,
				    group,
				    key,
				    &value);
	if (! res) {
		mdm_error ("Request for invalid configuration key %s", keystring);
		goto out;
	}

	if (value->type != MDM_CONFIG_VALUE_BOOL) {
		mdm_error ("Request for configuration key %s, but not type BOOLEAN", keystring);
		goto out;
	}

	result = mdm_config_value_get_bool (value);
 out:
	g_free (group);
	g_free (key);

	return result;
}

/**
 * Note that some GUI configuration parameters are read by the daemon,
 * and in order for them to work, it is necessary for the daemon to 
 * access a few keys in a per-display fashion.  These access functions
 * allow the daemon to access these keys properly.
 */

/**
 * mdm_daemon_config_get_value_int_per_display
 *
 * Gets the per-display version  of the configuration, or the default
 * value if none exists.
 */
int
mdm_daemon_config_get_value_int_per_display (const char *key,
					     const char *display)
{
	char    *perdispval;
	gboolean res;

	res = mdm_daemon_config_key_to_string_per_display (key, display, &perdispval);

	if (res) {
		int val;
		val = atoi (perdispval);
		g_free (perdispval);
		return val;
	} else {
		return mdm_daemon_config_get_value_int (key);
	}
}

/**
 * mdm_daemon_config_get_value_bool_per_display
 *
 * Gets the per-display version  of the configuration, or the default
 * value if none exists.
 */
gboolean
mdm_daemon_config_get_value_bool_per_display (const char *key,
					      const char *display)
{
	char    *perdispval;
	gboolean res;

	res = mdm_daemon_config_key_to_string_per_display (key, display, &perdispval);

	if (res) {
		if (perdispval[0] == 'T' ||
		    perdispval[0] == 't' ||
		    perdispval[0] == 'Y' ||
		    perdispval[0] == 'y' ||
		    atoi (perdispval) != 0) {
			g_free (perdispval);
			return TRUE;
		} else {
			return FALSE;
		}
	} else {
		return mdm_daemon_config_get_value_bool (key);
	}
}

/**
 * mdm_daemon_config_get_value_string_per_display
 *
 * Gets the per-display version  of the configuration, or the default
 * value if none exists.  Note that this value needs to be freed,
 * unlike the non-per-display version.
 */
char *
mdm_daemon_config_get_value_string_per_display (const char *key,
						const char *display)
{
	char    *perdispval;
	gboolean res;

	res = mdm_daemon_config_key_to_string_per_display (key, display, &perdispval);

	if (res) {
		return perdispval;
	} else {
		return g_strdup (mdm_daemon_config_get_value_string (key));
	}
}

/**
 * mdm_daemon_config_key_to_string_per_display
 *
 * If the key makes sense to be per-display, return the value,
 * otherwise return NULL.  Keys that only apply to the daemon
 * process do not make sense for per-display configuration
 * Valid keys include any key in the greeter or gui categories,
 * and the MDM_KEY_PAM_STACK key.
 *
 * If additional keys make sense for per-display usage, make
 * sure they are added to the if-test below.
 */
gboolean
mdm_daemon_config_key_to_string_per_display (const char *keystring,
					     const char *display,
					     char      **retval)
{
	char    *file;
	char    *group;
	char    *key;
	gboolean res;
	gboolean ret;

	ret = FALSE;

	*retval = NULL;
	group = key = NULL;

	if (display == NULL) {
		goto out;
	}

	mdm_debug ("Looking up per display value for %s", keystring);

	res = mdm_common_config_parse_key_string (keystring,
						  &group,
						  &key,
						  NULL,
						  NULL);
	if (! res) {
		goto out;
	}

	file = mdm_daemon_config_get_per_display_custom_config_file (display);

	if (strcmp (group, "greeter") == 0 ||
	    strcmp (group, "gui") == 0 ||
	    is_key (keystring, MDM_KEY_PAM_STACK)) {
		ret = mdm_daemon_config_key_to_string (file, keystring, retval);
	}

	g_free (file);

 out:
	g_free (group);
	g_free (key);

	return ret;
}

/**
 * mdm_daemon_config_key_to_string
 *
 * Gets a specific key from the config file.
 * Note this returns the value in string form, so the caller needs
 * to parse it properly if it is a bool or int.
 *
 * Returns TRUE if successful..
 */
gboolean
mdm_daemon_config_key_to_string (const char *file,
				 const char *keystring,
				 char      **retval)
{
	GKeyFile             *config;
	MdmConfigValueType    type;
	gboolean              res;
	gboolean              ret;
	char                 *group;
	char                 *key;
	char                 *locale;
	char                 *result;
	const MdmConfigEntry *entry;

	if (retval != NULL) {
		*retval = NULL;
	}

	ret = FALSE;
	result = NULL;

	group = key = locale = NULL;
	res = mdm_common_config_parse_key_string (keystring,
						  &group,
						  &key,
						  &locale,
						  NULL);
	mdm_debug ("Requesting group=%s key=%s locale=%s", group, key, locale ? locale : "(null)");

	if (! res) {
		mdm_error ("Could not parse configuration key %s", keystring);
		goto out;
	}

	entry = mdm_config_lookup_entry (daemon_config, group, key);
	if (entry == NULL) {
		mdm_error ("Request for invalid configuration key %s", keystring);
		goto out;
	}
	type = entry->type;

	config = mdm_common_config_load (file, NULL);
	/* If file doesn't exist, then just return */
	if (config == NULL) {
		goto out;
	}

	mdm_debug ("Returning value for key <%s>\n", keystring);

	switch (type) {
	case MDM_CONFIG_VALUE_BOOL:
		{
			gboolean value;
			res = mdm_common_config_get_boolean (config, keystring, &value, NULL);
			if (res) {
				if (value) {
					result = g_strdup ("true");
				} else {
					result = g_strdup ("false");
				}
			}
		}
		break;
	case MDM_CONFIG_VALUE_INT:
		{
			int value;
			res = mdm_common_config_get_int (config, keystring, &value, NULL);
			if (res) {
				result = g_strdup_printf ("%d", value);
			}
		}
		break;
	case MDM_CONFIG_VALUE_STRING:
		{
			char *value;
			res = mdm_common_config_get_string (config, keystring, &value, NULL);
			if (res) {
				result = value;
			}
		}
		break;
	case MDM_CONFIG_VALUE_LOCALE_STRING:
		{
			char *value;
			res = mdm_common_config_get_string (config, keystring, &value, NULL);
			if (res) {
				result = value;
			}
		}
		break;
	default:
		break;
	}

	if (res) {
		if (retval != NULL) {
			*retval = g_strdup (result);
		}
		ret = TRUE;
	}

	g_key_file_free (config);
 out:
	g_free (result);
	g_free (group);
	g_free (key);
	g_free (locale);

	return ret;
}

/**
 * mdm_daemon_config_to_string
 *
 * Returns a configuration option as a string.  Used by MDM's
 * GET_CONFIG socket command.
 */
gboolean
mdm_daemon_config_to_string (const char *keystring,
			     const char *display,
			     char      **retval)
{
	gboolean res;
	gboolean ret;
	MdmConfigValue *value;
	char *group;
	char *key;
	char *locale;
	char *result;

	/*
	 * See if there is a per-display config file, returning that value
	 * if it exists.
	 */
	if (display != NULL) {
		res = mdm_daemon_config_key_to_string_per_display (keystring, display, retval);
		if (res) {
			mdm_debug ("Using per display value for key: %s", keystring);
			return TRUE;
		}
	}

	ret = FALSE;
	result = NULL;

	mdm_debug ("Looking up key: %s", keystring);

	group = NULL;
	key = NULL;
	locale = NULL;
	res = mdm_common_config_parse_key_string (keystring,
						  &group,
						  &key,
						  &locale,
						  NULL);
	if (! res) {
		mdm_error ("Could not parse configuration key %s", keystring);
		goto out;
	}

	if (group == NULL || key == NULL) {
		mdm_error ("Request for invalid configuration key %s", keystring);
		goto out;
	}

	/* Backward Compatibility */
	if ((strcmp (group, "daemon") == 0) &&
	    (strcmp (key, "PidFile") == 0)) {
		result = g_strdup (MDM_PID_FILE);
		goto out;
	} else if ((strcmp (group, "daemon") == 0) &&
		   (strcmp (key, "AlwaysRestartServer") == 0)) {
		result = g_strdup ("true");
	}

	res = mdm_config_get_value (daemon_config,
				    group,
				    key,
				    &value);

	if (! res) {
		mdm_error ("Request for invalid configuration key %s", keystring);
		goto out;
	}

	result = mdm_config_value_to_string (value);
	ret = TRUE;

 out:
	g_free (group);
	g_free (key);
	g_free (locale);


	*retval = result;

	return ret;
}

/**
 * mdm_daemon_config_compare_displays
 *
 * Support function for loading displays from the configuration
 * file
 */
int
mdm_daemon_config_compare_displays (gconstpointer a, gconstpointer b)
{
	const MdmDisplay *d1 = a;
	const MdmDisplay *d2 = b;
	if (d1->dispnum < d2->dispnum)
		return -1;
	else if (d1->dispnum > d2->dispnum)
		return 1;
	else
		return 0;
}

static char *
lookup_notify_key (MdmConfig  *config,
		   const char *group,
		   const char *key)
{
	char *nkey;
	char *keystring;

	keystring = g_strdup_printf ("%s/%s", group, key);

	/* pretty lame but oh well */
	nkey = NULL;

	/* bools */
	if (is_key (keystring, MDM_KEY_ALLOW_ROOT))
		nkey = g_strdup (MDM_NOTIFY_ALLOW_ROOT);
	else if (is_key (keystring, MDM_KEY_SYSTEM_MENU))
		nkey = g_strdup (MDM_NOTIFY_SYSTEM_MENU);
	else if (is_key (keystring, MDM_KEY_CONFIG_AVAILABLE))
		nkey = g_strdup (MDM_NOTIFY_CONFIG_AVAILABLE);	
	else if (is_key (keystring, MDM_KEY_DISALLOW_TCP))
		nkey = g_strdup (MDM_NOTIFY_DISALLOW_TCP);
	else if (is_key (keystring, MDM_KEY_ADD_GTK_MODULES))
		nkey = g_strdup (MDM_NOTIFY_ADD_GTK_MODULES);
	else if (is_key (keystring, MDM_KEY_TIMED_LOGIN_ENABLE))
		nkey = g_strdup (MDM_NOTIFY_TIMED_LOGIN_ENABLE);
	/* ints */
	else if (is_key (keystring, MDM_KEY_RETRY_DELAY))
		nkey = g_strdup (MDM_NOTIFY_RETRY_DELAY);
	else if (is_key (keystring, MDM_KEY_TIMED_LOGIN_DELAY))
		nkey = g_strdup (MDM_NOTIFY_TIMED_LOGIN_DELAY);
	/* strings */
	else if (is_key (keystring, MDM_KEY_GREETER))
		nkey = g_strdup (MDM_NOTIFY_GREETER);
	else if (is_key (keystring, MDM_KEY_SOUND_ON_LOGIN_FILE))
		nkey = g_strdup (MDM_NOTIFY_SOUND_ON_LOGIN_FILE);
	else if (is_key (keystring, MDM_KEY_SOUND_ON_LOGIN_SUCCESS_FILE))
		nkey = g_strdup (MDM_NOTIFY_SOUND_ON_LOGIN_SUCCESS_FILE);
	else if (is_key (keystring, MDM_KEY_SOUND_ON_LOGIN_FAILURE_FILE))
		nkey = g_strdup (MDM_NOTIFY_SOUND_ON_LOGIN_FAILURE_FILE);
	else if (is_key (keystring, MDM_KEY_GTK_MODULES_LIST))
		nkey = g_strdup (MDM_NOTIFY_GTK_MODULES_LIST);
	else if (is_key (keystring, MDM_KEY_TIMED_LOGIN))
		nkey = g_strdup (MDM_NOTIFY_TIMED_LOGIN);

	g_free (keystring);

	return nkey;
}

/**
 * notify_displays_value
 *
 * This will notify the slave programs
 * (mdmgreeter, mdmlogin, etc.) that a configuration option has
 * been changed so the slave can update with the new option
 * value.  MDM does this notify when it receives a
 * MDM_CONFIG_UPDATE socket command from mdmsetup or from the
 * mdmflexiserver --command option.
 */
static void
notify_displays_value (MdmConfig      *config,
		       const char     *group,
		       const char     *key,
		       MdmConfigValue *value)
{
	GSList *li;
	char   *valstr;
	char   *keystr;

	keystr = lookup_notify_key (config, group, key);

	/* unfortunately, can't always mdm_config_value_to_string()
	 * here because booleans need to be sent as ints
	 */
	switch (value->type) {
	case MDM_CONFIG_VALUE_BOOL:
		if (mdm_config_value_get_bool (value)) {
			valstr = g_strdup ("1");
		} else {
			valstr = g_strdup ("0");
		}
		break;
	default:
		valstr = mdm_config_value_to_string (value);
		break;
	}

	if (valstr == NULL) {
		valstr = g_strdup (" ");
	}

	for (li = displays; li != NULL; li = li->next) {
		MdmDisplay *disp = li->data;

		if (disp->master_notify_fd < 0) {
			/* no point */
			continue;
		}

		mdm_fdprintf (disp->master_notify_fd,
			      "%c%s %s\n",
			      MDM_SLAVE_NOTIFY_KEY,
			      keystr,
			      valstr);

		if (disp != NULL && disp->slavepid > 1) {
			kill (disp->slavepid, SIGUSR2);
		}
	}

	g_free (keystr);
	g_free (valstr);
}

/* The following were used to internally set the
 * stored configuration values.  Now we'll just
 * ask the MdmConfig to store the entry. */
void
mdm_daemon_config_set_value_string (const gchar *keystring,
				    const gchar *value_in)
{
	char           *group;
	char           *key;
	gboolean        res;
	MdmConfigValue *value;

	res = mdm_common_config_parse_key_string (keystring, &group, &key, NULL, NULL);
	if (! res) {
		mdm_error ("Could not parse configuration key %s", keystring);
		return;
	}

	value = mdm_config_value_new (MDM_CONFIG_VALUE_STRING);
	mdm_config_value_set_string (value, value_in);

	res = mdm_config_set_value (daemon_config, group, key, value);

	mdm_config_value_free (value);
	g_free (group);
	g_free (key);
}

void
mdm_daemon_config_set_value_bool (const gchar *keystring,
				  gboolean     value_in)
{
	char           *group;
	char           *key;
	gboolean        res;
	MdmConfigValue *value;

	res = mdm_common_config_parse_key_string (keystring, &group, &key, NULL, NULL);
	if (! res) {
		mdm_error ("Could not parse configuration key %s", keystring);
		return;
	}

	value = mdm_config_value_new (MDM_CONFIG_VALUE_BOOL);
	mdm_config_value_set_bool (value, value_in);

	res = mdm_config_set_value (daemon_config, group, key, value);

	mdm_config_value_free (value);
	g_free (group);
	g_free (key);
}

void
mdm_daemon_config_set_value_int (const gchar *keystring,
				 gint         value_in)
{
	char           *group;
	char           *key;
	gboolean        res;
	MdmConfigValue *value;

	res = mdm_common_config_parse_key_string (keystring, &group, &key, NULL, NULL);
	if (! res) {
		mdm_error ("Could not parse configuration key %s", keystring);
		return;
	}

	value = mdm_config_value_new (MDM_CONFIG_VALUE_INT);
	mdm_config_value_set_int (value, value_in);

	res = mdm_config_set_value (daemon_config, group, key, value);

	mdm_config_value_free (value);
	g_free (group);
	g_free (key);
}

/**
 * mdm_daemon_config_find_xserver
 *
 * Return an xserver with a given ID, or NULL if not found.
 */
MdmXserver *
mdm_daemon_config_find_xserver (const gchar *id)
{
	GSList *li;

	if (xservers == NULL)
		return NULL;

	if (id == NULL)
		return xservers->data;

	for (li = xservers; li != NULL; li = li->next) {
		MdmXserver *svr = li->data;
		if (strcmp (ve_sure_string (svr->id), ve_sure_string (id)) == 0)
			return svr;
	}

	return NULL;
}

/**
 * mdm_daemon_config_get_xservers
 *
 * Prepare a string to be returned for the GET_SERVER_LIST
 * sockets command.
 */
gchar *
mdm_daemon_config_get_xservers (void)
{
	GSList *li;
	gchar *retval = NULL;

	if (xservers == NULL)
		return NULL;

	for (li = xservers; li != NULL; li = li->next) {
		MdmXserver *svr = li->data;
		if (retval != NULL)
			retval = g_strconcat (retval, ";", svr->id, NULL);
		else
			retval = g_strdup (svr->id);
	}

	return retval;
}

#define MDM_PRIO_MIN PRIO_MIN
#define MDM_PRIO_MAX PRIO_MAX
#define MDM_PRIO_DEFAULT 0

/**
 * mdm_daemon_config_load_xserver
 *
 * Load [server-foo] sections from a configuration file.
 */
static void
mdm_daemon_config_load_xserver (MdmConfig  *config,
				const char *group,
				const char *name)
{
	MdmXserver     *svr;
	int             n;
	gboolean        res;
	MdmConfigValue *value;

	/* Do not add xserver if name doesn't exist */
	if (mdm_daemon_config_find_xserver (name) != NULL) {
		return;
	}

	svr = g_new0 (MdmXserver, 1);
	svr->id = g_strdup (name);

	/* string */
	res = mdm_config_get_value (config, group, "name", &value);
	if (res) {
		svr->name = g_strdup (mdm_config_value_get_string (value));
	}
	res = mdm_config_get_value (config, group, "command", &value);
	if (res) {
		svr->command = g_strdup (mdm_config_value_get_string (value));
	}

	/* bool */
	res = mdm_config_get_value (config, group, "flexible", &value);
	if (res) {
		svr->flexible = mdm_config_value_get_bool (value);
	}
	res = mdm_config_get_value (config, group, "choosable", &value);
	if (res) {
		svr->choosable = mdm_config_value_get_bool (value);
	}
	res = mdm_config_get_value (config, group, "handled", &value);
	if (res) {
		svr->handled = mdm_config_value_get_bool (value);
	}	

	/* int */
	res = mdm_config_get_value (config, group, "priority", &value);
	if (res) {
		svr->priority = mdm_config_value_get_int (value);
	}

	/* do some bounds checking */
	n = svr->priority;
	if (n < MDM_PRIO_MIN)
		n = MDM_PRIO_MIN;
	else if (n > MDM_PRIO_MAX)
		n = MDM_PRIO_MAX;

	if (n != svr->priority) {
		mdm_error ("mdm_config_parse: Priority out of bounds; changed to %d", n);
		svr->priority = n;
	}

	if (ve_string_empty (svr->command)) {
		mdm_error ("mdm_config_parse: Empty server command; using standard command.");
		g_free (svr->command);
		svr->command = g_strdup (X_SERVER);
	}

	xservers = g_slist_append (xservers, svr);
}

static void
mdm_daemon_config_unload_xservers (MdmConfig *config)
{
	GSList *xli;

	/* Free list if already loaded */
	for (xli = xservers; xli != NULL; xli = xli->next) {
		MdmXserver *xsvr = xli->data;

		g_free (xsvr->id);
		g_free (xsvr->name);
		g_free (xsvr->command);
	}

	if (xservers != NULL) {
		g_slist_free (xservers);
		xservers = NULL;
	}
}

static void
mdm_daemon_config_ensure_one_xserver (MdmConfig *config)
{
	/* If no "Standard" server was created, then add it */
	if (xservers == NULL || mdm_daemon_config_find_xserver (MDM_STANDARD) == NULL) {
		MdmXserver *svr = g_new0 (MdmXserver, 1);

		svr->id        = g_strdup (MDM_STANDARD);
		svr->name      = g_strdup ("Standard server");
		svr->command   = g_strdup (X_SERVER);
		svr->flexible  = TRUE;
		svr->choosable = TRUE;
		svr->handled   = TRUE;
		svr->priority  = MDM_PRIO_DEFAULT;

		xservers       = g_slist_append (xservers, svr);
	}
}

static void
load_xservers_group (MdmConfig *config)
{
	GPtrArray  *server_groups;
	char      **vname_array;
	char       *xserver_group;
	int         i, j;

	server_groups = mdm_config_get_server_groups (config);

	for (i=0; i < server_groups->len; i++) {
		xserver_group = g_ptr_array_index (server_groups, i);
		mdm_debug ("Processing server group <%s>", xserver_group);

		if (g_str_has_prefix (xserver_group, "server-")) {
			char * xserver_name;

			for (j = 0; j < G_N_ELEMENTS (mdm_daemon_server_config_entries); j++) {
				MdmConfigEntry *srv_entry;
				if (mdm_daemon_server_config_entries[j].key == NULL) {
					continue;
				}
				srv_entry = mdm_config_entry_copy (&mdm_daemon_server_config_entries[j]);
				g_free (srv_entry->group);
				srv_entry->group = g_strdup (xserver_group);
				mdm_config_process_entry (config, srv_entry, NULL);
				mdm_config_entry_free (srv_entry);
			}

			/* Strip "server-" prefix from name */
			xserver_name = xserver_group + strlen ("server-");

			/* Now we can add this server */
			if (xserver_name != NULL)
				mdm_daemon_config_load_xserver (config, xserver_group, xserver_name);
		}
        }

	g_ptr_array_free (server_groups, TRUE);
}

static void
mdm_daemon_config_load_xservers (MdmConfig *config)
{
	mdm_daemon_config_unload_xservers (config);
	load_xservers_group (config);
	mdm_daemon_config_ensure_one_xserver (config);
}

/**
 * check_logdir
 * check_servauthdir
 *
 * Support functions for mdm_config_parse.
 */
static void
check_logdir (void)
{
        struct stat     statbuf;
        int             r;
	char           *log_path;
	const char     *auth_path;
	MdmConfigValue *value;

	log_path = NULL;
	auth_path = NULL;

	mdm_config_get_string_for_id (daemon_config, MDM_ID_LOG_DIR, &log_path);

	mdm_config_get_value_for_id (daemon_config, MDM_ID_SERV_AUTHDIR, &value);
	auth_path = mdm_config_value_get_string (value);

        VE_IGNORE_EINTR (r = g_stat (log_path, &statbuf));
        if (r < 0 || ! S_ISDIR (statbuf.st_mode))  {
                mdm_error ("mdm_config_parse: Logdir %s does not exist or isn't a directory.  Using ServAuthDir %s.", log_path, auth_path);
		mdm_config_set_value_for_id (daemon_config, MDM_ID_LOG_DIR, value);
        }

	g_free (log_path);
	mdm_config_value_free (value);
}

static void
check_servauthdir (const char  *auth_path,
		   struct stat *statbuf)
{
	int        r;
	gboolean   console_notify;

	console_notify = FALSE;
	mdm_config_get_bool_for_id (daemon_config, MDM_ID_CONSOLE_NOTIFY, &console_notify);

	/* Enter paranoia mode */
	VE_IGNORE_EINTR (r = g_stat (auth_path, statbuf));
	if G_UNLIKELY (r < 0) {
		if (console_notify) {
			gchar *s = g_strdup_printf (C_(N_("Server Authorization directory "
							  "(daemon/ServAuthDir) is set to %s "
							  "but this does not exist. Please "
							  "correct MDM configuration and "
							  "restart MDM.")),
						    auth_path);

			mdm_text_message_dialog (s);
			g_free (s);
		}

		mdm_fail ("mdm_config_parse: Authdir %s does not exist. Aborting.", auth_path);
	}

	if G_UNLIKELY (! S_ISDIR (statbuf->st_mode)) {
		if (console_notify) {
			gchar *s = g_strdup_printf (C_(N_("Server Authorization directory "
							  "(daemon/ServAuthDir) is set to %s "
							  "but this is not a directory. Please "
							  "correct MDM configuration and "
							  "restart MDM.")),
						    auth_path);

			mdm_text_message_dialog (s);
			g_free (s);
		}

		mdm_fail ("mdm_config_parse: Authdir %s is not a directory. Aborting.", auth_path);
	}
}

static void
mdm_daemon_config_load_displays (MdmConfig *config)
{
	char       **keys;
	const char *display_value;
	gsize      len;
	int        i,j;

	keys = mdm_config_get_keys_for_group (config,
		MDM_CONFIG_GROUP_SERVERS, &len, NULL);

	for (i = 0; i < len; i++) {
		MdmConfigEntry   entry;
		MdmConfigValue  *value;
		const char      *display_value;
		const char      *name;
		char           **value_list;
		char            *new_group;
		int              j;
		gboolean         res;

		entry.group         = MDM_CONFIG_GROUP_SERVERS;
		entry.key           = keys[i];
		entry.type          = MDM_CONFIG_VALUE_STRING;
		entry.default_value = NULL;
		entry.id            = MDM_CONFIG_INVALID_ID;

		mdm_config_add_entry (config, &entry);
		mdm_config_process_entry (config, &entry, NULL);
	}

	/* Now construct entries for these groups */
	for (i = 0; i < len; i++) {
		char           **value_list;
		GString         *command     = NULL;
		MdmDisplay      *disp;
		MdmConfigValue  *value;
		const char      *name        = NULL;
		const char      *device_name = NULL;
		int              keynum;
		gboolean         res;

		name   = keys[i];

		if (!isdigit (*name)) {
			continue;
		}

		keynum = atoi (name);

		res = mdm_config_get_value (config, MDM_CONFIG_GROUP_SERVERS,
			keys[i], &value);
		if (! res) {
			continue;
		}

		display_value = mdm_config_value_get_string (value);

		/* Skip displays marked as inactive */
		if (g_ascii_strcasecmp (display_value, "inactive") == 0)
			continue;

		value_list = g_strsplit (display_value, " ", -1);

		if (value_list == NULL || value_list[0] == '\0') {
			mdm_config_value_free (value);
			g_strfreev (value_list);
			continue;
		}

		command = g_string_new (NULL);

		/*
		 * Allow an optional device to be passed in as an argument
		 * with the format "device=/dev/foo".
		 * In the future, if more per-display configuration is desired, 
		 * this can be made more sophisticated to handle additional
		 * arguments.
		 */
		j=0;
		while (value_list[j] != NULL) {
			if (strncmp (value_list[j], "device=",
				     strlen ("device=")) == 0) {
			        device_name = value_list[j] + strlen ("device=");
			} else {
				g_string_append (command, value_list[j]);
				g_string_append (command, " ");
			}
			j++;
		}

		mdm_debug ("Loading display for key '%d'", keynum);

		disp = mdm_display_alloc (keynum, command->str, device_name);
		g_string_free (command, TRUE);
		if (disp == NULL) {
			g_strfreev (value_list);
			continue;
		}

		displays = g_slist_insert_sorted (displays, disp,
			mdm_daemon_config_compare_displays);
		if (keynum > high_display_num) {
			high_display_num = keynum;
		}

		g_strfreev (value_list);
	}

	g_free (keys);
}

static gboolean
validate_path (MdmConfig          *config,
	       MdmConfigSourceType source,
	       MdmConfigValue     *value)
{
	char    *str;

	/* If the /etc/default has a PATH use that */
	str = mdm_read_default ("PATH=");
	if (str != NULL) {
		mdm_config_value_set_string (value, str);
		g_free (str);
	}

	return TRUE;
}

static gboolean
validate_root_path (MdmConfig          *config,
		    MdmConfigSourceType source,
		    MdmConfigValue     *value)
{
	char    *str;

	/* If the /etc/default has a PATH use that */
	str = mdm_read_default ("SUPATH=");
	if (str != NULL) {
		mdm_config_value_set_string (value, str);
		g_free (str);
	}

	return TRUE;
}

static gboolean
validate_base_xsession (MdmConfig          *config,
			MdmConfigSourceType source,
			MdmConfigValue     *value)
{
	const char *str;

	str = mdm_config_value_get_string (value);
	if (str == NULL || str[0] == '\0') {
		char *path;
		path = g_build_filename (MDMCONFDIR, "mdm", "Xsession", NULL);
		mdm_info ("mdm_config_parse: BaseXsession empty; using %s", path);
		mdm_config_value_set_string (value, path);
		g_free (path);
	}

	return TRUE;
}

static gboolean
validate_power_action (MdmConfig          *config,
		       MdmConfigSourceType source,
		       MdmConfigValue     *value)
{
	/* FIXME: should weed out the commands that don't work */

	return TRUE;
}

static gboolean
validate_standard_xserver (MdmConfig          *config,
			   MdmConfigSourceType source,
			   MdmConfigValue     *value)
{
	gboolean    res;
	gboolean    is_ok;
	const char *str;
	char       *new;

	is_ok = FALSE;
	new = NULL;
	str = mdm_config_value_get_string (value);

	if (str != NULL) {
		char **argv;

		if (g_shell_parse_argv (str, NULL, &argv, NULL)) {
			if (g_access (argv[0], X_OK) == 0) {
				is_ok = TRUE;
			}
			g_strfreev (argv);
		}
	}

	if G_UNLIKELY (! is_ok) {
		mdm_info ("mdm_config_parse: Standard X server not found; trying alternatives");
		if (g_access ("/usr/X11R6/bin/X", X_OK) == 0) {
			new = g_strdup ("/usr/X11R6/bin/X");
		} else if (g_access ("/opt/X11R6/bin/X", X_OK) == 0) {
			new = g_strdup ("/opt/X11R6/bin/X");
		} else if (g_access ("/usr/bin/X11/X", X_OK) == 0) {
			new = g_strdup ("/usr/bin/X11/X");
		}
	}

	if (new != NULL) {
		mdm_config_value_set_string (value, new);
		g_free (new);
	}

	res = TRUE;

	return res;
}

static gboolean
validate_graphical_theme_dir (MdmConfig          *config,
			      MdmConfigSourceType source,
			      MdmConfigValue     *value)
{
	const char *str;

	str = mdm_config_value_get_string (value);

	if (str == NULL || !g_file_test (str, G_FILE_TEST_IS_DIR)) {
		mdm_config_value_set_string (value, GREETERTHEMEDIR);
	}

	return TRUE;
}

static gboolean
validate_graphical_theme (MdmConfig          *config,
			  MdmConfigSourceType source,
			  MdmConfigValue     *value)
{
	const char *str;

	str = mdm_config_value_get_string (value);

	if (str == NULL || str[0] == '\0') {
		mdm_config_value_set_string (value, "circles");
	}

	return TRUE;
}

static gboolean
validate_greeter (MdmConfig          *config,
		  MdmConfigSourceType source,
		  MdmConfigValue     *value)
{
	const char *str;

	str = mdm_config_value_get_string (value);

	if (str == NULL || str[0] == '\0') {
		mdm_error ("mdm_config_parse: No greeter specified.");
	}

	return TRUE;
}

static gboolean
validate_session_desktop_dir (MdmConfig          *config,
			      MdmConfigSourceType source,
			      MdmConfigValue     *value)
{
	const char *str;

	str = mdm_config_value_get_string (value);

	if (str == NULL || str[0] == '\0') {
		mdm_error ("mdm_config_parse: No sessions directory specified.");
	}

	return TRUE;
}

static gboolean
validate_password_required (MdmConfig          *config,
			    MdmConfigSourceType source,
			    MdmConfigValue     *value)
{
	char *str;

	str = mdm_read_default ("PASSREQ=");
	if (str != NULL && str[0] == '\0') {
		gboolean val;
		val = (g_ascii_strcasecmp (str, "YES") == 0);
		mdm_config_value_set_bool (value, val);
	}

	return TRUE;
}

/* Cause debug to affect logging as soon as the config value is read */
static gboolean
validate_debug (MdmConfig          *config,
		MdmConfigSourceType source,
		MdmConfigValue     *value)
{
	gboolean debugval;

	debugval = mdm_config_value_get_bool (value);
	mdm_log_set_debug (debugval);

	return TRUE;
}

static gboolean
validate_at_least_int (MdmConfig          *config,
		       MdmConfigSourceType source,
		       MdmConfigValue     *value,
		       int                 minval,
		       int                 defval)
{
	if (mdm_config_value_get_int (value) < minval) {
		mdm_config_value_set_int (value, defval);
	}

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
	gboolean res;

	res = TRUE;

        switch (id) {
        case MDM_ID_DEBUG:
		res = validate_debug (config, source, value);
		break;
        case MDM_ID_PATH:
		res = validate_path (config, source, value);
		break;
        case MDM_ID_ROOT_PATH:
		res = validate_root_path (config, source, value);
		break;
        case MDM_ID_BASE_XSESSION:
		res = validate_base_xsession (config, source, value);
		break;
        case MDM_ID_HALT:
        case MDM_ID_REBOOT:
        case MDM_ID_SUSPEND:
		res = validate_power_action (config, source, value);
		break;
        case MDM_ID_STANDARD_XSERVER:
		res = validate_standard_xserver (config, source, value);
		break;
        case MDM_ID_GRAPHICAL_THEME_DIR:
		res = validate_graphical_theme_dir (config, source, value);
		break;
        case MDM_ID_GRAPHICAL_THEME:
		res = validate_graphical_theme (config, source, value);
		break;
        case MDM_ID_GREETER:
		res = validate_greeter (config, source, value);
		break;
        case MDM_ID_SESSION_DESKTOP_DIR:
		res = validate_session_desktop_dir (config, source, value);
		break;
        case MDM_ID_PASSWORD_REQUIRED:
		res = validate_password_required (config, source, value);
		break;
	case MDM_ID_TIMED_LOGIN_DELAY:
		res = validate_at_least_int (config, source, value, 5, 5);
		break;
	case MDM_ID_MAX_ICON_WIDTH:
	case MDM_ID_MAX_ICON_HEIGHT:
		res = validate_at_least_int (config, source, value, 0, 128);
		break;
	case MDM_ID_SCAN_TIME:
		res = validate_at_least_int (config, source, value, 1, 1);
		break;
        case MDM_ID_NONE:
        case MDM_CONFIG_INVALID_ID:
		break;
	default:
		break;
	}

	return res;
}

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

static gboolean
notify_cb (MdmConfig          *config,
	   MdmConfigSourceType source,
	   const char         *group,
	   const char         *key,
	   MdmConfigValue     *value,
	   int                 id,
	   gpointer            data)
{
	char *valstr;

        switch (id) {
        case MDM_ID_GREETER:
        case MDM_ID_SOUND_ON_LOGIN_FILE:
        case MDM_ID_SOUND_ON_LOGIN_SUCCESS_FILE:
        case MDM_ID_SOUND_ON_LOGIN_FAILURE_FILE:
        case MDM_ID_GTK_MODULES_LIST:
        case MDM_ID_TIMED_LOGIN:
        case MDM_ID_ALLOW_ROOT:
        case MDM_ID_SYSTEM_MENU:
        case MDM_ID_CONFIG_AVAILABLE:
        case MDM_ID_DISALLOW_TCP:
        case MDM_ID_ADD_GTK_MODULES:
        case MDM_ID_TIMED_LOGIN_ENABLE:
	case MDM_ID_RETRY_DELAY:
	case MDM_ID_TIMED_LOGIN_DELAY:
		notify_displays_value (config, group, key, value);
		break;
        case MDM_ID_NONE:
        case MDM_CONFIG_INVALID_ID:
		{
			/* doesn't have an ID : match group/key */
			if (group != NULL) {
				if (strcmp (group, MDM_CONFIG_GROUP_SERVERS) == 0) {
					/* FIXME: handle this? */
				} 
			}
		}
                break;
	default:
		break;
        }

	valstr = mdm_config_value_to_string (value);
	mdm_debug ("Got config %s/%s=%s <%s>\n",
		   group,
		   key,
		   valstr,
		   source_to_name (source));
	g_free (valstr);

        return TRUE;
}

static void
handle_no_displays (MdmConfig *config,
		    gboolean   no_console)
{
	const char *server;
	gboolean    console_notify;

	console_notify = FALSE;
	mdm_config_get_bool_for_id (daemon_config, MDM_ID_CONSOLE_NOTIFY, &console_notify);

	/*
	 * If we requested no static servers (there is no console),
	 * then don't display errors in console messages
	 */
	if (no_console) {
		mdm_fail ("mdm_config_parse: No static servers defined. Aborting!");
	}

	server = X_SERVER;
	if G_LIKELY (g_access (server, X_OK) == 0) {
	} else if (g_access ("/usr/bin/X11/X", X_OK) == 0) {
		server = "/usr/bin/X11/X";
	} else if (g_access ("/usr/X11R6/bin/X", X_OK) == 0) {
		server = "/usr/X11R6/bin/X";
	} else if (g_access ("/opt/X11R6/bin/X", X_OK) == 0) {
		server = "/opt/X11R6/bin/X";
	}

	/* yay, we can add a backup emergency server */
	if (server != NULL) {
		MdmDisplay *d;

		int num = mdm_get_free_display (0 /* start */, 0 /* server uid */);

		mdm_error ("mdm_config_parse: No static servers defined. Adding %s on :%d to allow configuration!", server, num);

		d = mdm_display_alloc (num, server, NULL);
		d->is_emergency_server = TRUE;

		displays = g_slist_append (displays, d);

		/* ALWAYS run the greeter and don't log anyone in,
		 * this is just an emergency session */
		mdm_config_set_string_for_id (daemon_config, MDM_ID_AUTOMATIC_LOGIN, NULL);
		mdm_config_set_string_for_id (daemon_config, MDM_ID_TIMED_LOGIN, NULL);

	} else {
		if (console_notify) {
			gchar *s = g_strdup_printf (C_(N_("MDM "
							  "cannot find any static server "
							  "to start.  Aborting!  Please "
							  "correct the configuration "
							  "and restart MDM.")));
			mdm_text_message_dialog (s);
			g_free (s);
		}

		mdm_fail ("mdm_config_parse: No static servers defined. Aborting!");
	}
}

static void
mdm_daemon_change_user (MdmConfig *config,
			uid_t     *uidp,
			gid_t     *gidp)
{
	gboolean    console_notify;
	char       *username;
	char       *groupname;
	uid_t       uid;
	gid_t       gid;
	struct passwd *pwent;
	struct group  *grent;

	console_notify = FALSE;
	username = NULL;
	groupname = NULL;
	uid = 0;
	gid = 0;

	mdm_config_get_bool_for_id (daemon_config, MDM_ID_CONSOLE_NOTIFY, &console_notify);
	mdm_config_get_string_for_id (daemon_config, MDM_ID_USER, &username);
	mdm_config_get_string_for_id (daemon_config, MDM_ID_GROUP, &groupname);

	/* Lookup user and groupid for the MDM user */
	pwent = getpwnam (username);

	/* Set uid and gid */
	if G_UNLIKELY (pwent == NULL) {

		if (console_notify) {
			gchar *s = g_strdup_printf (C_(N_("The MDM user '%s' does not exist. "
							  "Please correct MDM configuration "
							  "and restart MDM.")),
						    username);
			mdm_text_message_dialog (s);
			g_free (s);
		}

		mdm_fail ("mdm_config_parse: Can't find the MDM user '%s'. Aborting!", username);
	} else {
		uid = pwent->pw_uid;
	}

	if G_UNLIKELY (uid == 0) {
		if (console_notify) {
			gchar *s = g_strdup_printf (C_(N_("The MDM user is set to be root, but "
							  "this is not allowed since it can "
							  "pose a security risk.  Please "
							  "correct MDM configuration and "
							  "restart MDM.")));

			mdm_text_message_dialog (s);
			g_free (s);
		}

		mdm_fail ("mdm_config_parse: The MDM user should not be root. Aborting!");
	}

	grent = getgrnam (groupname);

	if G_UNLIKELY (grent == NULL) {
		if (console_notify) {
			gchar *s = g_strdup_printf (C_(N_("The MDM group '%s' does not exist. "
							  "Please correct MDM configuration "
							  "and restart MDM.")),
						    groupname);
			mdm_text_message_dialog (s);
			g_free (s);
		}

		mdm_fail ("mdm_config_parse: Can't find the MDM group '%s'. Aborting!", groupname);
	} else  {
		gid = grent->gr_gid;
	}

	if G_UNLIKELY (gid == 0) {
		if (console_notify) {
			gchar *s = g_strdup_printf (C_(N_("The MDM group is set to be root, but "
							  "this is not allowed since it can "
							  "pose a security risk. Please "
							  "correct MDM configuration and "
							  "restart MDM.")));
			mdm_text_message_dialog (s);
			g_free (s);
		}

		mdm_fail ("mdm_config_parse: The MDM group should not be root. Aborting!");
	}

	/* gid remains `mdm' */
	NEVER_FAILS_root_set_euid_egid (uid, gid);

	if (uidp != NULL) {
		*uidp = uid;
	}

	if (gidp != NULL) {
		*gidp = gid;
	}

	g_free (username);
	g_free (groupname);
}

static void
mdm_daemon_check_permissions (MdmConfig *config,
			      uid_t      uid,
			      gid_t      gid)
{
	struct stat statbuf;
	char       *auth_path;
	gboolean    console_notify;

	console_notify = FALSE;
	mdm_config_get_bool_for_id (daemon_config, MDM_ID_CONSOLE_NOTIFY, &console_notify);
	auth_path = NULL;
	mdm_config_get_string_for_id (config, MDM_ID_SERV_AUTHDIR, &auth_path);

	/* Enter paranoia mode */
	check_servauthdir (auth_path, &statbuf);

	NEVER_FAILS_root_set_euid_egid (0, 0);

	/* Now set things up for us as  */
	chown (auth_path, 0, gid);
	g_chmod (auth_path, (S_IRWXU|S_IRWXG|S_ISVTX));

	NEVER_FAILS_root_set_euid_egid (uid, gid);

	/* Again paranoid */
	check_servauthdir (auth_path, &statbuf);

	if G_UNLIKELY (statbuf.st_uid != 0 || statbuf.st_gid != gid)  {
		if (console_notify) {
			gchar *s = g_strdup_printf (C_(N_("Server Authorization directory "
							  "(daemon/ServAuthDir) is set to %s "
							  "but is not owned by user %d and group "
							  "%d. Please correct the ownership or "
							  "MDM configuration and restart "
							  "MDM.")),
						    auth_path,
						    (int)uid,
						    (int)gid);
			mdm_text_message_dialog (s);
			g_free (s);
		}

		mdm_fail ("mdm_config_parse: Authdir %s is not owned by user %d, group %d. Aborting.", auth_path, (int)uid, (int)gid);
	}

	if G_UNLIKELY (statbuf.st_mode != (S_IFDIR|S_IRWXU|S_IRWXG|S_ISVTX))  {
		if (console_notify) {
			gchar *s = g_strdup_printf (C_(N_("Server Authorization directory "
							  "(daemon/ServAuthDir) is set to %s "
							  "but has the wrong permissions: it "
							  "should have permissions of %o. "
							  "Please correct the permissions or "
							  "the MDM configuration and "
							  "restart MDM.")),
						    auth_path,
						    (S_IRWXU|S_IRWXG|S_ISVTX));
			mdm_text_message_dialog (s);
			g_free (s);
		}

		mdm_fail ("mdm_config_parse: Authdir %s has wrong permissions %o. Should be %o. Aborting.", auth_path, statbuf.st_mode, (S_IRWXU|S_IRWXG|S_ISVTX));
	}

	g_free (auth_path);
}

void
mdm_daemon_load_config_file (MdmConfig **load_config)
{
	GError       *error;

	if (*load_config == NULL)
		*load_config = mdm_config_new ();

	mdm_config_set_validate_func (*load_config, validate_cb, NULL);
	mdm_config_add_static_entries (*load_config, mdm_daemon_config_entries);
	mdm_config_set_default_file (*load_config, default_config_file);
	mdm_config_set_distro_file (*load_config, "/usr/share/mdm/distro.conf");
	mdm_config_set_custom_file (*load_config, custom_config_file);

	/* load the data files */
	error = NULL;
	mdm_config_load (*load_config, &error);
	if (error != NULL) {
		mdm_error ("Unable to load configuration: %s", error->message);
		g_error_free (error);
	}

	/* populate the database with all specified entries */
	mdm_config_process_all (*load_config, &error);
}

/**
 * mdm_daemon_config_update_key
 *
 * Will cause a the MDM daemon to re-read the key from the configuration
 * file and cause notify signal to be sent to the slaves for the
 * specified key, if appropriate.
 * Obviously notification is not needed for configuration options only
 * used by the daemon.  This function is called when the UPDDATE_CONFIG
 * sockets command is called.
 *
 * To add a new notification, a MDM_NOTIFY_* argument will need to be
 * defined in mdm-daemon-config-keys.h, supporting logic placed in the
 * notify_cb function and in the mdm_slave_handle_notify function
 * in slave.c.
 */
gboolean
mdm_daemon_config_update_key (const char *keystring)
{
	const MdmConfigEntry *entry;
	MdmConfigValue       *value;
        MdmConfig            *temp_config;
	gboolean              rc;
	gboolean              res;
	char                 *group;
	char                 *key;
	char                 *locale;

	rc = FALSE;
	group = key = locale = NULL;
	temp_config = NULL;

	/*
	 * Do not allow these keys to be updated, since MDM would need
	 * additional work, or at least heavy testing, to make these keys
	 * flexible enough to be changed at runtime.
	 */
	if (is_key (keystring, MDM_KEY_PID_FILE) ||
	    is_key (keystring, MDM_KEY_CONSOLE_NOTIFY) ||
	    is_key (keystring, MDM_KEY_USER) ||
	    is_key (keystring, MDM_KEY_GROUP) ||
	    is_key (keystring, MDM_KEY_LOG_DIR) ||
	    is_key (keystring, MDM_KEY_SERV_AUTHDIR) ||
	    is_key (keystring, MDM_KEY_USER_AUTHDIR) ||
	    is_key (keystring, MDM_KEY_USER_AUTHFILE) ||
	    is_key (keystring, MDM_KEY_USER_AUTHDIR_FALLBACK)) {
		return FALSE;
	}

	/* Load configuration file */
	mdm_daemon_load_config_file (&temp_config);

	if (is_key (keystring, "xservers/PARAMETERS")) {
		mdm_daemon_config_load_xservers (temp_config);
		goto out;
	}
	
	/* find the entry for the key */
	res = mdm_common_config_parse_key_string (keystring,
						  &group,
						  &key,
						  &locale,
						  NULL);
	if (! res) {
		mdm_error ("Could not parse configuration key %s", keystring);
		goto out;
	}

	entry = mdm_config_lookup_entry (temp_config, group, key);
	if (entry == NULL) {
		mdm_error ("Request for invalid configuration key %s", keystring);
		goto out;
	}

	rc = mdm_config_process_entry (temp_config, entry, NULL);

	mdm_config_get_value_for_id (temp_config, entry->id, &value);
	mdm_config_set_value_for_id (daemon_config, entry->id, value);

 out:
	if (temp_config != NULL)
		mdm_config_free (temp_config);
	g_free (group);
	g_free (key);
	g_free (locale);

	return rc;
}

/**
 * mdm_daemon_config_parse
 *
 * Loads initial configuration settings.
 */
void
mdm_daemon_config_parse (const char *config_file,
			 gboolean    no_console)
{
	uid_t         uid;
	gid_t         gid;

	displays            = NULL;
	high_display_num    = 0;

	/* Not NULL if config_file was set by command-line option. */
	if (config_file == NULL) {
		config_file = MDM_DEFAULTS_CONF;
	}

	default_config_file = config_file;
	custom_config_file  = g_strdup (MDM_CUSTOM_CONF);

	mdm_daemon_load_config_file (&daemon_config);
	mdm_config_set_notify_func (daemon_config, notify_cb, NULL);
	mdm_daemon_config_load_xservers (daemon_config);

	/* Only read the list if no_console is FALSE at this stage */
	if (! no_console) {
		mdm_daemon_config_load_displays (daemon_config);
	}
	
	if G_UNLIKELY (displays == NULL) {
		handle_no_displays (daemon_config, no_console);
	}

	/* If no displays were found, then obviously
	   we're in a no console mode */
	if (displays == NULL) {
		no_console = TRUE;
	}

	if (no_console) {
		mdm_config_set_bool_for_id (daemon_config, MDM_ID_CONSOLE_NOTIFY, FALSE);
	}

	mdm_daemon_change_user (daemon_config, &uid, &gid);

	mdm_daemon_check_permissions (daemon_config, uid, gid);

	NEVER_FAILS_root_set_euid_egid (0, 0);

	check_logdir ();

	MdmUserId = uid;
	MdmGroupId = gid;
}

/**
 * mdm_daemon_config_get_mdmuid
 * mdm_daemon_config_get_mdmgid
 *
 * Access functions for getting the MDM user ID and group ID.
 */
uid_t
mdm_daemon_config_get_mdmuid (void)
{
	return MdmUserId;
}

gid_t
mdm_daemon_config_get_mdmgid (void)
{
	return MdmGroupId;
}

/**
 * mdm_daemon_config_get_high_display_num
 * mdm_daemon_config_get_high_display_num
 *
 * Access functions for getting the high display number.
 */
gint
mdm_daemon_config_get_high_display_num (void)
{
	return high_display_num;
}

void
mdm_daemon_config_set_high_display_num (gint val)
{
	high_display_num = val;
}

/**
 *  mdm_daemon_config_close
 *
 *  Cleanup
 */
void
mdm_daemon_config_close (void)
{
	mdm_config_free (daemon_config);
}

/**
 * mdm_is_valid_key
 *
 * Returns TRUE if the key is a valid key, FALSE otherwise.
 */
gboolean
mdm_daemon_config_is_valid_key (const char *keystring)
{
	char    *group;
	char    *key;
	gboolean ret;
	const MdmConfigEntry *entry;

	ret = mdm_common_config_parse_key_string (keystring,
						  &group,
						  &key,
						  NULL,
						  NULL);
	if (! ret) {
		goto out;
	}


	entry = mdm_config_lookup_entry (daemon_config, group, key);
	ret = (entry != NULL);

	g_free (group);
	g_free (key);
 out:
	return ret;
}

/**
 * mdm_signal_terminthup_was_notified
 *
 * returns TRUE if signal SIGTERM, SIGINT, or SIGHUP was received.
 * This just hides these vicious-extensions functions from the
 * other files
 */
gboolean
mdm_daemon_config_signal_terminthup_was_notified (void)
{
	if (ve_signal_was_notified (SIGTERM) ||
	    ve_signal_was_notified (SIGINT) ||
	    ve_signal_was_notified (SIGHUP)) {
		return TRUE;
	} else {
		return FALSE;
	}
}

static gboolean
is_prog_in_path (const char *prog)
{
	char    *f;
	gboolean ret;

	f = g_find_program_in_path (prog);
	ret = (f != NULL);
	g_free (f);
	return ret;
}

/**
 * mdm_daemon_config_get_session_exec
 *
 * This function accesses the MDM session desktop file and returns
 * the execution command for starting the session.
 *
 * Must be called with the PATH set correctly to find session exec.
 */
char *
mdm_daemon_config_get_session_exec (const char *session_name,
				    gboolean    check_try_exec)
{
	char        *session_filename;
	const char  *path_str;
	char       **search_dirs;
	GKeyFile    *cfg;
	static char *exec;
	static char *cached = NULL;
	gboolean     hidden;
	char        *ret;

	cfg = NULL;

	/* clear cache */
	if (session_name == NULL) {
		g_free (exec);
		exec = NULL;
		g_free (cached);
		cached = NULL;
		return NULL;
	}

	if (cached != NULL && strcmp (ve_sure_string (session_name), ve_sure_string (cached)) == 0)
		return g_strdup (exec);

	g_free (exec);
	exec = NULL;
	g_free (cached);
	cached = g_strdup (session_name);

	session_filename = mdm_ensure_extension (session_name, ".desktop");

	path_str = mdm_daemon_config_get_value_string (MDM_KEY_SESSION_DESKTOP_DIR);
	if (path_str == NULL) {
		mdm_error ("No session desktop directories defined");
		goto out;
	}

	search_dirs = g_strsplit (path_str, ":", -1);

	cfg = mdm_common_config_load_from_dirs (session_filename,
						(const char **)search_dirs,
						NULL);
	g_strfreev (search_dirs);

	if (cfg == NULL) {
		g_free (exec);
		exec = NULL;
		goto out;
	}

	hidden = FALSE;
	mdm_common_config_get_boolean (cfg, "Desktop Entry/Hidden=false", &hidden, NULL);
	if (hidden) {
		g_free (exec);
		exec = NULL;
		goto out;
	}

	if (check_try_exec) {
		char *tryexec;

		tryexec = NULL;
		mdm_common_config_get_string (cfg, "Desktop Entry/TryExec", &tryexec, NULL);

		if (tryexec != NULL &&
		    tryexec[0] != '\0' &&
		    ! is_prog_in_path (tryexec)) {
			g_free (tryexec);
			g_free (exec);
			exec = NULL;
			goto out;
		}
		g_free (tryexec);
	}

	exec = NULL;
	mdm_common_config_get_string (cfg, "Desktop Entry/Exec", &exec, NULL);

 out:

	ret = g_strdup (exec);

	g_key_file_free (cfg);

	return ret;
}

/**
 * mdm_daemon_config_get_session_xserver_args
 *
 * This function accesses the MDM session desktop file and returns
 * additional Xserver arguments to be used with this session
 */
char *
mdm_daemon_config_get_session_xserver_args (const char *session_name)
{
	char        *session_filename;
	const char  *path_str;
	char       **search_dirs;
	GKeyFile    *cfg;
	static char *xserver_args;
	static char *cached = NULL;
	char        *ret;

	cfg = NULL;

	/* clear cache */
	if (session_name == NULL) {
		g_free (xserver_args);
		xserver_args = NULL;
		g_free (cached);
		cached = NULL;
		return NULL;
	}

	if (cached != NULL && strcmp (ve_sure_string (session_name), ve_sure_string (cached)) == 0)
		return g_strdup (xserver_args);

	g_free (xserver_args);
	xserver_args = NULL;
	g_free (cached);
	cached = g_strdup (session_name);

	path_str = mdm_daemon_config_get_value_string (MDM_KEY_SESSION_DESKTOP_DIR);
	if (path_str == NULL) {
		mdm_error ("No session desktop directories defined");
		goto out;
	}

	search_dirs = g_strsplit (path_str, ":", -1);

	cfg = mdm_common_config_load_from_dirs (session_filename,
						(const char **)search_dirs,
						NULL);
	g_strfreev (search_dirs);

	xserver_args = NULL;
	mdm_common_config_get_string (cfg, "Desktop Entry/X-Mdm-XserverArgs", &xserver_args, NULL);

 out:

	ret = g_strdup (xserver_args);

	g_key_file_free (cfg);

	return ret;
}

/**
 * mdm_daemon_config_get_user_session_lang
 *
 * These functions get and set the user's language and setting in their
 * $HOME/.dmrc file.
 */
void
mdm_daemon_config_set_user_session_lang (gboolean savesess,
					 gboolean savelang,
					 const char *home_dir,
					 const char *save_session,
					 const char *save_language)
{
	GKeyFile *dmrc;
	gchar *cfgstr;

	cfgstr = g_build_filename (home_dir, ".dmrc", NULL);
	dmrc = mdm_common_config_load (cfgstr, NULL);
	if (dmrc == NULL) {
		gint fd = -1;
		mdm_debug ("The user dmrc file %s does not exist - creating it", cfgstr);
		VE_IGNORE_EINTR (fd = g_open (cfgstr,
			O_CREAT | O_TRUNC | O_RDWR, 0644));

		if (fd < 0)
			return;

		write (fd, "\n", 2);
		close (fd);
		dmrc = mdm_common_config_load (cfgstr, NULL);

		if (dmrc == NULL) {
			mdm_debug ("Failed to open dmrc file %s after trying to create it", cfgstr);
			return;
		}
	}

	if (savesess) {
		g_key_file_set_string (dmrc, "Desktop", "Session", ve_sure_string (save_session));
	}

	if (savelang) {
		if (ve_string_empty (save_language)) {
			/*
			 * We chose the system default language so wipe the
			 * lang key
			 */
			g_key_file_remove_key (dmrc, "Desktop", "Language", NULL);
		} else {
			g_key_file_set_string (dmrc, "Desktop", "Language", save_language);
		}
	}

	if (dmrc != NULL) {
		mode_t oldmode;
		oldmode = umask (077);
		mdm_common_config_save (dmrc, cfgstr, NULL);
		umask (oldmode);
	}

	g_free (cfgstr);
	g_key_file_free (dmrc);
}

void
mdm_daemon_config_get_user_session_lang (char      **usrsess,
					 char      **usrlang,
					 const char *home_dir)
{
	char *p;
	char *cfgfile;
	GKeyFile *cfg;
	char *session = NULL;
	char *lang = NULL;

	cfgfile = g_build_filename (home_dir, ".dmrc", NULL);
	cfg = mdm_common_config_load (cfgfile, NULL);
	g_free (cfgfile);

	if (cfg != NULL) {
		mdm_common_config_get_string (cfg, "Desktop/Session", &session, NULL);
		mdm_common_config_get_string (cfg, "Desktop/Language", &lang, NULL);
		g_key_file_free (cfg);
	}

	if (session == NULL || strcmp(session, "default") == 0) {
		// Don't allow .dmrc to specify the session as 'default'
		// This was allowed in MDM <= 1.8.
		// After this version MDM uses x-session-manager only as a last resort when no configuration/detection is working
		session = g_strdup ("");
	}
	if (usrsess != NULL) {
		*usrsess = g_strdup (session);
	}

	if (lang == NULL) {
		lang = g_strdup ("");
	}
	if (usrlang != NULL) {
		*usrlang = g_strdup (lang);
	}

	g_free (session);
	g_free (lang);
}
