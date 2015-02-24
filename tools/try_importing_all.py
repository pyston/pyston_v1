import glob
import os
import subprocess

modules = []
for fn in glob.glob("from_cpython/Lib/*.py"):
    modules.append(os.path.basename(fn)[:-3])

for fn in glob.glob("from_cpython/Lib/*"):
    if not os.path.isdir(fn):
        continue
    modules.append(os.path.basename(fn))
print modules

nworked = 0
total = 0
print len(modules)
for m in modules:
    p = subprocess.Popen(["./pyston_dbg"], stdin=subprocess.PIPE)
    p.stdin.write("import %s\n" % m)
    p.stdin.close()
    code = p.wait()
    if code == 0:
        print m, "worked",
        nworked += 1
    else:
        print code
        print m, "failed",
    total += 1
    print "%d/%d" % (nworked, total)
