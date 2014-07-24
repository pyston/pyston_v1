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

#include <algorithm>
#include <cstring>
#include <sstream>
#include <unordered_map>

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include "codegen/compvars.h"
#include "core/common.h"
#include "core/types.h"
#include "gc/collector.h"
#include "runtime/gc_runtime.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"
#include "runtime/util.h"
#include "runtime/list.h"


namespace pyston {

/* helper macro to fixup start/end slice values */
inline void adjust_indices(int& start, int& end, const int& len) {
    if (end > len)
        end = len;
    else if (end < 0) {
        end += len;
        if (end < 0)
            end = 0;
    }
    if (start < 0) {
        start += len;
        if (start < 0)
            start = 0;
    }
}

int internalStrFind(const std::string& str, const std::string& sub, int start, int end) {
    adjust_indices(start, end, (int)str.size());

    std::string::size_type result = str.find(sub, start);

    if (result == std::string::npos || (result + sub.size() > (std::string::size_type)end)) {
        return -1;
    }

    return (int)result;
}

int internalStrRfind(const std::string& str, const std::string& sub, int start, int end) {
    adjust_indices(start, end, (int)str.size());

    std::string::size_type result = str.rfind(sub, end);

    if (result == std::string::npos || result < (std::string::size_type)start
        || (result + sub.size() > (std::string::size_type)end))
        return -1;

    return (int)result;
}

/* Matches the end (direction >= 0) or start (direction < 0) of self
    * against substr, using the start and end arguments. Returns
    * -1 on error, 0 if not found and 1 if found.
    */

int _string_tailmatch(const std::string& self, const std::string& substr, int start, int end, int direction) {
    std::string::size_type len = self.size();
    std::string::size_type slen = substr.size();

    const char* sub = substr.c_str();
    const char* str = self.c_str();

    adjust_indices(start, end, len);

    if (direction < 0) {
        // startswith
        if (start + slen > len)
            return 0;
    } else {
        // endswith
        if (end - start < slen || start > len)
            return 0;
        if (end - slen > start)
            start = end - slen;
    }
    if (end - start >= slen)
        return (!std::memcmp(str + start, sub, slen));

    return 0;
}

extern "C" BoxedString* strAdd(BoxedString* lhs, Box* _rhs) {
    assert(lhs->cls == str_cls);

    if (_rhs->cls != str_cls) {
        raiseExcHelper(TypeError, "cannot concatenate 'str' and '%s' objects", getTypeName(_rhs)->c_str());
    }

    BoxedString* rhs = static_cast<BoxedString*>(_rhs);
    return new BoxedString(lhs->s + rhs->s);
}

extern "C" Box* strMod(BoxedString* lhs, Box* rhs) {
    assert(lhs->cls == str_cls);

    const BoxedTuple::GCVector* elts;
    BoxedTuple::GCVector _elts;
    if (rhs->cls == tuple_cls) {
        elts = &static_cast<BoxedTuple*>(rhs)->elts;
    } else {
        elts = &_elts;
        _elts.push_back(rhs);
    }

    const char* fmt = lhs->s.c_str();
    const char* fmt_end = fmt + lhs->s.size();

    int elt_num = 0;
    int num_elts = elts->size();

    std::ostringstream os("");
    while (fmt < fmt_end) {
        if (*fmt != '%') {
            os << (*fmt);
            fmt++;
        } else {
            fmt++;

            int nspace = 0;
            int ndot = 0;
            int nzero = 0;
            int mode = 0;
            while (true) {
                RELEASE_ASSERT(fmt < fmt_end, "");
                char c = *fmt;
                fmt++;

                if (c == ' ') {
                    assert(mode == 0);
                    mode = 1;
                } else if (c == '.') {
                    assert(mode == 0);
                    mode = 2;
                } else if (mode == 0 && c == '0') {
                    mode = 3;
                } else if ('0' <= c && c <= '9') {
                    assert(mode == 1 || mode == 2 || mode == 3);
                    if (mode == 1) {
                        nspace = nspace * 10 + c - '0';
                    } else if (mode == 2) {
                        ndot = ndot * 10 + c - '0';
                    } else if (mode == 3) {
                        nzero = nzero * 10 + c - '0';
                    } else {
                        assert(0);
                    }
                } else if (c == '%') {
                    for (int i = 1; i < nspace; i++) {
                        os << ' ';
                    }
                    os << '%';
                    break;
                } else if (c == 's') {
                    RELEASE_ASSERT(ndot == 0, "");
                    RELEASE_ASSERT(nzero == 0, "");
                    RELEASE_ASSERT(nspace == 0, "");

                    RELEASE_ASSERT(elt_num < num_elts, "insufficient number of arguments for format string");
                    Box* b = (*elts)[elt_num];
                    elt_num++;

                    BoxedString* s = str(b);
                    os << s->s;
                    break;
                } else if (c == 'd') {
                    RELEASE_ASSERT(elt_num < num_elts, "insufficient number of arguments for format string");
                    Box* b = (*elts)[elt_num];
                    elt_num++;

                    RELEASE_ASSERT(b->cls == int_cls, "unsupported");

                    std::ostringstream fmt("");
                    fmt << '%';
                    if (nspace)
                        fmt << ' ' << nspace;
                    else if (ndot)
                        fmt << '.' << ndot;
                    else if (nzero)
                        fmt << '0' << nzero;
                    fmt << "ld";

                    char buf[20];
                    snprintf(buf, 20, fmt.str().c_str(), static_cast<BoxedInt*>(b)->n);
                    os << std::string(buf);
                    break;
                } else if (c == 'f') {
                    RELEASE_ASSERT(elt_num < num_elts, "insufficient number of arguments for format string");
                    Box* b = (*elts)[elt_num];
                    elt_num++;

                    double d;
                    if (b->cls == float_cls) {
                        d = static_cast<BoxedFloat*>(b)->d;
                    } else if (b->cls == int_cls) {
                        d = static_cast<BoxedInt*>(b)->n;
                    } else {
                        RELEASE_ASSERT(0, "unsupported");
                    }

                    std::ostringstream fmt("");
                    fmt << '%';
                    if (nspace)
                        fmt << ' ' << nspace;
                    else if (ndot)
                        fmt << '.' << ndot;
                    else if (nzero)
                        fmt << '0' << nzero;
                    fmt << "f";

                    char buf[20];
                    snprintf(buf, 20, fmt.str().c_str(), d);
                    os << std::string(buf);
                    break;
                } else {
                    RELEASE_ASSERT(0, "unsupported format character '%c'", c);
                }
            }
        }
    }
    assert(fmt == fmt_end && "incomplete format");

    return boxString(os.str());
}

Box* strMul(BoxedString* lhs, BoxedInt* rhs) {
    assert(lhs->cls == str_cls);
    assert(rhs->cls == int_cls);

    const std::string& str = lhs->s;
    int n = static_cast<BoxedInt*>(rhs)->n;

    if (n <= 0)
        return boxString("");
    if (n == 1)
        return boxString(str);

    std::ostringstream os;
    for (int i = 0; i < n; ++i) {
        os << str;
    }
    return boxString(os.str());
}

extern "C" Box* strLt(BoxedString* lhs, Box* rhs) {
    assert(lhs->cls == str_cls);

    if (rhs->cls != str_cls)
        return NotImplemented;

    BoxedString* srhs = static_cast<BoxedString*>(rhs);
    return boxBool(lhs->s < srhs->s);
}

extern "C" Box* strLe(BoxedString* lhs, Box* rhs) {
    assert(lhs->cls == str_cls);

    if (rhs->cls != str_cls)
        return NotImplemented;

    BoxedString* srhs = static_cast<BoxedString*>(rhs);
    return boxBool(lhs->s <= srhs->s);
}

extern "C" Box* strGt(BoxedString* lhs, Box* rhs) {
    assert(lhs->cls == str_cls);

    if (rhs->cls != str_cls)
        return NotImplemented;

    BoxedString* srhs = static_cast<BoxedString*>(rhs);
    return boxBool(lhs->s > srhs->s);
}

extern "C" Box* strGe(BoxedString* lhs, Box* rhs) {
    assert(lhs->cls == str_cls);

    if (rhs->cls != str_cls)
        return NotImplemented;

    BoxedString* srhs = static_cast<BoxedString*>(rhs);
    return boxBool(lhs->s >= srhs->s);
}

extern "C" Box* strEq(BoxedString* lhs, Box* rhs) {
    assert(lhs->cls == str_cls);

    if (rhs->cls != str_cls)
        return boxBool(false);

    BoxedString* srhs = static_cast<BoxedString*>(rhs);
    return boxBool(lhs->s == srhs->s);
}

extern "C" Box* strNe(BoxedString* lhs, Box* rhs) {
    assert(lhs->cls == str_cls);

    if (rhs->cls != str_cls)
        return boxBool(true);

    BoxedString* srhs = static_cast<BoxedString*>(rhs);
    return boxBool(lhs->s != srhs->s);
}

extern "C" Box* strLen(BoxedString* self) {
    assert(self->cls == str_cls);

    return boxInt(self->s.size());
}

extern "C" Box* strStr(BoxedString* self) {
    assert(self->cls == str_cls);

    return self;
}

static bool _needs_escaping[256]
    = { true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,
        true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,
        false, false, false, false, false, false, false, true,  false, false, false, false, false, false, false, false,
        false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
        false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
        false, false, false, false, false, false, false, false, false, false, false, false, true,  false, false, false,
        false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
        false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, true,
        true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,
        true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,
        true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,
        true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,
        true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,
        true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,
        true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,
        true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  true };
static char _hex[17] = "0123456789abcdef"; // really only needs to be 16 but clang will complain
extern "C" Box* strRepr(BoxedString* self) {
    assert(self->cls == str_cls);

    std::ostringstream os("");

    const std::string& s = self->s;
    char quote = '\'';
    if (s.find('\'', 0) != std::string::npos && s.find('\"', 0) == std::string::npos) {
        quote = '\"';
    }
    os << quote;
    for (int i = 0; i < s.size(); i++) {
        char c = s[i];
        if ((c == '\'' && quote == '\"') || !_needs_escaping[c & 0xff]) {
            os << c;
        } else {
            char special = 0;
            switch (c) {
                case '\t':
                    special = 't';
                    break;
                case '\n':
                    special = 'n';
                    break;
                case '\r':
                    special = 'r';
                    break;
                case '\'':
                    special = '\'';
                    break;
                case '\"':
                    special = '\"';
                    break;
                case '\\':
                    special = '\\';
                    break;
            }
            if (special) {
                os << '\\';
                os << special;
            } else {
                os << '\\';
                os << 'x';
                os << _hex[(c & 0xff) / 16];
                os << _hex[(c & 0xff) % 16];
            }
        }
    }
    os << quote;

    return boxString(os.str());
}

extern "C" Box* strHash(BoxedString* self) {
    assert(self->cls == str_cls);

    std::hash<std::string> H;
    return boxInt(H(self->s));
}

extern "C" Box* strNonzero(BoxedString* self) {
    assert(self->cls == str_cls);

    return boxBool(self->s.size() != 0);
}

extern "C" Box* strNew(BoxedClass* cls, Box* obj) {
    assert(cls == str_cls);

    return str(obj);
}

Box* _strSlice(BoxedString* self, i64 start, i64 stop, i64 step) {
    assert(self->cls == str_cls);

    const std::string& s = self->s;

    assert(step != 0);
    if (step > 0) {
        assert(0 <= start);
        assert(stop <= s.size());
    } else {
        assert(start < s.size());
        assert(-1 <= stop);
    }

    std::vector<char> chars;
    int cur = start;
    while ((step > 0 && cur < stop) || (step < 0 && cur > stop)) {
        chars.push_back(s[cur]);
        cur += step;
    }
    // TODO too much copying
    return boxString(std::string(chars.begin(), chars.end()));
}

Box* strIsAlpha(BoxedString* self) {
    assert(self->cls == str_cls);

    std::string str(self->s);
    std::string::size_type i;
    if (str.empty())
        return False;

    for (i = 0; i < str.size(); ++i) {
        if (!std::isalpha(str[i]))
            return False;
    }
    return True;
}

Box* strIsDigit(BoxedString* self) {
    assert(self->cls == str_cls);

    std::string str(self->s);
    std::string::size_type i;
    if (str.empty())
        return False;

    for (i = 0; i < str.size(); ++i) {
        if (!std::isdigit(str[i]))
            return False;
    }
    return True;
}

Box* strIsAlnum(BoxedString* self) {
    assert(self->cls == str_cls);

    std::string str(self->s);
    std::string::size_type i;
    if (str.empty())
        return False;

    for (i = 0; i < str.size(); ++i) {
        if (!std::isalnum(str[i]))
            return False;
    }
    return True;
}

Box* strLower(BoxedString* self) {
    assert(self->cls == str_cls);
    return boxString(llvm::StringRef(self->s).lower());
}

Box* strUpper(BoxedString* self) {
    assert(self->cls == str_cls);
    return boxString(llvm::StringRef(self->s).upper());
}

Box* strSwapcase(BoxedString* self) {
    std::string s(self->s);
    std::string::size_type len = s.size(), i;

    for (i = 0; i < len; ++i) {
        if (std::islower(s[i]))
            s[i] = std::toupper(s[i]);
        else if (std::isupper(s[i]))
            s[i] = std::tolower(s[i]);
    }

    return boxString(s);
}

Box* strIsLower(BoxedString* self) {
    assert(self->cls == str_cls);

    std::string str(self->s);
    std::string::size_type i;
    if (str.empty())
        return False;

    for (i = 0; i < str.size(); ++i) {
        if (!std::islower(str[i]))
            return False;
    }
    return True;
}

Box* strIsUpper(BoxedString* self) {
    assert(self->cls == str_cls);

    std::string str(self->s);
    std::string::size_type i;
    if (str.empty())
        return False;

    for (i = 0; i < str.size(); ++i) {
        if (!std::isupper(str[i]))
            return False;
    }
    return True;
}

Box* strIsSpace(BoxedString* self) {
    assert(self->cls == str_cls);

    std::string str(self->s);
    std::string::size_type i;
    if (str.empty())
        return False;

    for (i = 0; i < str.size(); ++i) {
        if (!std::isspace(str[i]))
            return False;
    }
    return True;
}

Box* strIsTitle(BoxedString* self) {
    assert(self->cls == str_cls);

    std::string str(self->s);
    std::string::size_type i;

    if (str.empty())
        return False;
    if (str.size() == 1)
        return boxBool(std::isupper(str[0]));

    bool cased = false, previous_is_cased = false;

    for (i = 0; i < str.size(); ++i) {
        if (std::isupper(str[i])) {
            if (previous_is_cased) {
                return False;
            }

            previous_is_cased = true;
            cased = true;
        } else if (std::islower(str[i])) {
            if (!previous_is_cased) {
                return False;
            }

            previous_is_cased = true;
            cased = true;

        } else {
            previous_is_cased = false;
        }
    }

    return boxBool(cased);
}

Box* strCapitalize(BoxedString* self) {
    assert(self->cls == str_cls);

    std::string s(self->s);
    std::string::size_type len = s.size(), i;

    if (len > 0) {
        if (std::islower(s[0]))
            s[0] = std::toupper(s[0]);
    }

    for (i = 1; i < len; ++i) {
        if (std::isupper(s[i]))
            s[i] = std::tolower(s[i]);
    }

    return boxString(s);
}


Box* strJoin(BoxedString* self, Box* rhs) {
    assert(self->cls == str_cls);

    if (rhs->cls == list_cls) {
        BoxedList* list = static_cast<BoxedList*>(rhs);
        std::ostringstream os;

        if (list->size == 0)
            return boxString("");
        if (list->size == 1)
            return str(list->elts->elts[0]);

        for (int i = 0; i < list->size; i++) {
            if (i > 0)
                os << self->s;
            BoxedString* elt_str = str(list->elts->elts[i]);
            os << elt_str->s;
        }
        return boxString(os.str());
    } else if (rhs->cls == tuple_cls) {
        BoxedTuple* tuple = static_cast<BoxedTuple*>(rhs);
        std::ostringstream os;

        if (tuple->elts.size() == 0)
            return boxString("");
        if (tuple->elts.size() == 1)
            return str(tuple->elts[0]);

        for (int i = 0; i < tuple->elts.size(); i++) {
            if (i > 0)
                os << self->s;
            BoxedString* elt_str = str(tuple->elts[i]);
            os << elt_str->s;
        }
        return boxString(os.str());
    } else {
        raiseExcHelper(TypeError, "");
    }
}

Box* strSplit(BoxedString* self, BoxedString* sepbox, Box* maxsplitbox) {
    assert(self->cls == str_cls);

    BoxedList* rtn = new BoxedList();

    int maxsplit = INT32_MAX;

    std::string str(self->s);
    std::string sep = "";

    if (sepbox->cls == str_cls) {
        sep = sepbox->s;
    }

    if (maxsplitbox->cls == int_cls) {
        maxsplit = static_cast<BoxedInt*>(maxsplitbox)->n;
    }

    std::string::size_type i, j, len = str.size(), n = sep.size();

    i = j = 0;

    if (maxsplit < 0)
        maxsplit = INT32_MAX;


    if (sep.empty()) {
        // split by whitespace
        for (i = j = 0; i < len;) {

            while (i < len && std::isspace(str[i]))
                i++;
            j = i;

            while (i < len && !std::isspace(str[i]))
                i++;

            if (j < i) {
                if (maxsplit-- <= 0)
                    break;

                listAppendInternal(rtn, boxString(str.substr(j, i - j)));

                while (i < len && std::isspace(str[i]))
                    i++;
                j = i;
            }
        }
        if (j < len) {
            listAppendInternal(rtn, boxString(str.substr(j, len - j)));
        }

        return rtn;
    }


    while (i + n <= len) {
        if (str[i] == sep[0] && str.substr(i, n) == sep) {
            if (maxsplit-- <= 0)
                break;

            listAppendInternal(rtn, boxString(str.substr(j, i - j)));
            i = j = i + n;
        } else {
            i++;
        }
    }

    listAppendInternal(rtn, boxString(str.substr(j, len - j)));

    return rtn;
}

Box* strRsplit(BoxedString* self, BoxedString* sepbox, Box* maxsplitbox) {
    assert(self->cls == str_cls);

    BoxedList* rtn = new BoxedList();

    int maxsplit = INT32_MAX;

    std::string str(self->s);
    std::string sep = "";

    if (sepbox->cls == str_cls) {
        sep = sepbox->s;
    }

    if (maxsplitbox->cls == int_cls) {
        maxsplit = static_cast<BoxedInt*>(maxsplitbox)->n;
    }

    std::string::size_type i, j, len = str.size(), n = sep.size();

    i = j = len;

    if (maxsplit < 0) {
        return strSplit(self, sepbox, maxsplitbox);
    }

    if (sep.empty()) {
        for (i = j = len; i > 0;) {

            while (i > 0 && std::isspace(str[i - 1]))
                i--;
            j = i;

            while (i > 0 && !std::isspace(str[i - 1]))
                i--;



            if (j > i) {
                if (maxsplit-- <= 0)
                    break;

                listAppendInternal(rtn, boxString(str.substr(i, j - i)));

                while (i > 0 && std::isspace(str[i - 1]))
                    i--;
                j = i;
            }
        }
        if (j > 0) {
            listAppendInternal(rtn, boxString(str.substr(0, j)));
        }

        listReverse(rtn);
        return rtn;
    }

    while (i >= n) {
        if (str[i - 1] == sep[n - 1] && str.substr(i - n, n) == sep) {
            if (maxsplit-- <= 0)
                break;

            listAppendInternal(rtn, boxString(str.substr(i, j - i)));
            i = j = i - n;
        } else {
            i--;
        }
    }

    listAppendInternal(rtn, boxString(str.substr(0, j)));

    listReverse(rtn);
    return rtn;
}

Box* strSplitlines(BoxedString* self, BoxedBool* keepends) {
    assert(self->cls == str_cls);
    if (keepends->cls != bool_cls && keepends->cls != int_cls) {
        raiseExcHelper(TypeError, "an integer is required");
    }
    BoxedList* rtn = new BoxedList();

    const std::string& str = self->s;
    std::string::size_type len = str.size(), i, j, eol;

    for (i = j = 0; i < len;) {
        while (i < len && str[i] != '\n' && str[i] != '\r')
            i++;

        eol = i;
        if (i < len) {
            if (str[i] == '\r' && i + 1 < len && str[i + 1] == '\n') {
                i += 2;
            } else {
                i++;
            }
            if (static_cast<BoxedBool*>(keepends)->b)
                eol = i;
        }

        listAppendInternal(rtn, boxString(str.substr(j, eol - j)));
        j = i;
    }

    if (j < len) {
        listAppendInternal(rtn, boxString(str.substr(j, len - j)));
    }

    return rtn;
}

Box* strReplace(BoxedString* self, BoxedString* oldstrbox, BoxedString* newstrbox, Box** args) {
    assert(self->cls == str_cls);

    Box* countbox = args[0];

    if (oldstrbox->cls != str_cls) {
        raiseExcHelper(TypeError, "expected a character buffer object");
    } else if (newstrbox->cls != str_cls) {
        raiseExcHelper(TypeError, "expected a character buffer object");
    } else if (countbox->cls != int_cls && countbox->cls != none_cls) {
        raiseExcHelper(TypeError, "slice indices must be integers or None or have an __index__ method");
    }

    const std::string& str = self->s;
    const std::string& oldstr = static_cast<BoxedString*>(oldstrbox)->s;
    const std::string& newstr = static_cast<BoxedString*>(newstrbox)->s;

    int sofar = 0;
    int cursor = 0;
    int count = static_cast<BoxedInt*>(countbox)->n;
    std::string s(str);

    std::string::size_type oldlen = oldstr.size(), newlen = newstr.size();

    cursor = internalStrFind(s, oldstr, cursor, INT32_MAX);

    while (cursor != -1 && cursor <= (int)s.size()) {
        if (count > -1 && sofar >= count) {
            break;
        }

        s.replace(cursor, oldlen, newstr);
        cursor += (int)newlen;

        if (oldlen != 0) {
            cursor = internalStrFind(s, oldstr, cursor, INT32_MAX);
        } else {
            ++cursor;
        }

        ++sofar;
    }

    return boxString(s);
}

Box* strStrip(BoxedString* self, Box* chars) {
    assert(self->cls == str_cls);

    if (chars->cls == str_cls) {
        return new BoxedString(llvm::StringRef(self->s).trim(static_cast<BoxedString*>(chars)->s));
    } else if (chars->cls == none_cls) {
        return new BoxedString(llvm::StringRef(self->s).trim(" \t\n\r\f\v"));
    } else {
        raiseExcHelper(TypeError, "strip arg must be None, str or unicode");
    }
}

Box* strLStrip(BoxedString* self, Box* chars) {
    assert(self->cls == str_cls);

    if (chars->cls == str_cls) {
        return new BoxedString(llvm::StringRef(self->s).ltrim(static_cast<BoxedString*>(chars)->s));
    } else if (chars->cls == none_cls) {
        return new BoxedString(llvm::StringRef(self->s).ltrim(" \t\n\r\f\v"));
    } else {
        raiseExcHelper(TypeError, "lstrip arg must be None, str or unicode");
    }
}

Box* strRStrip(BoxedString* self, Box* chars) {
    assert(self->cls == str_cls);

    if (chars->cls == str_cls) {
        return new BoxedString(llvm::StringRef(self->s).rtrim(static_cast<BoxedString*>(chars)->s));
    } else if (chars->cls == none_cls) {
        return new BoxedString(llvm::StringRef(self->s).rtrim(" \t\n\r\f\v"));
    } else {
        raiseExcHelper(TypeError, "rstrip arg must be None, str or unicode");
    }
}

Box* strContains(BoxedString* self, Box* elt) {
    assert(self->cls == str_cls);
    if (elt->cls != str_cls)
        raiseExcHelper(TypeError, "'in <string>' requires string as left operand, not %s", getTypeName(elt)->c_str());

    BoxedString* sub = static_cast<BoxedString*>(elt);

    size_t found_idx = self->s.find(sub->s);
    if (found_idx == std::string::npos)
        return False;
    return True;
}


extern "C" Box* strGetitem(BoxedString* self, Box* slice) {
    assert(self->cls == str_cls);

    if (slice->cls == int_cls) {
        BoxedInt* islice = static_cast<BoxedInt*>(slice);
        int64_t n = islice->n;
        int size = self->s.size();
        if (n < 0)
            n = size + n;

        if (n < 0 || n >= size) {
            raiseExcHelper(IndexError, "string index out of range");
        }

        char c = self->s[n];
        return new BoxedString(std::string(1, c));
    } else if (slice->cls == slice_cls) {
        BoxedSlice* sslice = static_cast<BoxedSlice*>(slice);

        i64 start, stop, step;
        parseSlice(sslice, self->s.size(), &start, &stop, &step);
        return _strSlice(self, start, stop, step);
    } else {
        raiseExcHelper(TypeError, "string indices must be integers, not %s", getTypeName(slice)->c_str());
    }
}

Box* strSlice(BoxedString* self, BoxedInt* startbox, BoxedInt* stopbox) {
    assert(self->cls == str_cls);

    i64 start = 0, stop = INT32_MAX, step = 1;

    if (startbox->cls == int_cls) {
        start = static_cast<BoxedInt*>(startbox)->n;
    } else {
        raiseExcHelper(TypeError, "an integer is required");
    }

    if (stopbox->cls == int_cls) {
        stop = static_cast<BoxedInt*>(stopbox)->n;
    } else {
        raiseExcHelper(TypeError, "an integer is required");
    }

    if (stop >= self->s.size()) {
        stop = self->s.size();
    }

    return _strSlice(self, start, stop, step);
}

// TODO it looks like strings don't have their own iterators, but instead
// rely on the sequence iteration protocol.
// Should probably implement that, and maybe once that's implemented get
// rid of the striterator class?
BoxedClass* str_iterator_cls = NULL;
extern "C" void strIteratorGCHandler(GCVisitor* v, void* p);
extern "C" const ObjectFlavor str_iterator_flavor(&strIteratorGCHandler, NULL);

class BoxedStringIterator : public Box {
public:
    BoxedString* s;
    std::string::const_iterator it, end;

    BoxedStringIterator(BoxedString* s)
        : Box(&str_iterator_flavor, str_iterator_cls), s(s), it(s->s.begin()), end(s->s.end()) {}

    static bool hasnextUnboxed(BoxedStringIterator* self) {
        assert(self->cls == str_iterator_cls);
        return self->it != self->end;
    }

    static Box* hasnext(BoxedStringIterator* self) {
        assert(self->cls == str_iterator_cls);
        return boxBool(self->it != self->end);
    }

    static Box* next(BoxedStringIterator* self) {
        assert(self->cls == str_iterator_cls);
        assert(hasnextUnboxed(self));

        char c = *self->it;
        ++self->it;
        return new BoxedString(std::string(1, c));
    }
};

extern "C" void strIteratorGCHandler(GCVisitor* v, void* p) {
    boxGCHandler(v, p);
    BoxedStringIterator* it = (BoxedStringIterator*)p;
    v->visit(it->s);
}

Box* strIter(BoxedString* self) {
    assert(self->cls == str_cls);
    return new BoxedStringIterator(self);
}

Box* strFind(BoxedString* self, Box* elt, Box* startbox, Box** args) {
    assert(self->cls == str_cls);

    Box* endbox = args[0];

    if (elt->cls != str_cls) {
        raiseExcHelper(TypeError, "expected a character buffer object");
    } else if (startbox->cls != int_cls && startbox->cls != none_cls) {
        raiseExcHelper(TypeError, "slice indices must be integers or None or have an __index__ method");
    } else if (endbox->cls != int_cls && endbox->cls != none_cls) {
        raiseExcHelper(TypeError, "slice indices must be integers or None or have an __index__ method");
    }

    const std::string& str = self->s;
    const std::string& pattern = static_cast<BoxedString*>(elt)->s;

    std::size_t found = 0;
    int start = 0, end = INT32_MAX;

    if (startbox->cls == int_cls) {
        start = static_cast<BoxedInt*>(startbox)->n;
    }
    if (endbox->cls == int_cls) {
        end = static_cast<BoxedInt*>(endbox)->n;
    }

    found = internalStrFind(str, pattern, start, end);

    return boxInt(found);
}

Box* strRfind(BoxedString* self, Box* elt, Box* startbox, Box** args) {
    assert(self->cls == str_cls);

    Box* endbox = args[0];

    if (elt->cls != str_cls) {
        raiseExcHelper(TypeError, "expected a character buffer object");
    } else if (startbox->cls != int_cls && startbox->cls != none_cls) {
        raiseExcHelper(TypeError, "slice indices must be integers or None or have an __index__ method");
    } else if (endbox->cls != int_cls && endbox->cls != none_cls) {
        raiseExcHelper(TypeError, "slice indices must be integers or None or have an __index__ method");
    }

    const std::string& str = self->s;
    const std::string& pattern = static_cast<BoxedString*>(elt)->s;

    std::size_t found = 0;
    int start = 0, end = INT32_MAX;

    if (startbox->cls == int_cls) {
        start = static_cast<BoxedInt*>(startbox)->n;
    }
    if (endbox->cls == int_cls) {
        end = static_cast<BoxedInt*>(endbox)->n;
    }

    found = internalStrRfind(str, pattern, start, end);

    return boxInt(found);
}

Box* strEndswith(BoxedString* self, Box* suffixbox, Box* startbox, Box** args) {
    assert(self->cls == str_cls);

    Box* endbox = args[0];

    if (suffixbox->cls != str_cls) {
        raiseExcHelper(TypeError, "expected a character buffer object");
    } else if (startbox->cls != int_cls && startbox->cls != none_cls) {
        raiseExcHelper(TypeError, "slice indices must be integers or None or have an __index__ method");
    } else if (endbox->cls != int_cls && endbox->cls != none_cls) {
        raiseExcHelper(TypeError, "slice indices must be integers or None or have an __index__ method");
    }

    const std::string& str = self->s;
    const std::string& suffix = static_cast<BoxedString*>(suffixbox)->s;

    std::size_t start = 0, end = INT32_MAX;

    if (startbox->cls == int_cls) {
        start = static_cast<BoxedInt*>(startbox)->n;
    }
    if (endbox->cls == int_cls) {
        end = static_cast<BoxedInt*>(endbox)->n;
    }

    int result = _string_tailmatch(str, suffix, start, end, +1);
    // if (result == -1) // TODO: Error condition

    return boxBool(static_cast<bool>(result));
}


Box* strStartswith(BoxedString* self, Box* prefixbox, Box* startbox, Box** args) {
    assert(self->cls == str_cls);

    Box* endbox = args[0];

    if (prefixbox->cls != str_cls) {
        raiseExcHelper(TypeError, "expected a character buffer object");
    } else if (startbox->cls != int_cls && startbox->cls != none_cls) {
        raiseExcHelper(TypeError, "slice indices must be integers or None or have an __index__ method");
    } else if (endbox->cls != int_cls && endbox->cls != none_cls) {
        raiseExcHelper(TypeError, "slice indices must be integers or None or have an __index__ method");
    }

    const std::string& str = self->s;
    const std::string& prefix = static_cast<BoxedString*>(prefixbox)->s;

    std::size_t start = 0, end = INT32_MAX;

    if (startbox->cls == int_cls) {
        start = static_cast<BoxedInt*>(startbox)->n;
    }
    if (endbox->cls == int_cls) {
        end = static_cast<BoxedInt*>(endbox)->n;
    }

    int result = _string_tailmatch(str, prefix, start, end, -1);
    // if (result == -1) // TODO: Error condition

    return boxBool(static_cast<bool>(result));
}

Box* strCount(BoxedString* self, Box* elt, Box* startbox, Box** args) {
    assert(self->cls == str_cls);

    Box* endbox = args[0];

    if (elt->cls != str_cls) {
        raiseExcHelper(TypeError, "expected a character buffer object");
    } else if (startbox->cls != int_cls && startbox->cls != none_cls) {
        raiseExcHelper(TypeError, "slice indices must be integers or None or have an __index__ method");
    } else if (endbox->cls != int_cls && endbox->cls != none_cls) {
        raiseExcHelper(TypeError, "slice indices must be integers or None or have an __index__ method");
    }

    const std::string& str = self->s;
    const std::string& pattern = static_cast<BoxedString*>(elt)->s;

    std::size_t found = 0;
    int start = 0, end = INT32_MAX;

    if (startbox->cls == int_cls) {
        start = static_cast<BoxedInt*>(startbox)->n;
    }
    if (endbox->cls == int_cls) {
        end = static_cast<BoxedInt*>(endbox)->n;
    }

    while (1) {
        start = internalStrFind(str, pattern, start, end);
        if (start < 0)
            break;

        if (pattern.size() != 0) {
            start = internalStrFind(str, pattern, start, end);

            start += pattern.size();
        } else {
            ++start;
        }

        found++;
    }

    return boxInt(found);
}

Box* strIndex(BoxedString* self, Box* elt, Box* startbox, Box** args) {
    Box* rtn = strFind(self, elt, startbox, args);
    int rtn_val = static_cast<BoxedInt*>(rtn)->n;

    if (rtn_val == -1) {
        raiseExcHelper(ValueError, "substring not found");
    }
    return boxInt(rtn_val);
}

Box* strRindex(BoxedString* self, Box* elt, Box* startbox, Box** args) {
    Box* rtn = strRfind(self, elt, startbox, args);
    int rtn_val = static_cast<BoxedInt*>(rtn)->n;

    if (rtn_val == -1) {
        raiseExcHelper(ValueError, "substring not found");
    }
    return boxInt(rtn_val);
}

Box* strExpandtabs(BoxedString* self, Box* tabs) {
    assert(self->cls == str_cls);

    if (tabs->cls != int_cls) {
        raiseExcHelper(TypeError, "an integer is required");
    }

    const std::string& str = self->s;
    std::string s(str);

    std::string::size_type len = str.size(), i = 0, tabsize = static_cast<BoxedInt*>(tabs)->n;
    int offset = 0, j = 0;

    for (i = 0; i < len; ++i) {
        if (str[i] == '\t') {

            if (tabsize > 0) {
                int fillsize = tabsize - (j % tabsize);
                j += fillsize;
                s.replace(i + offset, 1, std::string(fillsize, ' '));
                offset += fillsize - 1;
            } else {
                s.replace(i + offset, 1, "");
                offset -= 1;
            }

        } else {
            j++;

            if (str[i] == '\n' || str[i] == '\r') {
                j = 0;
            }
        }
    }

    return boxString(s);
}

Box* strZfill(BoxedString* self, BoxedInt* boxwidth) {
    assert(self->cls == str_cls);

    const std::string& str = self->s;

    if (boxwidth->cls != int_cls) {
        raiseExcHelper(TypeError, "an integer is required");
    }

    std::string::size_type width = static_cast<BoxedInt*>(boxwidth)->n;

    int len = (int)str.size();

    if (len >= width) {
        return boxString(str);
    }

    std::string s(str);

    int fill = width - len;

    s = std::string(fill, '0') + s;


    if (s[fill] == '+' || s[fill] == '-') {
        s[0] = s[fill];
        s[fill] = '0';
    }

    return boxString(s);
}

Box* strTitle(BoxedString* self) {
    assert(self->cls == str_cls);

    std::string s(self->s);
    std::string::size_type len = s.size(), i;
    bool previous_is_cased = false;

    for (i = 0; i < len; ++i) {
        int c = s[i];
        if (std::islower(c)) {
            if (!previous_is_cased) {
                s[i] = (char)std::toupper(c);
            }
            previous_is_cased = true;
        } else if (std::isupper(c)) {
            if (previous_is_cased) {
                s[i] = (char)std::tolower(c);
            }
            previous_is_cased = true;
        } else {
            previous_is_cased = false;
        }
    }

    return boxString(s);
}

Box* strTranslate(BoxedString* self, Box* tablebox, Box* deletecharsbox) {
    assert(self->cls == str_cls);

    const std::string& str = self->s;

    std::string table;
    std::string deletechars;


    if (tablebox->cls == none_cls) {
        table = "";
        return boxString(str);
    } else if (tablebox->cls == str_cls) {
        table = static_cast<BoxedString*>(tablebox)->s;
    } else {
    }

    if (deletecharsbox->cls == none_cls) {
        deletechars = "";
    } else if (deletecharsbox->cls == str_cls) {
        deletechars = static_cast<BoxedString*>(deletecharsbox)->s;
    } else {
        // TODO: traise exception
    }

    std::string s;
    std::string::size_type len = str.size(), dellen = deletechars.size();

    if (table.size() == 0 && deletechars.size() == 0) {
        return boxString(str);
    }

    if (table.size() != 256) {
        raiseExcHelper(ValueError, "translation table must be 256 characters long");
    }

    // if nothing is deleted, use faster code
    if (dellen == 0) {
        s = str;
        for (std::string::size_type i = 0; i < len; ++i) {
            s[i] = table[s[i]];
        }
        return boxString(s);
    }


    int trans_table[256];
    for (int i = 0; i < 256; i++) {
        trans_table[i] = table[i];
    }

    for (std::string::size_type i = 0; i < dellen; i++) {
        trans_table[(int)deletechars[i]] = -1;
    }

    for (std::string::size_type i = 0; i < len; ++i) {
        if (trans_table[(int)str[i]] != -1) {
            s += table[str[i]];
        }
    }

    return boxString(s);
}

Box* strLjust(BoxedString* self, BoxedInt* boxwidth, Box* boxfillchar) {
    assert(self->cls == str_cls);

    const std::string& str = self->s;
    std::string fillchar;

    if (boxwidth->cls != int_cls) {
        raiseExcHelper(TypeError, "an integer is required");
    }

    if (boxfillchar->cls == none_cls) {
        fillchar = " ";
    } else if (boxfillchar->cls == str_cls) {
        fillchar = static_cast<BoxedString*>(boxfillchar)->s;
    } else {
        // TODO: traise exception
    }

    std::string::size_type width = static_cast<BoxedInt*>(boxwidth)->n, len = str.size();
    if (((int)len) >= width)
        return boxString(str);

    return boxString(str + std::string(width - len, (char)fillchar.front()));
}

Box* strRjust(BoxedString* self, BoxedInt* boxwidth, Box* boxfillchar) {
    assert(self->cls == str_cls);

    const std::string& str = self->s;
    std::string fillchar;

    if (boxwidth->cls != int_cls) {
        raiseExcHelper(TypeError, "an integer is required");
    }

    if (boxfillchar->cls == none_cls) {
        fillchar = " ";
    } else if (boxfillchar->cls == str_cls) {
        fillchar = static_cast<BoxedString*>(boxfillchar)->s;
    } else {
        // TODO: traise exception
    }

    std::string::size_type width = static_cast<BoxedInt*>(boxwidth)->n, len = str.size();

    if (((int)len) >= width)
        return boxString(str);

    return boxString(std::string(width - len, (char)fillchar.front()) + str);
}

Box* strCenter(BoxedString* self, BoxedInt* boxwidth, Box* boxfillchar) {
    assert(self->cls == str_cls);

    const std::string& str = self->s;
    std::string fillchar;

    if (boxwidth->cls != int_cls) {
        raiseExcHelper(TypeError, "an integer is required");
    }

    if (boxfillchar->cls == none_cls) {
        fillchar = " ";
    } else if (boxfillchar->cls == str_cls) {
        fillchar = static_cast<BoxedString*>(boxfillchar)->s;
    } else {
        // TODO: traise exception
    }

    std::string::size_type width = static_cast<BoxedInt*>(boxwidth)->n, len = str.size();

    int marg, left;

    if (len >= width)
        return boxString(str);

    marg = width - len;
    left = marg / 2 + (marg & width & 1);

    return boxString(std::string(left, (char)fillchar.front()) + str
                     + std::string(marg - left, (char)fillchar.front()));
}

Box* strPartition(BoxedString* self, BoxedString* sepbox) {
    assert(self->cls == str_cls);

    std::string str(self->s);
    std::string sep = "";
    BoxedTuple::GCVector elts;

    if (sepbox->cls == str_cls) {
        sep = sepbox->s;
    }

    int index = internalStrFind(str, sep, 0, INT32_MAX);
    if (index < 0) {
        elts.push_back(boxString(str));
        elts.push_back(boxString(""));
        elts.push_back(boxString(""));
    } else {
        elts.push_back(boxString(str.substr(0, index)));
        elts.push_back(boxString(sep));
        elts.push_back(boxString(str.substr(index + sep.size(), str.size())));
    }

    return new BoxedTuple(std::move(elts));
}

Box* strRpartition(BoxedString* self, BoxedString* sepbox) {
    assert(self->cls == str_cls);

    std::string str(self->s);
    std::string sep = "";
    BoxedTuple::GCVector elts;

    if (sepbox->cls == str_cls) {
        sep = sepbox->s;
    }

    int index = internalStrRfind(str, sep, 0, INT32_MAX);
    if (index < 0) {
        elts.push_back(boxString(""));
        elts.push_back(boxString(""));
        elts.push_back(boxString(str));
    } else {
        elts.push_back(boxString(str.substr(0, index)));
        elts.push_back(boxString(sep));
        elts.push_back(boxString(str.substr(index + sep.size(), str.size())));
    }

    return new BoxedTuple(std::move(elts));
}


void setupStr() {
    str_iterator_cls = new BoxedClass(object_cls, 0, sizeof(BoxedString), false);
    gc::registerStaticRootObj(str_iterator_cls);
    str_iterator_cls->giveAttr("__name__", boxStrConstant("striterator"));
    str_iterator_cls->giveAttr("__hasnext__",
                               new BoxedFunction(boxRTFunction((void*)BoxedStringIterator::hasnext, BOXED_BOOL, 1)));
    str_iterator_cls->giveAttr("next", new BoxedFunction(boxRTFunction((void*)BoxedStringIterator::next, STR, 1)));
    str_iterator_cls->freeze();

    str_cls->giveAttr("__name__", boxStrConstant("str"));

    str_cls->giveAttr("__len__", new BoxedFunction(boxRTFunction((void*)strLen, BOXED_INT, 1)));
    str_cls->giveAttr("__str__", new BoxedFunction(boxRTFunction((void*)strStr, STR, 1)));
    str_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)strRepr, STR, 1)));
    str_cls->giveAttr("__hash__", new BoxedFunction(boxRTFunction((void*)strHash, BOXED_INT, 1)));
    str_cls->giveAttr("__nonzero__", new BoxedFunction(boxRTFunction((void*)strNonzero, BOXED_BOOL, 1)));

    str_cls->giveAttr("capitalize", new BoxedFunction(boxRTFunction((void*)strCapitalize, STR, 1)));
    str_cls->giveAttr("center", new BoxedFunction(boxRTFunction((void*)strCenter, STR, 3, 1, false, false), { None }));
    str_cls->giveAttr("count",
                      new BoxedFunction(boxRTFunction((void*)strCount, BOXED_INT, 4, 2, true, false), { None, None }));
    // str.decode([encoding[, errors]])
    // str.encode([encoding[, errors]])
    str_cls->giveAttr("endswith", new BoxedFunction(boxRTFunction((void*)strEndswith, BOXED_BOOL, 4, 2, true, false),
                                                    { None, None }));
    str_cls->giveAttr("expandtabs",
                      new BoxedFunction(boxRTFunction((void*)strExpandtabs, STR, 2, 1, false, false), { boxInt(8) }));

    str_cls->giveAttr("find",
                      new BoxedFunction(boxRTFunction((void*)strFind, BOXED_INT, 4, 2, true, false), { None, None }));
    // str.format(*args, **kwargs)
    str_cls->giveAttr("index",
                      new BoxedFunction(boxRTFunction((void*)strIndex, BOXED_INT, 4, 2, true, false), { None, None }));

    str_cls->giveAttr("isalnum", new BoxedFunction(boxRTFunction((void*)strIsAlnum, STR, 1)));
    str_cls->giveAttr("isalpha", new BoxedFunction(boxRTFunction((void*)strIsAlpha, STR, 1)));
    str_cls->giveAttr("isdigit", new BoxedFunction(boxRTFunction((void*)strIsDigit, STR, 1)));
    str_cls->giveAttr("islower", new BoxedFunction(boxRTFunction((void*)strIsLower, STR, 1)));
    str_cls->giveAttr("isspace", new BoxedFunction(boxRTFunction((void*)strIsSpace, STR, 1)));
    str_cls->giveAttr("istitle", new BoxedFunction(boxRTFunction((void*)strIsTitle, STR, 1)));
    str_cls->giveAttr("isupper", new BoxedFunction(boxRTFunction((void*)strIsUpper, STR, 1)));
    str_cls->giveAttr("join", new BoxedFunction(boxRTFunction((void*)strJoin, STR, 2)));

    str_cls->giveAttr("ljust", new BoxedFunction(boxRTFunction((void*)strLjust, STR, 3, 1, false, false), { None }));

    str_cls->giveAttr("lower", new BoxedFunction(boxRTFunction((void*)strLower, STR, 1)));

    str_cls->giveAttr("lstrip", new BoxedFunction(boxRTFunction((void*)strLStrip, STR, 2, 1, false, false), { None }));

    str_cls->giveAttr("partition", new BoxedFunction(boxRTFunction((void*)strPartition, STR, 2)));

    str_cls->giveAttr("replace",
                      new BoxedFunction(boxRTFunction((void*)strReplace, STR, 4, 1, false, false), { None }));

    str_cls->giveAttr("rfind",
                      new BoxedFunction(boxRTFunction((void*)strRfind, BOXED_INT, 4, 2, true, false), { None, None }));
    str_cls->giveAttr("rindex",
                      new BoxedFunction(boxRTFunction((void*)strRindex, BOXED_INT, 4, 2, true, false), { None, None }));
    str_cls->giveAttr("rjust", new BoxedFunction(boxRTFunction((void*)strRjust, STR, 3, 1, false, false), { None }));
    str_cls->giveAttr("rpartition", new BoxedFunction(boxRTFunction((void*)strRpartition, STR, 2)));

    str_cls->giveAttr("rsplit",
                      new BoxedFunction(boxRTFunction((void*)strRsplit, LIST, 3, 2, false, false), { None, None }));
    str_cls->giveAttr("rstrip", new BoxedFunction(boxRTFunction((void*)strRStrip, STR, 2, 1, false, false), { None }));
    str_cls->giveAttr("split",
                      new BoxedFunction(boxRTFunction((void*)strSplit, LIST, 3, 2, false, false), { None, None }));

    str_cls->giveAttr("splitlines",
                      new BoxedFunction(boxRTFunction((void*)strSplitlines, LIST, 2, 1, false, false), { False }));

    str_cls->giveAttr(
        "startswith",
        new BoxedFunction(boxRTFunction((void*)strStartswith, BOXED_BOOL, 4, 2, true, false), { None, None }));

    str_cls->giveAttr("strip", new BoxedFunction(boxRTFunction((void*)strStrip, STR, 2, 1, false, false), { None }));

    str_cls->giveAttr("swapcase", new BoxedFunction(boxRTFunction((void*)strSwapcase, STR, 1)));

    str_cls->giveAttr("title", new BoxedFunction(boxRTFunction((void*)strTitle, STR, 1)));
    str_cls->giveAttr("translate",
                      new BoxedFunction(boxRTFunction((void*)strTranslate, STR, 3, 1, false, false), { None }));
    str_cls->giveAttr("upper", new BoxedFunction(boxRTFunction((void*)strUpper, STR, 1)));
    str_cls->giveAttr("zfill", new BoxedFunction(boxRTFunction((void*)strZfill, STR, 2)));

    str_cls->giveAttr("__contains__", new BoxedFunction(boxRTFunction((void*)strContains, BOXED_BOOL, 2)));

    str_cls->giveAttr("__add__", new BoxedFunction(boxRTFunction((void*)strAdd, UNKNOWN, 2)));
    str_cls->giveAttr("__mod__", new BoxedFunction(boxRTFunction((void*)strMod, STR, 2)));
    str_cls->giveAttr("__mul__", new BoxedFunction(boxRTFunction((void*)strMul, UNKNOWN, 2)));

    str_cls->giveAttr("__lt__", new BoxedFunction(boxRTFunction((void*)strLt, UNKNOWN, 2)));
    str_cls->giveAttr("__le__", new BoxedFunction(boxRTFunction((void*)strLe, UNKNOWN, 2)));
    str_cls->giveAttr("__gt__", new BoxedFunction(boxRTFunction((void*)strGt, UNKNOWN, 2)));
    str_cls->giveAttr("__ge__", new BoxedFunction(boxRTFunction((void*)strGe, UNKNOWN, 2)));
    str_cls->giveAttr("__eq__", new BoxedFunction(boxRTFunction((void*)strEq, UNKNOWN, 2)));
    str_cls->giveAttr("__ne__", new BoxedFunction(boxRTFunction((void*)strNe, UNKNOWN, 2)));

    str_cls->giveAttr("__getitem__", new BoxedFunction(boxRTFunction((void*)strGetitem, STR, 2)));
    str_cls->giveAttr("__getslice__", new BoxedFunction(boxRTFunction((void*)strSlice, STR, 3)));

    str_cls->giveAttr("__iter__", new BoxedFunction(boxRTFunction((void*)strIter, typeFromClass(str_iterator_cls), 1)));

    str_cls->giveAttr("__new__", new BoxedFunction(boxRTFunction((void*)strNew, UNKNOWN, 2, 1, false, false),
                                                   { boxStrConstant("") }));

    str_cls->freeze();
}

void teardownStr() {
}
}
