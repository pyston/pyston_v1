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

#ifndef PYSTON_ANALYSIS_SCOPINGANALYSIS_H
#define PYSTON_ANALYSIS_SCOPINGANALYSIS_H

#include "core/common.h"
#include "core/stringpool.h"

namespace pyston {

class AST;
class AST_Module;
class AST_Expression;
class AST_Suite;

struct DerefInfo {
    size_t num_parents_from_passed_closure;
    size_t offset;
};

class ScopeInfo {
public:
    ScopeInfo() {}
    virtual ~ScopeInfo() {}
    virtual ScopeInfo* getParent() = 0;

    virtual bool createsClosure() = 0;
    virtual bool takesClosure() = 0;
    virtual bool passesThroughClosure() = 0;

    // Various ways a variable name can be resolved.
    // These all correspond to STORE_* or LOAD_* bytecodes in CPython.
    //
    // By way of example:
    //
    //  def f():
    //      print a # GLOBAL
    //
    //      b = 0
    //      print b # FAST
    //
    //      c = 0 # CLOSURE
    //      def g():
    //          print c # DEREF
    //
    //  class C(object):
    //      print d # NAME
    //
    //  def g():
    //      exec "sdfasdfds()"
    //      # existence of 'exec' statement forces this to NAME:
    //      print e # NAME
    //
    //  # protip: you can figure this stuff out by doing something like this in CPython:
    //  import dis
    //  print dis.dis(g)

    enum class VarScopeType {
        FAST,
        GLOBAL,
        CLOSURE,
        DEREF,
        NAME,

        // This is never returned by any function in this class, but it is used by
        // the ast_interpreter currently.
        UNKNOWN
    };
    virtual VarScopeType getScopeTypeOfName(InternedString name) = 0;

    // Returns true if the scope may contain NAME variables.
    // In particular, it returns true for ClassDef scope, for any scope
    // with an `exec` statement or `import *` statement in it, or for any
    // `exec` or `eval` scope.
    virtual bool usesNameLookup() = 0;

    virtual bool areLocalsFromModule() = 0;

    virtual DerefInfo getDerefInfo(InternedString name) = 0;
    virtual const std::vector<std::pair<InternedString, DerefInfo>>& getAllDerefVarsAndInfo() = 0;
    virtual size_t getClosureOffset(InternedString name) = 0;
    virtual size_t getClosureSize() = 0;

    virtual InternedString mangleName(InternedString id) = 0;
    virtual InternedString internString(llvm::StringRef) = 0;
};

class ScopingAnalysis {
public:
    struct ScopeNameUsage;
    typedef std::unordered_map<AST*, ScopeNameUsage*> NameUsageMap;

private:
    std::unordered_map<AST*, ScopeInfo*> scopes;
    AST_Module* parent_module;
    InternedStringPool& interned_strings;

    std::unordered_map<AST*, AST*> scope_replacements;

    ScopeInfo* analyzeSubtree(AST* node);
    void processNameUsages(NameUsageMap* usages);

public:
    // The scope-analysis is done before any CFG-ization is done,
    // but many of the queries will be done post-CFG-ization.
    // The CFG process can replace scope AST nodes with others (ex:
    // generator expressions with generator functions), so we need to
    // have a way of mapping the original analysis with the new queries.
    // This is a hook for the CFG process to register when it has replaced
    // a scope-node with a different node.
    void registerScopeReplacement(AST* original_node, AST* new_node);

    ScopingAnalysis(AST_Module* m);
    ScopingAnalysis(AST_Expression* e);
    ScopingAnalysis(AST_Suite* s);
    ScopeInfo* getScopeInfoForNode(AST* node);

    InternedStringPool& getInternedStrings();
};

bool containsYield(AST* ast);
}

#endif
