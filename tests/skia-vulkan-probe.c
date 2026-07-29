/* SPDX-License-Identifier: GPL-2.0-or-later */
/* A tiny C-only caller for the experimental Skia/Vulkan renderer. */

#include "render/skia/skia_backend.h"

#include <math.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <xcb/xcb.h>

static double monotonic_seconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0.0;
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

int main(void)
{
    int screen_number = 0;
    xcb_connection_t *connection = xcb_connect(NULL, &screen_number);
    if (!connection || xcb_connection_has_error(connection))
    {
        fprintf(stderr, "Could not connect to the X server\n");
        return EXIT_FAILURE;
    }

    const xcb_setup_t *setup = xcb_get_setup(connection);
    xcb_screen_iterator_t iterator = xcb_setup_roots_iterator(setup);
    for (int index = 0; index < screen_number; ++index)
        xcb_screen_next(&iterator);
    xcb_screen_t *screen = iterator.data;
    if (!screen)
    {
        fprintf(stderr, "Could not find the default X screen\n");
        xcb_disconnect(connection);
        return EXIT_FAILURE;
    }

    uint32_t width = 720;
    uint32_t height = 320;
    xcb_window_t window = xcb_generate_id(connection);
    uint32_t values[] = {
        screen->black_pixel,
        XCB_EVENT_MASK_EXPOSURE |
        XCB_EVENT_MASK_STRUCTURE_NOTIFY |
        XCB_EVENT_MASK_KEY_PRESS,
    };
    xcb_create_window(connection,
                      XCB_COPY_FROM_PARENT,
                      window,
                      screen->root,
                      60, 60,
                      (uint16_t)width, (uint16_t)height,
                      0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      screen->root_visual,
                      XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK,
                      values);

    const char title[] = "AwesomeWM Skia/Vulkan probe";
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window,
                        XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
                        (uint32_t)(sizeof(title) - 1), title);
    xcb_map_window(connection, window);
    xcb_flush(connection);

    char error[512] = {0};
    awesome_skia_renderer_t *renderer = awesome_skia_renderer_create(
        connection, window, width, height, error, sizeof(error));
    if (!renderer)
    {
        fprintf(stderr, "Renderer creation failed: %s\n", error);
        xcb_destroy_window(connection, window);
        xcb_disconnect(connection);
        return EXIT_FAILURE;
    }

    const int xfd = xcb_get_file_descriptor(connection);
    const double started = monotonic_seconds();
    bool running = true;

    while (running)
    {
        struct pollfd pollfd = {
            .fd = xfd,
            .events = POLLIN,
            .revents = 0,
        };
        (void)poll(&pollfd, 1, 16);

        xcb_generic_event_t *event;
        while ((event = xcb_poll_for_event(connection)) != NULL)
        {
            const uint8_t type = event->response_type & ~0x80;
            if (type == XCB_KEY_PRESS)
            {
                running = false;
            }
            else if (type == XCB_DESTROY_NOTIFY)
            {
                running = false;
            }
            else if (type == XCB_CONFIGURE_NOTIFY)
            {
                const xcb_configure_notify_event_t *configure =
                    (const xcb_configure_notify_event_t *)event;
                if (configure->width != width || configure->height != height)
                {
                    width = configure->width;
                    height = configure->height;
                    if (width > 0 && height > 0 &&
                        !awesome_skia_renderer_resize(renderer, width, height,
                                                     error, sizeof(error)))
                    {
                        fprintf(stderr, "Resize failed: %s\n", error);
                        running = false;
                    }
                }
            }
            free(event);
        }

        if (!running || width == 0 || height == 0)
            continue;

        const double elapsed = monotonic_seconds() - started;
        const float phase = (float)fmod(elapsed / 3.0, 1.0);
        if (!awesome_skia_renderer_draw_demo(renderer, phase,
                                             error, sizeof(error)))
        {
            fprintf(stderr, "Rendering failed: %s\n", error);
            running = false;
        }
    }

    awesome_skia_renderer_destroy(renderer);
    xcb_destroy_window(connection, window);
    xcb_flush(connection);
    xcb_disconnect(connection);
    return EXIT_SUCCESS;
}
