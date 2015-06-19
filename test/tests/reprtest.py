a=1
try:
    a.b
except AttributeError, e:
    print repr(e)

print repr("both\'\"quotes")
print repr("single\'quote")
print repr("double\"quote")
