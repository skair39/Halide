#include "Halide.h"

namespace {

class Wrappee : public Halide::Generator<Wrappee> {
public:
    GeneratorParam<Type> input_type{ "input_type", UInt(8) };
    GeneratorParam<Type> output_type{ "output_type", Float(32) };
    GeneratorParam<int> array_count{ "array_count", 2 };

    Input<Func[]> input{ array_count, "input", input_type, 3 };
    Input<float> float_arg{ "float_arg", 1.0f, 0.0f, 100.0f }; 
    Input<int32_t[]> int_arg{ array_count, "int_arg", 1 };

    Output<Func> f{"f", {input_type, output_type}, 3};
    Output<Func[]> g{ array_count, "g", Int(16), 2};

    void generate() {
        assert(array_count >= 1);

        f(x, y, c) = Tuple(
                input[0](x, y, c),
                cast(output_type, input[0](x, y, c) * float_arg + int_arg[0]));

        for (int i = 0; i < array_count; ++i) {
            g[i](x, y) = cast<int16_t>(input[i](x, y, 0) + int_arg[i]);
        }
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
