import math
import os
import subprocess
import sys

def test(rev, args):
    subprocess.check_call(["make", "llvm_up", "USE_TEST_LLVM=1", "LLVM_REVISION=%d" % rev])
    subprocess.check_call(["make", "llvm_release", "llvm_quick", "USE_TEST_LLVM=1", "LLVM_REVISION=%d" % rev])
    code = subprocess.call(["make", "USE_TEST_LLVM=1", "LLVM_REVISION=%d" % rev] + args)
    return code == 0

def efficiency(rid):
    p = 1.0 * (rid - good_rev) / (bad_rev - good_rev)
    learnt = - (p * math.log(p) + (1-p) * math.log(1-p))

    # k represents how much of the codebase an "average commit" causes to be rebuilt:
    k = 0.10

    effort = 1 - math.exp(- k * (1 + min(rid - good_rev, bad_rev - rid)))
    assert effort > 0
    # print good_rev, rid, bad_rev, learnt, effort, learnt/effort
    return learnt/effort

if __name__ == "__main__":
    good_rev, bad_rev = sys.argv[1:3]
    good_rev = int(good_rev)
    bad_rev = int(bad_rev)

    args = sys.argv[3:]
    assert args

    # print "Confirming that %d works" % good_rev
    # b = test(good_rev, args)
    # assert b, "good_rev must be working"
# 
    # print "Confirming that %d is broken" % bad_rev
    # b = test(bad_rev, args)
    # assert not b, "bad_rev must not work"

    while bad_rev > good_rev + 1:
        print "%d is good, %d is bad" % (good_rev, bad_rev)
        open("find_problem.status", "w").write("%d %d\n" % (good_rev, bad_rev))
        middle = (good_rev + bad_rev + 1) / 2
        revs = range(good_rev+1, middle+1)
        revs.sort(reverse=True, key=efficiency)
        # print good_rev, bad_rev, (good_rev + bad_rev) / 2, revs[0]
        next_rev = revs[0]
        print "Testing revision %d (p=%.1f%%)" % (next_rev, 100.0 * (next_rev - good_rev) / (bad_rev - good_rev))
        b = test(next_rev, args)

        print "Revision", next_rev, "works" if b else "failed"
        if b:
            good_rev = next_rev
        else:
            bad_rev = next_rev

    open("find_problem.status", "w").write("%d %d\n" % (good_rev, bad_rev))
    print "Rev %d is good, rev %d is bad" % (good_rev, bad_rev)
