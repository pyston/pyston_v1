def f(a, b):
    print a, b, "<", a < b
    print a, b, "<=", a <= b
    print a, b, ">", a > b
    print a, b, ">=", a >= b
    print a, b, "==", a == b
    print a, b, "!=", a != b
    print a, b, "is", a is b
    print a, b, "is not", a is not b

class C(object):
    pass

class Z(object):
    pass

args = [0, 1, 0.1, 1.1, "hello", float('nan'), float('inf'), float('-inf')]#, C(), Z()]

for i in xrange(len(args)):
    for j in xrange(i):
        f(args[i], args[j])

def sort(l):
    n = len(l)
    for i in xrange(n):
        for j in xrange(i):
            if l[j] > l[i]:
                l[j], l[i] = l[i], l[j]
    return l

sortable_args = [0, 1, 0.1, 1.1, "hello", float('inf'), float('-inf')]#, C(), Z()]
print sort(sortable_args)

# Nevermind, the ordering isn't defined (only defined to be consistent), and I don't feel like matching it
# # Ordering across types:
# class C(object):
    # pass
# l = [1, 2.0, "hello", [], (), {}, C, int, C(), repr]
# sort(l)
# print l

class C(object):
    def __init__(self, n):
        self.n = n
    def __repr__(self):
        return "<<c object>>"
    def __lt__(self, rhs):
        print "lt"
        return self.n
    def __le__(self, rhs):
        print "le"
        return False
    def __eq__(self, rhs):
        print "eq"
        return False
    def __gt__(self, rhs):
        print "gt"
        return self.n
    def __ge__(self, rhs):
        print "ge"
        return False

for i in xrange(2):
    print C("") > 2
    print i < 2
    print min(C("hello"), 2)
    print min(C(""), 2)
    print max(C("ello"), 2)
    print max(C(""), 2)
    print C("hi") > 1
    print C("") > 1

print (1, 2) < (1, 3)
print (1, 4) < (1, 3)
print [1, 2] < [1, 3]
print {1:2} < {1:3}

class Reverse(object):
    def __init__(self, n):
        self.n = n

    def __lt__(self, rhs):
        print "lt"
        return self.n < rhs.n

    def __le__(self, rhs):
        print "le"
        return self.n <= rhs.n

print Reverse(4) > Reverse(3), Reverse(4) > Reverse(4)
