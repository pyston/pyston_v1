# skip-if: '-O' in EXTRA_JIT_ARGS or '-n' in EXTRA_JIT_ARGS
# statcheck: noninit_count('num_deopt') == 2

try:
    import __pyston__
    __pyston__.setOption("OSR_THRESHOLD_BASELINE", 50)
    __pyston__.setOption("REOPT_THRESHOLD_BASELINE", 50)
    __pyston__.setOption("SPECULATION_THRESHOLD", 10)
except ImportError:
    pass

def triggers_deopt(x):
    if x < 90:
        return ""
    return unicode("")

class C():
    def f(self, x, b):
        if b and x < 90:
            y = 1
        triggers_deopt(x).isalnum
        try:
            print y
            assert b
        except UnboundLocalError as e:
            print e

c = C()
for i in range(91):
    c.f(i, True)

c = C()
for i in range(91):
    c.f(i, False)
