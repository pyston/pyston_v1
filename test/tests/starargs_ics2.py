

try:
    def f():
        pass

    for i in [0, 10]:
        f(*range(i))
except Exception as e:
    print e.message

try:
    def f(a, b, c, d):
        pass

    for i in [4, 3]:
        f(*range(i))
except Exception as e:
    print e.message
