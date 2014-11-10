#! /bin/sh

TOTAL_RESULT=0

pushd `dirname $0` > /dev/null
SCRIPTPATH=`pwd -P`
popd > /dev/null

TESTSDIR=`realpath $SCRIPTPATH/../test/tests`

pad=$(printf '%0.1s' " "{1..90})
for test_script in `find $TESTSDIR -name "*.py"`;
do
    $SCRIPTPATH/astprint $test_script > $test_script.python
    $SCRIPTPATH/astprint -x $test_script > $test_script.pypa
    diff -q $test_script.python $test_script.pypa 2>&1 > /dev/null && result=`echo -e "[\033[0;32mSUCCESS\033[0m]"` || result=`echo -e "[\033[0;31mFAILED\033[0m]"` && TOTALRESULT=1
    reltestscript=$(perl -MFile::Spec -e "print File::Spec->abs2rel(q($test_script),q($SCRIPTPATH))")
    echo "$reltestscript${pad:${#test_script}} " $result
done

exit $TOTAL_RESULT
