class Index(object):
    def __init__(self, i):
        self.i = i

    def __index__(self):
        print "called __index__"
        return self.i

s = "String"
l = [1, 2, "List"]
t = (1, 2, "Tuple")
print s[Index(2L)]
print l[Index(2L)]
print t[Index(2L)]
