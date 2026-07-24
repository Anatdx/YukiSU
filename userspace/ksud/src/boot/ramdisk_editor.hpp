#pragma once

#include <string>

namespace ksud {

// Runs one persistent CPIO document session. Requests are read from input_fd
// and versioned binary responses are written to output_fd.
int run_ramdisk_editor(const std::string& archive_path, int input_fd, int output_fd);

// Opens a boot/init_boot/vendor_boot image, edits each CPIO fragment in memory,
// and writes one rebuilt image only when the client sends DUMP.
int run_boot_ramdisk_editor(const std::string& source_image_path,
                            const std::string& output_image_path, int input_fd, int output_fd);

}  // namespace ksud
