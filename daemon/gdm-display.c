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
#include <glib-object.h>

#include "gdm-common.h"
#include "gdm-display.h"
#include "gdm-display-glue.h"
#include "gdm-display-access-file.h"

#include "gdm-settings-direct.h"
#include "gdm-settings-keys.h"

#include "gdm-slave-proxy.h"
#include "gdm-slave-glue.h"
#include "gdm-dbus-util.h"

#define GDM_DISPLAY_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), GDM_TYPE_DISPLAY, GdmDisplayPrivate))

#define GDM_SLAVE_PATH "/org/gnome/DisplayManager/Slave"
#define DEFAULT_SLAVE_COMMAND LIBEXECDIR "/gdm-simple-slave"

struct GdmDisplayPrivate
{
        char                 *id;
        char                 *seat_id;
        char                 *session_id;

        char                 *remote_hostname;
        int                   x11_display_number;
        char                 *x11_display_name;
        int                   status;
        time_t                creation_time;
        GTimer               *slave_timer;
        char                 *slave_command;

        char                 *x11_cookie;
        gsize                 x11_cookie_size;
        GdmDisplayAccessFile *access_file;

        gboolean              is_local;
        guint                 finish_idle_id;

        GdmSlaveProxy        *slave_proxy;
        char                 *slave_bus_name;
        GdmDBusSlave         *slave_bus_proxy;
        int                   slave_name_id;
        GDBusConnection      *connection;
        GdmDisplayAccessFile *user_access_file;

        GdmDBusDisplay       *display_skeleton;
        GDBusObjectSkeleton  *object_skeleton;

        gboolean              is_initial;
};

enum {
        PROP_0,
        PROP_ID,
        PROP_STATUS,
        PROP_SEAT_ID,
        PROP_SESSION_ID,
        PROP_REMOTE_HOSTNAME,
        PROP_X11_DISPLAY_NUMBER,
        PROP_X11_DISPLAY_NAME,
        PROP_X11_COOKIE,
        PROP_X11_AUTHORITY_FILE,
        PROP_IS_LOCAL,
        PROP_SLAVE_COMMAND,
        PROP_IS_INITIAL
};

static void     gdm_display_class_init  (GdmDisplayClass *klass);
static void     gdm_display_init        (GdmDisplay      *display);
static void     gdm_display_finalize    (GObject         *object);
static void     queue_finish            (GdmDisplay      *display);

G_DEFINE_ABSTRACT_TYPE (GdmDisplay, gdm_display, G_TYPE_OBJECT)

GQuark
gdm_display_error_quark (void)
{
        static GQuark ret = 0;
        if (ret == 0) {
                ret = g_quark_from_static_string ("gdm_display_error");
        }

        return ret;
}

time_t
gdm_display_get_creation_time (GdmDisplay *display)
{
        g_return_val_if_fail (GDM_IS_DISPLAY (display), 0);

        return display->priv->creation_time;
}

int
gdm_display_get_status (GdmDisplay *display)
{
        g_return_val_if_fail (GDM_IS_DISPLAY (display), 0);

        return display->priv->status;
}

char *
gdm_display_get_session_id (GdmDisplay *display)
{
        return g_strdup (display->priv->session_id);
}

static GdmDisplayAccessFile *
_create_access_file_for_user (GdmDisplay  *display,
                              const char  *username,
                              GError     **error)
{
        GdmDisplayAccessFile *access_file;
        GError *file_error;

        access_file = gdm_display_access_file_new (username);

        file_error = NULL;
        if (!gdm_display_access_file_open (access_file, &file_error)) {
                g_propagate_error (error, file_error);
                return NULL;
        }

        return access_file;
}

static gboolean
gdm_display_real_create_authority (GdmDisplay *display)
{
        GdmDisplayAccessFile *access_file;
        GError               *error;
        gboolean              res;

        g_return_val_if_fail (GDM_IS_DISPLAY (display), FALSE);
        g_return_val_if_fail (display->priv->access_file == NULL, FALSE);

        error = NULL;
        access_file = _create_access_file_for_user (display, GDM_USERNAME, &error);

        if (access_file == NULL) {
                g_critical ("could not create display access file: %s", error->message);
                g_error_free (error);
                return FALSE;
        }

        g_free (display->priv->x11_cookie);
        display->priv->x11_cookie = NULL;
        res = gdm_display_access_file_add_display (access_file,
                                                   display,
                                                   &display->priv->x11_cookie,
                                                   &display->priv->x11_cookie_size,
                                                   &error);

        if (! res) {

                g_critical ("could not add display to access file: %s", error->message);
                g_error_free (error);
                gdm_display_access_file_close (access_file);
                g_object_unref (access_file);
                return FALSE;
        }

        display->priv->access_file = access_file;

        return TRUE;
}

gboolean
gdm_display_create_authority (GdmDisplay *display)
{
        gboolean ret;

        g_return_val_if_fail (GDM_IS_DISPLAY (display), FALSE);

        g_object_ref (display);
        ret = GDM_DISPLAY_GET_CLASS (display)->create_authority (display);
        g_object_unref (display);

        return ret;
}

static gboolean
gdm_display_real_add_user_authorization (GdmDisplay *display,
                                         const char *username,
                                         char      **filename,
                                         GError    **error)
{
        GdmDisplayAccessFile *access_file;
        GError               *access_file_error;
        gboolean              res;

        g_return_val_if_fail (GDM_IS_DISPLAY (display), FALSE);
        g_return_val_if_fail (display->priv->access_file != NULL, FALSE);

        g_debug ("GdmDisplay: Adding user authorization for %s", username);

        access_file_error = NULL;
        access_file = _create_access_file_for_user (display,
                                                    username,
                                                    &access_file_error);

        if (access_file == NULL) {
                g_propagate_error (error, access_file_error);
                return FALSE;
        }

        res = gdm_display_access_file_add_display_with_cookie (access_file,
                                                               display,
                                                               display->priv->x11_cookie,
                                                               display->priv->x11_cookie_size,
                                                               &access_file_error);
        if (! res) {
                g_debug ("GdmDisplay: Unable to add user authorization for %s: %s",
                         username,
                         access_file_error->message);
                g_propagate_error (error, access_file_error);
                gdm_display_access_file_close (access_file);
                g_object_unref (access_file);
                return FALSE;
        }

        *filename = gdm_display_access_file_get_path (access_file);
        display->priv->user_access_file = access_file;

        g_debug ("GdmDisplay: Added user authorization for %s: %s", username, *filename);

        return TRUE;
}

gboolean
gdm_display_add_user_authorization (GdmDisplay *display,
                                    const char *username,
                                    char      **filename,
                                    GError    **error)
{
        gboolean ret;

        g_return_val_if_fail (GDM_IS_DISPLAY (display), FALSE);

        g_debug ("GdmDisplay: Adding authorization for user:%s on display %s", username, display->priv->x11_display_name);

        g_object_ref (display);
        ret = GDM_DISPLAY_GET_CLASS (display)->add_user_authorization (display, username, filename, error);
        g_object_unref (display);

        return ret;
}

static void
on_name_vanished (GDBusConnection *connection,
                  const char      *name,
                  gpointer         user_data)
{
        queue_finish (GDM_DISPLAY (user_data));
}

static gboolean
gdm_display_real_set_slave_bus_name (GdmDisplay *display,
                                     const char *name,
                                     GError    **error)
{
        g_free (display->priv->slave_bus_name);
        display->priv->slave_bus_name = g_strdup (name);

        if (display->priv->slave_name_id > 0) {
                g_bus_unwatch_name (display->priv->slave_name_id);
        }

        display->priv->slave_name_id = g_bus_watch_name_on_connection (display->priv->connection,
                                                                       name,
                                                                       G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                                       NULL, /* name appeared */
                                                                       on_name_vanished,
                                                                       display,
                                                                       NULL);

        g_clear_object (&display->priv->slave_bus_proxy);
        display->priv->slave_bus_proxy = GDM_DBUS_SLAVE (gdm_dbus_slave_proxy_new_sync (display->priv->connection,
                                                                                        G_DBUS_PROXY_FLAGS_NONE,
                                                                                        name,
                                                                                        GDM_SLAVE_PATH,
                                                                                        NULL, NULL));
        g_object_bind_property (G_OBJECT (display->priv->slave_bus_proxy),
                                "session-id",
                                G_OBJECT (display),
                                "session-id",
                                G_BINDING_DEFAULT);

        return TRUE;
}

gboolean
gdm_display_set_slave_bus_name (GdmDisplay *display,
                                const char *name,
                                GError    **error)
{
        gboolean ret;

        g_return_val_if_fail (GDM_IS_DISPLAY (display), FALSE);

        g_debug ("GdmDisplay: Setting slave bus name:%s on display %s", name, display->priv->x11_display_name);

        g_object_ref (display);
        ret = GDM_DISPLAY_GET_CLASS (display)->set_slave_bus_name (display, name, error);
        g_object_unref (display);

        return ret;
}

static void
gdm_display_real_get_timed_login_details (GdmDisplay *display,
                                          gboolean   *enabledp,
                                          char      **usernamep,
                                          int        *delayp)
{
        gboolean res;
        gboolean enabled;
        int      delay;
        char    *username;

        enabled = FALSE;
        username = NULL;
        delay = 0;

#ifdef WITH_SYSTEMD
        /* FIXME: More careful thought needs to happen before we
         * can support auto/timed login on auxilliary seats in the
         * systemd path.
         */
        if (LOGIND_RUNNING()) {
                if (g_strcmp0 (display->priv->seat_id, "seat0") != 0) {
                        goto out;
                }
        }
#endif

        res = gdm_settings_direct_get_boolean (GDM_KEY_AUTO_LOGIN_ENABLE, &enabled);
        if (res && enabled) {
            res = gdm_settings_direct_get_string (GDM_KEY_AUTO_LOGIN_USER, &username);
        }

        if (enabled && res && username != NULL && username[0] != '\0') {
                goto out;
        }

        g_free (username);
        username = NULL;
        enabled = FALSE;

        res = gdm_settings_direct_get_boolean (GDM_KEY_TIMED_LOGIN_ENABLE, &enabled);
        if (res && ! enabled) {
                goto out;
        }

        res = gdm_settings_direct_get_string (GDM_KEY_TIMED_LOGIN_USER, &username);
        if (res && (username == NULL || username[0] == '\0')) {
                enabled = FALSE;
                g_free (username);
                username = NULL;
                goto out;
        }

        delay = 0;
        res = gdm_settings_direct_get_int (GDM_KEY_TIMED_LOGIN_DELAY, &delay);

        if (res && delay <= 0) {
                /* we don't allow the timed login to have a zero delay */
                delay = 10;
        }

 out:
        if (enabledp != NULL) {
                *enabledp = enabled;
        }
        if (usernamep != NULL) {
                *usernamep = username;
        } else {
                g_free (username);
        }
        if (delayp != NULL) {
                *delayp = delay;
        }
}

gboolean
gdm_display_get_timed_login_details (GdmDisplay *display,
                                     gboolean   *enabled,
                                     char      **username,
                                     int        *delay,
                                     GError    **error)
{
        g_return_val_if_fail (GDM_IS_DISPLAY (display), FALSE);

        GDM_DISPLAY_GET_CLASS (display)->get_timed_login_details (display, enabled, username, delay);

        g_debug ("GdmSlave: Got timed login details for display %s: %d '%s' %d",
                 display->priv->x11_display_name,
                 *enabled,
                 *username ? *username : "(null)",
                 *delay);

        return TRUE;
}

static gboolean
gdm_display_real_remove_user_authorization (GdmDisplay *display,
                                            const char *username,
                                            GError    **error)
{
        gdm_display_access_file_close (display->priv->user_access_file);

        return TRUE;
}

gboolean
gdm_display_remove_user_authorization (GdmDisplay *display,
                                       const char *username,
                                       GError    **error)
{
        gboolean ret;

        g_return_val_if_fail (GDM_IS_DISPLAY (display), FALSE);

        g_debug ("GdmDisplay: Removing authorization for user:%s on display %s", username, display->priv->x11_display_name);

        g_object_ref (display);
        ret = GDM_DISPLAY_GET_CLASS (display)->remove_user_authorization (display, username, error);
        g_object_unref (display);

        return ret;
}

gboolean
gdm_display_get_x11_cookie (GdmDisplay *display,
                            GArray    **x11_cookie,
                            GError    **error)
{
        g_return_val_if_fail (GDM_IS_DISPLAY (display), FALSE);

        if (x11_cookie != NULL) {
                *x11_cookie = g_array_new (TRUE, FALSE, sizeof (char));
                g_array_append_vals (*x11_cookie,
                                     display->priv->x11_cookie,
                                     display->priv->x11_cookie_size);
        }

        return TRUE;
}

gboolean
gdm_display_get_x11_authority_file (GdmDisplay *display,
                                    char      **filename,
                                    GError    **error)
{
        g_return_val_if_fail (GDM_IS_DISPLAY (display), FALSE);
        g_return_val_if_fail (filename != NULL, FALSE);

        if (display->priv->access_file != NULL) {
                *filename = gdm_display_access_file_get_path (display->priv->access_file);
        } else {
                *filename = NULL;
        }

        return TRUE;
}

gboolean
gdm_display_get_remote_hostname (GdmDisplay *display,
                                 char      **hostname,
                                 GError    **error)
{
        g_return_val_if_fail (GDM_IS_DISPLAY (display), FALSE);

        if (hostname != NULL) {
                *hostname = g_strdup (display->priv->remote_hostname);
        }

        return TRUE;
}

gboolean
gdm_display_get_x11_display_number (GdmDisplay *display,
                                    int        *number,
                                    GError    **error)
{
       g_return_val_if_fail (GDM_IS_DISPLAY (display), FALSE);

       if (number != NULL) {
               *number = display->priv->x11_display_number;
       }

       return TRUE;
}

gboolean
gdm_display_get_seat_id (GdmDisplay *display,
                         char      **seat_id,
                         GError    **error)
{
       g_return_val_if_fail (GDM_IS_DISPLAY (display), FALSE);

       if (seat_id != NULL) {
               *seat_id = g_strdup (display->priv->seat_id);
       }

       return TRUE;
}

gboolean
gdm_display_is_initial (GdmDisplay  *display,
                        gboolean    *is_initial,
                        GError     **error)
{
        g_return_val_if_fail (GDM_IS_DISPLAY (display), FALSE);

        if (is_initial != NULL) {
                *is_initial = display->priv->is_initial;
        }

        return TRUE;
}

static gboolean
finish_idle (GdmDisplay *display)
{
        display->priv->finish_idle_id = 0;
        /* finish may end up finalizing object */
        gdm_display_finish (display);
        return FALSE;
}

static void
queue_finish (GdmDisplay *display)
{
        if (display->priv->finish_idle_id == 0) {
                display->priv->finish_idle_id = g_idle_add ((GSourceFunc)finish_idle, display);
        }
}

static void
slave_exited (GdmSlaveProxy       *proxy,
              int                  code,
              GdmDisplay          *display)
{
        g_debug ("GdmDisplay: Slave exited: %d", code);

        queue_finish (display);
}

static void
slave_died (GdmSlaveProxy       *proxy,
            int                  signum,
            GdmDisplay          *display)
{
        g_debug ("GdmDisplay: Slave died: %d", signum);

        queue_finish (display);
}


static void
_gdm_display_set_status (GdmDisplay *display,
                         int         status)
{
        if (status != display->priv->status) {
                display->priv->status = status;
                g_object_notify (G_OBJECT (display), "status");
        }
}

static gboolean
gdm_display_real_prepare (GdmDisplay *display)
{
        char *command;
        char *log_file;
        char *log_path;

        g_return_val_if_fail (GDM_IS_DISPLAY (display), FALSE);

        g_debug ("GdmDisplay: prepare display");

        g_assert (display->priv->slave_proxy == NULL);

        if (!gdm_display_create_authority (display)) {
                g_warning ("Unable to set up access control for display %d",
                           display->priv->x11_display_number);
                return FALSE;
        }

        _gdm_display_set_status (display, GDM_DISPLAY_PREPARED);

        display->priv->slave_proxy = gdm_slave_proxy_new ();
        g_signal_connect (display->priv->slave_proxy,
                          "exited",
                          G_CALLBACK (slave_exited),
                          display);
        g_signal_connect (display->priv->slave_proxy,
                          "died",
                          G_CALLBACK (slave_died),
                          display);

        log_file = g_strdup_printf ("%s-slave.log", display->priv->x11_display_name);
        log_path = g_build_filename (LOGDIR, log_file, NULL);
        g_free (log_file);
        gdm_slave_proxy_set_log_path (display->priv->slave_proxy, log_path);
        g_free (log_path);

        command = g_strdup_printf ("%s --display-id %s",
                                   display->priv->slave_command,
                                   display->priv->id);
        gdm_slave_proxy_set_command (display->priv->slave_proxy, command);
        g_free (command);

        return TRUE;
}

gboolean
gdm_display_prepare (GdmDisplay *display)
{
        gboolean ret;

        g_return_val_if_fail (GDM_IS_DISPLAY (display), FALSE);

        g_debug ("GdmDisplay: Preparing display: %s", display->priv->id);

        g_object_ref (display);
        ret = GDM_DISPLAY_GET_CLASS (display)->prepare (display);
        g_object_unref (display);

        return ret;
}


static gboolean
gdm_display_real_manage (GdmDisplay *display)
{
        gboolean res;

        g_return_val_if_fail (GDM_IS_DISPLAY (display), FALSE);

        g_debug ("GdmDisplay: manage display");

        /* If not explicitly prepared, do it now */
        if (display->priv->status == GDM_DISPLAY_UNMANAGED) {
                res = gdm_display_prepare (display);
                if (! res) {
                        return FALSE;
                }
        }

        g_assert (display->priv->slave_proxy != NULL);

        g_timer_start (display->priv->slave_timer);

        gdm_slave_proxy_start (display->priv->slave_proxy);

        return TRUE;
}

gboolean
gdm_display_manage (GdmDisplay *display)
{
        gboolean ret;

        g_return_val_if_fail (GDM_IS_DISPLAY (display), FALSE);

        g_debug ("GdmDisplay: Managing display: %s", display->priv->id);

        g_object_ref (display);
        ret = GDM_DISPLAY_GET_CLASS (display)->manage (display);
        g_object_unref (display);

        return ret;
}

static gboolean
gdm_display_real_finish (GdmDisplay *display)
{
        g_return_val_if_fail (GDM_IS_DISPLAY (display), FALSE);

        _gdm_display_set_status (display, GDM_DISPLAY_FINISHED);

        g_debug ("GdmDisplay: finish display");

        return TRUE;
}

gboolean
gdm_display_finish (GdmDisplay *display)
{
        gboolean ret;

        g_return_val_if_fail (GDM_IS_DISPLAY (display), FALSE);

        g_debug ("GdmDisplay: Finishing display: %s", display->priv->id);

        g_object_ref (display);
        ret = GDM_DISPLAY_GET_CLASS (display)->finish (display);
        g_object_unref (display);

        return ret;
}

static gboolean
gdm_display_real_unmanage (GdmDisplay *display)
{
        gdouble elapsed;

        g_return_val_if_fail (GDM_IS_DISPLAY (display), FALSE);

        g_debug ("GdmDisplay: unmanage display");

        g_timer_stop (display->priv->slave_timer);

        if (display->priv->slave_proxy != NULL) {
                gdm_slave_proxy_stop (display->priv->slave_proxy);

                g_object_unref (display->priv->slave_proxy);
                display->priv->slave_proxy = NULL;
        }

        if (display->priv->user_access_file != NULL) {
                gdm_display_access_file_close (display->priv->user_access_file);
                g_object_unref (display->priv->user_access_file);
                display->priv->user_access_file = NULL;
        }

        if (display->priv->access_file != NULL) {
                gdm_display_access_file_close (display->priv->access_file);
                g_object_unref (display->priv->access_file);
                display->priv->access_file = NULL;
        }

        elapsed = g_timer_elapsed (display->priv->slave_timer, NULL);
        if (elapsed < 3) {
                g_warning ("GdmDisplay: display lasted %lf seconds", elapsed);
                _gdm_display_set_status (display, GDM_DISPLAY_FAILED);
        } else {
                _gdm_display_set_status (display, GDM_DISPLAY_UNMANAGED);
        }

        if (display->priv->slave_name_id > 0) {
                g_bus_unwatch_name (display->priv->slave_name_id);
                display->priv->slave_name_id = 0;
        }

        return TRUE;
}

gboolean
gdm_display_unmanage (GdmDisplay *display)
{
        gboolean ret;

        g_return_val_if_fail (GDM_IS_DISPLAY (display), FALSE);

        g_debug ("GdmDisplay: Unmanaging display");

        g_object_ref (display);
        ret = GDM_DISPLAY_GET_CLASS (display)->unmanage (display);
        g_object_unref (display);

        return ret;
}

gboolean
gdm_display_get_id (GdmDisplay         *display,
                    char              **id,
                    GError            **error)
{
        g_return_val_if_fail (GDM_IS_DISPLAY (display), FALSE);

        if (id != NULL) {
                *id = g_strdup (display->priv->id);
        }

        return TRUE;
}

gboolean
gdm_display_get_x11_display_name (GdmDisplay   *display,
                                  char        **x11_display,
                                  GError      **error)
{
        g_return_val_if_fail (GDM_IS_DISPLAY (display), FALSE);

        if (x11_display != NULL) {
                *x11_display = g_strdup (display->priv->x11_display_name);
        }

        return TRUE;
}

gboolean
gdm_display_is_local (GdmDisplay *display,
                      gboolean   *local,
                      GError    **error)
{
        g_return_val_if_fail (GDM_IS_DISPLAY (display), FALSE);

        if (local != NULL) {
                *local = display->priv->is_local;
        }

        return TRUE;
}

static void
_gdm_display_set_id (GdmDisplay     *display,
                     const char     *id)
{
        g_free (display->priv->id);
        display->priv->id = g_strdup (id);
}

static void
_gdm_display_set_seat_id (GdmDisplay     *display,
                          const char     *seat_id)
{
        g_free (display->priv->seat_id);
        display->priv->seat_id = g_strdup (seat_id);
}

static void
_gdm_display_set_session_id (GdmDisplay     *display,
                             const char     *session_id)
{
        g_free (display->priv->session_id);
        display->priv->session_id = g_strdup (session_id);
}

static void
_gdm_display_set_remote_hostname (GdmDisplay     *display,
                                  const char     *hostname)
{
        g_free (display->priv->remote_hostname);
        display->priv->remote_hostname = g_strdup (hostname);
}

static void
_gdm_display_set_x11_display_number (GdmDisplay     *display,
                                     int             num)
{
        display->priv->x11_display_number = num;
}

static void
_gdm_display_set_x11_display_name (GdmDisplay     *display,
                                   const char     *x11_display)
{
        g_free (display->priv->x11_display_name);
        display->priv->x11_display_name = g_strdup (x11_display);
}

static void
_gdm_display_set_x11_cookie (GdmDisplay     *display,
                             const char     *x11_cookie)
{
        g_free (display->priv->x11_cookie);
        display->priv->x11_cookie = g_strdup (x11_cookie);
}

static void
_gdm_display_set_is_local (GdmDisplay     *display,
                           gboolean        is_local)
{
        display->priv->is_local = is_local;
}

static void
_gdm_display_set_slave_command (GdmDisplay     *display,
                                const char     *command)
{
        g_free (display->priv->slave_command);
        display->priv->slave_command = g_strdup (command);
}

static void
_gdm_display_set_is_initial (GdmDisplay     *display,
                             gboolean        initial)
{
        display->priv->is_initial = initial;
}

static void
gdm_display_set_property (GObject        *object,
                          guint           prop_id,
                          const GValue   *value,
                          GParamSpec     *pspec)
{
        GdmDisplay *self;

        self = GDM_DISPLAY (object);

        switch (prop_id) {
        case PROP_ID:
                _gdm_display_set_id (self, g_value_get_string (value));
                break;
        case PROP_STATUS:
                _gdm_display_set_status (self, g_value_get_int (value));
                break;
        case PROP_SEAT_ID:
                _gdm_display_set_seat_id (self, g_value_get_string (value));
                break;
        case PROP_SESSION_ID:
                _gdm_display_set_session_id (self, g_value_get_string (value));
                break;
        case PROP_REMOTE_HOSTNAME:
                _gdm_display_set_remote_hostname (self, g_value_get_string (value));
                break;
        case PROP_X11_DISPLAY_NUMBER:
                _gdm_display_set_x11_display_number (self, g_value_get_int (value));
                break;
        case PROP_X11_DISPLAY_NAME:
                _gdm_display_set_x11_display_name (self, g_value_get_string (value));
                break;
        case PROP_X11_COOKIE:
                _gdm_display_set_x11_cookie (self, g_value_get_string (value));
                break;
        case PROP_IS_LOCAL:
                _gdm_display_set_is_local (self, g_value_get_boolean (value));
                break;
        case PROP_SLAVE_COMMAND:
                _gdm_display_set_slave_command (self, g_value_get_string (value));
                break;
        case PROP_IS_INITIAL:
                _gdm_display_set_is_initial (self, g_value_get_boolean (value));
                break;
        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
                break;
        }
}

static void
gdm_display_get_property (GObject        *object,
                          guint           prop_id,
                          GValue         *value,
                          GParamSpec     *pspec)
{
        GdmDisplay *self;

        self = GDM_DISPLAY (object);

        switch (prop_id) {
        case PROP_ID:
                g_value_set_string (value, self->priv->id);
                break;
        case PROP_STATUS:
                g_value_set_int (value, self->priv->status);
                break;
        case PROP_SEAT_ID:
                g_value_set_string (value, self->priv->seat_id);
                break;
        case PROP_SESSION_ID:
                g_value_set_string (value, self->priv->session_id);
                break;
        case PROP_REMOTE_HOSTNAME:
                g_value_set_string (value, self->priv->remote_hostname);
                break;
        case PROP_X11_DISPLAY_NUMBER:
                g_value_set_int (value, self->priv->x11_display_number);
                break;
        case PROP_X11_DISPLAY_NAME:
                g_value_set_string (value, self->priv->x11_display_name);
                break;
        case PROP_X11_COOKIE:
                g_value_set_string (value, self->priv->x11_cookie);
                break;
        case PROP_X11_AUTHORITY_FILE:
                g_value_take_string (value,
                                     gdm_display_access_file_get_path (self->priv->access_file));
                break;
        case PROP_IS_LOCAL:
                g_value_set_boolean (value, self->priv->is_local);
                break;
        case PROP_SLAVE_COMMAND:
                g_value_set_string (value, self->priv->slave_command);
                break;
        case PROP_IS_INITIAL:
                g_value_set_boolean (value, self->priv->is_initial);
                break;
        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
                break;
        }
}

static gboolean
handle_get_id (GdmDBusDisplay        *skeleton,
               GDBusMethodInvocation *invocation,
               GdmDisplay            *display)
{
        char *id;

        gdm_display_get_id (display, &id, NULL);

        gdm_dbus_display_complete_get_id (skeleton, invocation, id);

        g_free (id);
        return TRUE;
}

static gboolean
handle_get_remote_hostname (GdmDBusDisplay        *skeleton,
                            GDBusMethodInvocation *invocation,
                            GdmDisplay            *display)
{
        char *hostname;

        gdm_display_get_remote_hostname (display, &hostname, NULL);

        gdm_dbus_display_complete_get_remote_hostname (skeleton,
                                                       invocation,
                                                       hostname ? hostname : "");

        g_free (hostname);
        return TRUE;
}

static gboolean
handle_get_seat_id (GdmDBusDisplay        *skeleton,
                    GDBusMethodInvocation *invocation,
                    GdmDisplay            *display)
{
        char *seat_id;

        seat_id = NULL;
        gdm_display_get_seat_id (display, &seat_id, NULL);

        if (seat_id == NULL) {
                seat_id = g_strdup ("");
        }
        gdm_dbus_display_complete_get_seat_id (skeleton, invocation, seat_id);

        g_free (seat_id);
        return TRUE;
}

static gboolean
handle_get_timed_login_details (GdmDBusDisplay        *skeleton,
                                GDBusMethodInvocation *invocation,
                                GdmDisplay            *display)
{
        gboolean enabled;
        char *username;
        int delay;

        gdm_display_get_timed_login_details (display, &enabled, &username, &delay, NULL);

        gdm_dbus_display_complete_get_timed_login_details (skeleton,
                                                           invocation,
                                                           enabled,
                                                           username ? username : "",
                                                           delay);

        g_free (username);
        return TRUE;
}

static gboolean
handle_get_x11_authority_file (GdmDBusDisplay        *skeleton,
                               GDBusMethodInvocation *invocation,
                               GdmDisplay            *display)
{
        char *file;

        gdm_display_get_x11_authority_file (display, &file, NULL);

        gdm_dbus_display_complete_get_x11_authority_file (skeleton, invocation, file);

        g_free (file);
        return TRUE;
}

static gboolean
handle_get_x11_cookie (GdmDBusDisplay        *skeleton,
                       GDBusMethodInvocation *invocation,
                       GdmDisplay            *display)
{
        GArray *cookie = NULL;
        GVariant *variant;

        gdm_display_get_x11_cookie (display, &cookie, NULL);

        variant = g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                             cookie->data,
                                             cookie->len,
                                             sizeof (char));
        gdm_dbus_display_complete_get_x11_cookie (skeleton, invocation, variant);

        g_array_unref (cookie);
        return TRUE;
}

static gboolean
handle_get_x11_display_name (GdmDBusDisplay        *skeleton,
                             GDBusMethodInvocation *invocation,
                             GdmDisplay            *display)
{
        char *name;

        gdm_display_get_x11_display_name (display, &name, NULL);

        gdm_dbus_display_complete_get_x11_display_name (skeleton, invocation, name);

        g_free (name);
        return TRUE;
}

static gboolean
handle_get_x11_display_number (GdmDBusDisplay        *skeleton,
                               GDBusMethodInvocation *invocation,
                               GdmDisplay            *display)
{
        int name;

        gdm_display_get_x11_display_number (display, &name, NULL);

        gdm_dbus_display_complete_get_x11_display_number (skeleton, invocation, name);

        return TRUE;
}

static gboolean
handle_is_local (GdmDBusDisplay        *skeleton,
                 GDBusMethodInvocation *invocation,
                 GdmDisplay            *display)
{
        gboolean is_local;

        gdm_display_is_local (display, &is_local, NULL);

        gdm_dbus_display_complete_is_local (skeleton, invocation, is_local);

        return TRUE;
}

static gboolean
handle_is_initial (GdmDBusDisplay        *skeleton,
                   GDBusMethodInvocation *invocation,
                   GdmDisplay            *display)
{
        gboolean is_initial = FALSE;

        gdm_display_is_initial (display, &is_initial, NULL);

        gdm_dbus_display_complete_is_initial (skeleton, invocation, is_initial);

        return TRUE;
}

static gboolean
handle_get_slave_bus_name (GdmDBusDisplay        *skeleton,
                           GDBusMethodInvocation *invocation,
                           GdmDisplay            *display)
{
        if (display->priv->slave_bus_name != NULL) {
                gdm_dbus_display_complete_get_slave_bus_name (skeleton, invocation,
                                                              display->priv->slave_bus_name);
        } else {
                g_dbus_method_invocation_return_dbus_error (invocation,
                                                            "org.gnome.DisplayManager.NotReady",
                                                            "Slave is not yet registered");
        }

        return TRUE;
}

static gboolean
handle_set_slave_bus_name (GdmDBusDisplay        *skeleton,
                           GDBusMethodInvocation *invocation,
                           const char            *bus_name,
                           GdmDisplay            *display)
{
        GError *error = NULL;

        if (gdm_display_set_slave_bus_name (display, bus_name, &error)) {
                gdm_dbus_display_complete_set_slave_bus_name (skeleton, invocation);
        } else {
                g_dbus_method_invocation_return_gerror (invocation, error);
                g_error_free (error);
        }

        return TRUE;
}

static gboolean
handle_add_user_authorization (GdmDBusDisplay        *skeleton,
                               GDBusMethodInvocation *invocation,
                               const char            *username,
                               GdmDisplay            *display)
{
        char *filename;
        GError *error = NULL;

        if (gdm_display_add_user_authorization (display, username, &filename, &error)) {
                gdm_dbus_display_complete_add_user_authorization (skeleton,
                                                                  invocation,
                                                                  filename);
                g_free (filename);
        } else {
                g_dbus_method_invocation_return_gerror (invocation, error);
                g_error_free (error);
        }

        return TRUE;
}

static gboolean
handle_remove_user_authorization (GdmDBusDisplay        *skeleton,
                                  GDBusMethodInvocation *invocation,
                                  const char            *username,
                                  GdmDisplay            *display)
{
        GError *error = NULL;

        if (gdm_display_remove_user_authorization (display, username, &error)) {
                gdm_dbus_display_complete_remove_user_authorization (skeleton, invocation);
        } else {
                g_dbus_method_invocation_return_gerror (invocation, error);
                g_error_free (error);
        }

        return TRUE;
}

static gboolean
register_display (GdmDisplay *display)
{
        GError *error = NULL;

        error = NULL;
        display->priv->connection = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error);
        if (display->priv->connection == NULL) {
                g_critical ("error getting system bus: %s", error->message);
                g_error_free (error);
                exit (1);
        }

        display->priv->object_skeleton = g_dbus_object_skeleton_new (display->priv->id);
        display->priv->display_skeleton = GDM_DBUS_DISPLAY (gdm_dbus_display_skeleton_new ());

        g_signal_connect (display->priv->display_skeleton, "handle-get-id",
                          G_CALLBACK (handle_get_id), display);
        g_signal_connect (display->priv->display_skeleton, "handle-get-remote-hostname",
                          G_CALLBACK (handle_get_remote_hostname), display);
        g_signal_connect (display->priv->display_skeleton, "handle-get-seat-id",
                          G_CALLBACK (handle_get_seat_id), display);
        g_signal_connect (display->priv->display_skeleton, "handle-get-timed-login-details",
                          G_CALLBACK (handle_get_timed_login_details), display);
        g_signal_connect (display->priv->display_skeleton, "handle-get-x11-authority-file",
                          G_CALLBACK (handle_get_x11_authority_file), display);
        g_signal_connect (display->priv->display_skeleton, "handle-get-x11-cookie",
                          G_CALLBACK (handle_get_x11_cookie), display);
        g_signal_connect (display->priv->display_skeleton, "handle-get-x11-display-name",
                          G_CALLBACK (handle_get_x11_display_name), display);
        g_signal_connect (display->priv->display_skeleton, "handle-get-x11-display-number",
                          G_CALLBACK (handle_get_x11_display_number), display);
        g_signal_connect (display->priv->display_skeleton, "handle-is-local",
                          G_CALLBACK (handle_is_local), display);
        g_signal_connect (display->priv->display_skeleton, "handle-is-initial",
                          G_CALLBACK (handle_is_initial), display);
        g_signal_connect (display->priv->display_skeleton, "handle-get-slave-bus-name",
                          G_CALLBACK (handle_get_slave_bus_name), display);
        g_signal_connect (display->priv->display_skeleton, "handle-set-slave-bus-name",
                          G_CALLBACK (handle_set_slave_bus_name), display);
        g_signal_connect (display->priv->display_skeleton, "handle-add-user-authorization",
                          G_CALLBACK (handle_add_user_authorization), display);
        g_signal_connect (display->priv->display_skeleton, "handle-remove-user-authorization",
                          G_CALLBACK (handle_remove_user_authorization), display);

        g_dbus_object_skeleton_add_interface (display->priv->object_skeleton,
                                              G_DBUS_INTERFACE_SKELETON (display->priv->display_skeleton));

        return TRUE;
}

char *
gdm_display_open_session_sync (GdmDisplay    *display,
                               GPid           pid_of_caller,
                               uid_t          uid_of_caller,
                               GCancellable  *cancellable,
                               GError       **error)
{
        char *address;
        int ret;

        if (display->priv->slave_bus_proxy == NULL) {
                g_set_error (error,
                             G_DBUS_ERROR,
                             G_DBUS_ERROR_ACCESS_DENIED,
                             _("No session available yet"));
                return NULL;
        }

        address = NULL;
        ret = gdm_dbus_slave_call_open_session_sync (display->priv->slave_bus_proxy,
                                                     (int) pid_of_caller,
                                                     (int) uid_of_caller,
                                                     &address,
                                                     cancellable,
                                                     error);

        if (!ret) {
                return NULL;
        }

        return address;
}

char *
gdm_display_open_reauthentication_channel_sync (GdmDisplay    *display,
                                                const char    *username,
                                                GPid           pid_of_caller,
                                                uid_t          uid_of_caller,
                                                GCancellable  *cancellable,
                                                GError       **error)
{
        char *address;
        int ret;

        if (display->priv->slave_bus_proxy == NULL) {
                g_set_error (error,
                             G_DBUS_ERROR,
                             G_DBUS_ERROR_ACCESS_DENIED,
                             _("No session available yet"));
                return NULL;
        }

        address = NULL;
        ret = gdm_dbus_slave_call_open_reauthentication_channel_sync (display->priv->slave_bus_proxy,
                                                                      username,
                                                                      pid_of_caller,
                                                                      uid_of_caller,
                                                                      &address,
                                                                      cancellable,
                                                                      error);

        if (!ret) {
                return NULL;
        }

        return address;
}

/*
  dbus-send --system --print-reply --dest=org.gnome.DisplayManager /org/gnome/DisplayManager/Displays/1 org.freedesktop.DBus.Introspectable.Introspect
*/

static GObject *
gdm_display_constructor (GType                  type,
                         guint                  n_construct_properties,
                         GObjectConstructParam *construct_properties)
{
        GdmDisplay      *display;
        char            *canonical_display_name;
        gboolean         res;

        display = GDM_DISPLAY (G_OBJECT_CLASS (gdm_display_parent_class)->constructor (type,
                                                                                       n_construct_properties,
                                                                                       construct_properties));

        canonical_display_name = g_strdelimit (g_strdup (display->priv->x11_display_name),
                                               ":" G_STR_DELIMITERS, '_');

        g_free (display->priv->id);
        display->priv->id = g_strdup_printf ("/org/gnome/DisplayManager/Displays/%s",
                                             canonical_display_name);

        g_free (canonical_display_name);

        res = register_display (display);
        if (! res) {
                g_warning ("Unable to register display with system bus");
        }

        return G_OBJECT (display);
}

static void
gdm_display_dispose (GObject *object)
{
        GdmDisplay *display;

        display = GDM_DISPLAY (object);

        g_debug ("GdmDisplay: Disposing display");

        if (display->priv->finish_idle_id > 0) {
                g_source_remove (display->priv->finish_idle_id);
                display->priv->finish_idle_id = 0;
        }

        if (display->priv->slave_proxy != NULL) {
                gdm_slave_proxy_stop (display->priv->slave_proxy);

                g_object_unref (display->priv->slave_proxy);
                display->priv->slave_proxy = NULL;
        }

        if (display->priv->user_access_file != NULL) {
                gdm_display_access_file_close (display->priv->user_access_file);
                g_object_unref (display->priv->user_access_file);
                display->priv->user_access_file = NULL;
        }

        if (display->priv->access_file != NULL) {
                gdm_display_access_file_close (display->priv->access_file);
                g_object_unref (display->priv->access_file);
                display->priv->access_file = NULL;
        }

        if (display->priv->slave_name_id > 0) {
                g_bus_unwatch_name (display->priv->slave_name_id);
                display->priv->slave_name_id = 0;
        }

        G_OBJECT_CLASS (gdm_display_parent_class)->dispose (object);
}

static void
gdm_display_class_init (GdmDisplayClass *klass)
{
        GObjectClass   *object_class = G_OBJECT_CLASS (klass);

        object_class->get_property = gdm_display_get_property;
        object_class->set_property = gdm_display_set_property;
        object_class->constructor = gdm_display_constructor;
        object_class->dispose = gdm_display_dispose;
        object_class->finalize = gdm_display_finalize;

        klass->create_authority = gdm_display_real_create_authority;
        klass->add_user_authorization = gdm_display_real_add_user_authorization;
        klass->remove_user_authorization = gdm_display_real_remove_user_authorization;
        klass->set_slave_bus_name = gdm_display_real_set_slave_bus_name;
        klass->get_timed_login_details = gdm_display_real_get_timed_login_details;
        klass->prepare = gdm_display_real_prepare;
        klass->manage = gdm_display_real_manage;
        klass->finish = gdm_display_real_finish;
        klass->unmanage = gdm_display_real_unmanage;

        g_object_class_install_property (object_class,
                                         PROP_ID,
                                         g_param_spec_string ("id",
                                                              "id",
                                                              "id",
                                                              NULL,
                                                              G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
        g_object_class_install_property (object_class,
                                         PROP_REMOTE_HOSTNAME,
                                         g_param_spec_string ("remote-hostname",
                                                              "remote-hostname",
                                                              "remote-hostname",
                                                              NULL,
                                                              G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
        g_object_class_install_property (object_class,
                                         PROP_X11_DISPLAY_NUMBER,
                                         g_param_spec_int ("x11-display-number",
                                                          "x11 display number",
                                                          "x11 display number",
                                                          -1,
                                                          G_MAXINT,
                                                          -1,
                                                          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
        g_object_class_install_property (object_class,
                                         PROP_X11_DISPLAY_NAME,
                                         g_param_spec_string ("x11-display-name",
                                                              "x11-display-name",
                                                              "x11-display-name",
                                                              NULL,
                                                              G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
        g_object_class_install_property (object_class,
                                         PROP_SEAT_ID,
                                         g_param_spec_string ("seat-id",
                                                              "seat id",
                                                              "seat id",
                                                              NULL,
                                                              G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
        g_object_class_install_property (object_class,
                                         PROP_SESSION_ID,
                                         g_param_spec_string ("session-id",
                                                              "session id",
                                                              "session id",
                                                              NULL,
                                                              G_PARAM_READWRITE));
        g_object_class_install_property (object_class,
                                         PROP_IS_INITIAL,
                                         g_param_spec_boolean ("is-initial",
                                                               NULL,
                                                               NULL,
                                                               FALSE,
                                                               G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
        g_object_class_install_property (object_class,
                                         PROP_X11_COOKIE,
                                         g_param_spec_string ("x11-cookie",
                                                              "cookie",
                                                              "cookie",
                                                              NULL,
                                                              G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
        g_object_class_install_property (object_class,
                                         PROP_X11_AUTHORITY_FILE,
                                         g_param_spec_string ("x11-authority-file",
                                                              "authority file",
                                                              "authority file",
                                                              NULL,
                                                              G_PARAM_READABLE));

        g_object_class_install_property (object_class,
                                         PROP_IS_LOCAL,
                                         g_param_spec_boolean ("is-local",
                                                               NULL,
                                                               NULL,
                                                               TRUE,
                                                               G_PARAM_READWRITE | G_PARAM_CONSTRUCT));

        g_object_class_install_property (object_class,
                                         PROP_SLAVE_COMMAND,
                                         g_param_spec_string ("slave-command",
                                                              "slave command",
                                                              "slave command",
                                                              DEFAULT_SLAVE_COMMAND,
                                                              G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
        g_object_class_install_property (object_class,
                                         PROP_STATUS,
                                         g_param_spec_int ("status",
                                                           "status",
                                                           "status",
                                                           -1,
                                                           G_MAXINT,
                                                           GDM_DISPLAY_UNMANAGED,
                                                           G_PARAM_READWRITE | G_PARAM_CONSTRUCT));

        g_type_class_add_private (klass, sizeof (GdmDisplayPrivate));
}

static void
gdm_display_init (GdmDisplay *display)
{

        display->priv = GDM_DISPLAY_GET_PRIVATE (display);

        display->priv->creation_time = time (NULL);
        display->priv->slave_timer = g_timer_new ();
}

static void
gdm_display_finalize (GObject *object)
{
        GdmDisplay *display;

        g_return_if_fail (object != NULL);
        g_return_if_fail (GDM_IS_DISPLAY (object));

        display = GDM_DISPLAY (object);

        g_return_if_fail (display->priv != NULL);

        g_debug ("GdmDisplay: Finalizing display: %s", display->priv->id);
        g_free (display->priv->id);
        g_free (display->priv->seat_id);
        g_free (display->priv->remote_hostname);
        g_free (display->priv->x11_display_name);
        g_free (display->priv->x11_cookie);
        g_free (display->priv->slave_command);
        g_free (display->priv->slave_bus_name);

        g_clear_object (&display->priv->display_skeleton);
        g_clear_object (&display->priv->object_skeleton);
        g_clear_object (&display->priv->connection);

        if (display->priv->access_file != NULL) {
                g_object_unref (display->priv->access_file);
        }

        if (display->priv->user_access_file != NULL) {
                g_object_unref (display->priv->user_access_file);
        }

        if (display->priv->slave_timer != NULL) {
                g_timer_destroy (display->priv->slave_timer);
        }

        G_OBJECT_CLASS (gdm_display_parent_class)->finalize (object);
}

GDBusConnection *
gdm_display_get_bus_connection (GdmDisplay *display)
{
        return display->priv->connection;
}

GDBusObjectSkeleton *
gdm_display_get_object_skeleton (GdmDisplay *display)
{
        return display->priv->object_skeleton;
}
