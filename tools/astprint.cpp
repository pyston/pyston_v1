#include "analysis/scoping_analysis.h"
#include "codegen/parser.h"
#include "codegen/entry.h"
#include "core/cfg.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

using namespace pyston;


int main(int argc, char const ** argv) {
    GLOBAL_VERBOSITY=0;
    threading::registerMainThread();
    threading::GLReadRegion _glock;
    initCodegen();

    if(argc > 2 && argv[1][0] == '-' && argv[1][1] == 'x') {
        ENABLE_PYPA_PARSER = false;
    }

    std::string fn = argv[1 + int(argc > 2)];

    try {
        AST_Module* m = caching_parse(fn.c_str());
        PrintVisitor* visitor = new PrintVisitor(4);
        visitor->visit_module(m);
    } catch (Box* b) {
        std::string msg = formatException(b);
        printLastTraceback();
        fprintf(stderr, "%s\n", msg.c_str());

        return 1;
    }
    return 0;
}
