import os, sys
sys.path.append(os.path.dirname(__file__) + "/../lib")

EXTRA_PATH = os.path.dirname(os.path.abspath(__file__)) + "/../lib/decorator/src"
sys.path.insert(0, EXTRA_PATH)
EXTRA_PATH = os.path.dirname(os.path.abspath(__file__)) + "/../lib/decorator"
sys.path.insert(0, EXTRA_PATH)

import decorator
import tests.test

import unittest
unittest.main(tests.test)
