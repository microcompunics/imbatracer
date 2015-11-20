#ifndef IMBA_RAY_QUEUE_H
#define IMBA_RAY_QUEUE_H

#include "traversal.h"
#include "thorin_mem.h"

#include <vector>
#include <algorithm>
#include <cstring>
#include <cassert>
#include <atomic>

namespace imba {

/// Base class for storing the current state associated with a ray.
struct RayState {
    int pixel_id;
    int sample_id;
};

/// Stores the representation of the acceleration structure in the format required by the traversal library.
struct TraversalData {
    ThorinArray<::Node> nodes;
	ThorinArray<::Vec4> tris;  
};

/// Stores a set of rays for traversal along with their state.
template <typename StateType>
class RayQueue {
public:
    RayQueue() { }
    
    RayQueue(int capacity, TraversalData& traversal_data) 
        : ray_buffer_(capacity), hit_buffer_(capacity), state_buffer_(capacity), last_(-1), traversal_data_(traversal_data)
    {
    }
    
    RayQueue(const RayQueue<StateType>&) = delete;
    RayQueue& operator= (const RayQueue<StateType>&) = delete;
    
    RayQueue(RayQueue<StateType>&& rhs)
        : traversal_data_(rhs.traversal_data_),
          ray_buffer_(std::move(rhs.ray_buffer_)),
          hit_buffer_(std::move(rhs.hit_buffer_)),
          state_buffer_(std::move(rhs.state_buffer_)),
          last_(rhs.last_.load())
    {
    }
    
    RayQueue& operator= (RayQueue<StateType>&& rhs) {
        traversal_data_ = rhs.traversal_data_;
        ray_buffer_ = std::move(rhs.ray_buffer_);
        hit_buffer_ = std::move(rhs.hit_buffer_);
        state_buffer_ = std::move(rhs.state_buffer_);
        last_ = rhs.last_.load();
        
        return *this;
    }
    
    /*
    void resize(int capacity, ThorinArray<::Node>* nodes, ThorinArray<Vec4>* tris) {
        assert(nodes && tris && "No acceleration structure data passed to the RayQueue");
           
        nodes_ = nodes;
        tris_ = tris;
        ray_buffer_ = ThorinArray<::Ray>(capacity);
        hit_buffer_ = ThorinArray<::Hit>(capacity);
        state_buffer_.resize(capacity);
        last_ = -1;
    }*/

    int size() const { return last_ + 1; }
    
    ::Ray* rays() {
        return ray_buffer_.host_data();
    }
    
    StateType* states() {
        return state_buffer_.data();
    }
    
    ::Hit* hits() {
        return hit_buffer_.host_data();
    }
    
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
        int start_idx = end_idx - (count - 1);\
        
        assert(end_idx < ray_buffer_.size() && "ray queue full");
        
        // Copy ray and state data.
        std::copy(rays_begin, rays_end, ray_buffer_.begin() + start_idx);
        std::copy(states_begin, states_end, state_buffer_.begin() + start_idx);
    }
    
    /// Traverses the acceleration structure with the rays currently inside the queue.
    void traverse() {
        //printf("traverse: %d \n", size());
        assert(size() != 0);

        int count = size();
        if (count % 64 != 0) {
            count = count + 64 - count % 64;        
        }
        ray_buffer_.upload(size());
        TRAVERSAL_ROUTINE(traversal_data_.nodes.device_data(), traversal_data_.tris.device_data(), ray_buffer_.device_data(), hit_buffer_.device_data(), count);
        hit_buffer_.download(size());
    }
    
private:
    TraversalData& traversal_data_;
    
    ThorinArray<::Ray> ray_buffer_;
    ThorinArray<::Hit> hit_buffer_;
    std::vector<StateType> state_buffer_;
    
    std::atomic<int> last_;
};

}

#endif
