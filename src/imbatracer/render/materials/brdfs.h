#ifndef IMBA_BRDFS_H
#define IMBA_BRDFS_H

#include "bsdf.h"

namespace imba {

class Lambertian : public BxDF {
public:
    Lambertian(const float4& color)
        : BxDF(BxDFFlags(BSDF_DIFFUSE | BSDF_REFLECTION)), color_(color)
    {}

    virtual float4 eval(const float3& out_dir, const float3& in_dir) const override {
        return color_ * (1.0f / pi);
    }

private:
    float4 color_;
};

class SpecularReflection : public BxDF {
public:
    SpecularReflection(const float4& scale, const Fresnel& fresnel)
        : BxDF(BxDFFlags(BSDF_SPECULAR | BSDF_REFLECTION)), scale_(scale), fresnel_(fresnel)
    {}

    virtual float4 eval(const float3& out_dir, const float3& in_dir) const override {
        return float4(0.0f);
    }

    virtual float4 sample(const float3& out_dir, float3& in_dir, float rnd_num_1, float rnd_num_2, float& pdf) const override {
        in_dir = float3(-out_dir.x, -out_dir.y, out_dir.z); // Reflected direction in shading space (normal == z.)
        pdf = 1.0f;

        return fresnel_.eval(cos_theta(out_dir)) * scale_ / fabsf(cos_theta(in_dir));
    }

    virtual float pdf(const float3& out_dir, const float3& in_dir) const override {
        return 0.0f; // Probability between any two randomly choosen directions is zero due to delta distribution.
    }

private:
    float4 scale_;
    const Fresnel& fresnel_;
};

class Phong : public BxDF {
public:
    Phong(const float4& coefficient, float exponent)
        : BxDF(BxDFFlags(BSDF_GLOSSY | BSDF_REFLECTION)),
          coefficient_(coefficient), exponent_(exponent)
    {}

    virtual float4 eval(const float3& out_dir, const float3& in_dir) const override {
        auto reflected_in = float3(-in_dir.x, -in_dir.y, in_dir.z);
        float cos_r_o = std::max(0.0f, dot(reflected_in, out_dir));

        return (exponent_ + 2.0f) / (2.0f * pi) * coefficient_ * powf(cos_r_o, exponent_);
    }

    virtual float4 sample(const float3& out_dir, float3& in_dir, float rnd_num_1, float rnd_num_2, float& pdf) const override {
        // Sample a power weighted direction relative to the reflected direction
        auto dir_sample = sample_power_cos_hemisphere(exponent_, rnd_num_1, rnd_num_2);

        auto reflected_in = float3(-out_dir.x, -out_dir.y, out_dir.z);
        float3 reflected_tan, reflected_binorm;
        local_coordinates(reflected_in, reflected_tan, reflected_binorm);

        auto& dir = dir_sample.dir;

        in_dir = float3(reflected_binorm.x * dir.x + reflected_tan.x * dir.y + reflected_in.x * dir.z,
                        reflected_binorm.y * dir.x + reflected_tan.y * dir.y + reflected_in.y * dir.z,
                        reflected_binorm.z * dir.x + reflected_tan.z * dir.y + reflected_in.z * dir.z);

        pdf = dir_sample.pdf;

        return same_hemisphere(out_dir, in_dir) ? eval(out_dir, in_dir) : float4(0.0f);
    }

    virtual float pdf(const float3& out_dir, const float3& in_dir) const override {
        return power_cos_hemisphere_pdf(exponent_, in_dir);
    }

private:
    float4 coefficient_;
    float exponent_;
};


class OrenNayar : public BxDF {
public:
    OrenNayar(const float4& reflectance, float roughness_degrees)
        : BxDF(BxDFFlags(BSDF_DIFFUSE | BSDF_REFLECTION)),
          reflectance_(reflectance)
    {
        param_sigma_ = radians(roughness_degrees);
        param_sigma_sqr_ = param_sigma_ * param_sigma_;
        param_a_ = 1.0f - param_sigma_sqr_ / (2.0f * (param_sigma_sqr_ + 0.33f));
        param_b_ = 0.45 * param_sigma_sqr_ / (param_sigma_sqr_ + 0.09f);
    }

    virtual float4 eval(const float3& out_dir, const float3& in_dir) const override {
        float sin_theta_in = sin_theta(in_dir);
        float sin_theta_out = sin_theta(out_dir);

        // Compute max(0, cos(phi_i - phi_o)) by using cos(a-b) = cos(a) cos(b) + sin(a) sin(b)
        float max_cos = 0.0f;
        if (sin_theta_in > 0.0001f && sin_theta_out > 0.0001f) {
            float sin_phi_in = sin_phi(in_dir);
            float cos_phi_in = cos_phi(in_dir);

            float sin_phi_out = sin_phi(out_dir);
            float cos_phi_out = cos_phi(out_dir);

            max_cos = std::max(0.0f, cos_phi_in * cos_phi_out + sin_phi_in * sin_phi_out);
        }

        float sin_alpha, tan_beta;
        if (abs_cos_theta(in_dir) > abs_cos_theta(out_dir)) {
            sin_alpha = sin_theta_out;
            tan_beta  = sin_theta_in / abs_cos_theta(in_dir);
        } else {
            sin_alpha = sin_theta_in;
            tan_beta  = sin_theta_out / abs_cos_theta(out_dir);
        }

        return reflectance_ * (1.0f / pi) * (param_a_ + param_b_ * max_cos * sin_alpha * tan_beta);
    }

private:
    float4 reflectance_;

    // Parameters for the reflection model.
    float param_sigma_;
    float param_sigma_sqr_;
    float param_a_;
    float param_b_;
};

}

#endif