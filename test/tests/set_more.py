# expected: fail

print hasattr(set, "__ior__")
print hasattr(set, "__isub__")
print hasattr(set, "__iand__")
print hasattr(set, "__ixor__")

s1 = set() | set(range(3))
s2 = set(range(1, 5))
s3 = s1
s1 -= s2
print s1, s2, s3
