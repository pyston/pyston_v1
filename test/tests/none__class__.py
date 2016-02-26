print None.__class__

print type(None).__doc__, None.__doc__

try:
    type(None)()
except TypeError as e:
    print e
