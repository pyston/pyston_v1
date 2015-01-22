# Regression test: __dict__ should be accessible from __getattr__

class C(object):
    def __getattr__(self, attr):
        print len(self.__dict__)
        return 5

print C().a
