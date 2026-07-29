/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Lua-facing Skia canvas. This is deliberately a Cairo-shaped drawing API:
 * widgets keep receiving a mutable context with paths, transforms, clipping,
 * source colours, fill/stroke and save/restore. Unlike Cairo, every operation
 * targets the Skia surface acquired from the Vulkan swapchain for this frame.
 */

#include "render/skia/skia_lua.h"

#include "render/skia/skia_backend.h"

extern "C" {
#include <lauxlib.h>
}

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkData.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkImage.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkRect.h"
#include "include/core/SkShader.h"
#include "include/core/SkSurface.h"
#include "include/effects/SkGradient.h"
#include "include/ports/SkFontMgr_fontconfig.h"
#include "include/ports/SkFontScanner_FreeType.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr const char *k_frame_type = "awesome.skia.frame";
constexpr const char *k_pattern_type = "awesome.skia.pattern";
constexpr const char *k_image_type = "awesome.skia.image";

std::unordered_map<std::string, sk_sp<SkImage>> encoded_images;

struct canvas_state
{
    SkPaint paint;
};

struct lua_skia_frame
{
    /* Must stay first: the small C fallback registered from luaa.c has this
     * layout too, which keeps GC safe during an error while modules load. */
    awesome_skia_frame_t *frame = nullptr;
    /* Set instead of `frame` for an offscreen canvas created by
     * skia.new_image_surface(); such a canvas has no swapchain/present cycle
     * and is finished with :snapshot() instead of :present(). */
    sk_sp<SkSurface> offscreen;
    SkPathBuilder path;
    canvas_state state;
    std::vector<canvas_state> states;
    SkPoint current = {0, 0};
};

/* A decoded or rendered image ready to be drawn with frame:draw_image(). */
struct lua_skia_image
{
    sk_sp<SkImage> image;
};

enum class pattern_kind
{
    solid,
    linear,
    radial,
};

struct lua_skia_pattern
{
    pattern_kind kind = pattern_kind::solid;
    SkColor color = SK_ColorBLACK;
    SkPoint start = {0, 0};
    SkPoint end = {0, 0};
    SkScalar start_radius = 0;
    SkScalar end_radius = 0;
    SkTileMode tile_mode = SkTileMode::kClamp;
    std::vector<SkColor4f> colors;
    std::vector<SkScalar> offsets;
    sk_sp<SkShader> shader;
};

lua_skia_pattern *test_pattern(lua_State *L, int index)
{
    if (!lua_isuserdata(L, index) || !lua_getmetatable(L, index))
        return nullptr;
    luaL_getmetatable(L, k_pattern_type);
    const bool matches = lua_rawequal(L, -1, -2);
    lua_pop(L, 2);
    return matches ? static_cast<lua_skia_pattern *>(lua_touserdata(L, index)) : nullptr;
}

lua_skia_frame *check_frame(lua_State *L, int index)
{
    auto *frame = static_cast<lua_skia_frame *>(luaL_checkudata(L, index, k_frame_type));
    if (frame->offscreen)
        return frame;
    if (!frame->frame || !awesome_skia_frame_canvas(frame->frame))
        luaL_error(L, "Skia frame has already been presented");
    return frame;
}

int skia_is_canvas(lua_State *L)
{
    lua_pushboolean(L, luaL_testudata(L, 1, k_frame_type) != nullptr);
    return 1;
}

SkCanvas *canvas(lua_skia_frame *frame)
{
    if (frame->offscreen)
        return frame->offscreen->getCanvas();
    return awesome_skia_frame_canvas(frame->frame);
}

lua_skia_image *test_image(lua_State *L, int index)
{
    if (!lua_isuserdata(L, index) || !lua_getmetatable(L, index))
        return nullptr;
    luaL_getmetatable(L, k_image_type);
    const bool matches = lua_rawequal(L, -1, -2);
    lua_pop(L, 2);
    return matches ? static_cast<lua_skia_image *>(lua_touserdata(L, index)) : nullptr;
}

int skia_is_image(lua_State *L)
{
    lua_pushboolean(L, test_image(L, 1) != nullptr);
    return 1;
}

sk_sp<SkFontMgr> font_manager()
{
    static sk_sp<SkFontMgr> manager =
        SkFontMgr_New_FontConfig(nullptr, SkFontScanner_Make_FreeType());
    return manager;
}

sk_sp<SkImage> load_encoded_image(const char *path)
{
    const auto cached = encoded_images.find(path);
    if (cached != encoded_images.end())
        return cached->second;

    sk_sp<SkData> data = SkData::MakeFromFileName(path);
    if (!data)
        return nullptr;
    sk_sp<SkImage> image = SkImages::DeferredFromEncodedData(std::move(data));
    if (image)
        encoded_images.emplace(path, image);
    return image;
}

uint8_t component(double value)
{
    return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
}

SkColor rgba(double red, double green, double blue, double alpha)
{
    return SkColorSetARGB(component(alpha), component(red), component(green), component(blue));
}

const char *pattern_kind_name(pattern_kind kind)
{
    switch (kind)
    {
    case pattern_kind::solid:
        return "SOLID";
    case pattern_kind::linear:
        return "LINEAR";
    case pattern_kind::radial:
        return "RADIAL";
    }
    return "UNKNOWN";
}

void refresh_gradient(lua_skia_pattern *pattern)
{
    pattern->shader.reset();
    if (pattern->kind == pattern_kind::solid || pattern->colors.empty())
        return;

    std::vector<SkColor4f> colors = pattern->colors;
    std::vector<SkScalar> offsets = pattern->offsets;
    if (colors.size() == 1)
    {
        colors.push_back(colors.front());
        offsets = {0, 1};
    }

    const SkGradient gradient(
        SkGradient::Colors(SkSpan<const SkColor4f>(colors.data(), colors.size()),
                           SkSpan<const float>(offsets.data(), offsets.size()), pattern->tile_mode),
        {});
    if (pattern->kind == pattern_kind::linear)
    {
        const SkPoint points[] = {pattern->start, pattern->end};
        pattern->shader = SkShaders::LinearGradient(points, gradient);
    }
    else
    {
        pattern->shader = SkShaders::TwoPointConicalGradient(
            pattern->start, pattern->start_radius, pattern->end, pattern->end_radius,
            gradient);
    }
}

lua_skia_pattern *new_pattern(lua_State *L)
{
    void *storage = lua_newuserdata(L, sizeof(lua_skia_pattern));
    auto *pattern = new (storage) lua_skia_pattern;
    luaL_getmetatable(L, k_pattern_type);
    lua_setmetatable(L, -2);
    return pattern;
}

bool hex_digit(char character, uint8_t *result)
{
    if (character >= '0' && character <= '9')
        *result = static_cast<uint8_t>(character - '0');
    else if (character >= 'a' && character <= 'f')
        *result = static_cast<uint8_t>(character - 'a' + 10);
    else if (character >= 'A' && character <= 'F')
        *result = static_cast<uint8_t>(character - 'A' + 10);
    else
        return false;
    return true;
}

bool parse_hex_color(const char *value, size_t size, SkColor *result)
{
    if (!value || size < 4 || value[0] != '#')
        return false;

    if (size == 4 || size == 5)
    {
        uint8_t r, g, b, a = 15;
        if (!hex_digit(value[1], &r) || !hex_digit(value[2], &g) ||
            !hex_digit(value[3], &b) || (size == 5 && !hex_digit(value[4], &a)))
            return false;
        *result = SkColorSetARGB(a * 17, r * 17, g * 17, b * 17);
        return true;
    }
    if (size != 7 && size != 9)
        return false;

    std::array<uint8_t, 4> bytes = {0, 0, 0, 255};
    for (size_t index = 0; index < (size - 1) / 2; ++index)
    {
        uint8_t high, low;
        if (!hex_digit(value[1 + index * 2], &high) ||
            !hex_digit(value[2 + index * 2], &low))
            return false;
        bytes[index] = static_cast<uint8_t>((high << 4) | low);
    }
    *result = SkColorSetARGB(bytes[3], bytes[0], bytes[1], bytes[2]);
    return true;
}

SkColor check_color(lua_State *L, int index)
{
    if (lua_isnumber(L, index))
    {
        const lua_Integer raw = luaL_checkinteger(L, index);
        if (raw < 0 || static_cast<uint64_t>(raw) > UINT32_MAX)
            luaL_argerror(L, index, "expected 0xRRGGBBAA");
        const uint32_t color = static_cast<uint32_t>(raw);
        return SkColorSetARGB(color & 0xff, (color >> 24) & 0xff,
                              (color >> 16) & 0xff, (color >> 8) & 0xff);
    }
    size_t size = 0;
    const char *string = luaL_checklstring(L, index, &size);
    SkColor color;
    if (!parse_hex_color(string, size, &color))
        luaL_argerror(L, index, "expected #rgb, #rgba, #rrggbb, or #rrggbbaa");
    return color;
}

/* Cairo's Lua binding hands operators through as the numeric
 * cairo_operator_t value (e.g. cairo.Operator.CLEAR == 0), not a string, so
 * that is the primary form here; a string fallback stays for direct
 * skia-only callers. */
SkBlendMode blend_mode_from_operator(lua_State *L, int index)
{
    if (lua_isnumber(L, index))
    {
        switch (static_cast<int>(lua_tointeger(L, index)))
        {
        case 0: return SkBlendMode::kClear;  // CAIRO_OPERATOR_CLEAR
        case 1: return SkBlendMode::kSrc;    // CAIRO_OPERATOR_SOURCE
        case 3: return SkBlendMode::kSrcIn;  // CAIRO_OPERATOR_IN
        default: return SkBlendMode::kSrcOver; // CAIRO_OPERATOR_OVER and others
        }
    }
    const char *op = lua_tostring(L, index);
    if (op && (std::strcmp(op, "SOURCE") == 0 || std::strcmp(op, "source") == 0))
        return SkBlendMode::kSrc;
    if (op && (std::strcmp(op, "CLEAR") == 0 || std::strcmp(op, "clear") == 0))
        return SkBlendMode::kClear;
    return SkBlendMode::kSrcOver;
}

void reset_path(lua_skia_frame *frame)
{
    const SkPathFillType fill_type = frame->path.fillType();
    frame->path = SkPathBuilder(fill_type);
}

int frame_begin(lua_State *L)
{
    auto *renderer = static_cast<awesome_skia_renderer_t *>(lua_touserdata(L, 1));
    if (!renderer)
        return luaL_argerror(L, 1, "expected drawable.skia_renderer");

    char error[256] = {0};
    awesome_skia_frame_t *native = awesome_skia_renderer_begin_frame(renderer, error, sizeof(error));
    if (!native)
        return luaL_error(L, "could not begin Skia frame: %s", error);

    auto *frame = static_cast<lua_skia_frame *>(lua_newuserdata(L, sizeof(lua_skia_frame)));
    new (frame) lua_skia_frame();
    frame->frame = native;
    frame->state.paint.setAntiAlias(true);
    frame->states.push_back(frame->state);
    luaL_getmetatable(L, k_frame_type);
    lua_setmetatable(L, -2);
    return 1;
}

int frame_gc(lua_State *L)
{
    auto *frame = static_cast<lua_skia_frame *>(luaL_checkudata(L, 1, k_frame_type));
    if (frame->frame)
    {
        char ignored[256] = {0};
        awesome_skia_renderer_end_frame(frame->frame, ignored, sizeof(ignored));
        frame->frame = nullptr;
    }
    frame->~lua_skia_frame();
    return 0;
}

int frame_present(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    if (frame->offscreen)
        return luaL_error(L, "call :snapshot() on an offscreen image surface instead of :present()");
    char error[256] = {0};
    awesome_skia_frame_t *native = frame->frame;
    frame->frame = nullptr;
    if (!awesome_skia_renderer_end_frame(native, error, sizeof(error)))
        return luaL_error(L, "could not present Skia frame: %s", error);
    return 0;
}

int frame_clear(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    canvas(frame)->clear(check_color(L, 2));
    return 0;
}

int frame_save(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    canvas(frame)->save();
    frame->states.push_back(frame->state);
    return 0;
}

int frame_restore(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    if (frame->states.size() <= 1)
        return luaL_error(L, "Skia canvas restore without matching save");
    canvas(frame)->restore();
    frame->states.pop_back();
    frame->state = frame->states.back();
    return 0;
}

int frame_translate(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    canvas(frame)->translate(luaL_checknumber(L, 2), luaL_checknumber(L, 3));
    return 0;
}

int frame_rotate(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    canvas(frame)->rotate(luaL_checknumber(L, 2) * 180.0f / static_cast<float>(M_PI));
    return 0;
}

int frame_scale(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    canvas(frame)->scale(luaL_checknumber(L, 2), luaL_checknumber(L, 3));
    return 0;
}

int frame_transform_matrix(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    const char *names[] = { "xx", "yx", "xy", "yy", "x0", "y0" };
    SkScalar values[6] = {};
    for (int index = 0; index < 6; ++index)
    {
        lua_getfield(L, 2, names[index]);
        values[index] = luaL_checknumber(L, -1);
        lua_pop(L, 1);
    }
    SkMatrix matrix;
    matrix.setAll(values[0], values[2], values[4], values[1], values[3], values[5],
                  0, 0, 1);
    canvas(frame)->concat(matrix);
    return 0;
}

int frame_clip(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    canvas(frame)->clipPath(frame->path.snapshot(), SkClipOp::kIntersect, true);
    reset_path(frame);
    return 0;
}

int frame_clip_rect(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    canvas(frame)->clipRect(SkRect::MakeXYWH(luaL_checknumber(L, 2), luaL_checknumber(L, 3),
                                              luaL_checknumber(L, 4), luaL_checknumber(L, 5)));
    return 0;
}

int frame_clip_extents(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    SkRect bounds;
    if (!canvas(frame)->getLocalClipBounds(&bounds))
        bounds = SkRect::MakeEmpty();
    lua_pushnumber(L, bounds.left());
    lua_pushnumber(L, bounds.top());
    lua_pushnumber(L, bounds.right());
    lua_pushnumber(L, bounds.bottom());
    return 4;
}

int frame_rectangle(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    frame->path.addRect(SkRect::MakeXYWH(luaL_checknumber(L, 2), luaL_checknumber(L, 3),
                                          luaL_checknumber(L, 4), luaL_checknumber(L, 5)));
    return 0;
}

int frame_move_to(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    frame->current = { static_cast<SkScalar>(luaL_checknumber(L, 2)),
                       static_cast<SkScalar>(luaL_checknumber(L, 3)) };
    frame->path.moveTo(frame->current);
    return 0;
}

int frame_line_to(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    frame->current = { static_cast<SkScalar>(luaL_checknumber(L, 2)),
                       static_cast<SkScalar>(luaL_checknumber(L, 3)) };
    frame->path.lineTo(frame->current);
    return 0;
}

int frame_rel_move_to(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    frame->current = { frame->current.fX + static_cast<SkScalar>(luaL_checknumber(L, 2)),
                       frame->current.fY + static_cast<SkScalar>(luaL_checknumber(L, 3)) };
    frame->path.moveTo(frame->current);
    return 0;
}

int frame_rel_line_to(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    frame->current = { frame->current.fX + static_cast<SkScalar>(luaL_checknumber(L, 2)),
                       frame->current.fY + static_cast<SkScalar>(luaL_checknumber(L, 3)) };
    frame->path.lineTo(frame->current);
    return 0;
}

int frame_curve_to(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    const SkScalar x1 = luaL_checknumber(L, 2);
    const SkScalar y1 = luaL_checknumber(L, 3);
    const SkScalar x2 = luaL_checknumber(L, 4);
    const SkScalar y2 = luaL_checknumber(L, 5);
    frame->current = { static_cast<SkScalar>(luaL_checknumber(L, 6)),
                       static_cast<SkScalar>(luaL_checknumber(L, 7)) };
    frame->path.cubicTo(x1, y1, x2, y2, frame->current.fX, frame->current.fY);
    return 0;
}

int frame_close_path(lua_State *L)
{
    check_frame(L, 1)->path.close();
    return 0;
}

int frame_new_path(lua_State *L)
{
    reset_path(check_frame(L, 1));
    return 0;
}

int frame_arc(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    const float x = luaL_checknumber(L, 2);
    const float y = luaL_checknumber(L, 3);
    const float radius = luaL_checknumber(L, 4);
    const float start = luaL_checknumber(L, 5);
    const float finish = luaL_checknumber(L, 6);
    float sweep = finish - start;
    while (sweep < 0)
        sweep += 2.0f * static_cast<float>(M_PI);
    if (sweep > 2.0f * static_cast<float>(M_PI))
        sweep = 2.0f * static_cast<float>(M_PI);
    frame->path.arcTo(SkRect::MakeXYWH(x - radius, y - radius, radius * 2, radius * 2),
                      start * 180.0f / static_cast<float>(M_PI),
                      sweep * 180.0f / static_cast<float>(M_PI), false);
    frame->current = { x + radius * std::cos(finish), y + radius * std::sin(finish) };
    return 0;
}

int frame_arc_negative(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    const float x = luaL_checknumber(L, 2);
    const float y = luaL_checknumber(L, 3);
    const float radius = luaL_checknumber(L, 4);
    const float start = luaL_checknumber(L, 5);
    const float finish = luaL_checknumber(L, 6);
    float sweep = finish - start;
    while (sweep > 0)
        sweep -= 2.0f * static_cast<float>(M_PI);
    if (sweep < -2.0f * static_cast<float>(M_PI))
        sweep = -2.0f * static_cast<float>(M_PI);
    frame->path.arcTo(SkRect::MakeXYWH(x - radius, y - radius, radius * 2, radius * 2),
                      start * 180.0f / static_cast<float>(M_PI),
                      sweep * 180.0f / static_cast<float>(M_PI), false);
    frame->current = { x + radius * std::cos(finish), y + radius * std::sin(finish) };
    return 0;
}

int frame_fill(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    SkPaint paint = frame->state.paint;
    paint.setStyle(SkPaint::kFill_Style);
    canvas(frame)->drawPath(frame->path.detach(), paint);
    return 0;
}

int frame_fill_preserve(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    SkPaint paint = frame->state.paint;
    paint.setStyle(SkPaint::kFill_Style);
    canvas(frame)->drawPath(frame->path.snapshot(), paint);
    return 0;
}

int frame_stroke(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    SkPaint paint = frame->state.paint;
    paint.setStyle(SkPaint::kStroke_Style);
    canvas(frame)->drawPath(frame->path.detach(), paint);
    return 0;
}

int frame_stroke_preserve(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    SkPaint paint = frame->state.paint;
    paint.setStyle(SkPaint::kStroke_Style);
    canvas(frame)->drawPath(frame->path.snapshot(), paint);
    return 0;
}

int frame_paint(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    canvas(frame)->drawPaint(frame->state.paint);
    return 0;
}

int frame_paint_with_alpha(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    SkPaint paint = frame->state.paint;
    paint.setAlphaf(std::clamp(static_cast<float>(luaL_checknumber(L, 2)), 0.0f, 1.0f));
    canvas(frame)->drawPaint(paint);
    return 0;
}

int frame_set_source_rgba(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    frame->state.paint.setShader(nullptr);
    frame->state.paint.setColor(rgba(luaL_checknumber(L, 2), luaL_checknumber(L, 3),
                                     luaL_checknumber(L, 4), luaL_checknumber(L, 5)));
    return 0;
}

int frame_set_source_rgb(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    frame->state.paint.setShader(nullptr);
    frame->state.paint.setColor(rgba(luaL_checknumber(L, 2), luaL_checknumber(L, 3),
                                     luaL_checknumber(L, 4), 1));
    return 0;
}

int frame_set_source(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    frame->state.paint.setShader(nullptr);
    if (lua_skia_pattern *pattern = test_pattern(L, 2))
    {
        if (pattern->shader)
            frame->state.paint.setShader(pattern->shader);
        else
            frame->state.paint.setColor(pattern->color);
    }
    else
        frame->state.paint.setColor(check_color(L, 2));
    return 0;
}

int frame_set_line_width(lua_State *L)
{
    check_frame(L, 1)->state.paint.setStrokeWidth(luaL_checknumber(L, 2));
    return 0;
}

int frame_set_fill_rule(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    const char *rule = luaL_checkstring(L, 2);
    frame->path.setFillType(std::strcmp(rule, "EVEN_ODD") == 0 ||
                            std::strcmp(rule, "even_odd") == 0
                                ? SkPathFillType::kEvenOdd
                                : SkPathFillType::kWinding);
    return 0;
}

int frame_show_text(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    size_t length = 0;
    const char *text = luaL_checklstring(L, 2, &length);
    const char *family = luaL_optstring(L, 5, "sans");
    const float size = static_cast<float>(luaL_optnumber(L, 6, 12));
    sk_sp<SkTypeface> typeface;
    if (sk_sp<SkFontMgr> manager = font_manager())
        typeface = manager->matchFamilyStyle(family, SkFontStyle());
    SkFont font(typeface, std::max(size, 1.0f));
    canvas(frame)->drawString(text, luaL_checknumber(L, 3), luaL_checknumber(L, 4),
                              font, frame->state.paint);
    return 0;
}

/* Accepts either a file path (decoded and cached via load_encoded_image) or
 * an in-memory awesome.skia.image produced by an offscreen surface's
 * :snapshot(), e.g. a procedurally-drawn icon that never touched disk. */
sk_sp<SkImage> check_drawable_image(lua_State *L, int index)
{
    if (lua_skia_image *image = test_image(L, index))
    {
        if (!image->image)
            luaL_error(L, "Skia image has no pixel data");
        return image->image;
    }
    const char *path = luaL_checkstring(L, index);
    sk_sp<SkImage> image = load_encoded_image(path);
    if (!image)
        luaL_error(L, "Skia could not decode image '%s'", path);
    return image;
}

int frame_draw_image(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    sk_sp<SkImage> image = check_drawable_image(L, 2);
    canvas(frame)->drawImage(image, luaL_optnumber(L, 3, 0), luaL_optnumber(L, 4, 0));
    return 0;
}

int skia_image_dimensions(lua_State *L)
{
    if (lua_skia_image *image = test_image(L, 1))
    {
        if (!image->image)
        {
            lua_pushnil(L);
            return 1;
        }
        lua_pushinteger(L, image->image->width());
        lua_pushinteger(L, image->image->height());
        return 2;
    }
    const char *path = luaL_checkstring(L, 1);
    sk_sp<SkImage> image = load_encoded_image(path);
    if (!image)
    {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, image->width());
    lua_pushinteger(L, image->height());
    return 2;
}

int skia_new_image_surface(lua_State *L)
{
    const int width = static_cast<int>(luaL_checkinteger(L, 1));
    const int height = static_cast<int>(luaL_checkinteger(L, 2));
    if (width <= 0 || height <= 0)
        return luaL_error(L, "image surface dimensions must be positive");

    sk_sp<SkSurface> surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
    if (!surface)
        return luaL_error(L, "could not create offscreen Skia surface");
    surface->getCanvas()->clear(SK_ColorTRANSPARENT);

    auto *frame = static_cast<lua_skia_frame *>(lua_newuserdata(L, sizeof(lua_skia_frame)));
    new (frame) lua_skia_frame();
    frame->offscreen = std::move(surface);
    frame->state.paint.setAntiAlias(true);
    frame->states.push_back(frame->state);
    luaL_getmetatable(L, k_frame_type);
    lua_setmetatable(L, -2);
    return 1;
}

int frame_snapshot(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    if (!frame->offscreen)
        return luaL_error(L, "snapshot() only applies to an offscreen image surface "
                             "(created with skia.new_image_surface)");
    auto *image = static_cast<lua_skia_image *>(lua_newuserdata(L, sizeof(lua_skia_image)));
    new (image) lua_skia_image();
    image->image = frame->offscreen->makeImageSnapshot();
    frame->offscreen.reset();
    luaL_getmetatable(L, k_image_type);
    lua_setmetatable(L, -2);
    return 1;
}

int image_gc(lua_State *L)
{
    auto *image = static_cast<lua_skia_image *>(luaL_checkudata(L, 1, k_image_type));
    image->~lua_skia_image();
    return 0;
}

int image_index(lua_State *L)
{
    auto *image = static_cast<lua_skia_image *>(luaL_checkudata(L, 1, k_image_type));
    const char *name = luaL_checkstring(L, 2);
    if (std::strcmp(name, "width") == 0)
    {
        lua_pushinteger(L, image->image ? image->image->width() : 0);
        return 1;
    }
    if (std::strcmp(name, "height") == 0)
    {
        lua_pushinteger(L, image->image ? image->image->height() : 0);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

int frame_index(lua_State *L)
{
    luaL_getmetatable(L, k_frame_type);
    lua_pushvalue(L, 2);
    lua_rawget(L, -2);
    if (!lua_isnil(L, -1))
        return 1;
    lua_pop(L, 2);
    const char *name = luaL_checkstring(L, 2);
    if (std::strcmp(name, "status") == 0)
    {
        lua_pushliteral(L, "SUCCESS");
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

int frame_newindex(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    const char *name = luaL_checkstring(L, 2);
    if (std::strcmp(name, "operator") == 0)
    {
        frame->state.paint.setBlendMode(blend_mode_from_operator(L, 3));
        return 0;
    }
    return luaL_error(L, "Skia canvas has no writable '%s' property", name);
}

int frame_set_operator(lua_State *L)
{
    lua_skia_frame *frame = check_frame(L, 1);
    frame->state.paint.setBlendMode(blend_mode_from_operator(L, 2));
    return 0;
}

int pattern_create_rgba(lua_State *L)
{
    auto *pattern = new_pattern(L);
    pattern->color = rgba(luaL_checknumber(L, 1), luaL_checknumber(L, 2),
                          luaL_checknumber(L, 3), luaL_optnumber(L, 4, 1));
    return 1;
}

int pattern_create_linear(lua_State *L)
{
    auto *pattern = new_pattern(L);
    pattern->kind = pattern_kind::linear;
    pattern->start = {static_cast<SkScalar>(luaL_checknumber(L, 1)),
                      static_cast<SkScalar>(luaL_checknumber(L, 2))};
    pattern->end = {static_cast<SkScalar>(luaL_checknumber(L, 3)),
                    static_cast<SkScalar>(luaL_checknumber(L, 4))};
    return 1;
}

int pattern_create_radial(lua_State *L)
{
    auto *pattern = new_pattern(L);
    pattern->kind = pattern_kind::radial;
    pattern->start = {static_cast<SkScalar>(luaL_checknumber(L, 1)),
                      static_cast<SkScalar>(luaL_checknumber(L, 2))};
    pattern->start_radius = static_cast<SkScalar>(luaL_checknumber(L, 3));
    pattern->end = {static_cast<SkScalar>(luaL_checknumber(L, 4)),
                    static_cast<SkScalar>(luaL_checknumber(L, 5))};
    pattern->end_radius = static_cast<SkScalar>(luaL_checknumber(L, 6));
    return 1;
}

int pattern_add_color_stop_rgba(lua_State *L)
{
    auto *pattern = static_cast<lua_skia_pattern *>(luaL_checkudata(L, 1, k_pattern_type));
    if (pattern->kind == pattern_kind::solid)
        return luaL_error(L, "cannot add a color stop to a solid pattern");

    const SkScalar offset = std::clamp(static_cast<SkScalar>(luaL_checknumber(L, 2)), 0.0f, 1.0f);
    const SkColor4f color = SkColor4f::FromColor(rgba(luaL_checknumber(L, 3), luaL_checknumber(L, 4),
                                                        luaL_checknumber(L, 5), luaL_checknumber(L, 6)));
    const auto position = std::upper_bound(pattern->offsets.begin(), pattern->offsets.end(), offset);
    const size_t index = static_cast<size_t>(position - pattern->offsets.begin());
    pattern->offsets.insert(position, offset);
    pattern->colors.insert(pattern->colors.begin() + index, color);
    refresh_gradient(pattern);
    return 0;
}

int pattern_set_extend(lua_State *L)
{
    auto *pattern = static_cast<lua_skia_pattern *>(luaL_checkudata(L, 1, k_pattern_type));
    const char *extend = luaL_checkstring(L, 2);
    if (std::strcmp(extend, "REPEAT") == 0 || std::strcmp(extend, "repeat") == 0)
        pattern->tile_mode = SkTileMode::kRepeat;
    else if (std::strcmp(extend, "REFLECT") == 0 || std::strcmp(extend, "reflect") == 0)
        pattern->tile_mode = SkTileMode::kMirror;
    else if (std::strcmp(extend, "NONE") == 0 || std::strcmp(extend, "none") == 0)
        pattern->tile_mode = SkTileMode::kDecal;
    else
        pattern->tile_mode = SkTileMode::kClamp;
    refresh_gradient(pattern);
    return 0;
}

int pattern_get_extend(lua_State *L)
{
    const auto *pattern = static_cast<const lua_skia_pattern *>(luaL_checkudata(L, 1, k_pattern_type));
    const char *extend = "PAD";
    if (pattern->tile_mode == SkTileMode::kRepeat)
        extend = "REPEAT";
    else if (pattern->tile_mode == SkTileMode::kMirror)
        extend = "REFLECT";
    else if (pattern->tile_mode == SkTileMode::kDecal)
        extend = "NONE";
    lua_pushstring(L, extend);
    return 1;
}

int pattern_is_type_of(lua_State *L)
{
    /* LGI exposes this as Pattern:is_type_of(value), while callers may also
     * use it as Pattern.is_type_of(value). Accept both spellings. */
    const int value_index = lua_istable(L, 1) ? 2 : 1;
    lua_pushboolean(L, test_pattern(L, value_index) != nullptr);
    return 1;
}

int pattern_get_type(lua_State *L)
{
    const auto *pattern = static_cast<const lua_skia_pattern *>(luaL_checkudata(L, 1, k_pattern_type));
    lua_pushstring(L, pattern_kind_name(pattern->kind));
    return 1;
}

int pattern_get_rgba(lua_State *L)
{
    const auto *pattern = static_cast<lua_skia_pattern *>(luaL_checkudata(L, 1, k_pattern_type));
    lua_pushliteral(L, "SUCCESS");
    lua_pushnumber(L, SkColorGetR(pattern->color) / 255.0);
    lua_pushnumber(L, SkColorGetG(pattern->color) / 255.0);
    lua_pushnumber(L, SkColorGetB(pattern->color) / 255.0);
    lua_pushnumber(L, SkColorGetA(pattern->color) / 255.0);
    return 5;
}

int pattern_get_color_stop_count(lua_State *L)
{
    const auto *pattern = static_cast<const lua_skia_pattern *>(luaL_checkudata(L, 1, k_pattern_type));
    lua_pushliteral(L, "SUCCESS");
    lua_pushinteger(L, static_cast<lua_Integer>(pattern->colors.size()));
    return 2;
}

int pattern_get_color_stop_rgba(lua_State *L)
{
    const auto *pattern = static_cast<const lua_skia_pattern *>(luaL_checkudata(L, 1, k_pattern_type));
    const lua_Integer index = luaL_checkinteger(L, 2);
    if (index < 0 || static_cast<size_t>(index) >= pattern->colors.size())
        return luaL_error(L, "color stop index out of range");
    const SkColor color = pattern->colors[static_cast<size_t>(index)].toSkColor();
    lua_pushliteral(L, "SUCCESS");
    lua_pushnumber(L, pattern->offsets[static_cast<size_t>(index)]);
    lua_pushnumber(L, SkColorGetR(color) / 255.0);
    lua_pushnumber(L, SkColorGetG(color) / 255.0);
    lua_pushnumber(L, SkColorGetB(color) / 255.0);
    lua_pushnumber(L, SkColorGetA(color) / 255.0);
    return 6;
}

int pattern_get_linear_points(lua_State *L)
{
    const auto *pattern = static_cast<const lua_skia_pattern *>(luaL_checkudata(L, 1, k_pattern_type));
    if (pattern->kind != pattern_kind::linear)
    {
        lua_pushliteral(L, "PATTERN_TYPE_MISMATCH");
        return 1;
    }
    lua_pushliteral(L, "SUCCESS");
    lua_pushnumber(L, pattern->start.x());
    lua_pushnumber(L, pattern->start.y());
    lua_pushnumber(L, pattern->end.x());
    lua_pushnumber(L, pattern->end.y());
    return 5;
}

int pattern_get_radial_circles(lua_State *L)
{
    const auto *pattern = static_cast<const lua_skia_pattern *>(luaL_checkudata(L, 1, k_pattern_type));
    if (pattern->kind != pattern_kind::radial)
    {
        lua_pushliteral(L, "PATTERN_TYPE_MISMATCH");
        return 1;
    }
    lua_pushliteral(L, "SUCCESS");
    lua_pushnumber(L, pattern->start.x());
    lua_pushnumber(L, pattern->start.y());
    lua_pushnumber(L, pattern->start_radius);
    lua_pushnumber(L, pattern->end.x());
    lua_pushnumber(L, pattern->end.y());
    lua_pushnumber(L, pattern->end_radius);
    return 7;
}

int pattern_gc(lua_State *L)
{
    auto *pattern = static_cast<lua_skia_pattern *>(luaL_checkudata(L, 1, k_pattern_type));
    pattern->~lua_skia_pattern();
    return 0;
}

int pattern_index(lua_State *L)
{
    luaL_getmetatable(L, k_pattern_type);
    lua_pushvalue(L, 2);
    lua_rawget(L, -2);
    if (!lua_isnil(L, -1))
        return 1;
    lua_pop(L, 2);

    const char *name = luaL_checkstring(L, 2);
    if (std::strcmp(name, "type") == 0)
        return pattern_get_type(L);
    lua_pushnil(L);
    return 1;
}

void set_method(lua_State *L, const char *name, lua_CFunction method)
{
    lua_pushcfunction(L, method);
    lua_setfield(L, -2, name);
}

} // namespace

extern "C" void awesome_skia_lua_extend(lua_State *L)
{
    luaL_getmetatable(L, k_frame_type);
    set_method(L, "__gc", frame_gc);
    set_method(L, "__index", frame_index);
    set_method(L, "__newindex", frame_newindex);
    set_method(L, "present", frame_present);
    set_method(L, "clear", frame_clear);
    set_method(L, "save", frame_save);
    set_method(L, "restore", frame_restore);
    set_method(L, "translate", frame_translate);
    set_method(L, "scale", frame_scale);
    set_method(L, "rotate", frame_rotate);
    set_method(L, "transform_matrix", frame_transform_matrix);
    set_method(L, "clip", frame_clip);
    set_method(L, "clip_rect", frame_clip_rect);
    set_method(L, "clip_extents", frame_clip_extents);
    set_method(L, "rectangle", frame_rectangle);
    set_method(L, "move_to", frame_move_to);
    set_method(L, "line_to", frame_line_to);
    set_method(L, "rel_move_to", frame_rel_move_to);
    set_method(L, "rel_line_to", frame_rel_line_to);
    set_method(L, "curve_to", frame_curve_to);
    set_method(L, "arc", frame_arc);
    set_method(L, "arc_negative", frame_arc_negative);
    set_method(L, "close_path", frame_close_path);
    set_method(L, "new_path", frame_new_path);
    set_method(L, "fill", frame_fill);
    set_method(L, "fill_preserve", frame_fill_preserve);
    set_method(L, "stroke", frame_stroke);
    set_method(L, "stroke_preserve", frame_stroke_preserve);
    set_method(L, "paint", frame_paint);
    set_method(L, "paint_with_alpha", frame_paint_with_alpha);
    set_method(L, "set_source", frame_set_source);
    set_method(L, "set_source_rgb", frame_set_source_rgb);
    set_method(L, "set_source_rgba", frame_set_source_rgba);
    set_method(L, "set_line_width", frame_set_line_width);
    set_method(L, "set_fill_rule", frame_set_fill_rule);
    set_method(L, "set_operator", frame_set_operator);
    set_method(L, "show_text", frame_show_text);
    set_method(L, "draw_image", frame_draw_image);
    set_method(L, "snapshot", frame_snapshot);
    lua_pop(L, 1);

    luaL_newmetatable(L, k_image_type);
    set_method(L, "__gc", image_gc);
    set_method(L, "__index", image_index);
    lua_pop(L, 1);

    luaL_newmetatable(L, k_pattern_type);
    set_method(L, "__gc", pattern_gc);
    set_method(L, "__index", pattern_index);
    set_method(L, "get_type", pattern_get_type);
    set_method(L, "get_rgba", pattern_get_rgba);
    set_method(L, "add_color_stop_rgba", pattern_add_color_stop_rgba);
    set_method(L, "set_extend", pattern_set_extend);
    set_method(L, "get_extend", pattern_get_extend);
    set_method(L, "get_color_stop_count", pattern_get_color_stop_count);
    set_method(L, "get_color_stop_rgba", pattern_get_color_stop_rgba);
    set_method(L, "get_linear_points", pattern_get_linear_points);
    set_method(L, "get_radial_circles", pattern_get_radial_circles);
    lua_pop(L, 1);

    lua_getglobal(L, "skia");
    set_method(L, "begin", frame_begin);
    set_method(L, "is_canvas", skia_is_canvas);
    set_method(L, "is_image", skia_is_image);
    set_method(L, "image_dimensions", skia_image_dimensions);
    set_method(L, "new_image_surface", skia_new_image_surface);
    lua_newtable(L);
    set_method(L, "create_rgba", pattern_create_rgba);
    set_method(L, "create_linear", pattern_create_linear);
    set_method(L, "create_radial", pattern_create_radial);
    set_method(L, "is_type_of", pattern_is_type_of);
    lua_setfield(L, -2, "Pattern");
    lua_pop(L, 1);
}
