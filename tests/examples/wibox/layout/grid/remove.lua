--DOC_GEN_OUTPUT --DOC_GEN_IMAGE
local _, generic_before_after = ... --DOC_HIDE_ALL
local wibox = require("wibox") --DOC_HIDE

print("l:remove_widgets_at(1,1)") --DOC_HIDE

local w = generic_before_after(wibox.layout.grid, {
    column_count = 3,
    row_count    = 2,
    homogeneous  = true,
}, 6, "remove_widgets_at", {1, 1}) --DOC_HIDE

return w, w:fit({dpi=96}, 9999, 9999) --DOC_HIDE
