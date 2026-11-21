#!/usr/bin/env python3
"""
Pruebas de INTEGRACION: editor + compresion Huffman en segundo plano.

Se maneja el binario del editor por tuberias (stdin/stdout) y se verifica:
  1. Bloqueo de archivo: editar (a/i/d/x) es rechazado mientras se comprime; leer (p, j) no.
  2. La interfaz NO se congela: el editor responde mientras la tarea corre.
  3. Progreso en tiempo real: lineas "[tarea #1] ... N%" llegan solas, sin teclear nada.
  4. Integridad: lo comprimido por el editor se descomprime identico (md5), tambien
     con la CLI independiente (huffcli).
  5. Senales: Ctrl-C cancela las tareas y el editor sigue vivo; SIGTERM sale limpio.
  6. 'q' con tareas en curso pide confirmacion; luego cancela y hace join.
  7. Fin de entrada (EOF): el editor ESPERA a que terminen las tareas.
  8. Descomprimir sobre el archivo abierto: edicion bloqueada y recarga al terminar.
  9. Varias tareas simultaneas, destino ocupado, origen == destino, archivo corrupto.
 10. Compatibilidad: el comando 'edit' del shell (eafitOS) con la entrada por tuberia.
 11. 'o' fallido conserva el archivo abierto (correccion del Parcial 1).
Despues de cada escenario no deben quedar temporales ni procesos.

Uso: python3 tests/test_integracion.py bin/editor bin/huffcli [bin/eafitOS]
Variables: BIG_MB (tamano del archivo grande, por defecto 100), TIME_SCALE (factor de
espera; se aumenta con ThreadSanitizer, que es ~10x mas lento).
"""
import hashlib, os, re, shutil, signal, subprocess, sys, tempfile, threading, time

EDITOR = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "bin/editor")
HUFCLI = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else "bin/huffcli")
SHELL = os.path.abspath(sys.argv[3]) if len(sys.argv) > 3 else os.path.join(os.path.dirname(EDITOR), "eafitOS")
BIG_MB = int(os.environ.get("BIG_MB", "100"))
SCALE = float(os.environ.get("TIME_SCALE", "1"))
ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")

tmp = tempfile.mkdtemp(prefix="integ_")
fails = 0
passed = 0


def check(cond, name, detail=""):
    global fails, passed
    if cond:
        passed += 1
        print("  [OK]   " + name)
    else:
        fails += 1
        print("  [FAIL] " + name + (("\n         " + detail) if detail else ""))


def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


class Ed:
    """Un proceso del editor con un hilo lector que acumula su salida con marcas de tiempo."""

    def __init__(self, cwd, args=("-q", "-j", "1", "-b", "64"), binary=None, argv_tail=()):
        self.buf = ""
        self.stamps = []  # (tiempo, longitud acumulada)
        self.lock = threading.Lock()
        self.p = subprocess.Popen([binary or EDITOR, *args, *argv_tail], cwd=cwd, stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.t = threading.Thread(target=self._read, daemon=True)
        self.t.start()

    def _read(self):
        fd = self.p.stdout.fileno()
        while True:
            try:
                d = os.read(fd, 65536)
            except OSError:
                break
            if not d:
                break
            with self.lock:
                self.buf += ANSI.sub("", d.decode("utf8", "replace"))
                self.stamps.append((time.time(), len(self.buf)))

    def text(self):
        with self.lock:
            return self.buf

    def mark(self):
        return len(self.text())

    def since(self, m):
        return self.text()[m:]

    def send(self, line):
        self.p.stdin.write((line + "\n").encode())
        self.p.stdin.flush()

    def wait_for(self, pattern, timeout=60, since=0):
        end = time.time() + timeout * SCALE
        rx = re.compile(pattern)
        while time.time() < end:
            if rx.search(self.text()[since:]):
                return True
            if self.p.poll() is not None:
                time.sleep(0.1)
                return bool(rx.search(self.text()[since:]))
            time.sleep(0.02)
        return False

    def cmd(self, line, wait=None, timeout=20):
        """Envia un comando y espera su respuesta (el siguiente prompt) o un patron."""
        m = self.mark()
        self.send(line)
        self.wait_for(wait or r"edit[^\n>]*> ?$", timeout, since=m) if wait else self._wait_prompt(m, timeout)
        return self.since(m)

    def _wait_prompt(self, m, timeout):
        end = time.time() + timeout * SCALE
        while time.time() < end:
            if re.search(r"edit[^\n]*> $", self.since(m)):
                return
            time.sleep(0.01)

    def close_stdin(self):
        try:
            self.p.stdin.close()
        except Exception:
            pass

    def finish(self, timeout=60):
        try:
            return self.p.wait(timeout=timeout * SCALE)
        except subprocess.TimeoutExpired:
            self.p.kill()
            return None

    def kill(self):
        if self.p.poll() is None:
            self.p.kill()
            self.p.wait()


def fresh(name):
    d = os.path.join(tmp, name)
    os.makedirs(d)
    return d


def leftovers(d, allowed):
    return sorted(f for f in os.listdir(d) if f not in allowed)


def run_huf(args):
    return subprocess.run([HUFCLI, *args], stdout=subprocess.PIPE, stderr=subprocess.STDOUT).returncode


# ---------------------------------------------------------------------------------------------
print("== Preparando datos (%d MB de texto)" % BIG_MB)
BASE = os.path.join(tmp, "base.txt")
line = ("0123456789 abcdefghijklmnopqrstuvwxyz concurrencia y sincronizacion " * 15)[:999] + "\n"
with open(BASE, "w") as f:
    for _ in range(BIG_MB * 1024):
        f.write(line)
BASE_MD5 = md5(BASE)


def workdir(name, files=("doc.txt",)):
    d = fresh(name)
    for fn in files:
        shutil.copy(BASE, os.path.join(d, fn))
    return d


# ---------------------------------------------------------------------------------------------
print("== 1. Bloqueo de archivo y UI que no se congela (compresion en curso)")
d = workdir("s1")
e = Ed(d)
e.cmd("o doc.txt")
t0 = time.time()
r = e.cmd("z")
t_start = time.time() - t0
check("Tarea #1 iniciada" in r, "'z' inicia la tarea en segundo plano", r)
check(t_start < 2 * SCALE, "'z' retorna de inmediato (%.0f ms), no espera a la compresion" % (t_start * 1000))
r = e.cmd("a texto nuevo")
check("esta en uso por una tarea" in r, "editar ('a') es RECHAZADO mientras se comprime", r)
r = e.cmd("i 1 otra")
check("esta en uso por una tarea" in r, "insertar ('i') es rechazado", r)
r = e.cmd("d 1")
check("esta en uso por una tarea" in r, "borrar ('d') es rechazado", r)
t0 = time.time()
r = e.cmd("p 1")
lat = time.time() - t0
check("0123456789" in r, "leer ('p 1') SI se permite durante la compresion", r)
check(lat < 2 * SCALE and e.p.poll() is None, "la interfaz responde mientras comprime (%.0f ms)" % (lat * 1000))
r = e.cmd("j")
check("comprimiendo" in r and "%" in r, "'j' muestra la tarea con su avance", r)
check(e.wait_for(r"\[tarea #1 OK\]", 120), "la tarea termina con OK")
r = e.cmd("a texto nuevo")
check("anadida" in r, "terminada la tarea, editar vuelve a estar permitido", r)
e.cmd("q")
check(e.finish() == 0, "el editor sale con codigo 0")
check(os.path.exists(os.path.join(d, "doc.txt.huf")), "se creo doc.txt.huf")
rc = run_huf(["d", os.path.join(d, "doc.txt.huf"), os.path.join(d, "back.txt"), "-q"])
check(rc == 0 and md5(os.path.join(d, "back.txt")) == BASE_MD5,
      "INTEGRIDAD: el .huf (hecho por el editor) se descomprime identico al original (md5)")
check(not leftovers(d, {"doc.txt", "doc.txt.huf", "back.txt"}), "sin temporales residuales")

# ---------------------------------------------------------------------------------------------
print("== 2. Progreso en tiempo real (sin teclear nada)")
d = workdir("s2")
e = Ed(d)
e.cmd("o doc.txt")
m = e.mark()
e.send("z")
ok = e.wait_for(r"\[tarea #1 OK\]", 120, since=m)
out = e.since(m)
decs = sorted(set(int(x) for x in re.findall(r"\[tarea #1\] comprimiendo 'doc.txt': (\d+)%", out)))
check(ok, "la tarea termina sola mientras el editor espera en poll()")
check(len(decs) >= 3, "el avance llega SOLO, sin pulsar teclas: %s%%" % decs, out[-400:])
check(decs == sorted(decs), "el avance es monotono creciente")
e.cmd("q")
check(e.finish() == 0, "salida limpia (codigo 0)")

# ---------------------------------------------------------------------------------------------
print("== 3. Senales")
d = workdir("s3")
e = Ed(d)
e.cmd("o doc.txt")
e.cmd("z")
time.sleep(0.3)
e.p.send_signal(signal.SIGINT)
check(e.wait_for(r"\[tarea #1 CANCELADA\]", 60), "Ctrl-C cancela la tarea en curso")
check(e.p.poll() is None, "el editor sigue vivo tras Ctrl-C con tareas")
r = e.cmd("p 1")
check("0123456789" in r, "y sigue respondiendo comandos", r)
check(not os.path.exists(os.path.join(d, "doc.txt.huf")), "no queda .huf parcial")
check(not leftovers(d, {"doc.txt"}), "no quedan temporales tras cancelar")
e.p.send_signal(signal.SIGINT)  # sin tareas: sale
check(e.finish() == 0, "Ctrl-C sin tareas cierra el editor ordenadamente (codigo 0)")

d = workdir("s3b")
e = Ed(d)
e.cmd("o doc.txt")
e.cmd("z")
time.sleep(0.3)
e.p.send_signal(signal.SIGTERM)
rc = e.finish()
check(rc == 0, "SIGTERM con una tarea en curso: salida ordenada, codigo %s (no muerte por senal)" % rc)
check(not leftovers(d, {"doc.txt"}), "sin .huf ni temporales tras SIGTERM")

d = workdir("s3c")
e = Ed(d)
e.cmd("o doc.txt")
e.cmd("z")
e.p.send_signal(signal.SIGHUP)
check(e.finish() == 0, "SIGHUP (cierre de terminal): salida ordenada")
check(not leftovers(d, {"doc.txt"}), "sin temporales tras SIGHUP")

# ---------------------------------------------------------------------------------------------
print("== 4. 'q' con tareas en curso")
d = workdir("s4")
e = Ed(d)
e.cmd("o doc.txt")
e.cmd("z")
r = e.cmd("q")
check("q' otra vez" in r and e.p.poll() is None, "primer 'q': avisa y NO sale", r)
e.send("q")
check(e.finish() == 0, "segundo 'q': cancela, espera a los hilos (join) y sale con codigo 0")
check(not leftovers(d, {"doc.txt"}), "sin temporales")

# ---------------------------------------------------------------------------------------------
print("== 5. Fin de la entrada (EOF) espera a las tareas")
d = workdir("s5")
e = Ed(d)
e.send("o doc.txt")
e.send("z")
e.close_stdin()
rc = e.finish(180)
check(rc == 0, "el editor termina con codigo 0")
check("esperando 1 tarea" in e.text() and "[tarea #1 OK]" in e.text(), "EOF no cancela: espera y reporta OK", e.text()[-300:])
rc = run_huf(["d", os.path.join(d, "doc.txt.huf"), os.path.join(d, "back.txt"), "-q"])
check(rc == 0 and md5(os.path.join(d, "back.txt")) == BASE_MD5, "integridad md5 tras la espera por EOF")

# ---------------------------------------------------------------------------------------------
print("== 6. Descomprimir SOBRE el archivo abierto")
d = workdir("s6")
rc = run_huf(["c", os.path.join(d, "doc.txt"), os.path.join(d, "snap.huf"), "-q"])
e = Ed(d)
e.cmd("o doc.txt")
e.cmd("a LINEA EXTRA QUE DEBE DESAPARECER")
r = e.cmd("u snap.huf doc.txt")
check("Tarea #1 iniciada" in r, "'u' inicia la descompresion en segundo plano", r)
r = e.cmd("a no debe entrar")
check("esta en uso por una tarea" in r, "editar el destino mientras se descomprime es RECHAZADO", r)
r = e.cmd("p 1")
check("0123456789" in r, "leer sigue permitido (ve una copia consistente del archivo viejo)", r)
check(e.wait_for(r"recargando", 120), "al terminar, el editor RECARGA el archivo reemplazado")
time.sleep(0.3)
r = e.cmd("m")
check("Lineas indexadas:   %d" % (BIG_MB * 1024) in r, "el indice del editor refleja el archivo restaurado (%d lineas)" % (BIG_MB * 1024), r[-500:])
check(md5(os.path.join(d, "doc.txt")) == BASE_MD5, "INTEGRIDAD: doc.txt restaurado identico al original (md5)")
r = e.cmd("a ya se puede editar")
check("anadida" in r, "terminada la tarea se puede editar de nuevo", r)
e.cmd("q")
check(e.finish() == 0, "salida limpia")
check(not leftovers(d, {"doc.txt", "snap.huf"}), "sin temporales")

# ---------------------------------------------------------------------------------------------
print("== 7. Varias tareas, destino ocupado, origen == destino")
d = workdir("s7")
e = Ed(d, args=("-q", "-j", "2", "-b", "128"))
e.cmd("o doc.txt")
r = e.cmd("z uno.huf")
check("Tarea #1" in r, "tarea #1")
r = e.cmd("z dos.huf")
check("Tarea #2" in r, "tarea #2 (dos lectores sobre el mismo archivo son compatibles)", r)
r = e.cmd("z uno.huf")
check("en uso por otra tarea" in r, "mismo destino que una tarea en curso: RECHAZADO", r)
r = e.cmd("z doc.txt")
check("no pueden ser el mismo archivo" in r, "origen == destino: RECHAZADO", r)
r = e.cmd("j")
check("#1" in r and "#2" in r, "'j' lista ambas tareas", r)
r = e.cmd("c 2")
check("Cancelando la tarea #2" in r, "'c 2' cancela solo la tarea #2", r)
check(e.wait_for(r"\[tarea #2 CANCELADA\]", 60), "la tarea #2 termina CANCELADA")
check(e.wait_for(r"\[tarea #1 OK\]", 180), "la tarea #1 termina OK")
r = e.cmd("c")
check("no hay tareas en curso" in r, "'c' sin tareas da un mensaje claro", r)
e.cmd("q")
check(e.finish() == 0, "salida limpia")
check(os.path.exists(os.path.join(d, "uno.huf")) and not os.path.exists(os.path.join(d, "dos.huf")),
      "uno.huf existe; dos.huf (cancelada) NO existe")
check(not leftovers(d, {"doc.txt", "uno.huf"}), "sin temporales")

# ---------------------------------------------------------------------------------------------
print("== 8. Errores")
d = fresh("s8")
with open(os.path.join(d, "ok.txt"), "w") as f:
    f.write("hola\n" * 5000)
run_huf(["c", os.path.join(d, "ok.txt"), os.path.join(d, "ok.huf"), "-q"])
data = bytearray(open(os.path.join(d, "ok.huf"), "rb").read())
data[300] ^= 0xFF
open(os.path.join(d, "corrupto.huf"), "wb").write(data)
open(os.path.join(d, "falso.huf"), "w").write("esto no es un huf")
e = Ed(d)
e.cmd("o ok.txt")
e.cmd("u corrupto.huf salida1.txt")
check(e.wait_for(r"\[tarea #1 ERROR\]", 60), "archivo .huf corrupto: la tarea termina con ERROR (CRC)")
check(not os.path.exists(os.path.join(d, "salida1.txt")), "sin salida parcial")
e.cmd("u falso.huf salida2.txt")
check(e.wait_for(r"\[tarea #2 ERROR\].*no es un archivo .huf", 60), "archivo que no es .huf: ERROR claro")
r = e.cmd("u no_existe.huf x.txt")
check("no se puede leer" in r or "no existe" in r.lower() or "No such file" in r, "origen inexistente: error inmediato", r)
r = e.cmd("p 1")
check("hola" in r, "tras los errores el editor sigue funcionando", r)
r = e.cmd("o /no/existe/x.txt")
r2 = e.cmd("p 1")
check("hola" in r2, "'o' fallido CONSERVA el archivo abierto (correccion del Parcial 1)", r + r2)
e.cmd("q")
check(e.finish() == 0, "salida limpia")
check(not leftovers(d, {"ok.txt", "ok.huf", "corrupto.huf", "falso.huf"}), "sin temporales")

# ---------------------------------------------------------------------------------------------
print("== 9. Compatibilidad con el shell (eafitOS): 'edit' + comandos posteriores por tuberia")
if os.path.exists(SHELL):
    d = fresh("s9")
    # 9a: la tarea sigue en curso al llegar el primer 'q' => pide confirmacion; el segundo cancela.
    p = subprocess.run([SHELL], cwd=d, input=b"edit f.txt\na hola\nz\nq\nq\nhora\nexit\n",
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60 * SCALE)
    out = ANSI.sub("", p.stdout.decode("utf8", "replace"))
    check("Tarea #1 iniciada" in out, "comando 'z' disponible dentro de 'edit' en el shell", out[-300:])
    check("Escribe 'q' otra vez" in out, "el primer 'q' con una tarea en curso pide confirmacion", out[-300:])
    check("De vuelta en el shell" in out and "[syscall] time" in out,
          "tras el segundo 'q' el shell CONSERVA las lineas siguientes (comparte el buffer de stdin)", out[-300:])
    check(not leftovers(d, {"f.txt"}), "sin .huf ni temporales tras cancelar desde el shell")
    # 9b: sin tareas, 'q' sale de inmediato y el shell sigue leyendo la misma tuberia.
    p = subprocess.run([SHELL], cwd=d, input=b"edit g.txt\na hola\nq\nhora\nexit\n",
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60 * SCALE)
    out = ANSI.sub("", p.stdout.decode("utf8", "replace"))
    check("De vuelta en el shell" in out and "[syscall] time" in out, "sin tareas: 'q' vuelve al shell y 'hora' se ejecuta", out[-300:])
    check(open(os.path.join(d, "g.txt")).read() == "hola\n", "el editor escribio g.txt correctamente desde el shell")
else:
    print("  (omitido: no se encontro %s)" % SHELL)

# ---------------------------------------------------------------------------------------------
shutil.rmtree(tmp, ignore_errors=True)
print("\nResultado: %d OK, %d FAIL" % (passed, fails))
sys.exit(1 if fails else 0)
