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

from __future__ import division

import subprocess
import os

BASE_DIR = os.path.join(os.path.dirname(__file__), "..")
TEST_DIR = os.path.join(BASE_DIR, "test/tests/")
EXTMODULE_DIR_PYSTON = os.path.abspath(os.path.join(BASE_DIR, "test/test_extension/"))

def main():
    tests = os.listdir(os.path.join(TEST_DIR))
    ics_tests = [os.path.join(TEST_DIR, test) for test in tests if
            test.endswith(".py") and "ics" in test]

    results = []
    results.append(['test', 'ics', 'total ic size', 'average ic size'])
    totalNumIcs = 0
    totalSizeIcs = 0
    asm_log = []
    for ics_test in ics_tests:
        stats, assembly_logging = runTest(ics_test)
        numIcs = stats['ic_rewrites_committed']
        sizeIcs = stats['ic_rewrites_total_bytes']
        results.append([ics_test, str(numIcs), str(sizeIcs), div(sizeIcs, numIcs)])
        totalNumIcs += numIcs
        totalSizeIcs += sizeIcs
        asm_log.append(assembly_logging)

    print "\n".join(asm_log)

    results.append(['TOTAL', str(totalNumIcs), str(totalSizeIcs), div(totalSizeIcs, totalNumIcs)])

    printTable(results)

def div(a, b):
    if b == 0:
        return "undef"
    else:
        return '%.3f' % (a / b)

def runTest(filename):
    print 'running test', filename
    pyston = os.path.join(BASE_DIR, "pyston_dbg")

    env = dict(os.environ)

    env["PYTHONPATH"] = EXTMODULE_DIR_PYSTON

    proc = subprocess.Popen([pyston, "-a", "-s", filename],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            stdin=subprocess.PIPE,
            env=env)
    stdout, stderr = proc.communicate()
    if proc.wait() != 0:
        raise Exception("%s failed" % filename)

    stderr, stats_str = stderr.split("Stats:")
    stats_str, stderr_tail = stats_str.split("(End of stats)\n")
    other_stats_str, counter_str = stats_str.split("Counters:")
    stats = {}
    for l in counter_str.strip().split('\n'):
        assert l.count(':') == 1, l
        k, v = l.split(':')
        stats[k.strip()] = int(v)

    assembly_logging = stderr

    return (stats, assembly_logging)

def printTable(table):
    widths = [3 + max(len(table[i][j]) for i in xrange(len(table))) for j in xrange(len(table[0]))]
    for row in table:
        for col, elem in enumerate(row):
            print elem, ("" if col == len(row)-1 else " " * (widths[col] - len(elem))),
        print ''

if __name__ == '__main__':
    main()
