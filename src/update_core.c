#include "update_core.h"
#include <stdio.h>
#include <string.h>

#define UF2_MAGIC0 0x0A324655u
#define UF2_MAGIC1 0x9E5D5157u
#define UF2_MAGIC_END 0x0AB16F30u
#define UF2_FLAG_FAMILY 0x2000u
#define RP2350_ARM_S 0xE48BFF59u
#define RP2350_MARKER_FAMILY 0xE48BFF57u
#define RP2350_MARKER_FLAGS 0xA000u
#define RP2350_MARKER_ADDRESS 0x10FFFF00u

static bool parse_version(const char *s, unsigned v[3]) {
    for (int i = 0; i < 3; i++) {
        if (*s < '0' || *s > '9') return false;
        unsigned n = 0;
        while (*s >= '0' && *s <= '9') {
            if (n > 100000) return false;
            n = n * 10 + (unsigned)(*s++ - '0');
        }
        v[i] = n;
        if (i < 2 && *s++ != '.') return false;
    }
    return *s == 0;
}

bool update_is_newer(const char *release, const char *current) {
    unsigned r[3], c[3];
    if (!release || !parse_version(release, r)) return false;
    if (!current || !parse_version(current, c)) return true;
    for (int i = 0; i < 3; i++)
        if (r[i] != c[i]) return r[i] > c[i];
    return false;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool update_parse_sums(const char *text, uint8_t sha256[32], char name[UPDATE_NAME_LEN],
                       char version[UPDATE_VERSION_LEN]) {
    static const char prefix[] = "picocalc_ssh-v", suffix[] = ".uf2";
    for (const char *line = text; line && *line;) {
        const char *end = strchr(line, '\n');
        size_t length = end ? (size_t)(end - line) : strlen(line);
        if (length && line[length - 1] == '\r') length--;
        size_t at = 64;
        bool ok = length > at + 2;
        for (size_t i = 0; ok && i < 32; i++) {
            int hi = hex_value(line[i * 2]), lo = hex_value(line[i * 2 + 1]);
            if (hi < 0 || lo < 0) ok = false;
            else sha256[i] = (uint8_t)(hi << 4 | lo);
        }
        if (ok && line[at] == ' ' && (line[at + 1] == ' ' || line[at + 1] == '*')) {
            const char *file = line + at + 2;
            size_t file_len = length - at - 2, pre = sizeof(prefix) - 1, suf = sizeof(suffix) - 1;
            if (file_len > pre + suf && file_len < UPDATE_NAME_LEN && !strncmp(file, prefix, pre) &&
                !strncmp(file + file_len - suf, suffix, suf) && file_len - pre - suf < UPDATE_VERSION_LEN) {
                memcpy(name, file, file_len);
                name[file_len] = 0;
                memcpy(version, file + pre, file_len - pre - suf);
                version[file_len - pre - suf] = 0;
                unsigned v[3];
                if (parse_version(version, v)) return true;
            }
        }
        line = end ? end + 1 : NULL;
    }
    return false;
}

static uint32_t word(const uint8_t *b, unsigned offset) {
    return (uint32_t)b[offset] | (uint32_t)b[offset + 1] << 8 | (uint32_t)b[offset + 2] << 16 |
           (uint32_t)b[offset + 3] << 24;
}

void uf2_check_init(uf2_check_t *check) {
    memset(check, 0, sizeof(*check));
    check->min_address = UINT32_MAX;
}

static bool check_fail(uf2_check_t *check, const char *message) {
    if (!check->failed) snprintf(check->error, sizeof(check->error), "%s (block %lu)", message,
                                 (unsigned long)check->blocks);
    check->failed = true;
    return false;
}

bool uf2_check_block(uf2_check_t *check, const uint8_t block[UF2_BLOCK]) {
    if (check->failed) return false;
    uint32_t flags = word(block, 8), address = word(block, 12), size = word(block, 16);
    uint32_t number = word(block, 20), total = word(block, 24), family = word(block, 28);
    if (word(block, 0) != UF2_MAGIC0 || word(block, 4) != UF2_MAGIC1 || word(block, 508) != UF2_MAGIC_END)
        return check_fail(check, "Bad UF2 magic");
    if (size != 256 || !(flags & UF2_FLAG_FAMILY)) return check_fail(check, "Bad UF2 payload");
    if (family == RP2350_MARKER_FAMILY && flags == RP2350_MARKER_FLAGS && address == RP2350_MARKER_ADDRESS) {
        if (++check->markers > 1) return check_fail(check, "Duplicate RP2350 marker");
    } else {
        if (family != RP2350_ARM_S || flags != UF2_FLAG_FAMILY) return check_fail(check, "Not a Pico 2 app");
        if (address < UF2_APP_BASE || address > UF2_APP_LIMIT - 256 || address % 256)
            return check_fail(check, "Address outside app flash");
        if (number != check->app_blocks || (check->app_blocks && total != check->total) || !total)
            return check_fail(check, "Blocks out of order");
        uint32_t page = (address - UF2_APP_BASE) / 256u;
        if (check->seen[page / 8] & (1u << (page % 8))) return check_fail(check, "Duplicate app address");
        check->seen[page / 8] |= (uint8_t)(1u << (page % 8));
        check->total = total;
        check->app_blocks++;
        if (address < check->min_address) check->min_address = address;
    }
    check->blocks++;
    return true;
}

bool uf2_check_finish(uf2_check_t *check) {
    if (check->failed) return false;
    if (!check->app_blocks || check->app_blocks != check->total) return check_fail(check, "Incomplete UF2");
    if (check->min_address != UF2_APP_BASE) return check_fail(check, "Wrong app base address");
    return true;
}

uf2_match_t uf2_block_matches(const uint8_t block[UF2_BLOCK], const uint8_t *image, size_t image_size) {
    if (word(block, 0) != UF2_MAGIC0 || word(block, 4) != UF2_MAGIC1 || word(block, 508) != UF2_MAGIC_END ||
        word(block, 16) != 256)
        return UF2_MATCH_INVALID;
    uint32_t address = word(block, 12);
    if (word(block, 8) != UF2_FLAG_FAMILY || word(block, 28) != RP2350_ARM_S) return UF2_MATCH_OTHER;
    if (address < UF2_APP_BASE || address % 256 || address - UF2_APP_BASE > image_size ||
        image_size - (address - UF2_APP_BASE) < 256)
        return UF2_MATCH_DIFFERENT;
    const uint8_t *payload = block + 32, *flash = image + (address - UF2_APP_BASE);
    for (unsigned i = 0; i < 256; i++) {
        uint32_t at = address + i;
        if (at >= UF2_PROGINFO_START && at < UF2_PROGINFO_END) continue;
        if (payload[i] != flash[i]) return UF2_MATCH_DIFFERENT;
    }
    return UF2_MATCH_SAME;
}
