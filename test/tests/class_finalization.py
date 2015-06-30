from testing_helpers import test_gc

unordered_finalize = {}
ordered_finalize_increasing = []
ordered_finalize_decreasing = []

class ObjWithFinalizer(object):
    def __init__(self, index):
        self.index = index
    def __del__(self):
        unordered_finalize[self.index] = True

class ObjWithFinalizerAndRef(object):
    def __init__(self, index, append_list):
        self.index = index
        self.ref = None
        self.append_list = append_list
    def __del__(self):
        self.append_list.append(self.index)

def scope1():
    # No ordering guarantees.
    objs1 = [ObjWithFinalizer(i) for i in xrange(20)]

items_in_list = 8

def scope2():
    objs2 = [ObjWithFinalizerAndRef(i, ordered_finalize_increasing) for i in xrange(items_in_list)]
    for i in xrange(items_in_list - 1):
        objs2[i].ref = objs2[i+1]

def scope3():
    objs3 = [ObjWithFinalizerAndRef(i, ordered_finalize_decreasing) for i in xrange(items_in_list)]
    for i in xrange(items_in_list - 1):
        objs3[i+1].ref = objs3[i]

test_gc(scope1)

print sorted(unordered_finalize.keys())

# Try a few times - this test is still somewhat flaky because it's hard to guarantee
# finalizer calls for larger number of objects when we have conservative scanning.

def try_increasing(n=10):
    if n == 0:
        return

    global ordered_finalize_increasing
    ordered_finalize_increasing = []
    # Need to GC a lot of times because of reference chains of finalizers.
    test_gc(scope2, 25)
    if ordered_finalize_increasing == range(items_in_list):
        print "success! got "
        print ordered_finalize_increasing
        print "at least once"
    else:
        try_increasing(n-1)

def try_decreasing(n=10):
    if n == 0:
        return

    global ordered_finalize_decreasing
    ordered_finalize_decreasing = []
    test_gc(scope3, 25)
    if list(reversed(ordered_finalize_decreasing)) == range(items_in_list):
        print "success! got "
        print ordered_finalize_decreasing
        print "at least once"
    else:
        try_decreasing(n-1)

try_decreasing()
try_increasing()
