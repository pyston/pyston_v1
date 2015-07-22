# This file isn't really meant to be run as a test, though it won't really
# make a difference.

import gc

# Sometimes pointer objects from popped stack frames remain up the stack
# and end up being marked when the GC conservatively scans the stack, but
# this causes flaky tests because we really want the object to be collected.
# By having a deep recursive function, we ensure that the object we want to
# collect is really far in the stack and won't get scanned.
def call_function_far_up_the_stack(fn, num_calls_left=200):
    if num_calls_left == 0:
        return fn()
    else:
        return call_function_far_up_the_stack(fn, num_calls_left - 1)

# It's useful to call the GC at different locations in the stack in case that it's the
# call to the GC itself that left a lingering pointer (e.g. the pointer could be the
# __del__ attribute of an object we'd like to collect).
def call_gc_throughout_the_stack(number_of_gc_calls, num_calls_left=100):
    if num_calls_left > 0:
        call_gc_throughout_the_stack(number_of_gc_calls, num_calls_left - 1)
        if number_of_gc_calls >= num_calls_left:
            gc.collect()

# test_gc takes in a function fn that presumably allocations some objects and
# attempts to collect those objects in order to trigger a call to the finalizers.
#
# The problem is that it's actually quite hard to guarantee finalizer calls
# because with conservative scanning, there can always be lingering pointers
# on the stack. This function has a bunch of hacks to attempt to clear those
# lingering pointers.
def test_gc(fn, number_of_gc_calls=3):
    class DummyNewObject(object):
        pass
    class DummyOldObject():
        pass

    def dummyFunctionThatDoesSomeAllocation():
        # Allocating a few objects on the heap seems to be helpful.
        for _ in xrange(100):
            n, o = DummyNewObject(), DummyOldObject()
        objs = [DummyNewObject() for _ in xrange(100)]

    # Call fn after a few recursive calls to get those allocations.
    val = call_function_far_up_the_stack(fn)

    # Call a dummy function in the same way as fn. By following the same
    # code path, there is a better chance of clearing lingering references.
    call_function_far_up_the_stack(dummyFunctionThatDoesSomeAllocation)

    # Force garbage collection.
    call_gc_throughout_the_stack(number_of_gc_calls - 1)
    gc.collect()
    return val

