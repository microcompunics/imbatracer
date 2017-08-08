#include "imbatracer/render/integrators/deferred_vertices.h"
#include "imbatracer/render/ray_gen/tile_gen.h"
#include "imbatracer/render/ray_gen/ray_gen.h"
#include "imbatracer/render/integrators/integrator.h"
#include "imbatracer/render/scheduling/deferred_scheduler.h"

#define TBB_USE_EXCEPTIONS 0
#include <tbb/enumerable_thread_specific.h>

namespace imba {

void bounce(const Scene& scene, Ray& r, Hit& h, ProbeState& s, std::atomic<int>& vertex_count) {
    const auto isect = scene.calculate_intersection(h, r);

    MaterialValue mat;
    scene.material_system()->eval_material(isect, false, mat);
    mat.bsdf.prepare(s.throughput, isect.out_dir);

    ++vertex_count;

    float rr_pdf;
    if (!russian_roulette(s.throughput, s.rng.random_float(), rr_pdf))
        return;

    float pdf_dir_w;
    float3 sample_dir;
    bool specular;
    auto bsdf_value = mat.bsdf.sample(isect.out_dir, sample_dir, s.rng, pdf_dir_w, specular);
    if (pdf_dir_w == 0.0f || is_black(bsdf_value)) return;

    s.throughput *= bsdf_value / rr_pdf;

    const float offset = h.tmax * 1e-3f;

    Ray ray {
        { isect.pos.x, isect.pos.y, isect.pos.z, offset },
        { sample_dir.x, sample_dir.y, sample_dir.z, FLT_MAX }
    };

    r = ray;
}

int estimate_light_path_len(const Scene& scene, bool use_gpu, int probes) {
    UniformLightTileGen<ProbeState> tile_gen(scene.light_count(), probes, 512 * 512);
    DeferredScheduler<ProbeState> scheduler(&scene, 256 * 256, use_gpu);

    std::atomic<int> vertex_count(0);
    scheduler.run_iteration(&tile_gen,
        nullptr,
        [&vertex_count, &scene] (Ray& r, Hit& h, ProbeState& s) {
            bounce(scene, r, h, s, vertex_count);
        },
        [&vertex_count, &scene] (int ray_id, int light_id, ::Ray& ray, ProbeState& state) -> bool {
            state.rng = RNG(bernstein_seed(light_id, ray_id));
            auto& l = scene.light(light_id);
            // TODO: this pdf depends on the LightTileGen used!
            float pdf_lightpick = 1.0f / scene.light_count();

            Light::EmitSample sample = l->sample_emit(state.rng);
            ray.org.x = sample.isect.pos.x;
            ray.org.y = sample.isect.pos.y;
            ray.org.z = sample.isect.pos.z;
            ray.org.w = 1e-4f;

            ray.dir.x = sample.dir.x;
            ray.dir.y = sample.dir.y;
            ray.dir.z = sample.dir.z;
            ray.dir.w = FLT_MAX;

            state.throughput = sample.radiance / pdf_lightpick;

            vertex_count++;

            return true;
        });

    const float avg_len = static_cast<float>(vertex_count) / static_cast<float>(probes);
    return std::ceil(avg_len);
}

int estimate_cam_path_len(const Scene& scene, const PerspectiveCamera& cam, bool use_gpu, int probes) {
    DefaultTileGen<ProbeState> tile_gen(cam.width(), cam.height(), probes, 256);
    DeferredScheduler<ProbeState> scheduler(&scene, 256 * 256, use_gpu);

    std::atomic<int> vertex_count(0);
    scheduler.run_iteration(&tile_gen,
        nullptr,
        [&vertex_count, &scene] (Ray& r, Hit& h, ProbeState& s) {
            bounce(scene, r, h, s, vertex_count);
        },
        [&vertex_count, &cam] (int x, int y, ::Ray& ray, ProbeState& state) -> bool {
            state.rng = RNG(bernstein_seed(0, x  * cam.height() + y));
            const float sample_x = static_cast<float>(x) + state.rng.random_float();
            const float sample_y = static_cast<float>(y) + state.rng.random_float();

            ray = cam.generate_ray(sample_x, sample_y);

            state.throughput = rgb(1.0f);

            return true;
        });

    const float avg_len = static_cast<float>(vertex_count) / static_cast<float>(cam.width() * cam.height() * probes);
    return std::ceil(avg_len);
}

} // namespace imba