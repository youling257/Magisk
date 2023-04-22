#include <base.hpp>

using namespace std;

#define APK_SIGNING_BLOCK_MAGIC     "APK Sig Block 42"
#define SIGNATURE_SCHEME_V2_MAGIC   0x7109871a
#define EOCD_MAGIC                  0x6054b50

// Top-level block container
struct signing_block {
    uint64_t block_sz;

    struct id_value_pair {
        uint64_t len;
        struct /* v2_signature */ {
            uint32_t id;
            uint8_t value[0];  // size = (len - 4)
        };
    } id_value_pair_sequence[0];

    uint64_t block_sz_;   // *MUST* be same as block_sz
    char magic[16];       // "APK Sig Block 42"
};

struct len_prefixed {
    uint32_t len;
};

// Generic length prefixed raw data
struct len_prefixed_value : public len_prefixed {
    uint8_t value[0];
};

// V2 Signature Block
struct v2_signature {
    uint32_t id;  // 0x7109871a
    uint32_t signer_sequence_len;
    struct signer : public len_prefixed {
        struct signed_data : public len_prefixed {
            uint32_t digest_sequence_len;
            struct : public len_prefixed {
                uint32_t algorithm;
                len_prefixed_value digest;
            } digest_sequence[0];

            uint32_t certificate_sequence_len;
            len_prefixed_value certificate_sequence[0];

            uint32_t attribute_sequence_len;
            struct attribute : public len_prefixed {
                uint32_t id;
                uint8_t value[0];  // size = (len - 4)
            } attribute_sequence[0];
        } signed_data;

        uint32_t signature_sequence_len;
        struct : public len_prefixed {
            uint32_t id;
            len_prefixed_value signature;
        } signature_sequence[0];

        len_prefixed_value public_key;
    } signer_sequence[0];
};

// End of central directory record
struct EOCD {
    uint32_t magic;            // 0x6054b50
    uint8_t pad[8];            // 8 bytes of irrelevant data
    uint32_t central_dir_sz;   // size of central directory
    uint32_t central_dir_off;  // offset of central directory
    uint16_t comment_sz;       // size of comment
    char comment[0];
} __attribute__((packed));

/*
 * A v2/v3 signed APK has the format as following
 *
 * +---------------+
 * | zip content   |
 * +---------------+
 * | signing block |
 * +---------------+
 * | central dir   |
 * +---------------+
 * | EOCD          |
 * +---------------+
 *
 * Scan from end of file to find EOCD, and figure our way back to the
 * offset of the signing block. Next, directly extract the certificate
 * from the v2 signature block.
 *
 * All structures above are mostly just for documentation purpose.
 *
 * This method extracts the first certificate of the first signer
 * within the APK v2 signature block.
 */
string read_certificate(int fd, int version) {
    uint32_t size4;
    uint64_t size8;

    // Find EOCD
    for (int i = 0;; i++) {
        // i is the absolute offset to end of file
        uint16_t comment_sz = 0;
        lseek(fd, -((off_t) sizeof(comment_sz)) - i, SEEK_END);
        read(fd, &comment_sz, sizeof(comment_sz));
        if (comment_sz == i) {
            // Double check if we actually found the structure
            lseek(fd, -((off_t) sizeof(EOCD)), SEEK_CUR);
            uint32_t magic = 0;
            read(fd, &magic, sizeof(magic));
            if (magic == EOCD_MAGIC) {
                break;
            }
        }
        if (i == 0xffff) {
            // Comments cannot be longer than 0xffff (overflow), abort
            return {};
        }
    }

    // We are now at EOCD + sizeof(magic)
    // Seek and read central_dir_off to find start of central directory
    uint32_t central_dir_off = 0;
    {
        constexpr off_t off = offsetof(EOCD, central_dir_off) - sizeof(EOCD::magic);
        lseek(fd, off, SEEK_CUR);
    }
    read(fd, &central_dir_off, sizeof(central_dir_off));

    // Read comment
    if (version >= 0) {
        uint16_t comment_sz = 0;
        read(fd, &comment_sz, sizeof(comment_sz));
        string comment;
        comment.resize(comment_sz);
        read(fd, comment.data(), comment_sz);
        if (version > parse_int(comment)) {
            // Older version of magisk app is not supported
            return {};
        }
    }

    // Next, find the start of the APK signing block
    {
        constexpr int off = sizeof(signing_block::block_sz_) + sizeof(signing_block::magic);
        lseek(fd, (off_t) (central_dir_off - off), SEEK_SET);
    }
    read(fd, &size8, sizeof(size8));  // size8 = block_sz_
    char magic[sizeof(signing_block::magic)] = {0};
    read(fd, magic, sizeof(magic));
    if (memcmp(magic, APK_SIGNING_BLOCK_MAGIC, sizeof(magic)) != 0) {
        // Invalid signing block magic, abort
        return {};
    }
    uint64_t signing_blk_sz = 0;
    lseek(fd, (off_t) (central_dir_off - size8 - sizeof(signing_blk_sz)), SEEK_SET);
    read(fd, &signing_blk_sz, sizeof(signing_blk_sz));
    if (signing_blk_sz != size8) {
        // block_sz != block_sz_, invalid signing block format, abort
        return {};
    }

    // Finally, we are now at the beginning of the id-value pair sequence

    for (;;) {
        read(fd, &size8, sizeof(size8)); // id-value pair length
        if (size8 == signing_blk_sz) {
            // Outside of the id-value pair sequence; actually reading block_sz_
            break;
        }

        uint32_t id;
        read(fd, &id, sizeof(id));
        if (id == SIGNATURE_SCHEME_V2_MAGIC) {
            read(fd, &size4, sizeof(size4)); // signer sequence length

            read(fd, &size4, sizeof(size4)); // signer length
            read(fd, &size4, sizeof(size4)); // signed data length

            read(fd, &size4, sizeof(size4)); // digest sequence length
            lseek(fd, (off_t) (size4), SEEK_CUR); // skip all digests

            read(fd, &size4, sizeof(size4)); // cert sequence length
            read(fd, &size4, sizeof(size4)); // cert length

            string cert;
            cert.resize(size4);
            read(fd, cert.data(), size4);

            return cert;
        } else {
            // Skip this id-value pair
            lseek(fd, (off_t) (size8 - sizeof(id)), SEEK_CUR);
        }
    }
    return {};
}
