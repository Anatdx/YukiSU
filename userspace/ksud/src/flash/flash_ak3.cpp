#include "flash_ak3.hpp"
#include "../defs.hpp"
#include "../log.hpp"
#include "../utils.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>

namespace fs = std::filesystem;

namespace ksud {

// Work directory for AK3 flash operations
static constexpr const char* AK3_WORK_DIR = "/data/adb/ksu/tmp/ak3_flash";

/**
 * Check if device is A/B partitioned
 */
static bool is_ab_device() {
    auto result = exec_command({"getprop", "ro.build.ab_update"});
    if (result.exit_code != 0)
        return false;

    std::string value = trim(result.stdout_str);
    if (value != "true")
        return false;

    result = exec_command({"getprop", "ro.boot.slot_suffix"});
    return !trim(result.stdout_str).empty();
}

/**
 * Get current slot suffix
 */
static std::string get_current_slot() {
    auto result = exec_command({"getprop", "ro.boot.slot_suffix"});
    return trim(result.stdout_str);
}

/**
 * Set slot suffix temporarily via resetprop
 */
static bool set_slot_suffix(const std::string& slot) {
    std::string suffix = slot;
    if (!suffix.empty() && suffix[0] != '_') {
        suffix = "_" + slot;
    }
    auto result = exec_command({"resetprop", "-n", "ro.boot.slot_suffix", suffix});
    return result.exit_code == 0;
}

/**
 * Clean up work directory
 */
static void cleanup_workdir() {
    std::error_code ec;
    fs::remove_all(AK3_WORK_DIR, ec);
}

/**
 * Extract update-binary from AK3 zip
 */
static bool extract_update_binary(const std::string& zip_path, const std::string& workdir) {
    std::string binary_path = workdir + "/META-INF/com/google/android/update-binary";

    // Create parent directories
    fs::create_directories(workdir + "/META-INF/com/google/android");

    // Use unzip to extract update-binary
    auto result =
        exec_command({"unzip", "-o", "-j", zip_path, "META-INF/com/google/android/update-binary",
                      "-d", workdir + "/META-INF/com/google/android"});

    if (result.exit_code != 0) {
        LOGE("Failed to extract update-binary: %s", result.stderr_str.c_str());
        return false;
    }

    // Make it executable
    chmod(binary_path.c_str(), 0755);

    return fs::exists(binary_path);
}

/**
 * Check if zip contains AK3 structure
 */
bool is_ak3_package(const std::string& zip_path) {
    // Check for update-binary and anykernel.sh
    auto result = exec_command({"unzip", "-l", zip_path});

    if (result.exit_code != 0)
        return false;

    bool has_binary = result.stdout_str.find("update-binary") != std::string::npos;
    bool has_script = result.stdout_str.find("anykernel.sh") != std::string::npos;

    return has_binary && has_script;
}

/**
 * Get AK3 package info
 */
std::string get_ak3_info(const std::string& zip_path) {
    if (!is_ak3_package(zip_path)) {
        return "";
    }

    // Try to extract anykernel.sh and parse kernel info
    std::string temp_dir = "/data/local/tmp/ak3_info_" + std::to_string(getpid());
    fs::create_directories(temp_dir);

    auto result = exec_command({"unzip", "-o", "-j", zip_path, "anykernel.sh", "-d", temp_dir});

    std::string info;
    if (result.exit_code == 0) {
        std::ifstream script(temp_dir + "/anykernel.sh");
        std::string line;
        std::regex name_regex(R"(kernel\.string=(.+))");
        std::regex device_regex(R"(device\.name\d*=(.+))");

        std::smatch match;
        std::string kernel_name, devices;

        while (std::getline(script, line)) {
            if (std::regex_search(line, match, name_regex)) {
                kernel_name = match[1].str();
            } else if (std::regex_search(line, match, device_regex)) {
                if (!devices.empty())
                    devices += ", ";
                devices += match[1].str();
            }
        }

        if (!kernel_name.empty()) {
            info = kernel_name;
            if (!devices.empty()) {
                info += " (devices: " + devices + ")";
            }
        }
    }

    fs::remove_all(temp_dir);
    return info;
}

/**
 * Flash AK3 zip package
 */
Ak3FlashResult flash_ak3(const Ak3FlashConfig& config, Ak3LogCallback log_callback,
                         Ak3ProgressCallback progress_callback) {
    Ak3FlashResult result;

    auto log = [&](const std::string& msg) {
        result.logs.push_back(msg);
        if (log_callback) {
            log_callback(msg);
        }
        if (config.verbose) {
            printf("%s\n", msg.c_str());
            fflush(stdout);
        }
    };

    auto progress = [&](float p, const std::string& step) {
        if (progress_callback) {
            progress_callback(p, step);
        }
        if (config.verbose) {
            printf("[%3.0f%%] %s\n", p * 100, step.c_str());
            fflush(stdout);
        }
    };

    // Validate input
    if (!fs::exists(config.zip_path)) {
        result.error = "Zip file not found: " + config.zip_path;
        return result;
    }

    if (!is_ak3_package(config.zip_path)) {
        result.error = "Not a valid AnyKernel3 package";
        return result;
    }

    progress(0.05f, "Preparing...");
    log("Starting AnyKernel3 flash");
    log("Package: " + config.zip_path);

    // Clean up work directory
    cleanup_workdir();
    fs::create_directories(AK3_WORK_DIR);

    progress(0.1f, "Copying zip file...");

    // Copy zip to work directory
    std::string work_zip = std::string(AK3_WORK_DIR) + "/kernel.zip";
    {
        std::ifstream src(config.zip_path, std::ios::binary);
        std::ofstream dst(work_zip, std::ios::binary);
        dst << src.rdbuf();
    }

    if (!fs::exists(work_zip)) {
        result.error = "Failed to copy zip file";
        cleanup_workdir();
        return result;
    }

    progress(0.2f, "Extracting update-binary...");

    // Extract update-binary
    if (!extract_update_binary(work_zip, AK3_WORK_DIR)) {
        result.error = "Failed to extract update-binary";
        cleanup_workdir();
        return result;
    }

    std::string binary_path =
        std::string(AK3_WORK_DIR) + "/META-INF/com/google/android/update-binary";

    // Handle A/B slot selection
    std::string original_slot;
    bool need_restore_slot = false;

    if (is_ab_device() && config.slot.has_value()) {
        progress(0.25f, "Setting target slot...");
        original_slot = get_current_slot();

        std::string target_slot = config.slot.value();
        if (target_slot != "a" && target_slot != "b") {
            result.error = "Invalid slot: " + target_slot + " (must be 'a' or 'b')";
            cleanup_workdir();
            return result;
        }

        log("Original slot: " + original_slot);
        log("Target slot: _" + target_slot);

        if (set_slot_suffix(target_slot)) {
            need_restore_slot = true;
        } else {
            log("Warning: Failed to set target slot");
        }
    }

    progress(0.3f, "Flashing kernel...");
    log("Executing update-binary...");

    // Execute update-binary
    // AK3 update-binary expects: update-binary <api> <fd> <zip>
    // where <api> is recovery API version, <fd> is output FD

    int pipefd[2];
    if (pipe(pipefd) == -1) {
        result.error = "Failed to create pipe";
        cleanup_workdir();
        return result;
    }

    pid_t pid = fork();
    if (pid == -1) {
        result.error = "Failed to fork";
        close(pipefd[0]);
        close(pipefd[1]);
        cleanup_workdir();
        return result;
    }

    if (pid == 0) {
        // Child process
        close(pipefd[0]);  // Close read end

        // Set environment variables
        setenv("POSTINSTALL", AK3_WORK_DIR, 1);
        setenv("ZIPFILE", work_zip.c_str(), 1);
        setenv("OUTFD", std::to_string(pipefd[1]).c_str(), 1);

        // Write slot to file if specified
        if (config.slot.has_value()) {
            std::ofstream slot_file(std::string(AK3_WORK_DIR) + "/bootslot");
            slot_file << config.slot.value();
        }

        // Execute update-binary
        // Args: update-binary <api_version> <output_fd> <zip_path>
        execl("/system/bin/sh", "sh", binary_path.c_str(), "3", std::to_string(pipefd[1]).c_str(),
              work_zip.c_str(), nullptr);

        _exit(127);
    }

    // Parent process
    close(pipefd[1]);  // Close write end

    // Read output from update-binary
    char buf[4096];
    ssize_t n;
    std::string line_buffer;

    while ((n = read(pipefd[0], buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        line_buffer += buf;

        // Process complete lines
        size_t pos;
        while ((pos = line_buffer.find('\n')) != std::string::npos) {
            std::string line = line_buffer.substr(0, pos);
            line_buffer.erase(0, pos + 1);

            // Parse ui_print lines
            if (line.rfind("ui_print", 0) == 0) {
                std::string msg = line.substr(8);
                // Trim leading space
                if (!msg.empty() && msg[0] == ' ')
                    msg.erase(0, 1);
                log(msg);

                // Update progress based on keywords
                if (msg.find("extracting") != std::string::npos ||
                    msg.find("Extracting") != std::string::npos) {
                    progress(0.5f, "Extracting...");
                } else if (msg.find("installing") != std::string::npos ||
                           msg.find("Installing") != std::string::npos ||
                           msg.find("Flashing") != std::string::npos) {
                    progress(0.7f, "Installing...");
                } else if (msg.find("complete") != std::string::npos ||
                           msg.find("Complete") != std::string::npos ||
                           msg.find("Done") != std::string::npos) {
                    progress(0.9f, "Completing...");
                }
            }
        }
    }

    close(pipefd[0]);

    // Wait for child
    int status;
    waitpid(pid, &status, 0);

    // Restore original slot if needed
    if (need_restore_slot && !original_slot.empty()) {
        progress(0.95f, "Restoring original slot...");
        set_slot_suffix(original_slot);
        log("Restored slot to: " + original_slot);
    }

    // Check result - only check exit code, AK3 doesn't create done marker
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    if (exit_code == 0) {
        progress(1.0f, "Flash complete!");
        log("Flash completed successfully");
        result.success = true;
        result.exit_code = 0;
    } else {
        result.error = "Flash failed (exit code: " + std::to_string(exit_code) + ")";
        result.exit_code = exit_code;
        log(result.error);
    }

    // Save log to file if requested
    if (config.log_file.has_value()) {
        std::ofstream log_file(config.log_file.value());
        for (const auto& l : result.logs) {
            log_file << l << "\n";
        }
        log("Log saved to: " + config.log_file.value());
    }

    // Cleanup
    cleanup_workdir();

    return result;
}

/**
 * CLI command handler for flash
 */
int cmd_flash(const std::vector<std::string>& args) {
    if (args.empty()) {
        printf("Usage: ksud flash <subcommand> [options]\n");
        printf("\n");
        printf("Subcommands:\n");
        printf("  ak3 <zip>            Flash AnyKernel3 kernel package\n");
        printf("  info <zip>           Show AK3 package info\n");
        printf("\n");
        printf("Options for 'ak3':\n");
        printf("  --slot <a|b>         Target slot for A/B devices\n");
        printf("  --log <file>         Save flash log to file\n");
        printf("  -v, --verbose        Verbose output\n");
        return 1;
    }

    const std::string& subcmd = args[0];

    if (subcmd == "ak3") {
        if (args.size() < 2) {
            printf("Usage: ksud flash ak3 <zip> [--slot a|b] [--log <file>] [-v]\n");
            return 1;
        }

        Ak3FlashConfig config;
        config.zip_path = args[1];

        // Parse options
        for (size_t i = 2; i < args.size(); i++) {
            if (args[i] == "--slot" && i + 1 < args.size()) {
                config.slot = args[++i];
            } else if (args[i] == "--log" && i + 1 < args.size()) {
                config.log_file = args[++i];
            } else if (args[i] == "-v" || args[i] == "--verbose") {
                config.verbose = true;
            }
        }

        printf("Flashing AnyKernel3 package: %s\n", config.zip_path.c_str());
        fflush(stdout);

        auto result = flash_ak3(config);

        if (result.success) {
            printf("\n✓ Flash completed successfully!\n");
            printf("Reboot to apply the new kernel.\n");
            fflush(stdout);
            return 0;
        } else {
            printf("\n✗ Flash failed: %s\n", result.error.c_str());
            fflush(stdout);
            return 1;
        }

    } else if (subcmd == "info") {
        if (args.size() < 2) {
            printf("Usage: ksud flash info <zip>\n");
            return 1;
        }

        const std::string& zip_path = args[1];

        if (!fs::exists(zip_path)) {
            printf("Error: File not found: %s\n", zip_path.c_str());
            return 1;
        }

        if (!is_ak3_package(zip_path)) {
            printf("Not an AnyKernel3 package\n");
            return 1;
        }

        std::string info = get_ak3_info(zip_path);
        if (!info.empty()) {
            printf("AnyKernel3 Package Info:\n");
            printf("  %s\n", info.c_str());
        } else {
            printf("AnyKernel3 package (no kernel info available)\n");
        }

        return 0;
    }

    printf("Unknown flash subcommand: %s\n", subcmd.c_str());
    return 1;
}

}  // namespace ksud
