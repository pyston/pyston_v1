import gc

unordered_finalize = {}
ordered_finalize_increasing = []
ordered_finalize_decreasing = []

class C(object):
    def __init__(self, index):
        self.index = index
    def __del__(self):
        unordered_finalize[self.index] = True

class D(object):
    def __init__(self, index, append_list):
        self.index = index
        self.ref = None
        self.append_list = append_list
    def __del__(self):
        self.append_list.append(self.index)

def foo():
    objs1 = [C(i) for i in xrange(20)]
    objs2 = [D(i, ordered_finalize_increasing) for i in xrange(20)]
    objs3 = [D(i, ordered_finalize_decreasing) for i in xrange(20)]
    for i in xrange(19):
        objs2[i].ref = objs2[i+1]
        objs3[i+1].ref = objs3[i]

def fact(n):
    if n <= 1:
        return n
    return n * fact(n-1)

foo()

fact(10) # try to clear some memory

# Need to GC a lot of times because of reference chains of finalizers.
for i in xrange(42):
    gc.collect()

print sorted(unordered_finalize.keys())
print ordered_finalize_increasing
print ordered_finalize_decreasing
