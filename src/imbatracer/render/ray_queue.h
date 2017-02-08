#ifndef IMBA_RAY_QUEUE_H
#define IMBA_RAY_QUEUE_H

#include <vector>
#include <algorithm>
#include <cstring>
#include <cassert>
#include <atomic>
#include <mutex>
#include <memory>

#include <tbb/parallel_sort.h>

#include <anydsl_runtime.hpp>
#include <traversal.h>

#include "random.h"

namespace imba {

/// State associated with a ray.
struct RayState {
    int pixel_id;
    int sample_id;

    RNG rng;
};

/// State associated with a shadow ray
struct ShadowState {
    int pixel_id;
    rgb throughput;
};

#ifdef GPU_TRAVERSAL

#define TRAVERSAL_DEVICE    anydsl::Device(0)
#define TRAVERSAL_PLATFORM  anydsl::Platform::Cuda
#define TRAVERSAL_INTERSECT intersect_gpu_masked_instanced
#define TRAVERSAL_OCCLUDED  occluded_gpu_masked_instanced

// Do not allow running multiple traversal instances at the same time on the GPU.
static std::mutex traversal_mutex;
static constexpr int traversal_block_size() { return 64; }

#else

static constexpr int traversal_block_size() { return 8; }

#define TRAVERSAL_DEVICE    anydsl::Device(0)
#define TRAVERSAL_PLATFORM  anydsl::Platform::Host
#define TRAVERSAL_INTERSECT intersect_cpu_masked_instanced
#define TRAVERSAL_OCCLUDED  occluded_cpu_masked_instanced

#endif

/// Structure that contains the traversal data, such as the BVH nodes or opacity masks.
struct TraversalData {
    int root;
    anydsl::Array<::Node> nodes;
    anydsl::Array<::InstanceNode> instances;
    anydsl::Array<::Vec4> tris;
    anydsl::Array<::Vec2> texcoords;
    anydsl::Array<int> indices;
    anydsl::Array<::TransparencyMask> masks;
    anydsl::Array<char> mask_buffer;
};

/// Stores a set of rays for traversal along with their state.
template <typename StateType>
class RayQueue {
    static int align(int v) { return v % traversal_block_size() == 0 ? v : v + traversal_block_size() - v % traversal_block_size(); }

public:
    RayQueue() { }

    RayQueue(int capacity)
        : ray_buffer_     (align(capacity))
        , hit_buffer_     (align(capacity))
#ifdef GPU_TRAVERSAL
        , dev_ray_buffer_ (TRAVERSAL_PLATFORM, TRAVERSAL_DEVICE, align(capacity))
        , dev_hit_buffer_ (TRAVERSAL_PLATFORM, TRAVERSAL_DEVICE, align(capacity))
#endif
        , state_buffer_   (align(capacity))
        , sorted_indices_ (align(capacity))
        , last_(-1)
    {
        memset(ray_buffer_.data(), 0, sizeof(Ray) * align(capacity));
    }

    RayQueue(const RayQueue<StateType>&) = delete;
    RayQueue& operator= (const RayQueue<StateType>&) = delete;

    RayQueue(RayQueue<StateType>&& rhs)
        : ray_buffer_(std::move(rhs.ray_buffer_))
        , hit_buffer_(std::move(rhs.hit_buffer_))
        , state_buffer_(std::move(rhs.state_buffer_))
        , sorted_indices_(std::move(rhs.sorted_indices_))
        , last_(rhs.last_.load())
    {}

    RayQueue& operator= (RayQueue<StateType>&& rhs) {
        ray_buffer_ = std::move(rhs.ray_buffer_);
        hit_buffer_ = std::move(rhs.hit_buffer_);
        state_buffer_ = std::move(rhs.state_buffer_);
        sorted_indices_ = std::move(rhs.sorted_indices_);
        last_ = rhs.last_.load();

        return *this;
    }

    int size() const { return last_ + 1; }
    int capacity() const { return state_buffer_.size(); }

    // Shrinks the queue to the given size.
    void shrink(int size) { last_ = size - 1; }

    ::Ray* rays() { return ray_buffer_.data(); }
    StateType* states() { return state_buffer_.data(); }
    ::Hit* hits() { return hit_buffer_.data(); }

    Ray& ray(int idx) { return ray_buffer_[sorted_indices_[idx]]; }
    Hit& hit(int idx) { return hit_buffer_[sorted_indices_[idx]]; }
    StateType& state(int idx) { return state_buffer_[sorted_indices_[idx]]; }

    void clear() {
        last_ = -1;
    }

    /// Adds a single secondary or shadow ray to the queue. Thread-safe
    void push(const Ray& ray, const StateType& state) {
        int id = ++last_; // atomic inc. of last_

        assert(id < ray_buffer_.size() && "ray queue full");

        ray_buffer_[id] = ray;
        state_buffer_[id] = state;
    }

    /// Adds a set of camera rays to the queue. Thread-safe
    template<typename RayIter, typename StateIter>
    void push(RayIter rays_begin, RayIter rays_end, StateIter states_begin, StateIter states_end) {
        // Calculate the position at which the rays will be inserted.
        int count = rays_end - rays_begin;
        int end_idx = last_ += count; // atomic add to last_
        int start_idx = end_idx - (count - 1);

        assert(end_idx < ray_buffer_.size() && "ray queue full");

        // Copy ray and state data.
        std::copy(rays_begin, rays_end, ray_buffer_.begin() + start_idx);
        std::copy(states_begin, states_end, state_buffer_.begin() + start_idx);
    }

    // Appends the rays and state data from another queue to this queue. Hits are not copied.
    void append(const RayQueue<StateType>& other) {
        int count = other.size();
        int end_idx = last_ += count; // atomic add to last_
        int start_idx = end_idx - (count - 1);

        assert(end_idx < ray_buffer_.size() && "ray queue full");

        // Copy ray and state data.
        std::copy(other.ray_buffer_.begin(), other.ray_buffer_.end(), ray_buffer_.begin() + start_idx);
        std::copy(other.state_buffer_.begin(), other.state_buffer_.end(), state_buffer_.begin() + start_idx);
    }

    /// Compact the queue by moving all rays that hit something (and their associated states and hits) to the front.
    inline int compact_hits() {
        auto hits   = this->hits();
        auto states = this->states();
        auto rays   = this->rays();

        int last_empty = -1;
        for (int i = 0; i < size(); ++i) {
            if (hits[i].tri_id < 0 && last_empty == -1) {
                last_empty = i;
            } else if (hits[i].tri_id >= 0 && last_empty != -1) {
                std::swap(hits[last_empty],   hits[i]);
                std::swap(states[last_empty], states[i]);
                std::swap(rays[last_empty],   rays[i]);
                last_empty++;
            }
        }

        for (int i = 0; i <= last_; ++i)
            sorted_indices_[i] = i;

        if (last_empty != -1)
            return last_empty;
        else
            return size();
    }

    /// Compacts the queue by moving all continued rays to the front. Does not move the hits.
    inline void compact_rays() {
        auto states = this->states();
        auto rays   = this->rays();

        int last_empty = -1;
        for (int i = 0; i < size(); ++i) {
            if (states[i].pixel_id < 0 && last_empty == -1) {
                last_empty = i;
            } else if (states[i].pixel_id >= 0 && last_empty != -1) {
                states[last_empty] = states[i];
                rays[last_empty]   = rays[i];
                last_empty++;
            }
        }

        // If at least one empty ray was replaced, shrink the queue.
        // last_empty corresponds to the new queue size.
        if (last_empty != -1)
            shrink(last_empty);
    }

    typedef std::function<int (const Hit&)> GetMatIDFn;

    inline void sort_by_material(GetMatIDFn get_mat_id, int num_mats, int count) {
        if (matcount_.size() < num_mats)
            matcount_ = std::vector<std::atomic<int> >(num_mats);
        std::fill(matcount_.begin(), matcount_.end(), 0);

        // Count the number of hit points per material.
        tbb::parallel_for(tbb::blocked_range<int>(0, count),
            [&] (const tbb::blocked_range<int>& range)
        {
            for (auto i = range.begin(); i != range.end(); ++i) {
                const int mat_id = get_mat_id(hit_buffer_[i]);
                ray_buffer_[i].dir.w = int_as_float(mat_id);
                matcount_[mat_id]++;
            }
        });

        // Compute the starting index of every bin.
        int accum = 0;
        for (int i = 0; i < num_mats; ++i) {
            const int tmp = matcount_[i];
            matcount_[i] = accum;
            accum += tmp;
        }

        // Distribute the indices according to their material ids.
        tbb::parallel_for(tbb::blocked_range<int>(0, count),
            [&] (const tbb::blocked_range<int>& range)
        {
            for (auto i = range.begin(); i != range.end(); ++i) {
                const int mat = float_as_int(ray_buffer_[i].dir.w);
                sorted_indices_[matcount_[mat]++] = i;
            }
        });
    }

    /// Traverses the acceleration structure with the rays currently inside the queue.
    void traverse(const TraversalData& c_data) {
        assert(size() != 0);

        int count = align(size());
        TraversalData& data = const_cast<TraversalData&>(c_data);

#ifdef GPU_TRAVERSAL
        anydsl::copy(ray_buffer_, dev_ray_buffer_, size());
#else
        auto& dev_ray_buffer_ = ray_buffer_;
        auto& dev_hit_buffer_ = hit_buffer_;
#endif
        TRAVERSAL_INTERSECT(data.root, data.nodes.data(),
                            data.instances.data(),
                            data.tris.data(),
                            dev_ray_buffer_.data(),
                            dev_hit_buffer_.data(),
                            data.indices.data(),
                            data.texcoords.data(),
                            data.masks.data(),
                            data.mask_buffer.data(),
                            count);
#ifdef GPU_TRAVERSAL
        anydsl::copy(dev_hit_buffer_, hit_buffer_, size());
#endif
    }

    void traverse_occluded(const TraversalData& c_data) {
        assert(size() != 0);

        int count = align(size());
        TraversalData& data = const_cast<TraversalData&>(c_data);

#ifdef GPU_TRAVERSAL
        anydsl::copy(ray_buffer_, dev_ray_buffer_, size());
#else
        auto& dev_ray_buffer_ = ray_buffer_;
        auto& dev_hit_buffer_ = hit_buffer_;
#endif
        TRAVERSAL_OCCLUDED(data.root, data.nodes.data(),
                           data.instances.data(),
                           data.tris.data(),
                           dev_ray_buffer_.data(),
                           dev_hit_buffer_.data(),
                           data.indices.data(),
                           data.texcoords.data(),
                           data.masks.data(),
                           data.mask_buffer.data(),
                           count);
#ifdef GPU_TRAVERSAL
        anydsl::copy(dev_hit_buffer_, hit_buffer_, size());
#endif
    }

private:
    anydsl::Array<Ray> ray_buffer_;
    anydsl::Array<Hit> hit_buffer_;
#ifdef GPU_TRAVERSAL
    anydsl::Array<Ray> dev_ray_buffer_;
    anydsl::Array<Hit> dev_hit_buffer_;
#endif

    std::vector<StateType> state_buffer_;
    std::atomic<int> last_;

    // Used for sorting the hit points with counting sort
    std::vector<int> sorted_indices_;
    std::vector<std::atomic<int> > matcount_;
};

} // namespace imba

#endif // IMBA_RAY_QUEUE
