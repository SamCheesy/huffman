# Editor de texto CLI con compresión Huffman concurrente

**Universidad EAFIT — Sistemas Operativos — Parcial 2, Alternativa 1**
*Compresor de archivos Huffman concurrente integrado al editor del Parcial 1. Samuel Ruidiaz Camargo*

C11 + POSIX threads (`pthread`, `mutex`, variables de condición, `stdatomic`). Sin
librerías de alto nivel que abstraigan la sincronización. Todo el acceso a archivos
usa llamadas al sistema (`open/read/pread/write/pwrite/lseek/ftruncate/rename/close`),
igual que el Parcial 1: no hay `fopen/fread/fwrite/fclose` sobre ningún archivo.

> **Base:** el editor y el shell educativo (`eafitOS`) son el entregable del Parcial 1
> (equipo de tres integrantes). Este parcial añade el módulo de compresión y su
> integración. La sección [Cambios respecto al Parcial 1](#cambios-respecto-al-parcial-1)
> lista exactamente qué se tocó. El README original está en `docs/README_parcial1.md`.

---

## 1. Compilar, ejecutar y probar

```bash
make                 # bin/eafitOS (shell), bin/editor (editor) y bin/huffcli (CLI del compresor)
make test            # las tres suites siguientes
make test-p1         # 66 verificaciones originales del editor (regresión del Parcial 1)
make test-huffman    # 34 casos: integridad md5/cmp, errores, cancelación, origen modificado
make test-integ      # 69 verificaciones: editor + tareas en segundo plano (concurrencia)
make test-tsan       # todo lo anterior con ThreadSanitizer (carreras de datos)
make valgrind        # memcheck (fugas, fds) + helgrind (carreras, locks) sobre el editor
```

Requisitos: Linux, GCC con soporte de C11 (probado con GCC 13), Make, Python 3 (solo para `test-integ`), `valgrind` (opcional).
Las pruebas se invocan con `bash` / `python3` explícitamente, así que no dependen del bit
de ejecución del zip.

## 2. Uso

```
$ ./bin/editor archivo.txt           # editor autónomo   (opciones: -q sin traza, -j hilos, -b KiB)
$ ./bin/eafitOS                      # shell; dentro:  edit archivo.txt
```

Comandos nuevos (se suman a `o p s a i d y x m t h q` del Parcial 1):

| Comando | Qué hace |
|---|---|
| `z [destino]` | Comprime el archivo abierto **en segundo plano**. Por defecto `<archivo>.huf`. |
| `u <origen.huf> [destino]` | Descomprime en segundo plano. Por defecto quita la extensión `.huf`. |
| `j` | Estado y barra de avance de cada tarea. |
| `c [id]` | Cancela la tarea `#id` (sin id: todas). |
| `Ctrl-C` | Con tareas en curso las cancela; sin tareas cierra el editor ordenadamente. |

Sesión real (archivo de 300 MB, `./bin/editor -q -j 1 grande.txt`; salida recortada):

```
edit:grande.txt> z
Tarea #1 iniciada: comprimiendo 'grande.txt' -> 'grande.txt.huf' en segundo plano ('j' avance, 'c' cancelar)
[tarea #1] comprimiendo 'grande.txt': 0%
edit:grande.txt [1 tarea]> a esto se rechaza
editor: 'grande.txt' esta en uso por una tarea en segundo plano (compresion/descompresion). Espera a que termine ...
edit:grande.txt [1 tarea]> p 1                                   <- leer SI se permite
     1  linea de texto para la demo de compresion concurrente
edit:grande.txt [1 tarea]> j
--- Tareas en segundo plano ---
  #1  comprimiendo   grande.txt -> grande.txt.huf  [############--------]  62%
edit:grande.txt [1 tarea]>
[tarea #1] comprimiendo 'grande.txt': 70%                        <- el avance llega solo, sin teclear
[tarea #1] comprimiendo 'grande.txt': 80%
[tarea #1 OK] Comprimido: 300000000 -> 143753822 bytes (47.9%)
edit:grande.txt> a ahora si se puede                             <- terminada la tarea, se puede editar
Linea 5555557 anadida (19 bytes escritos, tamano actual 300000019 bytes)
```

## 3. Arquitectura

```
                         ┌────────────── hilo del REPL (UI) ──────────────┐
  teclado ─────────────► │ poll( stdin , evpipe , sigpipe )  — duerme, 0% CPU │
  evpipe  ◄── 1 byte ─── │ dueño ÚNICO de: buffer de líneas, índice, tabla de │
  sigpipe ◄── 1 byte ─── │ tareas y registro de bloqueos (filelock)           │
  (handler de señales)   └──────────┬───────────────────────────────────────┘
                                    │ huff_job_start()  (retorna de inmediato)
                       ┌────────────▼────────────┐
                       │  hilo COORDINADOR       │  conteo → árbol → codificación
                       └───┬──────────────┬──────┘
                 tareas FIFO│              │ ventana de 2N+2 bloques
                  ┌─────────▼───┐   ┌──────▼─────────┐
                  │ N WORKERS   │──►│ slots[idx]     │──► hilo ESCRITOR ──► archivo.huf
                  │ (pool)      │   │ mutex+cond     │    (orden estricto)
                  └─────────────┘   └────────────────┘
```

* **Los hilos de la tarea solo tocan archivos.** Nunca leen ni escriben estructuras del
  editor. Su único canal hacia la UI es `evpipe` (un byte por cambio de estado).
* **La UI nunca espera a una tarea:** `z` y `u` solo crean el hilo coordinador y retornan.

### Algoritmo de compresión (`src/huffman/huffman.c`)

1. **Conteo (paralelo):** el archivo se divide en bloques (1 MiB por defecto). Cada tarea del
   pool lee su bloque con `pread`, cuenta en un **histograma local sin locks** y suma al
   global bajo un mutex (un lock por bloque, no por byte: *reducción local*).
2. **Barrera:** `tpool_wait_idle()` — variable de condición, sin espera activa.
3. **Árbol de Huffman global** (secuencial, solo 256 símbolos) → longitudes → **códigos
   canónicos**. Solo se guardan las 256 longitudes; no hace falta serializar el árbol.
4. **Codificación (paralela):** una tarea por bloque deposita su resultado en `slots[idx]`.
5. **Escritor ordenado:** un hilo único espera `slots[next]`, lo escribe y avanza. El
   coordinador limita los bloques en vuelo a `2N+2` ⇒ memoria acotada y progreso garantizado.
6. **Commit atómico:** se escribe en `salida.XXXXXX`; solo si todo salió bien: `fsync` +
   `rename`. Si falla o se cancela, el temporal se borra y el destino queda intacto.

**Descompresión:** el coordinador recorre las cabeceras de bloque y encola una tarea por
bloque; cada worker decodifica, verifica el **CRC32** y hace `pwrite` en `idx·tamaño_bloque`
(regiones disjuntas ⇒ sin locks para los datos). La salida también es atómica.

Formato `.huf` (little-endian): `"HUF1" | u32 tam_bloque | u64 tam_original | u8 longitud[256]`
y por bloque `u32 raw_len | u32 comp_len | u32 crc32 | datos`. El resultado es **idéntico**
con 1 o con N hilos (los desempates del árbol son deterministas y la salida es por índice).

### Aceleración medida

Archivo de texto de 300 MB, máquina de **2 núcleos** (`huffcli c|d -j N`, bloques de 1 MiB):

| Operación | `-j 1` | `-j 2` | Aceleración |
|---|---|---|---|
| Comprimir | 3,8 – 4,3 s | 2,0 – 2,2 s | ≈ 1,9× |
| Descomprimir | ≈ 7,2 s | 3,7 – 5,1 s | ≈ 1,4 – 1,9× (más ruido por el I/O) |

El `.huf` resultante es byte a byte idéntico con 1 y con 2 hilos. Con más núcleos el tramo
secuencial (árbol de 256 símbolos y escritura ordenada) limita la ganancia (ley de Amdahl).

## 4. Protocolo de concurrencia

| Primitiva | Protege | Quién espera / quién despierta |
|---|---|---|
| `pool.mu` + `not_empty` | cola de tareas del pool | los workers duermen sin tareas; `submit` hace `signal` |
| `pool.mu` + `idle` | contadores cola/activas | `wait_idle` duerme hasta cola vacía y 0 activas |
| `job.fmu` | histograma global | workers (sección crítica de 256 sumas) |
| `job.om` + `om_ready` | `slots[]`, `next_write` | el escritor duerme hasta que `slots[next].ready` |
| `job.om` + `om_space` | ventana de bloques en vuelo | el coordinador duerme si `i − next_write ≥ ventana` |
| `job.mu` + `cv` | mensaje de estado, contador de eventos | `huff_job_wait*` |
| átomicos (`state`, `cancel`, `failed`, `done`, `total`) | estado y progreso | la UI los lee sin lock |
| `filelock.mu` | tabla de bloqueos por ruta | secciones críticas cortas; **nunca bloquea al llamador** |
| `evpipe` / `sigpipe` | comunicación hilos → REPL | el REPL duerme en `poll()` |

**Sin interbloqueo.** Nunca se mantiene más de un mutex a la vez; los workers solo esperan la
cola; la ventana siempre incluye el bloque que el escritor espera y la cola es FIFO, así que
ese bloque siempre termina de codificarse. Los bloqueos de archivo son *no bloqueantes*
(retornan error en vez de esperar) y se toman en orden fijo (origen, luego destino).

**Sin espera activa.** Toda espera es `pthread_cond_wait`, `poll()` sin timeout o `read` de
pipe. No hay `sleep`, `usleep` ni bucles de sondeo en el código del producto.

**Sin despertares perdidos.** Cada predicado se comprueba bajo el mismo mutex con el que se
despierta. Para cancelar se activa el átomico y luego se hace `lock + broadcast`. El REPL
primero *vacía* `evpipe` y después lee el estado: un evento intermedio deja su byte en el pipe.

**Cancelación y errores.** `cancel`/`failed` se consultan en cada tarea. Toda ruta de salida
hace la misma limpieza: join del escritor → destruir el pool (join de workers) → liberar
slots → cerrar descriptores → borrar el temporal → join del coordinador (`huff_job_free`).
Ningún hilo queda en `detach`.

**Señales.** El handler solo hace `write()` a un pipe (async-signal-safe). La lógica corre en
el hilo del REPL, dentro de `poll()`. Las señales se **bloquean** en los hilos de las tareas
(la máscara se hereda al crearlos), así que siempre las atiende el hilo de la UI. Los
manejadores se **restauran** al salir de `editor_repl`, porque dentro del shell `edit` es un
comando interno.

## 5. Integración con el editor: condiciones de carrera

El editor del Parcial 1 es *write-through*: cada edición va directo a disco y, para insertar
o borrar, **desplaza la cola del archivo en su sitio**; mientras tanto el archivo está
transitoriamente inconsistente. Por eso el acceso al archivo es un problema de
**lectores-escritor** (`src/huffman/filelock.c`, por ruta canónica):

| Situación | Comportamiento |
|---|---|
| Comprimir (`z`) | La tarea toma un bloqueo de **lectura** sobre el origen durante toda su vida. |
| Editar (`a i d x`) mientras se comprime | **Rechazado** con un mensaje (esperar congelaría la UI). |
| Leer (`p s y m`) mientras se comprime | Permitido (lector + lector son compatibles). |
| Dos compresiones del mismo archivo | Permitido (dos lectores). Mismo destino ⇒ rechazado. |
| Origen = destino | Rechazado antes de crear la tarea. |
| Descomprimir (`u`) sobre el archivo abierto | Bloqueo de **escritura** en el destino: editar se rechaza; leer ve una copia consistente del archivo viejo; al terminar el editor **recarga** el archivo reemplazado. |
| Otro proceso modifica el origen durante la tarea | Se detecta (`tamaño/inodo/mtime`) y la tarea falla sin dejar salida. |
| Guardado / salida del job | Atómicos (`tmp` + `rename`): nunca se ve un archivo a medias. |
| `q` con tareas en curso | Avisa; el segundo `q` cancela, espera a los hilos y sale. |
| Fin de la entrada (Ctrl-D, tuberías) | **Espera** a las tareas en curso en vez de cancelarlas. |

El editor del Parcial 1 no tiene un comando "guardar" porque cada edición ya escribe; por eso
"editar o guardar mientras se comprime" equivale a `a`, `i`, `d` y `x`.

**Progreso en tiempo real.** Cada cambio de porcentaje entero genera un evento; el REPL
informa cada 10 % y en `j` muestra la barra. Es un REPL por líneas: una barra animada
obligaría a un modo de terminal sin eco, que cambia por completo el editor del Parcial 1.

**Lectura de comandos.** `fgets` bloqueaba la interfaz, así que el REPL espera ahora con
`poll()` sobre teclado + eventos + señales. Se sigue leyendo con `getc(stdin)` (y antes de
dormir se consume lo que ya esté en el buffer de stdio) porque el shell comparte ese
mismo `FILE*`: así las líneas que el shell ya había leído por tubería no se pierden. Esto
usa un campo interno de glibc (`_IO_read_ptr`); con otra libc solo se usaría `poll()`.

## 6. Cobertura de la rúbrica (100 pts)

| Criterio | Dónde está / evidencia |
|---|---|
| **Concurrencia en Huffman (25)** | Conteo y codificación por bloques en un pool; reducción local; escritor ordenado. Prueba: salida **idéntica** con 1 y 8 hilos (`test-huffman`). |
| **Sincronización (25)** | mutex + variables de condición + átomicos; tabla de protocolo (§4); sin espera activa; TSan y helgrind sin reportes. |
| **Integración con el editor (20)** | Segundo plano sin congelar la UI, progreso en vivo, bloqueo lectores-escritor, recarga, señales (`test-integ`, §5). |
| **Corrección algorítmica (15)** | `md5` + `cmp` en 23 combinaciones (vacío, 1 byte, 1 símbolo, aleatorio, sesgado, múltiplos de bloque…), CRC32 por bloque, y ida y vuelta editor ↔ CLI. |
| **Errores y recursos (15)** | Una única ruta de limpieza, join de todos los hilos, temporales borrados, `valgrind --track-fds` limpio, cancelación, SIGINT/SIGTERM/SIGHUP, archivos corruptos o truncados. |

## 7. Cambios respecto al Parcial 1

| Archivo | Cambio |
|---|---|
| `src/editor/editor.c` | Bucle de eventos con `poll()`; comandos `z u j c`; bloqueo de archivo en `a i d x`; manejo de señales; cierre con cancelación y join. |
| `include/editor.h` | Estado de las tareas en `Editor`; API `editor_compress/decompress/jobs_*`. |
| `src/editor/editor_main.c` | Banderas `-j` y `-b`. |
| `Makefile` | `-pthread`, `-std=gnu11` (C11 por `_Atomic`), módulos nuevos, objetivos `test-*`, `tsan`, `valgrind`; las pruebas se lanzan con `bash`. |
| `tests/test_editor.sh` | 2 verificaciones adaptadas (ver abajo). El resto, intacto: **66/66**. |
| `src/huffman/*`, `include/{huffman,threadpool,filelock}.h` | **Nuevos**: compresor, pool de hilos, bloqueos. |
| `tests/test_huffman.sh`, `test_integracion.py`, `test_valgrind.sh` | **Nuevos**. |

Defectos del Parcial 1 corregidos de paso:

* **`o` con un archivo que falla** cerraba primero el actual y dejaba al usuario sin archivo.
  Ahora se abre el nuevo antes de soltar el anterior. Consecuencia: el descriptor alterna
  entre dos números consecutivos, por lo que la prueba *"el fd se reutiliza"* ahora comprueba
  *"el fd no crece"* (que es lo que realmente importaba: ausencia de fugas).
* **`s` con bytes NUL:** usaba `strstr` y no encontraba texto posterior a un NUL; ahora `memmem`.
* **`z` ya es un comando válido,** así que la prueba de "comando desconocido" usa `w`.
* `make test` fallaba con *Permission denied* en un zip sin bit de ejecución.

## 8. Límites conocidos

* El bloqueo de archivo es **dentro del proceso**. Otro proceso (otro editor, `p_exec
  ./bin/editor`, un editor externo) no lo respeta; el chequeo de `tamaño/inodo/mtime` al final
  *detecta* la modificación y hace fallar la tarea, pero no la *impide*.
* Mientras se comprime, editar se **rechaza**; no se encola ni se difiere.
* El progreso se imprime por líneas (cada 10 %), no como barra animada.
* `Ctrl-C` no interrumpe un comando síncrono ya en ejecución (`p` de un archivo enorme); se
  atiende al volver a `poll()`.
* `u` no admite nombres de archivo con espacios.
* Longitud de código máxima: 56 bits (solo se excede con archivos de cientos de GB muy
  sesgados; la tarea falla con un mensaje claro en vez de producir datos erróneos).
* La decodificación es bit a bit (clara y correcta, no la más rápida; se podría usar una tabla
  de búsqueda).

## 9. Estructura del proyecto

```
.
├── Makefile
├── README.md                    este archivo
├── SUSTENTACION.md              guion de sustentación y preguntas probables
├── include/
│   ├── editor.h  shell.h        (Parcial 1; editor.h ampliado)
│   └── huffman.h threadpool.h filelock.h
├── src/
│   ├── editor/    editor.c editor_main.c
│   ├── huffman/   huffman.c threadpool.c filelock.c huffcli.c
│   └── shell/     main.c cat_*.c        (Parcial 1)
├── tests/
│   ├── test_editor.sh           66 verificaciones del Parcial 1
│   ├── test_huffman.sh          34 casos del compresor
│   ├── test_integracion.py      69 verificaciones de integración
│   └── test_valgrind.sh         memcheck + helgrind
├── docs/README_parcial1.md
└── extras/                      material de apoyo del Parcial 1
```
