// This file is originally from CPython 2.7, with modifications for Pyston

/***********************************************************************/
/* Implements the string (as opposed to unicode) version of the
   built-in formatters for string, int, float.  That is, the versions
   of int.__format__, etc., that take and return string objects */

#include "Python.h"
#include "../Objects/stringlib/stringdefs.h"

#define FORMAT_STRING  _PyBytes_FormatAdvanced
#define FORMAT_LONG    _PyLong_FormatAdvanced
#define FORMAT_INT     _PyInt_FormatAdvanced
#define FORMAT_FLOAT   _PyFloat_FormatAdvanced
#ifndef WITHOUT_COMPLEX
#define FORMAT_COMPLEX _PyComplex_FormatAdvanced
#endif

// Pyston change: had to change this path
#include "../Objects/stringlib/formatter.h"

