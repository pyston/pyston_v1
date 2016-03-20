# skip-if: '-O' in EXTRA_JIT_ARGS or '-n' in EXTRA_JIT_ARGS
# statcheck: 4 == noninit_count('num_deopt')
# this used to hit an abort in our LLVM tier codegen
try:
    import __pyston__
    __pyston__.setOption("OSR_THRESHOLD_BASELINE", 50)
    __pyston__.setOption("REOPT_THRESHOLD_BASELINE", 50)
    __pyston__.setOption("SPECULATION_THRESHOLD", 10)
except ImportError:
    pass

from thread import allocate_lock

def triggers_deopt(x):
    if x < 90:
        return ""
    return unicode("")

class C():
    def __init__(self):
        self.l = allocate_lock()
 
    def f(self, x):
        with self.l:
            triggers_deopt(x).isalnum

c = C()
for i in range(100):
    c.f(i)
