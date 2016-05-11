#include "Halide.h"
#include <stdio.h>
#ifndef _MSC_VER
#include <unistd.h>
#endif

using namespace Halide;

Target from_string(const char* target_string) {
    Target t;
    if (!t.from_string(target_string)) {
        fprintf(stderr, "from_string failed\n");
        exit(-1);
    }
    return t;
}

void testCompileToOutput(Func j) {
    const char *fn_object = "compile_to_multitarget.o";

    #ifndef _MSC_VER
    if (access(fn_object, F_OK) == 0) { unlink(fn_object); }
    assert(access(fn_object, F_OK) != 0 && "Output file already exists.");
    #endif

    std::vector<Target> targets = {
         from_string("host-profile-debug"),
         from_string("host-profile"),
    };
    j.compile_to_multitarget_object(fn_object, j.infer_arguments(), "my_func", targets);

    #ifndef _MSC_VER
    assert(access(fn_object, F_OK) == 0 && "Output file not created.");
    #endif
}

int main(int argc, char **argv) {
    Param<float> factor("factor");
    Func f, g, h, j;
    Var x, y;
    f(x, y) = x + y;
    g(x, y) = cast<float>(f(x, y) + f(x+1, y));
    h(x, y) = f(x, y) + g(x, y);
    j(x, y) = h(x, y) * 2 * factor;

    f.compute_root();
    g.compute_root();
    h.compute_root();

    testCompileToOutput(j);

    printf("Success!\n");
    return 0;
}
