# Not sure if these are important or not:

p = property(1, 2, 3)
print p.fset
property.__init__(p, 4, 5, 6)
print p.fset

s = staticmethod(7)
print s.__get__(8)
staticmethod.__init__(s, 9)
print s.__get__(10)

c = classmethod(11)
print c.__get__(12).im_func
classmethod.__init__(c, 13)
print c.__get__(13).im_func

sl = slice(14, 15, 16)
print sl.start
slice.__init__(sl, 17, 18, 19)
print sl.start

# Not supported yet:
# l = list((20,))
# print l
# list.__init__(l, (21,))
# print l

sp = super(int, 1)
print sp.__thisclass__
super.__init__(sp, float, 1.0)
print sp.__thisclass__
