try:
    import non_existent_module
    assert 0, "shouldn't get here"
except ImportError:
    pass

try:
    from non_existent_module import a
    assert 0, "shouldn't get here"
except ImportError:
    pass

try:
    from sys import non_existent_attribute
    assert 0, "shouldn't get here"
except ImportError:
    pass
