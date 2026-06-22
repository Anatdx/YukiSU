#include "../assets.hpp"
#include "restorecon.hpp"

#include <sys/stat.h>
#include <filesystem>
#include <string>

#include "../defs.hpp"
#include "../log.hpp"
#include "../utils.hpp"

namespace ksud {

// ensure_binaries(), get_asset(), copy_asset_to_file() etc. are implemented in
// generated/assets_data.cpp (scripts/embed_assets.py at build time). This file
// holds hand-written asset helpers.

// Stage the YukiZygisk payload into /data/adb/ksu/lib/yukizygisk/. The kernel
// later reads libzloader.so from there (as ksu_cred) and hands it to the zygote
// through a memfd, so these files only need to be writable by ksud and readable
// by the privileged kernel path -- the zygote never opens them. Labeled
// adb_data_file like the rest of /data/adb/ksu. No-op for builds that don't
// embed the payload.
int ensure_yukizygisk(bool ignore_if_exist) {
    struct Payload {
        const char* asset;
        const char* dest;
    };
    static const Payload payload[] = {
        {"libzloader.so", ZLOADER_PATH},
        {"libzygisk.so", ZCORE_PATH},
    };

    // Only touch the dir if at least one lib is actually embedded in this ksud.
    bool embedded = false;
    for (const auto& p : payload) {
        const uint8_t* data = nullptr;
        size_t size = 0;
        if (get_asset(p.asset, data, size)) {
            embedded = true;
            break;
        }
    }
    if (!embedded) {
        return 0;
    }

    // copy_asset_to_file() won't create parent dirs, so do it first.
    if (!ensure_dir_exists(YUKIZYGISK_DIR)) {
        LOGE("yukizygisk: failed to create %s", YUKIZYGISK_DIR);
        return 1;
    }

    for (const auto& p : payload) {
        const uint8_t* data = nullptr;
        size_t size = 0;
        if (!get_asset(p.asset, data, size)) {
            continue;
        }

        // Unconditionally overwrite: always re-stage the embedded lib so a
        // stale copy from an older build is never left in place (no size check,
        // no skip-if-exists). copy_asset_to_file removes the dest first.
        (void)ignore_if_exist;
        if (!copy_asset_to_file(p.asset, p.dest)) {
            LOGE("yukizygisk: failed to stage %s", p.dest);
            continue;
        }
        chmod(p.dest, 0644);
        lsetfilecon(p.dest, ADB_CON);
        LOGI("yukizygisk: staged %s", p.dest);
    }
    return 0;
}

}  // namespace ksud
