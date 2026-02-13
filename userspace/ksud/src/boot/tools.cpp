#include "tools.hpp"
#include "../defs.hpp"
#include "../log.hpp"
#include "../utils.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <filesystem>
#include <vector>

namespace ksud {

namespace fs = std::filesystem;

// Find magiskboot binary
std::string find_magiskboot(const std::string& specified_path, const std::string& workdir) {
    if (!specified_path.empty()) {
        // For .so files from app package, use them directly from nativeLibraryDir
        // which is usually mounted with exec permission, unlike app cache/data dirs
        // The .so extension is just a workaround for Android APK extraction
        if (specified_path.find(".so") != std::string::npos) {
            // Get canonical (absolute) path
            char resolved_path[PATH_MAX];
            if (realpath(specified_path.c_str(), resolved_path) != nullptr) {
                // Prefer sibling libmagiskboot.so when caller passed libksud.so.
                // libksud.so may embed a limited magiskboot entry that lacks cpio commands.
                fs::path specified_lib = resolved_path;
                fs::path lib_dir = specified_lib.parent_path();
                fs::path sibling_magiskboot = lib_dir / "libmagiskboot.so";
                if (fs::exists(sibling_magiskboot) &&
                    access(sibling_magiskboot.c_str(), X_OK) == 0) {
                    printf("- Using sibling magiskboot: %s\n", sibling_magiskboot.c_str());
                    return sibling_magiskboot.string();
                }
                if (access(MAGISKBOOT_PATH, X_OK) == 0) {
                    printf("- Using installed magiskboot: %s\n", MAGISKBOOT_PATH);
                    return MAGISKBOOT_PATH;
                }

                // Check if directly executable from nativeLibraryDir
                if (access(resolved_path, X_OK) == 0) {
                    printf("- Using magiskboot directly: %s\n", resolved_path);
                    return resolved_path;
                }
            }

            if (!workdir.empty()) {
                // Fallback: try copying to workdir (might fail due to noexec)
                std::string local_copy = workdir + "/magiskboot";
                printf("- Trying to copy magiskboot to workdir...\n");

                int src_fd = open(specified_path.c_str(), O_RDONLY);
                if (src_fd < 0) {
                    LOGE("Failed to open source magiskboot: %s (errno=%d)", specified_path.c_str(),
                         errno);
                    return "";
                }

                int dst_fd = open(local_copy.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0755);
                if (dst_fd < 0) {
                    LOGE("Failed to create dest magiskboot (errno=%d)", errno);
                    close(src_fd);
                    return "";
                }

                char buf[8192];
                ssize_t bytes_read;
                while ((bytes_read = read(src_fd, buf, sizeof(buf))) > 0) {
                    write(dst_fd, buf, bytes_read);
                }

                fsync(dst_fd);
                close(src_fd);
                close(dst_fd);

                if (access(local_copy.c_str(), X_OK) == 0) {
                    return local_copy;
                }
                LOGE("magiskboot copy failed or not executable (noexec mount?)");
                return "";
            }
        }

        if (access(specified_path.c_str(), X_OK) == 0) {
            return specified_path;
        }
        LOGE("Specified magiskboot not found or not executable: %s", specified_path.c_str());
        return "";
    }

    // Check standard locations
    if (access(MAGISKBOOT_PATH, X_OK) == 0) {
        return MAGISKBOOT_PATH;
    }

    // Check next to our own executable (e.g. libksud.so -> libmagiskboot.so)
    char self_path[PATH_MAX];
    if (readlink("/proc/self/exe", self_path, sizeof(self_path)) != -1) {
        fs::path bin_path = self_path;
        fs::path lib_dir = bin_path.parent_path();

        // Check for libmagiskboot.so (standard app layout)
        fs::path magiskboot_lib = lib_dir / "libmagiskboot.so";
        if (fs::exists(magiskboot_lib)) {
            // If we have a workdir, try to copy it there to ensure it's executable
            // because lib dir might not be executable if it's in /data/app/...
            // wait, nativeLibraryDir IS executable. But let's check access.
            if (access(magiskboot_lib.c_str(), X_OK) == 0) {
                return magiskboot_lib.string();
            }

            // If not executable, try to copy to workdir if available
            if (!workdir.empty()) {
                std::string local_copy = workdir + "/magiskboot";
                if (fs::exists(local_copy) && access(local_copy.c_str(), X_OK) == 0) {
                    return local_copy;
                }
                try {
                    fs::copy_file(magiskboot_lib, local_copy, fs::copy_options::overwrite_existing);
                    chmod(local_copy.c_str(), 0755);
                    return local_copy;
                } catch (...) {
                }
            }
        }

        // Check for magiskboot (binary layout)
        fs::path magiskboot_bin = lib_dir / "magiskboot";
        if (access(magiskboot_bin.c_str(), X_OK) == 0) {
            return magiskboot_bin.string();
        }
    }

    // Check PATH
    auto result = exec_command({"which", "magiskboot"});
    if (result.exit_code == 0) {
        std::string path = trim(result.stdout_str);
        if (!path.empty() && access(path.c_str(), X_OK) == 0) {
            return path;
        }
    }

    // If workdir is provided but we still haven't found it, don't error yet, caller might handle it
    // But original code logged error.
    LOGE("magiskboot not found, please install it first");
    return "";
}

// DD command wrapper
bool exec_dd(const std::string& input, const std::string& output) {
    auto result = exec_command({"dd", "if=" + input, "of=" + output, "bs=4M", "conv=fsync"});
    if (result.exit_code == 0) {
        return true;
    }

    // Fallback for older toybox/busybox dd variants that may not support conv=fsync.
    auto fallback = exec_command({"dd", "if=" + input, "of=" + output});
    if (fallback.exit_code == 0) {
        return true;
    }

    LOGE("dd failed: if=%s of=%s", input.c_str(), output.c_str());
    if (!result.stderr_str.empty()) {
        LOGE("dd stderr(primary): %s", result.stderr_str.c_str());
    }
    if (!result.stdout_str.empty()) {
        LOGE("dd stdout(primary): %s", result.stdout_str.c_str());
    }
    if (!fallback.stderr_str.empty()) {
        LOGE("dd stderr(fallback): %s", fallback.stderr_str.c_str());
    }
    if (!fallback.stdout_str.empty()) {
        LOGE("dd stdout(fallback): %s", fallback.stdout_str.c_str());
    }
    return false;
}

}  // namespace ksud
