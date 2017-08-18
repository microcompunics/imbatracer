#ifndef IMBA_RAY_GEN_H
#define IMBA_RAY_GEN_H

#include "imbatracer/render/scheduling/ray_queue.h"
#include "imbatracer/render/random.h"

#include <cfloat>
#include <random>
#include <functional>

namespace imba {

template <typename StateType>
class RayGen {
public:
    virtual ~RayGen() {}

    typedef std::function<bool (int, int, ::Ray&, StateType&)> SampleFn;
    virtual void fill_queue(RayQueue<StateType>&, SampleFn) = 0;
    virtual void start_frame() = 0;
    virtual bool is_empty() const = 0;
};

/// Generates n primary rays per pixel in range [0,0] to [w,h]
template <typename StateType>
class PixelRayGen : public RayGen<StateType> {
public:
    PixelRayGen(int w, int h, int spp)
        : width_(w), height_(h), n_samples_(spp), next_pixel_(0)
    {}

    void start_frame() override { next_pixel_ = 0; }

    bool is_empty() const override { return next_pixel_ >= max_rays(); }

    void fill_queue(RayQueue<StateType>& out, typename RayGen<StateType>::SampleFn sample_pixel) override {
        // only generate at most n samples per pixel
        if (next_pixel_ >= max_rays()) return;

        // calculate how many rays are needed to fill the queue
        int count = out.capacity() - out.size();
        if (count <= 0) return;

        // make sure that no pixel is sampled more than n_samples_ times
        if (next_pixel_ + count > max_rays()) {
            count = max_rays() - next_pixel_;
        }

        for (int i = next_pixel_; i < next_pixel_ + count; ++i) {
            // Compute coordinates, id etc.
            int pixel_idx = i / n_samples_;
            int sample_idx = i % n_samples_;
            int y = pixel_idx / width_;
            int x = pixel_idx % width_;

            // Create the ray and its state.
            StateType state;
            ::Ray ray;

            state.pixel_id = pixel_idx;
            state.sample_id = sample_idx;

            if (!sample_pixel(x, y, ray, state)) continue;

            out.push(ray, state);
        }

        // store which pixel has to be sampled next
        next_pixel_ += count;
    }

protected:
    int next_pixel_;
    const int width_;
    const int height_;
    const int n_samples_;

    int max_rays() const { return width_ * height_ * n_samples_; }
};

/// Generates primary rays for the pixels within a tile. Simply adds an offset to the pixel coordinates from the
/// PixelRayGen, according to the position of the tile.
template<typename StateType>
class TiledRayGen : public PixelRayGen<StateType> {
public:
    TiledRayGen(int left, int top, int w, int h, int spp, int full_width, int full_height)
        : PixelRayGen<StateType>(w, h, spp), top_(top), left_(left)
        , full_height_(full_height), full_width_(full_width)
    {}

    void fill_queue(RayQueue<StateType>& out, typename RayGen<StateType>::SampleFn sample_pixel) override {
        PixelRayGen<StateType>::fill_queue(out,
            [sample_pixel, this](int x, int y, ::Ray& r, StateType& s) -> bool {
                s.pixel_id = (y + top_) * full_width_ + (x + left_);
                return sample_pixel(x + left_, y + top_, r, s);
            });
    }

private:
    const int top_, left_;
    const int full_width_, full_height_;
};

/// Generates rays starting from the light sources in the scene.
template<typename StateType>
class LightRayGen : public RayGen<StateType> {
public:
    LightRayGen(int light, int ray_count)
        : light_(light), ray_count_(ray_count)
    {}

    virtual void fill_queue(RayQueue<StateType>& out, typename RayGen<StateType>::SampleFn sample_light) override {
        // calculate how many rays are needed to fill the queue
        int count = out.capacity() - out.size();
        count = std::min(count, ray_count_ - generated_);
        if (count <= 0) return;

        for (int i = generated_; i < generated_ + count; ++i) {
            // Create the ray and its state.
            StateType state;
            ::Ray ray;

            state.ray_id = i;
            state.light_id = light_;

            if (!sample_light(i, light_, ray, state)) continue;

            out.push(ray, state);
        }

        generated_ += count;
    }

    virtual void start_frame() override {
        generated_ = 0;
    }

    virtual bool is_empty() const override {
        return generated_ >= ray_count_;
    }

private:
    int light_;
    int ray_count_;
    int generated_;
};

/// Generates rays for every element in an array.
template<typename StateType>
class ArrayRayGen : public RayGen<StateType> {
public:
    ArrayRayGen(int offset, int len, int samples)
        : offset_(offset), len_(len * samples), generated_(0), samples_(samples)
    {}

    void fill_queue(RayQueue<StateType>& out, typename RayGen<StateType>::SampleFn sample) override {
        // calculate how many rays are needed to fill the queue
        // TODO this is shared by all ray gens -> factor this out
        int count = out.capacity() - out.size();
        count = std::min(count, len_ - generated_);
        if (count <= 0) return;

        for (int i = generated_; i < generated_ + count; ++i) {
            // Create the ray and its state.
            StateType state;
            ::Ray ray;

            state.ray_id   = i / samples_ + offset_;
            state.light_id = i % samples_ + offset_;

            if (!sample(state.ray_id, 0, ray, state)) continue;

            out.push(ray, state);
        }

        generated_ += count;
    }

    virtual void start_frame() override {
        generated_ = 0;
    }

    virtual bool is_empty() const override {
        return generated_ >= len_;
    }

private:
    const int offset_;
    const int len_;
    int generated_;
    int samples_;
};

template <typename T>
constexpr size_t max_ray_gen_size() {
    return std::max(sizeof(PixelRayGen<T>), std::max(sizeof(TiledRayGen<T>), std::max(sizeof(LightRayGen<T>), sizeof(ArrayRayGen<T>))));
}

} // namespace imba

#endif // IMBA_RAY_GEN_H