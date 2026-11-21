#!/usr/bin/env bash
# Pruebas de corrección: compresión -> descompresión debe reproducir el original
# bit a bit (md5 + cmp), con distintos nº de hilos y tamaños de bloque, más
# casos de error (archivo corrupto, truncado, inexistente) y cancelación.
#
# Uso: bash tests/test_huffman.sh [ruta/a/huffcli]
set -u
CLI=${1:-bin/huffcli}
CLI=$(readlink -f "$CLI")
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
cd "$T" || exit 1

pass=0; fail=0
ok()  { pass=$((pass+1)); echo "  [OK]   $1"; }
bad() { fail=$((fail+1)); echo "  [FAIL] $1"; }

roundtrip() { # archivo hilos bloque_KiB
    local f=$1 j=$2 b=$3
    "$CLI" c "$f" "$f.huf" -q -j "$j" -b "$b" || { bad "compress $f j=$j b=$b"; return; }
    "$CLI" d "$f.huf" "$f.out" -q -j "$j"      || { bad "decompress $f j=$j b=$b"; return; }
    local m1 m2
    m1=$(md5sum < "$f"); m2=$(md5sum < "$f.out")
    if [ "$m1" = "$m2" ] && cmp -s "$f" "$f.out"; then
        ok "$f j=$j b=${b}K ($(stat -c%s "$f") -> $(stat -c%s "$f.huf") bytes)"
    else
        bad "md5/cmp distinto: $f j=$j b=$b"
    fi
    rm -f "$f.huf" "$f.out"
}

echo "== Datos de prueba"
: > empty.bin
printf 'x' > one.bin
printf 'hola mundo\n' > small.txt
head -c 500000 /dev/zero > zeros.bin
head -c 4096 /dev/urandom > rnd4k.bin
head -c 8000000 /dev/urandom > rnd8m.bin
yes "Concurrencia y sincronizacion en sistemas operativos" | head -c 30000000 > text30m.txt
python3 -c "
import random
random.seed(7)
s=random.choices(range(256),weights=[0.5**(i%40) +1e-9 for i in range(256)],k=3000000)
open('skew.bin','wb').write(bytes(s))
"
# tamaño exacto múltiplo del bloque y múltiplo+1
head -c $((64*1024*5)) /dev/urandom > exact.bin
head -c $((64*1024*5+1)) /dev/urandom > exactp1.bin

echo "== Ida y vuelta (integridad md5 + cmp)"
for f in empty.bin one.bin small.txt zeros.bin rnd4k.bin exact.bin exactp1.bin skew.bin; do
    for j in 1 4; do roundtrip $f $j 64; done
done
for j in 1 2 8; do
    roundtrip rnd8m.bin $j 256
    roundtrip text30m.txt $j 1024
done
roundtrip text30m.txt 3 4   # bloque minimo (4 KiB): miles de bloques

echo "== Determinismo: mismo resultado con 1 y 8 hilos"
"$CLI" c text30m.txt a1.huf -q -j 1 -b 512
"$CLI" c text30m.txt a8.huf -q -j 8 -b 512
if cmp -s a1.huf a8.huf; then ok "salida identica independiente del nº de hilos (orden correcto)"; else bad "salida difiere entre 1 y 8 hilos"; fi

echo "== Errores"
if "$CLI" c no_existe.bin x.huf -q 2>/dev/null; then bad "origen inexistente deberia fallar"; else ok "origen inexistente -> error"; fi
[ -e x.huf ] && bad "dejo salida parcial x.huf" || ok "sin archivo parcial tras error"

"$CLI" c text30m.txt t.huf -q -j 4 -b 256
cp t.huf corrupt.huf
printf '\xff\xff\xff\xff' | dd of=corrupt.huf bs=1 seek=100000 conv=notrunc 2>/dev/null
if "$CLI" d corrupt.huf c.out -q 2>/dev/null; then bad "archivo corrupto no detectado"; else ok "corrupcion detectada (CRC/decodificacion)"; fi
ls c.out* 2>/dev/null | grep -q . && bad "dejo salida parcial tras corrupcion" || ok "sin salida parcial tras corrupcion"

head -c 50000 t.huf > trunc.huf
if "$CLI" d trunc.huf tr.out -q 2>/dev/null; then bad "archivo truncado no detectado"; else ok "truncamiento detectado"; fi
printf 'esto no es un huf' > fake.huf
if "$CLI" d fake.huf f.out -q 2>/dev/null; then bad "magic invalido no detectado"; else ok "magic invalido detectado"; fi

echo "== Cancelacion (SIGINT en mitad de la compresion)"
python3 - <<'PY'
import os
with open('big.bin','wb') as f:
    b = os.urandom(1<<20)
    for _ in range(int(os.environ.get('BIG_MB','150'))): f.write(b)
PY
"$CLI" c big.bin big.huf -q -j 2 -b 64 &
pid=$!
sleep 0.4
kill -INT $pid
wait $pid; rc=$?
if [ $rc -eq 130 ] || [ $rc -eq 0 ]; then ok "cancelacion -> codigo $rc"; else bad "codigo inesperado $rc"; fi
if [ $rc -eq 130 ]; then
    ls big.huf* 2>/dev/null | grep -q . && bad "dejo archivos tras cancelar" || ok "sin temporales tras cancelar"
fi

echo "== Origen modificado mientras se comprime"
"$CLI" c big.bin big2.huf -q -j 1 -b 64 2>/dev/null &
pid=$!
sleep 0.3
printf 'x' >> big.bin
wait $pid; rc=$?
if [ $rc -eq 1 ]; then ok "la modificacion concurrente del origen se detecta (codigo 1)"; else bad "modificacion concurrente: codigo inesperado $rc"; fi
ls big2.huf* 2>/dev/null | grep -q . && bad "dejo salida tras detectar la modificacion" || ok "sin salida parcial ni temporales"

echo
echo "Resultado: $pass OK, $fail FAIL"
[ $fail -eq 0 ]
