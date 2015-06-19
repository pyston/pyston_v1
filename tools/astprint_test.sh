#! /bin/sh

TOTAL_RESULT=0

pushd `dirname $0` > /dev/null
SCRIPTPATH=`pwd -P`
popd > /dev/null

pad=$(printf '%0.1s' " "{1..90})
TESTSDIR=`realpath $SCRIPTPATH/../test/tests`

mkdir -p results
for test_script in `find $TESTSDIR -name "*.py"`
do
    resultspath="./results/${test_script#$TESTSDIR}"
    echo $resultspath | grep "encoding" && continue
    echo $resultspath | grep "codec" && continue

    $SCRIPTPATH/astprint $test_script 2>&1 > /dev/null || (echo $test_script "[SKIPPED]" && continue)

    mkdir -p `dirname $resultspath`
    touch $resultspath.python $resultspath.pypa
    $SCRIPTPATH/astprint $test_script > $resultspath.python
    grep "Warning: converting unicode literal to str" $resultspath.python 2>&1 > /dev/null && rm -f $resultspath.python && continue
    $SCRIPTPATH/astprint -x $test_script > $resultspath.pypa
    diff -q $resultspath.python $resultspath.pypa 2>&1 > /dev/null && result=`echo -e "[\033[0;32mSUCCESS\033[0m]"` && rm -f $resultspath.pypa $resultspath.python || result=`echo -e "[\033[0;31mFAILED\033[0m]"` && TOTALRESULT=1
    reltestscript=$(perl -MFile::Spec -e "print File::Spec->abs2rel(q($test_script),q($SCRIPTPATH))")
    echo "$reltestscript${pad:${#test_script}} " $result
done
