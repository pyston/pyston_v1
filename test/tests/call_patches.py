# run_args: -n
# statcheck: noninit_count('slowpath_callattr') <= 20
# statcheck: noninit_count('slowpath_runtimecall') <= 20
# statcheck: noninit_count("slowpath_callclfunc") <= 20
# statcheck: noninit_count('rewriter_nopatch') <= 20

def outer():
    def f():
        return 1

    t = 0
    for i in xrange(10000):
        t = t + f()
    print t
outer()

t = 0
for i in xrange(1000):
    t = t + i
print t
