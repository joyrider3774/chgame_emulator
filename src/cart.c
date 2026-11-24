/*
 * Reading a .chgame cart: the ZIP's central directory (entries stored or
 * deflated, no ZIP64, as spec/chgame.md allows), info.json through a small
 * JSON reader, and the entries the manifest names.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cart.h"
#include "inflate.h"

#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define make_dir(p) mkdir((p), 0777)
#endif

/* ------------------------------------------------------------------------ */
/* JSON: enough for a manifest                                              */
/* ------------------------------------------------------------------------ */

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct Json {
    JType t;
    char *key;              /* in an object */
    char *s;                /* J_STR */
    double n;               /* J_NUM, J_BOOL */
    struct Json *child;     /* J_ARR, J_OBJ: the first item */
    struct Json *next;
} Json;

typedef struct { const char *p, *end; int err; } JParse;

static void json_free(Json *j)
{
    while (j) {
        Json *next = j->next;
        json_free(j->child);
        free(j->key);
        free(j->s);
        free(j);
        j = next;
    }
}

static void ws(JParse *q)
{
    while (q->p < q->end && (*q->p == ' ' || *q->p == '\t' || *q->p == '\n' || *q->p == '\r')) q->p++;
}

/* a string's text, escapes resolved; \u above 0x7F becomes '?', which is
   all an id, a title or a file name in a cart may hold anyway */
static char *jstring(JParse *q)
{
    if (q->p >= q->end || *q->p != '"') { q->err = 1; return NULL; }
    q->p++;
    size_t cap = 32, n = 0;
    char *s = malloc(cap);
    if (!s) { q->err = 1; return NULL; }
    while (q->p < q->end && *q->p != '"') {
        char c = *q->p++;
        if (c == '\\') {
            if (q->p >= q->end) break;
            const char e = *q->p++;
            switch (e) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u': {
                unsigned v = 0;
                for (int i = 0; i < 4 && q->p < q->end; i++, q->p++) {
                    const char h = *q->p;
                    v = v * 16 + (unsigned)(h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 :
                                             h >= 'A' && h <= 'F' ? h - 'A' + 10 : 0);
                }
                c = v < 0x80 ? (char)v : '?';
                break;
            }
            default: c = e; break;      /* \" \\ \/ */
            }
        }
        if (n + 2 > cap) {
            char *t = realloc(s, cap *= 2);
            if (!t) { free(s); q->err = 1; return NULL; }
            s = t;
        }
        s[n++] = c;
    }
    if (q->p >= q->end) { free(s); q->err = 1; return NULL; }
    q->p++;
    s[n] = 0;
    return s;
}

static Json *jvalue(JParse *q, int depth)
{
    ws(q);
    if (q->p >= q->end || depth > 32) { q->err = 1; return NULL; }
    Json *j = calloc(1, sizeof *j);
    if (!j) { q->err = 1; return NULL; }
    const char c = *q->p;
    if (c == '{' || c == '[') {
        j->t = c == '{' ? J_OBJ : J_ARR;
        q->p++;
        Json **tail = &j->child;
        ws(q);
        if (q->p < q->end && *q->p == (c == '{' ? '}' : ']')) { q->p++; return j; }
        for (;;) {
            char *key = NULL;
            if (j->t == J_OBJ) {
                ws(q);
                key = jstring(q);
                ws(q);
                if (q->err || q->p >= q->end || *q->p != ':') { free(key); q->err = 1; break; }
                q->p++;
            }
            Json *item = jvalue(q, depth + 1);
            if (!item) { free(key); break; }
            item->key = key;
            *tail = item;
            tail = &item->next;
            ws(q);
            if (q->p < q->end && *q->p == ',') { q->p++; continue; }
            if (q->p < q->end && *q->p == (j->t == J_OBJ ? '}' : ']')) { q->p++; break; }
            q->err = 1;
            break;
        }
    } else if (c == '"') {
        j->t = J_STR;
        j->s = jstring(q);
    } else if (!strncmp(q->p, "true", 4) && q->end - q->p >= 4) {
        j->t = J_BOOL; j->n = 1; q->p += 4;
    } else if (!strncmp(q->p, "false", 5) && q->end - q->p >= 5) {
        j->t = J_BOOL; q->p += 5;
    } else if (!strncmp(q->p, "null", 4) && q->end - q->p >= 4) {
        j->t = J_NULL; q->p += 4;
    } else {
        char *e;
        j->t = J_NUM;
        j->n = strtod(q->p, &e);
        if (e == q->p) q->err = 1;
        q->p = e;
    }
    if (q->err) { json_free(j); return NULL; }
    return j;
}

static const Json *jget(const Json *obj, const char *key)
{
    if (!obj || obj->t != J_OBJ) return NULL;
    for (const Json *j = obj->child; j; j = j->next)
        if (j->key && !strcmp(j->key, key)) return j;
    return NULL;
}

static const char *jstr(const Json *obj, const char *key)
{
    const Json *j = jget(obj, key);
    return j && j->t == J_STR ? j->s : NULL;
}

/* ------------------------------------------------------------------------ */
/* ZIP                                                                      */
/* ------------------------------------------------------------------------ */

typedef struct {
    char *name;
    uint16_t method;
    uint32_t crc, csize, usize, offset;
} Entry;

struct ChgCart {
    uint8_t *data;
    size_t size;
    Entry *entries;
    int nentries;
    Json *info;
    const Json **games;
    int ngames;
};

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | (uint32_t)rd16(p + 2) << 16; }

static uint32_t crc32(const uint8_t *d, size_t n)
{
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

bool chg_is_zip(const uint8_t *data, size_t size)
{
    return size >= 4 && rd32(data) == 0x04034b50u;
}

static const Entry *find_entry(const ChgCart *c, const char *name)
{
    for (int i = 0; i < c->nentries; i++)
        if (!strcmp(c->entries[i].name, name)) return &c->entries[i];
    return NULL;
}

/* an entry's contents, malloc'ed (one byte more, a NUL, for text) */
static uint8_t *read_entry(const ChgCart *c, const Entry *e, char *err, size_t errlen)
{
    const size_t o = e->offset;
    if (o + 30 > c->size || rd32(c->data + o) != 0x04034b50u) {
        snprintf(err, errlen, "the cart's %s is damaged", e->name);
        return NULL;
    }
    const size_t start = o + 30 + rd16(c->data + o + 26) + rd16(c->data + o + 28);
    if (start + e->csize > c->size) {
        snprintf(err, errlen, "the cart's %s is cut short", e->name);
        return NULL;
    }
    uint8_t *out = malloc((size_t)e->usize + 1);
    if (!out) { snprintf(err, errlen, "out of memory"); return NULL; }
    out[e->usize] = 0;
    int bad;
    if (e->method == 0) {
        bad = e->csize != e->usize;
        if (!bad) memcpy(out, c->data + start, e->usize);
    } else if (e->method == 8) {
        bad = chg_inflate(c->data + start, e->csize, out, e->usize) != 0;
    } else {
        snprintf(err, errlen, "the cart's %s is compressed a way a .chgame may not be (method %u)", e->name, e->method);
        free(out);
        return NULL;
    }
    if (bad || crc32(out, e->usize) != e->crc) {
        snprintf(err, errlen, "the cart's %s is damaged (CRC)", e->name);
        free(out);
        return NULL;
    }
    return out;
}

static bool read_directory(ChgCart *c, char *err, size_t errlen)
{
    /* the end of central directory record: 22 bytes and a comment of at most
       64 KB, so it is searched for from the end */
    const uint8_t *d = c->data;
    size_t eocd = 0;
    bool found = false;
    for (size_t i = c->size >= 22 ? c->size - 22 : 0; c->size >= 22; i--) {
        if (rd32(d + i) == 0x06054b50u) { eocd = i; found = true; break; }
        if (i == 0 || c->size - i > 22 + 65535) break;
    }
    if (!found) { snprintf(err, errlen, "not a ZIP file (no central directory)"); return false; }
    const unsigned count = rd16(d + eocd + 10);
    size_t p = rd32(d + eocd + 16);
    c->entries = calloc(count ? count : 1, sizeof(Entry));
    if (!c->entries) { snprintf(err, errlen, "out of memory"); return false; }
    for (unsigned i = 0; i < count; i++) {
        if (p + 46 > c->size || rd32(d + p) != 0x02014b50u) {
            snprintf(err, errlen, "the cart's ZIP directory is damaged");
            return false;
        }
        const unsigned nlen = rd16(d + p + 28), xlen = rd16(d + p + 30), clen = rd16(d + p + 32);
        if (p + 46 + nlen > c->size) { snprintf(err, errlen, "the cart's ZIP directory is damaged"); return false; }
        if (rd16(d + p + 8) & 1) { snprintf(err, errlen, "the cart is encrypted"); return false; }
        Entry *e = &c->entries[c->nentries++];
        e->method = rd16(d + p + 10);
        e->crc = rd32(d + p + 16);
        e->csize = rd32(d + p + 20);
        e->usize = rd32(d + p + 24);
        e->offset = rd32(d + p + 42);
        e->name = malloc(nlen + 1);
        if (!e->name) { snprintf(err, errlen, "out of memory"); return false; }
        memcpy(e->name, d + p + 46, nlen);
        e->name[nlen] = 0;
        p += 46 + nlen + xlen + clen;
    }
    return true;
}

ChgCart *chg_cart_open(const uint8_t *data, size_t size, char *err, size_t errlen)
{
    ChgCart *c = calloc(1, sizeof *c);
    if (!c || !(c->data = malloc(size ? size : 1))) {
        free(c);
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    memcpy(c->data, data, size);
    c->size = size;
    if (!read_directory(c, err, errlen)) { chg_cart_free(c); return NULL; }

    const Entry *e = find_entry(c, "info.json");
    if (!e) { snprintf(err, errlen, "not a .chgame cart (no info.json)"); chg_cart_free(c); return NULL; }
    char *text = (char *)read_entry(c, e, err, errlen);
    if (!text) { chg_cart_free(c); return NULL; }
    JParse q = { text, text + e->usize, 0 };
    c->info = jvalue(&q, 0);
    free(text);
    if (!c->info || c->info->t != J_OBJ) {
        snprintf(err, errlen, "the cart's info.json is not valid JSON");
        chg_cart_free(c);
        return NULL;
    }
    const Json *v = jget(c->info, "schemaVersion");
    if (!v || v->t != J_NUM || v->n != 1) {
        snprintf(err, errlen, "the cart's format (schemaVersion) is not version 1");
        chg_cart_free(c);
        return NULL;
    }
    const Json *games = jget(c->info, "games");
    if (!games || games->t != J_ARR || !games->child) {
        snprintf(err, errlen, "the cart has no games");
        chg_cart_free(c);
        return NULL;
    }
    for (const Json *g = games->child; g; g = g->next) c->ngames++;
    c->games = calloc((size_t)c->ngames, sizeof *c->games);
    if (!c->games) { snprintf(err, errlen, "out of memory"); chg_cart_free(c); return NULL; }
    int i = 0;
    for (const Json *g = games->child; g; g = g->next) {
        if (g->t != J_OBJ || !jstr(g, "id")) {
            snprintf(err, errlen, "a game in the cart has no id");
            chg_cart_free(c);
            return NULL;
        }
        c->games[i++] = g;
    }
    return c;
}

void chg_cart_free(ChgCart *c)
{
    if (!c) return;
    for (int i = 0; i < c->nentries; i++) free(c->entries[i].name);
    free(c->entries);
    free(c->games);
    json_free(c->info);
    free(c->data);
    free(c);
}

int chg_cart_games(const ChgCart *c) { return c->ngames; }

int chg_cart_find(const ChgCart *c, const char *id)
{
    if (!id || !*id) return c->ngames ? 0 : -1;
    for (int i = 0; i < c->ngames; i++)
        if (!strcmp(jstr(c->games[i], "id"), id)) return i;
    return -1;
}

const char *chg_cart_id(const ChgCart *c, int game) { return jstr(c->games[game], "id"); }

const char *chg_cart_title(const ChgCart *c, int game)
{
    const char *t = jstr(c->games[game], "title");
    return t ? t : chg_cart_id(c, game);
}

/* the game's binaries entry for a device */
static const char *binary_file(const ChgCart *c, int game, const char *device)
{
    const Json *b = jget(c->games[game], "binaries");
    if (!b || b->t != J_ARR) return NULL;
    for (const Json *j = b->child; j; j = j->next) {
        const char *d = jstr(j, "device"), *f = jstr(j, "filename");
        if (d && f && !strcmp(d, device)) return f;
    }
    return NULL;
}

void chg_cart_devices(const ChgCart *c, int game, char *out, size_t outlen)
{
    size_t n = 0;
    out[0] = 0;
    const Json *b = jget(c->games[game], "binaries");
    if (!b || b->t != J_ARR) return;
    for (const Json *j = b->child; j && n < outlen; j = j->next) {
        const char *d = jstr(j, "device");
        if (d) n += (size_t)snprintf(out + n, outlen - n, "%s%s", n ? " " : "", d);
    }
}

bool chg_cart_has(const ChgCart *c, int game, const char *device)
{
    const char *f = binary_file(c, game, device);
    return f && find_entry(c, f);
}

uint8_t *chg_cart_binary(const ChgCart *c, int game, const char *device, size_t *len,
                         char *err, size_t errlen)
{
    const char *f = binary_file(c, game, device);
    const Entry *e = f ? find_entry(c, f) : NULL;
    if (!e) {
        char devs[128];
        chg_cart_devices(c, game, devs, sizeof devs);
        snprintf(err, errlen, "%s has no binary for %s (it has: %s)", chg_cart_title(c, game), device,
                 devs[0] ? devs : "none");
        return NULL;
    }
    uint8_t *data = read_entry(c, e, err, errlen);
    if (data) *len = e->usize;
    return data;
}

/* a path inside the card that stays inside it: relative, no "..", no
   drive or backslash (spec/chgame.md's path rules) */
static bool safe_path(const char *p)
{
    if (!*p || *p == '/' || strchr(p, '\\') || strchr(p, ':')) return false;
    for (const char *s = p; *s; ) {
        const char *e = strchr(s, '/');
        const size_t n = e ? (size_t)(e - s) : strlen(s);
        if (n == 0 || (n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.')) return false;
        s += n;
        if (*s == '/') s++;
    }
    return true;
}

int chg_cart_write_sdcard(const ChgCart *c, int game, const char *folder, char *err, size_t errlen)
{
    const char *prefix = jstr(c->games[game], "sdcard");
    if (!prefix || !*prefix) return 0;
    const size_t plen = strlen(prefix);
    int written = 0;
    make_dir(folder);
    for (int i = 0; i < c->nentries; i++) {
        const Entry *e = &c->entries[i];
        const size_t nlen = strlen(e->name);
        if (nlen <= plen || strncmp(e->name, prefix, plen) || e->name[nlen - 1] == '/') continue;
        const char *rel = e->name + plen;
        if (!safe_path(rel)) continue;
        uint8_t *data = read_entry(c, e, err, errlen);
        if (!data) return -1;
        char path[2048];
        snprintf(path, sizeof path, "%s/%s", folder, rel);
        /* the folders on the way */
        for (char *s = path + strlen(folder) + 1; (s = strchr(s, '/')); s++) {
            *s = 0;
            make_dir(path);
            *s = '/';
        }
        /* written only when it differs: a card file the game changed since is
           replaced by the cart's, as a card prepared from the cart would have it */
        bool same = false;
        FILE *f = fopen(path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            if ((long)e->usize == ftell(f)) {
                fseek(f, 0, SEEK_SET);
                uint8_t *old = malloc((size_t)e->usize + 1);
                same = old && fread(old, 1, e->usize, f) == e->usize && !memcmp(old, data, e->usize);
                free(old);
            }
            fclose(f);
        }
        if (!same) {
            f = fopen(path, "wb");
            if (!f || fwrite(data, 1, e->usize, f) != e->usize) {
                if (f) fclose(f);
                free(data);
                snprintf(err, errlen, "cannot write %s", path);
                return -1;
            }
            fclose(f);
            written++;
        }
        free(data);
    }
    return written;
}
