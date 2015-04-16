"""
Frame Hack Recipe #1: Ruby-style string interpolation (version 1)
"""
# from http://farmdev.com/src/secrets/framehack/interpolate/solutions/interpolate1.py
import sys
from string import Template

def interpolate(templateStr):
    frame = sys._getframe(1)
    framedict = frame.f_locals

    t = Template(templateStr)
    return t.substitute(**framedict)
    
name = 'Feihong'
place = 'Chicago'
print interpolate("My name is ${name}. I work in ${place}.")
