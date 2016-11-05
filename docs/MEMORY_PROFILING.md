There are different ways to do heap profiling but it looks like jemalloc heap profiling works currently best for Pyston. (unfortunately valgrind often crashes currently if the ICs are enabled)

## Prerequisites
Pyston comes with jemalloc which is already configured with enabled profiling.
But because we currently don't install the header one needs to run as a workaround `sudo apt-get install libjemalloc-dev` to install the system package and rebuild jemalloc afterwards.

## Usage
The jemalloc documentation about profiling can be found [here](https://github.com/jemalloc/jemalloc/wiki/Use-Case%3A-Heap-Profiling) and the one about leak checking [here](https://github.com/jemalloc/jemalloc/wiki/Use-Case%3A-Leak-Checking).

an example usage is:
```
~/pyston$ MALLOC_CONF="prof:true,prof_prefix:jeprof.out" ./pyston_release minibenchmarks/raytrace.py
~/pyston$ build_deps/jemalloc/bin/pprof pyston_release jeprof.out.*
(pprof) top3
Total: 4.5 MB
     1.0  22.2%  22.2%      1.0  22.2% pyston::BST_FunctionDef::operator new (inline)
     0.5  11.1%  33.3%      0.5  11.1% pyston::DenseMap::allocateBuckets (inline)
     0.5  11.1%  44.5%      0.5  11.1% pyston::CFG::addBlock
```

