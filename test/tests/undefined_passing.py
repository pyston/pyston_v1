# I think we have some other similar tests to this, but it's hard to find them.

# Reopt:
def f(x):
    if x:
        y = 1
    if '':
        pass
    y

for i in xrange(10000):
    f(1)
try:
    f(0)
    assert 0
except UnboundLocalError:
    pass


# OSR:
s = """
def f(x):
    if x:
        y = 1
    for i in xrange(10000):
        pass
    print y
"""

exec s
f(1)

exec s
try:
    f(0)
    assert 0
except UnboundLocalError:
    pass
