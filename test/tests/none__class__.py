print None.__class__

print type(None).__doc__, None.__doc__

try:
    print type(None).__new__(type(None))
except TypeError as e:
    print e
try:
    type(None)()
except TypeError as e:
    print e
