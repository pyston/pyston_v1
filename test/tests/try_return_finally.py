def f():
    try:
        # Looks like this returns from the function, but it needs to go to the finally block
        return
    finally:
        pass
f()
