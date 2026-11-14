#ifndef UPDATE_CORE_H
#define UPDATE_CORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UPDATE_VERSION_LEN 24
#define UPDATE_NAME_LEN 64
#define UF2_BLOCK 512u
#define UF2_APP_BASE 0x10000000u
#define UF2_APP_LIMIT 0x10400000u
/* uf2loader patches proginfo into this range of the first flash page on RP2040; ignore it. */
#define UF2_PROGINFO_START 0x10000020u
#define UF2_PROGINFO_END 0x1000002Cu
#define UF2_MAX_FILE (UF2_BLOCK * ((UF2_APP_LIMIT - UF2_APP_BASE) / 256u + 1u))

/* True when release (strict MAJOR.MINOR.PATCH) is newer than current. Development or
 * unparseable current versions are always treated as older than a release. */
bool update_is_newer(const char *release, const char *current);

/* Finds the "<sha256>  picocalc_ssh-vX.Y.Z.uf2" line in a release SHA256SUMS file. */
bool update_parse_sums(const char *text, uint8_t sha256[32], char name[UPDATE_NAME_LEN],
                       char version[UPDATE_VERSION_LEN]);

typedef struct {
    uint32_t blocks, app_blocks, markers, total;
    uint32_t min_address;
    uint8_t seen[(UF2_APP_LIMIT - UF2_APP_BASE) / 256u / 8u]; /* application pages already written */
    bool failed;
    char error[64];
} uf2_check_t;

void uf2_check_init(uf2_check_t *check);
bool uf2_check_block(uf2_check_t *check, const uint8_t block[UF2_BLOCK]);
/* Final whole-file checks; returns false and sets check->error when invalid. */
bool uf2_check_finish(uf2_check_t *check);

typedef enum { UF2_MATCH_OTHER, UF2_MATCH_SAME, UF2_MATCH_DIFFERENT, UF2_MATCH_INVALID } uf2_match_t;
/* Compares one UF2 block with the running image mapped at UF2_APP_BASE. Non-application
 * blocks (e.g. the RP2350 compatibility marker) return UF2_MATCH_OTHER. */
uf2_match_t uf2_block_matches(const uint8_t block[UF2_BLOCK], const uint8_t *image, size_t image_size);
#endif
