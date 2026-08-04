/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "render/skia/skia_lua.h"

/* Keep the Lua module entry point separate from the implementation archive.
 * Awesome links the archive into its executable, while standalone Lua users
 * load this thin shared-module wrapper. */
extern "C" int luaopen_skia(lua_State *L)
{
    return awesome_skia_lua_open(L);
}
