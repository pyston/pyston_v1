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

extern "C" BoxedString* strMul(BoxedString* lhs, BoxedInt* rhs) {
    assert(lhs->cls == str_cls);
    assert(rhs->cls == int_cls);

    RELEASE_ASSERT(rhs->n >= 0, "");

    int sz = lhs->s.size();
    int n = rhs->n;
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

Box* strLower(BoxedString* self) {
    assert(self->cls == str_cls);

    std::string lowered(self->s);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), tolower);
    return boxString(std::move(lowered));
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

Box* strSplit(BoxedString* self, BoxedString* sep) {
    assert(self->cls == str_cls);

    if (sep->cls == str_cls) {
        if (!sep->s.empty()) {
            llvm::SmallVector<llvm::StringRef, 16> parts;
            llvm::StringRef(self->s).split(parts, sep->s);

            BoxedList* rtn = new BoxedList();
            for (const auto& s : parts)
                listAppendInternal(rtn, boxString(s.str()));
            return rtn;
        } else {
            raiseExcHelper(ValueError, "empty separator");
        }
    } else if (sep->cls == none_cls) {
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

    str_cls->giveAttr("lower", new BoxedFunction(boxRTFunction((void*)strLower, STR, 1)));

    str_cls->giveAttr("strip", new BoxedFunction(boxRTFunction((void*)strStrip, STR, 2, 1, false, false), { None }));

    str_cls->giveAttr("lstrip", new BoxedFunction(boxRTFunction((void*)strLStrip, STR, 2, 1, false, false), { None }));

    str_cls->giveAttr("rstrip", new BoxedFunction(boxRTFunction((void*)strRStrip, STR, 2, 1, false, false), { None }));

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

    str_cls->giveAttr("__iter__", new BoxedFunction(boxRTFunction((void*)strIter, typeFromClass(str_iterator_cls), 1)));

    str_cls->giveAttr("join", new BoxedFunction(boxRTFunction((void*)strJoin, STR, 2)));

    str_cls->giveAttr("split", new BoxedFunction(boxRTFunction((void*)strSplit, LIST, 2, 1, false, false), { None }));
    str_cls->giveAttr("rsplit", str_cls->getattr("split"));

    CLFunction* count = boxRTFunction((void*)strCount2Unboxed, INT, 2);
    addRTFunction(count, (void*)strCount2, BOXED_INT);
    str_cls->giveAttr("count", new BoxedFunction(count));

    str_cls->giveAttr("__new__", new BoxedFunction(boxRTFunction((void*)strNew, UNKNOWN, 2, 1, false, false),
                                                   { boxStrConstant("") }));

    str_cls->freeze();
}

void teardownStr() {
}
}
