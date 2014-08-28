# expected: fail
# - we currently can't even compile this correctly
#
# The unpacking should be atomic: ie either all of the names get set or none of them do.
# We currently assume that unpacking can only throw an exception from the tuple being the
# wrong length, but instead we should be emulating the UNPACK_SEQUENCE bytecode.

class C(object):
    def __getitem__(self, i):
        print "__getitem__", i
        if i == 0:
            return 1
        raise IndexError

    def __len__(self):
        print "len"
        return 2

def f():
    try:
        x, y = C()
    except ValueError:
        pass

    try:
        print x
    except NameError, e:
        print e
    try:
        print y
    except NameError, e:
        print e

f()
