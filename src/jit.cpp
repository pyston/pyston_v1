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

#include <cassert>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <readline/history.h>
#include <readline/readline.h>
#include <stdint.h>
#include <sys/time.h>
#include <unistd.h>

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Signals.h"

#include "codegen/entry.h"
#include "codegen/irgen/hooks.h"
#include "codegen/parser.h"
#include "core/ast.h"
#include "core/common.h"
#include "core/options.h"
#include "core/stats.h"
#include "core/threading.h"
#include "core/types.h"
#include "core/util.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"


#ifndef GITREV
#error
#endif

using namespace pyston;

int main(int argc, char** argv) {
    Timer _t("for jit startup");
    // llvm::sys::PrintStackTraceOnErrorSignal();
    // llvm::PrettyStackTraceProgram X(argc, argv);
    llvm::llvm_shutdown_obj Y;

    int code;
    bool caching = true;
    bool force_repl = false;
    bool repl = true;
    bool stats = false;
    while ((code = getopt(argc, argv, "+Oqcdibpjtrsvnx")) != -1) {
        if (code == 'O')
            FORCE_OPTIMIZE = true;
        else if (code == 't')
            TRAP = true;
        else if (code == 'q')
            GLOBAL_VERBOSITY = 0;
        else if (code == 'v')
            GLOBAL_VERBOSITY++;
        // else if (code == 'c') // now always enabled
        // caching = true;
        else if (code == 'd')
            SHOW_DISASM = true;
        else if (code == 'i')
            force_repl = true;
        else if (code == 'n') {
            ENABLE_INTERPRETER = false;
        } else if (code == 'p') {
            PROFILE = true;
        } else if (code == 'j') {
            DUMPJIT = true;
        } else if (code == 's') {
            stats = true;
        } else if (code == 'r') {
            USE_STRIPPED_STDLIB = true;
        } else if (code == 'b') {
            USE_REGALLOC_BASIC = false;
        } else if (code == 'x') {
            ENABLE_PYPA_PARSER = true;
        } else if (code == '?')
            abort();
    }

    const char* fn = NULL;

    threading::registerMainThread();
    threading::acquireGLRead();

    {
        Timer _t("for initCodegen");
        initCodegen();
    }

    if (optind != argc) {
        fn = argv[optind];
        if (strcmp("-", fn) == 0)
            fn = NULL;
        else if (!force_repl)
            repl = false;

        for (int i = optind; i < argc; i++) {
            addToSysArgv(argv[i]);
        }
    } else {
        addToSysArgv("");
    }

    std::string self_path = llvm::sys::fs::getMainExecutable(argv[0], (void*)main);
    assert(self_path.size());

    llvm::SmallString<128> stdlib_dir(self_path);
    llvm::sys::path::remove_filename(stdlib_dir); // executable name
    llvm::sys::path::append(stdlib_dir, "from_cpython");
    llvm::sys::path::append(stdlib_dir, "Lib");
    appendToSysPath(stdlib_dir.c_str());

    // go from ./from_cpython/Lib to ./lib_pyston
    llvm::sys::path::remove_filename(stdlib_dir);
    llvm::sys::path::remove_filename(stdlib_dir);
    llvm::sys::path::append(stdlib_dir, "lib_pyston");
    appendToSysPath(stdlib_dir.c_str());

    // end of argument parsing

    _t.split("to run");
    BoxedModule* main_module = NULL;
    if (fn != NULL) {
        llvm::SmallString<128> path;

        if (!llvm::sys::path::is_absolute(fn)) {
            char cwd_buf[1026];
            char* cwd = getcwd(cwd_buf, sizeof(cwd_buf));
            assert(cwd);
            path = cwd;
        }

        llvm::sys::path::append(path, fn);
        llvm::sys::path::remove_filename(path);
        prependToSysPath(path.str());

        try {
            main_module = createAndRunModule("__main__", fn);
        } catch (ExcInfo e) {
            if (e.matches(SystemExit)) {
                printf("Warning: ignoring SystemExit code\n");
                return 1;
            } else {
                std::string msg = formatException(e.value);
                printLastTraceback();
                fprintf(stderr, "%s\n", msg.c_str());
                return 1;
            }
        }
    }

    if (repl) {
        printf("Pyston v%d.%d (rev " STRINGIFY(GITREV) ")", PYSTON_VERSION_MAJOR, PYSTON_VERSION_MINOR);
        printf(", targeting Python %d.%d.%d\n", PYTHON_VERSION_MAJOR, PYTHON_VERSION_MINOR, PYTHON_VERSION_MICRO);

        if (!main_module) {
            main_module = createModule("__main__", "<stdin>");
        } else {
            main_module->fn = "<stdin>";
        }

        while (repl) {
            char* line = readline(">> ");

            if (!line) {
                repl = false;
            } else {
                add_history(line);
                int size = strlen(line);

                Timer _t("repl");

                char buf[] = "pystontmp_XXXXXX";
                char* tmpdir = mkdtemp(buf);
                assert(tmpdir);
                std::string tmp = std::string(tmpdir) + "/in.py";
                if (VERBOSITY() >= 1) {
                    printf("writing %d bytes to %s\n", size, tmp.c_str());
                }

                FILE* f = fopen(tmp.c_str(), "w");
                fwrite(line, 1, size, f);
                fclose(f);

                AST_Module* m = parse(tmp.c_str());
                removeDirectoryIfExists(tmpdir);

                if (m->body.size() > 0 && m->body[0]->type == AST_TYPE::Expr) {
                    AST_Expr* e = ast_cast<AST_Expr>(m->body[0]);
                    AST_Call* c = new AST_Call();
                    AST_Name* r = new AST_Name("repr", AST_TYPE::Load, 0);
                    c->func = r;
                    c->starargs = NULL;
                    c->kwargs = NULL;
                    c->args.push_back(e->value);
                    c->lineno = 0;

                    AST_Print* p = new AST_Print();
                    p->dest = NULL;
                    p->nl = true;
                    p->values.push_back(c);
                    p->lineno = 0;
                    m->body[0] = p;
                }

                try {
                    compileAndRunModule(m, main_module);
                } catch (ExcInfo e) {
                    if (e.matches(SystemExit)) {
                        printf("Warning: ignoring SystemExit code\n");
                        return 1;
                    } else {
                        std::string msg = formatException(e.value);
                        printLastTraceback();
                        fprintf(stderr, "%s\n", msg.c_str());
                    }
                }
            }
        }
    }

    threading::finishMainThread();

    // Acquire the GIL to make sure we stop the other threads, since we will tear down
    // data structures they are potentially running on.
    // Note: we will purposefully not release the GIL on exiting.
    threading::promoteGL();

    _t.split("joinRuntime");

    int rtncode = joinRuntime();
    _t.split("finishing up");

    if (VERBOSITY() >= 1 || stats)
        Stats::dump();

    return rtncode;
}
