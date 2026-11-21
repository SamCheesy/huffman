/*
 * huffcli - Interfaz de línea de comandos del compresor Huffman concurrente.
 *
 *   huffcli c <entrada> <salida.huf> [-j hilos] [-b KiB] [-q]
 *   huffcli d <entrada.huf> <salida>  [-j hilos] [-q]
 *
 * Código de salida: 0 ok, 1 error, 130 cancelado (SIGINT/SIGTERM).
 * Las señales se manejan con un hilo dedicado en sigwait() (nada de lógica
 * dentro de handlers asíncronos).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "huffman.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static huff_job_t *g_job;

static void *sig_thread(void *arg)
{
    sigset_t *set = arg;
    for (;;) {
        int sig = 0;
        if (sigwait(set, &sig) != 0)
            continue;
        if (sig == SIGUSR1)
            return NULL; /* orden de salida desde main */
        huff_job_cancel(g_job);
    }
}

static void usage(void)
{
    fprintf(stderr, "uso: huffcli (c|d) <entrada> <salida> [-j hilos] [-b KiB] [-q]\n");
}

int main(int argc, char **argv)
{
    if (argc < 4 || (strcmp(argv[1], "c") != 0 && strcmp(argv[1], "d") != 0)) {
        usage();
        return 2;
    }
    huff_opts_t o = {0};
    int quiet = 0;
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "-j") && i + 1 < argc)
            o.workers = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-b") && i + 1 < argc)
            o.block_size = (size_t)atol(argv[++i]) * 1024;
        else if (!strcmp(argv[i], "-q"))
            quiet = 1;
        else {
            usage();
            return 2;
        }
    }

    /* Bloquear señales ANTES de crear hilos: todos los hilos las heredan. */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
    signal(SIGPIPE, SIG_IGN);

    char err[256];
    huff_op_t op = argv[1][0] == 'c' ? HUFF_COMPRESS : HUFF_DECOMPRESS;
    g_job = huff_job_start(op, argv[2], argv[3], &o, err, sizeof err);
    if (!g_job) {
        fprintf(stderr, "huffcli: %s\n", err);
        return 1;
    }
    pthread_t st;
    pthread_create(&st, NULL, sig_thread, &set);

    uint64_t seq = 0;
    int tty = isatty(STDERR_FILENO);
    do {
        seq = huff_job_wait_event(g_job, seq);
        if (!quiet && tty) {
            int p = huff_job_percent(g_job);
            fprintf(stderr, "\r[%-30.*s] %3d%%", p * 30 / 100, "##############################", p);
            fflush(stderr);
        }
    } while (huff_job_state(g_job) == HUFF_RUNNING);
    if (!quiet && tty)
        fputc('\n', stderr);

    huff_state_t s = huff_job_state(g_job);
    char msg[320];
    huff_job_message(g_job, msg, sizeof msg);
    pthread_kill(st, SIGUSR1);
    pthread_join(st, NULL);
    huff_job_free(g_job);

    if (s == HUFF_DONE) {
        if (!quiet)
            fprintf(stderr, "%s\n", msg);
        return 0;
    }
    fprintf(stderr, "huffcli: %s\n", msg);
    return s == HUFF_CANCELED ? 130 : 1;
}
