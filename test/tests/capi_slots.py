import slots_test

for i in xrange(3):
    t = slots_test.SlotsTesterSeq(i + 5)
    print t, repr(t), str(t), t(), t[2]
    print hash(t), t < 1, t > 2, t != 3, bool(t)

# print slots_test.SlotsTesterSeq.__doc__
print slots_test.SlotsTesterSeq.set_through_tpdict, slots_test.SlotsTesterSeq(5).set_through_tpdict

for i in xrange(3):
    t = slots_test.SlotsTesterMap(i + 5)
    print len(t), t[2], repr(t), str(t)
    t[1] = 5
    del t[2]

for i in xrange(3):
    t = slots_test.SlotsTesterNum(i)
    print bool(t)

class C(object):
    def __repr__(self):
        print "__repr__()"
        return "<C object>"

    def __getitem__(self, idx):
        print "__getitem__", idx
        return idx - 5

    def __len__(self):
        print "__len__"
        return 3

slots_test.call_funcs(C())

# Test to make sure that updating an existing class also updates the tp_* slots:
def repr2(self):
    return "repr2()"
C.__repr__ = repr2

def nonzero(self):
    print "nonzero"
    return True
C.__nonzero__ = nonzero

slots_test.call_funcs(C())
