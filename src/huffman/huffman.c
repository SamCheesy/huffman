/*
 * huffman.c - Compresión/descompresión Huffman concurrente.
 *
 * Arquitectura de un job de COMPRESIÓN
 * ------------------------------------
 *   hilo coordinador ──► pool de N workers
 *
 *   Fase 1 (conteo): cada tarea hace pread() de un bloque, cuenta frecuencias
 *     en un histograma LOCAL (sin locks) y al final lo suma al global bajo
 *     `fmu` (reducción: 1 lock por bloque, no por byte).
 *   Barrera: tpool_wait_idle() (variable de condición, sin busy-wait).
 *   Árbol: el coordinador construye longitudes de código y la tabla canónica.
 *   Fase 2 (codificación): cada tarea codifica un bloque y deposita el
 *     resultado en slots[idx] (mutex `om` + cond `om_ready`).
 *   Escritor ordenado: un hilo único espera el slot `next_write`, lo escribe
 *     en el archivo de salida y avanza. El coordinador limita los bloques "en
 *     vuelo" con una ventana (cond `om_space`) => memoria acotada y sin
 *     interbloqueo (ver README, sección protocolo de concurrencia).
 *
 * DESCOMPRESIÓN: el coordinador recorre las cabeceras de bloque y encola una
 * tarea por bloque; cada worker decodifica y hace pwrite() en el offset
 * idx*block_size (escrituras disjuntas => no necesitan locks). Cada bloque
 * lleva CRC32 que se verifica.
 *
 * Salida atómica: se escribe a "<out>.XXXXXX" y solo al terminar bien se hace
 * rename() sobre el destino; si falla o se cancela, se borra el temporal.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "huffman.h"
#include "threadpool.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define HUF_MAGIC "HUF1"
#define HDR_SIZE 272 /* 4 + 4 + 8 + 256 */
#define BLK_HDR 12
#define MAXLEN 56 /* longitud máxima de código soportada */
#define DEFAULT_BS (1u << 20)
#define MIN_BS 4096u
#define MAX_BS (1u << 28)
#define MAX_WORKERS 64

/* ------------------------------------------------------------------ utilidades */

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static void put64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t get64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

static uint32_t crc_table[256];
static pthread_once_t crc_once = PTHREAD_ONCE_INIT;
static void crc_init(void)
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}
/* La tabla se inicializa en huff_job_start(), ANTES de crear ningun hilo del job:
 * pthread_create establece el happens-before, asi los workers solo leen. */
static uint32_t crc32_buf(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++)
        c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

static int pread_full(int fd, void *buf, size_t n, off_t off)
{
    uint8_t *p = buf;
    while (n) {
        ssize_t r = pread(fd, p, n, off);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0) {
            errno = EIO; /* EOF inesperado: el archivo se truncó */
            return -1;
        }
        p += r;
        n -= (size_t)r;
        off += r;
    }
    return 0;
}
static int pwrite_full(int fd, const void *buf, size_t n, off_t off)
{
    const uint8_t *p = buf;
    while (n) {
        ssize_t r = pwrite(fd, p, n, off);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += r;
        n -= (size_t)r;
        off += r;
    }
    return 0;
}
static int write_full(int fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    while (n) {
        ssize_t r = write(fd, p, n);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

/* ------------------------------------------------------------------ códigos */

typedef struct {
    uint8_t len[256];
    uint64_t code[256];
    int maxlen;
    int count[MAXLEN + 2];
    uint64_t first[MAXLEN + 2];
    int offset[MAXLEN + 2];
    uint8_t sorted[256];
} codec_t;

/* Construye longitudes óptimas (Huffman) a partir de frecuencias.
 * Desempates deterministas (peso, id). 0 ok, -1 si excede MAXLEN. */
static int build_lengths(const uint64_t freq[256], uint8_t len[256])
{
    uint64_t w[512];
    int parent[512];
    int active[256];
    int nact = 0;

    memset(len, 0, 256);
    for (int s = 0; s < 256; s++) {
        if (freq[s]) {
            w[s] = freq[s];
            parent[s] = -1;
            active[nact++] = s;
        }
    }
    if (nact == 0)
        return 0;
    if (nact == 1) {
        len[active[0]] = 1;
        return 0;
    }
    int next = 256;
    while (nact > 1) {
        int a = -1, b = -1;
        for (int i = 0; i < nact; i++) {
            int id = active[i];
            if (a < 0 || w[id] < w[active[a]] || (w[id] == w[active[a]] && id < active[a])) {
                b = a;
                a = i;
            } else if (b < 0 || w[id] < w[active[b]] || (w[id] == w[active[b]] && id < active[b])) {
                b = i;
            }
        }
        int ia = active[a], ib = active[b];
        w[next] = w[ia] + w[ib];
        parent[next] = -1;
        parent[ia] = next;
        parent[ib] = next;
        int lo = a < b ? a : b, hi = a < b ? b : a;
        active[lo] = next;
        nact--;
        active[hi] = active[nact];
        next++;
    }
    for (int s = 0; s < 256; s++) {
        if (!freq[s])
            continue;
        int d = 0;
        for (int n = s; parent[n] >= 0; n = parent[n])
            d++;
        if (d > MAXLEN)
            return -1;
        len[s] = (uint8_t)d;
    }
    return 0;
}

/* Tabla canónica a partir de longitudes (compresor y descompresor la usan). */
static int codec_build(codec_t *c, const uint8_t len[256])
{
    memset(c, 0, sizeof *c);
    memcpy(c->len, len, 256);
    for (int s = 0; s < 256; s++) {
        if (len[s]) {
            if (len[s] > MAXLEN)
                return -1;
            c->count[len[s]]++;
            if (len[s] > c->maxlen)
                c->maxlen = len[s];
        }
    }
    if (c->maxlen) {
        uint64_t kraft = 0;
        for (int l = 1; l <= c->maxlen; l++)
            kraft += (uint64_t)c->count[l] << (c->maxlen - l);
        if (kraft > (1ULL << c->maxlen))
            return -1; /* código no prefijo: archivo corrupto */
    }
    uint64_t code = 0;
    int idx = 0;
    for (int l = 1; l <= c->maxlen; l++) {
        code = (code + (uint64_t)(l > 1 ? c->count[l - 1] : 0)) << 1;
        c->first[l] = code;
        c->offset[l] = idx;
        idx += c->count[l];
    }
    int nidx[MAXLEN + 2];
    uint64_t ncode[MAXLEN + 2];
    for (int l = 1; l <= c->maxlen; l++) {
        nidx[l] = c->offset[l];
        ncode[l] = c->first[l];
    }
    for (int s = 0; s < 256; s++) {
        if (!len[s])
            continue;
        int l = len[s];
        c->sorted[nidx[l]++] = (uint8_t)s;
        c->code[s] = ncode[l]++;
    }
    return 0;
}

static int decode_block(const codec_t *c, const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen)
{
    size_t pos = 0;
    unsigned cur = 0;
    int nb = 0;
    for (size_t i = 0; i < outlen; i++) {
        uint64_t code = 0;
        int l;
        for (l = 1; l <= c->maxlen; l++) {
            if (nb == 0) {
                if (pos >= inlen)
                    return -1;
                cur = in[pos++];
                nb = 8;
            }
            nb--;
            code = (code << 1) | ((cur >> nb) & 1u);
            if (code >= c->first[l] && code - c->first[l] < (uint64_t)c->count[l]) {
                out[i] = c->sorted[c->offset[l] + (int)(code - c->first[l])];
                break;
            }
        }
        if (l > c->maxlen)
            return -1;
    }
    return pos == inlen ? 0 : -1;
}

/* ------------------------------------------------------------------ job */

typedef struct {
    uint8_t *data;
    uint32_t raw_len, comp_len, crc;
    int ready;
} slot_t;

typedef struct {
    struct huff_job *job;
    uint64_t idx;
} btask_t;

typedef struct {
    struct huff_job *job;
    uint64_t comp_off;
    uint32_t comp_len, raw_len, crc;
    uint64_t out_off;
} dtask_t;

struct huff_job {
    huff_op_t op;
    char *in_path, *out_path, *tmp_path;
    int workers;
    size_t bs;
    huff_notify_fn notify;
    void *nctx;

    pthread_t coord;
    int coord_started;

    _Atomic int state;
    _Atomic int cancel;
    _Atomic int failed;
    _Atomic uint64_t done;
    _Atomic uint64_t total; /* la escribe el coordinador y la lee la UI: atomica */
    _Atomic int last_pct;

    /* eventos / mensaje (protegidos por mu) */
    pthread_mutex_t mu;
    pthread_cond_t cv;
    uint64_t seq;
    char msg[320];

    int in_fd, out_fd;
    struct stat st;
    uint64_t size; /* tamaño original (sin comprimir) */
    uint64_t nblocks;

    /* reducción de frecuencias */
    pthread_mutex_t fmu;
    uint64_t freq[256];

    codec_t codec;

    /* salida ordenada */
    pthread_mutex_t om;
    pthread_cond_t om_ready;
    pthread_cond_t om_space;
    slot_t *slots;
    uint64_t next_write;
    uint64_t out_bytes; /* solo lo toca el escritor / coordinador tras join */
    pthread_t writer;
    int writer_started;
};

static int should_stop(struct huff_job *j)
{
    return atomic_load(&j->failed) || atomic_load(&j->cancel);
}

static void wake_waiters(struct huff_job *j)
{
    pthread_mutex_lock(&j->om);
    pthread_cond_broadcast(&j->om_ready);
    pthread_cond_broadcast(&j->om_space);
    pthread_mutex_unlock(&j->om);
}

static void job_event(struct huff_job *j)
{
    pthread_mutex_lock(&j->mu);
    j->seq++;
    pthread_cond_broadcast(&j->cv);
    pthread_mutex_unlock(&j->mu);
    if (j->notify)
        j->notify(j->nctx);
}

static void job_fail(struct huff_job *j, const char *fmt, ...)
{
    char buf[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&j->mu);
    if (!atomic_load(&j->failed)) {
        snprintf(j->msg, sizeof j->msg, "%s", buf);
        atomic_store(&j->failed, 1);
    }
    pthread_mutex_unlock(&j->mu);
    wake_waiters(j);
}

static void progress_add(struct huff_job *j, uint64_t n)
{
    uint64_t d = atomic_fetch_add(&j->done, n) + n;
    uint64_t tot = atomic_load(&j->total);
    int pct = tot ? (int)(d * 100 / tot) : 100;
    if (pct > 99)
        pct = 99; /* 100 solo cuando el job termina de verdad */
    if (atomic_exchange(&j->last_pct, pct) != pct)
        job_event(j);
}

static uint32_t blk_len(const struct huff_job *j, uint64_t idx)
{
    uint64_t off = idx * j->bs;
    uint64_t rem = j->size - off;
    return (uint32_t)(rem < j->bs ? rem : j->bs);
}

/* ------------------------------------------------------------------ compresión */

static void count_task(void *p)
{
    btask_t *t = p;
    struct huff_job *j = t->job;
    if (should_stop(j))
        return;
    uint32_t len = blk_len(j, t->idx);
    uint8_t *buf = malloc(len);
    if (!buf) {
        job_fail(j, "sin memoria");
        return;
    }
    if (pread_full(j->in_fd, buf, len, (off_t)(t->idx * j->bs)) < 0) {
        job_fail(j, "error leyendo '%s': %s", j->in_path, strerror(errno));
        free(buf);
        return;
    }
    uint64_t local[256] = {0};
    for (uint32_t i = 0; i < len; i++)
        local[buf[i]]++;
    free(buf);
    pthread_mutex_lock(&j->fmu);
    for (int k = 0; k < 256; k++)
        j->freq[k] += local[k];
    pthread_mutex_unlock(&j->fmu);
    progress_add(j, len);
}

static void encode_task(void *p)
{
    btask_t *t = p;
    struct huff_job *j = t->job;
    if (should_stop(j))
        return;
    uint32_t len = blk_len(j, t->idx);
    uint8_t *buf = malloc(len);
    if (!buf) {
        job_fail(j, "sin memoria");
        return;
    }
    if (pread_full(j->in_fd, buf, len, (off_t)(t->idx * j->bs)) < 0) {
        job_fail(j, "error leyendo '%s': %s", j->in_path, strerror(errno));
        free(buf);
        return;
    }
    const codec_t *c = &j->codec;
    uint64_t bits = 0;
    for (uint32_t i = 0; i < len; i++) {
        uint8_t l = c->len[buf[i]];
        if (!l) {
            job_fail(j, "el archivo de origen cambió durante la compresión");
            free(buf);
            return;
        }
        bits += l;
    }
    size_t comp = (size_t)((bits + 7) / 8);
    uint8_t *out = malloc(comp ? comp : 1);
    if (!out) {
        free(buf);
        job_fail(j, "sin memoria");
        return;
    }
    uint64_t acc = 0;
    int nb = 0;
    size_t pos = 0;
    for (uint32_t i = 0; i < len; i++) {
        uint8_t s = buf[i], l = c->len[s];
        acc = (acc << l) | c->code[s];
        nb += l;
        while (nb >= 8) {
            nb -= 8;
            out[pos++] = (uint8_t)(acc >> nb);
        }
    }
    if (nb > 0)
        out[pos++] = (uint8_t)(acc << (8 - nb));
    uint32_t crc = crc32_buf(buf, len);
    free(buf);
    if (pos != comp) {
        free(out);
        job_fail(j, "error interno de codificación");
        return;
    }

    pthread_mutex_lock(&j->om);
    slot_t *s = &j->slots[t->idx];
    s->data = out;
    s->raw_len = len;
    s->comp_len = (uint32_t)comp;
    s->crc = crc;
    s->ready = 1;
    pthread_cond_signal(&j->om_ready); /* solo el escritor espera aquí */
    pthread_mutex_unlock(&j->om);
}

/* Hilo escritor: escribe los bloques estrictamente en orden de índice. */
static void *writer_main(void *arg)
{
    struct huff_job *j = arg;
    for (uint64_t i = 0; i < j->nblocks; i++) {
        pthread_mutex_lock(&j->om);
        while (!j->slots[i].ready && !should_stop(j))
            pthread_cond_wait(&j->om_ready, &j->om);
        if (should_stop(j)) {
            pthread_mutex_unlock(&j->om);
            return NULL;
        }
        slot_t s = j->slots[i];
        j->slots[i].data = NULL; /* el escritor es ahora dueño del buffer */
        pthread_mutex_unlock(&j->om);

        uint8_t hdr[BLK_HDR];
        put32(hdr, s.raw_len);
        put32(hdr + 4, s.comp_len);
        put32(hdr + 8, s.crc);
        int rc = write_full(j->out_fd, hdr, BLK_HDR);
        if (rc == 0 && s.comp_len)
            rc = write_full(j->out_fd, s.data, s.comp_len);
        int saved = errno;
        free(s.data);
        if (rc < 0) {
            job_fail(j, "error escribiendo '%s': %s", j->out_path, strerror(saved));
            return NULL;
        }
        j->out_bytes += BLK_HDR + s.comp_len;

        pthread_mutex_lock(&j->om);
        j->next_write = i + 1;
        pthread_cond_broadcast(&j->om_space);
        pthread_mutex_unlock(&j->om);
        progress_add(j, s.raw_len);
    }
    return NULL;
}

static int open_tmp_output(struct huff_job *j)
{
    size_t n = strlen(j->out_path) + 8;
    j->tmp_path = malloc(n);
    if (!j->tmp_path) {
        job_fail(j, "sin memoria");
        return -1;
    }
    snprintf(j->tmp_path, n, "%s.XXXXXX", j->out_path);
    j->out_fd = mkostemp(j->tmp_path, O_CLOEXEC);
    if (j->out_fd < 0) {
        job_fail(j, "no se puede crear salida para '%s': %s", j->out_path, strerror(errno));
        free(j->tmp_path);
        j->tmp_path = NULL;
        return -1;
    }
    fchmod(j->out_fd, 0644);
    return 0;
}

static int open_input(struct huff_job *j)
{
    j->in_fd = open(j->in_path, O_RDONLY | O_CLOEXEC);
    if (j->in_fd < 0) {
        job_fail(j, "no se puede abrir '%s': %s", j->in_path, strerror(errno));
        return -1;
    }
    if (fstat(j->in_fd, &j->st) < 0) {
        job_fail(j, "fstat '%s': %s", j->in_path, strerror(errno));
        return -1;
    }
    if (!S_ISREG(j->st.st_mode)) {
        job_fail(j, "'%s' no es un archivo regular", j->in_path);
        return -1;
    }
    return 0;
}

static int input_unchanged(struct huff_job *j)
{
    struct stat now;
    if (fstat(j->in_fd, &now) < 0)
        return 0;
    return now.st_size == j->st.st_size && now.st_ino == j->st.st_ino &&
           now.st_mtim.tv_sec == j->st.st_mtim.tv_sec && now.st_mtim.tv_nsec == j->st.st_mtim.tv_nsec;
}

static int commit_output(struct huff_job *j)
{
    if (should_stop(j))
        return -1;
    if (fsync(j->out_fd) < 0) {
        job_fail(j, "fsync: %s", strerror(errno));
        return -1;
    }
    if (close(j->out_fd) < 0) {
        j->out_fd = -1;
        job_fail(j, "close: %s", strerror(errno));
        return -1;
    }
    j->out_fd = -1;
    if (rename(j->tmp_path, j->out_path) < 0) {
        job_fail(j, "no se pudo crear '%s': %s", j->out_path, strerror(errno));
        return -1;
    }
    free(j->tmp_path);
    j->tmp_path = NULL;
    return 0;
}

static int run_compress(struct huff_job *j)
{
    int rc = -1;
    tpool_t *pool = NULL;
    btask_t *bt = NULL;

    if (open_input(j) < 0)
        return -1;
    j->size = (uint64_t)j->st.st_size;
    j->nblocks = (j->size + j->bs - 1) / j->bs;
    atomic_store(&j->total, j->size ? 2 * j->size : 1);
    if (open_tmp_output(j) < 0)
        return -1;

    pool = tpool_create(j->workers);
    bt = calloc(j->nblocks ? j->nblocks : 1, sizeof *bt);
    j->slots = calloc(j->nblocks ? j->nblocks : 1, sizeof *j->slots);
    if (!pool || !bt || !j->slots) {
        job_fail(j, "sin memoria");
        goto out;
    }

    /* ---- Fase 1: conteo de frecuencias en paralelo ---- */
    for (uint64_t i = 0; i < j->nblocks && !should_stop(j); i++) {
        bt[i].job = j;
        bt[i].idx = i;
        if (tpool_submit(pool, count_task, &bt[i]) < 0) {
            job_fail(j, "no se pudo encolar tarea");
            break;
        }
    }
    tpool_wait_idle(pool); /* barrera */
    if (should_stop(j))
        goto out;

    /* ---- Árbol de Huffman global (secuencial, 256 símbolos) ---- */
    uint8_t lens[256];
    if (build_lengths(j->freq, lens) < 0 || codec_build(&j->codec, lens) < 0) {
        job_fail(j, "distribución de frecuencias demasiado sesgada (código > %d bits)", MAXLEN);
        goto out;
    }
    uint8_t hdr[HDR_SIZE];
    memcpy(hdr, HUF_MAGIC, 4);
    put32(hdr + 4, (uint32_t)j->bs);
    put64(hdr + 8, j->size);
    memcpy(hdr + 16, lens, 256);
    if (write_full(j->out_fd, hdr, HDR_SIZE) < 0) {
        job_fail(j, "error escribiendo '%s': %s", j->out_path, strerror(errno));
        goto out;
    }
    j->out_bytes = HDR_SIZE;

    /* ---- Fase 2: codificación paralela + escritor ordenado ---- */
    if (j->nblocks > 0) {
        if (pthread_create(&j->writer, NULL, writer_main, j) != 0) {
            job_fail(j, "no se pudo crear el hilo escritor");
            goto out;
        }
        j->writer_started = 1;
    }
    uint64_t window = (uint64_t)j->workers * 2 + 2;
    for (uint64_t i = 0; i < j->nblocks; i++) {
        pthread_mutex_lock(&j->om);
        while (!should_stop(j) && i - j->next_write >= window)
            pthread_cond_wait(&j->om_space, &j->om);
        pthread_mutex_unlock(&j->om);
        if (should_stop(j))
            break;
        if (tpool_submit(pool, encode_task, &bt[i]) < 0) {
            job_fail(j, "no se pudo encolar tarea");
            break;
        }
    }
    tpool_wait_idle(pool);
    if (j->writer_started) {
        pthread_join(j->writer, NULL);
        j->writer_started = 0;
    }
    if (should_stop(j))
        goto out;

    if (!input_unchanged(j)) {
        job_fail(j, "el archivo de origen fue modificado durante la compresión");
        goto out;
    }
    if (commit_output(j) == 0)
        rc = 0;

out:
    /* Limpieza: el escritor ya fue unido o nunca arrancó; el pool se destruye
     * (join de todos los workers) antes de liberar lo que ellos usan. */
    if (j->writer_started) {
        wake_waiters(j);
        pthread_join(j->writer, NULL);
        j->writer_started = 0;
    }
    tpool_destroy(pool);
    if (j->slots) {
        for (uint64_t i = 0; i < j->nblocks; i++)
            free(j->slots[i].data);
        free(j->slots);
        j->slots = NULL;
    }
    free(bt);
    if (rc == 0) {
        pthread_mutex_lock(&j->mu);
        snprintf(j->msg, sizeof j->msg, "Comprimido: %llu -> %llu bytes (%.1f%%)", (unsigned long long)j->size,
                 (unsigned long long)j->out_bytes, j->size ? 100.0 * (double)j->out_bytes / (double)j->size : 100.0);
        pthread_mutex_unlock(&j->mu);
    }
    return rc;
}

/* ------------------------------------------------------------------ descompresión */

static void decode_task(void *p)
{
    dtask_t *t = p;
    struct huff_job *j = t->job;
    if (should_stop(j))
        return;
    uint8_t *cbuf = malloc(t->comp_len ? t->comp_len : 1);
    uint8_t *rbuf = malloc(t->raw_len ? t->raw_len : 1);
    if (!cbuf || !rbuf) {
        free(cbuf);
        free(rbuf);
        job_fail(j, "sin memoria");
        return;
    }
    if (pread_full(j->in_fd, cbuf, t->comp_len, (off_t)t->comp_off) < 0) {
        job_fail(j, "error leyendo '%s': %s", j->in_path, strerror(errno));
        goto done;
    }
    if (decode_block(&j->codec, cbuf, t->comp_len, rbuf, t->raw_len) < 0) {
        job_fail(j, "datos corruptos en '%s' (bloque en offset %llu)", j->in_path, (unsigned long long)t->comp_off);
        goto done;
    }
    if (crc32_buf(rbuf, t->raw_len) != t->crc) {
        job_fail(j, "CRC32 inválido en '%s' (bloque en offset %llu)", j->in_path, (unsigned long long)t->comp_off);
        goto done;
    }
    if (pwrite_full(j->out_fd, rbuf, t->raw_len, (off_t)t->out_off) < 0) {
        job_fail(j, "error escribiendo '%s': %s", j->out_path, strerror(errno));
        goto done;
    }
    progress_add(j, t->raw_len);
done:
    free(cbuf);
    free(rbuf);
}

static int run_decompress(struct huff_job *j)
{
    int rc = -1;
    tpool_t *pool = NULL;
    dtask_t *dt = NULL;

    if (open_input(j) < 0)
        return -1;
    uint64_t fsize = (uint64_t)j->st.st_size;
    uint8_t hdr[HDR_SIZE];
    if (fsize < HDR_SIZE || pread_full(j->in_fd, hdr, HDR_SIZE, 0) < 0 || memcmp(hdr, HUF_MAGIC, 4) != 0) {
        job_fail(j, "'%s' no es un archivo .huf válido", j->in_path);
        return -1;
    }
    uint32_t bs = get32(hdr + 4);
    uint64_t orig = get64(hdr + 8);
    if (bs < 1 || bs > MAX_BS || orig > (1ULL << 62) || codec_build(&j->codec, hdr + 16) < 0 ||
        (orig > 0 && j->codec.maxlen == 0)) {
        job_fail(j, "cabecera corrupta en '%s'", j->in_path);
        return -1;
    }
    j->bs = bs;
    j->size = orig;
    j->nblocks = (orig + bs - 1) / bs;
    if (j->nblocks > (fsize - HDR_SIZE) / BLK_HDR) {
        job_fail(j, "'%s' está truncado o corrupto", j->in_path);
        return -1;
    }
    atomic_store(&j->total, orig ? orig : 1);
    if (open_tmp_output(j) < 0)
        return -1;
    if (ftruncate(j->out_fd, (off_t)orig) < 0) {
        job_fail(j, "no se pudo reservar espacio para '%s': %s", j->out_path, strerror(errno));
        return -1;
    }

    pool = tpool_create(j->workers);
    dt = calloc(j->nblocks ? j->nblocks : 1, sizeof *dt);
    if (!pool || !dt) {
        job_fail(j, "sin memoria");
        goto out;
    }

    uint64_t off = HDR_SIZE;
    for (uint64_t i = 0; i < j->nblocks && !should_stop(j); i++) {
        uint8_t bh[BLK_HDR];
        if (pread_full(j->in_fd, bh, BLK_HDR, (off_t)off) < 0) {
            job_fail(j, "'%s' está truncado", j->in_path);
            break;
        }
        uint32_t raw = get32(bh), comp = get32(bh + 4), crc = get32(bh + 8);
        uint64_t expect = (i + 1 < j->nblocks) ? bs : orig - i * (uint64_t)bs;
        if (raw != expect || comp > (uint64_t)raw * 7 + 8 || off + BLK_HDR + comp > fsize) {
            job_fail(j, "bloque %llu corrupto en '%s'", (unsigned long long)i, j->in_path);
            break;
        }
        dt[i] = (dtask_t){j, off + BLK_HDR, comp, raw, crc, i * (uint64_t)bs};
        if (tpool_submit(pool, decode_task, &dt[i]) < 0) {
            job_fail(j, "no se pudo encolar tarea");
            break;
        }
        off += BLK_HDR + comp;
    }
    if (!should_stop(j) && off != fsize)
        job_fail(j, "datos sobrantes al final de '%s'", j->in_path);
    tpool_wait_idle(pool);
    if (should_stop(j))
        goto out;
    if (!input_unchanged(j)) {
        job_fail(j, "el archivo de origen fue modificado durante la descompresión");
        goto out;
    }
    if (commit_output(j) == 0)
        rc = 0;

out:
    tpool_destroy(pool);
    free(dt);
    if (rc == 0) {
        pthread_mutex_lock(&j->mu);
        snprintf(j->msg, sizeof j->msg, "Descomprimido: %llu bytes", (unsigned long long)j->size);
        pthread_mutex_unlock(&j->mu);
    }
    return rc;
}

/* ------------------------------------------------------------------ ciclo de vida */

static void *coord_main(void *arg)
{
    struct huff_job *j = arg;
    int rc = (j->op == HUFF_COMPRESS) ? run_compress(j) : run_decompress(j);

    /* Limpieza común (única responsabilidad: no dejar fds ni temporales) */
    if (j->in_fd >= 0)
        close(j->in_fd);
    j->in_fd = -1;
    if (j->out_fd >= 0)
        close(j->out_fd);
    j->out_fd = -1;
    if (j->tmp_path) {
        unlink(j->tmp_path);
        free(j->tmp_path);
        j->tmp_path = NULL;
    }

    int st;
    if (rc == 0) {
        st = HUFF_DONE;
    } else if (atomic_load(&j->failed)) {
        st = HUFF_ERROR;
    } else {
        st = HUFF_CANCELED;
        pthread_mutex_lock(&j->mu);
        snprintf(j->msg, sizeof j->msg, "Cancelado");
        pthread_mutex_unlock(&j->mu);
    }
    atomic_store(&j->state, st); /* antes del evento final: sin wakeups perdidos */
    job_event(j);
    return NULL;
}

static void set_err(char *err, size_t n, const char *fmt, const char *a)
{
    if (err && n)
        snprintf(err, n, fmt, a);
}

huff_job_t *huff_job_start(huff_op_t op, const char *in_path, const char *out_path, const huff_opts_t *opts, char *err,
                           size_t errlen)
{
    if (access(in_path, R_OK) < 0) {
        set_err(err, errlen, "no se puede leer el origen: %s", strerror(errno));
        return NULL;
    }
    pthread_once(&crc_once, crc_init);
    struct huff_job *j = calloc(1, sizeof *j);
    if (!j) {
        set_err(err, errlen, "%s", "sin memoria");
        return NULL;
    }
    j->op = op;
    j->in_path = strdup(in_path);
    j->out_path = strdup(out_path);
    j->in_fd = j->out_fd = -1;
    int w = opts ? opts->workers : 0;
    if (w <= 0) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        w = n > 0 ? (int)n : 2;
    }
    if (w > MAX_WORKERS)
        w = MAX_WORKERS;
    j->workers = w;
    size_t bs = (opts && opts->block_size) ? opts->block_size : DEFAULT_BS;
    if (bs < MIN_BS)
        bs = MIN_BS;
    if (bs > MAX_BS)
        bs = MAX_BS;
    j->bs = bs;
    if (opts) {
        j->notify = opts->notify;
        j->nctx = opts->notify_ctx;
    }
    atomic_init(&j->state, HUFF_RUNNING);
    atomic_init(&j->cancel, 0);
    atomic_init(&j->failed, 0);
    atomic_init(&j->done, 0);
    atomic_init(&j->last_pct, -1);
    pthread_mutex_init(&j->mu, NULL);
    pthread_cond_init(&j->cv, NULL);
    pthread_mutex_init(&j->fmu, NULL);
    pthread_mutex_init(&j->om, NULL);
    pthread_cond_init(&j->om_ready, NULL);
    pthread_cond_init(&j->om_space, NULL);
    atomic_init(&j->total, 1);
    if (!j->in_path || !j->out_path || pthread_create(&j->coord, NULL, coord_main, j) != 0) {
        set_err(err, errlen, "%s", "no se pudo crear el hilo coordinador");
        free(j->in_path);
        free(j->out_path);
        pthread_mutex_destroy(&j->mu);
        pthread_cond_destroy(&j->cv);
        pthread_mutex_destroy(&j->fmu);
        pthread_mutex_destroy(&j->om);
        pthread_cond_destroy(&j->om_ready);
        pthread_cond_destroy(&j->om_space);
        free(j);
        return NULL;
    }
    j->coord_started = 1;
    return j;
}

huff_state_t huff_job_state(huff_job_t *j)
{
    return (huff_state_t)atomic_load(&j->state);
}

int huff_job_percent(huff_job_t *j)
{
    if (atomic_load(&j->state) == HUFF_DONE)
        return 100;
    uint64_t tot = atomic_load(&j->total);
    int p = tot ? (int)(atomic_load(&j->done) * 100 / tot) : 0;
    return p > 99 ? 99 : p;
}

void huff_job_message(huff_job_t *j, char *buf, size_t n)
{
    pthread_mutex_lock(&j->mu);
    snprintf(buf, n, "%s", j->msg);
    pthread_mutex_unlock(&j->mu);
}

void huff_job_cancel(huff_job_t *j)
{
    atomic_store(&j->cancel, 1);
    wake_waiters(j); /* despierta escritor/coordinador dormidos en om_* */
}

uint64_t huff_job_wait_event(huff_job_t *j, uint64_t last)
{
    pthread_mutex_lock(&j->mu);
    while (j->seq == last)
        pthread_cond_wait(&j->cv, &j->mu);
    uint64_t s = j->seq;
    pthread_mutex_unlock(&j->mu);
    return s;
}

huff_state_t huff_job_wait(huff_job_t *j)
{
    pthread_mutex_lock(&j->mu);
    while (atomic_load(&j->state) == HUFF_RUNNING)
        pthread_cond_wait(&j->cv, &j->mu);
    pthread_mutex_unlock(&j->mu);
    return (huff_state_t)atomic_load(&j->state);
}

void huff_job_free(huff_job_t *j)
{
    if (!j)
        return;
    if (atomic_load(&j->state) == HUFF_RUNNING)
        huff_job_cancel(j);
    if (j->coord_started)
        pthread_join(j->coord, NULL);
    pthread_mutex_destroy(&j->mu);
    pthread_cond_destroy(&j->cv);
    pthread_mutex_destroy(&j->fmu);
    pthread_mutex_destroy(&j->om);
    pthread_cond_destroy(&j->om_ready);
    pthread_cond_destroy(&j->om_space);
    free(j->in_path);
    free(j->out_path);
    free(j);
}
