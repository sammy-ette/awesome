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
            local merged = {
                x = rect.x, y = rect.y,
                width = rect.width, height = rect.height,
            }
            -- Dirty rectangles are small and few. Merge intersecting (or
            -- edge-touching) entries so callers retain Cairo Region's useful
            -- coalescing behaviour without depending on a pixel backend.
            local index = 1
            while index <= #self._rectangles do
                local other = self._rectangles[index]
                local merged_right = merged.x + merged.width
                local merged_bottom = merged.y + merged.height
                local other_right = other.x + other.width
                local other_bottom = other.y + other.height
                if merged.x <= other_right and other.x <= merged_right and
                   merged.y <= other_bottom and other.y <= merged_bottom then
                    local left = math.min(merged.x, other.x)
                    local top = math.min(merged.y, other.y)
                    merged.width = math.max(merged_right, other_right) - left
                    merged.height = math.max(merged_bottom, other_bottom) - top
                    merged.x, merged.y = left, top
                    table.remove(self._rectangles, index)
                else
                    index = index + 1
                end
            end
            table.insert(self._rectangles, merged)
        end
    end

    return result
end

return region
