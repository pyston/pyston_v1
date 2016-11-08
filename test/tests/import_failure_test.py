try:
    import non_existent_module
    assert 0, "shouldn't get here"
except ImportError, e:
    print e

try:
    from non_existent_module import a
    assert 0, "shouldn't get here"
except ImportError, e:
    print e

try:
    from sys import non_existent_attribute
    assert 0, "shouldn't get here"
except ImportError, e:
    print e

# Run it all again inside a function scope:
def f():
    try:
        import non_existent_module
        assert 0, "shouldn't get here"
    except ImportError, e:
        print e

    try:
        from non_existent_module import a
        assert 0, "shouldn't get here"
    except ImportError, e:
        print e

    try:
        from sys import non_existent_attribute
        assert 0, "shouldn't get here"
    except ImportError, e:
        print e

    try:
        print os
        1/0
    except NameError, e:
        print e

    try:
        import os, aoeu
    except ImportError, e:
        print e

    print type(os)
f()

def f2():
    try:
        from os import path, doesnt_exist
    except ImportError, e:
        print e

    print type(path)
    try:
        print doesnt_exist
    except NameError, e:
        print e
f2()

try:
    import import_failure_target
    raise Exception("This should not be importable")
except Exception, e:
    print type(e), e

try:
    import import_failure_target
    raise Exception("This should not be importable if we tried it again")
except Exception, e:
    print type(e), e
