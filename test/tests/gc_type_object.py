import gc

# Dynamically create new classes and instances of those classes in such a way
# that both the class object and the instance object will be freed in the same
# garbage collection pass. Hope that this doesn't cause any problems.
def generateClassAndInstances():
    for i in xrange(5000):
        def method(self, x):
            return x + self.i
        NewType1 = type("Class1_" + str(i), (),
                dict(a={}, b=range(10), i=1, f=method))
        NewType2 = type("Class2_" + str(i), (object,),
                dict(a={}, b=range(10), i=2, f=method))
        NewType3 = type("Class3_" + str(i), (NewType2,), {})
        NewType4 = type("Class4_" + str(i), (NewType3,), {})
        NewType5 = type("Class5_" + str(i), (NewType4,), {})
        obj1 = NewType1()
        obj2 = NewType2()
        obj3 = NewType3()
        obj4 = NewType4()
        obj5 = NewType5()

generateClassAndInstances()
gc.collect()
gc.collect()
