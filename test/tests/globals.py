def f():
    z = 1
    print x # non-builtin, but defined
    print True # builtin, redefined
    print False # builtin, not redefined
    print z # local
    try:
        print y # non-builtin, not defined
    except NameError, e:
        print e

x = 2
z = 2
True = "new_true"
f()

assert globals() is globals()
