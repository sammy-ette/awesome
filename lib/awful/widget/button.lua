---------------------------------------------------------------------------
-- A simple button widget based on a background image.
--
-- @DOC_wibox_awidget_defaults_button_EXAMPLE@
--
-- @author Julien Danjou &lt;julien@danjou.info&gt;
-- @copyright 2008-2009 Julien Danjou
-- @widgetmod awful.widget.button
-- @supermodule wibox.widget.imagebox
---------------------------------------------------------------------------

local setmetatable = setmetatable
local abutton = require("awful.button")
local imagebox = require("wibox.widget.imagebox")
local widget = require("wibox.widget.base")
local surface = require("gears.surface")
local skia = rawget(_G, "skia")
local cairo = require("lgi").cairo
local gtable = require("gears.table")

local button = { mt = {} }

--- Create a button widget. When clicked, the image is deplaced to make it like
-- a real button.
--
-- @constructorfct awful.widget.button
-- @tparam table args Widget arguments.
-- @tparam string args.image "image" is the image to display (mandatory).
-- @tparam table args.buttons The buttons.
-- @return A textbox widget configured as a button.
function button.new(args)
    args = args or {}
    if not args.image then
        return widget.empty_widget()
    end

    local w = imagebox()
    local orig_set_image = w.set_image
    local img_release
    local img_press

    function w:set_image(image)
        img_release = surface.load(image)
        if skia then
            -- Same "pressed" look: the image nudged 2px down and right.
            local iw, ih = img_release:get_width(), img_release:get_height()
            local canvas = skia.new_image_surface(math.max(iw, 1), math.max(ih, 1))
            canvas:draw_image(img_release, 2, 2)
            img_press = canvas:snapshot()
        else
            img_press = img_release:create_similar(cairo.Content.COLOR_ALPHA,
                img_release.width, img_release.height)
            local cr = cairo.Context(img_press)
            cr:set_source_surface(img_release, 2, 2)
            cr:paint()
        end
        orig_set_image(self, img_release)
    end
    w:set_image(args.image)

    local btns = gtable.clone(args.buttons or {}, false)

    table.insert(btns,
        abutton({}, 1, function () orig_set_image(w, img_press) end,
                       function () orig_set_image(w, img_release) end)
    )

    w.buttons = btns

    w:connect_signal("mouse::leave", function(self) orig_set_image(self, img_release) end)

    return w
end

function button.mt:__call(...)
    return button.new(...)
end

return setmetatable(button, button.mt)

-- vim: filetype=lua:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:textwidth=80
