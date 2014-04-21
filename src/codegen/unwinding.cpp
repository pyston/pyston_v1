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

#include <dlfcn.h>
#include <sys/types.h>
#include <unistd.h>

#include "llvm/DebugInfo/DIContext.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/ExecutionEngine/JITEventListener.h"
#include "llvm/ExecutionEngine/ObjectImage.h"

#include "codegen/codegen.h"

namespace pyston {

class TracebacksEventListener : public llvm::JITEventListener {
    public:
        void NotifyObjectEmitted(const llvm::ObjectImage &Obj) {
            llvm::DIContext* Context = llvm::DIContext::getDWARFContext(Obj.getObjectFile());

            llvm::error_code ec;
            for (llvm::object::symbol_iterator I = Obj.begin_symbols(),
                    E = Obj.end_symbols();
                    I != E && !ec;
                    ++I) {
                std::string SourceFileName;

                llvm::object::SymbolRef::Type SymType;
                if (I->getType(SymType)) continue;
                if (SymType == llvm::object::SymbolRef::ST_Function) {
                    llvm::StringRef  Name;
                    uint64_t   Addr;
                    uint64_t   Size;
                    if (I->getName(Name)) continue;
                    if (I->getAddress(Addr)) continue;
                    if (I->getSize(Size)) continue;

                    llvm::DILineInfoTable lines = Context->getLineInfoForAddressRange(Addr, Size, llvm::DILineInfoSpecifier::FunctionName | llvm::DILineInfoSpecifier::FileLineInfo);
                    for (int i = 0; i < lines.size(); i++) {
                        //printf("%s:%d, %s: %lx\n", lines[i].second.getFileName(), lines[i].second.getLine(), lines[i].second.getFunctionName(), lines[i].first);
                    }
                }
            }

        }
};

std::string getPythonFuncAt(void* addr, void* sp) {
    return "";
}

llvm::JITEventListener* makeTracebacksListener() {
    return new TracebacksEventListener();
}

}
