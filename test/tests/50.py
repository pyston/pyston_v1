# Very basic class handling

class C(object):
    pass
print C.__name__

C.n = 1
print C.n
c = C()
print c.n
