# skip-if: True
import os
import sys
import subprocess
import re

def create_virtenv(name, package_list = None, force_create = False):
    if force_create or not os.path.exists(name) or os.stat(sys.executable).st_mtime > os.stat(name + "/bin/python").st_mtime:
        if os.path.exists(name):
            subprocess.check_call(["rm", "-rf", name])

        print "Creating virtualenv to install testing dependencies..."

        VIRTUALENV_SCRIPT = os.path.dirname(__file__) + "/../lib/virtualenv/virtualenv.py"

        try:
            args = [sys.executable, VIRTUALENV_SCRIPT, "-p", sys.executable, name]
            print "Running", args
            subprocess.check_call(args)

            if package_list:
                pip_install(name, package_list)
        except:
            print "Error occurred; trying to remove partially-created directory"
            ei = sys.exc_info()
            try:
                subprocess.check_call(["rm", "-rf", name])
            except Exception as e:
                print e
            raise ei[0], ei[1], ei[2]
    else:
        print "Reusing existing virtualenv"

def pip_install(name, package_list):
    subprocess.check_call([name + "/bin/pip", "install"] + package_list)

def parse_output(output):
    result = []
    for l in output.split('\n'):
        # nosetest
        m = re.match("Ran (\d+) tests in", l)
        if m:
            result.append({"ran": int(m.group(1))})
            continue
        for res_type in ("errors", "failures", "skipped"):
            m = re.match("FAILED \(.*%s=(\d+).*\)" % res_type, l)
            if m:
                result[-1][res_type] = int(m.group(1))

        # py.test
        m = re.match(".* in \d+[.]\d+ seconds [=]*", l)
        if m:
            result.append({})
            for res_type in ("failed", "passed", "skipped", "xfailed", "error"):
                m = re.match(".* (\d+) %s.*" % res_type, l)
                if m:
                    result[-1][res_type] = int(m.group(1))
    return result

def run_test(cmd, cwd, expected, expected_log_hash="", env=None):
    assert isinstance(expected_log_hash, str)
    print "Running", cmd
    process = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, cwd=cwd, env=env)
    output, unused_err = process.communicate()
    errcode = process.poll()
    result = parse_output(output)

    print
    print "Return code:", errcode
    assert errcode in (0, 1), "\n\n%s\nTest process crashed" % output

    expected_log_hash = expected_log_hash.strip()
    this_log_hash = log_hash(output)

    if expected_log_hash == "":
        raise Exception("please set the expected log hash: \nexpected_log_hash = '''\n%s\n'''" % (this_log_hash,))

    if expected == result:
        print "Received expected output"
        different = check_hash(output, expected_log_hash)

        # These checks are useful for making sure that we have the right expected
        # hashes in our test files, but I don't think it's worth failing the build for them:
        # assert not different, "expected_log_hash = '''\n%s\n'''" % (this_log_hash,)
        # assert this_log_hash == expected_log_hash, "expected_log_hash = '''\n%s\n'''" % (this_log_hash,)
    else:
        print >> sys.stderr, '\n'.join(output.split('\n')[-500:])
        print >> sys.stderr, '\n'
        different = check_hash(output, expected_log_hash)
        print >> sys.stderr, '\n'
        print >> sys.stderr, "WRONG output"
        print >> sys.stderr, "is:", result
        print >> sys.stderr, "expected:", expected

        if not different:
            print >> sys.stderr, "(log hash can't detect missing lines)"
        if this_log_hash != expected_log_hash:
            print >> sys.stderr, "expected_log_hash = '''\n%s\n'''" % (this_log_hash,)
        assert result == expected

# Try to canonicalize the log to remove most spurious differences.
# We won't be able to get 100% of them, since there will always be differences in the number of
# python warnings or compiler messages.
# But try to remove the most egregious things (filename differences, timing differences) so that the output is easier to parse.
def process_log(log):
    r = []
    for l in log.split('\n'):
        # Remove timing data:
        l = re.sub("tests in ([\\d\\.])+s", "", l)
        l = re.sub("in ([\\d\\.])+ seconds", "", l)

        # Remove filenames:
        # log = re.sub("/[^ ]*.py:\\d", "", log)
        # log = re.sub("/[^ ]*.py.*line \\d", "", log)
        if "http://" not in l:
            l = re.sub("(^|[ \"\'/])/[^ :\"\']*($|[ \":\'])", "", l)

        # Remove pointer ids:
        l = re.sub('0x([0-9a-f]{8,})', "", l)

        r.append(l)

    return r

def log_hash(log, nbits=1024):
    log_lines = process_log(log)

    bits = [0] * nbits

    for l in log_lines:
        bits[hash(l) % nbits] = 1

    assert sum(bits) < nbits * .67, "hash is very full!"

    l = []
    for i in xrange(0, nbits, 8):
        t = 0
        for j in xrange(8):
            if bits[i + j]:
                t += 1 << (7 - j)
        l.append(chr(t))
    return ''.join(l).encode('base64').strip()

def check_hash(log, expected_hash):
    orig_log_lines = log.split('\n')
    log_lines = process_log(log)

    s = expected_hash.decode('base64')
    nbits = len(s) * 8
    bits = [0] * nbits

    for i in xrange(len(s)):
        c = ord(s[i])
        for j in xrange(8):
            bit = (c >> (7 - j)) & 1
            if bit:
                bits[i * 8 + j] = True

    missing = [False] * len(log_lines)
    for i, l in enumerate(log_lines):
        if not bits[hash(l) % nbits]:
            missing[i] = True

    ncontext = 2
    def ismissing(idx, within):
        for i in xrange(max(0, idx-within), min(len(log_lines), idx+within+1)):
            if missing[i]:
                return True
        return False
    different = False
    for i in xrange(len(log_lines)):
        if ismissing(i, 0):
            different = True
            if orig_log_lines[i] != log_lines[i]:
                print >>sys.stderr, "\033[30m+ % 4d: %s\033[0m" % (i + 1, orig_log_lines[i])
                print >>sys.stderr, "+ % 4d: %s" % (i + 1, log_lines[i])
            else:
                print >>sys.stderr, "+ % 4d: %s" % (i + 1, orig_log_lines[i])
        elif ismissing(i, ncontext):
            print >>sys.stderr, "  % 4d: %s" % (i + 1, orig_log_lines[i])
    assert different == any(missing)
    return any(missing)
