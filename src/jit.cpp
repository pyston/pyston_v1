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

#include "osdefs.h"

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

// returns true iff we got a request to exit, i.e. SystemExit, placing the
// return code in `*retcode`. does not touch `*retcode* if it returns false.
static bool handle_toplevel_exn(const ExcInfo& e, int* retcode) {
    if (e.matches(SystemExit)) {
        Box* code = e.value->getattr("code");
        *retcode = 1;
        if (code && isSubclass(code->cls, pyston::int_cls))
            *retcode = static_cast<BoxedInt*>(code)->n;
        return true;
    }
    e.printExcAndTraceback();
    return false;
}

int main(int argc, char** argv) {
    Timer _t("for jit startup");
    // llvm::sys::PrintStackTraceOnErrorSignal();
    // llvm::PrettyStackTraceProgram X(argc, argv);
    llvm::llvm_shutdown_obj Y;

    int code;
    bool force_repl = false;
    bool stats = false;
    const char* command = NULL;
    while ((code = getopt(argc, argv, "+OqdIibpjtrsvnxc:")) != -1) {
        if (code == 'O')
            FORCE_OPTIMIZE = true;
        else if (code == 't')
            TRAP = true;
        else if (code == 'q')
            GLOBAL_VERBOSITY = 0;
        else if (code == 'v')
            GLOBAL_VERBOSITY++;
        else if (code == 'd')
            SHOW_DISASM = true;
        else if (code == 'I')
            FORCE_INTERPRETER = true;
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
        } else if (code == 'c') {
            command = optarg;
            // no more option parsing; the rest of our arguments go into sys.argv.
            break;
        } else
            abort();
    }

    const char* fn = NULL;

    threading::registerMainThread();
    threading::acquireGLRead();

    Py_SetProgramName(argv[0]);

    {
        Timer _t("for initCodegen");
        initCodegen();
    }

    // Arguments left over after option parsing are of the form:
    //     [ script | - ] [ arguments... ]
    // unless we've been already parsed a `-c command` option, in which case only:
    //     [ arguments...]
    // are parsed.
    if (command)
        addToSysArgv("-c");
    else if (optind != argc) {
        addToSysArgv(argv[optind]);
        if (strcmp("-", argv[optind]) != 0)
            fn = argv[optind];
        ++optind;
    } else
        addToSysArgv("");

    for (int i = optind; i < argc; i++) {
        addToSysArgv(argv[i]);
    }

    llvm::StringRef module_search_path = Py_GetPath();
    while (true) {
        std::pair<llvm::StringRef, llvm::StringRef> split_str = module_search_path.split(DELIM);
        if (split_str.first == module_search_path)
            break; // could not find the delimiter
        appendToSysPath(split_str.first);
        module_search_path = split_str.second;
    }

    // end of argument parsing

    _t.split("to run");
    BoxedModule* main_module = NULL;

    // if the user invoked `pyston -c command`
    if (command != NULL) {
        main_module = createModule("__main__", "<string>");
        AST_Module* m = parse_string(command);
        try {
            compileAndRunModule(m, main_module);
        } catch (ExcInfo e) {
            int retcode = 1;
            (void)handle_toplevel_exn(e, &retcode);
            return retcode;
        }
    }

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
            int retcode = 1;
            (void)handle_toplevel_exn(e, &retcode);
            return retcode;
        }
    }

    if (force_repl || !(command || fn)) {
        printf("Pyston v%d.%d (rev " STRINGIFY(GITREV) ")", PYSTON_VERSION_MAJOR, PYSTON_VERSION_MINOR);
        printf(", targeting Python %d.%d.%d\n", PYTHON_VERSION_MAJOR, PYTHON_VERSION_MINOR, PYTHON_VERSION_MICRO);

        if (!main_module) {
            main_module = createModule("__main__", "<stdin>");
        } else {
            main_module->fn = "<stdin>";
        }

        for (;;) {
            char* line = readline(">> ");
            if (!line)
                break;

            add_history(line);

            AST_Module* m = parse_string(line);

            Timer _t("repl");

            if (m->body.size() > 0 && m->body[0]->type == AST_TYPE::Expr) {
                AST_Expr* e = ast_cast<AST_Expr>(m->body[0]);
                AST_Call* c = new AST_Call();
                AST_Name* r = new AST_Name(m->interned_strings->get("repr"), AST_TYPE::Load, 0);
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
                int retcode = 0xdeadbeef; // should never be seen
                if (handle_toplevel_exn(e, &retcode))
                    return retcode;
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
