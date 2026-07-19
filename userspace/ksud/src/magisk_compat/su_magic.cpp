#include "magisk_compat/su_magic.hpp"

#include "assets.hpp"
#include "core/ksucalls.hpp"
#include "log.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <climits>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

// Older bionic headers may not expose every propagation flag.
#ifndef MS_SLAVE
#define MS_SLAVE (1 << 19)
#endif  // #ifndef MS_SLAVE
#ifndef MS_PRIVATE
#define MS_PRIVATE (1 << 18)
#endif  // #ifndef MS_PRIVATE

namespace ksud {

namespace {

constexpr const char* kWorkRoot = "/mnt/ksu_magisk_su";
constexpr const char* kSkelTmpfs = "/mnt/ksu_magisk_su/bin";
constexpr const char* kSkelData = "/data/adb/ksu/su_skel";

constexpr const char* kSystemBin = "/system/bin";
constexpr const char* kSuDataDir = "/data/adb/ksu";
constexpr const char* kSuDataPath = "/data/adb/ksu/su";
constexpr const char* kMountSource = "KSU";
constexpr const char* kSuContext = "u:object_r:shell_exec:s0";
constexpr const char* kSelinuxXattr = "security.selinux";
constexpr const char* kBinDirContextFallback = "u:object_r:system_file:s0";

const char* g_skel = kSkelTmpfs;
bool g_on_tmpfs = true;

bool get_context(const char* path, char* out, size_t out_sz) {
    ssize_t const n = lgetxattr(path, kSelinuxXattr, out, out_sz - 1);
    if (n <= 0) {
        return false;
    }
    out[n] = '\0';
    return true;
}

void set_context(const char* path, const char* ctx) {
    lsetxattr(path, kSelinuxXattr, ctx, strlen(ctx) + 1, 0);
}

bool tmpfs_xattr_supported() {
    if (getenv("KSU_SU_NO_XATTR") != nullptr) {
        return false;
    }
    const char* probe = "/mnt/.ksu_su_xattr_probe";
    mkdir(probe, 0700);
    bool ok = false;
    if (mount("tmpfs", probe, "tmpfs", 0, nullptr) == 0) {
        const std::string f = std::string(probe) + "/probe";
        int const fd = open(f.c_str(), O_CREAT | O_WRONLY, 0600);
        if (fd >= 0) {
            close(fd);
            if (lsetxattr(f.c_str(), kSelinuxXattr, kBinDirContextFallback,
                          strlen(kBinDirContextFallback) + 1, 0) == 0) {
                char back[256];
                if (get_context(f.c_str(), back, sizeof(back)) &&
                    strcmp(back, kBinDirContextFallback) == 0) {
                    ok = true;
                }
            }
        }
        umount2(probe, MNT_DETACH);
    }
    rmdir(probe);
    return ok;
}

// Preserve per-entry labels such as system_linker_exec.
void clone_attr(const char* src, const char* dst) {
    struct stat st{};
    if (lstat(src, &st) != 0) {
        return;
    }
    chmod(dst, st.st_mode & 07777);
    chown(dst, st.st_uid, st.st_gid);
    char ctx[256];
    if (get_context(src, ctx, sizeof(ctx))) {
        set_context(dst, ctx);
    }
}

bool bind_mount(const char* src, const char* dst) {
    return mount(src, dst, nullptr, MS_BIND, nullptr) == 0;
}

bool mirror_entry(const std::string& src, const std::string& dst) {
    struct stat st{};
    if (lstat(src.c_str(), &st) != 0) {
        LOGW("magisk_su: lstat %s failed: %s", src.c_str(), strerror(errno));
        return false;
    }

    if (S_ISREG(st.st_mode)) {
        int const fd = open(dst.c_str(), O_CREAT | O_WRONLY | O_TRUNC, st.st_mode & 07777);
        if (fd < 0) {
            LOGE("magisk_su: create mirror %s failed: %s", dst.c_str(), strerror(errno));
            return false;
        }
        close(fd);
        if (!bind_mount(src.c_str(), dst.c_str())) {
            LOGE("magisk_su: bind %s -> %s failed: %s", src.c_str(), dst.c_str(), strerror(errno));
            return false;
        }
    } else if (S_ISLNK(st.st_mode)) {
        char tgt[PATH_MAX];
        ssize_t const len = readlink(src.c_str(), tgt, sizeof(tgt) - 1);
        if (len < 0) {
            LOGE("magisk_su: readlink %s failed: %s", src.c_str(), strerror(errno));
            return false;
        }
        tgt[len] = '\0';
        if (symlink(tgt, dst.c_str()) != 0) {
            LOGE("magisk_su: symlink %s failed: %s", dst.c_str(), strerror(errno));
            return false;
        }
        char ctx[256];
        if (get_context(src.c_str(), ctx, sizeof(ctx))) {
            set_context(dst.c_str(), ctx);
        }
    } else if (S_ISDIR(st.st_mode)) {
        if (mkdir(dst.c_str(), st.st_mode & 07777) != 0 && errno != EEXIST) {
            LOGE("magisk_su: mkdir %s failed: %s", dst.c_str(), strerror(errno));
            return false;
        }
        clone_attr(src.c_str(), dst.c_str());
        DIR* d = opendir(src.c_str());
        if (!d) {
            LOGE("magisk_su: opendir %s failed: %s", src.c_str(), strerror(errno));
            return false;
        }
        bool ok = true;
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
                continue;
            }
            if (!mirror_entry(src + "/" + e->d_name, dst + "/" + e->d_name)) {
                ok = false;
                break;
            }
        }
        closedir(d);
        if (!ok) {
            return false;
        }
    }
    return true;
}

void rm_rf(const std::string& path) {
    DIR* d = opendir(path.c_str());
    if (d) {
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
                continue;
            }
            const std::string child = path + "/" + e->d_name;
            struct stat st{};
            if (lstat(child.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                rm_rf(child);
            } else {
                unlink(child.c_str());
            }
        }
        closedir(d);
    }
    rmdir(path.c_str());
}

bool prepare_data_su() {
    mkdir(kSuDataDir, 0755);
    if (!copy_asset_to_file("su", std::string(kSuDataPath))) {
        LOGE("magisk_su: failed to extract embedded su asset");
        return false;
    }
    chmod(kSuDataPath, 0755);
    set_context(kSuDataPath, kSuContext);
    char ctx[256];
    if (!get_context(kSuDataPath, ctx, sizeof(ctx)) || strcmp(ctx, kSuContext) != 0) {
        LOGE("magisk_su: su on /data mislabeled ('%s'); aborting", ctx);
        return false;
    }
    return true;
}

}  // namespace

bool su_magic_mounted() {
    std::ifstream in("/proc/mounts");
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream iss(line);
        std::string src;
        std::string mnt;
        if ((iss >> src >> mnt) && mnt == "/system/bin") {
            return true;
        }
    }
    return false;
}

void magic_umount_su() {
    for (int i = 0; i < 8 && su_magic_mounted(); ++i) {
        if (umount2(kSystemBin, MNT_DETACH) != 0) {
            break;
        }
    }
    umount2(kWorkRoot, MNT_DETACH);
    umount2(kSkelData, MNT_DETACH);
    umount_list_del(kSystemBin);
    rm_rf(kSkelData);
    unlink(kSuDataPath);
}

bool magic_mount_su() {
    magic_umount_su();

    g_on_tmpfs = tmpfs_xattr_supported();
    g_skel = g_on_tmpfs ? kSkelTmpfs : kSkelData;
    LOGI("magisk_su: assembling clone on %s (tmpfs xattr %s)", g_on_tmpfs ? "tmpfs" : "/data",
         g_on_tmpfs ? "yes" : "no");

    char dir_ctx[256];
    if (!get_context(kSystemBin, dir_ctx, sizeof(dir_ctx))) {
        strncpy(dir_ctx, kBinDirContextFallback, sizeof(dir_ctx) - 1);
        dir_ctx[sizeof(dir_ctx) - 1] = '\0';
    }

    if (!prepare_data_su()) {
        return false;
    }

    if (g_on_tmpfs) {
        mkdir(kWorkRoot, 0755);
        if (mount(kMountSource, kWorkRoot, "tmpfs", 0, nullptr) != 0) {
            LOGE("magisk_su: work tmpfs at %s failed: %s", kWorkRoot, strerror(errno));
            magic_umount_su();
            return false;
        }
        mount("none", kWorkRoot, nullptr, MS_PRIVATE, nullptr);
        mkdir(g_skel, 0755);
    } else {
        rm_rf(kSkelData);
        mkdir(kSuDataDir, 0755);
        if (mkdir(kSkelData, 0755) != 0 && errno != EEXIST) {
            LOGE("magisk_su: mkdir %s failed: %s", kSkelData, strerror(errno));
            magic_umount_su();
            return false;
        }
    }

    // A private self-bind makes the skeleton a movable mount point.
    clone_attr(kSystemBin, g_skel);
    if (!bind_mount(g_skel, g_skel)) {
        LOGE("magisk_su: self-bind %s failed: %s", g_skel, strerror(errno));
        magic_umount_su();
        return false;
    }
    mount("none", g_skel, nullptr, MS_PRIVATE, nullptr);

    DIR* d = opendir(kSystemBin);
    if (!d) {
        LOGE("magisk_su: opendir %s failed: %s", kSystemBin, strerror(errno));
        magic_umount_su();
        return false;
    }
    bool ok = true;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        if (!mirror_entry(std::string(kSystemBin) + "/" + e->d_name,
                          std::string(g_skel) + "/" + e->d_name)) {
            ok = false;
            break;
        }
    }
    closedir(d);
    if (!ok) {
        LOGE("magisk_su: mirroring /system/bin failed; aborting");
        magic_umount_su();
        return false;
    }

    const std::string su_dst = std::string(g_skel) + "/su";
    {
        int const fd = open(su_dst.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0755);
        if (fd >= 0) {
            close(fd);
        }
    }
    if (!bind_mount(kSuDataPath, su_dst.c_str())) {
        LOGE("magisk_su: bind su failed: %s", strerror(errno));
        magic_umount_su();
        return false;
    }
    mount(nullptr, su_dst.c_str(), nullptr, MS_REMOUNT | MS_BIND | MS_RDONLY, nullptr);

    // Validate the clone before shadowing /system/bin.
    char ctx[256];
    if (!get_context(su_dst.c_str(), ctx, sizeof(ctx)) || strcmp(ctx, kSuContext) != 0) {
        LOGE("magisk_su: su mislabeled in clone ('%s'); aborting", ctx);
        magic_umount_su();
        return false;
    }
    if (!get_context(g_skel, ctx, sizeof(ctx)) || strcmp(ctx, dir_ctx) != 0) {
        LOGE("magisk_su: clone dir mislabeled ('%s', want '%s'); aborting", ctx, dir_ctx);
        magic_umount_su();
        return false;
    }
    if (access((std::string(g_skel) + "/sh").c_str(), F_OK) != 0) {
        LOGE("magisk_su: clone missing sh; mirror incomplete, aborting");
        magic_umount_su();
        return false;
    }

    // /data-backed trees cannot be moved out of their shared mount lineage.
    mount(nullptr, g_skel, nullptr, MS_REMOUNT | MS_BIND | MS_RDONLY, nullptr);
    const unsigned long commit_flags = g_on_tmpfs ? MS_MOVE : (MS_BIND | MS_REC);
    if (mount(g_skel, kSystemBin, nullptr, commit_flags, nullptr) != 0) {
        LOGE("magisk_su: commit clone -> %s failed: %s", kSystemBin, strerror(errno));
        magic_umount_su();
        return false;
    }
    mount("none", kSystemBin, nullptr, MS_SLAVE, nullptr);

    if (umount_list_add(kSystemBin, MNT_DETACH) != 0) {
        LOGW("magisk_su: failed to register %s for per-app umount", kSystemBin);
    }

    LOGI("magisk_su: visible su magic-mounted at %s/su", kSystemBin);
    return true;
}

}  // namespace ksud
