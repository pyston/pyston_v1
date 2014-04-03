x = 10
y = x - 1
z = 1
while x:
    x = x - 1
    if x == 2:
        break
        # dead but legal:
        0
        continue
    elif x == 5:
        continue
    print x
else:
    print "finished"
print y, z
