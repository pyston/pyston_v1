#! /bin/bash

tools_dir=`dirname $0`
llvm_hash=$(${tools_dir}/llvm_patchset_hash.sh)
llvm_filename=llvm-${llvm_hash}.tar.gz
echo "Looking for" ${llvm_filename}
if [ -e $HOME/llvm-package/${llvm_filename} ]; then
  tar zxvf ~/llvm-package/${llvm_filename} -C $HOME
  echo "LLVM: `pwd`/llvm-install"
else
  echo "LLVM build not cached" 
  git clone git://github.com/llvm-mirror/llvm.git ~/pyston_deps/llvm-trunk
  git clone git://github.com/llvm-mirror/clang.git ~/pyston_deps/llvm-trunk/tools/clang
  git config --global user.email "you@example.com"
  git config --global user.name "Your Name"
  make -C $TRAVIS_BUILD_DIR llvm_up
fi

