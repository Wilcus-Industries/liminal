// Placeholder translation unit so the static library has something to link.
// Real engine sources land in Phase 1.
#include <liminal/liminal.hpp>

namespace liminal {

const char* version() noexcept {
    return LIMINAL_VERSION_STRING;
}

} // namespace liminal
