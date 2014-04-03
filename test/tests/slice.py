class C(object):
    def __getitem__(self, idx):
        print idx
c = C()
c[1]
c[1:2]
