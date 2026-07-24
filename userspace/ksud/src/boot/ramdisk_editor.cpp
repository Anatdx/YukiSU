#include "ramdisk_editor.hpp"

#include "boot_ramdisk.hpp"
#include "cpio.hpp"

#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ksud {
namespace {

constexpr std::array<std::uint8_t, 4> kProtocolMagic = {'Y', 'R', 'C', 'P'};
constexpr std::uint16_t kProtocolVersion = 2;
constexpr std::uint16_t kResponseFlag = 0x8000U;
constexpr std::size_t kFrameHeaderSize = 20;
constexpr std::size_t kMaximumControlPayload = std::size_t{16} * 1024U * 1024U;
constexpr std::uint64_t kMaximumRequestPayload =
    kCpioDefaultMaxContentSize + kMaximumControlPayload;
constexpr std::uint32_t kNullStringLength = std::numeric_limits<std::uint32_t>::max();
constexpr std::uint32_t kCapabilityRead = 1U << 0U;
constexpr std::uint32_t kCapabilityReplace = 1U << 1U;
constexpr std::uint32_t kCapabilityCreateFile = 1U << 2U;
constexpr std::uint32_t kCapabilityCreateDirectory = 1U << 3U;
constexpr std::uint32_t kCapabilityCreateSymbolicLink = 1U << 4U;
constexpr std::uint32_t kCapabilityCreateHardLink = 1U << 5U;
constexpr std::uint32_t kCapabilityCopy = 1U << 6U;
constexpr std::uint32_t kCapabilityMove = 1U << 7U;
constexpr std::uint32_t kCapabilityRemove = 1U << 8U;
constexpr std::uint32_t kCapabilityUpdateMetadata = 1U << 9U;
constexpr std::uint32_t kCapabilityAtomicDump = 1U << 10U;
constexpr std::uint32_t kCapabilityRangedRead = 1U << 11U;
constexpr std::uint32_t kCapabilityImplicitDirectories = 1U << 12U;
constexpr std::uint32_t kCapabilityMultipleRamdisks = 1U << 13U;
constexpr std::uint32_t kCapabilities =
    kCapabilityRead | kCapabilityReplace | kCapabilityCreateFile | kCapabilityCreateDirectory |
    kCapabilityCreateSymbolicLink | kCapabilityCreateHardLink | kCapabilityCopy | kCapabilityMove |
    kCapabilityRemove | kCapabilityUpdateMetadata | kCapabilityAtomicDump | kCapabilityRangedRead |
    kCapabilityImplicitDirectories | kCapabilityMultipleRamdisks;
constexpr unsigned kDocumentIdShift = 48U;
constexpr std::uint64_t kLocalNodeIdMask = (std::uint64_t{1} << kDocumentIdShift) - 1U;
constexpr std::size_t kMaximumDocumentCount =
    std::numeric_limits<std::uint16_t>::max() - 1U;

enum class Opcode : std::uint8_t {
    HELLO = 1,
    STAT = 2,
    LIST = 3,
    READ = 4,
    REPLACE = 5,
    CREATE_FILE = 6,
    CREATE_DIRECTORY = 7,
    CREATE_SYMBOLIC_LINK = 8,
    CREATE_HARD_LINK = 9,
    COPY = 10,
    MOVE = 11,
    REMOVE = 12,
    UPDATE_METADATA = 13,
    DUMP = 14,
    CLOSE = 15,
    LIST_RAMDISKS = 16,
};

enum class Status : std::uint8_t {
    OK = 0,
    INVALID_REQUEST = 1,
    NOT_FOUND = 2,
    OPERATION_FAILED = 3,
    IO_ERROR = 4,
    LIMIT_EXCEEDED = 5,
    UNSUPPORTED_VERSION = 6,
};

enum class HeaderReadResult : std::uint8_t {
    OK,
    END_OF_STREAM,
    ERROR,
};

struct FrameHeader {
    std::uint16_t version = 0;
    std::uint16_t opcode = 0;
    std::uint32_t request_id = 0;
    std::uint64_t payload_size = 0;
};

using DumpAction = std::function<bool()>;

struct SessionDocument {
    CpioDocument* document = nullptr;
    std::string name;
    std::uint32_t vendor_type = 0;
    std::array<std::uint32_t, 16> board_id{};
    std::uint64_t packed_size = 0;
    FileFormat compression = FileFormat::UNKNOWN;
    bool is_vendor = false;
};

struct ResolvedNode {
    CpioDocument* document = nullptr;
    std::size_t document_index = 0;
    CpioNodeId local_id = kCpioRootNodeId;
};

std::optional<std::uint64_t> encode_node_id(std::size_t document_index, CpioNodeId local_id) {
    if (document_index >= kMaximumDocumentCount || local_id > kLocalNodeIdMask) {
        return std::nullopt;
    }
    return (static_cast<std::uint64_t>(document_index + 1U) << kDocumentIdShift) | local_id;
}

std::optional<ResolvedNode> resolve_node(std::vector<SessionDocument>& documents,
                                         std::uint64_t encoded_id) {
    const std::uint64_t encoded_document = encoded_id >> kDocumentIdShift;
    if (encoded_document == 0 || encoded_document > documents.size()) {
        return std::nullopt;
    }
    const std::size_t index = static_cast<std::size_t>(encoded_document - 1U);
    return ResolvedNode{documents[index].document, index, encoded_id & kLocalNodeIdMask};
}

const char* compression_name(FileFormat format) {
    switch (format) {
    case FileFormat::GZIP:
        return "gzip";
    case FileFormat::ZOPFLI:
        return "zopfli";
    case FileFormat::XZ:
        return "xz";
    case FileFormat::LZ4:
        return "lz4";
    case FileFormat::LZ4_LEGACY:
        return "lz4_legacy";
    case FileFormat::LZ4_LG:
        return "lz4_lg";
    case FileFormat::UNKNOWN:
        return "none";
    default:
        return "unsupported";
    }
}

bool read_all(int fd, void* output, std::size_t size) {
    auto* bytes = static_cast<std::uint8_t*>(output);
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t count = ::read(fd, bytes + offset, size - offset);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (count == 0) {
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

bool write_all(int fd, const void* input, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(input);
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t count = ::write(fd, bytes + offset, size - offset);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (count == 0) {
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

std::uint16_t decode_u16(const std::uint8_t* input) {
    return static_cast<std::uint16_t>(input[0]) | (static_cast<std::uint16_t>(input[1]) << 8U);
}

std::uint32_t decode_u32(const std::uint8_t* input) {
    return static_cast<std::uint32_t>(input[0]) | (static_cast<std::uint32_t>(input[1]) << 8U) |
           (static_cast<std::uint32_t>(input[2]) << 16U) |
           (static_cast<std::uint32_t>(input[3]) << 24U);
}

std::uint64_t decode_u64(const std::uint8_t* input) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<std::uint64_t>(input[index]) << (index * 8U);
    }
    return value;
}

void encode_u16(std::uint8_t* output, std::uint16_t value) {
    output[0] = static_cast<std::uint8_t>(value);
    output[1] = static_cast<std::uint8_t>(value >> 8U);
}

void encode_u32(std::uint8_t* output, std::uint32_t value) {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        output[index] = static_cast<std::uint8_t>(value >> (index * 8U));
    }
}

void encode_u64(std::uint8_t* output, std::uint64_t value) {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        output[index] = static_cast<std::uint8_t>(value >> (index * 8U));
    }
}

HeaderReadResult read_header(int fd, FrameHeader& header) {
    std::array<std::uint8_t, kFrameHeaderSize> bytes{};
    ssize_t first_count;
    do {
        first_count = ::read(fd, bytes.data(), 1);
    } while (first_count < 0 && errno == EINTR);
    if (first_count == 0) {
        return HeaderReadResult::END_OF_STREAM;
    }
    if (first_count < 0 || !read_all(fd, bytes.data() + 1, bytes.size() - 1)) {
        return HeaderReadResult::ERROR;
    }
    if (!std::equal(kProtocolMagic.begin(), kProtocolMagic.end(), bytes.begin())) {
        return HeaderReadResult::ERROR;
    }
    header.version = decode_u16(bytes.data() + 4);
    header.opcode = decode_u16(bytes.data() + 6);
    header.request_id = decode_u32(bytes.data() + 8);
    header.payload_size = decode_u64(bytes.data() + 12);
    return HeaderReadResult::OK;
}

bool write_header(int fd, std::uint16_t opcode, std::uint32_t request_id,
                  std::uint64_t payload_size) {
    std::array<std::uint8_t, kFrameHeaderSize> bytes{};
    std::copy(kProtocolMagic.begin(), kProtocolMagic.end(), bytes.begin());
    encode_u16(bytes.data() + 4, kProtocolVersion);
    encode_u16(bytes.data() + 6, opcode | kResponseFlag);
    encode_u32(bytes.data() + 8, request_id);
    encode_u64(bytes.data() + 12, payload_size);
    return write_all(fd, bytes.data(), bytes.size());
}

class PayloadReader {
public:
    PayloadReader(int fd, std::uint64_t size) : fd_(fd), remaining_(size) {}

    bool read_u8(std::uint8_t& value) { return read_bytes(&value, sizeof(value)); }

    bool read_u32(std::uint32_t& value) {
        std::array<std::uint8_t, sizeof(value)> bytes{};
        if (!read_bytes(bytes.data(), bytes.size())) {
            return false;
        }
        value = decode_u32(bytes.data());
        return true;
    }

    bool read_u64(std::uint64_t& value) {
        std::array<std::uint8_t, sizeof(value)> bytes{};
        if (!read_bytes(bytes.data(), bytes.size())) {
            return false;
        }
        value = decode_u64(bytes.data());
        return true;
    }

    bool read_string(std::string& value) {
        std::uint32_t length = 0;
        if (!read_u32(length) || length > kCpioMaxPathSize || length > remaining_) {
            return false;
        }
        value.resize(length);
        return length == 0 || read_bytes(value.data(), length);
    }

    bool read_bytes(void* output, std::size_t size) {
        if (size > remaining_) {
            return false;
        }
        if (size != 0 && !read_all(fd_, output, size)) {
            return false;
        }
        remaining_ -= size;
        return true;
    }

    ssize_t read_some(std::uint8_t* output, std::size_t capacity) {
        if (remaining_ == 0) {
            return 0;
        }
        const std::size_t requested =
            static_cast<std::size_t>(std::min<std::uint64_t>(capacity, remaining_));
        while (true) {
            const ssize_t count = ::read(fd_, output, requested);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                return -1;
            }
            remaining_ -= static_cast<std::size_t>(count);
            return count;
        }
    }

    bool drain() {
        std::array<std::uint8_t, std::size_t{64} * 1024U> buffer{};
        while (remaining_ != 0) {
            if (read_some(buffer.data(), buffer.size()) <= 0) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] std::uint64_t remaining() const { return remaining_; }

private:
    int fd_;
    std::uint64_t remaining_;
};

void append_u8(std::vector<std::uint8_t>& output, std::uint8_t value) {
    output.push_back(value);
}

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    const std::size_t offset = output.size();
    output.resize(offset + sizeof(value));
    encode_u32(output.data() + offset, value);
}

void append_u64(std::vector<std::uint8_t>& output, std::uint64_t value) {
    const std::size_t offset = output.size();
    output.resize(offset + sizeof(value));
    encode_u64(output.data() + offset, value);
}

bool append_node_id(std::vector<std::uint8_t>& output, std::size_t document_index,
                    CpioNodeId local_id) {
    const auto encoded_id = encode_node_id(document_index, local_id);
    if (!encoded_id) {
        return false;
    }
    append_u64(output, *encoded_id);
    return true;
}

bool append_string(std::vector<std::uint8_t>& output, const std::string& value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    append_u32(output, static_cast<std::uint32_t>(value.size()));
    output.insert(output.end(), value.begin(), value.end());
    return true;
}

bool append_optional_string(std::vector<std::uint8_t>& output,
                            const std::optional<std::string>& value) {
    if (!value) {
        append_u32(output, kNullStringLength);
        return true;
    }
    return append_string(output, *value);
}

bool append_node(std::vector<std::uint8_t>& output, const CpioNodeInfo& node,
                 std::size_t document_index) {
    const auto id = encode_node_id(document_index, node.id);
    const auto parent_id = encode_node_id(document_index, node.parent_id);
    if (!id || !parent_id) {
        return false;
    }
    append_u64(output, *id);
    append_u64(output, *parent_id);
    append_u64(output, node.size);
    append_u32(output, node.ino);
    append_u32(output, node.mode);
    append_u32(output, node.uid);
    append_u32(output, node.gid);
    append_u32(output, node.nlink);
    append_u32(output, node.mtime);
    append_u32(output, node.dev_major);
    append_u32(output, node.dev_minor);
    append_u32(output, node.rdev_major);
    append_u32(output, node.rdev_minor);
    append_u8(output, node.synthetic ? 1U : 0U);
    return append_string(output, node.name) && append_string(output, node.path) &&
           append_optional_string(output, node.link_target) &&
           output.size() <= kMaximumControlPayload;
}

bool send_response(int fd, const FrameHeader& request, Status status,
                   const std::vector<std::uint8_t>& body = {}) {
    const std::uint64_t payload_size = sizeof(std::uint32_t) + body.size();
    if (!write_header(fd, request.opcode, request.request_id, payload_size)) {
        return false;
    }
    std::array<std::uint8_t, sizeof(std::uint32_t)> status_bytes{};
    encode_u32(status_bytes.data(), static_cast<std::uint32_t>(status));
    return write_all(fd, status_bytes.data(), status_bytes.size()) &&
           (body.empty() || write_all(fd, body.data(), body.size()));
}

Status mutation_status(bool success) {
    return success ? Status::OK : Status::OPERATION_FAILED;
}

bool require_empty(PayloadReader& reader) {
    if (reader.remaining() == 0) {
        return true;
    }
    reader.drain();
    return false;
}

bool send_invalid_response(PayloadReader& reader, int output_fd, const FrameHeader& request) {
    return reader.drain() && send_response(output_fd, request, Status::INVALID_REQUEST);
}

bool handle_read(std::vector<SessionDocument>& documents, PayloadReader& reader, int output_fd,
                 const FrameHeader& request) {
    std::uint64_t encoded_id = 0;
    std::uint64_t offset = 0;
    std::uint64_t requested_length = 0;
    if (!reader.read_u64(encoded_id) || !reader.read_u64(offset) ||
        !reader.read_u64(requested_length) || !require_empty(reader)) {
        return send_invalid_response(reader, output_fd, request);
    }
    const auto resolved = resolve_node(documents, encoded_id);
    if (!resolved) {
        return send_response(output_fd, request, Status::NOT_FOUND);
    }
    const auto node = resolved->document->stat(resolved->local_id);
    if (!node) {
        return send_response(output_fd, request, Status::NOT_FOUND);
    }
    if (offset > node->size) {
        return send_invalid_response(reader, output_fd, request);
    }
    const std::uint64_t length = std::min(requested_length, node->size - offset);
    if (!write_header(output_fd, request.opcode, request.request_id,
                      sizeof(std::uint32_t) + length)) {
        return false;
    }
    std::array<std::uint8_t, sizeof(std::uint32_t)> status_bytes{};
    encode_u32(status_bytes.data(), static_cast<std::uint32_t>(Status::OK));
    if (!write_all(output_fd, status_bytes.data(), status_bytes.size())) {
        return false;
    }
    return resolved->document->read_content(
        resolved->local_id, offset, length, [&](const std::uint8_t* data, std::size_t size) {
            return write_all(output_fd, data, size);
        });
}

bool handle_request(std::vector<SessionDocument>& documents, const DumpAction& dump_action,
                    bool& dirty,
                    bool& should_close, PayloadReader& reader, int output_fd,
                    const FrameHeader& request) {
    if (request.opcode < static_cast<std::uint16_t>(Opcode::HELLO) ||
        request.opcode > static_cast<std::uint16_t>(Opcode::LIST_RAMDISKS)) {
        return send_invalid_response(reader, output_fd, request);
    }
    const auto opcode = static_cast<Opcode>(request.opcode);
    std::vector<std::uint8_t> body;

    switch (opcode) {
    case Opcode::HELLO:
        if (!require_empty(reader)) {
            return send_invalid_response(reader, output_fd, request);
        }
        if (documents.empty()) {
            return send_response(output_fd, request, Status::OPERATION_FAILED);
        }
        append_u32(body, kProtocolVersion);
        if (!append_node_id(body, 0, kCpioRootNodeId)) {
            return send_response(output_fd, request, Status::OPERATION_FAILED);
        }
        append_u64(body, kCpioDefaultMaxContentSize);
        append_u64(body, kCpioMaxEntryCount);
        append_u32(body, kCapabilities);
        append_u8(body, dirty ? 1U : 0U);
        return send_response(output_fd, request, Status::OK, body);

    case Opcode::STAT: {
        std::uint64_t id = 0;
        if (!reader.read_u64(id) || !require_empty(reader)) {
            return send_invalid_response(reader, output_fd, request);
        }
        const auto resolved = resolve_node(documents, id);
        if (!resolved) {
            return send_response(output_fd, request, Status::NOT_FOUND);
        }
        const auto node = resolved->document->stat(resolved->local_id);
        if (!node) {
            return send_response(output_fd, request, Status::NOT_FOUND);
        }
        if (!append_node(body, *node, resolved->document_index)) {
            return send_response(output_fd, request, Status::LIMIT_EXCEEDED);
        }
        return send_response(output_fd, request, Status::OK, body);
    }

    case Opcode::LIST: {
        std::uint64_t id = 0;
        if (!reader.read_u64(id) || !require_empty(reader)) {
            return send_invalid_response(reader, output_fd, request);
        }
        const auto resolved = resolve_node(documents, id);
        if (!resolved) {
            return send_response(output_fd, request, Status::NOT_FOUND);
        }
        const auto directory = resolved->document->stat(resolved->local_id);
        if (!directory) {
            return send_response(output_fd, request, Status::NOT_FOUND);
        }
        if ((directory->mode & S_IFMT) != S_IFDIR) {
            return send_invalid_response(reader, output_fd, request);
        }
        const auto entries = resolved->document->list(resolved->local_id);
        append_u32(body, static_cast<std::uint32_t>(entries.size()));
        for (const auto& entry : entries) {
            if (!append_node(body, entry, resolved->document_index)) {
                return send_response(output_fd, request, Status::LIMIT_EXCEEDED);
            }
        }
        return send_response(output_fd, request, Status::OK, body);
    }

    case Opcode::READ:
        return handle_read(documents, reader, output_fd, request);

    case Opcode::REPLACE: {
        std::uint64_t id = 0;
        if (!reader.read_u64(id)) {
            return send_invalid_response(reader, output_fd, request);
        }
        const auto resolved = resolve_node(documents, id);
        const bool success =
            resolved && resolved->document->replace_content(
                            resolved->local_id, [&](std::uint8_t* output, std::size_t capacity) {
                return reader.read_some(output, capacity);
            });
        if (!reader.drain()) {
            return false;
        }
        dirty = dirty || success;
        return send_response(output_fd, request, mutation_status(success));
    }

    case Opcode::CREATE_FILE: {
        std::uint64_t parent_id = 0;
        std::uint32_t permissions = 0;
        std::uint32_t uid = 0;
        std::uint32_t gid = 0;
        std::string name;
        if (!reader.read_u64(parent_id) || !reader.read_u32(permissions) || !reader.read_u32(uid) ||
            !reader.read_u32(gid) || !reader.read_string(name)) {
            return send_invalid_response(reader, output_fd, request);
        }
        const auto parent = resolve_node(documents, parent_id);
        CpioNodeId created_id = kCpioRootNodeId;
        const bool success =
            parent &&
            parent->document->create_file(
                parent->local_id, name, permissions, uid, gid,
                [&](std::uint8_t* output, std::size_t capacity) {
                    return reader.read_some(output, capacity);
                },
                &created_id);
        if (!reader.drain()) {
            return false;
        }
        dirty = dirty || success;
        if (success && !append_node_id(body, parent->document_index, created_id)) {
            return send_response(output_fd, request, Status::LIMIT_EXCEEDED);
        }
        return send_response(output_fd, request, mutation_status(success), body);
    }

    case Opcode::CREATE_DIRECTORY: {
        std::uint64_t parent_id = 0;
        std::uint32_t permissions = 0;
        std::uint32_t uid = 0;
        std::uint32_t gid = 0;
        std::string name;
        if (!reader.read_u64(parent_id) || !reader.read_u32(permissions) || !reader.read_u32(uid) ||
            !reader.read_u32(gid) || !reader.read_string(name) || !require_empty(reader)) {
            return send_invalid_response(reader, output_fd, request);
        }
        const auto parent = resolve_node(documents, parent_id);
        CpioNodeId created_id = kCpioRootNodeId;
        const bool success =
            parent && parent->document->create_directory(parent->local_id, name, permissions, uid,
                                                         gid, &created_id);
        dirty = dirty || success;
        if (success && !append_node_id(body, parent->document_index, created_id)) {
            return send_response(output_fd, request, Status::LIMIT_EXCEEDED);
        }
        return send_response(output_fd, request, mutation_status(success), body);
    }

    case Opcode::CREATE_SYMBOLIC_LINK: {
        std::uint64_t parent_id = 0;
        std::uint32_t uid = 0;
        std::uint32_t gid = 0;
        std::string name;
        std::string target;
        if (!reader.read_u64(parent_id) || !reader.read_u32(uid) || !reader.read_u32(gid) ||
            !reader.read_string(name) || !reader.read_string(target) || !require_empty(reader)) {
            return send_invalid_response(reader, output_fd, request);
        }
        const auto parent = resolve_node(documents, parent_id);
        CpioNodeId created_id = kCpioRootNodeId;
        const bool success =
            parent && parent->document->create_symbolic_link(parent->local_id, name, target, uid,
                                                             gid, &created_id);
        dirty = dirty || success;
        if (success && !append_node_id(body, parent->document_index, created_id)) {
            return send_response(output_fd, request, Status::LIMIT_EXCEEDED);
        }
        return send_response(output_fd, request, mutation_status(success), body);
    }

    case Opcode::CREATE_HARD_LINK: {
        std::uint64_t parent_id = 0;
        std::uint64_t target_id = 0;
        std::string name;
        if (!reader.read_u64(parent_id) || !reader.read_u64(target_id) ||
            !reader.read_string(name) || !require_empty(reader)) {
            return send_invalid_response(reader, output_fd, request);
        }
        const auto parent = resolve_node(documents, parent_id);
        const auto target = resolve_node(documents, target_id);
        CpioNodeId created_id = kCpioRootNodeId;
        const bool success =
            parent && target && parent->document_index == target->document_index &&
            parent->document->create_hard_link(parent->local_id, name, target->local_id,
                                               &created_id);
        dirty = dirty || success;
        if (success && !append_node_id(body, parent->document_index, created_id)) {
            return send_response(output_fd, request, Status::LIMIT_EXCEEDED);
        }
        return send_response(output_fd, request, mutation_status(success), body);
    }

    case Opcode::COPY:
    case Opcode::MOVE: {
        std::uint64_t id = 0;
        std::uint64_t destination_id = 0;
        std::string name;
        if (!reader.read_u64(id) || !reader.read_u64(destination_id) || !reader.read_string(name) ||
            !require_empty(reader)) {
            return send_invalid_response(reader, output_fd, request);
        }
        const auto source = resolve_node(documents, id);
        const auto destination = resolve_node(documents, destination_id);
        CpioNodeId created_id = kCpioRootNodeId;
        const bool success =
            source && destination && source->document_index == destination->document_index &&
            (opcode == Opcode::COPY
                 ? source->document->copy(source->local_id, destination->local_id, name, &created_id)
                 : source->document->move(source->local_id, destination->local_id, name));
        dirty = dirty || success;
        if (success && opcode == Opcode::COPY &&
            !append_node_id(body, source->document_index, created_id)) {
            return send_response(output_fd, request, Status::LIMIT_EXCEEDED);
        }
        return send_response(output_fd, request, mutation_status(success), body);
    }

    case Opcode::REMOVE: {
        std::uint64_t id = 0;
        std::uint8_t recursive = 0;
        if (!reader.read_u64(id) || !reader.read_u8(recursive) || !require_empty(reader) ||
            recursive > 1) {
            return send_invalid_response(reader, output_fd, request);
        }
        const auto resolved = resolve_node(documents, id);
        const bool success =
            resolved && resolved->document->remove(resolved->local_id, recursive != 0);
        dirty = dirty || success;
        return send_response(output_fd, request, mutation_status(success));
    }

    case Opcode::UPDATE_METADATA: {
        std::uint64_t id = 0;
        std::uint32_t mask = 0;
        if (!reader.read_u64(id) || !reader.read_u32(mask) || (mask & ~0xFU) != 0 || mask == 0) {
            return send_invalid_response(reader, output_fd, request);
        }
        CpioMetadataPatch patch;
        std::uint32_t value = 0;
        if ((mask & 0x1U) != 0) {
            if (!reader.read_u32(value)) {
                return send_invalid_response(reader, output_fd, request);
            }
            patch.permissions = value;
        }
        if ((mask & 0x2U) != 0) {
            if (!reader.read_u32(value)) {
                return send_invalid_response(reader, output_fd, request);
            }
            patch.uid = value;
        }
        if ((mask & 0x4U) != 0) {
            if (!reader.read_u32(value)) {
                return send_invalid_response(reader, output_fd, request);
            }
            patch.gid = value;
        }
        if ((mask & 0x8U) != 0) {
            if (!reader.read_u32(value)) {
                return send_invalid_response(reader, output_fd, request);
            }
            patch.mtime = value;
        }
        if (!require_empty(reader)) {
            return send_invalid_response(reader, output_fd, request);
        }
        const auto resolved = resolve_node(documents, id);
        const bool success =
            resolved && resolved->document->update_metadata(resolved->local_id, patch);
        dirty = dirty || success;
        return send_response(output_fd, request, mutation_status(success));
    }

    case Opcode::DUMP: {
        if (!require_empty(reader)) {
            return send_invalid_response(reader, output_fd, request);
        }
        const bool success = dump_action();
        if (success) {
            dirty = false;
        }
        return send_response(output_fd, request, success ? Status::OK : Status::IO_ERROR);
    }

    case Opcode::CLOSE:
        if (!require_empty(reader)) {
            return send_invalid_response(reader, output_fd, request);
        }
        should_close = true;
        return send_response(output_fd, request, Status::OK);

    case Opcode::LIST_RAMDISKS:
        if (!require_empty(reader)) {
            return send_invalid_response(reader, output_fd, request);
        }
        append_u32(body, static_cast<std::uint32_t>(documents.size()));
        for (std::size_t index = 0; index < documents.size(); ++index) {
            const auto& ramdisk = documents[index];
            const auto root_id = encode_node_id(index, kCpioRootNodeId);
            if (!root_id) {
                return send_response(output_fd, request, Status::LIMIT_EXCEEDED);
            }
            append_u32(body, static_cast<std::uint32_t>(index));
            append_u64(body, *root_id);
            append_u64(body, ramdisk.packed_size);
            append_u32(body, ramdisk.vendor_type);
            append_u8(body, ramdisk.is_vendor ? 1U : 0U);
            if (!append_string(body, ramdisk.name) ||
                !append_string(body, compression_name(ramdisk.compression))) {
                return send_response(output_fd, request, Status::LIMIT_EXCEEDED);
            }
            for (const std::uint32_t value : ramdisk.board_id) {
                append_u32(body, value);
            }
            if (body.size() > kMaximumControlPayload) {
                return send_response(output_fd, request, Status::LIMIT_EXCEEDED);
            }
        }
        return send_response(output_fd, request, Status::OK, body);
    }

    return send_invalid_response(reader, output_fd, request);
}

int run_document_session(std::vector<SessionDocument> documents, const DumpAction& dump_action,
                         int input_fd, int output_fd) {
    if (documents.empty() || documents.size() > kMaximumDocumentCount ||
        std::any_of(documents.begin(), documents.end(),
                    [](const SessionDocument& item) { return item.document == nullptr; })) {
        return 1;
    }
    bool dirty = false;
    bool should_close = false;
    while (!should_close) {
        FrameHeader request;
        const HeaderReadResult header_result = read_header(input_fd, request);
        if (header_result == HeaderReadResult::END_OF_STREAM) {
            return 0;
        }
        if (header_result == HeaderReadResult::ERROR) {
            return 1;
        }
        if (request.version != kProtocolVersion) {
            if (!send_response(output_fd, request, Status::UNSUPPORTED_VERSION)) {
                return 1;
            }
            return 1;
        }
        if ((request.opcode & kResponseFlag) != 0 ||
            request.payload_size > kMaximumRequestPayload) {
            if (!send_response(output_fd, request,
                               request.payload_size > kMaximumRequestPayload
                                   ? Status::LIMIT_EXCEEDED
                                   : Status::INVALID_REQUEST)) {
                return 1;
            }
            return 1;
        }

        PayloadReader reader(input_fd, request.payload_size);
        const bool response_sent = handle_request(documents, dump_action, dirty, should_close,
                                                  reader, output_fd, request);
        if (!reader.drain() || !response_sent) {
            return 1;
        }
    }
    return 0;
}

}  // namespace

int run_ramdisk_editor(const std::string& archive_path, int input_fd, int output_fd) {
    CpioDocument document;
    if (!document.load(archive_path)) {
        return 1;
    }
    std::vector<SessionDocument> documents = {
        SessionDocument{&document, "ramdisk", 0, {}, 0, FileFormat::UNKNOWN, false},
    };
    return run_document_session(
        std::move(documents), [&document, &archive_path]() { return document.dump(archive_path); },
        input_fd, output_fd);
}

int run_boot_ramdisk_editor(const std::string& source_image_path,
                            const std::string& output_image_path, int input_fd, int output_fd) {
    BootRamdiskDocument image;
    if (!image.load(source_image_path)) {
        LOGE("Failed to open boot ramdisk: %s\n", image.last_error().c_str());
        return 1;
    }
    std::vector<SessionDocument> documents;
    documents.reserve(image.ramdisk_count());
    for (std::size_t index = 0; index < image.ramdisk_count(); ++index) {
        const auto& info = image.ramdisk_info(index);
        documents.push_back(SessionDocument{&image.ramdisk(index), info.name, info.vendor_type,
                                            info.board_id, info.packed_size, info.compression,
                                            info.is_vendor});
    }
    return run_document_session(
        std::move(documents),
        [&image, &output_image_path]() { return image.dump(output_image_path); }, input_fd,
        output_fd);
}

}  // namespace ksud
