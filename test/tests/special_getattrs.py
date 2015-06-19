def show(obj):
    print obj.__class__
    for b in obj.__class__.__mro__:
        print b,
    print

    if isinstance(obj, type):
        print obj.__bases__, obj.__mro__
    if hasattr(obj, "__dict__"):
        print sorted(obj.__dict__.items())

show(object())

class C: # oldstyle
    pass

print C.__dict__["__module__"]
print len(C().__dict__)
# show(C)
# show(C())
