import types

assert type('test') == str
assert type(2) == int
assert type(set([2, 1])) == set
assert type([2, 1]) == list
assert type({'a': 1, 2: 'b'}) == dict
assert type((1, 2)) == tuple

assert type(None) == types.NoneType

assert type('test') == types.StringType
assert type(1) == types.IntType
assert type(1.0) == types.FloatType

class A(object):
    pass

assert type(A) == types.TypeType

def abc():
    pass

assert type(abc) == types.FunctionType
assert type(abc()) == types.NoneType

class B(object):
    def bcd(self):
        return 'ok'

assert type(B().bcd) == types.MethodType
assert type(B().bcd) == types.UnboundMethodType
assert type(B().bcd()) == types.StringType

# Uncomment when old style classes will be implemented
#class D():
#    pass
#
#assert type(D) == types.ClassType
#assert type(D()) == types.InstanceType

assert type(types) == types.ModuleType

