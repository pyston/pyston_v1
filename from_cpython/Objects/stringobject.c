// Pyston change: this is just a shim to import stuff from stringlib/

#include "Python.h"

#include "stringlib/stringdefs.h"
#include "stringlib/string_format.h"

#define _Py_InsertThousandsGrouping _PyString_InsertThousandsGrouping
#include "stringlib/localeutil.h"

// do_string_format needs to be declared as a static function, since it's used by both stringobject.c
// and unicodeobject.c.  We want to access it from str.cpp, though, so just use this little forwarding
// function.
// We could also potentially have tried to modifie string_format.h to choose whether to mark the function
// as static or not.
PyObject * _do_string_format(PyObject *self, PyObject *args, PyObject *kwargs) {
    return do_string_format(self, args, kwargs);
}
