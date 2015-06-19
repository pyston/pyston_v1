# Using exceptions safely in Pyston

In addition to following general best practices for writing exception-safe C++, when writing Pyston there are a few special rules (because it has a custom unwinder):

1. **Only throw `ExcInfo` values.** All Pyston exceptions are of type `ExcInfo`, which represents a Python exception. In fact, usually you should never `throw`; instead, call `raiseRaw`, `raiseExc`, `raise3`, or similar.

2. **Always catch by value.** That is, always write:

   ```c++
   try { ... } catch (ExcInfo e) { ... } // Do this!
   ```

   And **never** write:

   ```c++
   try { ... } catch (ExcInfo& e) { ... } // DO NOT DO THIS!
   ```

   The reason for this has to do with the way exceptions are stored in thread-local storage in Pyston; see `docs/UNWINDING.md` for the gory details.

3. **Never rethrow with bare `throw;`.** Instead, write `throw e;`, where `e` is the exception you caught previously.

4. **Never invoke the GC from a destructor.** The GC is not currently aware of the place the exception-currently-being-unwound is stored. Invoking the GC from a destructor might collect the exception, producing a use-after-free bug!

5. **Never throw an exception inside a destructor.** This is a general rule in C++ anyways, but worth reiterating here. In fact, don't even invoke code that *throws an exception but handles it*! This, again, has to do with the way exceptions are stored.

6. **Don't throw exceptions inside signal handlers.** It should be okay if you throw an exception and *always* catch it inside the handler, but I haven't tested this. In theory the exception should just unwind through the signal frame, and libunwind will take care of resetting the signal mask. However, as this codepath hasn't been tested, it's best avoided.

Most of these restrictions could be eliminated in principle. See `docs/UNWINDING.md` for the gory details.
