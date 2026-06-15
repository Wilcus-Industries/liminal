-- spin_center.lua: rotate the cube counter-clockwise about ONE axis (Z, the
-- axis pointing at the camera) around the cube's true geometric center.
--
-- Why the math: the builtin:box mesh spans local y=0..1, so its local origin
-- sits at the BOTTOM-center, not the middle. Transform rotation pivots about
-- the local origin, so a raw Z spin would swing the cube around its bottom
-- edge. The cube's real center is local (0, halfH, 0). To pin that center in
-- place we counter-translate the position every frame so the center stays put.
local speed = 90.0 -- degrees per second, +Z = counter-clockwise toward camera

local M = {}
local cx, cy, cz, halfH

function M.on_start(self)
    lm.log("spin_center start: " .. self.name)

    local t = self:get_transform()
    halfH = 0.5 * t.scale.y               -- distance from local origin to center
    -- pinned world-space center, captured at the resting (unrotated) pose
    cx = t.position.x
    cy = t.position.y + halfH
    cz = t.position.z

    local mr = self:get_mesh_renderer()
    if mr then
        mr:set_color(0.5, 0.45, 0.95)
    end
end

function M.on_update(self, dt)
    local t = self:get_transform()
    t.rotation.z = t.rotation.z + speed * dt

    -- Re-pin: world center = position + R_z(a) * (0, halfH, 0).
    -- R_z(a)*(0,halfH,0) = (-halfH*sin a, halfH*cos a, 0). Solve for position.
    local a = math.rad(t.rotation.z)
    t.position.x = cx + halfH * math.sin(a)
    t.position.y = cy - halfH * math.cos(a)
    t.position.z = cz
end

return M
