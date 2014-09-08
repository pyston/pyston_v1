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
#include "core/util.h"
#include "gc/collector.h"
#include "runtime/dict.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"
#include "runtime/util.h"

namespace pyston {

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

    BoxedDict* dict = NULL;
    if (rhs->cls == dict_cls)
        dict = static_cast<BoxedDict*>(rhs);

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

                Box* val_to_use = NULL;
                if (*fmt == '(') {
                    if (dict == NULL)
                        raiseExcHelper(TypeError, "format requires a mapping");

                    int pcount = 1;
                    fmt++;
                    const char* keystart = fmt;

                    while (pcount > 0 && fmt < fmt_end) {
                        char c = *fmt;
                        if (c == ')')
                            pcount--;
                        else if (c == '(')
                            pcount++;
                        fmt++;
                    }

                    if (pcount > 0)
                        raiseExcHelper(ValueError, "incomplete format key");

                    BoxedString* key = boxStrConstantSize(keystart, fmt - keystart - 1);
                    val_to_use = dictGetitem(dict, key);
                }

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

                    if (!val_to_use) {
                        if (elt_num >= num_elts)
                            raiseExcHelper(TypeError, "not enough arguments for format string");
                        val_to_use = (*elts)[elt_num];
                        elt_num++;
                    }

                    BoxedString* s = str(val_to_use);
                    os << s->s;
                    break;
                } else if (c == 'c') {
                    if (!val_to_use) {
                        if (elt_num >= num_elts)
                            raiseExcHelper(TypeError, "not enough arguments for format string");
                        val_to_use = (*elts)[elt_num];
                        elt_num++;
                    }

                    RELEASE_ASSERT(val_to_use->cls == int_cls, "unsupported");
                    RELEASE_ASSERT(nspace == 0, "unsupported");
                    RELEASE_ASSERT(ndot == 0, "unsupported");
                    RELEASE_ASSERT(nzero == 0, "unsupported");

                    int64_t n = static_cast<BoxedInt*>(val_to_use)->n;
                    if (n < 0)
                        raiseExcHelper(OverflowError, "unsigned byte integer is less than minimum");
                    if (n >= 256)
                        raiseExcHelper(OverflowError, "unsigned byte integer is greater than maximum");
                    os << (char)n;
                    break;
                } else if (c == 'd' || c == 'i') {
                    if (!val_to_use) {
                        if (elt_num >= num_elts)
                            raiseExcHelper(TypeError, "not enough arguments for format string");
                        val_to_use = (*elts)[elt_num];
                        elt_num++;
                    }

                    RELEASE_ASSERT(val_to_use->cls == int_cls, "unsupported");

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
                    snprintf(buf, 20, fmt.str().c_str(), static_cast<BoxedInt*>(val_to_use)->n);
                    os << std::string(buf);
                    break;
                } else if (c == 'f') {
                    if (!val_to_use) {
                        if (elt_num >= num_elts)
                            raiseExcHelper(TypeError, "not enough arguments for format string");
                        val_to_use = (*elts)[elt_num];
                        elt_num++;
                    }

                    double d;
                    if (val_to_use->cls == float_cls) {
                        d = static_cast<BoxedFloat*>(val_to_use)->d;
                    } else if (val_to_use->cls == int_cls) {
                        d = static_cast<BoxedInt*>(val_to_use)->n;
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

    if (dict == NULL && elt_num < num_elts) {
        raiseExcHelper(TypeError, "not all arguments converted during string formatting");
    }

    return boxString(os.str());
}

extern "C" Box* strMul(BoxedString* lhs, Box* rhs) {
    assert(lhs->cls == str_cls);

    int n;
    if (isSubclass(rhs->cls, int_cls))
        n = static_cast<BoxedInt*>(rhs)->n;
    else if (isSubclass(rhs->cls, bool_cls))
        n = static_cast<BoxedBool*>(rhs)->b;
    else
        return NotImplemented;

    // TODO: use createUninitializedString and getWriteableStringContents
    int sz = lhs->s.size();
    char* buf = new char[sz * n + 1];
    for (int i = 0; i < n; i++) {
        memcpy(buf + (sz * i), lhs->s.c_str(), sz);
    }
    buf[sz * n] = '\0';

    return new BoxedString(buf);
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

extern "C" Box* basestringNew(BoxedClass* cls, Box* args, Box* kwargs) {
    raiseExcHelper(TypeError, "The basestring type cannot be instantiated");
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

    const std::string& str(self->s);
    if (str.empty())
        return False;

    for (const auto& c : str) {
        if (!std::isalpha(c))
            return False;
    }

    return True;
}

Box* strIsDigit(BoxedString* self) {
    assert(self->cls == str_cls);

    const std::string& str(self->s);
    if (str.empty())
        return False;

    for (const auto& c : str) {
        if (!std::isdigit(c))
            return False;
    }

    return True;
}

Box* strIsAlnum(BoxedString* self) {
    assert(self->cls == str_cls);

    const std::string& str(self->s);
    if (str.empty())
        return False;

    for (const auto& c : str) {
        if (!std::isalnum(c))
            return False;
    }

    return True;
}

Box* strIsLower(BoxedString* self) {
    assert(self->cls == str_cls);

    const std::string& str(self->s);
    bool lowered = false;

    if (str.empty())
        return False;

    for (const auto& c : str) {
        if (std::isspace(c) || std::isdigit(c)) {
            continue;
        } else if (!std::islower(c)) {
            return False;
        } else {
            lowered = true;
        }
    }

    return boxBool(lowered);
}

Box* strIsUpper(BoxedString* self) {
    assert(self->cls == str_cls);

    const std::string& str(self->s);
    bool uppered = false;

    if (str.empty())
        return False;

    for (const auto& c : str) {
        if (std::isspace(c) || std::isdigit(c)) {
            continue;
        } else if (!std::isupper(c)) {
            return False;
        } else {
            uppered = true;
        }
    }

    return boxBool(uppered);
}

Box* strIsSpace(BoxedString* self) {
    assert(self->cls == str_cls);

    const std::string& str(self->s);
    if (str.empty())
        return False;

    for (const auto& c : str) {
        if (!std::isspace(c))
            return False;
    }

    return True;
}

Box* strIsTitle(BoxedString* self) {
    assert(self->cls == str_cls);

    const std::string& str(self->s);

    if (str.empty())
        return False;
    if (str.size() == 1)
        return boxBool(std::isupper(str[0]));

    bool cased = false, start_of_word = true;

    for (const auto& c : str) {
        if (std::isupper(c)) {
            if (!start_of_word) {
                return False;
            }

            start_of_word = false;
            cased = true;
        } else if (std::islower(c)) {
            if (start_of_word) {
                return False;
            }

            start_of_word = false;
            cased = true;
        } else {
            start_of_word = true;
        }
    }

    return boxBool(cased);
}

Box* strJoin(BoxedString* self, Box* rhs) {
    assert(self->cls == str_cls);

    if (rhs->cls == list_cls) {
        BoxedList* list = static_cast<BoxedList*>(rhs);
        std::ostringstream os;
        for (int i = 0; i < list->size; i++) {
            if (i > 0)
                os << self->s;
            BoxedString* elt_str = str(list->elts->elts[i]);
            os << elt_str->s;
        }
        return boxString(os.str());
    } else {
        raiseExcHelper(TypeError, "");
    }
}

Box* strReplace(Box* _self, Box* _old, Box* _new, Box** _args) {
    RELEASE_ASSERT(_self->cls == str_cls, "");
    BoxedString* self = static_cast<BoxedString*>(_self);

    RELEASE_ASSERT(_old->cls == str_cls, "");
    BoxedString* old = static_cast<BoxedString*>(_old);

    RELEASE_ASSERT(_new->cls == str_cls, "");
    BoxedString* new_ = static_cast<BoxedString*>(_new);

    Box* _count = _args[0];

    RELEASE_ASSERT(_count->cls == int_cls, "an integer is required");
    BoxedInt* count = static_cast<BoxedInt*>(_count);

    RELEASE_ASSERT(count->n < 0, "'count' argument unsupported");

    BoxedString* rtn = new BoxedString(self->s);
    // From http://stackoverflow.com/questions/2896600/how-to-replace-all-occurrences-of-a-character-in-string
    size_t start_pos = 0;
    while ((start_pos = rtn->s.find(old->s, start_pos)) != std::string::npos) {
        rtn->s.replace(start_pos, old->s.length(), new_->s);
        start_pos += new_->s.length(); // Handles case where 'to' is a substring of 'from'
    }
    return rtn;
}

Box* strSplit(BoxedString* self, BoxedString* sep, BoxedInt* _max_split) {
    assert(self->cls == str_cls);
    if (_max_split->cls != int_cls)
        raiseExcHelper(TypeError, "an integer is required");

    if (sep->cls == str_cls) {
        if (!sep->s.empty()) {
            llvm::SmallVector<llvm::StringRef, 16> parts;
            llvm::StringRef(self->s).split(parts, sep->s, _max_split->n);

            BoxedList* rtn = new BoxedList();
            for (const auto& s : parts)
                listAppendInternal(rtn, boxString(s.str()));
            return rtn;
        } else {
            raiseExcHelper(ValueError, "empty separator");
        }
    } else if (sep->cls == none_cls) {
        RELEASE_ASSERT(_max_split->n < 0, "this case hasn't been updated to handle limited splitting amounts");
        BoxedList* rtn = new BoxedList();

        std::ostringstream os("");
        for (char c : self->s) {
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
                if (os.tellp()) {
                    listAppendInternal(rtn, boxString(os.str()));
                    os.str("");
                }
            } else {
                os << c;
            }
        }
        if (os.tellp()) {
            listAppendInternal(rtn, boxString(os.str()));
        }
        return rtn;
    } else {
        raiseExcHelper(TypeError, "expected a character buffer object");
    }
}

Box* strRsplit(BoxedString* self, BoxedString* sep, BoxedInt* _max_split) {
    // TODO: implement this for real
    // for now, just forward rsplit() to split() in the cases they have to return the same value
    assert(_max_split->cls == int_cls);
    RELEASE_ASSERT(_max_split->n <= 0, "");
    return strSplit(self, sep, _max_split);
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

Box* strCapitalize(BoxedString* self) {
    assert(self->cls == str_cls);

    std::string s(self->s);

    for (auto& i : s) {
        i = std::tolower(i);
    }

    if (!s.empty()) {
        s[0] = std::toupper(s[0]);
    }

    return boxString(s);
}

Box* strTitle(BoxedString* self) {
    assert(self->cls == str_cls);

    std::string s(self->s);
    bool start_of_word = false;

    for (auto& i : s) {
        if (std::islower(i)) {
            if (!start_of_word) {
                i = std::toupper(i);
            }
            start_of_word = true;
        } else if (std::isupper(i)) {
            if (start_of_word) {
                i = std::tolower(i);
            }
            start_of_word = true;
        } else {
            start_of_word = false;
        }
    }
    return boxString(s);
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

    for (auto& i : s) {
        if (std::islower(i))
            i = std::toupper(i);
        else if (std::isupper(i))
            i = std::tolower(i);
    }

    return boxString(s);
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

Box* strStartswith(BoxedString* self, Box* elt) {
    if (self->cls != str_cls)
        raiseExcHelper(TypeError, "descriptor 'startswith' requires a 'str' object but received a '%s'",
                       getTypeName(elt)->c_str());

    if (elt->cls != str_cls)
        raiseExcHelper(TypeError, "expected a character buffer object");

    BoxedString* sub = static_cast<BoxedString*>(elt);

    return boxBool(startswith(self->s, sub->s));
}

Box* strEndswith(BoxedString* self, Box* elt) {
    if (self->cls != str_cls)
        raiseExcHelper(TypeError, "descriptor 'endswith' requires a 'str' object but received a '%s'",
                       getTypeName(elt)->c_str());

    if (elt->cls != str_cls)
        raiseExcHelper(TypeError, "expected a character buffer object");

    BoxedString* sub = static_cast<BoxedString*>(elt);

    return boxBool(endswith(self->s, sub->s));
}

Box* strFind(BoxedString* self, Box* elt) {
    if (self->cls != str_cls)
        raiseExcHelper(TypeError, "descriptor 'find' requires a 'str' object but received a '%s'",
                       getTypeName(elt)->c_str());

    if (elt->cls != str_cls)
        raiseExcHelper(TypeError, "expected a character buffer object");

    BoxedString* sub = static_cast<BoxedString*>(elt);

    size_t r = self->s.find(sub->s);
    if (r == std::string::npos)
        return boxInt(-1);
    return boxInt(r);
}

Box* strRfind(BoxedString* self, Box* elt) {
    if (self->cls != str_cls)
        raiseExcHelper(TypeError, "descriptor 'rfind' requires a 'str' object but received a '%s'",
                       getTypeName(elt)->c_str());

    if (elt->cls != str_cls)
        raiseExcHelper(TypeError, "expected a character buffer object");

    BoxedString* sub = static_cast<BoxedString*>(elt);

    size_t r = self->s.rfind(sub->s);
    if (r == std::string::npos)
        return boxInt(-1);
    return boxInt(r);
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


// TODO it looks like strings don't have their own iterators, but instead
// rely on the sequence iteration protocol.
// Should probably implement that, and maybe once that's implemented get
// rid of the striterator class?
BoxedClass* str_iterator_cls = NULL;

class BoxedStringIterator : public Box {
public:
    BoxedString* s;
    std::string::const_iterator it, end;

    BoxedStringIterator(BoxedString* s) : Box(str_iterator_cls), s(s), it(s->s.begin()), end(s->s.end()) {}

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

extern "C" void strIteratorGCHandler(GCVisitor* v, Box* b) {
    boxGCHandler(v, b);
    BoxedStringIterator* it = (BoxedStringIterator*)b;
    v->visit(it->s);
}

Box* strIter(BoxedString* self) {
    assert(self->cls == str_cls);
    return new BoxedStringIterator(self);
}

int64_t strCount2Unboxed(BoxedString* self, Box* elt) {
    assert(self->cls == str_cls);

    if (elt->cls != str_cls)
        raiseExcHelper(TypeError, "expected a character buffer object");

    const std::string& s = self->s;
    const std::string& pattern = static_cast<BoxedString*>(elt)->s;

    int found = 0;
    size_t start = 0;
    while (start < s.size()) {
        size_t next = s.find(pattern, start);
        if (next == std::string::npos)
            break;

        found++;
        start = next + pattern.size();
    }
    return found;
}

Box* strCount2(BoxedString* self, Box* elt) {
    return boxInt(strCount2Unboxed(self, elt));
}

extern "C" PyObject* PyString_FromString(const char* s) {
    return boxStrConstant(s);
}

BoxedString* createUninitializedString(ssize_t n) {
    // I *think* this should avoid doing any copies, by using move constructors:
    return new BoxedString(std::string(n, '\x00'));
}

char* getWriteableStringContents(BoxedString* s) {
    ASSERT(s->s.size() > 0, "not sure whether this is valid for strings with zero size");

    // After doing some reading, I think this is ok:
    // http://stackoverflow.com/questions/14290795/why-is-modifying-a-string-through-a-retrieved-pointer-to-its-data-not-allowed
    // In C++11, std::string is required to store its data contiguously.
    // It looks like it's also required to make it available to write via the [] operator.
    // - Taking a look at GCC's libstdc++, calling operator[] on a non-const string will return
    //   a writeable reference, and "unshare" the string.
    // So surprisingly, this looks ok!
    return &s->s[0];
}

extern "C" PyObject* PyString_FromStringAndSize(const char* s, ssize_t n) {
    if (s == NULL)
        return createUninitializedString(n);
    return boxStrConstantSize(s, n);
}

extern "C" char* PyString_AsString(PyObject* o) {
    RELEASE_ASSERT(o->cls == str_cls, "");

    BoxedString* s = static_cast<BoxedString*>(o);
    return getWriteableStringContents(s);
}

extern "C" Py_ssize_t PyString_Size(PyObject* s) {
    RELEASE_ASSERT(s->cls == str_cls, "");
    return static_cast<BoxedString*>(s)->s.size();
}

extern "C" int _PyString_Resize(PyObject** pv, Py_ssize_t newsize) {
    Py_FatalError("unimplemented");
}

static Py_ssize_t string_buffer_getreadbuf(PyObject* self, Py_ssize_t index, const void** ptr) {
    RELEASE_ASSERT(index == 0, "");
    // I think maybe this can just be a non-release assert?  shouldn't be able to call this with
    // the wrong type
    RELEASE_ASSERT(self->cls == str_cls, "");

    auto s = static_cast<BoxedString*>(self);
    *ptr = s->s.c_str();
    return s->s.size();
}

static Py_ssize_t string_buffer_getsegcount(PyObject* o, Py_ssize_t* lenp) {
    RELEASE_ASSERT(lenp == NULL, "");
    RELEASE_ASSERT(o->cls == str_cls, "");

    return 1;
}

static PyBufferProcs string_as_buffer = {
    (readbufferproc)string_buffer_getreadbuf, // comments are the only way I've found of
    (writebufferproc)NULL,                    // forcing clang-format to break these onto multiple lines
    (segcountproc)string_buffer_getsegcount,  //
    (charbufferproc)NULL,                     //
    (getbufferproc)NULL,                      //
    (releasebufferproc)NULL,
};

void setupStr() {
    str_iterator_cls = new BoxedClass(type_cls, object_cls, &strIteratorGCHandler, 0, sizeof(BoxedString), false);
    str_iterator_cls->giveAttr("__name__", boxStrConstant("striterator"));
    str_iterator_cls->giveAttr("__hasnext__",
                               new BoxedFunction(boxRTFunction((void*)BoxedStringIterator::hasnext, BOXED_BOOL, 1)));
    str_iterator_cls->giveAttr("next", new BoxedFunction(boxRTFunction((void*)BoxedStringIterator::next, STR, 1)));
    str_iterator_cls->freeze();

    str_cls->tp_as_buffer = &string_as_buffer;

    str_cls->giveAttr("__name__", boxStrConstant("str"));

    str_cls->giveAttr("__len__", new BoxedFunction(boxRTFunction((void*)strLen, BOXED_INT, 1)));
    str_cls->giveAttr("__str__", new BoxedFunction(boxRTFunction((void*)strStr, STR, 1)));
    str_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)strRepr, STR, 1)));
    str_cls->giveAttr("__hash__", new BoxedFunction(boxRTFunction((void*)strHash, BOXED_INT, 1)));
    str_cls->giveAttr("__nonzero__", new BoxedFunction(boxRTFunction((void*)strNonzero, BOXED_BOOL, 1)));

    str_cls->giveAttr("isalnum", new BoxedFunction(boxRTFunction((void*)strIsAlnum, STR, 1)));
    str_cls->giveAttr("isalpha", new BoxedFunction(boxRTFunction((void*)strIsAlpha, STR, 1)));
    str_cls->giveAttr("isdigit", new BoxedFunction(boxRTFunction((void*)strIsDigit, STR, 1)));
    str_cls->giveAttr("islower", new BoxedFunction(boxRTFunction((void*)strIsLower, STR, 1)));
    str_cls->giveAttr("isspace", new BoxedFunction(boxRTFunction((void*)strIsSpace, STR, 1)));
    str_cls->giveAttr("istitle", new BoxedFunction(boxRTFunction((void*)strIsTitle, STR, 1)));
    str_cls->giveAttr("isupper", new BoxedFunction(boxRTFunction((void*)strIsUpper, STR, 1)));

    str_cls->giveAttr("lower", new BoxedFunction(boxRTFunction((void*)strLower, STR, 1)));
    str_cls->giveAttr("swapcase", new BoxedFunction(boxRTFunction((void*)strSwapcase, STR, 1)));
    str_cls->giveAttr("upper", new BoxedFunction(boxRTFunction((void*)strUpper, STR, 1)));

    str_cls->giveAttr("strip", new BoxedFunction(boxRTFunction((void*)strStrip, STR, 2, 1, false, false), { None }));
    str_cls->giveAttr("lstrip", new BoxedFunction(boxRTFunction((void*)strLStrip, STR, 2, 1, false, false), { None }));
    str_cls->giveAttr("rstrip", new BoxedFunction(boxRTFunction((void*)strRStrip, STR, 2, 1, false, false), { None }));

    str_cls->giveAttr("capitalize", new BoxedFunction(boxRTFunction((void*)strCapitalize, STR, 1)));
    str_cls->giveAttr("title", new BoxedFunction(boxRTFunction((void*)strTitle, STR, 1)));

    str_cls->giveAttr("__contains__", new BoxedFunction(boxRTFunction((void*)strContains, BOXED_BOOL, 2)));

    str_cls->giveAttr("startswith", new BoxedFunction(boxRTFunction((void*)strStartswith, BOXED_BOOL, 2)));
    str_cls->giveAttr("endswith", new BoxedFunction(boxRTFunction((void*)strEndswith, BOXED_BOOL, 2)));

    str_cls->giveAttr("find", new BoxedFunction(boxRTFunction((void*)strFind, BOXED_INT, 2)));
    str_cls->giveAttr("rfind", new BoxedFunction(boxRTFunction((void*)strRfind, BOXED_INT, 2)));

    str_cls->giveAttr("__add__", new BoxedFunction(boxRTFunction((void*)strAdd, UNKNOWN, 2)));
    str_cls->giveAttr("__mod__", new BoxedFunction(boxRTFunction((void*)strMod, STR, 2)));
    str_cls->giveAttr("__mul__", new BoxedFunction(boxRTFunction((void*)strMul, UNKNOWN, 2)));
    // TODO not sure if this is right in all cases:
    str_cls->giveAttr("__rmul__", new BoxedFunction(boxRTFunction((void*)strMul, UNKNOWN, 2)));

    str_cls->giveAttr("__lt__", new BoxedFunction(boxRTFunction((void*)strLt, UNKNOWN, 2)));
    str_cls->giveAttr("__le__", new BoxedFunction(boxRTFunction((void*)strLe, UNKNOWN, 2)));
    str_cls->giveAttr("__gt__", new BoxedFunction(boxRTFunction((void*)strGt, UNKNOWN, 2)));
    str_cls->giveAttr("__ge__", new BoxedFunction(boxRTFunction((void*)strGe, UNKNOWN, 2)));
    str_cls->giveAttr("__eq__", new BoxedFunction(boxRTFunction((void*)strEq, UNKNOWN, 2)));
    str_cls->giveAttr("__ne__", new BoxedFunction(boxRTFunction((void*)strNe, UNKNOWN, 2)));

    str_cls->giveAttr("__getitem__", new BoxedFunction(boxRTFunction((void*)strGetitem, STR, 2)));

    str_cls->giveAttr("__iter__", new BoxedFunction(boxRTFunction((void*)strIter, typeFromClass(str_iterator_cls), 1)));

    str_cls->giveAttr("join", new BoxedFunction(boxRTFunction((void*)strJoin, STR, 2)));

    str_cls->giveAttr("replace",
                      new BoxedFunction(boxRTFunction((void*)strReplace, STR, 4, 1, false, false), { boxInt(-1) }));

    str_cls->giveAttr(
        "split", new BoxedFunction(boxRTFunction((void*)strSplit, LIST, 3, 2, false, false), { None, boxInt(-1) }));
    str_cls->giveAttr(
        "rsplit", new BoxedFunction(boxRTFunction((void*)strRsplit, LIST, 3, 2, false, false), { None, boxInt(-1) }));

    CLFunction* count = boxRTFunction((void*)strCount2Unboxed, INT, 2);
    addRTFunction(count, (void*)strCount2, BOXED_INT);
    str_cls->giveAttr("count", new BoxedFunction(count));

    str_cls->giveAttr("__new__", new BoxedFunction(boxRTFunction((void*)strNew, UNKNOWN, 2, 1, false, false),
                                                   { boxStrConstant("") }));

    str_cls->freeze();

    basestring_cls->giveAttr(
        "__doc__", boxStrConstant("Type basestring cannot be instantiated; it is the base for str and unicode."));
    basestring_cls->giveAttr("__new__",
                             new BoxedFunction(boxRTFunction((void*)basestringNew, UNKNOWN, 1, 0, true, true)));
    basestring_cls->giveAttr("__name__", boxStrConstant("basestring"));
    basestring_cls->freeze();
}

void teardownStr() {
}
}
