/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2010 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <dbus/dbus.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <getopt.h>
#include <pwd.h>

#include "log.h"
#include "util.h"
#include "macro.h"
#include "pager.h"
#include "dbus-common.h"
#include "build.h"
#include "strv.h"
#include "cgroup-show.h"
#include "sysfs-show.h"

static char **arg_property = NULL;
static bool arg_all = false;
static bool arg_no_pager = false;
static const char *arg_kill_who = NULL;
static int arg_signal = SIGTERM;
static enum transport {
        TRANSPORT_NORMAL,
        TRANSPORT_SSH,
        TRANSPORT_POLKIT
} arg_transport = TRANSPORT_NORMAL;
static const char *arg_host = NULL;

static bool on_tty(void) {
        static int t = -1;

        /* Note that this is invoked relatively early, before we start
         * the pager. That means the value we return reflects whether
         * we originally were started on a tty, not if we currently
         * are. But this is intended, since we want colour and so on
         * when run in our own pager. */

        if (_unlikely_(t < 0))
                t = isatty(STDOUT_FILENO) > 0;

        return t;
}

static void pager_open_if_enabled(void) {

        /* Cache result before we open the pager */
        on_tty();

        if (!arg_no_pager)
                pager_open();
}

static int list_sessions(DBusConnection *bus, char **args, unsigned n) {
        DBusMessage *m = NULL, *reply = NULL;
        DBusError error;
        int r;
        DBusMessageIter iter, sub, sub2;
        unsigned k = 0;

        dbus_error_init(&error);

        assert(bus);

        pager_open_if_enabled();

        m = dbus_message_new_method_call(
                        "org.freedesktop.login1",
                        "/org/freedesktop/login1",
                        "org.freedesktop.login1.Manager",
                        "ListSessions");
        if (!m) {
                log_error("Could not allocate message.");
                return -ENOMEM;
        }

        reply = dbus_connection_send_with_reply_and_block(bus, m, -1, &error);
        if (!reply) {
                log_error("Failed to issue method call: %s", bus_error_message(&error));
                r = -EIO;
                goto finish;
        }

        if (!dbus_message_iter_init(reply, &iter) ||
            dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY ||
            dbus_message_iter_get_element_type(&iter) != DBUS_TYPE_STRUCT)  {
                log_error("Failed to parse reply.");
                r = -EIO;
                goto finish;
        }

        dbus_message_iter_recurse(&iter, &sub);

        if (on_tty())
                printf("%10s %10s %-16s %-16s\n", "SESSION", "UID", "USER", "SEAT");

        while (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_INVALID) {
                const char *id, *user, *seat, *object;
                uint32_t uid;

                if (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_STRUCT) {
                        log_error("Failed to parse reply.");
                        r = -EIO;
                        goto finish;
                }

                dbus_message_iter_recurse(&sub, &sub2);

                if (bus_iter_get_basic_and_next(&sub2, DBUS_TYPE_STRING, &id, true) < 0 ||
                    bus_iter_get_basic_and_next(&sub2, DBUS_TYPE_UINT32, &uid, true) < 0 ||
                    bus_iter_get_basic_and_next(&sub2, DBUS_TYPE_STRING, &user, true) < 0 ||
                    bus_iter_get_basic_and_next(&sub2, DBUS_TYPE_STRING, &seat, true) < 0 ||
                    bus_iter_get_basic_and_next(&sub2, DBUS_TYPE_OBJECT_PATH, &object, false) < 0) {
                        log_error("Failed to parse reply.");
                        r = -EIO;
                        goto finish;
                }

                printf("%10s %10u %-16s %-16s\n", id, (unsigned) uid, user, seat);

                k++;

                dbus_message_iter_next(&sub);
        }

        if (on_tty())
                printf("\n%u sessions listed.\n", k);

        r = 0;

finish:
        if (m)
                dbus_message_unref(m);

        if (reply)
                dbus_message_unref(reply);

        dbus_error_free(&error);

        return r;
}

static int list_users(DBusConnection *bus, char **args, unsigned n) {
        DBusMessage *m = NULL, *reply = NULL;
        DBusError error;
        int r;
        DBusMessageIter iter, sub, sub2;
        unsigned k = 0;

        dbus_error_init(&error);

        assert(bus);

        pager_open_if_enabled();

        m = dbus_message_new_method_call(
                        "org.freedesktop.login1",
                        "/org/freedesktop/login1",
                        "org.freedesktop.login1.Manager",
                        "ListUsers");
        if (!m) {
                log_error("Could not allocate message.");
                return -ENOMEM;
        }

        reply = dbus_connection_send_with_reply_and_block(bus, m, -1, &error);
        if (!reply) {
                log_error("Failed to issue method call: %s", bus_error_message(&error));
                r = -EIO;
                goto finish;
        }

        if (!dbus_message_iter_init(reply, &iter) ||
            dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY ||
            dbus_message_iter_get_element_type(&iter) != DBUS_TYPE_STRUCT)  {
                log_error("Failed to parse reply.");
                r = -EIO;
                goto finish;
        }

        dbus_message_iter_recurse(&iter, &sub);

        if (on_tty())
                printf("%10s %-16s\n", "UID", "USER");

        while (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_INVALID) {
                const char *user, *object;
                uint32_t uid;

                if (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_STRUCT) {
                        log_error("Failed to parse reply.");
                        r = -EIO;
                        goto finish;
                }

                dbus_message_iter_recurse(&sub, &sub2);

                if (bus_iter_get_basic_and_next(&sub2, DBUS_TYPE_UINT32, &uid, true) < 0 ||
                    bus_iter_get_basic_and_next(&sub2, DBUS_TYPE_STRING, &user, true) < 0 ||
                    bus_iter_get_basic_and_next(&sub2, DBUS_TYPE_OBJECT_PATH, &object, false) < 0) {
                        log_error("Failed to parse reply.");
                        r = -EIO;
                        goto finish;
                }

                printf("%10u %-16s\n", (unsigned) uid, user);

                k++;

                dbus_message_iter_next(&sub);
        }

        if (on_tty())
                printf("\n%u users listed.\n", k);

        r = 0;

finish:
        if (m)
                dbus_message_unref(m);

        if (reply)
                dbus_message_unref(reply);

        dbus_error_free(&error);

        return r;
}

static int list_seats(DBusConnection *bus, char **args, unsigned n) {
        DBusMessage *m = NULL, *reply = NULL;
        DBusError error;
        int r;
        DBusMessageIter iter, sub, sub2;
        unsigned k = 0;

        dbus_error_init(&error);

        assert(bus);

        pager_open_if_enabled();

        m = dbus_message_new_method_call(
                        "org.freedesktop.login1",
                        "/org/freedesktop/login1",
                        "org.freedesktop.login1.Manager",
                        "ListSeats");
        if (!m) {
                log_error("Could not allocate message.");
                return -ENOMEM;
        }

        reply = dbus_connection_send_with_reply_and_block(bus, m, -1, &error);
        if (!reply) {
                log_error("Failed to issue method call: %s", bus_error_message(&error));
                r = -EIO;
                goto finish;
        }

        if (!dbus_message_iter_init(reply, &iter) ||
            dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY ||
            dbus_message_iter_get_element_type(&iter) != DBUS_TYPE_STRUCT)  {
                log_error("Failed to parse reply.");
                r = -EIO;
                goto finish;
        }

        dbus_message_iter_recurse(&iter, &sub);

        if (on_tty())
                printf("%-16s\n", "SEAT");

        while (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_INVALID) {
                const char *seat, *object;

                if (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_STRUCT) {
                        log_error("Failed to parse reply.");
                        r = -EIO;
                        goto finish;
                }

                dbus_message_iter_recurse(&sub, &sub2);

                if (bus_iter_get_basic_and_next(&sub2, DBUS_TYPE_STRING, &seat, true) < 0 ||
                    bus_iter_get_basic_and_next(&sub2, DBUS_TYPE_OBJECT_PATH, &object, false) < 0) {
                        log_error("Failed to parse reply.");
                        r = -EIO;
                        goto finish;
                }

                printf("%-16s\n", seat);

                k++;

                dbus_message_iter_next(&sub);
        }

        if (on_tty())
                printf("\n%u seats listed.\n", k);

        r = 0;

finish:
        if (m)
                dbus_message_unref(m);

        if (reply)
                dbus_message_unref(reply);

        dbus_error_free(&error);

        return r;
}

typedef struct SessionStatusInfo {
        const char *id;
        uid_t uid;
        const char *name;
        usec_t timestamp;
        const char *control_group;
        int vtnr;
        const char *seat;
        const char *tty;
        const char *display;
        bool remote;
        const char *remote_host;
        const char *remote_user;
        const char *service;
        pid_t leader;
        const char *type;
        bool active;
} SessionStatusInfo;

typedef struct UserStatusInfo {
        uid_t uid;
        const char *name;
        usec_t timestamp;
        const char *control_group;
        const char *state;
        char **sessions;
        const char *display;
} UserStatusInfo;

typedef struct SeatStatusInfo {
        const char *id;
        const char *active_session;
        char **sessions;
} SeatStatusInfo;

static void print_session_status_info(SessionStatusInfo *i) {
        char since1[FORMAT_TIMESTAMP_PRETTY_MAX], *s1;
        char since2[FORMAT_TIMESTAMP_MAX], *s2;
        assert(i);

        printf("%s - ", strna(i->id));

        if (i->name)
                printf("%s (%u)\n", i->name, (unsigned) i->uid);
        else
                printf("%u\n", (unsigned) i->uid);

        s1 = format_timestamp_pretty(since1, sizeof(since1), i->timestamp);
        s2 = format_timestamp(since2, sizeof(since2), i->timestamp);

        if (s1)
                printf("\t   Since: %s; %s\n", s2, s1);
        else if (s2)
                printf("\t   Since: %s\n", s2);

        if (i->leader > 0) {
                char *t = NULL;

                printf("\t  Leader: %u", (unsigned) i->leader);

                get_process_name(i->leader, &t);
                if (t) {
                        printf(" (%s)", t);
                        free(t);
                }

                printf("\n");
        }

        if (i->seat) {
                printf("\t    Seat: %s", i->seat);

                if (i->vtnr > 0)
                        printf("; vc%i", i->vtnr);

                printf("\n");
        }

        if (i->tty)
                printf("\t     TTY: %s\n", i->tty);
        else if (i->display)
                printf("\t Display: %s\n", i->display);

        if (i->remote_host && i->remote_user)
                printf("\t  Remote: %s@%s\n", i->remote_user, i->remote_host);
        else if (i->remote_host)
                printf("\t  Remote: %s\n", i->remote_host);
        else if (i->remote_user)
                printf("\t  Remote: user %s\n", i->remote_user);
        else if (i->remote)
                printf("\t  Remote: Yes\n");

        if (i->service) {
                printf("\t Service: %s", i->service);

                if (i->type)
                        printf("; type %s", i->type);

                printf("\n");
        } else if (i->type)
                printf("\t    Type: %s\n", i->type);

        printf("\t  Active: %s\n", yes_no(i->active));

        if (i->control_group) {
                unsigned c;

                printf("\t  CGroup: %s\n", i->control_group);

                if (arg_transport != TRANSPORT_SSH) {
                        c = columns();
                        if (c > 18)
                                c -= 18;
                        else
                                c = 0;

                        show_cgroup_by_path(i->control_group, "\t\t  ", c);
                }
        }
}

static void print_user_status_info(UserStatusInfo *i) {
        char since1[FORMAT_TIMESTAMP_PRETTY_MAX], *s1;
        char since2[FORMAT_TIMESTAMP_MAX], *s2;
        assert(i);

        if (i->name)
                printf("%s (%u)\n", i->name, (unsigned) i->uid);
        else
                printf("%u\n", (unsigned) i->uid);

        s1 = format_timestamp_pretty(since1, sizeof(since1), i->timestamp);
        s2 = format_timestamp(since2, sizeof(since2), i->timestamp);

        if (s1)
                printf("\t   Since: %s; %s\n", s2, s1);
        else if (s2)
                printf("\t   Since: %s\n", s2);

        if (!isempty(i->state))
                printf("\t   State: %s\n", i->state);

        if (!strv_isempty(i->sessions)) {
                char **l;
                printf("\tSessions:");

                STRV_FOREACH(l, i->sessions) {
                        if (streq_ptr(*l, i->display))
                                printf(" *%s", *l);
                        else
                                printf(" %s", *l);
                }

                printf("\n");
        }

        if (i->control_group) {
                unsigned c;

                printf("\t  CGroup: %s\n", i->control_group);

                if (arg_transport != TRANSPORT_SSH) {
                        c = columns();
                        if (c > 18)
                                c -= 18;
                        else
                                c = 0;

                        show_cgroup_by_path(i->control_group, "\t\t  ", c);
                }
        }
}

static void print_seat_status_info(SeatStatusInfo *i) {
        assert(i);

        printf("%s\n", strna(i->id));

        if (!strv_isempty(i->sessions)) {
                char **l;
                printf("\tSessions:");

                STRV_FOREACH(l, i->sessions) {
                        if (streq_ptr(*l, i->active_session))
                                printf(" *%s", *l);
                        else
                                printf(" %s", *l);
                }

                printf("\n");
        }

        if (arg_transport != TRANSPORT_SSH) {
                unsigned c;

                c = columns();
                if (c > 21)
                        c -= 21;
                else
                        c = 0;

                printf("\t Devices:\n");

                show_sysfs(i->id, "\t\t  ", c);
        }
}

static int status_property_session(const char *name, DBusMessageIter *iter, SessionStatusInfo *i) {
        assert(name);
        assert(iter);
        assert(i);

        switch (dbus_message_iter_get_arg_type(iter)) {

        case DBUS_TYPE_STRING: {
                const char *s;

                dbus_message_iter_get_basic(iter, &s);

                if (!isempty(s)) {
                        if (streq(name, "Id"))
                                i->id = s;
                        else if (streq(name, "Name"))
                                i->name = s;
                        else if (streq(name, "ControlGroupPath"))
                                i->control_group = s;
                        else if (streq(name, "TTY"))
                                i->tty = s;
                        else if (streq(name, "Display"))
                                i->display = s;
                        else if (streq(name, "RemoteHost"))
                                i->remote_host = s;
                        else if (streq(name, "RemoteUser"))
                                i->remote_user = s;
                        else if (streq(name, "Service"))
                                i->service = s;
                        else if (streq(name, "Type"))
                                i->type = s;
                }
                break;
        }

        case DBUS_TYPE_UINT32: {
                uint32_t u;

                dbus_message_iter_get_basic(iter, &u);

                if (streq(name, "VTNr"))
                        i->vtnr = (int) u;
                else if (streq(name, "Leader"))
                        i->leader = (pid_t) u;

                break;
        }

        case DBUS_TYPE_BOOLEAN: {
                dbus_bool_t b;

                dbus_message_iter_get_basic(iter, &b);

                if (streq(name, "Remote"))
                        i->remote = b;
                else if (streq(name, "Active"))
                        i->active = b;

                break;
        }

        case DBUS_TYPE_UINT64: {
                uint64_t u;

                dbus_message_iter_get_basic(iter, &u);

                if (streq(name, "Timestamp"))
                        i->timestamp = (usec_t) u;

                break;
        }

        case DBUS_TYPE_STRUCT: {
                DBusMessageIter sub;

                dbus_message_iter_recurse(iter, &sub);

                if (dbus_message_iter_get_arg_type(&sub) == DBUS_TYPE_UINT32 && streq(name, "User")) {
                        uint32_t u;

                        dbus_message_iter_get_basic(&sub, &u);
                        i->uid = (uid_t) u;

                } else if (dbus_message_iter_get_arg_type(&sub) == DBUS_TYPE_STRING && streq(name, "Seat")) {
                        const char *s;

                        dbus_message_iter_get_basic(&sub, &s);

                        if (!isempty(s))
                                i->seat = s;
                }

                break;
        }
        }

        return 0;
}

static int status_property_user(const char *name, DBusMessageIter *iter, UserStatusInfo *i) {
        assert(name);
        assert(iter);
        assert(i);

        switch (dbus_message_iter_get_arg_type(iter)) {

        case DBUS_TYPE_STRING: {
                const char *s;

                dbus_message_iter_get_basic(iter, &s);

                if (!isempty(s)) {
                        if (streq(name, "Name"))
                                i->name = s;
                        else if (streq(name, "ControlGroupPath"))
                                i->control_group = s;
                        else if (streq(name, "State"))
                                i->state = s;
                }
                break;
        }

        case DBUS_TYPE_UINT32: {
                uint32_t u;

                dbus_message_iter_get_basic(iter, &u);

                if (streq(name, "UID"))
                        i->uid = (uid_t) u;

                break;
        }

        case DBUS_TYPE_UINT64: {
                uint64_t u;

                dbus_message_iter_get_basic(iter, &u);

                if (streq(name, "Timestamp"))
                        i->timestamp = (usec_t) u;

                break;
        }

        case DBUS_TYPE_STRUCT: {
                DBusMessageIter sub;

                dbus_message_iter_recurse(iter, &sub);

                if (dbus_message_iter_get_arg_type(&sub) == DBUS_TYPE_STRING && streq(name, "Display")) {
                        const char *s;

                        dbus_message_iter_get_basic(&sub, &s);

                        if (!isempty(s))
                                i->display = s;
                }

                break;
        }

        case DBUS_TYPE_ARRAY: {

                if (dbus_message_iter_get_element_type(iter) == DBUS_TYPE_STRUCT && streq(name, "Sessions")) {
                        DBusMessageIter sub, sub2;

                        dbus_message_iter_recurse(iter, &sub);
                        while (dbus_message_iter_get_arg_type(&sub) == DBUS_TYPE_STRUCT) {
                                const char *id;
                                const char *path;

                                dbus_message_iter_recurse(&sub, &sub2);

                                if (bus_iter_get_basic_and_next(&sub2, DBUS_TYPE_STRING, &id, true) >= 0 &&
                                    bus_iter_get_basic_and_next(&sub2, DBUS_TYPE_OBJECT_PATH, &path, false) >= 0) {
                                        char **l;

                                        l = strv_append(i->sessions, id);
                                        if (!l)
                                                return -ENOMEM;

                                        strv_free(i->sessions);
                                        i->sessions = l;
                                }

                                dbus_message_iter_next(&sub);
                        }

                        return 0;
                }
        }
        }

        return 0;
}

static int status_property_seat(const char *name, DBusMessageIter *iter, SeatStatusInfo *i) {
        assert(name);
        assert(iter);
        assert(i);

        switch (dbus_message_iter_get_arg_type(iter)) {

        case DBUS_TYPE_STRING: {
                const char *s;

                dbus_message_iter_get_basic(iter, &s);

                if (!isempty(s)) {
                        if (streq(name, "Id"))
                                i->id = s;
                }
                break;
        }

        case DBUS_TYPE_STRUCT: {
                DBusMessageIter sub;

                dbus_message_iter_recurse(iter, &sub);

                if (dbus_message_iter_get_arg_type(&sub) == DBUS_TYPE_STRING && streq(name, "ActiveSession")) {
                        const char *s;

                        dbus_message_iter_get_basic(&sub, &s);

                        if (!isempty(s))
                                i->active_session = s;
                }

                break;
        }

        case DBUS_TYPE_ARRAY: {

                if (dbus_message_iter_get_element_type(iter) == DBUS_TYPE_STRUCT && streq(name, "Sessions")) {
                        DBusMessageIter sub, sub2;

                        dbus_message_iter_recurse(iter, &sub);
                        while (dbus_message_iter_get_arg_type(&sub) == DBUS_TYPE_STRUCT) {
                                const char *id;
                                const char *path;

                                dbus_message_iter_recurse(&sub, &sub2);

                                if (bus_iter_get_basic_and_next(&sub2, DBUS_TYPE_STRING, &id, true) >= 0 &&
                                    bus_iter_get_basic_and_next(&sub2, DBUS_TYPE_OBJECT_PATH, &path, false) >= 0) {
                                        char **l;

                                        l = strv_append(i->sessions, id);
                                        if (!l)
                                                return -ENOMEM;

                                        strv_free(i->sessions);
                                        i->sessions = l;
                                }

                                dbus_message_iter_next(&sub);
                        }

                        return 0;
                }
        }
        }

        return 0;
}

static int print_property(const char *name, DBusMessageIter *iter) {
        assert(name);
        assert(iter);

        if (arg_property && !strv_find(arg_property, name))
                return 0;

        switch (dbus_message_iter_get_arg_type(iter)) {

        case DBUS_TYPE_STRUCT: {
                DBusMessageIter sub;

                dbus_message_iter_recurse(iter, &sub);

                if (dbus_message_iter_get_arg_type(&sub) == DBUS_TYPE_STRING &&
                    (streq(name, "Display") || streq(name, "ActiveSession"))) {
                        const char *s;

                        dbus_message_iter_get_basic(&sub, &s);

                        if (arg_all || !isempty(s))
                                printf("%s=%s\n", name, s);
                        return 0;
                }
                break;
        }

        case DBUS_TYPE_ARRAY:

                if (dbus_message_iter_get_element_type(iter) == DBUS_TYPE_STRUCT && streq(name, "Sessions")) {
                        DBusMessageIter sub, sub2;
                        bool found = false;

                        dbus_message_iter_recurse(iter, &sub);
                        while (dbus_message_iter_get_arg_type(&sub) == DBUS_TYPE_STRUCT) {
                                const char *id;
                                const char *path;

                                dbus_message_iter_recurse(&sub, &sub2);

                                if (bus_iter_get_basic_and_next(&sub2, DBUS_TYPE_STRING, &id, true) >= 0 &&
                                    bus_iter_get_basic_and_next(&sub2, DBUS_TYPE_OBJECT_PATH, &path, false) >= 0) {
                                        if (found)
                                                printf(" %s", id);
                                        else {
                                                printf("%s=%s", name, id);
                                                found = true;
                                        }
                                }

                                dbus_message_iter_next(&sub);
                        }

                        if (!found && arg_all)
                                printf("%s=\n", name);
                        else if (found)
                                printf("\n");

                        return 0;
                }

                break;
        }

        if (generic_print_property(name, iter, arg_all) > 0)
                return 0;

        if (arg_all)
                printf("%s=[unprintable]\n", name);

        return 0;
}

static int show_one(const char *verb, DBusConnection *bus, const char *path, bool show_properties, bool *new_line) {
        DBusMessage *m = NULL, *reply = NULL;
        const char *interface = "";
        int r;
        DBusError error;
        DBusMessageIter iter, sub, sub2, sub3;
        SessionStatusInfo session_info;
        UserStatusInfo user_info;
        SeatStatusInfo seat_info;

        assert(bus);
        assert(path);
        assert(new_line);

        zero(session_info);
        zero(user_info);
        zero(seat_info);

        dbus_error_init(&error);

        m = dbus_message_new_method_call(
                        "org.freedesktop.login1",
                        path,
                        "org.freedesktop.DBus.Properties",
                        "GetAll");
        if (!m) {
                log_error("Could not allocate message.");
                r = -ENOMEM;
                goto finish;
        }

        if (!dbus_message_append_args(m,
                                      DBUS_TYPE_STRING, &interface,
                                      DBUS_TYPE_INVALID)) {
                log_error("Could not append arguments to message.");
                r = -ENOMEM;
                goto finish;
        }

        reply = dbus_connection_send_with_reply_and_block(bus, m, -1, &error);
        if (!reply) {
                log_error("Failed to issue method call: %s", bus_error_message(&error));
                r = -EIO;
                goto finish;
        }

        if (!dbus_message_iter_init(reply, &iter) ||
            dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY ||
            dbus_message_iter_get_element_type(&iter) != DBUS_TYPE_DICT_ENTRY)  {
                log_error("Failed to parse reply.");
                r = -EIO;
                goto finish;
        }

        dbus_message_iter_recurse(&iter, &sub);

        if (*new_line)
                printf("\n");

        *new_line = true;

        while (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_INVALID) {
                const char *name;

                if (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_DICT_ENTRY) {
                        log_error("Failed to parse reply.");
                        r = -EIO;
                        goto finish;
                }

                dbus_message_iter_recurse(&sub, &sub2);

                if (bus_iter_get_basic_and_next(&sub2, DBUS_TYPE_STRING, &name, true) < 0) {
                        log_error("Failed to parse reply.");
                        r = -EIO;
                        goto finish;
                }

                if (dbus_message_iter_get_arg_type(&sub2) != DBUS_TYPE_VARIANT)  {
                        log_error("Failed to parse reply.");
                        r = -EIO;
                        goto finish;
                }

                dbus_message_iter_recurse(&sub2, &sub3);

                if (show_properties)
                        r = print_property(name, &sub3);
                else if (strstr(verb, "session"))
                        r = status_property_session(name, &sub3, &session_info);
                else if (strstr(verb, "user"))
                        r = status_property_user(name, &sub3, &user_info);
                else
                        r = status_property_seat(name, &sub3, &seat_info);

                if (r < 0) {
                        log_error("Failed to parse reply.");
                        r = -EIO;
                        goto finish;
                }

                dbus_message_iter_next(&sub);
        }

        if (!show_properties) {
                if (strstr(verb, "session"))
                        print_session_status_info(&session_info);
                else if (strstr(verb, "user"))
                        print_user_status_info(&user_info);
                else
                        print_seat_status_info(&seat_info);
        }

        strv_free(seat_info.sessions);
        strv_free(user_info.sessions);

        r = 0;

finish:
        if (m)
                dbus_message_unref(m);

        if (reply)
                dbus_message_unref(reply);

        dbus_error_free(&error);

        return r;
}

static int show(DBusConnection *bus, char **args, unsigned n) {
        DBusMessage *m = NULL, *reply = NULL;
        int r, ret = 0;
        DBusError error;
        unsigned i;
        bool show_properties, new_line = false;

        assert(bus);
        assert(args);

        dbus_error_init(&error);

        show_properties = !strstr(args[0], "status");

        if (show_properties)
                pager_open_if_enabled();

        if (show_properties && n <= 1) {
                /* If not argument is specified inspect the manager
                 * itself */

                ret = show_one(args[0], bus, "/org/freedesktop/login1", show_properties, &new_line);
                goto finish;
        }

        for (i = 1; i < n; i++) {
                const char *path = NULL;

                if (strstr(args[0], "session")) {

                        m = dbus_message_new_method_call(
                                        "org.freedesktop.login1",
                                        "/org/freedesktop/login1",
                                        "org.freedesktop.login1.Manager",
                                        "GetSession");
                        if (!m) {
                                log_error("Could not allocate message.");
                                ret = -ENOMEM;
                                goto finish;
                        }

                        if (!dbus_message_append_args(m,
                                                      DBUS_TYPE_STRING, &args[i],
                                                      DBUS_TYPE_INVALID)) {
                                log_error("Could not append arguments to message.");
                                ret = -ENOMEM;
                                goto finish;
                        }

                } else if (strstr(args[0], "user")) {
                        uid_t uid;
                        uint32_t u;

                        r = get_user_creds((const char**) (args+i), &uid, NULL, NULL);
                        if (r < 0) {
                                log_error("User %s unknown.", args[i]);
                                r = -ENOENT;
                                goto finish;
                        }

                        m = dbus_message_new_method_call(
                                        "org.freedesktop.login1",
                                        "/org/freedesktop/login1",
                                        "org.freedesktop.login1.Manager",
                                        "GetUser");
                        if (!m) {
                                log_error("Could not allocate message.");
                                ret = -ENOMEM;
                                goto finish;
                        }

                        u = (uint32_t) uid;
                        if (!dbus_message_append_args(m,
                                                      DBUS_TYPE_UINT32, &u,
                                                      DBUS_TYPE_INVALID)) {
                                log_error("Could not append arguments to message.");
                                ret = -ENOMEM;
                                goto finish;
                        }
                } else {

                        m = dbus_message_new_method_call(
                                        "org.freedesktop.login1",
                                        "/org/freedesktop/login1",
                                        "org.freedesktop.login1.Manager",
                                        "GetSeat");
                        if (!m) {
                                log_error("Could not allocate message.");
                                ret = -ENOMEM;
                                goto finish;
                        }

                        if (!dbus_message_append_args(m,
                                                      DBUS_TYPE_STRING, &args[i],
                                                      DBUS_TYPE_INVALID)) {
                                log_error("Could not append arguments to message.");
                                ret = -ENOMEM;
                                goto finish;
                        }
                }

                reply = dbus_connection_send_with_reply_and_block(bus, m, -1, &error);
                if (!reply) {
                        log_error("Failed to issue method call: %s", bus_error_message(&error));
                        ret = -EIO;
                        goto finish;
                }

                if (!dbus_message_get_args(reply, &error,
                                           DBUS_TYPE_OBJECT_PATH, &path,
                                           DBUS_TYPE_INVALID)) {
                        log_error("Failed to parse reply: %s", bus_error_message(&error));
                        ret = -EIO;
                        goto finish;
                }

                r = show_one(args[0], bus, path, show_properties, &new_line);
                if (r != 0)
                        ret = r;

                dbus_message_unref(m);
                dbus_message_unref(reply);
                m = reply = NULL;
        }

finish:
        if (m)
                dbus_message_unref(m);

        if (reply)
                dbus_message_unref(reply);

        dbus_error_free(&error);

        return ret;
}

static int activate(DBusConnection *bus, char **args, unsigned n) {
        DBusMessage *m = NULL;
        int ret = 0;
        DBusError error;
        unsigned i;

        assert(bus);
        assert(args);

        dbus_error_init(&error);

        for (i = 1; i < n; i++) {
                DBusMessage *reply;

                m = dbus_message_new_method_call(
                                "org.freedesktop.login1",
                                "/org/freedesktop/login1",
                                "org.freedesktop.login1.Manager",
                                streq(args[0], "lock-session")      ? "LockSession" :
                                streq(args[0], "unlock-session")    ? "UnlockSession" :
                                streq(args[0], "terminate-session") ? "TerminateSession" :
                                                                      "ActivateSession");
                if (!m) {
                        log_error("Could not allocate message.");
                        ret = -ENOMEM;
                        goto finish;
                }

                if (!dbus_message_append_args(m,
                                              DBUS_TYPE_STRING, &args[i],
                                              DBUS_TYPE_INVALID)) {
                        log_error("Could not append arguments to message.");
                        ret = -ENOMEM;
                        goto finish;
                }

                reply = dbus_connection_send_with_reply_and_block(bus, m, -1, &error);
                if (!reply) {
                        log_error("Failed to issue method call: %s", bus_error_message(&error));
                        ret = -EIO;
                        goto finish;
                }

                dbus_message_unref(m);
                dbus_message_unref(reply);
                m = reply = NULL;
        }

finish:
        if (m)
                dbus_message_unref(m);

        dbus_error_free(&error);

        return ret;
}

static int kill_session(DBusConnection *bus, char **args, unsigned n) {
        DBusMessage *m = NULL;
        int ret = 0;
        DBusError error;
        unsigned i;

        assert(bus);
        assert(args);

        dbus_error_init(&error);

        if (!arg_kill_who)
                arg_kill_who = "all";

        for (i = 1; i < n; i++) {
                DBusMessage *reply;

                m = dbus_message_new_method_call(
                                "org.freedesktop.login1",
                                "/org/freedesktop/login1",
                                "org.freedesktop.login1.Manager",
                                "KillSession");
                if (!m) {
                        log_error("Could not allocate message.");
                        ret = -ENOMEM;
                        goto finish;
                }

                if (!dbus_message_append_args(m,
                                              DBUS_TYPE_STRING, &args[i],
                                              DBUS_TYPE_STRING, &arg_kill_who,
                                              DBUS_TYPE_INT32, arg_signal,
                                              DBUS_TYPE_INVALID)) {
                        log_error("Could not append arguments to message.");
                        ret = -ENOMEM;
                        goto finish;
                }

                reply = dbus_connection_send_with_reply_and_block(bus, m, -1, &error);
                if (!reply) {
                        log_error("Failed to issue method call: %s", bus_error_message(&error));
                        ret = -EIO;
                        goto finish;
                }

                dbus_message_unref(m);
                dbus_message_unref(reply);
                m = reply = NULL;
        }

finish:
        if (m)
                dbus_message_unref(m);

        dbus_error_free(&error);

        return ret;
}

static int enable_linger(DBusConnection *bus, char **args, unsigned n) {
        DBusMessage *m = NULL;
        int ret = 0;
        DBusError error;
        unsigned i;
        dbus_bool_t b, interactive = true;

        assert(bus);
        assert(args);

        dbus_error_init(&error);

        b = streq(args[0], "enable-linger");

        for (i = 1; i < n; i++) {
                DBusMessage *reply;
                uint32_t u;
                uid_t uid;

                m = dbus_message_new_method_call(
                                "org.freedesktop.login1",
                                "/org/freedesktop/login1",
                                "org.freedesktop.login1.Manager",
                                "SetUserLinger");
                if (!m) {
                        log_error("Could not allocate message.");
                        ret = -ENOMEM;
                        goto finish;
                }

                ret = get_user_creds((const char**) (args+i), &uid, NULL, NULL);
                if (ret < 0) {
                        log_error("Failed to resolve user %s: %s", args[i], strerror(-ret));
                        goto finish;
                }

                u = (uint32_t) uid;
                if (!dbus_message_append_args(m,
                                              DBUS_TYPE_UINT32, &u,
                                              DBUS_TYPE_BOOLEAN, &b,
                                              DBUS_TYPE_BOOLEAN, &interactive,
                                              DBUS_TYPE_INVALID)) {
                        log_error("Could not append arguments to message.");
                        ret = -ENOMEM;
                        goto finish;
                }

                reply = dbus_connection_send_with_reply_and_block(bus, m, -1, &error);
                if (!reply) {
                        log_error("Failed to issue method call: %s", bus_error_message(&error));
                        ret = -EIO;
                        goto finish;
                }

                dbus_message_unref(m);
                dbus_message_unref(reply);
                m = reply = NULL;
        }

        ret = 0;

finish:
        if (m)
                dbus_message_unref(m);

        dbus_error_free(&error);

        return ret;
}

static int terminate_user(DBusConnection *bus, char **args, unsigned n) {
        DBusMessage *m = NULL;
        int ret = 0;
        DBusError error;
        unsigned i;

        assert(bus);
        assert(args);

        dbus_error_init(&error);

        for (i = 1; i < n; i++) {
                uint32_t u;
                uid_t uid;
                DBusMessage *reply;

                m = dbus_message_new_method_call(
                                "org.freedesktop.login1",
                                "/org/freedesktop/login1",
                                "org.freedesktop.login1.Manager",
                                "TerminateUser");
                if (!m) {
                        log_error("Could not allocate message.");
                        ret = -ENOMEM;
                        goto finish;
                }

                ret = get_user_creds((const char**) (args+i), &uid, NULL, NULL);
                if (ret < 0) {
                        log_error("Failed to look up user %s: %s", args[i], strerror(-ret));
                        goto finish;
                }

                u = (uint32_t) uid;
                if (!dbus_message_append_args(m,
                                              DBUS_TYPE_UINT32, &u,
                                              DBUS_TYPE_INVALID)) {
                        log_error("Could not append arguments to message.");
                        ret = -ENOMEM;
                        goto finish;
                }

                reply = dbus_connection_send_with_reply_and_block(bus, m, -1, &error);
                if (!reply) {
                        log_error("Failed to issue method call: %s", bus_error_message(&error));
                        ret = -EIO;
                        goto finish;
                }

                dbus_message_unref(m);
                dbus_message_unref(reply);
                m = reply = NULL;
        }

        ret = 0;

finish:
        if (m)
                dbus_message_unref(m);

        dbus_error_free(&error);

        return ret;
}

static int kill_user(DBusConnection *bus, char **args, unsigned n) {
        DBusMessage *m = NULL;
        int ret = 0;
        DBusError error;
        unsigned i;

        assert(bus);
        assert(args);

        dbus_error_init(&error);

        if (!arg_kill_who)
                arg_kill_who = "all";

        for (i = 1; i < n; i++) {
                DBusMessage *reply;
                uid_t uid;
                uint32_t u;

                m = dbus_message_new_method_call(
                                "org.freedesktop.login1",
                                "/org/freedesktop/login1",
                                "org.freedesktop.login1.Manager",
                                "KillUser");
                if (!m) {
                        log_error("Could not allocate message.");
                        ret = -ENOMEM;
                        goto finish;
                }

                ret = get_user_creds((const char**) (args+i), &uid, NULL, NULL);
                if (ret < 0) {
                        log_error("Failed to look up user %s: %s", args[i], strerror(-ret));
                        goto finish;
                }

                u = (uint32_t) uid;
                if (!dbus_message_append_args(m,
                                              DBUS_TYPE_UINT32, &u,
                                              DBUS_TYPE_INT32, arg_signal,
                                              DBUS_TYPE_INVALID)) {
                        log_error("Could not append arguments to message.");
                        ret = -ENOMEM;
                        goto finish;
                }

                reply = dbus_connection_send_with_reply_and_block(bus, m, -1, &error);
                if (!reply) {
                        log_error("Failed to issue method call: %s", bus_error_message(&error));
                        ret = -EIO;
                        goto finish;
                }

                dbus_message_unref(m);
                dbus_message_unref(reply);
                m = reply = NULL;
        }

        ret = 0;

finish:
        if (m)
                dbus_message_unref(m);

        dbus_error_free(&error);

        return ret;
}

static int attach(DBusConnection *bus, char **args, unsigned n) {
        DBusMessage *m = NULL;
        int ret = 0;
        DBusError error;
        unsigned i;
        dbus_bool_t interactive = true;

        assert(bus);
        assert(args);

        dbus_error_init(&error);

        for (i = 2; i < n; i++) {
                DBusMessage *reply;

                m = dbus_message_new_method_call(
                                "org.freedesktop.login1",
                                "/org/freedesktop/login1",
                                "org.freedesktop.login1.Manager",
                                "AttachDevice");
                if (!m) {
                        log_error("Could not allocate message.");
                        ret = -ENOMEM;
                        goto finish;
                }

                if (!dbus_message_append_args(m,
                                              DBUS_TYPE_STRING, &args[1],
                                              DBUS_TYPE_STRING, &args[i],
                                              DBUS_TYPE_BOOLEAN, &interactive,
                                              DBUS_TYPE_INVALID)) {
                        log_error("Could not append arguments to message.");
                        ret = -ENOMEM;
                        goto finish;
                }

                reply = dbus_connection_send_with_reply_and_block(bus, m, -1, &error);
                if (!reply) {
                        log_error("Failed to issue method call: %s", bus_error_message(&error));
                        ret = -EIO;
                        goto finish;
                }

                dbus_message_unref(m);
                dbus_message_unref(reply);
                m = reply = NULL;
        }

finish:
        if (m)
                dbus_message_unref(m);

        dbus_error_free(&error);

        return ret;
}

static int flush_devices(DBusConnection *bus, char **args, unsigned n) {
        DBusMessage *m = NULL, *reply = NULL;
        int ret = 0;
        DBusError error;
        dbus_bool_t interactive = true;

        assert(bus);
        assert(args);

        dbus_error_init(&error);

        m = dbus_message_new_method_call(
                        "org.freedesktop.login1",
                        "/org/freedesktop/login1",
                        "org.freedesktop.login1.Manager",
                        "FlushDevices");
        if (!m) {
                log_error("Could not allocate message.");
                ret = -ENOMEM;
                goto finish;
        }

        if (!dbus_message_append_args(m,
                                      DBUS_TYPE_BOOLEAN, &interactive,
                                      DBUS_TYPE_INVALID)) {
                log_error("Could not append arguments to message.");
                ret = -ENOMEM;
                goto finish;
        }

        reply = dbus_connection_send_with_reply_and_block(bus, m, -1, &error);
        if (!reply) {
                log_error("Failed to issue method call: %s", bus_error_message(&error));
                ret = -EIO;
                goto finish;
        }

finish:
        if (m)
                dbus_message_unref(m);

        if (reply)
                dbus_message_unref(reply);

        dbus_error_free(&error);

        return ret;
}

static int terminate_seat(DBusConnection *bus, char **args, unsigned n) {
        DBusMessage *m = NULL;
        int ret = 0;
        DBusError error;
        unsigned i;

        assert(bus);
        assert(args);

        dbus_error_init(&error);

        for (i = 1; i < n; i++) {
                DBusMessage *reply;

                m = dbus_message_new_method_call(
                                "org.freedesktop.login1",
                                "/org/freedesktop/login1",
                                "org.freedesktop.login1.Manager",
                                "TerminateSeat");
                if (!m) {
                        log_error("Could not allocate message.");
                        ret = -ENOMEM;
                        goto finish;
                }

                if (!dbus_message_append_args(m,
                                              DBUS_TYPE_STRING, &args[i],
                                              DBUS_TYPE_INVALID)) {
                        log_error("Could not append arguments to message.");
                        ret = -ENOMEM;
                        goto finish;
                }

                reply = dbus_connection_send_with_reply_and_block(bus, m, -1, &error);
                if (!reply) {
                        log_error("Failed to issue method call: %s", bus_error_message(&error));
                        ret = -EIO;
                        goto finish;
                }

                dbus_message_unref(m);
                dbus_message_unref(reply);
                m = reply = NULL;
        }

finish:
        if (m)
                dbus_message_unref(m);

        dbus_error_free(&error);

        return ret;
}

static int help(void) {

        printf("%s [OPTIONS...] {COMMAND} ...\n\n"
               "Send control commands to or query the login manager.\n\n"
               "  -h --help           Show this help\n"
               "     --version        Show package version\n"
               "  -p --property=NAME  Show only properties by this name\n"
               "  -a --all            Show all properties, including empty ones\n"
               "     --kill-who=WHO   Who to send signal to\n"
               "  -s --signal=SIGNAL  Which signal to send\n"
               "  -H --host=[USER@]HOST\n"
               "                      Show information for remote host\n"
               "  -P --privileged     Acquire privileges before execution\n"
               "     --no-pager       Do not pipe output into a pager\n\n"
               "Commands:\n"
               "  list-sessions                   List sessions\n"
               "  session-status [ID...]          Show session status\n"
               "  show-session [ID...]            Show properties of one or more sessions\n"
               "  activate [ID]                   Activate a session\n"
               "  lock-session [ID...]            Screen lock one or more sessions\n"
               "  unlock-session [ID...]          Screen unlock one or more sessions\n"
               "  terminate-session [ID...]       Terminate one or more sessions\n"
               "  kill-session [ID...]            Send signal to processes of a session\n"
               "  list-users                      List users\n"
               "  user-status [USER...]           Show user status\n"
               "  show-user [USER...]             Show properties of one or more users\n"
               "  enable-linger [USER...]         Enable linger state of one or more users\n"
               "  disable-linger [USER...]        Disable linger state of one or more users\n"
               "  terminate-user [USER...]        Terminate all sessions of one or more users\n"
               "  kill-user [USER...]             Send signal to processes of a user\n"
               "  list-seats                      List seats\n"
               "  seat-status [NAME...]           Show seat status\n"
               "  show-seat [NAME...]             Show properties of one or more seats\n"
               "  attach [NAME] [DEVICE...]       Attach one or more devices to a seat\n"
               "  flush-devices                   Flush all device associations\n"
               "  terminate-seat [NAME...]        Terminate all sessions on one or more seats\n",
               program_invocation_short_name);

        return 0;
}

static int parse_argv(int argc, char *argv[]) {

        enum {
                ARG_VERSION = 0x100,
                ARG_NO_PAGER,
                ARG_KILL_WHO
        };

        static const struct option options[] = {
                { "help",      no_argument,       NULL, 'h'           },
                { "version",   no_argument,       NULL, ARG_VERSION   },
                { "property",  required_argument, NULL, 'p'           },
                { "all",       no_argument,       NULL, 'a'           },
                { "no-pager",  no_argument,       NULL, ARG_NO_PAGER  },
                { "kill-who",  required_argument, NULL, ARG_KILL_WHO  },
                { "signal",    required_argument, NULL, 's'           },
                { "host",      required_argument, NULL, 'H'           },
                { "privileged",no_argument,       NULL, 'P'           },
                { NULL,        0,                 NULL, 0             }
        };

        int c;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "hp:as:H:P", options, NULL)) >= 0) {

                switch (c) {

                case 'h':
                        help();
                        return 0;

                case ARG_VERSION:
                        puts(PACKAGE_STRING);
                        puts(DISTRIBUTION);
                        puts(SYSTEMD_FEATURES);
                        return 0;

                case 'p': {
                        char **l;

                        l = strv_append(arg_property, optarg);
                        if (!l)
                                return -ENOMEM;

                        strv_free(arg_property);
                        arg_property = l;

                        /* If the user asked for a particular
                         * property, show it to him, even if it is
                         * empty. */
                        arg_all = true;
                        break;
                }

                case 'a':
                        arg_all = true;
                        break;

                case ARG_NO_PAGER:
                        arg_no_pager = true;
                        break;

                case ARG_KILL_WHO:
                        arg_kill_who = optarg;
                        break;

                case 's':
                        arg_signal = signal_from_string_try_harder(optarg);
                        if (arg_signal < 0) {
                                log_error("Failed to parse signal string %s.", optarg);
                                return -EINVAL;
                        }
                        break;

                case 'P':
                        arg_transport = TRANSPORT_POLKIT;
                        break;

                case 'H':
                        arg_transport = TRANSPORT_SSH;
                        arg_host = optarg;
                        break;

                case '?':
                        return -EINVAL;

                default:
                        log_error("Unknown option code %c", c);
                        return -EINVAL;
                }
        }

        return 1;
}

static int loginctl_main(DBusConnection *bus, int argc, char *argv[], DBusError *error) {

        static const struct {
                const char* verb;
                const enum {
                        MORE,
                        LESS,
                        EQUAL
                } argc_cmp;
                const int argc;
                int (* const dispatch)(DBusConnection *bus, char **args, unsigned n);
        } verbs[] = {
                { "list-sessions",         LESS,   1, list_sessions    },
                { "session-status",        MORE,   2, show             },
                { "show-session",          MORE,   1, show             },
                { "activate",              EQUAL,  2, activate         },
                { "lock-session",          MORE,   2, activate         },
                { "unlock-session",        MORE,   2, activate         },
                { "terminate-session",     MORE,   2, activate         },
                { "kill-session",          MORE,   2, kill_session     },
                { "list-users",            EQUAL,  1, list_users       },
                { "user-status",           MORE,   2, show             },
                { "show-user",             MORE,   1, show             },
                { "enable-linger",         MORE,   2, enable_linger    },
                { "disable-linger",        MORE,   2, enable_linger    },
                { "terminate-user",        MORE,   2, terminate_user   },
                { "kill-user",             MORE,   2, kill_user        },
                { "list-seats",            EQUAL,  1, list_seats       },
                { "seat-status",           MORE,   2, show             },
                { "show-seat",             MORE,   1, show             },
                { "attach",                MORE,   3, attach           },
                { "flush-devices",         EQUAL,  1, flush_devices    },
                { "terminate-seat",        MORE,   2, terminate_seat   },
        };

        int left;
        unsigned i;

        assert(argc >= 0);
        assert(argv);
        assert(error);

        left = argc - optind;

        if (left <= 0)
                /* Special rule: no arguments means "list-sessions" */
                i = 0;
        else {
                if (streq(argv[optind], "help")) {
                        help();
                        return 0;
                }

                for (i = 0; i < ELEMENTSOF(verbs); i++)
                        if (streq(argv[optind], verbs[i].verb))
                                break;

                if (i >= ELEMENTSOF(verbs)) {
                        log_error("Unknown operation %s", argv[optind]);
                        return -EINVAL;
                }
        }

        switch (verbs[i].argc_cmp) {

        case EQUAL:
                if (left != verbs[i].argc) {
                        log_error("Invalid number of arguments.");
                        return -EINVAL;
                }

                break;

        case MORE:
                if (left < verbs[i].argc) {
                        log_error("Too few arguments.");
                        return -EINVAL;
                }

                break;

        case LESS:
                if (left > verbs[i].argc) {
                        log_error("Too many arguments.");
                        return -EINVAL;
                }

                break;

        default:
                assert_not_reached("Unknown comparison operator.");
        }

        if (!bus) {
                log_error("Failed to get D-Bus connection: %s", error->message);
                return -EIO;
        }

        return verbs[i].dispatch(bus, argv + optind, left);
}

int main(int argc, char*argv[]) {
        int r, retval = EXIT_FAILURE;
        DBusConnection *bus = NULL;
        DBusError error;

        dbus_error_init(&error);

        log_parse_environment();
        log_open();

        r = parse_argv(argc, argv);
        if (r < 0)
                goto finish;
        else if (r == 0) {
                retval = EXIT_SUCCESS;
                goto finish;
        }

        if (arg_transport == TRANSPORT_NORMAL)
                bus = dbus_bus_get_private(DBUS_BUS_SYSTEM, &error);
        else if (arg_transport == TRANSPORT_POLKIT)
                bus_connect_system_polkit(&bus, &error);
        else if (arg_transport == TRANSPORT_SSH)
                bus_connect_system_ssh(NULL, arg_host, &bus, &error);
        else
                assert_not_reached("Uh, invalid transport...");

        r = loginctl_main(bus, argc, argv, &error);
        retval = r < 0 ? EXIT_FAILURE : r;

finish:
        if (bus) {
                dbus_connection_flush(bus);
                dbus_connection_close(bus);
                dbus_connection_unref(bus);
        }

        dbus_error_free(&error);
        dbus_shutdown();

        strv_free(arg_property);

        pager_close();

        return retval;
}
