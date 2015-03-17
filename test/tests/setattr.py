
class Test(object):
    def __setattr__(self, name, val):
        print name, val

t = Test()
t.hello = "world"
