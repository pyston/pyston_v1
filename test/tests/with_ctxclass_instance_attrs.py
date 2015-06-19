class Mgr(object):
    def __init__(self): self.__enter__ = 'sucks to be you'
    def __enter__(self): pass
    def __exit__(self, typ, val, tback): pass

with Mgr() as m:
    print 'boom boom boom boom'

class Mgr2(object):
    def __init__(self): self.__exit__ = 'screwed!'
    def __enter__(self): pass
    def __exit__(self, typ, val, tback): pass

with Mgr2() as m:
    print 'bang bang bang bang'
