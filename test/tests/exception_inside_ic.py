def f(x):
    float(x)

try:
    f(1)
    f(1)
    f(1)
    f("str")
except Exception as e:
    print e
