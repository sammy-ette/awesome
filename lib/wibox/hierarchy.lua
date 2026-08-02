---------------------------------------------------------------------------
-- Management of widget hierarchies. Each widget hierarchy object has a widget
-- for which it saves e.g. size and transformation in its parent. Also, each
-- widget has a number of children.
--
-- @author Uli Schlachter
-- @copyright 2015 Uli Schlachter
-- @classmod wibox.hierarchy
---------------------------------------------------------------------------

local matrix = require("gears.matrix")
local skia = require("skia")
local protected_call = require("gears.protected_call")
local region_module = require("gears.region")
local base = require("wibox.widget.base")
local no_parent = base.no_parent_I_know_what_I_am_doing

local hierarchy = {}

-- gears.region tracks dirty rectangles as plain geometry tables.
local function rectangle_int(x, y, width, height)
    return { x = x, y = y, width = width, height = height }
end

local widgets_to_count = setmetatable({}, { __mode = "k" })

--- Add a widget to the list of widgets for which hierarchies should count their
-- occurrences. Note that for correct operations, the widget must not yet be
-- visible in any hierarchy.
-- @param widget The widget that should be counted.
-- @staticfct wibox.hierarchy.count_widget
-- @noreturn
function hierarchy.count_widget(widget)
    widgets_to_count[widget] = true
end

local function hierarchy_new(redraw_callback, layout_callback, callback_arg)
    local result = {
        _matrix = matrix.identity,
        _matrix_to_device = matrix.identity,
        _matrix_from_device = nil,
        _need_update = true,
        _widget = nil,
        _context = nil,
        _redraw_callback = redraw_callback,
        _layout_callback = layout_callback,
        _callback_arg = callback_arg,
        _size = {
            width = nil,
            height = nil
        },
        _draw_extents = {
            x = 0,
            y = 0,
            width = 0,
            height = 0
        },
        _parent = nil,
        _children = {},
        _widget_counts = {},
        -- Whether this exact update took the "subtree only moved" branch
        -- below (content provably unchanged, only position did) -- read by
        -- the parent's draw loop to decide whether a shift-blit already
        -- placed this child's pixels, making a normal redraw redundant.
        -- Read by the parent, so it must survive past this node's own
        -- hierarchy_update return; see :draw() in this file.
        _moved_only = false,
        -- Whether the *widget* that produced this node opted this specific
        -- child into shift-blit skipping (set from the layout-result entry's
        -- `shift_eligible` field, e.g. by wibox.layout.overflow for content
        -- rows but not its scrollbar). Without this, any node that happens
        -- to be moved-only would qualify -- which is wrong for something
        -- like a scrollbar that moves on its own schedule, not with the
        -- content around it.
        _shift_eligible = false,
        -- Whether this exact update found widget/context/size AND position
        -- all identical to last time -- strictly stronger than _moved_only,
        -- which allows position to differ. This is the condition under
        -- which a recorded picture of this node's content is valid to
        -- replay: nothing that could affect what it draws has changed.
        -- General and not opt-in (unlike _shift_eligible), since replaying
        -- a picture instead of re-running :draw() is safe for any widget
        -- once its content is provably unchanged, not just ones a specific
        -- parent container has coordinated with.
        _nothing_changed = false,
        -- Recorded picture cache for the above. `_picture` is nil until
        -- first recorded; `_picture_valid` is false whenever it needs
        -- re-recording (see _redraw()/_layout() and the branches below).
        _picture = nil,
        _picture_valid = false,
    }

    function result._redraw()
        -- This node's own content changed (a redraw_needed signal carries
        -- no size/position information, so hierarchy_update's matrix/size
        -- comparison alone cannot see this -- a node can be "_nothing_changed"
        -- by that comparison while its actual drawn content did change, e.g.
        -- wibox.container.background:set_bg()). Invalidates this node's own
        -- cached picture and every ancestor's, since an ancestor's cached
        -- picture has this node's old rendering baked into it.
        local h = result
        while h do
            h._picture_valid = false
            h._picture = nil
            h = h._parent
        end
        redraw_callback(result, callback_arg)
    end
    function result._layout()
        local h = result
        while h do
            h._need_update = true
            h = h._parent
        end
        layout_callback(result, callback_arg)
    end
    function result._emit_recursive(widget, name, ...)
        local cur = result
        assert(widget == cur._widget)
        while cur do
            if cur._widget then
                cur._widget:emit_signal(name, ...)
            end
            cur = cur._parent
        end
    end

    for k, f in pairs(hierarchy) do
        if type(f) == "function" then
            result[k] = f
        end
    end
    return result
end

-- Push new transforms through a subtree that is otherwise still valid.
-- Everything a node caches apart from its matrices -- its children, their
-- placement relative to it, its draw extents and its widget counts -- is
-- expressed in local coordinates, so moving the subtree leaves all of it
-- correct and only the matrices need refreshing.
local function hierarchy_retransform(self, matrix_to_parent, matrix_to_device)
    self._matrix = matrix_to_parent
    self._matrix_to_device = matrix_to_device
    self._matrix_from_device = nil
    for _, child in ipairs(self._children) do
        hierarchy_retransform(child, child._matrix, child._matrix * matrix_to_device)
    end
end

local hierarchy_update
function hierarchy_update(self, context, widget, width, height, region, matrix_to_parent, matrix_to_device)
    if (not self._need_update) and self._widget == widget and
            self._context == context and
            self._size.width == width and self._size.height == height and
            matrix.equals(self._matrix, matrix_to_parent) and
            matrix.equals(self._matrix_to_device, matrix_to_device) then
        -- Nothing changed
        self._nothing_changed = true
        return
    end

    -- The subtree only moved: same widget, same context, same size, and no
    -- layout signal from this widget or any descendant (`_layout` sets
    -- `_need_update` on the whole ancestor chain).  A widget's `:layout()`
    -- never sees a matrix, so its result cannot depend on where the subtree
    -- sits -- the children and their relative placement are still valid and
    -- re-running the layout would reproduce them exactly.  Just push the new
    -- transforms down and redraw the area the subtree left and the one it
    -- moved to.  This keeps a scroll (or any moving subtree) proportional to
    -- the nodes on screen instead of relaying out each one every frame.
    if (not self._need_update) and self._widget == widget and
            self._context == context and
            self._size.width == width and self._size.height == height then
        local ext_x, ext_y, ext_w, ext_h = self:get_draw_extents()

        local ox, oy, ow, oh = matrix.transform_rectangle(self._matrix_to_device,
            ext_x, ext_y, ext_w, ext_h)
        ox, oy = math.floor(ox), math.floor(oy)
        region:union_rectangle(rectangle_int(ox, oy, math.ceil(ow), math.ceil(oh)))

        hierarchy_retransform(self, matrix_to_parent, matrix_to_device)

        local nx, ny, nw, nh = matrix.transform_rectangle(matrix_to_device,
            ext_x, ext_y, ext_w, ext_h)
        nx, ny = math.floor(nx), math.floor(ny)
        region:union_rectangle(rectangle_int(nx, ny, math.ceil(nw), math.ceil(nh)))
        self._moved_only = true
        self._nothing_changed = false
        return
    end

    self._moved_only = false
    self._nothing_changed = false
    -- A structural change (widget/context/size actually differ, or this is
    -- the first update) makes any cached picture stale regardless of what
    -- _redraw()/_layout() already invalidated -- e.g. the widget itself was
    -- swapped out for a different one at this position.
    self._picture_valid = false
    self._picture = nil
    self._need_update = false

    local old_x, old_y, old_width, old_height
    local old_widget = self._widget
    if self._size.width and self._size.height then
        local x, y, w, h = matrix.transform_rectangle(self._matrix_to_device, 0, 0, self._size.width, self._size.height)
        old_x, old_y = math.floor(x), math.floor(y)
        old_width, old_height = math.ceil(x + w) - old_x, math.ceil(y + h) - old_y
    else
        old_x, old_y, old_width, old_height = 0, 0, 0, 0
    end

    -- Disconnect old signals
    if old_widget and old_widget ~= widget then
        self._widget:disconnect_signal("widget::redraw_needed", self._redraw)
        self._widget:disconnect_signal("widget::layout_changed", self._layout)
        self._widget:disconnect_signal("widget::emit_recursive", self._emit_recursive)
    end

    -- Save the arguments we need to save
    self._widget = widget
    self._context = context
    self._size.width = width
    self._size.height = height
    self._matrix = matrix_to_parent
    self._matrix_to_device = matrix_to_device
    self._matrix_from_device = nil

    -- Connect signals
    if old_widget ~= widget then
        widget:weak_connect_signal("widget::redraw_needed", self._redraw)
        widget:weak_connect_signal("widget::layout_changed", self._layout)
        widget:weak_connect_signal("widget::emit_recursive", self._emit_recursive)
    end

    -- Update children
    local old_children = self._children
    local layout_result = base.layout_widget(no_parent, context, widget, width, height)

    -- Give each widget back the node that was holding it last time, rather
    -- than pairing nodes to widgets by position.  A scrolling list shifts
    -- every child by a slot, so positional pairing would hand each node a
    -- widget it did not have before, defeating every per-node check below it
    -- and forcing a full update of the whole subtree.  The same widget may
    -- legitimately appear more than once (a layout's spacing widget does), so
    -- each one keeps a queue of its nodes.
    -- A node being built for the first time has nothing to match against, so
    -- skip the bookkeeping entirely rather than allocating for it.
    local nodes_by_widget, reused
    if old_children[1] then
        nodes_by_widget, reused = {}, {}
        for _, child in ipairs(old_children) do
            local w = child._widget
            local nodes = nodes_by_widget[w]
            if not nodes then
                nodes = {}
                nodes_by_widget[w] = nodes
            end
            nodes[#nodes + 1] = child
        end
    end

    local recycle_index = 1
    self._children = {}
    for _, w in ipairs(layout_result or {}) do
        local r
        if nodes_by_widget then
            local nodes = nodes_by_widget[w._widget]
            r = nodes and table.remove(nodes, 1)
            if not r then
                -- No node held this widget before.  Recycle a node whose widget
                -- is gone (it updates in full either way) before allocating.
                repeat
                    r = old_children[recycle_index]
                    recycle_index = recycle_index + 1
                until not r or not reused[r]
                if r then
                    local stale = nodes_by_widget[r._widget]
                    if stale then
                        for i, node in ipairs(stale) do
                            if node == r then
                                table.remove(stale, i)
                                break
                            end
                        end
                    end
                end
            end
            if r then
                reused[r] = true
            end
        end
        if not r then
            r = hierarchy_new(self._redraw_callback, self._layout_callback, self._callback_arg)
            r._parent = self
        end
        hierarchy_update(r, context, w._widget, w._width, w._height, region, w._matrix, w._matrix * matrix_to_device)
        -- Read after the call, since hierarchy_update() may reset it (a
        -- structural change on this exact node clears both flags at the top
        -- of the branch above); an opted-in widget re-asserts it on every
        -- :layout() call regardless, same as any other placement field.
        r._shift_eligible = w.shift_eligible or false
        table.insert(self._children, r)
    end

    -- Calculate the draw extents
    local x1, y1, x2, y2 = 0, 0, width, height
    if not widget.clip_child_extends then
        for _, h in ipairs(self._children) do
            local px, py, pwidth, pheight = matrix.transform_rectangle(h._matrix, h:get_draw_extents())
            x1 = math.min(x1, px)
            y1 = math.min(y1, py)
            x2 = math.max(x2, px + pwidth)
            y2 = math.max(y2, py + pheight)
        end
    end
    -- Mutated rather than replaced: a fresh table per node per update is
    -- garbage generated for every widget in every tree that relayouts.
    local extents = self._draw_extents
    extents.x, extents.y = x1, y1
    extents.width, extents.height = x2 - x1, y2 - y1

    -- Update widget counts, reusing the table for the same reason. It is
    -- empty for all but the handful of widgets registered via count_widget(),
    -- so the clear below almost always has nothing to do.
    local counts = self._widget_counts
    if next(counts) ~= nil then
        for w in pairs(counts) do
            counts[w] = nil
        end
    end
    if widgets_to_count[widget] and width > 0 and height > 0 then
        counts[widget] = 1
    end
    for _, h in ipairs(self._children) do
        for w, count in pairs(h._widget_counts) do
            counts[w] = (counts[w] or 0) + count
        end
    end

    -- Check which part needs to be redrawn

    -- Are there any children which were removed? Their area needs a redraw.
    -- Anything still held above was reused in place and is accounted for by
    -- its own update.
    for _, child in ipairs(old_children) do
        if not reused[child] then
            local x, y, w, h = matrix.transform_rectangle(child._matrix_to_device, child:get_draw_extents())
            x = math.floor(x)
            y = math.floor(y)
            w = math.ceil(w)
            h = math.ceil(h)
            region:union_rectangle(rectangle_int(x, y, w, h))
            child._parent = nil
        end
    end

    -- Did we change and need to be redrawn?
    local x, y, w, h = matrix.transform_rectangle(self._matrix_to_device, 0, 0, self._size.width, self._size.height)
    local new_x, new_y = math.floor(x), math.floor(y)
    local new_width, new_height = math.ceil(x + w) - new_x, math.ceil(y + h) - new_y
    if new_x ~= old_x or new_y ~= old_y or new_width ~= old_width or new_height ~= old_height or
            widget ~= old_widget then
        region:union_rectangle(rectangle_int(old_x, old_y, old_width, old_height))
        region:union_rectangle(rectangle_int(new_x, new_y, new_width, new_height))
    end
end

--- Create a new widget hierarchy that has no parent.
-- @param context The context in which we are laid out.
-- @param widget The widget that is at the base of the hierarchy.
-- @param width The available width for this hierarchy.
-- @param height The available height for this hierarchy.
-- @param redraw_callback Callback that is called with the corresponding widget
--   hierarchy on widget::redraw_needed on some widget.
-- @param layout_callback Callback that is called with the corresponding widget
--   hierarchy on widget::layout_changed on some widget.
-- @param callback_arg A second argument that is given to the above callbacks.
-- @return A new widget hierarchy
-- @constructorfct wibox.hierarchy.new
function hierarchy.new(context, widget, width, height, redraw_callback, layout_callback, callback_arg)
    local result = hierarchy_new(redraw_callback, layout_callback, callback_arg)
    result:update(context, widget, width, height)
    return result
end

--- Update a widget hierarchy with some new state.
-- @param context The context in which we are laid out.
-- @param widget The widget that is at the base of the hierarchy.
-- @param width The available width for this hierarchy.
-- @param height The available height for this hierarchy.
-- @param[opt] region A region to use for accumulating changed parts
-- @return A Skia region describing the changed parts (either the `region`
--   argument or a new, internally created region).
-- @method update
function hierarchy:update(context, widget, width, height, region)
    region = region or region_module.new()
    hierarchy_update(self, context, widget, width, height, region, self._matrix, self._matrix_to_device)
    return region
end

--- Get the widget that this hierarchy manages.
-- @method get_widget
-- @treturn wibox.widget The widget held by this node.
function hierarchy:get_widget()
    return self._widget
end

--- Get a matrix that transforms to the parent's coordinate space from this
-- hierarchy's coordinate system.
-- @return A matrix describing the transformation.
-- @method get_matrix_to_parent
function hierarchy:get_matrix_to_parent()
    return self._matrix
end

--- Get a matrix that transforms to the base of this hierarchy's coordinate
-- system (aka the coordinate system of the device that this
-- hierarchy is applied upon) from this hierarchy's coordinate system.
-- @return A matrix describing the transformation.
-- @method get_matrix_to_device
function hierarchy:get_matrix_to_device()
    return self._matrix_to_device
end

--- Get a matrix that transforms from the parent's coordinate space into this
-- hierarchy's coordinate system.
-- @return A matrix describing the transformation.
-- @method get_matrix_from_parent
function hierarchy:get_matrix_from_parent()
    local m = self:get_matrix_to_parent()
    return m:invert()
end

--- Get a matrix that transforms from the base of this hierarchy's coordinate
-- system (aka the coordinate system of the device that this
-- hierarchy is applied upon) into this hierarchy's coordinate system.
-- @return A matrix describing the transformation.
-- @method get_matrix_from_device
function hierarchy:get_matrix_from_device()
    if not self._matrix_from_device then
        self._matrix_from_device = self:get_matrix_to_device():invert()
    end
    return self._matrix_from_device
end

--- Get the extents that this hierarchy possibly draws to (in the current coordinate space).
-- This includes the size of this element plus the size of all children
-- (after applying the corresponding transformation).
-- @return x, y, width, height
-- @method get_draw_extents
function hierarchy:get_draw_extents()
    local ext = self._draw_extents
    return ext.x, ext.y, ext.width, ext.height
end

--- Get the size that this hierarchy logically covers (in the current coordinate space).
-- @return width, height
-- @method get_size
function hierarchy:get_size()
    local ext = self._size
    return ext.width, ext.height
end

--- Get a list of all children.
-- @return List of all children hierarchies.
-- @method get_children
function hierarchy:get_children()
    return self._children
end

--- Count how often this widget is visible inside this hierarchy. This function
-- only works with widgets registered via `count_widget`.
-- @param widget The widget that should be counted
-- @return The number of times that this widget is contained in this hierarchy.
-- @method get_count
function hierarchy:get_count(widget)
    return self._widget_counts[widget] or 0
end

-- Return whether a conservative rectangle can affect a clip whose bounds are
-- already known.  Hierarchy nodes retain draw extents, including descendants
-- that legitimately extend beyond their parent, so this lets us reject an
-- entire off-clip subtree before entering its Lua draw traversal.
--
-- The clip bounds are passed in rather than read here because `clip_extents()`
-- is a C round-trip returning four values, and the clip is necessarily
-- identical for every child of a node -- reading it once per child made the
-- per-frame cost of a long list scale with the number of children for no
-- added information.
local function rect_intersects_clip(clip_x1, clip_y1, clip_x2, clip_y2, x, y, width, height)
    if width <= 0 or height <= 0 then return false end
    return x < clip_x2 and clip_x1 < x + width and
        y < clip_y2 and clip_y1 < y + height
end

local function intersects_clip(cr, x, y, width, height)
    -- Checked before reading the clip so a degenerate node costs no C call.
    if width <= 0 or height <= 0 then return false end
    local clip_x1, clip_y1, clip_x2, clip_y2 = cr:clip_extents()
    return rect_intersects_clip(clip_x1, clip_y1, clip_x2, clip_y2, x, y, width, height)
end

local function child_intersects_clip(clip_x1, clip_y1, clip_x2, clip_y2, child)
    local x, y, width, height = matrix.transform_rectangle(
        child:get_matrix_to_parent(), child:get_draw_extents())
    return rect_intersects_clip(clip_x1, clip_y1, clip_x2, clip_y2, x, y, width, height)
end

-- Widget draw callbacks are invoked through these rather than a closure built
-- inside :draw().  A closure there captures the node, widget, context and
-- canvas, so it has to be allocated again for every node on every frame --
-- garbage produced on the hottest path in the widget system, for every tree,
-- whether or not the widget in question even has any of these callbacks.
-- Passing the state explicitly keeps these as plain shared functions.
local function call_hook(func, widget, context, cr, width, height)
    if not func then return end
    protected_call(func, widget, context, cr, width, height)
end

local function call_child_hook(func, widget, context, index, child_widget, cr, width, height)
    if not func then return end
    protected_call(func, widget, context, index, child_widget, cr, width, height)
end

-- Draw this node's own widget plus its children into `cr`, which is
-- whichever canvas the caller wants the commands to land on -- the live
-- frame for a normal draw, or a picture recorder when (re)building the
-- cache below. Both work identically here, since everything in this
-- function operates purely in this node's own local coordinates.
local function draw_content(self, context, cr, widget, self_width, self_height,
        clip_x1, clip_y1, clip_x2, clip_y2)
    -- Draw the widget. Most containers (fixed, place, constraint, stack,
    -- background, ...) have no :draw() of their own -- their visual
    -- effect, if any, is entirely in before/after_draw_children -- so
    -- the save/clip/restore around it would protect nothing and is
    -- skipped entirely rather than paid on every node of every tree.
    if widget.draw then
        cr:save()
        cr:rectangle(0, 0, self_width, self_height)
        cr:clip()
        call_hook(widget.draw, widget, context, cr, self_width, self_height)
        cr:restore()
        -- Clear any path that the widget might have left
        cr:new_path()
    end

    -- Draw its children (We already clipped to the draw extents above)
    -- before_draw_children is allowed to narrow the clip and leaves it
    -- narrowed for the children (wibox.container.background's shape does
    -- exactly that), so the bounds have to be re-read after it ran --
    -- but only then, since nothing else here touches the clip.
    if widget.before_draw_children then
        call_hook(widget.before_draw_children, widget, context, cr, self_width, self_height)
        clip_x1, clip_y1, clip_x2, clip_y2 = cr:clip_extents()
    end

    -- A widget's before_draw_children may have just shifted its own
    -- already-correct pixels into place instead of clearing them (see
    -- wibox.layout.overflow's scroll blit). When it has, a child it
    -- opted into this (._shift_eligible) whose own update this frame
    -- was itself provably a pure move (._moved_only, from
    -- hierarchy_update() above) does not need to be redrawn -- the
    -- shift already put its pixels where they belong, and re-running
    -- its draw would only be repainting something already correct.
    --
    -- Two different "this is a full repaint" signals are checked, not
    -- one: cr:needs_full_redraw() is a narrow C++/swapchain-recreation
    -- flag (true only on an actual resize), while
    -- context._full_content_repaint (set by drawable.lua) covers every
    -- other reason content_surface gets wiped to blank before this
    -- traversal -- e.g. any widget::layout_changed elsewhere in the same
    -- drawable, unrelated to this widget's own subtree. A widget's own
    -- scroll settling onto a frame where *that* happened to be true was
    -- the actual cause of a real bug: the blit shifted blank pixels and
    -- skipped redrawing rows on top of them, leaving them blank until
    -- something else forced a real repaint. Trusting only the narrower
    -- flag missed exactly this case.
    local skip_shifted = widget._private._shift_active
        and not cr:needs_full_redraw()
        and not context._full_content_repaint

    -- before/after_draw_child are allowed to alter the canvas for their
    -- child, so retain their exact call contract.  Ordinary layouts have
    -- no such hooks and can reject invisible child subtrees here instead
    -- of paying a save/transform/clip traversal for every one.
    local before_draw_child = widget.before_draw_child
    local after_draw_child = widget.after_draw_child
    local can_cull_children = not before_draw_child and not after_draw_child
    for i, wi in ipairs(self._children) do
        if not can_cull_children
                or child_intersects_clip(clip_x1, clip_y1, clip_x2, clip_y2, wi) then
            if before_draw_child then
                call_child_hook(before_draw_child, widget, context, i, wi._widget,
                    cr, self_width, self_height)
            end
            if not (skip_shifted and wi._shift_eligible and wi._moved_only) then
                wi:draw(context, cr)
            end
            if after_draw_child then
                call_child_hook(after_draw_child, widget, context, i, wi._widget,
                    cr, self_width, self_height)
            end
        end
    end
    call_hook(widget.after_draw_children, widget, context, cr, self_width, self_height)
    -- Clear any path that the widget might have left
    cr:new_path()
end

-- Record this node's content into a picture, independent of where it is
-- actually positioned on screen: recording happens in local,
-- content-relative coordinates (everything draw_content touches is relative
-- to this node's own origin), which is exactly what makes the result valid
-- to replay later regardless of what this node's transform has become.
-- Recording always covers the full draw extents rather than whatever the
-- live canvas's current clip happens to be, so the cached picture stays
-- complete and reusable even if more of it becomes visible on some later
-- frame that would otherwise have had a narrower clip.
-- `template_cr` seeds the recording's initial paint from that live frame's
-- current state (see skia.new_picture_recorder's own comment for why this
-- specific thing matters: a widget that draws using an inherited ambient
-- color rather than setting its own would otherwise render wrong the moment
-- its subtree was cached).
-- Returns a picture, or nil if the extents are degenerate.
local function record_picture(self, context, widget, self_width, self_height,
        ext_x, ext_y, ext_width, ext_height, template_cr)
    local w, h = math.ceil(ext_width), math.ceil(ext_height)
    if w <= 0 or h <= 0 then
        return nil
    end
    local recorder = skia.new_picture_recorder(w, h, template_cr)
    recorder:translate(-ext_x, -ext_y)
    draw_content(self, context, recorder, widget, self_width, self_height,
        ext_x, ext_y, ext_x + ext_width, ext_y + ext_height)
    return recorder:finish_picture()
end

--- Draw a hierarchy to some cairo context.
-- This function draws the widgets in this widget hierarchy to the given cairo
-- context. The context's clip is used to skip parts that aren't visible.
-- @param context The context in which widgets are drawn.
-- @param cr The cairo context that is used for drawing.
-- @method draw
-- @noreturn
function hierarchy:draw(context, cr)
    local widget = self:get_widget()
    if not widget._private.visible then
        return
    end

    local ext_x, ext_y, ext_width, ext_height = self:get_draw_extents()
    -- One read of the post-clip bounds serves both purposes below: rejecting
    -- this node when nothing can be drawn, and culling its children.
    local clip_x1, clip_y1, clip_x2, clip_y2

    -- push_node() collapses what used to be five separate Lua<->C round
    -- trips (save, transform, a clip_extents() to test whether this node's
    -- extents can affect anything, rectangle, clip, another clip_extents()
    -- for the post-clip bounds below) into one each way -- see the C++ side
    -- for why that specific sequence was worth collapsing. Guarded rather
    -- than assumed present: the test double for `skia` is lgi.cairo, which
    -- has neither push_node nor pop_node, so it falls back to the same
    -- sequence spelled out longhand.
    if cr.push_node then
        local ok
        ok, clip_x1, clip_y1, clip_x2, clip_y2 = cr:push_node(
            self:get_matrix_to_parent(), ext_x, ext_y, ext_width, ext_height)
        if not ok then
            cr:pop_node()
            return
        end
    else
        cr:save()
        cr:transform(self:get_matrix_to_parent())
        if not intersects_clip(cr, ext_x, ext_y, ext_width, ext_height) then
            cr:restore()
            return
        end
        cr:rectangle(ext_x, ext_y, ext_width, ext_height)
        cr:clip()
        clip_x1, clip_y1, clip_x2, clip_y2 = cr:clip_extents()
    end

    -- Draw if needed
    if clip_x2 - clip_x1 ~= 0 and clip_y2 - clip_y1 ~= 0 then
        local opacity = widget:get_opacity()
        -- Read once instead of per callback; it is a table lookup plus a
        -- two-value return that every hook below would otherwise repeat.
        local self_width, self_height = self:get_size()

        -- Prepare opacity handling
        if opacity ~= 1 and cr.push_group then
            cr:push_group()
        end

        -- _nothing_changed (from hierarchy_update() above) is strictly
        -- stronger than _moved_only: widget, context, size AND position are
        -- all identical to last time, not just content. That is exactly the
        -- condition under which a recorded picture is valid to replay
        -- instead of re-running every widget's :draw() in this subtree --
        -- general and not opt-in, unlike the shift-blit skip above, since it
        -- does not depend on any parent having coordinated a pixel shift.
        -- skia.new_picture_recorder is checked as the guard for whether the
        -- backend supports this at all (nil under the lgi.cairo test
        -- double), same pattern as cr.push_group above.
        if self._nothing_changed and skia.new_picture_recorder then
            if not self._picture_valid then
                self._picture = record_picture(self, context, widget, self_width, self_height,
                    ext_x, ext_y, ext_width, ext_height, cr)
                self._picture_valid = true
            end
            if self._picture then
                cr:draw_picture(self._picture, ext_x, ext_y)
            end
        else
            draw_content(self, context, cr, widget, self_width, self_height,
                clip_x1, clip_y1, clip_x2, clip_y2)
        end

        -- Apply opacity
        if opacity ~= 1 and cr.pop_group_to_source then
            cr:pop_group_to_source()
            cr:set_operator(skia.Operator.OVER)
            cr:paint_with_alpha(opacity)
        end
    end

    if cr.push_node then
        cr:pop_node()
    else
        cr:restore()
    end
end

return hierarchy

-- vim: filetype=lua:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:textwidth=80
