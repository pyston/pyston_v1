#include <cstdio>
#include <cstdlib>
#include <cxxabi.h>

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s IDENTIFIER\n", argv[0]);
        exit(1);
    }

    int status;
    char* demangled = abi::__cxa_demangle(argv[1], NULL, NULL, &status);
    if (demangled) {
        printf("%s\n", demangled);
    } else {
        fprintf(stderr, "Error: unable to demangle\n");
        exit(1);
    }
    return 0;
}
