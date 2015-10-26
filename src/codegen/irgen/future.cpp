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

#include "codegen/irgen/future.h"

#include <map>

#include "Python.h"

#include "core/ast.h"

namespace pyston {

struct FutureOption {
    int optional_version_hex;
    int mandatory_version_hex;
    int ff_mask;
};

const std::map<std::string, FutureOption> future_options
    = { { "absolute_import", { version_hex(2, 5, 0), version_hex(3, 0, 0), CO_FUTURE_ABSOLUTE_IMPORT } },
        { "division", { version_hex(2, 2, 0), version_hex(3, 0, 0), CO_FUTURE_DIVISION } },
        { "unicode_literals", { version_hex(2, 6, 0), version_hex(3, 0, 0), CO_FUTURE_UNICODE_LITERALS } },
        { "print_function", { version_hex(2, 6, 0), version_hex(3, 0, 0), CO_FUTURE_PRINT_FUNCTION } },
        { "with_statement", { version_hex(2, 5, 0), version_hex(3, 6, 0), CO_FUTURE_WITH_STATEMENT } },

        // These are mandatory in all versions we care about (>= 2.3)
        { "generators", { version_hex(2, 2, 0), version_hex(3, 0, 0), CO_GENERATOR } },
        { "nested_scopes", { version_hex(2, 1, 0), version_hex(2, 2, 0), CO_NESTED } } };

void raiseFutureImportErrorNotFound(const char* file, AST* node, const char* name) {
    raiseSyntaxErrorHelper(file, "", node, "future feature %s is not defined", name);
}

void raiseFutureImportErrorNotBeginning(const char* file, AST* node) {
    raiseSyntaxErrorHelper(file, "", node, "from __future__ imports must occur at the beginning of the file");
}

class BadFutureImportVisitor : public NoopASTVisitor {
public:
    virtual bool visit_importfrom(AST_ImportFrom* node) {
        if (node->module.s() == "__future__") {
            raiseFutureImportErrorNotBeginning(file, node);
        }
        return true;
    }

    // TODO optimization: have it skip things like expressions that you know
    // there is no need to descend into

    BadFutureImportVisitor(const char* file) : file(file) {}
    const char* file;
};

inline bool is_stmt_string(AST_stmt* stmt) {
    return stmt->type == AST_TYPE::Expr && static_cast<AST_Expr*>(stmt)->value->type == AST_TYPE::Str;
}

FutureFlags getFutureFlags(std::vector<AST_stmt*> const& body, const char* file) {
    FutureFlags ff = 0;

    // Set the defaults for the future flags depending on what version we are
    for (const std::pair<std::string, FutureOption>& p : future_options) {
        if (PY_VERSION_HEX >= p.second.mandatory_version_hex) {
            ff |= p.second.ff_mask;
        }
    }

    // Find all the __future__ imports, raising an error for those that do not
    // occur at the beginning of the file.
    bool future_import_allowed = true;
    BadFutureImportVisitor import_visitor(file);
    for (int i = 0; i < body.size(); i++) {
        AST_stmt* stmt = body[i];

        if (stmt->type == AST_TYPE::ImportFrom && static_cast<AST_ImportFrom*>(stmt)->module.s() == "__future__") {
            if (future_import_allowed) {
                // We have a `from __future__` import statement, and we are
                // still at the top of the file, so just set the appropriate
                // future flag for each imported option.

                for (AST_alias* alias : static_cast<AST_ImportFrom*>(stmt)->names) {
                    auto option_name = alias->name;
                    auto iter = future_options.find(option_name.s());
                    if (iter == future_options.end()) {
                        // If it's not one of the available options, throw an error.
                        // Note: the __future__ module also exposes "all_feature_names",
                        // but you still can't import that using a from-import, so we
                        // don't need to worry about that here.
                        raiseFutureImportErrorNotFound(file, alias, option_name.c_str());
                    } else {
                        const FutureOption& fo = iter->second;
                        if (PY_VERSION_HEX >= fo.optional_version_hex) {
                            ff |= fo.ff_mask;
                        } else {
                            raiseFutureImportErrorNotFound(file, alias, option_name.c_str());
                        }
                    }
                }
            } else {
                raiseFutureImportErrorNotBeginning(file, stmt);
            }
        } else {
            // A docstring is allowed at the beginning of a module; otherwise,
            // we cannot permit any __future__ import after this point.
            if (i > 0 || !is_stmt_string(stmt)) {
                // Recurse on the node and throw an error if it has any
                // `from __future__` import statement.
                stmt->accept(&import_visitor);

                future_import_allowed = false;
            }
        }
    }

    return ff;
}
}
