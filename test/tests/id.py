x = 'Hi'
y = 1
z = (42, 7)
f = (2,) + z
print id(x) == id(x)
print id(y) != id(x)
print id(z) == id(z)
print id(f) != id(z)

