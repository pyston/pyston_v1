import pickle

l = [[], (123,)]
l.append(l)
s = pickle.dumps(l)
print repr(s)

l2 = pickle.loads(s)
l3 = l2.pop()
print l2, l3, l2 is l3

print pickle.loads(pickle.dumps("hello world"))
