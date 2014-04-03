def f():
    z = 1
    print x # non-builtin, but defined
    print True # builtin, redefined
    print False # builtin, not redefined
    print z # local
    print y # non-builtin, not defined

x = 2
z = 2
True = "new_true"
f()
