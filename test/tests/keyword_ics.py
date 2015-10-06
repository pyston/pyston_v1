# statcheck: noninit_count('slowpath_runtimecall') <= 500
# statcheck: noninit_count('slowpath_callfunc') <= 500
# run_args: -n

def f(a=-1, b=-2):
    return a + b

s = 0
for i in xrange(20000):
    s += f(a=1, b=2)
    s += f(b=3, a=4)
    s += f(b=5)
print s
