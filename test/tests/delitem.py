a=[i for i in range(10)]
del a[0]
print a
del a[-1]
print a
del a[1]
print a

del a[0:2] 
print a 
del a[1:3:1]
print a
#test del all
del a[:]
print a
a.append(1)
print a

# Make sure that del's work correctly in sub-scopes:
x = 1
def f1():
    x = range(5)
    def f2():
        del x[1]
    return f2
f1()()
