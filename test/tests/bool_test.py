# expected: fail

long_ = type(31415926535897932384626)
assert isinstance(True, long_)
assert issubclass(bool, long_)
print(bool.__bases__)
