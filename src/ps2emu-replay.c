/*
 * ps2emu-replay.c
 * Copyright (C) 2015 Red Hat
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for more
 * details.
 */

#include "ps2emu-event.h"
#include "ps2emu-misc.h"
#include "ps2emu-line.h"
#include "ps2emu-section.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <errno.h>
#include <linux/serio.h>
#include <ps2emu.h>

static GSList *event_list;

static GSList *init_event_list;
static GSList *main_event_list;

static GIOStatus send_ps2emu_cmd(GIOChannel *ps2emu_channel,
                                 guint8 type,
                                 guint8 data,
                                 GError **error) {
    GIOStatus rc;
    struct ps2emu_cmd cmd = {
        .type = type,
        .data = data,
    };

    rc = g_io_channel_write_chars(ps2emu_channel, (gchar*)&cmd, sizeof(cmd),
                                  NULL, error);
    return rc;
}

static gboolean simulate_interrupt(GIOChannel *ps2emu_channel,
                                   time_t start_time,
                                   PS2Event *event,
                                   GError **error) {
    time_t current_time;
    GIOStatus rc;

    current_time = g_get_monotonic_time() - start_time;
    if (current_time < event->time)
        g_usleep(event->time - current_time);

    rc = send_ps2emu_cmd(ps2emu_channel, PS2EMU_CMD_SEND_INTERRUPT,
                         event->data, error);
    if (rc != G_IO_STATUS_NORMAL)
        return FALSE;

    return TRUE;
}

static gboolean simulate_receive(GIOChannel *ps2emu_channel,
                                 PS2Event *event,
                                 GError **error) {
    guchar data;
    gsize count;
    GIOStatus rc;

    rc = g_io_channel_read_chars(ps2emu_channel, (gchar*)&data,
                                 sizeof(event->data), &count, error);

    if (rc != G_IO_STATUS_NORMAL)
        return FALSE;

    if (event->data == data)
        printf("Received expected data %hhx\n", data);
    else
        printf("Expected %hhx, received %hhx\n", event->data, data);

    return TRUE;
}

static gboolean replay_event_list(GIOChannel *ps2emu_channel,
                                  GSList *event_list,
                                  GError **error) {
    PS2Event *event;
    GIOStatus rc;
    const time_t start_time = g_get_monotonic_time();

    for (GSList *l = event_list; l != NULL; l = l->next) {
        event = l->data;

        if (event->type == PS2_EVENT_TYPE_INTERRUPT) {
            if (!simulate_interrupt(ps2emu_channel, start_time, event,
                                    error))
                return FALSE;
        } else {
            if (!simulate_receive(ps2emu_channel, event, error))
                return FALSE;
        }
    }

    return TRUE;
}

static gboolean parse_events(GIOChannel *input_channel,
                             int log_version,
                             GError **error) {
    gchar *line;
    LineType line_type;
    PS2Event *event;
    LogSectionType section_type;
    gchar *msg_start;
    GSList **event_list_dest;
    GIOStatus rc;

    while ((rc = g_io_channel_read_line(input_channel, &line, NULL, NULL,
                                        error)) == G_IO_STATUS_NORMAL) {
        if (log_version < 1) {
            line_type = LINE_TYPE_EVENT;
            msg_start = line;
            event_list_dest = &event_list;
        } else
            line_type = get_line_type(line, &msg_start, error);

        switch (line_type) {
            case LINE_TYPE_EVENT:
                event = ps2_event_from_line(msg_start, error);

                if (!event) {
                    if (!*error)
                        continue;
                    else
                        return FALSE;
                }

                *event_list_dest = g_slist_prepend(*event_list_dest, event);
                break;
            case LINE_TYPE_SECTION:
                section_type = section_type_from_line(msg_start, error);

                switch (section_type) {
                    case SECTION_TYPE_INIT:
                        event_list_dest = &init_event_list;
                        break;
                    case SECTION_TYPE_MAIN:
                        event_list_dest = &main_event_list;
                        break;
                    case SECTION_TYPE_ERROR:
                        return FALSE;
                }
                break;
            case LINE_TYPE_INVALID:
                return FALSE;
        }
    }
    if (rc != G_IO_STATUS_EOF)
        return FALSE;

    if (log_version >= 1) {
        if (init_event_list)
            init_event_list = g_slist_reverse(init_event_list);
        if (main_event_list)
            main_event_list = g_slist_reverse(main_event_list);
    } else
        event_list = g_slist_reverse(event_list);

    return TRUE;
}

static int parse_log_version(GIOChannel *input_channel,
                             GError **error) {
    gchar *line = NULL;
    int log_version,
        parse_count;
    GIOStatus rc;

    rc = g_io_channel_read_line(input_channel, &line, NULL, NULL, error);
    if (rc != G_IO_STATUS_NORMAL) {
        if (rc == G_IO_STATUS_EOF) {
            g_set_error_literal(error, PS2EMU_ERROR, PS2EMU_ERROR_NO_EVENTS,
                                "Reached unexpected EOF");
        }

        goto error;
    }

    errno = 0;
    parse_count = sscanf(line, "# ps2emu-record V%d\n", &log_version);
    if (parse_count == 0 || errno != 0) {
        g_set_error_literal(error, PS2EMU_ERROR, PS2EMU_ERROR_INPUT,
                            "Invalid log file version");
        goto error;
    }

    g_free(line);
    return log_version;

error:
    g_free(line);
    return -1;
}

gint main(gint argc,
          gchar *argv[]) {
    GOptionContext *main_context =
        g_option_context_new("<event_log> - replay PS/2 devices");
    GIOChannel *input_channel,
               *ps2emu_channel;
    GIOStatus rc;
    int log_version;
    GError *error = NULL;

    GOptionEntry options[] = {
        { "version", 'V', G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK,
          print_version, NULL },
        { 0 }
    };

    g_option_context_add_main_entries(main_context, options, NULL);
    g_option_context_set_help_enabled(main_context, TRUE);
    g_option_context_set_description(main_context,
        "Replays a PS/2 device using any log created with ps2emu-record\n");

    if (!g_option_context_parse(main_context, &argc, &argv, &error))
        exit_on_bad_argument(main_context, TRUE, error->message);

    if (argc < 2)
        exit_on_bad_argument(main_context, FALSE,
                             "No filename specified! Use --help for more "
                             "information");

    input_channel = g_io_channel_new_file(argv[1], "r", &error);
    if (!input_channel) {
        g_prefix_error(&error, "While opening %s: ", argv[1]);
        goto error;
    }

    log_version = parse_log_version(input_channel, &error);
    if (log_version > PS2EMU_LOG_VERSION) {
        g_set_error(&error, PS2EMU_ERROR, PS2EMU_ERROR_INPUT,
                    "Log version is too new (found %d, we only support up to "
                    "%d)", log_version, PS2EMU_LOG_VERSION);
        goto error;
    }

    if (!parse_events(input_channel, log_version, &error))
        goto error;

    g_io_channel_unref(input_channel);

    ps2emu_channel = g_io_channel_new_file("/dev/ps2emu", "r+", &error);
    if (!ps2emu_channel) {
        g_prefix_error(&error, "While opening /dev/ps2emu: ");
        goto error;
    }

    rc = g_io_channel_set_encoding(ps2emu_channel, NULL, &error);
    if (rc != G_IO_STATUS_NORMAL) {
        g_prefix_error(&error, "While opening /dev/ps2emu: ");
        goto error;
    }
    g_io_channel_set_buffered(ps2emu_channel, FALSE);

    rc = send_ps2emu_cmd(ps2emu_channel, PS2EMU_CMD_SET_PORT_TYPE, SERIO_8042,
                         &error);
    if (rc != G_IO_STATUS_NORMAL) {
        g_prefix_error(&error, "While setting port type on /dev/ps2emu: ");
        goto error;
    }

    rc = send_ps2emu_cmd(ps2emu_channel, PS2EMU_CMD_BEGIN, SERIO_8042, &error);
    if (rc != G_IO_STATUS_NORMAL) {
        g_prefix_error(&error, "While starting device on /dev/ps2emu: ");
        goto error;
    }

    if (log_version == 0) {
        if (!replay_event_list(ps2emu_channel, event_list, &error))
            goto error;
    } else {
        printf("Replaying initialization sequence...\n");
        if (!replay_event_list(ps2emu_channel, init_event_list, &error))
            goto error;

        printf("Replaying event sequence...\n");
        if (!replay_event_list(ps2emu_channel, main_event_list, &error))
            goto error;
    }

    return 0;

error:
    fprintf(stderr, "Error: %s\n", error->message);

    return 1;
}
