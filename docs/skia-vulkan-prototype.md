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

## Lazy build

From the AwesomeWM repository, run:

```sh
./tools/bootstrap-skia-vulkan-probe.sh --run
```

The script performs the otherwise manual work:

1. Clones `depot_tools` and Skia into the user cache directory.
2. Synchronizes Skia's dependencies.
3. Builds a static Vulkan-enabled Skia library.
4. Configures and builds the GPU-enabled `awesome` executable and
   `skia-vulkan-probe`.
5. Runs the probe when `--run` is supplied.

Nothing is installed. By default, downloads and build files stay under:

```text
${XDG_CACHE_HOME:-$HOME/.cache}/awesome-skia-probe
```

Useful controls:

```sh
# Re-fetch the selected Skia branch/tag/commit.
SKIA_REF=main ./tools/bootstrap-skia-vulkan-probe.sh --update --run

# Remove all downloaded and generated probe files.
./tools/bootstrap-skia-vulkan-probe.sh --clean

# Override parallelism or the cache location.
JOBS=8 AWESOME_SKIA_CACHE_DIR=/tmp/awesome-skia \
    ./tools/bootstrap-skia-vulkan-probe.sh --run
```

The script does not use `sudo` or install distribution packages. Vulkan and XCB
development headers, a C/C++ compiler, Git, Python, CMake, and pkg-config still
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
The bootstrap script guarantees that pairing, but the backend may still need
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

The bootstrap configuration links the libraries selected by its GN arguments
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

## Current Skia compatibility

The probe is tested against current Skia rather than a pinned historical
revision. Skia paths are immutable in current releases, so the diagnostic curve
uses `SkPathBuilder` before creating its `SkPath`. Ganesh also requires wrapped
Vulkan render targets to advertise transfer-source and transfer-destination
usage. The probe requests those flags when creating its swapchain and reports
a clear initialization error if the presentation surface cannot provide them.
