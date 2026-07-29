---------------------------------------------------------------------------
-- Replicate the content of the widget over and over.
--
-- This contained is intended to be used for wallpapers. It currently doesn't
-- support mouse input in the replicated tiles.
--
--@DOC_wibox_container_defaults_tile_EXAMPLE@
-- @author Emmanuel Lepage-Vallee
-- @copyright 2021 Emmanuel Lepage-Vallee
-- @containermod wibox.container.tile
-- @supermodule wibox.container.place
local place = require("wibox.container.place")
local base = require("wibox.widget.base")
local gtable = require("gears.table")

local module = {mt = {}}

function module:draw(context, cr, width, height)
    -- Repetition is represented in layout(), so rendering needs no temporary
    -- raster surface or pattern cache.
    return
end

-- A Cairo pattern repeats a CPU-rasterized child.  The GPU equivalent is to
-- place that child once for each visible tile, leaving the canvas and image
-- decoding on the Skia path.  This also preserves ordinary widget semantics:
-- every copy is laid out and clipped by the normal hierarchy.
function module:layout(context, width, height)
    if not self._private.tiled then
        return place.layout(self, context, width, height)
    end
    if not self._private.widget then return end

    local x, y, child_width, child_height = self:_layout(context, width, height)
    if child_width <= 0 or child_height <= 0 then return end

    local horizontal_step = child_width + self.horizontal_spacing
    local vertical_step = child_height + self.vertical_spacing
    if horizontal_step <= 0 or vertical_step <= 0 then return end

    local first_x = x + math.floor((0 - x) / horizontal_step) * horizontal_step
    local first_y = y + math.floor((0 - y) / vertical_step) * vertical_step
    local result = {}

    for tile_y = first_y, height, vertical_step do
        local y_visible = tile_y + child_height > 0 and tile_y < height
        local y_complete = tile_y >= 0 and tile_y + child_height <= height
        if y_visible and (not self.vertical_crop or y_complete) then
            for tile_x = first_x, width, horizontal_step do
                local x_visible = tile_x + child_width > 0 and tile_x < width
                local x_complete = tile_x >= 0 and tile_x + child_width <= width
                if x_visible and (not self.horizontal_crop or x_complete) then
                    table.insert(result, base.place_widget_at(
                        self._private.widget, tile_x, tile_y, child_width, child_height
                    ))
                end
            end
        end
    end

    return result
end

--- The horizontal spacing between the tiled.
--
--@DOC_wibox_container_tile_horizontal_spacing_EXAMPLE@
--
-- @property horizontal_spacing
-- @tparam[opt=0] number horizontal_spacing
-- @propemits true false
-- @propertyunit pixel
-- @negativeallowed false
-- @see vertical_spacing

--- The vertical spacing between the tiled.
--
--@DOC_wibox_container_tile_vertical_spacing_EXAMPLE@
--
-- @property vertical_spacing
-- @tparam[opt=0] number vertical_spacing
-- @propertyunit pixel
-- @negativeallowed false
-- @propemits true false
-- @see horizontal_spacing

--- Avoid painting incomplete horizontal tiles.
--
--@DOC_wibox_container_tile_horizontal_crop_EXAMPLE@
--
-- @property horizontal_crop
-- @tparam[opt=false] boolean horizontal_crop
-- @see vertical_crop

--- Avoid painting incomplete vertical tiles.
--
--@DOC_wibox_container_tile_vertical_crop_EXAMPLE@
--
-- @property vertical_crop
-- @tparam[opt=false] boolean vertical_crop
-- @see horizontal_crop

--- Enable or disable the tiling.
--
-- When set to `false`, this container behaves exactly like
-- `wibox.container.place`.
--
--@DOC_wibox_container_tile_tiled_EXAMPLE@
--
-- @property tiled
-- @tparam[opt=true] boolean tiled

local defaults = {
    horizontal_spacing = 0,
    vertical_spacing   = 0,
    tiled              = true,
    horizontal_crop    = false,
    vertical_crop      = false,
}

for prop in pairs(defaults) do

    module["set_"..prop] = function(self, value)
        self._private[prop] = value
        self:emit_signal("widget::layout_changed", value)
    end

    module["get_"..prop] = function(self)
        if self._private[prop] == nil then
            return defaults[prop]
        end

        return self._private[prop]
    end
end

local function new(_, args)
    args = args or {}
    local ret = place(args.widget, args.halign, args.valign)
    gtable.crush(ret, module, true)
    ret._private.tiled = true

    local function redraw()
        ret:emit_signal("widget::redraw_needed")
    end

    -- Resize the pattern as needed.
    local function reset()
        if skia then return end
        if ret._private.surface then
            ret._private.surface:finish()
        end

        ret._private.cr = nil
        ret._private.surface = nil
        ret._private.pattern = nil
    end

    local w = nil

    ret:connect_signal("property::widget", function()
        reset()

        if w then
            w:disconnect_signal("widget::redraw_needed", redraw)
            w:disconnect_signal("widget::layout_changed", reset)
        end

        w = ret._private.widget

        if w then
            w:connect_signal("widget::redraw_needed", redraw)
            w:connect_signal("widget::layout_changed", reset)
        end
    end)

    return ret
end

--- Create a new tile container.
-- @tparam table args
-- @tparam wibox.widget args.widget args.widget The widget to tile.
-- @tparam string args.halign Either `left`, `right` or `center`.
-- @tparam string args.valign Either `top`, `bottom` or `center`.
-- @tparam number args.horizontal_spacing The horizontal spacing between the tiled.
-- @tparam number args.vertical_spacing The vertical spacing between the tiled.
-- @tparam boolean args.horizontal_crop Avoid painting incomplete horizontal tiles.
-- @tparam boolean args.vertical_crop Avoid painting incomplete vertical tiles.
-- @tparam boolean args.tiled Enable or disable the tiling.
-- @tparam wibox.widget args.widget The widget to be placed.
-- @tparam boolean args.fill_vertical Fill the vertical space.
-- @tparam boolean args.fill_horizontal Fill the horizontal space.
-- @tparam boolean args.content_fill_vertical Stretch the contained widget so it takes all the vertical space.
-- @tparam boolean args.content_fill_horizontal Stretch the contained widget so it takes all the horizontal space.
-- @constructorfct wibox.container.tile
function module.mt:__call(...)
    return new(...)
end

return setmetatable(module, module.mt)

-- vim: filetype=lua:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:textwidth=80
