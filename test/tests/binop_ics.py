# run_args: -n
# statcheck: noninit_count('slowpath_binop') <= 10
# statcheck: noninit_count('slowpath_runtimecall') <= 10

i = 1
f = 1.0
for i in xrange(1000):
    print 1 + 1
    print 1 + i
    print i + 1
    print i + i
    print f + i
    print i + f
    print f + 1
    print 1.0 + 1.0

