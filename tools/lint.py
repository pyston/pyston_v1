import os
import sys
import re

def file_is_from_cpython(fn):
    if 'from_cpython' in fn:
        return True

    if fn.endswith("/thread_pthread.h"):
        return True

    return False

def verify_include_guard(_, dir, files):
    for bn in files:
        fn = os.path.join(dir, bn)

        if not bn.endswith(".h"):
            continue

        if file_is_from_cpython(fn):
            continue

        expected_guard = "PYSTON" + fn[1:-2].replace('_', '').replace('-', '').replace('/', '_').upper() + "_H"
        with open(fn) as f:
            while True:
                l = f.readline()
                if l.startswith('//') or not l.strip():
                    continue
                break
        gotten_guard = l.split()[1]
        assert gotten_guard == expected_guard, (fn, gotten_guard, expected_guard)

def verify_license(_, dir, files):
    for bn in files:
        fn = os.path.join(dir, bn)

        if bn.endswith(".h") or bn.endswith(".cpp"):
            s = open(fn).read(1024)
            if file_is_from_cpython(fn):
                assert "This file is originally from CPython 2.7, with modifications for Pyston" in s, fn
            elif fn.endswith("/astprint.cpp"):
                continue
            else:
                assert "Copyright (c) 2014-2015 Dropbox, Inc." in s, fn
                assert "Apache License, Version 2.0" in s, fn

PYSTON_SRC_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "../src"))
PYSTON_SRC_SUBDIRS = [bn for bn in os.listdir(PYSTON_SRC_DIR) if os.path.isdir(os.path.join(PYSTON_SRC_DIR, bn))]
CAPI_HEADER_DIR = os.path.join(PYSTON_SRC_DIR, "../from_cpython/Include")
CAPI_HEADERS = [bn for bn in os.listdir(CAPI_HEADER_DIR) if bn.endswith(".h")]

include_re = re.compile(r'^\#include \s+ (<.+?>|".+?") (?:\s+ //.*)? $', re.VERBOSE)

def verify_include_order(_, dir, files):
    for bn in files:
        fn = os.path.join(dir, bn)

        if not bn.endswith(".cpp") and not bn.endswith('.h'):
            continue

        section = None
        sections = []
        with open(fn) as f:
            for l in f:
                l = l.strip()
                if l.startswith("//"):
                    if "lint: allow-unsorted-includes" in l:
                        break
                    continue
                if not l:
                    if section:
                        sections.append(section)
                        section = None
                    continue

                m = include_re.match(l)
                if m:
                    # Python-ast.h is a tricky include file since it 1) doesn't have include guards, and 2)
                    # it doesn't include its dependencies.  Let it (and cpython_ast.h which includes it)
                    # avoid the lint rules.
                    if m.group(1) == '"Python-ast.h"' or m.group(1) == '"cpython_ast.h"':
                        continue

                    if section is None:
                        section = []
                    section.append(m.group(1))
                    continue

                if l.startswith("#ifndef PYSTON") or l.startswith("#define PYSTON"):
                    assert not section
                    assert not sections
                    continue

                break

        def is_corresponding_header(section):
            if len(section) != 1:
                return False
            incl = section[0]
            if not incl.endswith('"'):
                return False

            included = incl.split('"')[1]
            return (included[:-1] + "cpp") in fn

        def is_system_section(section):
            return all(incl.endswith('>') for incl in section)

        def is_third_party_section(section):
            for incl in section:
                if incl.startswith('"llvm/'):
                    continue
                if '"opagent.h"' in incl:
                    continue

                matches_capi_header = False
                for h in CAPI_HEADERS:
                    if '"%s"' % h in incl:
                        matches_capi_header = True
                        break
                if matches_capi_header:
                    continue

                return False
            return True

        def is_pyston_section(section):
            # TODO generate this
            include_dirs = PYSTON_SRC_SUBDIRS
            for incl in section:
                if not any(incl.startswith('"%s/' % d) for d in include_dirs):
                    return False
            return True

        def check_sorted(section):
            section = [incl.lower() for incl in section]
            if section != list(sorted(section)):
                print >>sys.stderr, "The following section is not sorted in %s:" % fn
                print >>sys.stderr, '\n'.join(section)
                sys.exit(1)
            assert len(sections[0]) == len(set(sections[0]))

        dbg = []
        if sections and is_corresponding_header(sections[0]):
            dbg.append("corresponding")
            del sections[0]

        if sections and is_system_section(sections[0]):
            dbg.append("system")
            check_sorted(sections[0])
            del sections[0]

        while sections and is_third_party_section(sections[0]):
            dbg.append("3rdp")
            check_sorted(sections[0])
            del sections[0]

        if sections and is_pyston_section(sections[0]):
            dbg.append("pyston")
            check_sorted(sections[0])
            for incl in sections[0]:
                if is_corresponding_header([incl]):
                    print >>sys.stderr, "Include-order error in %s:" % fn
                    print >>sys.stderr, "%r should be put first" % incl
                    sys.exit(1)
            del sections[0]

        if sections:
            print >>sys.stderr, "Include-order error in %s:" % fn
            print >>sys.stderr, "Sections not appropriately grouped.  Should be:"
            print >>sys.stderr, "- Corresponding header"
            print >>sys.stderr, "- System headers"
            print >>sys.stderr, "- Third party headers"
            print >>sys.stderr, "- Pyston headers"
            print >>sys.stderr, "There should be an extra line between sections but not within sections"
            print >>sys.stderr, "\ndbg: %s" % dbg
            sys.exit(1)
        assert not sections, fn


if __name__ == "__main__":
    os.path.walk('.', verify_include_guard, None)
    os.path.walk('.', verify_include_order, None)
    os.path.walk('.', verify_license, None)
    os.path.walk('../tools', verify_license, None)
    print "Lint checks passed"

