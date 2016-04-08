
print list(reversed("hello"))
print list(reversed(""))
print list(reversed([1,2,3]))
print list(reversed((1,2,3)))
print list(reversed([]))

class RevIterTest(object):
  def __getitem__(self, idx):
    if idx >= 3:
      raise StopIteration()
    return idx

  def __len__(self):
    return 3

print list(RevIterTest())
print list(reversed(RevIterTest()))

# xrange and list have __reversed__ methods
print list(xrange(0,9).__reversed__())
print list([1,2,3,4,5,6].__reversed__())

r = reversed((1, 2, 3))
try:
    while True:
        print r.next(),
except StopIteration:
    print ""
