# making sure we can deoptimize within a loop (line 9)

if 1:
    x = 2
else:
    x = d() # d is undefined

print x
