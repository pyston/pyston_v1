# Simple optparse test, taken from the optparse.py docstring:

from optparse import OptionParser

parser = OptionParser()
parser.add_option("-f", "--file", dest="filename",
                  help="write report to FILE", metavar="FILE")
parser.add_option("-q", "--quiet",
                  action="store_false", dest="verbose", default=True,
                  help="don't print status messages to stdout")

(options, args) = parser.parse_args(['test', '--file=/dev/null', 'hello world'])
print sorted(options.__dict__.items()), args
