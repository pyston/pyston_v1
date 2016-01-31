# Simple datetime test
# Doesn't test much of the functionality, but even importing the module is tough:

import datetime

def typeErrorTest(val):
    try:
        x = datetime.timedelta(43)
        x // val
    except TypeError as e:
        return True
    return False


print repr(datetime.time())
print datetime.datetime.__base__
print repr(datetime.datetime(1, 2, 3))
print str(datetime.timedelta(0))

# now() works on both the class and on its instances:
datetime.datetime.now().now()

print datetime.datetime(1924, 2, 3).strftime("%B %d, %Y - %X")

a = 3
b = 3.4
assert not typeErrorTest(a)
assert typeErrorTest(b)
