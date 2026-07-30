/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Experimental Skia/Vulkan renderer for AwesomeWM.
 *
 * This header intentionally exposes a C ABI. Skia and Vulkan implementation
 * details stay in skia_backend.cc and must not leak into Awesome's C sources.
 */
#ifndef AWESOME_RENDER_SKIA_BACKEND_H
#define AWESOME_RENDER_SKIA_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <xcb/xcb.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct awesome_skia_renderer awesome_skia_renderer_t;
typedef struct awesome_skia_frame awesome_skia_frame_t;

/** Aggregate timing for all live Skia renderers sharing the Vulkan device.
 * Values are CPU wall-clock nanoseconds accumulated over window_nanoseconds.
 * `record_nanoseconds` covers Lua/Skia command recording between begin_frame
 * and end_frame; it is not a GPU timestamp. */
typedef struct awesome_skia_timing_stats_t {
    uint64_t frames;
    uint64_t content_repaints;
    uint64_t cache_replays;
    uint64_t acquire_nanoseconds;
    uint64_t record_nanoseconds;
    uint64_t submit_nanoseconds;
    uint64_t window_nanoseconds;
} awesome_skia_timing_stats_t;

/** Read shared timing counters. When reset is true, start a fresh interval
 * immediately after producing this snapshot. */
bool awesome_skia_get_timing_stats(awesome_skia_timing_stats_t *stats, bool reset);

/** Create a Skia/Vulkan renderer for an existing XCB window.
 *
 * The window must already be created and mapped by the caller. The renderer
 * opens its own XCB connection for Vulkan WSI so presentation cannot consume
 * the window manager's X events.
 */
awesome_skia_renderer_t *awesome_skia_renderer_create(
    xcb_connection_t *connection,
    xcb_window_t window,
    uint32_t width,
    uint32_t height,
    char *error,
    size_t error_size);

/** Destroy the renderer and all owned Vulkan/Skia resources. */
void awesome_skia_renderer_destroy(awesome_skia_renderer_t *renderer);

/** Recreate the swapchain after the X11 window size changes. */
bool awesome_skia_renderer_resize(
    awesome_skia_renderer_t *renderer,
    uint32_t width,
    uint32_t height,
    char *error,
    size_t error_size);

/** Begin a GPU frame. The returned frame owns one acquired swapchain image.
 *
 * A renderer has at most one active frame.  Finish it with
 * awesome_skia_renderer_end_frame() before beginning another one.  All canvas
 * operations below record into a persistent Vulkan-backed content surface;
 * presentation then composites that surface into the acquired swapchain image.
 */
awesome_skia_frame_t *awesome_skia_renderer_begin_frame(
    awesome_skia_renderer_t *renderer,
    char *error,
    size_t error_size);

/** Submit and present a frame returned by awesome_skia_renderer_begin_frame().
 * This consumes frame whether it succeeds or fails.
 */
bool awesome_skia_renderer_end_frame(
    awesome_skia_frame_t *frame,
    char *error,
    size_t error_size);

/** True for the first frame after the renderer's retained content surface was
 * created or resized. The caller must repaint its whole logical surface. */
bool awesome_skia_frame_needs_full_redraw(const awesome_skia_frame_t *frame);
void awesome_skia_frame_mark_content_repaint(awesome_skia_frame_t *frame);

/** Minimal GPU canvas ABI used by the first Skia-native drawing clients.
 * Colors use 0xRRGGBBAA. Coordinates are in device pixels.
 */
void awesome_skia_frame_clear(awesome_skia_frame_t *frame, uint32_t rgba);
void awesome_skia_frame_save(awesome_skia_frame_t *frame);
void awesome_skia_frame_restore(awesome_skia_frame_t *frame);
void awesome_skia_frame_translate(awesome_skia_frame_t *frame, float x, float y);
void awesome_skia_frame_scale(awesome_skia_frame_t *frame, float x, float y);
void awesome_skia_frame_clip_rect(awesome_skia_frame_t *frame,
                                  float x, float y, float width, float height);
void awesome_skia_frame_draw_rect(awesome_skia_frame_t *frame,
                                  float x, float y, float width, float height,
                                  uint32_t rgba);
void awesome_skia_frame_draw_round_rect(awesome_skia_frame_t *frame,
                                        float x, float y, float width, float height,
                                        float radius_x, float radius_y,
                                        uint32_t rgba);
void awesome_skia_frame_draw_circle(awesome_skia_frame_t *frame,
                                    float x, float y, float radius, uint32_t rgba);

/** Render and present a diagnostic frame.
 *
 * phase is expected to be in the range [0, 1), but other values are accepted.
 */
bool awesome_skia_renderer_draw_demo(
    awesome_skia_renderer_t *renderer,
    float phase,
    char *error,
    size_t error_size);

#ifdef __cplusplus
}

class SkCanvas;

/* Internal C++ hook for Awesome's Lua binding. It is intentionally outside the
 * C ABI above so C sources can never accidentally depend on Skia headers. */
SkCanvas *awesome_skia_frame_canvas(awesome_skia_frame_t *frame);

#endif

#endif
