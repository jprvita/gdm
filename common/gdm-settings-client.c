/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2007 William Jon McCann <mccann@jhu.edu>
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <glib-object.h>
#include <gio/gio.h>

#include "gdm-settings-client.h"
#include "gdm-settings-utils.h"
#include "gdm-settings-glue.h"

#define SETTINGS_DBUS_NAME      "org.gnome.DisplayManager"
#define SETTINGS_DBUS_PATH      "/org/gnome/DisplayManager/Settings"
#define SETTINGS_DBUS_INTERFACE "org.gnome.DisplayManager.Settings"

static GHashTable      *notifiers      = NULL;
static GHashTable      *schemas        = NULL;
static GdmDBusSettings *settings_proxy = NULL;
static guint32          id_serial      = 0;

typedef struct {
        guint                       id;
        char                       *root;
        GdmSettingsClientNotifyFunc func;
        gpointer                    user_data;
        GFreeFunc                   destroy_notify;
} GdmSettingsClientNotify;

static void
gdm_settings_client_notify_free (GdmSettingsClientNotify *notify)
{
        g_free (notify->root);

        if (notify->destroy_notify != NULL) {
                notify->destroy_notify (notify->user_data);
        }

        g_free (notify);
}

static GdmSettingsEntry *
get_entry_for_key (const char *key)
{
        GdmSettingsEntry *entry;

        entry = g_hash_table_lookup (schemas, key);

        if (entry == NULL) {
                g_warning ("GdmSettingsClient: unable to find schema for key: %s", key);
        }

        return entry;
}

static gboolean
set_value (const char *key,
           const char *value)
{
        GError  *error;
        gboolean res;

        /* FIXME: check cache */

        error = NULL;

        res = gdm_dbus_settings_call_set_value_sync (settings_proxy,
                                                     key,
                                                     value,
                                                     NULL,
                                                     &error);
        if (! res) {
                g_debug ("Failed to set value for %s: %s", key, error->message);
                g_error_free (error);
        }

        return res;
}

static gboolean
get_value (const char *key,
           char      **value)
{
        GError  *error;
        gboolean res;

        /* FIXME: check cache */

        error = NULL;

        res = gdm_dbus_settings_call_get_value_sync (settings_proxy,
                                                     key,
                                                     value,
                                                     NULL,
                                                     &error);
        if (! res) {
                g_debug ("Failed to get value for %s: %s", key, error->message);
                g_error_free (error);
        }

        return res;
}

static void
assert_signature (GdmSettingsEntry *entry,
                  const char       *signature)
{
        const char *sig;

        sig = gdm_settings_entry_get_signature (entry);

        g_assert (sig != NULL);
        g_assert (signature != NULL);
        g_assert (strcmp (signature, sig) == 0);
}

static guint32
get_next_serial (void)
{
        guint32 serial;

        serial = id_serial++;

        if ((gint32)id_serial < 0) {
                id_serial = 1;
        }

        return serial;
}

guint
gdm_settings_client_notify_add (const char                 *root,
                                GdmSettingsClientNotifyFunc func,
                                gpointer                    user_data,
                                GFreeFunc                   destroy_notify)
{
        guint32                  id;
        GdmSettingsClientNotify *notify;

        id = get_next_serial ();

        notify = g_new0 (GdmSettingsClientNotify, 1);
        notify->id = id;
        notify->root = g_strdup (root);
        notify->func = func;
        notify->user_data = user_data;
        notify->destroy_notify = destroy_notify;

        g_hash_table_insert (notifiers, GINT_TO_POINTER (id), notify);

        return id;
}

void
gdm_settings_client_notify_remove (guint id)
{
        g_hash_table_remove (notifiers, GINT_TO_POINTER (id));
}

gboolean
gdm_settings_client_get_string (const char  *key,
                                char       **value)
{
        GdmSettingsEntry *entry;
        gboolean          ret;
        gboolean          res;
        char             *str;

        g_return_val_if_fail (key != NULL, FALSE);

        entry = get_entry_for_key (key);
        g_assert (entry != NULL);

        assert_signature (entry, "s");

        ret = FALSE;

        res = get_value (key, &str);

        if (! res) {
                /* use the default */
                str = g_strdup (gdm_settings_entry_get_default_value (entry));
        }

        if (value != NULL) {
                *value = g_strdup (str);
        }

        ret = TRUE;

        g_free (str);

        return ret;
}

gboolean
gdm_settings_client_get_locale_string (const char  *key,
                                       const char  *locale,
                                       char       **value)
{
        char    *candidate_key;
        char    *translated_value;
        char   **languages;
        gboolean free_languages = FALSE;
        int      i;
        gboolean ret;

        g_return_val_if_fail (key != NULL, FALSE);

        candidate_key = NULL;
        translated_value = NULL;

        if (locale != NULL) {
                languages = g_new (char *, 2);
                languages[0] = (char *)locale;
                languages[1] = NULL;

                free_languages = TRUE;
        } else {
                languages = (char **) g_get_language_names ();
                free_languages = FALSE;
        }

        for (i = 0; languages[i]; i++) {
                gboolean res;

                candidate_key = g_strdup_printf ("%s[%s]", key, languages[i]);

                res = get_value (candidate_key, &translated_value);
                g_free (candidate_key);

                if (res) {
                        break;
                }

                g_free (translated_value);
                translated_value = NULL;
        }

        /* Fallback to untranslated key
         */
        if (translated_value == NULL) {
                get_value (key, &translated_value);
        }

        if (free_languages) {
                g_strfreev (languages);
        }

        if (translated_value != NULL) {
                ret = TRUE;
                if (value != NULL) {
                        *value = g_strdup (translated_value);
                }
        } else {
                ret = FALSE;
        }

        g_free (translated_value);

        return ret;
}

gboolean
gdm_settings_client_get_boolean (const char *key,
                                 gboolean   *value)
{
        GdmSettingsEntry *entry;
        gboolean          ret;
        gboolean          res;
        char             *str;

        g_return_val_if_fail (key != NULL, FALSE);

        entry = get_entry_for_key (key);
        g_assert (entry != NULL);

        assert_signature (entry, "b");

        ret = FALSE;

        res = get_value (key, &str);

        if (! res) {
                /* use the default */
                str = g_strdup (gdm_settings_entry_get_default_value (entry));
        }

        ret = gdm_settings_parse_value_as_boolean  (str, value);

        g_free (str);

        return ret;
}

gboolean
gdm_settings_client_get_int (const char *key,
                             int        *value)
{
        GdmSettingsEntry *entry;
        gboolean          ret;
        gboolean          res;
        char             *str;

        g_return_val_if_fail (key != NULL, FALSE);

        entry = get_entry_for_key (key);
        g_assert (entry != NULL);

        assert_signature (entry, "i");

        ret = FALSE;

        res = get_value (key, &str);

        if (! res) {
                /* use the default */
                str = g_strdup (gdm_settings_entry_get_default_value (entry));
        }

        ret = gdm_settings_parse_value_as_integer (str, value);

        g_free (str);

        return ret;
}

gboolean
gdm_settings_client_set_int (const char *key,
                             int         value)
{
        GdmSettingsEntry *entry;
        gboolean          res;
        char             *str;

        g_return_val_if_fail (key != NULL, FALSE);

        entry = get_entry_for_key (key);
        g_assert (entry != NULL);

        assert_signature (entry, "i");

        str = gdm_settings_parse_integer_as_value (value);

        res = set_value (key, str);

        g_free (str);

        return res;
}

gboolean
gdm_settings_client_set_string (const char *key,
                                const char *value)
{
        GdmSettingsEntry *entry;
        gboolean          res;

        g_return_val_if_fail (key != NULL, FALSE);

        entry = get_entry_for_key (key);
        g_assert (entry != NULL);

        assert_signature (entry, "s");

        res = set_value (key, value);

        return res;
}

gboolean
gdm_settings_client_set_boolean (const char *key,
                                 gboolean    value)
{
        GdmSettingsEntry *entry;
        gboolean          res;
        char             *str;

        g_return_val_if_fail (key != NULL, FALSE);

        entry = get_entry_for_key (key);
        g_assert (entry != NULL);

        assert_signature (entry, "b");

        str = gdm_settings_parse_boolean_as_value (value);

        res = set_value (key, str);

        g_free (str);

        return res;
}

static void
hashify_list (GdmSettingsEntry *entry,
              gpointer          data)
{
        g_hash_table_insert (schemas, g_strdup (gdm_settings_entry_get_key (entry)), entry);
}

static void
send_notification (gpointer                 key,
                   GdmSettingsClientNotify *notify,
                   GdmSettingsEntry        *entry)
{
        /* get out if the key is not in the region of interest */
        if (! g_str_has_prefix (gdm_settings_entry_get_key (entry), notify->root)) {
                return;
        }

        notify->func (notify->id, entry, notify->user_data);
}

static void
on_value_changed (GdmDBusSettings *proxy,
                  const char      *key,
                  const char      *old_value,
                  const char      *new_value,
                  gpointer         data)
{
        GdmSettingsEntry *entry;

        g_debug ("Value Changed key=%s old=%s new=%s", key, old_value, new_value);

        /* lookup entry */
        entry = get_entry_for_key (key);

        if (entry == NULL) {
                return;
        }

        gdm_settings_entry_set_value (entry, new_value);

        g_hash_table_foreach (notifiers, (GHFunc)send_notification, entry);
}

gboolean
gdm_settings_client_init (const char *file,
                          const char *root)
{
        GError  *error;
        GSList  *list;

        g_return_val_if_fail (file != NULL, FALSE);
        g_return_val_if_fail (root != NULL, FALSE);

        g_assert (schemas == NULL);

        error = NULL;

        settings_proxy = GDM_DBUS_SETTINGS (gdm_dbus_settings_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
                                                                                      G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                                                                      SETTINGS_DBUS_NAME,
                                                                                      SETTINGS_DBUS_PATH,
                                                                                      NULL,
                                                                                      &error));
        if (settings_proxy == NULL) {
                g_warning ("Unable to connect to settings server: %s", error->message);
                g_error_free (error);
                return FALSE;
        }

        list = NULL;
        if (! gdm_settings_parse_schemas (file, root, &list)) {
                g_warning ("Unable to parse schemas");
                g_clear_object (&settings_proxy);
                return FALSE;
        }

        notifiers = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify)gdm_settings_client_notify_free);

        schemas = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify)gdm_settings_entry_free);
        g_slist_foreach (list, (GFunc)hashify_list, NULL);

        g_signal_connect (settings_proxy, "value-changed", G_CALLBACK (on_value_changed), NULL);
        return TRUE;
}

void
gdm_settings_client_shutdown (void)
{

}
