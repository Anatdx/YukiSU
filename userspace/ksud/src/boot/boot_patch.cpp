#include "boot_patch.hpp"
#include "../assets.hpp"
#include "../defs.hpp"
#include "../log.hpp"
#include "../utils.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <regex>

namespace fs = std::filesystem;

namespace ksud {

// SuperKey magic marker (must match kernel's SUPERKEY_MAGIC)
constexpr uint64_t SUPERKEY_MAGIC = 0x5355504552;  // "SUPER" in hex

// SuperKey flags bit definitions
constexpr uint64_t SUPERKEY_FLAG_SIGNATURE_BYPASS = 1;  // bit 0: disable signature verification

// LKM Priority magic marker (must match kernel's LKM_PRIORITY_MAGIC)
// "LKMPRIO" in hex (little-endian)
constexpr uint64_t LKM_PRIORITY_MAGIC = 0x4F4952504D4B4C;

// Calculate superkey hash using the same algorithm as kernel
static uint64_t hash_superkey(const std::string& key) {
    uint64_t hash = 1000000007;
    for (char c : key) {
        hash = hash * 31 + static_cast<uint8_t>(c);
    }
    return hash;
}

// Inject superkey hash and flags into LKM file
static bool inject_superkey_to_lkm(const std::string& lkm_path, const std::string& superkey,
                                   bool signature_bypass) {
    uint64_t hash = hash_superkey(superkey);
    uint64_t flags = signature_bypass ? SUPERKEY_FLAG_SIGNATURE_BYPASS : 0;
    printf("- SuperKey hash: 0x%016llx\n", (unsigned long long)hash);
    printf("- Signature bypass: %s\n", signature_bypass ? "true" : "false");

    std::fstream file(lkm_path, std::ios::in | std::ios::out | std::ios::binary);
    if (!file) {
        LOGE("Failed to open LKM file: %s", lkm_path.c_str());
        return false;
    }

    // Read entire file
    file.seekg(0, std::ios::end);
    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> content(size);
    file.read(reinterpret_cast<char*>(content.data()), size);

    // Search for SUPERKEY_MAGIC in the binary
    uint8_t magic_bytes[8];
    memcpy(magic_bytes, &SUPERKEY_MAGIC, sizeof(magic_bytes));

    bool found = false;
    for (size_t i = 0; i + 24 <= size; i++) {
        if (memcmp(&content[i], magic_bytes, 8) == 0) {
            // Found magic, patch the hash at offset +8
            memcpy(&content[i + 8], &hash, sizeof(hash));
            // Patch the flags at offset +16
            memcpy(&content[i + 16], &flags, sizeof(flags));
            found = true;
            printf("- Injected SuperKey data at offset 0x%zx\n", i);
            break;
        }
    }

    if (!found) {
        printf("- Warning: SUPERKEY_MAGIC not found in LKM, SuperKey may not work\n");
        printf("- Make sure the kernel module is compiled with SuperKey support\n");
    } else {
        // Write back the patched content
        file.seekp(0, std::ios::beg);
        file.write(reinterpret_cast<char*>(content.data()), size);
        file.sync();
    }

    return true;
}

// Inject LKM priority setting into the kernel module
static bool inject_lkm_priority_to_lkm(const std::string& lkm_path, bool enabled) {
    uint32_t enabled_value = enabled ? 1 : 0;
    printf("- LKM priority over GKI: %s\n", enabled ? "true" : "false");

    std::fstream file(lkm_path, std::ios::in | std::ios::out | std::ios::binary);
    if (!file) {
        LOGE("Failed to open LKM file for priority patching: %s", lkm_path.c_str());
        return false;
    }

    // Read entire file
    file.seekg(0, std::ios::end);
    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> content(size);
    file.read(reinterpret_cast<char*>(content.data()), size);

    // Search for LKM_PRIORITY_MAGIC in the binary
    uint8_t magic_bytes[8];
    memcpy(magic_bytes, &LKM_PRIORITY_MAGIC, sizeof(magic_bytes));

    bool found = false;
    // Structure in kernel:
    // offset 0: magic (u64)
    // offset 8: enabled (u32)
    // offset 12: reserved (u32)
    for (size_t i = 0; i + 16 <= size; i++) {
        if (memcmp(&content[i], magic_bytes, 8) == 0) {
            // Found magic, patch the enabled field at offset +8
            memcpy(&content[i + 8], &enabled_value, sizeof(enabled_value));
            found = true;
            printf("- Injected LKM priority config at offset 0x%zx\n", i);
            break;
        }
    }

    if (!found) {
        printf("- Warning: LKM_PRIORITY_MAGIC not found in LKM\n");
        printf("- This LKM may not support GKI yield mechanism\n");
    } else {
        // Write back the patched content
        file.seekp(0, std::ios::beg);
        file.write(reinterpret_cast<char*>(content.data()), size);
        file.sync();
    }

    return true;
}

// Execute magiskboot cpio command
static bool do_cpio_cmd(const std::string& magiskboot, const std::string& workdir,
                        const std::string& cpio_path, const std::string& cmd) {
    auto result = exec_command({magiskboot, "cpio", cpio_path, cmd});
    if (result.exit_code != 0) {
        LOGE("magiskboot cpio %s failed", cmd.c_str());
        return false;
    }
    return true;
}

// Check if boot image is patched by Magisk
static bool is_magisk_patched(const std::string& magiskboot, const std::string& workdir,
                              const std::string& cpio_path) {
    auto result = exec_command({magiskboot, "cpio", cpio_path, "test"});
    // 0: stock, 1: magisk
    return result.exit_code == 1;
}

// Check if boot image is patched by KernelSU
static bool is_kernelsu_patched(const std::string& magiskboot, const std::string& workdir,
                                const std::string& cpio_path) {
    auto result = exec_command({magiskboot, "cpio", cpio_path, "exists kernelsu.ko"});
    return result.exit_code == 0;
}

// Find magiskboot binary
static std::string find_magiskboot(const std::string& specified_path, const std::string& workdir) {
    if (!specified_path.empty()) {
        // For .so files from app package, we need to copy them to workdir and make executable
        if (specified_path.find(".so") != std::string::npos) {
            std::string local_copy = workdir + "/magiskboot";
            std::ifstream src(specified_path, std::ios::binary);
            std::ofstream dst(local_copy, std::ios::binary);
            if (src && dst) {
                dst << src.rdbuf();
                dst.close();
                chmod(local_copy.c_str(), 0755);
                if (access(local_copy.c_str(), X_OK) == 0) {
                    return local_copy;
                }
            }
            LOGE("Failed to prepare magiskboot from %s", specified_path.c_str());
            return "";
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

    // Check PATH
    auto result = exec_command({"which", "magiskboot"});
    if (result.exit_code == 0) {
        std::string path = trim(result.stdout_str);
        if (!path.empty() && access(path.c_str(), X_OK) == 0) {
            return path;
        }
    }

    LOGE("magiskboot not found, please install it first");
    return "";
}

// DD command
static bool dd(const std::string& input, const std::string& output) {
    auto result = exec_command({"dd", "if=" + input, "of=" + output});
    return result.exit_code == 0;
}

// Flash boot image
static bool flash_boot(const std::string& bootdevice, const std::string& new_boot) {
    if (bootdevice.empty()) {
        LOGE("Boot device not found");
        return false;
    }

    // Set device to read-write
    auto result = exec_command({"blockdev", "--setrw", bootdevice});
    if (result.exit_code != 0) {
        LOGE("Failed to set boot device to rw");
        return false;
    }

    if (!dd(new_boot, bootdevice)) {
        LOGE("Failed to flash boot image");
        return false;
    }

    return true;
}

// Calculate SHA1 hash
static std::string calculate_sha1(const std::string& file_path) {
    // Use sha1sum command
    auto result = exec_command({"sha1sum", file_path});
    if (result.exit_code != 0) {
        return "";
    }
    // Output format: "hash  filename"
    size_t space = result.stdout_str.find(' ');
    if (space != std::string::npos) {
        return result.stdout_str.substr(0, space);
    }
    return trim(result.stdout_str);
}

// Backup stock boot image
static bool do_backup(const std::string& magiskboot, const std::string& workdir,
                      const std::string& cpio_path, const std::string& image) {
    std::string sha1 = calculate_sha1(image);
    if (sha1.empty()) {
        LOGE("Failed to calculate SHA1 of boot image");
        return false;
    }

    std::string filename = std::string(KSU_BACKUP_FILE_PREFIX) + sha1;
    printf("- Backup stock boot image\n");

    std::string target = std::string(KSU_BACKUP_DIR) + filename;

    // Copy image to backup location
    std::ifstream src(image, std::ios::binary);
    std::ofstream dst(target, std::ios::binary);
    if (!src || !dst) {
        LOGE("Failed to backup boot image to %s", target.c_str());
        return false;
    }
    dst << src.rdbuf();
    src.close();
    dst.close();

    // Write sha1 to workdir
    std::string sha1_file = workdir + "/" + BACKUP_FILENAME;
    write_file(sha1_file, sha1);

    // Add backup info to ramdisk
    if (!do_cpio_cmd(magiskboot, workdir, cpio_path,
                     "add 0755 " + std::string(BACKUP_FILENAME) + " " + sha1_file)) {
        return false;
    }

    printf("- Stock image has been backup to\n");
    printf("- %s\n", target.c_str());
    return true;
}

// Clean old backups
static void clean_backup(const std::string& current_sha1) {
    printf("- Clean up backup\n");
    std::string backup_name = std::string(KSU_BACKUP_FILE_PREFIX) + current_sha1;

    try {
        for (const auto& entry : fs::directory_iterator(KSU_BACKUP_DIR)) {
            if (!entry.is_regular_file())
                continue;

            std::string name = entry.path().filename().string();
            if (name != backup_name && starts_with(name, KSU_BACKUP_FILE_PREFIX)) {
                if (fs::remove(entry.path())) {
                    printf("- removed %s\n", name.c_str());
                }
            }
        }
    } catch (const std::exception& e) {
        LOGW("Clean backup error: %s", e.what());
    }
}

// Parse boot patch arguments
struct BootPatchArgs {
    std::string boot_image;         // -b, --boot
    std::string kernel;             // -k, --kernel
    std::string module;             // -m, --module (LKM path)
    std::string init;               // -i, --init
    std::string superkey;           // -s, --superkey
    bool signature_bypass = false;  // --signature-bypass
    bool lkm_priority = true;       // --lkm-priority
    bool ota = false;               // -u, --ota
    bool flash = false;             // -f, --flash
    std::string out;                // -o, --out
    std::string magiskboot;         // --magiskboot
    std::string kmi;                // --kmi
    std::string partition;          // --partition
    std::string out_name;           // --out-name
};

static BootPatchArgs parse_boot_patch_args(const std::vector<std::string>& args) {
    BootPatchArgs result;

    for (size_t i = 0; i < args.size(); i++) {
        const std::string& arg = args[i];

        if (arg == "-b" || arg == "--boot") {
            if (i + 1 < args.size())
                result.boot_image = args[++i];
        } else if (arg == "-k" || arg == "--kernel") {
            if (i + 1 < args.size())
                result.kernel = args[++i];
        } else if (arg == "-m" || arg == "--module") {
            if (i + 1 < args.size())
                result.module = args[++i];
        } else if (arg == "-i" || arg == "--init") {
            if (i + 1 < args.size())
                result.init = args[++i];
        } else if (arg == "-s" || arg == "--superkey") {
            if (i + 1 < args.size())
                result.superkey = args[++i];
        } else if (arg == "--signature-bypass") {
            result.signature_bypass = true;
        } else if (arg == "--lkm-priority") {
            if (i + 1 < args.size()) {
                std::string val = args[++i];
                result.lkm_priority = (val == "true" || val == "1");
            } else {
                result.lkm_priority = true;
            }
        } else if (arg == "-u" || arg == "--ota") {
            result.ota = true;
        } else if (arg == "-f" || arg == "--flash") {
            result.flash = true;
        } else if (arg == "-o" || arg == "--out") {
            if (i + 1 < args.size())
                result.out = args[++i];
        } else if (arg == "--magiskboot") {
            if (i + 1 < args.size())
                result.magiskboot = args[++i];
        } else if (arg == "--kmi") {
            if (i + 1 < args.size())
                result.kmi = args[++i];
        } else if (arg == "--partition") {
            if (i + 1 < args.size())
                result.partition = args[++i];
        } else if (arg == "--out-name") {
            if (i + 1 < args.size())
                result.out_name = args[++i];
        }
    }

    return result;
}

int boot_patch(const std::vector<std::string>& args) {
    auto parsed = parse_boot_patch_args(args);

    printf("\n");
    printf("__   __ _   _  _  __ ___  ____   _   _ \n");
    printf("\\ \\ / /| | | || |/ /|_ _|/ ___| | | | |\n");
    printf(" \\ V / | | | || ' /  | | \\___ \\ | | | |\n");
    printf("  | |  | |_| || . \\  | |  ___) || |_| |\n");
    printf("  |_|   \\___/ |_|\\_\\|___||____/  \\___/ \n");
    printf("\n");

    // Create temp working directory
    char tmpdir_template[] = "/data/local/tmp/KernelSU_XXXXXX";
    char* tmpdir = mkdtemp(tmpdir_template);
    if (!tmpdir) {
        LOGE("Failed to create temp directory");
        return 1;
    }
    std::string workdir = tmpdir;

    // Cleanup function
    auto cleanup = [&workdir]() {
        std::string cmd = "rm -rf " + workdir;
        system(cmd.c_str());
    };

    // Find magiskboot
    std::string magiskboot = find_magiskboot(parsed.magiskboot, workdir);
    if (magiskboot.empty()) {
        cleanup();
        return 1;
    }
    printf("- Using magiskboot: %s\n", magiskboot.c_str());

    // Get or detect KMI
    std::string kmi = parsed.kmi;
    if (kmi.empty()) {
        kmi = get_current_kmi();
        if (kmi.empty() && parsed.boot_image.empty()) {
            printf("- Failed to detect KMI and no boot image specified\n");
            cleanup();
            return 1;
        }
    }
    if (!kmi.empty()) {
        printf("- KMI: %s\n", kmi.c_str());
    }

    // Determine boot image path
    std::string bootimage;
    std::string bootdevice;
    bool patch_file = !parsed.boot_image.empty();

    if (patch_file) {
        bootimage = parsed.boot_image;
        if (access(bootimage.c_str(), R_OK) != 0) {
            LOGE("Boot image not found: %s", bootimage.c_str());
            cleanup();
            return 1;
        }
    } else {
        // Auto-detect boot partition
        std::string slot = get_slot_suffix(parsed.ota);
        std::string partition_name;

        // Determine if we're in replace kernel mode (when --kernel is specified)
        bool is_replace_kernel = !parsed.kernel.empty();

        if (!parsed.partition.empty()) {
            // User specified partition name (e.g., "init_boot" or "boot")
            partition_name =
                choose_boot_partition(kmi, parsed.ota, &parsed.partition, is_replace_kernel);
        } else {
            // Auto-detect: choose_boot_partition returns full path with slot
            partition_name = choose_boot_partition(kmi, parsed.ota, nullptr, is_replace_kernel);
        }

        printf("- Bootdevice: %s\n", partition_name.c_str());

        bootimage = workdir + "/boot.img";
        if (!dd(partition_name, bootimage)) {
            LOGE("Failed to read boot image from %s", partition_name.c_str());
            cleanup();
            return 1;
        }
        bootdevice = partition_name;
    }

    // Prepare LKM module
    printf("- Preparing assets\n");
    std::string kmod_file = workdir + "/kernelsu.ko";

    if (!parsed.module.empty()) {
        // Use specified module
        std::ifstream src(parsed.module, std::ios::binary);
        std::ofstream dst(kmod_file, std::ios::binary);
        if (!src || !dst) {
            LOGE("Failed to copy kernel module from %s", parsed.module.c_str());
            cleanup();
            return 1;
        }
        dst << src.rdbuf();
    } else {
        // Try to extract LKM from embedded assets first
        std::string kmi_lkm_name = kmi + "_kernelsu.ko";
        printf("- KMI: %s\n", kmi.c_str());

        if (copy_asset_to_file(kmi_lkm_name, kmod_file)) {
            printf("- Using embedded LKM: %s\n", kmi_lkm_name.c_str());
        } else {
            // Fallback: try to find LKM from known locations
            std::vector<std::string> search_paths = {
                std::string(BINARY_DIR) + kmi_lkm_name,  std::string(BINARY_DIR) + "kernelsu.ko",
                std::string(WORKING_DIR) + kmi_lkm_name, std::string(WORKING_DIR) + "kernelsu.ko",
                "/data/local/tmp/" + kmi_lkm_name,       "/data/local/tmp/kernelsu.ko",
            };

            bool found = false;
            for (const auto& path : search_paths) {
                if (access(path.c_str(), R_OK) == 0) {
                    printf("- Found LKM at %s\n", path.c_str());
                    std::ifstream src(path, std::ios::binary);
                    std::ofstream dst(kmod_file, std::ios::binary);
                    if (src && dst) {
                        dst << src.rdbuf();
                        found = true;
                        break;
                    }
                }
            }

            if (!found) {
                // List available KMIs from embedded assets
                auto supported = list_supported_kmi();

                printf("\n");
                printf("! No LKM module found for KMI: %s\n", kmi.c_str());
                printf("!\n");
                if (!supported.empty()) {
                    printf("! Supported KMIs in this build:\n");
                    for (const auto& k : supported) {
                        printf("!   - %s\n", k.c_str());
                    }
                    printf("!\n");
                }
                printf("! Please select an LKM file in Manager, or place it at:\n");
                printf("!   %s%s\n", BINARY_DIR, kmi_lkm_name.c_str());
                printf("!\n");
                printf("! You can download LKM from:\n");
                printf("!   https://github.com/Anatdx/YukiSU/releases\n");
                printf("\n");
                cleanup();
                return 1;
            }
        }
    }

    // Inject SuperKey if specified
    if (!parsed.superkey.empty()) {
        printf("- Injecting SuperKey into LKM\n");
        inject_superkey_to_lkm(kmod_file, parsed.superkey, parsed.signature_bypass);
    } else if (parsed.signature_bypass) {
        printf("- Warning: signature_bypass requires superkey to be set, ignoring\n");
    }

    // Inject LKM priority setting
    printf("- Configuring LKM priority\n");
    inject_lkm_priority_to_lkm(kmod_file, parsed.lkm_priority);

    // Prepare init if specified
    std::string init_file = workdir + "/init";
    if (!parsed.init.empty()) {
        std::ifstream src(parsed.init, std::ios::binary);
        std::ofstream dst(init_file, std::ios::binary);
        if (!src || !dst) {
            LOGE("Failed to copy init from %s", parsed.init.c_str());
            cleanup();
            return 1;
        }
        dst << src.rdbuf();
    } else {
        // Try to extract ksuinit from embedded assets first (like Rust version)
        if (copy_asset_to_file("ksuinit", init_file)) {
            printf("- Using embedded ksuinit\n");
        } else {
            // Fallback: check standard location
            std::string ksuinit_path = std::string(BINARY_DIR) + "ksuinit";
            if (access(ksuinit_path.c_str(), R_OK) == 0) {
                std::ifstream src(ksuinit_path, std::ios::binary);
                std::ofstream dst(init_file, std::ios::binary);
                dst << src.rdbuf();
                printf("- Using ksuinit from %s\n", ksuinit_path.c_str());
            } else {
                LOGE("ksuinit not found in embedded assets or %s", ksuinit_path.c_str());
                LOGE("Please install KernelSU Manager or rebuild ksud with ksuinit embedded");
                cleanup();
                return 1;
            }
        }
    }
    chmod(init_file.c_str(), 0755);

    // Unpack boot image
    printf("- Unpacking boot image\n");
    auto unpack_result = exec_command({magiskboot, "unpack", bootimage});
    if (unpack_result.exit_code != 0) {
        LOGE("magiskboot unpack failed");
        cleanup();
        return 1;
    }

    // Find ramdisk
    std::string ramdisk;
    std::vector<std::string> ramdisk_candidates = {workdir + "/ramdisk.cpio",
                                                   workdir + "/vendor_ramdisk/init_boot.cpio",
                                                   workdir + "/vendor_ramdisk/ramdisk.cpio"};

    for (const auto& candidate : ramdisk_candidates) {
        if (access(candidate.c_str(), R_OK) == 0) {
            ramdisk = candidate;
            break;
        }
    }

    if (ramdisk.empty()) {
        printf("- No ramdisk found, creating default\n");
        ramdisk = workdir + "/ramdisk.cpio";
        // Create empty ramdisk
        exec_command({magiskboot, "cpio", ramdisk, "mkdir 0755 ."});
    }

    // Check for Magisk
    if (is_magisk_patched(magiskboot, workdir, ramdisk)) {
        LOGE("Cannot work with Magisk patched image");
        cleanup();
        return 1;
    }

    printf("- Adding KernelSU LKM\n");
    bool already_patched = is_kernelsu_patched(magiskboot, workdir, ramdisk);

    if (!already_patched) {
        // Backup init if it exists
        auto init_exists = exec_command({magiskboot, "cpio", ramdisk, "exists init"});
        if (init_exists.exit_code == 0) {
            do_cpio_cmd(magiskboot, workdir, ramdisk, "mv init init.real");
        }
    }

    // Add init and kernelsu.ko
    // Change to workdir for cpio operations
    char orig_dir[PATH_MAX];
    getcwd(orig_dir, sizeof(orig_dir));
    chdir(workdir.c_str());

    do_cpio_cmd(magiskboot, workdir, ramdisk, "add 0755 init init");
    do_cpio_cmd(magiskboot, workdir, ramdisk, "add 0755 kernelsu.ko kernelsu.ko");

    // Backup if flashing and not already patched
    if (!already_patched && parsed.flash) {
        if (!do_backup(magiskboot, workdir, ramdisk, bootimage)) {
            printf("- Warning: Backup stock image failed\n");
        }
    }

    // Repack boot image
    printf("- Repacking boot image\n");
    auto repack_result = exec_command({magiskboot, "repack", bootimage});
    if (repack_result.exit_code != 0) {
        LOGE("magiskboot repack failed");
        chdir(orig_dir);
        cleanup();
        return 1;
    }

    std::string new_boot = workdir + "/new-boot.img";

    // Output patched image
    if (patch_file) {
        std::string output_dir = parsed.out.empty() ? "." : parsed.out;
        std::string name = parsed.out_name;
        if (name.empty()) {
            time_t now = time(nullptr);
            struct tm* tm_info = localtime(&now);
            char time_str[32];
            strftime(time_str, sizeof(time_str), "%Y%m%d_%H%M%S", tm_info);
            name = std::string("kernelsu_patched_") + time_str + ".img";
        }

        std::string output_image = output_dir + "/" + name;

        std::ifstream src(new_boot, std::ios::binary);
        std::ofstream dst(output_image, std::ios::binary);
        if (!src || !dst) {
            LOGE("Failed to write output file");
            chdir(orig_dir);
            cleanup();
            return 1;
        }
        dst << src.rdbuf();

        printf("- Output file is written to\n");
        printf("- %s\n", output_image.c_str());
    }

    // Flash if requested
    if (parsed.flash && !bootdevice.empty()) {
        printf("- Flashing new boot image\n");
        if (!flash_boot(bootdevice, new_boot)) {
            LOGE("Failed to flash boot image");
            chdir(orig_dir);
            cleanup();
            return 1;
        }
    }

    chdir(orig_dir);
    cleanup();

    printf("- Done!\n");
    return 0;
}

// Parse boot restore arguments
struct BootRestoreArgs {
    std::string boot_image;  // -b, --boot
    bool flash = false;      // -f, --flash
    std::string magiskboot;  // --magiskboot
    std::string out_name;    // --out-name
};

static BootRestoreArgs parse_boot_restore_args(const std::vector<std::string>& args) {
    BootRestoreArgs result;

    for (size_t i = 0; i < args.size(); i++) {
        const std::string& arg = args[i];

        if (arg == "-b" || arg == "--boot") {
            if (i + 1 < args.size())
                result.boot_image = args[++i];
        } else if (arg == "-f" || arg == "--flash") {
            result.flash = true;
        } else if (arg == "--magiskboot") {
            if (i + 1 < args.size())
                result.magiskboot = args[++i];
        } else if (arg == "--out-name") {
            if (i + 1 < args.size())
                result.out_name = args[++i];
        }
    }

    return result;
}

int boot_restore(const std::vector<std::string>& args) {
    auto parsed = parse_boot_restore_args(args);

    // Create temp working directory
    char tmpdir_template[] = "/data/local/tmp/KernelSU_XXXXXX";
    char* tmpdir = mkdtemp(tmpdir_template);
    if (!tmpdir) {
        LOGE("Failed to create temp directory");
        return 1;
    }
    std::string workdir = tmpdir;

    auto cleanup = [&workdir]() {
        std::string cmd = "rm -rf " + workdir;
        system(cmd.c_str());
    };

    // Find magiskboot
    std::string magiskboot = find_magiskboot(parsed.magiskboot, workdir);
    if (magiskboot.empty()) {
        cleanup();
        return 1;
    }

    // Get KMI for partition detection
    std::string kmi = get_current_kmi();

    // Determine boot image path
    std::string bootimage;
    std::string bootdevice;

    if (!parsed.boot_image.empty()) {
        bootimage = parsed.boot_image;
        if (access(bootimage.c_str(), R_OK) != 0) {
            LOGE("Boot image not found: %s", bootimage.c_str());
            cleanup();
            return 1;
        }
    } else {
        // Auto-detect boot partition (restore doesn't replace kernel)
        std::string partition_name = choose_boot_partition(kmi, false, nullptr, false);
        printf("- Bootdevice: %s\n", partition_name.c_str());

        bootimage = workdir + "/boot.img";
        if (!dd(partition_name, bootimage)) {
            LOGE("Failed to read boot image");
            cleanup();
            return 1;
        }
        bootdevice = partition_name;
    }

    // Unpack boot image
    printf("- Unpacking boot image\n");
    auto unpack_result = exec_command({magiskboot, "unpack", bootimage});
    if (unpack_result.exit_code != 0) {
        LOGE("magiskboot unpack failed");
        cleanup();
        return 1;
    }

    // Find ramdisk
    std::string ramdisk;
    std::vector<std::string> ramdisk_candidates = {workdir + "/ramdisk.cpio",
                                                   workdir + "/vendor_ramdisk/init_boot.cpio",
                                                   workdir + "/vendor_ramdisk/ramdisk.cpio"};

    for (const auto& candidate : ramdisk_candidates) {
        if (access(candidate.c_str(), R_OK) == 0) {
            ramdisk = candidate;
            break;
        }
    }

    if (ramdisk.empty()) {
        LOGE("No compatible ramdisk found");
        cleanup();
        return 1;
    }

    // Check if patched by KernelSU
    if (!is_kernelsu_patched(magiskboot, workdir, ramdisk)) {
        LOGE("Boot image is not patched by KernelSU");
        cleanup();
        return 1;
    }

    std::string new_boot;
    bool from_backup = false;

    // Try to find backup
    auto backup_exists =
        exec_command({magiskboot, "cpio", ramdisk, "exists " + std::string(BACKUP_FILENAME)});
    if (backup_exists.exit_code == 0) {
        // Extract backup sha1
        std::string backup_file = workdir + "/" + BACKUP_FILENAME;
        exec_command({magiskboot, "cpio", ramdisk,
                      "extract " + std::string(BACKUP_FILENAME) + " " + backup_file});

        auto sha_content = read_file(backup_file);
        if (sha_content) {
            std::string sha = trim(*sha_content);
            std::string backup_path = std::string(KSU_BACKUP_DIR) + KSU_BACKUP_FILE_PREFIX + sha;

            if (access(backup_path.c_str(), R_OK) == 0) {
                new_boot = backup_path;
                from_backup = true;
                clean_backup(sha);
            } else {
                printf("- Warning: no backup %s found!\n", backup_path.c_str());
            }
        }
    } else {
        printf("- Backup info is absent!\n");
    }

    // If no backup, manually remove KernelSU
    if (!from_backup) {
        // Remove kernelsu.ko
        do_cpio_cmd(magiskboot, workdir, ramdisk, "rm kernelsu.ko");

        // Restore init if init.real exists
        auto init_real_exists = exec_command({magiskboot, "cpio", ramdisk, "exists init.real"});
        if (init_real_exists.exit_code == 0) {
            do_cpio_cmd(magiskboot, workdir, ramdisk, "mv init.real init");
        }

        // Repack
        printf("- Repacking boot image\n");
        auto repack_result = exec_command({magiskboot, "repack", bootimage});
        if (repack_result.exit_code != 0) {
            LOGE("magiskboot repack failed");
            cleanup();
            return 1;
        }
        new_boot = workdir + "/new-boot.img";
    }

    // Output restored image
    if (!parsed.boot_image.empty()) {
        std::string name = parsed.out_name;
        if (name.empty()) {
            time_t now = time(nullptr);
            struct tm* tm_info = localtime(&now);
            char time_str[32];
            strftime(time_str, sizeof(time_str), "%Y%m%d_%H%M%S", tm_info);
            name = std::string("kernelsu_restore_") + time_str + ".img";
        }

        std::string output_image = "./" + name;

        std::ifstream src(new_boot, std::ios::binary);
        std::ofstream dst(output_image, std::ios::binary);
        if (!src || !dst) {
            LOGE("Failed to write output file");
            cleanup();
            return 1;
        }
        dst << src.rdbuf();

        printf("- Output file is written to\n");
        printf("- %s\n", output_image.c_str());
    }

    // Flash if requested
    if (parsed.flash && !bootdevice.empty()) {
        if (from_backup) {
            printf("- Flashing new boot image from %s\n", new_boot.c_str());
        } else {
            printf("- Flashing new boot image\n");
        }
        if (!flash_boot(bootdevice, new_boot)) {
            LOGE("Failed to flash boot image");
            cleanup();
            return 1;
        }
    }

    cleanup();
    printf("- Done!\n");
    return 0;
}

std::string get_current_kmi() {
    // Read from /proc/version or uname
    auto version = read_file("/proc/version");
    if (!version) {
        LOGE("Failed to read /proc/version");
        return "";
    }

    // Parse kernel version to get KMI
    // Example: "5.15.123-android14-6-g1234567"
    // KMI format: android14-5.15

    std::string line = *version;
    size_t start = line.find("Linux version ");
    if (start == std::string::npos)
        return "";

    start += 14;
    size_t end = line.find(' ', start);
    if (end == std::string::npos)
        end = line.length();

    std::string full_version = line.substr(start, end - start);

    // Extract major.minor
    size_t dot1 = full_version.find('.');
    if (dot1 == std::string::npos)
        return "";
    size_t dot2 = full_version.find('.', dot1 + 1);
    if (dot2 == std::string::npos)
        dot2 = full_version.length();

    std::string major_minor = full_version.substr(0, dot2);

    // Try to find android version
    size_t android_pos = full_version.find("-android");
    if (android_pos != std::string::npos) {
        size_t ver_start = android_pos + 8;
        size_t ver_end = full_version.find('-', ver_start);
        if (ver_end == std::string::npos)
            ver_end = full_version.length();

        std::string android_ver = full_version.substr(ver_start, ver_end - ver_start);
        return "android" + android_ver + "-" + major_minor;
    }

    return major_minor;
}

int boot_info_current_kmi() {
    std::string kmi = get_current_kmi();
    if (kmi.empty()) {
        printf("Failed to get current KMI\n");
        return 1;
    }
    printf("%s\n", kmi.c_str());
    return 0;
}

int boot_info_supported_kmis() {
    auto supported = list_supported_kmi();
    if (supported.empty()) {
        printf("No embedded LKMs found\n");
        return 1;
    }
    for (const auto& kmi : supported) {
        printf("%s\n", kmi.c_str());
    }
    return 0;
}

int boot_info_is_ab_device() {
    auto ab_update = getprop("ro.build.ab_update");
    bool is_ab = ab_update && trim(*ab_update) == "true";
    printf("%s\n", is_ab ? "true" : "false");
    return 0;
}

std::string get_slot_suffix(bool ota) {
    auto suffix = getprop("ro.boot.slot_suffix");
    if (!suffix || suffix->empty()) {
        return "";
    }

    if (ota) {
        // Toggle to other slot
        if (*suffix == "_a")
            return "_b";
        if (*suffix == "_b")
            return "_a";
    }

    return *suffix;
}

int boot_info_slot_suffix(bool ota) {
    std::string suffix = get_slot_suffix(ota);
    printf("%s\n", suffix.c_str());
    return 0;
}

std::string choose_boot_partition(const std::string& kmi, bool ota,
                                  const std::string* override_partition, bool is_replace_kernel) {
    // If specific partition is specified, use it
    if (override_partition && !override_partition->empty()) {
        // Validate partition name
        if (*override_partition == "boot" || *override_partition == "init_boot" ||
            *override_partition == "vendor_boot") {
            std::string slot = get_slot_suffix(ota);
            return "/dev/block/by-name/" + *override_partition + slot;
        }
        // Invalid partition name, fallback to auto-detect
    }

    std::string slot = get_slot_suffix(ota);

    // Android 12 GKI doesn't have init_boot
    bool skip_init_boot = kmi.find("android12-") == 0;

    // Check if init_boot exists
    std::string init_boot = "/dev/block/by-name/init_boot" + slot;
    struct stat st;
    bool init_boot_exist = (stat(init_boot.c_str(), &st) == 0);

    // Use init_boot if:
    // - Not replacing kernel (LKM mode)
    // - init_boot partition exists
    // - Not android12 (which doesn't have init_boot)
    if (!is_replace_kernel && init_boot_exist && !skip_init_boot) {
        return init_boot;
    }

    // Fallback to boot
    return "/dev/block/by-name/boot" + slot;
}

// Return partition name only (without path and slot suffix)
// Used by boot-info default-partition command for manager
std::string get_default_partition_name(const std::string& kmi, bool is_replace_kernel) {
    std::string slot = get_slot_suffix(false);

    // Android 12 GKI doesn't have init_boot
    bool skip_init_boot = kmi.find("android12-") == 0;

    // Check if init_boot exists
    std::string init_boot = "/dev/block/by-name/init_boot" + slot;
    struct stat st;
    bool init_boot_exist = (stat(init_boot.c_str(), &st) == 0);

    // Use init_boot if:
    // - Not replacing kernel (LKM mode)
    // - init_boot partition exists
    // - Not android12 (which doesn't have init_boot)
    if (!is_replace_kernel && init_boot_exist && !skip_init_boot) {
        return "init_boot";
    }

    return "boot";
}

int boot_info_default_partition() {
    std::string kmi = get_current_kmi();
    // Return partition name only, not full path (matching Rust behavior)
    std::string partition = get_default_partition_name(kmi, false);
    printf("%s\n", partition.c_str());
    return 0;
}

int boot_info_available_partitions() {
    // Return base partition names (without slot suffix) like Rust version
    // Manager will add slot suffix based on user's choice
    std::string slot = get_slot_suffix(false);

    const char* candidates[] = {"boot", "init_boot", "vendor_boot"};

    for (const char* name : candidates) {
        std::string full_path = std::string("/dev/block/by-name/") + name + slot;
        struct stat st;
        if (stat(full_path.c_str(), &st) == 0) {
            printf("%s\n", name);
        }
    }

    return 0;
}

}  // namespace ksud
