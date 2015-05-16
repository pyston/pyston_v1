# run_args: -n
# statcheck: noninit_count('slowpath_binop') < 10

class O(object):
    def __init__(self, n):
        self.n = n

def mul2(o):
    return o.n * 2

oi = O(1)
of = O(1.0)
for i in xrange(1000):
    print mul2(oi)
    print mul2(of)
