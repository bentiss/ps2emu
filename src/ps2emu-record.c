/*
 * ps2emu-record.c
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <error.h>
#include <glib.h>
#include <signal.h>

#include "misc.h"
#include "ps2emu-event.h"

typedef enum {
    PS2_ERROR_INPUT,
    PS2_ERROR_NO_EVENTS
} PS2Error;

typedef struct {
    GQuark type;

    union {
        PS2Event event;
        gint64 start_time;
    };
} LogMsgParseResult;

static gboolean record_kbd;
static gboolean record_aux;

static gint64 start_time = 0;

#define I8042_OUTPUT  (g_quark_from_static_string("i8042: "))
#define PS2EMU_OUTPUT (g_quark_from_static_string("ps2emu: "))

#define KEYBOARD_PORT 0

static GIOStatus get_next_module_line(GIOChannel *input_channel,
                                      GQuark *match,
                                      gchar **output,
                                      gchar **start_pos,
                                      GError **error) {
    static const gchar *search_strings[] = { "i8042: ", "ps2emu: " };
    int index;
    gchar *current_line;
    GIOStatus rc;

    while ((rc = g_io_channel_read_line(input_channel, &current_line, NULL,
                                        NULL, error)) == G_IO_STATUS_NORMAL) {
        for (index = 0; index < G_N_ELEMENTS(search_strings); index++) {
            *start_pos = strstr(current_line, search_strings[index]);
            if (*start_pos)
                break;
        }
        if (*start_pos)
            break;

        g_free(current_line);
    }

    if (rc != G_IO_STATUS_NORMAL) {
        return rc;
    }

    /* Move the start position after the initial 'i8042: ' */
    *start_pos += strlen(search_strings[index]);
    *output = current_line;

    *match = g_quark_from_static_string(search_strings[index]);

    return rc;
}

static gboolean parse_normal_event(const gchar *start_pos,
                                   PS2Event *event,
                                   GError **error) {
    __label__ error;
    gchar *type_str = NULL;
    gchar **type_str_args = NULL;
    int type_str_argc,
        parsed_count;

    errno = 0;
    parsed_count = sscanf(start_pos,
                          "[%ld] %hhx %*1[-<]%*1[->] i8042 (%m[^)])\n",
                          &event->time, &event->data, &type_str);

    if (errno != 0 || parsed_count != 3)
        return FALSE;

    type_str_args = g_strsplit(type_str, ",", 0);

    if (strcmp(type_str_args[0], "interrupt") == 0) {
        event->type = PS2_EVENT_TYPE_INTERRUPT;

        type_str_argc = g_strv_length(type_str_args);
        if (type_str_argc < 3) {
            g_set_error(error, PS2EMU_ERROR, PS2_ERROR_INPUT,
                        "Got interrupt event, but had less arguments then "
                        "expected");
            goto error;
        }

        errno = 0;
        event->port = strtol(type_str_args[1], NULL, 10);
        if (errno != 0) {
            g_set_error(error, PS2EMU_ERROR, PS2_ERROR_INPUT,
                        "Failed to parse port number from interrupt event: "
                        "%s\n",
                        strerror(errno));
            goto error;
        }

        errno = 0;
        event->irq = strtol(type_str_args[2], NULL, 10);
        if (errno != 0) {
            g_set_error(error, PS2EMU_ERROR, PS2_ERROR_INPUT,
                        "Failed to parse IRQ from interrupt event: %s\n",
                        strerror(errno));
            goto error;
        }
    }
    else if (strcmp(type_str, "command") == 0)
        event->type = PS2_EVENT_TYPE_COMMAND;
    else if (strcmp(type_str, "parameter") == 0)
        event->type = PS2_EVENT_TYPE_PARAMETER;
    else if (strcmp(type_str, "return") == 0)
        event->type = PS2_EVENT_TYPE_RETURN;
    else if (strcmp(type_str, "kbd-data") == 0)
        event->type = PS2_EVENT_TYPE_KBD_DATA;

    event->has_data = TRUE;

    g_free(type_str);
    g_strfreev(type_str_args);

    return TRUE;

error:
    g_free(type_str);
    g_strfreev(type_str_args);

    return FALSE;
}

static gboolean parse_interrupt_without_data(const gchar *start_pos,
                                             PS2Event *event,
                                             GError **error) {
    int parsed_count;

    errno = 0;
    parsed_count = sscanf(start_pos,
                          "[%ld] Interrupt %hd, without any data\n",
                          &event->time, &event->irq);

    if (errno != 0 || parsed_count != 2)
        return FALSE;

    event->type = PS2_EVENT_TYPE_INTERRUPT;
    event->has_data = FALSE;

    return TRUE;
}

static gboolean parse_record_start_marker(const gchar *start_pos,
                                          gint64 *start_time) {
    gint parsed_count;

    errno = 0;
    parsed_count = sscanf(start_pos,
                          "Start recording %ld\n",
                          start_time);

    if (errno != 0 || parsed_count != 1)
        return FALSE;

    return TRUE;
}

static GIOStatus parse_next_message(GIOChannel *input_channel,
                                    LogMsgParseResult *res,
                                    GError **error) {
    __label__ fail;
    gchar *start_pos;
    gchar *current_line = NULL;
    GIOStatus rc;

    while ((rc = get_next_module_line(input_channel, &res->type, &current_line,
                                      &start_pos, error)) ==
            G_IO_STATUS_NORMAL) {
        if (res->type == I8042_OUTPUT) {
            if (parse_normal_event(start_pos, &res->event, error))
                break;

            if (*error)
                goto fail;

            if (parse_interrupt_without_data(start_pos, &res->event, error))
                break;

            if (*error)
                goto fail;
        }
        else if (res->type == PS2EMU_OUTPUT) {
            if (parse_record_start_marker(start_pos, &res->start_time))
                break;
        }

        g_free(current_line);
    }

    if (rc != G_IO_STATUS_NORMAL)
        return rc;

    res->type = res->type;

fail:
    g_free(current_line);

    return rc;
}

static gboolean process_event(PS2Event *event,
                              GError **error) {
    gchar *event_str = NULL;

    /* The logic here is that we can only get two types of events from a
     * keyboard, kbd-data and interrupt. No other device sends kbd-data, so we
     * can judge if an event comes from a keyboard or not solely based off that.
     * With interrupts, we can tell if the interrupt is coming from the keyboard
     * or not by comparing the port number of the event to that of the KBD
     * port */
    if (!record_kbd) {
        if (event->type == PS2_EVENT_TYPE_INTERRUPT &&
            event->port == KEYBOARD_PORT)
            return TRUE;

        if (event->type == PS2_EVENT_TYPE_KBD_DATA)
            return TRUE;
    }

    if (!record_aux) {
        if (event->type == PS2_EVENT_TYPE_INTERRUPT) {
            if (event->port != KEYBOARD_PORT)
                return TRUE;
        }
        else if (event->type != PS2_EVENT_TYPE_KBD_DATA)
            return TRUE;
    }

    event_str = ps2_event_to_string(event);
    printf("%s\n", event_str);

    g_free(event_str);

    return TRUE;
}

static gboolean write_to_char_dev(const gchar *cdev,
                                  GError **error,
                                  const gchar *format,
                                  ...) {
    __label__ error;
    GIOChannel *channel = g_io_channel_new_file(cdev, "w", error);
    GIOStatus rc;
    gchar *data = NULL;
    gsize data_len,
          bytes_written;
    va_list args;

    if (!channel) {
        g_prefix_error(error, "While opening %s: ", cdev);

        goto error;
    }

    va_start(args, format);
    data = g_strdup_vprintf(format, args);
    va_end(args);

    data_len = strlen(data);

    rc = g_io_channel_write_chars(channel, data, data_len, &bytes_written,
                                  error);
    if (rc != G_IO_STATUS_NORMAL) {
        g_prefix_error(error, "While writing to %s: ", cdev);

        goto error;
    }

    g_io_channel_unref(channel);
    g_free(data);
    return TRUE;

error:
    if (channel)
        g_io_channel_unref(channel);

    g_free(data);

    return FALSE;
}

static gboolean enable_i8042_debugging(GError **error) {
    __label__ error;
    GDir *devices_dir = NULL;

    devices_dir = g_dir_open("/sys/devices/platform/i8042", 0, error);
    if (!devices_dir) {
        g_prefix_error(error, "While opening /sys/devices/platform/i8042: ");

        goto error;
    }

    /* Detach the devices before we do anything, this prevents potential race
     * conditions */
    for (gchar const *dir_name = g_dir_read_name(devices_dir);
         dir_name != NULL && *error == NULL;
         dir_name = g_dir_read_name(devices_dir)) {
        gchar *file_name;

        if (!g_str_has_prefix(dir_name, "serio"))
            continue;

        file_name = g_strconcat("/sys/devices/platform/i8042/", dir_name, "/",
                                "drvctl", NULL);
        if (!write_to_char_dev(file_name, error, "none")) {
            g_free(file_name);
            goto error;
        }

        g_free(file_name);
    }
    if (*error)
        goto error;

    /* We mark when the recording starts, so that we can separate this recording
     * from other recordings ran during this session */
    start_time = g_get_monotonic_time();

    if (!write_to_char_dev("/dev/kmsg", error, "ps2emu: Start recording %ld\n",
                           start_time))
        goto error;

    /* Enable the debugging output for i8042 */
    if (!write_to_char_dev("/sys/module/i8042/parameters/debug", error, "1\n"))
        goto error;

    /* Reattach the devices */
    g_dir_rewind(devices_dir);
    for (gchar const *dir_name = g_dir_read_name(devices_dir);
         dir_name != NULL && *error == NULL;
         dir_name = g_dir_read_name(devices_dir)) {
        gchar *file_name;

        if (!g_str_has_prefix(dir_name, "serio"))
            continue;

        file_name = g_strconcat("/sys/devices/platform/i8042/", dir_name, "/",
                                "drvctl", NULL);
        if (!write_to_char_dev(file_name, error, "rescan")) {
            g_free(file_name);
            goto error;
        }

        g_free(file_name);
    }
    if (*error)
        goto error;

    g_dir_close(devices_dir);

    return TRUE;

error:
    if (devices_dir)
        g_dir_close(devices_dir);

    return FALSE;
}

static void disable_i8042_debugging() {
    g_warn_if_fail(write_to_char_dev("/sys/module/i8042/parameters/debug",
                                     NULL, "0\n"));

    exit(0);
}

static gboolean record(GError **error) {
    GIOChannel *input_channel = g_io_channel_new_file("/dev/kmsg", "r", error);
    LogMsgParseResult res;
    GIOStatus rc;

    if (!input_channel)
        return FALSE;

    /* If we're not reading from a log and we put the devices into debug mode
     * ourselves, find the spot in the kernel log to begin to read from */
    if (start_time) {
        while ((rc = parse_next_message(input_channel, &res, error)) ==
               G_IO_STATUS_NORMAL) {
            if (res.type == I8042_OUTPUT)
                continue;
            else if (res.start_time == start_time)
                break;
        }
        if (rc != G_IO_STATUS_NORMAL) {
            g_set_error_literal(error, PS2EMU_ERROR, PS2_ERROR_NO_EVENTS,
                                "Reached EOF of /dev/kmsg and got no events");
            return FALSE;
        }
    }

    while ((rc = parse_next_message(input_channel, &res, error)) ==
           G_IO_STATUS_NORMAL) {
        if (res.type == I8042_OUTPUT) {
            if (!process_event(&res.event, error))
                return FALSE;
        }
    }

    return TRUE;
}

int main(int argc, char *argv[]) {
    GOptionContext *main_context =
        g_option_context_new("record PS/2 devices");
    gboolean rc;
    GError *error = NULL;
    gchar *record_kbd_str = NULL,
          *record_aux_str = NULL;

    GOptionEntry options[] = {
        { "record-kbd", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING,
          &record_kbd_str,
          "Enable recording of the KBD (keyboard) port, disabled by default",
          "<yes|no>" },
        { "record-aux", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING,
          &record_aux_str,
          "Enable recording of the AUX (auxillary, usually the port used for "
          "cursor devices) port, enabled by default",
          "<yes|no>" },
        { 0 }
    };

    g_option_context_add_main_entries(main_context, options, NULL);
    g_option_context_set_help_enabled(main_context, TRUE);
    g_option_context_set_description(main_context,
        "Allows the recording of all of the commands going in/out of a PS/2\n"
        "port, so that they may later be replayed using a virtual PS/2\n"
        "controller on another person's machine.\n"
        "\n"
        "By default, ps2emu-record does not record keyboard input. This is\n"
        "is because recording the user's keyboard input has the consequence\n"
        "of potentially recording sensitive information, such as a user's\n"
        "password (since the user usually needs to type their password into\n"
        "their keyboard to log in). If you need to record keyboard input,\n"
        "please read the documentation for this tool first.\n");

    rc = g_option_context_parse(main_context, &argc, &argv, &error);
    if (!rc) {
        exit_on_bad_argument(main_context, TRUE,
            "Invalid options: %s", error->message);
    }

    /* Don't record the keyboard if the user didn't explicitly enable it */
    if (!record_kbd_str || strcasecmp(record_kbd_str, "no") == 0)
        record_kbd = FALSE;
    else if (strcasecmp(record_kbd_str, "yes") == 0)
        record_kbd = TRUE;
    else {
        exit_on_bad_argument(main_context, TRUE,
            "Invalid value for --record-kbd: `%s`", record_kbd_str);
    }

    /* Record the AUX port unless the user explicitly disables it */
    if (!record_aux_str || strcasecmp(record_aux_str, "yes") == 0)
        record_aux = TRUE;
    else if (strcasecmp(record_aux_str, "no") == 0)
        record_aux = FALSE;
    else {
        exit_on_bad_argument(main_context, TRUE,
            "Invalid value for --record-aux: `%s`", record_aux_str);
    }

    /* Throw an error if recording of both KBD and AUX is disabled */
    if (!record_kbd && !record_aux)
        exit_on_bad_argument(main_context, FALSE, "Nothing to record!");

    struct sigaction sigaction_struct;

    if (!enable_i8042_debugging(&error)) {
        fprintf(stderr,
                "Failed to enable i8042 debugging: %s\n",
                error->message);
        exit(1);
    }

    memset(&sigaction_struct, 0, sizeof(sigaction_struct));
    sigaction_struct.sa_handler = disable_i8042_debugging;

    g_warn_if_fail(sigaction(SIGINT, &sigaction_struct, NULL) == 0);
    g_warn_if_fail(sigaction(SIGTERM, &sigaction_struct, NULL) == 0);
    g_warn_if_fail(sigaction(SIGHUP, &sigaction_struct, NULL) == 0);

    g_option_context_free(main_context);

    rc = record(&error);
    if (!rc) {
        fprintf(stderr, "Error: %s\n",
                error->message);

        return 1;
    }
}
