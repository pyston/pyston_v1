# I think we have some other similar tests to this, but it's hard to find them.

def f(x):
    if x:
        y = 1
    if '':
        pass
    print y

for i in xrange(10000):
    f(1)
try:
    f(0)
    assert 0
except UnboundLocalError:
    pass

