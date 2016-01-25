#include "bpt.h"
#include "../../core/float4.h"
#include "../../core/common.h"
#include "../random.h"

#include <cfloat>
#include <cassert>
#include <cmath>

namespace imba {

static const float offset = 0.001f;

void BidirPathTracer::render(Image& img) {
    reset_buffers();

    trace_light_paths();
    trace_camera_paths(img);

    // Merge light and camera images.
    for (int i = 0; i < width_ * height_; ++i)
        img.pixels()[i] += light_image_.pixels()[i];
}

void BidirPathTracer::reset_buffers() {
    for (auto& p : light_paths_) {
        for (auto& s : p) {
            s.reserve(MAX_LIGHT_PATH_LEN);
            s.clear();
        }
    }

    for (int i = 0; i < width_ * height_; ++i)
        light_image_.pixels()[i] = float4(0.0f);
}

void BidirPathTracer::trace_light_paths() {
    light_sampler_.start_frame();

    int in_queue = 0;
    int out_queue = 1;

    while(true) {
        light_sampler_.fill_queue(primary_rays_[in_queue]);

        if (primary_rays_[in_queue].size() <= 0)
            break;

        primary_rays_[in_queue].traverse(scene_);
        process_light_rays(primary_rays_[in_queue], primary_rays_[out_queue], shadow_rays_);
        primary_rays_[in_queue].clear();

        // Processing primary rays creates new primary rays and some shadow rays.
        if (shadow_rays_.size() > 0) {
            shadow_rays_.traverse_occluded(scene_);
            process_shadow_rays(shadow_rays_, light_image_);
            shadow_rays_.clear();
        }

        std::swap(in_queue, out_queue);
    }
}

void BidirPathTracer::trace_camera_paths(Image& img) {
    // Create the initial set of camera rays.
    auto camera = camera_sampler_;
    camera.start_frame();

    int in_queue = 0;
    int out_queue = 1;

    while(true) {
        camera.fill_queue(primary_rays_[in_queue]);

        if (primary_rays_[in_queue].size() <= 0)
            break;

        primary_rays_[in_queue].traverse(scene_);
        process_camera_rays(primary_rays_[in_queue], primary_rays_[out_queue], shadow_rays_, img);
        primary_rays_[in_queue].clear();

        // Processing primary rays creates new primary rays and some shadow rays.
        if (shadow_rays_.size() > 0) {
            shadow_rays_.traverse_occluded(scene_);
            process_shadow_rays(shadow_rays_, img);
            shadow_rays_.clear();
        }

        std::swap(in_queue, out_queue);
    }
}

void BidirPathTracer::process_light_rays(RayQueue<BPTState>& rays_in, RayQueue<BPTState>& rays_out, RayQueue<BPTState>& ray_out_shadow) {
    int ray_count = rays_in.size();
    BPTState* states = rays_in.states();
    const Hit* hits = rays_in.hits();
    const Ray* rays = rays_in.rays();

    #pragma omp parallel for
    for (int i = 0; i < ray_count; ++i) {
        if (hits[i].tri_id < 0)
            continue;

        RNG& rng = states[i].rng;

        Intersection isect = calculate_intersection(hits, rays, i);
        float cos_theta_i = fabsf(dot(isect.out_dir, isect.surf.normal));

        // Complete calculation of the partial weights.
        if (states[i].path_length > 1 || states[i].is_finite)
            states[i].dVCM *= sqr(isect.distance);

        states[i].dVCM *= 1.0f / cos_theta_i;
        states[i].dVC  *= 1.0f / cos_theta_i;

        auto& light_path = light_paths_[states[i].pixel_id][states[i].sample_id];
        if (!isect.mat->is_delta) { // Do not store vertices on materials described by a delta distribution.
            light_path.emplace_back(
                isect,
                states[i].throughput,
                states[i].continue_prob,
                states[i].dVC,
                states[i].dVCM
            );

            connect_to_camera(states[i], isect, ray_out_shadow);
        }

        // Decide wether or not to continue this path using russian roulette and a fixed maximum length.
        const float4 srgb(0.2126f, 0.7152f, 0.0722f, 0.0f);
        const float kill_prob = dot(states[i].throughput, srgb) * 10.0f;
        const float rrprob = std::min(1.0f, kill_prob);
        const float u_rr = rng.random_float();
        if (u_rr < rrprob && states[i].path_length < MAX_LIGHT_PATH_LEN){
            float pdf_dir_w;
            float3 sample_dir;
            bool is_specular;
            float4 mat_clr = sample_material(isect.mat, isect.out_dir, isect.surf, rng, sample_dir, true, pdf_dir_w, is_specular);

            if (is_black(mat_clr))
                continue; // do not bother to shoot a ray if the throughput is (less than) zero.

            float pdf_rev_w = pdf_material(isect.mat, isect.surf, isect.out_dir);
            float cos_theta_o = fabsf(dot(sample_dir, isect.surf.normal));

            float3 offset_pos = isect.pos;// + isect.surf.normal * offset;

            BPTState s = states[i];
            if (is_specular) {
                s.dVCM = 0.0f;
                s.dVC *= cos_theta_o;
            } else {
                s.dVC = cos_theta_o / (pdf_dir_w * rrprob) *
                        (s.dVC * pdf_rev_w * rrprob + s.dVCM);

                s.dVCM = 1.0f / (pdf_dir_w * rrprob);
            }

            s.throughput *= mat_clr / rrprob;
            s.path_length++;
            s.continue_prob = rrprob;

            Ray ray {
                { offset_pos.x, offset_pos.y, offset_pos.z, offset },
                { sample_dir.x, sample_dir.y, sample_dir.z, FLT_MAX }
            };

            rays_out.push(ray, s);
        }
    }
}

void BidirPathTracer::connect_to_camera(const BPTState& light_state, const Intersection& isect, RayQueue<BPTState>& ray_out_shadow) {
    float3 dir_to_cam = cam_.pos() - isect.pos;

    if (dot(-dir_to_cam, cam_.dir()) < 0.0f)
        return; // Vertex is behind the camera.

    const float2 raster_pos = cam_.world_to_raster(isect.pos);

    BPTState state = light_state;
    state.pixel_id = cam_.raster_to_id(raster_pos);

    if (state.pixel_id < 0 || state.pixel_id >= width_ * height_)
        return; // The point is outside the image plane.

    // Compute ray direction and distance.
    float dist_to_cam_sqr = lensqr(dir_to_cam);
    float dist_to_cam = sqrt(dist_to_cam_sqr);
    dir_to_cam = dir_to_cam / dist_to_cam;

    const float cos_theta_o = fabsf(dot(isect.surf.normal, dir_to_cam));

    // Evaluate the material.
    float pdf_dir_w, pdf_rev_w;
    const float4 mat_clr = evaluate_material(isect.mat, dir_to_cam, isect.surf, isect.out_dir, true, pdf_dir_w, pdf_rev_w);

    const float pdf_rev = pdf_rev_w * light_state.continue_prob;

    // Compute conversion factor from surface area to image plane and vice versa.
    const float cos_theta_i = fabsf(dot(cam_.dir(), -dir_to_cam));
    const float dist_pixel_to_cam = cam_.image_plane_dist() / cos_theta_i;
    const float img_to_solid_angle = sqr(dist_pixel_to_cam) / cos_theta_i;
    const float img_to_surf = img_to_solid_angle * cos_theta_o / dist_to_cam_sqr;
    const float surf_to_img = 1.0f / img_to_surf;

    // Compute the MIS weight.
    const float pdf_cam = img_to_surf; // Pixel sampling pdf is one as pixel area is one by convention.
    const float mis_weight_light = pdf_cam / light_path_count_ * (light_state.dVCM + light_state.dVC * pdf_rev);
    const float mis_weight = 1.0f / (mis_weight_light + 1.0f);

    // Contribution is divided by the number of samples (light_path_count_) and the factor that converts the (divided) pdf from surface area to image plane area.
    state.throughput *= mis_weight * mat_clr * img_to_surf / (light_path_count_ * cos_theta_o);

    Ray ray {
        { isect.pos.x, isect.pos.y, isect.pos.z, offset },
        { dir_to_cam.x, dir_to_cam.y, dir_to_cam.z, dist_to_cam - offset }
    };

    ray_out_shadow.push(ray, state);
}

void BidirPathTracer::process_camera_rays(RayQueue<BPTState>& rays_in, RayQueue<BPTState>& rays_out, RayQueue<BPTState>& ray_out_shadow, Image& img) {
    int ray_count = rays_in.size();
    BPTState* states = rays_in.states();
    const Hit* hits = rays_in.hits();
    const Ray* rays = rays_in.rays();

    #pragma omp parallel for
    for (int i = 0; i < ray_count; ++i) {
        if (hits[i].tri_id < 0)
            continue;

        RNG& rng = states[i].rng;

        Intersection isect = calculate_intersection(hits, rays, i);
        float cos_theta_o = fabsf(dot(isect.out_dir, isect.surf.normal));

        // Complete computation of partial MIS weights.
        states[i].dVCM *= sqr(isect.distance) / cos_theta_o; // convert divided pdf from solid angle to area
        states[i].dVC *= 1.0f / cos_theta_o;

        if (isect.mat->kind == Material::emissive) {
            Light* light_source = static_cast<EmissiveMaterial*>(isect.mat)->light();

            // A light source was hit directly. Add the weighted contribution.
            float pdf_lightpick = 1.0f / scene_.lights.size();
            float pdf_direct_a, pdf_emit_w;
            float4 radiance = light_source->radiance(isect.out_dir, pdf_direct_a, pdf_emit_w);

            const float pdf_di = pdf_direct_a * pdf_lightpick;
            const float pdf_e = pdf_emit_w * pdf_lightpick;

            if (!is_black(radiance)) {
                const float mis_weight_camera = pdf_di * states[i].dVCM + pdf_e * states[i].dVC;

                if (states[i].path_length > 1) {
                    float4 color = states[i].throughput * radiance * 1.0f / (mis_weight_camera + 1.0f);
                    img.pixels()[states[i].pixel_id] += color;
                } else
                    img.pixels()[states[i].pixel_id] += radiance; // Light directly visible, no weighting required.
            }
        }

        // Compute direct illumination.
        direct_illum(states[i], isect, ray_out_shadow);

        // Connect to light path vertices.
        connect(states[i], isect, ray_out_shadow);

        // Continue the path using russian roulette.
        const float4 srgb(0.2126f, 0.7152f, 0.0722f, 0.0f);
        const float kill_prob = dot(states[i].throughput, srgb) * 10.0f;
        const float rrprob = std::min(1.0f, kill_prob);
        const float u_rr = rng.random_float();
        const int max_recursion = 32; // prevent havoc
        if (u_rr < rrprob && states[i].path_length < max_recursion) {
            float pdf_dir_w;
            float3 sample_dir;
            bool is_specular;
            float4 bsdf = sample_material(isect.mat, isect.out_dir, isect.surf, rng, sample_dir, false, pdf_dir_w, is_specular);

            if (is_black(bsdf))
                continue; // necessary to prevent nans if no direction was sampled

            float pdf_rev_w = pdf_dir_w;
            if (!is_specular) // cannot evaluate reverse pdf of specular surfaces (but is the same as forward due to symmetry)
                pdf_rev_w = pdf_material(isect.mat, isect.surf, isect.out_dir);
            float cos_theta_o = fabsf(dot(sample_dir, isect.surf.normal));

            BPTState s = states[i];
            if (is_specular) {
                s.dVCM = 0.0f;
                s.dVC *= cos_theta_o;
            } else {
                s.dVC = cos_theta_o / (pdf_dir_w * rrprob) *
                        (s.dVC * pdf_rev_w * rrprob + s.dVCM);

                s.dVCM = 1.0f / (pdf_dir_w * rrprob);
            }

            s.throughput *= bsdf / (rrprob);
            s.path_length++;
            s.continue_prob = rrprob;

            Ray ray {
                { isect.pos.x, isect.pos.y, isect.pos.z, offset },
                { sample_dir.x, sample_dir.y, sample_dir.z, FLT_MAX }
            };

            rays_out.push(ray, s);
        }
    }
}

void BidirPathTracer::direct_illum(BPTState& cam_state, const Intersection& isect, RayQueue<BPTState>& rays_out_shadow) {
    RNG& rng = cam_state.rng;

    // Generate the shadow ray (sample one point on one lightsource)
    const int light_i = rng.random_int(0, scene_.lights.size());
    const auto ls = scene_.lights[light_i].get();
    const float inv_pdf_lightpick = scene_.lights.size();
    const auto sample = ls->sample_direct(isect.pos, rng);
    const float cos_theta_o = sample.cos_out;
    assert_normalized(sample.dir);

    // Ensure that the incoming and outgoing directions are on the same side of the surface.
    if (dot(isect.surf.geom_normal, sample.dir) *
        dot(isect.surf.geom_normal, isect.out_dir) <= 0.0f)
        return;

    Ray ray {
        { isect.pos.x, isect.pos.y, isect.pos.z, offset },
        { sample.dir.x, sample.dir.y, sample.dir.z, sample.distance - offset }
    };

    // Evaluate the bsdf.
    const float cos_theta_i = fabsf(dot(isect.surf.normal, sample.dir));
    float pdf_dir_w, pdf_rev_w;
    const float4 bsdf = evaluate_material(isect.mat, isect.out_dir, isect.surf, sample.dir, false, pdf_dir_w, pdf_rev_w);

    const float pdf_forward = cam_state.continue_prob * pdf_dir_w;
    const float pdf_reverse = cam_state.continue_prob * pdf_rev_w;

    // Compute full MIS weights for camera and light.
    const float mis_weight_light = pdf_forward * inv_pdf_lightpick / sample.pdf_direct_w;
    const float mis_weight_camera = sample.pdf_emit_w * cos_theta_i / (sample.pdf_direct_w * cos_theta_o) *
                                    (cam_state.dVCM + cam_state.dVC * pdf_reverse);

    const float mis_weight = 1.0f / (mis_weight_camera + 1.0f + mis_weight_light);

    BPTState s = cam_state;
    s.throughput *= mis_weight * bsdf * sample.radiance * inv_pdf_lightpick;

    rays_out_shadow.push(ray, s);
}

void BidirPathTracer::connect(BPTState& cam_state, const Intersection& isect, RayQueue<BPTState>& rays_out_shadow) {
    auto& light_path = light_paths_[cam_state.pixel_id][cam_state.sample_id];
    for (auto& light_vertex : light_path) {
        // Compute connection direction and distance.
        float3 connect_dir = light_vertex.isect.pos - isect.pos;
        const float connect_dist_sq = lensqr(connect_dir);
        const float connect_dist = std::sqrt(connect_dist_sq);
        connect_dir *= 1.0f / connect_dist;

        // Evaluate the bsdf at the camera vertex.
        const float cos_theta_cam = dot(isect.surf.normal, connect_dir);
        float pdf_dir_cam_w, pdf_rev_cam_w;
        const float4 bsdf_cam = evaluate_material(isect.mat, isect.out_dir, isect.surf, connect_dir, false, pdf_dir_cam_w, pdf_rev_cam_w);

        // Evaluate the bsdf at the light vertex.
        const float cos_theta_light = dot(light_vertex.isect.surf.normal, -connect_dir);
        float pdf_dir_light_w, pdf_rev_light_w;
        const float4 bsdf_light = evaluate_material(light_vertex.isect.mat, -connect_dir, light_vertex.isect.surf, light_vertex.isect.out_dir, true, pdf_dir_light_w, pdf_rev_light_w);

        if (cos_theta_cam <= 0.0f || cos_theta_light <= 0.0f)
            continue;

        // Compute and convert the pdfs
        const float pdf_cam_f = pdf_dir_cam_w * cam_state.continue_prob;
        const float pdf_cam_r = pdf_rev_cam_w * cam_state.continue_prob;

        const float pdf_light_f = pdf_dir_light_w * light_vertex.continue_prob;
        const float pdf_light_r = pdf_rev_light_w * light_vertex.continue_prob;

        const float pdf_cam_a = pdf_cam_f * cos_theta_light / connect_dist_sq;
        const float pdf_light_a = pdf_light_f * cos_theta_cam / connect_dist_sq;

        const float geom_term = 1.0f / connect_dist_sq; // Cosines are already included in the value returned from evaluate_material.

        // Compute the full MIS weight from the partial weights and pdfs.
        const float mis_weight_light = pdf_cam_a * (light_vertex.dVCM + light_vertex.dVC * pdf_light_r);
        const float mis_weight_camera = pdf_light_a * (cam_state.dVCM + cam_state.dVC * pdf_cam_r);

        const float mis_weight = 1.0f / (mis_weight_camera + 1.0f + mis_weight_light);

        BPTState s = cam_state;
        s.throughput *= mis_weight * geom_term * bsdf_cam * bsdf_light * light_vertex.throughput;

        Ray ray {
            { isect.pos.x, isect.pos.y, isect.pos.z, offset },
            { connect_dir.x, connect_dir.y, connect_dir.z, connect_dist - offset }
        };

        rays_out_shadow.push(ray, s);
    }
}

void BidirPathTracer::process_shadow_rays(RayQueue<BPTState>& rays_in, Image& img) {
    int ray_count = rays_in.size();
    const BPTState* states = rays_in.states();
    const Hit* hits = rays_in.hits();
    const Ray* rays = rays_in.rays();

    for (int i = 0; i < ray_count; ++i) {
        if (hits[i].tri_id < 0) {
            assert(states[i].pixel_id >= 0 && states[i].pixel_id < img.width() * img.height() && "Write outside of image detected. (BPT::process_shadow_rays)");
            img.pixels()[states[i].pixel_id] += states[i].throughput;
        }
    }
}

} // namespace imba
