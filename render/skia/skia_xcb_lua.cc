/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "render/skia/skia_xcb_lua.h"

extern "C" {
#include <lauxlib.h>
}

#include <xcb/randr.h>
#include <xcb/xcb.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-x11.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <strings.h>
#include <type_traits>
#include <unistd.h>

namespace {

constexpr const char *k_connection_type = "awesome.skia.xcb_connection";
constexpr const char *k_window_type = "awesome.skia.xcb_window";

struct xcb_atoms
{
    xcb_atom_t wm_protocols = XCB_ATOM_NONE;
    xcb_atom_t wm_delete_window = XCB_ATOM_NONE;
    xcb_atom_t wm_name = XCB_ATOM_NONE;
    xcb_atom_t wm_class = XCB_ATOM_NONE;
    xcb_atom_t wm_normal_hints = XCB_ATOM_NONE;
    xcb_atom_t utf8_string = XCB_ATOM_NONE;
    xcb_atom_t net_wm_name = XCB_ATOM_NONE;
    xcb_atom_t net_wm_pid = XCB_ATOM_NONE;
    xcb_atom_t net_wm_window_type = XCB_ATOM_NONE;
    xcb_atom_t net_wm_window_type_normal = XCB_ATOM_NONE;
    xcb_atom_t net_wm_window_type_dock = XCB_ATOM_NONE;
    xcb_atom_t net_wm_window_type_utility = XCB_ATOM_NONE;
    xcb_atom_t net_wm_window_type_toolbar = XCB_ATOM_NONE;
    xcb_atom_t net_wm_window_type_popup_menu = XCB_ATOM_NONE;
    xcb_atom_t net_wm_state = XCB_ATOM_NONE;
    xcb_atom_t net_wm_state_above = XCB_ATOM_NONE;
    xcb_atom_t motif_wm_hints = XCB_ATOM_NONE;
};

struct lua_xcb_connection
{
    xcb_connection_t *connection = nullptr;
    xcb_screen_t *screen = nullptr;
    xcb_atoms atoms;
    uint8_t randr_event_base = 0;
    struct xkb_context *xkb_context = nullptr;
    struct xkb_keymap *xkb_keymap = nullptr;
    struct xkb_state *xkb_state = nullptr;
};

struct lua_xcb_window
{
    lua_xcb_connection *owner = nullptr;
    xcb_window_t window = XCB_NONE;
    int connection_ref = LUA_NOREF;
    bool alive = false;
};

void set_method(lua_State *L, const char *name, lua_CFunction method)
{
    lua_pushcfunction(L, method);
    lua_setfield(L, -2, name);
}

lua_xcb_connection *check_connection(lua_State *L, int index)
{
    return static_cast<lua_xcb_connection *>(
        luaL_checkudata(L, index, k_connection_type));
}

lua_xcb_window *check_window(lua_State *L, int index)
{
    return static_cast<lua_xcb_window *>(luaL_checkudata(L, index, k_window_type));
}

xcb_atom_t intern_atom(xcb_connection_t *connection, const char *name)
{
    const xcb_intern_atom_cookie_t cookie = xcb_intern_atom(
        connection, 1, static_cast<uint16_t>(std::strlen(name)), name);
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(connection, cookie, nullptr);
    if (!reply)
        return XCB_ATOM_NONE;
    const xcb_atom_t atom = reply->atom;
    std::free(reply);
    return atom;
}

void load_atoms(lua_xcb_connection *connection)
{
    xcb_connection_t *c = connection->connection;
    connection->atoms.wm_protocols = intern_atom(c, "WM_PROTOCOLS");
    connection->atoms.wm_delete_window = intern_atom(c, "WM_DELETE_WINDOW");
    connection->atoms.wm_name = XCB_ATOM_WM_NAME;
    connection->atoms.wm_class = XCB_ATOM_WM_CLASS;
    connection->atoms.wm_normal_hints = XCB_ATOM_WM_NORMAL_HINTS;
    connection->atoms.utf8_string = intern_atom(c, "UTF8_STRING");
    connection->atoms.net_wm_name = intern_atom(c, "_NET_WM_NAME");
    connection->atoms.net_wm_pid = intern_atom(c, "_NET_WM_PID");
    connection->atoms.net_wm_window_type = intern_atom(c, "_NET_WM_WINDOW_TYPE");
    connection->atoms.net_wm_window_type_normal = intern_atom(c, "_NET_WM_WINDOW_TYPE_NORMAL");
    connection->atoms.net_wm_window_type_dock = intern_atom(c, "_NET_WM_WINDOW_TYPE_DOCK");
    connection->atoms.net_wm_window_type_utility = intern_atom(c, "_NET_WM_WINDOW_TYPE_UTILITY");
    connection->atoms.net_wm_window_type_toolbar = intern_atom(c, "_NET_WM_WINDOW_TYPE_TOOLBAR");
    connection->atoms.net_wm_window_type_popup_menu = intern_atom(c, "_NET_WM_WINDOW_TYPE_POPUP_MENU");
    connection->atoms.net_wm_state = intern_atom(c, "_NET_WM_STATE");
    connection->atoms.net_wm_state_above = intern_atom(c, "_NET_WM_STATE_ABOVE");
    connection->atoms.motif_wm_hints = intern_atom(c, "_MOTIF_WM_HINTS");
}

void load_xkb(lua_xcb_connection *connection)
{
    connection->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!connection->xkb_context)
        return;

    uint16_t major = 0;
    uint16_t minor = 0;
    uint8_t event_base = 0;
    uint8_t error_base = 0;
    if (!xkb_x11_setup_xkb_extension(
            connection->connection, XKB_X11_MIN_MAJOR_XKB_VERSION,
            XKB_X11_MIN_MINOR_XKB_VERSION, XKB_X11_SETUP_XKB_EXTENSION_NO_FLAGS,
            &major, &minor, &event_base, &error_base))
        return;

    const int32_t device = xkb_x11_get_core_keyboard_device_id(connection->connection);
    if (device < 0)
        return;
    connection->xkb_keymap = xkb_x11_keymap_new_from_device(
        connection->xkb_context, connection->connection, device,
        XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!connection->xkb_keymap)
        return;
    connection->xkb_state = xkb_x11_state_new_from_device(
        connection->xkb_keymap, connection->connection, device);
}

void select_randr(lua_xcb_connection *connection)
{
    const xcb_query_extension_reply_t *extension = xcb_get_extension_data(
        connection->connection, &xcb_randr_id);
    if (!extension || !extension->present)
        return;
    connection->randr_event_base = extension->first_event;
    xcb_randr_select_input(
        connection->connection, connection->screen->root,
        XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE |
            XCB_RANDR_NOTIFY_MASK_CRTC_CHANGE |
            XCB_RANDR_NOTIFY_MASK_OUTPUT_CHANGE |
            XCB_RANDR_NOTIFY_MASK_OUTPUT_PROPERTY);
}

template <typename T>
    requires (std::is_arithmetic_v<T> &&
              !std::is_same_v<std::remove_cv_t<T>, bool>)
void set_field(lua_State *L, const char *name, T value)
{
    lua_pushinteger(L, static_cast<lua_Integer>(value));
    lua_setfield(L, -2, name);
}

void set_field(lua_State *L, const char *name, const char *value)
{
    lua_pushstring(L, value);
    lua_setfield(L, -2, name);
}

void set_field(lua_State *L, const char *name, bool value)
{
    lua_pushboolean(L, value);
    lua_setfield(L, -2, name);
}

lua_Integer table_integer(lua_State *L, int index, const char *name, lua_Integer fallback)
{
    lua_getfield(L, index, name);
    const lua_Integer value = lua_isnil(L, -1) ? fallback : static_cast<lua_Integer>(
        std::llround(luaL_checknumber(L, -1)));
    lua_pop(L, 1);
    return value;
}

bool table_boolean(lua_State *L, int index, const char *name, bool fallback)
{
    lua_getfield(L, index, name);
    const bool value = lua_isnil(L, -1) ? fallback : lua_toboolean(L, -1);
    lua_pop(L, 1);
    return value;
}

const char *table_string(lua_State *L, int index, const char *name, const char *fallback)
{
    lua_getfield(L, index, name);
    const char *value = lua_isnil(L, -1) ? fallback : luaL_checkstring(L, -1);
    lua_pop(L, 1);
    return value;
}

void push_modifiers(lua_State *L, uint16_t state)
{
    lua_newtable(L);
    int index = 1;
    const struct modifier {
        uint16_t mask;
        const char *name;
    } modifiers[] = {
        {XCB_MOD_MASK_SHIFT, "Shift"},
        {XCB_MOD_MASK_LOCK, "Lock"},
        {XCB_MOD_MASK_CONTROL, "Control"},
        {XCB_MOD_MASK_1, "Mod1"},
        {XCB_MOD_MASK_2, "Mod2"},
        {XCB_MOD_MASK_3, "Mod3"},
        {XCB_MOD_MASK_4, "Mod4"},
        {XCB_MOD_MASK_5, "Mod5"},
    };
    for (const auto &modifier : modifiers)
    {
        if (state & modifier.mask)
        {
            lua_pushstring(L, modifier.name);
            lua_rawseti(L, -2, index++);
        }
    }
}

void push_buttons(lua_State *L, uint16_t state)
{
    lua_newtable(L);
    const uint16_t masks[] = {
        XCB_BUTTON_MASK_1,
        XCB_BUTTON_MASK_2,
        XCB_BUTTON_MASK_3,
        XCB_BUTTON_MASK_4,
        XCB_BUTTON_MASK_5,
    };
    for (int i = 0; i < 5; ++i)
    {
        lua_pushboolean(L, (state & masks[i]) != 0);
        lua_rawseti(L, -2, i + 1);
    }
}

void push_pointer_fields(lua_State *L, int16_t x, int16_t y, uint16_t state)
{
    set_field(L, "x", x);
    set_field(L, "y", y);
    push_modifiers(L, state);
    lua_setfield(L, -2, "modifiers");
    push_buttons(L, state);
    lua_setfield(L, -2, "buttons");
}

void push_key_fields(lua_State *L, lua_xcb_connection *connection,
                     uint8_t keycode, bool pressed)
{
    set_field(L, "keycode", keycode);
    std::string key_name = "";
    std::string text = "";
    if (connection->xkb_state)
    {
        char buffer[128] = {};
        const xkb_keysym_t keysym = xkb_state_key_get_one_sym(
            connection->xkb_state, keycode);
        if (xkb_keysym_get_name(keysym, buffer, sizeof(buffer)) > 0)
            key_name = buffer;
        char utf8[256] = {};
        const int length = xkb_state_key_get_utf8(
            connection->xkb_state, keycode, utf8, sizeof(utf8));
        if (length > 0)
            text.assign(utf8, static_cast<size_t>(length));
    }
    if (key_name.empty())
        key_name = "#" + std::to_string(keycode);
    set_field(L, "key", key_name.c_str());
    if (!text.empty())
        set_field(L, "text", text.c_str());
    if (connection->xkb_state)
        xkb_state_update_key(connection->xkb_state, keycode,
                             pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
}

bool push_next_event(lua_State *L, lua_xcb_connection *connection)
{
    if (!connection->connection)
        return false;

    while (xcb_generic_event_t *generic = xcb_poll_for_event(connection->connection))
    {
        const uint8_t response = generic->response_type & 0x7f;
        if (connection->randr_event_base != 0 &&
                (response == connection->randr_event_base + XCB_RANDR_SCREEN_CHANGE_NOTIFY ||
                 response == connection->randr_event_base + XCB_RANDR_NOTIFY))
        {
            lua_newtable(L);
            set_field(L, "type", "monitors_changed");
            std::free(generic);
            return true;
        }

        switch (response)
        {
        case XCB_CONFIGURE_NOTIFY:
        {
            const auto *event = reinterpret_cast<xcb_configure_notify_event_t *>(generic);
            lua_newtable(L);
            set_field(L, "type", "configure");
            set_field(L, "window", static_cast<lua_Integer>(event->window));
            set_field(L, "x", event->x);
            set_field(L, "y", event->y);
            set_field(L, "width", event->width);
            set_field(L, "height", event->height);
            std::free(generic);
            return true;
        }
        case XCB_EXPOSE:
        {
            const auto *event = reinterpret_cast<xcb_expose_event_t *>(generic);
            lua_newtable(L);
            set_field(L, "type", "expose");
            set_field(L, "window", static_cast<lua_Integer>(event->window));
            std::free(generic);
            return true;
        }
        case XCB_CLIENT_MESSAGE:
        {
            const auto *event = reinterpret_cast<xcb_client_message_event_t *>(generic);
            if (event->type == connection->atoms.wm_protocols &&
                    event->data.data32[0] == connection->atoms.wm_delete_window)
            {
                lua_newtable(L);
                set_field(L, "type", "delete");
                set_field(L, "window", static_cast<lua_Integer>(event->window));
                std::free(generic);
                return true;
            }
            break;
        }
        case XCB_DESTROY_NOTIFY:
        {
            const auto *event = reinterpret_cast<xcb_destroy_notify_event_t *>(generic);
            lua_newtable(L);
            set_field(L, "type", "destroy");
            set_field(L, "window", static_cast<lua_Integer>(event->window));
            std::free(generic);
            return true;
        }
        case XCB_BUTTON_PRESS:
        case XCB_BUTTON_RELEASE:
        {
            const auto *event = reinterpret_cast<xcb_button_press_event_t *>(generic);
            lua_newtable(L);
            set_field(L, "type", response == XCB_BUTTON_PRESS ? "button_press" : "button_release");
            set_field(L, "window", static_cast<lua_Integer>(event->event));
            set_field(L, "button", event->detail);
            push_pointer_fields(L, event->event_x, event->event_y, event->state);
            std::free(generic);
            return true;
        }
        case XCB_MOTION_NOTIFY:
        {
            const auto *event = reinterpret_cast<xcb_motion_notify_event_t *>(generic);
            lua_newtable(L);
            set_field(L, "type", "motion");
            set_field(L, "window", static_cast<lua_Integer>(event->event));
            push_pointer_fields(L, event->event_x, event->event_y, event->state);
            std::free(generic);
            return true;
        }
        case XCB_ENTER_NOTIFY:
        case XCB_LEAVE_NOTIFY:
        {
            const auto *event = reinterpret_cast<xcb_enter_notify_event_t *>(generic);
            lua_newtable(L);
            set_field(L, "type", response == XCB_ENTER_NOTIFY ? "enter" : "leave");
            set_field(L, "window", static_cast<lua_Integer>(event->event));
            push_pointer_fields(L, event->event_x, event->event_y, event->state);
            std::free(generic);
            return true;
        }
        case XCB_KEY_PRESS:
        case XCB_KEY_RELEASE:
        {
            const auto *event = reinterpret_cast<xcb_key_press_event_t *>(generic);
            lua_newtable(L);
            set_field(L, "type", response == XCB_KEY_PRESS ? "key_press" : "key_release");
            set_field(L, "window", static_cast<lua_Integer>(event->event));
            push_modifiers(L, event->state);
            lua_setfield(L, -2, "modifiers");
            push_key_fields(L, connection, event->detail, response == XCB_KEY_PRESS);
            std::free(generic);
            return true;
        }
        case XCB_FOCUS_IN:
        case XCB_FOCUS_OUT:
        {
            const auto *event = reinterpret_cast<xcb_focus_in_event_t *>(generic);
            lua_newtable(L);
            set_field(L, "type", response == XCB_FOCUS_IN ? "focus_in" : "focus_out");
            set_field(L, "window", static_cast<lua_Integer>(event->event));
            std::free(generic);
            return true;
        }
        default:
            break;
        }
        std::free(generic);
    }
    return false;
}

void destroy_connection(lua_xcb_connection *connection);

int connection_gc(lua_State *L)
{
    auto *connection = check_connection(L, 1);
    destroy_connection(connection);
    return 0;
}

int connection_flush(lua_State *L)
{
    auto *connection = check_connection(L, 1);
    lua_pushboolean(L, connection->connection && xcb_flush(connection->connection) >= 0);
    return 1;
}

int connection_fd(lua_State *L)
{
    auto *connection = check_connection(L, 1);
    lua_pushinteger(L, connection->connection ? xcb_get_file_descriptor(connection->connection) : -1);
    return 1;
}

int connection_poll_event(lua_State *L)
{
    auto *connection = check_connection(L, 1);
    if (push_next_event(L, connection))
        return 1;
    lua_pushnil(L);
    return 1;
}

int connection_screen(lua_State *L)
{
    auto *connection = check_connection(L, 1);
    if (!connection->screen)
    {
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);
    set_field(L, "root", static_cast<lua_Integer>(connection->screen->root));
    set_field(L, "width", connection->screen->width_in_pixels);
    set_field(L, "height", connection->screen->height_in_pixels);
    set_field(L, "width_mm", connection->screen->width_in_millimeters);
    set_field(L, "height_mm", connection->screen->height_in_millimeters);
    return 1;
}

int connection_monitors(lua_State *L)
{
    auto *connection = check_connection(L, 1);
    lua_newtable(L);
    if (!connection->connection || !connection->screen)
        return 1;

    const xcb_randr_get_monitors_cookie_t cookie = xcb_randr_get_monitors(
        connection->connection, connection->screen->root, 1);
    xcb_randr_get_monitors_reply_t *reply = xcb_randr_get_monitors_reply(
        connection->connection, cookie, nullptr);
    if (reply)
    {
        auto iterator = xcb_randr_get_monitors_monitors_iterator(reply);
        int index = 1;
        while (iterator.rem > 0)
        {
            const auto *monitor = iterator.data;
            lua_newtable(L);
            set_field(L, "x", monitor->x);
            set_field(L, "y", monitor->y);
            set_field(L, "width", monitor->width);
            set_field(L, "height", monitor->height);
            set_field(L, "primary", monitor->primary != 0);
            set_field(L, "width_mm", monitor->width_in_millimeters);
            set_field(L, "height_mm", monitor->height_in_millimeters);
            lua_rawseti(L, -2, index++);
            xcb_randr_monitor_info_next(&iterator);
        }
        std::free(reply);
    }
    if (lua_rawlen(L, -1) == 0)
    {
        lua_newtable(L);
        set_field(L, "x", 0);
        set_field(L, "y", 0);
        set_field(L, "width", connection->screen->width_in_pixels);
        set_field(L, "height", connection->screen->height_in_pixels);
        set_field(L, "primary", true);
        set_field(L, "width_mm", connection->screen->width_in_millimeters);
        set_field(L, "height_mm", connection->screen->height_in_millimeters);
        lua_rawseti(L, -2, 1);
    }
    return 1;
}

int connection_pointer(lua_State *L)
{
    auto *connection = check_connection(L, 1);
    if (!connection->connection || !connection->screen)
    {
        lua_pushnil(L);
        return 1;
    }
    const xcb_query_pointer_cookie_t cookie = xcb_query_pointer(
        connection->connection, connection->screen->root);
    xcb_query_pointer_reply_t *reply = xcb_query_pointer_reply(
        connection->connection, cookie, nullptr);
    if (!reply)
    {
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);
    set_field(L, "x", reply->root_x);
    set_field(L, "y", reply->root_y);
    set_field(L, "window", static_cast<lua_Integer>(reply->child));
    push_modifiers(L, reply->mask);
    lua_setfield(L, -2, "modifiers");
    push_buttons(L, reply->mask);
    lua_setfield(L, -2, "buttons");
    std::free(reply);
    return 1;
}

int connection_create_window(lua_State *L)
{
    auto *connection = check_connection(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    if (!connection->connection || !connection->screen)
        return luaL_error(L, "XCB connection is closed");

    const int x = static_cast<int>(table_integer(L, 2, "x", 0));
    const int y = static_cast<int>(table_integer(L, 2, "y", 0));
    const int width = static_cast<int>(table_integer(L, 2, "width", 400));
    const int height = static_cast<int>(table_integer(L, 2, "height", 400));
    if (width <= 0 || height <= 0 || width > UINT16_MAX || height > UINT16_MAX)
        return luaL_argerror(L, 2, "window dimensions must be positive 16-bit values");

    const char *title = table_string(L, 2, "title", "Awexygen");
    const char *type = table_string(L, 2, "type", "normal");
    const bool decorated = table_boolean(L, 2, "decorated", false);
    const bool ontop = table_boolean(L, 2, "ontop", false);

    const xcb_window_t window = xcb_generate_id(connection->connection);
    const uint32_t event_mask = XCB_EVENT_MASK_EXPOSURE |
        XCB_EVENT_MASK_STRUCTURE_NOTIFY |
        XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE |
        XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
        XCB_EVENT_MASK_POINTER_MOTION |
        XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW |
        XCB_EVENT_MASK_FOCUS_CHANGE;
    xcb_create_window(
        connection->connection, connection->screen->root_depth, window,
        connection->screen->root, static_cast<int16_t>(x), static_cast<int16_t>(y),
        static_cast<uint16_t>(width), static_cast<uint16_t>(height), 0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT, connection->screen->root_visual,
        XCB_CW_EVENT_MASK, &event_mask);

    const auto &atoms = connection->atoms;
    if (atoms.wm_protocols != XCB_ATOM_NONE && atoms.wm_delete_window != XCB_ATOM_NONE)
        xcb_change_property(connection->connection, XCB_PROP_MODE_REPLACE, window,
                            atoms.wm_protocols, XCB_ATOM_ATOM, 32, 1,
                            &atoms.wm_delete_window);
    xcb_change_property(connection->connection, XCB_PROP_MODE_REPLACE, window,
                        atoms.wm_name, XCB_ATOM_STRING, 8,
                        static_cast<uint32_t>(std::strlen(title)), title);
    if (atoms.net_wm_name != XCB_ATOM_NONE && atoms.utf8_string != XCB_ATOM_NONE)
        xcb_change_property(connection->connection, XCB_PROP_MODE_REPLACE, window,
                            atoms.net_wm_name, atoms.utf8_string, 8,
                            static_cast<uint32_t>(std::strlen(title)), title);
    const char wm_class[] = "awexygen\0awexygen\0";
    xcb_change_property(connection->connection, XCB_PROP_MODE_REPLACE, window,
                        atoms.wm_class, XCB_ATOM_STRING, 8, sizeof(wm_class) - 1, wm_class);
    const uint32_t pid = static_cast<uint32_t>(getpid());
    if (atoms.net_wm_pid != XCB_ATOM_NONE)
        xcb_change_property(connection->connection, XCB_PROP_MODE_REPLACE, window,
                            atoms.net_wm_pid, XCB_ATOM_CARDINAL, 32, 1, &pid);

    xcb_atom_t window_type = atoms.net_wm_window_type_normal;
    if (strcasecmp(type, "dock") == 0)
        window_type = atoms.net_wm_window_type_dock;
    else if (strcasecmp(type, "utility") == 0)
        window_type = atoms.net_wm_window_type_utility;
    else if (strcasecmp(type, "toolbar") == 0)
        window_type = atoms.net_wm_window_type_toolbar;
    else if (strcasecmp(type, "popup_menu") == 0)
        window_type = atoms.net_wm_window_type_popup_menu;
    if (atoms.net_wm_window_type != XCB_ATOM_NONE && window_type != XCB_ATOM_NONE)
        xcb_change_property(connection->connection, XCB_PROP_MODE_REPLACE, window,
                            atoms.net_wm_window_type, XCB_ATOM_ATOM, 32, 1, &window_type);

    if (!decorated && atoms.motif_wm_hints != XCB_ATOM_NONE)
    {
        const uint32_t hints[] = {2, 0, 0, 0, 0};
        xcb_change_property(connection->connection, XCB_PROP_MODE_REPLACE, window,
                            atoms.motif_wm_hints, atoms.motif_wm_hints, 32, 5, hints);
    }
    if (ontop && atoms.net_wm_state != XCB_ATOM_NONE &&
            atoms.net_wm_state_above != XCB_ATOM_NONE)
        xcb_change_property(connection->connection, XCB_PROP_MODE_REPLACE, window,
                            atoms.net_wm_state, XCB_ATOM_ATOM, 32, 1,
                            &atoms.net_wm_state_above);

    auto *result = static_cast<lua_xcb_window *>(lua_newuserdata(
        L, sizeof(lua_xcb_window)));
    new (result) lua_xcb_window();
    result->owner = connection;
    result->window = window;
    result->alive = true;
    lua_pushvalue(L, 1);
    result->connection_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    luaL_getmetatable(L, k_window_type);
    lua_setmetatable(L, -2);
    return 1;
}

int window_gc(lua_State *L)
{
    auto *window = check_window(L, 1);
    if (window->alive && window->owner && window->owner->connection)
        xcb_destroy_window(window->owner->connection, window->window);
    if (window->connection_ref != LUA_NOREF)
        luaL_unref(L, LUA_REGISTRYINDEX, window->connection_ref);
    window->~lua_xcb_window();
    return 0;
}

int window_id(lua_State *L)
{
    auto *window = check_window(L, 1);
    lua_pushinteger(L, window->alive ? static_cast<lua_Integer>(window->window) : 0);
    return 1;
}

int window_destroy(lua_State *L)
{
    auto *window = check_window(L, 1);
    if (window->alive && window->owner && window->owner->connection)
    {
        xcb_destroy_window(window->owner->connection, window->window);
        xcb_flush(window->owner->connection);
    }
    window->alive = false;
    return 0;
}

int window_map(lua_State *L)
{
    auto *window = check_window(L, 1);
    if (window->alive && window->owner && window->owner->connection)
    {
        xcb_map_window(window->owner->connection, window->window);
        xcb_flush(window->owner->connection);
    }
    return 0;
}

int window_unmap(lua_State *L)
{
    auto *window = check_window(L, 1);
    if (window->alive && window->owner && window->owner->connection)
    {
        xcb_unmap_window(window->owner->connection, window->window);
        xcb_flush(window->owner->connection);
    }
    return 0;
}

int window_set_override_redirect(lua_State *L)
{
    auto *window = check_window(L, 1);
    if (window->alive && window->owner && window->owner->connection)
    {
        const uint32_t value = lua_toboolean(L, 2) ? 1 : 0;
        xcb_change_window_attributes(window->owner->connection, window->window,
                                     XCB_CW_OVERRIDE_REDIRECT, &value);
        xcb_flush(window->owner->connection);
    }
    return 0;
}

int window_focus(lua_State *L)
{
    auto *window = check_window(L, 1);
    if (window->alive && window->owner && window->owner->connection)
    {
        xcb_set_input_focus(window->owner->connection, XCB_INPUT_FOCUS_NONE,
                            window->window, XCB_CURRENT_TIME);
        xcb_flush(window->owner->connection);
    }
    return 0;
}

int window_move_resize(lua_State *L)
{
    auto *window = check_window(L, 1);
    if (!window->alive || !window->owner || !window->owner->connection)
        return 0;
    const uint32_t values[] = {
        static_cast<uint32_t>(std::llround(luaL_checknumber(L, 2))),
        static_cast<uint32_t>(std::llround(luaL_checknumber(L, 3))),
        static_cast<uint32_t>(std::llround(luaL_checknumber(L, 4))),
        static_cast<uint32_t>(std::llround(luaL_checknumber(L, 5))),
    };
    xcb_configure_window(window->owner->connection, window->window,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                             XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                         values);
    xcb_flush(window->owner->connection);
    return 0;
}

int window_set_title(lua_State *L)
{
    auto *window = check_window(L, 1);
    const char *title = luaL_checkstring(L, 2);
    if (!window->alive || !window->owner || !window->owner->connection)
        return 0;
    const auto &atoms = window->owner->atoms;
    xcb_change_property(window->owner->connection, XCB_PROP_MODE_REPLACE, window->window,
                        atoms.wm_name, XCB_ATOM_STRING, 8,
                        static_cast<uint32_t>(std::strlen(title)), title);
    if (atoms.net_wm_name != XCB_ATOM_NONE && atoms.utf8_string != XCB_ATOM_NONE)
        xcb_change_property(window->owner->connection, XCB_PROP_MODE_REPLACE, window->window,
                            atoms.net_wm_name, atoms.utf8_string, 8,
                            static_cast<uint32_t>(std::strlen(title)), title);
    xcb_flush(window->owner->connection);
    return 0;
}

int window_geometry(lua_State *L)
{
    auto *window = check_window(L, 1);
    if (!window->alive || !window->owner || !window->owner->connection)
    {
        lua_pushnil(L);
        return 1;
    }
    const xcb_get_geometry_cookie_t cookie = xcb_get_geometry(
        window->owner->connection, window->window);
    xcb_get_geometry_reply_t *reply = xcb_get_geometry_reply(
        window->owner->connection, cookie, nullptr);
    if (!reply)
    {
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);
    set_field(L, "x", reply->x);
    set_field(L, "y", reply->y);
    set_field(L, "width", reply->width);
    set_field(L, "height", reply->height);
    std::free(reply);
    return 1;
}

void destroy_connection(lua_xcb_connection *connection)
{
    if (connection->xkb_state)
        xkb_state_unref(connection->xkb_state);
    if (connection->xkb_keymap)
        xkb_keymap_unref(connection->xkb_keymap);
    if (connection->xkb_context)
        xkb_context_unref(connection->xkb_context);
    if (connection->connection)
        xcb_disconnect(connection->connection);
    connection->xkb_state = nullptr;
    connection->xkb_keymap = nullptr;
    connection->xkb_context = nullptr;
    connection->connection = nullptr;
    connection->screen = nullptr;
}

int xcb_connect_lua(lua_State *L)
{
    const char *display = luaL_optstring(L, 1, nullptr);
    int screen_number = 0;
    xcb_connection_t *raw = xcb_connect(display, &screen_number);
    if (!raw || xcb_connection_has_error(raw))
    {
        if (raw)
            xcb_disconnect(raw);
        return luaL_error(L, "could not connect to the X server with XCB");
    }

    auto *connection = static_cast<lua_xcb_connection *>(lua_newuserdata(
        L, sizeof(lua_xcb_connection)));
    new (connection) lua_xcb_connection();
    connection->connection = raw;
    auto iterator = xcb_setup_roots_iterator(xcb_get_setup(raw));
    for (int i = 0; i < screen_number && iterator.rem > 0; ++i)
        xcb_screen_next(&iterator);
    connection->screen = iterator.data;
    if (!connection->screen)
    {
        destroy_connection(connection);
        return luaL_error(L, "XCB did not provide a usable screen");
    }
    load_atoms(connection);
    load_xkb(connection);
    select_randr(connection);
    xcb_flush(raw);
    luaL_getmetatable(L, k_connection_type);
    lua_setmetatable(L, -2);
    return 1;
}

void register_connection(lua_State *L)
{
    luaL_newmetatable(L, k_connection_type);
    set_method(L, "__gc", connection_gc);
    set_method(L, "fd", connection_fd);
    set_method(L, "flush", connection_flush);
    set_method(L, "poll_event", connection_poll_event);
    set_method(L, "screen", connection_screen);
    set_method(L, "monitors", connection_monitors);
    set_method(L, "pointer", connection_pointer);
    set_method(L, "create_window", connection_create_window);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
}

void register_window(lua_State *L)
{
    luaL_newmetatable(L, k_window_type);
    set_method(L, "__gc", window_gc);
    set_method(L, "id", window_id);
    set_method(L, "destroy", window_destroy);
    set_method(L, "map", window_map);
    set_method(L, "unmap", window_unmap);
    set_method(L, "set_override_redirect", window_set_override_redirect);
    set_method(L, "focus", window_focus);
    set_method(L, "move_resize", window_move_resize);
    set_method(L, "set_title", window_set_title);
    set_method(L, "geometry", window_geometry);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
}

} // namespace

void awesome_skia_xcb_lua_register(lua_State *L, int skia_index)
{
    register_connection(L);
    register_window(L);
    lua_pushvalue(L, skia_index);
    lua_newtable(L);
    set_method(L, "connect", xcb_connect_lua);
    lua_setfield(L, -2, "xcb");
    lua_pop(L, 1);
}
