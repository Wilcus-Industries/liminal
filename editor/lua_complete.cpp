// Completion engine implementation. The API model is hand-derived from
// src/script/lua_bindings.cpp (lm.*, Entity/Transform/MeshRenderer/vec) plus
// the Lua 5.4 stdlib subset scripts realistically reach for. Kept as flat
// static tables so lookups are pure prefix scans — no parser, no Lua state.

#include "lua_complete.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace liminal::editor {

namespace {

// --- the static API model ----------------------------------------------------

struct Api {
    const char* label;
    const char* detail;
};

const std::array<const char*, 22> kKeywords = {
    "and",  "break", "do",     "else",   "elseif", "end",    "false", "for",
    "function", "goto", "if",   "in",     "local",  "nil",    "not",   "or",
    "repeat", "return", "then", "true",   "until",  "while"};

// Global functions + table names always in scope.
const std::array<Api, 16> kGlobals = {{
    {"print", "(...)"},
    {"tostring", "(v)"},
    {"tonumber", "(v [, base])"},
    {"type", "(v)"},
    {"pcall", "(f, ...)"},
    {"error", "(msg [, level])"},
    {"assert", "(v [, msg])"},
    {"select", "(n, ...)"},
    {"ipairs", "(t)"},
    {"pairs", "(t)"},
    {"next", "(t [, k])"},
    {"rawget", "(t, k)"},
    {"rawset", "(t, k, v)"},
    {"setmetatable", "(t, mt)"},
    {"getmetatable", "(t)"},
    {"require", "(modname)"},
}};

// Standard library tables (bare names that, after `.`, expand to members).
const std::array<Api, 5> kStdTables = {{
    {"string", "stdlib"},
    {"table", "stdlib"},
    {"math", "stdlib"},
    {"os", "stdlib"},
    {"io", "stdlib"},
}};

const std::array<Api, 11> kStringMembers = {{
    {"format", "(fmt, ...)"}, {"find", "(s, pat)"},   {"gsub", "(s, pat, repl)"},
    {"match", "(s, pat)"},    {"gmatch", "(s, pat)"}, {"sub", "(s, i [, j])"},
    {"len", "(s)"},           {"rep", "(s, n)"},      {"upper", "(s)"},
    {"lower", "(s)"},         {"byte", "(s [, i])"},
}};

const std::array<Api, 6> kTableMembers = {{
    {"insert", "(t, [pos,] v)"}, {"remove", "(t [, pos])"}, {"concat", "(t [, sep])"},
    {"sort", "(t [, cmp])"},     {"unpack", "(t)"},         {"pack", "(...)"},
}};

const std::array<Api, 16> kMathMembers = {{
    {"floor", "(x)"},   {"ceil", "(x)"},   {"abs", "(x)"},    {"sqrt", "(x)"},
    {"sin", "(x)"},     {"cos", "(x)"},    {"tan", "(x)"},    {"atan", "(y [, x])"},
    {"min", "(...)"},   {"max", "(...)"},  {"random", "([m [, n]])"},
    {"randomseed", "([x])"}, {"huge", "number"}, {"pi", "number"},
    {"fmod", "(x, y)"}, {"modf", "(x)"},
}};

const std::array<Api, 4> kOsMembers = {{
    {"time", "([t])"}, {"clock", "()"}, {"date", "([fmt])"}, {"getenv", "(name)"},
}};

const std::array<Api, 8> kIoMembers = {{
    {"write", "(...)"},      {"read", "([fmt])"}, {"open", "(name [, mode])"},
    {"lines", "([name])"},   {"close", "([f])"},  {"stdout", "file"},
    {"stderr", "file"},      {"stdin", "file"},
}};

// --- engine API (from lua_bindings.cpp) --------------------------------------

const std::array<Api, 9> kLmMembers = {{
    {"log", "(msg)"},
    {"vec3", "([x [, y, z]])"},
    {"vec4", "([x [, y, z, w]])"},
    {"scene", "table"},
    {"input", "table"},
    {"audio", "table"},
    {"assets", "table"},
    {"ai", "table (inference builds)"},
    {"procgen", "table"},
    // time appended below to keep the array literal tidy; see kLmExtra.
}};
const std::array<Api, 1> kLmExtra = {{{"time", "table"}}};

const std::array<Api, 6> kLmSceneMembers = {{
    {"find", "(name) -> Entity?"},
    {"create", "(name) -> Entity"},
    {"find_all", "(name) -> Entity[]"},
    {"each", "(fn)"},
    {"destroy", "(entity)"},
    {"change", "(path)"},
}};

const std::array<Api, 4> kLmAudioMembers = {{
    {"set", "(name, value)"},
    {"get", "(name) -> num|bool"},
    {"event", "(name)"},
    {"ok", "() -> bool"},
}};

const std::array<Api, 1> kLmAssetsMembers = {{
    {"add_mesh", "(name, data) -> string"},
}};

// lm.procgen — the full procgen toolkit (chunk 4). Every entry point takes an
// explicit seed/Rng; town() is a deterministic one-shot mirroring 04_wfc_town.
const std::array<Api, 14> kLmProcgenMembers = {{
    {"rng", "(seed) -> Rng"},
    {"tileset", "([json_path]) -> TileSet"},
    {"terrain", "{seed=,kind=,water=,n=,tile_size=} -> HeightField"},
    {"terrain_mask", "(hf, params, ts) -> masks"},
    {"stamp_footprint", "(masks, ts, n, cx, cz, w, d) -> FootprintPlan?"},
    {"solve_wfc", "{tileset=,masks=,n=,seed=,max_restarts=} -> WfcResult"},
    {"grid_from", "(result, n, tile_size) -> TileGrid"},
    {"validate", "(grid, hf, sites, plans, ts) -> {repairs=,...}"},
    {"build_building", "(grid, hf, plan, family, rng) -> BuiltPiece"},
    {"collect_runs", "(grid, ts) -> DeckRun[]"},
    {"build_deck", "(grid, hf, ts, run) -> BuiltPiece"},
    {"terrain_mesh", "(hf) -> MeshData"},
    {"water_mesh", "(hf) -> MeshData"},
    {"town", "{seed=,n=,...} -> {grid=,terrain=,pieces=,repairs=,restarts=}"},
}};

// lm.ai — local LLM inference. Completion lists these regardless of build
// flags (detail flags that they only do anything in inference builds).
const std::array<Api, 9> kLmAiMembers = {{
    {"start", "{model=...} (inference builds)"},
    {"stop", "() (inference builds)"},
    {"status", "() -> status, msg (inference builds)"},
    {"submit", "{system=, user=, ...} -> id (inference builds)"},
    {"poll", "(id) -> {text, complete, ...} (inference builds)"},
    {"cancel", "(id) (inference builds)"},
    {"forget", "(id) (inference builds)"},
    {"busy", "() -> bool (inference builds)"},
    {"queue_depth", "() -> integer (inference builds)"},
}};

const std::array<Api, 1> kLmInputMembers = {{
    {"key_down", "(key) -> bool"},
}};

const std::array<Api, 1> kLmTimeMembers = {{
    {"now", "() -> number"},
}};

// Entity methods — surfaced after a `:` on any identifier (heuristic).
const std::array<Api, 6> kEntityMembers = {{
    {"name", "string"},
    {"valid", "() -> bool"},
    {"destroy", "()"},
    {"get_transform", "() -> Transform"},
    {"get_mesh_renderer", "() -> MeshRenderer?"},
    {"get_component", "(name) -> component?"},
}};

// Field-ish members surfaced after a `.` on an unknown identifier: union of
// Transform / MeshRenderer / vec fields (we can't tell which type it is).
const std::array<Api, 14> kFieldMembers = {{
    // Transform
    {"position", "vec3"}, {"rotation", "vec3 (deg)"}, {"scale", "vec3"},
    {"set_position", "(x, y, z)"}, {"set_rotation", "(x, y, z)"},
    {"set_scale", "(x, y, z)"},
    // MeshRenderer
    {"mesh", "string"}, {"texture", "string"}, {"color", "vec4"},
    {"set_color", "(r, g, b [, a])"},
    // vec
    {"x", "number"}, {"y", "number"}, {"z", "number"}, {"w", "number"},
}};
const std::array<Api, 1> kVecLength = {{{"length", "() -> number"}}};

// Script entry points + the implicit global — offered on a bare prefix.
const std::array<Api, 3> kScriptGlobals = {{
    {"entity", "Entity (implicit)"},
    {"on_start", "(entity)"},
    {"on_update", "(entity, dt)"},
}};

// --- helpers -----------------------------------------------------------------

bool isIdentChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

char lower(char c) { return char(std::tolower(static_cast<unsigned char>(c))); }

bool startsWithCI(const std::string& s, const std::string& prefix) {
    if (prefix.size() > s.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i)
        if (lower(s[i]) != lower(prefix[i])) return false;
    return true;
}

void push(std::vector<CompletionItem>& out, const std::string& prefix,
          const char* label, const char* detail, int rank,
          const char* insertOverride = nullptr) {
    if (!startsWithCI(label, prefix)) return;
    CompletionItem it;
    it.label = label;
    it.detail = detail ? detail : "";
    it.insert = insertOverride ? insertOverride : label;
    it.rank = rank;
    out.push_back(std::move(it));
}

template <size_t N>
void pushAll(std::vector<CompletionItem>& out, const std::string& prefix,
             const std::array<Api, N>& arr, int rank) {
    for (const auto& a : arr) push(out, prefix, a.label, a.detail, rank);
}

// Last contiguous identifier-or-dot-or-colon chain ending at the line's end,
// e.g. "  foo = lm.scene.fi" -> "lm.scene.fi". Returns the chain text.
std::string trailingChain(const std::string& line) {
    size_t i = line.size();
    while (i > 0) {
        char c = line[i - 1];
        if (isIdentChar(c) || c == '.' || c == ':') --i;
        else break;
    }
    return line.substr(i);
}

// Harvest identifiers from the buffer for the bare-prefix path. Dedup, skip
// the word being typed, deterministic (insertion order via seen-set).
void harvestBufferWords(const std::string& buffer, const std::string& prefix,
                        const std::string& typing,
                        std::vector<CompletionItem>& out) {
    std::vector<std::string> seen;
    std::string word;
    auto flush = [&]() {
        if (word.empty()) return;
        // Lua identifiers don't start with a digit.
        if (!std::isdigit(static_cast<unsigned char>(word[0])) &&
            word != typing && startsWithCI(word, prefix)) {
            if (std::find(seen.begin(), seen.end(), word) == seen.end()) {
                seen.push_back(word);
                CompletionItem it;
                it.label = word;
                it.insert = word;
                it.rank = 2;
                out.push_back(std::move(it));
            }
        }
        word.clear();
    };
    for (char c : buffer) {
        if (isIdentChar(c)) word.push_back(c);
        else flush();
    }
    flush();
}

} // namespace

std::vector<CompletionItem> computeCompletions(const std::string& lineText,
                                               const std::string& buffer,
                                               std::string& prefixOut,
                                               int maxItems) {
    std::vector<CompletionItem> items;
    const std::string chain = trailingChain(lineText);

    // Split the chain at its last separator. `base` is everything up to and
    // including the separator; `prefix` is the partial word after it.
    size_t sep = chain.find_last_of(".:");
    std::string base, prefix;
    char sepChar = '\0';
    if (sep == std::string::npos) {
        prefix = chain;
    } else {
        base = chain.substr(0, sep);
        prefix = chain.substr(sep + 1);
        sepChar = chain[sep];
    }
    prefixOut = prefix;

    if (sepChar == ':') {
        // Method call on any identifier -> Entity methods (heuristic).
        pushAll(items, prefix, kEntityMembers, 0);
    } else if (sepChar == '.') {
        if (base == "lm") {
            pushAll(items, prefix, kLmMembers, 0);
            pushAll(items, prefix, kLmExtra, 0);
        } else if (base == "lm.scene") {
            pushAll(items, prefix, kLmSceneMembers, 0);
        } else if (base == "lm.audio") {
            pushAll(items, prefix, kLmAudioMembers, 0);
        } else if (base == "lm.assets") {
            pushAll(items, prefix, kLmAssetsMembers, 0);
        } else if (base == "lm.procgen") {
            pushAll(items, prefix, kLmProcgenMembers, 0);
        } else if (base == "lm.ai") {
            pushAll(items, prefix, kLmAiMembers, 0);
        } else if (base == "lm.input") {
            pushAll(items, prefix, kLmInputMembers, 0);
        } else if (base == "lm.time") {
            pushAll(items, prefix, kLmTimeMembers, 0);
        } else if (base == "string") {
            pushAll(items, prefix, kStringMembers, 0);
        } else if (base == "table") {
            pushAll(items, prefix, kTableMembers, 0);
        } else if (base == "math") {
            pushAll(items, prefix, kMathMembers, 0);
        } else if (base == "os") {
            pushAll(items, prefix, kOsMembers, 0);
        } else if (base == "io") {
            pushAll(items, prefix, kIoMembers, 0);
        } else {
            // Unknown identifier: offer the field-ish union (Transform /
            // MeshRenderer / vec). No type inference, so this is a best guess.
            pushAll(items, prefix, kFieldMembers, 0);
            pushAll(items, prefix, kVecLength, 0);
        }
    } else {
        // Bare prefix: engine globals first, then keywords + stdlib globals,
        // then buffer words.
        pushAll(items, prefix, kScriptGlobals, 0);
        push(items, prefix, "lm", "engine API", 0);
        for (const char* kw : kKeywords) push(items, prefix, kw, nullptr, 1);
        pushAll(items, prefix, kGlobals, 1);
        pushAll(items, prefix, kStdTables, 1);
        harvestBufferWords(buffer, prefix, prefix, items);
    }

    // Sort: bucket (rank) first, then exact-prefix (case-sensitive) before
    // case-insensitive matches, then alphabetical. Stable for determinism.
    std::stable_sort(items.begin(), items.end(),
                     [&](const CompletionItem& a, const CompletionItem& b) {
                         if (a.rank != b.rank) return a.rank < b.rank;
                         const bool ae =
                             a.label.compare(0, prefix.size(), prefix) == 0;
                         const bool be =
                             b.label.compare(0, prefix.size(), prefix) == 0;
                         if (ae != be) return ae;
                         return a.label < b.label;
                     });

    // Dedup by label (the field-ish union can collide across types) and cap.
    std::vector<CompletionItem> result;
    for (auto& it : items) {
        bool dup = false;
        for (const auto& r : result)
            if (r.label == it.label) { dup = true; break; }
        if (dup) continue;
        result.push_back(std::move(it));
        if (int(result.size()) >= maxItems) break;
    }
    return result;
}

} // namespace liminal::editor
