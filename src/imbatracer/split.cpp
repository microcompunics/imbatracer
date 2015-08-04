#include <algorithm>
#include <cassert>
#include "split.h"
#include "mesh.h"

namespace imba {

struct Bin {
    int count;
    float lower, upper;
    BBox bbox;
};

inline void initialize_bins(Bin* bins, int bin_count, float min, float max) {
    const float step = (max - min) / bin_count;
    float lower = min;

    for (int i = 0; i < bin_count; i++) {
        bins[i].bbox = BBox::empty();
        bins[i].count = 0;
        bins[i].lower = lower;
        bins[i].upper = lower + step;
        lower = bins[i].upper;
    }
}

inline float bin_factor(int bin_count, float min, float max) {
    const float bin_offset = 0.0001f;
    return bin_count * (1.0f - bin_offset) / (max - min + bin_offset);
}

static SplitCandidate best_split(const Bin* bins, int bin_count) {
    // Sweep from the left: accumulate SAH cost
    float accum_cost[bin_count];

    BBox cur_bb = bins[0].bbox;
    int cur_count = bins[0].count;
    accum_cost[0] = 0;
    for (int i = 1; i < bin_count; i++) {
        accum_cost[i] = accum_cost[i - 1] + half_area(cur_bb) * cur_count;
        cur_bb = extend(cur_bb, bins[i].bbox);
        cur_count += bins[i].count;
    }

    // Sweep from the right: find best partition
    SplitCandidate candidate;
    candidate.cost = half_area(cur_bb) * cur_count + accum_cost[bin_count - 1];
    candidate.right_bb = cur_bb;

    int best_split = bin_count - 1;
    cur_bb = bins[bin_count - 1].bbox;
    cur_count = bins[bin_count - 1].count;
    for (int i = bin_count - 2; i > 0; i--) {
        cur_bb = extend(cur_bb, bins[i].bbox);
        cur_count += bins[i].count;
        float cost = half_area(cur_bb) * cur_count + accum_cost[i];        

        if (cost < candidate.cost) {
            candidate.right_bb = cur_bb;
            candidate.right_count = cur_count;
            candidate.cost = cost;
            best_split = i;
        }
    }

    candidate.position = bins[best_split].lower;

    // Find the bounding box and primitive count of the left child
    candidate.left_bb = bins[0].bbox;
    candidate.left_count = bins[0].count;
    for (int i = 1; i < best_split; i++) {
        candidate.left_bb = extend(candidate.left_bb, bins[i].bbox);
        candidate.left_count += bins[i].count;
    }

    return candidate;
}

SplitCandidate object_split(int axis, float min, float max,
                            const uint32_t* refs, int ref_count,
                            const float3* centroids, const BBox* bboxes) {
    constexpr int bin_count = 64;
    Bin bins[bin_count];
    initialize_bins(bins, bin_count, min, max);

    // Put the primitives in each bin
    const float factor = bin_factor(bin_count, min, max);
    for (int i = 0; i < ref_count; i++) {
        const float3& center = centroids[refs[i]];
        const int bin_id = factor * (center[axis] - min);
        assert(bin_id < bin_count);

        bins[bin_id].bbox = extend(bins[bin_id].bbox, bboxes[refs[i]]);
        bins[bin_id].count++;
    }

    SplitCandidate candidate = best_split(bins, bin_count);
    candidate.spatial = false;
    candidate.axis = axis;
    return candidate;
}

SplitCandidate spatial_split(int32_t axis, float min, float max,
                             const uint32_t* refs, uint32_t ref_count,
                             const Mesh& mesh, const BBox* bboxes) {
    constexpr int bin_count = 256;
    Bin bins[bin_count];
    initialize_bins(bins, bin_count, min, max);

    // Put the primitives in each bin
    const float factor = bin_factor(bin_count, min, max);
    for (uint32_t i = 0; i < ref_count; i++) {
        const BBox& bbox = bboxes[refs[i]];
        const uint32_t first_bin = factor * (bbox.min[axis] - min);
        const uint32_t last_bin = factor * (bbox.max[axis] - min);
        assert(first_bin < bin_count && last_bin < bin_count);

        for (uint32_t j = first_bin; j <= last_bin; j++) {
            const BBox& bbox = mesh.triangle(refs[i]).clipped_bbox(axis, bins[j].lower, bins[j].upper);
            bins[j].bbox = extend(bins[j].bbox, bbox);
            bins[j].count++;
        }
    }

    SplitCandidate candidate = best_split(bins, bin_count);
    candidate.spatial = true;
    candidate.axis = axis;
    return candidate;
}

void object_partition(const SplitCandidate& candidate,
                      uint32_t* refs, int ref_count,
                      const float3* centroids) {
    assert(!candidate.spatial);
    std::partition(refs, refs + ref_count, [=] (uint32_t ref) {
       return centroids[ref][candidate.axis] < candidate.position;
    }) - refs;
}

void spatial_partition(const SplitCandidate& candidate,
                       const uint32_t* refs, int ref_count,
                       uint32_t* left_refs, uint32_t* right_refs, 
                       const BBox* bboxes) {
    assert(candidate.spatial);
    int left_count = 0, right_count = 0;
    for (int i = 0; i < ref_count; i++) {
        uint32_t ref = refs[i];
        if (bboxes[ref].max[candidate.axis] > candidate.position)
            right_refs[right_count++] = ref;
        if (bboxes[ref].min[candidate.axis] < candidate.position)
            left_refs[left_count++] = ref;
    }
}

} // namespace imba
