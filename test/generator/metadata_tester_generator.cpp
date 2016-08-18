#include "Halide.h"

namespace {

enum class SomeEnum { Foo,
                      Bar };

class MetadataTester : public Halide::Generator<MetadataTester> {
public:
    GeneratorParam<Type> input_type{ "input_type", Int(16) };              // deliberately wrong value, must be overridden to UInt(8)
    GeneratorParam<int> input_dim{ "input_dim", 2 };                       // deliberately wrong value, must be overridden to 3
    GeneratorParam<Type> output_type{ "output_type", Int(16) };            // deliberately wrong value, must be overridden to Float(32)
    GeneratorParam<int> output_dim{ "output_dim", 2 };                     // deliberately wrong value, must be overridden to 3 
    GeneratorParam<int> array_outputs_count{ "array_outputs_count", 32 };  // deliberately wrong value, must be overridden to 2

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

    Output<Func> output{ "output", {output_type, Float(32)}, output_dim };
    Output<float> output_scalar{ "output_scalar" };
    Output<Func[]> array_outputs{ array_outputs_count, "array_outputs", Float(32), 3 };
    // array count of 0 means there are no outputs: for AOT, doesn't affect signature
    Output<Func[]> empty_outputs{ 0, "empty_outputs", Float(32), 3 };

    void generate() {
        Var x, y, c;

        Func f1, f2;
        f1(x, y, c) = cast(output_type, input(x, y, c));
        f2(x, y, c) = cast<float>(f1(x, y, c) + 1);

        output(x, y, c) = Tuple(f1(x, y, c), f2(x, y, c));
        output_scalar() = 1234.25f;
        for (size_t i = 0; i < array_outputs.size(); ++i) {
            array_outputs[i](x, y, c) = (i + 1) * 1.5f;
        }
    }

    void schedule() {
        // empty
    }
};

Halide::RegisterGenerator<MetadataTester> register_MetadataTester{ "metadata_tester" };

}  // namespace
