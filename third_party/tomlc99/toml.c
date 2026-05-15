/*
 * MIT License
 * Minimal TOML 1.0 parser — key subset for emd configuration.
 *
 * Supported:
 *   - Bare keys and quoted keys
 *   - String values (double-quoted)
 *   - Integer values (decimal, with _ separators)
 *   - Float values
 *   - Boolean values (true/false)
 *   - Tables: [section] and [section.subsection]
 *   - Comments (#)
 */

#include "toml.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <stdint.h>

/* --------------------------------------------------------------------- */
/* Internal structures                                                     */
/* --------------------------------------------------------------------- */

#define TOML_MAX_KEYS  256
#define TOML_MAX_TABS  64

typedef struct toml_kv {
    char  *key;
    char  *raw;   /* raw value string (points into conf copy) */
} toml_kv_t;

struct toml_table_t {
    char       *name;     /* dotted path, or empty for root */
    toml_kv_t   kv[TOML_MAX_KEYS];
    int         nkv;
    struct toml_table_t *subtabs[TOML_MAX_TABS];
    int         nsub;
    char       *_heap_buf; /* non-NULL only on root: heap buffer to free */
};

/* --------------------------------------------------------------------- */
/* Allocation helpers                                                      */
/* --------------------------------------------------------------------- */

static toml_table_t *tab_new(const char *name) {
    toml_table_t *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->name = name ? strdup(name) : strdup("");
    return t;
}

static void tab_free_recursive(toml_table_t *t) {
    if (!t) return;
    free(t->name);
    free(t->_heap_buf); /* non-NULL only on root when parsed from file */
    for (int i = 0; i < t->nkv; i++) {
        free(t->kv[i].key);
        free(t->kv[i].raw); /* raw is now strdup'd */
    }
    for (int i = 0; i < t->nsub; i++) {
        tab_free_recursive(t->subtabs[i]);
    }
    free(t);
}

void toml_free(toml_table_t *tab) {
    tab_free_recursive(tab);
}

/* --------------------------------------------------------------------- */
/* Lookup                                                                  */
/* --------------------------------------------------------------------- */

toml_table_t *toml_table_in(const toml_table_t *tab, const char *key) {
    if (!tab || !key) return NULL;
    for (int i = 0; i < tab->nsub; i++) {
        if (strcmp(tab->subtabs[i]->name, key) == 0)
            return tab->subtabs[i];
    }
    return NULL;
}

const char *toml_raw_in(const toml_table_t *tab, const char *key) {
    if (!tab || !key) return NULL;
    for (int i = 0; i < tab->nkv; i++) {
        if (strcmp(tab->kv[i].key, key) == 0)
            return tab->kv[i].raw;
    }
    return NULL;
}

const char *toml_key_in(const toml_table_t *tab, int idx) {
    if (!tab || idx < 0 || idx >= tab->nsub) return NULL;
    return tab->subtabs[idx]->name;
}

int toml_table_nkval(const toml_table_t *tab) {
    return tab ? tab->nkv : 0;
}

int toml_table_ntab(const toml_table_t *tab) {
    return tab ? tab->nsub : 0;
}

/* --------------------------------------------------------------------- */
/* Conversion helpers                                                      */
/* --------------------------------------------------------------------- */

int toml_rtos(const char *raw, char **ret) {
    if (!raw || !ret) return -1;
    /* raw should be "string" with surrounding quotes */
    const char *p = raw;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return -1;
    p++;
    size_t len = 0;
    const char *start = p;
    while (*p && *p != '"') {
        if (*p == '\\') p++; /* skip escape prefix */
        if (*p) p++;
        len++;
    }
    if (*p != '"') return -1;

    char *out = malloc(len + 1);
    if (!out) return -1;
    /* Simple copy with basic escape handling */
    size_t di = 0;
    p = start;
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            switch (*p) {
                case 'n':  out[di++] = '\n'; break;
                case 't':  out[di++] = '\t'; break;
                case 'r':  out[di++] = '\r'; break;
                case '"':  out[di++] = '"';  break;
                case '\\': out[di++] = '\\'; break;
                default:   out[di++] = *p;   break;
            }
        } else {
            out[di++] = *p;
        }
        p++;
    }
    out[di] = '\0';
    *ret = out;
    return 0;
}

int toml_rtoi(const char *raw, int64_t *ret) {
    if (!raw || !ret) return -1;
    const char *p = raw;
    while (*p == ' ' || *p == '\t') p++;

    /* Handle sign */
    int sign = 1;
    if (*p == '+') { p++; }
    else if (*p == '-') { sign = -1; p++; }

    if (!isdigit((unsigned char)*p)) return -1;

    /* Collect digits, skip underscores */
    char buf[32];
    size_t bi = 0;
    while (*p && bi < sizeof(buf) - 1) {
        if (isdigit((unsigned char)*p)) {
            buf[bi++] = *p++;
        } else if (*p == '_') {
            p++;
        } else {
            break;
        }
    }
    buf[bi] = '\0';

    char *end;
    errno = 0;
    long long v = strtoll(buf, &end, 10);
    if (end == buf || errno) return -1;
    *ret = (int64_t)(sign * v);
    return 0;
}

int toml_rtod(const char *raw, double *ret) {
    if (!raw || !ret) return -1;
    const char *p = raw;
    while (*p == ' ' || *p == '\t') p++;
    char *end;
    errno = 0;
    double v = strtod(p, &end);
    if (end == p || errno) return -1;
    *ret = v;
    return 0;
}

int toml_rtob(const char *raw, int *ret) {
    if (!raw || !ret) return -1;
    const char *p = raw;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "true", 4) == 0)  { *ret = 1; return 0; }
    if (strncmp(p, "false", 5) == 0) { *ret = 0; return 0; }
    return -1;
}

/* --------------------------------------------------------------------- */
/* Parser                                                                  */
/* --------------------------------------------------------------------- */

typedef struct {
    char       *src;
    size_t      pos;
    size_t      len;
    int         line;
    char       *errbuf;
    int         errbufsz;
} parser_t;

static inline int peof(const parser_t *p) {
    return p->pos >= p->len;
}

static inline char ppeek(const parser_t *p) {
    return peof(p) ? '\0' : p->src[p->pos];
}

static inline char padv(parser_t *p) {
    char c = p->src[p->pos++];
    if (c == '\n') p->line++;
    return c;
}

static void skip_whitespace(parser_t *p) {
    while (!peof(p) && (ppeek(p) == ' ' || ppeek(p) == '\t'))
        padv(p);
}

static void skip_to_eol(parser_t *p) {
    while (!peof(p) && ppeek(p) != '\n')
        padv(p);
}

static void skip_comment_and_ws(parser_t *p) {
    skip_whitespace(p);
    if (!peof(p) && ppeek(p) == '#')
        skip_to_eol(p);
}

/* Static key buffer — parse_key copies the key here so the source is not corrupted */
static char s_key_buf[256];

/* Parse a bare or quoted key; returns pointer to s_key_buf (NUL-terminated copy).
 * p->pos is left pointing at the first character AFTER the key. */
static char *parse_key(parser_t *p) {
    skip_whitespace(p);
    if (peof(p)) return NULL;

    if (ppeek(p) == '"') {
        /* Quoted key */
        padv(p); /* consume opening " */
        size_t di = 0;
        while (!peof(p) && ppeek(p) != '"') {
            if (di + 1 < sizeof(s_key_buf))
                s_key_buf[di++] = padv(p);
            else
                padv(p); /* overflow — advance but don't store */
        }
        if (peof(p)) return NULL;
        padv(p); /* consume closing " */
        s_key_buf[di] = '\0';
        return di ? s_key_buf : NULL;
    }

    /* Bare key: [A-Za-z0-9_-] */
    size_t di = 0;
    while (!peof(p)) {
        char c = ppeek(p);
        if (isalnum((unsigned char)c) || c == '_' || c == '-') {
            if (di + 1 < sizeof(s_key_buf))
                s_key_buf[di++] = padv(p);
            else
                padv(p);
        } else {
            break;
        }
    }
    if (di == 0) return NULL;
    s_key_buf[di] = '\0';
    return s_key_buf;
}

/* Static raw value buffer — parse_raw_value copies value here without corrupting src */
static char s_raw_buf[4096];

/* Parse raw value up to end-of-line; returns pointer to s_raw_buf (NUL-terminated).
 * p->pos is left pointing at the '\n' (or EOF) so the caller can advance past it. */
static char *parse_raw_value(parser_t *p) {
    skip_whitespace(p);
    size_t start = p->pos;
    /* Find end of value (newline or comment) */
    while (!peof(p) && ppeek(p) != '\n') {
        /* Handle quoted strings — allow # inside quotes */
        if (ppeek(p) == '"') {
            padv(p);
            while (!peof(p) && ppeek(p) != '"') {
                if (ppeek(p) == '\\') padv(p); /* skip escape */
                if (!peof(p)) padv(p);
            }
            if (!peof(p)) padv(p); /* closing quote */
        } else if (ppeek(p) == '#') {
            break; /* comment — stop before # */
        } else {
            padv(p);
        }
    }
    /* p->pos is now at '\n', EOF, or '#' — do NOT write into p->src */
    size_t end = p->pos;
    /* Trim trailing whitespace */
    while (end > start && (p->src[end-1] == ' ' || p->src[end-1] == '\t'))
        end--;
    /* Copy into static buffer */
    size_t vlen = end - start;
    if (vlen >= sizeof(s_raw_buf))
        vlen = sizeof(s_raw_buf) - 1;
    memcpy(s_raw_buf, p->src + start, vlen);
    s_raw_buf[vlen] = '\0';
    return s_raw_buf;
}

/* Get or create a subtable by name under parent */
static toml_table_t *get_or_create_subtab(toml_table_t *parent, const char *name) {
    for (int i = 0; i < parent->nsub; i++) {
        if (strcmp(parent->subtabs[i]->name, name) == 0)
            return parent->subtabs[i];
    }
    if (parent->nsub >= TOML_MAX_TABS) return NULL;
    toml_table_t *t = tab_new(name);
    if (!t) return NULL;
    parent->subtabs[parent->nsub++] = t;
    return t;
}

/* Navigate / create nested path like "cameras.driveway" */
static toml_table_t *navigate_path(toml_table_t *root, const char *path) {
    /* Make a mutable copy of path */
    char buf[256];
    size_t plen = strlen(path);
    if (plen >= sizeof(buf)) return NULL;
    memcpy(buf, path, plen + 1);

    toml_table_t *cur = root;
    char *seg = buf;
    while (seg && *seg) {
        char *dot = strchr(seg, '.');
        if (dot) *dot = '\0';
        cur = get_or_create_subtab(cur, seg);
        if (!cur) return NULL;
        seg = dot ? dot + 1 : NULL;
    }
    return cur;
}

static int tab_add_kv(toml_table_t *t, char *key, char *raw,
                       char *errbuf, int errbufsz) {
    if (t->nkv >= TOML_MAX_KEYS) {
        snprintf(errbuf, (size_t)errbufsz, "too many keys in table");
        return -1;
    }
    /* Duplicate existing? overwrite */
    for (int i = 0; i < t->nkv; i++) {
        if (strcmp(t->kv[i].key, key) == 0) {
            free(t->kv[i].raw);
            t->kv[i].raw = strdup(raw);
            return t->kv[i].raw ? 0 : -1;
        }
    }
    t->kv[t->nkv].key = strdup(key);
    t->kv[t->nkv].raw = strdup(raw);
    if (!t->kv[t->nkv].key || !t->kv[t->nkv].raw) {
        free(t->kv[t->nkv].key);
        free(t->kv[t->nkv].raw);
        snprintf(errbuf, (size_t)errbufsz, "out of memory");
        return -1;
    }
    t->nkv++;
    return 0;
}

/* --------------------------------------------------------------------- */
/* High-level datum API                                                   */
/* --------------------------------------------------------------------- */

toml_datum_t toml_string_in(const toml_table_t *tab, const char *key) {
    toml_datum_t d;
    memset(&d, 0, sizeof(d));
    const char *raw = toml_raw_in(tab, key);
    if (!raw) return d;
    char *s = NULL;
    if (toml_rtos(raw, &s) == 0) {
        d.ok   = 1;
        d.u.s  = s;
    }
    return d;
}

toml_datum_t toml_int_in(const toml_table_t *tab, const char *key) {
    toml_datum_t d;
    memset(&d, 0, sizeof(d));
    const char *raw = toml_raw_in(tab, key);
    if (!raw) return d;
    int64_t v = 0;
    if (toml_rtoi(raw, &v) == 0) {
        d.ok  = 1;
        d.u.i = v;
    }
    return d;
}

toml_datum_t toml_double_in(const toml_table_t *tab, const char *key) {
    toml_datum_t d;
    memset(&d, 0, sizeof(d));
    const char *raw = toml_raw_in(tab, key);
    if (!raw) return d;
    double v = 0.0;
    if (toml_rtod(raw, &v) == 0) {
        d.ok  = 1;
        d.u.d = v;
    }
    return d;
}

toml_datum_t toml_bool_in(const toml_table_t *tab, const char *key) {
    toml_datum_t d;
    memset(&d, 0, sizeof(d));
    const char *raw = toml_raw_in(tab, key);
    if (!raw) return d;
    int v = 0;
    if (toml_rtob(raw, &v) == 0) {
        d.ok  = 1;
        d.u.b = v;
    }
    return d;
}

toml_table_t *toml_parse_file(FILE *fp, char *errbuf, int errbufsz) {
    if (!fp) {
        snprintf(errbuf, (size_t)errbufsz, "null file pointer");
        return NULL;
    }

    /* Read entire file into a heap buffer */
    size_t cap  = 4096;
    size_t used = 0;
    char  *buf  = malloc(cap);
    if (!buf) {
        snprintf(errbuf, (size_t)errbufsz, "out of memory");
        return NULL;
    }

    int ch;
    while ((ch = fgetc(fp)) != EOF) {
        if (used + 2 >= cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) {
                free(buf);
                snprintf(errbuf, (size_t)errbufsz, "out of memory");
                return NULL;
            }
            buf = nb;
        }
        buf[used++] = (char)ch;
    }
    buf[used] = '\0';

    toml_table_t *root = toml_parse(buf, errbuf, errbufsz);
    if (!root) {
        free(buf);
        return NULL;
    }
    /* Attach buf to root so toml_free() can release it */
    root->_heap_buf = buf;
    return root;
}

/* --------------------------------------------------------------------- */
/* Main parser                                                             */
/* --------------------------------------------------------------------- */

toml_table_t *toml_parse(char *conf, char *errbuf, int errbufsz) {
    if (!conf) { snprintf(errbuf, (size_t)errbufsz, "null conf"); return NULL; }

    parser_t p;
    memset(&p, 0, sizeof(p));
    p.src      = conf;
    p.len      = strlen(conf);
    p.line     = 1;
    p.errbuf   = errbuf;
    p.errbufsz = errbufsz;

    toml_table_t *root = tab_new("");
    if (!root) {
        snprintf(errbuf, (size_t)errbufsz, "out of memory");
        return NULL;
    }

    toml_table_t *cur = root; /* current table being populated */

    while (!peof(&p)) {
        skip_whitespace(&p);
        if (peof(&p)) break;

        char c = ppeek(&p);

        if (c == '\n') {
            padv(&p);
            continue;
        }

        if (c == '#') {
            skip_to_eol(&p);
            continue;
        }

        if (c == '[') {
            padv(&p); /* consume [ */
            /* Check for [[ (array of tables) — not supported, skip */
            if (!peof(&p) && ppeek(&p) == '[') {
                skip_to_eol(&p);
                continue;
            }
            /* Table header */
            skip_whitespace(&p);
            size_t key_start = p.pos;
            while (!peof(&p) && ppeek(&p) != ']' && ppeek(&p) != '\n')
                padv(&p);
            if (peof(&p) || ppeek(&p) != ']') {
                snprintf(errbuf, (size_t)errbufsz, "line %d: unclosed [", p.line);
                toml_free(root);
                return NULL;
            }
            size_t key_end = p.pos;
            padv(&p); /* consume ] */
            /* Trim whitespace from table name — copy into static buffer, do NOT write to conf */
            while (key_end > key_start &&
                   (conf[key_end-1] == ' ' || conf[key_end-1] == '\t'))
                key_end--;
            /* Build tabpath in s_key_buf (reuse — it's safe here since parse_key not called) */
            size_t tlen = key_end - key_start;
            if (tlen >= sizeof(s_key_buf)) tlen = sizeof(s_key_buf) - 1;
            memcpy(s_key_buf, conf + key_start, tlen);
            s_key_buf[tlen] = '\0';
            /* Trim leading whitespace */
            char *tabpath = s_key_buf;
            while (*tabpath == ' ' || *tabpath == '\t') tabpath++;

            cur = navigate_path(root, tabpath);
            if (!cur) {
                snprintf(errbuf, (size_t)errbufsz,
                         "line %d: cannot create table '%s'", p.line, tabpath);
                toml_free(root);
                return NULL;
            }
            skip_comment_and_ws(&p);
            continue;
        }

        /* Key = value */
        char *key = parse_key(&p);
        if (!key) {
            skip_to_eol(&p);
            continue;
        }

        skip_whitespace(&p);
        if (peof(&p) || ppeek(&p) != '=') {
            skip_to_eol(&p);
            continue;
        }
        padv(&p); /* consume = */

        char *raw = parse_raw_value(&p);

        if (tab_add_kv(cur, key, raw, errbuf, errbufsz) < 0) {
            toml_free(root);
            return NULL;
        }

        /* skip to next line */
        if (!peof(&p) && ppeek(&p) == '\n') padv(&p);
    }

    return root;
}
