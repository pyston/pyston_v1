print 1.3 % 0.4
print -1.3 % 0.4
print 1.3 % -0.4
print -1.3 % -0.4

l = [2, 3, 1.0, 2.0, 0.3, 1.6]
for i in list(l):
    l.append(-i)
for i in l:
    for j in l:
        print i, j, i % j
