f = open(**{"name": "/dev/null", "mode": "r"})
print repr(f.read())
