/*
 * xwindow.c - X window handling functions
 *
 * Copyright © 2007-2009 Julien Danjou <julien@danjou.info>
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

#include "xwindow.h"
#include "common/atoms.h"
#include "objects/button.h"

#include <xcb/xcb.h>
#include <xcb/shape.h>
#include <string.h>

/** Mask shorthands */
#define BUTTONMASK     (XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE)

/** Set client state (WM_STATE) property.
 * \param win The window to set state.
 * \param state The state to set.
 */
void
xwindow_set_state(xcb_window_t win, uint32_t state)
{
    uint32_t data[] = { state, XCB_NONE };
    xcb_change_property(globalconf.connection, XCB_PROP_MODE_REPLACE, win,
                        WM_STATE, WM_STATE, 32, 2, data);
}

/** Send request to get a window state (WM_STATE).
 * \param w A client window.
 * \return The cookie associated with the request.
 */
xcb_get_property_cookie_t
xwindow_get_state_unchecked(xcb_window_t w)
{
    return xcb_get_property_unchecked(globalconf.connection, false, w, WM_STATE,
                                      WM_STATE, 0L, 2L);
}

/** Get a window state (WM_STATE).
 * \param cookie The cookie.
 * \return The current state of the window, or 0 on error.
 */
uint32_t
xwindow_get_state_reply(xcb_get_property_cookie_t cookie)
{
    /* If no property is set, we just assume a sane default. */
    uint32_t result = XCB_ICCCM_WM_STATE_NORMAL;
    xcb_get_property_reply_t *prop_r;

    if((prop_r = xcb_get_property_reply(globalconf.connection, cookie, NULL)))
    {
        if(xcb_get_property_value_length(prop_r))
            result = *(uint32_t *) xcb_get_property_value(prop_r);

        p_delete(&prop_r);
    }

    return result;
}

/** Configure a window with its new geometry and border size.
 * \param win The X window id to configure.
 * \param geometry The new window geometry.
 * \param border The new border size.
 */
void
xwindow_configure(xcb_window_t win, area_t geometry, int border)
{
    xcb_configure_notify_event_t ce;

    ce.response_type = XCB_CONFIGURE_NOTIFY;
    ce.event = win;
    ce.window = win;
    ce.x = geometry.x + border;
    ce.y = geometry.y + border;
    ce.width = geometry.width;
    ce.height = geometry.height;
    ce.border_width = border;
    ce.above_sibling = XCB_NONE;
    ce.override_redirect = false;
    xcb_send_event(globalconf.connection, false, win,
                   XCB_EVENT_MASK_STRUCTURE_NOTIFY, (char *) &ce);
}

/** Grab or ungrab buttons on a window.
 * \param win The window.
 * \param buttons The buttons to grab.
 */
void
xwindow_buttons_grab(xcb_window_t win, button_array_t *buttons)
{
    if(win == XCB_NONE)
        return;

    /* Ungrab everything first */
    xcb_ungrab_button(globalconf.connection, XCB_BUTTON_INDEX_ANY, win, XCB_BUTTON_MASK_ANY);

    foreach(b, *buttons)
        xcb_grab_button(globalconf.connection, false, win, BUTTONMASK,
                        XCB_GRAB_MODE_SYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE,
                        (*b)->button, (*b)->modifiers);
}

/** Grab key on a window.
 * \param win The window.
 * \param k The key.
 */
static void
xwindow_grabkey(xcb_window_t win, keyb_t *k)
{
    if(k->keycode)
        xcb_grab_key(globalconf.connection, true, win,
                     k->modifiers, k->keycode, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
    else if(k->keysym)
    {
        xcb_keycode_t *keycodes = xcb_key_symbols_get_keycode(globalconf.keysyms, k->keysym);
        if(keycodes)
        {
            for(xcb_keycode_t *kc = keycodes; *kc; kc++)
                xcb_grab_key(globalconf.connection, true, win,
                             k->modifiers, *kc, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
            p_delete(&keycodes);
        }
    }
}

void
xwindow_grabkeys(xcb_window_t win, key_array_t *keys)
{
    /* Ungrab everything first */
    xcb_ungrab_key(globalconf.connection, XCB_GRAB_ANY, win, XCB_BUTTON_MASK_ANY);

    foreach(k, *keys)
        xwindow_grabkey(win, *k);
}

/** Send a request for a window's opacity.
 * \param win The window
 * \return A cookie for xwindow_get_opacity_from_reply().
 */
xcb_get_property_cookie_t
xwindow_get_opacity_unchecked(xcb_window_t win)
{
    return xcb_get_property_unchecked(globalconf.connection, false, win,
                                      _NET_WM_WINDOW_OPACITY, XCB_ATOM_CARDINAL, 0L, 1L);
}

/** Get the opacity of a window.
 * \param win The window.
 * \return The opacity, between 0 and 1 or -1 or no opacity set.
 */
double
xwindow_get_opacity(xcb_window_t win)
{
    xcb_get_property_cookie_t prop_c =
        xwindow_get_opacity_unchecked(win);
    return xwindow_get_opacity_from_cookie(prop_c);
}

/** Get the opacity of a window.
 * \param cookie A cookie for a reply to a get property request for _NET_WM_WINDOW_OPACITY.
 * \return The opacity, between 0 and 1.
 */
double
xwindow_get_opacity_from_cookie(xcb_get_property_cookie_t cookie)
{
    xcb_get_property_reply_t *prop_r =
        xcb_get_property_reply(globalconf.connection, cookie, NULL);

    if(prop_r && prop_r->value_len && prop_r->format == 32)
    {
        uint32_t value = *(uint32_t *) xcb_get_property_value(prop_r);
        p_delete(&prop_r);
        return (double) value / (double) 0xffffffff;
    }
    p_delete(&prop_r);

    return -1;
}

/** Set opacity of a window.
 * \param win The window.
 * \param opacity Opacity of the window, between 0 and 1.
 */
void
xwindow_set_opacity(xcb_window_t win, double opacity)
{
    if(win)
    {
        if(opacity >= 0 && opacity <= 1)
        {
            uint32_t real_opacity = opacity * 0xffffffff;
            xcb_change_property(globalconf.connection, XCB_PROP_MODE_REPLACE, win,
                                _NET_WM_WINDOW_OPACITY, XCB_ATOM_CARDINAL, 32, 1L, &real_opacity);
        }
        else
            xcb_delete_property(globalconf.connection, win, _NET_WM_WINDOW_OPACITY);
    }
}

/** Send WM_TAKE_FOCUS client message to window
 * \param win destination window
 */
void
xwindow_takefocus(xcb_window_t win)
{
    xcb_client_message_event_t ev;

    /* Initialize all of event's fields first */
    p_clear(&ev, 1);

    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.window = win;
    ev.format = 32;
    ev.data.data32[1] = globalconf.timestamp;
    ev.type = WM_PROTOCOLS;
    ev.data.data32[0] = WM_TAKE_FOCUS;

    xcb_send_event(globalconf.connection, false, win,
                   XCB_EVENT_MASK_NO_EVENT, (char *) &ev);
}

/** Set window cursor.
 * \param w The window.
 * \param c The cursor.
 */
void
xwindow_set_cursor(xcb_window_t w, xcb_cursor_t c)
{
    xcb_change_window_attributes(globalconf.connection, w, XCB_CW_CURSOR,
                                 (const uint32_t[]) { c });
}

/** Set a window border color.
 * \param w The window.
 * \param color The color.
 */
void
xwindow_set_border_color(xcb_window_t w, color_t *color)
{
    if(w)
        xcb_change_window_attributes(globalconf.connection, w, XCB_CW_BORDER_PIXEL, &color->pixel);
}

/** Get one of a window's shapes as an alpha Skia image. */
awesome_skia_image_t *
xwindow_get_shape(xcb_window_t win, enum xcb_shape_sk_t kind)
{
    if (!globalconf.have_shape)
        return NULL;
    if (kind == XCB_SHAPE_SK_INPUT && !globalconf.have_input_shape)
        return NULL;

    int16_t x, y;
    uint16_t width, height;
    xcb_shape_get_rectangles_cookie_t rcookie = xcb_shape_get_rectangles(globalconf.connection, win, kind);
    if (kind == XCB_SHAPE_SK_INPUT)
    {
        /* We cannot query the size/existence of an input shape... */
        xcb_get_geometry_reply_t *geom = xcb_get_geometry_reply(globalconf.connection,
                xcb_get_geometry(globalconf.connection, win), NULL);
        if (!geom)
        {
            xcb_discard_reply(globalconf.connection, rcookie.sequence);
            return NULL;
        }
        x = 0;
        y = 0;
        width = geom->width;
        height = geom->height;
    }
    else
    {
        xcb_shape_query_extents_cookie_t ecookie = xcb_shape_query_extents(globalconf.connection, win);
        xcb_shape_query_extents_reply_t *extents = xcb_shape_query_extents_reply(globalconf.connection, ecookie, NULL);
        bool shaped;

        if (!extents)
        {
            xcb_discard_reply(globalconf.connection, rcookie.sequence);
            return NULL;
        }

        if (kind == XCB_SHAPE_SK_BOUNDING)
        {
            x = extents->bounding_shape_extents_x;
            y = extents->bounding_shape_extents_y;
            width = extents->bounding_shape_extents_width;
            height = extents->bounding_shape_extents_height;
            shaped = extents->bounding_shaped;
        } else {
            check(kind == XCB_SHAPE_SK_CLIP);
            x = extents->clip_shape_extents_x;
            y = extents->clip_shape_extents_y;
            width = extents->clip_shape_extents_width;
            height = extents->clip_shape_extents_height;
            shaped = extents->clip_shaped;
        }
        p_delete(&extents);

        if (!shaped)
        {
            xcb_discard_reply(globalconf.connection, rcookie.sequence);
            return NULL;
        }
    }

    xcb_shape_get_rectangles_reply_t *rects_reply = xcb_shape_get_rectangles_reply(globalconf.connection, rcookie, NULL);
    if (!rects_reply)
    {
        return NULL;
    }

    uint32_t *pixels = p_new(uint32_t, (size_t) width * height);
    memset(pixels, 0, (size_t) width * height * sizeof(*pixels));
    int num_rects = xcb_shape_get_rectangles_rectangles_length(rects_reply);
    xcb_rectangle_t *rects = xcb_shape_get_rectangles_rectangles(rects_reply);
    for (int i = 0; i < num_rects; i++)
        for (int yy = MAX(0, rects[i].y - y);
             yy < MIN(height, rects[i].y - y + rects[i].height); yy++)
            for (int xx = MAX(0, rects[i].x - x);
                 xx < MIN(width, rects[i].x - x + rects[i].width); xx++)
                pixels[yy * width + xx] = 0xffffffffu;

    awesome_skia_image_t *result = draw_image_from_data(width, height, pixels);
    p_delete(&pixels);
    free(rects_reply);
    return result;
}

/** Set one of a window's shapes from a Skia alpha mask. */
void
xwindow_set_shape(lua_State *L, int surface_index, xcb_window_t win, int width,
                  int height, enum xcb_shape_sk_t kind, int offset)
{
    if (!globalconf.have_shape)
        return;
    if (kind == XCB_SHAPE_SK_INPUT && !globalconf.have_input_shape)
        return;

    if (lua_isnil(L, surface_index)) {
        xcb_shape_mask(globalconf.connection, XCB_SHAPE_SO_SET, kind, win,
                       offset, offset, XCB_NONE);
        return;
    }

    uint8_t *alpha = p_new(uint8_t, (size_t) width * height);
    if (!awesome_skia_alpha_mask_from_lua(L, surface_index, alpha, width, height)) {
        p_delete(&alpha);
        luaL_error(L, "shape must be a Skia surface or image matching the window size");
        return;
    }
    xcb_rectangle_t *rectangles = p_new(xcb_rectangle_t, (size_t) width * height);
    size_t count = 0;
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width;) {
            while (x < width && alpha[y * width + x] < 128)
                x++;
            int start = x;
            while (x < width && alpha[y * width + x] >= 128)
                x++;
            if (x > start)
                rectangles[count++] = (xcb_rectangle_t) { start, y, x - start, 1 };
        }
    xcb_shape_rectangles(globalconf.connection, XCB_SHAPE_SO_SET, kind,
                         XCB_CLIP_ORDERING_UNSORTED, win, offset, offset,
                         count, rectangles);
    p_delete(&rectangles);
    p_delete(&alpha);
}

/** Calculate the position change that a window needs applied.
 * \param gravity The window gravity that should be used.
 * \param change_width_before The window width difference that will be applied.
 * \param change_height_before The window height difference that will be applied.
 * \param change_width_after The window width difference that will be applied.
 * \param change_height_after The window height difference that will be applied.
 * \param dx On return, this will be changed by the amount the pixel has to be moved.
 * \param dy On return, this will be changed by the amount the pixel has to be moved.
 */
void xwindow_translate_for_gravity(xcb_gravity_t gravity, int16_t change_width_before, int16_t change_height_before,
        int16_t change_width_after, int16_t change_height_after, int16_t *dx, int16_t *dy)
{
    int16_t x = 0, y = 0;
    int16_t change_height = change_height_before + change_height_after;
    int16_t change_width = change_width_before + change_width_after;

    switch (gravity) {
    case XCB_GRAVITY_WIN_UNMAP:
    case XCB_GRAVITY_NORTH_WEST:
        break;
    case XCB_GRAVITY_NORTH:
        x = -change_width / 2;
        break;
    case XCB_GRAVITY_NORTH_EAST:
        x = -change_width;
        break;
    case XCB_GRAVITY_WEST:
        y = -change_height / 2;
        break;
    case XCB_GRAVITY_CENTER:
        x = -change_width / 2;
        y = -change_height / 2;
        break;
    case XCB_GRAVITY_EAST:
        x = -change_width;
        y = -change_height / 2;
        break;
    case XCB_GRAVITY_SOUTH_WEST:
        y = -change_height;
        break;
    case XCB_GRAVITY_SOUTH:
        x = -change_width / 2;
        y = -change_height;
        break;
    case XCB_GRAVITY_SOUTH_EAST:
        x = -change_width;
        y = -change_height;
        break;
    case XCB_GRAVITY_STATIC:
        x = -change_width_before;
        y = -change_height_before;
        break;
    }

    if (dx)
        *dx += x;
    if (dy)
        *dy += y;
}

// vim: filetype=c:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:textwidth=80
