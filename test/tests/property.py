class C(object):
    def fget(self):
        return 5

    x = property(fget)

c = C()
print c.x
