i = "global"
def f(n):
    i = "hello"
    # The iterator element will get set, but only
    # if the range is non-empty
    print [0 for i in xrange(n)]

    print n, i * 3

f(0)
f(5)

i = 0
# Though if the ifs empty out the range, the
# name still gets set:
print [0 for i in xrange(5) if 0]
print i


i = "global"
def f3(n):
    # The global declaration here will apply to the i in the listcomp:
    global i
    [0 for i in xrange(n)]
# Calling with an empty range with do nothing:
f3(0)
print i
# Calling with a non-empty range will change the global i
f3(1)
print i

i = "global"
def f2(n, b):
    if b:
        i = 1
    print [0 for i in xrange(n)]
    # This i should refer to the local i even if n==0;
    # in that case this should be an error
    print i
f2(5, 0)
print i
f2(0, 1)
f2(0, 0)

