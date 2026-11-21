/*
 * threadpool.h - Pool de hilos trabajadores con cola FIFO sincronizada.
 *
 * Sincronización: un mutex protege la cola; dos variables de condición:
 *   not_empty : los workers duermen aquí cuando no hay tareas (sin espera activa)
 *   idle      : quien espera "todo terminado" duerme aquí
 */
#ifndef THREADPOOL_H
#define THREADPOOL_H

typedef struct tpool tpool_t;
typedef void (*tp_fn)(void *arg);

/* Crea un pool con n hilos (>=1). Devuelve NULL si no se pudo crear ninguno. */
tpool_t *tpool_create(int nthreads);

/* Encola una tarea. 0 = ok, -1 = error (sin memoria o pool cerrándose). */
int tpool_submit(tpool_t *p, tp_fn fn, void *arg);

/* Bloquea (dormido, sin busy-wait) hasta que la cola esté vacía y no haya
 * tareas en ejecución. */
void tpool_wait_idle(tpool_t *p);

/* Termina las tareas pendientes, hace join de todos los hilos y libera. */
void tpool_destroy(tpool_t *p);

#endif
