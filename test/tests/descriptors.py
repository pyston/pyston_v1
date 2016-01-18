def f1():
    class D(object):
        def __get__(self, instance, owner):
            print "get", instance, owner
            return 1

        def __set__(self, instance, value):
            print "set", instance, value

    class C(object):
        d = D()

        def __repr__(self):
            return "<C>"

    print C.d
    print C().d

    c = C()
    c.d = 2
    print c.d
f1()

def f2():
    class MaybeDescriptorMeta(type):
        def __getattribute__(self, attr):
            print "meta __getattribute__", attr
            return 2

    class MaybeDescriptor(object):
        __metaclass__ = MaybeDescriptorMeta
        def __getattribute__(self, attr):
            print "__getattribute__", attr
            return 1

        def __repr__(self):
            return "<MaybeDescriptor>"

    class HasDescriptor(object):
        x = MaybeDescriptor()

        def __repr__(self):
            return "<HasDescriptor>"

    hd = HasDescriptor()
    # Getting hd.x will look up type(hd.__dict__[x]).__get__
    # and not go through __getattribute__
    print hd.x
    print hd.x.__get__
    print type(hd.x).__get__

    # But we can still set it here:
    def get(*args):
        print "get", args
        return 3
    hd.x.__get__ = get
    print hd.x
    type(hd.x).__get__ = get
    print hd.x
f2()

def f3():
    print type(max).__dict__['__name__'].__get__(max, 1)
    try:
        type(max).__dict__['__name__'].__set__(max, 1)
    except AttributeError as e:
        print e
    try:
        type(max).__dict__['__name__'].__delete__(max)
    except AttributeError as e:
        print e

f3()
