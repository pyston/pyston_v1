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

def set_name(cls, name):
    try:
        cls.__name__ = name
    except Exception as e:
        print type(e), e
    print cls

def del_name(cls):
    try:
        del cls.__name__
    except Exception as e:
        print type(e), e
    print cls.__name__

set_name(int, "bob")
#TODO implement __del__ for getset descriptors
#del_name(int)
#del_name(C)
set_name(C, 5)
set_name(C, "b\0b")
set_name(C, "car")
