# handling re-built classes:

def make_class(n):
    class C(object):
        pass
    C.n = n
    return C

C1 = make_class(1)
C2 = make_class(2)
print C1.n
print C2.n
print C1 == C1
print C1 == C2

c1 = C1()
print c1.n
c2 = C2()
print c2.n
print c1 == c1
print c1 == c2
