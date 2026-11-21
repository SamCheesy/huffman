/*
 * huffman.h - Compresor/descompresor Huffman concurrente (API de "jobs").
 *
 * Un job se ejecuta en segundo plano: huff_job_start() devuelve de inmediato y
 * el trabajo lo hace un hilo coordinador + un pool de workers. El llamador
 * (editor, CLI) consulta progreso/estado sin bloquearse, o se entera por el
 * callback `notify` (que se invoca desde hilos internos: debe ser thread-safe
 * y rápido, p. ej. escribir un byte en un pipe).
 *
 * Formato .huf (little-endian):
 *   "HUF1" | u32 block_size | u64 original_size | u8 code_len[256]
 *   por bloque: u32 raw_len | u32 comp_len | u32 crc32(raw) | datos
 */
#ifndef HUFFMAN_H
#define HUFFMAN_H

#include <stddef.h>
#include <stdint.h>

typedef enum { HUFF_COMPRESS = 0, HUFF_DECOMPRESS = 1 } huff_op_t;
typedef enum { HUFF_RUNNING = 0, HUFF_DONE, HUFF_ERROR, HUFF_CANCELED } huff_state_t;

typedef struct huff_job huff_job_t;
typedef void (*huff_notify_fn)(void *ctx);

typedef struct {
    int workers;            /* 0 => nº de CPUs en línea */
    size_t block_size;      /* 0 => 1 MiB (solo compresión) */
    huff_notify_fn notify;  /* opcional */
    void *notify_ctx;
} huff_opts_t;

/* Lanza el job. Devuelve NULL (y llena err) si no se pudo iniciar. */
huff_job_t *huff_job_start(huff_op_t op, const char *in_path, const char *out_path,
                           const huff_opts_t *opts, char *err, size_t errlen);

huff_state_t huff_job_state(huff_job_t *j);
int huff_job_percent(huff_job_t *j);                 /* 0..100 */
void huff_job_message(huff_job_t *j, char *buf, size_t n);
void huff_job_cancel(huff_job_t *j);                 /* asíncrono, idempotente */

/* Bloquea (dormido) hasta que cambie el contador de eventos distinto a `last`.
 * Devuelve el nuevo valor. Útil para CLIs sin pipe. */
uint64_t huff_job_wait_event(huff_job_t *j, uint64_t last);

/* Bloquea hasta que termine; devuelve el estado final. */
huff_state_t huff_job_wait(huff_job_t *j);

/* Cancela si sigue corriendo, hace join del coordinador y libera todo. */
void huff_job_free(huff_job_t *j);

#endif
