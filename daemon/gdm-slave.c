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
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <pwd.h>
#include <grp.h>
#include <signal.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <glib-object.h>

#include <X11/Xlib.h> /* for Display */
#include <X11/Xatom.h> /* for XA_PIXMAP */
#include <X11/cursorfont.h> /* for watch cursor */
#include <X11/Xatom.h>

#ifdef WITH_SYSTEMD
#include <systemd/sd-login.h>
#endif

#include "gdm-common.h"
#include "gdm-xerrors.h"

#include "gdm-slave.h"
#include "gdm-slave-glue.h"
#include "gdm-display-glue.h"

#include "gdm-server.h"

#define GDM_SLAVE_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), GDM_TYPE_SLAVE, GdmSlavePrivate))

#define CK_NAME      "org.freedesktop.ConsoleKit"
#define CK_PATH      "/org/freedesktop/ConsoleKit"
#define CK_INTERFACE "org.freedesktop.ConsoleKit"

#define CK_MANAGER_PATH      "/org/freedesktop/ConsoleKit/Manager"
#define CK_MANAGER_INTERFACE "org.freedesktop.ConsoleKit.Manager"
#define CK_SEAT_INTERFACE    "org.freedesktop.ConsoleKit.Seat"
#define CK_SESSION_INTERFACE "org.freedesktop.ConsoleKit.Session"

#define GDM_DBUS_NAME              "org.gnome.DisplayManager"
#define GDM_DBUS_DISPLAY_INTERFACE "org.gnome.DisplayManager.Display"

#define GDM_SLAVE_PATH "/org/gnome/DisplayManager/Slave"

#define MAX_CONNECT_ATTEMPTS 10

struct GdmSlavePrivate
{
        GPid             pid;
        guint            output_watch_id;
        guint            error_watch_id;

        Display         *server_display;

        char            *session_id;

        /* cached display values */
        char            *display_id;
        char            *display_name;
        int              display_number;
        char            *display_hostname;
        gboolean         display_is_local;
        gboolean         display_is_parented;
        char            *display_seat_id;
        char            *display_x11_authority_file;
        char            *parent_display_name;
        char            *parent_display_x11_authority_file;
        char            *windowpath;
        GBytes          *display_x11_cookie;
        gboolean         display_is_initial;

        GdmDBusDisplay  *display_proxy;
        GDBusConnection *connection;
        GdmDBusSlave    *skeleton;
};

enum {
        PROP_0,
        PROP_SESSION_ID,
        PROP_DISPLAY_ID,
        PROP_DISPLAY_NAME,
        PROP_DISPLAY_NUMBER,
        PROP_DISPLAY_HOSTNAME,
        PROP_DISPLAY_IS_LOCAL,
        PROP_DISPLAY_SEAT_ID,
        PROP_DISPLAY_X11_AUTHORITY_FILE,
        PROP_DISPLAY_IS_INITIAL,
};

enum {
        STOPPED,
        LAST_SIGNAL
};

static guint signals [LAST_SIGNAL] = { 0, };

static void     gdm_slave_class_init    (GdmSlaveClass *klass);
static void     gdm_slave_init          (GdmSlave      *slave);
static void     gdm_slave_finalize      (GObject       *object);

G_DEFINE_ABSTRACT_TYPE (GdmSlave, gdm_slave, G_TYPE_OBJECT)

#define CURSOR_WATCH XC_watch

GQuark
gdm_slave_error_quark (void)
{
        static GQuark ret = 0;
        if (ret == 0) {
                ret = g_quark_from_static_string ("gdm-slave-error-quark");
        }

        return ret;
}

static void
gdm_slave_whack_temp_auth_file (GdmSlave *slave)
{
#if 0
        uid_t old;

        old = geteuid ();
        if (old != 0)
                seteuid (0);
        if (d->parent_temp_auth_file != NULL) {
                VE_IGNORE_EINTR (g_unlink (d->parent_temp_auth_file));
        }
        g_free (d->parent_temp_auth_file);
        d->parent_temp_auth_file = NULL;
        if (old != 0)
                seteuid (old);
#endif
}


static void
create_temp_auth_file (GdmSlave *slave)
{
#if 0
        if (d->type == TYPE_FLEXI_XNEST &&
            d->parent_auth_file != NULL) {
                if (d->parent_temp_auth_file != NULL) {
                        VE_IGNORE_EINTR (g_unlink (d->parent_temp_auth_file));
                }
                g_free (d->parent_temp_auth_file);
                d->parent_temp_auth_file =
                        copy_auth_file (d->server_uid,
                                        gdm_daemon_config_get_gdmuid (),
                                        d->parent_auth_file);
        }
#endif
}

static void
listify_hash (const char *key,
              const char *value,
              GPtrArray  *env)
{
        char *str;
        str = g_strdup_printf ("%s=%s", key, value);
        g_debug ("GdmSlave: script environment: %s", str);
        g_ptr_array_add (env, str);
}

static GPtrArray *
get_script_environment (GdmSlave   *slave,
                        const char *username)
{
        GPtrArray     *env;
        GHashTable    *hash;
        struct passwd *pwent;

        env = g_ptr_array_new ();

        /* create a hash table of current environment, then update keys has necessary */
        hash = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

        /* modify environment here */
        g_hash_table_insert (hash, g_strdup ("HOME"), g_strdup ("/"));
        g_hash_table_insert (hash, g_strdup ("PWD"), g_strdup ("/"));
        g_hash_table_insert (hash, g_strdup ("SHELL"), g_strdup ("/bin/sh"));

        if (username != NULL) {
                g_hash_table_insert (hash, g_strdup ("LOGNAME"),
                                     g_strdup (username));
                g_hash_table_insert (hash, g_strdup ("USER"),
                                     g_strdup (username));
                g_hash_table_insert (hash, g_strdup ("USERNAME"),
                                     g_strdup (username));

                gdm_get_pwent_for_name (username, &pwent);
                if (pwent != NULL) {
                        if (pwent->pw_dir != NULL && pwent->pw_dir[0] != '\0') {
                                g_hash_table_insert (hash, g_strdup ("HOME"),
                                                     g_strdup (pwent->pw_dir));
                                g_hash_table_insert (hash, g_strdup ("PWD"),
                                                     g_strdup (pwent->pw_dir));
                        }

                        g_hash_table_insert (hash, g_strdup ("SHELL"),
                                             g_strdup (pwent->pw_shell));
                }
        }

#if 0
        if (display_is_parented) {
                g_hash_table_insert (hash, g_strdup ("GDM_PARENT_DISPLAY"), g_strdup (parent_display_name));

                /*g_hash_table_insert (hash, "GDM_PARENT_XAUTHORITY"), slave->priv->parent_temp_auth_file));*/
        }
#endif

        if (! slave->priv->display_is_local) {
                g_hash_table_insert (hash, g_strdup ("REMOTE_HOST"), g_strdup (slave->priv->display_hostname));
        }

        /* Runs as root */
        g_hash_table_insert (hash, g_strdup ("XAUTHORITY"), g_strdup (slave->priv->display_x11_authority_file));
        g_hash_table_insert (hash, g_strdup ("DISPLAY"), g_strdup (slave->priv->display_name));
        g_hash_table_insert (hash, g_strdup ("PATH"), g_strdup (GDM_SESSION_DEFAULT_PATH));
        g_hash_table_insert (hash, g_strdup ("RUNNING_UNDER_GDM"), g_strdup ("true"));

        g_hash_table_remove (hash, "MAIL");


        g_hash_table_foreach (hash, (GHFunc)listify_hash, env);
        g_hash_table_destroy (hash);

        g_ptr_array_add (env, NULL);

        return env;
}

gboolean
gdm_slave_run_script (GdmSlave   *slave,
                      const char *dir,
                      const char *login)
{
        char      *script;
        char     **argv;
        gint       status;
        GError    *error;
        GPtrArray *env;
        gboolean   res;
        gboolean   ret;

        ret = FALSE;

        g_assert (dir != NULL);
        g_assert (login != NULL);

        script = g_build_filename (dir, slave->priv->display_name, NULL);
        g_debug ("GdmSlave: Trying script %s", script);
        if (! (g_file_test (script, G_FILE_TEST_IS_REGULAR)
               && g_file_test (script, G_FILE_TEST_IS_EXECUTABLE))) {
                g_debug ("GdmSlave: script %s not found; skipping", script);
                g_free (script);
                script = NULL;
        }

        if (script == NULL
            && slave->priv->display_hostname != NULL
            && slave->priv->display_hostname[0] != '\0') {
                script = g_build_filename (dir, slave->priv->display_hostname, NULL);
                g_debug ("GdmSlave: Trying script %s", script);
                if (! (g_file_test (script, G_FILE_TEST_IS_REGULAR)
                       && g_file_test (script, G_FILE_TEST_IS_EXECUTABLE))) {
                        g_debug ("GdmSlave: script %s not found; skipping", script);
                        g_free (script);
                        script = NULL;
                }
        }

        if (script == NULL) {
                script = g_build_filename (dir, "Default", NULL);
                g_debug ("GdmSlave: Trying script %s", script);
                if (! (g_file_test (script, G_FILE_TEST_IS_REGULAR)
                       && g_file_test (script, G_FILE_TEST_IS_EXECUTABLE))) {
                        g_debug ("GdmSlave: script %s not found; skipping", script);
                        g_free (script);
                        script = NULL;
                }
        }

        if (script == NULL) {
                g_debug ("GdmSlave: no script found");
                return TRUE;
        }

        create_temp_auth_file (slave);

        g_debug ("GdmSlave: Running process: %s", script);
        error = NULL;
        if (! g_shell_parse_argv (script, NULL, &argv, &error)) {
                g_warning ("Could not parse command: %s", error->message);
                g_error_free (error);
                goto out;
        }

        env = get_script_environment (slave, login);

        res = g_spawn_sync (NULL,
                            argv,
                            (char **)env->pdata,
                            G_SPAWN_SEARCH_PATH,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            &status,
                            &error);

        g_ptr_array_foreach (env, (GFunc)g_free, NULL);
        g_ptr_array_free (env, TRUE);
        g_strfreev (argv);

        if (! res) {
                g_warning ("GdmSlave: Unable to run script: %s", error->message);
                g_error_free (error);
        }

        gdm_slave_whack_temp_auth_file (slave);

        if (WIFEXITED (status)) {
                g_debug ("GdmSlave: Process exit status: %d", WEXITSTATUS (status));
                ret = WEXITSTATUS (status) == 0;
        }

 out:
        g_free (script);

        return ret;
}

static void
gdm_slave_setup_xhost_auth (XHostAddress *host_entries, XServerInterpretedAddress *si_entries)
{
        si_entries[0].type        = "localuser";
        si_entries[0].typelength  = strlen ("localuser");
        si_entries[1].type        = "localuser";
        si_entries[1].typelength  = strlen ("localuser");
        si_entries[2].type        = "localuser";
        si_entries[2].typelength  = strlen ("localuser");

        si_entries[0].value       = "root";
        si_entries[0].valuelength = strlen ("root");
        si_entries[1].value       = GDM_USERNAME;
        si_entries[1].valuelength = strlen (GDM_USERNAME);
        si_entries[2].value       = "gnome-initial-setup";
        si_entries[2].valuelength = strlen ("gnome-initial-setup");

        host_entries[0].family    = FamilyServerInterpreted;
        host_entries[0].address   = (char *) &si_entries[0];
        host_entries[0].length    = sizeof (XServerInterpretedAddress);
        host_entries[1].family    = FamilyServerInterpreted;
        host_entries[1].address   = (char *) &si_entries[1];
        host_entries[1].length    = sizeof (XServerInterpretedAddress);
        host_entries[2].family    = FamilyServerInterpreted;
        host_entries[2].address   = (char *) &si_entries[2];
        host_entries[2].length    = sizeof (XServerInterpretedAddress);
}

static void
gdm_slave_set_windowpath (GdmSlave *slave)
{
        /* setting WINDOWPATH for clients */
        Atom prop;
        Atom actualtype;
        int actualformat;
        unsigned long nitems;
        unsigned long bytes_after;
        unsigned char *buf;
        const char *windowpath;
        char *newwindowpath;
        unsigned long num;
        char nums[10];
        int numn;

        prop = XInternAtom (slave->priv->server_display, "XFree86_VT", False);
        if (prop == None) {
                g_debug ("no XFree86_VT atom\n");
                return;
        }
        if (XGetWindowProperty (slave->priv->server_display,
                DefaultRootWindow (slave->priv->server_display), prop, 0, 1,
                False, AnyPropertyType, &actualtype, &actualformat,
                &nitems, &bytes_after, &buf)) {
                g_debug ("no XFree86_VT property\n");
                return;
        }

        if (nitems != 1) {
                g_debug ("%lu items in XFree86_VT property!\n", nitems);
                XFree (buf);
                return;
        }

        switch (actualtype) {
        case XA_CARDINAL:
        case XA_INTEGER:
        case XA_WINDOW:
                switch (actualformat) {
                case  8:
                        num = (*(uint8_t  *)(void *)buf);
                        break;
                case 16:
                        num = (*(uint16_t *)(void *)buf);
                        break;
                case 32:
                        num = (*(long *)(void *)buf);
                        break;
                default:
                        g_debug ("format %d in XFree86_VT property!\n", actualformat);
                        XFree (buf);
                        return;
                }
                break;
        default:
                g_debug ("type %lx in XFree86_VT property!\n", actualtype);
                XFree (buf);
                return;
        }
        XFree (buf);

        windowpath = getenv ("WINDOWPATH");
        numn = snprintf (nums, sizeof (nums), "%lu", num);
        if (!windowpath) {
                newwindowpath = malloc (numn + 1);
                sprintf (newwindowpath, "%s", nums);
        } else {
                newwindowpath = malloc (strlen (windowpath) + 1 + numn + 1);
                sprintf (newwindowpath, "%s:%s", windowpath, nums);
        }

        slave->priv->windowpath = newwindowpath;

        g_setenv ("WINDOWPATH", newwindowpath, TRUE);
}

gboolean
gdm_slave_connect_to_x11_display (GdmSlave *slave)
{
        gboolean ret;
        sigset_t mask;
        sigset_t omask;

        ret = FALSE;

        /* We keep our own (windowless) connection (dsp) open to avoid the
         * X server resetting due to lack of active connections. */

        g_debug ("GdmSlave: Server is ready - opening display %s", slave->priv->display_name);

        g_setenv ("DISPLAY", slave->priv->display_name, TRUE);
        g_setenv ("XAUTHORITY", slave->priv->display_x11_authority_file, TRUE);

        sigemptyset (&mask);
        sigaddset (&mask, SIGCHLD);
        sigprocmask (SIG_BLOCK, &mask, &omask);

        /* Give slave access to the display independent of current hostname */
        if (slave->priv->display_x11_cookie != NULL) {
                XSetAuthorization ("MIT-MAGIC-COOKIE-1",
                                   strlen ("MIT-MAGIC-COOKIE-1"),
                                   (gpointer)
                                   g_bytes_get_data (slave->priv->display_x11_cookie, NULL),
                                   g_bytes_get_size (slave->priv->display_x11_cookie));
        }

        slave->priv->server_display = XOpenDisplay (slave->priv->display_name);

        sigprocmask (SIG_SETMASK, &omask, NULL);


        if (slave->priv->server_display == NULL) {
                g_warning ("Unable to connect to display %s", slave->priv->display_name);
                ret = FALSE;
        } else if (slave->priv->display_is_local) {
                XServerInterpretedAddress si_entries[3];
                XHostAddress              host_entries[3];
                int                       i;

                g_debug ("GdmSlave: Connected to display %s", slave->priv->display_name);
                ret = TRUE;

                /* Give programs run by the slave and greeter access to the
                 * display independent of current hostname
                 */
                gdm_slave_setup_xhost_auth (host_entries, si_entries);

                gdm_error_trap_push ();

                for (i = 0; i < G_N_ELEMENTS (host_entries); i++) {
                        XAddHost (slave->priv->server_display, &host_entries[i]);
                }

                XSync (slave->priv->server_display, False);
                if (gdm_error_trap_pop ()) {
                        g_warning ("Failed to give slave programs access to the display. Trying to proceed.");
                }

                gdm_slave_set_windowpath (slave);
        } else {
                g_debug ("GdmSlave: Connected to display %s", slave->priv->display_name);
                ret = TRUE;
        }

        return ret;
}

static gboolean
gdm_slave_set_slave_bus_name (GdmSlave *slave)
{
        gboolean    res;
        GError     *error;
        const char *name;

        name = g_dbus_connection_get_unique_name (slave->priv->connection);

        error = NULL;
        res = gdm_dbus_display_call_set_slave_bus_name_sync (slave->priv->display_proxy,
                                                             name,
                                                             NULL,
                                                             &error);
        if (! res) {
                g_warning ("Failed to set slave bus name on parent: %s", error->message);
                g_error_free (error);
        }

        return res;
}

static gboolean
gdm_slave_real_start (GdmSlave *slave)
{
        gboolean    res;
        char       *id;
        GError     *error;
        GVariant   *x11_cookie;
        const char *x11_cookie_bytes;
        gsize       x11_cookie_size;

        g_debug ("GdmSlave: Starting slave");

        g_assert (slave->priv->display_proxy == NULL);

        g_debug ("GdmSlave: Creating proxy for %s", slave->priv->display_id);
        error = NULL;
        slave->priv->display_proxy = GDM_DBUS_DISPLAY (gdm_dbus_display_proxy_new_sync (slave->priv->connection,
                                                                                        G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                                                                        GDM_DBUS_NAME,
                                                                                        slave->priv->display_id,
                                                                                        NULL,
                                                                                        &error));

        if (slave->priv->display_proxy == NULL) {
                g_warning ("Failed to create display proxy %s: %s", slave->priv->display_id, error->message);
                g_error_free (error);
                return FALSE;
        }

        error = NULL;
        res = gdm_dbus_display_call_get_id_sync (slave->priv->display_proxy,
                                                 &id,
                                                 NULL,
                                                 &error);
        if (! res) {
                g_warning ("Failed to get display ID %s: %s", slave->priv->display_id, error->message);
                g_error_free (error);
                return FALSE;
        }

        g_debug ("GdmSlave: Got display ID: %s", id);

        if (strcmp (id, slave->priv->display_id) != 0) {
                g_critical ("Display ID doesn't match");
                exit (1);
        }

        gdm_slave_set_slave_bus_name (slave);

        /* cache some values up front */
        error = NULL;
        res = gdm_dbus_display_call_is_local_sync (slave->priv->display_proxy,
                                                   &slave->priv->display_is_local,
                                                   NULL,
                                                   &error);
        if (! res) {
                g_warning ("Failed to get value: %s", error->message);
                g_error_free (error);
                return FALSE;
        }

        error = NULL;
        res = gdm_dbus_display_call_get_x11_display_name_sync (slave->priv->display_proxy,
                                                               &slave->priv->display_name,
                                                               NULL,
                                                               &error);
        if (! res) {
                g_warning ("Failed to get value: %s", error->message);
                g_error_free (error);
                return FALSE;
        }

        error = NULL;
        res = gdm_dbus_display_call_get_x11_display_number_sync (slave->priv->display_proxy,
                                                                 &slave->priv->display_number,
                                                                 NULL,
                                                                 &error);
        if (! res) {
                g_warning ("Failed to get value: %s", error->message);
                g_error_free (error);
                return FALSE;
        }

        error = NULL;
        res = gdm_dbus_display_call_get_remote_hostname_sync (slave->priv->display_proxy,
                                                              &slave->priv->display_hostname,
                                                              NULL,
                                                              &error);
        if (! res) {
                g_warning ("Failed to get value: %s", error->message);
                g_error_free (error);
                return FALSE;
        }

        error = NULL;
        res = gdm_dbus_display_call_get_x11_cookie_sync (slave->priv->display_proxy,
                                                         &x11_cookie,
                                                         NULL,
                                                         &error);
        if (! res) {
                g_warning ("Failed to get value: %s", error->message);
                g_error_free (error);
                return FALSE;
        }

        x11_cookie_bytes = g_variant_get_fixed_array (x11_cookie,
                                                      &x11_cookie_size,
                                                      sizeof (char));

        if (x11_cookie_bytes != NULL && x11_cookie_size > 0) {
                g_bytes_unref (slave->priv->display_x11_cookie);
                slave->priv->display_x11_cookie = g_bytes_new (x11_cookie_bytes,
                                                               x11_cookie_size);
        }

        g_variant_unref (x11_cookie);

        error = NULL;
        res = gdm_dbus_display_call_get_x11_authority_file_sync (slave->priv->display_proxy,
                                                                 &slave->priv->display_x11_authority_file,
                                                                 NULL,
                                                                 &error);
        if (! res) {
                g_warning ("Failed to get value: %s", error->message);
                g_error_free (error);
                return FALSE;
        }

        error = NULL;
        res = gdm_dbus_display_call_get_seat_id_sync (slave->priv->display_proxy,
                                                      &slave->priv->display_seat_id,
                                                      NULL,
                                                      &error);
        if (! res) {
                g_warning ("Failed to get value: %s", error->message);
                g_error_free (error);
                return FALSE;
        }

        error = NULL;
        res = gdm_dbus_display_call_is_initial_sync (slave->priv->display_proxy,
                                                     &slave->priv->display_is_initial,
                                                     NULL,
                                                     &error);
        if (! res) {
                g_warning ("Failed to get value: %s", error->message);
                g_error_free (error);
                return FALSE;
        }

        return TRUE;
}

static gboolean
gdm_slave_real_stop (GdmSlave *slave)
{
        g_debug ("GdmSlave: Stopping slave");

        g_clear_object (&slave->priv->display_proxy);

        return TRUE;
}

gboolean
gdm_slave_start (GdmSlave *slave)
{
        gboolean ret;

        g_return_val_if_fail (GDM_IS_SLAVE (slave), FALSE);

        g_debug ("GdmSlave: starting slave");

        g_object_ref (slave);
        ret = GDM_SLAVE_GET_CLASS (slave)->start (slave);
        g_object_unref (slave);

        return ret;
}

gboolean
gdm_slave_stop (GdmSlave *slave)
{
        gboolean ret;

        g_return_val_if_fail (GDM_IS_SLAVE (slave), FALSE);

        g_debug ("GdmSlave: stopping slave");

        g_object_ref (slave);

        ret = GDM_SLAVE_GET_CLASS (slave)->stop (slave);
        g_signal_emit (slave, signals [STOPPED], 0);

        g_object_unref (slave);
        return ret;
}

gboolean
gdm_slave_add_user_authorization (GdmSlave   *slave,
                                  const char *username,
                                  char      **filenamep)
{
        XServerInterpretedAddress si_entries[3];
        XHostAddress              host_entries[3];
        int                       i;
        gboolean                  res;
        GError                   *error;
        char                     *filename;

        filename = NULL;

        if (filenamep != NULL) {
                *filenamep = NULL;
        }

        g_debug ("GdmSlave: Requesting user authorization");

        error = NULL;
        res = gdm_dbus_display_call_add_user_authorization_sync (slave->priv->display_proxy,
                                                                 username,
                                                                 &filename,
                                                                 NULL,
                                                                 &error);

        if (! res) {
                g_warning ("Failed to add user authorization: %s", error->message);
                g_error_free (error);
        } else {
                g_debug ("GdmSlave: Got user authorization: %s", filename);
        }

        if (filenamep != NULL) {
                *filenamep = g_strdup (filename);
        }
        g_free (filename);

        /* Remove access for the programs run by slave and greeter now that the
         * user session is starting.
         */
        gdm_slave_setup_xhost_auth (host_entries, si_entries);
        gdm_error_trap_push ();
        for (i = 0; i < G_N_ELEMENTS (host_entries); i++) {
                XRemoveHost (slave->priv->server_display, &host_entries[i]);
        }
        XSync (slave->priv->server_display, False);
        if (gdm_error_trap_pop ()) {
                g_warning ("Failed to remove slave program access to the display. Trying to proceed.");
        }

        return res;
}

static char *
gdm_slave_parse_enriched_login (GdmSlave   *slave,
                                const char *username,
                                const char *display_name)
{
        char     **argv;
        int        username_len;
        GPtrArray *env;
        GError    *error;
        gboolean   res;
        char      *parsed_username;
        char      *command;
        char      *std_output;
        char      *std_error;

        parsed_username = NULL;

        if (username == NULL || username[0] == '\0') {
                return NULL;
        }

        /* A script may be used to generate the automatic/timed login name
           based on the display/host by ending the name with the pipe symbol
           '|'. */

        username_len = strlen (username);
        if (username[username_len - 1] != '|') {
                return g_strdup (username);
        }

        /* Remove the pipe symbol */
        command = g_strndup (username, username_len - 1);

        argv = NULL;
        error = NULL;
        if (! g_shell_parse_argv (command, NULL, &argv, &error)) {
                g_warning ("GdmSlave: Could not parse command '%s': %s", command, error->message);
                g_error_free (error);

                g_free (command);
                goto out;
        }

        g_debug ("GdmSlave: running '%s' to acquire auto/timed username", command);
        g_free (command);

        env = get_script_environment (slave, NULL);

        error = NULL;
        std_output = NULL;
        std_error = NULL;
        res = g_spawn_sync (NULL,
                            argv,
                            (char **)env->pdata,
                            G_SPAWN_SEARCH_PATH,
                            NULL,
                            NULL,
                            &std_output,
                            &std_error,
                            NULL,
                            &error);

        g_ptr_array_foreach (env, (GFunc)g_free, NULL);
        g_ptr_array_free (env, TRUE);
        g_strfreev (argv);

        if (! res) {
                g_warning ("GdmSlave: Unable to launch auto/timed login script '%s': %s", username, error->message);
                g_error_free (error);

                g_free (std_output);
                g_free (std_error);
                goto out;
        }

        if (std_output != NULL) {
                g_strchomp (std_output);
                if (std_output[0] != '\0') {
                        parsed_username = g_strdup (std_output);
                }
        }

 out:

        return parsed_username;
}

gboolean
gdm_slave_get_timed_login_details (GdmSlave   *slave,
                                   gboolean   *enabledp,
                                   char      **usernamep,
                                   int        *delayp)
{
        struct passwd *pwent;
        GError        *error;
        gboolean       res;
        gboolean       enabled;
        char          *username;
        int            delay;

        username = NULL;
        enabled = FALSE;
        delay = 0;

        g_debug ("GdmSlave: Requesting timed login details");

        error = NULL;
        res = gdm_dbus_display_call_get_timed_login_details_sync (slave->priv->display_proxy,
                                                                  &enabled,
                                                                  &username,
                                                                  &delay,
                                                                  NULL,
                                                                  &error);
        if (! res) {
                g_warning ("Failed to get timed login details: %s", error->message);
                g_error_free (error);
        } else {
                g_debug ("GdmSlave: Got timed login details: %d %s %d", enabled, username, delay);
        }

        if (usernamep != NULL) {
                *usernamep = gdm_slave_parse_enriched_login (slave,
                                                             username,
                                                             slave->priv->display_name);
        } else {
                g_free (username);

                if (enabledp != NULL) {
                        *enabledp = enabled;
                }
                if (delayp != NULL) {
                        *delayp = delay;
                }
                return TRUE;
        }
        g_free (username);

        if (usernamep != NULL && *usernamep != NULL) {
                gdm_get_pwent_for_name (*usernamep, &pwent);
                if (pwent == NULL) {
                        g_debug ("Invalid username %s for auto/timed login",
                                 *usernamep);
                        g_free (*usernamep);
                        *usernamep = NULL;
                } else {
                        g_debug ("Using username %s for auto/timed login",
                                 *usernamep);

                        if (enabledp != NULL) {
                                *enabledp = enabled;
                        }
                        if (delayp != NULL) {
                                *delayp = delay;
                        }
               }
        } else {
                g_debug ("Invalid NULL username for auto/timed login");
        }

        return res;
}

static gboolean
_get_uid_and_gid_for_user (const char *username,
                           uid_t      *uid,
                           gid_t      *gid)
{
        struct passwd *passwd_entry;

        g_assert (username != NULL);

        errno = 0;
        gdm_get_pwent_for_name (username, &passwd_entry);

        if (passwd_entry == NULL) {
                return FALSE;
        }

        if (uid != NULL) {
                *uid = passwd_entry->pw_uid;
        }

        if (gid != NULL) {
                *gid = passwd_entry->pw_gid;
        }

        return TRUE;
}

#ifdef WITH_CONSOLE_KIT

static gboolean
x11_session_is_on_seat (GdmSlave        *slave,
                        const char      *session_id,
                        const char      *seat_id)
{
        GError          *error = NULL;
        GVariant        *reply;
        char            *sid;
        gboolean         ret;
        char            *x11_display_device;
        char            *x11_display;

        ret = FALSE;
        sid = NULL;
        x11_display = NULL;
        x11_display_device = NULL;

        if (seat_id == NULL || seat_id[0] == '\0' || session_id == NULL || session_id[0] == '\0') {
                return FALSE;
        }

        reply = g_dbus_connection_call_sync (slave->priv->connection,
                                             CK_NAME,
                                             session_id,
                                             CK_SESSION_INTERFACE,
                                             "GetSeatId",
                                             NULL, /* parameters */
                                             G_VARIANT_TYPE ("(o)"),
                                             G_DBUS_CALL_FLAGS_NONE,
                                             -1,
                                             NULL,
                                             &error);
        if (reply == NULL) {
                g_debug ("Failed to identify the current seat: %s", error->message);
                g_error_free (error);
                return FALSE;
        }

        g_variant_get (reply, "(o)", &sid);
        g_variant_unref (reply);

        if (sid == NULL || sid[0] == '\0' || strcmp (sid, seat_id) != 0) {
                g_debug ("GdmSlave: session not on current seat: %s", seat_id);
                goto out;
        }

        reply = g_dbus_connection_call_sync (slave->priv->connection,
                                             CK_NAME,
                                             session_id,
                                             CK_SESSION_INTERFACE,
                                             "GetX11Display",
                                             NULL, /* parameters */
                                             G_VARIANT_TYPE ("(s)"),
                                             G_DBUS_CALL_FLAGS_NONE,
                                             -1,
                                             NULL,
                                             &error);
        if (reply == NULL) {
                g_error_free (error);
                goto out;
        }

        g_variant_get (reply, "(s)", &x11_display);
        g_variant_unref (reply);

        /* ignore tty sessions */
        if (x11_display == NULL || x11_display[0] == '\0') {
                goto out;
        }

        reply = g_dbus_connection_call_sync (slave->priv->connection,
                                             CK_NAME,
                                             session_id,
                                             CK_SESSION_INTERFACE,
                                             "GetX11DisplayDevice",
                                             NULL, /* parameters */
                                             G_VARIANT_TYPE ("(s)"),
                                             G_DBUS_CALL_FLAGS_NONE,
                                             -1,
                                             NULL,
                                             &error);
        if (reply == NULL) {
                g_error_free (error);
                goto out;
        }

        g_variant_get (reply, "(s)", &x11_display_device);
        g_variant_unref (reply);

        if (x11_display_device == NULL || x11_display_device[0] == '\0') {
                goto out;
        }

        ret = TRUE;
 out:
        g_free (x11_display_device);
        g_free (x11_display);
        g_free (sid);

        return ret;
}

#endif

#ifdef WITH_SYSTEMD
static char*
gdm_slave_get_primary_session_id_for_user_from_systemd (GdmSlave   *slave,
                                                        const char *username)
{
        int     res, i;
        char  **sessions;
        uid_t   uid;
        char   *primary_ssid;
        gboolean got_primary_ssid;

        primary_ssid = NULL;
        got_primary_ssid = FALSE;

        res = sd_seat_can_multi_session (slave->priv->display_seat_id);
        if (res < 0) {
                g_warning ("GdmSlave: Failed to determine whether seat is multi-session capable: %s", strerror (-res));
                return NULL;
        } else if (res == 0) {
                g_debug ("GdmSlave: seat is unable to activate sessions");
                return NULL;
        }

        if (! _get_uid_and_gid_for_user (username, &uid, NULL)) {
                g_debug ("GdmSlave: unable to determine uid for user: %s", username);
                return NULL;
        }

        res = sd_seat_get_sessions (slave->priv->display_seat_id, &sessions, NULL, NULL);
        if (res < 0) {
                g_warning ("GdmSlave: Failed to get sessions on seat: %s", strerror (-res));
                return NULL;
        }

        if (sessions == NULL) {
                g_debug ("GdmSlave: seat has no active sessions");
                return NULL;
        }

        for (i = 0; sessions[i] != NULL; i++) {
                char *type;
                char *state;
                gboolean is_closing;
                gboolean is_active;
                gboolean is_x11;
                uid_t other;

                res = sd_session_get_type (sessions[i], &type);

                if (res < 0) {
                        g_warning ("GdmSlave: could not fetch type of session '%s': %s",
                                   sessions[i], strerror (-res));
                        continue;
                }

                is_x11 = g_strcmp0 (type, "x11") == 0;
                free (type);

                /* Only migrate to graphical sessions
                 */
                if (!is_x11) {
                        continue;
                }

                /* Always give preference to non-active sessions,
                 * so we migrate when we can and don't when we can't
                 */
                res = sd_session_get_state (sessions[i], &state);
                if (res < 0) {
                        g_warning ("GdmSlave: could not fetch state of session '%s': %s",
                                   sessions[i], strerror (-res));
                        continue;
                }

                is_closing = g_strcmp0 (state, "closing") == 0;
                is_active = g_strcmp0 (state, "active") == 0;
                free (state);

                /* Ignore closing sessions
                 */
                if (is_closing) {
                        continue;
                }

                res = sd_session_get_uid (sessions[i], &other);
                if (res == 0 && other == uid && !got_primary_ssid) {
                        g_free (primary_ssid);
                        primary_ssid = g_strdup (sessions[i]);

                        if (!is_active) {
                                got_primary_ssid = TRUE;
                        }
                }
                free (sessions[i]);
        }

        free (sessions);
        return primary_ssid;
}
#endif

#ifdef WITH_CONSOLE_KIT
static char *
gdm_slave_get_primary_session_id_for_user_from_ck (GdmSlave   *slave,
                                                   const char *username)
{
        gboolean      can_activate_sessions;
        GError       *error;
        const char  **sessions;
        int           i;
        char         *primary_ssid;
        uid_t         uid;
        GVariant     *reply;

        error = NULL;
        primary_ssid = NULL;

        g_debug ("GdmSlave: getting proxy for seat: %s", slave->priv->display_seat_id);
        g_debug ("GdmSlave: checking if seat can activate sessions");

        reply = g_dbus_connection_call_sync (slave->priv->connection,
                                             CK_NAME,
                                             slave->priv->display_seat_id,
                                             CK_SEAT_INTERFACE,
                                             "CanActivateSessions",
                                             NULL, /* parameters */
                                             G_VARIANT_TYPE ("(b)"),
                                             G_DBUS_CALL_FLAGS_NONE,
                                             -1,
                                             NULL,
                                             &error);
        if (reply == NULL) {
                g_warning ("unable to determine if seat can activate sessions: %s", error->message);
                g_error_free (error);
                return NULL;
        }

        g_variant_get (reply, "(b)", &can_activate_sessions);
        g_variant_unref (reply);

        if (! can_activate_sessions) {
                g_debug ("GdmSlave: seat is unable to activate sessions");
                return NULL;
        }

        if (! _get_uid_and_gid_for_user (username, &uid, NULL)) {
                g_debug ("GdmSlave: unable to determine uid for user: %s", username);
                return NULL;
        }

        reply = g_dbus_connection_call_sync (slave->priv->connection,
                                             CK_NAME,
                                             CK_MANAGER_PATH,
                                             CK_MANAGER_INTERFACE,
                                             "GetSessionsForUnixUser",
                                             g_variant_new ("(u)", uid),
                                             G_VARIANT_TYPE ("(ao)"),
                                             G_DBUS_CALL_FLAGS_NONE,
                                             -1,
                                             NULL,
                                             &error);

        if (reply == NULL) {
                g_warning ("unable to determine sessions for user: %s", error->message);
                g_error_free (error);
                return NULL;
        }

        g_variant_get_child (reply, 0, "^a&o", &sessions);
        for (i = 0; sessions[i] != NULL; i++) {
                if (x11_session_is_on_seat (slave, sessions[i], slave->priv->display_seat_id)) {
                        primary_ssid = g_strdup (sessions[i]);
                        break;
                }
        }

        g_free (sessions);
        g_variant_unref (reply);
        return primary_ssid;
}
#endif

char *
gdm_slave_get_primary_session_id_for_user (GdmSlave   *slave,
                                           const char *username)
{

        if (slave->priv->display_seat_id == NULL || slave->priv->display_seat_id[0] == '\0') {
                g_debug ("GdmSlave: display seat ID is not set; can't switch sessions");
                return NULL;
        }

#ifdef WITH_SYSTEMD
        if (LOGIND_RUNNING()) {
                return gdm_slave_get_primary_session_id_for_user_from_systemd (slave, username);
        }
#endif

#ifdef WITH_CONSOLE_KIT
        return gdm_slave_get_primary_session_id_for_user_from_ck (slave, username);
#else
        return NULL;
#endif
}

#ifdef WITH_SYSTEMD
static gboolean
activate_session_id_for_systemd (GdmSlave   *slave,
                                 const char *seat_id,
                                 const char *session_id)
{
        GError *error = NULL;
        GVariant *reply;

        reply = g_dbus_connection_call_sync (slave->priv->connection,
                                             "org.freedesktop.login1",
                                             "/org/freedesktop/login1",
                                             "org.freedesktop.login1.Manager",
                                             "ActivateSessionOnSeat",
                                             g_variant_new ("(ss)", session_id, seat_id),
                                             NULL, /* expected reply */
                                             G_DBUS_CALL_FLAGS_NONE,
                                             -1,
                                             NULL,
                                             &error);
        if (reply == NULL) {
                g_debug ("GdmSlave: logind 'ActivateSessionOnSeat' %s raised:\n %s\n\n",
                         g_dbus_error_get_remote_error (error), error->message);
                g_error_free (error);
                return FALSE;
        }

        g_variant_unref (reply);

        return TRUE;
}
#endif

#ifdef WITH_CONSOLE_KIT
static gboolean
activate_session_id_for_ck (GdmSlave   *slave,
                            const char *seat_id,
                            const char *session_id)
{
        GError *error = NULL;
        GVariant *reply;

        reply = g_dbus_connection_call_sync (slave->priv->connection,
                                             CK_NAME,
                                             seat_id,
                                             "org.freedesktop.ConsoleKit.Seat",
                                             "ActivateSession",
                                             g_variant_new ("(o)", session_id),
                                             NULL, /* expected reply */
                                             G_DBUS_CALL_FLAGS_NONE,
                                             -1,
                                             NULL,
                                             &error);
        if (reply == NULL) {
                g_debug ("GdmSlave: ConsoleKit %s raised:\n %s\n\n",
                         g_dbus_error_get_remote_error (error), error->message);
                g_error_free (error);
                return FALSE;
        }

        g_variant_unref (reply);

        return TRUE;
}
#endif

static gboolean
activate_session_id (GdmSlave   *slave,
                     const char *seat_id,
                     const char *session_id)
{

#ifdef WITH_SYSTEMD
        if (LOGIND_RUNNING()) {
                return activate_session_id_for_systemd (slave, seat_id, session_id);
        }
#endif

#ifdef WITH_CONSOLE_KIT
        return activate_session_id_for_ck (slave, seat_id, session_id);
#else
        return FALSE;
#endif
}

#ifdef WITH_CONSOLE_KIT
static gboolean
ck_session_is_active (GdmSlave   *slave,
                      const char *seat_id,
                      const char *session_id)
{
        GError *error = NULL;
        GVariant *reply;
        gboolean is_active;

        reply = g_dbus_connection_call_sync (slave->priv->connection,
                                             CK_NAME,
                                             session_id,
                                             "org.freedesktop.ConsoleKit.Session",
                                             "IsActive",
                                             NULL,
                                             G_VARIANT_TYPE ("(b)"),
                                             G_DBUS_CALL_FLAGS_NONE,
                                             -1,
                                             NULL,
                                             &error);
        if (reply == NULL) {
                g_debug ("GdmSlave: ConsoleKit IsActive %s raised:\n %s\n\n",
                         g_dbus_error_get_remote_error (error), error->message);
                g_error_free (error);
                return FALSE;
        }

        g_variant_get (reply, "(b)", &is_active);
        g_variant_unref (reply);

        return is_active;
}
#endif

static gboolean
session_is_active (GdmSlave   *slave,
                   const char *seat_id,
                   const char *session_id)
{
#ifdef WITH_SYSTEMD
        if (LOGIND_RUNNING()) {
                return sd_session_is_active (session_id) > 0;
        }
#endif

#ifdef WITH_CONSOLE_KIT
        return ck_session_is_active (slave, seat_id, session_id);
#else
        return FALSE;
#endif
}

#ifdef WITH_SYSTEMD
static gboolean
session_unlock_for_systemd (GdmSlave   *slave,
                            const char *ssid)
{
        GError *error = NULL;
        GVariant *reply;

        reply = g_dbus_connection_call_sync (slave->priv->connection,
                                             "org.freedesktop.login1",
                                             "/org/freedesktop/login1",
                                             "org.freedesktop.login1.Manager",
                                             "UnlockSession",
                                             g_variant_new ("(s)", ssid),
                                             NULL, /* expected reply */
                                             G_DBUS_CALL_FLAGS_NONE,
                                             -1,
                                             NULL,
                                             &error);
        if (reply == NULL) {
                g_debug ("GdmSlave: logind 'UnlockSession' %s raised:\n %s\n\n",
                         g_dbus_error_get_remote_error (error), error->message);
                g_error_free (error);
                return FALSE;
        }

        g_variant_unref (reply);

        return TRUE;
}
#endif

#ifdef WITH_CONSOLE_KIT
static gboolean
session_unlock_for_ck (GdmSlave   *slave,
                       const char *ssid)
{
        GError *error = NULL;
        GVariant *reply;

        reply = g_dbus_connection_call_sync (slave->priv->connection,
                                             CK_NAME,
                                             ssid,
                                             CK_SESSION_INTERFACE,
                                             "Unlock",
                                             NULL, /* parameters */
                                             NULL, /* expected reply */
                                             G_DBUS_CALL_FLAGS_NONE,
                                             -1,
                                             NULL,
                                             &error);
        if (reply == NULL) {
                g_debug ("GdmSlave: ConsoleKit %s raised:\n %s\n\n",
                         g_dbus_error_get_remote_error (error), error->message);
                g_error_free (error);
                return FALSE;
        }

        g_variant_unref (reply);

        return TRUE;
}
#endif

static gboolean
session_unlock (GdmSlave   *slave,
                const char *ssid)
{

        g_debug ("Unlocking session %s", ssid);

#ifdef WITH_SYSTEMD
        if (LOGIND_RUNNING()) {
                return session_unlock_for_systemd (slave, ssid);
        }
#endif

#ifdef WITH_CONSOLE_KIT
        return session_unlock_for_ck (slave, ssid);
#else
        return TRUE;
#endif
}

gboolean
gdm_slave_switch_to_user_session (GdmSlave   *slave,
                                  const char *username,
                                  const char *session_id,
                                  gboolean    fail_if_already_switched)
{
        gboolean    res;
        gboolean    ret;
        gboolean    session_already_switched;
        char       *ssid_to_activate;

        ret = FALSE;

        if (session_id != NULL) {
                ssid_to_activate = g_strdup (session_id);
        } else {
                ssid_to_activate = gdm_slave_get_primary_session_id_for_user (slave, username);
                if (ssid_to_activate == NULL) {
                        g_debug ("GdmSlave: unable to determine session to activate");
                        goto out;
                }
        }

        session_already_switched = session_is_active (slave, slave->priv->display_seat_id, ssid_to_activate);

        g_debug ("GdmSlave: Activating session: '%s'", ssid_to_activate);

        if (session_already_switched && fail_if_already_switched) {
                g_debug ("GdmSlave: unable to activate session since it's already active: %s", ssid_to_activate);
                goto out;
        }

        if (!session_already_switched) {
                res = activate_session_id (slave, slave->priv->display_seat_id, ssid_to_activate);
                if (! res) {
                        g_debug ("GdmSlave: unable to activate session: %s", ssid_to_activate);
                        goto out;
                }
        }

        res = session_unlock (slave, ssid_to_activate);
        if (!res) {
                /* this isn't fatal */
                g_debug ("GdmSlave: unable to unlock session: %s", ssid_to_activate);
        }

        ret = TRUE;

 out:
        g_free (ssid_to_activate);

        return ret;
}

static void
_gdm_slave_set_session_id (GdmSlave   *slave,
                           const char *id)
{
        g_free (slave->priv->session_id);
        slave->priv->session_id = g_strdup (id);
}

static void
_gdm_slave_set_display_id (GdmSlave   *slave,
                           const char *id)
{
        g_free (slave->priv->display_id);
        slave->priv->display_id = g_strdup (id);
}

static void
_gdm_slave_set_display_name (GdmSlave   *slave,
                             const char *name)
{
        g_free (slave->priv->display_name);
        slave->priv->display_name = g_strdup (name);
}

static void
_gdm_slave_set_display_number (GdmSlave   *slave,
                               int         number)
{
        slave->priv->display_number = number;
}

static void
_gdm_slave_set_display_hostname (GdmSlave   *slave,
                                 const char *name)
{
        g_free (slave->priv->display_hostname);
        slave->priv->display_hostname = g_strdup (name);
}

static void
_gdm_slave_set_display_x11_authority_file (GdmSlave   *slave,
                                           const char *name)
{
        g_free (slave->priv->display_x11_authority_file);
        slave->priv->display_x11_authority_file = g_strdup (name);
}

static void
_gdm_slave_set_display_seat_id (GdmSlave   *slave,
                                const char *id)
{
        g_free (slave->priv->display_seat_id);
        slave->priv->display_seat_id = g_strdup (id);
}

static void
_gdm_slave_set_display_is_local (GdmSlave   *slave,
                                 gboolean    is)
{
        slave->priv->display_is_local = is;
}

static void
_gdm_slave_set_display_is_initial (GdmSlave   *slave,
                                   gboolean    is)
{
        slave->priv->display_is_initial = is;
}

static void
gdm_slave_set_property (GObject      *object,
                        guint         prop_id,
                        const GValue *value,
                        GParamSpec   *pspec)
{
        GdmSlave *self;

        self = GDM_SLAVE (object);

        switch (prop_id) {
        case PROP_SESSION_ID:
                _gdm_slave_set_session_id (self, g_value_get_string (value));
                break;
        case PROP_DISPLAY_ID:
                _gdm_slave_set_display_id (self, g_value_get_string (value));
                break;
        case PROP_DISPLAY_NAME:
                _gdm_slave_set_display_name (self, g_value_get_string (value));
                break;
        case PROP_DISPLAY_NUMBER:
                _gdm_slave_set_display_number (self, g_value_get_int (value));
                break;
        case PROP_DISPLAY_HOSTNAME:
                _gdm_slave_set_display_hostname (self, g_value_get_string (value));
                break;
        case PROP_DISPLAY_SEAT_ID:
                _gdm_slave_set_display_seat_id (self, g_value_get_string (value));
                break;
        case PROP_DISPLAY_X11_AUTHORITY_FILE:
                _gdm_slave_set_display_x11_authority_file (self, g_value_get_string (value));
                break;
        case PROP_DISPLAY_IS_LOCAL:
                _gdm_slave_set_display_is_local (self, g_value_get_boolean (value));
                break;
        case PROP_DISPLAY_IS_INITIAL:
                _gdm_slave_set_display_is_initial (self, g_value_get_boolean (value));
                break;
        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
                break;
        }
}

static void
gdm_slave_get_property (GObject    *object,
                        guint       prop_id,
                        GValue     *value,
                        GParamSpec *pspec)
{
        GdmSlave *self;

        self = GDM_SLAVE (object);

        switch (prop_id) {
        case PROP_SESSION_ID:
                g_value_set_string (value, self->priv->session_id);
                break;
        case PROP_DISPLAY_ID:
                g_value_set_string (value, self->priv->display_id);
                break;
        case PROP_DISPLAY_NAME:
                g_value_set_string (value, self->priv->display_name);
                break;
        case PROP_DISPLAY_NUMBER:
                g_value_set_int (value, self->priv->display_number);
                break;
        case PROP_DISPLAY_HOSTNAME:
                g_value_set_string (value, self->priv->display_hostname);
                break;
        case PROP_DISPLAY_SEAT_ID:
                g_value_set_string (value, self->priv->display_seat_id);
                break;
        case PROP_DISPLAY_X11_AUTHORITY_FILE:
                g_value_set_string (value, self->priv->display_x11_authority_file);
                break;
        case PROP_DISPLAY_IS_LOCAL:
                g_value_set_boolean (value, self->priv->display_is_local);
                break;
        case PROP_DISPLAY_IS_INITIAL:
                g_value_set_boolean (value, self->priv->display_is_initial);
                break;
        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
                break;
        }
}

static gboolean
handle_open_session (GdmDBusSlave          *skeleton,
                     GDBusMethodInvocation *invocation,
                     int                    pid_of_caller,
                     int                    uid_of_caller,
                     GdmSlave              *slave)
{
        GError        *error;
        GdmSlaveClass *slave_class;
        char          *address;

        slave_class = GDM_SLAVE_GET_CLASS (slave);
        if (slave_class->open_session == NULL) {
                g_dbus_method_invocation_return_dbus_error (invocation,
                                                            "org.gnome.DisplayManager.Slave.Unsupported",
                                                            "Connections to the slave are not supported by this slave");
                return TRUE;
        }

        error = NULL;
        address = NULL;
        if (!slave_class->open_session (slave,
                                        (GPid) pid_of_caller,
                                        (uid_t) uid_of_caller,
                                        &address,
                                        &error)) {
                g_dbus_method_invocation_return_gerror (invocation, error);
                g_error_free (error);
                return TRUE;
        }

        gdm_dbus_slave_complete_open_session (skeleton, invocation, address);

        g_free (address);
        return TRUE;
}

static void
on_reauthentication_channel_opened (GdmSlave              *slave,
                                    GAsyncResult          *result,
                                    GDBusMethodInvocation *invocation)
{
        GdmSlaveClass      *slave_class;
        GError             *error;
        char               *address;

        slave_class = GDM_SLAVE_GET_CLASS (slave);

        g_assert (slave_class->open_reauthentication_channel_finish != NULL);

        error = NULL;
        address = slave_class->open_reauthentication_channel_finish (slave, result, &error);

        if (address == NULL) {
                g_dbus_method_invocation_return_gerror (invocation, error);
        } else {
                gdm_dbus_slave_complete_open_reauthentication_channel (slave->priv->skeleton,
                                                                       invocation,
                                                                       address);
        }

        g_object_unref (invocation);
}

static gboolean
handle_open_reauthentication_channel (GdmDBusSlave          *skeleton,
                                      GDBusMethodInvocation *invocation,
                                      const char            *username,
                                      GPid                   pid_of_caller,
                                      uid_t                  uid_of_caller,
                                      GdmSlave              *slave)
{
        GdmSlaveClass *slave_class;

        slave_class = GDM_SLAVE_GET_CLASS (slave);
        if (slave_class->open_reauthentication_channel == NULL) {
                g_dbus_method_invocation_return_dbus_error (invocation,
                                                            "org.gnome.DisplayManager.Slave.Unsupported",
                                                            "Connections to the slave are not supported by this slave");
                return TRUE;
        }

        slave_class->open_reauthentication_channel (slave,
                                                    username,
                                                    pid_of_caller,
                                                    uid_of_caller,
                                                    (GAsyncReadyCallback)
                                                    on_reauthentication_channel_opened,
                                                    g_object_ref (invocation),
                                                    NULL);

        return TRUE;
}

static gboolean
register_slave (GdmSlave *slave)
{
        GError *error;

        error = NULL;
        slave->priv->connection = g_bus_get_sync (G_BUS_TYPE_SYSTEM,
                                                  NULL,
                                                  &error);
        if (slave->priv->connection == NULL) {
                g_critical ("error getting system bus: %s", error->message);
                g_error_free (error);
                exit (1);
        }

        slave->priv->skeleton = GDM_DBUS_SLAVE (gdm_dbus_slave_skeleton_new ());

        g_signal_connect (slave->priv->skeleton,
                          "handle-open-session",
                          G_CALLBACK (handle_open_session),
                          slave);
        g_signal_connect (slave->priv->skeleton,
                          "handle-open-reauthentication-channel",
                          G_CALLBACK (handle_open_reauthentication_channel),
                          slave);

        g_object_bind_property (G_OBJECT (slave),
                                "session-id",
                                G_OBJECT (slave->priv->skeleton),
                                "session-id",
                                G_BINDING_DEFAULT);

        gdm_slave_export_interface (slave,
                                    G_DBUS_INTERFACE_SKELETON (slave->priv->skeleton));

        return TRUE;
}

static GObject *
gdm_slave_constructor (GType                  type,
                       guint                  n_construct_properties,
                       GObjectConstructParam *construct_properties)
{
        GdmSlave      *slave;
        gboolean       res;

        slave = GDM_SLAVE (G_OBJECT_CLASS (gdm_slave_parent_class)->constructor (type,
                                                                                 n_construct_properties,
                                                                                 construct_properties));
        g_debug ("GdmSlave: Registering");

        res = register_slave (slave);
        if (! res) {
                g_warning ("Unable to register slave with system bus");
        }

        return G_OBJECT (slave);
}

static void
gdm_slave_class_init (GdmSlaveClass *klass)
{
        GObjectClass    *object_class = G_OBJECT_CLASS (klass);

        object_class->get_property = gdm_slave_get_property;
        object_class->set_property = gdm_slave_set_property;
        object_class->constructor = gdm_slave_constructor;
        object_class->finalize = gdm_slave_finalize;

        klass->start = gdm_slave_real_start;
        klass->stop = gdm_slave_real_stop;

        g_type_class_add_private (klass, sizeof (GdmSlavePrivate));

        g_object_class_install_property (object_class,
                                         PROP_SESSION_ID,
                                         g_param_spec_string ("session-id",
                                                              "Session id",
                                                              "ID of session",
                                                              NULL,
                                                              G_PARAM_READWRITE));
        g_object_class_install_property (object_class,
                                         PROP_DISPLAY_ID,
                                         g_param_spec_string ("display-id",
                                                              "id",
                                                              "id",
                                                              NULL,
                                                              G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
        g_object_class_install_property (object_class,
                                         PROP_DISPLAY_NAME,
                                         g_param_spec_string ("display-name",
                                                              "display name",
                                                              "display name",
                                                              NULL,
                                                              G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
        g_object_class_install_property (object_class,
                                         PROP_DISPLAY_NUMBER,
                                         g_param_spec_int ("display-number",
                                                           "display number",
                                                           "display number",
                                                           -1,
                                                           G_MAXINT,
                                                           -1,
                                                           G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
        g_object_class_install_property (object_class,
                                         PROP_DISPLAY_HOSTNAME,
                                         g_param_spec_string ("display-hostname",
                                                              "display hostname",
                                                              "display hostname",
                                                              NULL,
                                                              G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
        g_object_class_install_property (object_class,
                                         PROP_DISPLAY_SEAT_ID,
                                         g_param_spec_string ("display-seat-id",
                                                              "",
                                                              "",
                                                              NULL,
                                                              G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
        g_object_class_install_property (object_class,
                                         PROP_DISPLAY_X11_AUTHORITY_FILE,
                                         g_param_spec_string ("display-x11-authority-file",
                                                              "",
                                                              "",
                                                              NULL,
                                                              G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
        g_object_class_install_property (object_class,
                                         PROP_DISPLAY_IS_LOCAL,
                                         g_param_spec_boolean ("display-is-local",
                                                               "display is local",
                                                               "display is local",
                                                               TRUE,
                                                               G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
        g_object_class_install_property (object_class,
                                         PROP_DISPLAY_IS_INITIAL,
                                         g_param_spec_boolean ("display-is-initial",
                                                               NULL,
                                                               NULL,
                                                               FALSE,
                                                               G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

        signals [STOPPED] =
                g_signal_new ("stopped",
                              G_TYPE_FROM_CLASS (object_class),
                              G_SIGNAL_RUN_LAST,
                              G_STRUCT_OFFSET (GdmSlaveClass, stopped),
                              NULL,
                              NULL,
                              g_cclosure_marshal_VOID__VOID,
                              G_TYPE_NONE,
                              0);
}

static void
gdm_slave_init (GdmSlave *slave)
{

        slave->priv = GDM_SLAVE_GET_PRIVATE (slave);

        slave->priv->pid = -1;
}

static void
gdm_slave_finalize (GObject *object)
{
        GdmSlave *slave;

        g_return_if_fail (object != NULL);
        g_return_if_fail (GDM_IS_SLAVE (object));

        slave = GDM_SLAVE (object);

        g_return_if_fail (slave->priv != NULL);

        g_free (slave->priv->display_id);
        g_free (slave->priv->display_name);
        g_free (slave->priv->display_hostname);
        g_free (slave->priv->display_seat_id);
        g_free (slave->priv->display_x11_authority_file);
        g_free (slave->priv->parent_display_name);
        g_free (slave->priv->parent_display_x11_authority_file);
        g_free (slave->priv->windowpath);
        g_bytes_unref (slave->priv->display_x11_cookie);

        G_OBJECT_CLASS (gdm_slave_parent_class)->finalize (object);
}

void
gdm_slave_export_interface (GdmSlave               *slave,
                            GDBusInterfaceSkeleton *interface)
{
        g_dbus_interface_skeleton_export (interface,
                                          slave->priv->connection,
                                          GDM_SLAVE_PATH,
                                          NULL);
}
