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
TESTS_TO_SKIP = []
EXIT_CODE_ONLY = False
SKIP_FAILING_TESTS = False
VERBOSE = 1

PYTHONIOENCODING = 'utf-8'

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

EXTMODULE_DIR = None
EXTMODULE_DIR_PYSTON = None
THIS_FILE = os.path.abspath(__file__)

_global_mtime = None
def get_global_mtime():
    global _global_mtime
    if _global_mtime is not None:
        return _global_mtime

    # Start off by depending on the tester itself
    rtn = os.stat(THIS_FILE).st_mtime

    assert os.listdir(EXTMODULE_DIR), EXTMODULE_DIR
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
    env["PYTHONIOENCODING"] = PYTHONIOENCODING
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
            (r"unqualified exec is not allowed in function '(\w+)' it (.*)",
             r"unqualified exec is not allowed in function '\1' because it \2"),
            ]

    for pattern, subst_with in substitutions:
        stderr = re.sub(pattern, subst_with, stderr)

    return stderr

failed = []

class Options(object): pass

# returns a single string, or a tuple of strings that are spliced together (with spaces between) by our caller
def run_test(fn, check_stats, run_memcheck):
    opts = get_test_options(fn, check_stats, run_memcheck)
    del check_stats, run_memcheck

    if opts.skip:
        return "(skipped: %s)" % opts.skip

    env = dict(os.environ)
    env["PYTHONPATH"] = EXTMODULE_DIR_PYSTON
    env["PYTHONIOENCODING"] = PYTHONIOENCODING
    run_args = [os.path.abspath(IMAGE)] + opts.jit_args + [fn]
    start = time.time()
    p = subprocess.Popen(run_args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, stdin=open("/dev/null"),
                         preexec_fn=set_ulimits, env=env)
    out, stderr = p.communicate()
    code = p.wait()
    elapsed = time.time() - start

    if code >= 128:
        code -= 256

    return determine_test_result(fn, opts, code, out, stderr, elapsed)

def get_test_options(fn, check_stats, run_memcheck):
    opts = Options()

    opts.check_stats = check_stats
    opts.run_memcheck = run_memcheck
    opts.statchecks = []
    opts.jit_args = ["-rq"] + EXTRA_JIT_ARGS
    opts.collect_stats = True
    opts.expected = "success"
    opts.should_error = False
    opts.allow_warnings = []
    opts.skip = None

    for l in open(fn):
        l = l.strip()
        if not l:
            continue
        if not l.startswith("#"):
            break
        if l.startswith("# statcheck:"):
            l = l[len("# statcheck:"):].strip()
            opts.statchecks.append(l)
        elif l.startswith("# run_args:"):
            l = l[len("# run_args:"):].split()
            opts.jit_args += l
        elif l.startswith("# expected:"):
            opts.expected = l[len("# expected:"):].strip()
        elif l.startswith("# should_error"):
            opts.should_error = True
        elif l.startswith("# fail-if:"):
            condition = l.split(':', 1)[1].strip()
            if eval(condition):
                opts.expected = "fail"
        elif l.startswith("# skip-if:"):
            skip_if = l[len("# skip-if:"):].strip()
            if eval(skip_if):
                opts.skip = "skip-if: %s" % skip_if[:30]
        elif l.startswith("# allow-warning:"):
            opts.allow_warnings.append("Warning: " + l.split(':', 1)[1].strip())
        elif l.startswith("# no-collect-stats"):
            opts.collect_stats = False

    if not opts.skip:
        # consider other reasons for skipping file
        if SKIP_FAILING_TESTS and opts.expected == 'fail':
            opts.skip = 'expected to fail'
        elif os.path.basename(fn).split('.')[0] in TESTS_TO_SKIP:
            opts.skip = 'command line option'

    assert opts.expected in ("success", "fail", "statfail"), opts.expected

    if TEST_PYPY:
        opts.jit_args = []
        opts.collect_stats = False
        opts.check_stats = False
        opts.expected = "success"

    if opts.collect_stats:
        opts.jit_args = ['-s'] + opts.jit_args

    return opts

def diff_output(expected, received, expected_file_prefix, received_file_prefix):
    exp_fd, exp_fn = tempfile.mkstemp(prefix=expected_file_prefix)
    rec_fd, rec_fn = tempfile.mkstemp(prefix=received_file_prefix)
    os.fdopen(exp_fd, 'w').write(expected)
    os.fdopen(rec_fd, 'w').write(received)
    p = subprocess.Popen(["diff", "--unified=5", "-a", exp_fn, rec_fn], stdout=subprocess.PIPE, preexec_fn=set_ulimits)
    diff = p.stdout.read()
    assert p.wait() in (0, 1)
    os.unlink(exp_fn)
    os.unlink(rec_fn)
    return diff


def determine_test_result(fn, opts, code, out, stderr, elapsed):
    if opts.allow_warnings:
        out_lines = []
        for l in out.split('\n'):
            for regex in opts.allow_warnings:
                if re.match(regex, l):
                    break
            else:
                out_lines.append(l)
        out = "\n".join(out_lines)

    stats = None
    if opts.collect_stats:
        stats = {}
        have_stats = (stderr.count("Stats:") == 1 and stderr.count("(End of stats)") == 1)

        if code >= 0:
            if not have_stats:
                color = 31
                msg = "no stats available"
                if KEEP_GOING:
                    failed.append(fn)
                    if VERBOSE >= 1:
                        return "\033[%dmFAILED\033[0m (%s)\n%s" % (color, msg, stderr)
                    else:
                        return "\033[%dmFAILED\033[0m (%s)" % (color, msg)
                else:
                    raise Exception("%s\n%s" % (msg, stderr))
            assert have_stats

        if have_stats:
            assert stderr.count("Stats:") == 1
            stderr, stats_str = stderr.split("Stats:")
            stats_str, stderr_tail = stats_str.split("(End of stats)\n")
            stderr += stderr_tail

            other_stats_str, counter_str = stats_str.split("Counters:")
            for l in counter_str.strip().split('\n'):
                assert l.count(':') == 1, l
                k, v = l.split(':')
                stats[k.strip()] = int(v)

    last_stderr_line = stderr.strip().split('\n')[-1]

    if EXIT_CODE_ONLY:
        # fools the rest of this function into thinking the output is OK & just checking the exit code.
        # there oughtta be a cleaner way to do this.
        expected_code, expected_out, expected_err = 0, out, stderr
    else:
        # run CPython to get the expected output
        expected_code, expected_out, expected_err = get_expected_output(fn)

    color = 31 # red

    if code != expected_code:
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

        if opts.expected == "fail":
            return "Expected failure (got code %d, should be %d)" % (code, expected_code)
        elif KEEP_GOING:
            failed.append(fn)
            if VERBOSE >= 1:
                return "\033[%dmFAILED\033[0m (%s)\n%s" % (color, msg, stderr)
            else:
                return "\033[%dmFAILED\033[0m (%s)" % (color, msg)
        else:
            raise Exception("%s\n%s\n%s" % (msg, err, stderr))

    elif opts.should_error == (code == 0):
        if code == 0:
            msg = "Exited successfully; remove '# should_error' if this is expected"
        else:
            msg = "Exited with code %d; add '# should_error' if this is expected" % code

        if KEEP_GOING:
            failed.append(fn)
            return "\033[%dmFAILED\033[0m (%s)" % (color, msg)
        else:
            # show last line of stderr so we have some idea went wrong
            print "Last line of stderr: " + last_stderr_line
            raise Exception(msg)

    elif out != expected_out:
        if opts.expected == "fail":
            return "Expected failure (bad output)"
        else:
            diff = diff_output(expected_out, out, "expected_", "received_")
            if KEEP_GOING:
                failed.append(fn)
                if VERBOSE >= 1:
                    return "\033[%dmFAILED\033[0m (bad output)\n%s" % (color, diff)
                else:
                    return "\033[%dmFAILED\033[0m (bad output)" % (color,)

            else:
                raise Exception("Failed on %s:\n%s" % (fn, diff))
    elif not TEST_PYPY and canonicalize_stderr(stderr) != canonicalize_stderr(expected_err):
        if opts.expected == "fail":
            return "Expected failure (bad stderr)"
        else:
            diff = diff_output(expected_err, stderr, "expectederr_", "receivederr_")
            if KEEP_GOING:
                failed.append(fn)
                if VERBOSE >= 1:
                    return "\033[%dmFAILED\033[0m (bad stderr)\n%s" % (color, diff)
                else:
                    return "\033[%dmFAILED\033[0m (bad stderr)" % (color,)
            else:
                raise Exception((canonicalize_stderr(stderr), canonicalize_stderr(expected_err)))
    elif opts.expected == "fail":
        if KEEP_GOING:
            failed.append(fn)
            return "\033[31mFAILED\033[0m (unexpected success)"
        raise Exception("Unexpected success on %s" % fn)

    r = ("Correct output (%5.1fms)" % (elapsed * 1000,),)

    if opts.check_stats:
        def noninit_count(s):
            return stats.get(s, 0) - stats.get("_init_" + s, 0)

        for l in opts.statchecks:
            test = eval(l)
            if not test:
                if opts.expected == "statfail":
                    r += ("(expected statfailure)",)
                    break
                else:
                    msg = ()
                    m = re.match("""stats\[['"]([\w_]+)['"]]""", l)
                    if m:
                        statname = m.group(1)
                        msg = (l, statname, stats[statname])

                    m = re.search("""noninit_count\(['"]([\w_]+)['"]\)""", l)
                    if m and not msg:
                        statname = m.group(1)
                        msg = (l, statname, noninit_count(statname))

                    if not msg:
                        msg = (l, stats)

                    elif KEEP_GOING:
                        failed.append(fn)
                        if VERBOSE:
                            return r + ("\033[31mFailed statcheck\033[0m\n%s" % (msg,),)
                        else:
                            return r + ("\033[31mFailed statcheck\033[0m",)
                    else:
                        raise Exception(msg)
        else:
            # only can get here if all statchecks passed
            if opts.expected == "statfail":
                if KEEP_GOING:
                    failed.append(fn)
                    return r + ("\033[31mUnexpected statcheck success\033[0m",)
                else:
                    raise Exception(("Unexpected statcheck success!", opts.statchecks, stats))
    else:
        r += ("(ignoring stats)",)

    if opts.run_memcheck:
        if code == 0:
            start = time.time()
            p = subprocess.Popen(["valgrind", "--tool=memcheck", "--leak-check=no"] + run_args, stdout=open("/dev/null", 'w'), stderr=subprocess.PIPE, stdin=open("/dev/null"))
            out, err = p.communicate()
            assert p.wait() == 0
            if "Invalid read" not in err:
                elapsed = (time.time() - start)
                r += ("Memcheck passed (%4.1fs)" % (elapsed,),)
            else:
                if KEEP_GOING:
                    failed.append(fn)
                    return r + ("\033[31mMEMCHECKS FAILED\033[0m",)
                else:
                    raise Exception(err)
        else:
            r += ("(Skipping memchecks)",)

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
parser.add_argument('-e', '--exit-code-only', action='store_true',
                    help="only check exit code; don't run CPython to get expected output to compare against")
parser.add_argument('--skip-failing', action='store_true',
                    help="skip tests expected to fail")
parser.add_argument('--order-by-mtime', action='store_true',
                    help="order test execution by modification time, instead of file size")

parser.add_argument('test_dir')
parser.add_argument('pattern', nargs='*')

def main(orig_dir):
    global KEEP_GOING
    global IMAGE
    global EXTRA_JIT_ARGS
    global TIME_LIMIT
    global TEST_DIR
    global FN_JUST_SIZE
    global TESTS_TO_SKIP
    global EXIT_CODE_ONLY
    global SKIP_FAILING_TESTS
    global VERBOSE
    global EXTMODULE_DIR_PYSTON
    global EXTMODULE_DIR

    run_memcheck = False

    opts = parser.parse_args()
    run_memcheck = opts.run_memcheck
    NUM_THREADS = opts.num_threads
    IMAGE = os.path.join(orig_dir, opts.image)
    KEEP_GOING = opts.keep_going
    EXTRA_JIT_ARGS += opts.extra_args
    TIME_LIMIT = opts.time_limit
    TESTS_TO_SKIP = opts.skip_tests.split(',')
    TESTS_TO_SKIP = filter(bool, TESTS_TO_SKIP) # "".split(',') == ['']
    EXIT_CODE_ONLY = opts.exit_code_only
    SKIP_FAILING_TESTS = opts.skip_failing

    TEST_DIR = os.path.join(orig_dir, opts.test_dir)
    EXTMODULE_DIR_PYSTON = os.path.abspath(os.path.dirname(os.path.realpath(IMAGE)) + "/test/test_extension/")
    EXTMODULE_DIR = os.path.abspath(os.path.dirname(os.path.realpath(IMAGE)) + "/test/test_extension/build/lib.linux-x86_64-2.7/")
    patterns = opts.pattern

    if not patterns and not TESTS_TO_SKIP:
        TESTS_TO_SKIP = ["t", "t2"]

    assert os.path.isdir(TEST_DIR), "%s doesn't look like a directory with tests in it" % TEST_DIR

    if TEST_DIR.rstrip('/').endswith("cpython") and not EXIT_CODE_ONLY:
        print >>sys.stderr, "Test directory name ends in cpython; are you sure you don't want --exit-code-only?"

    # do we need this any more?
    IGNORE_STATS = ["%s/%d.py" % (TEST_DIR, i) for i in ()] + []

    tests = [t for t in glob.glob("%s/*.py" % TEST_DIR)]

    LIB_DIR = os.path.join(sys.prefix, "lib/python2.7")
    for t in tests:
        bn = os.path.basename(t)
        assert bn.endswith(".py")
        module_name = bn[:-3]

        if os.path.exists(os.path.join(LIB_DIR, module_name)) or \
           os.path.exists(os.path.join(LIB_DIR, module_name + ".py")) or \
           module_name in sys.builtin_module_names:
            raise Exception("Error: %s hides builtin module '%s'" % (t, module_name))

    if patterns:
        filtered_tests = []
        for t in tests:
            if any(re.match(os.path.join(TEST_DIR, p) + ".*\.py", t) for p in patterns):
                filtered_tests.append(t)
        tests = filtered_tests
    if not tests:
        print >>sys.stderr, "No tests matched the given patterns. OK by me!"
        # this can happen legitimately in e.g. `make check_test_foo` if test_foo.py is a CPython regression test.
        sys.exit(0)

    FN_JUST_SIZE = max(20, 2 + max(len(os.path.basename(fn)) for fn in tests))

    if TEST_PYPY:
        IMAGE = '/usr/local/bin/pypy'

    if not patterns:
        if opts.order_by_mtime:
            tests.sort(key=lambda fn:os.stat(fn).st_mtime, reverse=True)
        else:
            tests.sort(key=fileSize)

    for fn in tests:
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
        name = os.path.basename(fn).rjust(FN_JUST_SIZE)
        msgs = results[fn]
        if isinstance(msgs,str):
            msgs = [msgs]
        print '    '.join([name] + list(msgs))

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

# adding a comment here to invalidate cached expected results
