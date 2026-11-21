#!/usr/bin/env bash
# Verifica con valgrind el EDITOR con tareas en segundo plano y la CLI:
#   memcheck : fugas de memoria, accesos invalidos, descriptores abiertos (--track-fds)
#   helgrind : carreras de datos y orden de adquisicion de locks entre hilos
# Uso: bash tests/test_valgrind.sh [bin/editor] [bin/huffcli]
set -u
ED=$(readlink -f "${1:-bin/editor}")
CLI=$(readlink -f "${2:-bin/huffcli}")
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT; cd "$T" || exit 1
yes "linea de prueba para valgrind" | head -c 1500000 > a.txt
cp a.txt orig.txt
head -c 400000 /dev/urandom > r.bin
VG="valgrind -q --error-exitcode=9"
MC="$VG --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=all --track-fds=yes"
fail=0
ok()  { echo "  [OK]   $1"; }
bad() { echo "  [FAIL] $1"; sed 's/^/         /' "$2" | head -25; fail=1; }

run() { # nombre herramienta entrada...
    local name=$1 tool=$2 input=$3; shift 3
    printf "$input" | $tool "$ED" -q -j 3 -b 64 "$@" > out.txt 2> err.txt
    local rc=$?
    # valgrind escribe sus informes en stderr con el prefijo '=='; los mensajes de error
    # normales del editor (p. ej. un origen inexistente) tambien van a stderr y NO son fallos.
    if [ $rc -eq 0 ] && ! grep -q '^==' err.txt; then ok "$name"; else bad "$name (codigo $rc)" err.txt; fi
}

echo "== memcheck (fugas, fds, accesos invalidos)"
run "editor: comprimir y esperar por EOF"        "$MC" 'o a.txt\nz a.huf\n'
"$CLI" d a.huf chk.txt -q && cmp -s chk.txt orig.txt && ok "el .huf hecho bajo valgrind se descomprime identico (cmp)" || { echo "  [FAIL] integridad"; fail=1; }
run "editor: descomprimir sobre el archivo abierto (recarga)" "$MC" 'o a.txt\na linea extra\nu a.huf a.txt\n'
cmp -s a.txt orig.txt && ok "a.txt restaurado identico al original (cmp)" || { echo "  [FAIL] restauracion"; fail=1; }
run "editor: cancelar con 'q q' (cancel + join)"  "$MC" 'o a.txt\nz b.huf\nq\nq\n'
run "editor: dos tareas, cancelar una con 'c'"    "$MC" 'o a.txt\nz c1.huf\nz c2.huf\nc 2\nj\n'
run "editor: archivo binario aleatorio"           "$MC" 'o r.bin\nz r.huf\n'
run "editor: errores (corrupto, inexistente)"     "$MC" 'o a.txt\nu no_existe.huf x\nu a.txt y\n'

echo "== helgrind (carreras de datos, orden de locks)"
run "editor + hilos de la tarea (comprimir)"      "$VG --tool=helgrind" 'o a.txt\nz h.huf\n'
run "editor + hilos de la tarea (descomprimir)"   "$VG --tool=helgrind" 'o a.txt\nu a.huf h.txt\n'
run "editor + cancelacion"                        "$VG --tool=helgrind" 'o a.txt\nz h2.huf\nc\n'

[ $fail -eq 0 ] && echo "valgrind: TODO LIMPIO" || echo "valgrind: HAY PROBLEMAS"
exit $fail
