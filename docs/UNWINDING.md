# The Pyston Unwinder

Pyston uses a custom exception unwinder, replacing the general-purpose C++ unwinder provided by `libstdc++` and `libgcc`. We do this for two reasons:

1. **Efficiency**. The default clang/gcc C++ unwinder is slow, because it needs to support features we don't (such as two-phase unwinding, and having multiple exception types) and because it isn't optimized for speed (C++ assumes exceptions are uncommon).

2. **Customizability**. For example, Python handles backtraces differently than C++ does; with a custom unwinder, we can support Python-style backtraces more easily.

The custom unwinder is in `src/runtime/cxx_unwind.cpp`.

### Useful references on C++ exception handling

- [https://monoinfinito.wordpress.com/series/exception-handling-in-c/](https://monoinfinito.wordpress.com/series/exception-handling-in-c/): Good overview of C++ exceptions.
- [http://www.airs.com/blog/archives/460](http://www.airs.com/blog/archives/460): Covers dirty details of `.eh_frame`.
- [http://www.airs.com/blog/archives/464](http://www.airs.com/blog/archives/464): Covers dirty details of the personality function and the LSDA.

# How normal C++ unwinding works

The big picture is that when an exception is thrown, we walk the stack *twice*:

1. In the first phase, we look for a `catch`-block whose type matches the thrown exception. If we don't find one, we terminate the process.

2. In the second phase, we unwind up to the `catch`-block we found; along the way we run any intervening `finally` blocks or RAII destructors.

The purpose of the two-phase search is to make sure that *exceptions that won't be caught terminate the process immediately with a full stack-trace*. In Pyston we don't care about this --- stack traces work differently for us anyway.

## How normal C++ unwinding works, in detail

### Throwing

C++ `throw` statements are translated into a pair of method calls:

1. A call to `void *__cxxabiv1::__cxa_allocate_exception(size_t)` allocates space for an exception of the given size.

2. A call to `void __cxxabiv1::__cxa_throw(void *exc_obj, std::type_info *type_info, void (*dtor)(void*))` invokes the stack unwinder. `exc_obj` is the exception to be thrown; `type_info` is the RTTI for the exception's class, and `dtor` is a callback that (I think) is called to destroy the exception object.

These methods (and others in the `__cxxabiv1` namespace) are defined in `libstdc++`. `__cxa_throw` invokes the generic (non-C++-specific) unwinder by calling `_Unwind_RaiseException()`. This function (and others prefixed with `_Unwind`) are defined in `libgcc`. The details of the libgcc unwinder's interface are less important, and I omit them here.

### Unwinding and .eh_frame

The libgcc unwinder walks the call frame stack, looking up debug information about each function it unwinds through. It finds the debug information by searching for the instruction pointer that would be returned-to in a list of tables; one table for each loaded object (in the linker-and-loader sense of "object", i.e. executable file or shared library). For a given object, the debug info is in a section called `.eh_frame`. See [this blog post](http://www.airs.com/blog/archives/460) for more on the format of `.eh_frame`.

In particular, the unwinder checks whether the function has an associated "personality function", and calls it if it does. If there's no personality function, unwinding continues as normal. C functions do not have personality functions. C++ functions have the personality function `__gxx_personality_v0`, or (if they don't involve exceptions or RAII at all) no personality function.

The job of the personality function is to:

1. Determine what action, if any, needs to happen when unwinding this exception through this frame.

2. If we are in Phase 1, or if there is no action to be taken, report this information to the caller.

3. If we are in Phase 2, actually take the relevant action: jump into the relevant cleanup code, `finally`, or `catch` block. In this case, the personality function does not return.

### The LSDA, landing pads and switch values: how the personality function works

The personality function determines what to do by comparing the instruction pointer being unwound through against C++-specific unwinding information. This is contained in an area of `.eh_frame` called the LSDA (Language-Specific Data Area). See [this blog post](http://www.airs.com/blog/archives/464) for a detailed run-down.

If the personality function finds a "special" action to perform when unwinding, it is associated with two values:

- The *landing pad*, a code address, determined by the instruction pointer value.
- The *switch value*, an `int64_t`. This is *zero* if we're running cleanup code (RAII destructors or a `finally` block); otherwise it is an index that indicates *which* `catch` block we've matched (since there may be several `catch` blocks covering the code region we're unwinding through).

If we're in phase 2, the personality function then jumps to the landing pad, after (a) restoring execution state for this call frame and (b) storing the exception object pointer and the switch value in specific registers (`RAX` and `RDX` respectively). The code at the landing pad is emitted by the C++ compiler as part of the function being unwound through, and it dispatches on the switch value to determine what code to actually run.

It dispatches to code in one of two flavors: *cleanup code* (`finally` blocks and RAII destructors), or *handler code* (`catch` blocks).

#### Cleanup code (`finally`/RAII)

Cleanup code does what you'd expect: calls the appropriate destructors and/or runs the code in the appropriate `finally` block. It may also call `__cxa_end_catch()`, if we are unwinding out of a catch block - think of `__cxa_begin_catch()` and `__cxa_end_catch()` as like RAII constructor/destructor pairs; the latter is guaranteed to get called when leaving a catch block, whether normally or by exception.

After this is done, it calls `_Unwind_Resume()` to resume unwinding, passing it the exception object pointer that it received in `RAX` when the personality function jumped to the landing pad.

#### Handler code (`catch`)

Handler code, first of all, may *also* call RAII destructors or other cleanup code if necessary. After that, it *may* call `__cxa_get_exception_ptr` with the exception object pointer. I'm not sure why it does this, but it expects `__cxa_get_exception_ptr` to also *return* a pointer to the exception object, so it's effectively a no-op. (I think in a normal C++ unwinder maybe there's an exception *header* as well, and some pointer arithmetic going on, so that the pointer passed in `RAX` to the landing pad and the exception object itself are different?)

After this, it calls `__cxa_begin_catch()` with the exception object pointer. Again, `__cxa_begin_catch()` is expected to return the exception object pointer, so in Pyston this is basically a no-op. (Again, maybe there's some funky pointer arithmetic going on in regular C++ unwinding - I'm not sure.)

Then, *if* the exception is caught by-value (`catch (ExcInfo e)`) rather than by-reference (`catch (ExcInfo& e)`) - and Pyston must *always* catch by value - it copies the exception object onto the stack.

Then it runs the code inside the catch block, like you'd expect.

Finally, it calls `__cxa_end_catch()` (which takes no arguments). In regular C++ this destroys the current exception if appropriate. (It grabs the exception out of some thread-specific data structure that I don't fully understand.)

# How our unwinder is different

We use `libunwind` to deal with a lot of the tedious gruntwork (restoring register state, etc.) of unwinding.

First, we dispense with two-phase unwinding. It's slow and Python tracebacks work differently anyway. (Currently we grab tracebacks before we start unwinding; in the future, we ought to generate them incrementally *as* we unwind.)

Second, we allocate exceptions using a thread-local variable, rather than `malloc()`. By ensuring that only one exception is ever active on a given thread at a given time, this lets us be more efficient. However, we have not measured the performance improvement here; it may be negligible.

Third, when unwinding, we only check whether a function *has* a personality function. If it does, we assert that it is `__gxx_personality_v0`, but we *do not call it*. Instead, we run our own custom dispatch code. We do this because:

1. One argument to the personality function is the current unwind context, in a `libgcc`-specific format. libunwind uses a different format, so we *can't* call it.

2. It avoids an unnecessary indirect call.

3. The personality function checks the exception's type against `catch`-block types. All Pyston exceptions have the same type, so this is unnecessary.

## Functions we override
- `std::terminate`
- `__gxx_personality_v0`: stubbed out, should never be called
- `_Unwind_Resume`
- `__cxxabiv1::__cxa_allocate_exception`
- `__cxxabiv1::__cxa_begin_catch`
- `__cxxabiv1::__cxa_end_catch`
- `__cxxabiv1::__cxa_throw`
- `__cxxabiv1::__cxa_rethrow`: stubbed out, we never rethrow directly
- `__cxxabiv1::__cxa_get_exception_ptr`

# Future work

## Incremental traceback generation

Python tracebacks include only the area of the stack between where the exception was originally raised and where it gets caught. Currently we generate tracebacks (via `getTraceback`) using `unwindPythonStack()` in `src/codegen/unwinding.cpp`, which unwinds the whole stack at once.

Instead we ought to generate them *as we unwind*. This should be a straightforward matter of taking the code in `unwindPythonStack` and integrating it into `unwind_loop` (in `src/runtime/cxx_unwind.cpp`), so that we keep a "current traceback" object that we update as we unwind the stack and discover Python frames.

## Binary search in libunwind

Libunwind, like libgcc, keeps a linked list of objects (executables, shared libraries) to search for debug info. Since it's a linked list, if it's very long we can't find debug info efficiently; a better way would be to keep an array sorted by the start address of the object (since objects are non-overlapping). This comes up in practice because LLVM JITs each function as a separate object.

libunwind's linked list is updated in `_U_dyn_register` (in `libunwind/src/mi/dyn-register.c`) and scanned in `local_find_proc_info` (in `libunwind/src/mi/Gfind_dynamic_proc_info.c`) (and possibly elsewhere).

## GC awareness

Currently we store exceptions-being-unwound in a thread-local variable, `pyston::exception_ferry` (in `src/runtime/cxx_unwind.cpp`). This is invisible to the GC. This *should* be fine, since this variable is only relevant during unwinding, and unwinding *should not* trigger the GC. `catch`-block code might, but as long as we catch by-value (`catch (ExcInfo e)` rather than `catch (ExcInfo& e)`), the relevant pointers will be copied to our stack (thus GC-visible) before any catch-block code is run. The only other problem is if *destructors* can cause GC, since destructors *are* called during unwinding and there's nothing we can do about that. So don't do that!

It wouldn't be too hard to make the GC aware of `pyston::exception_ferry`. We could either:
- add code to the GC that regards `pyston::exception_ferry` as a source of roots, OR
- store the exception ferry in `cur_thread_state` instead of its own variable, and update `ThreadStateInternal::accept`

HOWEVER, there's a problem: if we do this, we need to *zero out* the exception ferry at the appropriate time (to avoid keeping an exception alive after it ought to be garbage), and this is harder than it seems. We can't zero it out in `__cxa_begin_catch`, because it's only *after* `__cxa_begin_catch` returns that the exception is copied to the stack. We can't zero it in `__cxa_end_catch`, because `__cxa_end_catch` is called *even if exiting a catch block due to an exception*, so we'd wipe an exception that we actually wanted to propagate!

So this is tricky.

## Decrementing IC counts when unwinding through ICs

To do this, we need some way to tell when we're unwinding through an IC. Keeping a global map from instruction-ranges to IC information should suffice. Then we just check and update this map inside of `unwind_loop`. This might slow us down a bit, but it's probably negligible; worth measuring, though.

Alternatively, there might be some way to use the existing cleanup-code support in the unwinder to do this. That would involve generating EH-frames on the fly, but we already do this! So probably we'd just need to generate more complicated EH frames.
