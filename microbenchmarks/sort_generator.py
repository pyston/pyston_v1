import itertools
import sys

N = 5
if len(sys.argv) > 1:
    N = int(sys.argv[1])

perms = list(itertools.permutations(range(N)))

def compatible_with(comps):
    if not comps:
        return perms
    r = []
    for p in perms:
        for c in comps:
            if p[c[0]] > p[c[1]]:
                break
        else:
            r.append(p)
    return r

def generate(indent, comps=[], f=sys.stdout):
    cur_compatible = compatible_with(comps)
    assert len(cur_compatible) > 0

    if len(cur_compatible) == 1:
        ans = [None] * N
        for i, r in enumerate(cur_compatible[0]):
            ans[r] = chr(ord('a') + i)
        print >>f, "%sreturn (%s)" % (' ' * indent, ', '.join(ans))
        return

    best = 0
    bestnew = None
    for i in xrange(N):
        for j in xrange(i):
            newcomps = list(comps)
            newcomps.append((j, i))
            c1 = len(compatible_with(newcomps))
            if c1 * 2 > len(cur_compatible):
                c1 = len(cur_compatible) - c1
            if c1 > best:
                best = c1
                bestnew = (j, i)

    assert best
    assert bestnew
    print >>f, "%sif %s<%s:" % (' ' * indent, chr(ord('a') + bestnew[0]), chr(ord('a') + bestnew[1]))
    generate(indent + 2, comps + [bestnew], f)
    print >>f, "%selse:" % (' ' * indent,)
    generate(indent + 2, comps + [(bestnew[1], bestnew[0])], f)

import StringIO
s = StringIO.StringIO()
print >>s, "def f(%s):" % (", ".join(chr(ord('a') + i) for i in xrange(N)))
generate(2, f=s)
exec s.getvalue()

for p in perms:
    assert list(f(*p)) == range(N)

print s.getvalue()
import random
for i in xrange(10):
    l = range(N)
    random.shuffle(l)
    print "print f%s" % (tuple(l),)
