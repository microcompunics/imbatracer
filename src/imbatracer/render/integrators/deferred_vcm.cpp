#include "imbatracer/render/integrators/deferred_vcm.h"

#define TBB_USE_EXCEPTIONS 0
#include <tbb/enumerable_thread_specific.h>
#include <future>

namespace imba {

// #define STATISTICS
#ifdef STATISTICS
#define PROFILE(cmd, name)  {auto time_start = std::chrono::high_resolution_clock::now(); \
                            cmd; \
                            auto time_end = std::chrono::high_resolution_clock::now(); \
                            auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(time_end - time_start).count(); \
                            std::cout << name << "\t-\t" << delta << "ms" << std::endl;}
#else
#define PROFILE(cmd, name) {cmd;}
#endif

/// Computes the cosine correction factor for particle tracing with shading normals.
inline float adjoint_correction(const Intersection& isect, const float3& in, const float3& out) {
    // TODO this will not work if OSL shaders are allowed to alter shading normals! -> has to be moved inside the Bsdf objects!
    float n = dot(isect.normal, out) * dot(isect.geom_normal, in);
    float d = dot(isect.normal, in) * dot(isect.geom_normal, out);
    return d == 0.0f ? 0.0f : n / d;
}

template <>
void DeferredVCM<mis::MisVCM>::render(AtomicImage& img) {
    const float radius_alpha = 0.75f;
    cur_iteration_++;
    pm_radius_ = base_radius_ / powf(static_cast<float>(cur_iteration_), 0.5f * (1.0f - radius_alpha));
    pm_radius_ = std::max(pm_radius_, 1e-7f); // ensure numerical stability
    merge_pdf_ = mis::merge_accept_weight(settings_.light_path_count, pm_radius_);

    cam_verts_->clear();
    light_verts_->clear();

    PROFILE(trace_camera_paths(), "Tracing camera paths");
    PROFILE(trace_light_paths(), "Tracing light paths");

    // TODO: sort the camera an light vertices (spatially + by material / tri-id)

    auto m = std::async(std::launch::async, [this, &img] {
        PROFILE(photon_grid_.build(light_verts_->begin(), light_verts_->end(), pm_radius_), "Building hash grid");
        PROFILE(merge(img), "Merge");
    });

    auto pt = std::async(std::launch::async, [this, &img] {
        PROFILE(path_tracing(img, true), "PT");
    });

    auto lt = std::async(std::launch::async, [this, &img] {
        PROFILE(light_tracing(img), "LT");
    });

    auto conn = std::async(std::launch::async, [this, &img] {
        PROFILE(connect(img), "Connect");
    });

    pt.wait();
    lt.wait();
    conn.wait();
    m.wait();
}

template <>
void DeferredVCM<mis::MisBPT>::render(AtomicImage& img) {
    cam_verts_->clear();
    light_verts_->clear();

    PROFILE(trace_camera_paths(), "Tracing camera paths");
    PROFILE(trace_light_paths(), "Tracing light paths");

    PROFILE(path_tracing(img, true), "PT");
    PROFILE(light_tracing(img), "LT");
    PROFILE(connect(img), "Connect");
}

template <>
void DeferredVCM<mis::MisPT>::render(AtomicImage& img) {
    cam_verts_->clear();
    light_verts_->clear();

    PROFILE(trace_camera_paths(), "Tracing camera paths");
    // PROFILE(trace_light_paths(), "Tracing light paths");

    PROFILE(path_tracing(img, true), "PT");
}

template <>
void DeferredVCM<mis::MisLT>::render(AtomicImage& img) {
    cam_verts_->clear();
    light_verts_->clear();

    // PROFILE(trace_camera_paths(), "Tracing camera paths");
    PROFILE(trace_light_paths(), "Tracing light paths");

    PROFILE(light_tracing(img), "LT");
}

template <>
void DeferredVCM<mis::MisTWPT>::render(AtomicImage& img) {
    cam_verts_->clear();
    light_verts_->clear();

    PROFILE(trace_camera_paths(), "Tracing camera paths");
    PROFILE(trace_light_paths(), "Tracing light paths");

    PROFILE(path_tracing(img, true), "PT");
    PROFILE(light_tracing(img), "LT");
}

template <>
void DeferredVCM<mis::MisSPPM>::render(AtomicImage& img) {
    const float radius_alpha = 0.75f;
    cur_iteration_++;
    pm_radius_ = base_radius_ / powf(static_cast<float>(cur_iteration_), 0.5f * (1.0f - radius_alpha));
    pm_radius_ = std::max(pm_radius_, 1e-7f); // ensure numerical stability
    merge_pdf_ = mis::merge_accept_weight(settings_.light_path_count, pm_radius_);

    cam_verts_->clear();
    light_verts_->clear();

    PROFILE(trace_camera_paths(), "Tracing camera paths");
    PROFILE(trace_light_paths(), "Tracing light paths");

    PROFILE(photon_grid_.build(light_verts_->begin(), light_verts_->end(), pm_radius_), "Building hash grid");
    PROFILE(path_tracing(img, true), "PT");
    PROFILE(merge(img), "Merge");
}

template <typename MisType>
void DeferredVCM<MisType>::trace_camera_paths() {
    typename DeferredScheduler<State>::ShadeEmptyFn env_hit;
    if (scene_.env_map()) {
        env_hit = [this] (Ray& r, State& s) {
            process_envmap_hits(r, s);
        };
    }

    scheduler_.run_iteration(&camera_tile_gen_,
        env_hit,
        [this] (Ray& r, Hit& h, State& s) {
            process_hits(r, h, s, cam_verts_.get(), false);
        },
        [this] (int x, int y, Ray& ray, State& state) -> bool {
            // Sample a ray from the camera.
            const float sample_x = static_cast<float>(x) + state.rng.random_float();
            const float sample_y = static_cast<float>(y) + state.rng.random_float();

            ray = cam_.generate_ray(sample_x, sample_y);

            state.throughput = rgb(1.0f);
            state.path_length = 1;

            float pdf = cam_.pdf(ray.dir);

            state.mis.init_camera(settings_.light_path_count, pdf);

            return true;
        });
}

template <typename MisType>
void DeferredVCM<MisType>::trace_light_paths() {
    scheduler_.run_iteration(&light_tile_gen_,
        nullptr,
        [this] (Ray& r, Hit& h, State& s) {
            process_hits(r, h, s, light_verts_.get(), true);
        },
        [this] (int ray_id, int light_id, ::Ray& ray, State& state) -> bool {
            auto& l = scene_.light(light_id);

            // TODO: this pdf depends on the LightTileGen used!
            float pdf_lightpick = 1.0f / scene_.light_count();

            Light::EmitSample sample = l->sample_emit(state.rng);
            ray.org = make_vec4(sample.pos, 1e-4f);
            ray.dir = make_vec4(sample.dir, FLT_MAX);

            state.throughput = sample.radiance / pdf_lightpick;
            state.path_length = 1;

            state.mis.init_light(sample.pdf_emit_w, sample.pdf_direct_a, pdf_lightpick, sample.cos_out, l->is_finite(), l->is_delta());

            light_verts_->add(Vertex(state.mis, state.throughput, -1, light_id, 1, sample.pos));

            return true;
        });
}

template <typename MisType>
void DeferredVCM<MisType>::bounce(State& state, const Intersection& isect, BSDF* bsdf, Ray& ray, bool adjoint, float offset) {
    if (state.path_length >= settings_.max_path_len) {
        terminate_path(state);
        return;
    }

    float rr_pdf;
    if (!russian_roulette(state.throughput, state.rng.random_float(), rr_pdf)) {
        terminate_path(state);
        return;
    }

    float pdf_dir_w;
    float3 sample_dir;
    bool specular;
    auto bsdf_value = bsdf->sample(isect.out_dir, sample_dir, state.rng, pdf_dir_w, specular);

    if (pdf_dir_w == 0.0f || is_black(bsdf_value)) {
        terminate_path(state);
        return;
    }

    float pdf_rev_w = 0.0f;
    if (!specular)
        pdf_rev_w = bsdf->pdf(sample_dir, isect.out_dir);

    const float cos_theta_i = fabsf(dot(sample_dir, isect.normal));

    state.throughput *= bsdf_value / rr_pdf * adjoint_correction(isect, sample_dir, isect.out_dir);
    state.mis.update_bounce(pdf_dir_w, pdf_rev_w, cos_theta_i, specular, merge_pdf_, state.path_length, !adjoint);

    ray = Ray {
        { isect.pos.x, isect.pos.y, isect.pos.z, offset },
        { sample_dir.x, sample_dir.y, sample_dir.z, FLT_MAX }
    };
}

template <typename MisType>
void DeferredVCM<MisType>::process_envmap_hits(Ray& r, State& state) {
    // TODO
    // The environment map was "hit"
    // I should record this hit somehow, to
    //   a) help guiding environment map sampling
    //   b) do the environment map hit evaluation in a deferred way as well, consistent with the rest of the algorithm
}

template <typename MisType>
void DeferredVCM<MisType>::process_hits(Ray& r, Hit& h, State& state, VertCache* cache, bool adjoint) {
    const auto isect = scene_.calculate_intersection(h, r);
    const float cos_theta_o = fabsf(dot(isect.out_dir, isect.normal));

    if (cos_theta_o == 0.0f) { // Prevent NaNs
        terminate_path(state);
        return;
    }

    MaterialValue mat;
    scene_.material_system()->eval_material(isect, adjoint, mat);
    mat.bsdf.prepare(state.throughput, isect.out_dir);

    state.mis.update_hit(cos_theta_o, h.tmax * h.tmax);
    state.path_length++;

    if (!mat.bsdf.is_specular())
        state.ancestor = cache->add(Vertex(state.mis, state.throughput, state.ancestor, state.pixel_id, state.path_length, isect));
    else
        state.ancestor = -1;

    // Continue the path using russian roulette.
    const float offset = h.tmax * 1e-4f;
    bounce(state, isect, &mat.bsdf, r, false, offset);
}

template <typename MisType>
void DeferredVCM<MisType>::path_tracing(AtomicImage& img, bool next_evt) {
    ArrayTileGen<ShadowState> tile_gen(settings_.tile_size * settings_.tile_size, cam_verts_->size());
    shadow_scheduler_pt_.run_iteration(&tile_gen,
        [this, &img] (Ray& r, ShadowState& s) {
            add_contribution(img, s.pixel_id, s.contrib);
        },
        nullptr, // hits --> occluded
        [this, &img, next_evt] (int vert_id, int unused, ::Ray& ray, ShadowState& state) -> bool {
            const auto& v = (*cam_verts_)[vert_id];

            MaterialValue mat;
            scene_.material_system()->eval_material(v.isect, false, mat);
            mat.bsdf.prepare(v.throughput, v.isect.out_dir);

            if (!is_black(mat.emit)) {
                float cos_out = dot(v.isect.normal, v.isect.out_dir);
                if (cos_out < 0.0f) return false;

                float pdf_lightpick = 1.0f / scene_.light_count();
                float pdf_direct_a  = 1.0f / v.isect.area;
                float pdf_emit_w    = 1.0f / v.isect.area * cos_hemisphere_pdf(cos_out);

                const float mis_weight = mis::weight_upt(v.mis, merge_pdf_, pdf_direct_a, pdf_emit_w, pdf_lightpick, v.path_len);

                rgb color = v.throughput * mat.emit * mis_weight;
                add_contribution(img, v.pixel_id, color);

                return false;
            } else if (!next_evt)
                return false;

            // Sample a point on a light
            const auto& ls = scene_.light(state.rng.random_int(0, scene_.light_count()));
            const float pdf_lightpick_inv = scene_.light_count();
            const auto sample = ls->sample_direct(v.isect.pos, state.rng);
            const float cos_theta_i = fabsf(dot(v.isect.normal, sample.dir));

            // Evaluate the BSDF and compute the pdf values
            auto bsdf = &mat.bsdf;
            auto bsdf_value = bsdf->eval(v.isect.out_dir, sample.dir);
            float pdf_dir_w = bsdf->pdf(v.isect.out_dir, sample.dir);
            float pdf_rev_w = bsdf->pdf(sample.dir, v.isect.out_dir);

            if (pdf_dir_w == 0.0f || pdf_rev_w == 0.0f)
               return false;

            const float mis_weight = mis::weight_di(v.mis, merge_pdf_, pdf_dir_w, pdf_rev_w,
                                                    sample.pdf_direct_w, sample.pdf_emit_w, pdf_lightpick_inv,
                                                    cos_theta_i, sample.cos_out, ls->is_delta(), v.path_len);

            const float offset = 1e-3f * (sample.distance == FLT_MAX ? 1.0f : sample.distance);

            ray.org = make_vec4(v.isect.pos, offset);
            ray.dir = make_vec4(sample.dir, sample.distance - offset);

            state.contrib  = v.throughput * bsdf_value * sample.radiance * mis_weight * pdf_lightpick_inv;
            state.pixel_id = v.pixel_id;

            return true;
        });
}

template <typename MisType>
void DeferredVCM<MisType>::light_tracing(AtomicImage& img) {
    ArrayTileGen<ShadowState> tile_gen(settings_.tile_size * settings_.tile_size, light_verts_->size());
    shadow_scheduler_lt_.run_iteration(&tile_gen,
        [this, &img] (Ray& r, ShadowState& s) {
            add_contribution(img, s.pixel_id, s.contrib);
        },
        nullptr, // hits --> occluded
        [this] (int vert_id, int unused, ::Ray& ray, ShadowState& state) -> bool {
            const auto& v = (*light_verts_)[vert_id];

            if (v.path_len == 1)
                return false; // Do not connect vertices on the light source itself

            float3 dir_to_cam = cam_.pos() - v.isect.pos;

            if (dot(-dir_to_cam, cam_.dir()) < 0.0f)
                return false; // Vertex is behind the camera.

            const float2 raster_pos = cam_.world_to_raster(v.isect.pos);
            state.pixel_id = cam_.raster_to_id(raster_pos);

            if (state.pixel_id < 0 || state.pixel_id >= settings_.width * settings_.height)
                return false; // The point is outside the image plane.

            // Compute ray direction and distance.
            const float dist_to_cam_sqr = lensqr(dir_to_cam);
            const float dist_to_cam = sqrtf(dist_to_cam_sqr);
            dir_to_cam = dir_to_cam / dist_to_cam;
            const float cos_theta_surf = dot(v.isect.normal, dir_to_cam);

            float pdf_cam = cam_.pdf(-dir_to_cam);
            pdf_cam *= 1.0f / dist_to_cam_sqr;

            // Evaluate the BSDF and compute the pdf values
            MaterialValue mat;
            scene_.material_system()->eval_material(v.isect, true, mat);
            mat.bsdf.prepare(v.throughput, v.isect.out_dir);

            auto bsdf = &mat.bsdf;
            auto bsdf_value = bsdf->eval(v.isect.out_dir, dir_to_cam) * adjoint_correction(v.isect, dir_to_cam, v.isect.out_dir);
            float pdf_rev_w = bsdf->pdf(dir_to_cam, v.isect.out_dir);

            if (pdf_rev_w == 0.0f) return false;

            const float mis_weight = mis::weight_lt(v.mis, merge_pdf_, pdf_cam * cos_theta_surf, pdf_rev_w, settings_.light_path_count, v.path_len);

            const float offset = dist_to_cam * 1e-4f;

            ray.org = make_vec4(v.isect.pos, offset);
            ray.dir = make_vec4(dir_to_cam, dist_to_cam - offset);

            state.contrib = v.throughput * bsdf_value * pdf_cam * mis_weight / settings_.light_path_count;

            return true;
        });
}

template <typename MisType>
void DeferredVCM<MisType>::connect(AtomicImage& img) {
    ArrayTileGen<ShadowState> tile_gen(settings_.tile_size * settings_.tile_size, cam_verts_->size(), settings_.num_connections);
    shadow_scheduler_connect_.run_iteration(&tile_gen,
        [this, &img] (Ray& r, ShadowState& s) {
            add_contribution(img, s.pixel_id, s.contrib);
        },
        nullptr, // hits --> occluded
        [this] (int vert_id, int unused, ::Ray& ray, ShadowState& state) -> bool {
            const auto& cam_v   = *cam_verts_;
            const auto& light_v = *light_verts_;
            const auto& v = cam_v[vert_id];

            // PDF conversion factor from using the vertex cache.
            // Vertex Cache is equivalent to randomly sampling a path with pdf ~ path length and uniformly sampling a vertex on this path.
            const float vc_weight = light_v.size() / (float(settings_.light_path_count) * float(settings_.num_connections));

            int lv_idx = state.rng.random_int(0, light_v.size());
            auto& light_vertex = light_v[lv_idx];
            if (light_vertex.path_len == 1)
                return false;

            MaterialValue lmat;
            scene_.material_system()->eval_material(light_vertex.isect, true, lmat);
            lmat.bsdf.prepare(light_vertex.throughput, light_vertex.isect.out_dir);

            MaterialValue cmat;
            scene_.material_system()->eval_material(v.isect, false, cmat);
            cmat.bsdf.prepare(v.throughput, v.isect.out_dir);

            const auto light_bsdf = &lmat.bsdf;
            const auto cam_bsdf   = &cmat.bsdf;

            // Compute connection direction and distance.
            float3 connect_dir = light_vertex.isect.pos - v.isect.pos;
            const float connect_dist_sq = lensqr(connect_dir);
            const float connect_dist = std::sqrt(connect_dist_sq);
            connect_dir *= 1.0f / connect_dist;

            if (connect_dist < base_radius_) {
                // If two points are too close to each other, they are either occluded or have cosine terms
                // that are close to zero. Numerical inaccuracies might yield an overly bright pixel.
                // The correct result is usually black or close to black so we just ignore those connections.

                // TODO: check if this makes any sense, maybe adapt MIS weights for this case to get rid of bias!
                return false;
            }

            // Evaluate the bsdf at the camera vertex.
            const auto bsdf_value_cam = cam_bsdf->eval(v.isect.out_dir, connect_dir);
            const float pdf_dir_cam_w = cam_bsdf->pdf(v.isect.out_dir, connect_dir);
            const float pdf_rev_cam_w = cam_bsdf->pdf(connect_dir, v.isect.out_dir);

            // Evaluate the bsdf at the light vertex.
            const auto bsdf_value_light = light_bsdf->eval(light_vertex.isect.out_dir, -connect_dir)
                                        * adjoint_correction(light_vertex.isect, -connect_dir, light_vertex.isect.out_dir);
            const float pdf_dir_light_w = light_bsdf->pdf(light_vertex.isect.out_dir, -connect_dir);
            const float pdf_rev_light_w = light_bsdf->pdf(-connect_dir, light_vertex.isect.out_dir);

            if (pdf_dir_cam_w == 0.0f || pdf_dir_light_w == 0.0f ||
                pdf_rev_cam_w == 0.0f || pdf_rev_light_w == 0.0f)
                return false;  // A pdf value of zero means that there has to be zero contribution from this pair of directions as well.

            // Compute the cosine terms. We need to use the adjoint for the light vertex BSDF.
            const float cos_theta_cam   = fabsf(dot(v.isect.normal, connect_dir));
            const float cos_theta_light = fabsf(dot(light_vertex.isect.normal, -connect_dir));

            const float geom_term = 1.0f / connect_dist_sq; // Cosine contained in bsdf

            const float mis_weight = mis::weight_connect(v.mis, light_vertex.mis, merge_pdf_, pdf_dir_cam_w, pdf_rev_cam_w,
                                                         pdf_dir_light_w, pdf_rev_light_w,
                                                         cos_theta_cam, cos_theta_light, connect_dist_sq,
                                                         v.path_len, light_vertex.path_len);

            state.pixel_id = v.pixel_id;
            state.contrib  = v.throughput * vc_weight * mis_weight * geom_term * bsdf_value_cam * bsdf_value_light * light_vertex.throughput;

            const float offset = 1e-4f * connect_dist;
            ray.org = make_vec4(v.isect.pos, offset);
            ray.dir = make_vec4(connect_dir, connect_dist - offset);

            return true;
        });
}

template <typename MisType>
void DeferredVCM<MisType>::merge(AtomicImage& img) {
    const auto& cam_v = *cam_verts_;

    tbb::parallel_for(tbb::blocked_range<int>(0, cam_verts_->size()),
        [&] (const tbb::blocked_range<int>& range)
    {
        for (auto i = range.begin(); i != range.end(); ++i) {
            const auto& v = cam_v[i];

            MaterialValue mat;
            scene_.material_system()->eval_material(v.isect, false, mat);
            mat.bsdf.prepare(v.throughput, v.isect.out_dir);
            const auto bsdf = &mat.bsdf;

            const int k = settings_.num_knn;
            auto photons = V_ARRAY(VertexHandle, k);
            int count = photon_grid_.query(v.isect.pos, photons, k);
            const float radius_sqr = (count == k) ? lensqr(photons[k - 1].vert->isect.pos - v.isect.pos) : (pm_radius_ * pm_radius_);

            rgb contrib(0.0f);
            for (int i = 0; i < count; ++i) {
                auto p = photons[i].vert;
                if (p->path_len <= 1) continue;

                const auto& photon_in_dir = p->isect.out_dir;

                const auto& bsdf_value = bsdf->eval(v.isect.out_dir, photon_in_dir);
                const float pdf_dir_w = bsdf->pdf(v.isect.out_dir, photon_in_dir);
                const float pdf_rev_w = bsdf->pdf(photon_in_dir, v.isect.out_dir);

                if (pdf_dir_w == 0.0f || pdf_rev_w == 0.0f || is_black(bsdf_value))
                    continue;

                const float mis_weight = mis::weight_merge(v.mis, p->mis, merge_pdf_, pdf_dir_w, pdf_rev_w, v.path_len, p->path_len);

                // Epanechnikov filter
                const float d = lensqr(p->isect.pos - v.isect.pos);
                const float kernel = 1.0f - d / radius_sqr;

                contrib += mis_weight * bsdf_value / fabsf(dot(photon_in_dir, v.isect.normal)) * kernel * p->throughput;
            }

            // Complete the Epanechnikov kernel
            contrib *= 2.0f / (pi * radius_sqr * settings_.light_path_count);

            add_contribution(img, v.pixel_id, v.throughput * contrib);
        }
    });
}

template class DeferredVCM<mis::MisVCM>;
template class DeferredVCM<mis::MisBPT>;
template class DeferredVCM<mis::MisPT>;
template class DeferredVCM<mis::MisLT>;
template class DeferredVCM<mis::MisTWPT>;
template class DeferredVCM<mis::MisSPPM>;

} // namespace imba
