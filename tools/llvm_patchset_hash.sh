#! /bin/bash
pyston_dir=`dirname $0`/..
cd $pyston_dir
sha1sum llvm_revision.txt llvm_patches/*.patch clang_patches/*.patch | sort | sha1sum | cut -b-7
