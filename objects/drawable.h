/*
 * drawable.h - drawable functions header
 *
 * Copyright © 2007-2009 Julien Danjou <julien@danjou.info>
 * Copyright © 2010-2012 Uli Schlachter <psychon@znc.in>
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
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

#ifndef AWESOME_OBJECTS_DRAWABLE_H
#define AWESOME_OBJECTS_DRAWABLE_H

#include "common/luaclass.h"
#include "draw.h"

#include "render/skia/skia_backend.h"

typedef void drawable_refresh_callback(void *);

/** drawable type */
struct drawable_t
{
    LUA_OBJECT_HEADER
    /** The pixmap we are drawing to. */
    xcb_pixmap_t pixmap;
    /** Vulkan/Skia presenter for an X11 window, if this drawable owns one. */
    awesome_skia_renderer_t *skia_renderer;
    /** Size of the swapchain currently owned by skia_renderer. Geometry is
     * updated eagerly for Lua, while a Vulkan resize is deferred until the
     * coalesced wibox redraw that will actually present it. */
    uint16_t skia_renderer_width;
    uint16_t skia_renderer_height;
    /** Largest backing size requested by a drawin. Drawins present through a
     * child window so their outer window can animate bounds without resizing
     * the Vulkan swapchain every frame. */
    uint16_t skia_capacity_width;
    uint16_t skia_capacity_height;
    /** The part of a stable backing currently exposed by its outer drawin.
     * Lua keeps laying widgets out against the capacity, but clips drawing to
     * these bounds while the drawin is animated open or closed. */
    uint16_t skia_visible_width;
    uint16_t skia_visible_height;
    bool skia_stable_backing;
    /** The X11 window to which the renderer presents directly. */
    xcb_window_t presentation_window;
    /** The geometry of the drawable (in root window coordinates). */
    area_t geometry;
    /** Surface contents are undefined if this is false. */
    bool refreshed;
    /** Callback for refreshing. */
    drawable_refresh_callback *refresh_callback;
    /** Data for refresh callback. */
    void *refresh_data;
};
typedef struct drawable_t drawable_t;

drawable_t *drawable_allocator(lua_State *, drawable_refresh_callback *, void *,
                               xcb_window_t);
void drawable_set_geometry(lua_State *, int, area_t);
void drawable_ensure_renderer(drawable_t *);
void drawable_class_setup(lua_State *);

#endif
// vim: filetype=c:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:textwidth=80
