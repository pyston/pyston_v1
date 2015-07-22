import gc

# Dynamically create new classes and instances of those classes in such a way
# that both the class object and the instance object will be freed in the same
# garbage collection pass. Hope that this doesn't cause any problems.
def generateClassAndInstances():
    for i in xrange(12000):
        NewType = type("Class" + str(i), (), {})
        obj = NewType()

generateClassAndInstances()
gc.collect()
gc.collect()
