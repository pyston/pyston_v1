# expected: reffail
class C(object):
  i = 0
  def next(self):
    if self.i<5:
        self.i += 1
        return self.i
    else:
        raise StopIteration
  def __iter__(self):
    return self

print list(C())


