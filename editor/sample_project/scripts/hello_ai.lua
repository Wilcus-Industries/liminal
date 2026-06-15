-- hello_ai.lua — use lm.ai (local LLM) to generate "hello world", print to console.
--
-- NOTE: there is no on-screen text/UI API in the lm surface. "On screen" here
-- means the engine console (lm.log). Drop a real chat .gguf at the path below
-- (the project only ships vocab-only tokenizer stubs, which cannot generate).

local MODEL = "models/Meta-Llama-3-8B-Instruct.Q4_K_M.gguf"  -- <-- point this at a real chat .gguf (resolved through Assets)

local M = {}

local req = nil       -- pending request id
local done = false    -- finished?
local waited = 0.0    -- seconds spent waiting for "ready"

function M.on_start(self)
    done, req, waited = false, nil, 0.0
    lm.log("hello_ai: on_start")

    if not lm.ai then
        lm.log("hello_ai: built WITHOUT inference (lm.ai is nil)")
        done = true
        return
    end

    local status, message = lm.ai.status()
    lm.log("hello_ai: ai status = " .. tostring(status) .. " (" .. tostring(message) .. ")")
    if status ~= "ready" and status ~= "loading" then
        lm.ai.start{ model = MODEL, context = 2048, threads = 4 }
        lm.log("hello_ai: requested model load: " .. MODEL)
    end
end

function M.on_update(self, dt)
    if done then return end

    local status, message = lm.ai.status()

    if status == "failed" then
        lm.log("hello_ai: model load FAILED: " .. tostring(message))
        done = true
        return
    end

    if status ~= "ready" then
        -- "stopped"/"loading": don't spin silently forever
        waited = waited + dt
        if waited > 10.0 then
            lm.log("hello_ai: gave up after 10s, status still '" .. tostring(status) ..
                   "' — is MODEL a real .gguf with weights?")
            done = true
        end
        return
    end

    -- ready: submit once, then poll
    if not req then
        req = lm.ai.submit{
            system = "You greet the world. Reply with exactly: Hello, world!",
            user   = "Say hello.",
            max_tokens = 16,
            temperature = 0.0,
        }
        lm.log("hello_ai: submitted request " .. tostring(req))
        return
    end

    local r = lm.ai.poll(req)
    if r.complete then
        lm.log("LLM says: " .. (r.text or ""))
        lm.ai.forget(req)  -- required: finished responses leak until forgotten
        req, done = nil, true
    elseif r.failed then
        lm.log("hello_ai: request failed: " .. tostring(r.error))
        lm.ai.forget(req)
        req, done = nil, true
    end
end

return M

