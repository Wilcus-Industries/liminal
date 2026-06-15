#pragma once
// In-process Lua completion engine — a tiny "built-in LSP" with no external
// process and no type inference. It serves three things from static tables:
// Lua keywords, the Lua stdlib surface scripts actually touch, and the liminal
// engine API (lm.*, Entity/Transform/MeshRenderer/vec members, script entry
// points). Context is a heuristic on the token chain immediately before the
// cursor — `lm.` lists lm members, `:` lists Entity methods, a bare prefix
// mixes keywords + globals + words harvested from the current buffer.
//
// Everything is deterministic and allocation-light: the static API lives in
// function-local statics, the per-keystroke work is a prefix scan over those
// plus a single word-walk of the buffer.

#include <string>
#include <vector>

namespace liminal::editor {

// One row in the popup. `detail` is the dimmed signature/type hint (may be
// empty); `insert` is what actually gets typed (usually == label).
struct CompletionItem {
    std::string label;
    std::string detail;
    std::string insert;
    int rank = 0; // sort bucket: 0 engine API, 1 keyword/stdlib, 2 buffer word
};

// Produce up to `maxItems` completions for the cursor sitting at the end of
// `lineText` (the full text of the current line, left of the cursor is all
// that matters). `buffer` is the whole document, scanned for identifiers.
// `prefixOut` receives the partial word being completed (the bit already
// typed that a chosen item must replace by inserting only its remainder).
std::vector<CompletionItem> computeCompletions(const std::string& lineText,
                                               const std::string& buffer,
                                               std::string& prefixOut,
                                               int maxItems = 10);

} // namespace liminal::editor
