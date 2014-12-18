// Pyston change: this is just a shim to import stuff from stringlib/

#include "Python.h"

#include "stringlib/stringdefs.h"
#include "stringlib/string_format.h"

#define _Py_InsertThousandsGrouping _PyString_InsertThousandsGrouping
#include "stringlib/localeutil.h"
