#pragma once

#define CHITTA_VERSION "5.62.3"
#define CHITTA_PROTOCOL_VERSION_MAJOR 1
#define CHITTA_PROTOCOL_VERSION_MINOR 0

namespace chitta {
namespace version {

inline bool protocol_compatible(int major, int minor) {
    // Major version must match exactly (breaking changes)
    // Minor version: daemon must be >= client (backward compatible additions)
    return major == CHITTA_PROTOCOL_VERSION_MAJOR &&
           minor >= CHITTA_PROTOCOL_VERSION_MINOR;
}

} // namespace version
} // namespace chitta
