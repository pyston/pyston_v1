# Programs are well-defined even if they contain potentially-undefined variables
# The exceptions are well-defined too, but I don't support that yet

if 1:
    x = 1
print x


z = 1

if z:
    y = 2
else:
    pass

if z:
    print y
    w = y
else:
    pass
print w

while 1:
    a = 1
    break
print a

if 0:
    # this is only an error if the code gets hit:
    print not_defined
