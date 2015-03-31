# Copyright (c) 2014-2015 Dropbox, Inc.
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

import Queue
import argparse
import cPickle
import datetime
import functools
import glob
import os
import re
import resource
import shutil
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
TIME_LIMIT = 25

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

EXTMODULE_DIR = os.path.abspath(os.path.dirname(os.path.realpath(__file__)) + "/../test/test_extension/build/lib.linux-x86_64-2.7/")
THIS_FILE = os.path.abspath(__file__)
_global_mtime = None
def get_global_mtime():
    global _global_mtime
    if _global_mtime is not None:
        return _global_mtime

    # Start off by depending on the tester itself
    rtn = os.stat(THIS_FILE).st_mtime

    for fn in os.listdir(EXTMODULE_DIR):
        if not fn.endswith(".so"):
            continue
        rtn = max(rtn, os.stat(os.path.join(EXTMODULE_DIR, fn)).st_mtime)
    _global_mtime = rtn
    return rtn

def get_expected_output(fn):
    sys.stdout.flush()
    assert fn.endswith(".py")
    expected_fn = fn[:-3] + ".expected"
    if os.path.exists(expected_fn):
        return 0, open(expected_fn).read(), ""

    cache_fn = fn[:-3] + ".expected_cache"
    if os.path.exists(cache_fn):
        cache_mtime = os.stat(cache_fn).st_mtime
        if cache_mtime > os.stat(fn).st_mtime and cache_mtime > get_global_mtime():
            try:
                return cPickle.load(open(cache_fn))
            except (EOFError, ValueError):
                pass

    # TODO don't suppress warnings globally:
    env = dict(os.environ)
    env["PYTHONPATH"] = EXTMODULE_DIR
    p = subprocess.Popen(["python", "-Wignore", fn], stdout=subprocess.PIPE, stderr=subprocess.PIPE, stdin=open("/dev/null"), preexec_fn=set_ulimits, env=env)
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
            ("AttributeError: '(\w+)' object attribute '(\w+)' is read-only", "AttributeError: \\2"),
            (r"TypeError: object.__new__\(\) takes no parameters", "TypeError: object() takes no parameters"),
            ("IndexError: list assignment index out of range", "IndexError: list index out of range"),
            ]

    for pattern, subst_with in substitutions:
        stderr = re.sub(pattern, subst_with, stderr)

    return stderr

failed = []
def run_test(fn, check_stats, run_memcheck):
    r = os.path.basename(fn).rjust(FN_JUST_SIZE)
    test_base_name = os.path.basename(fn).split('.')[0]

    if test_base_name in TESTS_TO_SKIP:
        return r + "    (skipped due to command line option)"

    statchecks = []
    jit_args = ["-rq"] + EXTRA_JIT_ARGS
    collect_stats = True
    expected = "success"
    should_error = False
    allow_warnings = []
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
        elif l.startswith("# should_error"):
            should_error = True
        elif l.startswith("# fail-if:"):
            condition = l.split(':', 1)[1].strip()
            if eval(condition):
                expected = "fail"
        elif l.startswith("# skip-if:"):
            skip_if = l[len("# skip-if:"):].strip()
            skip = eval(skip_if)
            if skip:
                return r + "    (skipped due to 'skip-if: %s')" % skip_if[:30]
        elif fn.split('.')[0] in TESTS_TO_SKIP:
                return r + "    (skipped due to command line option)"
        elif l.startswith("# allow-warning:"):
            allow_warnings.append("Warning: " + l.split(':', 1)[1].strip())
        elif l.startswith("# no-collect-stats"):
            collect_stats = False

    if TEST_PYPY:
        collect_stats = False

    if collect_stats:
        jit_args = ['-s'] + jit_args

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

    if allow_warnings:
        out_lines = []
        for l in out.split('\n'):
            for regex in allow_warnings:
                if re.match(regex, l):
                    break
            else:
                out_lines.append(l)
        out = "\n".join(out_lines)

    stats = None
    if code >= 0 and collect_stats:
        stats = {}
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
        elif KEEP_GOING:
            r += "    \033[%dmFAILED\033[0m (%s)" % (color, msg)
            failed.append(fn)
            return r
        else:
            raise Exception("%s\n%s\n%s" % (msg, err, stderr))

    elif should_error == (code == 0):
        color = 31              # red
        if code == 0:
            msg = "Exited successfully; remove '# should_error' if this is expected"
        else:
            msg = "Exited with code %d; add '# should_error' if this is expected" % code

        if KEEP_GOING:
            r += "    \033[%dmFAILED\033[0m (%s)" % (color, msg)
            failed.append(fn)
            return r
        else:
            # show last line of stderr so we have some idea went wrong
            print "Last line of stderr: " + last_stderr_line
            raise Exception(msg)

    elif out != expected_out:
        if expected == "fail":
            r += "    Expected failure (bad output)"
            return r
        else:
            if KEEP_GOING:
                r += "    \033[31mFAILED\033[0m (bad output)"
                failed.append(fn)
                return r
            exp_fd, exp_fn = tempfile.mkstemp(prefix="expected_")
            out_fd, out_fn = tempfile.mkstemp(prefix="received_")
            os.fdopen(exp_fd, 'w').write(expected_out)
            os.fdopen(out_fd, 'w').write(out)
            p = subprocess.Popen(["diff", "--unified=5", "-a", exp_fn, out_fn], stdout=subprocess.PIPE, preexec_fn=set_ulimits)
            diff = p.stdout.read()
            assert p.wait() in (0, 1)
            os.unlink(exp_fn)
            os.unlink(out_fn)
            raise Exception("Failed on %s:\n%s" % (fn, diff))
    elif not TEST_PYPY and canonicalize_stderr(stderr) != canonicalize_stderr(expected_err):
        if expected == "fail":
            r += "    Expected failure (bad stderr)"
            return r
        elif KEEP_GOING:
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
        def noninit_count(s):
            return stats.get(s, 0) - stats.get("_init_" + s, 0)

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

                    m = re.search("""noninit_count\(['"]([\w_]+)['"]\)""", l)
                    if m:
                        statname = m.group(1)
                        raise Exception((l, statname, noninit_count(statname)))

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
            quit[job[0]] = job[0] + ':\n' + traceback.format_exc()
            results[job[0]] = None
            with cv:
                cv.notifyAll()
            # os._exit(-1)

def fileSize(fn):
    return os.stat(fn).st_size
    # return len(list(open(fn)))

# our arguments
parser = argparse.ArgumentParser(description='Runs Pyston tests.')
parser.add_argument('-m', '--run-memcheck', action='store_true', help='run memcheck')
parser.add_argument('-j', '--num-threads', metavar='N', type=int, default=NUM_THREADS,
                    help='number of threads')
parser.add_argument('-k', '--keep-going', default=KEEP_GOING, action='store_true',
                    help='keep going after test failure')
parser.add_argument('-R', '--image', default=IMAGE,
                    help='the executable to test (default: %s)' % IMAGE)
parser.add_argument('-K', '--no-keep-going', dest='keep_going', action='store_false',
                    help='quit after test failure')
parser.add_argument('-a', '--extra-args', default=[], action='append',
                    help="additional arguments to pyston (must be invoked with equal sign: -a=-ARG)")
parser.add_argument('-t', '--time-limit', type=int, default=TIME_LIMIT,
                    help='set time limit in seconds for each test')
parser.add_argument('-s', '--skip-tests', type=str, default='',
                    help='tests to skip (comma-separated)')

parser.add_argument('test_dir')
parser.add_argument('patterns', nargs='*')

def main(orig_dir):
    global KEEP_GOING
    global IMAGE
    global EXTRA_JIT_ARGS
    global TIME_LIMIT
    global TEST_DIR
    global FN_JUST_SIZE
    global TESTS_TO_SKIP

    run_memcheck = False
    start = 1

    opts = parser.parse_args()
    run_memcheck = opts.run_memcheck
    NUM_THREADS = opts.num_threads
    IMAGE = os.path.join(orig_dir, opts.image)
    KEEP_GOING = opts.keep_going
    EXTRA_JIT_ARGS += opts.extra_args
    TIME_LIMIT = opts.time_limit
    TESTS_TO_SKIP = opts.skip_tests.split(',')

    TEST_DIR = os.path.join(orig_dir, opts.test_dir)
    patterns = opts.patterns

    assert os.path.isdir(TEST_DIR), "%s doesn't look like a directory with tests in it" % TEST_DIR

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

    LIB_DIR = os.path.join(sys.prefix, "lib/python2.7")
    for t in tests:
        bn = os.path.basename(t)
        assert bn.endswith(".py")
        module_name = bn[:-3]

        if os.path.exists(os.path.join(LIB_DIR, module_name)) or \
           os.path.exists(os.path.join(LIB_DIR, module_name + ".py")) or \
           module_name in sys.builtin_module_names:
            raise Exception("Error: %s hides builtin module '%s'" % (t, module_name))

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

    FN_JUST_SIZE = max(20, 2 + max(len(os.path.basename(fn)) for fn in tests))

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
            print os.path.basename(fn).rjust(FN_JUST_SIZE),
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

if __name__ == "__main__":
    origdir = os.getcwd()
    tmpdir = tempfile.mkdtemp()
    os.chdir(tmpdir)
    try:
        main(origdir)
    finally:
        shutil.rmtree(tmpdir)
