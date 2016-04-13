import os
import pty
import subprocess
import sys

def test(s, expected_code=0):
    pid, fd = pty.fork()
    if pid == 0:
        os.execl(sys.executable, sys.executable, '-S')

    written = 0
    # The same fd is used for reading and writing, so I'm not sure how to signal that it's closed
    # to the child but still have it be readable by the parent.
    s += '\nimport os\nos._exit(0)\n'
    while written < len(s):
        written += os.write(fd, s[written:])

    _, wcode = os.waitpid(pid, 0)

    print
    r = os.read(fd, 10240)
    lines = r.split('\n')
    while not (lines[0].startswith('Python') or lines[0].startswith('Pyston')):
        lines.pop(0)
    if lines[0].startswith('Python'):
        lines.pop(0)
    lines.pop(0)

    # Filter out syntax error location lines and make carets consistent:
    lines = [l.replace('>>> ', '>> ') for l in lines if l.strip() != '^']

    print '\n'.join(lines)

    assert os.WIFEXITED(wcode), wcode
    code = os.WEXITSTATUS(wcode)

    assert code == expected_code, "Expected %d, got %d" % (expected_code, code)

test("1")
test("1/0")
test("import sys; sys.exit(2)", 2)
test("import sys\nsys.exit(2)", 2)
test("import sys; sys.exit(\n2)", 2)
test("class C(object):\n    a=1\nprint C().a", 0)
test("class C(object):\n    a=1\n\nprint C().a", 0)
test("continue", 0)
test("break", 0)
# test("exec '+'", 0) # We don't get the traceback 100% right on this one
