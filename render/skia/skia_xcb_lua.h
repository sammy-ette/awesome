/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef AWESOME_RENDER_SKIA_XCB_LUA_H
#define AWESOME_RENDER_SKIA_XCB_LUA_H

#ifdef __cplusplus
extern "C" {
#endif

#include <lua.h>

#ifdef __cplusplus
}
#endif

/* Register the XCB window and event API on the existing skia Lua table. */
void awesome_skia_xcb_lua_register(lua_State *L, int skia_index);

#endif
