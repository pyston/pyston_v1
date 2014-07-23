# The __all__ variable must support iteration, through the old-style (__getitem__) protocol

z = 123

class C(object):
    def __getitem__(self, idx):
        print "__getitem__", idx
        if idx == 0:
            return 'z'
        raise IndexError

__all__ = C()

