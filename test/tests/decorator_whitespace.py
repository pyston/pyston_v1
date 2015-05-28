# fail-if: '-x' not in EXTRA_JIT_ARGS

def dec(f):
    return f

@dec

def f():
    pass
