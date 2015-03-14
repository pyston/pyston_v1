# expected: fail
# we don't support tp_compare yet

s = 'Hello world'
t = buffer(s, 6, 5)

s2 = "Goodbye world"
t2 = buffer(s2, 8, 5)

print t == t2
