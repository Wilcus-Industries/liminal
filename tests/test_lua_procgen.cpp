// Determinism test for the lm.procgen Lua bindings. Builds a windowless
// ScriptHost (ScriptContext{} defaults — no window/audio/GL), runs a Lua
// string that calls lm.procgen.town{seed=...} with a small n, and hashes the
// result (grid tiles + per-piece vertex counts + repairs/restarts). Runs the
// SAME Lua twice and asserts the two hashes match, then checks a recorded
// golden so a determinism regression fails loudly.
//
// Pure CPU: the town pipeline produces MeshData (CPU vertex vectors) without
// touching the GPU, so no GL context is needed.
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <liminal/script/script_host.hpp>

namespace {

// Recorded golden for town{seed=1234, n=20}. Re-record on intentional
// generation changes by running with LIMINAL_RECORD_GOLDEN=1. (0 = unset →
// golden check skipped, run-to-run equality still enforced.)
constexpr std::uint64_t kGolden = 43426674ull;

// The Lua program: generate a town, fold a stable hash over the grid tiles and
// every piece's vertex count, plus repairs/restarts, and return it as a number.
// FNV-1a folded in Lua over integers (kept inside 53-bit double range by
// masking to 32 bits each step).
const char* kLua = R"LUA(
local t = lm.procgen.town{ seed = 1234, n = 20 }
local h = 2166136261
local function mix(x)
  h = (h ~ (x & 0xFFFFFFFF)) & 0xFFFFFFFF
  -- multiply by FNV prime 16777619, keep low 32 bits
  h = (h * 16777619) & 0xFFFFFFFF
end
local g = t.grid
local n = g.n
for z = 0, n - 1 do
  for x = 0, n - 1 do
    mix(g:at(x, z))
  end
end
mix(t.repairs)
mix(t.restarts)
mix(#t.pieces)
for i = 1, #t.pieces do
  mix(t.pieces[i]:vertex_count())
end
return h
)LUA";

std::uint64_t runOnce() {
    liminal::ScriptHost host; // ScriptContext{} defaults: windowless
    sol::state& lua = host.lua();
    sol::protected_function_result r = lua.safe_script(kLua, sol::script_pass_on_error);
    if (!r.valid()) {
        sol::error e = r;
        std::fprintf(stderr, "FAIL: lua error: %s\n", e.what());
        std::exit(1);
    }
    return std::uint64_t(r.get<double>());
}

} // namespace

int main() {
    const std::uint64_t a = runOnce();
    const std::uint64_t b = runOnce();

    std::printf("lm.procgen.town hash (run 1): %llu\n",
                static_cast<unsigned long long>(a));
    std::printf("lm.procgen.town hash (run 2): %llu\n",
                static_cast<unsigned long long>(b));

    if (a != b) {
        std::fprintf(stderr,
                     "FAIL: two runs disagree (%llu != %llu) — lm.procgen.town "
                     "is non-deterministic\n",
                     static_cast<unsigned long long>(a),
                     static_cast<unsigned long long>(b));
        return 1;
    }

    if (std::getenv("LIMINAL_RECORD_GOLDEN")) {
        std::printf("record this as kGolden: 0x%llxull\n",
                    static_cast<unsigned long long>(a));
        return 0;
    }

    if (kGolden != 0ull && a != kGolden) {
        std::fprintf(stderr,
                     "FAIL: hash %llu != golden %llu — generation changed\n",
                     static_cast<unsigned long long>(a),
                     static_cast<unsigned long long>(kGolden));
        return 1;
    }

    std::printf("OK\n");
    return 0;
}
