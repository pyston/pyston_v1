from testing_helpers import test_gc

unordered_finalize = {}

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

# We run several attempts in parallel and check that at least one of the works - because
# this test requires that a large number of objects is finalized, it's hard to make sure
# that none of them get retained for longer than they should due to conservative collection.
number_of_attempts = 10

def scope2():
    increasing_lists = []

    for _ in xrange(number_of_attempts):
        increasing_list = []
        increasing_lists.append(increasing_list)
        objs = [ObjWithFinalizerAndRef(i, increasing_list) for i in xrange(items_in_list)]
        for i in xrange(items_in_list - 1):
            objs[i].ref = objs[i+1]

    return increasing_lists

def scope3():
    decreasing_lists = []

    for _ in xrange(number_of_attempts):
        decreasing_list = []
        decreasing_lists.append(decreasing_list)
        objs = [ObjWithFinalizerAndRef(i, decreasing_list) for i in xrange(items_in_list)]
        for i in xrange(items_in_list - 1):
            objs[i+1].ref = objs[i]

    return decreasing_lists

test_gc(scope1)
print sorted(unordered_finalize.keys())

increasing_lists = test_gc(scope2, 25)
decreasing_lists = test_gc(scope3, 25)

for increasing_list in increasing_lists:
    if increasing_list == range(items_in_list):
        print "success! got "
        print increasing_list
        print "at least once"
        break
for decreasing_list in decreasing_lists:
    decreasing_list.reverse()
    if decreasing_list == range(items_in_list):
        print "success! got "
        print decreasing_list
        print "at least once"
        break
