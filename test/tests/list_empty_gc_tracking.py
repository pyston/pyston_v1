# Regression test:
# - create some lists
# - pop from them to make them empty, but still have capacity allocated
# - do some stuff to try to force a GC collection
# -- the old bug was that the element array would get freed because size=0 even though capacity>0
# - add more stuff to the array to try to get it to realloc more space
# -- crash

l = []
for i in xrange(100):
    l2 = [0]
    l.append(l2)
    l2.pop()
for l2 in l:
    l2 += range(100)

    # allocate some data to try to force a collection:
    # TODO shouldn't be too hard to add support for gc.collect()
    range(10000)
