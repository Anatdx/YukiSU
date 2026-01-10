/**
 * YukiZygisk - Logging wrapper for ksud integration
 *
 * Redirects YukiZygisk logging to ksud's log system
 */

#pragma once

#include <cerrno>
#include <cstring>
#include "../../log.hpp"

// Map YukiZygisk logging to ksud logging
// Note: ksud uses LOGI, LOGE, LOGW, LOGD already defined in log.hpp

namespace yuki::ptracer {}  // namespace yuki::ptracer
