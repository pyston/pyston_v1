#! /usr/bin/env python2.7

"""
A script to help with rebasing to a new CPython version
"""

import os
import subprocess
import sys

FROM = 'v2.7.7'
TO = 'v2.7.8'


def is_skipped_folder(cpython_repo, path):
    skipped_folders = [
        '.hgtags',
        'Demo',
        'Doc',
        'Mac',
        'Makefile',
        'Misc',
        'PCbuild',
        'README',
        'Tools',
        'configure',
    ]
    for skipped_folder in skipped_folders:
        if path.startswith(os.path.join(cpython_repo, skipped_folder)):
            return True
    return False


def get_rev_file(path, rev):
    bn = os.path.basename(path)
    if not os.path.exists("/tmp/" + rev):
        os.makedirs("/tmp/" + rev)
    filepath = "/tmp/%s/%s.%s" % (rev, bn.rsplit('.', 1)[0], bn.rsplit('.')[-1])
    code = os.system('hg cat %s -r %s > %s' % (path, rev, filepath))
    if code != 0:
        return None
    return filepath


def main(cpython_repo, pyston_repo, skip):
    output = subprocess.check_output(['hg', 'status', cpython_repo, '--rev', '%s:%s' % (FROM, TO)])

    diff_files = [os.path.abspath(line.split()[1]) for line in output.splitlines()]
    diff_files = filter(lambda x: not is_skipped_folder(cpython_repo, x), diff_files)
    diff_files = sorted(diff_files)


    for i, filepath in enumerate(diff_files):
        if i < skip:
            continue

        print

        pyston_filepath = os.path.join(pyston_repo, filepath[len(cpython_repo):])
        print 'diff', i, ':', filepath
        args = (get_rev_file(filepath, FROM), get_rev_file(filepath, TO), pyston_filepath)

        if not args[0]:
            print "Guessing that this got added; copying in"
            assert args[1]
            if os.path.exists(pyston_filepath):
                assert open(pyston_filepath).read() == open(args[1]).read()
            else:
                open(pyston_filepath, 'w').write(open(args[1]).read())
            os.system("git add %s" % pyston_filepath)
            continue

        if not args[1]:
            print "Guessing that this got deleted; removing"
            if os.path.exists(pyston_filepath):
                os.system("git rm -f %s" % pyston_filepath)
            continue

        if not os.path.exists(pyston_filepath):
            print filepath, "doesn't exist in pyston"
            os.system('vimdiff %s %s' % (args[0], args[1]))
            continue

        p = subprocess.Popen(["diff", "-U2", args[0], args[1]], stdout=subprocess.PIPE)
        p2 = subprocess.Popen(["patch", "-p3"], stdin=p.stdout, cwd=os.path.dirname(pyston_filepath), stdout=subprocess.PIPE)

        output = p2.stdout.read()
        code = p2.wait()
        
        if code == 0 or "Reversed (or previously applied) patch detected!" in output:
            print "Patched successfully, skipping manual diff"
        else:
            print ('vimdiff %s %s %s' % args)
            code = os.system('vimdiff %s %s %s' % args)
            assert code == 0



if __name__ == '__main__':
    if len(sys.argv) != 4:
        print 'Usage: ./cpython_merge_helper.py <cpython_hg_repo> <pyston_repo> <skip>'
        sys.exit(1)
    cpython_repo = os.path.abspath(sys.argv[1]) + '/'
    pyston_repo = os.path.abspath(sys.argv[2])
    skip = int(sys.argv[3])
    print cpython_repo
    main(cpython_repo, pyston_repo, skip)
