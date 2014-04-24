class C(object):
    def __getitem__(self, idx):
        print idx
c = C()
c[1]
c[1:2]

sl = slice(1, 2)
print sl, sl.start, sl.stop, sl.step

sl = slice([])
print sl

sl = slice(1, 2, "hello")
print sl
