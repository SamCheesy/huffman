#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "threadpool.h"

#include <pthread.h>
#include <stdlib.h>

typedef struct tnode {
    tp_fn fn;
    void *arg;
    struct tnode *next;
} tnode;

struct tpool {
    pthread_mutex_t mu;
    pthread_cond_t not_empty;
    pthread_cond_t idle;
    tnode *head, *tail;
    int active; /* tareas en ejecución */
    int stop;
    pthread_t *th;
    int nth;
};

static void *worker_main(void *arg)
{
    tpool_t *p = arg;
    for (;;) {
        pthread_mutex_lock(&p->mu);
        while (!p->head && !p->stop)
            pthread_cond_wait(&p->not_empty, &p->mu);
        if (!p->head) { /* stop && cola vacía */
            pthread_mutex_unlock(&p->mu);
            return NULL;
        }
        tnode *n = p->head;
        p->head = n->next;
        if (!p->head)
            p->tail = NULL;
        p->active++;
        pthread_mutex_unlock(&p->mu);

        n->fn(n->arg);
        free(n);

        pthread_mutex_lock(&p->mu);
        p->active--;
        if (!p->head && p->active == 0)
            pthread_cond_broadcast(&p->idle);
        pthread_mutex_unlock(&p->mu);
    }
}

tpool_t *tpool_create(int nthreads)
{
    if (nthreads < 1)
        nthreads = 1;
    tpool_t *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    p->th = calloc((size_t)nthreads, sizeof *p->th);
    if (!p->th) {
        free(p);
        return NULL;
    }
    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->not_empty, NULL);
    pthread_cond_init(&p->idle, NULL);
    for (int i = 0; i < nthreads; i++) {
        if (pthread_create(&p->th[i], NULL, worker_main, p) != 0)
            break;
        p->nth++;
    }
    if (p->nth == 0) {
        pthread_mutex_destroy(&p->mu);
        pthread_cond_destroy(&p->not_empty);
        pthread_cond_destroy(&p->idle);
        free(p->th);
        free(p);
        return NULL;
    }
    return p;
}

int tpool_submit(tpool_t *p, tp_fn fn, void *arg)
{
    tnode *n = malloc(sizeof *n);
    if (!n)
        return -1;
    n->fn = fn;
    n->arg = arg;
    n->next = NULL;
    pthread_mutex_lock(&p->mu);
    if (p->stop) {
        pthread_mutex_unlock(&p->mu);
        free(n);
        return -1;
    }
    if (p->tail)
        p->tail->next = n;
    else
        p->head = n;
    p->tail = n;
    pthread_cond_signal(&p->not_empty);
    pthread_mutex_unlock(&p->mu);
    return 0;
}

void tpool_wait_idle(tpool_t *p)
{
    pthread_mutex_lock(&p->mu);
    while (p->head || p->active > 0)
        pthread_cond_wait(&p->idle, &p->mu);
    pthread_mutex_unlock(&p->mu);
}

void tpool_destroy(tpool_t *p)
{
    if (!p)
        return;
    pthread_mutex_lock(&p->mu);
    p->stop = 1;
    pthread_cond_broadcast(&p->not_empty);
    pthread_mutex_unlock(&p->mu);
    for (int i = 0; i < p->nth; i++)
        pthread_join(p->th[i], NULL);
    /* Los workers drenan la cola antes de salir; por seguridad: */
    while (p->head) {
        tnode *n = p->head;
        p->head = n->next;
        free(n);
    }
    pthread_mutex_destroy(&p->mu);
    pthread_cond_destroy(&p->not_empty);
    pthread_cond_destroy(&p->idle);
    free(p->th);
    free(p);
}
