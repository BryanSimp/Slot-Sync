/* SlotSync binary wire protocol v1, client side.
 *
 * The C counterpart of server/src/slotsync/protocol.py. Written from
 * docs/PROTOCOL.md rather than translated from the Python, for the same reason
 * scripts/fake_console.py is standalone: two implementations that agree are
 * evidence, one that agrees with itself is not.
 *
 * Freestanding. Fixed-size header filled in place in a caller-owned buffer, no
 * allocation, no globals, no connection state -- PLAN.md section 2, because
 * this is the code that eventually moves into Nintendont's ARM kernel.
 */

#ifndef SLOTSYNC_PROTOCOL_H
#define SLOTSYNC_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define SS_MAGIC "SLOT"
#define SS_VERSION 1

#define SS_HEADER_SIZE 96
#define SS_HMAC_OFFSET 64
#define SS_HMAC_SIZE 32
#define SS_NONCE_OFFSET 48
#define SS_NONCE_SIZE 16

/* One chunk must fit in a single datagram under a 1500-byte MTU. Do not raise:
 * a 2006 WiFi stack cannot be assumed to handle IP fragmentation. */
#define SS_MAX_PAYLOAD 1024
#define SS_MAX_DATAGRAM (SS_HEADER_SIZE + SS_MAX_PAYLOAD)

#define SS_GAME_ID_LEN 6

/* Message types. */
enum {
    SS_HELLO = 0x01,
    SS_PULL_REQ = 0x02,
    SS_PUSH_BEGIN = 0x03,
    SS_PUSH_CHUNK = 0x04,
    SS_PUSH_END = 0x05,
    SS_ACK = 0x06,
    SS_NACK = 0x07,
    SS_PULL_CHUNK = 0x08,
    SS_HEARTBEAT = 0x09,
    SS_PUSH_DELTA = 0x0A
};

/* Header flags. Bit 0 on a PUSH_BEGIN asks the server to seed its staging
 * buffer from `parent_version` rather than from zeros, so only the chunks that
 * changed have to be sent. Every other message sends flags = 0.
 * docs/PROTOCOL.md, "Delta push". */
#define SS_FLAG_DELTA 0x0001

/* NACK error codes, as they appear on the wire. Prefixed SS_NACK_ to keep them
 * distinct from the SS_ERR_ result codes in client.h -- the two sets overlap
 * numerically and mean entirely different things. */
enum {
    SS_NACK_BAD_HMAC = 0x01,
    SS_NACK_UNSUPPORTED_VERSION = 0x02,
    SS_NACK_MALFORMED = 0x03,
    SS_NACK_UNKNOWN_CARD = 0x04,
    SS_NACK_CONFLICT = 0x05,
    SS_NACK_MISSING_CHUNKS = 0x06,
    SS_NACK_CHECKSUM = 0x07,
    SS_NACK_VALIDATION = 0x08,
    SS_NACK_TOO_LARGE = 0x09,
    SS_NACK_RATE_LIMITED = 0x0A,
    SS_NACK_STAGING_EXPIRED = 0x0B,
    SS_NACK_DELTA_UNAVAILABLE = 0x0C
};

/* The header, unpacked. Field names match docs/PROTOCOL.md. */
typedef struct {
    uint8_t version;
    uint8_t msg_type;
    uint16_t flags;
    uint64_t device_id;
    char game_id[SS_GAME_ID_LEN + 1]; /* NUL-terminated for convenience */
    uint8_t slot;
    uint32_t card_version;
    uint32_t parent_version;
    uint32_t offset;
    uint32_t length;
    uint32_t total_size;
    uint32_t sequence;
    uint8_t nonce[SS_NONCE_SIZE];
} ss_header;

/* Zero a header and set the fields every message carries. */
void ss_header_init(ss_header *h, uint8_t msg_type, uint64_t device_id,
                    const char *game_id, uint8_t slot);

/* Serialise `h` plus `payload` into `out`, signing with `key`.
 * `out` must have room for SS_HEADER_SIZE + payload_len bytes.
 * Returns the total length written, or -1 if the payload is too large. */
int ss_pack(uint8_t *out, size_t out_cap, const ss_header *h, const uint8_t *payload,
            size_t payload_len, const uint8_t *key, size_t key_len);

/* Parse and authenticate `datagram`. On success fills `h`, and points
 * `*payload` into `datagram` (no copy). Returns 0, or a negative ss_parse
 * error. */
int ss_unpack(const uint8_t *datagram, size_t len, const uint8_t *key, size_t key_len,
              ss_header *h, const uint8_t **payload, size_t *payload_len);

enum {
    SS_PARSE_OK = 0,
    SS_PARSE_SHORT = -1,     /* smaller than the header, or over MTU */
    SS_PARSE_BAD_MAGIC = -2,
    SS_PARSE_BAD_VERSION = -3,
    SS_PARSE_BAD_HMAC = -4,
    SS_PARSE_BAD_LENGTH = -5 /* header disagrees with the bytes present */
};

/* Chunk bitmaps. A bitmap covers a window starting at a chunk index carried in
 * the header's `sequence` field, because a whole 16 MiB card needs 2048 bitmap
 * bytes against a 1024-byte payload cap. Bit order is LSB-first: chunk
 * `base + n` is bitmap[n >> 3] & (1 << (n & 7)). */
int ss_bitmap_test(const uint8_t *bitmap, size_t bitmap_len, uint32_t base,
                   uint32_t index);
void ss_bitmap_set(uint8_t *bitmap, size_t bitmap_len, uint32_t base, uint32_t index);

/* Chunks a transfer of `total_size` bytes takes. */
uint32_t ss_chunk_count(uint32_t total_size);

/* Big-endian helpers, exposed because the platform layer needs them too. */
void ss_put_u16(uint8_t *p, uint16_t v);
void ss_put_u32(uint8_t *p, uint32_t v);
void ss_put_u64(uint8_t *p, uint64_t v);
uint16_t ss_get_u16(const uint8_t *p);
uint32_t ss_get_u32(const uint8_t *p);
uint64_t ss_get_u64(const uint8_t *p);

#endif /* SLOTSYNC_PROTOCOL_H */
