a = (3, 2, 1)
b = (2, 'a', (3, 2))

# TODO: uncomment when hassattr will be implemented
#assert hasattr(a, '__iter__')

iter_a = a.__iter__()
assert iter_a.next() == 3
assert iter_a.next() == 2
assert iter_a.next() == 1

iter_b = b.__iter__()
assert iter_b.next() == 2
assert iter_b.next() == 'a'
assert iter_b.next() == (3, 2)
