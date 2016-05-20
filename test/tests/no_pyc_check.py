import os.path
try:
    assert not os.path.exists(__file__ + 'c')
except AssertionError:
    os.unlink(__file__ + 'c')
    raise
