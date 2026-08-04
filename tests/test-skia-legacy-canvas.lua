local skia = require("skia")

local surface = skia.ImageSurface.create(skia.Format.ARGB32, 64, 32)
local cr = skia.Context(surface)

cr:select_font_face("sans", "italic", "bold")
cr:set_font_size(16)

local face = cr:get_font_face()
assert(face.family == "sans")
assert(face.slant == "italic")
assert(face.weight == "bold")

local extents = cr:text_extents("compat")
assert(extents.width > 0)
assert(extents.x_advance > 0)

cr:move_to(4, 20)
local x, y = cr:get_current_point()
assert(x == 4 and y == 20)
cr:show_text("compat")

cr:save()
cr:set_font_size(8)
cr:restore()
assert(cr.font_size == 16)

cr.font_face = face
local restored_face = cr.font_face
assert(restored_face.family == "sans")
assert(restored_face.slant == "italic")
assert(restored_face.weight == "bold")

local image_canvas = skia.new_image_surface(8, 8)
local image = image_canvas:snapshot()
local image_context = skia.Context(image)
local left, top, right, bottom = image_context:clip_extents()
assert(right - left >= 8 and bottom - top >= 8)
image:finish()

print("Test finished successfully.")
awesome.quit()
