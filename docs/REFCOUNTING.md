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

Invariant: all paths through a function should leave each variable with 0 net refs.  (This includes paths out via exceptions.)

## Refcounting in the rewriter, baseline jit, and llvm jit

## Debugging

A use-after-free will typically show up as a segfault (where the accessed memory location is something like 0xdbdbdbdb), or as an "Object has negative ref count" error message.  In both cases, I find it best to do `watch -l op->ob_refcnt`.  I find it helpful to open up a text file and write down what each of the ref operations means, and then figure out which ones were extra or which ones were missing.

If the program ends with a message such as "10 refs remaining!", try to bisect down the program until you find the point that the ref was leaked.  Unfortunately there is not a good way to enumerate all live objects to figure out what they are.


If the assertion `assert(var->reftype != RefType::UNKNOWN)` fails in Rewriter::commitReturning(), this means that someone forgot to call `setType` on a refcounted RewriterVar.  Unfortunately, we don't have any tracking of where RewriterVars get created, so we will use GDB to help us.

- Type `run` a few (~3) times to make sure that the program enters steady-state behavior (on-disk caches get filled).
- When the assertion hits, go up to the `commitReturning()` stack frame that contains the assertion.
- Type `watch -l *(char**)&var->reftype`.  This adds a breakpoint that will get hit whenever the reftype field gets modified.  The `-l` flag tells gdb to evaluate the expression once and then watch the resulting address (rather than symbolically reevaluating it each time).  The `*(char**)&` part is a workaround since otherwise GDB will fail to reset the breakpoint when we do the next step.
- Type `run` to restart the program.  You should see the breakpoint get hit.  We are only interested in the last time the breakpoint gets hit, so we need to do the following:
- Type `continue` until the program exits.  Then do `info break` to see how many times the breakpoint got hit ("breakpoint already hit 47 times").  You could also count it out yourself but I find this much easier.
- Then do `run`, then `continue N-1` where N was the number of times the breakpoint got hit.
- Now we should be at the point that the RewriterVar got created.  You can do a backtrace to see who created the RewriterVar and thus where the annotation needs to get added.

Similarly, you can get this assertion about an llvm::Value*, in which case you should try `watch -l *(void**)v`.
