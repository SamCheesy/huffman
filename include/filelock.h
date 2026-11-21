/*
 * filelock.h - Registro de bloqueos de archivo dentro del proceso
 * (lectores-escritor por ruta canónica), protegido por un mutex.
 *
 * Reglas:   READ  se concede si no hay escritor.
 *           WRITE se concede si no hay escritor ni lectores.
 * Nunca bloquea: devuelve -1 si está ocupado (la UI nunca se congela).
 */
#ifndef FILELOCK_H
#define FILELOCK_H

typedef enum { FL_READ = 0, FL_WRITE = 1 } fl_mode_t;

int fl_acquire(const char *path, fl_mode_t mode);      /* 0 ok, -1 ocupado */
void fl_release(const char *path, fl_mode_t mode);
int fl_is_locked(const char *path, fl_mode_t mode);    /* FL_READ: hay lectores; FL_WRITE: hay escritor */

#endif
