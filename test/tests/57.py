# expected: fail
# - lambdas, varargs

# Testing im-boxing:

class C(object):
    def __call__(*args):
        print args
        return args
def mul(*args):
    return args
class C1(object):
    __add__ = C()
    __sub__ = lambda *args: args
    __mul__ = mul

c = C1()
print c + 1
print c - 1
print c * 1
print c.__add__ == C1.__add__
print c.__sub__ == C1.__sub__
print c.__mul__ == C1.__mul__
