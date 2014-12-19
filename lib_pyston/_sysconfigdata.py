# TODO: we will have to figure out a better way of generating this file

build_time_vars = {
        "CC": "gcc -pthread",
        "CXX": "g++ -pthread",
        "OPT": "-DNDEBUG -g -fwrapv -O3 -Wall -Wstrict-prototypes",
        "CFLAGS": "-fno-strict-aliasing -g -O3 -DNDEBUG -g -fwrapv -O3 -Wall -Wstrict-prototypes",
        "CCSHARED": "-fPIC",
        "LDSHARED": "gcc -pthread -shared",
        "SO": ".pyston.so",
        "AR": "ar",
        "ARFLAGS": "rc",
        }
