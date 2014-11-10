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
        ENABLE_PYPA_PARSER = true;
    }

    std::string fn = argv[1 + int(argc > 2)];

    AST_Module* m = caching_parse(fn.c_str());
    PrintVisitor* visitor = new PrintVisitor(4);
    visitor->visit_module(m);
    return 0;
}
