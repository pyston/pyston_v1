// Copyright (c) 2014-2015 Dropbox, Inc.
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

#ifndef PYSTON_CODEGEN_ENTRY_H
#define PYSTON_CODEGEN_ENTRY_H

#include <string>
#include <openssl/evp.h>

#include <llvm/Support/raw_ostream.h>

namespace pyston {

class AST_Module;
class BoxedModule;

void initCodegen();
void teardownCodegen();
void printAllIR();
int joinRuntime();

// Stream which calculates the SHA256 hash of the data writen to.
class HashOStream : public llvm::raw_ostream {
    EVP_MD_CTX* md_ctx;
    void write_impl(const char* ptr, size_t size) override;
    uint64_t current_pos() const override;

public:
    HashOStream();
    ~HashOStream();
    std::string getHash();
};
}

#endif
