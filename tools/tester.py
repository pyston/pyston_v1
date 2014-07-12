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

# For fun, can test pypy.
# Tough because the tester will check to see if the error messages are exactly the
# same as the system CPython, but the error messages change over micro CPython versions.
# Pyston compile-time checks the system CPython version to try to give compatible error messages.
TEST_PYPY = 0

def set_ulimits():
    # Guard the process from running too long with a hard rlimit.
    # But first try to kill it after a second with a SIGALRM, though that's catchable/clearable by the program:
    signal.alarm(TIME_LIMIT)
    resource.setrlimit(resource.RLIMIT_CPU, (TIME_LIMIT + 1, TIME_LIMIT + 1))

    MAX_MEM_MB = 100
    resource.setrlimit(resource.RLIMIT_RSS, (MAX_MEM_MB * 1024 * 1024, MAX_MEM_MB * 1024 * 1024))

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

    r = code, out, err
    assert code >= 0, "CPython exited with an unexpected exit code: %d" % (code,)

    cPickle.dump(r, open(cache_fn, 'w'))
    return r

def canonicalize_stderr(stderr):
    """
    For a while we were trying to maintain *exact* stderr compatibility with CPython,
    at least for the last line of the stderr.

    It was starting to get silly to do this, so instead apply some "canonicalizations"
    to map certain groups of error messages together.
    """

    stderr = stderr.strip().split('\n')[-1]

    substitutions = [
            ("NameError: global name '", "NameError: name '"),
            ]

    for pattern, subst_with in substitutions:
        stderr = stderr.replace(pattern, subst_with)

    return stderr

failed = []
def run_test(fn, check_stats, run_memcheck):
    r = fn.rjust(FN_JUST_SIZE)

    statchecks = []
    jit_args = ["-csrq"] + EXTRA_JIT_ARGS
    expected = "success"
    for l in open(fn):
        l = l.strip()
        if not l:
            continue
        if not l.startswith("#"):
            break
        if l.startswith("# statcheck:"):
            l = l[len("# statcheck:"):].strip()
            statchecks.append(l)
        elif l.startswith("# run_args:"):
            l = l[len("# run_args:"):].split()
            jit_args += l
        elif l.startswith("# expected:"):
            expected = l[len("# expected:"):].strip()
        elif l.startswith("# skip-if:"):
            skip_if = l[len("# skip-if:"):].strip()
            skip = eval(skip_if)
            if skip:
                return r + "    (skipped due to 'skip-if: %s')" % skip_if[:30]

    assert expected in ("success", "fail", "statfail"), expected

    if TEST_PYPY:
        jit_args = []
        check_stats = False
        expected = "success"

    run_args = [os.path.abspath(IMAGE)] + jit_args + [fn]
    start = time.time()
    p = subprocess.Popen(run_args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, stdin=open("/dev/null"), preexec_fn=set_ulimits)
    out, stderr = p.communicate()
    last_stderr_line = stderr.strip().split('\n')[-1]

    code = p.wait()
    elapsed = time.time() - start

    stats = {}
    if code == 0 and not TEST_PYPY:
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
        else:
            err = last_stderr_line

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
                raise Exception("%s\n%s\n%s" % (msg, err, stderr))
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
            p = subprocess.Popen(["diff", "-C2", "-a", exp_fn, out_fn], stdout=subprocess.PIPE, preexec_fn=set_ulimits)
            diff = p.stdout.read()
            assert p.wait() in (0, 1)
            raise Exception("Failed on %s:\n%s" % (fn, diff))
    elif not TEST_PYPY and canonicalize_stderr(stderr) != canonicalize_stderr(expected_err):
        if KEEP_GOING:
            r += "    \033[31mFAILED\033[0m (bad stderr)"
            failed.append(fn)
            return r
        else:
            raise Exception((canonicalize_stderr(stderr), canonicalize_stderr(expected_err)))
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
                if expected == "statfail":
                    r += "    (expected statfailure)"
                    break
                elif KEEP_GOING:
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
            # only can get here if all statchecks passed
            if expected == "statfail":
                if KEEP_GOING:
                    r += "    \033[31mUnexpected statcheck success\033[0m"
                    failed.append(fn)
                    return r
                else:
                    raise Exception(("Unexpected statcheck success!", statchecks, stats))
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

def fileSize(fn):
    return os.stat(fn).st_size
    # return len(list(open(fn)))

if __name__ == "__main__":
    run_memcheck = False
    start = 1

    opts, patterns = getopt.gnu_getopt(sys.argv[1:], "j:a:t:mR:k")
    for (t, v) in opts:
        if t == '-m':
            run_memcheck = True
        elif t == '-j':
            NUM_THREADS = int(v)
            assert NUM_THREADS > 0
        elif t == '-R':
            IMAGE = v
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

    if TEST_PYPY:
        IMAGE = '/usr/local/bin/pypy'

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
