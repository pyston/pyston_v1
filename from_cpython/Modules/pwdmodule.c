
/* UNIX password file access module */

#include "Python.h"
#include "structseq.h"
#include "posixmodule.h"

#include <pwd.h>

static PyStructSequence_Field struct_pwd_type_fields[] = {
    {"pw_name", "user name"},
    {"pw_passwd", "password"},
    {"pw_uid", "user id"},
    {"pw_gid", "group id"},
    {"pw_gecos", "real name"},
    {"pw_dir", "home directory"},
    {"pw_shell", "shell program"},
    {0}
};

PyDoc_STRVAR(struct_passwd__doc__,
"pwd.struct_passwd: Results from getpw*() routines.\n\n\
This object may be accessed either as a tuple of\n\
  (pw_name,pw_passwd,pw_uid,pw_gid,pw_gecos,pw_dir,pw_shell)\n\
or via the object attributes as named in the above tuple.");

static PyStructSequence_Desc struct_pwd_type_desc = {
    "pwd.struct_passwd",
    struct_passwd__doc__,
    struct_pwd_type_fields,
    7,
};

PyDoc_STRVAR(pwd__doc__,
"This module provides access to the Unix password database.\n\
It is available on all Unix versions.\n\
\n\
Password database entries are reported as 7-tuples containing the following\n\
items from the password database (see `<pwd.h>'), in order:\n\
pw_name, pw_passwd, pw_uid, pw_gid, pw_gecos, pw_dir, pw_shell.\n\
The uid and gid items are integers, all others are strings. An\n\
exception is raised if the entry asked for cannot be found.");


static int initialized;
static PyTypeObject StructPwdType;

static void
sets(PyObject *v, int i, char* val)
{
  if (val)
      PyStructSequence_SET_ITEM(v, i, PyString_FromString(val));
  else {
      PyStructSequence_SET_ITEM(v, i, Py_None);
      Py_INCREF(Py_None);
  }
}

static PyObject *
mkpwent(struct passwd *p)
{
    int setIndex = 0;
    PyObject *v = PyStructSequence_New(&StructPwdType);
    if (v == NULL)
        return NULL;

#define SETI(i,val) PyStructSequence_SET_ITEM(v, i, PyInt_FromLong((long) val))
#define SETS(i,val) sets(v, i, val)

    SETS(setIndex++, p->pw_name);
#ifdef __VMS
    SETS(setIndex++, "");
#else
    SETS(setIndex++, p->pw_passwd);
#endif
    PyStructSequence_SET_ITEM(v, setIndex++, _PyInt_FromUid(p->pw_uid));
    PyStructSequence_SET_ITEM(v, setIndex++, _PyInt_FromGid(p->pw_gid));
#ifdef __VMS
    SETS(setIndex++, "");
#else
    SETS(setIndex++, p->pw_gecos);
#endif
    SETS(setIndex++, p->pw_dir);
    SETS(setIndex++, p->pw_shell);

#undef SETS
#undef SETI

    if (PyErr_Occurred()) {
        Py_XDECREF(v);
        return NULL;
    }

    return v;
}

PyDoc_STRVAR(pwd_getpwuid__doc__,
"getpwuid(uid) -> (pw_name,pw_passwd,pw_uid,\n\
                  pw_gid,pw_gecos,pw_dir,pw_shell)\n\
Return the password database entry for the given numeric user ID.\n\
See help(pwd) for more on password database entries.");

static PyObject *
pwd_getpwuid(PyObject *self, PyObject *args)
{
    uid_t uid;
    struct passwd *p;
    if (!PyArg_ParseTuple(args, "O&:getpwuid", _Py_Uid_Converter, &uid)) {
        if (PyErr_ExceptionMatches(PyExc_OverflowError))
            PyErr_Format(PyExc_KeyError,
                         "getpwuid(): uid not found");
        return NULL;
    }
    if ((p = getpwuid(uid)) == NULL) {
        if (uid < 0)
            PyErr_Format(PyExc_KeyError,
                         "getpwuid(): uid not found: %ld", (long)uid);
        else
            PyErr_Format(PyExc_KeyError,
                         "getpwuid(): uid not found: %lu", (unsigned long)uid);
        return NULL;
    }
    return mkpwent(p);
}

PyDoc_STRVAR(pwd_getpwnam__doc__,
"getpwnam(name) -> (pw_name,pw_passwd,pw_uid,\n\
                    pw_gid,pw_gecos,pw_dir,pw_shell)\n\
Return the password database entry for the given user name.\n\
See help(pwd) for more on password database entries.");

static PyObject *
pwd_getpwnam(PyObject *self, PyObject *args)
{
    char *name;
    struct passwd *p;
    if (!PyArg_ParseTuple(args, "s:getpwnam", &name))
        return NULL;
    if ((p = getpwnam(name)) == NULL) {
        PyErr_Format(PyExc_KeyError,
                     "getpwnam(): name not found: %s", name);
        return NULL;
    }
    return mkpwent(p);
}

#ifdef HAVE_GETPWENT
PyDoc_STRVAR(pwd_getpwall__doc__,
"getpwall() -> list_of_entries\n\
Return a list of all available password database entries, \
in arbitrary order.\n\
See help(pwd) for more on password database entries.");

static PyObject *
pwd_getpwall(PyObject *self)
{
    PyObject *d;
    struct passwd *p;
    if ((d = PyList_New(0)) == NULL)
        return NULL;
#if defined(PYOS_OS2) && defined(PYCC_GCC)
    if ((p = getpwuid(0)) != NULL) {
#else
    setpwent();
    while ((p = getpwent()) != NULL) {
#endif
        PyObject *v = mkpwent(p);
        if (v == NULL || PyList_Append(d, v) != 0) {
            Py_XDECREF(v);
            Py_DECREF(d);
            endpwent();
            return NULL;
        }
        Py_DECREF(v);
    }
    endpwent();
    return d;
}
#endif

static PyMethodDef pwd_methods[] = {
    {"getpwuid",        pwd_getpwuid, METH_VARARGS, pwd_getpwuid__doc__},
    {"getpwnam",        pwd_getpwnam, METH_VARARGS, pwd_getpwnam__doc__},
#ifdef HAVE_GETPWENT
    {"getpwall",        (PyCFunction)pwd_getpwall,
        METH_NOARGS,  pwd_getpwall__doc__},
#endif
    {NULL,              NULL}           /* sentinel */
};

PyMODINIT_FUNC
initpwd(void)
{
    PyObject *m;
    m = Py_InitModule3("pwd", pwd_methods, pwd__doc__);
    if (m == NULL)
        return;

    if (!initialized)
        PyStructSequence_InitType(&StructPwdType,
                                  &struct_pwd_type_desc);
    Py_INCREF((PyObject *) &StructPwdType);
    PyModule_AddObject(m, "struct_passwd", (PyObject *) &StructPwdType);
    /* And for b/w compatibility (this was defined by mistake): */
    Py_INCREF((PyObject *) &StructPwdType);
    PyModule_AddObject(m, "struct_pwent", (PyObject *) &StructPwdType);
    initialized = 1;
}
