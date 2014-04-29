# expected: fail
# - metaclasses, inheritance

# This would make a good Python quiz:

sl = slice(1,2)
class C(sl):
    pass

print C

print C.start, C.stop, C.step
print type(C)
