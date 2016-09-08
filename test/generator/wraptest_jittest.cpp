#include "Halide.h"

#include "wraptest.wrapper.h"

using Halide::Argument;
using Halide::Expr;
using Halide::Func;
using Halide::Image;
using WrapNS1::WrapNS2::Wrapper;

const int kSize = 32;

Halide::Var x, y, c;

template<typename Type>
Image<Type> MakeImage(int extra) {
    Image<Type> im(kSize, kSize, 3);
    for (int x = 0; x < kSize; x++) {
        for (int y = 0; y < kSize; y++) {
            for (int c = 0; c < 3; c++) {
                im(x, y, c) = static_cast<Type>(x + y + c + extra);
            }
        }
    }
    return im;
}

template<typename Type>
Func MakeFunc(const Image<Type>& im) {
    Func f;
    f(x, y, c) = im(x, y, c);
    return f;
}

template<typename InputType, typename OutputType>
void verify(const Image<InputType> &input, float float_arg, int int_arg, const Image<OutputType> &output) {
    if (input.width() != output.width() ||
        input.height() != output.height()) {
        fprintf(stderr, "size mismatch\n");
        exit(-1);
    }
    int channels = std::max(1, std::min(input.channels(), output.channels()));
    for (int x = 0; x < output.width(); x++) {
        for (int y = 0; y < output.height(); y++) {
            for (int c = 0; c < channels; c++) {
                const OutputType expected = static_cast<OutputType>(input(x, y, c) * float_arg + int_arg);
                const OutputType actual = output(x, y, c);
                if (expected != actual) {
                    fprintf(stderr, "img[%d, %d, %d] = %f, expected %f\n", x, y, c, (double)actual, (double)expected);
                    exit(-1);
                }
            }
        }
    }
}

int main(int argc, char **argv) {
    Halide::JITGeneratorContext context(Halide::get_target_from_environment());
    
    constexpr int kArrayCount = 2;

    Image<float> src[kArrayCount] = {
        MakeImage<float>(0),
        MakeImage<float>(1)
    };

    std::vector<int> int_args = { 33, 66 };

    // the Wrapper wants Expr, so make a conversion in place
    std::vector<Expr> int_args_expr(int_args.begin(), int_args.end());

    Wrapper::GeneratorParams gp;
    gp.input_type = Halide::Float(32);
    gp.output_type = Halide::Int(16);
    gp.array_count = kArrayCount;
    Wrapper gen(context, { MakeFunc(src[0]), MakeFunc(src[1]) }, 1.234f, int_args_expr, gp);

    gen.schedule();

    Halide::Realization f_realized = gen.realize(kSize, kSize, 3);
    Image<float> f0 = f_realized[0];
    Image<int16_t> f1 = f_realized[1];
    verify(src[0], 1.0f, 0, f0);
    verify(src[0], 1.234f, 33, f1);

    // TODO: gen.g is declared const (so we can't accidentally assign to it), 
    // but we also can't call non-const methods on it (e.g. realize(), or, well, most things). Fix.
    for (int i = 0; i < kArrayCount; ++i) {
        Func gtmp = gen.g[i];
        Halide::Realization g_realized = gtmp.realize(kSize, kSize);
        Image<int16_t> g0 = g_realized;
        verify(src[i], 1.0f, int_args[i], g0);
    }

    printf("Success!\n");
    return 0;
}
