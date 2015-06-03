import os, os.path
import subprocess
import sys

def main():
    repo = sys.argv[1]
    patches = sys.argv[2:]
    gitfile = os.path.join(repo, '.git')

    assert os.path.isdir(repo), "Expected to find repo at %s" % (repo,)
    assert os.path.exists(gitfile), "Expected %s to exist" % (gitfile,)
    for fn in patches:
        assert os.path.exists(fn), "Expected a patch file/dir at %s" % (fn,)

    os.chdir(repo)
    code = subprocess.call(["git", "am", "--"] + patches)
    if not code:
        sys.exit(0)

    # git am errored. recover by unconditionally aborting.
    print >>sys.stderr, "----- Running `git am --abort' -----"
    subprocess.check_call(["git", "am", "--abort"])
    sys.exit(1)

if __name__ == '__main__':
    main()
