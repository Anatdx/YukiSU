#include "apk_sign.hpp"
#include <mbedtls/sha256.h>
#include "../log.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

namespace ksud {

namespace {

constexpr const char* hex_chars = "0123456789abcdef";

std::string sha256_digest(const uint8_t* data, size_t len) {
    unsigned char digest[32];
    mbedtls_sha256(data, len, digest, 0);
    std::string out;
    out.reserve(64);
    for (size_t i = 0; i < 32; i++) {
        out.push_back(hex_chars[(digest[i] >> 4) & 0xf]);
        out.push_back(hex_chars[digest[i] & 0xf]);
    }
    return out;
}

struct ZipCentralDirectoryHeader {
    uint32_t signature;
    uint16_t version_made_by;
    uint16_t version_needed;
    uint16_t flags;
    uint16_t compression;
    uint16_t mod_time;
    uint16_t mod_date;
    uint32_t crc32;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint16_t file_name_length;
    uint16_t extra_field_length;
    uint16_t file_comment_length;
    uint16_t disk_number_start;
    uint16_t internal_file_attributes;
    uint32_t external_file_attributes;
    uint32_t relative_offset;
} __attribute__((packed));

bool has_v1_signature_file(std::ifstream& ifs, uint32_t cd_offset, uint32_t cd_size) {
    constexpr uint32_t kCentralDirectoryHeaderSignature = 0x02014b50;
    constexpr const char* kManifest = "META-INF/MANIFEST.MF";
    const auto original_pos = ifs.tellg();
    const auto cd_end = static_cast<std::streamoff>(cd_offset) + cd_size;

    ifs.clear();
    ifs.seekg(cd_offset, std::ios::beg);

    ZipCentralDirectoryHeader header{};
    while (ifs.tellg() < cd_end && ifs.read(reinterpret_cast<char*>(&header), sizeof(header))) {
        if (header.signature != kCentralDirectoryHeaderSignature) {
            break;
        }

        std::string file_name(header.file_name_length, '\0');
        ifs.read(file_name.data(), header.file_name_length);
        if (!ifs) {
            break;
        }

        if (file_name == kManifest) {
            ifs.clear();
            ifs.seekg(original_pos);
            return true;
        }

        ifs.seekg(
            static_cast<std::streamoff>(header.extra_field_length) + header.file_comment_length,
            std::ios::cur);
        if (!ifs) {
            break;
        }
    }

    ifs.clear();
    ifs.seekg(original_pos);
    return false;
}

}  // namespace

ApkSignatureInfo get_apk_signature(const std::string& apk_path) {
    ApkSignatureInfo info{};

    std::ifstream ifs(apk_path, std::ios::binary | std::ios::ate);
    if (!ifs) {
        LOGE("Failed to open APK: %s", apk_path.c_str());
        return info;
    }

    const std::streamsize file_size = ifs.tellg();
    ifs.seekg(0);

    // Find EOCD (End of Central Directory)
    int64_t i = 0;
    bool found_eocd = false;
    uint32_t cd_offset = 0;

    while (true) {
        uint16_t n;
        ifs.seekg(-i - 2, std::ios::end);
        ifs.read(reinterpret_cast<char*>(&n), 2);

        if (static_cast<int64_t>(n) == i) {
            ifs.seekg(-22, std::ios::cur);
            uint32_t magic;
            ifs.read(reinterpret_cast<char*>(&magic), 4);

            // Check if this is EOCD and has APK Signing Block marker
            if ((magic ^ 0xcafebabe) == 0xccfbf1ee) {
                found_eocd = true;
                break;
            }
        }

        if (n == 0xffff) {
            LOGE("Not a valid ZIP file");
            return info;
        }

        i++;
        if (i > file_size) {
            LOGE("EOCD not found");
            return info;
        }
    }

    if (!found_eocd) {
        return info;
    }
    info.valid = true;

    // Read central directory offset
    uint32_t cd_size = 0;
    ifs.seekg(8, std::ios::cur);
    ifs.read(reinterpret_cast<char*>(&cd_size), 4);
    ifs.read(reinterpret_cast<char*>(&cd_offset), 4);
    info.v1 = has_v1_signature_file(ifs, cd_offset, cd_size);

    if (cd_offset < 0x18) {
        return info;
    }

    // Seek to APK Signing Block
    ifs.seekg(cd_offset - 0x18, std::ios::beg);

    uint64_t block_size;
    std::array<char, 16> magic{};
    ifs.read(reinterpret_cast<char*>(&block_size), 8);
    ifs.read(magic.data(), 16);
    if (!ifs) {
        return info;
    }

    if (memcmp(magic.data(), "APK Sig Block 42", 16) != 0) {
        return info;
    }

    // Seek to start of signing block
    if (block_size + 8 > cd_offset) {
        return info;
    }
    const uint64_t block_start = cd_offset - (block_size + 8);
    ifs.seekg(block_start, std::ios::beg);

    uint64_t block_size_check;
    ifs.read(reinterpret_cast<char*>(&block_size_check), 8);

    if (block_size != block_size_check) {
        LOGE("APK Signing Block size mismatch");
        return info;
    }

    // Parse ID-value pairs
    while (ifs.tellg() < static_cast<std::streamoff>(cd_offset - 0x18)) {
        uint64_t pair_len;
        uint32_t pair_id;

        ifs.read(reinterpret_cast<char*>(&pair_len), 8);
        if (!ifs || pair_len < sizeof(pair_id)) {
            break;
        }

        // Check if we've reached the end marker
        if (pair_len == block_size) {
            break;
        }

        ifs.read(reinterpret_cast<char*>(&pair_id), 4);
        const std::streampos value_start = ifs.tellg();

        if (pair_id == 0x7109871a) {
            // V2 signature scheme
            uint32_t signer_seq_len;
            uint32_t signer_len;
            uint32_t signed_data_len;
            ifs.read(reinterpret_cast<char*>(&signer_seq_len), 4);
            ifs.read(reinterpret_cast<char*>(&signer_len), 4);
            ifs.read(reinterpret_cast<char*>(&signed_data_len), 4);

            // Skip digests
            uint32_t digests_len;
            ifs.read(reinterpret_cast<char*>(&digests_len), 4);
            ifs.seekg(digests_len, std::ios::cur);

            // Read certificate
            uint32_t certs_len;
            uint32_t cert_len;
            ifs.read(reinterpret_cast<char*>(&certs_len), 4);
            ifs.read(reinterpret_cast<char*>(&cert_len), 4);

            std::vector<uint8_t> cert_data(cert_len);
            ifs.read(reinterpret_cast<char*>(cert_data.data()), cert_len);

            info.v2 = {true, cert_len, sha256_digest(cert_data.data(), cert_len)};

        } else if (pair_id == 0xf05368c0) {
            // V3 signature scheme
            info.v3 = true;
        } else if (pair_id == 0x1b93ad61) {
            // V3.1 signature scheme
            info.v31 = true;
        }

        // Skip to next pair
        ifs.seekg(value_start);
        ifs.seekg(pair_len - 4, std::ios::cur);
    }

    return info;
}

}  // namespace ksud
