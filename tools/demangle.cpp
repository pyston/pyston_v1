// Copyright (c) 2014 Dropbox, Inc.
// 
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// 
//    http://www.apache.org/licenses/LICENSE-2.0
// 
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

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
