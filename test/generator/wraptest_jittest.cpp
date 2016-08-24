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
Image<Type> MakeImage() {
    Image<Type> im(kSize, kSize, 3);
    for (int x = 0; x < kSize; x++) {
        for (int y = 0; y < kSize; y++) {
            for (int c = 0; c < 3; c++) {
                im(x, y, c) = static_cast<Type>(x + y + c);
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
    for (int x = 0; x < kSize; x++) {
        for (int y = 0; y < kSize; y++) {
            for (int c = 0; c < 3; c++) {
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
    
    Image<float> src = MakeImage<float>();
    Func f = MakeFunc(src);

    Wrapper::GeneratorParams gp;
    gp.input_type = Halide::Float(32);
    gp.output_type = Halide::Int(16);
    Wrapper gen(context, f, 1.234f, 33, gp);

    gen.schedule();

    Halide::Realization r = gen.realize(kSize, kSize, 3);
    Image<int16_t> dst = r[1];
    verify(src, 1.234f, 33, dst);

    printf("Success!\n");
    return 0;
}
