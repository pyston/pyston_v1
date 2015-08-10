# run_args: -n
# statcheck: noninit_count('slowpath_nonzero') <= 25

def f():
    for i in xrange(-10, 10):
        print i,
        if i:
            print "is truth-y"
        else:
            print "is false-y"
f()

num_nonzero = 0
num_len = 0
class C(object):
	def mynonzero(self):
		global num_nonzero
		num_nonzero += 1
		return True
	def mylen(self):
		global num_len
		num_len += 1
		return True
f = C()
s = 0
for i in range(1000):
	if f:
		pass
	if i == 200:
		C.__len__ = C.mylen
	if i == 400:
		C.__nonzero__ = C.mynonzero
	if i == 600:
		del C.__len__
	if i == 800:
		del C.__nonzero__
print num_nonzero, num_len
