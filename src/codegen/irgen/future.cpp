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

#include "future.h"

namespace pyston {

struct FutureOption {
    int optional_version_hex;
    int mandatory_version_hex;
    int ff_mask;
};

const std::map<std::string, FutureOption> future_options
    = { { "absolute_import", { version_hex(2, 5, 0), version_hex(3, 0, 0), FF_ABSOLUTE_IMPORT } },
        { "division", { version_hex(2, 2, 0), version_hex(3, 0, 0), FF_DIVISION } },
        { "generators", { version_hex(2, 2, 0), version_hex(3, 0, 0), FF_GENERATOR } },
        { "unicode_literals", { version_hex(2, 6, 0), version_hex(3, 0, 0), FF_UNICODE_LITERALS } },
        { "print_function", { version_hex(2, 6, 0), version_hex(3, 0, 0), FF_PRINT_FUNCTION } },
        { "nested_scopes", { version_hex(2, 1, 0), version_hex(2, 2, 0), FF_NESTED_SCOPES } },
        { "with_statement", { version_hex(2, 5, 0), version_hex(3, 6, 0), FF_WITH_STATEMENT } } };

// Helper function:
void raiseSyntaxError(const char* file, AST* node_at, const char* msg, ...) {
    va_list ap;
    va_start(ap, msg);

    char buf[1024];
    vsnprintf(buf, sizeof(buf), msg, ap);


    // TODO I'm not sure that it's safe to raise an exception here, since I think
    // there will be things that end up not getting cleaned up.
    // Then again, there are a huge number of things that don't get cleaned up even
    // if an exception doesn't get thrown...

    // TODO output is still a little wrong, should be, for example
    //
    //  File "../test/tests/future_non_existent.py", line 1
    //    from __future__ import rvalue_references # should cause syntax error
    //
    // but instead it is
    //
    // Traceback (most recent call last):
    //  File "../test/tests/future_non_existent.py", line -1, in :
    //    from __future__ import rvalue_references # should cause syntax error
    ::pyston::raiseSyntaxError(buf, node_at->lineno, node_at->col_offset, file, "");
}

void raiseFutureImportErrorNotFound(const char* file, AST* node, const char* name) {
    raiseSyntaxError(file, node, "future feature %s is not defined", name);
}

void raiseFutureImportErrorNotBeginning(const char* file, AST* node) {
    raiseSyntaxError(file, node, "from __future__ imports must occur at the beginning of the file");
}

class BadFutureImportVisitor : public NoopASTVisitor {
public:
    virtual bool visit_importfrom(AST_ImportFrom* node) {
        if (node->module.str() == "__future__") {
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

FutureFlags getFutureFlags(AST_Module* m, const char* file) {
    FutureFlags ff = 0;

    // Set the defaults for the future flags depending on what version we are
    for (const std::pair<std::string, FutureOption>& p : future_options) {
        if (PYTHON_VERSION_HEX >= p.second.mandatory_version_hex) {
            ff |= p.second.ff_mask;
        }
    }

    // Find all the __future__ imports, raising an error for those that do not
    // occur at the beginning of the file.
    bool future_import_allowed = true;
    BadFutureImportVisitor import_visitor(file);
    for (int i = 0; i < m->body.size(); i++) {
        AST_stmt* stmt = m->body[i];

        if (stmt->type == AST_TYPE::ImportFrom && static_cast<AST_ImportFrom*>(stmt)->module.str() == "__future__") {
            if (future_import_allowed) {
                // We have a `from __future__` import statement, and we are
                // still at the top of the file, so just set the appropriate
                // future flag for each imported option.

                for (AST_alias* alias : static_cast<AST_ImportFrom*>(stmt)->names) {
                    const std::string& option_name = alias->name.str();
                    auto iter = future_options.find(option_name);
                    if (iter == future_options.end()) {
                        // If it's not one of the available options, throw an error.
                        // Note: the __future__ module also exposes "all_feature_names",
                        // but you still can't import that using a from-import, so we
                        // don't need to worry about that here.
                        raiseFutureImportErrorNotFound(file, alias, option_name.c_str());
                    } else {
                        const FutureOption& fo = iter->second;
                        if (PYTHON_VERSION_HEX >= fo.optional_version_hex) {
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
