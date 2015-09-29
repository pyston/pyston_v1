class TestEllipsis(object):
   def __getitem__(self, item):
      if item is Ellipsis:
         return "Ellipsis"
      else:
         return "return %r items" % item

print Ellipsis
v = Ellipsis
print v
print type(v)
x = TestEllipsis()
print x[2]
print x[...]
