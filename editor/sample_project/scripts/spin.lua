-- spin.lua: rotate the owning entity vertically. Edit `speed` while
-- the editor is in Play mode to watch hot reload.
local speed = 90.0 -- degrees per second

local M = {}

function M.on_start(self)
    lm.log("spin start: " .. self.name)

    local mr = self:get_mesh_renderer()
    if mr then
        mr:set_color(0.5, 0.45, 0.95)
    end
end

function M.on_update(self, dt)
    local t = self:get_transform()
    t.rotation.x = t.rotation.x + speed * dt
end

return M




