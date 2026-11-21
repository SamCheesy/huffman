#ifndef EDITOR_H
#define EDITOR_H

#include <sys/types.h>  /* off_t, ssize_t */
#include <stddef.h>     /* size_t         */
#include "huffman.h"  /* huff_job_t: tareas de compresion en segundo plano */

/**
 * ====================================================================================
 * MOTOR DEL EDITOR DE TEXTO CLI  (Universidad EAFIT - Sistemas Operativos)
 * ====================================================================================
 *
 * DECISION DE DISENO 1: INDEPENDENCIA DEL SHELL
 * ---------------------------------------------
 * Este modulo NO incluye "shell.h" ni depende de ninguna estructura del shell
 * educativo. Gracias a ese bajo acoplamiento el mismo codigo objeto (editor.o)
 * se enlaza en dos binarios distintos:
 *
 *   1. ./editor          -> binario autonomo (main en editor_main.c)
 *   2. ./eafitOS         -> comando 'edit' del shell (adaptador en cat_edicion.c)
 *
 * Esto permite demostrar en la sustentacion las dos estrategias de integracion
 * (comando interno del shell vs. proceso externo lanzado con fork+execvp) sin
 * escribir una sola linea de codigo adicional.
 *
 * DECISION DE DISENO 2: EL DISCO ES LA UNICA FUENTE DE VERDAD
 * -----------------------------------------------------------
 * A diferencia de vi (que carga todo el archivo en RAM y vuelca al guardar),
 * aqui el archivo en disco es el estado real. En memoria solo se mantiene un
 * INDICE DE LINEAS: un arreglo dinamico de desplazamientos (offsets) que dice
 * donde empieza y donde termina cada linea.
 *
 *   Ventajas : consumo de RAM independiente del tamano del archivo;
 *              imposible que la pantalla y el disco diverjan;
 *              obliga a dominar lseek(2), que es el objetivo del taller.
 *   Costos   : no hay "deshacer" (toda escritura es inmediata, write-through)
 *              y cada modificacion exige reconstruir el indice, que es O(n).
 *
 * RESTRICCION DE E/S RESPETADA
 * ----------------------------
 * Ninguna operacion sobre el archivo de texto usa stdio.h. Todo pasa por
 * open(2), read(2), write(2), lseek(2), ftruncate(2) y close(2).
 * printf/fgets se usan UNICAMENTE para el dialogo con el usuario por consola,
 * que es lo que el enunciado permite de forma explicita.
 */

/**
 * Descriptor de una linea dentro del archivo.
 * 'end' apunta al byte SIGUIENTE al '\n' que cierra la linea. Si la ultima
 * linea del archivo no termina en '\n', 'end' vale exactamente el tamano del
 * archivo. Guardar el '\n' dentro del rango [start, end) simplifica el borrado:
 * eliminar la linea n es simplemente eliminar ese rango de bytes.
 */
typedef struct {
    off_t start;  /* desplazamiento del primer byte de la linea */
    off_t end;    /* desplazamiento siguiente al ultimo byte (incluye el '\n') */
} Line;

/* Maximo de tareas simultaneas en segundo plano. */
#define ED_MAX_JOBS 4

/* Una tarea en segundo plano (compresion o descompresion) lanzada desde el editor. */
typedef struct {
    huff_job_t *job;        /* NULL = ranura libre                                  */
    huff_op_t   op;         /* HUFF_COMPRESS / HUFF_DECOMPRESS                      */
    char       *in;         /* ruta de origen  (bloqueo de LECTURA mientras corre)  */
    char       *out;        /* ruta de destino (bloqueo de ESCRITURA mientras corre)*/
    unsigned    id;         /* numero visible para el usuario (#id)                 */
    int         last_decile;/* ultimo avance informado (0..10), para no inundar     */
} EdJob;

/**
 * Estado completo del editor. Se pasa por puntero a todas las operaciones,
 * de modo que no existe una sola variable global en el modulo.
 */
typedef struct {
    int     fd;      /* descriptor del archivo abierto, o -1 si no hay ninguno */
    char   *path;    /* copia dinamica del nombre del archivo  -> free()       */
    off_t   size;    /* tamano actual del archivo en bytes                     */
    Line   *lines;   /* arreglo dinamico con el indice de lineas -> free()     */
    size_t  nlines;  /* cantidad de lineas realmente indexadas                 */
    size_t  cap;     /* capacidad reservada del arreglo (crece al doble)       */
    int     trace;   /* 1 = imprimir la traza de syscalls estilo strace        */

    /* Portapapeles secuencial local (comandos 'y'/'x', reto de equipo de 3).
     * Arreglo dinamico de lineas de texto copiadas con 'y', en el orden en
     * que se copiaron. Cada entrada es una cadena propia en el heap. */
    char  **clip;     /* arreglo dinamico de lineas copiadas -> free() c/u y el arreglo */
    size_t  nclip;    /* cantidad de lineas actualmente en el portapapeles      */
    size_t  clip_cap; /* capacidad reservada del arreglo (crece al doble)       */

    /* Tareas de compresion/descompresion en segundo plano (Parcial 2).
     * TODO este estado lo toca UNICAMENTE el hilo del REPL; los hilos de cada
     * tarea solo trabajan sobre archivos y se comunican con el REPL por 'evpipe'. */
    EdJob    jobs[ED_MAX_JOBS];
    unsigned job_seq;      /* contador para numerar las tareas (#1, #2, ...)     */
    int      evpipe[2];    /* pipe de eventos: los hilos escriben, el REPL lee   */
} Editor;

/* ------------------------------------------------------------------------- */
/* Ciclo interactivo completo (REPL). 'path' puede ser NULL para arrancar sin */
/* archivo abierto. Retorna 0 si termino correctamente, 1 si hubo un error    */
/* que impidio iniciar. Libera todos sus recursos antes de retornar.          */
/* ------------------------------------------------------------------------- */
int editor_repl(const char *path, int trace);

/* ------------------------------------------------------------------------- */
/* API individual de cada operacion. Se expone para poder probar el motor     */
/* desde codigo externo sin pasar por el REPL. Todas retornan 0 en exito y    */
/* -1 en error (reportando la causa con perror).                             */
/* ------------------------------------------------------------------------- */
void editor_init(Editor *ed, int trace);          /* deja el estado en limpio        */
int  editor_open(Editor *ed, const char *path);   /* comando 'o'                     */
int  editor_print(Editor *ed, long n);            /* comando 'p' (n<=0 => todo)      */
int editor_search(Editor *ed, char *word);
int  editor_append(Editor *ed, const char *text); /* comando 'a'                     */
int editor_insert(Editor *ed, long n, const char *text);   /* comando 'i'                     */
int  editor_delete(Editor *ed, long n);           /* comando 'd'                     */
int  editor_yank(Editor *ed, long n);             /* comando 'y' (copia la linea n)  */
int  editor_paste(Editor *ed, long n);            /* comando 'x' (pega antes de n)   */
void editor_close(Editor *ed);                    /* comando 'q' (idempotente)       */

/* --- Parcial 2: compresion Huffman concurrente en segundo plano ---------------- */
/* Opciones globales de las tareas: nº de hilos trabajadores (0 = uno por CPU) y
 * tamano de bloque en bytes (0 = 1 MiB). Se usan desde las banderas -j / -b. */
void editor_set_huff_options(int workers, size_t block_size);
/* Todas retornan de inmediato (nunca esperan a que termine la tarea).            */
int  editor_compress(Editor *ed, const char *out);                 /* comando 'z' */
int  editor_decompress(Editor *ed, const char *in, const char *out); /* comando 'u' */
void editor_jobs_list(Editor *ed);                                 /* comando 'j' */
int  editor_jobs_cancel(Editor *ed, unsigned id);                  /* comando 'c' (0 = todas) */

#endif /* EDITOR_H */
