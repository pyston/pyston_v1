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

#include "llvm/ADT/DenseMap.h"

#include "core/common.h"
#include "core/stringpool.h"

namespace pyston {

class AST;
class AST_Module;
class AST_Expression;
class AST_Suite;

// Each closure has an array (fixed-size for that particular scope) of variables
// and a parent pointer to a parent closure. To look up a variable from the passed-in
// closure (i.e., DEREF), you just need to know (i) how many parents up to go and
// (ii) what offset into the array to find the variable. This struct stores that
// information. You can query the ScopeInfo with a name to get this info.
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

    // For a variable with DEREF lookup, return the DerefInfo used to lookup
    // the variable in a passed closure.
    virtual DerefInfo getDerefInfo(InternedString name) = 0;

    // Gets the DerefInfo for each DEREF variable accessible in the scope.
    // The returned vector is in SORTED ORDER by the `num_parents_from_passed_closure` field
    // (ascending). This allows the caller to iterate through the vector while also walking up
    // the closure chain to collect all the DEREF variable values. This is useful, for example,
    // in the implementation of locals().
    //
    // Note that:
    //      (a) This may not return a variable even if it is in the passed-in scope,
    //          if the variable is not actually used in this scope or any child
    //          scopes. This can happen, because the variable
    //          could be in the closure to be accessed by a different function, e.g.
    //
    //                  def f();
    //                      a = 0
    //                      b = 0
    //                      def g():
    //                           print a
    //                      def h():
    //                           print b
    //                           # locals() should not contain `a` even though `h` is
    //                           # passed a closure object with `a` in it
    //                           print locals()
    //
    //      (b) This can contain a variable even if it is not access in this scope,
    //          if it used in a child scope instead. For example:
    //
    //                  def f():
    //                       a = 0
    //                       def g():
    //                           def h():
    //                               print a
    //                           print locals() # should contain `a`
    virtual const std::vector<std::pair<InternedString, DerefInfo>>& getAllDerefVarsAndInfo() = 0;

    // For a variable with CLOSURE lookup, returns the offset within the `elts`
    // array of a closure that this variable is stored.
    virtual size_t getClosureOffset(InternedString name) = 0;

    // Returns the size of the `elts` array for a closure created by this scope.
    // Should only be called if this scope creates a closure.
    virtual size_t getClosureSize() = 0;

    virtual InternedString mangleName(InternedString id) = 0;
    virtual InternedString internString(llvm::StringRef) = 0;
};

class ScopingAnalysis {
public:
    struct ScopeNameUsage;
    typedef llvm::DenseMap<AST*, ScopeNameUsage*> NameUsageMap;

private:
    llvm::DenseMap<AST*, ScopeInfo*> scopes;
    AST_Module* parent_module;
    InternedStringPool* interned_strings;

    llvm::DenseMap<AST*, AST*> scope_replacements;

    ScopeInfo* analyzeSubtree(AST* node);
    void processNameUsages(NameUsageMap* usages);

    bool globals_from_module;

public:
    // The scope-analysis is done before any CFG-ization is done,
    // but many of the queries will be done post-CFG-ization.
    // The CFG process can replace scope AST nodes with others (ex:
    // generator expressions with generator functions), so we need to
    // have a way of mapping the original analysis with the new queries.
    // This is a hook for the CFG process to register when it has replaced
    // a scope-node with a different node.
    void registerScopeReplacement(AST* original_node, AST* new_node);

    ScopingAnalysis(AST* ast, bool globals_from_module);
    ScopeInfo* getScopeInfoForNode(AST* node);

    InternedStringPool& getInternedStrings();
    bool areGlobalsFromModule() { return globals_from_module; }
};

bool containsYield(AST* ast);

class BoxedString;
BoxedString* mangleNameBoxedString(BoxedString* id, BoxedString* private_name);
}

#endif
