# expected: fail
# - we don't support setting __dict__ yet

class C(object):
    pass

c1 = C()
c2 = C()

c1.a = 2
c1.b = 3
c2.a = 4
c2.b = 5

def p():
    print sorted(c1.__dict__.items()), sorted(c2.__dict__.items())

p()

c1.__dict__ = c2.__dict__
p()

c1.a = 6
c2.b = 7
p()
