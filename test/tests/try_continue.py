# should_error
# skip-if: '-x' in EXTRA_JIT_ARGS
# Syntax error to have a continue outside a loop.
def foo():
    try: continue
    finally: pass
