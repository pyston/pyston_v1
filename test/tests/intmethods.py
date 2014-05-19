# can't try large numbers yet due to lack of long
for i in xrange(1, 100):
    for j in xrange(1, 100):
        print i.__divmod__(j)
