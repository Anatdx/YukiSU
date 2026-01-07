#include "apk_sign.hpp"
#include "../log.hpp"
#include "picosha2.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace ksud {

static std::string sha256_digest(const uint8_t* data, size_t len) {
    std::vector<unsigned char> hash(picosha2::k_digest_size);
    picosha2::hash256(data, data + len, hash.begin(), hash.end());
    return picosha2::bytes_to_hex_string(hash.begin(), hash.end());
}

std::pair<uint32_t, std::string> get_apk_signature(const std::string& apk_path) {
    std::ifstream ifs(apk_path, std::ios::binary | std::ios::ate);
    if (!ifs) {
        LOGE("Failed to open APK: %s", apk_path.c_str());
        return {0, ""};
    }

    std::streamsize file_size = ifs.tellg();
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
                if (i > 0) {
                    LOGW("APK comment length is %ld", i);
                }
                found_eocd = true;
                break;
            }
        }

        if (n == 0xffff) {
            LOGE("Not a valid ZIP file");
            return {0, ""};
        }

        i++;
        if (i > file_size) {
            LOGE("EOCD not found");
            return {0, ""};
        }
    }

    if (!found_eocd) {
        return {0, ""};
    }

    // Read central directory offset
    ifs.seekg(12, std::ios::cur);
    ifs.read(reinterpret_cast<char*>(&cd_offset), 4);

    // Seek to APK Signing Block
    ifs.seekg(cd_offset - 0x18, std::ios::beg);

    uint64_t block_size;
    char magic[16];
    ifs.read(reinterpret_cast<char*>(&block_size), 8);
    ifs.read(magic, 16);

    if (memcmp(magic, "APK Sig Block 42", 16) != 0) {
        LOGE("APK Signing Block not found");
        return {0, ""};
    }

    // Seek to start of signing block
    uint64_t block_start = cd_offset - (block_size + 8);
    ifs.seekg(block_start, std::ios::beg);

    uint64_t block_size_check;
    ifs.read(reinterpret_cast<char*>(&block_size_check), 8);

    if (block_size != block_size_check) {
        LOGE("APK Signing Block size mismatch");
        return {0, ""};
    }

    // Parse ID-value pairs
    std::pair<uint32_t, std::string> v2_signature;
    bool v3_found = false;
    bool v31_found = false;

    while (ifs.tellg() < static_cast<std::streamoff>(cd_offset - 0x18)) {
        uint64_t pair_len;
        uint32_t pair_id;

        ifs.read(reinterpret_cast<char*>(&pair_len), 8);

        // Check if we've reached the end marker
        if (pair_len == block_size) {
            break;
        }

        ifs.read(reinterpret_cast<char*>(&pair_id), 4);
        std::streampos value_start = ifs.tellg();

        if (pair_id == 0x7109871a) {
            // V2 signature scheme
            uint32_t signer_seq_len, signer_len, signed_data_len;
            ifs.read(reinterpret_cast<char*>(&signer_seq_len), 4);
            ifs.read(reinterpret_cast<char*>(&signer_len), 4);
            ifs.read(reinterpret_cast<char*>(&signed_data_len), 4);

            // Skip digests
            uint32_t digests_len;
            ifs.read(reinterpret_cast<char*>(&digests_len), 4);
            ifs.seekg(digests_len, std::ios::cur);

            // Read certificate
            uint32_t certs_len, cert_len;
            ifs.read(reinterpret_cast<char*>(&certs_len), 4);
            ifs.read(reinterpret_cast<char*>(&cert_len), 4);

            std::vector<uint8_t> cert_data(cert_len);
            ifs.read(reinterpret_cast<char*>(cert_data.data()), cert_len);

            v2_signature = {cert_len, sha256_digest(cert_data.data(), cert_len)};

        } else if (pair_id == 0xf05368c0) {
            // V3 signature scheme
            v3_found = true;
        } else if (pair_id == 0x1b93ad61) {
            // V3.1 signature scheme
            v31_found = true;
        }

        // Skip to next pair
        ifs.seekg(value_start);
        ifs.seekg(pair_len - 4, std::ios::cur);
    }

    if (v3_found || v31_found) {
        LOGE("Unexpected v3/v3.1 signature found!");
        return {0, ""};
    }

    if (v2_signature.first == 0) {
        LOGE("No v2 signature found!");
        return {0, ""};
    }

    return v2_signature;
}

}  // namespace ksud
