// Copyright (c) 2014 Dropbox, Inc.
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

#include "runtime/types.h"

namespace pyston {

// capi stuff

static std::string unicode_default_encoding = "ascii";
extern "C" const char* PyUnicode_GetDefaultEncoding(void) {
    return unicode_default_encoding.c_str();
}

extern "C" int PyUnicode_SetDefaultEncoding(const char* encoding) {
    unicode_default_encoding = encoding;
    return 0;
}

extern "C" int PyUnicode_ClearFreeList() {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_FromUnicode(const Py_UNICODE* u, Py_ssize_t size) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_FromStringAndSize(const char* u, Py_ssize_t size) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_FromString(const char* u) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_FromFormat(const char* format, ...) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_FromFormatV(const char* format, va_list vargs) {
    Py_FatalError("unimplemented");
}

extern "C" Py_UNICODE* PyUnicode_AsUnicode(PyObject* unicode) {
    Py_FatalError("unimplemented");
}

extern "C" Py_ssize_t PyUnicode_GetSize(PyObject* unicode) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_FromEncodedObject(PyObject* obj, const char* encoding, const char* errors) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_FromObject(PyObject* obj) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_FromWideChar(const wchar_t* w, Py_ssize_t size) {
    Py_FatalError("unimplemented");
}

extern "C" Py_ssize_t PyUnicode_AsWideChar(PyUnicodeObject* unicode, wchar_t* w, Py_ssize_t size) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_Decode(const char* s, Py_ssize_t size, const char* encoding, const char* errors) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_Encode(const Py_UNICODE* s, Py_ssize_t size, const char* encoding, const char* errors) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_AsEncodedObject(PyObject* unicode, const char* encoding, const char* errors) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_AsEncodedString(PyObject* unicode, const char* encoding, const char* errors) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_DecodeUTF8(const char* s, Py_ssize_t size, const char* errors) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_DecodeUTF8Stateful(const char* s, Py_ssize_t size, const char* errors,
                                                  Py_ssize_t* consumed) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_EncodeUTF8(const Py_UNICODE* s, Py_ssize_t size, const char* errors) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_AsUTF8String(PyObject* unicode) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_DecodeUTF32(const char* s, Py_ssize_t size, const char* errors, int* byteorder) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_DecodeUTF32Stateful(const char* s, Py_ssize_t size, const char* errors, int* byteorder,
                                                   Py_ssize_t* consumed) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_EncodeUTF32(const Py_UNICODE* s, Py_ssize_t size, const char* errors, int byteorder) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_AsUTF32String(PyObject* unicode) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_DecodeUTF16(const char* s, Py_ssize_t size, const char* errors, int* byteorder) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_DecodeUTF16Stateful(const char* s, Py_ssize_t size, const char* errors, int* byteorder,
                                                   Py_ssize_t* consumed) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_EncodeUTF16(const Py_UNICODE* s, Py_ssize_t size, const char* errors, int byteorder) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_AsUTF16String(PyObject* unicode) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_DecodeUTF7(const char* s, Py_ssize_t size, const char* errors) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_DecodeUTF7Stateful(const char* s, Py_ssize_t size, const char* errors,
                                                  Py_ssize_t* consumed) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_EncodeUTF7(const Py_UNICODE* s, Py_ssize_t size, int base64SetO, int base64WhiteSpace,
                                          const char* errors) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_DecodeUnicodeEscape(const char* s, Py_ssize_t size, const char* errors) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_EncodeUnicodeEscape(const Py_UNICODE* s, Py_ssize_t size) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_AsUnicodeEscapeString(PyObject* unicode) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_DecodeRawUnicodeEscape(const char* s, Py_ssize_t size, const char* errors) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_EncodeRawUnicodeEscape(const Py_UNICODE* s, Py_ssize_t size) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_AsRawUnicodeEscapeString(PyObject* unicode) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_DecodeLatin1(const char* s, Py_ssize_t size, const char* errors) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_EncodeLatin1(const Py_UNICODE* s, Py_ssize_t size, const char* errors) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_AsLatin1String(PyObject* unicode) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_DecodeASCII(const char* s, Py_ssize_t size, const char* errors) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_EncodeASCII(const Py_UNICODE* s, Py_ssize_t size, const char* errors) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_AsASCIIString(PyObject* unicode) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_DecodeCharmap(const char* s, Py_ssize_t size, PyObject* mapping, const char* errors) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_EncodeCharmap(const Py_UNICODE* s, Py_ssize_t size, PyObject* mapping,
                                             const char* errors) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_AsCharmapString(PyObject* unicode, PyObject* mapping) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_TranslateCharmap(const Py_UNICODE* s, Py_ssize_t size, PyObject* table,
                                                const char* errors) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_DecodeMBCS(const char* s, Py_ssize_t size, const char* errors) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_DecodeMBCSStateful(const char* s, int size, const char* errors, int* consumed) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_EncodeMBCS(const Py_UNICODE* s, Py_ssize_t size, const char* errors) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_AsMBCSString(PyObject* unicode) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_Concat(PyObject* left, PyObject* right) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_Split(PyObject* s, PyObject* sep, Py_ssize_t maxsplit) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_Splitlines(PyObject* s, int keepend) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_Translate(PyObject* str, PyObject* table, const char* errors) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_Join(PyObject* separator, PyObject* seq) {
    Py_FatalError("unimplemented");
}

extern "C" Py_ssize_t PyUnicode_Tailmatch(PyObject* str, PyObject* substr, Py_ssize_t start, Py_ssize_t end,
                                          int direction) {
    Py_FatalError("unimplemented");
}

extern "C" Py_ssize_t PyUnicode_Find(PyObject* str, PyObject* substr, Py_ssize_t start, Py_ssize_t end, int direction) {
    Py_FatalError("unimplemented");
}

extern "C" Py_ssize_t PyUnicode_Count(PyObject* str, PyObject* substr, Py_ssize_t start, Py_ssize_t end) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_Replace(PyObject* str, PyObject* substr, PyObject* replstr, Py_ssize_t maxcount) {
    Py_FatalError("unimplemented");
}

extern "C" int PyUnicode_Compare(PyObject* left, PyObject* right) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_RichCompare(PyObject* left, PyObject* right, int op) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* PyUnicode_Format(PyObject* format, PyObject* args) {
    Py_FatalError("unimplemented");
}

extern "C" int PyUnicode_Contains(PyObject* container, PyObject* element) {
    Py_FatalError("unimplemented");
}

extern "C" PyObject* _PyUnicode_AsDefaultEncodedString(PyObject*, const char*) {
    Py_FatalError("unimplemented");
}

extern "C" void _PyUnicode_Fini() {
    Py_FatalError("unimplemented");
}

extern "C" void _PyUnicode_Init() {
    Py_FatalError("unimplemented");
}

extern "C" int _PyUnicode_IsAlpha(Py_UNICODE ch) {
    Py_FatalError("unimplemented");
}

extern "C" int _PyUnicode_IsDecimalDigit(Py_UNICODE ch) {
    Py_FatalError("unimplemented");
}

extern "C" int _PyUnicode_IsDigit(Py_UNICODE ch) {
    Py_FatalError("unimplemented");
}

extern "C" int _PyUnicode_IsLinebreak(Py_UNICODE ch) {
    Py_FatalError("unimplemented");
}

extern "C" int _PyUnicode_IsLowercase(Py_UNICODE ch) {
    Py_FatalError("unimplemented");
}

extern "C" int _PyUnicode_IsNumeric(Py_UNICODE ch) {
    Py_FatalError("unimplemented");
}

extern "C" int _PyUnicode_IsTitlecase(Py_UNICODE ch) {
    Py_FatalError("unimplemented");
}

extern "C" int _PyUnicode_IsUppercase(Py_UNICODE ch) {
    Py_FatalError("unimplemented");
}

extern "C" int _PyUnicode_IsWhitespace(Py_UNICODE ch) {
    Py_FatalError("unimplemented");
}

extern "C" int _PyUnicode_ToDecimalDigit(Py_UNICODE ch) {
    Py_FatalError("unimplemented");
}

extern "C" int _PyUnicode_ToDigit(Py_UNICODE ch) {
    Py_FatalError("unimplemented");
}

extern "C" Py_UNICODE _PyUnicode_ToLowercase(Py_UNICODE ch) {
    Py_FatalError("unimplemented");
}

extern "C" double _PyUnicode_ToNumeric(Py_UNICODE ch) {
    Py_FatalError("unimplemented");
}

extern "C" Py_UNICODE _PyUnicode_ToTitlecase(Py_UNICODE ch) {
    Py_FatalError("unimplemented");
}

extern "C" Py_UNICODE _PyUnicode_ToUppercase(Py_UNICODE ch) {
    Py_FatalError("unimplemented");
}

// From CPython, unicodeobject.c
// Used by Py_UNICODE_ISSPACE in unicodeobject.h
/* Fast detection of the most frequent whitespace characters */
extern "C" const unsigned char _Py_ascii_whitespace[]
    = { 0, 0, 0, 0, 0, 0, 0, 0,
        /*     case 0x0009: * CHARACTER TABULATION */
        /*     case 0x000A: * LINE FEED */
        /*     case 0x000B: * LINE TABULATION */
        /*     case 0x000C: * FORM FEED */
        /*     case 0x000D: * CARRIAGE RETURN */
        0, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        /*     case 0x001C: * FILE SEPARATOR */
        /*     case 0x001D: * GROUP SEPARATOR */
        /*     case 0x001E: * RECORD SEPARATOR */
        /*     case 0x001F: * UNIT SEPARATOR */
        0, 0, 0, 0, 1, 1, 1, 1,
        /*     case 0x0020: * SPACE */
        1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };


void setupUnicode() {
    unicode_cls->giveAttr("__name__", boxStrConstant("unicode"));

    unicode_cls->freeze();
}

void teardownUnicode() {
}
}
