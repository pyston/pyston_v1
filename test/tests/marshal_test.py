import marshal
o = [-1, 1.23456789, complex(1.2, 3.4)]
o += [True, False, None]
o += ["Hello World!", u"Hello World!", intern("Interned")]
o += [{ "Key" : "Value" }, set(["Set"]), frozenset(["FrozenSet"]), (1, 2, 3), [1, 2, 3]]
for i in o:
    s = marshal.dumps(i)
    r = marshal.loads(s)
    print "Dumping:", i, "Loaded", r
