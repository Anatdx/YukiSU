#include "flash_partition.hpp"
#include "../defs.hpp"
#include "../log.hpp"
#include "../utils.hpp"

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

// Use PicoSHA2 instead of OpenSSL for SHA-256
#include "picosha2.h"

namespace fs = std::filesystem;

namespace ksud {
namespace flash {

// Helper: Convert bytes to hex string
static std::string bytes_to_hex(const unsigned char* data, size_t len) {
    static const char hex_chars[] = "0123456789abcdef";
    std::string result;
    result.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        result.push_back(hex_chars[(data[i] >> 4) & 0xF]);
        result.push_back(hex_chars[data[i] & 0xF]);
    }
    return result;
}

// Helper: Get file size (handles both regular files and block devices)
static uint64_t get_file_size(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        LOGE("Failed to stat %s: %s", path.c_str(), strerror(errno));
        return 0;
    }

    // For block devices, use ioctl to get the size
    if (S_ISBLK(st.st_mode)) {
        int fd = open(path.c_str(), O_RDONLY);
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
static std::string exec_cmd(const std::string& cmd) {
    auto result = exec_command({"/system/bin/sh", "-c", cmd});
    return trim(result.stdout_str);
}

std::string get_current_slot_suffix() {
    auto result = exec_command({"getprop", "ro.boot.slot_suffix"});
    return trim(result.stdout_str);
}

bool is_ab_device() {
    auto result = exec_command({"getprop", "ro.build.ab_update"});
    if (trim(result.stdout_str) != "true") {
        return false;
    }
    return !get_current_slot_suffix().empty();
}

std::string find_partition_block_device(const std::string& partition_name,
                                        const std::string& slot_suffix) {
    // 如果传入了明确的槽位后缀，使用它；否则使用当前槽位
    std::string suffix = slot_suffix;
    if (suffix.empty()) {
        suffix = get_current_slot_suffix();
    }

    // Check if this partition should not have slot suffix
    bool is_slotless = false;
    for (const char* slotless : SLOTLESS_PARTITIONS) {
        if (partition_name == slotless) {
            is_slotless = true;
            break;
        }
    }

    // Build candidate names
    std::vector<std::string> names_to_try;
    if (!is_slotless && !suffix.empty()) {
        // 对于有槽位的分区，只尝试带槽位后缀的名字
        names_to_try.push_back(partition_name + suffix);
    } else {
        // 对于无槽位分区，只尝试不带后缀的名字
        names_to_try.push_back(partition_name);
    }

    // Try multiple common locations
    std::vector<std::string> base_paths = {
        "/dev/block/by-name/",
        "/dev/block/mapper/",
        "/dev/block/bootdevice/by-name/",
    };

    for (const auto& name : names_to_try) {
        for (const auto& base : base_paths) {
            std::string path = base + name;
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
    std::string block_dev = find_partition_block_device(partition_name);
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
    std::string suffix = slot_suffix.empty() ? get_current_slot_suffix() : slot_suffix;

    // Scan /dev/block/by-name directory for physical partitions
    std::string by_name_dir = "/dev/block/by-name";
    if (fs::exists(by_name_dir)) {
        for (const auto& entry : fs::directory_iterator(by_name_dir)) {
            std::string name = entry.path().filename().string();

            // Remove slot suffix if present
            if (!suffix.empty() && name.length() > suffix.length()) {
                size_t pos = name.find(suffix);
                if (pos != std::string::npos && pos == name.length() - suffix.length()) {
                    name = name.substr(0, pos);
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
    std::string mapper_dir = "/dev/block/mapper";
    if (fs::exists(mapper_dir)) {
        for (const auto& entry : fs::directory_iterator(mapper_dir)) {
            std::string name = entry.path().filename().string();

            // Skip control devices
            if (name == "control" || name.find("loop") == 0) {
                continue;
            }

            // Remove slot suffix if present
            if (!suffix.empty() && name.length() > suffix.length()) {
                size_t pos = name.find(suffix);
                if (pos != std::string::npos && pos == name.length() - suffix.length()) {
                    name = name.substr(0, pos);
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
    for (const char* dangerous : DANGEROUS_PARTITIONS) {
        if (partition_name == dangerous) {
            return true;
        }
    }
    return false;
}

bool is_excluded_from_batch(const std::string& partition_name) {
    for (const char* excluded : EXCLUDED_FROM_BATCH) {
        if (partition_name == excluded) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> get_available_partitions(bool scan_all) {
    std::vector<std::string> available;
    std::string slot_suffix = get_current_slot_suffix();

    if (scan_all) {
        // Scan all partitions from /dev/block/by-name
        auto all_partitions = get_all_partitions(slot_suffix);
        for (const auto& name : all_partitions) {
            std::string block_dev = find_partition_block_device(name, slot_suffix);
            if (!block_dev.empty() && fs::exists(block_dev)) {
                available.push_back(name);
            }
        }
    } else {
        // Only check common partitions
        for (const char* name : COMMON_PARTITIONS) {
            std::string block_dev = find_partition_block_device(name, slot_suffix);
            if (!block_dev.empty() && fs::exists(block_dev)) {
                available.push_back(name);
            }
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
    uint64_t image_size = get_file_size(image_path);
    uint64_t partition_size = get_file_size(block_device);

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

    int fd = open(block_device.c_str(), O_WRONLY | O_SYNC);
    if (fd < 0) {
        LOGE("Failed to open block device for writing: %s", strerror(errno));
        return "";
    }

    std::vector<unsigned char> hash_data;
    char buffer[4096];
    std::string hash;
    bool success = true;

    while (input.read(buffer, sizeof(buffer)) || input.gcount() > 0) {
        size_t bytes_read = input.gcount();

        // Accumulate for hash
        if (verify_hash) {
            hash_data.insert(hash_data.end(), buffer, buffer + bytes_read);
        }

        // Write to partition
        ssize_t bytes_written = write(fd, buffer, bytes_read);
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

    uint64_t image_size = get_file_size(image_path);
    if (image_size == 0) {
        LOGE("Invalid image file: %s", image_path.c_str());
        return "";
    }

    std::string full_partition = partition_name + slot_suffix;
    std::string temp_partition = partition_name + "_kf";

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

        std::string block_dev = "/dev/block/mapper/" + full_partition;
        return flash_physical_partition(image_path, block_dev, verify_hash);
    }

    // Unmap and remap temp partition
    exec_cmd("lptools unmap " + temp_partition);
    exec_cmd("lptools map " + temp_partition);

    std::string temp_block_dev = "/dev/block/mapper/" + temp_partition;
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

bool flash_partition(const std::string& image_path, const std::string& partition_name,
                     const std::string& slot_suffix, bool verify_hash) {
    // Use provided slot, or auto-detect if empty
    std::string suffix = slot_suffix.empty() ? get_current_slot_suffix() : slot_suffix;

    PartitionInfo info = get_partition_info(partition_name, suffix);
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

bool backup_partition(const std::string& partition_name, const std::string& output_path,
                      const std::string& slot_suffix) {
    // Use provided slot, or auto-detect if empty
    std::string suffix = slot_suffix.empty() ? get_current_slot_suffix() : slot_suffix;

    PartitionInfo info = get_partition_info(partition_name, suffix);
    if (!info.exists) {
        LOGE("Partition %s not found", partition_name.c_str());
        return false;
    }

    LOGI("Backing up %s to %s", partition_name.c_str(), output_path.c_str());

    // Use dd for backup
    auto cmd = "dd if=" + info.block_device + " of=" + output_path + " bs=4096 2>/dev/null && sync";
    auto result = exec_cmd(cmd);

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
    std::string mapper_dir = "/dev/block/mapper";
    if (!fs::exists(mapper_dir)) {
        LOGE("Mapper directory does not exist");
        return false;
    }

    std::vector<std::string> logical_partitions;
    for (const auto& entry : fs::directory_iterator(mapper_dir)) {
        std::string name = entry.path().filename().string();

        // Skip control devices
        if (name == "control" || name.find("loop") == 0) {
            continue;
        }

        // Only consider partitions for the target slot
        if (!slot_suffix.empty() && name.length() > slot_suffix.length()) {
            size_t pos = name.find(slot_suffix);
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

    // First, try to load the super partition metadata for the target slot
    std::string super_device = find_partition_block_device("super", slot_suffix);
    if (super_device.empty()) {
        LOGW("Super partition not found for slot %s", slot_suffix.c_str());
    } else {
        LOGI("Super partition: %s", super_device.c_str());
    }

    // Get list of all potential logical partitions
    const char* common_logical[] = {"system",     "vendor",      "product", "odm",
                                    "system_ext", "vendor_dlkm", "odm_dlkm"};

    for (const char* part_base : common_logical) {
        std::string part_name = std::string(part_base) + slot_suffix;
        total_count++;

        // Check if already mapped
        std::string mapped_path = "/dev/block/mapper/" + part_name;
        if (fs::exists(mapped_path)) {
            LOGD("Partition %s already mapped", part_name.c_str());
            success_count++;
            continue;
        }

        // Try to map using dmctl (device-mapper control)
        std::string cmd = "dmctl create " + part_name;
        auto result = exec_cmd(cmd);

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

}  // namespace flash
}  // namespace ksud
