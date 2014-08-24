# Regression test: we rely internally on CompilerType.nonzero() always returning a BOOL,
# in at least these two cases.

def f(x):
    if x:
        pass

    print not x

f(None)
f({})
f([])
f({1})
f(())
f("")
f(0.0)
f(1L)
