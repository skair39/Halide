#include "Halide.h"
#include "wraptest.wrapper.h"

using WrapNS1::WrapNS2::Wrapper;

namespace {

class WrapUser : public Halide::Generator<WrapUser> {
public:
    GeneratorParam<Type> input_type{ "input_type", UInt(8) };
    GeneratorParam<Type> output_type{ "output_type", UInt(8) };
    GeneratorParam<int32_t> int_arg{ "int_arg", 33 };

    Input<Func> input{ "input", input_type, 3 };

    Output<Func> output{"output", output_type, 3};

    void generate() {

        Wrapper::GeneratorParams gp;
        gp.input_type = input_type;
        gp.output_type = output_type;
        // Override array_count to only expect 1 input and provide one output for g
        gp.array_count = 1;

        Expr float_arg_expr(1.234f);
        wrap = Wrapper(context(), { input }, float_arg_expr, { int_arg }, gp);

        const float kOffset = 2.f;
        output(x, y, c) = cast(output_type, wrap.f(x, y, c)[1] + kOffset);
    }

    void schedule() {
        wrap.schedule();
    }

private:
    Var x{"x"}, y{"y"}, c{"c"};
    Wrapper wrap;
};

// Note that HALIDE_REGISTER_GENERATOR() with just two args is functionally
// identical to the old HalideRegister<> syntax: no wrapper being defined,
// just AOT usage. (If you try to generate a wrapper for this class you'll
// fail with an error at generation time.)
auto register_me = HALIDE_REGISTER_GENERATOR(WrapUser, "wrap_user");

}  // namespace
