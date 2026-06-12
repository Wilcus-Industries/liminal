-- bob.lua: sine-bob on Y. Proves per-entity environment isolation: `elapsed`
-- and `base_y` are chunk locals, yet two entities running this same file bob
-- independently (phase offset derived from each entity's own start position).
local elapsed = 0.0
local base_y = 0.0
local phase = 0.0

local M = {}

function M.on_start(self)
    local t = self:get_transform()
    base_y = t.position.y
    phase = t.position.x * 2.1 -- per-entity phase from placement
    lm.log("bob start: " .. self.name .. " (phase " .. string.format("%.2f", phase) .. ")")
end

function M.on_update(self, dt)
    elapsed = elapsed + dt
    local t = self:get_transform()
    t.position.y = base_y + 0.5 * math.sin(elapsed * 2.0 + phase)
    t.rotation.y = t.rotation.y - 30.0 * dt
end

return M
