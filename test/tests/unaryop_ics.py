# run_args: -n
# statcheck: noninit_count('slowpath_unaryop') <= 10
# statcheck: noninit_count('slowpath_runtimecall') <= 10

for i in xrange(1000):
    print -i
    print ~i
