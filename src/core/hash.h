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

#ifndef PYSTON_CORE_HASH_H
#define PYSTON_CORE_HASH_H

#include <iostream>
#include <openssl/evp.h>
#include <string>

#include "llvm/Support/raw_ostream.h"

namespace pyston {

// Stream which calculates the SHA256 hash of the data written to it.
class SHA256OStream : public llvm::raw_ostream {
    EVP_MD_CTX* md_ctx;

    void write_impl(const char* ptr, size_t size) override { EVP_DigestUpdate(md_ctx, ptr, size); }
    uint64_t current_pos() const override { return 0; }

public:
    SHA256OStream() {
        md_ctx = EVP_MD_CTX_create();
        RELEASE_ASSERT(md_ctx, "");
        int ret = EVP_DigestInit_ex(md_ctx, EVP_sha256(), NULL);
        RELEASE_ASSERT(ret == 1, "");
    }
    ~SHA256OStream() { EVP_MD_CTX_destroy(md_ctx); }

    std::string getHash() {
        flush();
        unsigned char md_value[EVP_MAX_MD_SIZE];
        unsigned int md_len = 0;
        int ret = EVP_DigestFinal_ex(md_ctx, md_value, &md_len);
        RELEASE_ASSERT(ret == 1, "");

        std::string str;
        str.reserve(md_len * 2 + 1);
        llvm::raw_string_ostream stream(str);
        for (int i = 0; i < md_len; ++i)
            stream.write_hex(md_value[i]);
        return stream.str();
    }

    void getHash(uint64_t* hash_output) {
        flush();
        unsigned char md_value[EVP_MAX_MD_SIZE];
        unsigned int md_len = 0;
        int ret = EVP_DigestFinal_ex(md_ctx, md_value, &md_len);
        RELEASE_ASSERT(ret == 1, "");
        RELEASE_ASSERT(md_len = 32, "");

        uint64_t* p = reinterpret_cast<uint64_t*>(&md_value[0]);
        hash_output[0] = p[0];
        hash_output[1] = p[1];
        hash_output[2] = p[2];
        hash_output[3] = p[3];
    }
};
}

#endif
