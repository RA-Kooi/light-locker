/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2004-2005 William Jon McCann <mccann@jhu.edu>
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
 * Authors: William Jon McCann <mccann@jhu.edu>
 *
 */

#include "config.h"

#include <stdlib.h>
#include <glib.h>
#include <glib/gi18n.h>

#define DBUS_API_SUBJECT_TO_CHANGE
#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>

#define GS_SERVICE   "org.gnome.screensaver"
#define GS_PATH      "/org/gnome/screensaver"
#define GS_INTERFACE "org.gnome.screensaver"

/* this is for dbus < 0.3 */
#if ((DBUS_VERSION_MAJOR == 0) && (DBUS_VERSION_MINOR < 30))
#define dbus_bus_name_has_owner(connection, name, err) dbus_bus_service_exists(connection, name, err)
#endif

static gboolean do_quit       = FALSE;
static gboolean do_lock       = FALSE;
static gboolean do_cycle      = FALSE;
static gboolean do_activate   = FALSE;
static gboolean do_deactivate = FALSE;
static gboolean do_throttle   = FALSE;
static gboolean do_unthrottle = FALSE;
static gboolean do_version    = FALSE;
static gboolean do_poke       = FALSE;

static gboolean do_query      = FALSE;

static GOptionEntry entries [] = {
        { "exit", 0, 0, G_OPTION_ARG_NONE, &do_quit,
          N_("Causes the screensaver to exit gracefully"), NULL },
        { "query", 0, 0, G_OPTION_ARG_NONE, &do_query,
          N_("Query the state of the screensaver"), NULL },
        { "lock", 0, 0, G_OPTION_ARG_NONE, &do_lock,
          N_("Tells the running screensaver process to lock the screen immediately"), NULL },
        { "cycle", 0, 0, G_OPTION_ARG_NONE, &do_cycle,
          N_("If the screensaver is active then switch to another graphics demo"), NULL },
        { "activate", 0, 0, G_OPTION_ARG_NONE, &do_activate,
          N_("Turn the screensaver on (blank the screen)"), NULL },
        { "deactivate", 0, 0, G_OPTION_ARG_NONE, &do_deactivate,
          N_("If the screensaver is active then deactivate it (un-blank the screen)"), NULL },
        { "throttle", 0, 0, G_OPTION_ARG_NONE, &do_throttle,
          N_("Disable running graphical themes while blanked"), NULL },
        { "unthrottle", 0, 0, G_OPTION_ARG_NONE, &do_unthrottle,
          N_("Enable running graphical themes while blanked (if applicable)"), NULL },
        { "poke", 0, 0, G_OPTION_ARG_NONE, &do_poke,
          N_("Poke the running screensaver to simulate user activity"), NULL },
        { "version", 0, 0, G_OPTION_ARG_NONE, &do_version,
          N_("Version of this application"), NULL },
        { NULL }
};

static GMainLoop *loop = NULL;

static gboolean
screensaver_is_running (DBusConnection *connection)
{
        DBusError               error;
        gboolean                exists;

        g_return_val_if_fail (connection != NULL, FALSE);

        dbus_error_init (&error);
        exists = dbus_bus_name_has_owner (connection, GS_SERVICE, &error);
        if (dbus_error_is_set (&error))
                dbus_error_free (&error);

        return exists;
}

static DBusMessage *
screensaver_send_message_bool (DBusConnection *connection,
                               const char     *name,
                               gboolean        value)
{
        DBusMessage    *message;
        DBusMessage    *reply;
        DBusError       error;
	DBusMessageIter iter;

        g_return_val_if_fail (connection != NULL, NULL);
        g_return_val_if_fail (name != NULL, NULL);

        dbus_error_init (&error);

        message = dbus_message_new_method_call (GS_SERVICE, GS_PATH, GS_INTERFACE, name);
        if (message == NULL) {
                g_warning ("Couldn't allocate the dbus message");
                return NULL;
        }

	dbus_message_iter_init_append (message, &iter);
	dbus_message_iter_append_basic (&iter, DBUS_TYPE_BOOLEAN, &value);

        reply = dbus_connection_send_with_reply_and_block (connection,
                                                           message,
                                                           -1, &error);
        if (dbus_error_is_set (&error)) {
                g_warning ("%s raised:\n %s\n\n", error.name, error.message);
                reply = NULL;
        }

        dbus_connection_flush (connection);

        dbus_message_unref (message);
        dbus_error_free (&error);

        return reply;
}

static DBusMessage *
screensaver_send_message_void (DBusConnection *connection,
                               const char     *name,
                               gboolean        expect_reply)
{
        DBusMessage *message;
        DBusMessage *reply;
        DBusError    error;

        g_return_val_if_fail (connection != NULL, NULL);
        g_return_val_if_fail (name != NULL, NULL);

        dbus_error_init (&error);

        message = dbus_message_new_method_call (GS_SERVICE, GS_PATH, GS_INTERFACE, name);
        if (message == NULL) {
                g_warning ("Couldn't allocate the dbus message");
                return NULL;
        }

        if (! expect_reply) {
                if (!dbus_connection_send (connection, message, NULL))
                        g_warning ("could not send message");
                reply = NULL;
        } else {
                reply = dbus_connection_send_with_reply_and_block (connection,
                                                                   message,
                                                                   -1, &error);
                if (dbus_error_is_set (&error)) {
                        g_warning ("%s raised:\n %s\n\n", error.name, error.message);
                        reply = NULL;
                }
        }
        dbus_connection_flush (connection);

        dbus_message_unref (message);
        dbus_error_free (&error);

        return reply;
}

static gboolean
do_command (DBusConnection *connection)
{
        DBusMessage *reply;

        if (do_quit) {
                reply = screensaver_send_message_void (connection, "quit", FALSE);
                goto done;
        }

        if (do_query) {
                DBusMessageIter iter;
                dbus_bool_t     v;

                reply = screensaver_send_message_void (connection, "getActive", TRUE);
                if (! reply) {
                        g_message ("Did not receive a reply from the screensaver.");
                        goto done;
                }

                dbus_message_iter_init (reply, &iter);
                dbus_message_iter_get_basic (&iter, &v);
                g_print (_("The screensaver is %s\n"), v ? _("active") : _("inactive"));

                dbus_message_unref (reply);
        }

        if (do_lock) {
                reply = screensaver_send_message_void (connection, "lock", FALSE);
        }

        if (do_cycle) {
                reply = screensaver_send_message_void (connection, "cycle", FALSE);
        }

        if (do_poke) {
                reply = screensaver_send_message_void (connection, "poke", FALSE);
        }

        if (do_activate) {
                reply = screensaver_send_message_bool (connection, "setActive", TRUE);
                if (! reply) {
                        g_message ("Did not receive a reply from the screensaver.");
                        goto done;
                }
                dbus_message_unref (reply);
        }

        if (do_deactivate) {
                reply = screensaver_send_message_bool (connection, "setActive", FALSE);
                if (! reply) {
                        g_message ("Did not receive a reply from the screensaver.");
                        goto done;
                }
                dbus_message_unref (reply);
        }

        if (do_throttle) {
                reply = screensaver_send_message_bool (connection, "setThrottleEnabled", TRUE);
                if (! reply) {
                        g_message ("Did not receive a reply from the screensaver.");
                        goto done;
                }
                dbus_message_unref (reply);
        }

        if (do_unthrottle) {
                reply = screensaver_send_message_bool (connection, "setThrottleEnabled", FALSE);
                if (! reply) {
                        g_message ("Did not receive a reply from the screensaver.");
                        goto done;
                }
                dbus_message_unref (reply);
        }

 done:
        g_main_loop_quit (loop);

        return FALSE;
}

int
main (int    argc,
      char **argv)
{
        DBusConnection *connection;
        DBusError       dbus_error;        
        GOptionContext *context;
        gboolean        retval;
        GError         *error = NULL;

        g_type_init ();

        context = g_option_context_new (NULL);
        g_option_context_set_ignore_unknown_options (context, TRUE);
        g_option_context_add_main_entries (context, entries, NULL);
        retval = g_option_context_parse (context, &argc, &argv, &error);

        g_option_context_free (context);

        if (! retval) {
                g_warning ("%s", error->message);
                g_error_free (error);
                exit (1);
        }

        if (do_version) {
                g_print ("%s %s\n", argv [0], VERSION);
                exit (1);
        }

        dbus_error_init (&dbus_error);
        connection = dbus_bus_get (DBUS_BUS_SESSION, &dbus_error);
        if (! connection) {
                g_message ("Failed to connect to the D-BUS daemon: %s", dbus_error.message);
                dbus_error_free (&dbus_error);
                exit (1);
        }

        dbus_connection_setup_with_g_main (connection, NULL);

        if (! screensaver_is_running (connection)) {
                g_message ("Screensaver is not running!");
                exit (1);
        }

        g_idle_add ((GSourceFunc)do_command, connection);

        loop = g_main_loop_new (NULL, FALSE);
        g_main_loop_run (loop);

        dbus_connection_disconnect (connection);

	return 0;
}
