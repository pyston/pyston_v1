# expected: fail
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

# Add testcase
l1 = [
    {"ka": 1},
    {"ka": 2},
    {"kb": 3},
    {"ka": 4},
    {"kc": 5},

    {"ka": 1, "kb": 1},
    {"ka": 1, "kb": 2},
    {"ka": 2, "kb": 1},
    {"ka": 2, "kb": 2},

    {"ka": 1, "kb": None},
    {"ka": None, "kb": 2},
    {"ka": 1, "kb": None},
    {"ka": None, "kb": 2},

    {"ka": 1, "kb": 1, "kc": 1},
    {"ka": 1, "kb": 1, "kc": 2},
    {"ka": 1, "kb": 1, "kc": 3},
    {"ka": 1, "kb": 2, "kc": 1},
    {"ka": 1, "kb": 2, "kc": 2},
    {"ka": 1, "kb": 2, "kc": 3},
    {"ka": 1, "kb": 3, "kc": 1},
    {"ka": 1, "kb": 3, "kc": 2},
    {"ka": 1, "kb": 3, "kc": 3},

    {"ka": 3, "kb": 1, "kc": 1},
    {"ka": 3, "kb": 1, "kc": 2},
    {"ka": 3, "kb": 1, "kc": 3},
    {"ka": 3, "kb": 2, "kc": 1},
    {"ka": 3, "kb": 2, "kc": 2},
    {"ka": 3, "kb": 2, "kc": 3},
    {"ka": 3, "kb": 3, "kc": 1},
    {"ka": 3, "kb": 3, "kc": 2},
    {"ka": 3, "kb": 3, "kc": 3},
]

l2 = [
    {"ka": 1},
    {"ka": 2},
    {"kb": 3},
    {"ka": 4},
    {"kc": 5},

    {"ka": 1, "kb": 1},
    {"ka": 1, "kb": 2},
    {"ka": 2, "kb": 1},
    {"ka": 2, "kb": 2},

    {"ka": None, "kb": 2},
    {"ka": 1, "kb": None},
    {"ka": None, "kb": 1},
    {"ka": 2, "kb": None},

    {"ka": 1, "kb": 1, "kc": 1},
    {"ka": 1, "kb": 1, "kc": 2},
    {"ka": 1, "kb": 1, "kc": 3},
    {"ka": 1, "kb": 2, "kc": 1},
    {"ka": 1, "kb": 2, "kc": 2},
    {"ka": 1, "kb": 2, "kc": 3},
    {"ka": 1, "kb": 3, "kc": 1},
    {"ka": 1, "kb": 3, "kc": 2},
    {"ka": 1, "kb": 3, "kc": 3},

    {"ka": 2, "kb": 1, "kc": 1},
    {"ka": 2, "kb": 1, "kc": 2},
    {"ka": 2, "kb": 1, "kc": 3},
    {"ka": 2, "kb": 2, "kc": 1},
    {"ka": 2, "kb": 2, "kc": 2},
    {"ka": 2, "kb": 2, "kc": 3},
    {"ka": 2, "kb": 3, "kc": 1},
    {"ka": 2, "kb": 3, "kc": 2},
    {"ka": 2, "kb": 3, "kc": 3},
]
t = [0, 0]
for a in l1:
    for b in l2:
        t[a < b] += 1
print t # should be [530, 431]
