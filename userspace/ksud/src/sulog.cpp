#include "sulog.hpp"

#include "core/ksucalls.hpp"
#include "defs.hpp"
#include "log.hpp"
#include "utils.hpp"

#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace ksud {

namespace {

constexpr uint16_t KSU_EVENT_QUEUE_TYPE_DROPPED = 0xFFFFU;
constexpr uint16_t KSU_EVENT_RECORD_FLAG_INTERNAL = 1U;
constexpr size_t TASK_COMM_LEN = 16U;
constexpr size_t READ_BUF_SIZE = 8192U;
constexpr mode_t SULOG_DIR_MODE = 0700;
constexpr mode_t SULOG_FILE_MODE = 0600;
constexpr const char* SULOG_CONFIG_MODULE_ID = "internal.ksud.sulogd";
constexpr const char* SULOG_RETENTION_CONFIG_KEY = "log.retention.days";
constexpr const char* SULOG_MAX_FILE_SIZE_CONFIG_KEY = "log.max_file_size";
constexpr const char* SULOG_STATE_PATH = "/data/adb/ksu/sulogd.state";
constexpr uint64_t MIN_VALID_WALL_TIME_MS = 946684800000ULL;  // 2000-01-01 00:00:00 UTC
constexpr uint64_t SULOG_TIME_SYNC_THRESHOLD_MS = 5000ULL;
constexpr uint64_t NS_PER_MILLISECOND = 1000000ULL;
constexpr uint64_t DEFAULT_SULOG_RETENTION_DAYS = 3U;
constexpr uint64_t DEFAULT_SULOG_MAX_FILE_SIZE = 10U * 1024U * 1024U;
constexpr std::chrono::seconds SULOGD_RESTART_DELAY(3);

#pragma pack(push, 1)
struct EventRecordHeader {
    uint16_t record_type;
    uint16_t flags;
    uint32_t payload_len;
    uint64_t seq;
    uint64_t ts_ns;
};

struct DroppedInfo {
    uint64_t dropped;
    uint64_t first_seq;
    uint64_t last_seq;
};

struct SulogEventHeader {
    uint16_t version;
    uint16_t event_type;
    int32_t retval;
    uint32_t pid;
    uint32_t tgid;
    uint32_t ppid;
    uint32_t uid;
    uint32_t euid;
    char comm[TASK_COMM_LEN];
    uint32_t filename_len;
    uint32_t argv_len;
};
#pragma pack(pop)

struct SulogEvent {
    uint16_t version{};
    uint16_t event_type{};
    int32_t retval{};
    uint32_t pid{};
    uint32_t tgid{};
    uint32_t ppid{};
    uint32_t uid{};
    uint32_t euid{};
    std::string comm;
    std::string file;
    std::string argv;
};

enum class ReadState {
    Drained,
    Closed,
};

enum class SessionExitReason {
    FdClosed,
    EpollHangup,
};

struct DailyLogWriter {
    std::string current_day;
    std::string boot_id;
    uint32_t current_index = 0;
    uint64_t current_size = 0;
    uint64_t max_file_size = DEFAULT_SULOG_MAX_FILE_SIZE;
    bool has_valid_time_anchor = false;
    uint64_t anchor_wall_time_ms = 0;
    uint64_t anchor_elapsed_ms = 0;
    uint64_t anchor_boot_time_ms = 0;
    int fd = -1;
};

struct SulogConfig {
    uint64_t retention_days = DEFAULT_SULOG_RETENTION_DAYS;
    uint64_t max_file_size = DEFAULT_SULOG_MAX_FILE_SIZE;
};

struct SulogState {
    std::string boot_id;
    std::string path;
};

auto daily_log_path(const std::string& day, uint32_t index) -> std::filesystem::path;
bool ensure_private_dir_exists(const std::filesystem::path& path);
bool open_daily_writer(DailyLogWriter* writer);
auto escape_field(const std::string& value) -> std::string;

class SulogdLockGuard {
public:
    SulogdLockGuard() = default;
    explicit SulogdLockGuard(int fd) : fd_(fd) {}

    SulogdLockGuard(const SulogdLockGuard&) = delete;
    auto operator=(const SulogdLockGuard&) -> SulogdLockGuard& = delete;

    SulogdLockGuard(SulogdLockGuard&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

    auto operator=(SulogdLockGuard&& other) noexcept -> SulogdLockGuard& {
        if (this == &other) {
            return *this;
        }
        reset();
        fd_ = other.fd_;
        other.fd_ = -1;
        return *this;
    }

    ~SulogdLockGuard() { reset(); }

    auto valid() const -> bool { return fd_ >= 0; }

    void reset() {
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_ = -1;
};

template <typename T>
bool read_packed_struct(const uint8_t* bytes, size_t len, T* out) {
    if (len < sizeof(T)) {
        return false;
    }
    std::memcpy(out, bytes, sizeof(T));
    return true;
}

bool write_all(int fd, const void* data, size_t len) {
    const auto* ptr = static_cast<const uint8_t*>(data);
    size_t written = 0;
    while (written < len) {
        const ssize_t ret = ::write(fd, ptr + written, len - written);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (ret == 0) {
            return false;
        }
        written += static_cast<size_t>(ret);
    }
    return true;
}

auto parse_c_string(const char* data, size_t len) -> std::string {
    size_t end = 0;
    while (end < len && data[end] != '\0') {
        end++;
    }
    return std::string(data, end);
}

auto parse_c_string(const uint8_t* data, size_t len) -> std::string {
    return parse_c_string(reinterpret_cast<const char*>(data), len);
}

auto current_log_day() -> std::string {
    std::time_t now = std::time(nullptr);
    struct tm tm_buf{};
    localtime_r(&now, &tm_buf);
    char buf[16] = {};
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm_buf);
    return std::string(buf);
}

auto current_epoch_millis() -> uint64_t {
    struct timespec ts{};
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return 0;
    }
    return static_cast<uint64_t>(ts.tv_sec) * 1000U +
           static_cast<uint64_t>(ts.tv_nsec / NS_PER_MILLISECOND);
}

auto current_elapsed_realtime_millis() -> uint64_t {
    struct timespec ts{};
    if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0) {
        return 0;
    }
    return static_cast<uint64_t>(ts.tv_sec) * 1000U +
           static_cast<uint64_t>(ts.tv_nsec / NS_PER_MILLISECOND);
}

bool is_valid_wall_time(uint64_t wall_time_ms) {
    return wall_time_ms >= MIN_VALID_WALL_TIME_MS;
}

bool is_valid_time_anchor(uint64_t wall_time_ms, uint64_t elapsed_ms) {
    return is_valid_wall_time(wall_time_ms) && wall_time_ms >= elapsed_ms;
}

bool is_valid_log_day(const std::string& day) {
    return day.size() == 10 && day >= "2000-01-01";
}

auto absolute_diff_u64(uint64_t lhs, uint64_t rhs) -> uint64_t {
    return lhs >= rhs ? lhs - rhs : rhs - lhs;
}

auto day_before(uint64_t days) -> std::string {
    std::time_t now = std::time(nullptr) - static_cast<std::time_t>(days * 24U * 60U * 60U);
    struct tm tm_buf{};
    localtime_r(&now, &tm_buf);
    char buf[16] = {};
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm_buf);
    return std::string(buf);
}

bool parse_log_name(const std::filesystem::path& path, std::string* day, uint32_t* index) {
    const std::string name = path.filename().string();
    if (name.rfind("sulog-", 0) != 0 || name.size() < 15 ||
        name.substr(name.size() - 4) != ".log") {
        return false;
    }

    const std::string body = name.substr(6, name.size() - 10);
    if (body.size() == 10) {
        *day = body;
        *index = 0;
        return true;
    }

    if (body.size() > 11 && body[10] == '-') {
        const std::string day_part = body.substr(0, 10);
        const std::string index_part = body.substr(11);
        if (index_part.empty()) {
            return false;
        }
        for (const char ch : index_part) {
            if (ch < '0' || ch > '9') {
                return false;
            }
        }
        *day = day_part;
        *index = static_cast<uint32_t>(std::stoul(index_part));
        return true;
    }

    return false;
}

auto sulog_config_dir() -> std::filesystem::path {
    return std::filesystem::path(MODULE_CONFIG_DIR) / SULOG_CONFIG_MODULE_ID;
}

auto sulog_persist_config_path() -> std::filesystem::path {
    return sulog_config_dir() / PERSIST_CONFIG_NAME;
}

auto sulog_temp_config_path() -> std::filesystem::path {
    return sulog_config_dir() / TEMP_CONFIG_NAME;
}

auto load_key_value_config(const std::filesystem::path& path)
    -> std::map<std::string, std::string> {
    std::map<std::string, std::string> config;
    auto content = read_file(path.string());
    if (!content) {
        return config;
    }

    std::istringstream input(*content);
    std::string line;
    while (std::getline(input, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        config[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return config;
}

bool save_key_value_config(const std::filesystem::path& path,
                           const std::map<std::string, std::string>& config) {
    std::ostringstream output;
    for (const auto& [key, value] : config) {
        output << key << "=" << value << "\n";
    }
    if (!write_file(path, output.str())) {
        LOGW("Failed to save sulog config %s", path.c_str());
        return false;
    }
    return true;
}

bool ensure_sulog_config_dir_exists() {
    return ensure_dir_exists(MODULE_CONFIG_DIR) && ensure_dir_exists(sulog_config_dir().string());
}

bool parse_positive_u64(const std::string& raw, uint64_t* out_value) {
    const std::string value = trim(raw);
    if (value.empty()) {
        return false;
    }

    size_t consumed = 0;
    try {
        const auto parsed = std::stoull(value, &consumed, 10);
        if (consumed != value.size() || parsed == 0) {
            return false;
        }
        *out_value = static_cast<uint64_t>(parsed);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

auto ensure_sulog_config_value(const char* key, uint64_t default_value) -> std::string {
    const std::string default_string = std::to_string(default_value);
    if (!ensure_sulog_config_dir_exists()) {
        return default_string;
    }

    const auto temp_config = load_key_value_config(sulog_temp_config_path());
    const auto temp_iter = temp_config.find(key);
    if (temp_iter != temp_config.end()) {
        return temp_iter->second;
    }

    auto persist_config = load_key_value_config(sulog_persist_config_path());
    const auto persist_iter = persist_config.find(key);
    if (persist_iter != persist_config.end()) {
        return persist_iter->second;
    }

    persist_config[key] = default_string;
    save_key_value_config(sulog_persist_config_path(), persist_config);
    return default_string;
}

auto load_sulog_config() -> SulogConfig {
    SulogConfig config;

    const std::string retention_value =
        ensure_sulog_config_value(SULOG_RETENTION_CONFIG_KEY, DEFAULT_SULOG_RETENTION_DAYS);
    if (!parse_positive_u64(retention_value, &config.retention_days)) {
        LOGW("Invalid sulog retention config '%s=%s', using default %llu",
             SULOG_RETENTION_CONFIG_KEY, retention_value.c_str(),
             static_cast<unsigned long long>(DEFAULT_SULOG_RETENTION_DAYS));
        config.retention_days = DEFAULT_SULOG_RETENTION_DAYS;
    }

    const std::string max_file_size_value =
        ensure_sulog_config_value(SULOG_MAX_FILE_SIZE_CONFIG_KEY, DEFAULT_SULOG_MAX_FILE_SIZE);
    if (!parse_positive_u64(max_file_size_value, &config.max_file_size)) {
        LOGW("Invalid sulog max file size config '%s=%s', using default %llu",
             SULOG_MAX_FILE_SIZE_CONFIG_KEY, max_file_size_value.c_str(),
             static_cast<unsigned long long>(DEFAULT_SULOG_MAX_FILE_SIZE));
        config.max_file_size = DEFAULT_SULOG_MAX_FILE_SIZE;
    }

    return config;
}

bool load_sulog_state(SulogState* state) {
    const auto config = load_key_value_config(SULOG_STATE_PATH);
    const auto boot_id_iter = config.find("boot_id");
    const auto path_iter = config.find("path");
    if (boot_id_iter == config.end() || path_iter == config.end() || path_iter->second.empty()) {
        return false;
    }

    state->boot_id = boot_id_iter->second;
    state->path = path_iter->second;
    return !state->boot_id.empty();
}

bool save_sulog_state(const DailyLogWriter& writer) {
    if (!ensure_private_dir_exists(WORKING_DIR)) {
        return false;
    }

    const auto path = daily_log_path(writer.current_day, writer.current_index);
    return save_key_value_config(SULOG_STATE_PATH, {
                                                       {"boot_id", writer.boot_id},
                                                       {"path", path.string()},
                                                   });
}

bool ensure_private_dir_exists(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
        LOGE("Failed to create %s: %s", path.c_str(), ec.message().c_str());
        return false;
    }

    if (::chmod(path.c_str(), SULOG_DIR_MODE) != 0) {
        LOGW("Failed to chmod %s: %s", path.c_str(), strerror(errno));
    }
    return true;
}

void close_writer(DailyLogWriter* writer) {
    if (writer->fd >= 0) {
        close(writer->fd);
        writer->fd = -1;
    }
}

auto daily_log_path(const std::string& day, uint32_t index) -> std::filesystem::path {
    if (index == 0) {
        return std::filesystem::path(LOG_DIR) / ("sulog-" + day + ".log");
    }
    return std::filesystem::path(LOG_DIR) / ("sulog-" + day + "-" + std::to_string(index) + ".log");
}

auto parse_daemon_start_boot_id(const std::string& line) -> std::string {
    constexpr char prefix[] = "type=daemon_start boot_id=\"";
    const size_t start = line.find(prefix);
    if (start == std::string::npos) {
        return {};
    }

    std::string boot_id;
    bool escaped = false;
    for (size_t index = start + sizeof(prefix) - 1; index < line.size(); ++index) {
        const char ch = line[index];
        if (escaped) {
            boot_id.push_back(ch);
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            continue;
        }
        if (ch == '"') {
            return boot_id;
        }
        boot_id.push_back(ch);
    }

    return {};
}

auto read_last_boot_id_for_log(const std::filesystem::path& path) -> std::string {
    std::ifstream input(path);
    if (!input.is_open()) {
        return {};
    }

    std::string line;
    std::string last_boot_id;
    while (std::getline(input, line)) {
        const std::string boot_id = parse_daemon_start_boot_id(line);
        if (!boot_id.empty()) {
            last_boot_id = boot_id;
        }
    }

    return last_boot_id;
}

void cleanup_expired_logs(
    uint64_t retention_days,
    const std::optional<std::filesystem::path>& preserved_path = std::nullopt) {
    const std::filesystem::path log_dir(LOG_DIR);
    if (!std::filesystem::exists(log_dir)) {
        return;
    }

    const std::string cutoff = day_before(retention_days > 0 ? retention_days - 1 : 0);
    for (const auto& entry : std::filesystem::directory_iterator(log_dir)) {
        std::string day;
        uint32_t index = 0;
        if (!parse_log_name(entry.path(), &day, &index)) {
            continue;
        }
        if (preserved_path && entry.path() == *preserved_path) {
            continue;
        }
        if (day < cutoff) {
            std::error_code ec;
            std::filesystem::remove(entry.path(), ec);
            if (ec) {
                LOGW("Failed to remove expired sulog %s: %s", entry.path().c_str(),
                     ec.message().c_str());
            }
        }
    }
}

bool open_log_file(const std::filesystem::path& path, int* out_fd) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, SULOG_FILE_MODE);
    if (fd < 0) {
        LOGE("Failed to open %s: %s", path.c_str(), strerror(errno));
        return false;
    }
    if (::fchmod(fd, SULOG_FILE_MODE) != 0) {
        LOGW("Failed to chmod %s: %s", path.c_str(), strerror(errno));
    }
    *out_fd = fd;
    return true;
}

bool open_log_writer_at_path(const std::filesystem::path& path, const std::string& day,
                             uint32_t index, DailyLogWriter* writer) {
    uint64_t size = 0;
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        size = std::filesystem::file_size(path, ec);
        if (ec) {
            size = 0;
        }
    }

    int fd = -1;
    if (!open_log_file(path, &fd)) {
        return false;
    }

    close_writer(writer);
    writer->current_day = day;
    writer->current_index = index;
    writer->current_size = size;
    writer->fd = fd;
    return true;
}

auto next_log_path_for_day(const std::string& day) -> std::pair<std::filesystem::path, uint32_t> {
    const std::filesystem::path log_dir(LOG_DIR);
    uint32_t highest_index = 0;
    bool found = false;

    for (const auto& entry : std::filesystem::directory_iterator(log_dir)) {
        std::string entry_day;
        uint32_t entry_index = 0;
        if (!parse_log_name(entry.path(), &entry_day, &entry_index)) {
            continue;
        }
        if (entry_day == day) {
            highest_index = std::max(highest_index, entry_index);
            found = true;
        }
    }

    const uint32_t index = found ? highest_index + 1U : 0U;
    return {daily_log_path(day, index), index};
}

bool allocate_new_log_writer_for_day(const std::string& day, DailyLogWriter* writer) {
    const auto [path, index] = next_log_path_for_day(day);
    return open_log_writer_at_path(path, day, index, writer);
}

bool open_log_writer_from_state(const SulogState& state, DailyLogWriter* writer) {
    std::string day;
    uint32_t index = 0;
    const std::filesystem::path path(state.path);
    if (!parse_log_name(path, &day, &index)) {
        return false;
    }
    return open_log_writer_at_path(path, day, index, writer);
}

bool migrate_log_writer_to_day(DailyLogWriter* writer, const std::string& target_day) {
    if (writer->current_day == target_day) {
        return true;
    }

    const auto source_path = daily_log_path(writer->current_day, writer->current_index);
    const auto [target_path, target_index] = next_log_path_for_day(target_day);
    std::error_code ec;
    std::filesystem::rename(source_path, target_path, ec);
    if (ec) {
        LOGW("Failed to migrate sulog %s -> %s: %s", source_path.c_str(), target_path.c_str(),
             ec.message().c_str());
        return false;
    }

    writer->current_day = target_day;
    writer->current_index = target_index;
    return save_sulog_state(*writer);
}

bool roll_log_writer_to_new_day(DailyLogWriter* writer, const std::string& target_day) {
    if (writer->current_day == target_day) {
        return true;
    }
    if (!allocate_new_log_writer_for_day(target_day, writer)) {
        return false;
    }
    return save_sulog_state(*writer);
}

bool rotate_if_needed(DailyLogWriter* writer, size_t next_write_len) {
    if (writer->fd < 0) {
        return open_daily_writer(writer);
    }

    const uint64_t next_len = static_cast<uint64_t>(next_write_len);
    if (writer->current_size > 0 && writer->current_size + next_len > writer->max_file_size) {
        writer->current_index++;
        const std::filesystem::path path =
            daily_log_path(writer->current_day, writer->current_index);
        if (!open_log_writer_at_path(path, writer->current_day, writer->current_index, writer)) {
            return false;
        }
        save_sulog_state(*writer);
    }

    return true;
}

bool open_daily_writer(DailyLogWriter* writer) {
    if (!ensure_private_dir_exists(LOG_DIR)) {
        return false;
    }
    const SulogConfig config = load_sulog_config();
    writer->max_file_size = config.max_file_size;

    SulogState state;
    const bool has_active_state = load_sulog_state(&state) && state.boot_id == writer->boot_id;
    const std::optional<std::filesystem::path> preserved_path =
        has_active_state ? std::optional<std::filesystem::path>(std::filesystem::path(state.path))
                         : std::nullopt;
    cleanup_expired_logs(config.retention_days, preserved_path);

    if (has_active_state && open_log_writer_from_state(state, writer)) {
        return true;
    }

    if (!allocate_new_log_writer_for_day(current_log_day(), writer)) {
        return false;
    }
    save_sulog_state(*writer);
    return true;
}

bool align_writer_to_current_day(DailyLogWriter* writer) {
    const uint64_t wall_time_ms = current_epoch_millis();
    const uint64_t elapsed_ms = current_elapsed_realtime_millis();
    if (!is_valid_time_anchor(wall_time_ms, elapsed_ms)) {
        return true;
    }

    const std::string current_day = current_log_day();
    if (!is_valid_log_day(current_day) || writer->current_day == current_day) {
        return true;
    }

    if (!is_valid_log_day(writer->current_day)) {
        return migrate_log_writer_to_day(writer, current_day);
    }

    return roll_log_writer_to_new_day(writer, current_day);
}

bool write_log_line(DailyLogWriter* writer, const std::string& line) {
    const size_t write_len = line.size() + 1U;
    if (!rotate_if_needed(writer, write_len)) {
        return false;
    }
    if (!write_all(writer->fd, line.data(), line.size()) || !write_all(writer->fd, "\n", 1U)) {
        LOGE("Failed to write sulog line: %s", strerror(errno));
        return false;
    }
    writer->current_size += static_cast<uint64_t>(write_len);
    return true;
}

void update_writer_time_anchor(DailyLogWriter* writer, uint64_t wall_time_ms, uint64_t elapsed_ms) {
    writer->has_valid_time_anchor = is_valid_time_anchor(wall_time_ms, elapsed_ms);
    if (writer->has_valid_time_anchor) {
        writer->anchor_wall_time_ms = wall_time_ms;
        writer->anchor_elapsed_ms = elapsed_ms;
        writer->anchor_boot_time_ms = wall_time_ms - elapsed_ms;
    } else {
        writer->anchor_wall_time_ms = 0;
        writer->anchor_elapsed_ms = 0;
        writer->anchor_boot_time_ms = 0;
    }
}

bool write_time_sync_marker(DailyLogWriter* writer, uint64_t wall_time_ms, uint64_t elapsed_ms,
                            const char* reason) {
    std::string line = "type=daemon_time_sync boot_id=\"" + escape_field(writer->boot_id) +
                       "\" wall_time_ms=" + std::to_string(wall_time_ms) +
                       " elapsed_ms=" + std::to_string(elapsed_ms);
    if (reason != nullptr && reason[0] != '\0') {
        line += " reason=";
        line += reason;
    }
    if (!write_log_line(writer, line)) {
        return false;
    }
    update_writer_time_anchor(writer, wall_time_ms, elapsed_ms);
    return true;
}

bool ensure_writer_has_valid_wall_time(DailyLogWriter* writer) {
    const uint64_t wall_time_ms = current_epoch_millis();
    const uint64_t elapsed_ms = current_elapsed_realtime_millis();
    if (!is_valid_time_anchor(wall_time_ms, elapsed_ms)) {
        return true;
    }

    const std::string current_day = current_log_day();
    const bool current_day_is_valid = is_valid_log_day(current_day);
    if (!current_day_is_valid) {
        return true;
    }

    bool moved_to_real_day = false;
    bool rolled_to_new_day = false;
    if (!is_valid_log_day(writer->current_day) && writer->current_day != current_day) {
        if (!migrate_log_writer_to_day(writer, current_day)) {
            return false;
        }
        moved_to_real_day = true;
    } else if (is_valid_log_day(writer->current_day) && writer->current_day != current_day) {
        if (!roll_log_writer_to_new_day(writer, current_day)) {
            return false;
        }
        rolled_to_new_day = true;
    }

    const uint64_t current_boot_time_ms = wall_time_ms - elapsed_ms;
    const bool clock_adjusted =
        writer->has_valid_time_anchor &&
        absolute_diff_u64(writer->anchor_boot_time_ms, current_boot_time_ms) >=
            SULOG_TIME_SYNC_THRESHOLD_MS;

    if (writer->has_valid_time_anchor && !moved_to_real_day && !rolled_to_new_day &&
        !clock_adjusted) {
        return true;
    }

    const char* reason = "anchor_refresh";
    if (moved_to_real_day) {
        reason = "clock_ready";
    } else if (rolled_to_new_day) {
        reason = "day_rollover";
    } else if (clock_adjusted) {
        reason = "clock_adjusted";
    }

    return write_time_sync_marker(writer, wall_time_ms, elapsed_ms, reason);
}

auto escape_field(const std::string& value) -> std::string {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char ch : value) {
        switch (ch) {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20U) {
                char buf[5] = {};
                std::snprintf(buf, sizeof(buf), "\\x%02x", static_cast<unsigned char>(ch));
                escaped += buf;
            } else {
                escaped.push_back(ch);
            }
            break;
        }
    }
    return escaped;
}

auto parse_event_name(uint16_t event_type) -> const char* {
    switch (event_type) {
    case 1:
        return "root_execve";
    case 2:
        return "sucompat";
    case 3:
        return "ioctl_grant_root";
    default:
        return "unknown";
    }
}

bool parse_sulog_event(const uint8_t* payload, size_t len, SulogEvent* event) {
    SulogEventHeader header{};
    if (!read_packed_struct(payload, len, &header)) {
        return false;
    }

    const size_t fixed_len = sizeof(SulogEventHeader);
    const size_t filename_len = header.filename_len;
    const size_t argv_len = header.argv_len;
    if (fixed_len + filename_len + argv_len != len) {
        return false;
    }

    event->version = header.version;
    event->event_type = header.event_type;
    event->retval = header.retval;
    event->pid = header.pid;
    event->tgid = header.tgid;
    event->ppid = header.ppid;
    event->uid = header.uid;
    event->euid = header.euid;
    event->comm = parse_c_string(header.comm, sizeof(header.comm));
    event->file = parse_c_string(payload + fixed_len, filename_len);
    event->argv = parse_c_string(payload + fixed_len + filename_len, argv_len);
    return true;
}

auto format_event_line(const EventRecordHeader& header, const SulogEvent& event) -> std::string {
    return "ts_ns=" + std::to_string(header.ts_ns) + " seq=" + std::to_string(header.seq) +
           " type=" + parse_event_name(event.event_type) +
           " version=" + std::to_string(event.version) + " retval=" + std::to_string(event.retval) +
           " pid=" + std::to_string(event.pid) + " tgid=" + std::to_string(event.tgid) +
           " ppid=" + std::to_string(event.ppid) + " uid=" + std::to_string(event.uid) +
           " euid=" + std::to_string(event.euid) + " comm=\"" + escape_field(event.comm) +
           "\" file=\"" + escape_field(event.file) + "\" argv=\"" + escape_field(event.argv) + "\"";
}

auto format_dropped_line(const EventRecordHeader& header, const DroppedInfo& info) -> std::string {
    return "ts_ns=" + std::to_string(header.ts_ns) + " seq=" + std::to_string(header.seq) +
           " type=dropped dropped=" + std::to_string(info.dropped) +
           " first_seq=" + std::to_string(info.first_seq) +
           " last_seq=" + std::to_string(info.last_seq);
}

bool format_record_line(const EventRecordHeader& header, const uint8_t* payload, size_t len,
                        std::string* line) {
    if (header.record_type == KSU_EVENT_QUEUE_TYPE_DROPPED) {
        if ((header.flags & KSU_EVENT_RECORD_FLAG_INTERNAL) == 0) {
            return false;
        }
        DroppedInfo info{};
        if (!read_packed_struct(payload, len, &info)) {
            return false;
        }
        *line = format_dropped_line(header, info);
        return true;
    }

    SulogEvent event{};
    if (!parse_sulog_event(payload, len, &event)) {
        return false;
    }
    *line = format_event_line(header, event);
    return true;
}

auto read_boot_id() -> std::string {
    const auto boot_id = read_file("/proc/sys/kernel/random/boot_id");
    if (!boot_id) {
        return "unknown";
    }
    return trim(*boot_id);
}

bool open_sulog_fd(int* out_fd) {
    const int fd = get_sulog_fd();
    if (fd < 0) {
        return false;
    }

    const int flags = fcntl(fd, F_GETFL);
    if (flags < 0) {
        close(fd);
        return false;
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        return false;
    }

    *out_fd = fd;
    return true;
}

bool write_session_marker(DailyLogWriter* writer, const std::string& boot_id,
                          uint64_t restart_count) {
    const uint64_t wall_time_ms = current_epoch_millis();
    const uint64_t elapsed_ms = current_elapsed_realtime_millis();
    std::string line;
    if (restart_count == 0) {
        line = "type=daemon_start boot_id=\"" + escape_field(boot_id) + "\"";
    } else {
        line = "type=daemon_restart boot_id=\"" + escape_field(boot_id) +
               "\" restart=" + std::to_string(restart_count);
    }
    line += " wall_time_ms=" + std::to_string(wall_time_ms) +
            " elapsed_ms=" + std::to_string(elapsed_ms);
    if (!write_log_line(writer, line)) {
        return false;
    }
    update_writer_time_anchor(writer, wall_time_ms, elapsed_ms);
    return true;
}

bool handle_readable(int fd, DailyLogWriter* writer, ReadState* state) {
    std::array<uint8_t, READ_BUF_SIZE> buf{};

    for (;;) {
        const ssize_t read_len = ::read(fd, buf.data(), buf.size());
        if (read_len < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                *state = ReadState::Drained;
                return true;
            }
            LOGE("Failed to read sulog queue: %s", strerror(errno));
            return false;
        }

        if (read_len == 0) {
            *state = ReadState::Closed;
            return true;
        }

        size_t offset = 0;
        const size_t total = static_cast<size_t>(read_len);
        while (offset < total) {
            const size_t remaining = total - offset;
            EventRecordHeader header{};
            if (!read_packed_struct(buf.data() + offset, remaining, &header)) {
                LOGW("Dropping truncated sulog frame header");
                break;
            }

            const size_t payload_len = header.payload_len;
            const size_t frame_len = sizeof(EventRecordHeader) + payload_len;
            if (frame_len > remaining) {
                LOGW("Dropping truncated sulog frame payload");
                break;
            }

            std::string line;
            if (format_record_line(header, buf.data() + offset + sizeof(EventRecordHeader),
                                   payload_len, &line)) {
                if (!ensure_writer_has_valid_wall_time(writer)) {
                    return false;
                }
                if (!write_log_line(writer, line)) {
                    return false;
                }
            } else {
                LOGW("Dropping malformed sulog record seq=%llu type=%u",
                     static_cast<unsigned long long>(header.seq), header.record_type);
            }

            offset += frame_len;
        }
    }
}

bool acquire_sulogd_lock(SulogdLockGuard* guard) {
    if (!ensure_private_dir_exists(WORKING_DIR)) {
        return false;
    }

    const int fd = ::open(SULOGD_LOCK_PATH, O_RDWR | O_CREAT | O_CLOEXEC, SULOG_FILE_MODE);
    if (fd < 0) {
        LOGE("Failed to open %s: %s", SULOGD_LOCK_PATH, strerror(errno));
        return false;
    }

    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            close(fd);
            return false;
        }
        LOGE("Failed to lock %s: %s", SULOGD_LOCK_PATH, strerror(errno));
        close(fd);
        return false;
    }

    *guard = SulogdLockGuard(fd);
    return true;
}

int spawn_sulogd() {
    pid_t pid = fork();
    if (pid < 0) {
        LOGE("Failed to fork sulogd launcher: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        if (setpgid(0, 0) != 0) {
            LOGW("Failed to detach sulogd process group: %s", strerror(errno));
        }
        switch_cgroups();

        const int devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) {
                close(devnull);
            }
        }

        const pid_t grandchild = fork();
        if (grandchild < 0) {
            _exit(127);
        }
        if (grandchild > 0) {
            _exit(0);
        }

        char* const argv[] = {const_cast<char*>(DAEMON_PATH), const_cast<char*>("sulogd"), nullptr};
        execv(DAEMON_PATH, argv);

        char* const fallback_argv[] = {const_cast<char*>("ksud"), const_cast<char*>("sulogd"),
                                       nullptr};
        execv("/proc/self/exe", fallback_argv);
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        LOGW("waitpid for sulogd launcher failed: %s", strerror(errno));
    }
    return 0;
}

bool run_sulog_session(uint64_t restart_count, SessionExitReason* reason) {
    const std::string boot_id = read_boot_id();
    int sulog_fd = -1;
    if (!open_sulog_fd(&sulog_fd)) {
        LOGE("Failed to open sulog fd");
        return false;
    }

    DailyLogWriter writer;
    writer.boot_id = boot_id;
    if (!open_daily_writer(&writer)) {
        close(sulog_fd);
        return false;
    }
    if (!align_writer_to_current_day(&writer)) {
        close_writer(&writer);
        close(sulog_fd);
        return false;
    }

    if (!write_session_marker(&writer, boot_id, restart_count)) {
        close_writer(&writer);
        close(sulog_fd);
        return false;
    }
    if (!ensure_writer_has_valid_wall_time(&writer)) {
        close_writer(&writer);
        close(sulog_fd);
        return false;
    }

    const int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        LOGE("Failed to create epoll fd: %s", strerror(errno));
        close_writer(&writer);
        close(sulog_fd);
        return false;
    }

    struct epoll_event event{};
    event.events = EPOLLIN | EPOLLERR | EPOLLHUP;
    event.data.fd = sulog_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sulog_fd, &event) != 0) {
        LOGE("Failed to register sulog fd to epoll: %s", strerror(errno));
        close(epoll_fd);
        close_writer(&writer);
        close(sulog_fd);
        return false;
    }

    LOGI("sulogd session started, restart=%llu", static_cast<unsigned long long>(restart_count));

    std::array<struct epoll_event, 4> events{};
    while (true) {
        const int ready = epoll_wait(epoll_fd, events.data(), static_cast<int>(events.size()), -1);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            LOGE("epoll_wait failed for sulogd: %s", strerror(errno));
            break;
        }

        for (int i = 0; i < ready; ++i) {
            const uint32_t mask = events[static_cast<size_t>(i)].events;
            if ((mask & EPOLLIN) != 0U) {
                ReadState state = ReadState::Drained;
                if (!handle_readable(sulog_fd, &writer, &state)) {
                    close(epoll_fd);
                    close_writer(&writer);
                    close(sulog_fd);
                    return false;
                }
                if (state == ReadState::Closed) {
                    *reason = SessionExitReason::FdClosed;
                    close(epoll_fd);
                    close_writer(&writer);
                    close(sulog_fd);
                    return true;
                }
            }

            if ((mask & (EPOLLERR | EPOLLHUP)) != 0U) {
                ReadState state = ReadState::Drained;
                if (!handle_readable(sulog_fd, &writer, &state)) {
                    close(epoll_fd);
                    close_writer(&writer);
                    close(sulog_fd);
                    return false;
                }
                *reason = SessionExitReason::EpollHangup;
                close(epoll_fd);
                close_writer(&writer);
                close(sulog_fd);
                return true;
            }
        }
    }

    close(epoll_fd);
    close_writer(&writer);
    close(sulog_fd);
    return false;
}

}  // namespace

int run_sulogd() {
    SulogdLockGuard lock;
    if (!acquire_sulogd_lock(&lock)) {
        LOGI("sulogd lock is held, skipping start");
        return 0;
    }

    uint64_t restart_count = 0;
    while (true) {
        SessionExitReason reason = SessionExitReason::FdClosed;
        if (run_sulog_session(restart_count, &reason)) {
            if (reason == SessionExitReason::FdClosed) {
                LOGW("sulog fd closed, restarting in %llds",
                     static_cast<long long>(SULOGD_RESTART_DELAY.count()));
            } else {
                LOGW("sulog epoll hangup, restarting in %llds",
                     static_cast<long long>(SULOGD_RESTART_DELAY.count()));
            }
        } else {
            LOGW("sulogd session failed, restarting in %llds",
                 static_cast<long long>(SULOGD_RESTART_DELAY.count()));
        }

        restart_count++;
        std::this_thread::sleep_for(SULOGD_RESTART_DELAY);
    }
}

int ensure_sulogd_running() {
    return spawn_sulogd();
}

void ensure_sulogd_running_if_enabled() {
    const auto [value, supported] = get_feature(static_cast<uint32_t>(FeatureId::SuLog));
    if (!supported || value == 0) {
        return;
    }

    if (ensure_sulogd_running() != 0) {
        LOGW("Failed to ensure sulogd is running");
    }
}

}  // namespace ksud
