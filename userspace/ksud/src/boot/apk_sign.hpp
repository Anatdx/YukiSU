#pragma once

#include <cstdint>
#include <string>

namespace ksud {

struct ApkSignatureInfo {
    struct V2Info {
        bool has = false;
        uint32_t size = 0;
        std::string hash;
    };

    bool valid = false;
    bool v1 = false;
    V2Info v2;
    bool v3 = false;
    bool v31 = false;
};

ApkSignatureInfo get_apk_signature(const std::string& apk_path);

}  // namespace ksud
