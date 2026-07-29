/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef AWESOME_RENDER_SKIA_LUA_H
#define AWESOME_RENDER_SKIA_LUA_H

#ifdef __cplusplus
extern "C" {
#endif

#include <lua.h>

/* Extend the minimal C ABI module registered by luaa.c with a Skia-native
 * Canvas. The public Lua spelling intentionally follows Cairo's context API.
 */
void awesome_skia_lua_extend(lua_State *L);

#ifdef __cplusplus
}
#endif

#endif
