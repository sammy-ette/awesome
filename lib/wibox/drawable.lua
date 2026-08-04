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

local function monotonic_seconds()
    return glib.get_monotonic_time() / 1e6
end

-- There is no frame-clock/vsync signal from the backend to align to, so
-- coalesce invalidations to at most one repaint per this interval instead of
-- redrawing on every idle-loop iteration a fast event source can trigger.
-- Present mode is already mailbox where available (skia_backend.cc), so the
-- GPU already discards excess frames correctly; this just stops the Lua
-- record/submit side from doing work faster than a frame could ever be shown,
-- which is what turns even throughput into uneven, stutter-looking pacing.
local target_fps = tonumber(os.getenv("AWESOME_SKIA_TARGET_FPS")) or 60
local MIN_FRAME_INTERVAL = 1 / target_fps

-- Deliberate under-shoot, because deferring a redraw goes through
-- `gears.timer`, which quantizes its timeout to whole milliseconds
-- (`gmath.round(timeout * 1000)`). At 60fps the interval is 16.667ms, so a
-- wait computed to land exactly on it rounds *up* to 17ms. Raster
-- presentation can already consume most of the frame budget, so leave enough
-- slack that it falls through to the idle path instead of adding another
-- rounded timeout. Fast invalidation bursts are still coalesced here.
local FRAME_PACING_SLACK = 0.008

local function defer_redraw(self)
    if self._frame_retry_pending then return end
    self._frame_retry_pending = true
    timer.start_new(1 / 120, function()
        self._frame_retry_pending = false
        self:draw()
        return false
    end)
end

-- Per-drawable performance overlay, enabled with AWESOME_SKIA_DEBUG_OVERLAY=1.
-- The overlay's box is damaged like any other changed region rather than
-- forcing a full repaint, so enabling it costs one small clipped repaint per
-- frame instead of redrawing the whole drawable.
local DEBUG_OVERLAY = os.getenv("AWESOME_SKIA_DEBUG_OVERLAY") == "1"
local OVERLAY_FONT_SIZE, OVERLAY_LINE_HEIGHT, OVERLAY_PAD = 10, 12, 4

-- Everything shown describes the *previous* frame: the overlay has to be
-- both damaged and drawn before the frame carrying it can finish.
local function debug_overlay_lines(self, width, height)
    local stats = self._debug_stats
    return {
        tostring(self.drawable_name),
        string.format("%4.1f fps   %5.2f ms", stats.fps, stats.last_ms),
        string.format("%s   %dx%d", stats.mode, width, height),
    }
end

-- show_text() exposes no measurement call, so the box is sized from an
-- approximate advance width for the monospace face.
local function debug_overlay_box(lines)
    local text_width = 0
    for _, line in ipairs(lines) do
        text_width = math.max(text_width, #line * OVERLAY_FONT_SIZE * 0.62)
    end
    return math.ceil(text_width) + OVERLAY_PAD * 2,
        #lines * OVERLAY_LINE_HEIGHT + OVERLAY_PAD * 2
end

local function draw_debug_overlay(self, cr)
    local stats = self._debug_stats
    local lines = stats.lines
    if not lines then return end

    cr:save()
    cr:set_source("#000000c8")
    cr:rectangle(0, 0, stats.box_width, stats.box_height)
    cr:fill()
    cr:new_path()
    -- Red once a frame misses the 60fps budget, green while it fits.
    cr:set_source(stats.last_ms > 16.67 and "#ff5555ff" or "#55ff55ff")
    for i, line in ipairs(lines) do
        cr:show_text(line, OVERLAY_PAD,
            OVERLAY_PAD + i * OVERLAY_LINE_HEIGHT - 3, "monospace", OVERLAY_FONT_SIZE)
    end
    cr:restore()
end

local function do_redraw(self)
    if not self.drawable.valid then return end
    if self._forced_screen and not self._forced_screen.valid then return end

    -- Stamped at the start of work, not after cr:present(): in this
    -- single-threaded, blocking render loop, the next redraw request can't
    -- even be delivered until the main loop frees up, which happens right
    -- as the previous present() finishes. Pacing off the finish time would
    -- make "elapsed since last frame" read as ~0 every time processing takes
    -- real wall-clock time, tacking a full extra MIN_FRAME_INTERVAL onto
    -- every redraw instead of letting an already-at-or-over-budget frame
    -- run at its natural rate.
    self._last_redraw_start_time = monotonic_seconds()

    local raster = self.drawable.skia_raster
    local raster_retained = raster and self.drawable.skia_raster_retained
    local renderer = raster and true or self.drawable.skia_renderer
    if not raster and not renderer then
        -- A drawable with no area gets no renderer; there is nothing to draw.
        local geom = self.drawable:geometry()
        if geom.width == 0 or geom.height == 0 then return end
        error("Skia/Vulkan could not create a drawable renderer")
    end

    local geom = self.drawable:geometry()
    local width, height = geom.width, geom.height
    local cr, begin_error
    if raster then
        cr = self.drawable:begin_skia_frame(width, height)
    else
        cr, begin_error = skia.begin(renderer)
    end
    if not cr then
        if begin_error == "Skia frame unavailable" or
            begin_error == "Skia surface recovered" then
            self._need_complete_repaint = true
            defer_redraw(self)
            return
        end
        error("Skia/Vulkan could not begin a drawable frame: " ..
            tostring(begin_error))
    end
    -- A non-retained raster adapter creates a fresh CPU surface for each
    -- frame. Its snapshot is intentionally consumptive, so never replay an
    -- unpainted surface when the Lua widget tree has no new damage. Retained
    -- adapters keep the surface (and its pixels) between frames, which lets
    -- the normal dirty-region and picture-cache paths do their job.
    if raster and not raster_retained then
        self._need_complete_repaint = true
    end
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
    if DEBUG_OVERLAY then
        -- Damage the overlay's own box like any other changed region, so it
        -- rides the normal partial-repaint path: the content underneath it is
        -- repainted (giving the text a clean background) without touching the
        -- rest of the drawable. Cover the previous box too, or shrinking text
        -- would leave the old, wider box behind.
        local stats = self._debug_stats
        local lines = debug_overlay_lines(self, width, height)
        local box_width, box_height = debug_overlay_box(lines)
        self._dirty_area:union_rectangle({
            x = 0, y = 0,
            width = math.max(box_width, stats.box_width or 0),
            height = math.max(box_height, stats.box_height or 0),
        })
        stats.lines, stats.box_width, stats.box_height = lines, box_width, box_height
    end

    local dirty_region = self._dirty_area
    local content_invalid = forced_content_repaint or layout_invalidated or
        not dirty_region:is_empty()
    if content_invalid and os.getenv("AWESOME_SKIA_PROFILE_DETAIL") == "1" then
        print("Skia cache rebuild", self.drawable_name,
            "full=" .. tostring(forced_content_repaint),
            "dirty=" .. tostring(not dirty_region:is_empty()))
    end
    self._dirty_area = region.new()

    -- A host does not need a new pixel upload for a move/expose redraw when
    -- the retained raster image is already presented. Keep this
    -- check after hierarchy/context maintenance: those can discover a new
    -- screen/DPI context and set _need_complete_repaint themselves.
    if raster_retained and not content_invalid and
            self.drawable.has_skia_raster_content and
            self.drawable:has_skia_raster_content() then
        self.drawable:refresh()
        return
    end

    if content_invalid then
        cr:mark_content_repaint()
        -- Saved so the overlay below can draw outside whatever clip the
        -- content pass ends up with.
        cr:save()
        -- Paint the cache at its complete logical size. The outer drawin can
        -- reveal only a portion while animating, but later frames must already
        -- have valid pixels for rows that become visible.
        cr:clip_rect(0, 0, width, height)

        -- A full-repaint trigger can invalidate the cache without leaving a
        -- matching dirty rectangle, so only take the partial path when the
        -- region is present and trusted to cover everything that changed.
        -- Retained raster drawables can also use a layout's old/new damage:
        -- this is what lets wibox.layout.overflow shift its existing pixels
        -- during scroll instead of repainting the entire page. The retained
        -- surface remains correct because hierarchy:update() unions both
        -- sides of every moved node into this region.
        local layout_requires_full = layout_invalidated and
            (not raster_retained or dirty_region:is_empty())
        local partial = not forced_content_repaint and not layout_requires_full

        -- A retained raster layout can have a large old/new damage union
        -- during scrolling. wibox.layout.overflow uses that union to prove
        -- that its existing pixels cover the moving content, then shifts the
        -- cache and repaints only the exposed edge. Do not turn that useful
        -- layout damage back into a full repaint based on its area.
        if partial and not (raster_retained and layout_invalidated) then
            -- Many small clipped passes cost more than one full repaint once
            -- the dirty region covers most of the surface or has fragmented
            -- into a lot of rectangles; bail out to the full path instead.
            local rect_count = dirty_region:num_rectangles()
            local dirty_area = 0
            for i = 0, rect_count - 1 do
                local r = dirty_region:get_rectangle(i)
                dirty_area = dirty_area + r.width * r.height
            end
            if rect_count > 16 or dirty_area > 0.6 * width * height then
                partial = false
            end
        end

        if partial then
            cr:new_path()
            local damage = {}
            for i = 0, dirty_region:num_rectangles() - 1 do
                local r = dirty_region:get_rectangle(i)
                cr:rectangle(r.x, r.y, r.width, r.height)
                damage[#damage + 1] = r
            end
            cr:clip()
            -- Let the presentation engine copy only what changed. Purely a
            -- hint: a complete image is rendered either way, so a driver
            -- without VK_KHR_incremental_present is equally correct.
            cr:set_present_damage(damage)
        end

        if DEBUG_OVERLAY then
            self._debug_stats.mode = partial
                and ("partial x" .. dirty_region:num_rectangles())
                or "full"
        end

        cr:clear(0x00000000)
        cr:set_source(self._background_color_spec or "#000000")
        cr:paint()

        if self.background_image and type(self.background_image) == "function" then
            self.background_image(context, cr, width, height, unpack(self.background_image_args))
        end

        if self._widget_hierarchy then
            cr:set_source(self._foreground_color_spec or "#ffffff")
            -- Whether content_surface was just wiped to blank before this
            -- traversal (the `partial` clip above, negated). A widget that
            -- shifts its own already-correct pixels back onto itself instead
            -- of redrawing (see wibox.layout.overflow) needs to know this: it
            -- is a *different* condition from cr:needs_full_redraw(), which
            -- is a much narrower C++/swapchain-recreation flag and stays
            -- false here even though the surface it would be shifting from
            -- was just cleared. `context` is otherwise a stable, reused
            -- table (see get_widget_context), so this is one more field on
            -- it, not a new parameter threaded through every :draw() call.
            context._full_content_repaint = not partial
            self._widget_hierarchy:draw(context, cr)
        end

        -- Drop the content clip (including any partial-repaint region) so the
        -- overlay is never itself clipped away.
        cr:restore()
        if DEBUG_OVERLAY then
            draw_debug_overlay(self, cr)
        end
    end
    local presented, present_error
    if raster then
        local image = cr:snapshot()
        presented, present_error = self.drawable:present_skia_image(image)
    else
        presented, present_error = cr:present()
    end
    if not presented and present_error == "Skia surface recovered" then
        self._need_complete_repaint = true
        defer_redraw(self)
        return
    end
    self.drawable:refresh()

    if DEBUG_OVERLAY then
        local stats = self._debug_stats
        local now = monotonic_seconds()
        -- The figure shown is the previous frame's, since the overlay is
        -- necessarily drawn before the frame it belongs to is finished.
        stats.last_ms = (now - self._last_redraw_start_time) * 1000
        stats.window_frames = stats.window_frames + 1
        local elapsed = now - stats.window_start
        if elapsed >= 1 then
            stats.fps = stats.window_frames / elapsed
            stats.window_frames = 0
            stats.window_start = now
        end
    end
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
    -- geometry().width/height can be inflated to the drawable's historical
    -- high-water mark (see skia_stable_backing in objects/drawable.c); use
    -- the actual currently-visible size for the hit bounds check, or mouse
    -- moves within the now-invisible margin would wrongly count as "inside".
    local visible_width, visible_height = self.drawable:skia_visible_size()

    if x < 0 or y < 0 or x > visible_width or y > visible_height then
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
    ret._last_redraw_start_time = 0
    ret._debug_stats = {
        fps = 0, last_ms = 0, mode = "full",
        window_frames = 0, window_start = monotonic_seconds(),
    }
    ret._do_redraw = function()
        ret._redraw_pending = false
        do_redraw(ret)
    end

    -- Connect our signal when we need a redraw
    ret.draw = function()
        if ret._redraw_pending then return end
        ret._redraw_pending = true

        -- Pace to at most one repaint per MIN_FRAME_INTERVAL. A drag or
        -- animation can emit invalidations faster than any refresh could
        -- show them; without this they still only coalesce down to one
        -- redraw per idle-loop iteration, which is uneven and reads as
        -- stutter even though average throughput is fine.
        --
        -- Measured from the last redraw's start, not its present. A frame
        -- that already takes >= MIN_FRAME_INTERVAL to process is already at
        -- its natural pace by the time it can request the next one; pacing
        -- off the present timestamp would double-count that processing time
        -- as extra wait on top of it.
        local wait = MIN_FRAME_INTERVAL - FRAME_PACING_SLACK
            - (monotonic_seconds() - ret._last_redraw_start_time)
        if wait > 0 then
            timer.start_new(wait, function()
                ret._do_redraw()
                return false
            end)
        else
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

    -- A titlebar is backed by its own Vulkan swapchain.  Resizing it changes
    -- that swapchain's extent and invalidates its retained content, so a
    -- geometry signal must schedule a full repaint as well.  Without these
    -- connections a resize can leave the old image on screen or reveal a
    -- newly-created swapchain without ever painting it.
    d:connect_signal("property::width", ret._do_complete_repaint)
    d:connect_signal("property::height", ret._do_complete_repaint)

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
