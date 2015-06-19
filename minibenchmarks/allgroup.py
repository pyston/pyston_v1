# From https://mail.python.org/pipermail/pypy-dev/2014-May/012515.html


def allgroup(expansions, n=0, groups = []):
     expgroup = [expansions[n]]
     if n == len(expansions) - 1:
         yield groups + [expgroup]
         for i in xrange(len(groups)):
             tmp = groups[i]
             groups[i] = tmp + expgroup
             yield groups
             groups[i] = tmp
     else:
         for g in allgroup(expansions, n+1, groups + [expgroup]):
             yield g
         for i in xrange(len(groups)):
             tmp = groups[i]
             groups[i] = tmp + expgroup
             for g in allgroup(expansions, n + 1, groups):
                 yield g
             groups[i] = tmp

count = 0
for i in allgroup(range(11)):
     count += 1
print count
