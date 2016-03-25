from types import ModuleType

class MyModule(ModuleType):
    # Make sure that we can override __init__:
    def __init__(self, fn, doc, myarg):
        super(MyModule, self).__init__(fn, doc)
        self.myarg = myarg

print MyModule('o', "doc", 1)

