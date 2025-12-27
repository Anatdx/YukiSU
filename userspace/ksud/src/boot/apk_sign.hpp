#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace ksud {

// APK signature functions
std::pair<uint32_t, std::string> get_apk_signature(const std::string& apk_path);

}  // namespace ksud
