# skip-if: True
# Can passed in local machine, but failed on travis CI with _tkinter.TclError: out of stack space,
# threading issue?
import os
import unittest
from test import test_support

# Skip this test if _tkinter wasn't built or gui resource is not available.
test_support.import_module('_tkinter')
test_support.requires('gui')

this_dir = os.path.dirname(os.path.abspath(__file__))
# Pyston change: modify the search path
lib_tk_test = os.path.abspath(os.path.join(this_dir, '../../from_cpython/Lib', 'lib-tk', 'test'))
# lib_tk_test = os.path.abspath(os.path.join(this_dir, os.path.pardir,
#     'lib-tk', 'test'))

with test_support.DirsOnSysPath(lib_tk_test):
    import runtktests

import ttk
from _tkinter import TclError

try:
    ttk.Button()
except TclError, msg:
    # assuming ttk is not available
    raise unittest.SkipTest("ttk not available: %s" % msg)

def test_main(enable_gui=False):
    if enable_gui:
        if test_support.use_resources is None:
            test_support.use_resources = ['gui']
        elif 'gui' not in test_support.use_resources:
            test_support.use_resources.append('gui')

    with test_support.DirsOnSysPath(lib_tk_test):
        from test_ttk.support import get_tk_root
        try:
            test_support.run_unittest(
                *runtktests.get_tests(text=False, packages=['test_ttk']))
        finally:
            get_tk_root().destroy()

if __name__ == '__main__':
    test_main(enable_gui=True)
