# Skia backend performance plan

Goal, stated as three things that must be true when this is done:

1. Scrolling the overflow layout looks smooth.
2. Resizing a wibox is smooth.
3. Complex layouts don't lag.

This plan is written against `skia-with-vibes` after the caching fixes already
landed in this branch (see "Already done" at the bottom).

---

## The architecture, as it actually is

Worth stating plainly, because it changes what the right fix is.

Each drawable owns a **persistent offscreen GPU surface** (`content_surface`,
`skia_backend.cc:254`). The render loop is:

1. `begin_frame()` acquires a swapchain image and points the Lua-facing canvas
   at `content_surface` — *not* at the swapchain image (`skia_backend.cc:1031`).
2. Lua widgets draw into `content_surface`, but only if something invalidated
   (`drawable.lua:116`, `if content_invalid then`).
3. `end_frame()` snapshots `content_surface` and blits it **whole** onto the
   acquired swapchain image, every frame, unconditionally
   (`skia_backend.cc:1068-1080`).

There is already a fast path where nothing changed and Lua is never entered —
that's the `cache_replays` counter (`skia_backend.cc:1147`).

**The consequence that matters:** because `content_surface` persists across
frames and is copied to the swapchain in full every frame, *partial repainting
into `content_surface` is safe with no per-swapchain-image damage tracking at
all.* The swapchain images never hold partial state — they get a complete image
each time. The earlier plan called for per-image dirty regions and buffer-age
tracking; that was written assuming direct-to-swapchain rendering and is not
needed here. This makes phase 1 dramatically cheaper than previously scoped.

---

## Phase 1 — Partial repaint (biggest win, lowest risk)

**Problem.** The dirty region is computed and then thrown away.
`hierarchy:update()` accumulates real damage into `self._dirty_area`
(`drawable.lua:88`), and `drawable.lua:114` immediately resets it and then
paints with `cr:clip_rect(0, 0, width, height)` (`drawable.lua:121`) — the
entire drawable. So *any* change repaints *every* widget: one clock tick
repaints the whole bar, one hover highlight repaints the whole start menu.

**Why the fix is nearly free.** `hierarchy:draw()` already culls whole subtrees
against the canvas clip — `intersects_clip()` at `hierarchy.lua:327` and the
per-child check at `hierarchy.lua:399`. It already works and is already called.
It just never gets a clip tighter than the full surface, so it never rejects
anything. Give it a real clip and subtree culling turns on by itself.

**Work.**
- Replace the unconditional full-surface clip with a clip built from
  `self._dirty_area`'s rectangles, when the repaint is incremental (i.e. when
  `forced_content_repaint` is false and the region is non-empty).
- Clear only the dirty rectangles instead of the whole surface, and repaint the
  background/background_image only within them.
- Keep the full-surface path for `forced_content_repaint` (first paint, context
  change, swapchain rebuild, `needs_full_redraw()`).
- `gears.region` already merges overlapping rects (`region.lua`), so the rect
  count stays small. Add a cheap guard: if the region covers most of the
  surface, or has too many rects, just do the full repaint — cheaper than many
  small clipped passes.

**Risk.** Moderate but contained. The failure mode is visual (stale pixels in a
region that should have been repainted), and it's self-correcting on the next
full repaint. `layout_invalidated` already exists (`drawable.lua:81`) precisely
to force a full repaint when a layout change keeps identical extents — keep
honouring it.

**Delivers:** goal 3 directly, and most of goal 1 (scrolling a list inside a
complex layout stops repainting the rest of the layout).

---

## Phase 2 — Stop rebuilding the swapchain on every resize

**Problem.** This is the resize jank, and it's severe. Every geometry change
runs `drawable_ensure_renderer()` (`drawable.c:152`), which compares the
requested size against the renderer's exact current size and, on any
difference, calls `awesome_skia_renderer_resize()` → `create_swapchain()`
(`skia_backend.cc:760`). Per resize step that means:

- `xcb_aux_sync()` — a **blocking X round-trip** (`drawable.c:179`),
- `vkCreateSwapchainKHR` plus new image views and Skia surfaces,
- a fresh `content_surface` render target (`skia_backend.cc:929`),
- `content_needs_full_redraw = true` (`skia_backend.cc:942`) — forcing a full
  Lua widget repaint,
- the old swapchain pushed onto the retired list for fenced teardown.

Dragging a resize does all of that *per pixel step*.

**The scaffolding for the fix already exists and is inert.**
`skia_stable_backing`, `skia_capacity_width/height` and `skia_visible_width/
height` are all declared and handled in `drawable.c` — but
`skia_stable_backing` is **never set to `true` anywhere in the tree**. Verified
by grep across all `.c/.h/.cc/.lua`. The entire capacity path is dead code
today.

**Work.**
- Turn the capacity idea on for real: allocate the swapchain and
  `content_surface` at a **rounded-up** capacity (e.g. next multiple of 64px,
  or a growth factor), and only rebuild when the requested size exceeds it.
- Within capacity, a resize becomes: change the logical size used for layout
  and clipping, keep the swapchain. No `vkCreateSwapchainKHR`, no
  `xcb_aux_sync`, no forced full repaint.
- Present only the visible sub-rect: `end_frame()` currently blits the whole
  content image at 0,0 — blit the visible region instead, using
  `skia_visible_width/height`.
- Add the shrink/reclaim policy the old plan flagged: capacity currently only
  grows, so one oversized popup would hold that memory for the drawable's
  lifetime. Reclaim on unmap or after an idle period at a smaller size.
- Audit every consumer of `drawable:geometry()` before enabling, since it will
  start reporting capacity rather than visible size. Known call sites:
  `placement.lua:739`, `overflow.lua:426`, `slider.lua:546`,
  `drawable.lua:51/65/143`. (`handle_motion` is already migrated to
  `skia_visible_size()`.)

**Risk.** Highest of the phases — it changes what geometry means. Mitigation:
gate it behind a flag, enable for drawins only (not client titlebars), and do
the `drawable:geometry()` audit *first*.

**Delivers:** goal 2.

---

## Phase 3 — Frame pacing

**Problem.** Redraws are scheduled with `glib.idle_add` at
`PRIORITY_DEFAULT_IDLE` (`drawable.lua:429`) with a `_redraw_pending` guard,
plus a 1/120s deferral when a frame is unavailable (`drawable.lua:37`). There
is no vsync alignment. A drag that emits events faster than the refresh rate
produces redraws that are coalesced only by the idle guard, so pacing is
uneven — which reads as stutter even when average throughput is fine.

**Work.**
- Coalesce invalidations to at most one repaint per display refresh interval.
- Since present mode is already mailbox where available
  (`skia_backend.cc:751`), the GPU side discards excess frames correctly — the
  waste and the jitter are on the Lua/record side.
- Keep it simple: a monotonic "last presented at" timestamp per drawable and a
  minimum interval, rather than trying to build a real Choreographer.

**Risk.** Low. Worst case is one extra frame of latency; revert is trivial.

**Delivers:** the perceptual half of goals 1 and 2 — evenness, not throughput.

---

## Phase 4 — Retained subtrees (only if still needed)

Skia's `SkPicture` is the right primitive, and `skia.new_image_surface()` /
`:snapshot()` already exist in the bindings (`skia_lua.cc:1727`, `:2677`).
A container that records its subtree once and replays it while nothing in that
subtree has signalled would cut the record cost for static chrome (taglist,
systray, separators).

**Deliberately last.** After phase 1, unchanged subtrees are already skipped by
clip culling, which captures most of this benefit without any new API or
invalidation model to get wrong. Re-measure before building this; it may not be
necessary.

---

## Phase 5 — Micro-optimisations

Cheap, independent, do any time:

- **Textbox draw path.** `textbox:draw()` calls `setup_layout()` then
  `cr:update_layout()` on every draw (`textbox.lua:42-54`). Pango already
  short-circuits an unchanged `set_width` (confirmed by measurement earlier in
  this work), so the remaining cost is the `update_layout` round-trip. Check
  whether it can be skipped when neither text, font, DPI nor size changed.
- **Overflow off-screen fits.** `overflow:layout()` iterates from index 1 every
  time and only breaks once past the visible portion, so scrolled to the bottom
  it fits every child. Fit results are cached now, so this is bounded — but
  starting the scan at an estimated first-visible index would make it O(visible)
  instead of O(scrolled-past).
- **`hierarchy:draw()` opacity groups.** `push_group()` per node with
  `opacity ~= 1` (`hierarchy.lua:378`) maps to a `saveLayer`-equivalent on
  Ganesh, which is more expensive on GPU than it was on software Cairo. Worth a
  look if opacity is used heavily in the config.

---

## Order of work

1. **Phase 1** — partial repaint. Done. Biggest win, contained blast radius,
   and it makes phase 4 probably unnecessary.
2. **Phase 3** — frame pacing. Done. Small, and it's what makes phase 1's win
   actually *look* smooth.
3. **Phase 2** — stable backing. Not started. Do the `drawable:geometry()`
   audit first.
4. **Phase 5** — micro-optimisations, as filler.
5. **Phase 4** — only if measurement still demands it.

Verification is by eye in a real session on `awesome-skia`, not by synthetic
benchmark. `AWESOME_SKIA_PROFILE_DETAIL=1` prints cache rebuild vs replay
(`drawable.lua:109`), and the backend prints
`frames/rebuilds/replays/fps/acquire/record/submit` (`skia_backend.cc:281`) —
useful for confirming a change did what was intended, but the acceptance test
is whether it looks right.

---

## Already done in this branch

- **Widget fit/layout cache no longer GC-evictable.** `gears.cache` uses weak
  values (`__mode = "v"`), so an unrelated GC sweep could drop a widget's
  cached `:fit()`/`:layout()` mid-scroll, forcing full Pango re-shaping.
  `wibox/widget/base.lua` and `wibox/container/scroll.lua` now use a
  strongly-referenced cache with identical semantics, still fully invalidated
  by the existing `clear_caches()` path. This was the dominant cost in
  scrolling a long list.
- **Inverted matrix cached** in `hierarchy:get_matrix_from_device()`, which
  `find_widgets()` calls per node on every mouse move.
- **`overflow.lua` transform-scroll fast path reverted** to real per-frame
  layout. It froze hierarchy positions at `scroll_factor = 0` while translating
  at draw time, so hit-testing hit stale rows. Cheap again after the cache fix.
- **`handle_motion()`** now uses `drawable:skia_visible_size()` rather than
  the potentially-inflated `geometry()`.
- **Phase 1 (partial repaint).** `drawable.lua`'s `do_redraw()` keeps a
  reference to the dirty region before resetting it, and builds the paint
  clip from its rectangles (`cr:rectangle` per rect + `cr:clip()`, intersected
  with the full-surface `clip_rect`) instead of always clipping to
  `0, 0, width, height`. Falls back to a full repaint when
  `forced_content_repaint`/`layout_invalidated` is set, or the region has
  grown to more than 16 rects or over 60% of the surface area (many small
  clipped passes cost more than one full one). `hierarchy:draw()`'s existing
  subtree culling does the rest for free once the clip is real.
- **Phase 3 (frame pacing).** `drawable.lua` tracks `_last_redraw_start_time`
  and paces `ret.draw()` to at most one repaint per `1/60`s
  (`AWESOME_SKIA_TARGET_FPS` env var to override): if less than one frame
  interval has elapsed since the last redraw *started*, the redraw is
  scheduled via `gears.timer` for the remaining time instead of firing on the
  next idle-loop iteration. There is still no real vsync signal from the
  backend to align to; this only stops Lua from doing wasted record/submit
  work faster than a frame could ever be shown.
  **Bug fixed after profiling:** this originally paced off
  `_last_present_time`, stamped *after* `cr:present()`. In this
  single-threaded, blocking render loop the next redraw request can't be
  delivered until the main loop frees up — which happens right as the
  previous present finishes — so "elapsed since last present" read as ~0 on
  essentially every redraw, forcing a full extra `1/60`s wait on top of every
  frame regardless of how long it actually took to process. Profiling a
  scroll (`AWESOME_SKIA_PROFILE_DETAIL=1`) showed record+submit ≈ 17-18ms/frame
  but an actual frame period of ~35ms — exactly one processing time plus one
  spurious extra interval. Pacing off the redraw's *start* time instead fixes
  this: a frame that already takes ≥ `1/60`s to process no longer gets a
  second interval tacked on, while genuinely fast/cheap redraws (e.g. rapid
  mouse-move) are still capped as intended.
- **Wheel-scroll easing (not in the original plan).** After phases 1 and 3
  landed, scrolling still didn't feel smooth — turned out this wasn't a
  rendering-throughput problem at all. `overflow:scroll()`
  (`overflow.lua:289`, called from the mouse-wheel `button::press` handler)
  jumped `scroll_factor` straight to its target with `set_scroll_factor()`,
  no interpolation, so every wheel tick was an instant snap regardless of how
  fast the pipeline could render it. `overflow.lua` now keeps a separate
  `_private.scroll_target` and eases the displayed `scroll_factor` toward it
  every `1/60`s via a fixed per-tick factor (`SCROLL_ANIM_ALPHA`, derived from
  a time constant `SCROLL_ANIM_TAU = 0.09`) instead of a measured wall-clock
  delta — deterministic and simpler, and it's what makes the behaviour
  testable without a running GLib main loop. A new wheel tick just moves the
  target further; the in-flight timer picks it up on its next tick, so a fast
  burst of ticks reads as one continuous motion. Dragging the scrollbar
  itself (`build_grabber`) still tracks the cursor 1:1 and cancels any
  in-flight easing first (`_cancel_scroll_animation`), since a direct drag
  should never lag behind the pointer.

## Ranked follow-up list (widget-system efficiency, not overflow-specific)

After the fixes above, the remaining complaint wasn't rendering throughput —
it was general per-frame overhead in the widget/hierarchy system. Worked in
the order requested, skipping the `protected_call`-per-draw-callback item
(rejected: the safety net it removes is load-bearing) and deferring
buffer-age tracking for the `content_surface` → swapchain blit (parked, not
needed yet since that blit is always a full-surface copy regardless of what
changed upstream).

1. **Translation damage / scroll shift-blit.** `overflow.lua`'s row
   placement marks every content row (not the scrollbar or spacing widgets)
   `placed.shift_eligible = true`. `before_draw_children()`
   (`overflow.lua:434`) computes how far the content moved since the last
   drawn frame (`pending_shift`, valid only when `cache_valid` — no known-good
   prior frame to shift from otherwise) and, when a real shift is pending,
   grabs `cr:snapshot_content()` (what `content_surface` already held before
   this frame's redraw touches it), clears, and blits it back translated by
   the shift amount instead of re-running every row's draw from scratch.
   Confined to the content rect via `cr:rectangle()` + `cr:clip()`, and
   verified rather than assumed to actually cover that whole rect afterwards
   (`SHIFT_CLIP_EPSILON`-bounded `clip_extents()` check) before committing —
   `cr:clip()` only narrows, so if `drawable.lua`'s own partial-repaint clip
   is already tighter than the full content area this frame, the shift would
   otherwise silently apply to less than intended and leave stale pixels at
   an edge; falls back to a normal full redraw of every row in that case.
   `hierarchy:draw()`'s loop then skips redrawing any row that is both
   `shift_eligible` and provably unchanged since the last update
   (`hierarchy_update()`'s `_moved_only`), since the shift-blit already left
   correct pixels there.
   **Wrong first attempt, kept in history:** the initial version gated the
   skip on the narrower `cr:needs_full_redraw()` alone (true only across
   swapchain recreation). That missed the case where `content_surface` gets
   wiped for a reason local to this widget but unrelated to the shift itself
   — a sibling widget's `widget::layout_changed` forcing
   `context._full_content_repaint` elsewhere in the same drawable — which
   the shift-blit path has no way to see from `needs_full_redraw()` alone.
   Fixed by threading a second, explicit flag,
   `context._full_content_repaint` (set in `drawable.lua` before calling
   `self._widget_hierarchy:draw()`), down to both `overflow.lua` and
   `hierarchy.lua`, and gating on both.
2. **Retained `SkPicture`s for unchanged subtrees.** `hierarchy.lua`'s
   `hierarchy_update()` now distinguishes three states per node: a full
   structural change (`_nothing_changed = false`, `_picture_valid = false`,
   cache dropped), moved-only (position changed, content didn't —
   `_moved_only = true`), and truly nothing changed — neither position nor
   content (`_nothing_changed = true`, new). `hierarchy:draw()` (line ~696)
   checks the last case: if `skia.new_picture_recorder` exists (guards the
   `lgi.cairo` test stub, which lacks it) and the node's cached `_picture` is
   still valid, it replays the cached picture (`cr:draw_picture`) instead of
   walking the widget's `draw()`/`before_draw_children`/children chain again.
   The picture is (re)built via `record_picture()` (line 625), a local
   function that opens a `skia.new_picture_recorder(w, h, template_cr)`,
   translates to the node's extents origin, and records through the same
   `draw_content()` path used by the live-canvas case — so there is exactly
   one code path for "how a node draws its children," just aimed at either a
   live canvas or a recorder.
   Two failure modes were designed out up front, both because the earlier
   (reverted) raster-snapshot "Phase 4" attempt died from exactly this class
   of bug:
   - *Ambient paint state.* A freshly-opened recording canvas starts from
     Skia's own default (opaque black), not whatever was ambient on the live
     canvas that requested the recording. `skia.new_picture_recorder` takes
     an explicit template frame argument and copies its *entire* `SkPaint`
     (not just a color string) onto the new recorder's frame state
     (`skia_lua.cc`, `skia_new_picture_recorder`), so anti-aliasing, alpha,
     blend mode, etc. all carry over correctly.
   - *Redraw-only changes.* A widget can request a repaint
     (`widget::redraw_needed`) without its layout changing at all — text
     blinking a cursor, a color animation. `hierarchy.lua`'s `_redraw()` was
     extended to walk ancestors invalidating `_picture_valid`/`_picture` in
     addition to its existing job of calling `redraw_callback`, so a
     redraw-only change still drops the stale cached picture even though
     `hierarchy_update()` never ran or saw a content change.
   Verified with a hand-built mock recorder (`skia.new_picture_recorder`
   returning a fake picture object, `cr:draw_picture` recording what it was
   asked to draw) rather than by inspection alone, confirming: (a) an
   unchanged subtree draws once, then replays on subsequent frames without
   calling the underlying widgets' `draw()` again, (b) a redraw-only signal
   invalidates the cache and forces a real re-record, (c) the template
   frame's paint state round-trips through the recorder unchanged.
   Deliberately **not** extended to `_moved_only` nodes yet (that's
   `overflow.lua`'s shift-blit's job for its own specific case, item 1 above)
   — broadening this to cover movement generally, right after two real bugs
   already came out of this exact feature area in one session, is exactly
   the kind of scope creep likely to produce a third one. Left as a
   deliberate follow-up, not to be picked up without being asked.
3. **Fewer Lua↔C crossings per node.** `hierarchy:draw()`'s per-node entry
   and exit used to be five separate calls into the C canvas binding every
   frame, per node: `save()`, `transform()`, an `intersects_clip()` helper
   that itself called `clip_extents()` to test against the extents box,
   `rectangle()`, `clip()`, then a second `clip_extents()` afterwards to read
   back the post-clip bounds used for culling children. At roughly 135 nodes
   and a 60fps target, that's on the order of 40,000 Lua↔C round trips a
   second just for bookkeeping that does no actual drawing.
   Collapsed into one call each way: `cr:push_node(matrix, x, y, width,
   height)` (`skia_lua.cc:974`) applies the transform, tests the extents box
   against the current clip in native code, and — only if there's a real
   intersection — narrows the clip and returns the post-clip bounds directly
   as four return values (`ok, x1, y1, x2, y2`), avoiding the round trip
   entirely when a subtree would have been culled anyway. `cr:pop_node()`
   (`skia_lua.cc:1029`) is the matching restore. `hierarchy.lua` (line ~664)
   uses this fast path when `cr.push_node` exists, and falls back to the
   original five-call sequence otherwise — kept for the `lgi.cairo` test
   stub, which implements neither method, so the automated suite still
   exercises a real, if slower, correctness path.
   Verified with two hand-built `cr` mocks over a real `wibox.hierarchy`
   tree: one implementing only `push_node`/`pop_node` (tracking an actual
   clip-rectangle stack to faithfully emulate the native intersection
   semantics), one implementing only the old `save`/`transform`/
   `clip_extents`/`rectangle`/`clip` sequence. Compared which widgets had
   their `draw()` invoked, and in what order, across both a clip that admits
   every row and a narrowed clip that should cull some — identical results
   (`drawn=1,2,3,4,5` full-clip; `drawn=1,2` narrow-clip) on both paths in
   both cases.
5. **`VK_KHR_incremental_present`.** Presentation-hint-only: it tells the
   presentation engine which regions of the swapchain image actually changed
   this frame (`VkPresentRegionsKHR`/`VkPresentRegionKHR`, chained onto
   `VkPresentInfoKHR` in `end_frame()`, `skia_backend.cc:1203`), letting it
   skip recompositing/copying the untouched parts of the screen on
   compositors that respect the hint. Safe to add without any buffer-age or
   per-swapchain-image damage tracking specifically because of the
   architecture fact at the top of this document: `content_surface` is
   always blitted onto the swapchain image whole, every frame, regardless of
   what changed — so "what changed" for this hint's purposes is just the
   drawable's own dirty region (already tracked for Phase 1's partial
   repaint), no different across swapchain images. Enabled conditionally at
   device-creation time if the extension is present
   (`create_device_and_skia()`), reported under `AWESOME_SKIA_PROFILE=1`.
   Bundled in with this: **device selection was previously "first match,"**
   which could silently select `llvmpipe` (a CPU software rasterizer) over a
   real discrete/integrated GPU if `llvmpipe` happened to enumerate first.
   `choose_device()` now ranks candidates by `VkPhysicalDeviceType`
   (discrete > integrated > virtual > other > CPU) and picks the best,
   printing the selected device name and a warning if it's a software
   rasterizer under `AWESOME_SKIA_PROFILE=1`.