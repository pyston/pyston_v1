# Refcounting

Pyston for the most part handles refcounts in the same way that CPython does.

`pyston_dbg` automatically turns on CPython's `Py_REF_DEBUG`, which adds some extra refcount checks.  This checks for some simple forms of mistakes such as decref'ing an object to a refcount below 0.  We also turn on `WITH_PYMALLOC` and `PYMALLOC_DEBUG`, which means that at as soon as an object gets freed it has all of its memory wiped, so any use-after-free bugs will also get caught.

In addition, on exit we do some more serious checking than CPython does.  We go to decent lengths to completely tear down the runtime and then check that there are zero objects alive.

Also of note is that in the debug build, there is the `_Py_RefTotal` global variable that contains the sum of all objects' refcounts.  It can be handy to watch this variable (useful gdb command: `display _Py_RefTotal`), and this is how we assert that zero objects are alive at the end of the program.

## Refcounting discipline

Each object has a refcount, but from the point of view of a *program*, the program operates on an abstract sense of "owned references".  These references refer to the variables in the source code, not actual heap objects.  Maintaining correct refcounts means constructing your program to correctly maintain owned references to all the variables it touches.  This usually means that through every possible path through your function, the net ref change is zero.

Sometimes there are also "borrowed references".  These are references that someone else is promising to keep alive for you.

Unless specified, arguments are borrowed and return values are returned as owned.  This can be specified by saying an argument is `STOLEN(Box*)` or that a return value is `BORROWED(Box*)`.  STOLEN and BORROWED are no-ops that are just for documentation (and eventually linting).

For example, take this function:

```C++
Box* doSomething(Box* arg) {
    Box* x;
    if (some_condition()) {
        x = arg;
    } else {
        x = calculate();
    }
    return x;
}
```

To make it safe, we need to add a `Py_INCREF(x);` call after the `x = arg` assignment.  This is because the `return` statement expects to received an owned reference (such as is presumably returned by `calculate()`), and we need to add an incref to match that.

One additional thing to consider is that "all paths through your function" includes C++ exceptions that might get thrown.

### Differences from CPython

First off is exception-safety.  Since we have C++ exceptions, we need to make sure that references are maintained when an exception gets thrown via any function call.  This can be done with our helpers.

We have a number of C++ helpers that can make refcounting easier, or at least easier to write and view.  The most common ones are:

- `incref(o)`.  This is just a simple function that calls `Py_INCREF(o)` and then returns `o`.  This is useful to not have to break up a long expression in order to use the Py_INCREF macro.
- `autoDecref(o)`.  Similar to `incref`, but slightly different.  This will decref `o` at *the end of the containing expression*.  This means you can do something like `return unboxInt(autoDecref(boxInt(5)));`.  Without adding the autoDecref, the reference that boxInt returns will escape.  If autoDecref did its decref immediately, it wouldn't be safe to pass its result on.  This is exception-safe (will execute the decref even if an exception is thrown).
- `AUTO_DECREF(o);`.  This is a statement that evaluates to an RAII object that will decref its argument on block exit (or exception).  Under the hood it evaluates to something like `_tmp = autoDecref(o);`.  Note that this takes its argument *by value*, so if the value of `o` is changed, the original value is what will get decrefd.
- `KEEP_ALIVE(o)`.  This is equivalent to `AUTO_DECREF(incref(o))`.  This has no net ref effect, but will keep an object alive for the duration of the block.
- `DecrefHandle`.  This is the type that underlies autoDecref() and AUTO_DECREF.  It's a "managed reference holder", which automatically decrefs its pointed-to object.  You can also assign to it, which will decref any previous value it holds, and then decref the new value when appropriate.

### Special cases

Most exception-related functions steal refs.  This is actually similar to CPython, but with our C++ exceptions there a number of new functions that this applies to.  If you throw an exception in C code (via `throw [ExcInfo] e`), it consumes the refs.  When you catch an exception, you own the references to the caught ExcInfo structure.

Special / magic / scary note:
In CPython (and thus in Pyston), there are some times that the runtime passes a "borrowed reference" to a function but doesn't actually promise that it will stay alive.  This applies to tp_call and tp_descr_get.  CPython happens to (mostly) be safe from this, but we may have to be a bit more careful.

## Refcounting in the rewriter, baseline jit, and llvm jit

Refcounting in the JIT tiers is handled differently from normal code.  In this case, the JIT will do the work of figuring out what incref/decref operations are needed.  What's needed from the programmer is to specify annotations about the flow of references through the program.  This is mainly:

- `setType(RefType)` to establish the initial type (borrowed vs owned) of the reference.
- `refConsumed()` says that a reference was handed out of the JIT's set of objects.  This could be handling `return` statements, calling functions that steal a ref from their arguments, or storing the reference in a data structure (which thus inherits the reference).
- `refUsed()`.  This shouldn't be needed that often, but tells the refcounting system that there was a use of a reference that the refcounter was unable to see.  For example, we have some calling conventions where objects are passed in an array; the uses of the array are thus uses of the individual variables.  You probably don't have to worry about this.

The automatic refcounter will then look at the IR we generated (either the LLVM IR or our Rewriter IR) and determine where to insert incref and decref operations.  Note that it will insert decrefs at the first possible point that it thinks they are acceptable.  So if you have some use of your variable that is not visible to the refcounter, you will need to call refUsed().  But for the most part things should just work.

Here's an example of what it can look like.  The case is storing a variable in an array.

```C++
void storeVar(RewriterVar* val, RewriterVar* array) {
    RewriterVar* old_var = array->getAttr(0);
    old_var->setType(RefType::OWNED)->setNullable(true);
    array->setAttr(0, val);
    val->refConsumed();
}
```

Note that we get the previous value and then set the type to OWNED.  The refcounter will see this, then make sure that this value gets to zero net references, and emit a decref (an xdecref in this case since setNullable was called).

## Updating C extensions

C extensions don't require any updates, but they often leak some references (usually a constant number) which the refcounting system
can't distinguish from steady-state leaks.  For standard libraries that are implemented as C extensions, it's usually nice to go through and fix these so that the system can get down to "0 refs remaining" at finalization.

Usually this is just a matter of calling PyGC_RegisterStaticConstant() around any static constants.  This will usually be things like static strings, or sometimes singletons or interned values.

Similarly, this can happen in our code.  If you store something in a `static` C/C++ variable, you will typically have to call PyGC_RegisterStaticConstant.  There is a helper function for the common case -- getStaticString() is equivalent to `PyGC_RegisterStaticConstant(PyString_InternFromString())`, which happens a decent amount.

## Testing

Some misc testing notes:

- Most literals will get interned.  This also includes the constants produced via some simple constant folding.  This means that it is hard to use them as the object that you think might get collected or not (ie `does_this_get_freed_too_early(1)`).  I tend to use something like `2.0 ** 5`, since this is something that the JITs will not try to constant-fold.

## Debugging

First, this is all much much much easier when everything is super deterministic.

- Disable ASLR (`echo 0 | sudo tee /proc/sys/kernel/randomize_va_space`)
- Turn off ENABLE_JIT_OBJECT_CACHE
- Turn off pyc caching in the parser

There are two main classes of refcounting bugs: not increffing enough, and not decreffing enough.

Not increffing enough means that we will free an object too early.  When run in a debug build, we will overwrite all of the memory of freed objects, so this means we will typically try to dereference an invalid pointer like 0xdbdbdbdb.  It will also have an invalid refcount, so if the next operation is a decref, you will see an "Object has negative ref count" error message.  When any of this happens, I've found the most effective thing to do is to set a watchpoint on the objects refcount: `watch -l op->ob_refcnt`.  Then you can rerun the program and watch what all the refcount operations were.  I find it helpful to open up a text file and write down what each of the ref operations means, and then figure out which ones were extra or which ones were missing.

If the program ends with a message such as "10 refs remaining!", this means that we leaked some references.  The first thing to try is to bisect down the program; often this is enough to point you to what got leaked.  We also support running in `Py_TRACE_REFS` mode, in which we will keep track of all live objects and then print out a list at the end.  To enable this, uncomment `#define Py_TRACE_REFS` in from_cpython/Include/Python.h.  Then you can do something like `watch -l (('pyston::Box'*)0x2aaaaaaaa000)->ob_refcnt` and run through the program.


If the assertion `assert(var->reftype != RefType::UNKNOWN)` fails in Rewriter::commitReturning(), this means that someone forgot to call `setType` on a refcounted RewriterVar.  Unfortunately, we don't have any tracking of where RewriterVars get created, so we will again use GDB to help us.

- First, turn off ENABLE_JIT_OBJECT_CACHE in options.cpp (maybe we should add a command line flag for this).
-- You may need to type `run` a few (~3) times to make sure that the program enters steady-state behavior (on-disk caches get filled), though with the object cache off this shouldn't be necessary.
- When the assertion hits, go up to the `commitReturning()` stack frame that contains the assertion.
- Type `watch -l *(char**)&var->reftype`.  This adds a breakpoint that will get hit whenever the reftype field gets modified.  The `-l` flag tells gdb to evaluate the expression once and then watch the resulting address (rather than symbolically reevaluating it each time).  The `*(char**)&` part is a workaround since otherwise GDB will fail to reset the breakpoint when we do the next step.
- Type `run` to restart the program.  You should see the breakpoint get hit.  We are only interested in the last time the breakpoint gets hit, so we need to do the following:
- Type `continue` until the program exits.  Then do `info break` to see how many times the breakpoint got hit ("breakpoint already hit 47 times").  You could also count it out yourself but I find this much easier.
- Then do `run`, then `continue N-1` where N was the number of times the breakpoint got hit.
- Now we should be at the point that the RewriterVar got created.  You can do a backtrace to see who created the RewriterVar and thus where the annotation needs to get added.


Similarly, you can get this assertion about an llvm::Value*, in which case you should try `watch -l *(void**)v`.
