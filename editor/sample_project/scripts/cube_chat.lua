-- cube_chat.lua — two cubes that are AWARE of each other and hold a
-- back-and-forth conversation driven by the local LLM (lm.ai).
--
-- Attach this SAME file to both cube entities ("spinner" and "spinner2").
-- Each instance picks its persona from its entity Name. The two instances
-- share one conversation through a global table (_G.cube_chat): per-entity
-- script environments fall through to globals for reads, and `_G.x = v`
-- writes onto the real shared globals table, so both cubes see the same
-- history / turn / pending request. Only the cube whose turn it is submits
-- to the single shared inference engine, which keeps requests serialized.
--
-- The speaking cube brightens and bobs; the listening cube dims. Lines are
-- printed to the engine console via lm.log.
--
-- MOVEMENT: each turn the LLM also decides where to step. The reply is
-- grammar-constrained to "MOVE=<dir> SAY=<line>", dir in {N,S,E,W,X}. The
-- script turns the chosen direction into a new floor target and the cube
-- glides there over the next frames. So the model literally drives the
-- cubes around the plaza while it talks.
--
-- PROXIMITY / EMOTION: every frame each cube measures the planar distance to
-- the other. That distance feeds a 0..1 "tension" value that (1) warps the
-- LLM's mood — close = warm/tender, far = tense/uneasy — so the model picks
-- where to step accordingly, (2) tints the cubes (warm hue when close, cold
-- steel-blue when far), and (3) drives audio (throb/dread rise with distance;
-- a one-shot fires when they cross into the near or far zone).

local MODEL = "models/Meta-Llama-3-8B-Instruct.Q4_K_M.gguf"

local TURN_GAP   = 1.5        -- seconds of pause between turns (readability)
local STEP       = 1.2        -- floor units moved per accepted MOVE
local BOUND      = 5.0        -- keep cubes within +/- this on X and Z
local MOVE_SPEED = 3.0        -- glide rate toward the target (1/sec)
local NEAR       = 1.6        -- <= this apart: intimate / warm
local FAR        = 4.0        -- >= this apart: tense / uneasy
local CONTEXT    = 16384      -- LLM context window (changing this hot-reloads the model)

-- GBNF grammar: forces the model to emit a parseable move + line.
-- dir N=-Z S=+Z E=+X W=-X X=stay.
local GRAMMAR = [[
root ::= "MOVE=" dir " SAY=" say
dir  ::= "N" | "S" | "E" | "W" | "X"
say  ::= char+
char ::= [^\r\n]
]]

-- Persona per entity Name. `color` is the cube's "speaking" color.
local PERSONAS = {
    spinner = {
        name  = "Pip",
        color = { 0.95, 0.55, 0.35 },
        system = "You are Pip, a small cheerful cube in a tiny 3D world. "
              .. "You are talking to Bo, the cube next to you. Keep replies to "
              .. "ONE short, warm, curious sentence. No stage directions, no "
              .. "quotation marks, just your spoken line.",
    },
    spinner2 = {
        name  = "Bo",
        color = { 0.40, 0.70, 0.95 },
        system = "You are Bo, a small dry, deadpan cube in a tiny 3D world. "
              .. "You are talking to Pip, the cube next to you. Keep replies to "
              .. "ONE short, wry sentence. No stage directions, no quotation "
              .. "marks, just your spoken line.",
    },
}

-- ---- shared conversation state -------------------------------------------

local function chat()
    if not _G.cube_chat then
        _G.cube_chat = {
            history    = {},     -- { {speaker="Pip", text="..."}, ... }
            turn       = nil,    -- entity Name ("spinner"/"spinner2") to speak next
            pending    = nil,    -- { owner=<name>, req=<id> }
            ai_started = false,  -- model load requested once
            cooldown   = 0.0,    -- pause timer between turns
        }
    end
    return _G.cube_chat
end

local function other(key)
    if key == "spinner" then return "spinner2" else return "spinner" end
end

-- trim + strip a leading "Name:" prefix the model sometimes adds
local function clean(s)
    s = (s or ""):gsub("^%s+", ""):gsub("%s+$", "")
    s = s:gsub("^%a[%w]*:%s*", "")
    return s
end

-- 0 when hugging-close, 1 when far apart
local function tension_of(dist)
    if not dist then return 0.0 end
    local t01 = (dist - NEAR) / (FAR - NEAR)
    if t01 < 0 then return 0.0 elseif t01 > 1 then return 1.0 end
    return t01
end

local function zone_of(dist)
    if not dist then return "mid" end
    if dist <= NEAR then return "near"
    elseif dist >= FAR then return "far"
    else return "mid" end
end

local MOOD_TEXT = {
    near = "\n\nYou are now VERY CLOSE to the other cube — almost touching. There "
        .. "is warmth and intimacy between you; speak tenderly and want to stay near.",
    mid  = "",
    far  = "\n\nYou have drifted FAR across the empty plaza from the other cube. The "
        .. "distance breeds tension and unease; let that strain bleed into your words.",
}

local MOVE_RULES =
    "\n\nYou stand on a small square plaza and can step around it. Respond in "
    .. "EXACTLY this format: MOVE=<dir> SAY=<line>. <dir> is one of N S E W X "
    .. "(N/S/E/W step one tile, X stays put). Step toward the other cube to get "
    .. "closer, away to back off, or X to hold. <line> is your single short "
    .. "spoken sentence, no name prefix."

local function build_user(c)
    if #c.history == 0 then
        return "Open the conversation with a short friendly hello to the other cube."
            .. MOVE_RULES
    end
    local lines = {}
    for _, m in ipairs(c.history) do
        lines[#lines + 1] = m.speaker .. ": " .. m.text
    end
    return "Conversation so far:\n" .. table.concat(lines, "\n")
        .. MOVE_RULES .. (MOOD_TEXT[zone_of(c.dist)] or "")
end

-- pull "MOVE=<dir> SAY=<line>" out of a reply; tolerant of a missing wrapper
local function parse_reply(text)
    local dir = text:match("MOVE=(%a+)")
    local say = text:match("SAY=(.*)")
    say = clean(say or text)
    return dir, say
end

local function clamp(v)
    if v < -BOUND then return -BOUND elseif v > BOUND then return BOUND end
    return v
end

-- ---- behavior ------------------------------------------------------------

local M = {}

-- per-instance state (file-scope locals are per environment / per entity)
local baseY = nil           -- resting height so the speaking bob returns home
local tgtX, tgtZ = nil, nil -- floor target the cube glides toward

function M.on_start(self)
    local key = self.name
    local me = PERSONAS[key]
    if not me then
        lm.log("cube_chat: no persona for entity '" .. tostring(key) .. "'")
        return
    end

    -- remember resting pose; seed the move target at the current spot
    local t = self:get_transform()
    baseY = t.position.y
    tgtX, tgtZ = t.position.x, t.position.z

    local c = chat()
    if not c.turn and not c.pending then
        c.turn = "spinner"   -- Pip opens
    end

    if not lm.ai then
        lm.log("cube_chat: built WITHOUT inference (lm.ai is nil) — cubes can't talk")
    else
        lm.log("cube_chat: " .. me.name .. " is awake and listening for the other cube")
    end
end

function M.on_update(self, dt)
    local key = self.name
    local me = PERSONAS[key]
    if not me then return end

    local t = self:get_transform()
    local mr = self:get_mesh_renderer()

    -- gentle idle spin about the vertical axis (horizontal rotation only)
    t.rotation.x = 0.0
    t.rotation.z = 0.0
    t.rotation.y = t.rotation.y + 40.0 * dt

    -- glide toward the LLM-chosen floor target
    if tgtX then
        local k = math.min(1.0, dt * MOVE_SPEED)
        t.position.x = t.position.x + (tgtX - t.position.x) * k
        t.position.z = t.position.z + (tgtZ - t.position.z) * k
    end

    if not lm.ai then return end
    local c = chat()

    -- proximity awareness: planar distance to the other cube, shared so the
    -- prompt builder and both cubes can read it
    local you = lm.scene.find(other(key))
    if you and you:valid() then
        local yt = you:get_transform()
        local dx = t.position.x - yt.position.x
        local dz = t.position.z - yt.position.z
        c.dist = math.sqrt(dx * dx + dz * dz)
    end

    -- one cube drives audio + zone-crossing one-shots (avoid double-firing)
    if key == "spinner" and c.dist then
        local tn = tension_of(c.dist)
        lm.audio.set("dread", tn * 0.8)
        lm.audio.set("throb_level", tn)
        local z = zone_of(c.dist)
        if z ~= c.zone then
            if z == "near" then
                lm.log("cube_chat: \u{2665} Pip and Bo huddle close")
                lm.audio.event("mumble")
            elseif z == "far" then
                lm.log("cube_chat: ... tension rises as they drift apart")
                lm.audio.event("door_creak")
            end
            c.zone = z
        end
    end

    -- load the model once (any instance may kick it off)
    local status = lm.ai.status()
    if status == "stopped" and not c.ai_started then
        c.ai_started = true
        lm.ai.start{ model = MODEL, context = CONTEXT, threads = 6 }
        c.loaded_ctx = CONTEXT
        return
    end
    -- hot-apply a CONTEXT change: unload so it reloads at the new size
    if status == "ready" and c.loaded_ctx ~= CONTEXT then
        lm.log("cube_chat: reloading model at context = " .. CONTEXT)
        lm.ai.stop()
        c.ai_started = false
        return
    end
    if status == "failed" then
        if not c.failed_logged then
            lm.log("cube_chat: model load FAILED — drop a real chat .gguf at " .. MODEL)
            c.failed_logged = true
        end
        return
    end

    -- am I the active speaker (about to talk, or mid-generation)?
    local active = (c.pending and c.pending.owner == key)
                or (c.turn == key and not c.pending and status == "ready")

    -- visual: hue warps with proximity (warm persona hue when close, cold
    -- steel-blue when far), brightness tracks who is speaking
    if mr then
        local tn = tension_of(c.dist)
        local cr, cg, cb = 0.20, 0.28, 0.45              -- tense "far" color
        local r = me.color[1] + (cr - me.color[1]) * tn
        local g = me.color[2] + (cg - me.color[2]) * tn
        local b = me.color[3] + (cb - me.color[3]) * tn
        local bright = (active and 1.0 or 0.4) + (1.0 - tn) * 0.15
        mr:set_color(r * bright, g * bright, b * bright)
    end
    if baseY then
        local lift = active and (0.12 * (1.0 + math.sin(lm.time.now() * 6.0))) or 0.0
        t.position.y = baseY + lift
    end

    if status ~= "ready" then return end

    if c.cooldown > 0 then
        c.cooldown = c.cooldown - dt
    end

    -- drive a pending request to completion (only its owner polls it)
    if c.pending then
        if c.pending.owner == key then
            local r = lm.ai.poll(c.pending.req)
            if r.complete then
                local dir, say = parse_reply(r.text or "")
                c.history[#c.history + 1] = { speaker = me.name, text = say }

                -- apply the move this cube decided on
                if dir == "N" then tgtZ = clamp(tgtZ - STEP)
                elseif dir == "S" then tgtZ = clamp(tgtZ + STEP)
                elseif dir == "E" then tgtX = clamp(tgtX + STEP)
                elseif dir == "W" then tgtX = clamp(tgtX - STEP) end

                lm.log(me.name .. " [" .. (dir or "?") .. "]: " .. say)
                lm.ai.forget(c.pending.req)
                c.pending = nil
                c.turn = other(key)
                c.cooldown = TURN_GAP
            elseif r.failed then
                lm.log("cube_chat: request failed: " .. tostring(r.error))
                lm.ai.forget(c.pending.req)
                c.pending = nil
                c.cooldown = 1.0
            end
        end
        return
    end

    -- my turn: submit one line
    if c.turn == key and c.cooldown <= 0 and not lm.ai.busy() then
        -- no cap: full history grows every turn and is fed back in, so the
        -- conversation drifts and warps as it fills the 8k context window
        local req = lm.ai.submit{
            system      = me.system,
            user        = build_user(c),
            grammar     = GRAMMAR,
            max_tokens  = 64,
            temperature = 1.7,
            top_p       = 1.0,
            seed        = #c.history + 1,
        }
        c.pending = { owner = key, req = req }
    end
end

return M
