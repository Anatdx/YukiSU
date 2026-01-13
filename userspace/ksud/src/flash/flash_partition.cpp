#include "flash_partition.hpp"
#include "../defs.hpp"
#include "../log.hpp"
#include "../utils.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
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

// Helper: Get file size
static uint64_t get_file_size(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        return 0;
    }
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
    std::string suffix = slot_suffix.empty() ? get_current_slot_suffix() : slot_suffix;
    std::string full_name = partition_name + suffix;

    // Try multiple common locations
    std::vector<std::string> candidates = {
        "/dev/block/by-name/" + full_name,
        "/dev/block/mapper/" + full_name,
        "/dev/block/bootdevice/by-name/" + full_name,
    };

    for (const auto& path : candidates) {
        if (fs::exists(path)) {
            LOGD("Found partition %s at %s", partition_name.c_str(), path.c_str());
            return path;
        }
    }

    LOGW("Partition %s not found", full_name.c_str());
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
    info.is_logical = is_partition_logical(partition_name);

    if (info.exists) {
        info.size = get_file_size(info.block_device);
    } else {
        info.size = 0;
    }

    return info;
}

std::vector<std::string> get_available_partitions() {
    std::vector<std::string> available;
    std::string slot_suffix = get_current_slot_suffix();

    for (const char* name : PARTITION_NAMES) {
        std::string block_dev = find_partition_block_device(name, slot_suffix);
        if (!block_dev.empty() && fs::exists(block_dev)) {
            available.push_back(name);
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

}  // namespace flash
}  // namespace ksud
