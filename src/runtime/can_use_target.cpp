#include "HalideRuntime.h"

extern "C" {

WEAK int halide_can_use_target_features(uint64_t features) {
    return true;
}

}
