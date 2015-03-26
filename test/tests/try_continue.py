# expected: fail
# Syntax error to have a continue outside a loop.
def foo():
    try: continue
    finally: pass
