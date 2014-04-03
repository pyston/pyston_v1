# Osr test where we will OSR into a version with speculations, but where the existing speculations will already be false

def i(y):
    print y * 2
    return "hello"

n = 20000
x = 0
while n:
    n = n - 1
    x = i(x) * 2
print x
