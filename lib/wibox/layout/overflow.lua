---------------------------------------------------------------------------
-- A layout that allows its children to take more space than what's available
-- in the surrounding container. If the content does exceed the available
-- size, a scrollbar is added and scrolling behavior enabled.
--
--@DOC_wibox_layout_defaults_overflow_EXAMPLE@
-- @author Lucas Schwiderski
-- @copyright 2021 Lucas Schwiderski
-- @layoutmod wibox.layout.overflow
-- @supermodule wibox.layout.fixed
---------------------------------------------------------------------------

local base = require('wibox.widget.base')
local fixed = require('wibox.layout.fixed')
local separator = require('wibox.widget.separator')
local gtable = require('gears.table')
local gshape = require('gears.shape')
local gobject = require('gears.object')
local timer = require('gears.timer')
local mousegrabber = mousegrabber

local overflow = { mt = {} }

-- The animation timer below runs at a fixed cadence, so a constant per-tick
-- easing factor (rather than one derived from measured wall-clock delta) is
-- both simpler and deterministic to test.
--
-- 8ms, not 1/60s: `gears.timer` quantizes its timeout to whole milliseconds
-- (`gmath.round(timeout * 1000)`), so asking for 1/60 (16.667ms) actually
-- schedules 17ms and caps this animation -- and therefore the redraws it
-- drives -- at 58.8fps, which can never reach the 60 target. 0.008 is an
-- exact integer number of milliseconds, so it survives that rounding
-- unchanged. Ticking faster than the display costs nothing extra: the
-- drawable's own frame pacer coalesces the redraws back down.
local SCROLL_ANIM_FRAME_INTERVAL = 0.008
-- Time constant (seconds) for easing the displayed scroll position toward
-- a wheel-scroll target. Smaller settles faster, larger feels floatier.
local SCROLL_ANIM_TAU = 0.09
local SCROLL_ANIM_ALPHA = 1 - math.exp(-SCROLL_ANIM_FRAME_INTERVAL / SCROLL_ANIM_TAU)
-- Below this distance from the target, snap instead of asymptotically
-- crawling toward it forever.
local SCROLL_ANIM_EPSILON = 0.0008

-- Determine the required space to draw the layout's children and, if necessary,
-- the scrollbar.
function overflow:fit(context, orig_width, orig_height)
    local widgets = self._private.widgets
    local num_widgets = #widgets
    if num_widgets < 1 then
        return 0, 0
    end

    local width, height = orig_width, orig_height
    local scrollbar_width = self._private.scrollbar_width
    local scrollbar_enabled = self._private.scrollbar_enabled
    local used_in_dir, used_max = 0, 0
    local is_y = self._private.dir == "y"
    local avail_in_dir = is_y and orig_height or orig_width

    -- Set the direction covered by scrolling to the maximum value
    -- to allow widgets to take as much space as they want.
    if is_y then
        height = math.huge
    else
        width = math.huge
    end

    -- First, determine widget sizes.
    -- Only when the content doesn't fit and needs scrolling should
    -- we reduce content size to make space for a scrollbar.
    for _, widget in ipairs(widgets) do
        local w, h = base.fit_widget(self, context, widget, width, height)

        if is_y then
            used_max = math.max(used_max, w)
            used_in_dir = used_in_dir + h
        else
            used_max = math.max(used_max, h)
            used_in_dir = used_in_dir + w
        end
    end

    local spacing = self._private.spacing * (num_widgets - 1)
    used_in_dir = used_in_dir + spacing

    local need_scrollbar = scrollbar_enabled and used_in_dir > avail_in_dir

    -- Even if `used_max == orig_(width|height)` already, `base.fit_widget`
    -- will clamp return values, so we can "overextend" here.
    if need_scrollbar then
        used_max = used_max + scrollbar_width
    end

    if is_y then
        return used_max, used_in_dir
    else
        return used_in_dir, used_max
    end
end

-- Recursively connect a one-shot "something in this subtree actually
-- changed" listener, mirroring what hierarchy.lua's own _layout() ancestor
-- walk already guarantees, but surfaced to plain widget code: connects to
-- every descendant, once each (guarded by `connected`), so a change anywhere
-- below a row -- not just on the row's own top-level wrapper -- is caught.
--
-- Also, and just as importantly, re-emits "widget::layout_changed" on the
-- overflow widget itself. A row that is not currently on screen has its
-- hierarchy node detached (hierarchy_update() only reuses nodes for widgets
-- actually present in the current layout_result -- see the "give each widget
-- back the node that was holding it last time" comment below), and once GC
-- collects that detached node, hierarchy.lua's OWN weak_connect_signal() for
-- it goes with it -- there is nothing else keeping that closure alive. That
-- connection is what walks the node's ancestors to mark them needing an
-- update and calls drawable.lua's layout_callback, i.e. it is the thing
-- that actually schedules a relayout. Lose it, and toggling a property on an
-- off-screen row invalidates this cache correctly (this handler doesn't
-- depend on any hierarchy node, so it isn't affected) but nothing ever
-- schedules the redraw that would consult it -- exactly the real bug this
-- was written to fix: search-filtering an overflow list changed rows that
-- had previously scrolled off-screen, and the list only actually reflowed
-- once something else (scrolling) incidentally forced a relayout. Re-emitting
-- here routes through the SAME mechanism scrolling relies on
-- (set_scroll_factor() emits on this widget directly) -- overflow's own
-- hierarchy node is always attached whenever it is drawn at all, so this
-- specific emission's connection can never be the one that got collected.
local function connect_dirty_listener(self, widget, connected)
    if connected[widget] then
        return
    end
    -- `connected` has weak keys but strong values, so storing the handler as
    -- the value is what keeps it alive: weak_connect_signal() does not retain
    -- the closure itself, and an anonymous one with no other reference would
    -- be collected at the next sweep -- silently leaving the sizing cache
    -- with nothing left to invalidate it. The entry still disappears when the
    -- widget itself is collected.
    local handler = function()
        self._private._sizing_dirty = true
        self:emit_signal("widget::layout_changed")
    end
    connected[widget] = handler
    widget:weak_connect_signal("widget::layout_changed", handler)
    if type(widget.get_children) == "function" then
        local ok, children = pcall(widget.get_children, widget)
        if ok then
            for _, child in ipairs(children) do
                connect_dirty_listener(self, child, connected)
            end
        end
    end
end

-- Layout children, scrollbar and spacing widgets.
-- Only those widgets that are currently visible will be placed.
function overflow:layout(context, orig_width, orig_height)
    local result = {}
    local is_y = self._private.dir == "y"
    local widgets = self._private.widgets
    local avail_in_dir = is_y and orig_height or orig_width
    local scrollbar_width = self._private.scrollbar_width
    local scrollbar_enabled = self._private.scrollbar_enabled
    local scrollbar_position = self._private.scrollbar_position
    local base_spacing = self._private.spacing
    local scrollbar_widget = self._private.scrollbar_widget
    local spacing_widget = self._private.spacing_widget

    -- Everything below except the final scroll offset is independent of
    -- scroll_factor, but set_scroll_factor() emits "widget::layout_changed"
    -- on this widget just like a real content change would, and that's what
    -- triggers a fresh :layout() call -- so a naive implementation re-fits
    -- every child from scratch on every scroll-animation tick (~60/s) even
    -- though none of it changed. Cache it, invalidated by anything that
    -- actually can change it: the widget list itself (identity/order/count,
    -- checked directly below -- cheap and can't go stale) or a genuine
    -- content change anywhere in a row's subtree (`_sizing_dirty`, driven by
    -- connect_dirty_listener -- never set by set_scroll_factor, since that
    -- only emits on this widget, not on any child).
    self._private._connected_widgets = self._private._connected_widgets or setmetatable({}, { __mode = "k" })
    local connected = self._private._connected_widgets
    for _, w in ipairs(widgets) do
        connect_dirty_listener(self, w, connected)
    end

    local cache = self._private._sizing_cache
    local cache_valid = cache and not self._private._sizing_dirty
        and cache.context == context
        and cache.orig_width == orig_width
        and cache.orig_height == orig_height
        and cache.spacing == base_spacing
        and cache.scrollbar_width == scrollbar_width
        and cache.scrollbar_enabled == scrollbar_enabled
        and cache.scrollbar_position == scrollbar_position
        and cache.fill_space == self._private.fill_space
        and cache.scrollbar_widget == scrollbar_widget
        and cache.spacing_widget == spacing_widget
        and cache.count == #widgets
    if cache_valid then
        for i = 1, #widgets do
            if cache.widgets[i] ~= widgets[i] then
                cache_valid = false
                break
            end
        end
    end

    local used_in_dir, used_max, need_scrollbar
    local width, height, widget_x, widget_y, bar_w, bar_h, bar_length, spacing, sizes

    if cache_valid then
        used_in_dir, used_max, need_scrollbar = cache.used_in_dir, cache.used_max, cache.need_scrollbar
        width, height = cache.width, cache.height
        widget_x, widget_y = cache.widget_x, cache.widget_y
        bar_w, bar_h, bar_length = cache.bar_w, cache.bar_h, cache.bar_length
        spacing = cache.spacing_size
        sizes = cache.sizes
    else
        width, height = orig_width, orig_height
        widget_x, widget_y = 0, 0
        used_in_dir, used_max = 0, 0

        -- Set the direction covered by scrolling to the maximum value
        -- to allow widgets to take as much space as they want.
        if is_y then
            height = math.huge
        else
            width = math.huge
        end

        -- First, determine widget sizes.
        -- Only when the content doesn't fit and needs scrolling should
        -- we reduce content size to make space for a scrollbar.
        for _, widget in pairs(widgets) do
            local w, h = base.fit_widget(self, context, widget, width, height)

            if is_y then
                used_max = math.max(used_max, w)
                used_in_dir = used_in_dir + h
            else
                used_max = math.max(used_max, h)
                used_in_dir = used_in_dir + w
            end
        end

        used_in_dir = used_in_dir + base_spacing * (#widgets-1)

        need_scrollbar = used_in_dir > avail_in_dir and scrollbar_enabled

        bar_w, bar_h, bar_length = nil, nil, nil
        if need_scrollbar then
            -- The percentage of how much of the content can be visible
            -- within the available space.
            local visible_percent = avail_in_dir / used_in_dir
            -- Make scrollbar length reflect `visible_percent`
            -- TODO: Apply a default minimum length
            bar_length = math.floor(visible_percent * avail_in_dir)

            if is_y then
                bar_w, bar_h = base.fit_widget(self, context, scrollbar_widget, scrollbar_width, bar_length)
                if scrollbar_position == "left" then
                    widget_x = widget_x + bar_w
                end
                width = width - bar_w
            else
                bar_w, bar_h = base.fit_widget(self, context, scrollbar_widget, bar_length, scrollbar_width)
                if scrollbar_position == "top" then
                    widget_y = widget_y + bar_h
                end
                height = height - bar_h
            end
        end

        spacing = base_spacing
        if spacing_widget then
            if is_y then
                local _
                _, spacing = base.fit_widget(self, context, spacing_widget, width, spacing)
            else
                spacing = base.fit_widget(self, context, spacing_widget, spacing, height)
            end
        end

        sizes = {}
        for i, w in ipairs(widgets) do
            local content_w, content_h = base.fit_widget(self, context, w, width, height)
            sizes[i] = { w = content_w, h = content_h }
        end

        self._private._sizing_cache = {
            context = context, orig_width = orig_width, orig_height = orig_height,
            spacing = base_spacing, scrollbar_width = scrollbar_width,
            scrollbar_enabled = scrollbar_enabled, scrollbar_position = scrollbar_position,
            fill_space = self._private.fill_space,
            scrollbar_widget = scrollbar_widget, spacing_widget = spacing_widget,
            count = #widgets, widgets = gtable.clone(widgets, false),
            used_in_dir = used_in_dir, used_max = used_max, need_scrollbar = need_scrollbar,
            width = width, height = height, widget_x = widget_x, widget_y = widget_y,
            bar_w = bar_w, bar_h = bar_h, bar_length = bar_length,
            spacing_size = spacing, sizes = sizes,
        }
        self._private._sizing_dirty = false
    end

    -- Save size for scrolling behavior
    self._private.avail_in_dir = avail_in_dir
    self._private.used_in_dir = used_in_dir

    local scroll_position = self._private.scroll_factor

    if need_scrollbar then
        local bar_x, bar_y = 0, 0
        local bar_pos = (avail_in_dir - bar_length) * scroll_position

        if is_y then
            bar_y = bar_pos
            if scrollbar_position == "right" then
                bar_x = orig_width - bar_w
            end
            self._private.bar_length = bar_h
        else
            bar_x = bar_pos
            if scrollbar_position == "bottom" then
                bar_y = orig_height - bar_h
            end
            self._private.bar_length = bar_w
        end

        table.insert(result, base.place_widget_at(
            scrollbar_widget,
            math.floor(bar_x),
            math.floor(bar_y),
            math.floor(bar_w),
            math.floor(bar_h)
        ))
    end

    local pos = 0
    local interval = used_in_dir - avail_in_dir

    -- How far the content actually moved since the layout that was last
    -- drawn, in the same pixels `scrolled_pos` below is computed in. Valid
    -- only when nothing structural changed in between (`cache_valid`, for
    -- THIS call -- by induction this holds back to whenever the cache was
    -- last rebuilt, since every call in an unbroken cache-valid streak
    -- checks the same thing): the drawable can then shift what it already
    -- rendered instead of re-running every row's draw, see
    -- before_draw_children(). Left at 0 -- meaning "nothing to shift, draw
    -- everything normally" -- on the first layout and after any structural
    -- change, both times for the same reason: there is no known-good prior
    -- frame to shift from.
    local pending_shift = 0
    if cache_valid and self._private._last_scroll_position then
        pending_shift = (self._private._last_scroll_position - scroll_position) * interval
    end
    self._private._last_scroll_position = scroll_position
    self._private._pending_shift = pending_shift

    for i, w in ipairs(widgets) do
        local content_x, content_y
        local content_w, content_h = sizes[i].w, sizes[i].h

        -- When scrolling down, the content itself moves up -> substract
        local scrolled_pos = pos - (scroll_position * interval)

        -- Stop processing completely once we're passed the visible portion
        if scrolled_pos > avail_in_dir then
            break
        end

        if is_y then
            content_x, content_y = widget_x, scrolled_pos
            pos = pos + content_h + spacing

            if self._private.fill_space then
                content_w = width
            end
        else
            content_x, content_y = scrolled_pos, widget_y
            pos = pos + content_w + spacing

            if self._private.fill_space then
                content_h = height
            end
        end

        local is_in_view = is_y
                           and (scrolled_pos + content_h > 0)
                           or (scrolled_pos + content_w > 0)

        if is_in_view then
            -- Add the spacing widget, but not before the first widget
            if i > 1 and spacing_widget then
                table.insert(result, base.place_widget_at(
                    spacing_widget,
                    -- The way how spacing is added for regular widgets
                    -- and the `spacing_widget` is disconnected:
                    -- The offset for regular widgets is added to `pos` one
                    -- iteration _before_ the one where the widget is actually
                    -- placed.
                    -- Because of that, the placement for the spacing widget
                    -- needs to substract that offset to be placed right after
                    -- the previous regular widget.
                    math.floor(is_y and content_x or (content_x - spacing)),
                    math.floor(is_y and (content_y - spacing) or content_y),
                    math.floor(is_y and content_w or spacing),
                    math.floor(is_y and spacing or content_h)
                ))
            end

            -- Marked so hierarchy.lua's draw loop can identify this as one
            -- of the rows a shift-blit is allowed to cover -- as opposed to
            -- the scrollbar/spacing widgets above and below, which do not
            -- move with the content and must always be drawn fresh.
            local placed = base.place_widget_at(
                w,
                math.floor(content_x),
                math.floor(content_y),
                math.floor(content_w),
                math.floor(content_h)
            )
            placed.shift_eligible = true
            table.insert(result, placed)
        end
    end

    return result
end

-- Half a pixel either way, so floor/ceil rounding elsewhere in this file
-- (dirty rects, placement coordinates) can't make an actually-sufficient
-- clip look one rounding error too small and fail the check below for no
-- real reason.
local SHIFT_CLIP_EPSILON = 0.5

function overflow:before_draw_children(context, cr, width, height)
    local shift = self._private._pending_shift
    self._private._pending_shift = nil
    self._private._shift_active = false

    -- Rounded to match how row positions themselves are floored when
    -- placed (see the placement loop above); it also keeps the translate
    -- below an integer pixel offset, avoiding any resampling blur a
    -- fractional one could introduce.
    local shift_amount = shift and math.floor(shift + 0.5) or 0

    -- context._full_content_repaint (set by drawable.lua) means
    -- content_surface was just wiped to blank for a reason that can be
    -- entirely unrelated to this widget (any widget::layout_changed
    -- elsewhere in the same drawable) -- there is nothing valid left to
    -- shift from. cr:needs_full_redraw() alone does not cover this: it is
    -- specific to swapchain recreation and stays false here.
    if shift_amount ~= 0 and cr.snapshot_content and not cr:needs_full_redraw()
            and not context._full_content_repaint then
        -- Confined to the content area: the scrollbar strip (if any) is
        -- excluded, since it does not move with the content and is never
        -- marked shift_eligible -- it is always drawn fresh regardless.
        local sizing = self._private._sizing_cache
        local content_x = sizing and sizing.widget_x or 0
        local content_y = sizing and sizing.widget_y or 0
        local content_w = sizing and sizing.width or width
        local content_h = sizing and sizing.height or height

        local is_y = self._private.dir == "y"
        local dx = is_y and 0 or shift_amount
        local dy = is_y and shift_amount or 0

        cr:save()
        cr:rectangle(content_x, content_y, content_w, content_h)
        cr:clip()

        -- cr:clip() only ever narrows whatever clip is already active, so if
        -- a tighter one is already in force this frame -- e.g. drawable.lua's
        -- own partial-repaint region not yet covering the whole content area
        -- -- the shift below would silently apply to less than intended,
        -- leaving a sliver of stale pixels at the edge. Verified rather than
        -- assumed: fall back to a normal, full redraw of every row instead of
        -- risking that.
        local cx1, cy1, cx2, cy2 = cr:clip_extents()
        local clip_ok = cx1 <= content_x + SHIFT_CLIP_EPSILON
            and cy1 <= content_y + SHIFT_CLIP_EPSILON
            and cx2 >= content_x + content_w - SHIFT_CLIP_EPSILON
            and cy2 >= content_y + content_h - SHIFT_CLIP_EPSILON

        if clip_ok then
            -- What content_surface already holds, before anything this
            -- frame's redraw does gets applied -- i.e. exactly what was on
            -- screen last frame, since content_surface persists across
            -- frames and this hook runs before any row's own draw call.
            local snapshot = cr:snapshot_content()
            cr:clear(0x00000000)
            cr:draw_image(snapshot, dx, dy)
            -- Read by hierarchy.lua's draw loop: rows it marked
            -- shift_eligible that were themselves provably unchanged this
            -- update (hierarchy_update()'s _moved_only) already have correct
            -- pixels here and do not need their own redraw.
            self._private._shift_active = true
        end
        cr:restore()
    end

    -- Clip drawing for children to the space we're allowed to draw in
    cr:rectangle(0, 0, width, height)
    cr:clip()
end


--- The amount of units to advance per scroll event.
--
-- This affects calls to `scroll` and the default mouse wheel handler.
--
-- The default is `10`.
--
-- @property step
-- @tparam number step The step size.
function overflow:set_step(step)
    self._private.step = step
    -- We don't need to emit enything here, since changing step only really
    -- takes effect the next time the user scrolls
end


-- Stop any in-flight wheel-scroll animation, e.g. because a drag grabbed
-- the scrollbar and should take over the position directly.
function overflow:_cancel_scroll_animation()
    if self._private.scroll_anim_timer then
        self._private.scroll_anim_timer:stop()
        self._private.scroll_anim_timer = nil
    end
    self._private.scroll_target = nil
end

-- Advance the eased position one frame toward the target. Returns true once
-- it has arrived and the driving timer should stop.
function overflow:_scroll_step()
    local current = self._private.scroll_factor
    local target = self._private.scroll_target
    if not target then
        return true
    end

    local next_factor = current + (target - current) * SCROLL_ANIM_ALPHA
    if math.abs(target - next_factor) < SCROLL_ANIM_EPSILON then
        next_factor = target
    end

    self:set_scroll_factor(next_factor)

    if next_factor == target then
        self._private.scroll_target = nil
        return true
    end
    return false
end

-- Drive `scroll_factor` toward `_private.scroll_target` with exponential
-- smoothing instead of jumping there in one step. Idempotent: if a timer is
-- already running, a new target is simply picked up on its next tick.
function overflow:_start_scroll_animation()
    if self._private.scroll_anim_timer then
        return
    end

    self._private.scroll_anim_timer = timer.start_new(SCROLL_ANIM_FRAME_INTERVAL, function()
        if self:_scroll_step() then
            self._private.scroll_anim_timer = nil
            return false
        end
        return true
    end)
end

--- Scroll the layout's content by `amount * step`.
--
-- A positive amount scrolls down/right, a negative amount scrolls up/left.
--
-- The amount of units scrolled is affected by `step`.
--
-- Unlike setting `scroll_factor` directly, this eases the displayed position
-- toward the new target instead of jumping to it, so repeated wheel ticks
-- (or a fast burst of them) read as one smooth motion rather than a series
-- of steps.
--
-- @method overflow:scroll
-- @tparam number amount The amount to scroll by.
-- @emits property::overflow::scroll_factor
-- @emitstparam property::overflow::scroll_factor number scroll_factor The new
--   scroll factor.
-- @emits widget::layout_changed
-- @emits widget::redraw_needed
function overflow:scroll(amount)
    if amount == 0 then
        return
    end
    -- Mirror set_scroll_factor's own "nothing to scroll" guard so a wheel
    -- tick over non-overflowing content can't start a timer that never
    -- converges (set_scroll_factor would keep rejecting every update).
    if self._private.used_in_dir <= self._private.avail_in_dir then
        return
    end

    local interval = self._private.used_in_dir
    local delta = self._private.step / interval

    local base_factor = self._private.scroll_target or self._private.scroll_factor
    self._private.scroll_target = math.min(1, math.max(base_factor + (delta * amount), 0))

    -- Move on this event rather than waiting for the timer's first tick. A
    -- freshly started timer does not fire until a full interval has elapsed,
    -- so without this the content stays still for that interval after the
    -- wheel is turned -- dead time at the start of every gesture, which is
    -- exactly where a scroll is judged.
    if not self:_scroll_step() then
        self:_start_scroll_animation()
    end
end


--- The scroll factor.
--
-- The scroll factor represents how far the layout's content is currently
-- scrolled. It is represented as a fraction from `0` to `1`, where `0` is the
-- start of the content and `1` is the end.
--
-- @property scroll_factor
-- @tparam number scroll_factor The scroll factor.
-- @propemits true false

function overflow:set_scroll_factor(factor)
    local current = self._private.scroll_factor
    local interval = self._private.used_in_dir - self._private.avail_in_dir
    if current == factor
        -- the content takes less space than what is available, i.e. everything
        -- is already visible
        or interval <= 0
        -- the scroll factor is out of range
        or (current <= 0 and factor < 0)
        or (current >= 1 and factor > 1) then
        return
    end

    self._private.scroll_factor = math.min(1, math.max(factor, 0))

    self:emit_signal("widget::layout_changed")
    self:emit_signal("property::scroll_factor", factor)
end

function overflow:get_scroll_factor()
    return self._private.scroll_factor
end


--- The scrollbar width.
--
-- For horizontal scrollbars, this is the scrollbar height
--
-- The default is `5`.
--
--@DOC_wibox_layout_overflow_scrollbar_width_EXAMPLE@
--
-- @property scrollbar_width
-- @tparam number scrollbar_width The scrollbar width.
-- @propemits true false

function overflow:set_scrollbar_width(width)
    if self._private.scrollbar_width == width then
        return
    end

    self._private.scrollbar_width = width

    self:emit_signal("widget::layout_changed")
    self:emit_signal("property::scrollbar_width", width)
end


--- The scrollbar position.
--
-- For horizontal scrollbars, this can be `"top"` or `"bottom"`,
-- for vertical scrollbars this can be `"left"` or `"right"`.
-- The default is `"right"`/`"bottom"`.
--
--@DOC_wibox_layout_overflow_scrollbar_position_EXAMPLE@
--
-- @property scrollbar_position
-- @tparam string scrollbar_position The scrollbar position.
-- @propemits true false

function overflow:set_scrollbar_position(position)
    if self._private.scrollbar_position == position then
        return
    end

    self._private.scrollbar_position = position

    self:emit_signal("widget::layout_changed")
    self:emit_signal("property::scrollbar_position", position)
end

function overflow:get_scrollbar_position()
    return self._private.scrollbar_position
end


--- The scrollbar visibility.
--
-- If this is set to `false`, no scrollbar will be rendered, even if the layout's
-- content overflows. Mouse wheel scrolling will work regardless.
--
-- The default is `true`.
--
-- @property scrollbar_enabled
-- @tparam boolean scrollbar_enabled The scrollbar visibility.
-- @propemits true false

function overflow:set_scrollbar_enabled(enabled)
    if self._private.scrollbar_enabled == enabled then
        return
    end

    self._private.scrollbar_enabled = enabled

    self:emit_signal("widget::layout_changed")
    self:emit_signal("property::scrollbar_enabled", enabled)
end

function overflow:get_scrollbar_enabled()
    return self._private.scrollbar_enabled
end

-- Wraps a callback function for `mousegrabber` that is capable of
-- updating the scroll factor.
local function build_grabber(container, initial_x, initial_y, geo)
    local is_y = container._private.dir == "y"
    local bar_interval = container._private.avail_in_dir - container._private.bar_length
    local start_pos = container._private.scroll_factor * bar_interval
    local start = is_y and initial_y or initial_x

    -- Calculate a matrix transforming from screen coordinates into widget
    -- coordinates.
    -- This is required for mouse movement to work when the widget has been
    -- transformed by something like `wibox.container.rotate`.
    local matrix_from_device = geo.hierarchy:get_matrix_from_device()
    local wgeo = geo.drawable.drawable:geometry()
    local matrix = matrix_from_device:translate(-wgeo.x, -wgeo.y)

    return function(mouse)
        if not mouse.buttons[1] then
            return false
        end

        local x, y = matrix:transform_point(mouse.x, mouse.y)
        local pos = is_y and y or x
        container:set_scroll_factor((start_pos + (pos - start)) / bar_interval)

        return true
    end
end

-- Applies a mouse button signal using `build_grabber` to a scrollbar widget.
local function apply_scrollbar_mouse_signal(container, w)
    w:connect_signal('button::press', function(_, x, y, button_id, _, geo)
        if button_id ~= 1 then
            return
        end
        container:_cancel_scroll_animation()
        mousegrabber.run(build_grabber(container, x, y, geo), "fleur")
    end)
end


--- The scrollbar widget.
-- This widget is rendered as the scrollbar element.
--
-- The default is `wibox.widget.separator{ shape = gears.shape.rectangle }`.
--
--@DOC_wibox_layout_overflow_scrollbar_widget_EXAMPLE@
--
-- @property scrollbar_widget
-- @tparam widget scrollbar_widget The scrollbar widget.
-- @propemits true false

function overflow:set_scrollbar_widget(widget)
    local w = base.make_widget_from_value(widget)

    apply_scrollbar_mouse_signal(self, w)

    self._private.scrollbar_widget = w

    self:emit_signal("widget::layout_changed")
    self:emit_signal("property::scrollbar_widget", widget)
end

function overflow:get_scrollbar_widget()
    return self._private.scrollbar_widget
end


function overflow:reset()
    self:_cancel_scroll_animation()
    self._private.widgets = {}
    self._private.scroll_factor = 0

    local scrollbar_widget = separator({ shape = gshape.rectangle })
    apply_scrollbar_mouse_signal(self, scrollbar_widget)
    self._private.scrollbar_widget = scrollbar_widget

    self:emit_signal("widget::layout_changed")
    self:emit_signal("widget::reset")
    self:emit_signal("widget::reseted")
end

local function new(dir, ...)
    local ret = fixed[dir](...)

    gtable.crush(ret, overflow, true)
    ret.widget_name = gobject.modulename(2)
    -- Tell the widget system to prevent clicks outside the layout's extends
    -- to register with child widgets, even if they actually extend that far.
    -- This prevents triggering button presses on hidden/clipped widgets.
    ret.clip_child_extends = true

    -- Manually set the scroll factor here. We don't know the bounding size yet.
    ret._private.scroll_factor = 0

    -- Apply defaults. Bypass setters to avoid signals.
    ret._private.step = 10
    ret._private.fill_space = true
    ret._private.scrollbar_width = 5
    ret._private.scrollbar_enabled = true
    ret._private.scrollbar_position = dir == "vertical" and "right" or "bottom"

    local scrollbar_widget = separator({ shape = gshape.rectangle })
    apply_scrollbar_mouse_signal(ret, scrollbar_widget)
    ret._private.scrollbar_widget = scrollbar_widget

    ret:connect_signal('button::press', function(self, _, _, button)
        if button == 4 then
            self:scroll(-1)
        elseif button == 5 then
            self:scroll(1)
        end
    end)

    return ret
end


--- Returns a new horizontal overflow layout.
-- Child widgets are placed similar to `wibox.layout.fixed`, except that
-- they may take as much width as they want. If the total width of all child
-- widgets exceeds the width available whithin the layout's outer container
-- a scrollbar will be added and scrolling behavior enabled.
-- @tparam widget ... Widgets that should be added to the layout.
-- @constructorfct wibox.layout.overflow.horizontal
function overflow.horizontal(...)
    return new("horizontal", ...)
end


--- Returns a new vertical overflow layout.
-- Child widgets are placed similar to `wibox.layout.fixed`, except that
-- they may take as much height as they want. If the total height of all child
-- widgets exceeds the height available whithin the layout's outer container
-- a scrollbar will be added and scrolling behavior enabled.
-- @tparam widget ... Widgets that should be added to the layout.
-- @constructorfct wibox.layout.overflow.vertical
function overflow.vertical(...)
    return new("vertical", ...)
end

return setmetatable(overflow, overflow.mt)

-- vim: filetype=lua:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:textwidth=80
