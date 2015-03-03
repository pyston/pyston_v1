# Random regression test:

def _escape():
    code = DOESNT_EXIST
    if code:
        return code

try:
    _escape()
except NameError, e:
    print e
