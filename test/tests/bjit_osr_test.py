# this specific test used to crash
def f(x):
    if x:
        raise Exception

def osr_f():
    for i in range(10000):
        f(False)
    f(True)
try:
    osr_f()
except Exception:
    print "exc"
