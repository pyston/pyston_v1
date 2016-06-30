# Dict comparisons are supposed to be based on contents.
# Note -- if you fix this file, please update from_cpython/Lib/mapping_tests.py
l1 = []
l2 = []
for i in xrange(100):
    l1.append({1:1})
    l2.append({2:2})
t = [0,0]
for a in l1:
    for b in l2:
        t[a < b] += 1
print t # should be [0, 10000]
