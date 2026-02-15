#include "flash_partition.hpp"
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>  // for std::istringstream
#include <string>
#include <string_view>
#include <vector>
#include "../boot/tools.hpp"
#include "../log.hpp"
#include "../utils.hpp"
#include "picosha2.h"

// miniz is header-only in this context or linked
#define MINIZ_HEADER_FILE_ONLY
#include "miniz.h"

namespace ksud::flash {

namespace fs = std::filesystem;

namespace {

// Helper wrapper to match my previous logic
ExecResult exec_command_sync(const std::vector<std::string>& args) {
    return exec_command(args);
}

// Helper: Convert bytes to hex string
std::string bytes_to_hex(const unsigned char* data, size_t len) {
    static constexpr std::string_view hex_chars = "0123456789abcdef";
    std::string result;
    result.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        result.push_back(hex_chars[(data[i] >> 4) & 0xF]);
        result.push_back(hex_chars[data[i] & 0xF]);
    }
    return result;
}

// Helper: Get file size (handles both regular files and block devices)
uint64_t get_file_size(const std::string& path) {
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) {
        LOGE("Failed to stat %s: %s", path.c_str(), strerror(errno));
        return 0;
    }

    // For block devices, use ioctl to get the size
    if (S_ISBLK(st.st_mode)) {
        const int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            LOGE("Failed to open block device %s: %s", path.c_str(), strerror(errno));
            return 0;
        }

        uint64_t size = 0;
        if (ioctl(fd, BLKGETSIZE64, &size) < 0) {
            LOGE("Failed to get block device size for %s: %s", path.c_str(), strerror(errno));
            close(fd);
            return 0;
        }

        close(fd);
        LOGD("Block device %s size: %lu bytes", path.c_str(), (unsigned long)size);
        return size;
    }

    // For regular files, use st_size
    return st.st_size;
}

// Helper: Execute command and get output
std::string exec_cmd(const std::string& cmd) {
    auto result = exec_command_sync({"/system/bin/sh", "-c", cmd});
    return trim(result.stdout_str);
}

}  // namespace

std::string get_current_slot_suffix() {
    auto result = exec_command_sync({"getprop", "ro.boot.slot_suffix"});
    return trim(result.stdout_str);
}

bool is_ab_device() {
    auto result = exec_command_sync({"getprop", "ro.build.ab_update"});
    if (trim(result.stdout_str) != "true") {
        return false;
    }
    return !get_current_slot_suffix().empty();
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) partition_name then slot_suffix
std::string find_partition_block_device(const std::string& partition_name,
                                        const std::string& slot_suffix) {
    // 检查分区名是否以 _a 或 _b 结尾（slotful分区）
    bool is_slotful = false;
    if (partition_name.length() >= 2) {
        const std::string last_two = partition_name.substr(partition_name.length() - 2);
        if (last_two == "_a" || last_two == "_b") {
            is_slotful = true;
        }
    }

    // 确定要使用的槽位后缀
    std::string suffix;
    if (is_slotful) {
        // 如果分区名本身已经带了槽位后缀，不再添加
        suffix = "";
    } else if (!slot_suffix.empty()) {
        // 使用传入的槽位后缀
        suffix = slot_suffix;
    } else {
        // 使用当前槽位
        suffix = get_current_slot_suffix();
    }

    // Build candidate names
    std::vector<std::string> names_to_try;
    // 总是先尝试不带后缀的名字（slotless分区、或者已经带后缀的分区名）
    names_to_try.push_back(partition_name);
    // 如果分区名本身不带_a/_b后缀，且在AB设备上，再尝试带槽位后缀的版本
    if (!suffix.empty() && !is_slotful) {
        names_to_try.push_back(partition_name + suffix);
    }

    // Try multiple common locations
    const std::vector<std::string> base_paths = {
        "/dev/block/by-name/",
        "/dev/block/mapper/",
        "/dev/block/bootdevice/by-name/",
    };

    for (const auto& name : names_to_try) {
        for (const auto& base : base_paths) {
            const std::string path = base + name;
            if (fs::exists(path)) {
                LOGD("Found partition %s at %s", partition_name.c_str(), path.c_str());
                return path;
            }
        }
    }

    LOGW("Partition %s not found", partition_name.c_str());
    return "";
}

bool is_partition_logical(const std::string& partition_name) {
    // Check if partition is in mapper (logical partitions use device-mapper)
    const std::string block_dev = find_partition_block_device(partition_name);
    if (block_dev.empty()) {
        return false;
    }

    // Logical partitions are typically in /dev/block/mapper
    return block_dev.find("/dev/block/mapper/") == 0;
}

PartitionInfo get_partition_info(const std::string& partition_name,
                                 const std::string& slot_suffix) {
    PartitionInfo info;
    info.name = partition_name;
    info.block_device = find_partition_block_device(partition_name, slot_suffix);
    info.exists = !info.block_device.empty() && fs::exists(info.block_device);

    // 基于实际的块设备路径判断是否为逻辑分区
    info.is_logical =
        !info.block_device.empty() && info.block_device.find("/dev/block/mapper/") == 0;

    if (info.exists) {
        info.size = get_file_size(info.block_device);
    } else {
        info.size = 0;
    }

    return info;
}

std::vector<std::string> get_all_partitions(const std::string& slot_suffix) {
    std::vector<std::string> partitions;
    const std::string suffix = slot_suffix.empty() ? get_current_slot_suffix() : slot_suffix;

    // Scan /dev/block/by-name directory for physical partitions
    const std::string by_name_dir = "/dev/block/by-name";
    if (fs::exists(by_name_dir)) {
        for (const auto& entry : fs::directory_iterator(by_name_dir)) {
            std::string name = entry.path().filename().string();

            // 只有当名字确实以 _a 或 _b 结尾时才去掉槽位后缀
            if (!suffix.empty() && name.length() > 2) {
                const std::string last_two = name.substr(name.length() - 2);
                if (last_two == "_a" || last_two == "_b") {
                    // 确认这确实是槽位后缀
                    if (last_two == suffix) {
                        name = name.substr(0, name.length() - 2);
                    } else {
                        // 不是当前槽位的分区，跳过
                        continue;
                    }
                }
            }

            // Avoid duplicates
            if (std::find(partitions.begin(), partitions.end(), name) == partitions.end()) {
                partitions.push_back(name);
            }
        }
    } else {
        LOGW("Directory %s does not exist", by_name_dir.c_str());
    }

    // Scan /dev/block/mapper directory for logical partitions
    const std::string mapper_dir = "/dev/block/mapper";
    if (fs::exists(mapper_dir)) {
        for (const auto& entry : fs::directory_iterator(mapper_dir)) {
            std::string name = entry.path().filename().string();

            // Skip control devices and virtual partitions
            if (name == "control" || name.find("loop") == 0 ||
                name.find("-verity") != std::string::npos ||
                name.find("-cow") != std::string::npos) {
                continue;
            }

            // 只有当名字确实以 _a 或 _b 结尾时才去掉槽位后缀
            if (!suffix.empty() && name.length() > 2) {
                const std::string last_two = name.substr(name.length() - 2);
                if (last_two == "_a" || last_two == "_b") {
                    // 确认这确实是槽位后缀
                    if (last_two == suffix) {
                        name = name.substr(0, name.length() - 2);
                    } else {
                        // 不是当前槽位的分区，跳过
                        continue;
                    }
                }
            }

            // Avoid duplicates
            if (std::find(partitions.begin(), partitions.end(), name) == partitions.end()) {
                partitions.push_back(name);
            }
        }
    }

    // Sort alphabetically
    std::sort(partitions.begin(), partitions.end());

    LOGD("Found %zu partitions in total", partitions.size());
    return partitions;
}

bool is_dangerous_partition(const std::string& partition_name) {
    return std::any_of(std::begin(DANGEROUS_PARTITIONS), std::end(DANGEROUS_PARTITIONS),
                       [&partition_name](const char* p) { return partition_name == p; });
}

bool is_excluded_from_batch(const std::string& partition_name) {
    return std::any_of(std::begin(EXCLUDED_FROM_BATCH), std::end(EXCLUDED_FROM_BATCH),
                       [&partition_name](const char* p) { return partition_name == p; });
}

std::vector<std::string> get_available_partitions(bool scan_all) {
    std::vector<std::string> available;
    const std::string slot_suffix = get_current_slot_suffix();

    if (scan_all) {
        // Scan all partitions from /dev/block/by-name
        auto all_partitions = get_all_partitions(slot_suffix);
        for (const auto& name : all_partitions) {
            const std::string block_dev = find_partition_block_device(name, slot_suffix);
            if (!block_dev.empty() && fs::exists(block_dev)) {
                available.push_back(name);
            }
        }
    } else {
        // Only check common partitions (silently skip if not found)
        for (const char* name : COMMON_PARTITIONS) {
            const std::string block_dev = find_partition_block_device(name, slot_suffix);
            if (!block_dev.empty() && fs::exists(block_dev)) {
                available.push_back(name);
            }
            // 找不到也不报错，有些设备可能没有某些常用分区（如recovery）
        }
    }

    return available;
}

std::string flash_physical_partition(const std::string& image_path, const std::string& block_device,
                                     bool verify_hash) {
    LOGI("Flashing %s to %s (physical)", image_path.c_str(), block_device.c_str());

    if (!fs::exists(image_path)) {
        LOGE("Image file not found: %s", image_path.c_str());
        return "";
    }

    if (!fs::exists(block_device)) {
        LOGE("Block device not found: %s", block_device.c_str());
        return "";
    }

    // Check sizes
    const uint64_t image_size = get_file_size(image_path);
    const uint64_t partition_size = get_file_size(block_device);

    if (image_size > partition_size) {
        LOGE("Image size (%lu) exceeds partition size (%lu)", image_size, partition_size);
        return "";
    }

    // Zero out partition if image is smaller
    if (image_size < partition_size) {
        LOGD("Zeroing partition before flash");
        auto cmd = "dd bs=4096 if=/dev/zero of=" + block_device + " 2>/dev/null && sync";
        exec_cmd(cmd);
    }

    // Flash with dd and compute SHA256
    std::ifstream input(image_path, std::ios::binary);
    if (!input) {
        LOGE("Failed to open image file");
        return "";
    }

    const int fd = open(block_device.c_str(), O_WRONLY | O_SYNC);
    if (fd < 0) {
        LOGE("Failed to open block device for writing: %s", strerror(errno));
        return "";
    }

    std::vector<unsigned char> hash_data;
    std::array<char, 4096> buffer{};
    std::string hash;
    bool success = true;

    while (input.read(buffer.data(), buffer.size()) || input.gcount() > 0) {
        const size_t bytes_read = input.gcount();

        // Accumulate for hash
        if (verify_hash) {
            hash_data.insert(hash_data.end(), buffer.data(), buffer.data() + bytes_read);
        }

        // Write to partition
        const ssize_t bytes_written = write(fd, buffer.data(), bytes_read);
        if (bytes_written != static_cast<ssize_t>(bytes_read)) {
            LOGE("Write failed: %s", strerror(errno));
            success = false;
            break;
        }
    }

    fsync(fd);
    close(fd);
    input.close();

    if (success && verify_hash) {
        hash = picosha2::hash256_hex_string(hash_data);
        LOGI("Flash complete, SHA256: %s", hash.c_str());
    } else if (success) {
        hash = "success";
        LOGI("Flash complete (no verification)");
    }

    sync();
    return hash;
}

std::string flash_logical_partition(const std::string& image_path,
                                    const std::string& partition_name,
                                    const std::string& slot_suffix, bool verify_hash) {
    LOGI("Flashing %s to %s%s (logical)", image_path.c_str(), partition_name.c_str(),
         slot_suffix.c_str());

    const uint64_t image_size = get_file_size(image_path);
    if (image_size == 0) {
        LOGE("Invalid image file: %s", image_path.c_str());
        return "";
    }

    const std::string full_partition = partition_name + slot_suffix;
    const std::string temp_partition = partition_name + "_kf";

    // Try to create temporary partition
    LOGD("Creating temporary partition %s", temp_partition.c_str());
    auto cmd = "lptools create " + temp_partition + " " + std::to_string(image_size);
    if (exec_cmd(cmd).find("Created") == std::string::npos) {
        LOGW("Failed to create temp partition, trying resize method");

        // Fallback: resize existing partition
        cmd = "lptools unmap " + full_partition;
        if (exec_cmd(cmd).empty()) {
            LOGE("Failed to unmap %s", full_partition.c_str());
            return "";
        }

        cmd = "lptools resize " + full_partition + " " + std::to_string(image_size);
        if (exec_cmd(cmd).empty()) {
            LOGE("Failed to resize %s", full_partition.c_str());
            return "";
        }

        cmd = "lptools map " + full_partition;
        if (exec_cmd(cmd).empty()) {
            LOGE("Failed to remap %s", full_partition.c_str());
            return "";
        }

        const std::string block_dev = "/dev/block/mapper/" + full_partition;
        return flash_physical_partition(image_path, block_dev, verify_hash);
    }

    // Unmap and remap temp partition
    exec_cmd("lptools unmap " + temp_partition);
    exec_cmd("lptools map " + temp_partition);

    const std::string temp_block_dev = "/dev/block/mapper/" + temp_partition;
    std::string hash = flash_physical_partition(image_path, temp_block_dev, verify_hash);

    if (hash.empty()) {
        LOGE("Failed to flash temporary partition");
        exec_cmd("lptools remove " + temp_partition);
        return "";
    }

    // Replace original partition with temp
    LOGD("Replacing %s with %s", full_partition.c_str(), temp_partition.c_str());
    cmd = "lptools replace " + temp_partition + " " + full_partition;
    if (exec_cmd(cmd).empty()) {
        LOGE("Failed to replace partition");
        exec_cmd("lptools remove " + temp_partition);
        return "";
    }

    return hash;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) image_path vs partition_name are distinct
bool flash_partition(const std::string& image_path, const std::string& partition_name,
                     const std::string& slot_suffix, bool verify_hash) {
    // Use provided slot, or auto-detect if empty
    const std::string suffix = slot_suffix.empty() ? get_current_slot_suffix() : slot_suffix;

    const PartitionInfo info = get_partition_info(partition_name, suffix);
    if (!info.exists) {
        LOGE("Partition %s not found", partition_name.c_str());
        return false;
    }

    std::string hash;
    if (info.is_logical) {
        hash = flash_logical_partition(image_path, partition_name, suffix, verify_hash);
    } else {
        hash = flash_physical_partition(image_path, info.block_device, verify_hash);
    }

    return !hash.empty();
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) partition_name vs output_path are distinct
bool backup_partition(const std::string& partition_name, const std::string& output_path,
                      const std::string& slot_suffix) {
    // Use provided slot, or auto-detect if empty
    const std::string suffix = slot_suffix.empty() ? get_current_slot_suffix() : slot_suffix;

    const PartitionInfo info = get_partition_info(partition_name, suffix);
    if (!info.exists) {
        LOGE("Partition %s not found", partition_name.c_str());
        return false;
    }

    LOGI("Backing up %s to %s", partition_name.c_str(), output_path.c_str());

    // Use dd for backup
    const std::string cmd =
        "dd if=" + info.block_device + " of=" + output_path + " bs=4096 2>/dev/null && sync";
    (void)exec_cmd(cmd);

    if (fs::exists(output_path) && get_file_size(output_path) > 0) {
        LOGI("Backup complete: %s", output_path.c_str());
        return true;
    }

    LOGE("Backup failed");
    return false;
}

bool map_logical_partitions(const std::string& slot_suffix) {
    LOGI("Mapping logical partitions for slot %s", slot_suffix.c_str());

    // Get all partitions from mapper directory
    const std::string mapper_dir = "/dev/block/mapper";
    if (!fs::exists(mapper_dir)) {
        LOGE("Mapper directory does not exist");
        return false;
    }

    std::vector<std::string> logical_partitions;
    for (const auto& entry : fs::directory_iterator(mapper_dir)) {
        const std::string name = entry.path().filename().string();

        // Skip control devices
        if (name == "control" || name.find("loop") == 0) {
            continue;
        }

        // Only consider partitions for the target slot
        if (!slot_suffix.empty() && name.length() > slot_suffix.length()) {
            const size_t pos = name.find(slot_suffix);
            if (pos != std::string::npos && pos == name.length() - slot_suffix.length()) {
                logical_partitions.push_back(name);
            }
        }
    }

    if (logical_partitions.empty()) {
        LOGW("No logical partitions found for slot %s", slot_suffix.c_str());
    }

    // Try to map logical partitions using lptools/dmctl
    int success_count = 0;
    int total_count = 0;

    // Super partition is slotless, don't pass slot_suffix
    const std::string super_device = find_partition_block_device("super", "");
    if (super_device.empty()) {
        LOGW("Super partition not found");
    } else {
        LOGI("Super partition: %s", super_device.c_str());
    }

    // Get list of all potential logical partitions
    const std::array<const char*, 7> common_logical = {
        "system", "vendor", "product", "odm", "system_ext", "vendor_dlkm", "odm_dlkm"};

    for (const char* part_base : common_logical) {
        const std::string part_name = std::string(part_base) + slot_suffix;
        total_count++;

        // Check if already mapped
        const std::string mapped_path = "/dev/block/mapper/" + part_name;
        if (fs::exists(mapped_path)) {
            LOGD("Partition %s already mapped", part_name.c_str());
            success_count++;
            continue;
        }

        // Try to map using dmctl (device-mapper control)
        const std::string cmd = "dmctl create " + part_name;
        (void)exec_cmd(cmd);

        if (fs::exists(mapped_path)) {
            LOGI("Successfully mapped %s", part_name.c_str());
            success_count++;
        } else {
            LOGD("Could not map %s (may not exist)", part_name.c_str());
        }
    }

    LOGI("Mapped %d/%d logical partitions for slot %s", success_count, total_count,
         slot_suffix.c_str());
    return success_count > 0;
}

std::string get_avb_status() {
    // Check AVB flags in vbmeta
    const std::string vbmeta_device = find_partition_block_device("vbmeta", "");
    if (vbmeta_device.empty()) {
        LOGW("vbmeta partition not found");
        return "";
    }

    // Read vbmeta header flags (offset 123-126)
    const int fd = open(vbmeta_device.c_str(), O_RDONLY);
    if (fd < 0) {
        LOGE("Failed to open vbmeta: %s", strerror(errno));
        return "";
    }

    std::array<unsigned char, 4> flags{};
    if (lseek(fd, 123, SEEK_SET) != 123 || read(fd, flags.data(), flags.size()) != 4) {
        LOGE("Failed to read vbmeta flags");
        close(fd);
        return "";
    }
    close(fd);

    // Check if verification is disabled (flags = 2 or 3)
    if (flags[0] == 0 && flags[1] == 0 && flags[2] == 0 && (flags[3] == 2 || flags[3] == 3)) {
        return "disabled";
    }

    return "enabled";
}

bool patch_vbmeta_disable_verification() {
    const std::string vbmeta_device = find_partition_block_device("vbmeta", "");
    if (vbmeta_device.empty()) {
        LOGE("vbmeta partition not found");
        return false;
    }

    LOGI("Patching vbmeta to disable verification: %s", vbmeta_device.c_str());

    const int fd = open(vbmeta_device.c_str(), O_RDWR);
    if (fd < 0) {
        LOGE("Failed to open vbmeta: %s", strerror(errno));
        return false;
    }

    // Set flags at offset 123 to disable verification (value = 3)
    std::array<unsigned char, 4> flags = {0, 0, 0, 3};

    if (lseek(fd, 123, SEEK_SET) != 123 || write(fd, flags.data(), flags.size()) != 4) {
        LOGE("Failed to write vbmeta flags");
        close(fd);
        return false;
    }

    fsync(fd);
    close(fd);
    sync();

    LOGI("vbmeta patched successfully");
    return true;
}

std::string get_kernel_version(const std::string& slot_suffix) {
    std::string boot_partition_name = "boot";
    if (!find_partition_block_device("init_boot", slot_suffix).empty()) {
        boot_partition_name = "init_boot";
    }

    const std::string device = find_partition_block_device(boot_partition_name, slot_suffix);
    if (device.empty()) {
        LOGE("Could not find boot partition device for slot '%s'", slot_suffix.c_str());
        return "";
    }

    LOGI("Reading kernel version from partition: %s (%s)", device.c_str(),
         boot_partition_name.c_str());

    // Create a temporary directory for unpacking
    std::array<char, 64> tmp_dir_template{};
    (void)strcpy(tmp_dir_template.data(), "/data/local/tmp/ksu_unpack_XXXXXX");
    if (mkdtemp(tmp_dir_template.data()) == nullptr) {
        LOGE("Failed to create temp directory: %s", strerror(errno));
        return "";
    }
    const std::string workdir = tmp_dir_template.data();

    // Unpack boot image in the workdir (magiskboot runs in-process)
    const std::string kernel_path = workdir + "/kernel";
    auto unpack_result = exec_command_magiskboot({"unpack", device}, workdir);

    std::string result;
    if (unpack_result.exit_code == 0) {
        LOGI("Boot image unpacked successfully");

        // Try using strings command first (most efficient)
        auto strings_result = exec_command_sync({"strings", kernel_path});
        if (strings_result.exit_code == 0) {
            std::istringstream iss(strings_result.stdout_str);
            std::string line;
            while (std::getline(iss, line)) {
                if (line.find("Linux version ") != std::string::npos) {
                    result = line;
                    LOGI("Found kernel version: %s", result.c_str());
                    break;
                }
            }
        }

        // Fallback: read kernel file directly if strings failed
        if (result.empty()) {
            LOGW("strings command failed, reading kernel file directly");
            std::ifstream kernel_file(kernel_path, std::ios::binary);
            if (kernel_file) {
                const std::string search_str = "Linux version ";
                std::array<char, 4096> buffer{};
                std::string content_buffer;
                const size_t max_bytes =
                    static_cast<size_t>(64) * 1024 * 1024;  // Limit to first 64MB
                size_t total_read = 0;

                while (total_read < max_bytes && kernel_file.read(buffer.data(), buffer.size())) {
                    const size_t bytes_read = kernel_file.gcount();
                    total_read += bytes_read;
                    content_buffer.append(buffer.data(), bytes_read);

                    const size_t pos = content_buffer.find(search_str);
                    if (pos != std::string::npos) {
                        size_t end_pos = content_buffer.find('\0', pos);
                        if (end_pos == std::string::npos) {
                            end_pos = content_buffer.find('\n', pos);
                        }
                        if (end_pos != std::string::npos) {
                            result = content_buffer.substr(pos, end_pos - pos);
                            LOGI("Found kernel version: %s", result.c_str());
                            break;
                        }
                    }
                    // Keep last part of buffer to handle version string across chunks
                    if (content_buffer.length() > search_str.length() + 256) {
                        content_buffer = content_buffer.substr(content_buffer.length() - 256);
                    }
                }
                kernel_file.close();
            } else {
                LOGE("Failed to open kernel file: %s", kernel_path.c_str());
            }
        }
    } else {
        LOGE("magiskboot unpack failed with code %d: %s", unpack_result.exit_code,
             unpack_result.stderr_str.c_str());
        LOGE("stdout: %s", unpack_result.stdout_str.c_str());
    }

    // Cleanup
    exec_command_sync({"rm", "-rf", workdir});

    if (result.empty()) {
        LOGE("Failed to get kernel version");
    }

    return result;
}

std::string get_boot_slot_info() {
    if (!is_ab_device()) {
        return "{\"is_ab\":false}";
    }

    const std::string current_slot = get_current_slot_suffix();
    const std::string other_slot = (current_slot == "_a") ? "_b" : "_a";

    // Get slot info from properties
    auto result_a = exec_command_sync({"getprop", "ro.boot.slot_suffix"});
    auto unbootable = exec_command_sync({"getprop", "ro.boot.slot.unbootable"});
    auto successful = exec_command_sync({"getprop", "ro.boot.slot.successful"});

    std::string json = "{";
    json += "\"is_ab\":true,";
    json += "\"current_slot\":\"" + current_slot + "\",";
    json += "\"other_slot\":\"" + other_slot + "\"";
    json += "}";

    return json;
}

}  // namespace ksud::flash
