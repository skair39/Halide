#include "Halide.h"

namespace {

// TODO: convert to new-style once Input<Buffer> added
class ReactionDiffusion2Init : public Halide::Generator<ReactionDiffusion2Init> {
public:
    Output<Buffer<float>> output{"output", 3};

    void generate() {
        output(x, y, c) = Halide::random_float();
    }

    void schedule() {
        if (get_target().has_gpu_feature()) {
            Func(output)
                .reorder(c, x, y)
                .bound(c, 0, 3)
                .vectorize(c)
                .gpu_tile(x, y, 4, 4);
            output
                .dim(0).set_stride(3)
                .dim(2).set_bounds(0, 3).set_stride(1);
        }
    }

private:
    Var x, y, c;
};

HALIDE_REGISTER_GENERATOR(ReactionDiffusion2Init, "reaction_diffusion_2_init")

// TODO: convert to new-style once Input<Buffer> added
class ReactionDiffusion2Update : public Halide::Generator<ReactionDiffusion2Update> {
public:
    Input<Buffer<float>> state{"state", 3};
    Input<int> mouse_x{"mouse_x"};
    Input<int> mouse_y{"mouse_y"};
    Input<int> frame{"frame"};
    Output<Buffer<float>> new_state{"new_state", 3};

    void generate() {
        clamped = Halide::BoundaryConditions::repeat_edge(state);

        blur_x(x, y, c) = (clamped(x-3, y, c) +
                           clamped(x-1, y, c) +
                           clamped(x, y, c) +
                           clamped(x+1, y, c) +
                           clamped(x+3, y, c));
        blur_y(x, y, c) = (clamped(x, y-3, c) +
                           clamped(x, y-1, c) +
                           clamped(x, y, c) +
                           clamped(x, y+1, c) +
                           clamped(x, y+3, c));
        blur(x, y, c) = (blur_x(x, y, c) + blur_y(x, y, c))/10;

        Expr R = blur(x, y, 0);
        Expr G = blur(x, y, 1);
        Expr B = blur(x, y, 2);

        // Push the colors outwards with a sigmoid
        Expr s = 0.5f;
        R *= (1 - s) + s * R * (3 - 2 * R);
        G *= (1 - s) + s * G * (3 - 2 * G);
        B *= (1 - s) + s * B * (3 - 2 * B);

        // Reaction
        Expr dR = B * (1 - R - G);
        Expr dG = (1 - B) * (R - G);
        Expr dB = 1 - B + 2 * G * R - R - G;

        Expr bump = (frame % 1024) / 1024.0f;
        bump *= 1 - bump;
        Expr alpha = lerp(0.3f, 0.7f, bump);
        dR = select(dR > 0, dR*alpha, dR);

        Expr t = 0.1f;

        R += t * dR;
        G += t * dG;
        B += t * dB;

        R = clamp(R, 0.0f, 1.0f);
        G = clamp(G, 0.0f, 1.0f);
        B = clamp(B, 0.0f, 1.0f);

        new_state(x, y, c) = select(c == 0, R, select(c == 1, G, B));

        // Noise at the edges
        new_state(x, state.dim(1).min(), c) = random_float(frame)*0.2f;
        new_state(x, state.dim(1).max(), c) = random_float(frame)*0.2f;
        new_state(state.dim(0).min(), y, c) = random_float(frame)*0.2f;
        new_state(state.dim(0).max(), y, c) = random_float(frame)*0.2f;

        // Add some white where the mouse is
        Expr min_x = clamp(mouse_x - 20, 0, state.dim(0).extent()-1);
        Expr max_x = clamp(mouse_x + 20, 0, state.dim(0).extent()-1);
        Expr min_y = clamp(mouse_y - 20, 0, state.dim(1).extent()-1);
        Expr max_y = clamp(mouse_y + 20, 0, state.dim(1).extent()-1);
        clobber = RDom(min_x, max_x - min_x + 1, min_y, max_y - min_y + 1);

        Expr dx = clobber.x - mouse_x;
        Expr dy = clobber.y - mouse_y;
        Expr radius = dx * dx + dy * dy;
        new_state(clobber.x, clobber.y, c) = select(radius < 400.0f, 1.0f, new_state(clobber.x, clobber.y, c));
    }

    void schedule() {
        state.dim(2).set_bounds(0, 3);
        Func(new_state)
            .reorder(c, x, y)
            .bound(c, 0, 3)
            .unroll(c);

        if (get_target().has_gpu_feature()) {
            blur
                .reorder(c, x, y)
                .vectorize(c)
                .compute_at(new_state, Var::gpu_threads());

            Func(new_state).gpu_tile(x, y, 8, 2);

            for (int i = 0; i <= 1; ++i) {
                Func(new_state).update(i)
                    .reorder(c, x)
                    .unroll(c)
                    .gpu_tile(x, 8);
            }
            for (int i = 2; i <= 3; ++i) {
                Func(new_state).update(i)
                    .reorder(c, y)
                    .unroll(c)
                    .gpu_tile(y, 8);
            }
            Func(new_state).update(4)
                .reorder(c, clobber.x)
                .unroll(c)
                .gpu_tile(clobber.x, clobber.y, 1, 1);

            state
                .dim(0).set_stride(3)
                .dim(2).set_stride(1).set_extent(3);
            new_state
                .dim(0).set_stride(3)
                .dim(2).set_stride(1).set_extent(3);
        } else {
            Var yi;
            Func(new_state)
                .split(y, y, yi, 64)
                .parallel(y)
                .vectorize(x, 4);

            blur
                .compute_at(new_state, yi)
                .vectorize(x, 4);

            clamped
                .store_at(new_state, y)
                .compute_at(new_state, yi);
        }
    }

private:
    Func blur_x, blur_y, blur, clamped;
    Var x, y, c;
    RDom clobber;
};

HALIDE_REGISTER_GENERATOR(ReactionDiffusion2Update, "reaction_diffusion_2_update")

// TODO: convert to new-style once Input<Buffer> added
class ReactionDiffusion2Render : public Halide::Generator<ReactionDiffusion2Render> {
public:
    Input<Buffer<float>> state{"state", 3};
    Output<Buffer<int32_t>> render{"render", 2};

    void generate() {
        Func contour;
        contour(x, y, c) = pow(state(x, y, c) * (1 - state(x, y, c)) * 4, 8);

        Expr c0 = contour(x, y, 0), c1 = contour(x, y, 1), c2 = contour(x, y, 2);

        Expr R = min(c0, max(c1, c2));
        Expr G = (c0 + c1 + c2)/3;
        Expr B = max(c0, max(c1, c2));

        // TODO: ugly hack to rearrange output
        const int32_t kRFactor = get_target().has_gpu_feature() ? (1 << 16) : (1 << 0);
        const int32_t kGFactor = get_target().has_gpu_feature() ? (1 << 8) : (1 << 8);
        const int32_t kBFactor = get_target().has_gpu_feature() ? (1 << 0) : (1 << 16);

        Expr alpha = cast<int32_t>(255 << 24);
        Expr red = cast<int32_t>(R * 255) * kRFactor;
        Expr green = cast<int32_t>(G * 255) * kGFactor;
        Expr blue = cast<int32_t>(B * 255) * kBFactor;

        render(x, y) = cast<int32_t>(alpha + red + green + blue);
    }

    void schedule() {
        if (get_target().has_gpu_feature()) {
            state
                .dim(0).set_stride(3)
                .dim(2).set_stride(1).set_bounds(0, 3);
            Func(render)
                .gpu_tile(x, y, 32, 4);
        } else {
            Var yi;
            Func(render)
                .vectorize(x, 4)
                .split(y, y, yi, 64)
                .parallel(y);
        }
    }

private:
    Var x, y, c;
};

HALIDE_REGISTER_GENERATOR(ReactionDiffusion2Render, "reaction_diffusion_2_render")

}  // namespace
