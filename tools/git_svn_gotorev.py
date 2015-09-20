# Similar to "git svn find-rev" but:
# - doesn't require git-svn to be installed
# - is faster
# - actually works

import os
import subprocess
import sys

def find_rev(svn_id, fetch_if_necessary=True):
    p = subprocess.Popen(["git", "log", "origin/master"], cwd=repo, stdout=subprocess.PIPE)

    isfirst = True
    commit = None
    while True:
        s = p.stdout.readline()

        if s.startswith("commit"):
            commit = s.strip().split()[1]
        elif s.startswith("    git-svn-id:"):
            rid = int(s.split()[1].split('@')[1])
            if rid <= svn_rev:
                break
            isfirst = False
    else:
        raise Exception("???")
    assert commit

    p.kill()

    if isfirst:
        if fetch_if_necessary:
            print "Need to fetch the repo"
            subprocess.check_call(["git", "fetch"], cwd=repo)
            print "Trying again"
            return find_rev(svn_id, False)
        else:
            print "This is the newest one??"
    else:
        print "Don't need to fetch"

    print "Commit to go to is:", commit, rid
    return commit, rid

if __name__ == "__main__":
    repo, svn_rev, patch_dir = sys.argv[1:]
    svn_rev = int(svn_rev)
    assert os.path.isdir(repo), "Expected to find repo directory at %s" % (repo,)

    commit, rid = find_rev(svn_rev)

    p = subprocess.Popen(["git", "show", "--format=short"], cwd=repo, stdout=subprocess.PIPE)
    cur_commit = p.stdout.readline().strip().split()[1]
    print "Currently:", cur_commit
    p.kill()
    # TODO: can't do this optimization now that we have patches to apply;
    # maybe could try to determine that the patches are the same?
    # if cur_commit == commit:
        # print "Up to date"
        # sys.exit(0)

    exitcode = subprocess.call(["git", "diff", "--exit-code"], cwd=repo, stdout=open("/dev/null"))
    diffs = (exitcode != 0)
    if diffs:
        print >>sys.stderr, "Error: the llvm directory has modifications that would be lost!"
        print >>sys.stderr, "Please stash or revert them before running this script."
        sys.exit(1)

    subprocess.check_call(["git", "diff", "--dirstat", commit], cwd=repo)
    subprocess.check_call(["git", "checkout", "-B", "tmp", commit], cwd=repo)
    subprocess.check_call(["git", "checkout", "-B", "cur", commit], cwd=repo)

    if not os.path.exists(patch_dir):
        patch_fns = []
    else:
        patch_fns = sorted(os.listdir(patch_dir))
    for patch_fn in patch_fns:
        if patch_fn.startswith('.'):
            continue
        if patch_fn.startswith("LICENSE"):
            continue
        if "Update-TailCallElim" in patch_fn and svn_rev >= 208017:
            continue
        if "Update-IntelJITEvents" in patch_fn and svn_rev >= 209989:
            continue
        if "stackmap-sections-for-ELF" in patch_fn and svn_rev >= 214538:
            continue
        if "Enable-invoking-the-patchpoint-intrinsic" in patch_fn and svn_rev >= 220055:
            continue
        if "support-varargs-intrinsics" in patch_fn and svn_rev >= 220205:
            continue
        if "Expose-getSymbolLoadAddress" in patch_fn and svn_rev <= 222840:
            continue
        if "Filter-out-extraneous-registers-from-live-outs-like" in patch_fn:
            continue
        if "getSymbolLoadAddress" in patch_fn:
            continue

        patch_fn = os.path.abspath(os.path.join(patch_dir, patch_fn))
        code = subprocess.call(["git", "am", patch_fn], cwd=repo)

        if code != 0:
            print "Running 'git am --abort'..."
            subprocess.check_call(["git", "am", "--abort"], cwd=repo)
            sys.exit(1)

    if diffs:
        subprocess.check_call(["git", "stash", "pop", "-q"], cwd=repo)
