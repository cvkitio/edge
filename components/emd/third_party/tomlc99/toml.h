/*
 * MIT License
 * tomlc99 — minimal TOML 1.0 parser subset
 * Supports: string, int, bool, float, tables [section]
 */
#ifndef TOML_H
#define TOML_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct toml_table_t toml_table_t;
typedef struct toml_array_t toml_array_t;

/* Parse a TOML document from a writable copy of the config text.
 * Returns a table on success, NULL on error (errbuf filled). */
toml_table_t *toml_parse(char *conf, char *errbuf, int errbufsz);

/* Free a table returned by toml_parse. */
void toml_free(toml_table_t *tab);

/* Look up a subtable by key.  Returns NULL if not found. */
toml_table_t *toml_table_in(const toml_table_t *tab, const char *key);

/* Look up a raw value string by key.  Returns NULL if not found.
 * The returned pointer is into the original conf buffer. */
const char *toml_raw_in(const toml_table_t *tab, const char *key);

/* Conversion helpers: return 0 on success, -1 on error. */
int toml_rtos(const char *raw, char **ret);    /* raw → heap-allocated string */
int toml_rtoi(const char *raw, int64_t *ret);  /* raw → int64_t              */
int toml_rtod(const char *raw, double *ret);   /* raw → double               */
int toml_rtob(const char *raw, int *ret);      /* raw → bool (0 or 1)        */

/* Iterate keys in a table (for camera enumeration).
 * idx counts from 0; returns NULL when exhausted. */
const char *toml_key_in(const toml_table_t *tab, int idx);

/* Number of direct keys in a table */
int toml_table_nkval(const toml_table_t *tab);

/* Number of sub-tables */
int toml_table_ntab(const toml_table_t *tab);

/* --------------------------------------------------------------------- */
/* High-level "datum" API (compatible with tomlc99 v2 API)               */
/* --------------------------------------------------------------------- */

typedef struct {
    int ok;             /* 1 if value was found and parsed, 0 otherwise */
    union {
        char   *s;      /* heap-allocated string (caller must free) */
        int64_t i;
        double  d;
        int     b;      /* bool: 0 or 1 */
    } u;
} toml_datum_t;

/* Look up and decode a string value by key.  On success, ok=1 and u.s is
 * a heap-allocated NUL-terminated string the caller must free(). */
toml_datum_t toml_string_in(const toml_table_t *tab, const char *key);

/* Look up and decode an integer value by key.  On success, ok=1, u.i set. */
toml_datum_t toml_int_in(const toml_table_t *tab, const char *key);

/* Look up and decode a double value by key.  On success, ok=1, u.d set. */
toml_datum_t toml_double_in(const toml_table_t *tab, const char *key);

/* Look up and decode a boolean value by key.  On success, ok=1, u.b set. */
toml_datum_t toml_bool_in(const toml_table_t *tab, const char *key);

/* Parse a TOML document from a FILE*.  Reads until EOF into a buffer,
 * then calls toml_parse().  Returns NULL on error (errbuf filled). */
toml_table_t *toml_parse_file(FILE *fp, char *errbuf, int errbufsz);

#ifdef __cplusplus
}
#endif

#endif /* TOML_H */
