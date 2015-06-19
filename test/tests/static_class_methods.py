class C(object):
    @staticmethod
    def f(a, b, c, d):
        print a, b, c, d

    @classmethod
    def g(cls, a, b, c, d):
        print cls, a, b, c, d

c = C()
c.f(1, 2, 3, 4)
c.g(5, 6, 7, 8)

C.f(9, 10, 11, 12)
C.f(13, 14, 15, 16)

@staticmethod
def f(a, b, c, d):
    print a, b, c, d

@classmethod
def g(cls, a, b, c, d):
    print cls, a, b, c, d

f.__get__(c, C)(17, 18, 19, 20)
g.__get__(c, C)(21, 22, 23, 24)
