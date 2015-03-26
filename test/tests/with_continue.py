# should_error
class Mgr(object):
    def __enter__(self):
        print 'entered!'
    def __exit__(self, *args):
        print 'exited!'

with Mgr() as m:
    continue
