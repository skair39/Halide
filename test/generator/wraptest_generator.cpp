#include "Halide.h"

namespace {

class Wrappee : public Halide::Generator<Wrappee> {
public:
    GeneratorParam<Type> input_type{ "input_type", UInt(8) };
    GeneratorParam<Type> output_type{ "output_type", Float(32) };

    Input<Func> input{ "input", input_type, 3 };
    Input<float> float_arg{ "float_arg", 1.0f, 0.0f, 100.0f }; 
    Input<int32_t> int_arg{ "int_arg", 1 };

    Output<Func> f{"f", {input_type, output_type}, 3};
    Output<Func> g{"g", Int(16), 2};

    void generate() {
        f(x, y, c) = Tuple(
                input(x, y, c),
                cast(output_type, input(x, y, c) * float_arg + int_arg));

        g(x, y) = cast<int16_t>(input(x, y, 0));
    }

    void schedule() {
        // empty
    }

private:
    Var x{"x"}, y{"y"}, c{"c"};
};

}  // namespace

namespace WrapNS1 {
namespace WrapNS2 {

// must forward-declare the name we want for the wrapper, inside the proper namespace(s).
// None of the namespace(s) may be anonymous (if you do, failures will occur at Halide
// compilation time).
class Wrapper;

// If the fully-qualified wrapper name specified for third argument hasn't been declared
// properly, a compile error will result. The fully-qualified name *must* have at least one
// namespace (i.e., a name at global scope is not acceptable). The class may not be inside
// anonymous namespace(s), o
auto register_me = HALIDE_REGISTER_GENERATOR(Wrappee, "wraptest", WrapNS1::WrapNS2::Wrapper);

}  // namespace WrapNS2
}  // namespace WrapNS1
