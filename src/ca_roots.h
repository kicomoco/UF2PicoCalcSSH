#ifndef CA_ROOTS_H
#define CA_ROOTS_H
typedef struct {
    const char *name;
    const unsigned char *der;
    unsigned size;
} ca_root_t;
extern const ca_root_t ca_roots[];
extern const unsigned ca_root_count;
#endif
