# Copyright (c) 2014 Dropbox, Inc.
# 
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
# 
#    http://www.apache.org/licenses/LICENSE-2.0
# 
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

#!/usr/bin/env python

import cPickle
import datetime
import functools
import getopt
import glob
import os
import Queue
import re
import resource
import signal
import subprocess
import sys
import tempfile
import threading
import time

NUM_THREADS = 1
IMAGE = "pyston_dbg"
KEEP_GOING = False
FN_JUST_SIZE = 20
EXTRA_JIT_ARGS = []
TIME_LIMIT = 2

def set_ulimits():
    # Guard the process from running too long with a hard rlimit.
    # But first try to kill it after a second with a SIGALRM, though that's catchable/clearable by the program:
    signal.alarm(TIME_LIMIT)
    resource.setrlimit(resource.RLIMIT_CPU, (TIME_LIMIT + 1, TIME_LIMIT + 1))

    MAX_MEM_MB = 100
    resource.setrlimit(resource.RLIMIT_AS, (MAX_MEM_MB * 1024 * 1024, MAX_MEM_MB * 1024 * 1024))

def get_expected_output(fn):
    sys.stdout.flush()
    assert fn.endswith(".py")
    expected_fn = fn[:-3] + ".expected"
    if os.path.exists(expected_fn):
        return 0, open(expected_fn).read(), ""

    cache_fn = fn[:-3] + ".expected_cache"
    if os.path.exists(cache_fn):
        if os.stat(cache_fn).st_mtime > os.stat(fn).st_mtime:
            try:
                return cPickle.load(open(cache_fn))
            except EOFError:
                pass

    # TODO don't suppress warnings globally:
    p = subprocess.Popen(["python", "-Wignore", fn], stdout=subprocess.PIPE, stderr=subprocess.PIPE, stdin=open("/dev/null"), preexec_fn=set_ulimits)
    out, err = p.communicate()
    code = p.wait()

    r = code, out, err.strip().split('\n')[-1]
    assert code >= 0, "CPython exited with an unexpected exit code: %d" % (code,)

    cPickle.dump(r, open(cache_fn, 'w'))
    return r

failed = []
def run_test(fn, check_stats, run_memcheck):
    r = fn.rjust(FN_JUST_SIZE)

    statchecks = []
    jit_args = ["-csr"] + EXTRA_JIT_ARGS
    expected = "success"
    for l in open(fn):
        if not l.startswith("#"):
            break
        if l.startswith("# statcheck:"):
            l = l[len("# statcheck:"):].strip()
            statchecks.append(l)
        elif l.startswith("# run_args:"):
            l = l[len("# run_args:"):].split()
            jit_args += l
        elif l.startswith("# expected:"):
            expected = l[len("# run_args:"):].strip()

    assert expected in ("success", "fail"), expected

    run_args = ["./%s" % IMAGE] + jit_args + ["-q", fn]
    start = time.time()
    p = subprocess.Popen(run_args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, stdin=open("/dev/null"), preexec_fn=set_ulimits)
    out, err = p.communicate()
    full_err = err
    err = err.strip().split('\n')[-1]

    code = p.wait()
    elapsed = time.time() - start

    stats = {}
    if code == 0:
        assert out.count("Stats:") == 1
        out, stats_str = out.split("Stats:")
        for l in stats_str.strip().split('\n'):
            k, v = l.split(':')
            stats[k.strip()] = int(v)

    expected_code, expected_out, expected_err = get_expected_output(fn)
    if code != expected_code:
        color = 31 # red

        if code == 0:
            err = "(Unexpected success)"

        if code == -signal.SIGALRM:
            msg = "Timed out"
            color = 33 # yellow
        elif code == -signal.SIGKILL:
            msg = "Killed!"
        else:
            msg = "Exited with code %d (expected code %d)" % (code, expected_code)

        if expected == "fail":
            r += "    Expected failure (got code %d, should be %d)" % (code, expected_code)
            return r
        else:
            if KEEP_GOING:
                r += "    \033[%dmFAILED\033[0m (%s)" % (color, msg)
                failed.append(fn)
                return r
            else:
                raise Exception("%s\n%s\n%s" % (msg, err, full_err))
    elif out != expected_out:
        if expected == "fail":
            r += "    Expected failure (bad output)"
            return r
        else:
            if KEEP_GOING:
                r += "    \033[31mFAILED\033[0m (bad output)"
                failed.append(fn)
                return r
            exp_fd, exp_fn = tempfile.mkstemp()
            out_fd, out_fn = tempfile.mkstemp()
            os.fdopen(exp_fd, 'w').write(expected_out)
            os.fdopen(out_fd, 'w').write(out)
            p = subprocess.Popen(["diff", "-a", exp_fn, out_fn], stdout=subprocess.PIPE, preexec_fn=set_ulimits)
            diff = p.stdout.read()
            assert p.wait() in (0, 1)
            raise Exception("Failed on %s:\n%s" % (fn, diff))
    elif err != expected_err:
        if KEEP_GOING:
            r += "    \033[31mFAILED\033[0m (bad stderr)"
            failed.append(fn)
            return r
        else:
            raise Exception((err, expected_err))
    elif expected == "fail":
        if KEEP_GOING:
            r += "    \033[31mFAILED\033[0m (unexpected success)"
            failed.append(fn)
            return r
        raise Exception("Unexpected success on %s" % fn)

    r += "    Correct output (%5.1fms)" % (elapsed * 1000,)

    if check_stats:
        for l in statchecks:
            test = eval(l)
            if not test:
                if KEEP_GOING:
                    r += "    \033[31mFailed statcheck\033[0m"
                    failed.append(fn)
                    return r
                else:
                    m = re.match("""stats\[['"]([\w_]+)['"]]""", l)
                    if m:
                        statname = m.group(1)
                        raise Exception((l, statname, stats[statname]))
                    raise Exception((l, stats))
    else:
        r += "    (ignoring stats)"

    if run_memcheck:
        if code == 0:
            start = time.time()
            p = subprocess.Popen(["valgrind", "--tool=memcheck", "--leak-check=no"] + run_args, stdout=open("/dev/null", 'w'), stderr=subprocess.PIPE, stdin=open("/dev/null"))
            out, err = p.communicate()
            assert p.wait() == 0
            if "Invalid read" not in err:
                elapsed = (time.time() - start)
                r += "    Memcheck passed (%4.1fs)" % (elapsed,)
            else:
                if KEEP_GOING:
                    r += "    \033[31mMEMCHECKS FAILED\033[0m"
                    failed.append(fn)
                    return r
                else:
                    raise Exception(err)
        else:
            r += "    (Skipping memchecks)   "

    return r

q = Queue.Queue()
cv = threading.Condition()
results = {}
quit = {}
def worker_thread():
    while not quit:
        try:
            job = q.get()
            if job is None:
                break

            results[job[0]] = run_test(*job)
            with cv:
                cv.notifyAll()
        except:
            import traceback
            # traceback.print_exc()
            results[job[0]] = None
            quit[job[0]] = job[0] + ':\n' + traceback.format_exc()
            with cv:
                cv.notifyAll()
            # os._exit(-1)

def verify_include(_, dir, files):
    for bn in files:
        fn = os.path.join(dir, bn)

        if not bn.endswith(".h"):
            continue

        expected_guard = "PYSTON" + fn[1:-2].replace('_', '').replace('/', '_').upper() + "_H"
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
            assert "Copyright (c) 2014 Dropbox, Inc." in s, fn
            assert "Apache License, Version 2.0" in s, fn

def fileSize(fn):
    return os.stat(fn).st_size
    # return len(list(open(fn)))

if __name__ == "__main__":
    os.path.walk('.', verify_include, None)
    os.path.walk('.', verify_license, None)
    os.path.walk('../tools', verify_license, None)

    run_memcheck = False
    start = 1

    opts, patterns = getopt.getopt(sys.argv[1:], "j:a:t:mRPk")
    for (t, v) in opts:
        if t == '-m':
            run_memcheck = True
        elif t == '-j':
            NUM_THREADS = int(v)
            assert NUM_THREADS > 0
        elif t == '-R':
            IMAGE = "pyston"
        elif t == '-P':
            IMAGE = "pyston_prof"
        elif t == '-k':
            KEEP_GOING = True
        elif t == '-a':
            EXTRA_JIT_ARGS.append(v)
        elif t == '-t':
            TIME_LIMIT = int(v)
        else:
            raise Exception((t, v))

    TEST_DIR = patterns[0]
    assert os.path.isdir(TEST_DIR), "%s doesn't look like a directory with tests in it" % TEST_DIR

    patterns = patterns[1:]

    TOSKIP = ["%s/%s.py" % (TEST_DIR, i) for i in (
        "tuple_depth",
        "longargs_stackusage",
            )]

    IGNORE_STATS = ["%s/%d.py" % (TEST_DIR, i) for i in (
        )] + [
        ]

    def _addto(l, tests):
        if isinstance(tests, str):
            tests = [tests]
        for t in tests:
            l.append("%s/%s.py" % (TEST_DIR, t))
    skip = functools.partial(_addto, TOSKIP)
    nostat = functools.partial(_addto, IGNORE_STATS)

    if '-O' in EXTRA_JIT_ARGS:
        # OSR tests, doesn't make sense with -O
        skip(["30", "listcomp_osr"])

    if datetime.datetime.now() < datetime.datetime(2014,4,29):
        nostat(["nondirectly_callable_ics"]) # WIP
        skip(["finalization_cycles", "resurrection", "nonself_resurrection"]) # WIP

    if datetime.datetime.now() < datetime.datetime(2014,4,29):
        skip(["class_noctor", "non_function_ctors"]) # object.__new__
        skip(["setattr_patching_under"]) # requires __del__

    if datetime.datetime.now() < datetime.datetime(2014,5,1):
        # varargs
        skip([57, 61])
        # arbitrary stuff in classes
        skip([56, "classdef_arbitrary", "scoping_classes", "return_in_class"])
        # sequence comparisons
        skip(["comparisons_more"])

    if datetime.datetime.now() < datetime.datetime(2014,5,1):
        # Metaclass tests
        skip([46, 55])

    if datetime.datetime.now() < datetime.datetime(2014,5,1):
        # random tests that aren't too important right now:
        skip([54, 65, 66, 69, 70, 72, "many_attrs_setattr", "for_iter", "none_not_settable", "math_more", "global_and_local", "metaclass_parent", "class_freeing_time", "xrange", "binops_subclass", "class_changing"])

    if not patterns:
        skip(["t", "t2"])

    def tryParse(s):
        if s.isdigit():
            return int(s)
        return s
    def key(name):
        i = tryParse(name)
        if i < start:
            return i + 100000
        return i

    tests = sorted([t for t in glob.glob("%s/*.py" % TEST_DIR)], key=lambda t:key(t[6:-3]))
    tests += [
            ]
    big_tests = [
            ]
    tests += big_tests

    for t in TOSKIP:
        assert t in ("%s/t.py" % TEST_DIR, "%s/t2.py" % TEST_DIR) or t in tests, t

    if patterns:
        filtered_tests = []
        for t in tests:
            if any(re.match("%s/%s.*\.py" % (TEST_DIR, p), t) for p in patterns):
                filtered_tests.append(t)
        tests = filtered_tests
    if not tests:
        print >>sys.stderr, "No tests specified!"
        sys.exit(1)

    FN_JUST_SIZE = max(20, 2 + max(map(len, tests)))

    print "Building...",
    sys.stdout.flush()
    subprocess.check_call(["make", "-j4", IMAGE], stdout=open("/dev/null", 'w'), stderr=subprocess.PIPE)
    print "done"

    if not patterns:
        tests.sort(key=fileSize)

    for fn in tests:
        if fn in TOSKIP:
            continue
        check_stats = fn not in IGNORE_STATS
        q.put((fn, check_stats, run_memcheck))

    threads = []
    for i in xrange(NUM_THREADS):
        t = threading.Thread(target=worker_thread)
        t.setDaemon(True)
        t.start()
        threads.append(t)
        q.put(None)

    for fn in tests:
        if fn in TOSKIP:
            print fn.rjust(FN_JUST_SIZE),
            print "   Skipping"
            continue

        with cv:
            while fn not in results:
                try:
                    cv.wait(1)
                except KeyboardInterrupt:
                    print >>sys.stderr, "Interrupted"
                    sys.exit(1)

        if results[fn] is None:
            assert quit
            print quit.pop(fn).strip()
            for fn, s in quit.items():
                print "(%s also failed)" % fn
            sys.exit(1)
            break
        print results[fn]

    for t in threads:
        t.join()

    if failed:
        sys.exit(1)
