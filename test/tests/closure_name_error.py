x3 = 0
def bad_addr3(_x):
    if 0:
        x3 = _

    def g(y3):
        return x3 + y3
    return g
try:
    print bad_addr3(1)(2)
except NameError, e:
    print e
