/* strdup() y otras extensiones requieren esta macro. Se protege con #ifndef
 * porque el Makefile ya la define con -D_GNU_SOURCE; así el archivo compila
 * igual de bien si alguien lo compila a mano sin esa bandera. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "editor.h"

#include <stdio.h>      /* printf, fgets, snprintf: SOLO para dialogo por consola */
#include <stdlib.h>     /* malloc, realloc, free, strtol                          */
#include <string.h>     /* strlen, memcpy, strcmp                                 */
#include <errno.h>      /* errno                                                  */
#include <fcntl.h>      /* open, O_RDWR, O_CREAT                                  */
#include <unistd.h>     /* read, write, lseek, ftruncate, close, STDOUT_FILENO    */
#include <sys/stat.h>   /* fstat, struct stat: metadatos del inodo (comando 'm')  */
#include <time.h>       /* localtime_r, strftime: formatear st_mtime              */
#include <poll.h>       /* poll: esperar teclado + eventos + senales sin espera activa */
#include <signal.h>     /* sigaction, sigprocmask: manejo de SIGINT/SIGTERM/SIGHUP */
#include <pthread.h>    /* pthread_sigmask                                         */
#include <stdint.h>     /* intptr_t                                                */
#include <limits.h>     /* PATH_MAX                                                */

#include "filelock.h"   /* registro de bloqueos lectores/escritor por archivo      */

/* ==========================================================================
 * Colores ANSI.
 * Se redefinen aqui (en vez de incluir shell.h) para preservar la
 * independencia del motor respecto del shell. Es una duplicacion consciente:
 * el precio de mantener el modulo reutilizable.
 * ========================================================================== */
#define C_RESET   "\033[0m"
#define C_PROMPT  "\033[1;36m"   /* cian    */
#define C_SYSCALL "\033[1;35m"   /* magenta */
#define C_OK      "\033[1;32m"   /* verde   */
#define C_ERR     "\033[1;31m"   /* rojo    */
#define C_INFO    "\033[0;90m"   /* gris    */
#define C_TITLE   "\033[1;97m"   /* blanco  */

/* Tamano del bufer de trabajo. 4096 bytes coincide con el tamano de pagina y
 * con el tamano de bloque tipico de los sistemas de archivos de Linux, asi que
 * es el punto donde el costo por syscall se amortiza mejor. */
#define BUFSZ    4096
#define MAX_LINE 4096

/* ==========================================================================
 * Macros de trazado (estilo strace simplificado).
 * TRACE imprime la llamada SIN salto de linea; TRACE_OK o TRACE_ERR la cierran.
 * Entre ambas no puede imprimirse nada mas, o la linea quedaria partida.
 * ========================================================================== */
#define TRACE(ed, name, fmt, ...)                                             \
    do { if ((ed)->trace)                                                     \
        printf(C_SYSCALL "[syscall] " name C_RESET "(" fmt ") ... ",          \
               ##__VA_ARGS__); } while (0)

#define TRACE_OK(ed, res)                                                     \
    do { if ((ed)->trace)                                                     \
        printf("= " C_OK "%lld" C_RESET "\n", (long long)(res)); } while (0)

#define TRACE_ERR(ed)                                                         \
    do { if ((ed)->trace)                                                     \
        printf("= " C_ERR "-1 (%s)" C_RESET "\n", strerror(errno)); } while (0)

/* ==========================================================================
 * SECCION 1: ENVOLTORIOS ROBUSTOS DE LAS SYSCALLS
 * --------------------------------------------------------------------------
 * read(2) y write(2) pueden transferir MENOS bytes de los solicitados sin que
 * eso constituya un error, y pueden ser interrumpidas por una senal retornando
 * -1 con errno == EINTR. Ignorar estos dos hechos es el error mas frecuente en
 * codigo que trabaja con descriptores de archivo. Estos envoltorios lo manejan
 * de forma centralizada para que el resto del modulo quede legible.
 *
 * Nota sobre errno: printf podria modificarlo, asi que se guarda y se restaura
 * antes de retornar, para que quien llame pueda usar perror() con seguridad.
 * ========================================================================== */

/* Lee hasta 'n' bytes. Retorna los bytes leidos (< n solo si llego a EOF), -1 error. */
static ssize_t ed_read(Editor *ed, void *buf, size_t n)
{
    size_t total = 0;

    while (total < n) {
        TRACE(ed, "read", "%d, buf+%zu, %zu", ed->fd, total, n - total);
        ssize_t r = read(ed->fd, (char *)buf + total, n - total);

        if (r == -1) {
            if (errno == EINTR) {                    /* interrumpida por senal */
                if (ed->trace) printf("= " C_ERR "EINTR" C_RESET " (reintentando)\n");
                continue;
            }
            int saved = errno;
            TRACE_ERR(ed);
            errno = saved;
            return -1;
        }
        TRACE_OK(ed, r);
        if (r == 0) break;                            /* fin de archivo (EOF) */
        total += (size_t)r;
    }
    return (ssize_t)total;
}

/* Escribe exactamente 'n' bytes en el archivo. Retorna 0 en exito, -1 en error. */
static int ed_write(Editor *ed, const void *buf, size_t n)
{
    size_t total = 0;

    while (total < n) {
        TRACE(ed, "write", "%d, buf+%zu, %zu", ed->fd, total, n - total);
        ssize_t w = write(ed->fd, (const char *)buf + total, n - total);

        if (w == -1) {
            if (errno == EINTR) {
                if (ed->trace) printf("= " C_ERR "EINTR" C_RESET " (reintentando)\n");
                continue;
            }
            int saved = errno;
            TRACE_ERR(ed);
            errno = saved;
            return -1;
        }
        TRACE_OK(ed, w);
        total += (size_t)w;                    /* puede haber escrito parcial */
    }
    return 0;
}

/* Reposiciona el puntero del archivo. Retorna el nuevo desplazamiento o -1. */
static off_t ed_lseek(Editor *ed, off_t offset, int whence, const char *wname)
{
    TRACE(ed, "lseek", "%d, %lld, %s", ed->fd, (long long)offset, wname);
    off_t pos = lseek(ed->fd, offset, whence);

    if (pos == (off_t)-1) {
        int saved = errno;
        TRACE_ERR(ed);
        errno = saved;
        return (off_t)-1;
    }
    TRACE_OK(ed, pos);
    return pos;
}

/**
 * Escritura hacia la salida estandar (descriptor 1) usando write(2).
 *
 * Se usa write() y no printf("%s") por una razon tecnica concreta: printf con
 * %s se detiene en el primer byte nulo, mientras que write() es seguro a nivel
 * de bytes y por lo tanto funciona incluso si el archivo contiene datos
 * binarios o bytes '\0' intercalados.
 *
 * El fflush(stdout) previo es OBLIGATORIO: printf acumula en un bufer de
 * espacio de usuario mientras que write() va directo al kernel. Sin el vaciado
 * el texto saldria desordenado, sobre todo cuando la salida es una tuberia.
 *
 * La traza se imprime DESPUES de la escritura porque el contenido del archivo
 * partiria la linea "[syscall] write(1, ...) ... = N" por la mitad.
 */
static int out_write(Editor *ed, const void *buf, size_t n)
{
    size_t total = 0;

    fflush(stdout);
    while (total < n) {
        ssize_t w = write(STDOUT_FILENO, (const char *)buf + total, n - total);
        if (w == -1) {
            if (errno == EINTR) continue;
            perror("editor: write(STDOUT)");
            return -1;
        }
        total += (size_t)w;
    }
    if (ed->trace)
        printf(C_SYSCALL "[syscall] write" C_RESET "(1, buf, %zu) ... = "
               C_OK "%zu" C_RESET "\n", n, total);
    return 0;
}

/* ==========================================================================
 * SECCION 2: INDICE DE LINEAS EN MEMORIA DINAMICA
 * ========================================================================== */

/* Agrega una linea al indice, duplicando la capacidad cuando se agota.
 * Duplicar (en lugar de crecer de uno en uno) amortiza el costo de realloc. */
static int index_push(Editor *ed, off_t start, off_t end)
{
    if (ed->nlines == ed->cap) {
        size_t ncap = (ed->cap == 0) ? 16 : ed->cap * 2;
        Line  *tmp  = realloc(ed->lines, ncap * sizeof(Line));

        /* Se asigna a una variable temporal: si realloc falla retorna NULL y
         * asignarlo directamente a ed->lines perderia el bloque original. */
        if (tmp == NULL) {
            perror("editor: realloc del indice de lineas");
            return -1;
        }
        ed->lines = tmp;
        ed->cap   = ncap;
    }
    ed->lines[ed->nlines].start = start;
    ed->lines[ed->nlines].end   = end;
    ed->nlines++;
    return 0;
}

/**
 * Reconstruye el indice recorriendo el archivo completo en bloques de 4 KB y
 * anotando la posicion de cada '\n'. Tambien recalcula el tamano real.
 * Se invoca al abrir y despues de cada modificacion.
 */
static int index_build(Editor *ed)
{
    char  buf[BUFSZ];
    off_t pos = 0, line_start = 0;

    ed->nlines = 0;   /* se reutiliza la capacidad ya reservada, no se libera */

    if (ed_lseek(ed, 0, SEEK_SET, "SEEK_SET") == (off_t)-1) {
        perror("editor: lseek al indexar");
        return -1;
    }

    for (;;) {
        ssize_t r = ed_read(ed, buf, sizeof buf);
        if (r == -1) { perror("editor: read al indexar"); return -1; }
        if (r == 0)  break;                                        /* EOF */

        for (ssize_t i = 0; i < r; i++) {
            pos++;                       /* pos = desplazamiento tras el byte */
            if (buf[i] == '\n') {
                if (index_push(ed, line_start, pos) == -1) return -1;
                line_start = pos;
            }
        }

        /* ed_read solo devuelve menos de lo pedido cuando alcanzo EOF, asi que
         * no hace falta una vuelta extra para confirmarlo: una syscall menos. */
        if ((size_t)r < sizeof buf) break;
    }

    /* Caso borde: el archivo termina sin '\n'. Esos bytes finales tambien son
     * una linea valida, aunque incompleta. */
    if (pos > line_start && index_push(ed, line_start, pos) == -1)
        return -1;

    ed->size = pos;
    return 0;
}

/* Verifica que haya un archivo abierto antes de operar. */
static int require_open(const Editor *ed)
{
    if (ed->fd == -1) {
        fprintf(stderr, C_ERR "editor: no hay ningun archivo abierto. "
                        "Usa 'o <archivo>' primero.\n" C_RESET);
        return 0;
    }
    return 1;
}

/**
 * Libera unicamente los recursos ligados al ARCHIVO actualmente abierto (el
 * descriptor, el nombre y el indice de lineas). Deliberadamente NO toca el
 * portapapeles ('clip'): lo usan tanto 'editor_close' (comando 'q', donde si
 * se vacia el portapapeles) como 'editor_open' al reemplazar un archivo por
 * otro. Si 'editor_open' vaciara el portapapeles, seria imposible copiar una
 * linea de un archivo, abrir uno distinto con 'o' y pegarla alli con 'x' -
 * justo el caso de uso mas natural de un portapapeles. Idempotente, igual
 * que editor_close.
 */
static void close_file_resources(Editor *ed)
{
    if (ed->fd != -1) {
        TRACE(ed, "close", "%d", ed->fd);
        if (close(ed->fd) == -1) {
            int saved = errno;
            TRACE_ERR(ed);
            errno = saved;
            perror("editor: close");
        } else {
            TRACE_OK(ed, 0);
        }
        ed->fd = -1;
    }

    free(ed->path);            /* free(NULL) es seguro segun el estandar C */
    free(ed->lines);
    ed->path   = NULL;
    ed->lines  = NULL;
    ed->nlines = 0;
    ed->cap    = 0;
    ed->size   = 0;
}

/* ==========================================================================
 * SECCION 3: OPERACIONES DEL EDITOR
 * ========================================================================== */

void editor_init(Editor *ed, int trace)
{
    ed->fd       = -1;
    ed->path     = NULL;
    ed->size     = 0;
    ed->lines    = NULL;
    ed->nlines   = 0;
    ed->cap      = 0;
    ed->trace    = trace;
    ed->clip     = NULL;
    ed->nclip    = 0;
    ed->clip_cap = 0;
    memset(ed->jobs, 0, sizeof ed->jobs);
    ed->job_seq  = 0;
    ed->evpipe[0] = ed->evpipe[1] = -1;
}

/**
 * COMANDO 'o <archivo>' -- abrir o crear.
 *
 * Syscalls: open(2), lseek(2), read(2) [al indexar], close(2) [si ya habia uno].
 *
 * Detalle critico: se usa O_RDWR | O_CREAT pero NUNCA O_TRUNC. Truncar al abrir
 * destruiria el contenido del usuario, que es justo lo contrario de lo que un
 * editor debe hacer. El modo 0644 (-rw-r--r--) solo se aplica si el archivo se
 * crea; si ya existe, sus permisos no se tocan.
 */
int editor_open(Editor *ed, const char *path)
{
    char *copy = strdup(path);     /* memoria dinamica: se libera en editor_close */
    if (copy == NULL) {
        perror("editor: strdup del nombre de archivo");
        return -1;
    }

    /* CORRECCION respecto al Parcial 1: se abre el archivo NUEVO antes de soltar el
     * actual. Antes se cerraba primero, de modo que un 'o' fallido (ruta inexistente,
     * sin permisos) dejaba al usuario sin ningun archivo abierto. Ahora, si 'open'
     * o el indexado fallan, el editor conserva intacto el archivo anterior. */
    TRACE(ed, "open", "\"%s\", O_RDWR|O_CREAT, 0644", path);
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd == -1) {
        int saved = errno;
        TRACE_ERR(ed);
        errno = saved;
        perror("editor: open");
        free(copy);
        return -1;
    }
    TRACE_OK(ed, fd);

    /* Se aparta el estado del archivo actual y se instala el nuevo. */
    Editor old = *ed;
    ed->fd = fd;  ed->path = copy;
    ed->lines = NULL;  ed->nlines = 0;  ed->cap = 0;  ed->size = 0;

    if (index_build(ed) == -1) {   /* si el indexado falla, no dejamos el fd abierto */
        close_file_resources(ed);  /* libera SOLO lo del archivo nuevo */
        ed->fd = old.fd;  ed->path = old.path;
        ed->lines = old.lines;  ed->nlines = old.nlines;  ed->cap = old.cap;  ed->size = old.size;
        return -1;
    }

    /* El nuevo esta listo: ahora si se cierra el anterior (cada 'o' liberaria un fd
     * huerfano si se omitiera esto). 'old' conserva el descriptor, el nombre y el indice. */
    if (old.fd != -1) {
        printf(C_INFO "Cerrando el archivo anterior ('%s')...\n" C_RESET, old.path);
        close_file_resources(&old);
    }

    printf(C_OK "Archivo '%s' abierto." C_RESET
           C_INFO "  fd=%d  tamano=%lld bytes  lineas=%zu\n" C_RESET,
           ed->path, ed->fd, (long long)ed->size, ed->nlines);
    return 0;
}

/**
 * Imprime una linea del indice, precedida de su numero.
 *
 * El numero y el contenido se ensamblan en un UNICO bufer dinamico para
 * emitirlos con una sola llamada a write(2). Hacerlo en dos llamadas separadas
 * funcionaria igual, pero partiria visualmente la salida cuando la traza de
 * syscalls esta activa.
 */
static int print_line(Editor *ed, size_t idx)
{
    off_t  start = ed->lines[idx].start;
    size_t len   = (size_t)(ed->lines[idx].end - start);
    char   prefix[32];

    int plen = snprintf(prefix, sizeof prefix, "%6zu  ", idx + 1);

    /* +1 por si hay que agregar un '\n' que el archivo no tiene */
    char *buf = malloc((size_t)plen + len + 1);
    if (buf == NULL) { perror("editor: malloc"); return -1; }

    memcpy(buf, prefix, (size_t)plen);

    if (ed_lseek(ed, start, SEEK_SET, "SEEK_SET") == (off_t)-1) {
        perror("editor: lseek al imprimir");
        free(buf);
        return -1;
        
    }
    ssize_t r = ed_read(ed, buf + plen, len);
    if (r == -1) {
        perror("editor: read al imprimir");
        free(buf);
        return -1;
    }

    size_t total = (size_t)plen + (size_t)r;

    /* Si la ultima linea del archivo no trae '\n', se agrega SOLO en pantalla
     * para que el prompt no quede pegado al texto. El archivo no se modifica. */
    if (r == 0 || buf[total - 1] != '\n')
        buf[total++] = '\n';

    out_write(ed, buf, total);

    free(buf);
    return 0;
}

/**
 * COMANDO 'p [n]' -- imprimir.
 *
 * Syscalls: lseek(2), read(2), write(2) sobre el descriptor 1.
 * Con n <= 0 imprime el archivo completo recorriendo el indice de lineas.
 */
int editor_print(Editor *ed, long n)
{
    if (!require_open(ed)) return -1;

    if (ed->nlines == 0) {
        printf(C_INFO "(el archivo esta vacio)\n" C_RESET);
        return 0;
    }

    if (n > 0) {
        if ((size_t)n > ed->nlines) {
            fprintf(stderr, C_ERR "editor: la linea %ld no existe "
                            "(el archivo tiene %zu lineas).\n" C_RESET, n, ed->nlines);
            return -1;
        }
        return print_line(ed, (size_t)n - 1);
    }

    for (size_t i = 0; i < ed->nlines; i++)
        if (print_line(ed, i) == -1) return -1;

    return 0;
}

int editor_search(Editor *ed, char *word)
{
    if (!require_open(ed)) return -1;

    if (ed->nlines == 0) {
        printf(C_INFO "(el archivo esta vacio)\n" C_RESET);
        return 0;
    }

    for (size_t i = 0; i < ed->nlines; i++) {

        off_t start = ed->lines[i].start;
        off_t end   = ed->lines[i].end;

        size_t len = (size_t)(end - start);

        char *buf = malloc(len + 1);
        if (buf == NULL) {
            perror("editor: malloc al buscar");
            return -1;
        }

        if (ed_lseek(ed, start, SEEK_SET, "SEEK_SET") == (off_t)-1) {
            perror("editor: lseek al buscar");
            free(buf);
            return -1;
        }

        ssize_t r = ed_read(ed, buf, len);
        if (r == -1) {
            perror("editor: read al buscar");
            free(buf);
            return -1;
        }

        buf[r] = '\0';

        /* memmem (GNU) en vez de strstr: strstr se detiene en el primer byte NUL y no
         * encontraba coincidencias que aparecen despues de el en archivos con datos
         * binarios. */
        if (memmem(buf, (size_t)r, word, strlen(word)) != NULL) {
            if (print_line(ed, i) == -1) {
                free(buf);
                return -1;
            }
        }

        free(buf);
    }

    return 0;
}


/**
 * COMANDO 'a <texto>' -- anadir una linea al final.
 *
 * Syscalls: lseek(2) con SEEK_END, read(2), write(2).
 *
 * Se resuelven dos detalles que suelen pasarse por alto:
 *
 * 1) Si el archivo no esta vacio y su ultimo byte NO es '\n', hay que insertar
 *    uno antes del texto nuevo; de lo contrario la linea anadida quedaria
 *    pegada a la anterior. Para averiguarlo se lee ese ultimo byte con
 *    lseek(fd, -1, SEEK_END) + read().
 *
 * 2) El salto previo, el texto y el '\n' final se arman en un unico bufer de
 *    memoria dinamica y se emiten con UNA sola llamada a write(). Menos
 *    cambios de contexto a modo kernel y menos riesgo de dejar el archivo a
 *    medio escribir si algo falla en el intermedio.
 *
 * Alternativa considerada: abrir con O_APPEND haria innecesario el lseek final,
 * pero se prefirio el lseek explicito porque hace visible el concepto de
 * puntero de archivo, que es el objetivo pedagogico del taller.
 */
static int append_nolock(Editor *ed, const char *text)
{
    if (!require_open(ed)) return -1;

    size_t tlen    = strlen(text);
    int    need_nl = 0;

    if (ed->size > 0) {
        char last;
        if (ed_lseek(ed, -1, SEEK_END, "SEEK_END") == (off_t)-1) {
            perror("editor: lseek al inspeccionar el ultimo byte");
            return -1;
        }
        if (ed_read(ed, &last, 1) == -1) {
            perror("editor: read del ultimo byte");
            return -1;
        }
        if (last != '\n') need_nl = 1;
    }

    size_t blen = (size_t)need_nl + tlen + 1;   /* ['\n'] + texto + '\n' */
    char  *buf  = malloc(blen);
    if (buf == NULL) { perror("editor: malloc"); return -1; }

    size_t off = 0;
    if (need_nl) buf[off++] = '\n';
    memcpy(buf + off, text, tlen);
    off += tlen;
    buf[off] = '\n';

    if (ed_lseek(ed, 0, SEEK_END, "SEEK_END") == (off_t)-1) {
        perror("editor: lseek al final del archivo");
        free(buf);
        return -1;
    }
    if (ed_write(ed, buf, blen) == -1) {
        perror("editor: write al anadir");
        free(buf);
        return -1;
    }
    free(buf);                                  /* sin fugas: se libera siempre */

    if (index_build(ed) == -1) return -1;

    printf(C_OK "Linea %zu anadida" C_RESET C_INFO " (%zu bytes escritos, "
           "tamano actual %lld bytes)\n" C_RESET,
           ed->nlines, blen, (long long)ed->size);
    return 0;
}


/**
 * COMANDO 'i <n> <texto>' -- anadir una linea en la posicion n.
 *
 * Syscalls: lseek(2) con SEEK_END, read(2), write(2).
 *.
 */
static int insert_nolock(Editor *ed, long n, const char *text)
{
    if (!require_open(ed)) return -1;

    /* Si el archivo está vacío o se inserta tras la última línea, es lo mismo que el append */
    if (ed->nlines == 0 || (size_t)n > ed->nlines) {
        return append_nolock(ed, text);
    }

    size_t tlen    = strlen(text);
    size_t ins_len = tlen + 1; /* texto + '\n' */

    off_t target_pos = ed->lines[n-1].start;

    /*Construimos  el bufer que se va a insertar*/
    char *ins_buf = malloc(ins_len);
    if(ins_buf == NULL){
        perror("editor: malloc al insertar"); return -1;
    }
    memcpy(ins_buf, text, tlen);
    ins_buf[tlen] = '\n';

    /* Desplazamos la cola del archivo hacia la derecha*/
    off_t bytes_to_move = ed->size - target_pos;
    char buf[BUFSZ];

    while (bytes_to_move > 0) {
        size_t chunk = (bytes_to_move < (off_t)sizeof(buf)) 
                       ? (size_t)bytes_to_move : sizeof(buf);

        off_t read_pos = target_pos + bytes_to_move - chunk;
        off_t write_pos = read_pos + ins_len;

        if (ed_lseek(ed, read_pos, SEEK_SET, "SEEK_SET") == (off_t)-1) {
            perror("editor: lseek de lectura al insertar");
            free(ins_buf);
            return -1;
        }
        ssize_t r = ed_read(ed, buf, chunk);
        if (r == -1) { perror("editor: read al insertar"); free(ins_buf); return -1; }

        if (ed_lseek(ed, write_pos, SEEK_SET, "SEEK_SET") == (off_t)-1) {
            perror("editor: lseek de escritura al insertar");
            free(ins_buf);
            return -1;
        }
        if (ed_write(ed, buf, (size_t)r) == -1) {
            perror("editor: write al insertar");
            free(ins_buf);
            return -1;
        }

        bytes_to_move -= chunk;
    }


    /*Escribimos la nueva linea en la posicion que liberamos*/
    if (ed_lseek(ed, target_pos, SEEK_SET, "SEEK_SET") == (off_t)-1) {
        perror("editor: lseek al escribir nueva linea");
        free(ins_buf);
        return -1;
    }

    if (ed_write(ed, ins_buf, ins_len) == -1) {
        perror("editor: write nueva linea");
        free(ins_buf);
        return -1;
    }

    free(ins_buf);

    if (index_build(ed) == -1) return -1;

    printf(C_OK "Linea insertada en la posicion %ld" C_RESET C_INFO " (%zu bytes escritos, "
           "tamano actual %lld bytes)\n" C_RESET,
           n, ins_len, (long long)ed->size);

    return 0;

}
/**
 * COMANDO 'd <n>' -- borrar la linea n.
 *
 * Syscalls: lseek(2), read(2), write(2), ftruncate(2).
 *
 * Los sistemas de archivos POSIX no permiten "sacar" bytes del medio de un
 * archivo: no existe una operacion de eliminacion en el interior. El
 * procedimiento correcto es de dos fases:
 *
 *   FASE 1 (desplazamiento): copiar toda la cola del archivo 'gap' bytes hacia
 *   atras, usando dos punteros logicos (lectura y escritura) que avanzan en
 *   paralelo bloque a bloque. Como solo hay un descriptor, cada iteracion
 *   necesita dos lseek: uno para leer y otro para escribir.
 *
 *   FASE 2 (recorte): ftruncate() elimina los 'gap' bytes sobrantes que
 *   quedaron duplicados al final.
 *
 * Caso borde: al borrar la ultima linea no hay cola que desplazar, el bucle
 * simplemente no se ejecuta y basta con el ftruncate.
 *
 * Alternativa considerada: pread(2)/pwrite(2) leen y escriben en un
 * desplazamiento dado sin mover el puntero, ahorrando la mitad de los lseek.
 * Se conservo lseek por ser el que el enunciado pide demostrar.
 */
static int delete_nolock(Editor *ed, long n)
{
    if (!require_open(ed)) return -1;

    if (ed->nlines == 0) {
        fprintf(stderr, C_ERR "editor: el archivo esta vacio, no hay nada que borrar.\n" C_RESET);
        return -1;
    }
    if (n < 1 || (size_t)n > ed->nlines) {
        fprintf(stderr, C_ERR "editor: la linea %ld no existe "
                        "(el archivo tiene %zu lineas).\n" C_RESET, n, ed->nlines);
        return -1;
    }

    off_t start = ed->lines[n - 1].start;
    off_t end   = ed->lines[n - 1].end;
    off_t gap   = end - start;             /* bytes que van a desaparecer */
    char  buf[BUFSZ];

    /* --- FASE 1: desplazar la cola hacia atras --- */
    off_t rp = end;      /* desde donde se lee   */
    off_t wp = start;    /* hacia donde se escribe */

    while (rp < ed->size) {
        off_t  remaining = ed->size - rp;
        size_t want = (remaining < (off_t)sizeof buf)
                        ? (size_t)remaining : sizeof buf;

        if (ed_lseek(ed, rp, SEEK_SET, "SEEK_SET") == (off_t)-1) {
            perror("editor: lseek de lectura al borrar");
            return -1;
        }
        ssize_t r = ed_read(ed, buf, want);
        if (r == -1) { perror("editor: read al borrar"); return -1; }
        if (r == 0)  break;

        if (ed_lseek(ed, wp, SEEK_SET, "SEEK_SET") == (off_t)-1) {
            perror("editor: lseek de escritura al borrar");
            return -1;
        }
        if (ed_write(ed, buf, (size_t)r) == -1) {
            perror("editor: write al borrar");
            return -1;
        }
        rp += r;
        wp += r;
    }

    /* --- FASE 2: recortar el sobrante --- */
    off_t newsize = ed->size - gap;
    TRACE(ed, "ftruncate", "%d, %lld", ed->fd, (long long)newsize);
    if (ftruncate(ed->fd, newsize) == -1) {
        int saved = errno;
        TRACE_ERR(ed);
        errno = saved;
        perror("editor: ftruncate");
        return -1;
    }
    TRACE_OK(ed, 0);

    ed->size = newsize;
    if (index_build(ed) == -1) return -1;

    printf(C_OK "Linea %ld borrada" C_RESET C_INFO " (%lld bytes eliminados, "
           "quedan %zu lineas / %lld bytes)\n" C_RESET,
           n, (long long)gap, ed->nlines, (long long)ed->size);
    return 0;
}

/**
 * COMANDO 'y <n>' -- copiar (yank) la linea n al portapapeles.
 *
 * No introduce syscalls nuevas: reutiliza lseek(2)/read(2) exactamente igual
 * que print_line(), solo que en vez de escribir a STDOUT guarda el resultado
 * en un bufer propio del heap.
 *
 * DISENO: "portapapeles SECUENCIAL". Cada 'y' ANADE una copia al final del
 * arreglo 'clip' (crece con la misma estrategia de duplicar capacidad que el
 * indice de lineas), en vez de sobrescribir una unica ranura. Eso permite
 * encadenar varias copias -- 'y 2', 'y 5', 'y 7' -- antes de pegarlas todas
 * de una vez con 'x', conservando el orden en que se copiaron. Es la lectura
 * mas literal de "portapapeles secuencial local" del enunciado, y ademas
 * demuestra el mismo patron de buffer dinamico que ya exige 'i'.
 */
int editor_yank(Editor *ed, long n)
{
    if (!require_open(ed)) return -1;

    if (ed->nlines == 0) {
        fprintf(stderr, C_ERR "editor: el archivo esta vacio, no hay nada que copiar.\n" C_RESET);
        return -1;
    }
    if (n < 1 || (size_t)n > ed->nlines) {
        fprintf(stderr, C_ERR "editor: la linea %ld no existe "
                        "(el archivo tiene %zu lineas).\n" C_RESET, n, ed->nlines);
        return -1;
    }

    off_t  start = ed->lines[n - 1].start;
    off_t  end   = ed->lines[n - 1].end;
    size_t len   = (size_t)(end - start);

    char *buf = malloc(len + 1);
    if (buf == NULL) { perror("editor: malloc al copiar"); return -1; }

    if (ed_lseek(ed, start, SEEK_SET, "SEEK_SET") == (off_t)-1) {
        perror("editor: lseek al copiar");
        free(buf);
        return -1;
    }
    ssize_t r = ed_read(ed, buf, len);
    if (r == -1) {
        perror("editor: read al copiar");
        free(buf);
        return -1;
    }

    /* Se guarda SIN el '\n' final: 'editor_insert'/'editor_append' agregan
     * su propio salto de linea, igual que si el texto viniera de 'a'/'i'. */
    size_t rlen = (size_t)r;
    if (rlen > 0 && buf[rlen - 1] == '\n') rlen--;
    buf[rlen] = '\0';

    if (ed->nclip == ed->clip_cap) {
        size_t  ncap = (ed->clip_cap == 0) ? 8 : ed->clip_cap * 2;
        char  **tmp  = realloc(ed->clip, ncap * sizeof(char *));
        if (tmp == NULL) {
            perror("editor: realloc del portapapeles");
            free(buf);
            return -1;
        }
        ed->clip     = tmp;
        ed->clip_cap = ncap;
    }
    ed->clip[ed->nclip++] = buf;   /* 'buf' queda vivo en el portapapeles */

    printf(C_OK "Linea %ld copiada al portapapeles" C_RESET C_INFO
           " (%zu linea%s en cola)\n" C_RESET,
           n, ed->nclip, ed->nclip == 1 ? "" : "s");
    return 0;
}

/**
 * COMANDO 'x <n>' -- pegar el portapapeles antes de la linea n.
 *
 * Inserta, EN ORDEN, todas las lineas que haya en el portapapeles, delegando
 * en 'editor_insert' para cada una: se reutiliza integramente el desplazamiento
 * de bytes de la seccion anterior en vez de duplicar esa logica. Tras pegar la
 * linea i-esima en la posicion n+i, la siguiente se pega en n+i+1 para que el
 * bloque completo quede contiguo y en el mismo orden en que se copio.
 *
 * ALTERNATIVA CONSIDERADA: dejar el portapapeles intacto tras pegar (como el
 * portapapeles de un editor grafico, que persiste hasta la proxima copia). Se
 * prefirio VACIARLO al pegar con exito: el enunciado lo describe como
 * "Copiar y Pegar" -- una operacion de una sola vez, no un valor persistente
 * -- y asi 'x x' repetido por error no duplica contenido sin que el usuario
 * lo pida explicitamente. Si 'editor_insert' falla a medio camino (por un
 * error real de E/S), las lineas ya pegadas se retiran del portapapeles pero
 * las que faltan se conservan, para no perder trabajo del usuario.
 */
static int paste_nolock(Editor *ed, long n)
{
    if (!require_open(ed)) return -1;

    if (ed->nclip == 0) {
        fprintf(stderr, C_ERR "editor: el portapapeles esta vacio. "
                        "Usa 'y <n>' para copiar una linea primero.\n" C_RESET);
        return -1;
    }

    size_t total  = ed->nclip;
    size_t pasted = 0;

    for (pasted = 0; pasted < total; pasted++) {
        if (insert_nolock(ed, n + (long)pasted, ed->clip[pasted]) == -1)
            break;   /* editor_insert ya reporto la causa con perror/TRACE_ERR */
    }

    for (size_t i = 0; i < pasted; i++) free(ed->clip[i]);

    if (pasted == total) {
        ed->nclip = 0;   /* 'clip_cap' se conserva: el heap se reutiliza en la proxima 'y' */
        printf(C_OK "%zu linea%s pegada%s a partir de la linea %ld\n" C_RESET,
               pasted, pasted == 1 ? "" : "s", pasted == 1 ? "" : "s", n);
        return 0;
    }

    /* Solo se pego un prefijo: se retira del arreglo, el resto queda en cola. */
    memmove(ed->clip, ed->clip + pasted, (total - pasted) * sizeof(char *));
    ed->nclip = total - pasted;
    fprintf(stderr, C_ERR "editor: solo se pegaron %zu de %zu lineas "
                    "(quedan %zu en el portapapeles).\n" C_RESET,
                    pasted, total, ed->nclip);
    return -1;
}

/* ==========================================================================
 * SECCION 3b: BLOQUEO DE ARCHIVO ANTE MODIFICACIONES CONCURRENTES
 * --------------------------------------------------------------------------
 * El editor escribe directamente en disco (write-through) y, para insertar o
 * borrar, DESPLAZA la cola del archivo en su sitio: mientras eso ocurre el
 * archivo esta transitoriamente inconsistente. Un hilo de compresion que lo
 * leyera en ese momento comprimiria basura.
 *
 * Solucion: problema clasico de LECTORES-ESCRITOR por archivo (modulo
 * filelock.c, protegido por un mutex y NUNCA bloqueante):
 *   - una tarea de compresion toma un bloqueo de LECTURA sobre su origen
 *     durante toda su vida (varios lectores a la vez estan permitidos);
 *   - una tarea de descompresion toma uno de ESCRITURA sobre su destino;
 *   - las operaciones que modifican el archivo (a, i, d, x) toman el bloqueo
 *     de ESCRITURA mientras se ejecutan. Si no se concede, la orden se RECHAZA
 *     con un mensaje en vez de esperar: esperar congelaria la interfaz, que es
 *     justo lo que el enunciado prohibe.
 * Las lecturas (p, s, y, m) no necesitan bloqueo propio: nunca ven un estado
 * a medias porque el editor es de un solo hilo y los hilos de tarea no
 * escriben en el archivo que se esta editando (descomprimen a un temporal y
 * hacen rename atomico).
 * ========================================================================== */

static int lock_for_write(Editor *ed)
{
    if (!require_open(ed)) return -1;
    if (fl_acquire(ed->path, FL_WRITE) != 0) {
        fprintf(stderr, C_ERR "editor: '%s' esta en uso por una tarea en segundo plano "
                        "(compresion/descompresion). Espera a que termine ('j' muestra el "
                        "avance) o cancelala con 'c'.\n" C_RESET, ed->path);
        return -1;
    }
    return 0;
}

static void unlock_write(Editor *ed)
{
    fl_release(ed->path, FL_WRITE);
}

int editor_append(Editor *ed, const char *text)
{
    if (lock_for_write(ed) == -1) return -1;
    int rc = append_nolock(ed, text);
    unlock_write(ed);
    return rc;
}

int editor_insert(Editor *ed, long n, const char *text)
{
    if (lock_for_write(ed) == -1) return -1;
    int rc = insert_nolock(ed, n, text);
    unlock_write(ed);
    return rc;
}

int editor_delete(Editor *ed, long n)
{
    if (lock_for_write(ed) == -1) return -1;
    int rc = delete_nolock(ed, n);
    unlock_write(ed);
    return rc;
}

/* 'x' toma el bloqueo UNA vez para todo el pegado (insert_nolock no lo vuelve a
 * tomar): el bloque pegado se vuelve visible de forma atomica para las tareas. */
int editor_paste(Editor *ed, long n)
{
    if (lock_for_write(ed) == -1) return -1;
    int rc = paste_nolock(ed, n);
    unlock_write(ed);
    return rc;
}

/* ==========================================================================
 * SECCION 3c: TAREAS EN SEGUNDO PLANO (compresion / descompresion Huffman)
 * --------------------------------------------------------------------------
 * MODELO DE HILOS
 *   - Hilo del REPL (este): lee comandos, es DUENO exclusivo de 'Editor' (buffer
 *     de lineas, indice, portapapeles, tabla de tareas y registro de bloqueos).
 *   - Hilos de cada tarea (coordinador + N workers + escritor, ver huffman.c):
 *     solo tocan ARCHIVOS. Jamas leen ni escriben estructuras del editor.
 *   - Unico canal de los hilos hacia el REPL: 'evpipe'. Cada cambio de estado
 *     escribe 1 byte; el REPL duerme en poll() sobre {teclado, evpipe, senales}.
 *     Sin espera activa, sin temporizadores, sin que la UI se congele.
 *
 * SENALES
 *   - El handler solo hace write() a un pipe (async-signal-safe). La logica
 *     (cancelar tareas, salir) corre en el hilo del REPL, dentro de poll().
 *   - Las senales se BLOQUEAN en los hilos de las tareas (la mascara se hereda
 *     al crearlos), de modo que siempre las atiende el hilo del REPL.
 * ========================================================================== */

static int g_sigpipe[2] = {-1, -1};

/* Opciones de las tareas (ver editor_set_huff_options). */
static int    g_huff_workers = 0;
static size_t g_huff_block   = 0;

void editor_set_huff_options(int workers, size_t block_size)
{
    g_huff_workers = workers;
    g_huff_block   = block_size;
}

static void on_signal(int sig)
{
    int saved = errno;
    unsigned char c = (sig == SIGINT) ? 'I' : 'T';
    ssize_t r = write(g_sigpipe[1], &c, 1);
    (void)r;
    errno = saved;
}

/* Callback que invocan los hilos de la tarea (huffman.c). Debe ser thread-safe y
 * rapido: solo escribe un byte en un pipe NO bloqueante. Si el pipe esta lleno ya
 * hay eventos pendientes, asi que perder este byte no pierde informacion. */
static void on_job_event(void *ctx)
{
    int fd = (int)(intptr_t)ctx;
    unsigned char b = 1;
    ssize_t r = write(fd, &b, 1);
    (void)r;
}

static int jobs_init(Editor *ed)
{
    if (ed->evpipe[0] != -1) return 0;
    if (pipe2(ed->evpipe, O_NONBLOCK | O_CLOEXEC) == -1) {
        perror("editor: pipe2");
        ed->evpipe[0] = ed->evpipe[1] = -1;
        return -1;
    }
    return 0;
}

static int jobs_running(const Editor *ed)
{
    int n = 0;
    for (int i = 0; i < ED_MAX_JOBS; i++)
        if (ed->jobs[i].job) n++;
    return n;
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* Mismo archivo = mismo (dispositivo, inodo). Si alguno no existe se compara el texto. */
static int same_file(const char *a, const char *b)
{
    struct stat sa, sb;
    if (stat(a, &sa) == 0 && stat(b, &sb) == 0)
        return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
    return strcmp(a, b) == 0;
}

/* Libera los bloqueos y la memoria de una ranura. */
static void job_release(EdJob *e)
{
    if (e->in)  fl_release(e->in,  FL_READ);
    if (e->out) fl_release(e->out, FL_WRITE);
    free(e->in);
    free(e->out);
    memset(e, 0, sizeof *e);
}

static int start_job(Editor *ed, huff_op_t op, const char *in, const char *out)
{
    if (jobs_init(ed) == -1) return -1;

    if (same_file(in, out)) {
        fprintf(stderr, C_ERR "editor: el origen y el destino no pueden ser el mismo archivo.\n" C_RESET);
        return -1;
    }
    EdJob *slot = NULL;
    for (int i = 0; i < ED_MAX_JOBS; i++)
        if (!ed->jobs[i].job) { slot = &ed->jobs[i]; break; }
    if (!slot) {
        fprintf(stderr, C_ERR "editor: demasiadas tareas en curso (maximo %d). Usa 'j' o 'c'.\n" C_RESET,
                ED_MAX_JOBS);
        return -1;
    }

    /* Bloqueos: lectura sobre el origen, escritura sobre el destino. Orden fijo
     * (origen y luego destino) y NO bloqueantes: no puede haber interbloqueo. */
    if (fl_acquire(in, FL_READ) != 0) {
        fprintf(stderr, C_ERR "editor: '%s' esta siendo escrito por otra tarea; intenta luego.\n" C_RESET, in);
        return -1;
    }
    if (fl_acquire(out, FL_WRITE) != 0) {
        fl_release(in, FL_READ);
        fprintf(stderr, C_ERR "editor: '%s' esta en uso por otra tarea.\n" C_RESET, out);
        return -1;
    }

    char *cin = strdup(in), *cout = strdup(out);
    if (!cin || !cout) {
        perror("editor: strdup");
        free(cin); free(cout);
        fl_release(in, FL_READ); fl_release(out, FL_WRITE);
        return -1;
    }

    huff_opts_t o = {
        .workers = g_huff_workers,          /* 0 = un worker por CPU en linea */
        .block_size = g_huff_block,         /* 0 = bloques de 1 MiB            */
        .notify = on_job_event,
        .notify_ctx = (void *)(intptr_t)ed->evpipe[1],
    };
    char err[256];

    /* Se bloquean las senales mientras se crean los hilos de la tarea: la mascara
     * se hereda, asi que ningun hilo de trabajo recibira SIGINT/SIGTERM/SIGHUP. */
    sigset_t block, prev;
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    sigaddset(&block, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &block, &prev);
    huff_job_t *j = huff_job_start(op, in, out, &o, err, sizeof err);
    pthread_sigmask(SIG_SETMASK, &prev, NULL);

    if (!j) {
        fprintf(stderr, C_ERR "editor: no se pudo iniciar la tarea: %s\n" C_RESET, err);
        free(cin); free(cout);
        fl_release(in, FL_READ); fl_release(out, FL_WRITE);
        return -1;
    }

    slot->job = j;
    slot->op = op;
    slot->in = cin;
    slot->out = cout;
    slot->id = ++ed->job_seq;
    slot->last_decile = -1;

    printf(C_OK "Tarea #%u iniciada" C_RESET C_INFO ": %s '%s' -> '%s' en segundo plano "
           "('j' avance, 'c' cancelar)\n" C_RESET,
           slot->id, op == HUFF_COMPRESS ? "comprimiendo" : "descomprimiendo", in, out);
    return 0;
}

int editor_compress(Editor *ed, const char *out)
{
    if (!require_open(ed)) return -1;
    char def[PATH_MAX];
    if (out == NULL || *out == '\0') {
        if (strlen(ed->path) + 5 > sizeof def) {
            fprintf(stderr, C_ERR "editor: ruta demasiado larga.\n" C_RESET);
            return -1;
        }
        snprintf(def, sizeof def, "%s.huf", ed->path);
        out = def;
    }
    return start_job(ed, HUFF_COMPRESS, ed->path, out);
}

int editor_decompress(Editor *ed, const char *in, const char *out)
{
    char def[PATH_MAX];
    if (in == NULL || *in == '\0') {
        fprintf(stderr, C_ERR "Uso: u <archivo.huf> [destino]\n" C_RESET);
        return -1;
    }
    if (out == NULL || *out == '\0') {
        size_t n = strlen(in);
        if (n + 5 > sizeof def) {
            fprintf(stderr, C_ERR "editor: ruta demasiado larga.\n" C_RESET);
            return -1;
        }
        if (n > 4 && strcmp(in + n - 4, ".huf") == 0)
            snprintf(def, sizeof def, "%.*s", (int)(n - 4), in);
        else
            snprintf(def, sizeof def, "%s.dec", in);
        out = def;
    }
    return start_job(ed, HUFF_DECOMPRESS, in, out);
}

/* Tras descomprimir SOBRE el archivo abierto, el rename atomico dejo al descriptor
 * del editor apuntando al inodo viejo (ya sin nombre). Se detecta comparando el
 * inodo del fd con el de la ruta. El editor no guarda cambios pendientes en memoria
 * (todo es write-through), asi que recargar es seguro. */
static int fd_is_stale(const Editor *ed)
{
    struct stat a, b;
    if (ed->fd == -1 || ed->path == NULL) return 0;
    if (fstat(ed->fd, &a) == -1 || stat(ed->path, &b) == -1) return 0;
    return a.st_ino != b.st_ino || a.st_dev != b.st_dev;
}

static void drain_fd(int fd)
{
    unsigned char buf[256];
    while (fd != -1 && read(fd, buf, sizeof buf) > 0) { }
}

/* Atiende los eventos de las tareas: informa el avance (cada 10 %) y recoge las
 * que terminaron (join del hilo coordinador, liberacion de bloqueos y memoria).
 * Retorna cuantas lineas imprimio. 'async' = 1 cuando el prompt ya esta en pantalla
 * esperando teclado: se antepone un salto de linea para no pisarlo.
 *
 * Orden correcto: primero se VACIA el pipe y despues se lee el estado. Un evento
 * que llegue entre ambos pasos deja su byte en el pipe y se atendera en la
 * siguiente vuelta de poll(): no hay despertares perdidos. */
static int jobs_service(Editor *ed, int async)
{
    int printed = 0;
    drain_fd(ed->evpipe[0]);

    for (int i = 0; i < ED_MAX_JOBS; i++) {
        EdJob *e = &ed->jobs[i];
        if (!e->job) continue;

        huff_state_t st = huff_job_state(e->job);
        if (st == HUFF_RUNNING) {
            int dec = huff_job_percent(e->job) / 10;
            if (dec > e->last_decile) {
                e->last_decile = dec;
                if (async && printed == 0) putchar('\n');
                printf(C_INFO "[tarea #%u] %s '%s': %d%%\n" C_RESET, e->id,
                       e->op == HUFF_COMPRESS ? "comprimiendo" : "descomprimiendo",
                       base_name(e->in), dec * 10);
                printed++;
            }
            continue;
        }

        /* La tarea termino: se recogen sus resultados antes de liberar nada. */
        char msg[320];
        huff_job_message(e->job, msg, sizeof msg);
        huff_job_free(e->job);          /* join: no quedan hilos huerfanos */
        e->job = NULL;

        char out[PATH_MAX];
        snprintf(out, sizeof out, "%s", e->out);
        unsigned id = e->id;
        huff_op_t op = e->op;
        char shown_in[PATH_MAX];
        snprintf(shown_in, sizeof shown_in, "%s", base_name(e->in));
        char shown_out[PATH_MAX];
        snprintf(shown_out, sizeof shown_out, "%s", base_name(e->out));
        job_release(e);                 /* libera bloqueos ANTES de recargar */

        if (async && printed == 0) putchar('\n');
        if (st == HUFF_DONE)
            printf(C_OK "[tarea #%u OK]" C_RESET " %s\n", id, msg);
        else if (st == HUFF_CANCELED)
            printf(C_ERR "[tarea #%u CANCELADA]" C_RESET " '%s' -> '%s' "
                   "(el destino no fue modificado)\n", id, shown_in, shown_out);
        else
            printf(C_ERR "[tarea #%u ERROR]" C_RESET " %s\n", id, msg);
        printed++;

        if (st == HUFF_DONE && op == HUFF_DECOMPRESS && fd_is_stale(ed) && same_file(out, ed->path)) {
            char *p = strdup(ed->path);
            if (p) {
                printf(C_INFO "El archivo abierto fue reemplazado por la descompresion; recargando...\n" C_RESET);
                editor_open(ed, p);
                free(p);
                printed++;
            }
        }
    }
    fflush(stdout);
    return printed;
}

void editor_jobs_list(Editor *ed)
{
    int n = 0;
    for (int i = 0; i < ED_MAX_JOBS; i++) {
        EdJob *e = &ed->jobs[i];
        if (!e->job) continue;
        if (n++ == 0) printf(C_TITLE "--- Tareas en segundo plano ---\n" C_RESET);
        int pct = huff_job_percent(e->job);
        char bar[21];
        for (int k = 0; k < 20; k++) bar[k] = (k < pct / 5) ? '#' : '-';
        bar[20] = '\0';
        printf("  #%u  %-14s %s -> %s  [%s] %3d%%\n", e->id,
               e->op == HUFF_COMPRESS ? "comprimiendo" : "descomprimiendo",
               base_name(e->in), base_name(e->out), bar, pct);
    }
    if (n == 0) printf(C_INFO "(no hay tareas en segundo plano)\n" C_RESET);
}

/* Solicita la cancelacion (asincrona e idempotente). La tarea la atiende en su
 * siguiente bloque; el REPL se entera por 'evpipe' cuando termina. */
int editor_jobs_cancel(Editor *ed, unsigned id)
{
    int found = 0;
    for (int i = 0; i < ED_MAX_JOBS; i++) {
        EdJob *e = &ed->jobs[i];
        if (!e->job || (id != 0 && e->id != id)) continue;
        huff_job_cancel(e->job);
        printf(C_INFO "Cancelando la tarea #%u...\n" C_RESET, e->id);
        found++;
    }
    if (!found) {
        fprintf(stderr, C_ERR "editor: %s\n" C_RESET,
                id ? "no existe esa tarea en curso." : "no hay tareas en curso.");
        return -1;
    }
    return 0;
}

/* Cancela y ESPERA a todas las tareas (join), libera bloqueos y cierra el pipe.
 * Idempotente. Se invoca al cerrar el editor: ningun hilo sobrevive a la sesion. */
static void jobs_shutdown(Editor *ed)
{
    for (int i = 0; i < ED_MAX_JOBS; i++) {
        EdJob *e = &ed->jobs[i];
        if (!e->job) continue;
        huff_job_cancel(e->job);
        huff_job_free(e->job);          /* espera al coordinador y limpia temporales */
        e->job = NULL;
        job_release(e);
    }
    /* Solo ahora es seguro cerrar el pipe: ya no hay hilos que escriban en el. */
    if (ed->evpipe[0] != -1) close(ed->evpipe[0]);
    if (ed->evpipe[1] != -1) close(ed->evpipe[1]);
    ed->evpipe[0] = ed->evpipe[1] = -1;
}

/**
 * COMANDO 'q' -- cerrar y liberar.
 *
 * Syscall: close(2).
 *
 * Libera absolutamente toda la memoria dinamica (nombre del archivo e indice de
 * lineas) y deja la estructura en un estado consistente, de modo que llamarla
 * dos veces seguidas es seguro (idempotente).
 */
void editor_close(Editor *ed)
{
    jobs_shutdown(ed);      /* primero las tareas: cancelar + join de todos los hilos */
    close_file_resources(ed);

    /* A diferencia de 'close_file_resources', esta SI es la salida real de la
     * sesion (comando 'q' o Ctrl+D), asi que aqui se vacia tambien el
     * portapapeles: ninguna cadena copiada con 'y' debe sobrevivir al cierre
     * del editor. Idempotente: en una segunda llamada nclip ya es 0 y el
     * bucle no itera. */
    for (size_t i = 0; i < ed->nclip; i++)
        free(ed->clip[i]);
    free(ed->clip);
    ed->clip     = NULL;
    ed->nclip    = 0;
    ed->clip_cap = 0;
}

/* ==========================================================================
 * SECCION 4: CICLO INTERACTIVO (REPL)
 * ========================================================================== */

static void editor_help(void)
{
    printf(C_TITLE "\n--- Editor de texto CLI (comandos) ---\n" C_RESET);
    printf("  " C_PROMPT "o <archivo>" C_RESET "  Abre el archivo; si no existe lo crea.\n");
    printf("  " C_PROMPT "p [n]" C_RESET "        Imprime la linea n. Sin numero, todo el archivo.\n");
    printf("  " C_PROMPT "s [palabra]" C_RESET "  Busca una palabra dentro del archivo.\n");
    printf("  " C_PROMPT "a <texto>" C_RESET "    Anade el texto como nueva linea al final.\n");
    printf("  " C_PROMPT "i <n> <texto>" C_RESET "   Inserta el texto en la linea n especificada.\n");
    printf("  " C_PROMPT "d <n>" C_RESET "        Borra la linea n.\n");
    printf("  " C_PROMPT "y <n>" C_RESET "        Copia la linea n al portapapeles.\n");
    printf("  " C_PROMPT "x <n>" C_RESET "        Pega el portapapeles antes de la linea n.\n");
    printf("  " C_PROMPT "m" C_RESET "            Estado interno y metadatos del archivo (fstat).\n");
    printf("  " C_PROMPT "z [destino]" C_RESET "  Comprime (Huffman) el archivo abierto EN SEGUNDO PLANO. Por defecto <archivo>.huf\n");
    printf("  " C_PROMPT "u <huf> [dest]" C_RESET " Descomprime un .huf en segundo plano. Por defecto quita el .huf\n");
    printf("  " C_PROMPT "j" C_RESET "            Estado y avance de las tareas en segundo plano.\n");
    printf("  " C_PROMPT "c [id]" C_RESET "       Cancela la tarea #id (sin id: todas). Ctrl-C tambien las cancela.\n");
    printf("  " C_PROMPT "t" C_RESET "            Activa o desactiva la traza de syscalls.\n");
    printf("  " C_PROMPT "h" C_RESET "            Muestra esta ayuda.\n");
    printf("  " C_PROMPT "q" C_RESET "            Cierra el archivo y sale del editor.\n");
    printf(C_INFO "  (Ctrl+D equivale a 'q')\n\n" C_RESET);
}

/* Traduce los 9 bits de permiso de st_mode al formato "rwxr-xr-x" de ls -l.
 * Solo dialogo por consola: no toca el archivo del usuario. */
static void perm_to_str(mode_t mode, char out[10])
{
    const char *bits = "rwx";
    for (int grp = 0; grp < 3; grp++) {          /* dueno, grupo, otros */
        for (int b = 0; b < 3; b++) {
            int shift = (2 - grp) * 3 + (2 - b);
            out[grp * 3 + b] = (mode & (1 << shift)) ? bits[b] : '-';
        }
    }
    out[9] = '\0';
}

/**
 * COMANDO 'm' (parte 2) -- metadatos del archivo via fstat(2).
 *
 * A diferencia de open/read/write/lseek, que operan sobre el CONTENIDO del
 * archivo, fstat(2) consulta el INODO: los metadatos que el sistema de
 * archivos guarda aparte de los bytes (tamano real en disco, permisos,
 * numero de inodo, fecha de la ultima modificacion...).
 *
 * Se usa fstat(fd, ...) y no stat(path, ...) a proposito: el editor ya tiene
 * el descriptor abierto, asi que evita resolver la ruta una segunda vez y no
 * sufre una condicion de carrera TOCTOU si el archivo fue renombrado o
 * reemplazado entre el 'o' y el 'm' (fstat siempre consulta el inodo al que
 * apunta el fd, sin importar el nombre que tenga ahora en el directorio).
 */
static int editor_metadata_print(const Editor *ed)
{
    struct stat st;

    TRACE(ed, "fstat", "%d, &st", ed->fd);
    if (fstat(ed->fd, &st) == -1) {
        int saved = errno;
        TRACE_ERR(ed);
        errno = saved;
        perror("editor: fstat");
        return -1;
    }
    TRACE_OK(ed, 0);

    char permisos[10];
    perm_to_str(st.st_mode, permisos);

    char fecha[32];
    struct tm tmv;
    localtime_r(&st.st_mtime, &tmv);
    strftime(fecha, sizeof fecha, "%Y-%m-%d %H:%M:%S", &tmv);

    printf(C_TITLE "--- Metadatos del archivo (fstat) ---\n" C_RESET);
    printf("  Tamano en disco:     %lld bytes\n", (long long)st.st_size);
    printf("  Permisos:            %o (%s)\n", (unsigned)(st.st_mode & 0777), permisos);
    printf("  Inodo:               %llu\n", (unsigned long long)st.st_ino);
    printf("  Ultima modificacion: %s\n", fecha);
    printf(C_TITLE "--------------------------------------\n" C_RESET);
    return 0;
}

static void editor_info(const Editor *ed)
{
    printf(C_TITLE "--- Estado del editor ---\n" C_RESET);
    if (ed->fd == -1) {
        printf("  Sin archivo abierto.\n");
    } else {
        printf("  Archivo:            " C_OK "%s" C_RESET "\n", ed->path);
        printf("  Descriptor (fd):    %d\n", ed->fd);
        printf("  Tamano:             %lld bytes\n", (long long)ed->size);
        printf("  Lineas indexadas:   %zu\n", ed->nlines);
        printf("  Capacidad indice:   %zu (%zu bytes de heap)\n",
               ed->cap, ed->cap * sizeof(Line));
        printf("  Portapapeles:       %zu linea%s en cola\n",
               ed->nclip, ed->nclip == 1 ? "" : "s");
        printf("  Tareas en 2o plano: %d\n", jobs_running(ed));
    }
    printf(C_TITLE "-------------------------\n" C_RESET);

    /* El comando 'm' del enunciado pide especificamente tamano, permisos,
     * inodo y fecha de modificacion via fstat(); se imprime como un segundo
     * bloque en vez de reemplazar el estado interno de arriba, que ya tenia
     * un uso pedagogico propio (fd, indice, heap) y esta cubierto por
     * pruebas existentes. */
    if (ed->fd != -1) editor_metadata_print(ed);
}

/* Convierte un texto a numero de linea validando que sea realmente un entero.
 * Retorna 0 si el texto esta vacio (equivale a "sin argumento") y -1 si es
 * invalido. */
static long parse_line_number(const char *s, int *ok)
{
    char *endp;
    *ok = 1;

    while (*s == ' ' || *s == '\t') s++;
    if (*s == '\0') return 0;                       /* sin argumento */

    errno = 0;
    long v = strtol(s, &endp, 10);
    while (*endp == ' ' || *endp == '\t') endp++;

    if (endp == s || *endp != '\0' || errno == ERANGE) {
        *ok = 0;
        return 0;
    }
    return v;
}

/* Elimina espacios al inicio y al final (usado para nombres de archivo). */
static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    size_t l = strlen(s);
    while (l > 0 && (s[l - 1] == ' ' || s[l - 1] == '\t')) s[--l] = '\0';
    return s;
}

/* ==========================================================================
 * Lectura de comandos SIN congelar la interfaz
 * --------------------------------------------------------------------------
 * El REPL del Parcial 1 se bloqueaba en fgets(stdin): mientras esperaba una
 * tecla era imposible mostrar el avance de una tarea. Ahora espera en poll()
 * sobre TRES descriptores a la vez:
 *      stdin        -> el usuario escribio algo
 *      evpipe[0]    -> una tarea cambio de estado / avanzo
 *      g_sigpipe[0] -> llego SIGINT / SIGTERM / SIGHUP
 * poll() duerme al proceso (cero CPU) hasta que ocurra cualquiera de los tres.
 *
 * Detalle importante: se sigue leyendo con stdio (getc) y no con read(2) directo
 * porque el shell (eafitOS) comparte el mismo FILE* stdin: si el editor leyera
 * por su cuenta, las lineas que el shell ya hubiera guardado en el buffer de
 * stdio (entrada por tuberia) se perderian. Por eso, antes de dormir en poll()
 * se consume lo que ya este bufereado; poll() solo ve el descriptor, no el
 * buffer de stdio, y dormiria con datos pendientes.
 * ========================================================================== */

#define RC_EOF   0
#define RC_LINE  1
#define RC_QUIT -1

static int stdin_buffered(void)
{
#ifdef __GLIBC__
    return stdin->_IO_read_ptr != NULL && stdin->_IO_read_ptr < stdin->_IO_read_end;
#else
    return 0;   /* otras libc: solo poll(); ver nota de limitaciones en el README */
#endif
}

static void print_prompt(const Editor *ed)
{
    int  n = jobs_running(ed);
    char tag[32] = "";
    if (n > 0) snprintf(tag, sizeof tag, " [%d tarea%s]", n, n == 1 ? "" : "s");
    printf(C_PROMPT "edit%s%s%s> " C_RESET, ed->path ? ":" : "", ed->path ? ed->path : "", tag);
    fflush(stdout);   /* el prompt no lleva '\n', hay que forzar el vaciado */
}

/* Acumula un caracter. Retorna 1 cuando se completo una linea. Las lineas que no
 * caben en 'cap' se descartan enteras (con un aviso), igual que en el Parcial 1,
 * para que su resto no se interprete como un comando nuevo. */
static int accept_char(int c, char *line, size_t cap, size_t *len, int *discarding)
{
    if (c == '\n') {
        if (*discarding) { *discarding = 0; *len = 0; return 0; }
        line[*len] = '\0';
        *len = 0;
        return 1;
    }
    if (*discarding) return 0;
    if (*len < cap - 1) { line[(*len)++] = (char)c; return 0; }
    fprintf(stderr, C_ERR "editor: linea demasiado larga, se ignora.\n" C_RESET);
    *discarding = 1;
    *len = 0;
    return 0;
}

/* Procesa los bytes del pipe de senales. Retorna 1 si hay que salir del editor. */
static int handle_signals(Editor *ed)
{
    unsigned char b[32];
    ssize_t n = read(g_sigpipe[0], b, sizeof b);
    int quit = 0;

    for (ssize_t i = 0; i < n; i++) {
        if (b[i] == 'T') {                          /* SIGTERM / SIGHUP: salida ordenada */
            printf("\n" C_INFO "(senal de terminacion) cerrando el editor...\n" C_RESET);
            quit = 1;
        } else {                                    /* SIGINT (Ctrl-C) */
            int nj = jobs_running(ed);
            if (nj > 0) {
                printf("\n" C_INFO "(Ctrl-C) cancelando %d tarea(s) en segundo plano...\n" C_RESET, nj);
                editor_jobs_cancel(ed, 0);
            } else {
                printf("\n" C_INFO "(Ctrl-C) cerrando el editor...\n" C_RESET);
                quit = 1;
            }
        }
    }
    fflush(stdout);
    return quit;
}

/* Espera el siguiente comando. Retorna RC_LINE (hay una linea en 'line'), RC_EOF
 * (fin de la entrada y ya no quedan tareas) o RC_QUIT (senal de terminacion). */
static int read_command(Editor *ed, char *line, size_t cap)
{
    size_t len = 0;
    int discarding = 0, eof = 0, announced = 0;

    for (;;) {
        /* A) Lo que ya esta en el buffer de stdio se consume sin dormir. */
        while (!eof && stdin_buffered()) {
            int c = getc(stdin);
            if (c == EOF) { eof = 1; break; }
            if (accept_char(c, line, cap, &len, &discarding)) return RC_LINE;
        }

        /* B) Fin de la entrada: se entrega una ultima linea sin '\n' (como fgets) y
         *    despues se ESPERA a que terminen las tareas en curso. Asi un script
         *    como  printf 'o f\nz f.huf\n' | editor  no cancela la compresion al
         *    llegar al EOF. Ctrl-C la cancela. */
        if (eof) {
            if (len > 0 && !discarding) { line[len] = '\0'; return RC_LINE; }
            int nj = jobs_running(ed);
            if (nj == 0) return RC_EOF;
            if (!announced) {
                printf("\n" C_INFO "(fin de la entrada) esperando %d tarea(s) en segundo plano; "
                       "Ctrl-C las cancela.\n" C_RESET, nj);
                fflush(stdout);
                announced = 1;
            }
        }

        /* C) Dormir hasta que pase algo en CUALQUIERA de los descriptores. */
        struct pollfd fds[3];
        int n = 0, i_in = -1, i_ev = -1, i_sig = -1;
        if (!eof) { fds[n] = (struct pollfd){ STDIN_FILENO, POLLIN, 0 }; i_in = n++; }
        if (ed->evpipe[0] != -1) { fds[n] = (struct pollfd){ ed->evpipe[0], POLLIN, 0 }; i_ev = n++; }
        fds[n] = (struct pollfd){ g_sigpipe[0], POLLIN, 0 }; i_sig = n++;

        if (poll(fds, (nfds_t)n, -1) < 0) {
            if (errno == EINTR) continue;           /* la senal ya dejo su byte en el pipe */
            perror("editor: poll");
            return RC_QUIT;
        }

        if (fds[i_sig].revents & POLLIN) {
            if (handle_signals(ed)) return RC_QUIT;
        }
        if (i_ev >= 0 && (fds[i_ev].revents & POLLIN)) {
            if (jobs_service(ed, 1) > 0 && !eof)
                print_prompt(ed);                   /* el mensaje pisó el prompt: se repone */
        }
        if (i_in >= 0 && (fds[i_in].revents & (POLLIN | POLLHUP | POLLERR))) {
            int c = getc(stdin);                    /* poll dijo "listo": no bloquea */
            if (c == EOF) eof = 1;
            else if (accept_char(c, line, cap, &len, &discarding)) return RC_LINE;
        }
    }
}

static int repl_body(const char *path, int trace)
{
    Editor ed;
    char   line[MAX_LINE];
    int    quit_armed = 0;

    editor_init(&ed, trace);
    if (jobs_init(&ed) == -1) return 1;

    printf(C_TITLE "\n=== Editor de texto CLI (syscalls POSIX) ===\n" C_RESET);
    printf(C_INFO "Escribe 'h' para ver los comandos disponibles.\n\n" C_RESET);

    if (path != NULL && editor_open(&ed, path) == -1) {
        editor_close(&ed);
        return 1;
    }

    for (;;) {
        jobs_service(&ed, 0);     /* informa tareas que terminaron entre dos comandos */
        print_prompt(&ed);

        int rc = read_command(&ed, line, sizeof line);
        if (rc == RC_EOF) {
            printf("\n" C_INFO "(fin de entrada)\n" C_RESET);
            break;
        }
        if (rc == RC_QUIT) {
            break;
        }

        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') continue;                       /* linea vacia */

        char  cmd  = *p;
        char *rest = p + 1;

        /* Los comandos son de una sola letra: lo que sigue debe ser un espacio
         * o el fin de la linea. Asi 'oo' no se confunde con 'o'. */
        if (*rest != '\0' && *rest != ' ' && *rest != '\t') {
            fprintf(stderr, C_ERR "editor: comando '%s' no reconocido. "
                            "Escribe 'h' para la ayuda.\n" C_RESET, p);
            continue;
        }

        int  ok;
        long n;
        int  was_armed = quit_armed;
        quit_armed = 0;           /* cualquier otro comando desarma la confirmacion de 'q' */

        switch (cmd) {
        case 'o': {
            char *fname = trim(rest);
            if (*fname == '\0') {
                fprintf(stderr, C_ERR "Uso: o <archivo>\n" C_RESET);
                break;
            }
            editor_open(&ed, fname);
            break;
        }
        case 'p':
            n = parse_line_number(rest, &ok);
            if (!ok) { fprintf(stderr, C_ERR "Uso: p [n]  (n debe ser un entero)\n" C_RESET); break; }
            editor_print(&ed, n);
            break;

        case 's':
            rest = trim(rest);

            if (*rest == '\0') {
                fprintf(stderr, C_ERR "Uso: s <palabra>\n" C_RESET);
                break;
            }

            editor_search(&ed, rest);
            break;

        case 'a':
            /* Se salta UN solo espacio separador; el resto del texto se toma de
             * forma literal para no perder la indentacion que escriba el usuario. */
            if (*rest == ' ') rest++;
            editor_append(&ed, rest);
            break;

        case 'd':
            n = parse_line_number(rest, &ok);
            if (!ok || n == 0) { fprintf(stderr, C_ERR "Uso: d <n>  (n debe ser un entero positivo)\n" C_RESET); break; }
            editor_delete(&ed, n);
            break;

        case 'm':
            editor_info(&ed);
            break;

        case 'i': {
            char *endp;
            errno = 0;

            /* Se extrae el numero de linea directamente con strtol, sin pasar
             * por parse_line_number: aqui "sin argumento" no es un caso valido
             * (a diferencia de 'p'), asi que basta validar endp == rest. */
            long line_num = strtol(rest, &endp, 10);

            if (endp == rest || line_num <= 0 || errno == ERANGE) {
                fprintf(stderr, C_ERR "Uso: i <n> <texto>  (n debe ser un entero positivo)\n" C_RESET);
                break;
            }

            /* Salta el o los espacios entre el numero de linea y el texto */
            while (*endp == ' ' || *endp == '\t') endp++;

            editor_insert(&ed, line_num, endp);
            break;
        }

        case 'y':
            n = parse_line_number(rest, &ok);
            if (!ok || n == 0) { fprintf(stderr, C_ERR "Uso: y <n>  (n debe ser un entero positivo)\n" C_RESET); break; }
            editor_yank(&ed, n);
            break;

        case 'x':
            n = parse_line_number(rest, &ok);
            if (!ok || n == 0) { fprintf(stderr, C_ERR "Uso: x <n>  (n debe ser un entero positivo)\n" C_RESET); break; }
            editor_paste(&ed, n);
            break;

        /* ---- Parcial 2: tareas en segundo plano ---- */
        case 'z':     /* z [destino.huf] : comprime el archivo abierto */
            editor_compress(&ed, trim(rest));
            break;

        case 'u': {   /* u <origen.huf> [destino] : descomprime */
            char *args = trim(rest);
            char *sp = args;
            while (*sp && *sp != ' ' && *sp != '\t') sp++;
            char *dst = NULL;
            if (*sp) { *sp++ = '\0'; dst = trim(sp); }
            editor_decompress(&ed, args, dst);
            break;
        }

        case 'j':     /* estado y avance de las tareas */
            editor_jobs_list(&ed);
            break;

        case 'c':     /* c [id] : cancela una tarea (o todas) */
            n = parse_line_number(rest, &ok);
            if (!ok || n < 0) { fprintf(stderr, C_ERR "Uso: c [id]  (sin id cancela todas)\n" C_RESET); break; }
            editor_jobs_cancel(&ed, (unsigned)n);
            break;

        case 't':
            ed.trace = !ed.trace;
            printf(C_INFO "Traza de syscalls: %s\n" C_RESET,
                   ed.trace ? "ACTIVADA" : "DESACTIVADA");
            break;

        case 'h':
            editor_help();
            break;

        case 'q': {
            int nj = jobs_running(&ed);
            if (nj > 0 && !was_armed) {
                quit_armed = 1;
                printf(C_INFO "Hay %d tarea(s) en curso. Escribe 'q' otra vez para cancelarlas y salir.\n" C_RESET, nj);
                break;
            }
            editor_close(&ed);      /* cancela + join de las tareas, libera todo */
            printf(C_INFO "Editor cerrado. Recursos liberados.\n" C_RESET);
            return 0;
        }

        default:
            fprintf(stderr, C_ERR "editor: comando '%c' no reconocido. "
                            "Escribe 'h' para la ayuda.\n" C_RESET, cmd);
            break;
        }
    }

    /* Salida por Ctrl+D / senal de terminacion: hay que liberar igual que en 'q'. */
    editor_close(&ed);
    return 0;
}

/**
 * Punto de entrada publico. Envuelve al REPL con la instalacion de los manejadores
 * de senales y su RESTAURACION al salir: 'edit' es un comando interno del shell
 * (eafitOS), asi que el editor no puede dejar sus manejadores instalados al volver.
 */
int editor_repl(const char *path, int trace)
{
    struct sigaction sa, old_int, old_term, old_hup;

    if (pipe2(g_sigpipe, O_NONBLOCK | O_CLOEXEC) == -1) {
        perror("editor: pipe2 de senales");
        return 1;
    }
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;           /* las syscalls lentas se reanudan solas */
    sigaction(SIGINT,  &sa, &old_int);
    sigaction(SIGTERM, &sa, &old_term);
    sigaction(SIGHUP,  &sa, &old_hup);

    int rc = repl_body(path, trace);

    sigaction(SIGINT,  &old_int,  NULL);
    sigaction(SIGTERM, &old_term, NULL);
    sigaction(SIGHUP,  &old_hup,  NULL);
    close(g_sigpipe[0]);
    close(g_sigpipe[1]);
    g_sigpipe[0] = g_sigpipe[1] = -1;
    return rc;
}
