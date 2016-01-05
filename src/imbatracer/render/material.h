#ifndef IMBA_MATERIAL_H
#define IMBA_MATERIAL_H

#include "random.h"
#include "texture_sampler.h"
#include "light.h"

#include "../core/float3.h"
#include "../core/common.h"

#include <iostream>

namespace imba {

struct Material {
    enum Kind {
        lambert,
        mirror,
        emissive,
        combine,
        glass
    } kind;

    // whether or not the material is described by a delta distribution
    bool is_delta;

    Material(Kind k, bool is_delta) : kind(k), is_delta(is_delta) {}
    virtual ~Material() {}
};

struct SurfaceInfo {
    float3 normal;
    float2 uv;
    float3 geom_normal;
};

float4 evaluate_material(Material* mat, const float3& out_dir, const SurfaceInfo& surf, const float3& in_dir, float& pdf_dir, float& pdf_rev);

/// Samples a direction for incoming light.
float4 sample_material_in(Material* mat, const float3& out_dir, const SurfaceInfo& surf, RNG& rng, float3& in_dir, float& pdf, bool& specular);

/// Samples a direction for outgoing light.
float4 sample_material_out(Material* mat, const float3& in_dir, const SurfaceInfo& surf, RNG& rng, float3& out_dir, float& pdf, bool& specular);

inline float fresnel_conductor(float cosi, float eta, float kappa)
{
    const float ekc = (eta*eta + kappa*kappa) * cosi*cosi;
    const float par =
        (ekc - (2.f * eta * cosi) + 1) /
        (ekc + (2.f * eta * cosi) + 1);

    const float ek = eta*eta + kappa*kappa;
    const float perp =
        (ek - (2.f * eta * cosi) + cosi*cosi) /
        (ek + (2.f * eta * cosi) + cosi*cosi);

    return (par + perp) / 2.f;
}

inline float fresnel_dielectric(float cosi, float coso, float etai, float etao)
{
    const float par  = (etao * cosi - etai * coso) / (etao * cosi + etai * coso);
    const float perp = (etai * cosi - etao * coso) / (etai * cosi + etao * coso);

    return (par * par + perp * perp) / 2.f;
}

class LambertMaterial : public Material {
public:
    LambertMaterial() : Material(lambert, false), diffuse_(1.0f, 0.0f, 1.0f, 1.0f), sampler_(nullptr) { }
    LambertMaterial(const float4& color) : Material(lambert, false), diffuse_(color), sampler_(nullptr) { }
    LambertMaterial(TextureSampler* sampler) : Material(lambert, false), sampler_(sampler) { }

    inline float4 sample_in(const float3& out_dir, const SurfaceInfo& surf, RNG& rng, float3& in_dir, float& pdf, bool& specular) {
        float4 clr = diffuse_;
        if (sampler_) {
            clr = sampler_->sample(surf.uv);
        }

        DirectionSample hemi_sample = sample_cos_hemisphere(surf.normal, rng.random_float(), rng.random_float());
        in_dir = hemi_sample.dir;
        pdf = hemi_sample.pdf;
        specular = false;
        return clr; // Cosine and 1/pi cancle out with pdf from the hemisphere sampling.
    }

    inline float4 sample_out(const float3& in_dir, const SurfaceInfo& surf, RNG& rng, float3& out_dir, float& pdf, bool& specular) {
        float4 clr = diffuse_;
        if (sampler_) {
            clr = sampler_->sample(surf.uv);
        }

        //DirectionSample hemi_sample = sample_uniform_hemisphere(surf.normal, rng.random_float(), rng.random_float());
        DirectionSample hemi_sample = sample_cos_hemisphere(surf.normal, rng.random_float(), rng.random_float());
        out_dir = hemi_sample.dir;
        pdf = hemi_sample.pdf;
        specular = false;

        return clr * dot(out_dir, surf.normal) * (1.0f / pi) * (1.0f / pdf); // 1/pi cancles out with pdf from the hemisphere sampling, leaving only the factor 2.
    }

    inline float4 eval(const float3& out_dir, const SurfaceInfo& surf, const float3& in_dir, float& pdf_dir, float& pdf_rev) {
        float4 clr = diffuse_;
        if (sampler_) {
            clr = sampler_->sample(surf.uv);
        }

        pdf_dir = (1.0f / pi) * dot(surf.normal, in_dir);
        pdf_rev = pdf_dir;

        return clr * (1.0f / pi);// * dot(surf.normal, in_dir);
    }

private:
    float4 diffuse_;
    TextureSampler* sampler_;
};

/// Combines two materials together using weights from a texture. 0 => full contribution from the first material
/// 1 => full contribution from the second material
class CombineMaterial : public Material {
public:
    CombineMaterial(TextureSampler* scale, std::unique_ptr<Material> m1, std::unique_ptr<Material> m2)
        : Material(combine, m1->is_delta && m2->is_delta), scale_(scale), m1_(std::move(m1)), m2_(std::move(m2)) { }

    inline float4 sample_in(const float3& out_dir, const SurfaceInfo& surf, RNG& rng, float3& in_dir, float& pdf, bool& specular) {
        const float s = scale_->sample(surf.uv).x;
        const float r = rng.random_float();
        if (r < s) {
            return sample_material_in(m1_.get(), out_dir, surf, rng, in_dir, pdf, specular);
        } else {
            return sample_material_in(m2_.get(), out_dir, surf, rng, in_dir, pdf, specular);
        }
    }

    inline float4 sample_out(const float3& in_dir, const SurfaceInfo& surf, RNG& rng, float3& out_dir, float& pdf, bool& specular) {
    }

    inline float4 eval(const float3& out_dir, const SurfaceInfo& surf, const float3& in_dir, float& pdf_dir, float& pdf_rev) {
        const float s = scale_->sample(surf.uv).x;

        const float4 v1 = evaluate_material(m1_.get(), out_dir, surf, in_dir, pdf_dir, pdf_rev);

        float pd, pr;
        const float4 v2 = evaluate_material(m2_.get(), out_dir, surf, in_dir, pd, pr);

        pdf_dir *= pd;
        pdf_rev *= pr;

        return v1 * s + (1.0f - s)  * v2;
    }

private:
    TextureSampler* scale_;
    std::unique_ptr<Material> m1_;
    std::unique_ptr<Material> m2_;
};


/// Perfect mirror reflection.
class MirrorMaterial : public Material {
public:
    MirrorMaterial(float eta, float kappa, const float3& ks) : Material(mirror, true), eta_(eta), kappa_(kappa), ks_(ks, 1.0f) { }

    inline float4 sample_in(const float3& out_dir, const SurfaceInfo& surf, RNG& rng, float3& in_dir, float& pdf, bool& specular) {
        // calculate the reflected direction
        in_dir = -out_dir + 2.0f * surf.normal * dot(out_dir, surf.normal);
        float cos_theta = fabsf(dot(surf.normal, out_dir));

        pdf = 1.0f;
        specular = true;

        return float4(fresnel_conductor(cos_theta, eta_, kappa_));
    }

    inline float4 sample_out(const float3& in_dir, const SurfaceInfo& surf, RNG& rng, float3& out_dir, float& pdf, bool& specular) {
    }

    inline float4 eval(const float3& out_dir, const SurfaceInfo& surf, const float3& in_dir, float& pdf_dir, float& pdf_rev) {
        pdf_rev = pdf_dir = 0.0f;
        return float4(0.0f);
    }

private:
    float eta_;
    float kappa_;
    float4 ks_;
};

class GlassMaterial : public Material {
public:
    GlassMaterial(float eta, const float3& tf, const float3& ks) : Material(glass, true), eta_(eta), tf_(/*tf,*/ 1.0f), ks_(/*ks,*/ 1.0f) {}

    inline float4 sample_in(const float3& out_dir, const SurfaceInfo& surf, RNG& rng, float3& in_dir, float& pdf, bool& specular) {
        specular = true;

        float3 normal = surf.normal;

        float cos_theta = dot(normal, out_dir);
        float eta_i = 1.0f;
        float eta_o = eta_;

        if (cos_theta < 0) {
            std::swap(eta_i, eta_o);
            cos_theta = -cos_theta;
            normal = -normal;
        }

        const float etafrac = eta_i / eta_o;
        const float sin2sq = etafrac * etafrac * (1.0f - cos_theta * cos_theta);

        const float3 reflect_dir = reflect(-out_dir, normal);

        if (sin2sq >= 1.0f) {
            // total internal reflection
            in_dir = reflect_dir;
            pdf = 1.0f;
            return float4(1.0f);
        }

        const float cos_o = sqrtf(1.0f - sin2sq);
        const float fr = fresnel_dielectric(cos_theta, cos_o, eta_i, eta_o);

        const float rnd_num = rng.random_float();
        if (rnd_num < fr) {
            in_dir = reflect_dir;
            pdf = 1.0f;
            return ks_;
        } else {
            const float3 refract_dir = -etafrac * out_dir + (etafrac * cos_theta - cos_o) * normal;

            in_dir = refract_dir;
            pdf = 1.0f;

            return tf_ * (1.0f / (etafrac * etafrac));
        }
    }

    inline float4 sample_out(const float3& in_dir, const SurfaceInfo& surf, RNG& rng, float3& out_dir, float& pdf, bool& specular) {
    }

    inline float4 eval(const float3& out_dir, const SurfaceInfo& surf, const float3& in_dir, float& pdf_dir, float& pdf_rev) {
        pdf_rev = pdf_dir = 0.0f;
        return float4(0.0f);
    }

private:
    float eta_;
    float4 tf_;
    float4 ks_;
};

/// Material for diffuse emissive objects.
class EmissiveMaterial : public Material {
public:
    EmissiveMaterial(const float4& color) : color_(color), Material(emissive, false) { }

    inline float4 sample_in(const float3& out_dir, const SurfaceInfo& surf, RNG& rng, float3& in_dir, float& pdf, bool& specular) {
        // uniform sample the hemisphere
        DirectionSample hemi_sample = sample_cos_hemisphere(surf.normal, rng.random_float(), rng.random_float());
        in_dir = hemi_sample.dir;
        pdf = hemi_sample.pdf;
        specular = false;
        return float4(0.0f) * (1.0f / pi);
    }

    inline float4 sample_out(const float3& in_dir, const SurfaceInfo& surf, RNG& rng, float3& out_dir, float& pdf, bool& specular) {
        return float4(0.0f);
    }

    inline float4 eval(const float3& out_dir, const SurfaceInfo& surf, const float3& in_dir, float& pdf_dir, float& pdf_rev) {
        pdf_rev = pdf_dir = 0.0f;
        return float4(0.0f);
    }

    inline float4 color() { return color_; }

    inline void set_light(Light* l) { light_ = l; }
    inline Light* light() { return light_; }

private:
    float4 color_;
    Light* light_;
};

#define ALL_MATERIALS() \
    HANDLE_MATERIAL(Material::lambert, LambertMaterial) \
    HANDLE_MATERIAL(Material::mirror, MirrorMaterial) \
    HANDLE_MATERIAL(Material::emissive, EmissiveMaterial) \
    HANDLE_MATERIAL(Material::combine, CombineMaterial) \
    HANDLE_MATERIAL(Material::glass, GlassMaterial)

inline float4 sample_material_in(Material* mat, const float3& out_dir, const SurfaceInfo& surf, RNG& rng, float3& in_dir, float& pdf, bool& specular) {
    switch (mat->kind) {
#define HANDLE_MATERIAL(m,T) \
        case m: return static_cast<T*>(mat)->sample_in(out_dir, surf, rng, in_dir, pdf, specular);
    ALL_MATERIALS()
#undef HANDLE_MATERIAL
    }
}

inline float4 sample_material_out(Material* mat, const float3& in_dir, const SurfaceInfo& surf, RNG& rng, float3& out_dir, float& pdf, bool& specular) {
    switch (mat->kind) {
#define HANDLE_MATERIAL(m,T) \
        case m: return static_cast<T*>(mat)->sample_out(in_dir, surf, rng, out_dir, pdf, specular);
    ALL_MATERIALS()
#undef HANDLE_MATERIAL
    }
}

inline float4 evaluate_material(Material* mat, const float3& out_dir, const SurfaceInfo& surf, const float3& in_dir, float& pdf_dir, float& pdf_rev) {
    switch (mat->kind) {
#define HANDLE_MATERIAL(m,T) \
        case m: return static_cast<T*>(mat)->eval(out_dir, surf, in_dir, pdf_dir, pdf_rev);
    ALL_MATERIALS()
#undef HANDLE_MATERIAL
    }
}

#undef ALL_MATERIALS

using MaterialContainer = std::vector<std::unique_ptr<imba::Material>>;

}

#endif
