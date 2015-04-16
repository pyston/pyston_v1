import sys

def sysframetest():
    return sys._getframe(0)

def sysframetestwrapper():
    return sysframetest()

fr = sysframetest()
print sysframetest.__name__
print fr.f_code.co_name
print fr.f_code.co_filename

fr = sysframetestwrapper()
print sysframetestwrapper.__name__
print fr.f_code.co_name
print fr.f_code.co_filename
