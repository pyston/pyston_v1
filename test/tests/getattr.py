l = range(5)
print getattr(l, "pop")()

print getattr([], "a", "default")
print getattr([], "a")
