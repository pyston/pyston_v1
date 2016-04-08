import itertools

its = []
its.append(itertools.count(5))
its.append(itertools.cycle([1, 2, 3]))
its.append(itertools.repeat(1337))
its.append(itertools.chain([1, 2, 3], itertools.repeat(5)))
its.append(itertools.compress(itertools.count(), itertools.count(-3)))


for i in xrange(10):
    for it in its:
        print it.next(),
    print

print list(itertools.dropwhile(lambda x: x == 0, reversed((1, 2, 3))))

print list(itertools.product(range(4), range(4)))
