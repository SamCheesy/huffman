#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "filelock.h"

#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FL_MAX 64

typedef struct {
    int used;
    char key[PATH_MAX];
    int readers;
    int writer;
} entry_t;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static entry_t tab[FL_MAX];

/* Ruta canónica aunque el archivo aún no exista (realpath del directorio). */
static void make_key(const char *path, char *key)
{
    char *r = realpath(path, NULL);
    if (r) {
        snprintf(key, PATH_MAX, "%s", r);
        free(r);
        return;
    }
    const char *slash = strrchr(path, '/');
    char dir[PATH_MAX];
    const char *base;
    if (!slash) {
        strcpy(dir, ".");
        base = path;
    } else {
        size_t n = (size_t)(slash - path);
        if (n == 0) {
            strcpy(dir, "/");
        } else {
            if (n >= sizeof dir)
                n = sizeof dir - 1;
            memcpy(dir, path, n);
            dir[n] = '\0';
        }
        base = slash + 1;
    }
    char *rd = realpath(dir, NULL);
    if (rd) {
        snprintf(key, PATH_MAX, "%s/%s", strcmp(rd, "/") ? rd : "", base);
        free(rd);
    } else {
        snprintf(key, PATH_MAX, "%s", path);
    }
}

static entry_t *find(const char *key, int create)
{
    entry_t *freeslot = NULL;
    for (int i = 0; i < FL_MAX; i++) {
        if (tab[i].used && strcmp(tab[i].key, key) == 0)
            return &tab[i];
        if (!tab[i].used && !freeslot)
            freeslot = &tab[i];
    }
    if (create && freeslot) {
        memset(freeslot, 0, sizeof *freeslot);
        freeslot->used = 1;
        snprintf(freeslot->key, PATH_MAX, "%s", key);
        return freeslot;
    }
    return NULL;
}

int fl_acquire(const char *path, fl_mode_t mode)
{
    char key[PATH_MAX];
    make_key(path, key);
    int rc = -1;
    pthread_mutex_lock(&mu);
    entry_t *e = find(key, 1);
    if (e) {
        if (mode == FL_READ && !e->writer) {
            e->readers++;
            rc = 0;
        } else if (mode == FL_WRITE && !e->writer && e->readers == 0) {
            e->writer = 1;
            rc = 0;
        }
        if (rc != 0 && e->readers == 0 && !e->writer)
            e->used = 0; /* entrada recién creada y no usada */
    }
    pthread_mutex_unlock(&mu);
    return rc;
}

void fl_release(const char *path, fl_mode_t mode)
{
    char key[PATH_MAX];
    make_key(path, key);
    pthread_mutex_lock(&mu);
    entry_t *e = find(key, 0);
    if (e) {
        if (mode == FL_READ && e->readers > 0)
            e->readers--;
        else if (mode == FL_WRITE)
            e->writer = 0;
        if (e->readers == 0 && !e->writer)
            e->used = 0;
    }
    pthread_mutex_unlock(&mu);
}

int fl_is_locked(const char *path, fl_mode_t mode)
{
    char key[PATH_MAX];
    make_key(path, key);
    int r = 0;
    pthread_mutex_lock(&mu);
    entry_t *e = find(key, 0);
    if (e)
        r = (mode == FL_READ) ? e->readers > 0 : e->writer;
    pthread_mutex_unlock(&mu);
    return r;
}
