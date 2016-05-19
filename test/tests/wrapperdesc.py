class C(object):
    pass
print C.__str__ is object.__str__
print type(C).__str__ is object.__str__
print type(None).__str__ is object.__str__
print type(None).__str__ is None.__str__
print type(None.__str__)
print(None.__str__.__name__)
print type(type(None).__str__.__get__(None, type(None)))
