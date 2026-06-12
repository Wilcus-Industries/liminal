-- spin.lua: rotate the entity and pulse its color. Demonstrates the script
-- contract (return a table with on_start/on_update) and Transform reference
-- semantics (t.position.y = ... mutates the real component).
local speed = 90.0 -- degrees per second; edit while running to test hot reload

local M = {}

function M.on_start(self)
    lm.log("spin start: " .. self.name)

    -- Tripwire: member access must be by reference, not by copy.
    local t = self:get_transform()
    local y0 = t.position.y
    t.position.y = y0 + 1.0
    assert(math.abs(self:get_transform().position.y - (y0 + 1.0)) < 1e-4,
           "Transform reference semantics broken: t.position.y write was lost")
    t.position.y = y0
end

function M.on_update(self, dt)
    local t = self:get_transform()
    t.rotation.y = t.rotation.y + speed * dt

    local mr = self:get_mesh_renderer()
    if mr then
        local pulse = 0.5 + 0.5 * math.sin(lm.time.now() * 2.0)
        mr:set_color(0.35 + 0.6 * pulse, 0.45, 0.95 - 0.6 * pulse)
    end
end

return M
