import glob
import os
import subprocess

LIB_DIR = "from_cpython/Lib"
EXE_PATH = "./pyston_dbg"

modules = []
for fn in glob.glob("%s/*.py" % LIB_DIR):
    modules.append(os.path.basename(fn)[:-3])

for fn in glob.glob("%s/*" % LIB_DIR):
    if not os.path.isdir(fn):
        continue
    modules.append(os.path.basename(fn))
print modules

nworked = 0
total = 0
print len(modules)
f = open("failures.txt", 'w')
for m in modules:
    if '-' in m:
        continue
    p = subprocess.Popen([EXE_PATH, "-q"], stdin=subprocess.PIPE, stderr=subprocess.PIPE)
    # We need to make pyston exit with a non-zero return code if an exception was thrown,
    # so use this little dance.  If the import succeeds, then we call sys.exit(0), otherwise
    # we skip to the next line and call sys.exit(1)
    p.stdin.write("import sys\n")
    p.stdin.write("import %s; sys.exit(0)\n" % m)
    p.stdin.write("sys.exit(1)\n")
    p.stdin.close()
    err = p.stderr.read()
    code = p.wait()
    if code == 0:
        print m, "worked",
        nworked += 1
    else:
        print code
        print m, "failed",
        print >>f, m
        print >>f, '\n'.join(err.split('\n')[-4:])
        f.flush()
    total += 1
    print "%d/%d" % (nworked, total)
