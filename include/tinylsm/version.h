#pragma once

namespace tinylsm {

// Library version string (semver-style major.minor.patch).
// The returned pointer has static storage duration and is valid for the process lifetime.
const char* Version() noexcept;

}  // namespace tinylsm
