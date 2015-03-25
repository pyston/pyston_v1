l = range(5)
print getattr(l, "pop")()

print getattr([], "a", "default")
try:
    print getattr([], "a")
except AttributeError, e:
    print e
