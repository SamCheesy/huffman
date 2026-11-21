# =====================================================================================
# Makefile - Shell Educativo + Editor de Texto CLI + Compresor Huffman concurrente
# Universidad EAFIT - Sistemas Operativos - Parcial 2 (Alternativa 1)
# Base: Parcial 1 (editor con syscalls POSIX) + modulo de compresion en segundo plano
# =====================================================================================
#
# Targets:
#   make               compila bin/eafitOS, bin/editor y bin/huffcli
#   make run           compila y ejecuta el shell integrado
#   make editor        compila solo el editor autonomo
#   make test          pruebas del Parcial 1 + Huffman + integracion (concurrencia)
#   make test-p1       solo las pruebas originales del editor (66 verificaciones)
#   make test-huffman  integridad md5/cmp, errores y cancelacion del compresor (CLI)
#   make test-integ    editor + tareas en segundo plano (bloqueo, progreso, senales)
#   make tsan          recompila con ThreadSanitizer en bin-tsan/ (carreras de datos)
#   make test-tsan     todas las pruebas con ThreadSanitizer (debe haber 0 reportes)
#   make valgrind      memcheck (fugas, fds) y helgrind (carreras, locks)
#   make clean         elimina binarios, objetos y temporales
# =====================================================================================

CC      = gcc
# gnu11: el modulo de Huffman usa _Atomic / <stdatomic.h> (C11).
# -pthread: hilos POSIX (compilacion y enlace).
CFLAGS  = -Wall -Wextra -std=gnu11 -g -D_GNU_SOURCE -Iinclude -pthread
SAN     =
CFLAGS += $(if $(SAN),-fsanitize=$(SAN) -fno-omit-frame-pointer -O1,)

# NOTA: no se usa una variable llamada SHELL (en GNU Make es especial).
BIN_DIR    = bin
OBJ_DIR    = build
BIN_SHELL  = $(BIN_DIR)/eafitOS
BIN_EDITOR = $(BIN_DIR)/editor
BIN_HUFF   = $(BIN_DIR)/huffcli

# Modulo de compresion (sin dependencias del editor ni del shell)
SRCS_HUFF   = src/huffman/huffman.c src/huffman/threadpool.c src/huffman/filelock.c

# El motor del editor (editor.o) y el modulo Huffman se enlazan en AMBOS binarios.
SRCS_SHELL  = src/shell/main.c src/shell/cat_datos.c src/shell/cat_memoria.c \
              src/shell/cat_monitoreo.c src/shell/cat_util.c src/shell/cat_edicion.c \
              src/editor/editor.c $(SRCS_HUFF)
SRCS_EDITOR = src/editor/editor_main.c src/editor/editor.c $(SRCS_HUFF)
SRCS_CLI    = src/huffman/huffcli.c src/huffman/huffman.c src/huffman/threadpool.c

OBJS_SHELL  = $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(SRCS_SHELL))
OBJS_EDITOR = $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(SRCS_EDITOR))
OBJS_CLI    = $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(SRCS_CLI))

all: $(BIN_SHELL) $(BIN_EDITOR) $(BIN_HUFF)

editor: $(BIN_EDITOR)

$(BIN_SHELL): $(OBJS_SHELL) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^

$(BIN_EDITOR): $(OBJS_EDITOR) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^

$(BIN_HUFF): $(OBJS_CLI) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^

# Regla generica: cada .c de src/ produce su .o en build/, respetando subcarpetas.
# Cualquier cabecera que cambie fuerza la recompilacion (dependencia amplia y segura).
HEADERS = $(wildcard include/*.h)
$(OBJ_DIR)/%.o: src/%.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN_DIR):
	@mkdir -p $(BIN_DIR)

# Compilar y ejecutar quedan separados a proposito.
run: $(BIN_SHELL)
	./$(BIN_SHELL)

# ---------------------------------------------------------------------------------
# Pruebas. Los scripts se invocan con 'bash' porque un zip puede perder el bit de
# ejecucion y 'make test' fallaria con "Permission denied".
# ---------------------------------------------------------------------------------
test: test-p1 test-huffman test-integ

test-p1: $(BIN_EDITOR)
	bash tests/test_editor.sh

test-huffman: $(BIN_HUFF)
	bash tests/test_huffman.sh $(BIN_HUFF)

test-integ: $(BIN_EDITOR) $(BIN_HUFF)
	python3 tests/test_integracion.py $(BIN_EDITOR) $(BIN_HUFF)

# Variante instrumentada con ThreadSanitizer (binarios en bin-tsan/, objetos en build-tsan/)
tsan:
	$(MAKE) BIN_DIR=bin-tsan OBJ_DIR=build-tsan SAN=thread all

test-tsan: tsan
	BIG_MB=12 bash tests/test_huffman.sh bin-tsan/huffcli
	BIG_MB=12 TIME_SCALE=10 TSAN_OPTIONS="halt_on_error=0 log_path=$(CURDIR)/bin-tsan/tsan-report" python3 tests/test_integracion.py bin-tsan/editor bin-tsan/huffcli
	@if ls bin-tsan/tsan-report* >/dev/null 2>&1; then echo "TSan: HAY REPORTES (ver bin-tsan/tsan-report*)"; exit 1; \
	 else echo "TSan: 0 reportes de carreras de datos"; fi

valgrind: $(BIN_EDITOR) $(BIN_HUFF)
	bash tests/test_valgrind.sh $(BIN_EDITOR) $(BIN_HUFF)

# Archivos de demostracion creados a mano durante las verificaciones del Parcial 1.
DEMO_FILES = demo.txt desde_shell.txt verif.txt prueba_trunc.txt prueba_nl.txt

clean:
	rm -rf bin build bin-tsan build-tsan pruebas_editor pruebas_huffman
	rm -f $(DEMO_FILES)

.PHONY: all editor run test test-p1 test-huffman test-integ tsan test-tsan valgrind clean
