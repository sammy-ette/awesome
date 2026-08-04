# Skia/Vulkan renderer migration

`WITH_SKIA_VULKAN=ON` builds Skia into the production `awesome` executable as
well as the standalone diagnostic target. Each eligible drawin now has a
Vulkan swapchain whose images are wrapped by Skia Ganesh surfaces. Rendering is
available through a deliberately renderer-neutral C ABI and a small native Lua
`skia` module.

The runtime module is intentionally explicit during the migration:

```lua
local frame = skia.begin(my_drawin.drawable.skia_renderer)
frame:clear(0x11141cff)
frame:fill_round_rect(24, 24, 300, 80, 16, 16, 0x58a6ffff)
frame:present()
```

`frame` is a GPU frame: it cannot be presented twice, and its garbage collector
path closes an abandoned frame safely.

The same implementation can be loaded outside the Awesome executable as a
native Lua module (`skia.so`). This is the integration point used by the
vendored Awexygen runtime: GTK3 remains its window/event host, but Skia owns
the rasterization and Vulkan presentation directly into the X11 child window.
Awexygen does not use a Cairo presentation bridge or expose a Cairo canvas to
its renderer. Set `AWESOMEWM_SKIA_MODULE_DIR` to the directory containing the
module when launching Awexygen. The module must match the launcher's Lua ABI;
for example, a module built against Lua 5.4 cannot be loaded by a Lua 5.1
interpreter.

## Build and install

From the AwesomeWM repository, run:

```sh
make
sudo make install
```

`make` clones and builds Skia under `build/_deps/`, configures Awesome against
that exact checkout, and builds Awesome once. It does not use `$HOME/.cache`.
`sudo make install` is install-only: it never downloads or builds dependencies
as root.

Useful controls for the local dependency checkout:

```sh
# Re-fetch the selected Skia branch/tag/commit.
SKIA_REF=main tools/build-skia-vulkan.sh --update

# Remove all downloaded and generated probe files.
tools/build-skia-vulkan.sh --clean
```

The build helper does not use `sudo` or install distribution packages. Vulkan
and XCB development headers, a C/C++ compiler, Git, Python, CMake, and pkg-config still
need to exist on the system. The CMake target discovers the system libraries
used by Skia's static archive (Fontconfig, FreeType, HarfBuzz, PNG, JPEG, WebP,
Expat, and zlib) through pkg-config; they must have their development packages
installed as well.

## Manual Skia build

Skia currently requires a C++20 compiler and uses GN/Ninja rather than CMake.
To build it manually:

```sh
python3 tools/git-sync-deps
bin/gn gen out/Release --args='\
    is_official_build=true \
    skia_use_vulkan=true \
    skia_use_gl=false'
ninja -C out/Release skia
```

This prototype uses Skia's private Vulkan allocator factory because Skia does
not expose a public factory for the allocator selected by its build. Therefore,
the Skia source directory and `libskia.a` **must come from the same revision**.
The build helper guarantees that pairing, but the backend may still need
adjustment if a newer Skia revision changes that private API. Set `SKIA_REF` to
a known-working commit when reproducibility becomes important.

## Manual probe build

From the AwesomeWM repository:

```sh
cmake -S . -B build \
    -DWITH_SKIA_VULKAN=ON \
    -DSKIA_SOURCE_DIR=/path/to/skia \
    -DSKIA_LIBRARY=/path/to/skia/out/Release/libskia.a \
    -DGENERATE_DOC=OFF \
    -DGENERATE_MANPAGES=OFF
cmake --build build --target skia-vulkan-probe -j
./build/skia-vulkan-probe
```

The build configuration links the libraries selected by its GN arguments
automatically. `SKIA_EXTRA_LIBRARIES` remains available only for an explicitly
customized Skia build that needs additional libraries:

```sh
-DSKIA_EXTRA_LIBRARIES='fontconfig;freetype;z'
```

Press any key in the probe window to exit.

## GPU scheduling and remaining migration work

Frame acquisition, Skia completion, and presentation use Vulkan binary
semaphores. Two frame slots are recycled with queue fences, and Skia submits
with `GrSyncCpu::kNo`; normal rendering does not wait for GPU rendering on the
CPU.

This is not yet a complete Cairo removal. Awesome's installed Lua API exposes
native `cairo_surface_t*` values, widgets pass Cairo contexts into user draw
callbacks, and PangoCairo is used for text. Finishing the migration requires a
breaking replacement canvas API (or a very large Cairo ABI compatibility
implementation), a shared Vulkan device/Skia context for all windows, and
GPU-native shaped text. The code deliberately does not pretend that linking
Skia makes those Cairo contracts disappear.

Client titlebars render through a second renderer mode. Up to four titlebars
share one client frame window, so they cannot each own a swapchain; instead
`awesome_skia_renderer_create_raster()` draws with Skia into a CPU surface and
uploads it into the X pixmap Awesome already blits to the frame window. The
Lua-facing canvas is identical to a GPU frame's, so no widget code knows the
difference, and replacing this with a real GPU frame compositor later needs no
Lua changes.

GPU images are now bridged: `skia.new_image_surface(width, height)` creates an
offscreen raster canvas (sharing the same Cairo-shaped drawing methods as a
live frame) that finishes into a reusable `skia.image` via `:snapshot()`.
`frame:draw_image()`/`skia.image_dimensions()` accept either a file path or
one of these in-memory images, so both file-backed icons and procedurally
drawn ones (e.g. `beautiful.awesome_icon`, generated once from the existing
vector logo code) work through `wibox.widget.imagebox`. `drawable:set_bgimage`
also accepts a file path directly under Skia. Arbitrary Cairo-surface/RSVG
backgrounds are still unsupported since they need the same rasterize bridge
but aren't the common case.

## Porting existing configs

The lowest-risk migration boundary is the Cairo-shaped Lua canvas. Existing
`cr:rectangle()`, `cr:clip()`, `cr:set_source_surface()`, `cr:paint()`, and
similar calls can continue to work; use the native `skia` namespace only for
surface construction and let the renderer choose the implementation:

```lua
local drawing = rawget(_G, "skia") or require("lgi").cairo

local surface = drawing.ImageSurface.create(
    drawing.Format.ARGB32, width, height)
local cr = drawing.Context(surface)
```

On this build the first branch selects Skia. On a stock Awesome installation
the second branch preserves the original LGI Cairo behavior. The Skia canvas
also provides the common Cairo toy-text calls used by older widgets:
`get_current_point`, `set_font_size`, `select_font_face`, `get_font_face`,
`set_font_face`, `text_extents`, and `show_text`.
When legacy code constructs a context around a source-only Skia image, the
binding uses a writable raster copy, so thumbnail code can keep its existing
`Context(content)` flow.

Invalidation regions are not pixel surfaces, so code that creates
`cairo.Region`/`cairo.RectangleInt` values should use the renderer-neutral
`gears.region` API instead:

```lua
local region = require("gears.region").new()
region:union_rectangle { x = x, y = y, width = width, height = height }
```

Do not require a private `_native` field on Skia surfaces. Pass the Skia
surface or image directly; the client, imagebox, and drawing bindings accept
those objects. If one line must run on both backends, an explicit compatibility
fallback such as `img._native or img` is appropriate. Genuine Cairo
interop—GDK/RSVG/PangoCairo objects, raw `cairo_surface_t*` access, and Cairo
data-buffer APIs—still needs to remain in an isolated Cairo-only component
until an explicit bridge is added.

The Awexygen adapter intentionally does not provide that bridge. GTK3 is
retained internally for lifecycle, input, and optional wrapped GTK widgets,
and its own implementation may use Cairo internally, but no Awexygen runtime
module imports `lgi.cairo` or constructs a Cairo object. Shape masks are still
stored for API compatibility and visual clipping remains in the Skia widget
tree; native X Shape/input-mask integration is follow-up work.

## Current Skia compatibility

The probe is tested against current Skia rather than a pinned historical
revision. Skia paths are immutable in current releases, so the diagnostic curve
uses `SkPathBuilder` before creating its `SkPath`. Ganesh also requires wrapped
Vulkan render targets to advertise transfer-source and transfer-destination
usage. The probe requests those flags when creating its swapchain and reports
a clear initialization error if the presentation surface cannot provide them.
