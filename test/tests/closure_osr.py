# statcheck: '-L' in EXTRA_JIT_ARGS or 1 <= stats['num_osr_exits'] <= 5

def f(x):
    def inner():
        t = 0
        for i in xrange(20000):
            t += x
        return t

    return inner

f = f(5)
print f()

