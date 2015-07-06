import gc

finalized_at_least_once = False

class ObjWithFinalizerAndRef(object):
    def __init__(self, index):
        self.index = index
        self.ref = None
    def __del__(self):
        global finalized_at_least_once
        finalized_at_least_once = True

items_in_list = 100

# Make a lot of cycles
for _ in xrange(100):
    # Create a finalizer cycle. We should break those arbitrarily.
    objs = [ObjWithFinalizerAndRef(i) for i in xrange(items_in_list)]
    for i in xrange(items_in_list):
        objs[i].ref = objs[(i+1) % items_in_list]
    gc.collect()

print "finished"
if not finalized_at_least_once:
    raise Exception("should gc at least once - consider creating more cycles?")
