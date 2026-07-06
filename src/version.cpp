#include "tinylsm/version.h"

#include "tinylsm_version.h"

namespace tinylsm {

const char* Version() noexcept { return TINYLSM_VERSION_STRING; }

}  // namespace tinylsm
