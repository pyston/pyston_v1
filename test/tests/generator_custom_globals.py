s = """
def g():
    yield x
    yield y
"""

g = {'x': 1, 'y': 4}
l = {}

exec s in g, l

print list(l['g']())
