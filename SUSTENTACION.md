# Guion de sustentación — Editor con compresión Huffman concurrente

Formato: video grabado (10–12 min). Estructura: problema → diseño → demo → pruebas → cierre.
Regla de oro del enunciado: *"presentar y defender rigurosamente los argumentos desde la
perspectiva de la ingeniería"*. Para cada decisión: **qué elegí, qué alternativa descarté, qué
se gana y qué se pierde.**

## 1. Problema (1 min)
- Comprimir un archivo grande **sin congelar el editor**, aprovechando todos los núcleos, con
  un resultado **idéntico** al original al descomprimir y **el mismo `.huf` con 1 o con N hilos**.
- Retos de sistemas operativos: reparto de trabajo, sincronización sin espera activa, orden de
  salida, cancelación limpia, señales, y **condiciones de carrera entre el editor y el archivo**.
- Punto de partida: el editor del Parcial 1 (REPL por líneas, *write-through* a disco).

## 2. Diseño del compresor (3 min) — dibujar el diagrama del README §3
1. **Bloques de 1 MiB** = unidad de trabajo independiente.
2. **Conteo paralelo** con histograma local + una suma bajo mutex por bloque (de un lock por
   byte a uno por bloque ⇒ casi sin contención).
3. **Barrera** con variable de condición → árbol global (secuencial: solo 256 símbolos) →
   **códigos canónicos** (solo se guardan las longitudes).
4. **Codificación paralela** → `slots[idx]` → **escritor único** en orden. **Ventana** de
   `2N+2` bloques: memoria acotada y garantía de progreso.
5. **Commit atómico**: `tmp` + `fsync` + `rename`. Cancelar o fallar no deja rastro.
6. **CRC32 por bloque**: detecta corrupción y truncamiento al descomprimir.

## 3. Diseño de la integración (3 min) — lo que es nuevo respecto al compresor solo
1. **Modelo de hilos:** el hilo del REPL es dueño único del editor; los hilos de la tarea solo
   tocan archivos. Un único canal hacia la UI: un pipe (`evpipe`).
2. **`poll()` sobre {teclado, evpipe, señales}:** la UI duerme (0 % CPU) y se despierta por
   cualquiera de los tres. Sin temporizadores ni sondeo.
3. **Bloqueo lectores-escritor por archivo**, *no bloqueante*: comprimir = lector, editar =
   escritor. Si no se concede, se **rechaza** el comando (esperar congelaría la UI).
4. **Señales:** handler mínimo (`write` a un pipe), lógica en el hilo de la UI; señales
   bloqueadas en los hilos de trabajo; manejadores restaurados al volver al shell.
5. **Descomprimir sobre el archivo abierto:** `rename` atómico ⇒ el descriptor del editor queda
   apuntando al inodo viejo ⇒ al terminar se detecta (inodo del fd ≠ inodo de la ruta) y se recarga.

## 4. Demo (3 min) — comandos exactos
Preparar antes: `yes "linea de texto para la demo de compresion" | head -c 300000000 > grande.txt`
```
make && ./bin/editor -j 1 grande.txt     # -j 1: la tarea dura lo bastante para verse
z                                         # inicia en segundo plano; el prompt vuelve al instante
a esto se rechaza                         # RECHAZADO: archivo en uso (bloqueo de escritura)
p 1                                       # permitido: lectura mientras se comprime
j                                         # barra de avance
(esperar sin teclear)                     # llegan solas las líneas "[tarea #1] ... 40%"
a ahora si                                # tras terminar, se puede editar
u grande.txt.huf copia.txt                # descompresión en segundo plano
z otro.huf   y luego  Ctrl-C              # cancela: no queda .huf ni temporales
z x.huf   y luego  q  q                   # 'q' avisa; el segundo cancela, hace join y sale
```
Fuera del editor: `md5sum grande.txt copia.txt` (iguales) y `make test`.

## 5. Evidencias (1 min)
`make test` (66 + 34 + 69 verificaciones), `make test-tsan` (0 reportes de carreras),
`make valgrind` (memcheck + helgrind limpios, `--track-fds`). Mencionar el hallazgo real de
ThreadSanitizer durante el desarrollo: el campo `total` (leído por la UI, escrito por el
coordinador) era una carrera de datos; se corrigió haciéndolo atómico.

## 6. Preguntas probables y respuestas cortas

**¿Por qué no hay interbloqueo?**
Nunca se sostienen dos mutex a la vez; los workers solo esperan la cola; la ventana siempre
incluye el bloque que el escritor espera y la cola es FIFO; y los bloqueos de archivo no
bloquean (devuelven error), con orden fijo origen→destino.

**¿Por qué no hay espera activa?**
Todo es `cond_wait`, `poll` sin timeout o `read` de pipe. No hay `sleep` ni sondeo.

**¿Cómo evitas despertares perdidos (*lost wakeups*)?**
El predicado se evalúa bajo el mismo mutex con el que se hace `broadcast`. Para cancelar: se
activa el átomico y luego `lock + broadcast`. El REPL vacía el pipe *antes* de leer el estado.

**¿Qué pasa si el usuario edita mientras se comprime?**
Se rechaza (el archivo se desplaza en su sitio al insertar/borrar y la tarea leería datos a
medias). Leer sí se permite. Alternativa descartada: encolar la edición — complica el orden
de los números de línea; o tomar una copia del archivo — duplica el I/O.

**¿Por qué rechazar y no esperar?**
Esperar con un mutex congelaría la interfaz, que el enunciado prohíbe.

**¿Por qué un registro de bloqueos propio y no `flock`?**
Se necesita semántica no bloqueante con mensaje a la UI y cubrir "el archivo abierto en el
editor". `flock` es consultivo entre procesos y no distingue qué parte del programa lo usa.
Límite honesto: el registro es por proceso; otro proceso no lo respeta (se *detecta* la
modificación al final con tamaño/inodo/mtime, pero no se impide). `flock` sería la extensión.

**¿Por qué `poll()` y no un hilo que lea el teclado?**
Un hilo lector necesitaría su propia sincronización con el REPL y haría que `Editor` fuera
compartido. Con `poll()` el editor sigue siendo de **un solo hilo**: cero carreras sobre el buffer.

**¿Por qué un pipe para las señales (*self-pipe*)?**
En un handler solo se pueden usar funciones async-signal-safe (`write` lo es; `printf` o
`pthread_mutex_lock` no). El handler escribe un byte y la lógica corre en el REPL.

**¿Por qué bloqueas las señales en los hilos de trabajo?**
Para que siempre las reciba el hilo de la UI; si no, el kernel puede entregarlas a cualquier hilo.

**¿Por qué sigues usando `getc(stdin)` en vez de `read(0)`?**
El shell comparte el mismo `FILE*`. Si el editor leyera con `read` directamente, las líneas que
el shell ya tenía en su buffer (entrada por tubería) se perderían. Limitación: depende de un
campo de glibc para saber si hay datos en el buffer.

**¿Qué pasa con Ctrl-D o una tubería que termina?**
El editor espera a las tareas en curso (no las cancela) y sale; Ctrl-C las cancela.

**¿Qué hace `u` sobre el archivo abierto?**
Bloqueo de escritura en el destino (editar se rechaza), `rename` atómico al terminar y recarga
del editor al detectar que el inodo del descriptor ya no es el de la ruta.

**¿El resultado depende del número de hilos?**
No: desempates deterministas en el árbol y salida por índice de bloque (probado: 1 vs 8 hilos).

**¿Qué mides para decir "concurrente"?**
Mostrar `time` de `huffcli c` con `-j 1` y `-j N` en tu máquina. Mis medidas (2 núcleos, 300 MB): comprimir 3,8 s → 2,0 s (≈ 1,9×); el `.huf` es idéntico. Explicar el límite: el árbol y la escritura ordenada son secuenciales (Amdahl).

**¿Qué cambiaste del Parcial 1?**
Ver README §7. Cambios mínimos y localizados; 66/66 pruebas originales (2 adaptadas con
justificación). Corregí dos defectos de paso (`o` fallido cerraba el archivo actual; `s` con NUL).

**¿Qué mejorarías?**
Barra animada con terminal en modo *raw*, `flock` para otros procesos, decodificación por tabla,
limitar longitud de código (package-merge), y encolar ediciones en vez de rechazarlas.

## 7. Antes de entregar (checklist)
- [ ] `make clean && make` sin warnings; `make test` en verde (mostrarlo en el video).
- [ ] Leer `src/huffman/huffman.c` y la tabla del README §4 hasta poder explicar cada mutex.
- [ ] Probar la demo del §4 en TU terminal (no solo en las pruebas automáticas).
- [ ] Revisar la política del curso sobre uso de herramientas de IA y cumplirla.
- [ ] Citar que la base (editor y shell) es el Parcial 1 del equipo.
- [ ] Subir código + README + video a EAFIT Interactiva (**semana 12**).
