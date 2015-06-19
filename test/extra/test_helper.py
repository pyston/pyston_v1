# skip-if: True
import os
import sys
import subprocess
import re

def create_virtenv(name, package_list = None, force_create = False):
    if force_create or not os.path.exists(name) or os.stat(sys.executable).st_mtime > os.stat(name + "/bin/python").st_mtime:
        if os.path.exists(name):
            subprocess.check_call(["rm", "-rf", name])
        
        print "Creating virtualenv to install testing dependencies..."
        
        VIRTUALENV_SCRIPT = os.path.dirname(__file__) + "/../integration/virtualenv/virtualenv.py"

        try:
            args = [sys.executable, VIRTUALENV_SCRIPT, "-p", sys.executable, name]
            print "Running", args
            subprocess.check_call(args)

            if package_list:
                pip_install(name, package_list)
        except:
            print "Error occurred; trying to remove partially-created directory"
            ei = sys.exc_info()
            try:
                subprocess.check_call(["rm", "-rf", name])
            except Exception as e:
                print e
            raise ei[0], ei[1], ei[2]
    else:
        print "Reusing existing virtualenv"

def pip_install(name, package_list):
    subprocess.check_call([name + "/bin/pip", "install"] + package_list)

def parse_output(output):
    result = []
    it = re.finditer("FAILED \(failures=(\d+), errors=(\d+)\)", output)
    for m in it:
        d = { "failures" : int(m.group(1)), "errors" : int(m.group(2)) }
        result.append(d)
    it = re.finditer("FAILED \(errors=(\d+), failures=(\d+)\)", output)
    for m in it:
        d = { "failures" : int(m.group(2)), "errors" : int(m.group(1)) }
        result.append(d)
    it = re.finditer("FAILED \(failures=(\d+)\)", output)
    for m in it:
        d = { "failures" : int(m.group(1)), "errors" : 0 }
        result.append(d)
    it = re.finditer("FAILED \(errors=(\d+)\)", output)
    for m in it:
        d = { "failures" : 0, "errors" : int(m.group(1)) }
        result.append(d)
    return result

def run_test(cmd, cwd, expected, env = None):
    process = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, cwd=cwd, env=env)
    output, unused_err = process.communicate()
    errcode = process.poll()
    result = parse_output(output)
    
    print
    print "Return code:", errcode
    if expected == result:
        print "Received expected output"
    else:
        print >> sys.stderr, output
        print >> sys.stderr, "WRONG output"
        print >> sys.stderr, "is:", result
        print >> sys.stderr, "expected:", expected
        assert result == expected

