# Regression test: make sure that irgen and type analysis
# both handle tuple -packing and -unpcaking.

def get_str():
    return "Hello world"

def f():
    _, a = 1, get_str()
    a.lower()

for i in xrange(100000):
    f()
