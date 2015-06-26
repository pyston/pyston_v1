# Since gc-related metadata is (currently) placed on classes,
# it's tricky for us if a class object becomes garbage in the same
# collection as some of its instances.  If we try to collect the class
# first, we will not have the metadata any more on how to collect the instances.

def f():
    class C(object):
        pass
    for i in xrange(1000):
        pass

for j in xrange(1000):
    f()

import gc
gc.collect()
