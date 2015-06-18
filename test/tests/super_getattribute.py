def test_supers():
    # Testing super...

    class A(object):
        def meth(self, a):
            return "A(%r)" % a

    print(A().meth(1), "A(1)")

    class B(A):
        def __init__(self):
            self.__super = super(B, self)
        def meth(self, a):
            return "B(%r)" % a + self.__super.meth(a)

    print(B().meth(2), "B(2)A(2)")

    class C(A):
        def meth(self, a):
            return "C(%r)" % a + self.__super.meth(a)
    C._C__super = super(C)

    print(C().meth(3), "C(3)A(3)")

    class D(C, B):
        def meth(self, a):
            return "D(%r)" % a + super(D, self).meth(a)

    print(D().meth(4), "D(4)C(4)B(4)A(4)")

    # Test for subclassing super

    class mysuper(super):
        def __init__(self, *args):
            return super(mysuper, self).__init__(*args)
        
    class E(D):
        def meth(self, a):
            return "E(%r)" % a + mysuper(E, self).meth(a)
    
    print(E().meth(5), "E(5)D(5)C(5)B(5)A(5)")
    
    class F(E):
        def meth(self, a):
            s = self.__super # == mysuper(F, self)
            return "F(%r)[%s]" % (a, s.__class__.__name__) + s.meth(a)
    F._F__super = mysuper(F)
        
    print(F().meth(6), "F(6)[mysuper]E(6)D(6)C(6)B(6)A(6)")
        
    # Make sure certain errors are raised


test_supers()
