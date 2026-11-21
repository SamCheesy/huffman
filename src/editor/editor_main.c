#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "editor.h"

/**
 * ====================================================================================
 * BINARIO AUTONOMO DEL EDITOR
 * ====================================================================================
 * Este main() existe unicamente para envolver al motor (editor.c) y permitir
 * ejecutarlo fuera del shell:
 *
 *     ./editor archivo.txt      -> abre el archivo con traza de syscalls
 *     ./editor -q archivo.txt   -> modo silencioso, sin traza (util para el script
 *                                  de pruebas, donde la traza ensuciaria la salida)
 *     ./editor                  -> arranca sin archivo abierto
 *     ./editor -j 4 -b 256 f    -> tareas de compresion con 4 hilos y bloques de 256 KiB
 *                                  (por defecto: un hilo por CPU y bloques de 1 MiB)
 *
 * Tener este binario permite ademas demostrar en la sustentacion la estrategia de
 * integracion alternativa: lanzarlo desde el propio shell con
 *
 *     eafitOS> p_exec ./editor archivo.txt
 *
 * que usa fork(2) + execvp(3) y ejecuta el editor como proceso independiente.
 */
int main(int argc, char **argv)
{
    int         trace = 1;
    const char *path  = NULL;
    int         workers = 0;
    size_t      block = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-q") == 0) {
            trace = 0;
        } else if (strcmp(argv[i], "-j") == 0 && i + 1 < argc) {
            workers = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            block = (size_t)atol(argv[++i]) * 1024;
        } else if (path == NULL) {
            path = argv[i];
        } else {
            fprintf(stderr, "Uso: %s [-q] [-j hilos] [-b KiB] [archivo]\n", argv[0]);
            return 1;
        }
    }

    editor_set_huff_options(workers, block);

    /* El REPL libera todos sus recursos antes de retornar, asi que no queda
     * nada por limpiar en este nivel. */
    return editor_repl(path, trace);
}
