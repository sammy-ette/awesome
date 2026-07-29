/*
 * draw.c - draw functions
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

#include "config.h"
#include "draw.h"
#include "globalconf.h"

#include <langinfo.h>
#include <errno.h>
#include <ctype.h>
#include <math.h>

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <lauxlib.h>

/* Decode image bytes once, then hand the resulting RGBA pixels to Skia. This
 * replaces the former GdkPixbuf -> Cairo image-surface bridge; Skia retains
 * its own copy and uploads it when the image is drawn by the Vulkan canvas. */
awesome_skia_image_t *
draw_image_from_pixbuf(GdkPixbuf *buf)
{
    const int width = gdk_pixbuf_get_width(buf);
    const int height = gdk_pixbuf_get_height(buf);
    const int stride = gdk_pixbuf_get_rowstride(buf);
    const int channels = gdk_pixbuf_get_n_channels(buf);
    const guchar *pixels = gdk_pixbuf_get_pixels(buf);
    uint32_t *argb;
    awesome_skia_image_t *image;

    if (width <= 0 || height <= 0 || (channels != 3 && channels != 4))
        return NULL;

    argb = p_new(uint32_t, (size_t) width * height);
    for (int y = 0; y < height; ++y)
    {
        const guchar *row = pixels + (size_t) y * stride;
        for (int x = 0; x < width; ++x)
        {
            const guchar *pixel = row + x * channels;
            const uint32_t alpha = channels == 4 ? pixel[3] : 0xff;
            argb[(size_t) y * width + x] = (alpha << 24) | (pixel[0] << 16) |
                (pixel[1] << 8) | pixel[2];
        }
    }

    image = awesome_skia_image_from_argb32(width, height, argb);
    p_delete(&argb);
    return image;
}

awesome_skia_image_t *
draw_image_from_data(int width, int height, const uint32_t *data)
{
    return awesome_skia_image_from_argb32(width, height, data);
}

awesome_skia_image_t *
draw_load_skia_image(const char *path, GError **error)
{
    GdkPixbuf *buf = gdk_pixbuf_new_from_file(path, error);
    if (!buf)
        return NULL;
    awesome_skia_image_t *image = draw_image_from_pixbuf(buf);
    g_object_unref(buf);
    return image;
}

xcb_visualtype_t *draw_find_visual(const xcb_screen_t *s, xcb_visualid_t visual)
{
    xcb_depth_iterator_t depth_iter = xcb_screen_allowed_depths_iterator(s);

    if(depth_iter.data)
        for(; depth_iter.rem; xcb_depth_next (&depth_iter))
            for(xcb_visualtype_iterator_t visual_iter = xcb_depth_visuals_iterator(depth_iter.data);
                visual_iter.rem; xcb_visualtype_next (&visual_iter))
                if(visual == visual_iter.data->visual_id)
                    return visual_iter.data;

    return NULL;
}

xcb_visualtype_t *draw_default_visual(const xcb_screen_t *s)
{
    return draw_find_visual(s, s->root_visual);
}

xcb_visualtype_t *draw_argb_visual(const xcb_screen_t *s)
{
    xcb_depth_iterator_t depth_iter = xcb_screen_allowed_depths_iterator(s);

    if(depth_iter.data)
        for(; depth_iter.rem; xcb_depth_next (&depth_iter))
            if(depth_iter.data->depth == 32)
                for(xcb_visualtype_iterator_t visual_iter = xcb_depth_visuals_iterator(depth_iter.data);
                    visual_iter.rem; xcb_visualtype_next (&visual_iter))
                    return visual_iter.data;

    return NULL;
}

uint8_t draw_visual_depth(const xcb_screen_t *s, xcb_visualid_t vis)
{
    xcb_depth_iterator_t depth_iter = xcb_screen_allowed_depths_iterator(s);

    if(depth_iter.data)
        for(; depth_iter.rem; xcb_depth_next (&depth_iter))
            for(xcb_visualtype_iterator_t visual_iter = xcb_depth_visuals_iterator(depth_iter.data);
                visual_iter.rem; xcb_visualtype_next (&visual_iter))
                if(vis == visual_iter.data->visual_id)
                    return depth_iter.data->depth;

    fatal("Could not find a visual's depth");
}

// vim: filetype=c:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:textwidth=80
