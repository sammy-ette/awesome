--------------------------------------------------------------------------
-- Renderer-neutral rectangle region used by the Skia widget path.
-- It only tracks invalidation geometry; it is never a pixel buffer.
--------------------------------------------------------------------------

local region = {}

function region.new()
    local result = { _rectangles = {} }

    function result:is_empty()
        return #self._rectangles == 0
    end

    function result:num_rectangles()
        return #self._rectangles
    end

    function result:get_rectangle(index)
        return self._rectangles[index + 1]
    end

    function result:union_rectangle(rect)
        if rect.width > 0 and rect.height > 0 then
            table.insert(self._rectangles, {
                x = rect.x, y = rect.y,
                width = rect.width, height = rect.height,
            })
        end
    end

    return result
end

return region
