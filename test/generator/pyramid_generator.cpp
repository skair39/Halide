#include "Halide.h"

namespace {

class Pyramid : public Halide::Generator<Pyramid> {
public:
    GeneratorParam<int> levels{"levels", 1};  // deliberately wrong value, must be overridden to 10

    Input<Func> input{ "input", Float(32), 2 };

    Output<Func[]> pyramid{ levels, "pyramid", Float(32), 2 }; 

    void generate() {
        pyramid[0](x, y) = input(x, y);

        for (int i = 1; i < levels; i++) {
            pyramid[i](x, y) = downsample(pyramid[i-1])(x, y);
        }
    }

    void schedule() {
        for (int i = 0; i < levels; i++) {
            // No need to specify compute_root() for outputs
            pyramid[i].parallel(y);
            // Vectorize if we're still wide enough at this level
            const int v = natural_vector_size<float>();
            pyramid[i].specialize(pyramid[i].output_buffer().width() >= v).vectorize(x, v);
        }
     }

private:
    Var x, y;

    Func downsample(Func big) {
        Func small;
        small(x, y) = (big(2*x, 2*y) +
                       big(2*x+1, 2*y) +
                       big(2*x, 2*y+1) +
                       big(2*x+1, 2*y+1))/4;
        return small;
    }
};

Halide::RegisterGenerator<Pyramid> register_my_gen{"pyramid"};

}  // namespace
