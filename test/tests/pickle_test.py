import pickle

l = [[], (123,)]
l.append(l)
s = pickle.dumps(l)
print repr(s)

l2 = pickle.loads(s)
l3 = l2.pop()
print l2, l3, l2 is l3

print pickle.loads(pickle.dumps("hello world"))

# Sqlalchemy wants this:
import operator
print repr(pickle.dumps(len))
print repr(pickle.dumps(operator.and_))

class C(object):
    pass
c = C()
c.a = 1
print repr(pickle.dumps(c))
print pickle.loads(pickle.dumps(c)).a
