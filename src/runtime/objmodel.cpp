// Copyright (c) 2014-2016 Dropbox, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "runtime/objmodel.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>
#include <stdint.h>

#include "asm_writing/icinfo.h"
#include "asm_writing/rewriter.h"
#include "capi/typeobject.h"
#include "capi/types.h"
#include "codegen/ast_interpreter.h"
#include "codegen/codegen.h"
#include "codegen/compvars.h"
#include "codegen/irgen/hooks.h"
#include "codegen/parser.h"
#include "codegen/type_recording.h"
#include "codegen/unwinding.h"
#include "core/ast.h"
#include "core/options.h"
#include "core/stats.h"
#include "core/types.h"
#include "runtime/classobj.h"
#include "runtime/dict.h"
#include "runtime/float.h"
#include "runtime/generator.h"
#include "runtime/hiddenclass.h"
#include "runtime/ics.h"
#include "runtime/iterobject.h"
#include "runtime/long.h"
#include "runtime/rewrite_args.h"
#include "runtime/types.h"
#include "runtime/util.h"

#ifndef NDEBUG
#define DEBUG 1
#else
#define DEBUG 0
#endif

namespace pyston {

static const std::string iter_str("__iter__");
static const std::string new_str("__new__");
static const std::string none_str("None");
static const std::string repr_str("__repr__");
static const std::string str_str("__str__");

#if 0
void REWRITE_ABORTED(const char* reason) {
}
#else
#define REWRITE_ABORTED(reason) ((void)(reason))
#endif

template <ExceptionStyle S, Rewritable rewritable>
static inline Box* runtimeCallInternal0(Box* obj, CallRewriteArgs* rewrite_args, ArgPassSpec argspec) {
    return runtimeCallInternal<S, rewritable>(obj, rewrite_args, argspec, NULL, NULL, NULL, NULL, NULL);
}
template <ExceptionStyle S, Rewritable rewritable>
static inline Box* runtimeCallInternal1(Box* obj, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1) {
    return runtimeCallInternal<S, rewritable>(obj, rewrite_args, argspec, arg1, NULL, NULL, NULL, NULL);
}
template <ExceptionStyle S, Rewritable rewritable>
static inline Box* runtimeCallInternal2(Box* obj, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1,
                                        Box* arg2) {
    return runtimeCallInternal<S, rewritable>(obj, rewrite_args, argspec, arg1, arg2, NULL, NULL, NULL);
}
template <ExceptionStyle S, Rewritable rewritable>
static inline Box* runtimeCallInternal3(Box* obj, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1,
                                        Box* arg2, Box* arg3) {
    return runtimeCallInternal<S, rewritable>(obj, rewrite_args, argspec, arg1, arg2, arg3, NULL, NULL);
}

bool checkClass(LookupScope scope) {
    return (scope & CLASS_ONLY) != 0;
}
bool checkInst(LookupScope scope) {
    return (scope & INST_ONLY) != 0;
}

template <ExceptionStyle S, Rewritable rewritable>
static inline Box* callattrInternal0(Box* obj, BoxedString* attr, LookupScope scope, CallattrRewriteArgs* rewrite_args,
                                     ArgPassSpec argspec) noexcept(S == CAPI) {
    return callattrInternal<S, rewritable>(obj, attr, scope, rewrite_args, argspec, NULL, NULL, NULL, NULL, NULL);
}
template <ExceptionStyle S, Rewritable rewritable>
static inline Box* callattrInternal1(Box* obj, BoxedString* attr, LookupScope scope, CallattrRewriteArgs* rewrite_args,
                                     ArgPassSpec argspec, Box* arg1) noexcept(S == CAPI) {
    return callattrInternal<S, rewritable>(obj, attr, scope, rewrite_args, argspec, arg1, NULL, NULL, NULL, NULL);
}

template <ExceptionStyle S, Rewritable rewritable>
static inline Box* callattrInternal2(Box* obj, BoxedString* attr, LookupScope scope, CallattrRewriteArgs* rewrite_args,
                                     ArgPassSpec argspec, Box* arg1, Box* arg2) noexcept(S == CAPI) {
    return callattrInternal<S, rewritable>(obj, attr, scope, rewrite_args, argspec, arg1, arg2, NULL, NULL, NULL);
}
template <ExceptionStyle S, Rewritable rewritable>
static inline Box* callattrInternal3(Box* obj, BoxedString* attr, LookupScope scope, CallattrRewriteArgs* rewrite_args,
                                     ArgPassSpec argspec, Box* arg1, Box* arg2, Box* arg3) noexcept(S == CAPI) {
    return callattrInternal<S, rewritable>(obj, attr, scope, rewrite_args, argspec, arg1, arg2, arg3, NULL, NULL);
}

extern "C" void xdecrefAll(int num, ...) {
    va_list va;
    va_start(va, num);

    for (int i = 0; i < num; i++) {
        Box* b = va_arg(va, Box*);
        Py_XDECREF(b);
    }

    va_end(va);
}

extern "C" Box* deopt(AST_expr* expr, Box* value) {
    STAT_TIMER(t0, "us_timer_deopt", 10);

    static StatCounter num_deopt("num_deopt");
    num_deopt.log();

    auto deopt_state = getDeoptState();

    // Should we only do this selectively?
    deopt_state.cf->speculationFailed();

    // Except of exc.type we skip initializing the exc fields inside the JITed code path (small perf improvement) that's
    // why we have todo it now if we didn't set an exception (which sets all fields)
    if (deopt_state.frame_state.frame_info->exc.type == NULL) {
        deopt_state.frame_state.frame_info->exc.traceback = NULL;
        deopt_state.frame_state.frame_info->exc.value = NULL;
    }

    AUTO_DECREF(deopt_state.frame_state.locals);
    return astInterpretDeopt(deopt_state.cf->md, expr, deopt_state.current_stmt, value, deopt_state.frame_state);
}

extern "C" void printHelper(Box* w, Box* v, bool nl) {
    // copied from cpythons PRINT_ITEM and PRINT_NEWLINE op handling code
    if (w == NULL || w == None) {
        w = PySys_GetObject("stdout");
        if (w == NULL)
            raiseExcHelper(RuntimeError, "lost sys.stdout");
    }

    // CPython comments:
    /* PyFile_SoftSpace() can exececute arbitrary code
       if sys.stdout is an instance with a __getattr__.
       If __getattr__ raises an exception, w will
       be freed, so we need to prevent that temporarily. */
    /* w.write() may replace sys.stdout, so we
     * have to keep our reference to it */
    Py_INCREF(w);
    AUTO_DECREF(w);

    int err = 0;

    if (v) {
        if (w != NULL && PyFile_SoftSpace(w, 0))
            err = PyFile_WriteString(" ", w);
        if (err == 0)
            err = PyFile_WriteObject(v, w, Py_PRINT_RAW);
        if (err == 0) {
            /* XXX move into writeobject() ? */
            if (PyString_Check(v)) {
                char* s = PyString_AS_STRING(v);
                Py_ssize_t len = PyString_GET_SIZE(v);
                if (len == 0 || !isspace(Py_CHARMASK(s[len - 1])) || s[len - 1] == ' ')
                    PyFile_SoftSpace(w, 1);
            }
#ifdef Py_USING_UNICODE
            else if (PyUnicode_Check(v)) {
                Py_UNICODE* s = PyUnicode_AS_UNICODE(v);
                Py_ssize_t len = PyUnicode_GET_SIZE(v);
                if (len == 0 || !Py_UNICODE_ISSPACE(s[len - 1]) || s[len - 1] == ' ')
                    PyFile_SoftSpace(w, 1);
            }
#endif
            else
                PyFile_SoftSpace(w, 1);
        }
    }

    if (err == 0 && nl) {
        if (w != NULL) {
            err = PyFile_WriteString("\n", w);
            if (err == 0)
                PyFile_SoftSpace(w, 0);
        }
        // Py_XDECREF(stream);
    }

    if (err != 0)
        throwCAPIException();
}

extern "C" void my_assert(bool b) {
    assert(b);
}

extern "C" void assertFail(Box* assertion_type, Box* msg) {
    RELEASE_ASSERT(assertion_type->cls == type_cls, "%s", assertion_type->cls->tp_name);
    if (msg) {
        BoxedString* tostr = str(msg);
        AUTO_DECREF(tostr);
        raiseExcHelper(static_cast<BoxedClass*>(assertion_type), "%s", tostr->data());
    } else {
        raiseExcHelper(static_cast<BoxedClass*>(assertion_type), (const char*)NULL);
    }
}

extern "C" void assertNameDefined(bool b, const char* name, BoxedClass* exc_cls, bool local_var_msg) {
    if (!b) {
        if (local_var_msg)
            raiseExcHelper(exc_cls, "local variable '%s' referenced before assignment", name);
        else
            raiseExcHelper(exc_cls, "name '%s' is not defined", name);
    }
}

extern "C" void assertFailDerefNameDefined(const char* name) {
    raiseExcHelper(NameError, "free variable '%s' referenced before assignment in enclosing scope", name);
}

extern "C" void raiseAttributeErrorStr(const char* typeName, llvm::StringRef attr) {
    assert(attr.data()[attr.size()] == '\0');
    raiseExcHelper(AttributeError, "'%s' object has no attribute '%s'", typeName, attr.data());
}

extern "C" void raiseAttributeErrorStrCapi(const char* typeName, llvm::StringRef attr) noexcept {
    assert(attr.data()[attr.size()] == '\0');
    PyErr_Format(AttributeError, "'%s' object has no attribute '%s'", typeName, attr.data());
}

extern "C" void raiseAttributeError(Box* obj, llvm::StringRef attr) {
    if (obj->cls == type_cls) {
        // Slightly different error message:
        assert(attr.data()[attr.size()] == '\0');
        raiseExcHelper(AttributeError, "type object '%s' has no attribute '%s'",
                       getNameOfClass(static_cast<BoxedClass*>(obj)), attr.data());
    } else {
        raiseAttributeErrorStr(getTypeName(obj), attr);
    }
}

extern "C" void raiseAttributeErrorCapi(Box* obj, llvm::StringRef attr) noexcept {
    if (obj->cls == type_cls) {
        // Slightly different error message:
        assert(attr.data()[attr.size()] == '\0');
        PyErr_Format(AttributeError, "type object '%s' has no attribute '%s'",
                     getNameOfClass(static_cast<BoxedClass*>(obj)), attr.data());
    } else {
        raiseAttributeErrorStrCapi(getTypeName(obj), attr);
    }
}

extern "C" PyObject* type_getattro(PyObject* o, PyObject* name) noexcept {
    assert(PyString_Check(name));
    BoxedString* s = static_cast<BoxedString*>(name);
    assert(PyString_CHECK_INTERNED(name));

    try {
        Box* r = getattrInternalGeneric<true, NOT_REWRITABLE>(o, s, NULL, false, false, NULL, NULL);
        if (!r && !PyErr_Occurred())
            PyErr_Format(PyExc_AttributeError, "type object '%.50s' has no attribute '%.400s'",
                         static_cast<BoxedClass*>(o)->tp_name, PyString_AS_STRING(name));
        return r;
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

extern "C" void raiseIndexErrorStr(const char* typeName) {
    raiseExcHelper(IndexError, "%s index out of range", typeName);
}

extern "C" void raiseIndexErrorStrCapi(const char* typeName) noexcept {
    PyErr_Format(IndexError, "%s index out of range", typeName);
}

extern "C" void raiseNotIterableError(const char* typeName) {
    raiseExcHelper(TypeError, "'%s' object is not iterable", typeName);
}

static void _checkUnpackingLength(i64 expected, i64 given) {
    if (given == expected)
        return;

    if (given > expected)
        raiseExcHelper(ValueError, "too many values to unpack");
    else {
        if (given == 1)
            raiseExcHelper(ValueError, "need more than %ld value to unpack", given);
        else
            raiseExcHelper(ValueError, "need more than %ld values to unpack", given);
    }
}

extern "C" Box** unpackIntoArray(Box* obj, int64_t expected_size, Box** out_keep_alive) {
    if (obj->cls == tuple_cls) {
        BoxedTuple* t = static_cast<BoxedTuple*>(obj);

        auto got_size = t->size();
        _checkUnpackingLength(expected_size, got_size);

        *out_keep_alive = incref(t);
        for (auto e : *t)
            Py_INCREF(e);
        return &t->elts[0];
    } else if (obj->cls == list_cls) {
        assert(obj->cls == list_cls);

        BoxedList* l = static_cast<BoxedList*>(obj);

        auto got_size = l->size;
        _checkUnpackingLength(expected_size, got_size);

        *out_keep_alive = incref(l);
        for (size_t i = 0; i < l->size; i++)
            Py_INCREF(l->elts->elts[i]);
        return &l->elts->elts[0];
    } else {
        BoxedTuple* keep_alive = BoxedTuple::create(expected_size);
        AUTO_DECREF(keep_alive);

        int i = 0;
        for (auto e : obj->pyElements()) {
            if (i >= expected_size) {
                Py_DECREF(e);
                _checkUnpackingLength(expected_size, i + 1);
                // unreachable:
                abort();
            }

            keep_alive->elts[i] = e;
            i++;
        }
        _checkUnpackingLength(expected_size, i);

        *out_keep_alive = incref(keep_alive);
        for (auto e : *keep_alive)
            Py_INCREF(e);
        return &keep_alive->elts[0];
    }

    abort();
}

static void clear_slots(PyTypeObject* type, PyObject* self) noexcept {
    Py_ssize_t i, n;
    PyMemberDef* mp;

    n = Py_SIZE(type);
    mp = PyHeapType_GET_MEMBERS((BoxedHeapClass*)type);
    for (i = 0; i < n; i++, mp++) {
        if (mp->type == T_OBJECT_EX && !(mp->flags & READONLY)) {
            char* addr = (char*)self + mp->offset;
            PyObject* obj = *(PyObject**)addr;
            if (obj != NULL) {
                *(PyObject**)addr = NULL;
                Py_DECREF(obj);
            }
        }
    }
}

static void subtype_dealloc(Box* self) noexcept {
    PyTypeObject* type, *base;
    destructor basedealloc;
    PyThreadState* tstate = PyThreadState_GET();

    /* Extract the type; we expect it to be a heap type */
    type = Py_TYPE(self);
    assert(type->tp_flags & Py_TPFLAGS_HEAPTYPE);

    /* Test whether the type has GC exactly once */

    if (!PyType_IS_GC(type)) {
        /* It's really rare to find a dynamic type that doesn't have
           GC; it can only happen when deriving from 'object' and not
           adding any slots or instance variables.  This allows
           certain simplifications: there's no need to call
           clear_slots(), or DECREF the dict, or clear weakrefs. */

        /* Maybe call finalizer; exit early if resurrected */
        if (type->tp_del) {
            type->tp_del(self);
            if (self->ob_refcnt > 0)
                return;
        }

        /* Find the nearest base with a different tp_dealloc */
        base = type;
        while ((basedealloc = base->tp_dealloc) == subtype_dealloc) {
            assert(Py_SIZE(base) == 0);
            base = base->tp_base;
            assert(base);
        }

        /* Extract the type again; tp_del may have changed it */
        type = Py_TYPE(self);

        /* Call the base tp_dealloc() */
        assert(basedealloc);
        basedealloc(self);

        /* Can't reference self beyond this point */
        Py_DECREF(type);

        /* Done */
        return;
    }

    /* We get here only if the type has GC */

    /* UnTrack and re-Track around the trashcan macro, alas */
    /* See explanation at end of function for full disclosure */
    PyObject_GC_UnTrack(self);
    ++_PyTrash_delete_nesting;
    ++tstate->trash_delete_nesting;
    Py_TRASHCAN_SAFE_BEGIN(self);
    --_PyTrash_delete_nesting;
    --tstate->trash_delete_nesting;
    /* DO NOT restore GC tracking at this point.  weakref callbacks
     * (if any, and whether directly here or indirectly in something we
     * call) may trigger GC, and if self is tracked at that point, it
     * will look like trash to GC and GC will try to delete self again.
     */

    /* Find the nearest base with a different tp_dealloc */
    base = type;
    while ((basedealloc = base->tp_dealloc) == subtype_dealloc) {
        base = base->tp_base;
        assert(base);
    }

    /* If we added a weaklist, we clear it.      Do this *before* calling
       the finalizer (__del__), clearing slots, or clearing the instance
       dict. */

    if (type->tp_weaklistoffset && !base->tp_weaklistoffset)
        PyObject_ClearWeakRefs(self);

    /* Maybe call finalizer; exit early if resurrected */
    if (unlikely(type->tp_del)) {
        _PyObject_GC_TRACK(self);
        type->tp_del(self);
        if (self->ob_refcnt > 0)
            goto endlabel; /* resurrected */
        else
            _PyObject_GC_UNTRACK(self);
        /* New weakrefs could be created during the finalizer call.
            If this occurs, clear them out without calling their
            finalizers since they might rely on part of the object
            being finalized that has already been destroyed. */
        if (type->tp_weaklistoffset && !base->tp_weaklistoffset) {
            /* Modeled after GET_WEAKREFS_LISTPTR() */
            PyWeakReference** list = (PyWeakReference**)PyObject_GET_WEAKREFS_LISTPTR(self);
            while (*list)
                _PyWeakref_ClearRef(*list);
        }
    }

    /*  Clear slots up to the nearest base with a different tp_dealloc */
    base = type;
    while (base->tp_dealloc == subtype_dealloc) {
        if (unlikely(Py_SIZE(base)))
            clear_slots(base, self);
        base = base->tp_base;
        assert(base);
    }

    /* If we added a dict, DECREF it */
    if (type->tp_dictoffset && !base->tp_dictoffset) {
        PyObject** dictptr = _PyObject_GetDictPtr(self);
        if (dictptr != NULL) {
            PyObject* dict = *dictptr;
            if (dict != NULL) {
                Py_DECREF(dict);
                *dictptr = NULL;
            }
        }
    }

    // Pyston addition: same for hcattrs
    if (type->attrs_offset && !base->attrs_offset) {
        self->getHCAttrsPtr()->clearForDealloc();
    }

    /* Extract the type again; tp_del may have changed it */
    type = Py_TYPE(self);

    /* Call the base tp_dealloc(); first retrack self if
     * basedealloc knows about gc.
     */
    if (PyType_IS_GC(base))
        _PyObject_GC_TRACK(self);
    assert(basedealloc);
    basedealloc(self);

    /* Can't reference self beyond this point */
    Py_DECREF(type);

endlabel:
    ++_PyTrash_delete_nesting;
    ++tstate->trash_delete_nesting;
    Py_TRASHCAN_SAFE_END(self);
    --_PyTrash_delete_nesting;
    --tstate->trash_delete_nesting;

    /* Explanation of the weirdness around the trashcan macros:

       Q. What do the trashcan macros do?

       A. Read the comment titled "Trashcan mechanism" in object.h.
          For one, this explains why there must be a call to GC-untrack
          before the trashcan begin macro.      Without understanding the
          trashcan code, the answers to the following questions don't make
          sense.

       Q. Why do we GC-untrack before the trashcan and then immediately
          GC-track again afterward?

       A. In the case that the base class is GC-aware, the base class
          probably GC-untracks the object.      If it does that using the
          UNTRACK macro, this will crash when the object is already
          untracked.  Because we don't know what the base class does, the
          only safe thing is to make sure the object is tracked when we
          call the base class dealloc.  But...  The trashcan begin macro
          requires that the object is *untracked* before it is called.  So
          the dance becomes:

         GC untrack
         trashcan begin
         GC track

       Q. Why did the last question say "immediately GC-track again"?
          It's nowhere near immediately.

       A. Because the code *used* to re-track immediately.      Bad Idea.
          self has a refcount of 0, and if gc ever gets its hands on it
          (which can happen if any weakref callback gets invoked), it
          looks like trash to gc too, and gc also tries to delete self
          then.  But we're already deleting self.  Double deallocation is
          a subtle disaster.

       Q. Why the bizarre (net-zero) manipulation of
          _PyTrash_delete_nesting around the trashcan macros?

       A. Some base classes (e.g. list) also use the trashcan mechanism.
          The following scenario used to be possible:

          - suppose the trashcan level is one below the trashcan limit

          - subtype_dealloc() is called

          - the trashcan limit is not yet reached, so the trashcan level
        is incremented and the code between trashcan begin and end is
        executed

          - this destroys much of the object's contents, including its
        slots and __dict__

          - basedealloc() is called; this is really list_dealloc(), or
        some other type which also uses the trashcan macros

          - the trashcan limit is now reached, so the object is put on the
        trashcan's to-be-deleted-later list

          - basedealloc() returns

          - subtype_dealloc() decrefs the object's type

          - subtype_dealloc() returns

          - later, the trashcan code starts deleting the objects from its
        to-be-deleted-later list

          - subtype_dealloc() is called *AGAIN* for the same object

          - at the very least (if the destroyed slots and __dict__ don't
        cause problems) the object's type gets decref'ed a second
        time, which is *BAD*!!!

          The remedy is to make sure that if the code between trashcan
          begin and end in subtype_dealloc() is called, the code between
          trashcan begin and end in basedealloc() will also be called.
          This is done by decrementing the level after passing into the
          trashcan block, and incrementing it just before leaving the
          block.

          But now it's possible that a chain of objects consisting solely
          of objects whose deallocator is subtype_dealloc() will defeat
          the trashcan mechanism completely: the decremented level means
          that the effective level never reaches the limit.      Therefore, we
          *increment* the level *before* entering the trashcan block, and
          matchingly decrement it after leaving.  This means the trashcan
          code will trigger a little early, but that's no big deal.

       Q. Are there any live examples of code in need of all this
          complexity?

       A. Yes.  See SF bug 668433 for code that crashed (when Python was
          compiled in debug mode) before the trashcan level manipulations
          were added.  For more discussion, see SF patches 581742, 575073
          and bug 574207.
    */
}

void BoxedClass::freeze() {
    assert(!is_constant);
    assert(tp_name); // otherwise debugging will be very hard

    fixup_slot_dispatchers(this);

    if (instancesHaveDictAttrs() || instancesHaveHCAttrs()) {
        auto dict_str = getStaticString("__dict__");
        ASSERT(this == closure_cls || this == classobj_cls || this == instance_cls || typeLookup(this, dict_str), "%s",
               tp_name);
    }

    is_constant = true;
}

std::vector<BoxedClass*> classes;
BoxedClass::BoxedClass(BoxedClass* base, int attrs_offset, int weaklist_offset, int instance_size, bool is_user_defined,
                       const char* name, bool is_subclassable, destructor dealloc, freefunc free, bool is_gc,
                       traverseproc traverse, inquiry clear)
    : attrs(HiddenClass::makeSingleton()),
      attrs_offset(attrs_offset),
      is_constant(false),
      is_user_defined(is_user_defined),
      is_pyston_class(true),
      has___class__(false),
      has_instancecheck(false),
      tpp_call(NULL, NULL) {

    bool ok_noclear = (clear == NOCLEAR);
    if (ok_noclear)
        clear = NULL;
    if (clear)
        assert(traverse);
    if (traverse)
        assert(dealloc);
    if (dealloc)
        assert(traverse || !is_gc);
    ASSERT(((bool)traverse == (bool)clear) || ok_noclear, "%s", name);

    classes.push_back(this);

    // Zero out the CPython tp_* slots:
    memset(&tp_name, 0, (char*)(&tp_version_tag + 1) - (char*)(&tp_name));
    tp_basicsize = instance_size;
    tp_weaklistoffset = weaklist_offset;
    tp_name = name;

    tp_flags |= Py_TPFLAGS_DEFAULT_CORE;
    tp_flags |= Py_TPFLAGS_CHECKTYPES;
    if (is_subclassable)
        tp_flags |= Py_TPFLAGS_BASETYPE;
    if (is_gc)
        tp_flags |= Py_TPFLAGS_HAVE_GC;

    if (base && (base->tp_flags & Py_TPFLAGS_HAVE_NEWBUFFER))
        tp_flags |= Py_TPFLAGS_HAVE_NEWBUFFER;

    // From CPython: It's a new-style number unless it specifically inherits any
    // old-style numeric behavior.
    if (base) {
        if ((base->tp_flags & Py_TPFLAGS_CHECKTYPES) || (base->tp_as_number == NULL))
            tp_flags |= Py_TPFLAGS_CHECKTYPES;
    }

    Py_XINCREF(base);
    tp_base = base;

    if (tp_base) {
        assert(tp_base->tp_alloc);
        tp_alloc = tp_base->tp_alloc;
    } else {
        assert(this == object_cls);
        tp_alloc = PyType_GenericAlloc;
    }

    if (cls == NULL) {
        assert(type_cls == NULL);
    } else {
        // The (cls == type_cls) part of the check is important because during bootstrapping
        // we might not have set up enough stuff in order to do proper subclass checking,
        // but those clases will either have cls == NULL or cls == type_cls
        assert(cls == type_cls || PyType_Check(this));
    }

    tp_traverse = traverse;
    tp_clear = clear;
    if (base && !PyType_IS_GC(this) & PyType_IS_GC(base) && !traverse && !tp_clear) {
        assert(tp_flags & Py_TPFLAGS_HAVE_RICHCOMPARE);

        tp_flags |= Py_TPFLAGS_HAVE_GC;
        assert(tp_free != PyObject_Del);
        if (!tp_traverse)
            tp_traverse = base->tp_traverse;
        if (!tp_clear)
            tp_clear = base->tp_clear;
    }

    ASSERT((bool)tp_traverse == PyType_IS_GC(this), "%s missing traverse", tp_name);
    ASSERT(((bool)tp_clear == PyType_IS_GC(this)) || ok_noclear, "%s missing clear", tp_name);

    if (dealloc)
        tp_dealloc = dealloc;
    else {
        assert(base && base->tp_dealloc);
        tp_dealloc = base->tp_dealloc;
    }

    if (free)
        tp_free = free;
    else if (base) {
        // Copied from PyType_Ready
        if (PyType_IS_GC(this) == PyType_IS_GC(base))
            tp_free = base->tp_free;
        else if (PyType_IS_GC(this) && base->tp_free == PyObject_Del)
            this->tp_free = PyObject_GC_Del;
    }

    assert(tp_dealloc);
    assert(tp_free);

    if (!base) {
        assert(this == object_cls);
        // we're constructing 'object'
        // Will have to add __base__ = None later
    } else {
        assert(object_cls);
        if (base->attrs_offset)
            RELEASE_ASSERT(attrs_offset == base->attrs_offset, "");
        assert(tp_basicsize >= base->tp_basicsize);
    }

    if (base && cls && str_cls) {
        Py_INCREF(base);
        giveAttr("__base__", base);
    }

    if (attrs_offset) {
        assert(tp_basicsize >= attrs_offset + sizeof(HCAttrs));
        assert(attrs_offset % sizeof(void*) == 0); // Not critical I suppose, but probably signals a bug
    }
}

BoxedClass* BoxedClass::create(BoxedClass* metaclass, BoxedClass* base, int attrs_offset, int weaklist_offset,
                               int instance_size, bool is_user_defined, const char* name, bool is_subclassable,
                               destructor dealloc, freefunc free, bool is_gc, traverseproc traverse, inquiry clear) {
    assert(!is_user_defined);
    BoxedClass* made = new (metaclass, 0)
        BoxedClass(base, attrs_offset, weaklist_offset, instance_size, is_user_defined, name, is_subclassable, dealloc,
                   free, is_gc, traverse, clear);

    // While it might be ok if these were set, it'd indicate a difference in
    // expectations as to who was going to calculate them:
    assert(!made->tp_mro);
    assert(!made->tp_bases);
    made->tp_bases = NULL;

    made->finishInitialization();
    assert(made->tp_mro);

    return made;
}

void BoxedClass::finishInitialization() {
    assert(!this->tp_dict);
    this->tp_dict = incref(this->getAttrWrapper());

    commonClassSetup(this);
    tp_flags |= Py_TPFLAGS_READY;
}

static int traverse_slots(BoxedClass* type, PyObject* self, visitproc visit, void* arg) noexcept {
    Py_ssize_t i, n;
    PyMemberDef* mp;

    n = Py_SIZE(type);
    mp = PyHeapType_GET_MEMBERS((BoxedHeapClass*)type);
    for (i = 0; i < n; i++, mp++) {
        if (mp->type == T_OBJECT_EX) {
            char* addr = (char*)self + mp->offset;
            PyObject* obj = *(PyObject**)addr;
            if (obj != NULL) {
                int err = visit(obj, arg);
                if (err)
                    return err;
            }
        }
    }
    return 0;
}

static int subtype_traverse(PyObject* self, visitproc visit, void* arg) noexcept {
    PyTypeObject* type, *base;
    traverseproc basetraverse;

    /* Find the nearest base with a different tp_traverse,
       and traverse slots while we're at it */
    type = Py_TYPE(self);
    base = type;
    while ((basetraverse = base->tp_traverse) == subtype_traverse) {
        if (Py_SIZE(base)) {
            int err = traverse_slots(base, self, visit, arg);
            if (err)
                return err;
        }
        base = base->tp_base;
        assert(base);
    }

    if (type->tp_dictoffset != base->tp_dictoffset) {
        PyObject** dictptr = _PyObject_GetDictPtr(self);
        if (dictptr && *dictptr)
            Py_VISIT(*dictptr);
    }

    if (type->attrs_offset != base->attrs_offset) {
        Py_TRAVERSE(*self->getHCAttrsPtr());
    }

    if (type->tp_flags & Py_TPFLAGS_HEAPTYPE)
        /* For a heaptype, the instances count as references
           to the type.          Traverse the type so the collector
           can find cycles involving this link. */
        Py_VISIT(type);

    if (basetraverse)
        return basetraverse(self, visit, arg);
    return 0;
}

static int subtype_clear(PyObject* self) noexcept {
    PyTypeObject* type, *base;
    inquiry baseclear;

    /* Find the nearest base with a different tp_clear
       and clear slots while we're at it */
    type = Py_TYPE(self);
    base = type;
    while ((baseclear = base->tp_clear) == subtype_clear) {
        if (Py_SIZE(base))
            clear_slots(base, self);
        base = base->tp_base;
        assert(base);
    }

    /* Clear the instance dict (if any), to break cycles involving only
       __dict__ slots (as in the case 'self.__dict__ is self'). */
    if (type->tp_dictoffset != base->tp_dictoffset) {
        PyObject** dictptr = _PyObject_GetDictPtr(self);
        if (dictptr && *dictptr)
            Py_CLEAR(*dictptr);
    }

    if (type->attrs_offset != base->attrs_offset) {
        self->getHCAttrsPtr()->clearForDealloc();
    }

    if (baseclear)
        return baseclear(self);
    return 0;
}

BoxedHeapClass::BoxedHeapClass(BoxedClass* base, int attrs_offset, int weaklist_offset, int instance_size,
                               bool is_user_defined, BoxedString* name)
    : BoxedClass(base, attrs_offset, weaklist_offset, instance_size, is_user_defined, name->data(), true,
                 subtype_dealloc, PyObject_GC_Del, true, subtype_traverse, subtype_clear),
      ht_name(incref(name)),
      ht_slots(NULL) {
    assert(is_user_defined);

    /* Always override allocation strategy to use regular heap */
    this->tp_alloc = PyType_GenericAlloc;
    assert(this->tp_flags & Py_TPFLAGS_HAVE_GC); // Otherwise, could have avoided setting GC tp slots.

    tp_as_number = &as_number;
    tp_as_mapping = &as_mapping;
    tp_as_sequence = &as_sequence;
    tp_as_buffer = &as_buffer;
    tp_flags |= Py_TPFLAGS_HEAPTYPE;

    if (!ht_name)
        assert(str_cls == NULL);

    memset(&as_number, 0, sizeof(as_number));
    memset(&as_mapping, 0, sizeof(as_mapping));
    memset(&as_sequence, 0, sizeof(as_sequence));
    memset(&as_buffer, 0, sizeof(as_buffer));
}

BoxedHeapClass* BoxedHeapClass::create(BoxedClass* metaclass, BoxedClass* base, int attrs_offset, int weaklist_offset,
                                       int instance_size, bool is_user_defined, BoxedString* name, BoxedTuple* bases,
                                       size_t nslots) {
    BoxedHeapClass* made = new (metaclass, nslots)
        BoxedHeapClass(base, attrs_offset, weaklist_offset, instance_size, is_user_defined, name);

    assert((name || str_cls == NULL) && "name can only be NULL before str_cls has been initialized.");

    // While it might be ok if these were set, it'd indicate a difference in
    // expectations as to who was going to calculate them:
    assert(!made->tp_mro);
    assert(!made->tp_bases);
    made->tp_bases = incref(bases);

    try {
        made->finishInitialization();
    } catch (ExcInfo e) {
        // XXX hack -- see comment in createUserClass
        if (isSubclass(made->cls, type_cls)) {
            RELEASE_ASSERT(classes.back() == made, "");
            classes.pop_back();
        }

        Py_DECREF(made);
        throw e;
    }
    assert(made->tp_mro);

    return made;
}

std::string getFullNameOfClass(BoxedClass* cls) {
    static BoxedString* module_str = getStaticString("__module__");
    Box* b = cls->getattr(module_str);
    if (!b)
        return cls->tp_name;
    assert(b);
    if (b->cls != str_cls)
        return cls->tp_name;

    BoxedString* module = static_cast<BoxedString*>(b);

    return (llvm::Twine(module->s()) + "." + cls->tp_name).str();
}

std::string getFullTypeName(Box* o) {
    return getFullNameOfClass(o->cls);
}

const char* getTypeName(Box* b) {
    return b->cls->tp_name;
}

const char* getNameOfClass(BoxedClass* cls) {
    return cls->tp_name;
}

size_t Box::getHCAttrsOffset() {
    assert(cls->instancesHaveHCAttrs());

    if (unlikely(cls->attrs_offset < 0)) {
        // negative indicates an offset from the end of an object
        if (cls->tp_itemsize != 0) {
            size_t ob_size = static_cast<BoxVar*>(this)->ob_size;
            return cls->tp_basicsize + ob_size * cls->tp_itemsize + cls->attrs_offset;
        } else {
            // This case is unlikely: why would we use a negative attrs_offset
            // if it wasn't a var-sized object? But I guess it's technically allowed.
            return cls->attrs_offset;
        }
    } else {
        return cls->attrs_offset;
    }
}

HCAttrs* Box::getHCAttrsPtr() {
    char* p = reinterpret_cast<char*>(this);
    p += this->getHCAttrsOffset();
    return reinterpret_cast<HCAttrs*>(p);
}

BoxedDict** Box::getDictPtr() {
    assert(cls->instancesHaveDictAttrs());
    RELEASE_ASSERT(cls->tp_dictoffset > 0, "not implemented: handle < 0 case like in getHCAttrsPtr");

    char* p = reinterpret_cast<char*>(this);
    p += cls->tp_dictoffset;

    BoxedDict** d_ptr = reinterpret_cast<BoxedDict**>(p);
    return d_ptr;
}

void Box::setDict(STOLEN(BoxedDict*) d) {
    assert(0 && "check refcounting");
    assert(cls->instancesHaveDictAttrs());

    *getDictPtr() = d;
}

BORROWED(BoxedDict*) Box::getDict() {
    assert(cls->instancesHaveDictAttrs());

    BoxedDict** d_ptr = getDictPtr();
    BoxedDict* d = *d_ptr;
    if (!d) {
        d = *d_ptr = new BoxedDict();
    }

    assert(d->cls == dict_cls);
    return d;
}

static StatCounter box_getattr_slowpath("slowpath_box_getattr");

template <Rewritable rewritable> BORROWED(Box*) Box::getattr(BoxedString* attr, GetattrRewriteArgs* rewrite_args) {
    if (rewritable == NOT_REWRITABLE) {
        assert(!rewrite_args);
        rewrite_args = NULL;
    }

    assert(attr->interned_state != SSTATE_NOT_INTERNED);

    // We have to guard on the class in order to know the object's layout,
    // ie to know which kinds of attributes the object has and where they
    // live in the object's layout.
    // TODO we could try guarding on those fields directly rather than on
    // the class itself (which implies all of them).  That might require
    // creating a single field that encompasses the relevant other fields
    // so that it can still be a single guard rather than multiple.
    if (rewrite_args && !rewrite_args->obj_shape_guarded)
        rewrite_args->obj->addAttrGuard(offsetof(Box, cls), (intptr_t)cls);

#if 0
    if (attr.data()[0] == '_' && attr.data()[1] == '_') {
        // Only do this logging for potentially-avoidable cases:
        if (!rewrite_args && cls != classobj_cls) {
            if (attr == "__setattr__")
                printf("");

            std::string per_name_stat_name = "slowpath_box_getattr." + std::string(attr);
            Stats::log(Stats::getStatCounter(per_name_stat_name));
        }
    }
#endif
    box_getattr_slowpath.log();

    // Have to guard on the memory layout of this object.
    // Right now, guard on the specific Python-class, which in turn
    // specifies the C structure.
    // In the future, we could create another field (the flavor?)
    // that also specifies the structure and can include multiple
    // classes.
    // Only matters if we end up getting multiple classes with the same
    // structure (ex user class) and the same hidden classes, because
    // otherwise the guard will fail anyway.;
    if (cls->instancesHaveHCAttrs()) {
        HCAttrs* attrs = getHCAttrsPtr();
        HiddenClass* hcls = attrs->hcls;

        if (unlikely(hcls && hcls->type == HiddenClass::DICT_BACKED)) {
            if (rewrite_args)
                assert(!rewrite_args->isSuccessful());
            rewrite_args = NULL;
            Box* d = attrs->attr_list->attrs[0];
            assert(d);
            assert(attr->data()[attr->size()] == '\0');
            Box* r = PyDict_GetItem(d, attr);
            // r can be NULL if the item didn't exist
            return r;
        }

        assert(!hcls || hcls->type == HiddenClass::NORMAL || hcls->type == HiddenClass::SINGLETON);

        if (unlikely(rewrite_args)) {
            if (!rewrite_args->obj_hcls_guarded) {
                if (cls->attrs_offset < 0) {
                    REWRITE_ABORTED("");
                    rewrite_args = NULL;
                } else {
                    if (!(rewrite_args->obj->isConstant() && cls == type_cls
                          && static_cast<BoxedClass*>(this)->is_constant)) {
                        rewrite_args->obj->addAttrGuard(cls->attrs_offset + offsetof(HCAttrs, hcls), (intptr_t)hcls);
                    }
                    if (hcls && hcls->type == HiddenClass::SINGLETON)
                        hcls->addDependence(rewrite_args->rewriter);
                }
            }
        }

        if (!hcls) {
            if (rewrite_args)
                rewrite_args->setReturn(NULL, ReturnConvention::NO_RETURN);
            return NULL;
        }

        int offset = hcls->getOffset(attr);
        if (offset == -1) {
            if (rewrite_args)
                rewrite_args->setReturn(NULL, ReturnConvention::NO_RETURN);
            return NULL;
        }

        if (rewrite_args) {
            if (cls->attrs_offset < 0) {
                REWRITE_ABORTED("");
                rewrite_args = NULL;
            } else {
                RewriterVar* r_attrs
                    = rewrite_args->obj->getAttr(cls->attrs_offset + offsetof(HCAttrs, attr_list), Location::any());
                RewriterVar* r_rtn = r_attrs->getAttr(offset * sizeof(Box*) + offsetof(HCAttrs::AttrList, attrs),
                                                      Location::any())->setType(RefType::BORROWED);
                rewrite_args->setReturn(r_rtn, ReturnConvention::HAS_RETURN);
            }
        }

        Box* rtn = attrs->attr_list->attrs[offset];
        return rtn;
    }

    if (cls->instancesHaveDictAttrs()) {
        if (rewrite_args)
            REWRITE_ABORTED("");

        BoxedDict* d = getDict();

        auto it = d->d.find(attr);
        if (it == d->d.end()) {
            return NULL;
        }
        return it->second;
    }

    if (rewrite_args)
        rewrite_args->setReturn(NULL, ReturnConvention::NO_RETURN);

    return NULL;
}
template Box* Box::getattr<REWRITABLE>(BoxedString*, GetattrRewriteArgs*);
template Box* Box::getattr<NOT_REWRITABLE>(BoxedString*, GetattrRewriteArgs*);

// Parameters that control the growth of the attributes array.
// Currently, starts at 4 elements and then doubles every time.
// TODO: find a growth strategy that fits better with the allocator.  We add the AttrList header, plus whatever malloc
// overhead, so the resulting size might not end up fitting that efficiently.
#define INITIAL_ARRAY_SIZE 4

// Freelist for attribute arrays.  Parameters have not been tuned.
#define ARRAYLIST_FREELIST_SIZE 100
#define ARRAYLIST_NUM_FREELISTS 4
#define MAX_FREELIST_SIZE (INITIAL_ARRAY_SIZE * (1 << (ARRAYLIST_NUM_FREELISTS - 1)))
struct Freelist {
    int size;
    HCAttrs::AttrList* next_free;
};

Freelist attrlist_freelist[ARRAYLIST_NUM_FREELISTS];
int freelist_index[]
    = { 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3 };
static_assert(sizeof(freelist_index) / sizeof(freelist_index[0]) == MAX_FREELIST_SIZE + 1, "");

static bool isPowerOfTwo(int n) {
    return __builtin_popcountll(n) == 1;
}
static bool arrayIsAtCapacity(int n) {
    return n >= INITIAL_ARRAY_SIZE && isPowerOfTwo(n);
}

static int nextAttributeArraySize(int n) {
    assert(arrayIsAtCapacity(n));
    return n * 2;
}

static int freelistIndex(int n) {
    assert(n <= sizeof(freelist_index) / sizeof(freelist_index[0]));
    return freelist_index[n];
}

static HCAttrs::AttrList* allocFromFreelist(int freelist_idx) {
    auto&& freelist = attrlist_freelist[freelist_idx];
    int size = freelist.size;
    if (size) {
        auto rtn = freelist.next_free;
        freelist.size = size - 1;
        freelist.next_free = *reinterpret_cast<HCAttrs::AttrList**>(rtn);

#ifndef NDEBUG
        int nattrs = (1 << freelist_idx) * INITIAL_ARRAY_SIZE;
        memset(rtn, 0xcb, sizeof(HCAttrs::AttrList) + nattrs * sizeof(Box*));
#endif
        return rtn;
    }

    int nattrs = (1 << freelist_idx) * INITIAL_ARRAY_SIZE;
    return (HCAttrs::AttrList*)PyObject_MALLOC(sizeof(HCAttrs::AttrList) + nattrs * sizeof(Box*));
}

static HCAttrs::AttrList* allocAttrs(int nattrs) {
    assert(arrayIsAtCapacity(nattrs));

    if (nattrs <= MAX_FREELIST_SIZE)
        return allocFromFreelist(freelistIndex(nattrs));

    return (HCAttrs::AttrList*)PyObject_MALLOC(sizeof(HCAttrs::AttrList) + nattrs * sizeof(Box*));
}

static void freeAttrs(HCAttrs::AttrList* attrs, int nattrs) {
    if (nattrs <= MAX_FREELIST_SIZE) {
        int idx = freelistIndex(nattrs);
        auto&& freelist = attrlist_freelist[idx];
        int size = freelist.size;

        // TODO: should drop an old item from the freelist, not a new one
        if (size == ARRAYLIST_FREELIST_SIZE) {
            PyObject_FREE(attrs);
            return;
        } else {
#ifndef NDEBUG
            memset(attrs, 0xdb, sizeof(HCAttrs::AttrList) + nattrs * sizeof(Box*));
#endif
            *reinterpret_cast<HCAttrs::AttrList**>(attrs) = freelist.next_free;
            freelist.next_free = attrs;
            freelist.size++;
            return;
        }
    }

    PyObject_FREE(attrs);
}

static HCAttrs::AttrList* reallocAttrs(HCAttrs::AttrList* attrs, int old_nattrs, int new_nattrs) {
    assert(arrayIsAtCapacity(old_nattrs));
    assert(new_nattrs > old_nattrs);

    HCAttrs::AttrList* rtn = allocAttrs(new_nattrs);
    memcpy(rtn, attrs, sizeof(HCAttrs::AttrList) + sizeof(Box*) * old_nattrs);
#ifndef NDEBUG
    memset(&rtn->attrs[old_nattrs], 0xcb, sizeof(Box*) * (new_nattrs - old_nattrs));
#endif
    freeAttrs(attrs, old_nattrs);

    return rtn;
}

void Box::setDictBacked(STOLEN(Box*) val) {
    // this checks for: v.__dict__ = v.__dict__
    if (val->cls == attrwrapper_cls && unwrapAttrWrapper(val) == this) {
        Py_DECREF(val);
        return;
    }

    assert(this->cls->instancesHaveHCAttrs());
    HCAttrs* hcattrs = this->getHCAttrsPtr();
    RELEASE_ASSERT(PyDict_Check(val) || val->cls == attrwrapper_cls, "");

    HiddenClass* hcls = hcattrs->hcls;
    if (!hcls)
        hcls = root_hcls;

    if (hcls->type == HiddenClass::DICT_BACKED) {
        auto old_dict = hcattrs->attr_list->attrs[0];
        hcattrs->attr_list->attrs[0] = val;
        Py_DECREF(old_dict);
        return;
    }

    // If there is an old attrwrapper it is not allowed to wrap the instance anymore instead it has to switch to a
    // private dictonary.
    // e.g.:
    //     a = v.__dict__
    //     v.__dict__ = {} # 'a' must switch now from wrapping 'v' to a the private dict.
    int offset = hcls->getAttrwrapperOffset();
    if (offset != -1) {
        Box* wrapper = hcattrs->attr_list->attrs[offset];
        RELEASE_ASSERT(wrapper->cls == attrwrapper_cls, "");
        convertAttrwrapperToPrivateDict(wrapper);
    }

    // assign the dict to the attribute list and switch to the dict backed strategy
    // Skips the attrlist freelist
    auto new_attr_list = (HCAttrs::AttrList*)PyObject_MALLOC(sizeof(HCAttrs::AttrList) + sizeof(Box*));
    new_attr_list->attrs[0] = val;

    auto old_attr_list = hcattrs->attr_list;
    int old_attr_list_size = hcls->attributeArraySize();

    hcattrs->hcls = HiddenClass::dict_backed;
    hcattrs->attr_list = new_attr_list;

    assert((bool)old_attr_list == (bool)old_attr_list_size);
    if (old_attr_list_size) {
        decrefArray(old_attr_list->attrs, old_attr_list_size);
        freeAttrs(old_attr_list, old_attr_list_size);
    }
}

void HCAttrs::_clearRaw() noexcept {
    HiddenClass* hcls = this->hcls;

    if (!hcls)
        return;

    auto old_attr_list = this->attr_list;
    auto old_attr_list_size = hcls->attributeArraySize();

    new ((void*)this) HCAttrs(NULL);

    if (old_attr_list) {
        decrefArray(old_attr_list->attrs, old_attr_list_size);

        // DICT_BACKED attrs don't use the freelist:
        if (hcls->type == HiddenClass::DICT_BACKED)
            PyObject_FREE(old_attr_list);
        else
            freeAttrs(old_attr_list, old_attr_list_size);
    }
}

void HCAttrs::clearForDealloc() noexcept {
    HiddenClass* hcls = this->hcls;

    if (!hcls)
        return;

    if (hcls->type == HiddenClass::NORMAL || hcls->type == HiddenClass::SINGLETON) {
        int offset = hcls->getAttrwrapperOffset();
        if (offset != -1) {
            Box* attrwrapper = this->attr_list->attrs[offset];
            if (attrwrapper->ob_refcnt != 1)
                convertAttrwrapperToPrivateDict(attrwrapper);
        }
    }

    _clearRaw();
}

void HCAttrs::moduleClear() noexcept {
    auto hcls = this->hcls;
    if (!hcls)
        return;

    RELEASE_ASSERT(hcls->type == HiddenClass::NORMAL || hcls->type == HiddenClass::SINGLETON, "");

    auto attr_list = this->attr_list;
    auto attr_list_size = hcls->attributeArraySize();

    for (auto&& p : hcls->getStrAttrOffsets()) {
        const char* s = p.first->c_str();
        if (s[0] == '_' && s[1] != '_') {
            int idx = p.second;
            Box* b = attr_list->attrs[idx];
            attr_list->attrs[idx] = incref(None);
            Py_DECREF(b);
        }
    }

    for (auto&& p : hcls->getStrAttrOffsets()) {
        const char* s = p.first->c_str();
        if (s[0] != '_' || strcmp(s, "__builtins__") != 0) {
            int idx = p.second;
            Box* b = attr_list->attrs[idx];
            attr_list->attrs[idx] = incref(None);
            Py_DECREF(b);
        }
    }
}

void Box::appendNewHCAttr(BORROWED(Box*) new_attr, SetattrRewriteArgs* rewrite_args) {
    assert(cls->instancesHaveHCAttrs());
    HCAttrs* attrs = getHCAttrsPtr();
    HiddenClass* hcls = attrs->hcls;

    if (hcls == NULL)
        hcls = root_hcls;
    assert(hcls->type == HiddenClass::NORMAL || hcls->type == HiddenClass::SINGLETON);

    int numattrs = hcls->attributeArraySize();

    RewriterVar* r_array = NULL;
    if (numattrs == 0 || arrayIsAtCapacity(numattrs)) {
        if (numattrs == 0) {
            attrs->attr_list = allocFromFreelist(0);
            if (rewrite_args) {
                RewriterVar* r_newsize = rewrite_args->rewriter->loadConst(0, Location::forArg(0));
                r_array = rewrite_args->rewriter->call(true, (void*)allocFromFreelist, r_newsize);
            }
        } else {
            int new_size = nextAttributeArraySize(numattrs);
            attrs->attr_list = (HCAttrs::AttrList*)reallocAttrs(attrs->attr_list, numattrs, new_size);
            if (rewrite_args) {
                if (cls->attrs_offset < 0) {
                    REWRITE_ABORTED("");
                    rewrite_args = NULL;
                } else {
                    RewriterVar* r_oldarray = rewrite_args->obj->getAttr(
                        cls->attrs_offset + offsetof(HCAttrs, attr_list), Location::forArg(0));
                    RewriterVar* r_oldsize = rewrite_args->rewriter->loadConst(numattrs, Location::forArg(1));
                    RewriterVar* r_newsize = rewrite_args->rewriter->loadConst(new_size, Location::forArg(2));
                    r_array = rewrite_args->rewriter->call(true, (void*)reallocAttrs, r_oldarray, r_oldsize, r_newsize);
                }
            }
        }
    }

    if (rewrite_args) {
        bool new_array = (bool)r_array;

        if (!new_array)
            r_array = rewrite_args->obj->getAttr(cls->attrs_offset + offsetof(HCAttrs, attr_list));

        r_array->setAttr(numattrs * sizeof(Box*) + offsetof(HCAttrs::AttrList, attrs), rewrite_args->attrval);
        rewrite_args->attrval->refConsumed();

        if (new_array)
            rewrite_args->obj->setAttr(cls->attrs_offset + offsetof(HCAttrs, attr_list), r_array);

        rewrite_args->out_success = true;
    }
    attrs->attr_list->attrs[numattrs] = incref(new_attr);
}

void Box::giveAttr(STOLEN(BoxedString*) attr, STOLEN(Box*) val) {
    assert(!this->hasattr(attr));
    // Would be nice to have a stealing version of setattr:
    this->setattr(attr, val, NULL);
    Py_DECREF(val);
    Py_DECREF(attr);
}

void Box::setattr(BoxedString* attr, BORROWED(Box*) val, SetattrRewriteArgs* rewrite_args) {
    assert(attr->interned_state != SSTATE_NOT_INTERNED);

    // Have to guard on the memory layout of this object.
    // Right now, guard on the specific Python-class, which in turn
    // specifies the C structure.
    // In the future, we could create another field (the flavor?)
    // that also specifies the structure and can include multiple
    // classes.
    // Only matters if we end up getting multiple classes with the same
    // structure (ex user class) and the same hidden classes, because
    // otherwise the guard will fail anyway.;
    if (rewrite_args)
        rewrite_args->obj->addAttrGuard(offsetof(Box, cls), (intptr_t)cls);

    RELEASE_ASSERT(attr->s() != none_str || this == builtins_module, "can't assign to None");

    if (cls->instancesHaveHCAttrs()) {
        HCAttrs* attrs = getHCAttrsPtr();
        HiddenClass* hcls = attrs->hcls;

        if (unlikely(hcls == NULL)) {
            // We could update PyObject_Init and PyObject_INIT to do this, but that has a small compatibility
            // issue (what if people don't call either of those) and I'm not sure that this check will be that
            // harmful.  But if it is we might want to try pushing this assignment to allocation time.
            hcls = root_hcls;
        }

        if (hcls->type == HiddenClass::DICT_BACKED) {
            if (rewrite_args)
                assert(!rewrite_args->out_success);
            rewrite_args = NULL;
            Box* d = attrs->attr_list->attrs[0];
            assert(d);
            assert(attr->data()[attr->size()] == '\0');
            PyDict_SetItem(d, attr, val);
            checkAndThrowCAPIException();
            return;
        }

        assert(hcls->type == HiddenClass::NORMAL || hcls->type == HiddenClass::SINGLETON);

        int offset = hcls->getOffset(attr);

        if (rewrite_args) {
            if (cls->attrs_offset < 0) {
                REWRITE_ABORTED("");
                rewrite_args = NULL;
            } else {
                rewrite_args->obj->addAttrGuard(cls->attrs_offset + offsetof(HCAttrs, hcls), (intptr_t)attrs->hcls);
                if (hcls->type == HiddenClass::SINGLETON)
                    hcls->addDependence(rewrite_args->rewriter);
            }
        }

        if (offset >= 0) {
            assert(offset < hcls->attributeArraySize());
            Box* prev = attrs->attr_list->attrs[offset];
            attrs->attr_list->attrs[offset] = val;
            Py_INCREF(val);
            Py_DECREF(prev);

            if (rewrite_args) {
                if (cls->attrs_offset < 0) {
                    REWRITE_ABORTED("");
                    rewrite_args = NULL;
                } else {
                    RewriterVar* r_hattrs
                        = rewrite_args->obj->getAttr(cls->attrs_offset + offsetof(HCAttrs, attr_list), Location::any());

                    // Don't need to do anything: just getting it and setting it to OWNED
                    // will tell the auto-refcount system to decref it.
                    r_hattrs->getAttr(offset * sizeof(Box*) + offsetof(HCAttrs::AttrList, attrs))
                        ->setType(RefType::OWNED);
                    r_hattrs->setAttr(offset * sizeof(Box*) + offsetof(HCAttrs::AttrList, attrs),
                                      rewrite_args->attrval);
                    rewrite_args->attrval->refConsumed();

                    rewrite_args->out_success = true;
                }
            }

            return;
        }

        assert(offset == -1);

        if (hcls->type == HiddenClass::NORMAL) {
            HiddenClass* new_hcls = hcls->getOrMakeChild(attr);
            // make sure we don't need to rearrange the attributes
            assert(new_hcls->getStrAttrOffsets().lookup(attr) == hcls->attributeArraySize());

            this->appendNewHCAttr(val, rewrite_args);
            attrs->hcls = new_hcls;

            if (rewrite_args) {
                if (!rewrite_args->out_success) {
                    rewrite_args = NULL;
                } else {
                    RewriterVar* r_hcls = rewrite_args->rewriter->loadConst((intptr_t)new_hcls);
                    rewrite_args->obj->setAttr(cls->attrs_offset + offsetof(HCAttrs, hcls), r_hcls);
                    rewrite_args->out_success = true;
                }
            }
        } else {
            assert(hcls->type == HiddenClass::SINGLETON);

            assert(!rewrite_args || !rewrite_args->out_success);
            rewrite_args = NULL;

            this->appendNewHCAttr(val, NULL);
            hcls->appendAttribute(attr);
        }

        return;
    }

    if (cls->instancesHaveDictAttrs()) {
        BoxedDict* d = getDict();
        int r = PyDict_SetItem(d, attr, val);
        if (r == -1)
            throwCAPIException();
        return;
    }

    // Unreachable
    abort();
}

extern "C" BORROWED(PyObject*) _PyType_Lookup(PyTypeObject* type, PyObject* name) noexcept {
    RELEASE_ASSERT(name->cls == str_cls, "");
    try {
        return typeLookup(type, static_cast<BoxedString*>(name));
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

#define MCACHE_MAX_ATTR_SIZE 100
#define MCACHE_SIZE_EXP 10
#define MCACHE_HASH(version, name_hash)                                                                                \
    (((unsigned int)(version) * (unsigned int)(name_hash)) >> (8 * sizeof(unsigned int) - MCACHE_SIZE_EXP))
#define MCACHE_HASH_METHOD(type, name) MCACHE_HASH((type)->tp_version_tag, ((BoxedString*)(name))->hash)
#define MCACHE_CACHEABLE_NAME(name) PyString_CheckExact(name) && PyString_GET_SIZE(name) <= MCACHE_MAX_ATTR_SIZE

struct method_cache_entry {
    // Pyston change:
    // unsigned int version;
    PY_UINT64_T version;
    PyObject* name;  /* reference to exactly a str or None */
    PyObject* value; /* borrowed */
};

static struct method_cache_entry method_cache[1 << MCACHE_SIZE_EXP];
static unsigned int next_version_tag = 0;
static bool is_wrap_around = false; // Pyston addition

extern "C" unsigned int PyType_ClearCache() noexcept {
    Py_ssize_t i;
    unsigned int cur_version_tag = next_version_tag - 1;

    for (i = 0; i < (1 << MCACHE_SIZE_EXP); i++) {
        method_cache[i].version = 0;
        Py_CLEAR(method_cache[i].name);
        method_cache[i].value = NULL;
    }
    next_version_tag = 0;
    /* mark all version tags as invalid */
    PyType_Modified(&PyBaseObject_Type);
    is_wrap_around = false;
    return cur_version_tag;
}

int assign_version_tag(PyTypeObject* type) noexcept {
    /* Ensure that the tp_version_tag is valid and set
       Py_TPFLAGS_VALID_VERSION_TAG.  To respect the invariant, this
       must first be done on all super classes.  Return 0 if this
       cannot be done, 1 if Py_TPFLAGS_VALID_VERSION_TAG.
    */
    Py_ssize_t i, n;
    PyObject* bases;

    if (PyType_HasFeature(type, Py_TPFLAGS_VALID_VERSION_TAG))
        return 1;
    if (!PyType_HasFeature(type, Py_TPFLAGS_HAVE_VERSION_TAG))
        return 0;
    if (!PyType_HasFeature(type, Py_TPFLAGS_READY))
        return 0;

    type->tp_version_tag = next_version_tag++;
    /* for stress-testing: next_version_tag &= 0xFF; */

    if (unlikely(type->tp_version_tag == 0)) {
        // Pyston change: check for a wrap around because they are not allowed to happen with our 64bit version tag
        if (is_wrap_around)
            abort();
        is_wrap_around = true;

        /* wrap-around or just starting Python - clear the whole
           cache by filling names with references to Py_None.
           Values are also set to NULL for added protection, as they
           are borrowed reference */
        for (i = 0; i < (1 << MCACHE_SIZE_EXP); i++) {
            method_cache[i].value = NULL;
            Py_XDECREF(method_cache[i].name);
            method_cache[i].name = Py_None;
            Py_INCREF(Py_None);
        }
        /* mark all version tags as invalid */
        PyType_Modified(&PyBaseObject_Type);
        return 1;
    }
    bases = type->tp_bases;
    n = PyTuple_GET_SIZE(bases);
    for (i = 0; i < n; i++) {
        PyObject* b = PyTuple_GET_ITEM(bases, i);
        assert(PyType_Check(b));
        if (!assign_version_tag((PyTypeObject*)b))
            return 0;
    }
    type->tp_flags |= Py_TPFLAGS_VALID_VERSION_TAG;
    return 1;
}

template <Rewritable rewritable>
BORROWED(Box*) typeLookup(BoxedClass* cls, BoxedString* attr, GetattrRewriteArgs* rewrite_args) {
    if (rewritable == NOT_REWRITABLE) {
        assert(!rewrite_args);
        rewrite_args = NULL;
    }

    Box* val = NULL;

    // CAPI types defined inside external extension normally don't have this flag set while all types inside pyston set
    // it.
    if (rewrite_args && !PyType_HasFeature(cls, Py_TPFLAGS_HAVE_VERSION_TAG)) {
        assert(!rewrite_args->isSuccessful());

        RewriterVar* obj_saved = rewrite_args->obj;

        auto _mro = cls->tp_mro;
        assert(_mro->cls == tuple_cls);
        BoxedTuple* mro = static_cast<BoxedTuple*>(_mro);

        // Guarding approach:
        // Guard on the value of the tp_mro slot, which should be a tuple and thus be
        // immutable.  Then we don't have to figure out the guards to emit that check
        // the individual mro entries.
        // We can probably move this guard to after we call getattr() on the given cls.
        //
        // TODO this can fail if we replace the mro with another mro that lives in the same
        // address.
        obj_saved->addAttrGuard(offsetof(BoxedClass, tp_mro), (intptr_t)mro);

        for (auto base : *mro) {
            if (rewrite_args) {
                if (base == cls) {
                    // Small optimization: don't have to load the class again since it was given to us in
                    // a register.
                    assert(rewrite_args->obj == obj_saved);
                } else {
                    rewrite_args->obj = rewrite_args->rewriter->loadConst((intptr_t)base, Location::any());
                    // We are passing a constant object, and objects are not allowed to change shape
                    // (at least the kind of "shape" that Box::getattr is referring to)
                    rewrite_args->obj_shape_guarded = true;
                }
            }
            val = base->getattr<rewritable>(attr, rewrite_args);

            if (rewrite_args && !rewrite_args->isSuccessful())
                rewrite_args = NULL;

            if (val)
                return val;

            if (rewrite_args) {
                rewrite_args->assertReturnConvention(ReturnConvention::NO_RETURN);
                rewrite_args->clearReturn();
            }
        }

        if (rewrite_args)
            rewrite_args->setReturn(NULL, ReturnConvention::NO_RETURN);
        return NULL;
    } else {
        assert(attr->interned_state != SSTATE_NOT_INTERNED);
        assert(cls->tp_mro);
        assert(cls->tp_mro->cls == tuple_cls);

        bool found_cached_entry = false;
        if (MCACHE_CACHEABLE_NAME(attr) && PyType_HasFeature(cls, Py_TPFLAGS_VALID_VERSION_TAG)) {
            if (attr->hash == -1)
                strHashUnboxed(attr);

            /* fast path */
            auto h = MCACHE_HASH_METHOD(cls, attr);
            if (method_cache[h].version == cls->tp_version_tag && method_cache[h].name == attr) {
                val = method_cache[h].value;
                found_cached_entry = true;
            }
        }

        if (!found_cached_entry) {
            for (auto b : *static_cast<BoxedTuple*>(cls->tp_mro)) {
                // object_cls will get checked very often, but it only
                // has attributes that start with an underscore.
                if (b == object_cls) {
                    if (attr->data()[0] != '_') {
                        assert(!b->getattr(attr));
                        continue;
                    }
                }

                val = b->getattr(attr);
                if (val)
                    break;
            }

            if (MCACHE_CACHEABLE_NAME(attr) && assign_version_tag(cls)) {
                auto h = MCACHE_HASH_METHOD(cls, attr);
                method_cache[h].version = cls->tp_version_tag;
                method_cache[h].value = val; /* borrowed */
                Py_INCREF(attr);
                Py_DECREF(method_cache[h].name);
                method_cache[h].name = attr;
            }
        }
        if (rewrite_args) {
            RewriterVar* obj_saved = rewrite_args->obj;
            static_assert(sizeof(BoxedClass::tp_flags) == 8, "addAttrGuard only supports 64bit values");
            static_assert(sizeof(BoxedClass::tp_version_tag) == 8, "addAttrGuard only supports 64bit values");
            obj_saved->addAttrGuard(offsetof(BoxedClass, tp_flags), (intptr_t)cls->tp_flags);
            obj_saved->addAttrGuard(offsetof(BoxedClass, tp_version_tag), (intptr_t)cls->tp_version_tag);
            if (!val)
                rewrite_args->setReturn(NULL, ReturnConvention::NO_RETURN);
            else {
                rewrite_args->setReturn(rewrite_args->rewriter->loadConst((int64_t)val)->setType(RefType::BORROWED),
                                        ReturnConvention::HAS_RETURN);
            }
        }
        return val;
    }
}
template Box* typeLookup<REWRITABLE>(BoxedClass*, BoxedString*, GetattrRewriteArgs*);
template Box* typeLookup<NOT_REWRITABLE>(BoxedClass*, BoxedString*, GetattrRewriteArgs*);

bool isNondataDescriptorInstanceSpecialCase(Box* descr) {
    return descr->cls == function_cls || descr->cls == instancemethod_cls || descr->cls == staticmethod_cls
           || descr->cls == classmethod_cls || descr->cls == wrapperdescr_cls;
}

template <Rewritable rewritable>
Box* nondataDescriptorInstanceSpecialCases(GetattrRewriteArgs* rewrite_args, Box* obj, Box* descr, RewriterVar* r_descr,
                                           bool for_call, Box** bind_obj_out, RewriterVar** r_bind_obj_out) {
    if (rewritable == NOT_REWRITABLE) {
        assert(!rewrite_args);
        rewrite_args = NULL;
    }

    // Special case: non-data descriptor: function, instancemethod or classmethod
    // Returns a bound instancemethod
    if (descr->cls == function_cls || descr->cls == instancemethod_cls || descr->cls == classmethod_cls
        || (descr->cls == method_cls
            && (static_cast<BoxedMethodDescriptor*>(descr)->method->ml_flags & (METH_CLASS | METH_STATIC)) == 0)) {
        Box* im_self = NULL, * im_func = NULL, * im_class = obj->cls;
        RewriterVar* r_im_self = NULL, * r_im_func = NULL, * r_im_class = NULL;

        if (rewrite_args) {
            r_im_class = rewrite_args->obj->getAttr(offsetof(Box, cls));
        }

        if (descr->cls == function_cls) {
            im_self = obj;
            im_func = descr;
            if (rewrite_args) {
                r_im_self = rewrite_args->obj;
                r_im_func = r_descr;
            }
        } else if (descr->cls == method_cls) {
            im_self = obj;
            im_func = descr;
            if (rewrite_args) {
                r_im_self = rewrite_args->obj;
                r_im_func = r_descr;
            }
        } else if (descr->cls == classmethod_cls) {
            static StatCounter slowpath("slowpath_classmethod_get");
            slowpath.log();

            BoxedClassmethod* cm = static_cast<BoxedClassmethod*>(descr);
            im_self = obj->cls;
            if (cm->cm_callable == NULL) {
                raiseExcHelper(RuntimeError, "uninitialized classmethod object");
            }
            im_func = cm->cm_callable;

            if (rewrite_args) {
                r_im_self = r_im_class;
                r_im_func = r_descr->getAttr(offsetof(BoxedClassmethod, cm_callable))->setType(RefType::BORROWED);
                r_im_func->addGuardNotEq(0);
            }
        } else if (descr->cls == instancemethod_cls) {
            static StatCounter slowpath("slowpath_instancemethod_get");
            slowpath.log();

            BoxedInstanceMethod* im = static_cast<BoxedInstanceMethod*>(descr);
            if (im->obj != NULL) {
                if (rewrite_args) {
                    r_descr->addAttrGuard(offsetof(BoxedInstanceMethod, obj), 0, /* negate */ true);
                }
                return incref(descr);
            } else {
                // TODO subclass check
                im_self = obj;
                im_func = im->func;
                if (rewrite_args) {
                    r_descr->addAttrGuard(offsetof(BoxedInstanceMethod, obj), 0, /* negate */ false);
                    r_im_self = rewrite_args->obj;
                    r_im_func = r_descr->getAttr(offsetof(BoxedInstanceMethod, func));
                }
            }
        } else {
            assert(false);
        }

        if (!for_call) {
            if (rewrite_args) {
                RewriterVar* r_rtn
                    = rewrite_args->rewriter->call(false, (void*)boxInstanceMethod, r_im_self, r_im_func, r_im_class);
                r_rtn->setType(RefType::OWNED);
                rewrite_args->setReturn(r_rtn, ReturnConvention::HAS_RETURN);
            }
            return boxInstanceMethod(im_self, im_func, im_class);
        } else {
            *bind_obj_out = incref(im_self);
            if (rewrite_args) {
                rewrite_args->setReturn(r_im_func, ReturnConvention::HAS_RETURN);
                *r_bind_obj_out = r_im_self;
            }
            return incref(im_func);
        }
    } else if (descr->cls == staticmethod_cls) {
        BoxedStaticmethod* sm = static_cast<BoxedStaticmethod*>(descr);
        if (sm->sm_callable == NULL) {
            raiseExcHelper(RuntimeError, "uninitialized staticmethod object");
        }

        if (rewrite_args) {
            RewriterVar* r_sm_callable
                = r_descr->getAttr(offsetof(BoxedStaticmethod, sm_callable))->setType(RefType::BORROWED);
            r_sm_callable->addGuardNotEq(0);
            rewrite_args->setReturn(r_sm_callable, ReturnConvention::HAS_RETURN);
        }

        return incref(sm->sm_callable);
    } else if (descr->cls == wrapperdescr_cls) {
        if (for_call) {
            if (rewrite_args) {
                rewrite_args->setReturn(r_descr, ReturnConvention::HAS_RETURN);
                *r_bind_obj_out = rewrite_args->obj;
            }
            *bind_obj_out = incref(obj);
            return incref(descr);
        } else {
            BoxedWrapperDescriptor* self = static_cast<BoxedWrapperDescriptor*>(descr);
            Box* inst = obj;
            Box* owner = obj->cls;
            Box* r = BoxedWrapperDescriptor::descr_get(self, inst, owner);

            if (rewrite_args) {
                // TODO: inline this?
                RewriterVar* r_rtn
                    = rewrite_args->rewriter->call(
                                                  /* has_side_effects= */ false,
                                                  (void*)&BoxedWrapperDescriptor::descr_get, r_descr, rewrite_args->obj,
                                                  r_descr->getAttr(offsetof(Box, cls), Location::forArg(2)))
                          ->setType(RefType::OWNED);

                rewrite_args->setReturn(r_rtn, ReturnConvention::CAPI_RETURN);
            }
            return r;
        }
    }

    return NULL;
}

// r_descr must represent a valid object.
template <Rewritable rewritable>
Box* descriptorClsSpecialCases(GetattrRewriteArgs* rewrite_args, BoxedClass* cls, Box* descr, RewriterVar* r_descr,
                               bool for_call, Box** bind_obj_out, RewriterVar** r_bind_obj_out) {
    if (rewritable == NOT_REWRITABLE) {
        assert(!rewrite_args);
        rewrite_args = NULL;
    }

    // Special case: functions
    if (descr->cls == function_cls || descr->cls == instancemethod_cls) {
        if (rewrite_args)
            r_descr->addAttrGuard(offsetof(Box, cls), (uint64_t)descr->cls);

        // TODO: we need to change this to support instancemethod_checking.py
        if (!for_call && descr->cls == function_cls) {
            if (rewrite_args) {
                // return an unbound instancemethod
                RewriterVar* r_cls = rewrite_args->obj;
                RewriterVar* r_rtn = rewrite_args->rewriter->call(true, (void*)boxUnboundInstanceMethod, r_descr, r_cls)
                                         ->setType(RefType::OWNED);
                rewrite_args->setReturn(r_rtn, ReturnConvention::HAS_RETURN);
            }
            return boxUnboundInstanceMethod(descr, cls);
        }

        if (rewrite_args) {
            // This is assuming that r_descr was passed in as a valid object
            rewrite_args->setReturn(r_descr, ReturnConvention::HAS_RETURN);
        }
        return incref(descr);
    }

    // These classes are descriptors, but only have special behavior when involved
    // in instance lookups
    if (descr->cls == member_descriptor_cls || descr->cls == wrapperdescr_cls) {
        if (rewrite_args)
            r_descr->addAttrGuard(offsetof(Box, cls), (uint64_t)descr->cls);

        if (rewrite_args) {
            // This is assuming that r_descr was passed in as a valid object
            rewrite_args->setReturn(r_descr, ReturnConvention::HAS_RETURN);
        }
        return incref(descr);
    }

    return NULL;
}

Box* boxChar(char c) {
    char d[1];
    d[0] = c;
    return boxString(llvm::StringRef(d, 1));
}

// r_descr needs to represent a valid object
template <Rewritable rewritable>
Box* dataDescriptorInstanceSpecialCases(GetattrRewriteArgs* rewrite_args, BoxedString* attr_name, Box* obj, Box* descr,
                                        RewriterVar* r_descr, bool for_call, Box** bind_obj_out,
                                        RewriterVar** r_bind_obj_out) {
    if (rewritable == NOT_REWRITABLE) {
        assert(!rewrite_args);
        rewrite_args = NULL;
    }

    // Special case: data descriptor: member descriptor
    if (descr->cls == member_descriptor_cls) {
        static StatCounter slowpath("slowpath_member_descriptor_get");
        slowpath.log();

        BoxedMemberDescriptor* member_desc = static_cast<BoxedMemberDescriptor*>(descr);
        // TODO should also have logic to raise a type error if type of obj is wrong

        if (rewrite_args) {
            // TODO we could use offset as the index in the assembly lookup rather than hardcoding
            // the value in the assembly and guarding on it be the same.

            // This could be optimized if addAttrGuard supported things < 64 bits
            static_assert(sizeof(member_desc->offset) == 4, "assumed by assembly instruction below");
            r_descr->getAttr(offsetof(BoxedMemberDescriptor, offset), Location::any(), assembler::MovType::ZLQ)
                ->addGuard(member_desc->offset);

            static_assert(sizeof(member_desc->type) == 4, "assumed by assembly instruction below");
            r_descr->getAttr(offsetof(BoxedMemberDescriptor, type), Location::any(), assembler::MovType::ZLQ)
                ->addGuard(member_desc->type);
        }

        switch (member_desc->type) {
            case BoxedMemberDescriptor::OBJECT_EX: {
                if (rewrite_args) {
                    RewriterVar* r_rtn = rewrite_args->obj->getAttr(member_desc->offset, rewrite_args->destination)
                                             ->setType(RefType::BORROWED);
                    r_rtn->addGuardNotEq(0);
                    rewrite_args->setReturn(r_rtn, ReturnConvention::HAS_RETURN);
                }

                Box* rtn = *reinterpret_cast<Box**>((char*)obj + member_desc->offset);
                if (rtn == NULL) {
                    assert(attr_name->data()[attr_name->size()] == '\0');
                    raiseExcHelper(AttributeError, "%s", attr_name->data());
                }
                return incref(rtn);
            }
            case BoxedMemberDescriptor::OBJECT: {
                if (rewrite_args) {
                    RewriterVar* r_interm = rewrite_args->obj->getAttr(member_desc->offset, rewrite_args->destination);
                    // TODO would be faster to not use a call
                    RewriterVar* r_rtn
                        = rewrite_args->rewriter->call(false, (void*)noneIfNull, r_interm)->setType(RefType::BORROWED);
                    rewrite_args->setReturn(r_rtn, ReturnConvention::HAS_RETURN);
                }

                Box* rtn = *reinterpret_cast<Box**>((char*)obj + member_desc->offset);
                return incref(noneIfNull(rtn));
            }

            case BoxedMemberDescriptor::DOUBLE: {
                if (rewrite_args) {
                    RewriterVar* r_unboxed_val = rewrite_args->obj->getAttrDouble(member_desc->offset, assembler::XMM0);
                    RewriterVar::SmallVector normal_args;
                    RewriterVar::SmallVector float_args;
                    float_args.push_back(r_unboxed_val);
                    RewriterVar* r_rtn = rewrite_args->rewriter->call(false, (void*)boxFloat, normal_args, float_args)
                                             ->setType(RefType::OWNED);
                    rewrite_args->setReturn(r_rtn, ReturnConvention::HAS_RETURN);
                }

                double rtn = *reinterpret_cast<double*>((char*)obj + member_desc->offset);
                return boxFloat(rtn);
            }
            case BoxedMemberDescriptor::FLOAT: {
                if (rewrite_args) {
                    RewriterVar* r_unboxed_val = rewrite_args->obj->getAttrFloat(member_desc->offset, assembler::XMM0);
                    RewriterVar::SmallVector normal_args;
                    RewriterVar::SmallVector float_args;
                    float_args.push_back(r_unboxed_val);
                    RewriterVar* r_rtn = rewrite_args->rewriter->call(true, (void*)boxFloat, normal_args, float_args)
                                             ->setType(RefType::OWNED);
                    rewrite_args->setReturn(r_rtn, ReturnConvention::HAS_RETURN);
                }

                float rtn = *reinterpret_cast<float*>((char*)obj + member_desc->offset);
                return boxFloat((double)rtn);
            }

#define CASE_INTEGER_TYPE(TYPE, type, boxFn, cast)                                                                     \
    case BoxedMemberDescriptor::TYPE: {                                                                                \
        if (rewrite_args) {                                                                                            \
            RewriterVar* r_unboxed_val = rewrite_args->obj->getAttrCast<type, cast>(member_desc->offset);              \
            RewriterVar* r_rtn                                                                                         \
                = rewrite_args->rewriter->call(true, (void*)boxFn, r_unboxed_val)->setType(RefType::OWNED);            \
            /* XXX assuming that none of these throw a capi error! */                                                  \
            rewrite_args->setReturn(r_rtn, ReturnConvention::HAS_RETURN);                                              \
        }                                                                                                              \
        type rtn = *reinterpret_cast<type*>((char*)obj + member_desc->offset);                                         \
        return boxFn((cast)rtn);                                                                                       \
    }
                // Note that (a bit confusingly) boxInt takes int64_t, not an int
                CASE_INTEGER_TYPE(BOOL, bool, boxBool, bool)
                CASE_INTEGER_TYPE(BYTE, int8_t, boxInt, int64_t)
                CASE_INTEGER_TYPE(INT, int, boxInt, int64_t)
                CASE_INTEGER_TYPE(SHORT, short, boxInt, int64_t)
                CASE_INTEGER_TYPE(LONG, long, boxInt, int64_t)
                CASE_INTEGER_TYPE(CHAR, char, boxChar, char)
                CASE_INTEGER_TYPE(UBYTE, uint8_t, PyLong_FromUnsignedLong, unsigned long)
                CASE_INTEGER_TYPE(USHORT, unsigned short, PyLong_FromUnsignedLong, unsigned long)
                CASE_INTEGER_TYPE(UINT, unsigned int, PyLong_FromUnsignedLong, unsigned long)
                CASE_INTEGER_TYPE(ULONG, unsigned long, PyLong_FromUnsignedLong, unsigned long)
                CASE_INTEGER_TYPE(LONGLONG, long long, PyLong_FromLongLong, long long)
                CASE_INTEGER_TYPE(ULONGLONG, unsigned long long, PyLong_FromUnsignedLongLong, unsigned long long)
                CASE_INTEGER_TYPE(PYSSIZET, Py_ssize_t, boxInt, Py_ssize_t)
            case BoxedMemberDescriptor::STRING: {
                if (rewrite_args) {
                    RewriterVar* r_interm = rewrite_args->obj->getAttr(member_desc->offset, rewrite_args->destination);
                    RewriterVar* r_rtn
                        = rewrite_args->rewriter->call(true, (void*)boxStringOrNone, r_interm)->setType(RefType::OWNED);
                    rewrite_args->setReturn(r_rtn, ReturnConvention::HAS_RETURN);
                }

                char* rtn = *reinterpret_cast<char**>((char*)obj + member_desc->offset);
                return boxStringOrNone(rtn);
            }
            case BoxedMemberDescriptor::STRING_INPLACE: {
                if (rewrite_args) {
                    RewriterVar* r_rtn
                        = rewrite_args->rewriter->call(true, (void*)boxStringFromCharPtr,
                                                       rewrite_args->rewriter->add(
                                                           rewrite_args->obj, member_desc->offset,
                                                           rewrite_args->destination))->setType(RefType::OWNED);
                    rewrite_args->setReturn(r_rtn, ReturnConvention::HAS_RETURN);
                }

                rewrite_args = NULL;
                REWRITE_ABORTED("");
                char* rtn = reinterpret_cast<char*>((char*)obj + member_desc->offset);
                return boxStringFromCharPtr(rtn);
            }

            default:
                RELEASE_ASSERT(0, "%d", member_desc->type);
        }
    }

    else if (descr->cls == property_cls) {
        BoxedProperty* prop = static_cast<BoxedProperty*>(descr);
        if (prop->prop_get == NULL || prop->prop_get == None) {
            raiseExcHelper(AttributeError, "unreadable attribute");
        }

        if (rewrite_args) {
            r_descr->addAttrGuard(offsetof(BoxedProperty, prop_get), (intptr_t)prop->prop_get);

            RewriterVar* r_prop_get = r_descr->getAttr(offsetof(BoxedProperty, prop_get));
            CallRewriteArgs crewrite_args(rewrite_args->rewriter, r_prop_get, rewrite_args->destination);
            crewrite_args.arg1 = rewrite_args->obj;

            Box* rtn = runtimeCallInternal1<CXX, rewritable>(prop->prop_get, &crewrite_args, ArgPassSpec(1), obj);
            if (!crewrite_args.out_success) {
                rewrite_args = NULL;
            } else {
                rewrite_args->setReturn(crewrite_args.out_rtn, ReturnConvention::MAYBE_EXC);
            }
            return rtn;
        }

        return runtimeCallInternal1<CXX, NOT_REWRITABLE>(prop->prop_get, NULL, ArgPassSpec(1), obj);
    }

    // Special case: data descriptor: getset descriptor
    else if (descr->cls == pyston_getset_cls || descr->cls == capi_getset_cls) {
        BoxedGetsetDescriptor* getset_descr = static_cast<BoxedGetsetDescriptor*>(descr);

        // TODO some more checks should go here
        // getset descriptors (and some other types of builtin descriptors I think) should have
        // a field which gives the type that the descriptor should apply to. We need to check that obj
        // is of that type.

        if (getset_descr->get == NULL) {
            assert(attr_name->data()[attr_name->size()] == '\0');
            raiseExcHelper(AttributeError, "attribute '%s' of '%s' object is not readable", attr_name->data(),
                           getTypeName(getset_descr));
        }

        Box* rtn = getset_descr->get(obj, getset_descr->closure);

        if (rewrite_args) {
            // hmm, maybe we should write assembly which can look up the function address and call any function
            r_descr->addAttrGuard(offsetof(BoxedGetsetDescriptor, get), (intptr_t)getset_descr->get);

            RewriterVar* r_closure = r_descr->getAttr(offsetof(BoxedGetsetDescriptor, closure));
            RewriterVar* r_rtn = rewrite_args->rewriter->call(
                                                             /* has_side_effects */ true, (void*)getset_descr->get,
                                                             rewrite_args->obj, r_closure)->setType(RefType::OWNED);

            rewrite_args->setReturn(r_rtn, descr->cls == capi_getset_cls ? ReturnConvention::CAPI_RETURN
                                                                         : ReturnConvention::MAYBE_EXC);
        }
        return rtn;
    }

    return NULL;
}

// Helper function: make sure that a capi function either returned a non-error value, or
// it set an exception.  This is only needed in specialized situations; usually the "error
// return without exception set" state can just be passed up to the caller.
static void ensureValidCapiReturn(Box* r) {
    if (!r)
        ensureCAPIExceptionSet();
}

template <ExceptionStyle S, Rewritable rewritable>
Box* getattrInternalEx(Box* obj, BoxedString* attr, GetattrRewriteArgs* rewrite_args, bool cls_only, bool for_call,
                       Box** bind_obj_out, RewriterVar** r_bind_obj_out) noexcept(S == CAPI) {
    if (rewritable == NOT_REWRITABLE) {
        assert(!rewrite_args);
        rewrite_args = NULL;
    }

    if (!cls_only) {
        BoxedClass* cls = obj->cls;

        // We could also use the old invalidation-based approach here:
        if (rewrite_args)
            rewrite_args->obj->getAttr(offsetof(Box, cls))
                ->addAttrGuard(offsetof(BoxedClass, tp_getattro), (uint64_t)obj->cls->tp_getattro);

        if (obj->cls->tp_getattro && obj->cls->tp_getattro != PyObject_GenericGetAttr) {
            STAT_TIMER(t0, "us_timer_slowpath_tpgetattro", 10);

            if (obj->cls->tp_getattro == slot_tp_getattr_hook) {
                return slotTpGetattrHookInternal<S, rewritable>(obj, attr, rewrite_args, for_call, bind_obj_out,
                                                                r_bind_obj_out);
            } else if (obj->cls->tp_getattro == instance_getattro) {
                return instanceGetattroInternal<S>(obj, attr, rewrite_args);
            } else if (obj->cls->tp_getattro == type_getattro) {
                try {
                    Box* r = getattrInternalGeneric<true, rewritable>(obj, attr, rewrite_args, cls_only, for_call,
                                                                      bind_obj_out, r_bind_obj_out);
                    return r;
                } catch (ExcInfo e) {
                    if (S == CAPI) {
                        setCAPIException(e);
                        return NULL;
                    } else {
                        throw e;
                    }
                }
            }

            Box* r = obj->cls->tp_getattro(obj, attr);

            // If attr is immortal, then we are free to write an embedded reference to it.
            // Immortal are (unfortunately) common right now, so this is an easy way to get
            // around the fact that we don't currently scan ICs for GC references, but eventually
            // we should just add that.
            if (rewrite_args && attr->interned_state == SSTATE_INTERNED_IMMORTAL) {
                auto r_box = rewrite_args->rewriter->loadConst((intptr_t)attr);
                auto r_rtn = rewrite_args->rewriter->call(true, (void*)obj->cls->tp_getattro, rewrite_args->obj, r_box);
                r_rtn->setType(RefType::OWNED);

                rewrite_args->rewriter->call(false, (void*)ensureValidCapiReturn, r_rtn);
                rewrite_args->setReturn(r_rtn, ReturnConvention::CAPI_RETURN);
            }

            if (!r) {
                if (S == CAPI) {
                    ensureCAPIExceptionSet();
                    return r;
                } else
                    throwCAPIException();
            }

            return r;
        }

        // We could also use the old invalidation-based approach here:
        if (rewrite_args)
            rewrite_args->obj->getAttr(offsetof(Box, cls))
                ->addAttrGuard(offsetof(BoxedClass, tp_getattr), (uint64_t)obj->cls->tp_getattr);

        if (obj->cls->tp_getattr) {
            STAT_TIMER(t0, "us_timer_slowpath_tpgetattr", 10);

            assert(attr->data()[attr->size()] == '\0');

            rewrite_args = NULL;

            Box* r = obj->cls->tp_getattr(obj, const_cast<char*>(attr->data()));

            if (S == CAPI) {
                if (!r)
                    ensureCAPIExceptionSet();
                return r;
            } else {
                if (!r)
                    throwCAPIException();
                return r;
            }
        }
    }

    if (S == CAPI) {
        try {
            assert(!PyType_Check(obj) || cls_only); // There would be a tp_getattro
            return getattrInternalGeneric<false, rewritable>(obj, attr, rewrite_args, cls_only, for_call, bind_obj_out,
                                                             r_bind_obj_out);
        } catch (ExcInfo e) {
            setCAPIException(e);
            return NULL;
        }
    } else {
        if (unlikely(rewrite_args && rewrite_args->rewriter->aggressiveness() < 20)) {
            class Helper {
            public:
                static Box* call(Box* obj, BoxedString* attr, bool cls_only) {
                    assert(!PyType_Check(obj) || cls_only); // There would be a tp_getattro
                    return getattrInternalGeneric<false, NOT_REWRITABLE>(obj, attr, NULL, cls_only, false, NULL, NULL);
                }
            };

            RewriterVar* r_rtn
                = rewrite_args->rewriter->call(true, (void*)Helper::call, rewrite_args->obj,
                                               rewrite_args->rewriter->loadConst((intptr_t)attr, Location::forArg(1)),
                                               rewrite_args->rewriter->loadConst(cls_only, Location::forArg(2)))
                      ->setType(RefType::OWNED);
            rewrite_args->setReturn(r_rtn, ReturnConvention::NOEXC_POSSIBLE);
            return Helper::call(obj, attr, cls_only);
        }

        assert(!PyType_Check(obj) || cls_only); // There would be a tp_getattro
        return getattrInternalGeneric<false, rewritable>(obj, attr, rewrite_args, cls_only, for_call, bind_obj_out,
                                                         r_bind_obj_out);
    }
}

template <Rewritable rewritable>
inline Box* getclsattrInternal(Box* obj, BoxedString* attr, GetattrRewriteArgs* rewrite_args) {
    return getattrInternalEx<CXX, rewritable>(obj, attr, rewrite_args,
                                              /* cls_only */ true,
                                              /* for_call */ false, NULL, NULL);
}

extern "C" Box* getclsattr(Box* obj, BoxedString* attr) {
    STAT_TIMER(t0, "us_timer_slowpath_getclsattr", 10);

    static StatCounter slowpath_getclsattr("slowpath_getclsattr");
    slowpath_getclsattr.log();

    Box* gotten;

    if (attr->data()[0] == '_' && attr->data()[1] == '_' && PyInstance_Check(obj)) {
        // __enter__ and __exit__ need special treatment.
        static std::string enter_str("__enter__"), exit_str("__exit__");
        if (attr->s() == enter_str || attr->s() == exit_str)
            return getattr(obj, attr);
    }

#if 0
    std::unique_ptr<Rewriter> rewriter(Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 2, 1, "getclsattr"));

    if (rewriter.get()) {
        //rewriter->trap();
        GetattrRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0));
        gotten = getclsattrInternal(obj, attr, &rewrite_args, NULL);

        if (rewrite_args.out_success && gotten) {
            rewrite_args.out_rtn.move(-1);
            rewriter->commit();
        }
#else
    std::unique_ptr<Rewriter> rewriter(
        Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 2, "getclsattr"));

    if (rewriter.get()) {
        // rewriter->trap();
        GetattrRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0), rewriter->getReturnDestination());
        gotten = getclsattrInternal<REWRITABLE>(obj, attr, &rewrite_args);

        if (rewrite_args.isSuccessful()) {
            if (gotten) {
                RewriterVar* r_rtn;
                ReturnConvention return_convention;
                std::tie(r_rtn, return_convention) = rewrite_args.getReturn();

                assert(return_convention == ReturnConvention::HAS_RETURN
                       || return_convention == ReturnConvention::MAYBE_EXC);
                rewriter->commitReturning(r_rtn);
            } else {
                rewrite_args.getReturn(); // just to make the asserts happy
                rewriter.reset(NULL);
            }
        }
    } else
#endif
    { gotten = getclsattrInternal<NOT_REWRITABLE>(obj, attr, NULL); }

    if (!gotten)
        raiseExcHelper(AttributeError, "%s", attr->data());
    return gotten;
}


// Does a simple call of the descriptor's __get__ if it exists;
// this function is useful for custom getattribute implementations that already know whether the descriptor
// came from the class or not.
Box* processDescriptorOrNull(Box* obj, Box* inst, Box* owner) {
    if (DEBUG >= 2) {
        static BoxedString* get_str = getStaticString("__get__");
        assert((obj->cls->tp_descr_get == NULL) == (typeLookup(obj->cls, get_str) == NULL));
    }
    if (obj->cls->tp_descr_get) {
        Box* r = obj->cls->tp_descr_get(obj, inst, owner);
        if (!r)
            throwCAPIException();
        return r;
    }
    return NULL;
}

Box* processDescriptor(Box* obj, Box* inst, Box* owner) {
    Box* descr_r = processDescriptorOrNull(obj, inst, owner);
    if (descr_r)
        return descr_r;
    return incref(obj);
}


template <bool IsType, Rewritable rewritable>
Box* getattrInternalGeneric(Box* obj, BoxedString* attr, GetattrRewriteArgs* rewrite_args, bool cls_only, bool for_call,
                            Box** bind_obj_out, RewriterVar** r_bind_obj_out) {
    if (rewritable == NOT_REWRITABLE) {
        assert(!rewrite_args);
        rewrite_args = NULL;
    }

    if (for_call) {
        *bind_obj_out = NULL;
    }

    if (IsType) {
        if (!PyType_Check(obj))
            raiseExcHelper(TypeError, "descriptor '__getattribute__' requires a 'type' object but received a '%s'",
                           obj->cls->tp_name);
    }

    assert(obj->cls != closure_cls);

    static BoxedString* get_str = getStaticString("__get__");
    static BoxedString* set_str = getStaticString("__set__");

    // Handle descriptor logic here.
    // A descriptor is either a data descriptor or a non-data descriptor.
    // data descriptors define both __get__ and __set__. non-data descriptors
    // only define __get__. Rules are different for the two types, which means
    // that even though __get__ is the one we might call, we still have to check
    // if __set__ exists.
    // If __set__ exists, it's a data descriptor, and it takes precedence over
    // the instance attribute.
    // Otherwise, it's non-data, and we only call __get__ if the instance
    // attribute doesn't exist.

    // In the cls_only case, we ignore the instance attribute
    // (so we don't have to check if __set__ exists at all)

    // Look up the class attribute (called `descr` here because it might
    // be a descriptor).
    Box* descr = NULL;
    RewriterVar* r_descr = NULL;
    if (rewrite_args) {
        RewriterVar* r_obj_cls = rewrite_args->obj->getAttr(offsetof(Box, cls), Location::any());
        GetattrRewriteArgs grewrite_args(rewrite_args->rewriter, r_obj_cls, rewrite_args->destination);
        descr = typeLookup<rewritable>(obj->cls, attr, &grewrite_args);

        if (!grewrite_args.isSuccessful()) {
            rewrite_args = NULL;
        } else if (descr) {
            r_descr = grewrite_args.getReturn(ReturnConvention::HAS_RETURN);
        } else {
            grewrite_args.assertReturnConvention(ReturnConvention::NO_RETURN);
        }
    } else {
        descr = typeLookup(obj->cls, attr);
    }

    XKEEP_ALIVE(descr);

    // Check if it's a data descriptor
    descrgetfunc descr_get = NULL;
    // Note: _get_ will only be retrieved if we think it will be profitable to try calling that as opposed to
    // the descr_get function pointer.
    Box* _get_ = NULL;
    RewriterVar* r_get = NULL;
    if (descr) {
        descr_get = descr->cls->tp_descr_get;

        if (rewrite_args)
            r_descr->addAttrGuard(offsetof(Box, cls), (uint64_t)descr->cls);

        // Special-case data descriptors (e.g., member descriptors)
        Box* res = dataDescriptorInstanceSpecialCases<rewritable>(rewrite_args, attr, obj, descr, r_descr, for_call,
                                                                  bind_obj_out, r_bind_obj_out);
        if (res) {
            return res;
        }

        // Let's only check if __get__ exists if it's not a special case
        // nondata descriptor. The nondata case is handled below, but
        // we can immediately know to skip this part if it's one of the
        // special case nondata descriptors.
        if (!isNondataDescriptorInstanceSpecialCase(descr)) {
            if (rewrite_args) {
                RewriterVar* r_descr_cls = r_descr->getAttr(offsetof(Box, cls), Location::any());
                r_descr_cls->addAttrGuard(offsetof(BoxedClass, tp_descr_get), (intptr_t)descr_get);
            }

            // Check if __get__ exists
            if (descr_get) {
                if (rewrite_args) {
                    RewriterVar* r_descr_cls = r_descr->getAttr(offsetof(Box, cls), Location::any());
                    GetattrRewriteArgs grewrite_args(rewrite_args->rewriter, r_descr_cls, Location::any());
                    _get_ = typeLookup<rewritable>(descr->cls, get_str, &grewrite_args);
                    assert(_get_);
                    if (!grewrite_args.isSuccessful()) {
                        rewrite_args = NULL;
                    } else if (_get_) {
                        r_get = grewrite_args.getReturn(ReturnConvention::HAS_RETURN);
                    }
                } else {
                    // Don't look up __get__ if we can't rewrite under the assumption that it will
                    // usually be faster to just call tp_descr_get:
                    //_get_ = typeLookup(descr->cls, get_str);
                }
            } else {
                if (DEBUG >= 2)
                    assert(typeLookup<rewritable>(descr->cls, get_str, NULL) == NULL);
            }

            // As an optimization, don't check for __set__ if we're in cls_only mode, since it won't matter.
            if (descr_get && !cls_only) {
                // Check if __set__ exists
                Box* _set_ = NULL;
                if (rewrite_args) {
                    RewriterVar* r_descr_cls = r_descr->getAttr(offsetof(Box, cls), Location::any());
                    GetattrRewriteArgs grewrite_args(rewrite_args->rewriter, r_descr_cls, Location::any());
                    _set_ = typeLookup<REWRITABLE>(descr->cls, set_str, &grewrite_args);
                    if (!grewrite_args.isSuccessful()) {
                        rewrite_args = NULL;
                    } else {
                        grewrite_args.assertReturnConvention(_set_ ? ReturnConvention::HAS_RETURN
                                                                   : ReturnConvention::NO_RETURN);
                    }
                } else {
                    _set_ = typeLookup<rewritable>(descr->cls, set_str, NULL);
                }

                // Call __get__(descr, obj, obj->cls)
                if (_set_) {
                    Box* res;
                    if (rewrite_args) {
                        CallRewriteArgs crewrite_args(rewrite_args->rewriter, r_get, rewrite_args->destination);
                        crewrite_args.arg1 = r_descr;
                        crewrite_args.arg2 = rewrite_args->obj;
                        crewrite_args.arg3 = rewrite_args->obj->getAttr(offsetof(Box, cls), Location::any());
                        res = runtimeCallInternal<CXX, REWRITABLE>(_get_, &crewrite_args, ArgPassSpec(3), descr, obj,
                                                                   obj->cls, NULL, NULL);
                        if (!crewrite_args.out_success) {
                            rewrite_args = NULL;
                        } else {
                            rewrite_args->setReturn(crewrite_args.out_rtn, ReturnConvention::HAS_RETURN);
                        }
                    } else {
                        res = descr_get(descr, obj, obj->cls);
                        if (!res)
                            throwCAPIException();
                    }
                    return res;
                }
            }
        }
    }

    XKEEP_ALIVE(_get_); // Maybe not necessary?

    if (!cls_only) {
        if (!IsType) {
            // Look up the val in the object's dictionary and if you find it, return it.

            if (unlikely(rewrite_args && !descr && obj->cls != instancemethod_cls
                         && rewrite_args->rewriter->aggressiveness() < 40
                         && attr->interned_state == SSTATE_INTERNED_IMMORTAL)) {
                class Helper {
                public:
                    static Box* call(Box* obj, BoxedString* attr) { return xincref(obj->getattr(attr)); }
                };

                RewriterVar* r_rtn
                    = rewrite_args->rewriter->call(false, (void*)Helper::call, rewrite_args->obj,
                                                   rewrite_args->rewriter->loadConst(
                                                       (intptr_t)attr, Location::forArg(1)))->setType(RefType::OWNED);
                rewrite_args->setReturn(r_rtn, ReturnConvention::NOEXC_POSSIBLE);
                return Helper::call(obj, attr);
            }

            Box* val;
            if (rewrite_args) {
                GetattrRewriteArgs hrewrite_args(rewrite_args->rewriter, rewrite_args->obj, rewrite_args->destination);
                val = obj->getattr(attr, &hrewrite_args);

                if (!hrewrite_args.isSuccessful()) {
                    rewrite_args = NULL;
                } else if (val) {
                    rewrite_args->setReturn(hrewrite_args.getReturn());
                } else {
                    hrewrite_args.assertReturnConvention(ReturnConvention::NO_RETURN);
                }
            } else {
                val = obj->getattr(attr);
            }

            if (val) {
                Py_INCREF(val);
                return val;
            }
        } else {
            // More complicated when obj is a type
            // We have to look up the attr in the entire
            // class hierarchy, and we also have to check if it is a descriptor,
            // in addition to the data/nondata descriptor logic.
            // (in CPython, see type_getattro in typeobject.c)

            Box* val;
            RewriterVar* r_val = NULL;
            if (rewrite_args) {
                GetattrRewriteArgs grewrite_args(rewrite_args->rewriter, rewrite_args->obj, rewrite_args->destination);

                val = typeLookup<REWRITABLE>(static_cast<BoxedClass*>(obj), attr, &grewrite_args);
                if (!grewrite_args.isSuccessful()) {
                    rewrite_args = NULL;
                } else if (val) {
                    r_val = grewrite_args.getReturn(ReturnConvention::HAS_RETURN);
                } else {
                    grewrite_args.assertReturnConvention(ReturnConvention::NO_RETURN);
                }
            } else {
                val = typeLookup<rewritable>(static_cast<BoxedClass*>(obj), attr, NULL);
            }

            if (val) {
                Box* res = descriptorClsSpecialCases<rewritable>(rewrite_args, static_cast<BoxedClass*>(obj), val,
                                                                 r_val, for_call, bind_obj_out, r_bind_obj_out);
                if (res) {
                    return res;
                }

                // Lookup __get__
                descrgetfunc local_get = val->cls->tp_descr_get;
                if (rewrite_args) {
                    RewriterVar* r_cls = r_val->getAttr(offsetof(Box, cls));
                    r_cls->addAttrGuard(offsetof(BoxedClass, tp_descr_get), (intptr_t)local_get);
                }

                if (!local_get) {
                    if (rewrite_args)
                        rewrite_args->setReturn(r_val, ReturnConvention::HAS_RETURN);
                    Py_INCREF(val);
                    return val;
                }

                KEEP_ALIVE(val); // CPython doesn't have this but I think it's good:

                // Call __get__(val, None, obj)
                Box* r = local_get(val, NULL, obj);
                if (!r)
                    throwCAPIException();

                if (rewrite_args) {
                    RewriterVar* r_rtn
                        = rewrite_args->rewriter->call(true, (void*)local_get, r_val,
                                                       rewrite_args->rewriter->loadConst(0, Location::forArg(1)),
                                                       rewrite_args->obj)->setType(RefType::OWNED);
                    // rewrite_args->rewriter->checkAndThrowCAPIException(rewrite_args->out_rtn);
                    rewrite_args->setReturn(r_rtn, ReturnConvention::CAPI_RETURN);
                }

                return r;
            }
        }
    }

    // If descr and __get__ exist, then call __get__
    if (descr) {
        // Special cases first
        Box* res = nondataDescriptorInstanceSpecialCases<rewritable>(rewrite_args, obj, descr, r_descr, for_call,
                                                                     bind_obj_out, r_bind_obj_out);
        if (res) {
            return res;
        }

        // We looked up __get__ above. If we found it, call it and return
        // the result.
        if (descr_get) {
            // this could happen for the callattr path...
            if (for_call) {
#if STAT_CALLATTR_DESCR_ABORTS
                if (rewrite_args) {
                    std::string attr_name = "num_callattr_descr_abort";
                    Stats::log(Stats::getStatCounter(attr_name));
                    logByCurrentPythonLine(attr_name);
                }
#endif

                rewrite_args = NULL;
                REWRITE_ABORTED("");
            }

            Box* res;
            if (rewrite_args) {
                assert(_get_);
                CallRewriteArgs crewrite_args(rewrite_args->rewriter, r_get, rewrite_args->destination);
                crewrite_args.arg1 = r_descr;
                crewrite_args.arg2 = rewrite_args->obj;
                crewrite_args.arg3 = rewrite_args->obj->getAttr(offsetof(Box, cls), Location::any());
                res = runtimeCallInternal<CXX, rewritable>(_get_, &crewrite_args, ArgPassSpec(3), descr, obj, obj->cls,
                                                           NULL, NULL);
                if (!crewrite_args.out_success) {
                    rewrite_args = NULL;
                } else {
                    rewrite_args->setReturn(crewrite_args.out_rtn, ReturnConvention::HAS_RETURN);
                }
            } else {
                res = descr_get(descr, obj, obj->cls);
                if (!res)
                    throwCAPIException();
            }
            return res;
        }

        // Otherwise, just return descr.
        if (rewrite_args) {
            rewrite_args->setReturn(r_descr, ReturnConvention::HAS_RETURN);
        }
        Py_INCREF(descr);
        return descr;
    }

    // TODO this shouldn't go here; it should be in instancemethod_cls->tp_getattr[o]
    if (obj->cls == instancemethod_cls) {
        assert(!rewrite_args || !rewrite_args->isSuccessful());
        return getattrInternalEx<CXX, NOT_REWRITABLE>(static_cast<BoxedInstanceMethod*>(obj)->func, attr, NULL,
                                                      cls_only, for_call, bind_obj_out, NULL);
    }

    if (rewrite_args)
        rewrite_args->setReturn(NULL, ReturnConvention::NO_RETURN);
    return NULL;
}

template <ExceptionStyle S, Rewritable rewritable>
Box* getattrInternal(Box* obj, BoxedString* attr, GetattrRewriteArgs* rewrite_args) noexcept(S == CAPI) {
    return getattrInternalEx<S, rewritable>(obj, attr, rewrite_args,
                                            /* cls_only */ false,
                                            /* for_call */ false, NULL, NULL);
}

// Force instantiation of the template
template Box* getattrInternal<CAPI, REWRITABLE>(Box*, BoxedString*, GetattrRewriteArgs*);
template Box* getattrInternal<CXX, REWRITABLE>(Box*, BoxedString*, GetattrRewriteArgs*);
template Box* getattrInternal<CAPI, NOT_REWRITABLE>(Box*, BoxedString*, GetattrRewriteArgs*);
template Box* getattrInternal<CXX, NOT_REWRITABLE>(Box*, BoxedString*, GetattrRewriteArgs*);

Box* getattrMaybeNonstring(Box* obj, Box* attr) {
    if (!PyString_Check(attr)) {
        if (PyUnicode_Check(attr)) {
            attr = _PyUnicode_AsDefaultEncodedString(attr, NULL);
            if (attr == NULL)
                throwCAPIException();
        } else {
            raiseExcHelper(TypeError, "attribute name must be string, not '%.200s'", Py_TYPE(attr)->tp_name);
        }
    }

    BoxedString* s = static_cast<BoxedString*>(attr);
    incref(s);
    internStringMortalInplace(s);
    AUTO_DECREF(s);

    Box* r = getattrInternal<CXX>(obj, s);
    if (!r)
        raiseAttributeError(obj, s->s());
    return r;
}

template <ExceptionStyle S> Box* _getattrEntry(Box* obj, BoxedString* attr, void* return_addr) noexcept(S == CAPI) {
    STAT_TIMER(t0, "us_timer_slowpath_getattr", 10);

    static StatCounter slowpath_getattr("slowpath_getattr");
    slowpath_getattr.log();

    assert(PyString_Check(attr));

    if (VERBOSITY() >= 2) {
#if !DISABLE_STATS
        std::string per_name_stat_name = "getattr__" + std::string(attr->s());
        uint64_t* counter = Stats::getStatCounter(per_name_stat_name);
        Stats::log(counter);
#endif
    }

    std::unique_ptr<Rewriter> rewriter(Rewriter::createRewriter(return_addr, 2, "getattr"));

#if 0 && STAT_TIMERS
    static uint64_t* st_id = Stats::getStatCounter("us_timer_slowpath_getattr_patchable");
    static uint64_t* st_id_nopatch = Stats::getStatCounter("us_timer_slowpath_getattr_nopatch");
    static uint64_t* st_id_megamorphic = Stats::getStatCounter("us_timer_slowpath_getattr_megamorphic");
    ICInfo* icinfo = getICInfo(return_addr);
    uint64_t* counter;
    if (!icinfo)
        counter = st_id_nopatch;
    else if (icinfo->isMegamorphic())
        counter = st_id_megamorphic;
    else {
        //counter = Stats::getStatCounter("us_timer_slowpath_getattr_patchable_" + std::string(obj->cls->tp_name));
        //counter = Stats::getStatCounter("us_timer_slowpath_getattr_patchable_" + std::string(attr->s()));
        counter = st_id;
        if (!rewriter.get())
            printf("");
    }

    if (icinfo && icinfo->start_addr == (void*)0x2aaaadb1477b)
        printf("");
    ScopedStatTimer st(counter, 10);
#endif

    if (unlikely(rewriter.get() && rewriter->aggressiveness() < 5)) {
        RewriterVar* r_rtn = rewriter->call(true, (void*)_getattrEntry<S>, rewriter->getArg(0), rewriter->getArg(1),
                                            rewriter->loadConst(0, Location::forArg(2)))->setType(RefType::OWNED);
        rewriter->commitReturning(r_rtn);
        rewriter.reset(NULL);
    }

    // getattrInternal (what we call) can return NULL without setting an exception, but this function's
    // convention is that an exception will need to be thrown.
    // Here's a simple helper to help with that:
    class NoexcHelper {
    public:
        static void call(Box* rtn, Box* obj, BoxedString* attr) {
            if (S == CAPI) {
                if (!rtn && !PyErr_Occurred())
                    raiseAttributeErrorCapi(obj, attr->s());
            } else {
                if (!rtn)
                    raiseAttributeError(obj, attr->s());
            }
        }
    };

    Box* val;
    if (rewriter.get()) {
        rewriter->getArg(0)->setType(RefType::BORROWED);
        rewriter->getArg(1)->setType(RefType::BORROWED);

        Location dest;
        TypeRecorder* recorder = rewriter->getTypeRecorder();
        if (recorder)
            dest = Location::forArg(1);
        else
            dest = rewriter->getReturnDestination();
        GetattrRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0), dest);
        val = getattrInternal<S>(obj, attr, &rewrite_args);

        if (rewrite_args.isSuccessful()) {
            RewriterVar* rtn;
            ReturnConvention return_convention;
            std::tie(rtn, return_convention) = rewrite_args.getReturn();

            // Try to munge the return into the right form:
            if (return_convention != ReturnConvention::HAS_RETURN) {
                if (attr->interned_state == SSTATE_INTERNED_IMMORTAL) {
                    if (return_convention == ReturnConvention::NO_RETURN) {
                        assert(!rtn);
                        rtn = rewriter->loadConst(0, Location::forArg(1))
                                  ->setType(RefType::BORROWED)
                                  ->setNullable(true);
                    }
                    if (S == CXX && return_convention == ReturnConvention::CAPI_RETURN) {
                        rewriter->checkAndThrowCAPIException(rtn);
                        return_convention = ReturnConvention::HAS_RETURN;
                    } else {
                        rewriter->call(true, (void*)NoexcHelper::call, rtn, rewriter->getArg(0),
                                       rewriter->loadConst((intptr_t)attr, Location::forArg(2)));
                        return_convention = (S == CXX) ? ReturnConvention::HAS_RETURN : ReturnConvention::CAPI_RETURN;
                    }
                }
            }

            if (return_convention == ReturnConvention::HAS_RETURN
                || (S == CAPI && return_convention == ReturnConvention::CAPI_RETURN)) {
                if (recorder) {
                    rtn = rewriter->call(false, (void*)recordType,
                                         rewriter->loadConst((intptr_t)recorder, Location::forArg(0)), rtn);
                    recordType(recorder, val);
                }

                rewriter->commitReturning(rtn);
            }
        }
    } else {
        val = getattrInternal<S>(obj, attr);
    }

    NoexcHelper::call(val, obj, attr);
    return val;
}

extern "C" Box* getattr_capi(Box* obj, BoxedString* attr) noexcept {
    return _getattrEntry<CAPI>(obj, attr, __builtin_extract_return_addr(__builtin_return_address(0)));
}

extern "C" Box* getattr(Box* obj, BoxedString* attr) {
    return _getattrEntry<CXX>(obj, attr, __builtin_extract_return_addr(__builtin_return_address(0)));
}

bool dataDescriptorSetSpecialCases(Box* obj, STOLEN(Box*) val, Box* descr, SetattrRewriteArgs* rewrite_args,
                                   RewriterVar* r_descr, BoxedString* attr_name) {

    // Special case: getset descriptor
    if (descr->cls == pyston_getset_cls || descr->cls == capi_getset_cls) {
        BoxedGetsetDescriptor* getset_descr = static_cast<BoxedGetsetDescriptor*>(descr);

        // TODO type checking goes here
        if (getset_descr->set == NULL) {
            assert(attr_name->data()[attr_name->size()] == '\0');
            Py_DECREF(val);
            raiseExcHelper(AttributeError, "attribute '%s' of '%s' objects is not writable", attr_name->data(),
                           getTypeName(obj));
        }

        if (rewrite_args) {
            RewriterVar* r_obj = rewrite_args->obj;
            RewriterVar* r_val = rewrite_args->attrval;

            r_descr->addAttrGuard(offsetof(BoxedGetsetDescriptor, set), (intptr_t)getset_descr->set);
            RewriterVar* r_closure = r_descr->getAttr(offsetof(BoxedGetsetDescriptor, closure));
            RewriterVar::SmallVector args;
            args.push_back(r_obj);
            args.push_back(r_val);
            args.push_back(r_closure);
            RewriterVar* r_rtn = rewrite_args->rewriter->call(
                /* has_side_effects */ true, (void*)getset_descr->set, args);

            if (descr->cls == capi_getset_cls)
                rewrite_args->rewriter->checkAndThrowCAPIException(r_rtn, -1);

            rewrite_args->out_success = true;
        }

        AUTO_DECREF(val);
        if (descr->cls == pyston_getset_cls) {
            getset_descr->set_pyston(obj, val, getset_descr->closure);
        } else {
            int r = getset_descr->set_capi(obj, val, getset_descr->closure);
            if (r)
                throwCAPIException();
        }

        return true;
    } else if (descr->cls == member_descriptor_cls) {
        BoxedMemberDescriptor* member_desc = static_cast<BoxedMemberDescriptor*>(descr);
        PyMemberDef member_def;
        memset(&member_def, 0, sizeof(member_def));
        member_def.offset = member_desc->offset;
        member_def.type = member_desc->type;
        if (member_desc->readonly)
            member_def.flags |= READONLY;
        PyMember_SetOne((char*)obj, &member_def, val);
        Py_DECREF(val);
        checkAndThrowCAPIException();
        return true;
    }

    return false;
}

template <Rewritable rewritable>
void setattrGeneric(Box* obj, BoxedString* attr, STOLEN(Box*) val, SetattrRewriteArgs* rewrite_args) {
    if (rewritable == NOT_REWRITABLE) {
        assert(!rewrite_args);
        rewrite_args = NULL;
    }

    assert(val);

    static BoxedString* set_str = getStaticString("__set__");

    // TODO this should be in type_setattro
    if (obj->cls == type_cls) {
        BoxedClass* cobj = static_cast<BoxedClass*>(obj);
        if (!cobj->is_user_defined) {
            Py_DECREF(val);
            raiseExcHelper(TypeError, "can't set attributes of built-in/extension type '%s'", getNameOfClass(cobj));
        }
    }

    // Lookup a descriptor
    Box* descr = NULL;
    RewriterVar* r_descr = NULL;
    // TODO probably check that the cls is user-defined or something like that
    // (figure out exactly what)
    // (otherwise no need to check descriptor logic)
    if (rewrite_args) {
        RewriterVar* r_cls = rewrite_args->obj->getAttr(offsetof(Box, cls), Location::any());
        GetattrRewriteArgs grewrite_args(rewrite_args->rewriter, r_cls, rewrite_args->rewriter->getReturnDestination());
        descr = typeLookup(obj->cls, attr, &grewrite_args);

        if (!grewrite_args.isSuccessful()) {
            rewrite_args = NULL;
        } else if (descr) {
            r_descr = grewrite_args.getReturn(ReturnConvention::HAS_RETURN);
        } else {
            grewrite_args.assertReturnConvention(ReturnConvention::NO_RETURN);
        }
    } else {
        descr = typeLookup(obj->cls, attr);
    }

    XKEEP_ALIVE(descr);

    Box* _set_ = NULL;
    RewriterVar* r_set = NULL;
    if (descr) {
        bool special_case_worked = dataDescriptorSetSpecialCases(obj, val, descr, rewrite_args, r_descr, attr);
        if (special_case_worked) {
            // We don't need to to the invalidation stuff in this case.
            return;
        }

        if (rewrite_args) {
            RewriterVar* r_cls = r_descr->getAttr(offsetof(Box, cls), Location::any());
            GetattrRewriteArgs grewrite_args(rewrite_args->rewriter, r_cls, Location::any());
            _set_ = typeLookup(descr->cls, set_str, &grewrite_args);
            if (!grewrite_args.isSuccessful()) {
                rewrite_args = NULL;
            } else if (_set_) {
                r_set = grewrite_args.getReturn(ReturnConvention::HAS_RETURN);
            } else {
                grewrite_args.assertReturnConvention(ReturnConvention::NO_RETURN);
            }
        } else {
            _set_ = typeLookup(descr->cls, set_str);
        }
    }

    // If `descr` has __set__ (thus making it a descriptor) we should call
    // __set__ with `val` rather than directly calling setattr
    if (descr && _set_) {
        AUTO_DECREF(val);
        Box* set_rtn;

        // __set__ gets called differently from __get__: __get__ gets called roughly as
        // descr.__class__.__get__(descr, obj)
        // But __set__ gets called more like
        // descr.__set__(obj, val)
        // This is the same for functions, but for non-functions we have to explicitly run it
        // through the descriptor protocol.
        if (rewrite_args && _set_->cls == function_cls) {
            r_set->addAttrGuard(offsetof(Box, cls), (uint64_t)_set_->cls);

            CallRewriteArgs crewrite_args(rewrite_args->rewriter, r_set, Location::any());
            crewrite_args.arg1 = r_descr;
            crewrite_args.arg2 = rewrite_args->obj;
            crewrite_args.arg3 = rewrite_args->attrval;
            set_rtn = runtimeCallInternal<CXX, REWRITABLE>(_set_, &crewrite_args, ArgPassSpec(3), descr, obj, val, NULL,
                                                           NULL);
            if (crewrite_args.out_success) {
                rewrite_args->out_success = true;
            }
        } else {
            _set_ = processDescriptor(_set_, descr, descr->cls);
            AUTO_DECREF(_set_);
            set_rtn = runtimeCallInternal<CXX, NOT_REWRITABLE>(_set_, NULL, ArgPassSpec(2), obj, val, NULL, NULL, NULL);
        }
        Py_DECREF(set_rtn);

        // We don't need to to the invalidation stuff in this case.
        return;
    } else {
        if (!obj->cls->instancesHaveHCAttrs() && !obj->cls->instancesHaveDictAttrs()) {
            Py_DECREF(val);
            raiseAttributeError(obj, attr->s());
        }

        // TODO: make Box::setattr() stealing
        obj->setattr(attr, val, rewrite_args);
        Py_DECREF(val);
    }

    // TODO this should be in type_setattro
    if (PyType_Check(obj)) {
        BoxedClass* self = static_cast<BoxedClass*>(obj);

        static BoxedString* base_str = getStaticString("__base__");
        if (attr->s() == "__base__" && self->getattr(base_str))
            raiseExcHelper(TypeError, "readonly attribute");

        bool touched_slot = update_slot(self, attr->s());
        if (touched_slot) {
            rewrite_args = NULL;
            REWRITE_ABORTED("");
        }

        // update_slot() calls PyType_Modified() internally so we only have to explicitly call it inside the IC
        if (rewrite_args && PyType_HasFeature(self, Py_TPFLAGS_HAVE_VERSION_TAG))
            rewrite_args->rewriter->call(true, (void*)PyType_Modified, rewrite_args->obj);
    }
}

// force template instantiation:
template void setattrGeneric<NOT_REWRITABLE>(Box* obj, BoxedString* attr, Box* val, SetattrRewriteArgs* rewrite_args);
template void setattrGeneric<REWRITABLE>(Box* obj, BoxedString* attr, Box* val, SetattrRewriteArgs* rewrite_args);

extern "C" void setattr(Box* obj, BoxedString* attr, STOLEN(Box*) attr_val) {
    STAT_TIMER(t0, "us_timer_slowpath_setattr", 10);

    static StatCounter slowpath_setattr("slowpath_setattr");
    slowpath_setattr.log();

    if (obj->cls->tp_setattr) {
        STAT_TIMER(t1, "us_timer_slowpath_tpsetattr", 10);

        assert(attr->data()[attr->size()] == '\0');
        AUTO_DECREF(attr_val);
        int rtn = obj->cls->tp_setattr(obj, const_cast<char*>(attr->data()), attr_val);
        if (rtn)
            throwCAPIException();
        return;
    }

    std::unique_ptr<Rewriter> rewriter(
        Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 3, "setattr"));

    setattrofunc tp_setattro = obj->cls->tp_setattro;
    assert(tp_setattro);

    assert(!obj->cls->tp_setattr);

    if (rewriter.get()) {
        rewriter->getArg(0)->setType(RefType::BORROWED);
        rewriter->getArg(1)->setType(RefType::BORROWED);
        rewriter->getArg(2)->setType(RefType::OWNED);

        auto r_cls = rewriter->getArg(0)->getAttr(offsetof(Box, cls));
        // rewriter->trap();
        r_cls->addAttrGuard(offsetof(BoxedClass, tp_setattr), 0);
        r_cls->addAttrGuard(offsetof(BoxedClass, tp_setattro), (intptr_t)tp_setattro);
    }

    // Note: setattr will only be retrieved if we think it will be profitable to try calling that as opposed to
    // the tp_setattr function pointer.
    Box* setattr = NULL;
    RewriterVar* r_setattr;

    if (tp_setattro == instance_setattro) {
        if (rewriter.get()) {
            SetattrRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0), rewriter->getArg(2));
            instanceSetattroInternal(obj, attr, attr_val, &rewrite_args);
            if (rewrite_args.out_success)
                rewriter->commit();
        } else
            instanceSetattroInternal(obj, attr, attr_val, NULL);

        return;
    } else if (tp_setattro != PyObject_GenericSetAttr) {
        static BoxedString* setattr_str = getStaticString("__setattr__");
        if (rewriter.get()) {
            GetattrRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0)->getAttr(offsetof(Box, cls)),
                                            Location::any());
            setattr = typeLookup(obj->cls, setattr_str, &rewrite_args);
            assert(setattr);

            if (rewrite_args.isSuccessful()) {
                r_setattr = rewrite_args.getReturn(ReturnConvention::HAS_RETURN);
                // TODO this is not good enough, since the object could get collected:
                r_setattr->addGuard((intptr_t)setattr);
            } else {
                rewriter.reset(NULL);
            }
        } else {
            // setattr = typeLookup(obj->cls, setattr_str);
        }
    }

    // This is a borrowed reference so we don't need to register it
    static Box* object_setattr = object_cls->getattr(getStaticString("__setattr__"));

    // I guess this check makes it ok for us to just rely on having guarded on the value of setattr without
    // invalidating on deallocation, since we assume that object.__setattr__ will never get deallocated.
    if (tp_setattro == PyObject_GenericSetAttr) {
        if (rewriter.get()) {
            // rewriter->trap();
            SetattrRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0), rewriter->getArg(2));
            setattrGeneric<REWRITABLE>(obj, attr, attr_val, &rewrite_args);
            if (rewrite_args.out_success)
                rewriter->commit();
        } else {
            setattrGeneric<NOT_REWRITABLE>(obj, attr, attr_val, NULL);
        }
        return;
    }

    AUTO_DECREF(attr_val);

    if (rewriter.get()) {
        assert(setattr);

        // TODO actually rewrite this?
        setattr = processDescriptor(setattr, obj, obj->cls);
        AUTO_DECREF(setattr);
        autoDecref(
            runtimeCallInternal<CXX, REWRITABLE>(setattr, NULL, ArgPassSpec(2), attr, attr_val, NULL, NULL, NULL));
    } else {
        STAT_TIMER(t0, "us_timer_slowpath_tpsetattro", 10);
        int r = tp_setattro(obj, attr, attr_val);
        if (r)
            throwCAPIException();
    }
}

static bool nonzeroHelper(STOLEN(Box*) r) {
    AUTO_DECREF(r);

    // I believe this behavior is handled by the slot wrappers in CPython:
    if (r->cls == bool_cls) {
        BoxedBool* b = static_cast<BoxedBool*>(r);
        bool rtn = b->n;
        return rtn;
    } else if (r->cls == int_cls) {
        BoxedInt* b = static_cast<BoxedInt*>(r);
        bool rtn = b->n != 0;
        return rtn;
    } else {
        raiseExcHelper(TypeError, "__nonzero__ should return bool or int, returned %s", getTypeName(r));
    }
}

extern "C" bool nonzero(Box* obj) {
    STAT_TIMER(t0, "us_timer_slowpath_nonzero", 10);

    static StatCounter slowpath_nonzero("slowpath_nonzero");
    slowpath_nonzero.log();

    std::unique_ptr<Rewriter> rewriter(
        Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 1, "nonzero"));

    RewriterVar* r_obj = NULL;
    if (rewriter.get()) {
        r_obj = rewriter->getArg(0)->setType(RefType::BORROWED);
        r_obj->addAttrGuard(offsetof(Box, cls), (intptr_t)obj->cls);
    }

    // Note: it feels silly to have all these special cases here, and we should probably be
    // able to at least generate rewrites that are as good as the ones we write here.
    // But for now we can't and these should be a bit faster:
    if (obj->cls == bool_cls) {
        // TODO: is it faster to compare to True? (especially since it will be a constant we can embed in the rewrite)
        if (rewriter.get()) {
            RewriterVar* b = r_obj->getAttr(offsetof(BoxedBool, n), rewriter->getReturnDestination());
            rewriter->commitReturningNonPython(b);
        }

        BoxedBool* bool_obj = static_cast<BoxedBool*>(obj);
        return bool_obj->n;
    } else if (obj->cls == int_cls) {
        if (rewriter.get()) {
            RewriterVar* n = r_obj->getAttr(offsetof(BoxedInt, n), rewriter->getReturnDestination());
            RewriterVar* b = n->toBool(rewriter->getReturnDestination());
            rewriter->commitReturningNonPython(b);
        }

        BoxedInt* int_obj = static_cast<BoxedInt*>(obj);
        return int_obj->n != 0;
    } else if (obj->cls == float_cls) {
        if (rewriter.get()) {
            RewriterVar* b = rewriter->call(false, (void*)floatNonzeroUnboxed, r_obj);
            rewriter->commitReturningNonPython(b);
        }
        return static_cast<BoxedFloat*>(obj)->d != 0;
    } else if (obj->cls == none_cls) {
        if (rewriter.get()) {
            RewriterVar* b = rewriter->loadConst(0, rewriter->getReturnDestination());
            rewriter->commitReturningNonPython(b);
        }
        return false;
    } else if (obj->cls == long_cls) {
        BoxedLong* long_obj = static_cast<BoxedLong*>(obj);
        bool r = longNonzeroUnboxed(long_obj);

        if (rewriter.get()) {
            RewriterVar* r_rtn = rewriter->call(false, (void*)longNonzeroUnboxed, r_obj);
            rewriter->commitReturningNonPython(r_rtn);
        }
        return r;
    } else if (obj->cls == tuple_cls) {
        BoxedTuple* tuple_obj = static_cast<BoxedTuple*>(obj);
        bool r = (tuple_obj->ob_size != 0);

        if (rewriter.get()) {
            RewriterVar* r_rtn
                = r_obj->getAttr(offsetof(BoxedTuple, ob_size))->toBool(rewriter->getReturnDestination());
            rewriter->commitReturningNonPython(r_rtn);
        }
        return r;
    } else if (obj->cls == list_cls) {
        BoxedList* list_obj = static_cast<BoxedList*>(obj);
        bool r = (list_obj->size != 0);

        if (rewriter.get()) {
            RewriterVar* r_rtn = r_obj->getAttr(offsetof(BoxedList, size))->toBool(rewriter->getReturnDestination());
            rewriter->commitReturningNonPython(r_rtn);
        }
        return r;
    } else if (obj->cls == str_cls) {
        BoxedString* str_obj = static_cast<BoxedString*>(obj);
        bool r = (str_obj->ob_size != 0);

        if (rewriter.get()) {
            RewriterVar* r_rtn
                = r_obj->getAttr(offsetof(BoxedString, ob_size))->toBool(rewriter->getReturnDestination());
            rewriter->commitReturningNonPython(r_rtn);
        }
        return r;
    } else if (obj->cls == unicode_cls) {
        PyUnicodeObject* unicode_obj = reinterpret_cast<PyUnicodeObject*>(obj);
        bool r = (unicode_obj->length != 0);

        if (rewriter.get()) {
            RewriterVar* r_rtn
                = r_obj->getAttr(offsetof(PyUnicodeObject, length))->toBool(rewriter->getReturnDestination());
            rewriter->commitReturningNonPython(r_rtn);
        }
        return r;
    }

    static BoxedString* nonzero_str = getStaticString("__nonzero__");
    static BoxedString* len_str = getStaticString("__len__");

    // try __nonzero__
    CallattrRewriteArgs crewrite_args(rewriter.get(), r_obj,
                                      rewriter.get() ? rewriter->getReturnDestination() : Location());
    Box* rtn = callattrInternal0<CXX, REWRITABLE>(obj, nonzero_str, CLASS_ONLY, rewriter.get() ? &crewrite_args : NULL,
                                                  ArgPassSpec(0));
    if (!crewrite_args.isSuccessful())
        rewriter.reset();

    if (!rtn) {
        if (rewriter.get())
            crewrite_args.assertReturnConvention(ReturnConvention::NO_RETURN);

        // try __len__
        crewrite_args = CallattrRewriteArgs(rewriter.get(), r_obj,
                                            rewriter.get() ? rewriter->getReturnDestination() : Location());
        rtn = callattrInternal0<CXX, REWRITABLE>(obj, len_str, CLASS_ONLY, rewriter.get() ? &crewrite_args : NULL,
                                                 ArgPassSpec(0));
        if (!crewrite_args.isSuccessful())
            rewriter.reset();

        if (rtn == NULL) {
            if (rewriter.get()) {
                crewrite_args.assertReturnConvention(ReturnConvention::NO_RETURN);
            }
            ASSERT(obj->cls->is_user_defined || obj->cls->instances_are_nonzero || obj->cls == classobj_cls
                       || obj->cls == type_cls || isSubclass(obj->cls, Exception) || obj->cls == &PyFile_Type
                       || obj->cls == &PyTraceBack_Type || obj->cls == instancemethod_cls || obj->cls == module_cls
                       || obj->cls == capifunc_cls || obj->cls == builtin_function_or_method_cls
                       || obj->cls == method_cls || obj->cls == frame_cls || obj->cls == generator_cls
                       || obj->cls == capi_getset_cls || obj->cls == pyston_getset_cls || obj->cls == wrapperdescr_cls
                       || obj->cls == wrapperobject_cls || obj->cls == code_cls,
                   "%s.__nonzero__", getTypeName(obj)); // TODO

            if (rewriter.get()) {
                RewriterVar* b = rewriter->loadConst(1, rewriter->getReturnDestination());
                rewriter->commitReturningNonPython(b);
            }
            return true;
        }
    }

    if (crewrite_args.isSuccessful()) {
        RewriterVar* rtn = crewrite_args.getReturn(ReturnConvention::HAS_RETURN);
        RewriterVar* b = rewriter->call(false, (void*)nonzeroHelper, rtn);
        rtn->refConsumed();
        rewriter->commitReturningNonPython(b);
    }
    return nonzeroHelper(rtn);
}

extern "C" BoxedString* str(Box* obj) {
    STAT_TIMER(t0, "us_timer_str", 10);
    static StatCounter slowpath_str("slowpath_str");
    slowpath_str.log();

    Box* rtn = PyObject_Str(obj);
    if (!rtn)
        throwCAPIException();
    assert(rtn->cls == str_cls); // PyObject_Str always returns a str
    return (BoxedString*)rtn;
}

extern "C" BoxedString* repr(Box* obj) {
    STAT_TIMER(t0, "us_timer_repr", 10);
    static StatCounter slowpath_repr("slowpath_repr");
    slowpath_repr.log();

    Box* rtn = PyObject_Repr(obj);
    if (!rtn)
        throwCAPIException();
    assert(rtn->cls == str_cls); // PyObject_Repr always returns a str
    return (BoxedString*)rtn;
}

extern "C" bool exceptionMatches(Box* obj, Box* cls) {
    STAT_TIMER(t0, "us_timer_exceptionMatches", 10);
    int rtn = PyErr_GivenExceptionMatches(obj, cls);
    RELEASE_ASSERT(rtn >= 0, "");
    return rtn;
}

/* Macro to get the tp_richcompare field of a type if defined */
#define RICHCOMPARE(t) (PyType_HasFeature((t), Py_TPFLAGS_HAVE_RICHCOMPARE) ? (t)->tp_richcompare : NULL)

extern "C" long PyObject_Hash(PyObject* v) noexcept {
    PyTypeObject* tp = v->cls;
    if (tp->tp_hash != NULL)
        return (*tp->tp_hash)(v);
#if 0 // pyston change
    /* To keep to the general practice that inheriting
     * solely from object in C code should work without
     * an explicit call to PyType_Ready, we implicitly call
     * PyType_Ready here and then check the tp_hash slot again
     */
    if (tp->tp_dict == NULL) {
        if (PyType_Ready(tp) < 0)
            return -1;
        if (tp->tp_hash != NULL)
            return (*tp->tp_hash)(v);
    }
#endif
    if (tp->tp_compare == NULL && RICHCOMPARE(tp) == NULL) {
        return _Py_HashPointer(v); /* Use address as hash value */
    }
    /* If there's a cmp but no hash defined, the object can't be hashed */
    return PyObject_HashNotImplemented(v);
}

int64_t hashUnboxed(Box* obj) {
    auto r = PyObject_Hash(obj);
    if (r == -1)
        throwCAPIException();
    return r;
}

extern "C" BoxedInt* hash(Box* obj) {
    int64_t r = hashUnboxed(obj);
    return (BoxedInt*)boxInt(r);
}

template <ExceptionStyle S, Rewritable rewritable>
BoxedInt* lenInternal(Box* obj, LenRewriteArgs* rewrite_args) noexcept(S == CAPI) {
    if (rewritable == NOT_REWRITABLE) {
        assert(!rewrite_args);
        rewrite_args = NULL;
    }

    static BoxedString* len_str = getStaticString("__len__");

    if (S == CAPI) {
        assert(!rewrite_args && "implement me");
        rewrite_args = NULL;
    }

    // Corresponds to the first part of PyObject_Size:
    PySequenceMethods* m = obj->cls->tp_as_sequence;
    if (m != NULL && m->sq_length != NULL && m->sq_length != slot_sq_length) {
        if (rewrite_args) {
            assert(S == CXX);

            RewriterVar* r_obj = rewrite_args->obj;
            RewriterVar* r_cls = r_obj->getAttr(offsetof(Box, cls));
            RewriterVar* r_m = r_cls->getAttr(offsetof(BoxedClass, tp_as_sequence));
            r_m->addGuardNotEq(0);

            // Currently, guard that the value of sq_length didn't change, and then
            // emit a call to the current function address.
            // It might be better to just load the current value of sq_length and call it
            // (after guarding it's not null), or maybe not.  But the rewriter doesn't currently
            // support calling a RewriterVar (can only call fixed function addresses).
            r_m->addAttrGuard(offsetof(PySequenceMethods, sq_length), (intptr_t)m->sq_length);
            RewriterVar* r_n = rewrite_args->rewriter->call(true, (void*)m->sq_length, r_obj);

            // Some CPython code seems to think that any negative return value means an exception,
            // but the docs say -1. TODO it would be nice to just handle any negative value.
            rewrite_args->rewriter->checkAndThrowCAPIException(r_n, -1);

            RewriterVar* r_r = rewrite_args->rewriter->call(false, (void*)boxInt, r_n)->setType(RefType::OWNED);

            rewrite_args->out_success = true;
            rewrite_args->out_rtn = r_r;
        }

        int r = (*m->sq_length)(obj);
        if (r == -1) {
            if (S == CAPI)
                return NULL;
            else
                throwCAPIException();
        }
        return (BoxedInt*)boxInt(r);
    }

    class FixupLenReturn {
    public:
        static BoxedInt* call(STOLEN(Box*) rtn) {
            // TODO: support returning longs as the length
            if (rtn->cls != int_cls) {
                Py_DECREF(rtn);
                if (S == CAPI) {
                    PyErr_Format(TypeError, "an integer is required");
                    return NULL;
                } else
                    raiseExcHelper(TypeError, "an integer is required");
            }

            return static_cast<BoxedInt*>(rtn);
        };
    };

    Box* rtn;
    RewriterVar* r_rtn = NULL;
    try {
        if (rewrite_args) {
            CallattrRewriteArgs crewrite_args(rewrite_args->rewriter, rewrite_args->obj, rewrite_args->destination);
            rtn = callattrInternal0<CXX, REWRITABLE>(obj, len_str, CLASS_ONLY, &crewrite_args, ArgPassSpec(0));
            if (!crewrite_args.isSuccessful())
                rewrite_args = NULL;
            else {
                RewriterVar* rtn;
                ReturnConvention return_convention;
                std::tie(rtn, return_convention) = crewrite_args.getReturn();
                if (return_convention != ReturnConvention::HAS_RETURN
                    && return_convention != ReturnConvention::NO_RETURN)
                    rewrite_args = NULL;
                else {
                    r_rtn = rtn;
                }

                if (rewrite_args)
                    assert((bool)rtn == (return_convention == ReturnConvention::HAS_RETURN));
            }
        } else {
            rtn = callattrInternal0<CXX, NOT_REWRITABLE>(obj, len_str, CLASS_ONLY, NULL, ArgPassSpec(0));
        }
    } catch (ExcInfo e) {
        if (S == CAPI) {
            setCAPIException(e);
            return NULL;
        } else {
            throw e;
        }
    }

    if (rtn == NULL) {
        if (S == CAPI) {
            if (!PyErr_Occurred())
                PyErr_Format(TypeError, "object of type '%s' has no len()", getTypeName(obj));
            return NULL;
        } else
            raiseExcHelper(TypeError, "object of type '%s' has no len()", getTypeName(obj));
    }

    if (rewrite_args) {
        if (S == CXX) {
            rewrite_args->out_rtn
                = rewrite_args->rewriter->call(true, (void*)FixupLenReturn::call, r_rtn)->setType(RefType::OWNED);
            r_rtn->refConsumed();
            rewrite_args->out_success = true;
        } else {
            // Don't know how to propagate the exception
            rewrite_args = NULL;
        }
    }

    return FixupLenReturn::call(rtn);
}

// force template instantiation:
template BoxedInt* lenInternal<CAPI, REWRITABLE>(Box*, LenRewriteArgs*);
template BoxedInt* lenInternal<CXX, REWRITABLE>(Box*, LenRewriteArgs*);
template BoxedInt* lenInternal<CAPI, NOT_REWRITABLE>(Box*, LenRewriteArgs*);
template BoxedInt* lenInternal<CXX, NOT_REWRITABLE>(Box*, LenRewriteArgs*);

Box* lenCallInternal(BoxedFunctionBase* func, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1, Box* arg2,
                     Box* arg3, Box** args, const std::vector<BoxedString*>* keyword_names) {
    if (argspec != ArgPassSpec(1))
        return callFunc<CXX>(func, rewrite_args, argspec, arg1, arg2, arg3, args, keyword_names);

    if (rewrite_args) {
        LenRewriteArgs lrewrite_args(rewrite_args->rewriter, rewrite_args->arg1, rewrite_args->destination);
        Box* rtn = lenInternal<CXX, REWRITABLE>(arg1, &lrewrite_args);
        if (!lrewrite_args.out_success) {
            rewrite_args = 0;
        } else {
            rewrite_args->out_rtn = lrewrite_args.out_rtn;
            rewrite_args->out_success = true;
        }
        return rtn;
    }
    return lenInternal<CXX, NOT_REWRITABLE>(arg1, NULL);
}

extern "C" BoxedInt* len(Box* obj) {
    STAT_TIMER(t0, "us_timer_slowpath_len", 10);

    static StatCounter slowpath_len("slowpath_len");
    slowpath_len.log();

    return lenInternal<CXX, NOT_REWRITABLE>(obj, NULL);
}

extern "C" i64 unboxedLen(Box* obj) {
    STAT_TIMER(t0, "us_timer_slowpath_unboxedLen", 10);

    static StatCounter slowpath_unboxedlen("slowpath_unboxedlen");
    slowpath_unboxedlen.log();

    std::unique_ptr<Rewriter> rewriter(
        Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 1, "unboxedLen"));

    BoxedInt* lobj;
    RewriterVar* r_boxed = NULL;
    if (rewriter.get()) {
        // rewriter->trap();
        LenRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0), rewriter->getReturnDestination());
        lobj = lenInternal<CXX, REWRITABLE>(obj, &rewrite_args);

        if (!rewrite_args.out_success) {
            rewriter.reset(NULL);
        } else
            r_boxed = rewrite_args.out_rtn;
    } else {
        lobj = lenInternal<CXX, NOT_REWRITABLE>(obj, NULL);
    }

    assert(lobj->cls == int_cls);
    i64 rtn = lobj->n;
    Py_DECREF(lobj);

    if (rewriter.get()) {
        assert(0 && "how do we know this will return an int?");
        RewriterVar* rtn = r_boxed->getAttr(offsetof(BoxedInt, n), Location(assembler::RAX));
        rewriter->commitReturning(rtn);
    }
    return rtn;
}

// For rewriting purposes, this function assumes that nargs will be constant.
// That's probably fine for some uses (ex binops), but otherwise it should be guarded on beforehand.
template <ExceptionStyle S, Rewritable rewritable>
Box* callattrInternal(Box* obj, BoxedString* attr, LookupScope scope, CallattrRewriteArgs* rewrite_args,
                      ArgPassSpec argspec, Box* arg1, Box* arg2, Box* arg3, Box** args,
                      const std::vector<BoxedString*>* keyword_names) noexcept(S == CAPI) {
    if (rewritable == NOT_REWRITABLE) {
        assert(!rewrite_args);
        rewrite_args = NULL;
    }

    int npassed_args = argspec.totalPassed();

    if (rewrite_args && !rewrite_args->args_guarded) {
        // TODO duplication with runtime_call
        // TODO should know which args don't need to be guarded, ex if we're guaranteed that they
        // already fit, either since the type inferencer could determine that,
        // or because they only need to fit into an UNKNOWN slot.

        if (npassed_args >= 1)
            rewrite_args->arg1->addAttrGuard(offsetof(Box, cls), (intptr_t)arg1->cls);
        if (npassed_args >= 2)
            rewrite_args->arg2->addAttrGuard(offsetof(Box, cls), (intptr_t)arg2->cls);
        if (npassed_args >= 3)
            rewrite_args->arg3->addAttrGuard(offsetof(Box, cls), (intptr_t)arg3->cls);

        if (npassed_args > 3) {
            for (int i = 3; i < npassed_args; i++) {
                // TODO if there are a lot of args (>16), might be better to increment a pointer
                // rather index them directly?
                RewriterVar* v = rewrite_args->args->getAttr((i - 3) * sizeof(Box*), Location::any());
                v->addAttrGuard(offsetof(Box, cls), (intptr_t)args[i - 3]->cls);
            }
        }

        rewrite_args->args_guarded = true;
    }

    // right now I don't think this is ever called with INST_ONLY?
    assert(scope != INST_ONLY);

    // Look up the argument. Pass in the arguments to getattrInternalGeneral or getclsattr_general
    // that will shortcut functions by not putting them into instancemethods
    Box* bind_obj = NULL; // Initialize this to NULL to allow getattrInternalEx to ignore it
    RewriterVar* r_bind_obj = NULL;
    Box* val;
    RewriterVar* r_val = NULL;
    if (rewrite_args) {
        GetattrRewriteArgs grewrite_args(rewrite_args->rewriter, rewrite_args->obj, Location::any());
        val = getattrInternalEx<S, REWRITABLE>(obj, attr, &grewrite_args, scope == CLASS_ONLY, true, &bind_obj,
                                               &r_bind_obj);

        if (!grewrite_args.isSuccessful())
            rewrite_args = NULL;
        else {
            RewriterVar* rtn;
            ReturnConvention return_convention;
            std::tie(rtn, return_convention) = grewrite_args.getReturn();

            if (S == CXX && return_convention == ReturnConvention::CAPI_RETURN) {
                rewrite_args->rewriter->checkAndThrowCAPIException(rtn);
                return_convention = ReturnConvention::HAS_RETURN;
            }

            if (return_convention != ReturnConvention::HAS_RETURN && return_convention != ReturnConvention::NO_RETURN)
                rewrite_args = NULL;
            else
                r_val = rtn;

            if (rewrite_args)
                assert((bool)val == (return_convention == ReturnConvention::HAS_RETURN));
        }
    } else {
        val = getattrInternalEx<S, NOT_REWRITABLE>(obj, attr, NULL, scope == CLASS_ONLY, true, &bind_obj, &r_bind_obj);
    }

    if (val == NULL) {
        if (rewrite_args)
            rewrite_args->setReturn(NULL, ReturnConvention::NO_RETURN);

        return val;
    }

    AUTO_XDECREF(bind_obj);

    if (bind_obj != NULL) {
        Box** new_args = NULL;
        if (npassed_args >= 3) {
            new_args = (Box**)alloca(sizeof(Box*) * (npassed_args + 1 - 3));
        }

        if (rewrite_args) {
            r_val->addGuard((int64_t)val);
            rewrite_args->obj = r_val;
            rewrite_args->func_guarded = true;
        }

        ArgPassSpec new_argspec
            = bindObjIntoArgs(bind_obj, r_bind_obj, rewrite_args, argspec, arg1, arg2, arg3, args, new_args);
        argspec = new_argspec;
        args = new_args;
    } else {
        if (rewrite_args) {
            rewrite_args->obj = r_val;
        }
    }

    Box* rtn;
    if (unlikely(rewrite_args && rewrite_args->rewriter->aggressiveness() < 50)) {
        class Helper {
        public:
            static Box* call(STOLEN(Box*) val, ArgPassSpec argspec, Box* arg1, Box* arg2, Box* arg3,
                             void** extra_args) {
                if (!val) {
                    assert(S == CAPI);
                    return NULL;
                }
                AUTO_DECREF(val);

                Box** args = (Box**)extra_args[0];
                const std::vector<BoxedString*>* keyword_names = (const std::vector<BoxedString*>*)extra_args[1];
                Box* rtn
                    = runtimeCallInternal<S, NOT_REWRITABLE>(val, NULL, argspec, arg1, arg2, arg3, args, keyword_names);
                return rtn;
            }
        };

        RewriterVar::SmallVector arg_vec;

        arg_vec.push_back(rewrite_args->obj);
        arg_vec.push_back(rewrite_args->rewriter->loadConst(argspec.asInt()));
        arg_vec.push_back(argspec.totalPassed() >= 1 ? rewrite_args->arg1 : rewrite_args->rewriter->loadConst(0));
        arg_vec.push_back(argspec.totalPassed() >= 2 ? rewrite_args->arg2 : rewrite_args->rewriter->loadConst(0));
        arg_vec.push_back(argspec.totalPassed() >= 3 ? rewrite_args->arg3 : rewrite_args->rewriter->loadConst(0));

        RewriterVar* arg_array = rewrite_args->rewriter->allocate(2);
        arg_vec.push_back(arg_array);
        if (argspec.totalPassed() >= 4)
            arg_array->setAttr(0, rewrite_args->args);
        else
            arg_array->setAttr(0, rewrite_args->rewriter->loadConst(0));
        if (argspec.num_keywords)
            arg_array->setAttr(8, rewrite_args->rewriter->loadConst((intptr_t)keyword_names));
        else
            arg_array->setAttr(8, rewrite_args->rewriter->loadConst(0));

        auto r_rtn = rewrite_args->rewriter->call(true, (void*)Helper::call, arg_vec)->setType(RefType::OWNED);
        rewrite_args->obj->refConsumed();
        rewrite_args->setReturn(r_rtn, S == CXX ? ReturnConvention::HAS_RETURN : ReturnConvention::CAPI_RETURN);

        void* _args[2] = { args, const_cast<std::vector<BoxedString*>*>(keyword_names) };
        Box* rtn = Helper::call(val, argspec, arg1, arg2, arg3, _args);
        return rtn;
    }

    AUTO_DECREF(val);

    Box* r;
    if (rewrite_args) {
        CallRewriteArgs crewrite_args(rewrite_args);
        r = runtimeCallInternal<S, REWRITABLE>(val, &crewrite_args, argspec, arg1, arg2, arg3, args, keyword_names);
        if (crewrite_args.out_success)
            rewrite_args->setReturn(crewrite_args.out_rtn,
                                    S == CXX ? ReturnConvention::HAS_RETURN : ReturnConvention::CAPI_RETURN);
    } else {
        r = runtimeCallInternal<S, NOT_REWRITABLE>(val, NULL, argspec, arg1, arg2, arg3, args, keyword_names);
    }
    return r;
}

// Force instantiation of the template
template Box* callattrInternal<CAPI, NOT_REWRITABLE>(Box*, BoxedString*, LookupScope, CallattrRewriteArgs*, ArgPassSpec,
                                                     Box*, Box*, Box*, Box**, const std::vector<BoxedString*>*);

template <ExceptionStyle S>
Box* _callattrEntry(Box* obj, BoxedString* attr, CallattrFlags flags, Box* arg1, Box* arg2, Box* arg3, Box** args,
                    const std::vector<BoxedString*>* keyword_names, void* return_addr) {
    STAT_TIMER(t0, "us_timer_slowpath_callattr", 10);

    if (S == CAPI)
        assert(!flags.null_on_nonexistent);

#if 0 && STAT_TIMERS
    static uint64_t* st_id = Stats::getStatCounter("us_timer_slowpath_callattr_patchable");
    static uint64_t* st_id_nopatch = Stats::getStatCounter("us_timer_slowpath_callattr_nopatch");
    static uint64_t* st_id_megamorphic = Stats::getStatCounter("us_timer_slowpath_callattr_megamorphic");
    ICInfo* icinfo = getICInfo(return_addr);
    uint64_t* counter;
    if (!icinfo)
        counter = st_id_nopatch;
    else if (icinfo->isMegamorphic())
        counter = st_id_megamorphic;
    else {
        //counter = Stats::getStatCounter("us_timer_slowpath_callattr_patchable_" + std::string(obj->cls->tp_name));
        counter = Stats::getStatCounter("us_timer_slowpath_callattr_patchable_" + std::string(attr->s()));
    }
    ScopedStatTimer st(counter, 10);
#endif

    ArgPassSpec argspec(flags.argspec);
    int npassed_args = argspec.totalPassed();

    static StatCounter slowpath_callattr("slowpath_callattr");
    slowpath_callattr.log();

    assert(attr);

    int num_orig_args = 4 + std::min(4, npassed_args);
    if (argspec.num_keywords)
        num_orig_args++;

    // Uncomment this to help debug if callsites aren't getting rewritten:
    // printf("Slowpath call: %p (%s.%s)\n", return_addr, obj->cls->tp_name, attr->c_str());

    std::unique_ptr<Rewriter> rewriter(Rewriter::createRewriter(return_addr, num_orig_args, "callattr"));
    Box* rtn;

    LookupScope scope = flags.cls_only ? CLASS_ONLY : CLASS_OR_INST;

    if (attr->data()[0] == '_' && attr->data()[1] == '_' && PyInstance_Check(obj)) {
        // __enter__ and __exit__ need special treatment.
        if (attr->s() == "__enter__" || attr->s() == "__exit__")
            scope = CLASS_OR_INST;
    }

    if (rewriter.get()) {
        // TODO feel weird about doing this; it either isn't necessary
        // or this kind of thing is necessary in a lot more places
        // rewriter->getArg(3).addGuard(npassed_args);

        CallattrRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0)->setType(RefType::BORROWED),
                                         rewriter->getReturnDestination());
        if (npassed_args >= 1)
            rewrite_args.arg1 = rewriter->getArg(3)->setType(RefType::BORROWED);
        if (npassed_args >= 2)
            rewrite_args.arg2 = rewriter->getArg(4)->setType(RefType::BORROWED);
        if (npassed_args >= 3)
            rewrite_args.arg3 = rewriter->getArg(5)->setType(RefType::BORROWED);
        if (npassed_args >= 4)
            rewrite_args.args = rewriter->getArg(6);
        rtn = callattrInternal<S, REWRITABLE>(obj, attr, scope, &rewrite_args, argspec, arg1, arg2, arg3, args,
                                              keyword_names);

        assert(!(S == CAPI && flags.null_on_nonexistent));
        if (!rewrite_args.isSuccessful()) {
            rewriter.reset(NULL);
        } else {
            RewriterVar* rtn;
            ReturnConvention return_convention;
            std::tie(rtn, return_convention) = rewrite_args.getReturn();

            if (return_convention == ReturnConvention::HAS_RETURN
                || (S == CAPI && return_convention == ReturnConvention::CAPI_RETURN)) {
                assert(rtn);
                rewriter->commitReturning(rtn);
            } else if (return_convention == ReturnConvention::NO_RETURN && flags.null_on_nonexistent) {
                assert(!rtn);
                rewriter->commitReturningNonPython(rewriter->loadConst(0, rewriter->getReturnDestination()));
            }
        }
    } else {
        rtn = callattrInternal<S, NOT_REWRITABLE>(obj, attr, scope, NULL, argspec, arg1, arg2, arg3, args,
                                                  keyword_names);
    }

    if (S == CXX && rtn == NULL && !flags.null_on_nonexistent) {
        raiseAttributeError(obj, attr->s());
    } else if (S == CAPI) {
        if (!rtn && !PyErr_Occurred())
            raiseAttributeErrorCapi(obj, attr->s());
    }

    return rtn;
}

extern "C" Box* callattr(Box* obj, BoxedString* attr, CallattrFlags flags, Box* arg1, Box* arg2, Box* arg3, Box** args,
                         const std::vector<BoxedString*>* keyword_names) {
    return _callattrEntry<CXX>(obj, attr, flags, arg1, arg2, arg3, args, keyword_names,
                               __builtin_extract_return_addr(__builtin_return_address(0)));
}

extern "C" Box* callattrCapi(Box* obj, BoxedString* attr, CallattrFlags flags, Box* arg1, Box* arg2, Box* arg3,
                             Box** args, const std::vector<BoxedString*>* keyword_names) noexcept {
    return _callattrEntry<CAPI>(obj, attr, flags, arg1, arg2, arg3, args, keyword_names,
                                __builtin_extract_return_addr(__builtin_return_address(0)));
}

static inline RewriterVar* getArg(int idx, _CallRewriteArgsBase* rewrite_args) {
    if (idx == 0)
        return rewrite_args->arg1;
    if (idx == 1)
        return rewrite_args->arg2;
    if (idx == 2)
        return rewrite_args->arg3;
    return rewrite_args->args->getAttr(sizeof(Box*) * (idx - 3));
}

static StatCounter slowpath_pickversion("slowpath_pickversion");
static CompiledFunction* pickVersion(FunctionMetadata* f, ExceptionStyle S, int num_output_args, Box* oarg1, Box* oarg2,
                                     Box* oarg3, Box** oargs) {
    LOCK_REGION(codegen_rwlock.asWrite());

    if (f->always_use_version && f->always_use_version->exception_style == S)
        return f->always_use_version;
    slowpath_pickversion.log();

    CompiledFunction* best_nonexcmatch = NULL;

    for (CompiledFunction* cf : f->versions) {
        assert(cf->spec->arg_types.size() == num_output_args);

        if (!cf->spec->boxed_return_value)
            continue;

        if (!cf->spec->accepts_all_inputs) {
            assert(cf->spec->rtn_type->llvmType() == UNKNOWN->llvmType());

            bool works = true;
            for (int i = 0; i < num_output_args; i++) {
                Box* arg = getArg(i, oarg1, oarg2, oarg3, oargs);

                ConcreteCompilerType* t = cf->spec->arg_types[i];
                if ((arg && !t->isFitBy(arg->cls)) || (!arg && t != UNKNOWN)) {
                    works = false;
                    break;
                }
            }

            if (!works)
                continue;
        }

        if (cf->exception_style == S)
            return cf;
        else if (!best_nonexcmatch)
            best_nonexcmatch = cf;
    }

    if (best_nonexcmatch)
        return best_nonexcmatch;

    if (f->source == NULL) {
        // TODO I don't think this should be happening any more?
        printf("Error: couldn't find suitable function version and no source to recompile!\n");
        printf("(First version: %p)\n", f->versions[0]->code);
        abort();
    }

    return NULL;
}

static llvm::StringRef getFunctionName(FunctionMetadata* f) {
    if (f->source)
        return f->source->getName()->s();
    else if (f->versions.size()) {
        return "<builtin function>";
        // std::ostringstream oss;
        // oss << "<function at " << f->versions[0]->code << ">";
        // return oss.str();
    }
    return "<unknown function>";
}

template <typename FuncNameCB>
static int placeKeyword(const ParamNames* param_names, llvm::SmallVector<bool, 8>& params_filled, BoxedString* kw_name,
                        Box* kw_val, Box*& oarg1, Box*& oarg2, Box*& oarg3, Box** oargs, BoxedDict* okwargs,
                        FuncNameCB func_name_cb) {
    assert(kw_val);
    assert(kw_name);

    for (int j = 0; j < param_names->args.size(); j++) {
        if (param_names->args[j] == kw_name->s() && kw_name->size() > 0) {
            if (params_filled[j]) {
                raiseExcHelper(TypeError, "%.200s() got multiple values for keyword argument '%s'", func_name_cb(),
                               kw_name->c_str());
            }
            getArg(j, oarg1, oarg2, oarg3, oargs) = incref(kw_val);
            params_filled[j] = true;
            return j;
        }
    }

    if (okwargs) {
        Box*& v = okwargs->d[kw_name];
        if (v) {
            raiseExcHelper(TypeError, "%.200s() got multiple values for keyword argument '%s'", func_name_cb(),
                           kw_name->c_str());
        }
        incref(kw_name);
        v = incref(kw_val);
        return -1;
    } else {
        raiseExcHelper(TypeError, "%.200s() got an unexpected keyword argument '%s'", func_name_cb(), kw_name->c_str());
    }
}

template <ExceptionStyle S>
static Box* _callFuncHelper(BoxedFunctionBase* func, ArgPassSpec argspec, Box* arg1, Box* arg2, Box* arg3,
                            void** extra_args) {
    Box** args = (Box**)extra_args[0];
    auto keyword_names = (const std::vector<BoxedString*>*)extra_args[1];
    return callFunc<S, NOT_REWRITABLE>(func, NULL, argspec, arg1, arg2, arg3, args, keyword_names);
}

typedef std::function<Box*(int, int, RewriterVar*&)> GetDefaultFunc;

ArgPassSpec bindObjIntoArgs(Box* bind_obj, RewriterVar* r_bind_obj, _CallRewriteArgsBase* rewrite_args,
                            ArgPassSpec argspec, Box*& arg1, Box*& arg2, Box*& arg3, Box** args, Box** new_args) {
    int npassed_args = argspec.totalPassed();

    assert((new_args != NULL) == (npassed_args >= 3));

    if (npassed_args >= 3) {
        new_args[0] = arg3;
        memcpy(new_args + 1, args, (npassed_args - 3) * sizeof(Box*));
    }

    arg3 = arg2;
    arg2 = arg1;
    arg1 = bind_obj;

    if (rewrite_args) {
        if (npassed_args >= 3) {
            rewrite_args->args = rewrite_args->rewriter->allocateAndCopyPlus1(
                rewrite_args->arg3, npassed_args == 3 ? NULL : rewrite_args->args, npassed_args - 3);
        }
        rewrite_args->arg3 = rewrite_args->arg2;
        rewrite_args->arg2 = rewrite_args->arg1;
        rewrite_args->arg1 = r_bind_obj;
    }

    return ArgPassSpec(argspec.num_args + 1, argspec.num_keywords, argspec.has_starargs, argspec.has_kwargs);
}

template <typename FT> class ExceptionCleanup {
private:
    FT functor;

public:
    ExceptionCleanup(FT ft) : functor(std::move(ft)) {}
    ~ExceptionCleanup() {
        if (isUnwinding())
            functor();
    }
};

void decrefOargs(RewriterVar* oargs, bool* oargs_owned, int num_oargs) {
    for (int i = 0; i < num_oargs; i++) {
        if (oargs_owned[i])
            oargs->getAttr(i * sizeof(Box*))->setType(RefType::OWNED);
    }
}

template <Rewritable rewritable, typename FuncNameCB>
void rearrangeArgumentsInternal(ParamReceiveSpec paramspec, const ParamNames* param_names, FuncNameCB func_name_cb,
                                Box** defaults, _CallRewriteArgsBase* rewrite_args, bool& rewrite_success,
                                ArgPassSpec argspec, Box*& oarg1, Box*& oarg2, Box*& oarg3, Box** args, Box** oargs,
                                const std::vector<BoxedString*>* keyword_names, bool* oargs_owned) {
    if (rewritable == NOT_REWRITABLE) {
        assert(!rewrite_args);
        rewrite_args = NULL;
    }

    /*
     * Procedure:
     * - First match up positional arguments; any extra go to varargs.  error if too many.
     * - Then apply keywords; any extra go to kwargs.  error if too many.
     * - Use defaults to fill in any missing
     * - error about missing parameters
     */

    int num_output_args = paramspec.totalReceived();
    int num_passed_args = argspec.totalPassed();

    assert((oargs != NULL) == (num_output_args > 3));
    assert((defaults != NULL) == (paramspec.num_defaults != 0));

    if (rewrite_args && oargs) {
        assert(oargs_owned);
        memset(oargs_owned, 0, (num_output_args - 3) * sizeof(bool));
    }

    if (rewrite_args) {
        rewrite_success = false; // default case
    }

    auto propagate_args = [&]() {
        if (num_output_args >= 1)
            Py_XINCREF(oarg1);
        if (num_output_args >= 2)
            Py_XINCREF(oarg2);
        if (num_output_args >= 3)
            Py_XINCREF(oarg3);
        if (num_output_args >= 3) {
            memcpy(oargs, args, sizeof(Box*) * (num_output_args - 3));
            for (int i = 0; i < num_output_args - 3; i++) {
                Py_XINCREF(oargs[i]);
            }
        }
    };

    // Super fast path:
    if (argspec.num_keywords == 0 && !argspec.has_starargs && !paramspec.takes_varargs && !argspec.has_kwargs
        && argspec.num_args == paramspec.num_args && !paramspec.takes_kwargs) {
        rewrite_success = true;
        propagate_args();
        return;
    }

    // Fast path: if it's a simple-enough call, we don't have to do anything special.  On a simple
    // django-admin test this covers something like 93% of all calls to callFunc.
    if (argspec.num_keywords == 0 && argspec.has_starargs == paramspec.takes_varargs && !argspec.has_kwargs
        && argspec.num_args == paramspec.num_args && (!paramspec.takes_kwargs || paramspec.kwargsIndex() < 3)) {

        // TODO could also do this for empty varargs
        if (paramspec.takes_kwargs) {
            assert(num_output_args == num_passed_args + 1);
            int idx = paramspec.kwargsIndex();
            assert(idx < 3);
            getArg(idx, oarg1, oarg2, oarg3, NULL) = NULL; // pass NULL for kwargs
            if (rewrite_args) {
                if (idx == 0)
                    rewrite_args->arg1 = rewrite_args->rewriter->loadConst(0)->setType(RefType::BORROWED);
                else if (idx == 1)
                    rewrite_args->arg2 = rewrite_args->rewriter->loadConst(0)->setType(RefType::BORROWED);
                else if (idx == 2)
                    rewrite_args->arg3 = rewrite_args->rewriter->loadConst(0)->setType(RefType::BORROWED);
                else
                    abort();
            }
        } else {
            assert(num_output_args == num_passed_args);
        }

        // If the caller passed starargs, we can only pass those directly to the callee if it's a tuple,
        // since otherwise modifications by the callee would be visible to the caller (hence why varargs
        // received by the caller are always tuples).
        // This is why we can't pass kwargs here.
        if (argspec.has_starargs) {
            Box* given_varargs = getArg(argspec.num_args + argspec.num_keywords, oarg1, oarg2, oarg3, args);
            if (given_varargs->cls == tuple_cls) {
                if (rewrite_args) {
                    getArg(argspec.num_args + argspec.num_keywords, rewrite_args)
                        ->addAttrGuard(offsetof(Box, cls), (intptr_t)tuple_cls);
                }
                rewrite_success = true;
                propagate_args();
                return;
            }
        } else {
            rewrite_success = true;
            propagate_args();
            return;
        }
    }

    // Save the original values:
    Box* arg1 = oarg1;
    Box* arg2 = oarg2;
    Box* arg3 = oarg3;
    oarg1 = oarg2 = oarg3 = NULL;

    // Clear any increfs we did for when we throw an exception:
    auto clear_refs = [&]() {
        Py_XDECREF(oarg1);
        Py_XDECREF(oarg2);
        Py_XDECREF(oarg3);
        for (int i = 0; i < num_output_args - 3; i++) {
            Py_XDECREF(oargs[i]);
        }
    };
    ExceptionCleanup<decltype(clear_refs)> cleanup(
        clear_refs); // I feel like there should be some way to automatically infer the decltype

    static StatCounter slowpath_rearrangeargs_slowpath("slowpath_rearrangeargs_slowpath");
    slowpath_rearrangeargs_slowpath.log();

    if (argspec.has_starargs || argspec.has_kwargs || (paramspec.takes_kwargs && argspec.num_keywords)) {
        rewrite_args = NULL;
    }

    if (paramspec.takes_varargs && argspec.num_args > paramspec.num_args + 6) {
        // We currently only handle up to 6 arguments into the varargs tuple
        rewrite_args = NULL;
    }

    // At this point we are not allowed to abort the rewrite any more, since we will start
    // modifying rewrite_args.

    if (rewrite_args)
        rewrite_success = true;

    if (rewrite_args) {
        // We might have trouble if we have more output args than input args,
        // such as if we need more space to pass defaults.
        if (num_output_args > 3 && num_output_args > num_passed_args) {
            int arg_bytes_required = (num_output_args - 3) * sizeof(Box*);
            RewriterVar* new_args = NULL;

            assert((rewrite_args->args == NULL) == (num_passed_args <= 3));
            if (num_passed_args <= 3) {
                // we weren't passed args
                new_args = rewrite_args->rewriter->allocate(num_output_args - 3);
            } else {
                new_args = rewrite_args->rewriter->allocateAndCopy(rewrite_args->args, num_output_args - 3);
            }

            rewrite_args->args = new_args;
        }
    }

    DecrefHandle<PyObject, true> varargs(NULL);
    size_t varargs_size = 0;
    if (argspec.has_starargs) {
        assert(!rewrite_args);
        Box* given_varargs = getArg(argspec.num_args + argspec.num_keywords, arg1, arg2, arg3, args);
        varargs = PySequence_Fast(given_varargs, "argument after * must be a sequence");
        if (!varargs)
            return throwCAPIException();
        varargs_size = PySequence_Fast_GET_SIZE(varargs);
    }

    ////
    // First, match up positional parameters to positional/varargs:
    int positional_to_positional = std::min(argspec.num_args, paramspec.num_args);
    for (int i = 0; i < positional_to_positional; i++) {
        getArg(i, oarg1, oarg2, oarg3, oargs) = incref(getArg(i, arg1, arg2, arg3, args));
    }

    int varargs_to_positional = std::min((int)varargs_size, paramspec.num_args - positional_to_positional);
    for (int i = 0; i < varargs_to_positional; i++) {
        assert(!rewrite_args && "would need to be handled here");
        getArg(i + positional_to_positional, oarg1, oarg2, oarg3, oargs) = incref(PySequence_Fast_GET_ITEM(varargs, i));
    }

    llvm::SmallVector<bool, 8> params_filled(num_output_args);
    for (int i = 0; i < positional_to_positional + varargs_to_positional; i++) {
        params_filled[i] = true;
    }

    // unussed_positional relies on the fact that all the args (including a potentially-created varargs) will keep its
    // contents alive
    llvm::SmallVector<BORROWED(Box*), 4> unused_positional;
    unused_positional.reserve(argspec.num_args - positional_to_positional + varargs_size - varargs_to_positional);

    RewriterVar::SmallVector unused_positional_rvars;
    for (int i = positional_to_positional; i < argspec.num_args; i++) {
        unused_positional.push_back(getArg(i, arg1, arg2, arg3, args));
        if (rewrite_args) {
            if (i == 0)
                unused_positional_rvars.push_back(rewrite_args->arg1);
            if (i == 1)
                unused_positional_rvars.push_back(rewrite_args->arg2);
            if (i == 2)
                unused_positional_rvars.push_back(rewrite_args->arg3);
            if (i >= 3)
                unused_positional_rvars.push_back(
                    rewrite_args->args->getAttr((i - 3) * sizeof(Box*))->setType(RefType::BORROWED));
        }
    }
    for (int i = varargs_to_positional; i < varargs_size; i++) {
        assert(!rewrite_args);
        unused_positional.push_back(PySequence_Fast_GET_ITEM(varargs, i));
    }

    if (paramspec.takes_varargs) {
        int varargs_idx = paramspec.num_args;
        if (rewrite_args) {
            assert(!varargs_size);
            assert(!argspec.has_starargs);

            RewriterVar* varargs_val;
            int varargs_size = unused_positional_rvars.size();
            bool is_owned = false;

            if (varargs_size == 0) {
                varargs_val
                    = rewrite_args->rewriter->loadConst((intptr_t)EmptyTuple,
                                                        varargs_idx < 3 ? Location::forArg(varargs_idx)
                                                                        : Location::any())->setType(RefType::BORROWED);
            } else {
                assert(varargs_size <= 6);
                void* create_ptrs[] = {
                    NULL,                       //
                    (void*)BoxedTuple::create1, //
                    (void*)BoxedTuple::create2, //
                    (void*)BoxedTuple::create3, //
                    (void*)BoxedTuple::create4, //
                    (void*)BoxedTuple::create5, //
                    (void*)BoxedTuple::create6, //
                };
                varargs_val = rewrite_args->rewriter->call(true, create_ptrs[varargs_size], unused_positional_rvars)
                                  ->setType(RefType::OWNED);
                is_owned = true;
            }

            if (varargs_val) {
                if (varargs_idx == 0)
                    rewrite_args->arg1 = varargs_val;
                if (varargs_idx == 1)
                    rewrite_args->arg2 = varargs_val;
                if (varargs_idx == 2)
                    rewrite_args->arg3 = varargs_val;
                if (varargs_idx >= 3) {
                    rewrite_args->args->setAttr((varargs_idx - 3) * sizeof(Box*), varargs_val);
                    if (is_owned) {
                        oargs_owned[varargs_idx - 3] = true;
                        varargs_val->refConsumed();
                    }
                }
            }
        }

        Box* ovarargs;
        if (argspec.num_args == 0 && paramspec.num_args == 0 && (!varargs || varargs->cls == tuple_cls)) {
            // We probably could have cut out a lot more of the overhead in this case:
            assert(varargs_size == unused_positional.size());

            if (!varargs)
                ovarargs = incref(EmptyTuple);
            else
                ovarargs
                    = incref(varargs.get()); // TODO we could have DecrefHandle be smart and hand off it's reference
        } else {
            ovarargs = BoxedTuple::create(unused_positional.size(), unused_positional.data());
        }
        assert(ovarargs->cls == tuple_cls);

        getArg(varargs_idx, oarg1, oarg2, oarg3, oargs) = ovarargs;
    } else if (unused_positional.size()) {
        raiseExcHelper(TypeError, "%s() takes at most %d argument%s (%ld given)", func_name_cb(), paramspec.num_args,
                       (paramspec.num_args == 1 ? "" : "s"), argspec.num_args + argspec.num_keywords + varargs_size);
    }

    ////
    // Second, apply any keywords:

    // Speed hack: we try to not create the kwargs dictionary if it will end up being empty.
    // So if we see that we need to pass something, first set it to NULL, and then store the
    // pointer here so that if we need to we can instantiate the dict and store it here.
    // If you need to access the dict, you should call get_okwargs()
    BoxedDict** _okwargs = NULL;
    if (paramspec.takes_kwargs) {
        int kwargs_idx = paramspec.num_args + (paramspec.takes_varargs ? 1 : 0);
        if (rewrite_args) {
            RewriterVar* r_kwargs = rewrite_args->rewriter->loadConst(0);

            if (kwargs_idx == 0)
                rewrite_args->arg1 = r_kwargs;
            if (kwargs_idx == 1)
                rewrite_args->arg2 = r_kwargs;
            if (kwargs_idx == 2)
                rewrite_args->arg3 = r_kwargs;
            if (kwargs_idx >= 3)
                rewrite_args->args->setAttr((kwargs_idx - 3) * sizeof(Box*), r_kwargs);
        }

        _okwargs = (BoxedDict**)&getArg(kwargs_idx, oarg1, oarg2, oarg3, oargs);
        *_okwargs = NULL;
    }
    auto get_okwargs = [=]() {
        if (!paramspec.takes_kwargs)
            return (BoxedDict*)nullptr;
        BoxedDict* okw = *_okwargs;
        if (okw)
            return okw;
        okw = *_okwargs = new BoxedDict();
        return okw;
    };

    if ((!param_names || !param_names->takes_param_names) && argspec.num_keywords && !paramspec.takes_kwargs) {
        raiseExcHelper(TypeError, "%s() doesn't take keyword arguments", func_name_cb());
    }

    if (argspec.num_keywords) {
        assert(argspec.num_keywords == keyword_names->size());

        RewriterVar::SmallVector r_vars;
        if (rewrite_args) {
            for (int i = argspec.num_args; i < argspec.num_args + argspec.num_keywords; i++) {
                if (i == 0)
                    r_vars.push_back(rewrite_args->arg1);
                if (i == 1)
                    r_vars.push_back(rewrite_args->arg2);
                if (i == 2)
                    r_vars.push_back(rewrite_args->arg3);
                if (i >= 3)
                    r_vars.push_back(rewrite_args->args->getAttr((i - 3) * sizeof(Box*))->setType(RefType::BORROWED));
            }
        }

        BoxedDict* okwargs = get_okwargs();
        for (int i = 0; i < argspec.num_keywords; i++) {
            if (rewrite_args)
                assert(!okwargs && "would need to be handled here");

            int arg_idx = i + argspec.num_args;
            Box* kw_val = getArg(arg_idx, arg1, arg2, arg3, args);

            if (!param_names || !param_names->takes_param_names) {
                assert(!rewrite_args); // would need to add it to r_kwargs
                okwargs->d[incref((*keyword_names)[i])] = incref(kw_val);
                continue;
            }

            int dest = placeKeyword(param_names, params_filled, (*keyword_names)[i], kw_val, oarg1, oarg2, oarg3, oargs,
                                    okwargs, func_name_cb);
            if (rewrite_args) {
                assert(dest != -1);
                if (dest == 0)
                    rewrite_args->arg1 = r_vars[i];
                else if (dest == 1)
                    rewrite_args->arg2 = r_vars[i];
                else if (dest == 2)
                    rewrite_args->arg3 = r_vars[i];
                else
                    rewrite_args->args->setAttr((dest - 3) * sizeof(Box*), r_vars[i]);
            }
        }
    }

    if (argspec.has_kwargs) {
        assert(!rewrite_args && "would need to be handled here");

        Box* kwargs
            = getArg(argspec.num_args + argspec.num_keywords + (argspec.has_starargs ? 1 : 0), arg1, arg2, arg3, args);

        if (!kwargs) {
            // TODO could try to avoid creating this
            kwargs = new BoxedDict();
        } else if (!PyDict_Check(kwargs)) {
            BoxedDict* d = new BoxedDict();
            dictMerge(d, kwargs);
            kwargs = d;
        } else {
            Py_INCREF(kwargs);
        }
        DecrefHandle<Box> _kwargs_handle(kwargs);

        assert(PyDict_Check(kwargs));
        BoxedDict* d_kwargs = static_cast<BoxedDict*>(kwargs);

        BoxedDict* okwargs = NULL;
        if (d_kwargs->d.size()) {
            okwargs = get_okwargs();

            if (!okwargs && (!param_names || !param_names->takes_param_names))
                raiseExcHelper(TypeError, "%s() doesn't take keyword arguments", func_name_cb());
        }

        for (const auto& p : *d_kwargs) {
            auto k = coerceUnicodeToStr<CXX>(p.first);
            AUTO_DECREF(k);

            if (k->cls != str_cls)
                raiseExcHelper(TypeError, "%s() keywords must be strings", func_name_cb());

            BoxedString* s = static_cast<BoxedString*>(k);

            if (param_names && param_names->takes_param_names) {
                assert(!rewrite_args && "would need to make sure that this didn't need to go into r_kwargs");
                placeKeyword(param_names, params_filled, s, p.second, oarg1, oarg2, oarg3, oargs, okwargs,
                             func_name_cb);
            } else {
                assert(!rewrite_args && "would need to make sure that this didn't need to go into r_kwargs");
                assert(okwargs);

                Box*& v = okwargs->d[p.first];
                if (v) {
                    raiseExcHelper(TypeError, "%s() got multiple values for keyword argument '%s'", func_name_cb(),
                                   s->data());
                }
                v = incref(p.second);
                incref(p.first);
                assert(!rewrite_args);
            }
        }
    }

    // Fill with defaults:

    for (int i = 0; i < paramspec.num_args - paramspec.num_defaults; i++) {
        if (params_filled[i])
            continue;

        int min_args = paramspec.num_args - paramspec.num_defaults;
        const char* exactly = (paramspec.num_defaults || paramspec.takes_varargs) ? "at least" : "exactly";
        raiseExcHelper(TypeError, "%s() takes %s %d argument%s (%ld given)", func_name_cb(), exactly, min_args,
                       min_args == 1 ? "" : "s", argspec.num_args + argspec.num_keywords + varargs_size);
    }

    // There can be more defaults than arguments.
    for (int arg_idx = std::max(0, paramspec.num_args - paramspec.num_defaults); arg_idx < paramspec.num_args;
         arg_idx++) {
        if (params_filled[arg_idx])
            continue;

        int default_idx = arg_idx + paramspec.num_defaults - paramspec.num_args;

        Box* default_obj = defaults[default_idx];

        if (rewrite_args) {
            if (arg_idx == 0)
                rewrite_args->arg1 = rewrite_args->rewriter->loadConst((intptr_t)default_obj, Location::forArg(0))
                                         ->setType(RefType::BORROWED);
            else if (arg_idx == 1)
                rewrite_args->arg2 = rewrite_args->rewriter->loadConst((intptr_t)default_obj, Location::forArg(1))
                                         ->setType(RefType::BORROWED);
            else if (arg_idx == 2)
                rewrite_args->arg3 = rewrite_args->rewriter->loadConst((intptr_t)default_obj, Location::forArg(2))
                                         ->setType(RefType::BORROWED);
            else {
                auto rvar = rewrite_args->rewriter->loadConst((intptr_t)default_obj);
                rewrite_args->args->setAttr((arg_idx - 3) * sizeof(Box*), rvar);
            }
        }

        getArg(arg_idx, oarg1, oarg2, oarg3, oargs) = xincref(default_obj);
    }
}

template <Rewritable rewritable>
void rearrangeArguments(ParamReceiveSpec paramspec, const ParamNames* param_names, const char* func_name,
                        Box** defaults, _CallRewriteArgsBase* rewrite_args, bool& rewrite_success, ArgPassSpec argspec,
                        Box*& oarg1, Box*& oarg2, Box*& oarg3, Box** args, Box** oargs,
                        const std::vector<BoxedString*>* keyword_names, bool* oargs_owned) {
    auto func = [func_name]() { return func_name; };
    return rearrangeArgumentsInternal<rewritable>(paramspec, param_names, func, defaults, rewrite_args, rewrite_success,
                                                  argspec, oarg1, oarg2, oarg3, args, oargs, keyword_names,
                                                  oargs_owned);
}
template void rearrangeArguments<REWRITABLE>(ParamReceiveSpec, const ParamNames*, const char*, Box**,
                                             _CallRewriteArgsBase*, bool&, ArgPassSpec, Box*&, Box*&, Box*&, Box**,
                                             Box**, const std::vector<BoxedString*>*, bool*);
template void rearrangeArguments<NOT_REWRITABLE>(ParamReceiveSpec, const ParamNames*, const char*, Box**,
                                                 _CallRewriteArgsBase*, bool&, ArgPassSpec, Box*&, Box*&, Box*&, Box**,
                                                 Box**, const std::vector<BoxedString*>*, bool*);

static StatCounter slowpath_callfunc("slowpath_callfunc");
template <ExceptionStyle S, Rewritable rewritable>
Box* callFunc(BoxedFunctionBase* func, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1, Box* arg2,
              Box* arg3, Box** args, const std::vector<BoxedString*>* keyword_names) noexcept(S == CAPI) {
    if (rewritable == NOT_REWRITABLE) {
        assert(!rewrite_args);
        rewrite_args = NULL;
    }

    if (rewritable == REWRITABLE && !rewrite_args)
        return callFunc<S, NOT_REWRITABLE>(func, rewrite_args, argspec, arg1, arg2, arg3, args, keyword_names);


#if STAT_TIMERS
    StatTimer::assertActive();
    STAT_TIMER(t0, "us_timer_slowpath_callFunc", 0);
#endif
    slowpath_callfunc.log();

    FunctionMetadata* md = func->md;
    ParamReceiveSpec paramspec = func->getParamspec();

    if (rewrite_args) {
        if (!rewrite_args->func_guarded) {
            rewrite_args->obj->addGuard((intptr_t)func);
        }
        // This covers the cases where the function gets freed, as well as
        // when the defaults get changed.
        rewrite_args->rewriter->addDependenceOn(func->dependent_ics);
    }

    Box** oargs = NULL;
    bool* oargs_owned = NULL;
    bool rewrite_success = false;

    int num_output_args = paramspec.totalReceived();
    int num_passed_args = argspec.totalPassed();

    if (num_output_args > 3) {
        int size = (num_output_args - 3) * sizeof(Box*);
        oargs = (Box**)alloca(size);

        oargs_owned = (bool*)alloca((num_output_args - 3) * sizeof(bool));

#ifndef NDEBUG
        memset(&oargs[0], 0, size);
#endif
    }

    try {
        auto func_name_cb = [md]() { return getFunctionName(md).data(); };
        rearrangeArgumentsInternal<rewritable>(
            paramspec, &md->param_names, func_name_cb, paramspec.num_defaults ? func->defaults->elts : NULL,
            rewrite_args, rewrite_success, argspec, arg1, arg2, arg3, args, oargs, keyword_names, oargs_owned);
    } catch (ExcInfo e) {
        if (S == CAPI) {
            setCAPIException(e);
            return NULL;
        } else
            throw e;
    }

    if (num_output_args < 1)
        arg1 = NULL;
    if (num_output_args < 2)
        arg2 = NULL;
    if (num_output_args < 3)
        arg3 = NULL;
    AUTO_XDECREF(arg1);
    AUTO_XDECREF(arg2);
    AUTO_XDECREF(arg3);
    AUTO_XDECREF_ARRAY(oargs, num_output_args - 3);

    if (rewrite_args && !rewrite_success) {
// These are the cases that we weren't able to rewrite.
// So instead, just rewrite them to be a call to callFunc, which helps a little bit.
// TODO we should extract the rest of this function from the end of this block,
// put it in a different function, and have the rewrites target that.

// Note(kmod): I tried moving this section to runtimeCallInternal, ie to the place that calls
// callFunc.  The thought was that this would let us apply this same optimization to other
// internal callables.  It ended up hurting perf slightly; my theory is that it's because if
// an internal callable failed, it's better to call the non-internal version since it's lower
// overhead.

// To investigate the cases where we can't rewrite, enable this block.
// This also will also log the times that we call into callFunc directly
// from a rewrite.
#if 0
        char buf[80];
        snprintf(buf, sizeof(buf), "zzz_aborted_%d_args_%d_%d_%d_%d_params_%d_%d_%d_%d", md->isGenerator(),
                 argspec.num_args, argspec.num_keywords, argspec.has_starargs, argspec.has_kwargs, paramspec.num_args,
                 paramspec.num_defaults, paramspec.takes_varargs, paramspec.takes_kwargs);
        uint64_t* counter = Stats::getStatCounter(buf);
        Stats::log(counter);
#endif

        if (rewrite_args) {
            Rewriter* rewriter = rewrite_args->rewriter;
            // rewriter->trap();
            RewriterVar* args_array = rewriter->allocate(2);
            if (num_passed_args >= 4) {
                RELEASE_ASSERT(rewrite_args->args, "");
                args_array->setAttr(0, rewrite_args->args);
            }
            if (argspec.num_keywords)
                args_array->setAttr(8, rewriter->loadConst((intptr_t)keyword_names));
            else
                args_array->setAttr(8, rewriter->loadConst(0));

            RewriterVar::SmallVector arg_vec;
            arg_vec.push_back(rewrite_args->obj);
            arg_vec.push_back(rewriter->loadConst(argspec.asInt(), Location::forArg(1)));
            if (num_passed_args >= 1)
                arg_vec.push_back(rewrite_args->arg1);
            else
                arg_vec.push_back(rewriter->loadConst(0, Location::forArg(2)));
            if (num_passed_args >= 2)
                arg_vec.push_back(rewrite_args->arg2);
            else
                arg_vec.push_back(rewriter->loadConst(0, Location::forArg(3)));
            if (num_passed_args >= 3)
                arg_vec.push_back(rewrite_args->arg3);
            else
                arg_vec.push_back(rewriter->loadConst(0, Location::forArg(4)));
            arg_vec.push_back(args_array);
            for (auto v : arg_vec)
                assert(v);
            RewriterVar* r_rtn = rewriter->call(true, (void*)_callFuncHelper<S>, arg_vec)->setType(RefType::OWNED);

            rewrite_args->out_success = true;
            rewrite_args->out_rtn = r_rtn;
            rewrite_args = NULL;
        }
    }

    BoxedClosure* closure = func->closure;

    // special handling for generators:
    // the call to function containing a yield should just create a new generator object.
    Box* res;
    if (md->isGenerator()) {
        res = createGenerator(func, arg1, arg2, arg3, oargs);

        if (rewrite_args) {
            RewriterVar* r_arg1 = num_output_args >= 1 ? rewrite_args->arg1 : rewrite_args->rewriter->loadConst(0);
            RewriterVar* r_arg2 = num_output_args >= 2 ? rewrite_args->arg2 : rewrite_args->rewriter->loadConst(0);
            RewriterVar* r_arg3 = num_output_args >= 3 ? rewrite_args->arg3 : rewrite_args->rewriter->loadConst(0);
            RewriterVar* r_args = num_output_args >= 4 ? rewrite_args->args : rewrite_args->rewriter->loadConst(0);
            rewrite_args->out_rtn
                = rewrite_args->rewriter->call(true, (void*)createGenerator, rewrite_args->obj, r_arg1, r_arg2, r_arg3,
                                               r_args)->setType(RefType::OWNED);

            rewrite_args->out_success = true;
        }
    } else {
        res = callCLFunc<S, rewritable>(md, rewrite_args, num_output_args, closure, NULL, func->globals, arg1, arg2,
                                        arg3, oargs);
    }

    if (rewrite_args && num_output_args > 3)
        decrefOargs(rewrite_args->args, oargs_owned, num_output_args - 3);

    return res;
}

template <ExceptionStyle S>
static Box* callChosenCF(CompiledFunction* chosen_cf, BoxedClosure* closure, BoxedGenerator* generator, Box* globals,
                         Box* oarg1, Box* oarg2, Box* oarg3, Box** oargs) noexcept(S == CAPI) {
    if (S != chosen_cf->exception_style) {
        if (S == CAPI) {
            try {
                return callChosenCF<CXX>(chosen_cf, closure, generator, globals, oarg1, oarg2, oarg3, oargs);
            } catch (ExcInfo e) {
                setCAPIException(e);
                return NULL;
            }
        } else {
            Box* r = callChosenCF<CAPI>(chosen_cf, closure, generator, globals, oarg1, oarg2, oarg3, oargs);
            if (!r)
                throwCAPIException();
            return r;
        }
    }

    assert((globals == NULL) == (!chosen_cf->md->source || chosen_cf->md->source->scoping->areGlobalsFromModule()));

    Box* maybe_args[3];
    int nmaybe_args = 0;
    if (closure)
        maybe_args[nmaybe_args++] = closure;
    if (generator)
        maybe_args[nmaybe_args++] = generator;
    if (globals)
        maybe_args[nmaybe_args++] = globals;

    if (nmaybe_args == 0)
        return chosen_cf->call(oarg1, oarg2, oarg3, oargs);
    else if (nmaybe_args == 1)
        return chosen_cf->call1(maybe_args[0], oarg1, oarg2, oarg3, oargs);
    else if (nmaybe_args == 2)
        return chosen_cf->call2(maybe_args[0], maybe_args[1], oarg1, oarg2, oarg3, oargs);
    else {
        assert(nmaybe_args == 3);
        return chosen_cf->call3(maybe_args[0], maybe_args[1], maybe_args[2], oarg1, oarg2, oarg3, oargs);
    }
}

// This function exists for the rewriter: astInterpretFunction takes 9 args, but the rewriter
// only supports calling functions with at most 6 since it can currently only pass arguments
// in registers.
static Box* astInterpretHelper(FunctionMetadata* f, BoxedClosure* closure, BoxedGenerator* generator, Box* globals,
                               Box** _args) {
    Box* arg1 = _args[0];
    Box* arg2 = _args[1];
    Box* arg3 = _args[2];
    Box* args = _args[3];

    return astInterpretFunction(f, closure, generator, globals, arg1, arg2, arg3, (Box**)args);
}

static Box* astInterpretHelperCapi(FunctionMetadata* f, BoxedClosure* closure, BoxedGenerator* generator, Box* globals,
                                   Box** _args) noexcept {
    try {
        return astInterpretHelper(f, closure, generator, globals, _args);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

static Box* astInterpretHelper2ArgsCapi(FunctionMetadata* f, BoxedClosure* closure, BoxedGenerator* generator,
                                        Box* globals, Box* arg1, Box* arg2) noexcept {
    try {
        return astInterpretFunction(f, closure, generator, globals, arg1, arg2, NULL, NULL);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

// TODO: is it better to take the func_ptr last (requiring passing all the args), or is it better to put it
// first (requiring moving all the args)?
static Box* capiCallCxxHelper(Box* (*func_ptr)(void*, void*, void*, void*, void*), void* a, void* b, void* c, void* d,
                              void* e) noexcept {
    try {
        return func_ptr(a, b, c, d, e);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

template <ExceptionStyle S, Rewritable rewritable>
Box* callCLFunc(FunctionMetadata* md, CallRewriteArgs* rewrite_args, int num_output_args, BoxedClosure* closure,
                BoxedGenerator* generator, Box* globals, Box* oarg1, Box* oarg2, Box* oarg3,
                Box** oargs) noexcept(S == CAPI) {
    if (rewritable == NOT_REWRITABLE) {
        assert(!rewrite_args);
        rewrite_args = NULL;
    }

    CompiledFunction* chosen_cf = pickVersion(md, S, num_output_args, oarg1, oarg2, oarg3, oargs);

    if (!chosen_cf) {
        if (rewrite_args) {
            RewriterVar::SmallVector arg_vec;

            rewrite_args->rewriter->addDependenceOn(md->dependent_interp_callsites);

            // TODO this kind of embedded reference needs to be tracked by the GC somehow?
            // Or maybe it's ok, since we've guarded on the function object?
            arg_vec.push_back(rewrite_args->rewriter->loadConst((intptr_t)md, Location::forArg(0)));
            arg_vec.push_back(rewrite_args->rewriter->loadConst((intptr_t)closure, Location::forArg(1)));
            arg_vec.push_back(rewrite_args->rewriter->loadConst((intptr_t)generator, Location::forArg(2)));
            arg_vec.push_back(rewrite_args->rewriter->loadConst((intptr_t)globals, Location::forArg(3)));

            if (num_output_args <= 2) {
                if (num_output_args >= 1)
                    arg_vec.push_back(rewrite_args->arg1);
                if (num_output_args >= 2)
                    arg_vec.push_back(rewrite_args->arg2);

                if (S == CXX)
                    rewrite_args->out_rtn = rewrite_args->rewriter->call(true, (void*)astInterpretFunction, arg_vec)
                                                ->setType(RefType::OWNED);
                else
                    rewrite_args->out_rtn = rewrite_args->rewriter->call(true, (void*)astInterpretHelper2ArgsCapi,
                                                                         arg_vec)->setType(RefType::OWNED);
            } else {
                // Hacky workaround: the rewriter can only pass arguments in registers, so use this helper function
                // to unpack some of the additional arguments:
                RewriterVar* arg_array = rewrite_args->rewriter->allocate(4);
                arg_vec.push_back(arg_array);
                if (num_output_args >= 1)
                    arg_array->setAttr(0, rewrite_args->arg1);
                if (num_output_args >= 2)
                    arg_array->setAttr(8, rewrite_args->arg2);
                if (num_output_args >= 3)
                    arg_array->setAttr(16, rewrite_args->arg3);
                if (num_output_args >= 4)
                    arg_array->setAttr(24, rewrite_args->args);

                if (S == CXX)
                    rewrite_args->out_rtn = rewrite_args->rewriter->call(true, (void*)astInterpretHelper, arg_vec)
                                                ->setType(RefType::OWNED);
                else
                    rewrite_args->out_rtn = rewrite_args->rewriter->call(true, (void*)astInterpretHelperCapi, arg_vec)
                                                ->setType(RefType::OWNED);

                if (num_output_args >= 1)
                    rewrite_args->arg1->refUsed();
                if (num_output_args >= 2)
                    rewrite_args->arg2->refUsed();
                if (num_output_args >= 3)
                    rewrite_args->arg3->refUsed();
                if (num_output_args >= 4)
                    rewrite_args->args->refUsed();
            }

            rewrite_args->out_success = true;
        }

        if (S == CAPI) {
            try {
                return astInterpretFunction(md, closure, generator, globals, oarg1, oarg2, oarg3, oargs);
            } catch (ExcInfo e) {
                setCAPIException(e);
                return NULL;
            }
        } else {
            return astInterpretFunction(md, closure, generator, globals, oarg1, oarg2, oarg3, oargs);
        }
    }

    if (rewrite_args) {
        rewrite_args->rewriter->addDependenceOn(chosen_cf->dependent_callsites);

        assert(!generator);

        RewriterVar::SmallVector arg_vec;

        void* func_ptr = (void*)chosen_cf->call;
        if (S == CAPI && chosen_cf->exception_style == CXX) {
            arg_vec.push_back(rewrite_args->rewriter->loadConst((intptr_t)func_ptr, Location::forArg(0)));
            func_ptr = (void*)capiCallCxxHelper;
        }

        if (closure)
            arg_vec.push_back(rewrite_args->rewriter->loadConst((intptr_t)closure, Location::forArg(0)));
        if (globals)
            arg_vec.push_back(rewrite_args->rewriter->loadConst((intptr_t)globals, Location::forArg(0)));
        if (num_output_args >= 1)
            arg_vec.push_back(rewrite_args->arg1);
        if (num_output_args >= 2)
            arg_vec.push_back(rewrite_args->arg2);
        if (num_output_args >= 3)
            arg_vec.push_back(rewrite_args->arg3);
        if (num_output_args >= 4)
            arg_vec.push_back(rewrite_args->args);

        rewrite_args->out_rtn = rewrite_args->rewriter->call(true, func_ptr, arg_vec)->setType(RefType::OWNED);
        if (S == CXX && chosen_cf->exception_style == CAPI)
            rewrite_args->rewriter->checkAndThrowCAPIException(rewrite_args->out_rtn);

        rewrite_args->out_success = true;
    }

    if (chosen_cf->exception_style != S) {
        static StatCounter sc("num_runtimecall_exc_mismatches");
        sc.log();

        if (rewrite_args) {
            static StatCounter sc2("num_runtimecall_exc_mismatches_rewriteable");
            sc2.log();
#if 0
            StatCounter sc3("num_runtime_call_exc_mismatches_rewriteable."
                            + g.func_addr_registry.getFuncNameAtAddress(chosen_cf->code, true, NULL));
            sc3.log();
#endif
        }
    }

    // We check for this assertion later too - by checking it twice, we know
    // if the error state was set before calling the chosen CF or after.
    ASSERT(!PyErr_Occurred(), "");

    Box* r;
    // we duplicate the call to callChosenCf here so we can
    // distinguish lexically between calls that target jitted python
    // code and calls that target to builtins.
    if (md->source) {
        UNAVOIDABLE_STAT_TIMER(t0, "us_timer_in_jitted_code");
        r = callChosenCF<S>(chosen_cf, closure, generator, globals, oarg1, oarg2, oarg3, oargs);
    } else {
        UNAVOIDABLE_STAT_TIMER(t0, "us_timer_in_builtins");
        r = callChosenCF<S>(chosen_cf, closure, generator, globals, oarg1, oarg2, oarg3, oargs);
    }

    if (!r) {
        assert(S == CAPI);
    } else {
        // If this assertion is triggered because the type isn't what we expected,
        // but something that should be allowed (e.g. NotImplementedType), it is
        // possible that the program has a bad type annotation. For example, an
        // attribute that we added in C++ should have return type UNKNOWN instead
        // of BOXED_SOMETHING.
        ASSERT(chosen_cf->spec->rtn_type->isFitBy(r->cls), "%s (%p) was supposed to return %s, but gave a %s",
               g.func_addr_registry.getFuncNameAtAddress(chosen_cf->code, true, NULL).c_str(), chosen_cf->code,
               chosen_cf->spec->rtn_type->debugName().c_str(), r->cls->tp_name);
        ASSERT(!PyErr_Occurred(), "%p", chosen_cf->code);
    }

    return r;
}

// force instantiation:
template Box* callCLFunc<CAPI, REWRITABLE>(FunctionMetadata* f, CallRewriteArgs* rewrite_args, int num_output_args,
                                           BoxedClosure* closure, BoxedGenerator* generator, Box* globals, Box* oarg1,
                                           Box* oarg2, Box* oarg3, Box** oargs);
template Box* callCLFunc<CXX, REWRITABLE>(FunctionMetadata* f, CallRewriteArgs* rewrite_args, int num_output_args,
                                          BoxedClosure* closure, BoxedGenerator* generator, Box* globals, Box* oarg1,
                                          Box* oarg2, Box* oarg3, Box** oargs);
template Box* callCLFunc<CAPI, NOT_REWRITABLE>(FunctionMetadata* f, CallRewriteArgs* rewrite_args, int num_output_args,
                                               BoxedClosure* closure, BoxedGenerator* generator, Box* globals,
                                               Box* oarg1, Box* oarg2, Box* oarg3, Box** oargs);
template Box* callCLFunc<CXX, NOT_REWRITABLE>(FunctionMetadata* f, CallRewriteArgs* rewrite_args, int num_output_args,
                                              BoxedClosure* closure, BoxedGenerator* generator, Box* globals,
                                              Box* oarg1, Box* oarg2, Box* oarg3, Box** oargs);

static void getclassname(PyObject* klass, char* buf, int bufsize) noexcept {
    PyObject* name;

    assert(bufsize > 1);
    strcpy(buf, "?"); /* Default outcome */
    if (klass == NULL)
        return;
    name = PyObject_GetAttrString(klass, "__name__");
    if (name == NULL) {
        /* This function cannot return an exception */
        PyErr_Clear();
        return;
    }
    if (PyString_Check(name)) {
        strncpy(buf, PyString_AS_STRING(name), bufsize);
        buf[bufsize - 1] = '\0';
    }
    Py_DECREF(name);
}

static void getinstclassname(PyObject* inst, char* buf, int bufsize) noexcept {
    PyObject* klass;

    if (inst == NULL) {
        assert(bufsize > 0 && (size_t)bufsize > strlen("nothing"));
        strcpy(buf, "nothing");
        return;
    }

    klass = PyObject_GetAttrString(inst, "__class__");
    if (klass == NULL) {
        /* This function cannot return an exception */
        PyErr_Clear();
        klass = (PyObject*)(inst->cls);
        Py_INCREF(klass);
    }
    getclassname(klass, buf, bufsize);
    Py_XDECREF(klass);
}

const char* PyEval_GetFuncName(PyObject* func) noexcept {
    if (PyMethod_Check(func))
        return PyEval_GetFuncName(PyMethod_GET_FUNCTION(func));
    else if (PyFunction_Check(func)) {
        auto name = ((BoxedFunction*)func)->name;
        if (!name)
            return "<unknown name>";
        return PyString_AsString(name);
    } else if (PyCFunction_Check(func))
        return ((PyCFunctionObject*)func)->m_ml->ml_name;
    else if (PyClass_Check(func))
        return PyString_AsString(((BoxedClassobj*)func)->name);
    else if (PyInstance_Check(func)) {
        return PyString_AsString(((BoxedInstance*)func)->inst_cls->name);
    } else {
        return func->cls->tp_name;
    }
}

const char* PyEval_GetFuncDesc(PyObject* func) noexcept {
    if (PyMethod_Check(func))
        return "()";
    else if (PyFunction_Check(func))
        return "()";
    else if (PyCFunction_Check(func))
        return "()";
    else if (PyClass_Check(func))
        return " constructor";
    else if (PyInstance_Check(func)) {
        return " instance";
    } else {
        return " object";
    }
}


template <ExceptionStyle S, Rewritable rewritable>
Box* runtimeCallInternal(Box* obj, CallRewriteArgs* rewrite_args, ArgPassSpec argspec, Box* arg1, Box* arg2, Box* arg3,
                         Box** args, const std::vector<BoxedString*>* keyword_names) noexcept(S == CAPI) {
    if (rewritable == NOT_REWRITABLE) {
        assert(!rewrite_args);
        rewrite_args = NULL;
    }

    int npassed_args = argspec.totalPassed();

    if (obj->cls != function_cls && obj->cls != builtin_function_or_method_cls && obj->cls != instancemethod_cls) {
        // TODO: maybe eventually runtimeCallInternal should just be the default tpp_call?
        if (obj->cls->tpp_call.get(S)) {
            KEEP_ALIVE(obj); // CPython doesn't have this, but I think they should
            return obj->cls->tpp_call.call<S>(obj, rewrite_args, argspec, arg1, arg2, arg3, args, keyword_names);
        } else if (S == CAPI && obj->cls->tpp_call.get<CXX>()) {
            KEEP_ALIVE(obj);
            try {
                return obj->cls->tpp_call.call<CXX>(obj, NULL, argspec, arg1, arg2, arg3, args, keyword_names);
            } catch (ExcInfo e) {
                setCAPIException(e);
                return NULL;
            }
        } else if (S == CXX && obj->cls->tpp_call.get<CAPI>()) {
            KEEP_ALIVE(obj);
            Box* r = obj->cls->tpp_call.call<CAPI>(obj, NULL, argspec, arg1, arg2, arg3, args, keyword_names);
            if (!r)
                throwCAPIException();
            return r;
        }

        STAT_TIMER(t0, "us_timer_slowpath_runtimecall_nonfunction", 20);

#if 0
        std::string per_name_stat_name = "zzz_runtimecall_nonfunction_" + std::string(obj->cls->tp_name);
        uint64_t* counter = Stats::getStatCounter(per_name_stat_name);
        Stats::log(counter);
        if (obj->cls == wrapperobject_cls)
            printf("");
#endif

        Box* rtn;

        static BoxedString* call_str = getStaticString("__call__");

        if (DEBUG >= 2) {
            assert((obj->cls->tp_call == NULL) == (typeLookup<rewritable>(obj->cls, call_str, NULL) == NULL));
        }

        if (rewrite_args) {
            CallattrRewriteArgs crewrite_args(rewrite_args);
            rtn = callattrInternal<S, REWRITABLE>(obj, call_str, CLASS_ONLY, &crewrite_args, argspec, arg1, arg2, arg3,
                                                  args, keyword_names);

            if (!crewrite_args.isSuccessful())
                rewrite_args = NULL;
            else {
                RewriterVar* rtn;
                ReturnConvention return_convention;
                std::tie(rtn, return_convention) = crewrite_args.getReturn();

                if (return_convention == ReturnConvention::HAS_RETURN) {
                    rewrite_args->out_rtn = rtn;
                    rewrite_args->out_success = true;
                } else if (return_convention == ReturnConvention::NO_RETURN) {
                    // Could handle this, but currently don't, and probably not that important.
                    rewrite_args = NULL;
                } else {
                    rewrite_args = NULL;
                }
            }
        } else {
            rtn = callattrInternal<S, NOT_REWRITABLE>(obj, call_str, CLASS_ONLY, NULL, argspec, arg1, arg2, arg3, args,
                                                      keyword_names);
        }

        if (!rtn) {
            if (S == CAPI) {
                if (!PyErr_Occurred()) {
                    assert(!rewrite_args); // would need to rewrite this.
                    PyErr_Format(TypeError, "'%s' object is not callable", getTypeName(obj));
                }
                return NULL;
            } else
                raiseExcHelper(TypeError, "'%s' object is not callable", getTypeName(obj));
        }
        return rtn;
    }

    if (rewrite_args) {
        if (!rewrite_args->args_guarded) {
            // TODO should know which args don't need to be guarded, ex if we're guaranteed that they
            // already fit, either since the type inferencer could determine that,
            // or because they only need to fit into an UNKNOWN slot.

            int kwargs_index = -1;
            if (argspec.has_kwargs)
                kwargs_index = argspec.kwargsIndex();

            for (int i = 0; i < npassed_args; i++) {
                Box* v = getArg(i, arg1, arg2, arg3, args);

                if (i == kwargs_index) {
                    if (v == NULL) {
                        // I don't think this case should ever get hit currently -- the only places
                        // we offer rewriting are places that don't have the ability to pass a NULL
                        // kwargs.
                        getArg(i, rewrite_args)->addGuard(0);
                    } else {
                        getArg(i, rewrite_args)->addAttrGuard(offsetof(Box, cls), (intptr_t)v->cls);
                    }
                } else {
                    assert(v);
                    getArg(i, rewrite_args)->addAttrGuard(offsetof(Box, cls), (intptr_t)v->cls);
                }
            }
            rewrite_args->args_guarded = true;
        }
    }

    if (obj->cls == function_cls || obj->cls == builtin_function_or_method_cls) {
        BoxedFunctionBase* f = static_cast<BoxedFunctionBase*>(obj);

        if (rewrite_args && !rewrite_args->func_guarded) {
            rewrite_args->obj->addGuard((intptr_t)f);
            rewrite_args->func_guarded = true;
            rewrite_args->rewriter->addDependenceOn(f->dependent_ics);
        }

        // Some functions are sufficiently important that we want them to be able to patchpoint themselves;
        // they can do this by setting the "internal_callable" field:
        auto callable = f->md->internal_callable.get<S>();

        if (S == CAPI)
            assert((bool(f->md->internal_callable.get(CXX)) == bool(callable))
                   && "too many opportunities for mistakes unless both CXX and CAPI versions are implemented");
        else
            assert((bool(f->md->internal_callable.get(CAPI)) == bool(callable))
                   && "too many opportunities for mistake unless both CXX and CAPI versions are implementeds");

        if (callable == NULL) {
            callable = callFunc<S>;
        }

        KEEP_ALIVE(f);
        Box* res = callable(f, rewrite_args, argspec, arg1, arg2, arg3, args, keyword_names);
        return res;
    } else if (obj->cls == instancemethod_cls) {
        BoxedInstanceMethod* im = static_cast<BoxedInstanceMethod*>(obj);

        RewriterVar* r_im_func = NULL;
        if (rewrite_args) {
            r_im_func = rewrite_args->obj->getAttr(offsetof(BoxedInstanceMethod, func), Location::any());
        }

        if (rewrite_args && !rewrite_args->func_guarded) {
            r_im_func->addGuard((intptr_t)im->func);
            rewrite_args->func_guarded = true;
        }

        // Guard on which type of instancemethod (bound or unbound)
        // That is, if im->obj is NULL, guard on it being NULL
        // otherwise, guard on it being non-NULL
        if (rewrite_args) {
            rewrite_args->obj->addAttrGuard(offsetof(BoxedInstanceMethod, obj), 0, im->obj != NULL);
        }

        if (im->obj == NULL) {
            Box* f = im->func;
            if (rewrite_args) {
                rewrite_args->obj = r_im_func;
            }

// TODO: add back this instancemethod checking (see instancemethod_checking.py)
#if 0
            Box* first_arg = NULL;
            if (argspec.num_args > 0) {
                first_arg = arg1;
            } else if (argspec.has_starargs) {
                Box* varargs = getArg(argspec.starargsIndex(), arg1, arg2, arg3, args);
                assert(varargs->cls == tuple_cls);
                auto t_varargs = static_cast<BoxedTuple*>(varargs);
                if (t_varargs->ob_size > 0)
                    first_arg = t_varargs->elts[0];
            }

            int ok;
            if (first_arg == NULL)
                ok = 0;
            else {
                ok = PyObject_IsInstance(first_arg, im->im_class);
                if (ok < 0)
                    return NULL;
            }

            if (!ok) {
                char clsbuf[256];
                char instbuf[256];
                getclassname(im->im_class, clsbuf, sizeof(clsbuf));
                getinstclassname(first_arg, instbuf, sizeof(instbuf));
                PyErr_Format(PyExc_TypeError, "unbound method %s%s must be called with "
                                              "%s instance as first argument "
                                              "(got %s%s instead)",
                             PyEval_GetFuncName(f), PyEval_GetFuncDesc(f), clsbuf, instbuf,
                             first_arg == NULL ? "" : " instance");
                if (S == CAPI)
                    return NULL;
                else
                    throwCAPIException();
            }
#endif

            Box* res
                = runtimeCallInternal<S, rewritable>(f, rewrite_args, argspec, arg1, arg2, arg3, args, keyword_names);
            return res;
        }

        Box** new_args = NULL;
        if (npassed_args >= 3) {
            new_args = (Box**)alloca(sizeof(Box*) * (npassed_args + 1 - 3));
        }

        RewriterVar* r_bind_obj = NULL;
        if (rewrite_args) {
            r_bind_obj = rewrite_args->obj->getAttr(offsetof(BoxedInstanceMethod, obj));
            rewrite_args->obj = r_im_func;
        }

        ArgPassSpec new_argspec
            = bindObjIntoArgs(im->obj, r_bind_obj, rewrite_args, argspec, arg1, arg2, arg3, args, new_args);
        return runtimeCallInternal<S, rewritable>(im->func, rewrite_args, new_argspec, arg1, arg2, arg3, new_args,
                                                  keyword_names);
    }
    assert(0);
    abort();
}
// Force instantiation:
template Box* runtimeCallInternal<CAPI, REWRITABLE>(Box*, CallRewriteArgs*, ArgPassSpec, Box*, Box*, Box*, Box**,
                                                    const std::vector<BoxedString*>*);
template Box* runtimeCallInternal<CXX, REWRITABLE>(Box*, CallRewriteArgs*, ArgPassSpec, Box*, Box*, Box*, Box**,
                                                   const std::vector<BoxedString*>*);
template Box* runtimeCallInternal<CAPI, NOT_REWRITABLE>(Box*, CallRewriteArgs*, ArgPassSpec, Box*, Box*, Box*, Box**,
                                                        const std::vector<BoxedString*>*);
template Box* runtimeCallInternal<CXX, NOT_REWRITABLE>(Box*, CallRewriteArgs*, ArgPassSpec, Box*, Box*, Box*, Box**,
                                                       const std::vector<BoxedString*>*);

template <ExceptionStyle S>
static Box* runtimeCallEntry(Box* obj, ArgPassSpec argspec, Box* arg1, Box* arg2, Box* arg3, Box** args,
                             const std::vector<BoxedString*>* keyword_names, void* return_addr) noexcept(S == CAPI) {
    STAT_TIMER(t0, "us_timer_slowpath_runtimecall", 10);

    int npassed_args = argspec.totalPassed();

    int num_orig_args = 2 + std::min(4, npassed_args);
    if (argspec.num_keywords > 0) {
        assert(argspec.num_keywords == keyword_names->size());
        num_orig_args++;
    }
    std::unique_ptr<Rewriter> rewriter(Rewriter::createRewriter(return_addr, num_orig_args, "runtimeCall"));

    Box* rtn;

#if 0 && STAT_TIMERS
    static uint64_t* st_id = Stats::getStatCounter("us_timer_slowpath_runtimecall_patchable");
    static uint64_t* st_id_nopatch = Stats::getStatCounter("us_timer_slowpath_runtimecall_nopatch");
    static uint64_t* st_id_megamorphic = Stats::getStatCounter("us_timer_slowpath_runtimecall_megamorphic");
    ICInfo* icinfo = getICInfo(return_addr);
    uint64_t* counter;
    if (!icinfo)
        counter = st_id_nopatch;
    else if (icinfo->isMegamorphic())
        counter = st_id_megamorphic;
    else {
        counter = Stats::getStatCounter("us_timer_slowpath_runtimecall_patchable_" + std::string(obj->cls->tp_name));
    }
    ScopedStatTimer st(counter, 10);

    static int n = 0;
    n++;

    if (n == 57261)
        printf("");
#endif

    if (rewriter.get()) {
        // TODO feel weird about doing this; it either isn't necessary
        // or this kind of thing is necessary in a lot more places
        // rewriter->getArg(1).addGuard(npassed_args);

        CallRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0)->setType(RefType::BORROWED),
                                     rewriter->getReturnDestination());
        if (npassed_args >= 1)
            rewrite_args.arg1 = rewriter->getArg(2)->setType(RefType::BORROWED);
        if (npassed_args >= 2)
            rewrite_args.arg2 = rewriter->getArg(3)->setType(RefType::BORROWED);
        if (npassed_args >= 3)
            rewrite_args.arg3 = rewriter->getArg(4)->setType(RefType::BORROWED);
        if (npassed_args >= 4)
            rewrite_args.args = rewriter->getArg(5);
        rtn = runtimeCallInternal<S, REWRITABLE>(obj, &rewrite_args, argspec, arg1, arg2, arg3, args, keyword_names);

        if (!rewrite_args.out_success) {
            rewriter.reset(NULL);
        } else
            rewriter->commitReturning(rewrite_args.out_rtn);
    } else {
        rtn = runtimeCallInternal<S, NOT_REWRITABLE>(obj, NULL, argspec, arg1, arg2, arg3, args, keyword_names);
    }
    assert(rtn || (S == CAPI && PyErr_Occurred()));

// XXX
#ifndef NDEBUG
    rewriter.release();
#endif

    return rtn;
}

extern "C" Box* runtimeCall(Box* obj, ArgPassSpec argspec, Box* arg1, Box* arg2, Box* arg3, Box** args,
                            const std::vector<BoxedString*>* keyword_names) {
    static StatCounter slowpath_runtimecall("slowpath_runtimecall");
    slowpath_runtimecall.log();
    return runtimeCallEntry<CXX>(obj, argspec, arg1, arg2, arg3, args, keyword_names,
                                 __builtin_extract_return_addr(__builtin_return_address(0)));
}

extern "C" Box* runtimeCallCapi(Box* obj, ArgPassSpec argspec, Box* arg1, Box* arg2, Box* arg3, Box** args,
                                const std::vector<BoxedString*>* keyword_names) noexcept {
    static StatCounter slowpath_runtimecall("slowpath_runtimecall_capi");
    slowpath_runtimecall.log();
    // TODO add rewriting
    return runtimeCallEntry<CAPI>(obj, argspec, arg1, arg2, arg3, args, keyword_names,
                                  __builtin_extract_return_addr(__builtin_return_address(0)));
}

template <Rewritable rewritable>
static Box* binopInternalHelper(BinopRewriteArgs*& rewrite_args, BoxedString* op_name, Box* lhs, Box* rhs,
                                RewriterVar* r_lhs, RewriterVar* r_rhs) {
    if (rewritable == NOT_REWRITABLE) {
        assert(!rewrite_args);
        rewrite_args = NULL;
    }

    struct NotImplementedHelper {
        static void call(Box* r, bool was_notimplemented) { assert((r == NotImplemented) == was_notimplemented); }
    };

    Box* rtn = NULL;
    if (rewrite_args) {
        CallattrRewriteArgs srewrite_args(rewrite_args->rewriter, r_lhs, rewrite_args->destination);
        srewrite_args.arg1 = r_rhs;
        srewrite_args.args_guarded = true;
        rtn = callattrInternal1<CXX, REWRITABLE>(lhs, op_name, CLASS_ONLY, &srewrite_args, ArgPassSpec(1), rhs);

        if (!srewrite_args.isSuccessful()) {
            rewrite_args = NULL;
        } else if (rtn) {
            rewrite_args->out_rtn = srewrite_args.getReturn(ReturnConvention::HAS_RETURN);
// If we allowed a rewrite to get here, it means that we assumed that the class will return NotImplemented
// or not based only on the types of the inputs.
#ifndef NDEBUG
            rewrite_args->rewriter->call(false, (void*)NotImplementedHelper::call, rewrite_args->out_rtn,
                                         rewrite_args->rewriter->loadConst(rtn == NotImplemented));
#endif
        } else {
            srewrite_args.assertReturnConvention(ReturnConvention::NO_RETURN);
        }

        if (rewrite_args && rtn) {
            if (rtn != NotImplemented)
                rewrite_args->out_success = true;
            else {
                // I think our guarding up to here is correct; the issue is we won't be able to complete
                // the rewrite since we have more guards to do, but we already did some mutations.
                rewrite_args->out_success = false;
                rewrite_args = NULL;
                REWRITE_ABORTED("");
            }
        }
        // we don't need to abort the rewrite when the attribute does not exist (rtn==null) because we only rewrite
        // binops when both sides are not user defined types for which we assume that they will never change.
    } else {
        rtn = callattrInternal1<CXX, NOT_REWRITABLE>(lhs, op_name, CLASS_ONLY, NULL, ArgPassSpec(1), rhs);
    }

    return rtn;
}

template <Rewritable rewritable>
Box* binopInternal(Box* lhs, Box* rhs, int op_type, bool inplace, BinopRewriteArgs* rewrite_args) {
    if (rewritable == NOT_REWRITABLE) {
        assert(!rewrite_args);
        rewrite_args = NULL;
    }

    RewriterVar* r_lhs = NULL;
    RewriterVar* r_rhs = NULL;
    if (rewrite_args) {
        r_lhs = rewrite_args->lhs;
        r_rhs = rewrite_args->rhs;

        RewriterVar* r_lhs_cls = r_lhs->getAttr(offsetof(Box, cls))->setType(RefType::BORROWED);
        r_lhs_cls->addGuard((intptr_t)lhs->cls);
        RewriterVar* r_rhs_cls = r_rhs->getAttr(offsetof(Box, cls))->setType(RefType::BORROWED);
        r_rhs_cls->addGuard((intptr_t)rhs->cls);

        r_lhs_cls->addAttrGuard(offsetof(BoxedClass, tp_mro), (intptr_t)lhs->cls->tp_mro);
        r_rhs_cls->addAttrGuard(offsetof(BoxedClass, tp_mro), (intptr_t)rhs->cls->tp_mro);
    }

    if (inplace) {
        // XXX I think we need to make sure that we keep these strings alive?
        DecrefHandle<BoxedString> iop_name = getInplaceOpName(op_type);
        Box* irtn = binopInternalHelper<rewritable>(rewrite_args, iop_name, lhs, rhs, r_lhs, r_rhs);
        if (irtn) {
            if (irtn != NotImplemented)
                return irtn;
            Py_DECREF(irtn);
        }
    }

    bool should_try_reverse = true;
    if (lhs->cls != rhs->cls && isSubclass(rhs->cls, lhs->cls)) {
        should_try_reverse = false;
        DecrefHandle<BoxedString> rop_name = getReverseOpName(op_type);
        Box* rrtn = binopInternalHelper<rewritable>(rewrite_args, rop_name, rhs, lhs, r_rhs, r_lhs);
        if (rrtn) {
            if (rrtn != NotImplemented)
                return rrtn;
            Py_DECREF(rrtn);
        }
    }

    BORROWED(BoxedString*)op_name = getOpName(op_type);
    Box* lrtn = binopInternalHelper<rewritable>(rewrite_args, op_name, lhs, rhs, r_lhs, r_rhs);
    if (lrtn) {
        if (lrtn != NotImplemented)
            return lrtn;
        Py_DECREF(lrtn);
    }

    if (should_try_reverse) {
        DecrefHandle<BoxedString> rop_name = getReverseOpName(op_type);
        Box* rrtn = binopInternalHelper<rewritable>(rewrite_args, rop_name, rhs, lhs, r_rhs, r_lhs);
        if (rrtn) {
            if (rrtn != NotImplemented)
                return rrtn;
            Py_DECREF(rrtn);
        }
    }

    llvm::StringRef op_sym = getOpSymbol(op_type);
    const char* op_sym_suffix = "";
    if (inplace) {
        op_sym_suffix = "=";
    }

    raiseExcHelper(TypeError, "unsupported operand type(s) for %s%s: '%s' and '%s'", op_sym.data(), op_sym_suffix,
                   getTypeName(lhs), getTypeName(rhs));
}
template Box* binopInternal<REWRITABLE>(Box*, Box*, int, bool, BinopRewriteArgs*);
template Box* binopInternal<NOT_REWRITABLE>(Box*, Box*, int, bool, BinopRewriteArgs*);

extern "C" Box* binop(Box* lhs, Box* rhs, int op_type) {
    STAT_TIMER(t0, "us_timer_slowpath_binop", 10);
    bool can_patchpoint = !lhs->cls->is_user_defined && !rhs->cls->is_user_defined;
#if 0
    static uint64_t* st_id = Stats::getStatCounter("us_timer_slowpath_binop_patchable");
    static uint64_t* st_id_nopatch = Stats::getStatCounter("us_timer_slowpath_binop_nopatch");
    bool havepatch = (bool)getICInfo(__builtin_extract_return_addr(__builtin_return_address(0)));
    ScopedStatTimer st((havepatch && can_patchpoint)? st_id : st_id_nopatch, 10);
#endif

    static StatCounter slowpath_binop("slowpath_binop");
    slowpath_binop.log();
    // static StatCounter nopatch_binop("nopatch_binop");

    // int id = Stats::getStatId("slowpath_binop_" + *getTypeName(lhs) + op_name + *getTypeName(rhs));
    // Stats::log(id);

    std::unique_ptr<Rewriter> rewriter((Rewriter*)NULL);
    // Currently can't patchpoint user-defined binops since we can't assume that just because
    // resolving it one way right now (ex, using the value from lhs.__add__) means that later
    // we'll resolve it the same way, even for the same argument types.
    // TODO implement full resolving semantics inside the rewrite?
    if (can_patchpoint)
        rewriter.reset(
            Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 3, "binop"));

    Box* rtn;
    if (rewriter.get()) {
        // rewriter->trap();
        BinopRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0)->setType(RefType::BORROWED),
                                      rewriter->getArg(1)->setType(RefType::BORROWED),
                                      rewriter->getReturnDestination());
        rtn = binopInternal<REWRITABLE>(lhs, rhs, op_type, false, &rewrite_args);
        assert(rtn);
        if (!rewrite_args.out_success) {
            rewriter.reset(NULL);
        } else {
            rewriter->commitReturning(rewrite_args.out_rtn);
        }
    } else {
        rtn = binopInternal<NOT_REWRITABLE>(lhs, rhs, op_type, false, NULL);
    }

// XXX
#ifndef NDEBUG
    rewriter.release();
#endif

    return rtn;
}

extern "C" Box* augbinop(Box* lhs, Box* rhs, int op_type) {
    STAT_TIMER(t0, "us_timer_slowpath_augbinop", 10);

    static StatCounter slowpath_augbinop("slowpath_augbinop");
    slowpath_augbinop.log();
    // static StatCounter nopatch_binop("nopatch_augbinop");

    // int id = Stats::getStatId("slowpath_augbinop_" + *getTypeName(lhs) + op_name + *getTypeName(rhs));
    // Stats::log(id);

    std::unique_ptr<Rewriter> rewriter((Rewriter*)NULL);
    // Currently can't patchpoint user-defined binops since we can't assume that just because
    // resolving it one way right now (ex, using the value from lhs.__add__) means that later
    // we'll resolve it the same way, even for the same argument types.
    // TODO implement full resolving semantics inside the rewrite?
    bool can_patchpoint = !lhs->cls->is_user_defined && !rhs->cls->is_user_defined;
    if (can_patchpoint)
        rewriter.reset(
            Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 3, "binop"));

    Box* rtn;
    if (rewriter.get()) {
        BinopRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0), rewriter->getArg(1),
                                      rewriter->getReturnDestination());
        rtn = binopInternal<REWRITABLE>(lhs, rhs, op_type, true, &rewrite_args);
        if (!rewrite_args.out_success) {
            rewriter.reset(NULL);
        } else {
            rewriter->commitReturning(rewrite_args.out_rtn);
        }
    } else {
        rtn = binopInternal<NOT_REWRITABLE>(lhs, rhs, op_type, true, NULL);
    }

    return rtn;
}

static bool convert3wayCompareResultToBool(Box* v, int op_type) {
    long result = PyInt_AsLong(v);
    if (result == -1 && PyErr_Occurred())
        throwCAPIException();
    switch (op_type) {
        case AST_TYPE::Eq:
            return result == 0;
        case AST_TYPE::NotEq:
            return result != 0;
        case AST_TYPE::Lt:
            return result < 0;
        case AST_TYPE::Gt:
            return result > 0;
        case AST_TYPE::LtE:
            return result < 0 || result == 0;
        case AST_TYPE::GtE:
            return result > 0 || result == 0;
        default:
            RELEASE_ASSERT(0, "op type %d not implemented", op_type);
    };
}

template <bool negate> Box* nonzeroAndBox(Box* b) {
    if (likely(b->cls == bool_cls)) {
        if (negate)
            return boxBool(b != True);
        return incref(b);
    }

    bool t = b->nonzeroIC();
    if (negate)
        t = !t;
    return boxBool(t);
}

template <Rewritable rewritable>
Box* compareInternal(Box* lhs, Box* rhs, int op_type, CompareRewriteArgs* rewrite_args) {
    if (rewritable == NOT_REWRITABLE) {
        assert(!rewrite_args);
        rewrite_args = NULL;
    }

    STAT_TIMER(t0, "us_timer_compareinternal", 0);

    if (op_type == AST_TYPE::Is || op_type == AST_TYPE::IsNot) {
        bool neg = (op_type == AST_TYPE::IsNot);

        if (rewrite_args) {
            RewriterVar* cmpres = rewrite_args->lhs->cmp(neg ? AST_TYPE::NotEq : AST_TYPE::Eq, rewrite_args->rhs,
                                                         rewrite_args->destination);
            rewrite_args->out_rtn
                = rewrite_args->rewriter->call(false, (void*)boxBool, cmpres)->setType(RefType::OWNED);
            rewrite_args->out_success = true;
        }

        return boxBool((lhs == rhs) ^ neg);
    }

    if (op_type == AST_TYPE::In || op_type == AST_TYPE::NotIn) {
        static BoxedString* contains_str = getStaticString("__contains__");

        // The checks for this branch are taken from CPython's PySequence_Contains
        if (PyType_HasFeature(rhs->cls, Py_TPFLAGS_HAVE_SEQUENCE_IN)) {
            PySequenceMethods* sqm = rhs->cls->tp_as_sequence;
            if (sqm != NULL && sqm->sq_contains != NULL && sqm->sq_contains != slot_sq_contains) {
                if (rewrite_args) {
                    RewriterVar* r_lhs = rewrite_args->lhs;
                    RewriterVar* r_rhs = rewrite_args->rhs;
                    RewriterVar* r_cls = r_rhs->getAttr(offsetof(Box, cls));
                    RewriterVar* r_sqm = r_cls->getAttr(offsetof(BoxedClass, tp_as_sequence));
                    r_sqm->addGuardNotEq(0);
                    // We might need to guard on tp_flags if they can change?

                    // Currently, guard that the value of sq_contains didn't change, and then
                    // emit a call to the current function address.
                    // It might be better to just load the current value of sq_contains and call it
                    // (after guarding it's not null), or maybe not.  But the rewriter doesn't currently
                    // support calling a RewriterVar (can only call fixed function addresses).
                    r_sqm->addAttrGuard(offsetof(PySequenceMethods, sq_contains), (intptr_t)sqm->sq_contains);
                    RewriterVar* r_b = rewrite_args->rewriter->call(true, (void*)sqm->sq_contains, r_rhs, r_lhs);
                    rewrite_args->rewriter->checkAndThrowCAPIException(r_b, -1);

                    // This could be inlined:
                    RewriterVar* r_r;
                    if (op_type == AST_TYPE::NotIn)
                        r_r = rewrite_args->rewriter->call(false, (void*)boxBoolNegated, r_b)->setType(RefType::OWNED);
                    else
                        r_r = rewrite_args->rewriter->call(false, (void*)boxBool, r_b)->setType(RefType::OWNED);

                    rewrite_args->out_success = true;
                    rewrite_args->out_rtn = r_r;
                }

                int r = (*sqm->sq_contains)(rhs, lhs);
                if (r == -1)
                    throwCAPIException();
                if (op_type == AST_TYPE::NotIn)
                    r = !r;
                return boxBool(r);
            }
        }

        Box* contained;
        RewriterVar* r_contained = NULL;
        if (rewrite_args) {
            CallattrRewriteArgs crewrite_args(rewrite_args->rewriter, rewrite_args->rhs, rewrite_args->destination);
            crewrite_args.arg1 = rewrite_args->lhs;
            contained = callattrInternal1<CXX, REWRITABLE>(rhs, contains_str, CLASS_ONLY, &crewrite_args,
                                                           ArgPassSpec(1), lhs);

            if (!crewrite_args.isSuccessful())
                rewrite_args = NULL;
            else {
                RewriterVar* rtn;
                ReturnConvention return_convention;
                std::tie(rtn, return_convention) = crewrite_args.getReturn();
                if (return_convention != ReturnConvention::HAS_RETURN
                    && return_convention != ReturnConvention::NO_RETURN)
                    rewrite_args = NULL;
                else
                    r_contained = rtn;

                if (rewrite_args)
                    assert((bool)contained == (return_convention == ReturnConvention::HAS_RETURN));
            }
        } else {
            contained
                = callattrInternal1<CXX, NOT_REWRITABLE>(rhs, contains_str, CLASS_ONLY, NULL, ArgPassSpec(1), lhs);
        }

        if (contained == NULL) {
            rewrite_args = NULL;

            int result = _PySequence_IterSearch(rhs, lhs, PY_ITERSEARCH_CONTAINS);
            if (result < 0)
                throwCAPIException();
            assert(result == 0 || result == 1);
            return boxBool(op_type == AST_TYPE::NotIn ? !result : result);
        }

        bool negate = (op_type == AST_TYPE::NotIn);
        if (rewrite_args) {
            RewriterVar* r_contained_box
                = rewrite_args->rewriter->call(true, (void*)(negate ? nonzeroAndBox<true> : nonzeroAndBox<false>),
                                               r_contained)->setType(RefType::OWNED);
            rewrite_args->out_rtn = r_contained_box;
            rewrite_args->out_success = true;
        }

        if (contained->cls == bool_cls) {
            if (op_type == AST_TYPE::NotIn) {
                Py_DECREF(contained);
                return boxBool(contained == False);
            } else {
                return contained;
            }
        }

        AUTO_DECREF(contained);
        bool b = contained->nonzeroIC();
        if (negate)
            b = !b;
        return boxBool(b);
    }

    bool any_user_defined = lhs->cls->is_user_defined || rhs->cls->is_user_defined;
    if (any_user_defined) {
        rewrite_args = NULL;
        REWRITE_ABORTED("");
    }

    // Can do the guard checks after the Is/IsNot handling, since that is
    // irrespective of the object classes
    if (rewrite_args) {
        // TODO probably don't need to guard on the lhs_cls since it
        // will get checked no matter what, but the check that should be
        // removed is probably the later one.
        // ie we should have some way of specifying what we know about the values
        // of objects and their attributes, and the attributes' attributes.
        rewrite_args->lhs->addAttrGuard(offsetof(Box, cls), (intptr_t)lhs->cls);
        rewrite_args->rhs->addAttrGuard(offsetof(Box, cls), (intptr_t)rhs->cls);
    }

    // TODO: switch from our op types to cpythons
    int cpython_op_type;
    switch (op_type) {
        case AST_TYPE::Eq:
            cpython_op_type = Py_EQ;
            break;
        case AST_TYPE::NotEq:
            cpython_op_type = Py_NE;
            break;
        case AST_TYPE::Lt:
            cpython_op_type = Py_LT;
            break;
        case AST_TYPE::LtE:
            cpython_op_type = Py_LE;
            break;
        case AST_TYPE::Gt:
            cpython_op_type = Py_GT;
            break;
        case AST_TYPE::GtE:
            cpython_op_type = Py_GE;
            break;
        default:
            RELEASE_ASSERT(0, "%d", op_type);
    }

    if (!any_user_defined && lhs->cls == rhs->cls && !PyInstance_Check(lhs) && lhs->cls->tp_richcompare != NULL
        && lhs->cls->tp_richcompare != slot_tp_richcompare) {
        // This branch is the `v->ob_type == w->ob_type` branch of PyObject_RichCompare, but
        // simplified by using the assumption that tp_richcompare exists and never returns NotImplemented
        // for builtin types when both arguments are the right type.

        assert(!lhs->cls->is_user_defined);

        Box* r = lhs->cls->tp_richcompare(lhs, rhs, cpython_op_type);
        RELEASE_ASSERT(r != NotImplemented, "%s returned notimplemented?", lhs->cls->tp_name);
        if (rewrite_args) {
            rewrite_args->out_rtn
                = rewrite_args->rewriter->call(true, (void*)lhs->cls->tp_richcompare, rewrite_args->lhs,
                                               rewrite_args->rhs, rewrite_args->rewriter->loadConst(cpython_op_type))
                      ->setType(RefType::OWNED);
            rewrite_args->out_success = true;
        }
        return r;
    }

    BORROWED(BoxedString*)op_name = getOpName(op_type);

    Box* lrtn;
    if (rewrite_args) {
        CallattrRewriteArgs crewrite_args(rewrite_args->rewriter, rewrite_args->lhs, rewrite_args->destination);
        crewrite_args.arg1 = rewrite_args->rhs;
        lrtn = callattrInternal1<CXX, REWRITABLE>(lhs, op_name, CLASS_ONLY, &crewrite_args, ArgPassSpec(1), rhs);

        if (!crewrite_args.isSuccessful()) {
            rewrite_args = NULL;
        } else {
            RewriterVar* rtn;
            ReturnConvention return_convention;
            std::tie(rtn, return_convention) = crewrite_args.getReturn();
            if (return_convention != ReturnConvention::HAS_RETURN && return_convention != ReturnConvention::NO_RETURN)
                rewrite_args = NULL;
            else
                rewrite_args->out_rtn = rtn;

            if (rewrite_args)
                assert((bool)lrtn == (return_convention == ReturnConvention::HAS_RETURN));
        }
    } else {
        lrtn = callattrInternal1<CXX, NOT_REWRITABLE>(lhs, op_name, CLASS_ONLY, NULL, ArgPassSpec(1), rhs);
    }

    if (lrtn) {
        if (lrtn != NotImplemented) {
            if (rewrite_args) {
                rewrite_args->out_success = true;
            }
            return lrtn;
        } else {
            Py_DECREF(lrtn);
            rewrite_args = NULL;
        }
    }

    // TODO patch these cases
    if (rewrite_args) {
        assert(rewrite_args->out_success == false);
        rewrite_args = NULL;
        REWRITE_ABORTED("");
    }

    BORROWED(BoxedString*)rop_name = getReverseOpName(op_type);
    Box* rrtn = callattrInternal1<CXX, NOT_REWRITABLE>(rhs, rop_name, CLASS_ONLY, NULL, ArgPassSpec(1), lhs);
    if (rrtn != NULL && rrtn != NotImplemented)
        return rrtn;
    Py_XDECREF(rrtn); // in case it is NotImplemented

    static BoxedString* cmp_str = getStaticString("__cmp__");
    lrtn = callattrInternal1<CXX, NOT_REWRITABLE>(lhs, cmp_str, CLASS_ONLY, NULL, ArgPassSpec(1), rhs);
    AUTO_XDECREF(lrtn);
    if (lrtn && lrtn != NotImplemented) {
        return boxBool(convert3wayCompareResultToBool(lrtn, op_type));
    }
    rrtn = callattrInternal1<CXX, NOT_REWRITABLE>(rhs, cmp_str, CLASS_ONLY, NULL, ArgPassSpec(1), lhs);
    AUTO_XDECREF(rrtn);
    if (rrtn && rrtn != NotImplemented) {
        bool success = false;
        int reversed_op = getReverseCmpOp(op_type, success);
        assert(success);
        return boxBool(convert3wayCompareResultToBool(rrtn, reversed_op));
    }

    if (op_type == AST_TYPE::Eq)
        return boxBool(lhs == rhs);
    if (op_type == AST_TYPE::NotEq)
        return boxBool(lhs != rhs);

#ifndef NDEBUG
    if ((lhs->cls == int_cls || lhs->cls == float_cls || lhs->cls == long_cls)
        && (rhs->cls == int_cls || rhs->cls == float_cls || rhs->cls == long_cls)) {
        printf("\n%s %s %s\n", lhs->cls->tp_name, op_name->c_str(), rhs->cls->tp_name);
        Py_FatalError("missing comparison between these classes");
    }
#endif

    int c = default_3way_compare(lhs, rhs);
    return convert_3way_to_object(cpython_op_type, c);
}

extern "C" Box* compare(Box* lhs, Box* rhs, int op_type) {
    STAT_TIMER(t0, "us_timer_slowpath_compare", 10);

    static StatCounter slowpath_compare("slowpath_compare");
    slowpath_compare.log();
    static StatCounter nopatch_compare("nopatch_compare");

    std::unique_ptr<Rewriter> rewriter(
        Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 3, "compare"));

    if (rewriter.get()) {
        // rewriter->trap();
        CompareRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0)->setType(RefType::BORROWED),
                                        rewriter->getArg(1)->setType(RefType::BORROWED),
                                        rewriter->getReturnDestination());
        Box* rtn = compareInternal<REWRITABLE>(lhs, rhs, op_type, &rewrite_args);
        if (!rewrite_args.out_success) {
            rewriter.reset(NULL);
        } else
            rewriter->commitReturning(rewrite_args.out_rtn);
        return rtn;
    } else {
        // TODO: switch from our op types to cpythons
        int cpython_op_type;
        if (op_type == AST_TYPE::In || op_type == AST_TYPE::NotIn)
            return compareInternal<NOT_REWRITABLE>(lhs, rhs, op_type, NULL);
        if (op_type == AST_TYPE::Is)
            return boxBool(lhs == rhs);
        if (op_type == AST_TYPE::IsNot)
            return boxBool(lhs != rhs);
        switch (op_type) {
            case AST_TYPE::Eq:
                cpython_op_type = Py_EQ;
                break;
            case AST_TYPE::NotEq:
                cpython_op_type = Py_NE;
                break;
            case AST_TYPE::Lt:
                cpython_op_type = Py_LT;
                break;
            case AST_TYPE::LtE:
                cpython_op_type = Py_LE;
                break;
            case AST_TYPE::Gt:
                cpython_op_type = Py_GT;
                break;
            case AST_TYPE::GtE:
                cpython_op_type = Py_GE;
                break;
            default:
                RELEASE_ASSERT(0, "%d", op_type);
        }
        Box* r = PyObject_RichCompare(lhs, rhs, cpython_op_type);
        if (!r)
            throwCAPIException();
        return r;
    }
}

extern "C" Box* unaryop(Box* operand, int op_type) {
    STAT_TIMER(t0, "us_timer_slowpath_unaryop", 10);

    static StatCounter slowpath_unaryop("slowpath_unaryop");
    slowpath_unaryop.log();

    BORROWED(BoxedString*)op_name = getOpName(op_type);

    std::unique_ptr<Rewriter> rewriter(
        Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 1, "unaryop"));

    Box* rtn = NULL;
    if (rewriter.get()) {
        CallattrRewriteArgs srewrite_args(rewriter.get(), rewriter->getArg(0), rewriter->getReturnDestination());
        rtn = callattrInternal0<CXX, REWRITABLE>(operand, op_name, CLASS_ONLY, &srewrite_args, ArgPassSpec(0));

        if (srewrite_args.isSuccessful()) {
            RewriterVar* rtn;
            ReturnConvention return_convention;
            std::tie(rtn, return_convention) = srewrite_args.getReturn();
            if (return_convention == ReturnConvention::HAS_RETURN)
                rewriter->commitReturning(rtn);
        }
    } else
        rtn = callattrInternal0<CXX, NOT_REWRITABLE>(operand, op_name, CLASS_ONLY, NULL, ArgPassSpec(0));

    if (rtn == NULL) {
        raiseExcHelper(TypeError, "bad operand type for unary '%s': '%s'", op_name->c_str(), getTypeName(operand));
    }
    return rtn;
}

template <ExceptionStyle S, Rewritable rewritable>
static Box* callItemAttr(Box* target, BoxedString* item_str, Box* item, Box* value,
                         CallRewriteArgs* rewrite_args) noexcept(S == CAPI) {
    if (rewritable == NOT_REWRITABLE) {
        assert(!rewrite_args);
        rewrite_args = NULL;
    }

    if (rewrite_args) {
        CallattrRewriteArgs crewrite_args(rewrite_args);
        Box* r;
        if (value)
            r = callattrInternal2<S, REWRITABLE>(target, item_str, CLASS_ONLY, &crewrite_args, ArgPassSpec(2), item,
                                                 value);
        else
            r = callattrInternal1<S, REWRITABLE>(target, item_str, CLASS_ONLY, &crewrite_args, ArgPassSpec(1), item);

        if (crewrite_args.isSuccessful()) {
            rewrite_args->out_success = true;
            if (r || PyErr_Occurred())
                rewrite_args->out_rtn
                    = crewrite_args.getReturn(S == CAPI ? ReturnConvention::CAPI_RETURN : ReturnConvention::HAS_RETURN);
            else
                rewrite_args->out_rtn = crewrite_args.getReturn(ReturnConvention::NO_RETURN);
        }
        return r;
    } else {
        if (value)
            return callattrInternal2<S, NOT_REWRITABLE>(target, item_str, CLASS_ONLY, NULL, ArgPassSpec(2), item,
                                                        value);
        else
            return callattrInternal1<S, NOT_REWRITABLE>(target, item_str, CLASS_ONLY, NULL, ArgPassSpec(1), item);
    }
}

#define ISINDEX(x) ((x) == NULL || PyInt_Check(x) || PyLong_Check(x) || PyIndex_Check(x))

extern "C" PyObject* apply_slice(PyObject* u, PyObject* v, PyObject* w) noexcept /* return u[v:w] */
{
    // TODO: add rewriting here

    PyTypeObject* tp = u->cls;
    PySequenceMethods* sq = tp->tp_as_sequence;

    if (sq && sq->sq_slice && ISINDEX(v) && ISINDEX(w)) {
        Py_ssize_t ilow = 0, ihigh = PY_SSIZE_T_MAX;
        if (!_PyEval_SliceIndex(v, &ilow))
            return NULL;
        if (!_PyEval_SliceIndex(w, &ihigh))
            return NULL;
        return PySequence_GetSlice(u, ilow, ihigh);
    } else {
        PyObject* slice = PySlice_New(v, w, NULL);
        if (slice != NULL) {
            PyObject* res = PyObject_GetItem(u, slice);
            Py_DECREF(slice);
            return res;
        } else
            return NULL;
    }
}

// This function decides whether to call the slice operator (e.g. __getslice__)
// or the item operator (__getitem__).
template <ExceptionStyle S, Rewritable rewritable>
static Box* callItemOrSliceAttr(Box* target, BoxedString* item_str, BoxedString* slice_str, Box* slice, Box* value,
                                CallRewriteArgs* rewrite_args) noexcept(S == CAPI) {
    if (rewritable == NOT_REWRITABLE) {
        assert(!rewrite_args);
        rewrite_args = NULL;
    }

    // This function contains a lot of logic for deciding between whether to call
    // the slice operator or the item operator, so we can match CPython's behavior
    // on custom classes that define those operators. However, for builtin types,
    // we know we can call either and the behavior will be the same. Adding all those
    // guards are unnecessary and bad for performance.
    //
    // Also, for special slicing logic (e.g. open slice ranges [:]), the builtin types
    // have C-implemented functions that already handle all the edge cases, so we don't
    // need to have a slowpath for them here.
    if (target->cls == list_cls || target->cls == str_cls || target->cls == unicode_cls) {
        if (rewrite_args) {
            rewrite_args->obj->addAttrGuard(offsetof(Box, cls), (uint64_t)target->cls);
        }
        return callItemAttr<S, rewritable>(target, item_str, slice, value, rewrite_args);
    }

    // Guard on the type of the object (need to have the slice operator attribute to call it).
    bool has_slice_attr = false;
    if (rewrite_args) {
        RewriterVar* target_cls = rewrite_args->obj->getAttr(offsetof(Box, cls));
        GetattrRewriteArgs grewrite_args(rewrite_args->rewriter, target_cls, Location::any());
        has_slice_attr = (bool)typeLookup(target->cls, slice_str, &grewrite_args);
        if (!grewrite_args.isSuccessful()) {
            rewrite_args = NULL;
        } else {
            RewriterVar* rtn;
            ReturnConvention return_convention;
            std::tie(rtn, return_convention) = grewrite_args.getReturn();
            if (return_convention != ReturnConvention::HAS_RETURN && return_convention != ReturnConvention::NO_RETURN)
                rewrite_args = NULL;

            if (rewrite_args)
                assert(has_slice_attr == (return_convention == ReturnConvention::HAS_RETURN));
        }
    } else {
        has_slice_attr = (bool)typeLookup(target->cls, slice_str);
    }

    if (!has_slice_attr) {
        return callItemAttr<S, rewritable>(target, item_str, slice, value, rewrite_args);
    }

    // Need a slice object to use the slice operators.
    if (rewrite_args) {
        rewrite_args->arg1->addAttrGuard(offsetof(Box, cls), (uint64_t)slice->cls);
    }
    if (slice->cls != slice_cls) {
        return callItemAttr<S, rewritable>(target, item_str, slice, value, rewrite_args);
    }

    BoxedSlice* bslice = (BoxedSlice*)slice;

    // If we use slice notation with a step parameter (e.g. o[1:10:2]), the slice operator
    // functions don't support that, so fallback to the item operator functions.
    if (bslice->step->cls != none_cls) {
        if (rewrite_args) {
            rewrite_args->arg1->getAttr(offsetof(BoxedSlice, step))
                ->addAttrGuard(offsetof(Box, cls), (uint64_t)none_cls, /*negate=*/true);
        }

        return callItemAttr<S, rewritable>(target, item_str, slice, value, rewrite_args);
    } else {
        rewrite_args = NULL;
        REWRITE_ABORTED("");

        // If the slice cannot be used as integer slices, also fall back to the get operator.
        // We could optimize further here by having a version of isSliceIndex that
        // creates guards, but it would only affect some rare edge cases.
        if (!isSliceIndex(bslice->start) || !isSliceIndex(bslice->stop)) {
            return callItemAttr<S, NOT_REWRITABLE>(target, item_str, slice, value, rewrite_args);
        }

        // If we don't specify the start/stop (e.g. o[:]), the slice operator functions
        // CPython seems to use 0 and sys.maxint as the default values.
        int64_t start = 0, stop = PyInt_GetMax();
        if (S == CAPI) {
            if (bslice->start != None)
                if (!_PyEval_SliceIndex(bslice->start, &start))
                    return NULL;
            if (bslice->stop != None)
                if (!_PyEval_SliceIndex(bslice->stop, &stop))
                    return NULL;
        } else {
            sliceIndex(bslice->start, &start);
            sliceIndex(bslice->stop, &stop);
        }

        adjustNegativeIndicesOnObject(target, &start, &stop);
        if (PyErr_Occurred())
            throwCAPIException();

        Box* boxedStart = boxInt(start);
        Box* boxedStop = boxInt(stop);
        AUTO_DECREF(boxedStart);
        AUTO_DECREF(boxedStop);

        if (rewrite_args) {
            CallattrRewriteArgs crewrite_args(rewrite_args);
            Box* r;
            if (value)
                r = callattrInternal3<S, REWRITABLE>(target, slice_str, CLASS_ONLY, &crewrite_args, ArgPassSpec(3),
                                                     boxedStart, boxedStop, value);
            else
                r = callattrInternal2<S, REWRITABLE>(target, slice_str, CLASS_ONLY, &crewrite_args, ArgPassSpec(2),
                                                     boxedStart, boxedStop);

            if (crewrite_args.isSuccessful()) {
                rewrite_args->out_success = true;
                rewrite_args->out_rtn = crewrite_args.getReturn(ReturnConvention::HAS_RETURN);
            }
            return r;
        } else {
            if (value)
                return callattrInternal3<S, NOT_REWRITABLE>(target, slice_str, CLASS_ONLY, NULL, ArgPassSpec(3),
                                                            boxedStart, boxedStop, value);
            else
                return callattrInternal2<S, NOT_REWRITABLE>(target, slice_str, CLASS_ONLY, NULL, ArgPassSpec(2),
                                                            boxedStart, boxedStop);
        }
    }
}

template <ExceptionStyle S, Rewritable rewritable>
Box* getitemInternal(Box* target, Box* slice, GetitemRewriteArgs* rewrite_args) noexcept(S == CAPI) {
    if (rewritable == NOT_REWRITABLE) {
        assert(!rewrite_args);
        rewrite_args = NULL;
    }

    // The PyObject_GetItem logic is:
    // - call mp_subscript if it exists
    // - if tp_as_sequence exists, try using that (with a number of conditions)
    // - else throw an exception.
    //
    // For now, just use the first clause: call mp_subscript if it exists.
    // And only if we think it's better than calling __getitem__, which should
    // exist if mp_subscript exists.
    PyMappingMethods* m = target->cls->tp_as_mapping;
    if (m && m->mp_subscript && m->mp_subscript != slot_mp_subscript) {
        if (rewrite_args) {
            RewriterVar* r_obj = rewrite_args->target;
            RewriterVar* r_slice = rewrite_args->slice;
            RewriterVar* r_cls = r_obj->getAttr(offsetof(Box, cls));
            RewriterVar* r_m = r_cls->getAttr(offsetof(BoxedClass, tp_as_mapping));
            r_m->addGuardNotEq(0);

            // Currently, guard that the value of mp_subscript didn't change, and then
            // emit a call to the current function address.
            // It might be better to just load the current value of mp_subscript and call it
            // (after guarding it's not null), or maybe not.  But the rewriter doesn't currently
            // support calling a RewriterVar (can only call fixed function addresses).
            r_m->addAttrGuard(offsetof(PyMappingMethods, mp_subscript), (intptr_t)m->mp_subscript);
            RewriterVar* r_rtn
                = rewrite_args->rewriter->call(true, (void*)m->mp_subscript, r_obj, r_slice)->setType(RefType::OWNED);
            if (S == CXX)
                rewrite_args->rewriter->checkAndThrowCAPIException(r_rtn);
            rewrite_args->out_success = true;
            rewrite_args->out_rtn = r_rtn;
        }
        Box* r = m->mp_subscript(target, slice);
        if (S == CXX && !r)
            throwCAPIException();
        return r;
    }

    static BoxedString* getitem_str = getStaticString("__getitem__");
    static BoxedString* getslice_str = getStaticString("__getslice__");

    Box* rtn;
    try {
        if (rewrite_args) {
            CallRewriteArgs crewrite_args(rewrite_args->rewriter, rewrite_args->target, rewrite_args->destination);
            crewrite_args.arg1 = rewrite_args->slice;

            rtn = callItemOrSliceAttr<S, REWRITABLE>(target, getitem_str, getslice_str, slice, NULL, &crewrite_args);

            if (!crewrite_args.out_success) {
                rewrite_args = NULL;
            } else if (rtn) {
                rewrite_args->out_rtn = crewrite_args.out_rtn;
            }
        } else {
            rtn = callItemOrSliceAttr<S, NOT_REWRITABLE>(target, getitem_str, getslice_str, slice, NULL, NULL);
        }
    } catch (ExcInfo e) {
        if (S == CAPI) {
            setCAPIException(e);
            return NULL;
        } else
            throw e;
    }

    if (rtn == NULL && !(S == CAPI && PyErr_Occurred())) {
        rewrite_args = NULL;

        // different versions of python give different error messages for this:
        if (PY_MAJOR_VERSION == 2 && PY_MINOR_VERSION < 7) {
            if (S == CAPI)
                PyErr_Format(TypeError, "'%s' object is unsubscriptable", getTypeName(target)); // tested on 2.6.6
            else
                raiseExcHelper(TypeError, "'%s' object is unsubscriptable", getTypeName(target)); // tested on 2.6.6
        } else if (PY_MAJOR_VERSION == 2 && PY_MINOR_VERSION == 7 && PY_MICRO_VERSION < 3) {
            if (S == CAPI)
                PyErr_Format(TypeError, "'%s' object is not subscriptable", getTypeName(target)); // tested on 2.7.1
            else
                raiseExcHelper(TypeError, "'%s' object is not subscriptable", getTypeName(target)); // tested on 2.7.1
        } else {
            // Changed to this in 2.7.3:
            if (S == CAPI)
                PyErr_Format(TypeError, "'%s' object has no attribute '__getitem__'", getTypeName(target));
            else
                raiseExcHelper(TypeError, "'%s' object has no attribute '__getitem__'", getTypeName(target));
        }
    }

    if (rewrite_args)
        rewrite_args->out_success = true;

    return rtn;
}
// Force instantiation of the template
template Box* getitemInternal<CAPI, REWRITABLE>(Box*, Box*, GetitemRewriteArgs*);
template Box* getitemInternal<CXX, REWRITABLE>(Box*, Box*, GetitemRewriteArgs*);
template Box* getitemInternal<CAPI, NOT_REWRITABLE>(Box*, Box*, GetitemRewriteArgs*);
template Box* getitemInternal<CXX, NOT_REWRITABLE>(Box*, Box*, GetitemRewriteArgs*);

// target[slice]
extern "C" Box* getitem(Box* target, Box* slice) {
    STAT_TIMER(t0, "us_timer_slowpath_getitem", 10);

    // This possibly could just be represented as a single callattr; the only tricky part
    // are the error messages.
    // Ex "(1)[1]" and "(1).__getitem__(1)" give different error messages.

    static StatCounter slowpath_getitem("slowpath_getitem");
    slowpath_getitem.log();

    std::unique_ptr<Rewriter> rewriter(
        Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 2, "getitem"));

    Box* rtn;
    if (rewriter.get()) {
        GetitemRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0), rewriter->getArg(1),
                                        rewriter->getReturnDestination());

        rtn = getitemInternal<CXX>(target, slice, &rewrite_args);

        if (!rewrite_args.out_success) {
            rewriter.reset(NULL);
        } else {
            rewriter->commitReturning(rewrite_args.out_rtn);
        }
    } else {
        rtn = getitemInternal<CXX>(target, slice);
    }
    assert(rtn);

    return rtn;
}

// target[slice]
extern "C" Box* getitem_capi(Box* target, Box* slice) noexcept {
    STAT_TIMER(t0, "us_timer_slowpath_getitem", 10);

    // This possibly could just be represented as a single callattr; the only tricky part
    // are the error messages.
    // Ex "(1)[1]" and "(1).__getitem__(1)" give different error messages.

    static StatCounter slowpath_getitem("slowpath_getitem");
    slowpath_getitem.log();

    std::unique_ptr<Rewriter> rewriter(
        Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 2, "getitem"));

    Box* rtn;
    if (rewriter.get()) {
        GetitemRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0), rewriter->getArg(1),
                                        rewriter->getReturnDestination());

        rtn = getitemInternal<CAPI>(target, slice, &rewrite_args);

        if (!rewrite_args.out_success) {
            rewriter.reset(NULL);
        } else if (rtn) {
            rewriter->commitReturning(rewrite_args.out_rtn);
        }
    } else {
        rtn = getitemInternal<CAPI>(target, slice);
    }

    return rtn;
}

static void setitemHelper(Box* target, Box* slice, Box* value) {
    int ret = target->cls->tp_as_mapping->mp_ass_subscript(target, slice, value);
    if (ret == -1)
        throwCAPIException();
}

// target[slice] = value
extern "C" void setitem(Box* target, Box* slice, Box* value) {
    STAT_TIMER(t0, "us_timer_slowpath_setitem", 10);

    static StatCounter slowpath_setitem("slowpath_setitem");
    slowpath_setitem.log();

    std::unique_ptr<Rewriter> rewriter(
        Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 3, "setitem"));

    static BoxedString* setitem_str = getStaticString("__setitem__");
    static BoxedString* setslice_str = getStaticString("__setslice__");

    auto&& m = target->cls->tp_as_mapping;
    if (m && m->mp_ass_subscript && m->mp_ass_subscript != slot_mp_ass_subscript) {
        if (rewriter.get()) {
            RewriterVar* r_obj = rewriter->getArg(0);
            RewriterVar* r_slice = rewriter->getArg(1);
            RewriterVar* r_value = rewriter->getArg(2);
            RewriterVar* r_cls = r_obj->getAttr(offsetof(Box, cls));
            RewriterVar* r_m = r_cls->getAttr(offsetof(BoxedClass, tp_as_mapping));
            r_m->addGuardNotEq(0);
            rewriter->call(true, (void*)setitemHelper, r_obj, r_slice, r_value);
            rewriter->commit();
        }

        setitemHelper(target, slice, value);
        return;
    }

    Box* rtn;
    if (rewriter.get()) {
        CallRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0), rewriter->getReturnDestination());
        rewrite_args.arg1 = rewriter->getArg(1);
        rewrite_args.arg2 = rewriter->getArg(2);

        rtn = callItemOrSliceAttr<CXX, REWRITABLE>(target, setitem_str, setslice_str, slice, value, &rewrite_args);

        if (!rewrite_args.out_success) {
            rewriter.reset(NULL);
        }
    } else {
        rtn = callItemOrSliceAttr<CXX, NOT_REWRITABLE>(target, setitem_str, setslice_str, slice, value, NULL);
    }

    if (rtn == NULL) {
        raiseExcHelper(TypeError, "'%s' object does not support item assignment", getTypeName(target));
    }
    Py_DECREF(rtn);

    if (rewriter.get())
        rewriter->commit();
}

// del target[slice]
extern "C" void delitem(Box* target, Box* slice) {
    STAT_TIMER(t0, "us_timer_slowpath_delitem", 10);

    static StatCounter slowpath_delitem("slowpath_delitem");
    slowpath_delitem.log();

    std::unique_ptr<Rewriter> rewriter(
        Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 2, "delitem"));

    static BoxedString* delitem_str = getStaticString("__delitem__");
    static BoxedString* delslice_str = getStaticString("__delslice__");

    Box* rtn;
    if (rewriter.get()) {
        CallRewriteArgs rewrite_args(rewriter.get(), rewriter->getArg(0), rewriter->getReturnDestination());
        rewrite_args.arg1 = rewriter->getArg(1);

        rtn = callItemOrSliceAttr<CXX, REWRITABLE>(target, delitem_str, delslice_str, slice, NULL, &rewrite_args);

        if (!rewrite_args.out_success) {
            rewriter.reset(NULL);
        }
    } else {
        rtn = callItemOrSliceAttr<CXX, NOT_REWRITABLE>(target, delitem_str, delslice_str, slice, NULL, NULL);
    }

    if (rtn == NULL) {
        raiseExcHelper(TypeError, "'%s' object does not support item deletion", getTypeName(target));
    }

    Py_DECREF(rtn);

    if (rewriter.get())
        rewriter->commit();
}

void Box::delattr(BoxedString* attr, DelattrRewriteArgs* rewrite_args) {
    assert(attr->interned_state != SSTATE_NOT_INTERNED);
    if (cls->instancesHaveHCAttrs()) {
        // as soon as the hcls changes, the guard on hidden class won't pass.
        HCAttrs* attrs = getHCAttrsPtr();
        HiddenClass* hcls = attrs->hcls;

        if (hcls->type == HiddenClass::DICT_BACKED) {
            if (rewrite_args)
                assert(!rewrite_args->out_success);
            rewrite_args = NULL;
            Box* d = attrs->attr_list->attrs[0];
            assert(d);
            assert(attr->data()[attr->size()] == '\0');
            PyDict_DelItem(d, attr);
            checkAndThrowCAPIException();
            return;
        }

        assert(hcls->type == HiddenClass::NORMAL || hcls->type == HiddenClass::SINGLETON);

        // The order of attributes is pertained as delAttrToMakeHC constructs
        // the new HiddenClass by invoking getOrMakeChild in the prevous order
        // of remaining attributes
        int num_attrs = hcls->attributeArraySize();
        int offset = hcls->getOffset(attr);
        assert(offset >= 0);
        Box* removed_object = attrs->attr_list->attrs[offset];
        Box** start = attrs->attr_list->attrs;
        memmove(start + offset, start + offset + 1, (num_attrs - offset - 1) * sizeof(Box*));

        if (hcls->type == HiddenClass::NORMAL) {
            HiddenClass* new_hcls = hcls->delAttrToMakeHC(attr);
            attrs->hcls = new_hcls;
        } else {
            assert(hcls->type == HiddenClass::SINGLETON);
            hcls->delAttribute(attr);
        }

        // guarantee the size of the attr_list equals the number of attrs
        int new_size = sizeof(HCAttrs::AttrList) + sizeof(Box*) * (num_attrs - 1);
        // TODO: we might want to free some of this memory eventually
        // attrs->attr_list = (HCAttrs::AttrList*)reallocAttrs(attrs->attr_list, num_attrs, new_size);

        Py_DECREF(removed_object);
        return;
    }

    if (cls->instancesHaveDictAttrs()) {
        BoxedDict* d = getDict();
        auto it = d->d.find(attr);
        assert(it != d->d.end());
        Box* key = it->first.value;
        Box* value = it->second;
        d->d.erase(it);
        Py_DECREF(key);
        Py_DECREF(value);
        return;
    }

    abort();
}

extern "C" void delattrGeneric(Box* obj, BoxedString* attr, DelattrRewriteArgs* rewrite_args) {
    // first check whether the deleting attribute is a descriptor
    Box* clsAttr = typeLookup(obj->cls, attr);
    if (clsAttr != NULL) {
        static BoxedString* delete_str = getStaticString("__delete__");
        Box* delAttr = typeLookup(static_cast<BoxedClass*>(clsAttr->cls), delete_str);

        if (delAttr != NULL) {
            Box* rtn = runtimeCallInternal<CXX, NOT_REWRITABLE>(delAttr, NULL, ArgPassSpec(2), clsAttr, obj, NULL, NULL,
                                                                NULL);
            Py_DECREF(rtn);
            return;
        }
    }

    // check if the attribute is in the instance's __dict__
    Box* attrVal = obj->getattr(attr);
    if (attrVal != NULL) {
        obj->delattr(attr, NULL);
    } else {
        // the exception cpthon throws is different when the class contains the attribute
        if (clsAttr != NULL) {
            assert(attr->data()[attr->size()] == '\0');
            raiseExcHelper(AttributeError, "'%s' object attribute '%s' is read-only", getTypeName(obj), attr->data());
        } else {
            assert(attr->data()[attr->size()] == '\0');
            raiseAttributeError(obj, attr->s());
        }
    }

    // TODO this should be in type_setattro
    if (PyType_Check(obj)) {
        BoxedClass* self = static_cast<BoxedClass*>(obj);

        static BoxedString* base_str = getStaticString("__base__");
        if (attr->s() == "__base__" && self->getattr(base_str))
            raiseExcHelper(TypeError, "readonly attribute");

        assert(attr->data()[attr->size()] == '\0');
        bool touched_slot = update_slot(self, attr->data());
        if (touched_slot) {
            rewrite_args = NULL;
            REWRITE_ABORTED("");
        }
    }

    // Extra "use" of rewrite_args to make the compiler happy:
    (void)rewrite_args;
}

extern "C" void delattrInternal(Box* obj, BoxedString* attr, DelattrRewriteArgs* rewrite_args) {
    static BoxedString* delattr_str = getStaticString("__delattr__");

    // TODO: need to pass rewrite args to typeLookup to have it check guards.
    // But we currently don't rewrite delattr anyway.
    rewrite_args = NULL;

    Box* delAttr = typeLookup(obj->cls, delattr_str);

    if (delAttr != NULL) {
        KEEP_ALIVE(delAttr);

        Box* rtn = runtimeCallInternal<CXX, NOT_REWRITABLE>(delAttr, NULL, ArgPassSpec(2), obj, attr, NULL, NULL, NULL);
        Py_DECREF(rtn);
        return;
    }

    delattrGeneric(obj, attr, rewrite_args);
}

// del target.attr
extern "C" void delattr(Box* obj, BoxedString* attr) {
    STAT_TIMER(t0, "us_timer_slowpath_delattr", 10);

    static StatCounter slowpath_delattr("slowpath_delattr");
    slowpath_delattr.log();

    if (obj->cls == type_cls) {
        BoxedClass* cobj = static_cast<BoxedClass*>(obj);
        if (!cobj->is_user_defined) {
            raiseExcHelper(TypeError, "can't set attributes of built-in/extension type '%s'\n", getNameOfClass(cobj));
        }
    }


    delattrInternal(obj, attr, NULL);
}

extern "C" Box* createBoxedIterWrapper(Box* o) {
    return new BoxedIterWrapper(o);
}

extern "C" Box* createBoxedIterWrapperIfNeeded(Box* o) {
    STAT_TIMER(t0, "us_timer_slowpath_createBoxedIterWrapperIfNeeded", 10);

    std::unique_ptr<Rewriter> rewriter(Rewriter::createRewriter(
        __builtin_extract_return_addr(__builtin_return_address(0)), 1, "createBoxedIterWrapperIfNeeded"));

    static BoxedString* hasnext_str = getStaticString("__hasnext__");

    if (rewriter.get()) {
        RewriterVar* r_o = rewriter->getArg(0)->setType(RefType::BORROWED);
        RewriterVar* r_cls = r_o->getAttr(offsetof(Box, cls));
        GetattrRewriteArgs rewrite_args(rewriter.get(), r_cls, rewriter->getReturnDestination());
        Box* r = typeLookup(o->cls, hasnext_str, &rewrite_args);
        if (!rewrite_args.isSuccessful()) {
            rewriter.reset(NULL);
        } else if (r) {
            RewriterVar* rtn = rewrite_args.getReturn(ReturnConvention::HAS_RETURN);
            rtn->addGuard((uint64_t)r);
            rewriter->commitReturning(r_o);
            return incref(o);
        } else /* if (!r) */ {
            rewrite_args.assertReturnConvention(ReturnConvention::NO_RETURN);
            RewriterVar* var = rewriter.get()->call(true, (void*)createBoxedIterWrapper, rewriter->getArg(0));
            var->setType(RefType::OWNED);
            rewriter->commitReturning(var);
            return createBoxedIterWrapper(o);
        }
    }

    // assert((typeLookup(o->cls, hasnext_str) == NULL) == (o->cls->tpp_hasnext == object_cls->tpp_hasnext));
    if (o->cls->tpp_hasnext == object_cls->tpp_hasnext)
        return new BoxedIterWrapper(o);
    return incref(o);
}

extern "C" Box* getPystonIter(Box* o) {
    STAT_TIMER(t0, "us_timer_slowpath_getPystonIter", 10);

    Box* r = getiter(o);
    // assert((typeLookup(r->cls, hasnext_str) == NULL) == (r->cls->tpp_hasnext == object_cls->tpp_hasnext));
    if (r->cls->tpp_hasnext == object_cls->tpp_hasnext)
        return new BoxedIterWrapper(autoDecref(r));
    return r;
}

extern "C" Box* getiterHelper(Box* o) {
    if (PySequence_Check(o))
        return new BoxedSeqIter(o, 0);
    raiseExcHelper(TypeError, "'%s' object is not iterable", getTypeName(o));
}

Box* getiter(Box* o) {
    // TODO add rewriting to this?  probably want to try to avoid this path though
    BoxedClass* type = o->cls;
    Box* r = NULL;
    if (PyType_HasFeature(type, Py_TPFLAGS_HAVE_ITER) && type->tp_iter != slot_tp_iter && type->tp_iter) {
        r = type->tp_iter(o);
        if (!r && PyErr_Occurred())
            throwCAPIException();
    } else {
        r = type->callIterIC(o);
    }
    if (r) {
        if (!PyIter_Check(r)) {
            AUTO_DECREF(r);
            raiseExcHelper(TypeError, "iter() returned non-iterator of type '%s'", r->cls->tp_name);
        }
        return r;
    }
    return getiterHelper(o);
}

void assertValidSlotIdentifier(Box* s) {
    // Ported from `valid_identifier` in cpython

    unsigned char* p;
    size_t i, n;

    if (!PyString_Check(s)) {
        raiseExcHelper(TypeError, "__slots__ items must be strings, not '%.200s'", Py_TYPE(s)->tp_name);
    }
    p = (unsigned char*)PyString_AS_STRING(s);
    n = PyString_GET_SIZE(s);
    /* We must reject an empty name.  As a hack, we bump the
       length to 1 so that the loop will balk on the trailing \0. */
    if (n == 0)
        n = 1;
    for (i = 0; i < n; i++, p++) {
        if (!(i == 0 ? isalpha(*p) : isalnum(*p)) && *p != '_') {
            raiseExcHelper(TypeError, "__slots__ must be identifiers");
        }
    }
}

Box* _typeNew(BoxedClass* metatype, BoxedString* name, BoxedTuple* bases, BoxedDict* attr_dict) {
    if (bases->size() == 0) {
        bases = BoxedTuple::create({ object_cls });
    } else {
        Py_INCREF(bases);
    }

    // Ported from CPython:
    int nbases = bases->size();
    BoxedClass* winner = metatype;

    AUTO_DECREF(bases);

    for (auto tmp : *bases) {
        auto tmptype = tmp->cls;
        if (tmptype == classobj_cls)
            continue;
        if (isSubclass(winner, tmptype))
            continue;
        if (isSubclass(tmptype, winner)) {
            winner = tmptype;
            continue;
        }
        raiseExcHelper(TypeError, "metaclass conflict: "
                                  "the metaclass of a derived class "
                                  "must be a (non-strict) subclass "
                                  "of the metaclasses of all its bases");
    }

    static BoxedString* new_box = getStaticString(new_str.c_str());
    if (winner != metatype) {
        if (winner->tp_new != type_new) {
            CallattrFlags callattr_flags
                = {.cls_only = false, .null_on_nonexistent = false, .argspec = ArgPassSpec(4) };
            Box* args[1] = { (Box*)attr_dict };
            return callattr(winner, new_box, callattr_flags, winner, name, bases, args, NULL);
        }
        metatype = winner;
    }

    BoxedClass* base = best_base(bases);
    checkAndThrowCAPIException();
    assert(base);
    if (!PyType_HasFeature(base, Py_TPFLAGS_BASETYPE))
        raiseExcHelper(TypeError, "type '%.100s' is not an acceptable base type", base->tp_name);
    assert(PyType_Check(base));

    // Handle slots
    static BoxedString* slots_str = getStaticString("__slots__");
    Box* boxedSlots = PyDict_GetItem(attr_dict, slots_str);
    int add_dict = 0;
    int add_weak = 0;
    bool may_add_dict = base->tp_dictoffset == 0 && base->attrs_offset == 0;
    bool may_add_weak = base->tp_weaklistoffset == 0 && base->tp_itemsize == 0;
    std::vector<Box*> final_slot_names; // owned
    if (boxedSlots == NULL) {
        if (may_add_dict) {
            add_dict++;
        }
        if (may_add_weak) {
            add_weak++;
        }
    } else {
        // Get a pointer to an array of slots.
        std::vector<Box*> slots;
        if (PyString_Check(boxedSlots) || PyUnicode_Check(boxedSlots)) {
            slots = { incref(boxedSlots) };
        } else {
            BoxedTuple* tuple = static_cast<BoxedTuple*>(PySequence_Tuple(boxedSlots));
            if (!tuple)
                throwCAPIException();
            slots = std::vector<Box*>(tuple->size());
            for (size_t i = 0; i < tuple->size(); i++) {
                slots[i] = incref((*tuple)[i]);
            }
            Py_DECREF(tuple);
        }
        AUTO_DECREF_ARRAY(&slots[0], slots.size());

        // Check that slots are allowed
        if (slots.size() > 0 && base->tp_itemsize != 0) {
            raiseExcHelper(TypeError, "nonempty __slots__ not supported for subtype of '%s'", base->tp_name);
        }

        // Convert unicode -> string
        for (size_t i = 0; i < slots.size(); i++) {
            Box* slot_name = slots[i];
            if (PyUnicode_Check(slot_name)) {
                slots[i] = _PyUnicode_AsDefaultEncodedString(slot_name, NULL);
                if (!slots[i])
                    throwCAPIException();
                Py_DECREF(slot_name);
            }
        }

        // Check for valid slot names and two special cases
        // Mangle and sort names
        for (size_t i = 0; i < slots.size(); i++) {
            Box* tmp = slots[i];
            assertValidSlotIdentifier(tmp);
            assert(PyString_Check(tmp));
            if (static_cast<BoxedString*>(tmp)->s() == "__dict__") {
                if (!may_add_dict || add_dict) {
                    raiseExcHelper(TypeError, "__dict__ slot disallowed: "
                                              "we already got one");
                }
                add_dict++;
                continue;
            } else if (static_cast<BoxedString*>(tmp)->s() == "__weakref__") {
                if (!may_add_weak || add_weak) {
                    raiseExcHelper(TypeError, "__weakref__ slot disallowed: "
                                              "either we already got one, "
                                              "or __itemsize__ != 0");
                }
                add_weak++;
                continue;
            }

            assert(tmp->cls == str_cls);
            final_slot_names.push_back(mangleNameBoxedString(static_cast<BoxedString*>(tmp), name));
        }

        std::sort(final_slot_names.begin(), final_slot_names.end(), PyLt());

        if (nbases > 1 && ((may_add_dict && !add_dict) || (may_add_weak && !add_weak))) {
            for (size_t i = 0; i < nbases; i++) {
                Box* tmp = PyTuple_GET_ITEM(bases, i);
                if (tmp == (PyObject*)base)
                    continue; /* Skip primary base */
                if (PyClass_Check(tmp)) {
                    /* Classic base class provides both */
                    if (may_add_dict && !add_dict)
                        add_dict++;
                    if (may_add_weak && !add_weak)
                        add_weak++;
                    break;
                }
                assert(PyType_Check(tmp));
                BoxedClass* tmptype = static_cast<BoxedClass*>(tmp);
                if (may_add_dict && !add_dict && (tmptype->tp_dictoffset != 0 || tmptype->attrs_offset != 0))
                    add_dict++;
                if (may_add_weak && !add_weak && tmptype->tp_weaklistoffset != 0)
                    add_weak++;
                if (may_add_dict && !add_dict)
                    continue;
                if (may_add_weak && !add_weak)
                    continue;
                /* Nothing more to check */
                break;
            }
        }
    }

    int attrs_offset = base->attrs_offset;
    int dict_offset = base->tp_dictoffset;
    int weaklist_offset = 0;
    int basic_size = 0;

    int cur_offset = base->tp_basicsize + sizeof(Box*) * final_slot_names.size();
    if (add_dict) {
        // CPython would set tp_dictoffset here, but we want to use attrs instead.
        if (base->tp_itemsize) {
            // A negative value indicates an offset from the end of the object
            attrs_offset = -(long)sizeof(HCAttrs);
        } else {
            attrs_offset = cur_offset;
        }
        cur_offset += sizeof(HCAttrs);
    }
    if (add_weak) {
        assert(!base->tp_itemsize);
        weaklist_offset = cur_offset;
        cur_offset += sizeof(Box*);
    }
    basic_size = cur_offset;

    // from cpython:
    /* Special-case __new__: if it's a plain function,
               make it a static function */
    Box* tmp = PyDict_GetItemString(attr_dict, "__new__");
    if (tmp != NULL && PyFunction_Check(tmp)) {
        tmp = PyStaticMethod_New(tmp);
        if (tmp == NULL)
            throwCAPIException();
        PyDict_SetItemString(attr_dict, "__new__", tmp);
        Py_DECREF(tmp);
    }

    size_t total_slots = final_slot_names.size();
    /*+ (base->tp_flags & Py_TPFLAGS_HEAPTYPE ? static_cast<BoxedHeapClass*>(base)->nslots() : 0);*/
    BoxedHeapClass* made = BoxedHeapClass::create(metatype, base, attrs_offset, weaklist_offset, basic_size, true, name,
                                                  bases, total_slots);
    made->tp_dictoffset = dict_offset;

    // XXX Hack: the classes vector lists all classes that have untracked references to them.
    // This is pretty much any class created in C code, since the C code will tend to hold on
    // to a reference to the created class.  So in the BoxedClass constructor we add the new class to
    // "classes", which will cause the class to get decref'd at the end.
    // But for classes created from Python, we don't have this extra untracked reference.
    // Rather than fix up the plumbing for now, just reach into the other system and remove this
    // class from the list.
    // This hack also exists in BoxedHeapClass::create
    RELEASE_ASSERT(classes.back() == made, "");
    classes.pop_back();

    if (boxedSlots) {
        // Set ht_slots
        BoxedTuple* slotsTuple = BoxedTuple::create(final_slot_names.size());
        for (size_t i = 0; i < final_slot_names.size(); i++)
            (*slotsTuple)[i] = final_slot_names[i]; // transfer ref
        assert(made->tp_flags & Py_TPFLAGS_HEAPTYPE);
        assert(!static_cast<BoxedHeapClass*>(made)->ht_slots);
        static_cast<BoxedHeapClass*>(made)->ht_slots = slotsTuple;

        PyMemberDef* mp = PyHeapType_GET_MEMBERS(made);

        // Add the member descriptors
        size_t offset = base->tp_basicsize;
        for (size_t i = 0; i < final_slot_names.size(); i++) {
            made->giveAttr(static_cast<BoxedString*>(slotsTuple->elts[i])->data(),
                           new BoxedMemberDescriptor(BoxedMemberDescriptor::OBJECT_EX, offset, false /* read only */));

            mp[i].name = static_cast<BoxedString*>(slotsTuple->elts[i])->data();
            mp[i].type = T_OBJECT_EX;
            mp[i].offset = offset;

            offset += sizeof(Box*);
        }
    } else {
        assert(!final_slot_names.size()); // would need to decref them here
    }

    if (made->instancesHaveHCAttrs() || made->instancesHaveDictAttrs()) {
        static BoxedString* dict_str = getStaticString("__dict__");
        made->setattr(dict_str, dict_descr, NULL);
    }

    bool are_all_dict_keys_strs = true;
    for (const auto& p : *attr_dict) {
        if (p.first->cls != str_cls) {
            are_all_dict_keys_strs = false;
            break;
        }
    }
    if (are_all_dict_keys_strs) {
        for (const auto& p : *attr_dict) {
            BoxedString* s = static_cast<BoxedString*>(p.first);
            Py_INCREF(s);
            internStringMortalInplace(s);
            made->setattr(s, p.second, NULL);
            Py_DECREF(s);
        }
    } else {
        Box* copy = PyDict_Copy(attr_dict);
        RELEASE_ASSERT(copy, "");
        made->setDictBacked(copy);
    }

    static BoxedString* module_str = getStaticString("__module__");
    if (!made->hasattr(module_str)) {
        Box* gl = getGlobalsDict();
        static BoxedString* name_str = getStaticString("__name__");
        Box* attr = PyDict_GetItem(gl, name_str);
        if (attr)
            made->setattr(module_str, attr, NULL);
    }

    static BoxedString* doc_str = getStaticString("__doc__");
    if (!made->hasattr(doc_str))
        made->setattr(doc_str, None, NULL);

    made->tp_new = base->tp_new;

    fixup_slot_dispatchers(made);

    made->tp_alloc = PyType_GenericAlloc;

    return made;
}

// Analogous to CPython's type_new.
// This is assigned directly to type_cls's (PyType_Type's) tp_new slot and skips
// doing an attribute lookup for __new__.
//
// We need this to support some edge cases. For example, in ctypes, we have a function:
// PyCSimpleType_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
// {
//     ...
//     result = (PyTypeObject *)PyType_Type.tp_new(type, args, kwds);
//     ...
// }
//
// Assigned to the tp_new of PyCSimpleType, a metaclass. By calling PyType_Type.tp_new,
// we don't want to do an attribute lookup for __new__, because that would end up calling
// the tp_new of the subclass PyCSimpleType (PyCSimpleType_new again) and end up in a loop.
Box* type_new(BoxedClass* metatype, Box* args, Box* kwds) noexcept {
    PyObject* name, *bases, *dict;
    static const char* kwlist[] = { "name", "bases", "dict", 0 };

    // Copied from CPython.
    assert(args != NULL && PyTuple_Check(args));
    assert(kwds == NULL || PyDict_Check(kwds));

    /* Special case: type(x) should return x->ob_type */
    {
        const Py_ssize_t nargs = PyTuple_GET_SIZE(args);
        const Py_ssize_t nkwds = kwds == NULL ? 0 : PyDict_Size(kwds);

        if (PyType_CheckExact(metatype) && nargs == 1 && nkwds == 0) {
            PyObject* x = PyTuple_GET_ITEM(args, 0);
            Py_INCREF(Py_TYPE(x));
            return (PyObject*)Py_TYPE(x);
        }

        /* SF bug 475327 -- if that didn't trigger, we need 3
           arguments. but PyArg_ParseTupleAndKeywords below may give
           a msg saying type() needs exactly 3. */
        if (nargs + nkwds != 3) {
            PyErr_SetString(PyExc_TypeError, "type() takes 1 or 3 arguments");
            return NULL;
        }
    }

    // Check arguments: (name, bases, dict)
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "SO!O!:type", const_cast<char**>(kwlist), &name, &PyTuple_Type, &bases,
                                     &PyDict_Type, &dict))
        return NULL;

    try {
        RELEASE_ASSERT(name->cls == str_cls, "");
        RELEASE_ASSERT(bases->cls == tuple_cls, "");
        RELEASE_ASSERT(dict->cls == dict_cls, "");

        return _typeNew(metatype, static_cast<BoxedString*>(name), static_cast<BoxedTuple*>(bases),
                        static_cast<BoxedDict*>(dict));
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

// This is the function we want uses of __new__ to call.
Box* typeNewGeneric(Box* _cls, Box* arg1, Box* arg2, Box** _args) {
    STAT_TIMER(t0, "us_timer_typeNew", 10);

    Box* arg3 = _args[0];

    if (!PyType_Check(_cls))
        raiseExcHelper(TypeError, "type.__new__(X): X is not a type object (%s)", getTypeName(_cls));

    BoxedClass* metatype = static_cast<BoxedClass*>(_cls);
    if (!isSubclass(metatype, type_cls))
        raiseExcHelper(TypeError, "type.__new__(%s): %s is not a subtype of type", getNameOfClass(metatype),
                       getNameOfClass(metatype));

    if (arg2 == NULL) {
        assert(arg3 == NULL);
        return incref(arg1->cls);
    }

    RELEASE_ASSERT(PyDict_Check(arg3), "%s", getTypeName(arg3));
    BoxedDict* attr_dict = static_cast<BoxedDict*>(arg3);

    RELEASE_ASSERT(arg2->cls == tuple_cls, "");
    BoxedTuple* bases = static_cast<BoxedTuple*>(arg2);

    RELEASE_ASSERT(arg1->cls == str_cls, "");
    BoxedString* name = static_cast<BoxedString*>(arg1);

    return _typeNew(metatype, name, bases, attr_dict);
}

extern "C" void delGlobal(Box* globals, BoxedString* name) {
    if (globals->cls == module_cls) {
        BoxedModule* m = static_cast<BoxedModule*>(globals);
        if (!m->getattr(name)) {
            assert(name->data()[name->size()] == '\0');
            raiseExcHelper(NameError, "name '%s' is not defined", name->data());
        }
        m->delattr(name, NULL);
    } else {
        assert(globals->cls == dict_cls);
        BoxedDict* d = static_cast<BoxedDict*>(globals);

        auto it = d->d.find(name);
        assert(name->data()[name->size()] == '\0');
        assertNameDefined(it != d->d.end(), name->data(), NameError, false /* local_var_msg */);
        int r = PyDict_DelItem(d, name);
        if (r == -1)
            throwCAPIException();
    }
}

extern "C" Box* getGlobal(Box* globals, BoxedString* name) {
    STAT_TIMER(t0, "us_timer_slowpath_getglobal", 10);

    static StatCounter slowpath_getglobal("slowpath_getglobal");
    slowpath_getglobal.log();
    static StatCounter nopatch_getglobal("nopatch_getglobal");

    if (VERBOSITY() >= 2) {
#if !DISABLE_STATS
        std::string per_name_stat_name = "getglobal__" + std::string(name->s());
        uint64_t* counter = Stats::getStatCounter(per_name_stat_name);
        Stats::log(counter);
#endif
    }

    { /* anonymous scope to make sure destructors get run before we err out */
        std::unique_ptr<Rewriter> rewriter(
            Rewriter::createRewriter(__builtin_extract_return_addr(__builtin_return_address(0)), 3, "getGlobal"));

        Box* r;
        if (globals->cls == module_cls) {
            BoxedModule* m = static_cast<BoxedModule*>(globals);
            if (rewriter.get()) {
                RewriterVar* r_mod = rewriter->getArg(0);

                // Guard on it being a module rather than a dict
                // TODO is this guard necessary? I'm being conservative now, but I think we can just
                // insist that the type passed in is fixed for any given instance of a getGlobal call.
                r_mod->addAttrGuard(offsetof(Box, cls), (intptr_t)module_cls);

                GetattrRewriteArgs rewrite_args(rewriter.get(), r_mod, rewriter->getReturnDestination());
                r = m->getattr(name, &rewrite_args);
                if (!rewrite_args.isSuccessful()) {
                    rewriter.reset(NULL);
                } else {
                    rewrite_args.assertReturnConvention(r ? ReturnConvention::HAS_RETURN : ReturnConvention::NO_RETURN);
                }
                if (r) {
                    if (rewriter.get()) {
                        RewriterVar* r_rtn = rewrite_args.getReturn(ReturnConvention::HAS_RETURN);
                        rewriter->commitReturning(r_rtn);
                    }

                    assert(r->ob_refcnt > 0);
                    Py_INCREF(r);
                    return r;
                }
            } else {
                r = m->getattr(name);
                nopatch_getglobal.log();
                if (r) {
                    assert(r->ob_refcnt > 0);

                    Py_INCREF(r);
                    return r;
                }
            }
        } else {
            ASSERT(globals->cls == dict_cls, "%s", globals->cls->tp_name);
            BoxedDict* d = static_cast<BoxedDict*>(globals);

            rewriter.reset(NULL);
            REWRITE_ABORTED("Rewriting not implemented for getGlobals with a dict globals yet");

            auto it = d->d.find(name);
            if (it != d->d.end()) {
                assert(it->second->ob_refcnt > 0);
                Py_INCREF(it->second);
                return it->second;
            }
        }

        static StatCounter stat_builtins("getglobal_builtins");
        stat_builtins.log();

        Box* rtn;
        if (rewriter.get()) {
            RewriterVar* builtins = rewriter->loadConst((intptr_t)builtins_module, Location::any());
            GetattrRewriteArgs rewrite_args(rewriter.get(), builtins, rewriter->getReturnDestination());
            rewrite_args.obj_shape_guarded = true; // always builtin module
            rtn = builtins_module->getattr(name, &rewrite_args);

            if (!rewrite_args.isSuccessful())
                rewriter.reset(NULL);
            else if (rtn) {
                auto r_rtn = rewrite_args.getReturn(ReturnConvention::HAS_RETURN);
                rewriter->commitReturning(r_rtn);
            } else {
                rewrite_args.getReturn(); // just to make the asserts happy
                rewriter.reset(NULL);
            }
        } else {
            rtn = builtins_module->getattr(name);
        }

// XXX
#ifndef NDEBUG
        rewriter.release();
#endif

        if (rtn) {
            assert(rtn->ob_refcnt > 0);
            Py_INCREF(rtn);
            return rtn;
        }
    }

    assert(name->data()[name->size()] == '\0');
    raiseExcHelper(NameError, "global name '%s' is not defined", name->data());
}

extern "C" void setGlobal(Box* globals, BoxedString* name, STOLEN(Box*) value) {
    if (globals->cls == attrwrapper_cls) {
        globals = unwrapAttrWrapper(globals);
        RELEASE_ASSERT(globals->cls == module_cls, "%s", globals->cls->tp_name);
    }

    if (globals->cls == module_cls) {
        // Note: in optimized builds, this will be a tail call, which will
        // preserve the return address, letting the setattr() call rewrite itself.
        // XXX this isn't really safe in general, since the guards that led to this
        // path need to end up in the rewrite.  I think this is safe for now since
        // writing the module case won't accidentally work for the dict case, but
        // we should make all the entrypoints (the ones that look at the return address)
        // be noinline.
        setattr(static_cast<BoxedModule*>(globals), name, value);
    } else {
        RELEASE_ASSERT(globals->cls == dict_cls, "%s", globals->cls->tp_name);
        int r = PyDict_SetItem(globals, name, value);
        Py_DECREF(value);
        if (r == -1)
            throwCAPIException();
    }
}

extern "C" Box* importFrom(Box* _m, BoxedString* name) {
    STAT_TIMER(t0, "us_timer_importFrom", 10);

    Box* r = getattrInternal<CXX>(_m, name);
    if (r)
        return r;

    raiseExcHelper(ImportError, "cannot import name %s", name->c_str());
}

extern "C" Box* importStar(Box* _from_module, Box* to_globals) {
    STAT_TIMER(t0, "us_timer_importStar", 10);

    RELEASE_ASSERT(PyModule_Check(_from_module), "%s", _from_module->cls->tp_name);
    BoxedModule* from_module = static_cast<BoxedModule*>(_from_module);

    static BoxedString* all_str = getStaticString("__all__");
    Box* all = from_module->getattr(all_str);

    if (all) {
        KEEP_ALIVE(all);

        static BoxedString* getitem_str = getStaticString("__getitem__");
        Box* all_getitem = typeLookup(all->cls, getitem_str);
        if (!all_getitem)
            raiseExcHelper(TypeError, "'%s' object does not support indexing", getTypeName(all));

        KEEP_ALIVE(all_getitem);

        int idx = 0;
        while (true) {
            Box* attr_name;
            try {
                attr_name = runtimeCallInternal2<CXX, NOT_REWRITABLE>(all_getitem, NULL, ArgPassSpec(2), all,
                                                                      autoDecref(boxInt(idx)));
            } catch (ExcInfo e) {
                if (e.matches(IndexError)) {
                    e.clear();
                    break;
                }
                throw e;
            }
            idx++;

            AUTO_DECREF(attr_name);
            attr_name = coerceUnicodeToStr<CXX>(attr_name);

            if (attr_name->cls != str_cls) {
                AUTO_DECREF(attr_name);
                raiseExcHelper(TypeError, "attribute name must be string, not '%s'", getTypeName(attr_name));
            }

            BoxedString* casted_attr_name = static_cast<BoxedString*>(attr_name);
            internStringMortalInplace(casted_attr_name);
            AUTO_DECREF(casted_attr_name);
            Box* attr_value = from_module->getattr(casted_attr_name);

            if (!attr_value)
                raiseExcHelper(AttributeError, "'module' object has no attribute '%s'", casted_attr_name->data());
            setGlobal(to_globals, casted_attr_name, incref(attr_value));
        }
        return incref(None);
    }

    HCAttrs* module_attrs = from_module->getHCAttrsPtr();
    for (auto& p : module_attrs->hcls->getStrAttrOffsets()) {
        if (p.first->data()[0] == '_')
            continue;

        setGlobal(to_globals, p.first, incref(module_attrs->attr_list->attrs[p.second]));
    }

    return incref(None);
}

// TODO Make these fast, do inline caches and stuff

extern "C" void boxedLocalsSet(Box* boxedLocals, BoxedString* attr, Box* val) {
    setitem(boxedLocals, attr, val);
}

extern "C" Box* boxedLocalsGet(Box* boxedLocals, BoxedString* attr, Box* globals) {
    assert(boxedLocals != NULL);

    if (boxedLocals->cls == dict_cls) {
        auto& d = static_cast<BoxedDict*>(boxedLocals)->d;
        auto it = d.find(attr);
        if (it != d.end()) {
            Box* value = it->second;
            return incref(value);
        }
    } else {
        try {
            return getitem(boxedLocals, attr);
        } catch (ExcInfo e) {
            // TODO should check the exact semantic here but it's something like:
            // If it throws a KeyError, then the variable doesn't exist so move on
            // and check the globals (below); otherwise, just propogate the exception.
            if (!isSubclass(e.value->cls, KeyError)) {
                throw e;
            }
            e.clear();
        }
    }

    // TODO exception name?
    return getGlobal(globals, attr);
}

extern "C" void boxedLocalsDel(Box* boxedLocals, BoxedString* attr) {
    assert(boxedLocals != NULL);
    RELEASE_ASSERT(boxedLocals->cls == dict_cls, "we don't support non-dict here yet");
    auto& d = static_cast<BoxedDict*>(boxedLocals)->d;
    auto it = d.find(attr);
    if (it == d.end()) {
        assert(attr->data()[attr->size()] == '\0');
        assertNameDefined(0, attr->data(), NameError, false /* local_var_msg */);
    }
    Box* key = it->first.value;
    Box* value = it->second;
    d.erase(it);
    Py_DECREF(key);
    Py_DECREF(value);
}

extern "C" void checkRefs(Box* b) {
    RELEASE_ASSERT(b->ob_refcnt >= 0, "%ld", b->ob_refcnt);
}
extern "C" Box* assertAlive(Box* b) {
    RELEASE_ASSERT(b->ob_refcnt > 0, "%ld", b->ob_refcnt);
    return b;
}
}
