# skip-if: True

# I was worried about using a recursive parser for obscenely-nested source code,
# but based off this example it looks like that's what cPython and pypy both use as well.

# To save test-file space, just generate the expression and then eval() it:
N = 100000
s = "(" * N + ")" * N
t = eval(s)
len(repr(t[0][0][0]))
