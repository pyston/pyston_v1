import warnings
import _warnings

warnings.filterwarnings('error')

try:
    warnings.warn("hello world", Warning)
except Warning as w:
    print(w.args[0])

try:
    _warnings.warn("deperecated", Warning)
except Warning as w:
    print(w.args[0])
