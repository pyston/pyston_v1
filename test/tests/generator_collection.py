# expected: fail
# - the GC will crash if any thread is inside a generator during a collection

# Allocate a few-dozen megabytes of memory inside a generator,
# to try to force a collection.
def f():
    l = range(1000)
    for i in xrange(10000):
        l = list(l)
    yield 0

print list(f())
