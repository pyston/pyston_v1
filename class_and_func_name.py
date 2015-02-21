class C(object):
    pass

print C
C.__name__ = "new_name"
print C
print repr(C.__module__)
C.__module__ = "new_module"
print C

C.__module__ = 1
print C

def set_name(obj, name):
    try:
        obj.__name__ = name
    except Exception as e:
        print type(e), e
    print obj.__name__

def del_name(obj):
    try:
        del obj.__name__
    except Exception as e:
        print type(e), e
    print obj.__name__

set_name(int, "bob")
#TODO implement __del__ for getset descriptors
#del_name(int)
#del_name(C)
set_name(C, 5)
set_name(C, "b\0b")
set_name(C, "car")

def g():
    pass
print g.__name__
set_name(g, "bob")
set_name(g, 5)
set_name(g, "b\0b")

f = lambda x : 5
print f.__name__
set_name(f, "bob")
set_name(f, 5)
set_name(f, "b\0b")
#del_name(f)
