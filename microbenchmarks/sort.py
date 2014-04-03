def sort(l):
    n = len(l)
    for i in xrange(n):
        print i
        for j in xrange(i):
            if l[i] < l[j]:
                l[i], l[j] = l[j], l[i]
    return l

l = []
N = 5000
for i in xrange(N):
    l.append(N - i)
sort(l)
print l
