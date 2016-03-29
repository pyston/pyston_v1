class CustomInt(int):
    pass

print int.__new__(CustomInt, "1")

class CustomFloat(float):
    pass
print float.__new__(CustomFloat, "1.0")

class CustomLong(long):
    pass
print long.__new__(CustomLong, "10")
