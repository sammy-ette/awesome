-- This script is given to Busted via the --helper argument. Modules loaded here
-- won't be cleared and reloaded by Busted. This is needed for lgi because lgi
-- is not safe to reload and yet Busted manages to do this.
local lgi = require "lgi"

-- Busted runs in a standalone Lua VM, while the production `skia` module is
-- embedded into the Awesome binary.  Give unit tests the same Cairo-shaped
-- API contract with LGI's existing drawing objects; renderer integration is
-- exercised by the Xephyr tests against the native module.
package.preload.skia = function()
   local skia = lgi.cairo
   local surface_is_type_of = skia.Surface.is_type_of
   skia.is_canvas = function() return false end
   skia.is_image = function() return false end
   skia.Surface.is_type_of = function(value)
      return surface_is_type_of(skia.Surface, value)
   end
   skia.Surface.from_bgra = function(_, width, height, stride)
      return lgi.cairo.ImageSurface(lgi.cairo.Format.ARGB32, width, height)
   end
   skia.LineCap = lgi.cairo.LineCap
   skia.FillRule = lgi.cairo.FillRule
   return skia
end

-- Always show deprecated messages
_G.awesome = {
   version = "v9999",
   api_level = 9999,
}

-- "fix" some intentional beautiful breakage done by .travis.yml
require("beautiful").init { a_key = "a_value" }

-- vim: filetype=lua:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:textwidth=80
