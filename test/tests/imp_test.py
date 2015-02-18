import imp
print len(imp.find_module("os"))
print imp.find_module("encodings")[0]
