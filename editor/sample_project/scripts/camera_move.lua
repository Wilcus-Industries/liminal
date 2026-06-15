-- camera_move.lua: WASD fly movement + mouse-look. Tab toggles cursor capture
-- (locked/hidden = mouse-look active; unlocked = free cursor). Movement is on
-- the XZ plane relative to the camera's yaw (rotation.y).
local speed = 5.0       -- units per second
local sensitivity = 0.12 -- degrees per pixel of mouse movement
local KEY_TAB = 258      -- GLFW_KEY_TAB

local M = {}
local tabWasDown = false

function M.on_start(self)
    lm.input.set_cursor_captured(true) -- start in mouse-look mode
end

function M.on_update(self, dt)
    local t = self:get_transform()

    -- Tab edge-detect → toggle capture
    local tabDown = lm.input.key_down(KEY_TAB)
    if tabDown and not tabWasDown then
        lm.input.set_cursor_captured(not lm.input.cursor_captured())
    end
    tabWasDown = tabDown

    -- Mouse-look only while captured
    if lm.input.cursor_captured() then
        local dx, dy = lm.input.mouse_delta()
        t.rotation.y = t.rotation.y - dx * sensitivity
        local pitch = t.rotation.x - dy * sensitivity
        if pitch > 89.0 then pitch = 89.0 end
        if pitch < -89.0 then pitch = -89.0 end
        t.rotation.x = pitch
    end

    local yaw = math.rad(t.rotation.y)
    -- yaw=0 looks down -Z, so forward = (-sin, -cos) and right = (cos, -sin)
    local fx, fz = -math.sin(yaw), -math.cos(yaw)
    local rx, rz = math.cos(yaw), -math.sin(yaw)

    local mx, mz = 0.0, 0.0
    if lm.input.key_down("w") then mx = mx + fx; mz = mz + fz end
    if lm.input.key_down("s") then mx = mx - fx; mz = mz - fz end
    if lm.input.key_down("d") then mx = mx + rx; mz = mz + rz end
    if lm.input.key_down("a") then mx = mx - rx; mz = mz - rz end

    -- normalize diagonal so it isn't faster
    local len = math.sqrt(mx * mx + mz * mz)
    if len > 0.0 then
        local step = speed * dt / len
        t.position.x = t.position.x + mx * step
        t.position.z = t.position.z + mz * step
    end
end

return M
