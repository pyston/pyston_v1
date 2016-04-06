# run_args: -n
# statcheck: noninit_count('slowpath_runtimecall') < 20

# A test to make sure we can rewrite certain kwargs cases

# For now, just support rewriting if the kw is empty:
def f(n, **kw):
    print len(kw)
    if n == 1000:
        kw[n] = 1

for i in xrange(20000):
    f(i)
