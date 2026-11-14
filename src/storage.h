#ifndef STORAGE_H
#define STORAGE_H
#include "model.h"
typedef struct {
    char name[256];
    bool directory;
} file_entry_t;
bool storage_init(settings_t *settings);
bool storage_save(const settings_t *settings);
const char *storage_error(void);
/* Device preferences live in a separate file so the profile format is unchanged. */
bool storage_load_prefs(prefs_t *prefs);
bool storage_save_prefs(const prefs_t *prefs);
bool storage_read_key(const char *path, uint8_t **data, size_t *size);
bool storage_list(const char *path, file_entry_t *entries, unsigned capacity,
                  unsigned *count, unsigned skip, bool *more);
#endif
