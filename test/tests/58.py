# int.__init__ should do nothing; it should all be handled by int.__new__

i = 1
j = 1
print int.__init__(i, 2)

def p(n):
    print n

p(i)
p(j)

