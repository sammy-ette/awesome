/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef AWESOME_RENDER_SKIA_LUA_H
#define AWESOME_RENDER_SKIA_LUA_H

#ifdef __cplusplus
extern "C" {
#endif

#include <lua.h>
#include <stdbool.h>
#include <stdint.h>

/* Opaque, reference-counted image used by the C/X11 side of Awesome.  It
 * keeps X11 icon/loading code out of Cairo while the actual texture is owned
 * by Skia and can be pushed directly to Lua as `skia.Image`. */
typedef struct awesome_skia_image awesome_skia_image_t;

awesome_skia_image_t *awesome_skia_image_from_argb32(int width, int height,
                                                      const uint32_t *pixels);
awesome_skia_image_t *awesome_skia_image_ref(awesome_skia_image_t *image);
void awesome_skia_image_unref(awesome_skia_image_t *image);
int awesome_skia_image_width(const awesome_skia_image_t *image);
int awesome_skia_image_height(const awesome_skia_image_t *image);
void awesome_skia_image_push_lua(lua_State *L, const awesome_skia_image_t *image);
awesome_skia_image_t *awesome_skia_image_from_lua(lua_State *L, int index);
/* Copy the alpha channel of a Skia ImageSurface or Image into caller-owned
 * storage. X Shape uses this to turn Skia-drawn masks into rectangles. */
bool awesome_skia_alpha_mask_from_lua(lua_State *L, int index, uint8_t *alpha,
                                      int width, int height);

/* Extend the minimal C ABI module registered by luaa.c with a Skia-native
 * Canvas. The public Lua spelling intentionally follows Cairo's context API.
 */
void awesome_skia_lua_extend(lua_State *L);

#ifdef __cplusplus
}
#endif

#endif
