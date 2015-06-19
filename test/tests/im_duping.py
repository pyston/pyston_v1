# Regressin test: internal tracking of non-instantiated instancemethods was incorrect

def f():
    items = [1 for i in xrange(10)]

    set = []
    setappend = set.append
    for item in range(5):
        setappend(item)
    print set
f()
