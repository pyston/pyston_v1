import os.path
import sys
import subprocess

PYSTON_DIR = os.path.join(os.path.dirname(__file__), '..')
STDMODULE_DIR = os.path.join(PYSTON_DIR, "from_cpython/Modules")

def find_module_source(module_name):
    for pattern in ("%smodule.c", "%s.c"):
        src = pattern % module_name
        if os.path.exists(os.path.join(STDMODULE_DIR, src)):
            return src
    print "couldn't find", module_name
    return None

def main():
    already_supported = ('__builtin__', '_collections', '_functools', '_md5', '_random', '_sha', '_sha256', '_sha512', '_sre', '_struct', 'binascii', 'datetime', 'errno', 'fcntl', 'gc', 'itertools', 'math', 'operator', 'posix', 'pwd', 'resource', 'select', 'signal', 'sys', 'thread', 'time')
    for module_name in sys.builtin_module_names:
        if module_name.startswith("__"):
            continue
        if module_name in already_supported:
            continue

        src = find_module_source(module_name)
        if not src:
            continue

        args = ["make", "EXTRA_STDMODULE_SRCS=%s" % src, "ERROR_LIMIT=0"]
        print ' '.join(args), "> %s.log" % module_name
        log_fn = "%s.log" % module_name
        f = open(log_fn, 'w')
        p = subprocess.Popen(args, cwd=PYSTON_DIR, stdout=f, stderr=subprocess.STDOUT)
        code = p.wait()
        f.close()
        if code == 0:
            print module_name, "worked!"
        else:
            print module_name, "didn't work",
            s = open(log_fn).read()
            if "undefined reference to" in s:
                print "Compiled but didn't link, %d link errors" % (s.count('\n') - 3)
            else:
                print s.split('\n')[-3]

if __name__ == "__main__":
    main()
