a = [3, 2, 1]
b = [2, 'a', (3, 2)]

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

assert type(a) == list

# __eq__ tests

assert [3, 2, 1] == [3, 2, 1]
assert [3, 2, 1] == list(a)
assert [3, 2, ['a']] == list([3, 2, ['a']])
assert [3, 2, ['a', 2]] == list([3, 2, ['a', 2]])

class A(object):
    pass

a_class = A()

assert [3, 2, ['a', 2]] == list([3, 2, ['a', 2]])

#

new_a = []
for i in a:
    new_a.append(i)

assert [3, 2, 1] == new_a

# __iter__s

assert [3, 2, 1] == list(a.__iter__())

new_a = []
for i in a.__iter__():
    new_a.append(i)

assert [3, 2, 1] == new_a

# StopIteration exception

try:
    iter_a.next()
    assert False, "next() called without StopIteration"
except StopIteration:
    assert True

