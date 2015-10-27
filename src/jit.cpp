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
#include <fcntl.h>
#include <limits.h>
#include <readline/history.h>
#include <readline/readline.h>
#include <stdint.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Signals.h"

#include "osdefs.h"
#include "patchlevel.h"

#include "asm_writing/disassemble.h"
#include "capi/types.h"
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
#include "runtime/import.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"


#ifndef GITREV
#error
#endif

namespace pyston {

extern void setEncodingAndErrors();


static bool unbuffered = false;

static const char* argv0;
static int pipefds[2];
static void signal_parent_watcher() {
    // Send our current PID to the parent, in case we forked.
    union {
        char buf[4];
        int pid;
    };
    pid = getpid();
    int r = write(pipefds[1], buf, 4);
    RELEASE_ASSERT(r == 4, "");

    while (true) {
        sleep(1);
    }
}

static void handle_sigsegv(int signum) {
    assert(signum == SIGSEGV);
    fprintf(stderr, "child encountered segfault!  signalling parent watcher to backtrace.\n");

    signal_parent_watcher();
}

static void handle_sigabrt(int signum) {
    assert(signum == SIGABRT);
    fprintf(stderr, "child aborted!  signalling parent watcher to backtrace.\n");

    signal_parent_watcher();
}

static int gdb_child_pid;
static void propagate_sig(int signum) {
    // fprintf(stderr, "parent received signal %d, passing to child and then ignoring\n", signum);
    assert(gdb_child_pid);
    int r = kill(gdb_child_pid, signum);
    assert(!r);
}

static void enableGdbSegfaultWatcher() {
    int r = pipe2(pipefds, 0);
    RELEASE_ASSERT(r == 0, "");

    gdb_child_pid = fork();
    if (gdb_child_pid) {
        // parent watcher process

        close(pipefds[1]);

        for (int i = 0; i < _NSIG; i++) {
            if (i == SIGCHLD)
                continue;
            signal(i, &propagate_sig);
        }

        while (true) {
            union {
                char buf[4];
                int died_child_pid;
            };
            int r = read(pipefds[0], buf, 4);

            if (r > 0) {
                RELEASE_ASSERT(r == 4, "%d", r);

                fprintf(stderr, "Parent process woken up by child %d; collecting backtrace and killing child\n",
                        died_child_pid);
                char pidbuf[20];
                snprintf(pidbuf, sizeof(pidbuf), "%d", died_child_pid);

                close(STDOUT_FILENO);
                dup2(STDERR_FILENO, STDOUT_FILENO);
                if (gdb_child_pid != died_child_pid) {
                    // If the non-direct-child died, we want to backtrace the one that signalled us,
                    // but we want to make sure to kill the original child.
                    char origpid_buf[30];
                    snprintf(origpid_buf, sizeof(origpid_buf), "attach %d", gdb_child_pid);

                    r = execlp("gdb", "gdb", "-p", pidbuf, argv0, "-batch", "-ex", "set pagination 0", "-ex",
                               "thread apply all bt", "-ex", "kill", "-ex", origpid_buf, "-ex", "kill", "-ex",
                               "quit -11", NULL);
                } else {
                    r = execlp("gdb", "gdb", "-p", pidbuf, argv0, "-batch", "-ex", "set pagination 0", "-ex",
                               "thread apply all bt", "-ex", "kill", "-ex", "quit -11", NULL);
                }
                RELEASE_ASSERT(0, "%d %d %s", r, errno, strerror(errno));
            }

            if (r == 0) {
                int status;
                r = waitpid(gdb_child_pid, &status, 0);
                RELEASE_ASSERT(r == gdb_child_pid, "%d %d %s", r, errno, strerror(errno));

                int rtncode = 0;
                if (WIFEXITED(status))
                    rtncode = WEXITSTATUS(status);
                else {
                    int from_signal = WTERMSIG(status);

                    // Try to die in the same way that the child did:
                    signal(from_signal, SIG_DFL);
                    raise(from_signal);

                    // If somehow that didn't work, fall back to this:
                    exit(128 + from_signal);
                }

                exit(rtncode);
            }

            RELEASE_ASSERT(0, "%d %d %s", r, errno, strerror(errno));
        }
        RELEASE_ASSERT(0, "");
    }

    close(pipefds[0]);
    signal(SIGSEGV, &handle_sigsegv);
    signal(SIGABRT, &handle_sigabrt);
}

int handleArg(char code) {
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
    else if (code == 'i') {
        Py_InspectFlag = true;
        Py_InteractiveFlag = true;
    } else if (code == 'n') {
        ENABLE_INTERPRETER = false;
    } else if (code == 'a') {
        ASSEMBLY_LOGGING = true;
    } else if (code == 'p') {
        PROFILE = true;
    } else if (code == 'j') {
        DUMPJIT = true;
    } else if (code == 's') {
        Stats::setEnabled(true);
    } else if (code == 'S') {
        Py_NoSiteFlag = 1;
    } else if (code == 'U') {
        Py_UnicodeFlag++;
    } else if (code == 'u') {
        unbuffered = true;
    } else if (code == 'r') {
        USE_STRIPPED_STDLIB = true;
    } else if (code == 'b') {
        USE_REGALLOC_BASIC = false;
    } else if (code == 'x') {
        ENABLE_PYPA_PARSER = false;
    } else if (code == 'X') {
        ENABLE_CPYTHON_PARSER = true;
    } else if (code == 'E') {
        Py_IgnoreEnvironmentFlag = 1;
    } else if (code == 'P') {
        PAUSE_AT_ABORT = true;
    } else if (code == 'F') {
        CONTINUE_AFTER_FATAL = true;
    } else if (code == 'T') {
        ENABLE_TRACEBACKS = false;
    } else if (code == 'G') {
        enableGdbSegfaultWatcher();
    } else {
        fprintf(stderr, "Unknown option: -%c\n", code);
        return 2;
    }
    return 0;
}

static int RunModule(const char* module, int set_argv0) {
    PyObject* runpy, *runmodule, *runargs, *result;
    runpy = PyImport_ImportModule("runpy");
    if (runpy == NULL) {
        fprintf(stderr, "Could not import runpy module\n");
        return -1;
    }
    runmodule = PyObject_GetAttrString(runpy, "_run_module_as_main");
    if (runmodule == NULL) {
        fprintf(stderr, "Could not access runpy._run_module_as_main\n");
        Py_DECREF(runpy);
        return -1;
    }
    runargs = Py_BuildValue("(si)", module, set_argv0);
    if (runargs == NULL) {
        fprintf(stderr, "Could not create arguments for runpy._run_module_as_main\n");
        Py_DECREF(runpy);
        Py_DECREF(runmodule);
        return -1;
    }
    result = PyObject_Call(runmodule, runargs, NULL);
    if (result == NULL) {
        PyErr_Print();
    }
    Py_DECREF(runpy);
    Py_DECREF(runmodule);
    Py_DECREF(runargs);
    if (result == NULL) {
        return -1;
    }
    Py_DECREF(result);
    return 0;
}

static int RunMainFromImporter(const char* filename) {
    PyObject* argv0 = NULL, * importer = NULL;

    if ((argv0 = PyString_FromString(filename)) && (importer = PyImport_GetImporter(argv0))
        && (importer->cls != null_importer_cls)) {
        /* argv0 is usable as an import source, so
               put it in sys.path[0] and import __main__ */
        PyObject* sys_path = NULL;
        if ((sys_path = PySys_GetObject("path")) && !PyList_SetItem(sys_path, 0, argv0)) {
            Py_INCREF(argv0);
            Py_DECREF(importer);
            sys_path = NULL;
            return RunModule("__main__", 0) != 0;
        }
    }
    Py_XDECREF(argv0);
    Py_XDECREF(importer);
    if (PyErr_Occurred()) {
        PyErr_Print();
        return 1;
    }
    return -1;
}

static int main(int argc, char** argv) {
    argv0 = argv[0];

    Timer _t("for jit startup");
    // llvm::sys::PrintStackTraceOnErrorSignal();
    // llvm::PrettyStackTraceProgram X(argc, argv);
    llvm::llvm_shutdown_obj Y;

    timespec before_ts, after_ts;

    Timer main_time;
    int rtncode = 0;
    {
#if STAT_TIMERS
        StatTimer timer(Stats::getStatCounter("us_timer_main_toplevel"), 0, true);
        timer.pushTopLevel(main_time.getStartTime());
#endif

        int code;
        const char* command = NULL;
        const char* module = NULL;

        char* env_args = getenv("PYSTON_RUN_ARGS");

        if (env_args) {
            while (*env_args) {
                int r = handleArg(*env_args);
                if (r)
                    return r;
                env_args++;
            }
        }

        // Suppress getopt errors so we can throw them ourselves
        opterr = 0;
        while ((code = getopt(argc, argv, "+:OqdIibpjtrsRSUvnxXEac:FuPTGm:")) != -1) {
            if (code == 'c') {
                assert(optarg);
                command = optarg;
                // no more option parsing; the rest of our arguments go into sys.argv.
                break;
            } else if (code == 'm') {
                assert(optarg);
                module = optarg;
                // no more option parsing; the rest of our arguments go into sys.argv.
                break;
            } else if (code == 'R') {
                Py_HashRandomizationFlag = 1;
                break;
            } else if (code == ':') {
                fprintf(stderr, "Argument expected for the -%c option\n", optopt);
                return 2;
            } else if (code == '?') {
                fprintf(stderr, "Unknown option: -%c\n", optopt);
                return 2;
            } else {
                int r = handleArg(code);
                if (r)
                    return r;
            }
        }
        /* The variable is only tested for existence here; _PyRandom_Init will
           check its value further. */
        char* p;
        if (!Py_HashRandomizationFlag && (p = Py_GETENV("PYTHONHASHSEED")) && *p != '\0')
            Py_HashRandomizationFlag = 1;

        _PyRandom_Init();
        Stats::startEstimatingCPUFreq();

        const char* fn = NULL;

        threading::registerMainThread();
        threading::acquireGLRead();

        Py_SetProgramName(argv[0]);

        if (unbuffered) {
            setvbuf(stdin, (char*)NULL, _IONBF, BUFSIZ);
            setvbuf(stdout, (char*)NULL, _IONBF, BUFSIZ);
            setvbuf(stderr, (char*)NULL, _IONBF, BUFSIZ);
        }

        if (ASSEMBLY_LOGGING) {
            assembler::disassemblyInitialize();
        }

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
        else if (module) {
            // CPython does this...
            addToSysArgv("-c");
        } else if (optind != argc) {
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

        if (!fn) {
            // if we are in repl or command mode prepend "" to the path
            prependToSysPath("");
        }

        if (!Py_NoSiteFlag) {
            try {
                std::string module_name = "site";
                importModuleLevel(module_name, None, None, 0);
            } catch (ExcInfo e) {
                e.printExcAndTraceback();
                return 1;
            }
        }

        // Set encoding for standard streams. This needs to be done after
        // sys.path is properly set up, so that we can import the
        // encodings module.
        setEncodingAndErrors();

        Stats::endOfInit();

        _t.split("to run");
        BoxedModule* main_module = NULL;

        // if the user invoked `pyston -c command`
        if (command != NULL) {
            try {
                main_module = createModule(boxString("__main__"), "<string>");
                AST_Module* m = parse_string(command, /* future_flags = */ 0);
                compileAndRunModule(m, main_module);
                rtncode = 0;
            } catch (ExcInfo e) {
                setCAPIException(e);
                PyErr_Print();
                rtncode = 1;
            }
        } else if (module != NULL) {
            // TODO: CPython uses the same main module for all code paths
            main_module = createModule(boxString("__main__"), "<string>");
            rtncode = (RunModule(module, 1) != 0);
        } else {
            main_module = createModule(boxString("__main__"), fn ? fn : "<stdin>");
            rtncode = 0;
            if (fn != NULL) {
                rtncode = RunMainFromImporter(fn);
            }

            if (rtncode == -1 && fn != NULL) {
                llvm::SmallString<PATH_MAX> path;

                if (!llvm::sys::fs::exists(fn)) {
                    fprintf(stderr, "[Errno 2] No such file or directory: '%s'\n", fn);
                    return 2;
                }

                if (!llvm::sys::path::is_absolute(fn)) {
                    char cwd_buf[1026];
                    char* cwd = getcwd(cwd_buf, sizeof(cwd_buf));
                    assert(cwd);
                    path = cwd;
                }

                llvm::sys::path::append(path, fn);
                llvm::sys::path::remove_filename(path);
                char* real_path
                    = realpath(path.str().str().c_str(), NULL); // inefficient way of null-terminating the string
                ASSERT(real_path, "%s %s", path.str().str().c_str(), strerror(errno));
                prependToSysPath(real_path);
                free(real_path);

                try {
                    AST_Module* ast = caching_parse_file(fn, /* future_flags = */ 0);
                    compileAndRunModule(ast, main_module);
                    rtncode = 0;
                } catch (ExcInfo e) {
                    setCAPIException(e);
                    PyErr_Print();
                    rtncode = 1;
                }
            }
        }

        if (Py_InspectFlag || !(command || fn || module)) {

            PyObject* v = PyImport_ImportModule("readline");
            if (!v)
                PyErr_Clear();

            printf("Pyston v%d.%d.%d (rev " STRINGIFY(GITREV) ")", PYSTON_VERSION_MAJOR, PYSTON_VERSION_MINOR,
                   PYSTON_VERSION_MICRO);
            printf(", targeting Python %d.%d.%d\n", PY_MAJOR_VERSION, PY_MINOR_VERSION, PY_MICRO_VERSION);

            Py_InspectFlag = 0;

            PyCompilerFlags cf;
            cf.cf_flags = 0;
            rtncode = PyRun_InteractiveLoopFlags(stdin, "<stdin>", &cf);
        }

        threading::finishMainThread();

        // Acquire the GIL to make sure we stop the other threads, since we will tear down
        // data structures they are potentially running on.
        // Note: we will purposefully not release the GIL on exiting.
        threading::promoteGL();

        _t.split("joinRuntime");

        joinRuntime();
        _t.split("finishing up");

#if STAT_TIMERS
        uint64_t main_time_ended_at;
        uint64_t main_time_duration = main_time.end(&main_time_ended_at);
        static StatCounter mt("ticks_in_main");
        mt.log(main_time_duration);

        timer.popTopLevel(main_time_ended_at);
#endif
    }
    Stats::dump(true);


    return rtncode;
}

} // namespace pyston

int main(int argc, char** argv) {
    return pyston::main(argc, argv);
}
