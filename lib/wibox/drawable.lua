---------------------------------------------------------------------------
--- Handling of drawables. A drawable is something that can be drawn to.
--
-- @author Uli Schlachter
-- @copyright 2012 Uli Schlachter
-- @classmod wibox.drawable
---------------------------------------------------------------------------

local drawable = {}
local capi = {
    awesome = awesome,
    root = root,
    screen = screen
}
local beautiful = require("beautiful")
local base = require("wibox.widget.base")
local skia = require("skia")
local color = require("gears.color")
local object = require("gears.object")
local surface = require("gears.surface")
local region = require("gears.region")
local timer = require("gears.timer")
local glib = require("lgi").GLib
local grect =  require("gears.geometry").rectangle
local matrix = require("gears.matrix")
local whierarchy = require("wibox.hierarchy")
local unpack = unpack or table.unpack -- luacheck: globals unpack (compatibility with Lua 5.1)

local visible_drawables = {}

local systray_widget
local get_widget_context

local function defer_redraw(self)
    if self._frame_retry_pending then return end
    self._frame_retry_pending = true
    timer.start_new(1 / 120, function()
        self._frame_retry_pending = false
        self:draw()
        return false
    end)
end

local function do_redraw(self)
    if not self.drawable.valid then return end
    if self._forced_screen and not self._forced_screen.valid then return end

    local renderer = self.drawable.skia_renderer
    if not renderer then
        -- A drawable with no area gets no renderer; there is nothing to draw.
        local geom = self.drawable:geometry()
        if geom.width == 0 or geom.height == 0 then return end
        error("Skia/Vulkan could not create a drawable renderer")
    end

    local cr, begin_error = skia.begin(renderer)
    if not cr then
        if begin_error == "Skia frame unavailable" then
            defer_redraw(self)
            return
        end
        error("Skia/Vulkan could not begin a drawable frame: " ..
            tostring(begin_error))
    end
    local geom = self.drawable:geometry()
    local x, y, width, height = geom.x, geom.y, geom.width, geom.height
    local context = get_widget_context(self)

    -- The renderer owns a complete retained GPU image for this drawin. Never
    -- patch it by a dirty rectangle: a widget/layout update invalidates the
    -- whole cached image, while pure outer geometry/opacity animation can
    -- present it again without traversing Lua widgets at all.
    if cr:needs_full_redraw() then
        self._need_complete_repaint = true
    end
    local forced_content_repaint = self._need_complete_repaint
    -- A layout signal is semantic invalidation even when the affected widgets
    -- happen to keep identical extents (for example, a same-size reorder).
    -- Keep it separate from the region so retained rendering cannot turn that
    -- valid update into a stale cache replay.
    local layout_invalidated = self._layout_invalidated
    self._layout_invalidated = false

    if self._need_relayout or self._need_complete_repaint then
        self._need_relayout = false
        if self._widget_hierarchy and self._widget then
            local had_systray = systray_widget and self._widget_hierarchy:get_count(systray_widget) > 0
            self._widget_hierarchy:update(context, self._widget, width, height, self._dirty_area)
            local has_systray = systray_widget and self._widget_hierarchy:get_count(systray_widget) > 0
            if had_systray and not has_systray then
                systray_widget:_kickout(context)
            end
        else
            forced_content_repaint = true
            self._need_complete_repaint = true
            if self._widget then
                self._widget_hierarchy_callback_arg = {}
                self._widget_hierarchy = whierarchy.new(context, self._widget, width, height,
                    self._redraw_callback, self._layout_callback, self._widget_hierarchy_callback_arg)
            else
                self._widget_hierarchy = nil
            end
        end
    end

    self._need_complete_repaint = false
    local content_invalid = forced_content_repaint or layout_invalidated or
        not self._dirty_area:is_empty()
    if content_invalid and os.getenv("AWESOME_SKIA_PROFILE_DETAIL") == "1" then
        print("Skia cache rebuild", self.drawable_name,
            "full=" .. tostring(forced_content_repaint),
            "dirty=" .. tostring(not self._dirty_area:is_empty()))
    end
    self._dirty_area = region.new()

    if content_invalid then
        cr:mark_content_repaint()
        -- Paint the cache at its complete logical size. The outer drawin can
        -- reveal only a portion while animating, but later frames must already
        -- have valid pixels for rows that become visible.
        cr:clip_rect(0, 0, width, height)
        cr:clear(0x00000000)
        cr:set_source(self._background_color_spec or "#000000")
        cr:paint()

        if self.background_image and type(self.background_image) == "function" then
            self.background_image(context, cr, width, height, unpack(self.background_image_args))
        end

        if self._widget_hierarchy then
            cr:set_source(self._foreground_color_spec or "#ffffff")
            self._widget_hierarchy:draw(context, cr)
        end
    end

    cr:present()
    self.drawable:refresh()
end

-- Get the widget context. This should always return the same table (if
-- possible), so that our draw and fit caches can work efficiently.
get_widget_context = function(self)
    local geom = self.drawable:geometry()

    local s = self._forced_screen
    if not s then
        local sgeos = {}

        for scr in capi.screen do
            sgeos[scr] = scr.geometry
        end

        s = grect.get_by_coord(sgeos, geom.x, geom.y) or capi.screen.primary
    end

    local context = self._widget_context
    local dpi = s and s.dpi or 96
    if (not context) or context.screen ~= s or context.dpi ~= dpi then
        context = {
            screen = s,
            dpi = dpi,
            drawable = self,
        }
        for k, v in pairs(self._widget_context_skeleton) do
            context[k] = v
        end
        self._widget_context = context

        -- Give widgets a chance to react to the new context
        self._need_complete_repaint = true
    end
    return context
end

local function find_widgets(self, result, hierarchy, x, y)
    local m = hierarchy:get_matrix_from_device()

    -- Is (x,y) inside of this hierarchy or any child (aka the draw extents)
    local x1, y1 = m:transform_point(x, y)
    local x2, y2, w2, h2 = hierarchy:get_draw_extents()
    if x1 < x2 or x1 >= x2 + w2 then
        return
    end
    if y1 < y2 or y1 >= y2 + h2 then
        return
    end

    -- Is (x,y) inside of this widget?
    local width, height = hierarchy:get_size()
    if x1 >= 0 and y1 >= 0 and x1 <= width and y1 <= height then
        -- Get the extents of this widget in the device space
        local x3, y3, w3, h3 = matrix.transform_rectangle(hierarchy:get_matrix_to_device(),
            0, 0, width, height)
        table.insert(result, {
            x = x3, y = y3, width = w3, height = h3,
            widget_width = width,
            widget_height = height,
            drawable = self,
            widget = hierarchy:get_widget(),
            hierarchy = hierarchy
        })
    end
    for _, child in ipairs(hierarchy:get_children()) do
        find_widgets(self, result, child, x, y)
    end
end

-- Find a widget by a point.
-- The drawable must have drawn itself at least once for this to work.
-- @param x X coordinate of the point
-- @param y Y coordinate of the point
-- @treturn table A table containing a description of all the widgets that
-- contain the given point. Each entry is a table containing this drawable as
-- its `.drawable` entry, the widget under `.widget` and the instance of
-- `wibox.hierarchy` describing the size and position of the widget under
-- `.hierarchy`. For convenience, `.x`, `.y`, `.width` and `.height` contain an
-- approximation of the widget's extents on the surface. `widget_width` and
-- `widget_height` contain the exact size of the widget in its own, local
-- coordinate system (which may e.g. be rotated and scaled).
function drawable:find_widgets(x, y)
    local result = {}
    if self._widget_hierarchy then
        find_widgets(self, result, self._widget_hierarchy, x, y)
    end
    return result
end

-- Private API. Not documented on purpose.
function drawable._set_systray_widget(widget)
    whierarchy.count_widget(widget)
    systray_widget = widget
end

--- Set the widget that the drawable displays
function drawable:set_widget(widget)
    self._widget = base.make_widget_from_value(widget)

    -- Make sure the widget gets drawn
    self._need_relayout = true
    self._layout_invalidated = true
    self.draw()
end

function drawable:get_widget()
    return rawget(self, "_widget")
end

--- Set the background of the drawable
-- @param c The background to use. This must either be a cairo pattern object,
--   nil or a string that gears.color() understands.
-- @see gears.color
function drawable:set_bg(c)
    c = c or "#000000"
    self._background_color_spec = c
    local t = type(c)

    if t == "string" or t == "table" then
        c = color(c)
    end

    -- If the background is completely opaque, we don't need to redraw when
    -- the drawable is moved
    -- XXX: This isn't needed when awesome.composite_manager_running is true,
    -- but a compositing manager could stop/start and we'd have to properly
    -- handle this. So for now we choose the lazy approach.
    local redraw_on_move = not color.create_opaque_pattern(c)
    if self._redraw_on_move ~= redraw_on_move then
        self._redraw_on_move = redraw_on_move
        if redraw_on_move then
            self.drawable:connect_signal("property::x", self._do_complete_repaint)
            self.drawable:connect_signal("property::y", self._do_complete_repaint)
        else
            self.drawable:disconnect_signal("property::x", self._do_complete_repaint)
            self.drawable:disconnect_signal("property::y", self._do_complete_repaint)
        end
    end

    self.background_color = c
    self._do_complete_repaint()
end

--- Set the background image of the drawable
-- If `image` is a function, it will be called with `(context, cr, width, height)`
-- as arguments. Any other arguments passed to this method will be appended.
-- @param image A background image or a function
function drawable:set_bgimage(image, ...)
    if image ~= nil and type(image) ~= "function" then
        -- gears.surface resolves a path or an existing surface; wrap the
        -- result in a draw callback so it flows through the same handling
        -- as a procedurally-drawn background.
        local resolved = surface.load(image)
        image = resolved and function(_, cr) cr:draw_image(resolved, 0, 0) end or nil
    end

    self.background_image = image
    self.background_image_args = {...}

    self._do_complete_repaint()
end

--- Set the foreground of the drawable
-- @param c The foreground to use. This must either be a cairo pattern object,
--   nil or a string that gears.color() understands.
-- @see gears.color
function drawable:set_fg(c)
    c = c or "#FFFFFF"
    self._foreground_color_spec = c
    if type(c) == "string" or type(c) == "table" then
        c = color(c)
    end
    self.foreground_color = c
    self._do_complete_repaint()
end

function drawable:_force_screen(s)
    self._forced_screen = s
end

function drawable:_inform_visible(visible)
    self._visible = visible
    if visible then
        visible_drawables[self] = true
        -- The wallpaper or widgets might have changed
        self:_do_complete_repaint()
    else
        visible_drawables[self] = nil
    end
end

local function emit_difference(name, list, skip)
    local function in_table(table, val)
        for _, v in pairs(table) do
            if v.widget == val.widget then
                return true
            end
        end
        return false
    end

    for _, v in pairs(list) do
        if not in_table(skip, v) then
            v.widget:emit_signal(name,v)
        end
    end
end

local function handle_leave(self)
    emit_difference("mouse::leave", self._widgets_under_mouse, {})
    self._widgets_under_mouse = {}
end

local function handle_motion(self, x, y)
    local dgeo = self.drawable:geometry()

    if x < 0 or y < 0 or x > dgeo.width or y > dgeo.height then
        return handle_leave(self)
    end

    -- Build a plain list of all widgets on that point
    local widgets_list = self:find_widgets(x, y)

    -- First, "leave" all widgets that were left
    emit_difference("mouse::leave", self._widgets_under_mouse, widgets_list)
    -- Then enter some widgets
    emit_difference("mouse::enter", widgets_list, self._widgets_under_mouse)

    self._widgets_under_mouse = widgets_list
end

local function setup_signals(self)
    local d = self.drawable

    local function clone_signal(name)
        -- When "name" is emitted on wibox.drawin, also emit it on wibox
        d:connect_signal(name, function(_, ...)
            self:emit_signal(name, ...)
        end)
    end
    clone_signal("button::press")
    clone_signal("button::release")
    clone_signal("mouse::enter")
    clone_signal("mouse::leave")
    clone_signal("mouse::move")
    clone_signal("property::surface")
    clone_signal("property::width")
    clone_signal("property::height")
    clone_signal("property::x")
    clone_signal("property::y")
end

function drawable.new(d, widget_context_skeleton, drawable_name)
    local ret = object()
    ret.drawable = d
    ret._widget_context_skeleton = widget_context_skeleton
    ret._need_complete_repaint = true
    ret._need_relayout = true
    ret._layout_invalidated = true
    ret._dirty_area = region.new()
    setup_signals(ret)

    for k, v in pairs(drawable) do
        if type(v) == "function" then
            ret[k] = v
        end
    end

    -- Only redraw a drawable once, even when we get told to do so multiple times.
    --
    -- Do not use gears.timer.delayed_call here.  That queue is drained in its
    -- entirety by Awesome's `refresh` signal, so opening a panel can run every
    -- visible drawin's layout, Pango and Vulkan submission in one main-loop
    -- iteration.  A single slow drawin is unavoidable on this thread; a whole
    -- batch is not.  An idle source gives pending X/input sources priority
    -- between drawins, while retaining redraw coalescing for each drawable.
    ret._redraw_pending = false
    ret._do_redraw = function()
        ret._redraw_pending = false
        do_redraw(ret)
    end

    -- Connect our signal when we need a redraw
    ret.draw = function()
        if not ret._redraw_pending then
            ret._redraw_pending = true
            glib.idle_add(glib.PRIORITY_DEFAULT_IDLE, function()
                ret._do_redraw()
                return false
            end)
        end
    end
    ret._do_complete_repaint = function()
        ret._need_complete_repaint = true
        ret:draw()
    end

    -- Do a full redraw if the surface changes (the new surface has no content yet)
    d:connect_signal("property::surface", ret._do_complete_repaint)

    -- Do a normal redraw when the drawable moves. This will likely do nothing
    -- in most cases, but it makes us do a complete repaint when we are moved to
    -- a different screen.
    d:connect_signal("property::x", ret.draw)
    d:connect_signal("property::y", ret.draw)

    -- Currently we aren't redrawing on move (signals not connected).
    -- :set_bg() will later recompute this.
    ret._redraw_on_move = false

    -- Set the default background
    ret:set_bg(beautiful.bg_normal)
    ret:set_fg(beautiful.fg_normal)

    -- Initialize internals
    ret._widgets_under_mouse = {}

    local function button_signal(name)
        d:connect_signal(name, function(_, x, y, button, modifiers)
            local widgets = ret:find_widgets(x, y)
            for _, v in pairs(widgets) do
                -- Calculate x/y inside of the widget
                local lx, ly = v.hierarchy:get_matrix_from_device():transform_point(x, y)
                v.widget:emit_signal(name, lx, ly, button, modifiers,v)
            end
        end)
    end
    button_signal("button::press")
    button_signal("button::release")

    d:connect_signal("mouse::move", function(_, x, y) handle_motion(ret, x, y) end)
    d:connect_signal("mouse::leave", function() handle_leave(ret) end)

    -- Set up our callbacks for repaints
    ret._redraw_callback = function(hierar, arg)
        -- Avoid crashes when a drawable was partly finalized and dirty_area is broken.
        if not ret._visible then
            return
        end
        if ret._widget_hierarchy_callback_arg ~= arg then
            return
        end
        local m = hierar:get_matrix_to_device()
        local x, y, width, height = matrix.transform_rectangle(m, hierar:get_draw_extents())
        local x1, y1 = math.floor(x), math.floor(y)
        local x2, y2 = math.ceil(x + width), math.ceil(y + height)
        ret._dirty_area:union_rectangle({
            x = x1, y = y1, width = x2 - x1, height = y2 - y1
        })
        ret:draw()
    end
    ret._layout_callback = function(_, arg)
        if ret._widget_hierarchy_callback_arg ~= arg then
            return
        end
        ret._need_relayout = true
        ret._layout_invalidated = true
        -- When not visible, we will be redrawn when we become visible. In the
        -- mean-time, the layout does not matter much.
        if ret._visible then
            ret:draw()
        end
    end

    -- Add __tostring method to metatable.
    ret.drawable_name = drawable_name or object.modulename(3)
    local mt = {}
    local orig_string = tostring(ret)
    mt.__tostring = function()
        return string.format("%s (%s)", ret.drawable_name, orig_string)
    end
    ret = setmetatable(ret, mt)

    -- Make sure the drawable is drawn at least once
    ret._do_complete_repaint()

    return setmetatable(ret, {
        __index = function(self, k)
            if rawget(self, "get_"..k) then
                return rawget(self, "get_"..k)(self)
            else
                return rawget(ret, k)
            end
        end,
        __newindex = function(self, k,v)
            if rawget(self, "set_"..k) then
                rawget(self, "set_"..k)(self, v)
            else
                rawset(self, k, v)
            end
        end
    })
end

-- Redraw all drawables when the wallpaper changes
capi.awesome.connect_signal("wallpaper_changed", function()
    for d in pairs(visible_drawables) do
        d:_do_complete_repaint()
    end
end)

-- Give drawables a chance to react to screen changes
local function draw_all()
    for d in pairs(visible_drawables) do
        d:draw()
    end
end
screen.connect_signal("property::geometry", draw_all)
screen.connect_signal("added", draw_all)
screen.connect_signal("removed", draw_all)

return setmetatable(drawable, { __call = function(_, ...) return drawable.new(...) end })

-- vim: filetype=lua:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:textwidth=80
