# run_args: -n
# statcheck: noninit_count('slowpath_getitem') <= 20
# statcheck: noninit_count('slowpath_setitem') <= 20

def sort(l):
    n = len(l)
    for i in xrange(n):
        print i
        for j in xrange(i):
            if l[i] < l[j]:
                l[i], l[j] = l[j], l[i]
    return l

l = []
N = 500
for i in xrange(N):
    l.append(N - i)
sort(l)
print l

