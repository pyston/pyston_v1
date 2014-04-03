# Testing to make sure that functions like __add__ only get looked up on the class, not the instance:

class C(object):
    def __add__(self, rhs):
        print "C.__add__"

def new_add(lhs, rhs):
    print "new __add__"

c = C()
c.__add__ = new_add # this should NOT change the result of the next add:
c + c

C.__add__ = new_add
c + c
