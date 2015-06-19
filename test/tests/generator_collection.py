# Allocate a few-dozen megabytes of memory inside a generator,
# to try to force a collection.
def f():
    l = range(500)

    # Something above the OSR threshold:
    for i in xrange(12000):
        l = (l * 4)[:100]
    yield 0

print list(f())
