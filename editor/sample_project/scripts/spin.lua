-- spin.lua: rotate the owning entity and pulse its tint. Edit `speed` while
-- the editor is in Play mode to watch hot reload.
local speed = 90.0 -- degrees per second

local M = {}

function M.on_start(self)
    lm.log("spin start: " .. self.name)
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
