# run_args: -n
# statcheck: stats.get('slowpath_callattr', 0) <= 20
# statcheck: stats['slowpath_runtimecall'] <= 20
# statcheck: stats.get("slowpath_callclfunc", 0) <= 20
# statcheck: stats['rewriter_nopatch'] <= 20

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
