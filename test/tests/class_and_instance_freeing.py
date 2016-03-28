import gc

l = []
for i in xrange(5100):
    class C(object):
        pass
    C.l = [C() for _ in xrange(17)]

    if i % 10 == 0:
        print i

        # gc.collect()
        # for i in xrange(100):
            # l.append('=' * i)
