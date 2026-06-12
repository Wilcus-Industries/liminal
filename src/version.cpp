// version.cpp — the engine version string, from the configured header.
#include <liminal/liminal.hpp>

namespace liminal {

const char* version() noexcept {
    return LIMINAL_VERSION_STRING;
}

} // namespace liminal
