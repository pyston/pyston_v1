class A(object):
    def __eq__(self, rhs):
        return True

class B(object):
    def __eq__(self, lhs):
        return False

print A() == B()
print B() == A()

print A() in [B()]
print B() in [A()]

print A() in (B(),)
print B() in (A(),)

print A() in {B(): 1}
print B() in {A(): 1}

print A() in {B()}
print B() in {A()}
