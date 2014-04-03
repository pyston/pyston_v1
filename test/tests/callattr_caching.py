# run_args: -n

class C(object):
    def foo(self):
        print "C.foo()"
class D(object):
    def foo(self):
        print "D.foo()"

class EH(object):
    def __call__(self):
        print "EH()"
class E(object):
    pass
E.foo = EH()

for i in [C(), D(), E(), C()]:
    i.foo()
