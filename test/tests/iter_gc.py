# Regression test: make sure we GC special iterator objects correctly

import gc

class C(object):
    def f(self, i):
        return i * i
    def __getitem__(self, i):
        if i < 200:
            return self.f(i)
        raise IndexError(i)

for i in C():
    gc.collect()
    print i

class C2(object):
    def __init__(self):
        self.n = 0

    def __iter__(self):
        return self

    def f(self):
        self.n += 1
        return self.n * 2
    def next(self):
        if self.n < 200:
            return self.f()

        raise StopIteration()

for i in C2():
    gc.collect()
    print i
