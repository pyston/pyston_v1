allocated = set()
freed = set()

reported = set()

for l in open("malloc.trace").readlines()[1:]:
    s = l.strip().split()
    if s[-3] == '+':
        p = s[-2]
        if p in freed:
            freed.remove(p)
    elif s[-2] == '-':
        p = s[-1]
        if p in freed:
            if p not in reported:
                print "double-freed", p
                reported.add(p)
        freed.add(p)
    elif s[-2] == '<':
        p = s[-1]
        assert p not in freed, p
        freed.add(p)
    elif s[-3] == '>':
        p = s[-2]
        if p in freed:
            freed.remove(p)
    else:
        assert 0, l
