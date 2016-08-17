#include "Halide.h"

namespace {

enum class SomeEnum { Foo,
                      Bar };

class MetadataTester : public Halide::Generator<MetadataTester> {
public:
    GeneratorParam<Type> input_type{ "input_type", UInt(16) };  // deliberately wrong value, must be overridden to UInt(8)
    GeneratorParam<int> input_dim{ "input_dim", 2 };            // deliberately wrong value, must be overridden to 3
    GeneratorParam<Type> output_type{ "output_type", Float(32) };

    Input<Func> input{ "input", input_type, input_dim };
    Input<bool> b{ "b", true };
    Input<int8_t> i8{ "i8", 8, -8, 127 }; 
    Input<int16_t> i16{ "i16", 16, -16, 127 };
    Input<int32_t> i32{ "i32", 32, -32, 127 };
    Input<int64_t> i64{ "i64", 64, -64, 127 };
    Input<uint8_t> u8{ "u8", 80, 8, 255 };
    Input<uint16_t> u16{ "u16", 160, 16, 2550 };
    Input<uint32_t> u32{ "u32", 320, 32, 2550 };
    Input<uint64_t> u64{ "u64", 640, 64, 2550 };
    Input<float> f32{ "f32", 32.1234f, -3200.1234f, 3200.1234f };
    Input<double> f64{ "f64", 64.25f, -6400.25f, 6400.25f };
    Input<void *> h{ "h", nullptr };

    Func build() {
        Var x, y, c;

        Func f1, f2;
        f1(x, y, c) = cast(output_type, input(x, y, c));
        f2(x, y, c) = cast<float>(f1(x, y, c) + 1);

        Func output("output");
        output(x, y, c) = Tuple(f1(x, y, c), f2(x, y, c));
        return output;
    }
};

Halide::RegisterGenerator<MetadataTester> register_MetadataTester{ "metadata_tester" };

}  // namespace
