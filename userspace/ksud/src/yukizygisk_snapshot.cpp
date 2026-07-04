#include "yukizygisk_snapshot.hpp"
#include "assets.hpp"
#include "core/ksucalls.hpp"
#include "core/restorecon.hpp"
#include "defs.hpp"
#include "log.hpp"
#include "userspace/zygisk/daemon/native_modules.hpp"
#include "utils.hpp"

#include "uapi/yukizygisk.h"

#include <elf.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <system_error>
#include <vector>

namespace ksud {

namespace {

namespace fs = std::filesystem;
using yukizygisk::native::NativeModule;

constexpr const char* kSnapshotDirName = "yukizygisk";
constexpr const char* kManifestName = "native_snapshot.bin";
constexpr const char* kModulesDirName = "modules";
constexpr const char* kLoaderName = "libyukilinker.so";
constexpr const char* kNativeCoreName = "libyukizncore.so";
constexpr const char* kSystemLinker64 = "/system/bin/linker64";
constexpr const char* kMetadataFileCon = "u:object_r:metadata_file:s0";

fs::path preinit_ksu_dir() {
    std::error_code ec;
    if (fs::is_directory("/metadata/watchdog", ec)) {
        return PREINIT_DIR_WATCHDOG;
    }
    return PREINIT_DIR_DEFAULT;
}

bool range_ok(size_t offset, size_t size, size_t file_size) {
    return offset <= file_size && size <= file_size - offset;
}

bool string_table_equals(const char* table, size_t table_size, uint32_t offset, const char* want) {
    if (offset >= table_size)
        return false;
    size_t want_len = strlen(want);
    if (want_len >= table_size - offset)
        return false;
    return memcmp(table + offset, want, want_len + 1) == 0;
}

bool safe_component(const std::string& s) {
    if (s.empty() || s.size() >= YZ_NATIVE_MODULE_ID_MAX)
        return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '_' || c == '-' || c == '.';
    });
}

void remove_snapshot_dir(const fs::path& base) {
    std::error_code ec;
    fs::remove_all(base / kSnapshotDirName, ec);
}

bool copy_regular_file(const fs::path& src, const fs::path& dst) {
    std::ifstream in(src, std::ios::binary);
    if (!in)
        return false;
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out << in.rdbuf();
    out.flush();
    return static_cast<bool>(out);
}

bool stage_asset_or_file(const char* asset, const fs::path& fallback, const fs::path& dst) {
    if (copy_asset_to_file(asset, dst.string())) {
        chmod(dst.c_str(), 0644);
        (void)lsetfilecon(dst, kMetadataFileCon);
        return true;
    }
    if (!copy_regular_file(fallback, dst))
        return false;
    chmod(dst.c_str(), 0644);
    (void)lsetfilecon(dst, kMetadataFileCon);
    return true;
}

uint64_t resolve_linker_sym(const char* path, const char* want) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;

    struct stat st{};
    if (fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(sizeof(Elf64_Ehdr))) {
        close(fd);
        return 0;
    }

    void* map = mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED)
        return 0;

    const auto* base = static_cast<const uint8_t*>(map);
    const auto* eh = reinterpret_cast<const Elf64_Ehdr*>(base);
    uint64_t result = 0;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) == 0 && eh->e_ident[EI_CLASS] == ELFCLASS64 &&
        eh->e_shoff > 0 && eh->e_shnum > 0) {
        size_t file_size = static_cast<size_t>(st.st_size);
        size_t sh_size = static_cast<size_t>(eh->e_shnum) * sizeof(Elf64_Shdr);
        if (eh->e_shentsize != sizeof(Elf64_Shdr) ||
            !range_ok(static_cast<size_t>(eh->e_shoff), sh_size, file_size)) {
            munmap(map, static_cast<size_t>(st.st_size));
            return 0;
        }

        const auto* sh = reinterpret_cast<const Elf64_Shdr*>(base + eh->e_shoff);
        for (int i = 0; i < eh->e_shnum && result == 0; i++) {
            if (sh[i].sh_type != SHT_DYNSYM || sh[i].sh_link >= eh->e_shnum)
                continue;
            if (!range_ok(static_cast<size_t>(sh[i].sh_offset), static_cast<size_t>(sh[i].sh_size),
                          file_size))
                continue;
            const Elf64_Shdr& str_sh = sh[sh[i].sh_link];
            if (!range_ok(static_cast<size_t>(str_sh.sh_offset),
                          static_cast<size_t>(str_sh.sh_size), file_size))
                continue;
            const auto* syms = reinterpret_cast<const Elf64_Sym*>(base + sh[i].sh_offset);
            const char* strs = reinterpret_cast<const char*>(base + str_sh.sh_offset);
            size_t n = sh[i].sh_size / sizeof(Elf64_Sym);
            for (size_t j = 0; j < n; j++) {
                if (string_table_equals(strs, static_cast<size_t>(str_sh.sh_size), syms[j].st_name,
                                        want)) {
                    result = syms[j].st_value;
                    break;
                }
            }
        }
    }
    munmap(map, static_cast<size_t>(st.st_size));
    return result;
}

uint64_t resolve_first(const char* const* cands, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        uint64_t off = resolve_linker_sym(kSystemLinker64, cands[i]);
        if (off != 0)
            return off;
    }
    return 0;
}

std::vector<std::pair<std::string, fs::path>> collect_active_module_roots() {
    std::map<std::string, fs::path> modules;
    std::map<std::string, bool> skipped_modules;
    std::error_code ec;

    for (const char* root : {MODULE_UPDATE_DIR, MODULE_DIR}) {
        if (!fs::is_directory(root, ec))
            continue;
        for (const auto& entry : fs::directory_iterator(root, ec)) {
            if (ec)
                break;
            if (!entry.is_directory(ec))
                continue;
            const std::string id = entry.path().filename().string();
            if (!safe_component(id))
                continue;
            if (fs::exists(entry.path() / DISABLE_FILE_NAME, ec) ||
                fs::exists(entry.path() / REMOVE_FILE_NAME, ec)) {
                modules.erase(id);
                skipped_modules[id] = true;
                continue;
            }
            if (skipped_modules.count(id) == 0 && modules.count(id) == 0)
                modules.emplace(id, entry.path());
        }
    }

    return {modules.begin(), modules.end()};
}

std::vector<NativeModule> scan_early_native_modules() {
    std::vector<NativeModule> out;

    for (const auto& [module_id, base] : collect_active_module_roots()) {
        std::ifstream f(base / "zn_modules.txt");
        if (!f)
            continue;

        std::string line;
        while (std::getline(f, line)) {
            NativeModule m{};
            if (!yukizygisk::native::parse_native_module_line(module_id, base.string(), line, &m))
                continue;
            if (m.has_companion)
                continue;
            if (m.lib_path.size() >= YZ_NATIVE_MODULE_PATH_MAX)
                continue;
            out.push_back(std::move(m));
            if (out.size() >= YZ_NATIVE_TARGET_MAX)
                return out;
        }
    }

    return out;
}

void fill_cstr(char* dst, size_t dst_size, const std::string& src) {
    if (dst_size == 0)
        return;
    snprintf(dst, dst_size, "%s", src.c_str());
}

bool stage_module_entry(const NativeModule& m, size_t idx, const fs::path& module_dir,
                        yz_early_native_entry* entry) {
    char file_name[128];
    snprintf(file_name, sizeof(file_name), "%03zu-%s.so", idx, m.module_id.c_str());
    fs::path dst = module_dir / file_name;
    if (!copy_regular_file(m.lib_path, dst)) {
        LOGW("yukizygisk early: failed to copy native module %s from %s", m.module_id.c_str(),
             m.lib_path.c_str());
        return false;
    }
    chmod(dst.c_str(), 0644);
    (void)lsetfilecon(dst, kMetadataFileCon);

    *entry = {};
    entry->target_type = m.target_type;
    fill_cstr(entry->module_id, sizeof(entry->module_id), m.module_id);
    fill_cstr(entry->target, sizeof(entry->target), m.target);
    fill_cstr(entry->lib_path, sizeof(entry->lib_path), dst.string());
    return true;
}

bool write_manifest(const fs::path& path, const yz_early_native_snapshot_header& header,
                    const std::vector<yz_early_native_entry>& entries) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    for (const auto& entry : entries)
        out.write(reinterpret_cast<const char*>(&entry), sizeof(entry));
    out.flush();
    return static_cast<bool>(out);
}

bool yukizygisk_enabled_for_next_boot() {
    if (is_safe_mode())
        return false;
    auto [value, supported] = get_feature(KSU_FEATURE_YUKIZYGISK);
    return supported && value != 0;
}

}  // namespace

void clear_yukizygisk_early_snapshot() {
    remove_snapshot_dir(PREINIT_DIR_WATCHDOG);
    remove_snapshot_dir(PREINIT_DIR_DEFAULT);
}

int refresh_yukizygisk_early_snapshot() {
    if (!yukizygisk_enabled_for_next_boot()) {
        clear_yukizygisk_early_snapshot();
        LOGI("yukizygisk early: snapshot cleared");
        return 0;
    }

    const fs::path preinit_dir = preinit_ksu_dir();
    const fs::path snapshot_dir = preinit_dir / kSnapshotDirName;
    const fs::path module_dir = snapshot_dir / kModulesDirName;
    const fs::path manifest_tmp = snapshot_dir / ".native_snapshot.tmp";
    const fs::path manifest = snapshot_dir / kManifestName;
    std::error_code ec;

    fs::create_directories(module_dir, ec);
    if (ec) {
        LOGW("yukizygisk early: failed to create %s: %s", module_dir.c_str(), ec.message().c_str());
        return 1;
    }

    fs::remove_all(module_dir, ec);
    fs::create_directories(module_dir, ec);
    if (ec) {
        LOGW("yukizygisk early: failed to reset %s: %s", module_dir.c_str(), ec.message().c_str());
        return 1;
    }

    const fs::path loader_path = snapshot_dir / kLoaderName;
    const fs::path native_core_path = snapshot_dir / kNativeCoreName;
    if (!stage_asset_or_file(kLoaderName, ZYUKILINKER_PATH, loader_path) ||
        !stage_asset_or_file(kNativeCoreName, ZNCORE_PATH, native_core_path)) {
        clear_yukizygisk_early_snapshot();
        LOGW("yukizygisk early: payload unavailable, snapshot cleared");
        return 1;
    }

    std::vector<yz_early_native_entry> entries;
    std::vector<NativeModule> modules = scan_early_native_modules();
    entries.reserve(modules.size());
    for (size_t i = 0; i < modules.size(); i++) {
        yz_early_native_entry entry{};
        if (stage_module_entry(modules[i], i, module_dir, &entry))
            entries.push_back(entry);
    }

    static const char* const kDlopen[] = {
        "__loader_android_dlopen_ext",
        "android_dlopen_ext",
    };
    static const char* const kDlsym[] = {
        "__loader_dlsym",
        "dlsym",
    };

    struct stat linker_stat{};
    bool linker_stat_ok = entries.empty() || stat(kSystemLinker64, &linker_stat) == 0;

    yz_early_native_snapshot_header header{};
    header.magic = YZ_EARLY_NATIVE_MAGIC;
    header.version = YZ_EARLY_NATIVE_VERSION;
    header.header_size = sizeof(header);
    header.entry_size = sizeof(yz_early_native_entry);
    header.flags = YZ_EARLY_NATIVE_FLAG_ENABLED;
    header.count = static_cast<uint32_t>(entries.size());
    header.dlopen_offset = entries.empty() ? 0 : resolve_first(kDlopen, 2);
    header.dlsym_offset = entries.empty() ? 0 : resolve_first(kDlsym, 2);
    header.linker_size = entries.empty() ? 0 : static_cast<uint64_t>(linker_stat.st_size);

    if (!entries.empty() && (!linker_stat_ok || header.dlopen_offset == 0 ||
                             header.dlsym_offset == 0 || header.linker_size == 0)) {
        clear_yukizygisk_early_snapshot();
        LOGW("yukizygisk early: linker offsets unavailable, snapshot cleared");
        return 1;
    }

    if (!write_manifest(manifest_tmp, header, entries)) {
        fs::remove(manifest_tmp, ec);
        LOGW("yukizygisk early: failed to write %s", manifest_tmp.c_str());
        return 1;
    }

    chmod(manifest_tmp.c_str(), 0644);
    (void)lsetfilecon(manifest_tmp, kMetadataFileCon);
    fs::rename(manifest_tmp, manifest, ec);
    if (ec) {
        fs::remove(manifest_tmp, ec);
        LOGW("yukizygisk early: failed to rename snapshot: %s", ec.message().c_str());
        return 1;
    }

    const fs::path stale_dir =
        (preinit_dir == PREINIT_DIR_WATCHDOG) ? PREINIT_DIR_DEFAULT : PREINIT_DIR_WATCHDOG;
    remove_snapshot_dir(stale_dir);

    LOGI("yukizygisk early: snapshot refreshed modules=%zu", entries.size());
    return 0;
}

}  // namespace ksud
