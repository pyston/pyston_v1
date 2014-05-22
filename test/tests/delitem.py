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
