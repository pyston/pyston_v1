# run_args: -n
# statcheck: noninit_count('slowpath_runtimecall') < 10

for i in xrange(1000):
    print int(i * 1.1)
