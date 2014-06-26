class TestResult(object):
    """
    OMG PONIES
    """
    pass

tr = TestResult()
assert tr.__doc__ == '\n    OMG PONIES\n    '

class TestResult2(object):
    """
    OMG PONIES 2
    """
    def __init__(self):
        """ THIS SHOULD BE IGNORED """
        pass

tr = TestResult2()
assert tr.__doc__ == '\n    OMG PONIES 2\n    '
