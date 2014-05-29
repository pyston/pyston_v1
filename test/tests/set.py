def sorted(s):
    l = list(s)
    l.sort()
    return repr(l)

s1 = set() | set(range(3))
print sorted(s1)
s2 = set(range(1, 5))
print sorted(s2)

print repr(sorted(s1)), str(sorted(s1))

print sorted(s1 - s2)
print sorted(s2 - s1)
print sorted(s1 ^ s2)
print sorted(s1 & s2)
print sorted(s1 | s2)

print len(set(range(5)))
