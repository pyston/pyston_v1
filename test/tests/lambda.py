s = lambda x=5: x**2
print s(8), s(100), s()

for i in range(10):
    print (lambda x, y: x < y)(i, 5)

t = lambda s: " ".join(s.split())
print t("test \tstr\ni\n ng")

def T(y):
     return (lambda x: x < y)
print T(10)(1), T(10)(20)

# Lambda closures:
def f(x):
    print map(lambda y: x * y, range(10))
f(3)
