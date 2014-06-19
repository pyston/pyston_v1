s = lambda x: x**2
print s(8), s(100)

for i in range(10):
    print (lambda x, y: x < y)(i, 5)

t = lambda s: " ".join(s.split())
print t("test \tstr\ni\n ng")
