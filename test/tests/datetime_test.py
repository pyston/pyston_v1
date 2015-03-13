# Simple datetime test
# Doesn't test much of the functionality, but even importing the module is tough:

import datetime
print repr(datetime.time())
print datetime.datetime.__base__
print repr(datetime.datetime(1, 2, 3))
print str(datetime.timedelta(0))

# now() works on both the class and on its instances:
datetime.datetime.now().now()
