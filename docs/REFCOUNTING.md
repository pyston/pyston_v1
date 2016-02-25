# Refcounting

Pyston for the most part handles refcounts in the same way that CPython does.

`pyston_dbg` automatically turns on CPython's `Py_REF_DEBUG`, which adds some extra refcount checks.  This checks for some simple forms of mistakes such as decref'ing an object to a refcount below 0.  We also turn on `WITH_PYMALLOC` and `PYMALLOC_DEBUG`, which means that at as soon as an object gets freed it has all of its memory wiped, so any use-after-free bugs will also get caught.

In addition, on exit we do some more serious checking than CPython does.  We go to decent lengths to completely tear down the runtime and then check that there are zero objects alive.

Also of note is that in the debug build, there is the `_Py_RefTotal` global variable that contains the sum of all objects' refcounts.  It can be handy to watch this variable (useful gdb command: `display _Py_RefTotal`), and this is how we assert that zero objects are alive at the end of the program.

## Refcounting discipline

Unless specified, arguments are borrowed and return values are owned.  This can be specified by saying an argument is `STOLEN(Box*)` or that a return value is `BORROWED(Box*)`.  STOLEN and BORROWED are no-ops that are just for documentation (and eventually linting).

Exception safety

Helpers:
AUTO_DECREF, autoDecref, incref

If you get a use-after-free (typically a segfault where we tried to access dead memory that looks like 0xdbdbdbdb): `watch -l op->ob_refcnt`.  I find it helpful to open up a text file and write down what each of the ref operations means, and then figure out which ones were extra or which ones were missing.

If you get extra refs at the end, try to bisect down the program until you find the point that the ref was leaked.  Unfortunately there is not a good way to enumerate all live objects to figure out what they are.

## Refcounting in the rewriter, baseline jit, and llvm jit
