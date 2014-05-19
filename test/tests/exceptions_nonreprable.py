# expected: fail
# - inheritance

class BadException(Exception):
    def __str__(self):
        print "str"
        raise NotImplementedError()

try:
    # This will raise:
    print BadException()
    assert 0
except NotImplementedError:
    pass

raise BadException()
