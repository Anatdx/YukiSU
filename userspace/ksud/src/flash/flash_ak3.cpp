#include "flash_ak3.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../log.hpp"
#include "../utils.hpp"
#include "flash_partition.hpp"

#define MINIZ_HEADER_FILE_ONLY
#include "miniz.h"

namespace ksud::flash {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kAk3Updater = "META-INF/com/google/android/update-binary";
constexpr std::string_view kAk3Script = "anykernel.sh";
constexpr std::string_view kAk3TempRoot = "/data/adb/ksu/tmp";
constexpr std::string_view kAk3LockPath = "/data/adb/ksu/tmp/ak3-flash.lock";
constexpr size_t kMaxMetadataSize = size_t{4} * 1024U * 1024U;
constexpr mz_uint kMaxArchiveEntries = 32768U;

class ScopedFd {
public:
    explicit ScopedFd(int fd = -1) : fd_(fd) {}
    ~ScopedFd() {
        if (fd_ >= 0)
            close(fd_);
    }

    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;

    ScopedFd(ScopedFd&& other) noexcept : fd_(other.release()) {}
    ScopedFd& operator=(ScopedFd&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0)
                close(fd_);
            fd_ = other.release();
        }
        return *this;
    }

    [[nodiscard]] int get() const { return fd_; }
    [[nodiscard]] bool valid() const { return fd_ >= 0; }
    int release() {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }

private:
    int fd_;
};

class ScopedWorkDir {
public:
    explicit ScopedWorkDir(std::string path) : path_(std::move(path)) {}
    ~ScopedWorkDir() {
        if (path_.empty())
            return;
        std::error_code error;
        fs::remove_all(path_, error);
        if (error) {
            LOGW("Failed to clean AK3 work directory %s: %s", path_.c_str(),
                 error.message().c_str());
        }
    }

    ScopedWorkDir(const ScopedWorkDir&) = delete;
    ScopedWorkDir& operator=(const ScopedWorkDir&) = delete;
    ScopedWorkDir(ScopedWorkDir&& other) noexcept : path_(std::move(other.path_)) {
        other.path_.clear();
    }
    ScopedWorkDir& operator=(ScopedWorkDir&& other) noexcept {
        if (this != &other) {
            path_ = std::move(other.path_);
            other.path_.clear();
        }
        return *this;
    }

    [[nodiscard]] const std::string& path() const { return path_; }

private:
    std::string path_;
};

class ScopedFile {
public:
    explicit ScopedFile(FILE* file) : file_(file) {}
    ~ScopedFile() {
        if (file_)
            (void)std::fclose(file_);
    }

    ScopedFile(const ScopedFile&) = delete;
    ScopedFile& operator=(const ScopedFile&) = delete;
    ScopedFile(ScopedFile&&) = delete;
    ScopedFile& operator=(ScopedFile&&) = delete;

private:
    FILE* file_;
};

class ZipReader {
public:
    explicit ZipReader(const std::string& path) : fd_(open(path.c_str(), O_RDONLY | O_CLOEXEC)) {
        if (fd_ < 0) {
            error_ = "Cannot open package: " + std::string(strerror(errno));
            return;
        }

        struct stat stat_buffer{};
        if (fstat(fd_, &stat_buffer) != 0 || !S_ISREG(stat_buffer.st_mode) ||
            stat_buffer.st_size <= 0) {
            error_ = "Package is not a non-empty regular file";
            return;
        }

        archive_.m_pRead = &ZipReader::read_at;
        archive_.m_pIO_opaque = this;
        if (!mz_zip_reader_init(&archive_, static_cast<mz_uint64>(stat_buffer.st_size), 0)) {
            error_ = std::string("Invalid ZIP archive: ") +
                     mz_zip_get_error_string(mz_zip_get_last_error(&archive_));
            return;
        }
        initialized_ = true;
    }

    ~ZipReader() {
        if (initialized_)
            mz_zip_reader_end(&archive_);
        if (fd_ >= 0)
            close(fd_);
    }

    ZipReader(const ZipReader&) = delete;
    ZipReader& operator=(const ZipReader&) = delete;
    ZipReader(ZipReader&&) = delete;
    ZipReader& operator=(ZipReader&&) = delete;

    [[nodiscard]] bool valid() const { return initialized_; }
    [[nodiscard]] const std::string& error() const { return error_; }
    mz_zip_archive* archive() { return &archive_; }

private:
    static size_t read_at(void* opaque, mz_uint64 offset, void* buffer, size_t size) {
        auto* self = static_cast<ZipReader*>(opaque);
        size_t total = 0;
        while (total < size) {
            const ssize_t count = pread(self->fd_, static_cast<char*>(buffer) + total, size - total,
                                        static_cast<off_t>(offset + total));
            if (count > 0) {
                total += static_cast<size_t>(count);
                continue;
            }
            if (count < 0 && errno == EINTR)
                continue;
            break;
        }
        return total;
    }

    int fd_ = -1;
    bool initialized_ = false;
    mz_zip_archive archive_{};
    std::string error_;
};

std::string trim_copy(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string strip_shell_value(std::string value) {
    value = trim_copy(value);
    while (!value.empty() && value.back() == ';')
        value.pop_back();
    value = trim_copy(value);
    if (value.size() >= 2 && ((value.front() == '\'' && value.back() == '\'') ||
                              (value.front() == '"' && value.back() == '"'))) {
        value = value.substr(1, value.size() - 2);
    }
    return trim_copy(value);
}

bool path_is_safe(std::string name) {
    if (name.empty())
        return false;
    std::replace(name.begin(), name.end(), '\\', '/');
    if (name.front() == '/' || (name.size() >= 2 && name[1] == ':'))
        return false;

    size_t start = 0;
    while (start <= name.size()) {
        const size_t end = name.find('/', start);
        const std::string_view component(name.data() + start,
                                         (end == std::string::npos ? name.size() : end) - start);
        if (component == "..")
            return false;
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return true;
}

std::optional<std::string> archive_filename(mz_zip_archive* archive, mz_uint index) {
    const mz_uint length = mz_zip_reader_get_filename(archive, index, nullptr, 0);
    if (length == 0 || length > 64U * 1024U)
        return std::nullopt;
    std::vector<char> buffer(static_cast<size_t>(length) + 1U, '\0');
    if (mz_zip_reader_get_filename(archive, index, buffer.data(),
                                   static_cast<mz_uint>(buffer.size())) == 0) {
        return std::nullopt;
    }
    return std::string(buffer.data());
}

bool archive_entry_is_symlink(const mz_zip_archive_file_stat& stat) {
    constexpr mz_uint16 kUnixHost = 3;
    constexpr mode_t kFileTypeMask = 0170000;
    constexpr mode_t kSymlinkType = 0120000;
    const mz_uint16 host = static_cast<mz_uint16>(stat.m_version_made_by >> 8U);
    const mode_t mode = static_cast<mode_t>(stat.m_external_attr >> 16U);
    return host == kUnixHost && (mode & kFileTypeMask) == kSymlinkType;
}

struct ArchiveInspection {
    bool valid = false;
    std::string error;
    mz_uint updater_index = 0;
    mz_uint script_index = 0;
};

ArchiveInspection validate_archive(ZipReader& reader) {
    ArchiveInspection result;
    auto* archive = reader.archive();
    const mz_uint count = mz_zip_reader_get_num_files(archive);
    if (count == 0 || count > kMaxArchiveEntries) {
        result.error = "Archive has an invalid number of entries";
        return result;
    }

    unsigned int updater_count = 0;
    unsigned int script_count = 0;
    for (mz_uint index = 0; index < count; ++index) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(archive, index, &stat)) {
            result.error = "Cannot read ZIP central directory";
            return result;
        }
        const auto name = archive_filename(archive, index);
        if (!name || !path_is_safe(*name)) {
            result.error = "Archive contains an unsafe path";
            return result;
        }
        if (stat.m_is_encrypted || !stat.m_is_supported) {
            result.error = "Archive contains an encrypted or unsupported entry";
            return result;
        }
        if (archive_entry_is_symlink(stat)) {
            result.error = "Archive contains symbolic links";
            return result;
        }
        if (*name == kAk3Updater) {
            result.updater_index = index;
            ++updater_count;
        } else if (*name == kAk3Script) {
            result.script_index = index;
            ++script_count;
        }
    }

    if (updater_count != 1 || script_count != 1) {
        result.error =
            "Not an AnyKernel3 package (update-binary or anykernel.sh is missing/duplicated)";
        return result;
    }
    result.valid = true;
    return result;
}

std::optional<std::string> extract_text(mz_zip_archive* archive, mz_uint index,
                                        std::string* error) {
    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(archive, index, &stat)) {
        *error = "Cannot read archive entry metadata";
        return std::nullopt;
    }
    if (stat.m_uncomp_size == 0 || stat.m_uncomp_size > kMaxMetadataSize) {
        *error = "Archive metadata entry has an invalid size";
        return std::nullopt;
    }

    std::string content(static_cast<size_t>(stat.m_uncomp_size), '\0');
    if (!mz_zip_reader_extract_to_mem(archive, index, content.data(), content.size(), 0)) {
        *error = std::string("Cannot extract archive metadata: ") +
                 mz_zip_get_error_string(mz_zip_get_last_error(archive));
        return std::nullopt;
    }
    return content;
}

Ak3PackageInfo parse_package_info(const std::string& script) {
    Ak3PackageInfo info;
    info.valid = true;

    size_t offset = 0;
    while (offset <= script.size()) {
        const size_t end = script.find('\n', offset);
        std::string line =
            script.substr(offset, (end == std::string::npos ? script.size() : end) - offset);
        line = trim_copy(line);
        if (!line.empty() && line.front() != '#') {
            const size_t equals = line.find('=');
            if (equals != std::string::npos) {
                const std::string key = trim_copy(line.substr(0, equals));
                const std::string value = strip_shell_value(line.substr(equals + 1));
                if (key == "kernel.string") {
                    info.kernel_name = value;
                } else if (key.rfind("device.name", 0) == 0 && !value.empty()) {
                    info.devices.push_back(value);
                } else if ((key == "SLOT_SELECT" || key == "slot_select") && !value.empty()) {
                    info.package_slot_policy = value;
                }
            }
        }
        if (end == std::string::npos)
            break;
        offset = end + 1;
    }

    std::sort(info.devices.begin(), info.devices.end());
    info.devices.erase(std::unique(info.devices.begin(), info.devices.end()), info.devices.end());
    return info;
}

std::string shell_quote(const std::string& value) {
    std::string quoted = "'";
    for (const char character : value) {
        if (character == '\'') {
            quoted += "'\\''";
        } else {
            quoted.push_back(character);
        }
    }
    quoted.push_back('\'');
    return quoted;
}

std::optional<std::string> current_executable() {
    std::array<char, 4096> buffer{};
    const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1U);
    if (length <= 0 || static_cast<size_t>(length) >= buffer.size())
        return std::nullopt;
    buffer[static_cast<size_t>(length)] = '\0';
    return std::string(buffer.data());
}

bool write_executable(const fs::path& path, const std::string& content) {
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0700);
    if (fd < 0)
        return false;
    const ScopedFd scoped(fd);
    size_t written = 0;
    while (written < content.size()) {
        const ssize_t count = write(fd, content.data() + written, content.size() - written);
        if (count > 0) {
            written += static_cast<size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        return false;
    }
    return fsync(fd) == 0;
}

std::optional<ScopedWorkDir> make_work_dir() {
    std::error_code error;
    fs::create_directories(kAk3TempRoot, error);
    if (error)
        return std::nullopt;
    chmod(std::string(kAk3TempRoot).c_str(), 0700);

    std::string pattern = std::string(kAk3TempRoot) + "/ak3-flash-XXXXXX";
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    char* created = mkdtemp(buffer.data());
    if (!created)
        return std::nullopt;
    chmod(created, 0700);
    return ScopedWorkDir(created);
}

std::string normalize_slot(std::string slot) {
    slot = trim_copy(slot);
    if (slot == "a" || slot == "b")
        slot.insert(slot.begin(), '_');
    return slot;
}

void emit_line(FILE* log_file, const std::string& line, bool error = false) {
    FILE* stream = error ? stderr : stdout;
    (void)std::fprintf(stream, "%s\n", line.c_str());
    (void)std::fflush(stream);
    if (log_file) {
        (void)std::fprintf(log_file, "%s\n", line.c_str());
        (void)std::fflush(log_file);
    }
}

struct UpdaterResult {
    int exit_code = 1;
    bool embedded_ksu_failure = false;
};

bool is_unzip_chatter(const std::string& line) {
    static constexpr std::array<std::string_view, 7> prefixes = {
        "Archive:",  "inflating:", "extracting:",
        "creating:", "linking:",   "finishing deferred symbolic links:",
        "->",
    };
    const std::string trimmed = trim_copy(line);
    return std::any_of(prefixes.begin(), prefixes.end(), [&trimmed](std::string_view prefix) {
        return trimmed.rfind(prefix, 0) == 0;
    });
}

bool is_embedded_ksu_failure(const std::string& line) {
    static constexpr std::array<std::string_view, 6> failures = {
        "! Module installation failed",         "! Failed to extract binary assets",
        "! Failed to create installer wrapper", "! Failed to open installer wrapper",
        "! Failed to write installer wrapper",  "! Failed to create metamodule symlink",
    };
    const std::string trimmed = trim_copy(line);
    return std::any_of(failures.begin(), failures.end(), [&trimmed](std::string_view failure) {
        return trimmed.rfind(failure, 0) == 0;
    });
}

void emit_ak3_protocol_output(FILE* log_file, std::string line) {
    if (!line.empty() && line.back() == '\r')
        line.pop_back();

    const size_t command_start = line.find_first_not_of(" \t");
    if (command_start == std::string::npos)
        return;
    line.erase(0, command_start);

    constexpr std::string_view ui_print = "ui_print";
    if (line.rfind(ui_print, 0) == 0) {
        line.erase(0, ui_print.size());
        if (line.empty())
            return;  // AK3 emits an indented bare ui_print as a protocol terminator.
        if (line.front() == ' ')
            line.erase(0, 1);
        emit_line(log_file, trim_copy(line));
        return;
    }
    if (line.rfind("progress ", 0) == 0 || line.rfind("set_progress ", 0) == 0)
        return;

    // The update-binary fd is a command channel, not ordinary stdout. Unknown
    // recovery commands must not leak into the user-facing installation log.
    LOGD("Ignoring unsupported AnyKernel3 protocol command: %s", line.c_str());
}

void emit_ak3_diagnostic_output(FILE* log_file, std::string line, bool error,
                                bool& embedded_ksu_failure) {
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    if (line.empty() || is_unzip_chatter(line))
        return;

    const bool failure = is_embedded_ksu_failure(line);
    embedded_ksu_failure = embedded_ksu_failure || failure;
    emit_line(log_file, line, error || failure);
}

UpdaterResult run_updater(const fs::path& updater, const std::string& zip_path,
                          const std::string& work_dir, const std::string& launcher_dir,
                          const std::optional<std::string>& relative_slot, FILE* log_file) {
    std::array<int, 2> protocol_pipe{};
    if (pipe(protocol_pipe.data()) != 0) {
        emit_line(log_file, "Cannot create AK3 protocol pipe", true);
        return {};
    }
    ScopedFd protocol_read(protocol_pipe[0]);
    ScopedFd protocol_write(protocol_pipe[1]);

    std::array<int, 2> stdout_pipe{};
    if (pipe(stdout_pipe.data()) != 0) {
        emit_line(log_file, "Cannot create AK3 stdout pipe", true);
        return {};
    }
    ScopedFd stdout_read(stdout_pipe[0]);
    ScopedFd stdout_write(stdout_pipe[1]);

    std::array<int, 2> stderr_pipe{};
    if (pipe(stderr_pipe.data()) != 0) {
        emit_line(log_file, "Cannot create AK3 stderr pipe", true);
        return {};
    }
    ScopedFd stderr_read(stderr_pipe[0]);
    ScopedFd stderr_write(stderr_pipe[1]);

    const pid_t child = fork();
    if (child < 0) {
        emit_line(log_file, "Cannot start AnyKernel3", true);
        return {};
    }

    if (child == 0) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() == 1)
            _exit(125);

        const int protocol_fd = protocol_write.get();
        close(protocol_read.release());
        close(stdout_read.release());
        close(stderr_read.release());
        if (dup2(stdout_write.get(), STDOUT_FILENO) < 0 ||
            dup2(stderr_write.get(), STDERR_FILENO) < 0) {
            _exit(126);
        }
        close(stdout_write.release());
        close(stderr_write.release());

        const std::string akhome = work_dir + "/anykernel";
        const char* old_path = getenv("PATH");
        const std::string path = launcher_dir + ":" + (old_path ? old_path : "/system/bin");
        setenv("AKHOME", akhome.c_str(), 1);
        setenv("PATH", path.c_str(), 1);
        unsetenv("POSTINSTALL");
        if (relative_slot) {
            setenv("SLOT_SELECT", relative_slot->c_str(), 1);
        } else {
            unsetenv("SLOT_SELECT");
        }

        if (chdir(work_dir.c_str()) != 0)
            _exit(126);

        const std::string fd_arg = std::to_string(protocol_fd);
        execl("/system/bin/sh", "sh", updater.c_str(), "3", fd_arg.c_str(), zip_path.c_str(),
              nullptr);
        _exit(127);
    }

    protocol_write = ScopedFd();
    stdout_write = ScopedFd();
    stderr_write = ScopedFd();

    std::array<ScopedFd, 3> read_ends = {
        std::move(protocol_read),
        std::move(stdout_read),
        std::move(stderr_read),
    };
    std::array<pollfd, 3> poll_fds = {{
        {read_ends[0].get(), POLLIN, 0},
        {read_ends[1].get(), POLLIN, 0},
        {read_ends[2].get(), POLLIN, 0},
    }};
    std::array<std::string, 3> pending;
    std::array<char, 4096> buffer{};
    UpdaterResult result;
    size_t open_streams = read_ends.size();
    bool output_error = false;

    auto emit_output = [&](size_t stream, std::string line) {
        if (stream == 0) {
            emit_ak3_protocol_output(log_file, std::move(line));
        } else {
            emit_ak3_diagnostic_output(log_file, std::move(line), stream == 2,
                                       result.embedded_ksu_failure);
        }
    };

    while (open_streams > 0) {
        const int ready = poll(poll_fds.data(), poll_fds.size(), -1);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            emit_line(log_file, "Cannot read AnyKernel3 output: " + std::string(strerror(errno)),
                      true);
            output_error = true;
            (void)kill(child, SIGTERM);
            break;
        }

        for (size_t stream = 0; stream < poll_fds.size(); ++stream) {
            auto& descriptor = poll_fds[stream];
            if (descriptor.fd < 0 ||
                !(descriptor.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
                continue;
            }

            const ssize_t count = read(read_ends[stream].get(), buffer.data(), buffer.size());
            if (count > 0) {
                pending[stream].append(buffer.data(), static_cast<size_t>(count));
                size_t newline = 0;
                while ((newline = pending[stream].find('\n')) != std::string::npos) {
                    emit_output(stream, pending[stream].substr(0, newline));
                    pending[stream].erase(0, newline + 1U);
                }
                continue;
            }
            if (count < 0 && errno == EINTR)
                continue;
            if (count < 0) {
                emit_line(log_file,
                          "Cannot read AnyKernel3 output: " + std::string(strerror(errno)), true);
                output_error = true;
            }
            if (!pending[stream].empty())
                emit_output(stream, std::move(pending[stream]));
            read_ends[stream] = ScopedFd();
            descriptor.fd = -1;
            --open_streams;
        }
    }

    for (auto& read_end : read_ends)
        read_end = ScopedFd();

    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR)
            continue;
        emit_line(log_file, "Cannot collect AnyKernel3 result", true);
        return result;
    }
    if (output_error)
        return result;
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
        return result;
    }
    if (WIFSIGNALED(status)) {
        emit_line(log_file, "AnyKernel3 terminated by signal " + std::to_string(WTERMSIG(status)),
                  true);
    }
    return result;
}

}  // namespace

Ak3PackageInfo inspect_ak3_package(const std::string& zip_path) {
    ZipReader reader(zip_path);
    if (!reader.valid()) {
        Ak3PackageInfo info;
        info.error = reader.error();
        return info;
    }

    const ArchiveInspection archive = validate_archive(reader);
    if (!archive.valid) {
        Ak3PackageInfo info;
        info.error = archive.error;
        return info;
    }

    std::string error;
    const auto script = extract_text(reader.archive(), archive.script_index, &error);
    if (!script) {
        Ak3PackageInfo info;
        info.error = std::move(error);
        return info;
    }

    return parse_package_info(*script);
}

int flash_ak3_package(const Ak3FlashConfig& config) {
    const Ak3PackageInfo package = inspect_ak3_package(config.zip_path);
    if (!package.valid) {
        emit_line(nullptr, "AK3 package rejected: " + package.error, true);
        return 1;
    }

    std::error_code directory_error;
    fs::create_directories(kAk3TempRoot, directory_error);
    if (directory_error) {
        emit_line(nullptr, "Cannot create AK3 temporary root: " + directory_error.message(), true);
        return 1;
    }

    const ScopedFd lock(
        open(std::string(kAk3LockPath).c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600));
    if (!lock.valid() || flock(lock.get(), LOCK_EX | LOCK_NB) != 0) {
        emit_line(nullptr, "Another AnyKernel3 flash is already running", true);
        return 1;
    }

    auto work_dir = make_work_dir();
    if (!work_dir) {
        emit_line(nullptr, "Cannot create private AK3 work directory", true);
        return 1;
    }

    FILE* log_file = nullptr;
    if (config.log_path) {
        log_file = std::fopen(config.log_path->c_str(), "w");
        if (!log_file) {
            emit_line(nullptr, "Cannot open AK3 log file: " + std::string(strerror(errno)), true);
            return 1;
        }
    }
    const ScopedFile log_closer(log_file);

    ZipReader reader(config.zip_path);
    const ArchiveInspection archive =
        reader.valid() ? validate_archive(reader) : ArchiveInspection{};
    if (!reader.valid() || !archive.valid) {
        emit_line(log_file,
                  "AK3 package changed during preflight: " +
                      (reader.valid() ? archive.error : reader.error()),
                  true);
        return 1;
    }

    std::string extraction_error;
    const auto updater_text =
        extract_text(reader.archive(), archive.updater_index, &extraction_error);
    if (!updater_text) {
        emit_line(log_file, extraction_error, true);
        return 1;
    }

    const fs::path updater_path = fs::path(work_dir->path()) / "update-binary";
    if (!write_executable(updater_path, *updater_text)) {
        emit_line(log_file, "Cannot stage AnyKernel3 update-binary", true);
        return 1;
    }

    const auto executable = current_executable();
    if (!executable) {
        emit_line(log_file, "Cannot locate the running ksud executable", true);
        return 1;
    }

    const fs::path launcher_dir = fs::path(work_dir->path()) / "launcher";
    fs::create_directory(launcher_dir, directory_error);
    if (directory_error) {
        emit_line(log_file, "Cannot create AK3 launcher directory", true);
        return 1;
    }
    chmod(launcher_dir.c_str(), 0700);

    // The first unzip happens before AK3 has installed its bundled BusyBox.
    // Route it through ksud's embedded BusyBox. If explicitly requested, expose
    // ksud's mkbootfs applet only inside this extracted AK3 tools directory.
    const std::string quoted_ksud = shell_quote(*executable);
    std::string unzip_wrapper =
        "#!/system/bin/sh\n" + quoted_ksud + " busybox unzip \"$@\"\nstatus=$?\n";
    if (config.use_mkbootfs) {
        unzip_wrapper +=
            "if [ \"$status\" -eq 0 ] && [ -n \"$AKHOME\" ] && [ -d \"$AKHOME/tools\" ]; "
            "then\n  " +
            quoted_ksud + " busybox ln -sf " + quoted_ksud + " \"$AKHOME/tools/mkbootfs\"\nfi\n";
    }
    unzip_wrapper += "exit \"$status\"\n";
    if (!write_executable(launcher_dir / "unzip", unzip_wrapper)) {
        emit_line(log_file, "Cannot create the AK3 unzip bridge", true);
        return 1;
    }

    std::optional<std::string> relative_slot;
    if (config.target_slot) {
        const std::string target = normalize_slot(*config.target_slot);
        const std::string current = normalize_slot(get_current_slot_suffix());
        if (target != "_a" && target != "_b") {
            emit_line(log_file, "Invalid target slot: " + *config.target_slot, true);
            return 1;
        }
        if (!is_ab_device() || (current != "_a" && current != "_b")) {
            emit_line(log_file, "A slot was selected on a non-A/B device", true);
            return 1;
        }
        relative_slot = target == current ? "active" : "inactive";
        if (!package.package_slot_policy.empty()) {
            std::string package_policy = trim_copy(package.package_slot_policy);
            std::transform(
                package_policy.begin(), package_policy.end(), package_policy.begin(),
                [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
            if (package_policy != "active" && package_policy != "inactive") {
                emit_line(log_file,
                          "Package declares an unsupported SLOT_SELECT policy: " +
                              package.package_slot_policy,
                          true);
                return 1;
            }
            if (package_policy != *relative_slot) {
                emit_line(log_file,
                          "Selected slot conflicts with the package SLOT_SELECT=" +
                              package.package_slot_policy,
                          true);
                return 1;
            }
        }
        emit_line(log_file, "Target slot: " + target + " (" + *relative_slot + ")");
    } else {
        emit_line(log_file, "Target slot: AnyKernel3 package default");
    }

    if (!package.kernel_name.empty())
        emit_line(log_file, "Kernel: " + package.kernel_name);
    if (!package.devices.empty()) {
        std::string devices;
        for (const auto& device : package.devices) {
            if (!devices.empty())
                devices += ", ";
            devices += device;
        }
        emit_line(log_file, "Declared devices: " + devices);
    }
    if (!package.package_slot_policy.empty()) {
        emit_line(log_file, "Package slot policy: " + package.package_slot_policy);
    }
    emit_line(log_file, config.use_mkbootfs ? "mkbootfs: ksud built-in"
                                            : "mkbootfs: AnyKernel3 package default");
    emit_line(log_file, "Starting AnyKernel3...");

    const UpdaterResult result = run_updater(updater_path, config.zip_path, work_dir->path(),
                                             launcher_dir.string(), relative_slot, log_file);
    if (result.exit_code == 0 && !result.embedded_ksu_failure) {
        emit_line(log_file, "AnyKernel3 flash completed successfully");
        return 0;
    }
    if (result.embedded_ksu_failure && result.exit_code == 0) {
        emit_line(log_file,
                  "AnyKernel3 flash failed: the package ignored an embedded KernelSU failure",
                  true);
    } else {
        emit_line(log_file,
                  "AnyKernel3 flash failed (exit code " + std::to_string(result.exit_code) + ")",
                  true);
    }
    return 1;
}

}  // namespace ksud::flash
