/*
 * The SD card's storage, and the FAT32 the emulator makes cards with.
 *
 * A card is laid out as a card from a shop: an MBR at sector 0 with one FAT32
 * (LBA) partition from sector 2048, 32 reserved sectors (boot sector, FSInfo at
 * +1, their backups at +6/+7), two FATs, the root directory at cluster 2.
 *
 * A folder card is such a card built in memory: every file of the folder is
 * written in, directories and long names included. sdcard_sync walks the
 * card's FAT the other way and writes back into the folder whatever changed,
 * and deletes files the program deleted.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include "sdcard.h"

#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#define fseek64 _fseeki64
#define ftell64 _ftelli64
#else
#define make_dir(p) mkdir(p, 0777)
#define fseek64 fseeko
#define ftell64 ftello
#endif

#define SECTOR 512u
#define PART_LBA 2048u
#define RSV 32u
#define EOC 0x0FFFFFFFu
#define FAT_DATE ((uint16_t)(((2026 - 1980) << 9) | (1 << 5) | 1))

struct SdCard {
    FILE    *f;             /* an image file */
    uint8_t *mem;           /* or a card in memory, built from folder */
    uint64_t size;
    char     folder[1024];
    bool     dirty;
    char   **files;         /* the folder's files (relative paths) at the last sync */
    int      nfiles;
};

/* ------------------------------------------------------------------------ */
/* The FAT32 layout of a card of a given size */

typedef struct {
    uint32_t sectors;       /* in the partition */
    uint32_t spc;           /* sectors a cluster */
    uint32_t fatsz;         /* sectors a FAT */
    uint32_t nclus;         /* data clusters, numbered from 2 */
    uint64_t fat_lba, data_lba;
} Geo;

static bool geometry(uint64_t size, Geo *g)
{
    const uint64_t total = size / SECTOR;
    if (total < PART_LBA + 70000u || total - PART_LBA > 0xFFFFFFFFu) return false;
    g->sectors = (uint32_t)(total - PART_LBA);
    g->spc = 1;
    while (g->sectors / g->spc > 4000000u && g->spc < 64) g->spc *= 2;
    g->fatsz = 1;
    for (;;) {
        const uint32_t data = g->sectors - RSV - 2 * g->fatsz;
        g->nclus = data / g->spc;
        const uint32_t need = (uint32_t)(((uint64_t)g->nclus + 2) * 4 + SECTOR - 1) / SECTOR;
        if (need <= g->fatsz) break;
        g->fatsz = need;
    }
    if (g->nclus < 65525u) return false;     /* too small to be FAT32 */
    g->fat_lba = PART_LBA + RSV;
    g->data_lba = g->fat_lba + 2ull * g->fatsz;
    return true;
}

static uint64_t clus_off(const Geo *g, uint32_t c)
{
    return (g->data_lba + (uint64_t)(c - 2) * g->spc) * SECTOR;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return get16(p) | (uint32_t)get16(p + 2) << 16; }

/* Where format writes: memory, or a file */
typedef struct { uint8_t *mem; FILE *f; } Out;

static bool out_put(Out *o, uint64_t off, const void *data, size_t n)
{
    if (o->mem) { memcpy(o->mem + off, data, n); return true; }
    return fseek64(o->f, (long long)off, SEEK_SET) == 0 && fwrite(data, 1, n, o->f) == n;
}

/* An empty FAT32 card. Memory is expected zeroed; a file gets its zeros. */
static bool format(Out *o, uint64_t size, const Geo *g)
{
    uint8_t s[SECTOR];

    /* MBR: one partition, FAT32 with LBA (0x0C) */
    memset(s, 0, SECTOR);
    uint8_t *pe = s + 0x1BE;
    pe[0] = 0x00;
    pe[1] = 0xFE; pe[2] = 0xFF; pe[3] = 0xFF;           /* CHS: past what CHS can say */
    pe[4] = 0x0C;
    pe[5] = 0xFE; pe[6] = 0xFF; pe[7] = 0xFF;
    put32(pe + 8, PART_LBA);
    put32(pe + 12, g->sectors);
    s[510] = 0x55; s[511] = 0xAA;
    if (!out_put(o, 0, s, SECTOR)) return false;

    /* the boot sector */
    memset(s, 0, SECTOR);
    memcpy(s, "\xEB\x58\x90" "MSWIN4.1", 11);
    put16(s + 11, SECTOR);
    s[13] = (uint8_t)g->spc;
    put16(s + 14, RSV);
    s[16] = 2;                                          /* FATs */
    s[21] = 0xF8;                                       /* fixed disk */
    put16(s + 24, 63); put16(s + 26, 255);
    put32(s + 28, PART_LBA);
    put32(s + 32, g->sectors);
    put32(s + 36, g->fatsz);
    put32(s + 44, 2);                                   /* root directory cluster */
    put16(s + 48, 1);                                   /* FSInfo */
    put16(s + 50, 6);                                   /* backup boot sector */
    s[64] = 0x80;
    s[66] = 0x29;
    put32(s + 67, 0x414B4131u);                         /* volume id */
    memcpy(s + 71, "AKA SD     FAT32   ", 19);
    s[510] = 0x55; s[511] = 0xAA;
    if (!out_put(o, PART_LBA * (uint64_t)SECTOR, s, SECTOR) ||
        !out_put(o, (PART_LBA + 6) * (uint64_t)SECTOR, s, SECTOR)) return false;

    /* FSInfo: free count and next free unknown (0xFFFFFFFF), which is always right */
    memset(s, 0, SECTOR);
    put32(s, 0x41615252u);
    put32(s + 484, 0x61417272u);
    put32(s + 488, 0xFFFFFFFFu);
    put32(s + 492, 0xFFFFFFFFu);
    put32(s + 508, 0xAA550000u);
    if (!out_put(o, (PART_LBA + 1) * (uint64_t)SECTOR, s, SECTOR) ||
        !out_put(o, (PART_LBA + 7) * (uint64_t)SECTOR, s, SECTOR)) return false;

    /* the FATs: media and end marks, the root directory's one cluster */
    for (int n = 0; n < 2; n++) {
        const uint64_t at = (g->fat_lba + (uint64_t)n * g->fatsz) * SECTOR;
        if (o->f) {
            static const uint8_t zero[SECTOR * 16];
            for (uint32_t i = 0; i < g->fatsz; i += 16) {
                const uint32_t k = g->fatsz - i < 16 ? g->fatsz - i : 16;
                if (!out_put(o, at + (uint64_t)i * SECTOR, zero, k * SECTOR)) return false;
            }
        }
        memset(s, 0, SECTOR);
        put32(s, 0x0FFFFFF8u);
        put32(s + 4, EOC);
        put32(s + 8, EOC);
        if (!out_put(o, at, s, SECTOR)) return false;
    }

    /* the root directory: the volume label, then nothing */
    memset(s, 0, SECTOR);
    memcpy(s, "AKA SD     ", 11);
    s[11] = 0x08;
    put16(s + 24, FAT_DATE);
    if (!out_put(o, clus_off(g, 2), s, SECTOR)) return false;
    if (o->f) {
        memset(s, 0, SECTOR);
        for (uint32_t i = 1; i < g->spc; i++)
            if (!out_put(o, clus_off(g, 2) + (uint64_t)i * SECTOR, s, SECTOR)) return false;
        /* the file reaches the card's size */
        if (!out_put(o, size - SECTOR, s, SECTOR)) return false;
    }
    return true;
}

/* ------------------------------------------------------------------------ */
/* Building a card in memory from a folder */

typedef struct {
    uint8_t *mem;
    Geo      g;
    uint32_t next;          /* the next free cluster */
    char   **files;
    int      nfiles, cap;
    bool     full;
} Build;

static uint32_t *fat(Build *b) { return (uint32_t *)(b->mem + b->g.fat_lba * SECTOR); }

/* n clusters in a row, chained; 0 for none */
static uint32_t alloc_chain(Build *b, uint32_t n)
{
    if (!n) return 0;
    if (b->next + n > b->g.nclus + 2) { b->full = true; return 0; }
    const uint32_t first = b->next;
    uint32_t *t = fat(b);
    for (uint32_t i = 0; i < n; i++) t[first + i] = i + 1 < n ? first + i + 1 : EOC;
    b->next += n;
    return first;
}

typedef struct {
    char     name[256];
    bool     dir;
    uint64_t size;
} Entry;

static int by_name(const void *a, const void *b)
{
    return strcmp(((const Entry *)a)->name, ((const Entry *)b)->name);
}

static Entry *list_dir(const char *path, int *count)
{
    *count = 0;
    DIR *d = opendir(path);
    if (!d) return NULL;
    int cap = 16;
    Entry *e = malloc(sizeof(Entry) * (size_t)cap);
    struct dirent *de;
    while (e && (de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..") || strlen(de->d_name) > 255) continue;
        char full[2048];
        snprintf(full, sizeof full, "%s/%s", path, de->d_name);
        struct stat st;
        if (stat(full, &st)) continue;
        if (*count == cap) { cap *= 2; e = realloc(e, sizeof(Entry) * (size_t)cap); if (!e) break; }
        Entry *x = &e[(*count)++];
        snprintf(x->name, sizeof x->name, "%s", de->d_name);
        x->dir = S_ISDIR(st.st_mode);
        x->size = x->dir ? 0 : (uint64_t)st.st_size;
    }
    closedir(d);
    if (e) qsort(e, (size_t)*count, sizeof(Entry), by_name);
    return e;
}

static bool sfn_char(int ch)
{
    return isalnum(ch) || strchr("$%'-_@~`!(){}^#&", ch) != NULL;
}

/* The 8.3 name of 'name', or its ~N alias. Returns the case flags (NT byte)
   when the name itself fits 8.3, -1 when it needs long name entries. */
static int short_name(const char *name, uint8_t out[11], uint8_t (*taken)[11], int ntaken)
{
    const char *dot = strrchr(name, '.');
    const size_t blen = dot && dot != name ? (size_t)(dot - name) : strlen(name);
    const char *ext = dot && dot != name ? dot + 1 : "";
    const size_t elen = strlen(ext);
    bool fits = blen >= 1 && blen <= 8 && elen <= 3 && (!dot || dot == strchr(name, '.'));
    int lower_b = 0, upper_b = 0, lower_e = 0, upper_e = 0;
    for (size_t i = 0; fits && i < blen; i++) {
        if (!sfn_char((unsigned char)name[i])) fits = false;
        lower_b |= islower((unsigned char)name[i]) != 0; upper_b |= isupper((unsigned char)name[i]) != 0;
    }
    for (size_t i = 0; fits && i < elen; i++) {
        if (!sfn_char((unsigned char)ext[i])) fits = false;
        lower_e |= islower((unsigned char)ext[i]) != 0; upper_e |= isupper((unsigned char)ext[i]) != 0;
    }
    if (fits && !(lower_b && upper_b) && !(lower_e && upper_e)) {
        memset(out, ' ', 11);
        for (size_t i = 0; i < blen; i++) out[i] = (uint8_t)toupper((unsigned char)name[i]);
        for (size_t i = 0; i < elen; i++) out[8 + i] = (uint8_t)toupper((unsigned char)ext[i]);
        return (lower_b ? 0x08 : 0) | (lower_e ? 0x10 : 0);
    }
    /* an alias: the first valid characters, ~N, the extension's first three */
    uint8_t base[8];
    int nb = 0;
    for (size_t i = 0; i < blen && nb < 6; i++)
        if (sfn_char((unsigned char)name[i])) base[nb++] = (uint8_t)toupper((unsigned char)name[i]);
    if (!nb) base[nb++] = '_';
    for (int n = 1; n < 1000000; n++) {
        char tail[12];
        const int tl = snprintf(tail, sizeof tail, "~%d", n);
        const int keep = nb + tl > 8 ? 8 - tl : nb;
        memset(out, ' ', 11);
        memcpy(out, base, (size_t)keep);
        memcpy(out + keep, tail, (size_t)tl);
        for (size_t i = 0, k = 0; i < elen && k < 3; i++)
            if (sfn_char((unsigned char)ext[i])) out[8 + k++] = (uint8_t)toupper((unsigned char)ext[i]);
        bool clash = false;
        for (int t = 0; t < ntaken && !clash; t++) clash = !memcmp(taken[t], out, 11);
        if (!clash) break;
    }
    return -1;
}

static uint8_t lfn_sum(const uint8_t sfn[11])
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + sfn[i]);
    return sum;
}

static void dirent_put(uint8_t *e, const uint8_t sfn[11], uint8_t attr, uint8_t nt, uint32_t clus, uint32_t size)
{
    memset(e, 0, 32);
    memcpy(e, sfn, 11);
    e[11] = attr;
    e[12] = nt;
    put16(e + 16, FAT_DATE);
    put16(e + 18, FAT_DATE);
    put16(e + 20, (uint16_t)(clus >> 16));
    put16(e + 24, FAT_DATE);
    put16(e + 26, (uint16_t)clus);
    put32(e + 28, size);
}

static void add_file(Build *b, const char *rel)
{
    if (b->nfiles == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 64;
        b->files = realloc(b->files, sizeof(char *) * (size_t)b->cap);
    }
    b->files[b->nfiles++] = strdup(rel);
}

/* Writes host folder 'path' into the card as the directory at cluster 'self'
   (allocated here, except the root's, which is cluster 2 already) */
static uint32_t import_dir(Build *b, const char *path, const char *rel, uint32_t parent, bool root)
{
    int n;
    Entry *list = list_dir(path, &n);
    if (!list && n) return 0;
    const uint32_t cbytes = b->g.spc * SECTOR;

    /* how big the directory is: . and .., each entry with its long name */
    uint32_t slots = root ? 1 : 2;
    for (int i = 0; i < n; i++) slots += 1 + (uint32_t)(strlen(list[i].name) + 12) / 13;
    const uint32_t nclus = (slots * 32 + cbytes) / cbytes;      /* room for the end mark */
    uint32_t self;
    if (root) {
        /* the root's chain starts at 2, before anything else is allocated */
        b->next = 2;
        self = alloc_chain(b, nclus);
    } else {
        self = alloc_chain(b, nclus);
    }
    if (!self) { free(list); return 0; }
    uint8_t *dir = b->mem + clus_off(&b->g, self);
    memset(dir, 0, (size_t)nclus * cbytes);
    uint32_t at = 0;
    if (root) {
        memcpy(dir, "AKA SD     ", 11);
        dir[11] = 0x08;
        at = 1;
    } else {
        uint8_t dot[11];
        memset(dot, ' ', 11); dot[0] = '.';
        dirent_put(dir, dot, 0x10, 0, self, 0);
        dot[1] = '.';
        dirent_put(dir + 32, dot, 0x10, 0, parent == 2 ? 0 : parent, 0);
        at = 2;
    }

    uint8_t (*taken)[11] = malloc(11 * (size_t)(n + 1));
    for (int i = 0; i < n && !b->full; i++) {
        Entry *x = &list[i];
        char hpath[2048], rpath[2048];
        snprintf(hpath, sizeof hpath, "%s/%s", path, x->name);
        snprintf(rpath, sizeof rpath, "%s%s%s", rel, *rel ? "/" : "", x->name);

        uint8_t sfn[11];
        const int nt = short_name(x->name, sfn, taken, i);
        memcpy(taken[i], sfn, 11);

        uint32_t clus = 0;
        if (x->dir) {
            clus = import_dir(b, hpath, rpath, self, false);
            if (!clus) continue;
        } else {
            if (x->size > 0xFFFFFFFFu) continue;
            clus = alloc_chain(b, (uint32_t)((x->size + cbytes - 1) / cbytes));
            if (x->size && !clus) continue;
            if (x->size) {
                FILE *f = fopen(hpath, "rb");
                if (!f) continue;
                const size_t got = fread(b->mem + clus_off(&b->g, clus), 1, (size_t)x->size, f);
                fclose(f);
                (void)got;
            }
            add_file(b, rpath);
        }
        if (nt < 0) {
            /* long name entries, last part first */
            const size_t len = strlen(x->name);
            const int parts = (int)(len + 12) / 13;
            const uint8_t sum = lfn_sum(sfn);
            for (int p = parts; p >= 1; p--) {
                uint8_t *e = dir + (size_t)at++ * 32;
                memset(e, 0, 32);
                e[0] = (uint8_t)(p | (p == parts ? 0x40 : 0));
                e[11] = 0x0F;
                e[13] = sum;
                static const int pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
                for (int k = 0; k < 13; k++) {
                    const size_t ci = (size_t)(p - 1) * 13 + (size_t)k;
                    const uint16_t ch = ci < len ? (uint8_t)x->name[ci] : ci == len ? 0 : 0xFFFF;
                    put16(e + pos[k], ch);
                }
            }
        }
        dirent_put(dir + (size_t)at++ * 32, sfn, x->dir ? 0x10 : 0x20, nt < 0 ? 0 : (uint8_t)nt, clus,
                   (uint32_t)x->size);
    }
    free(taken);
    free(list);
    return self;
}

static uint8_t *build_from_folder(const char *folder, uint64_t size, char ***files, int *nfiles, char *err,
                                  size_t errlen)
{
    Build b = { 0 };
    if (!geometry(size, &b.g)) { snprintf(err, errlen, "card size out of range"); return NULL; }
    b.mem = calloc(1, (size_t)size);
    if (!b.mem) { snprintf(err, errlen, "out of memory for a %u MB card", (unsigned)(size >> 20)); return NULL; }
    Out o = { b.mem, NULL };
    format(&o, size, &b.g);
    if (folder) {
        if (!import_dir(&b, folder, "", 0, true) || b.full) {
            snprintf(err, errlen, b.full ? "%s does not fit on the card" : "cannot read %s", folder);
            for (int i = 0; i < b.nfiles; i++) free(b.files[i]);
            free(b.files);
            free(b.mem);
            return NULL;
        }
        memcpy(b.mem + (b.g.fat_lba + b.g.fatsz) * SECTOR, b.mem + b.g.fat_lba * SECTOR,
               (size_t)b.g.fatsz * SECTOR);
    }
    if (files) { *files = b.files; *nfiles = b.nfiles; }
    else {
        for (int i = 0; i < b.nfiles; i++) free(b.files[i]);
        free(b.files);
    }
    return b.mem;
}

/* The size a folder's card is made at: its files with room to spare, at least 128 MB */
static uint64_t folder_card_size(const char *path)
{
    uint64_t used = 0;
    int n;
    Entry *list = list_dir(path, &n);
    for (int i = 0; list && i < n; i++) {
        if (list[i].dir) {
            char sub[2048];
            /* a path too long for the buffer is left out rather than cut short */
            if (snprintf(sub, sizeof sub, "%s/%s", path, list[i].name) >= (int)sizeof sub) continue;
            used += folder_card_size(sub) + 4096;
        } else {
            used += (list[i].size + 4095) & ~4095ull;
        }
    }
    free(list);
    return used;
}

static uint64_t card_size_for(const char *folder)
{
    const uint64_t used = folder ? folder_card_size(folder) : 0;
    uint64_t size = used + used / 4 + (32u << 20);
    if (size < (128u << 20)) size = 128u << 20;
    return (size + (1u << 20) - 1) & ~(uint64_t)((1u << 20) - 1);
}

/* ------------------------------------------------------------------------ */
/* Writing a card's files back into a folder */

typedef struct {
    const uint8_t *mem;
    Geo            g;
    char         **seen;
    int            nseen, cap;
} Walk;

static uint32_t next_clus(const Walk *w, uint32_t c)
{
    const uint32_t v = get32(w->mem + w->g.fat_lba * SECTOR + (uint64_t)c * 4) & 0x0FFFFFFFu;
    return v >= 2 && v < w->g.nclus + 2 ? v : 0;
}

static void seen_add(Walk *w, const char *rel)
{
    if (w->nseen == w->cap) {
        w->cap = w->cap ? w->cap * 2 : 64;
        w->seen = realloc(w->seen, sizeof(char *) * (size_t)w->cap);
    }
    w->seen[w->nseen++] = strdup(rel);
}

static bool write_if_changed(const char *path, const uint8_t *data, size_t n)
{
    FILE *f = fopen(path, "rb");
    if (f) {
        bool same = true;
        uint8_t buf[4096];
        size_t at = 0, got;
        while (same && (got = fread(buf, 1, sizeof buf, f)) > 0) {
            if (at + got > n || memcmp(buf, data + at, got)) same = false;
            at += got;
        }
        fclose(f);
        if (same && at == n) return true;
    }
    f = fopen(path, "wb");
    if (!f) return false;
    const bool ok = fwrite(data, 1, n, f) == n;
    fclose(f);
    return ok;
}

static void export_dir(Walk *w, uint32_t clus, const char *host, const char *rel, int depth)
{
    if (depth > 32) return;
    const uint32_t cbytes = w->g.spc * SECTOR;
    uint16_t lfn[256];
    int lfn_parts = 0;
    uint8_t lfn_sumv = 0;
    for (uint32_t c = clus, guard = 0; c && guard < w->g.nclus; c = next_clus(w, c), guard++) {
        const uint8_t *d = w->mem + clus_off(&w->g, c);
        for (uint32_t i = 0; i < cbytes; i += 32) {
            const uint8_t *e = d + i;
            if (e[0] == 0) return;
            if (e[0] == 0xE5) { lfn_parts = 0; continue; }
            if (e[11] == 0x0F) {
                const int ord = e[0] & 0x3F;
                if (ord < 1 || ord > 20) { lfn_parts = 0; continue; }
                if (e[0] & 0x40) { lfn_parts = ord; lfn_sumv = e[13]; memset(lfn, 0, sizeof lfn); }
                static const int pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
                for (int k = 0; k < 13; k++) {
                    const int ci = (ord - 1) * 13 + k;
                    if (ci < 255) lfn[ci] = get16(e + pos[k]);
                }
                continue;
            }
            if (e[11] & 0x08) { lfn_parts = 0; continue; }         /* the volume label */
            char name[256];
            if (lfn_parts && lfn_sumv == lfn_sum(e)) {
                int k = 0;
                for (; k < 255 && lfn[k] && lfn[k] != 0xFFFF; k++)
                    name[k] = lfn[k] < 0x100 ? (char)lfn[k] : '_';
                name[k] = 0;
            } else {
                int k = 0;
                for (int j = 0; j < 8 && e[j] != ' '; j++)
                    name[k++] = (char)((e[12] & 0x08) ? tolower(e[j]) : e[j]);
                if (e[8] != ' ') {
                    name[k++] = '.';
                    for (int j = 8; j < 11 && e[j] != ' '; j++)
                        name[k++] = (char)((e[12] & 0x10) ? tolower(e[j]) : e[j]);
                }
                name[k] = 0;
            }
            lfn_parts = 0;
            if (!strcmp(name, ".") || !strcmp(name, "..") || !name[0]) continue;
            if (strchr(name, '/') || strchr(name, '\\')) continue;

            char hpath[2048], rpath[2048];
            snprintf(hpath, sizeof hpath, "%s/%s", host, name);
            snprintf(rpath, sizeof rpath, "%s%s%s", rel, *rel ? "/" : "", name);
            const uint32_t first = (uint32_t)get16(e + 20) << 16 | get16(e + 26);
            if (e[11] & 0x10) {
                make_dir(hpath);
                if (first >= 2) export_dir(w, first, hpath, rpath, depth + 1);
                continue;
            }
            const uint32_t size = get32(e + 28);
            uint8_t *data = malloc(size ? size : 1);
            if (!data) continue;
            uint32_t at = 0;
            for (uint32_t fc = first, g2 = 0; fc >= 2 && at < size && g2 < w->g.nclus; fc = next_clus(w, fc), g2++) {
                const uint32_t k = size - at < cbytes ? size - at : cbytes;
                memcpy(data + at, w->mem + clus_off(&w->g, fc), k);
                at += k;
            }
            if (at == size) {
                write_if_changed(hpath, data, size);
                seen_add(w, rpath);
            }
            free(data);
        }
    }
}

static bool sync_folder(SdCard *c)
{
    Walk w = { c->mem, { 0 }, NULL, 0, 0 };
    /* the partition the card was made with */
    const uint8_t *bs = c->mem + PART_LBA * SECTOR;
    if (get16(bs + 510) != 0xAA55 || get16(bs + 11) != SECTOR) return false;
    w.g.spc = bs[13];
    w.g.fatsz = get32(bs + 36);
    w.g.fat_lba = PART_LBA + get16(bs + 14);
    w.g.data_lba = w.g.fat_lba + (uint64_t)bs[16] * w.g.fatsz;
    w.g.sectors = get32(bs + 32);
    if (!w.g.spc || w.g.data_lba >= PART_LBA + (uint64_t)w.g.sectors) return false;
    w.g.nclus = (uint32_t)((PART_LBA + (uint64_t)w.g.sectors - w.g.data_lba) / w.g.spc);
    export_dir(&w, get32(bs + 44), c->folder, "", 0);

    /* files the program deleted go from the folder too */
    for (int i = 0; i < c->nfiles; i++) {
        bool still = false;
        for (int j = 0; j < w.nseen && !still; j++) still = !strcmp(c->files[i], w.seen[j]);
        if (!still) {
            char hpath[2048];
            snprintf(hpath, sizeof hpath, "%s/%s", c->folder, c->files[i]);
            remove(hpath);
        }
        free(c->files[i]);
    }
    free(c->files);
    c->files = w.seen;
    c->nfiles = w.nseen;
    return true;
}

/* ------------------------------------------------------------------------ */

static bool is_dir(const char *path)
{
    struct stat st;
    return !stat(path, &st) && S_ISDIR(st.st_mode);
}

SdCard *sdcard_open(const char *path, uint32_t size_mb, char *err, size_t errlen)
{
    SdCard *c = calloc(1, sizeof *c);
    if (!c) { snprintf(err, errlen, "out of memory"); return NULL; }
    char folder[1024];
    snprintf(folder, sizeof folder, "%s", path);
    size_t l = strlen(folder);
    while (l > 1 && (folder[l - 1] == '/' || folder[l - 1] == '\\')) folder[--l] = 0;

    if (is_dir(folder)) {
        snprintf(c->folder, sizeof c->folder, "%s", folder);
        c->size = size_mb ? (uint64_t)size_mb << 20 : card_size_for(folder);
        c->mem = build_from_folder(folder, c->size, &c->files, &c->nfiles, err, errlen);
        if (!c->mem) { free(c); return NULL; }
        return c;
    }

    c->f = fopen(path, "r+b");
    if (!c->f) {
        /* no such image yet: an empty card */
        if (!sdcard_make_image(path, NULL, size_mb, err, errlen)) { free(c); return NULL; }
        c->f = fopen(path, "r+b");
    }
    if (!c->f) { snprintf(err, errlen, "cannot open %s", path); free(c); return NULL; }
    fseek64(c->f, 0, SEEK_END);
    c->size = (uint64_t)ftell64(c->f) & ~(uint64_t)(SECTOR - 1);
    if (c->size < SECTOR) { snprintf(err, errlen, "%s is empty", path); fclose(c->f); free(c); return NULL; }
    return c;
}

void sdcard_close(SdCard *c)
{
    if (!c) return;
    sdcard_sync(c);
    if (c->f) fclose(c->f);
    free(c->mem);
    for (int i = 0; i < c->nfiles; i++) free(c->files[i]);
    free(c->files);
    free(c);
}

uint64_t sdcard_sectors(const SdCard *c) { return c->size / SECTOR; }

bool sdcard_read(SdCard *c, uint64_t lba, uint8_t *buf, uint32_t count)
{
    const uint64_t off = lba * SECTOR, n = (uint64_t)count * SECTOR;
    if (off + n > c->size) return false;
    if (c->mem) { memcpy(buf, c->mem + off, (size_t)n); return true; }
    return fseek64(c->f, (long long)off, SEEK_SET) == 0 && fread(buf, 1, (size_t)n, c->f) == n;
}

bool sdcard_write(SdCard *c, uint64_t lba, const uint8_t *buf, uint32_t count)
{
    const uint64_t off = lba * SECTOR, n = (uint64_t)count * SECTOR;
    if (off + n > c->size) return false;
    c->dirty = true;
    if (c->mem) { memcpy(c->mem + off, buf, (size_t)n); return true; }
    return fseek64(c->f, (long long)off, SEEK_SET) == 0 && fwrite(buf, 1, (size_t)n, c->f) == n;
}

bool sdcard_sync(SdCard *c)
{
    if (!c || !c->dirty) return true;
    c->dirty = false;
    if (c->f) return fflush(c->f) == 0;
    return sync_folder(c);
}

bool sdcard_make_image(const char *out, const char *folder, uint32_t size_mb, char *err, size_t errlen)
{
    const uint64_t size = size_mb ? (uint64_t)size_mb << 20 : folder ? card_size_for(folder) : 256ull << 20;
    Geo g;
    if (!geometry(size, &g)) {
        snprintf(err, errlen, "a card must be 40 MB to 2 TB");
        return false;
    }
    if (folder) {
        /* built in memory, then written out */
        uint8_t *mem = build_from_folder(folder, size, NULL, NULL, err, errlen);
        if (!mem) return false;
        FILE *f = fopen(out, "wb");
        bool ok = f && fwrite(mem, 1, (size_t)size, f) == size;
        if (f && fclose(f)) ok = false;
        free(mem);
        if (!ok) snprintf(err, errlen, "cannot write %s", out);
        return ok;
    }
    FILE *f = fopen(out, "w+b");
    if (!f) { snprintf(err, errlen, "cannot create %s", out); return false; }
    Out o = { NULL, f };
    bool ok = format(&o, size, &g);
    if (fclose(f)) ok = false;
    if (!ok) snprintf(err, errlen, "cannot write %s", out);
    return ok;
}
