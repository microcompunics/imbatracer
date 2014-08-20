#ifndef INTERFACE_H
#define INTERFACE_H

#include <math.h>
#include <iostream>
#include <float.h>

namespace rt {
    class Scene;
}

namespace impala {
    // C-side of the Impala structs
    struct Point
    {
        float x, y, z;

        Point() = default;
        Point(float x, float y, float z) : x(x), y(y), z(z) {}

        float &operator[](unsigned i) {
            switch (i) {
            case 0:  return x;
            case 1:  return y;
            default: return z;
            }
        }
        float operator[](unsigned i) const {
            switch (i) {
            case 0:  return x;
            case 1:  return y;
            default: return z;
            }
        }
    };
    inline std::ostream &operator<<(std::ostream &o, const Point &p)
    {
        return o << "(" << p.x << ", " << p.y << ", " << p.z << ")";
    }

    struct Vec
    {
        float x, y, z;

        Vec() = default;
        Vec(float x, float y, float z) : x(x), y(y), z(z) {}

        float &operator[](unsigned i) {
            switch (i) {
            case 0:  return x;
            case 1:  return y;
            default: return z;
            }
        }
        float operator[](unsigned i) const {
            switch (i) {
            case 0:  return x;
            case 1:  return y;
            default: return z;
            }
        }
        float len() const { return sqrtf(x*x + y*y + z*z); }
        Vec normal() const { if(!x && !y && !z) return Vec(0,0,0); float il=1/len(); return Vec(il*x, il*y, il*z); }
    };
    inline std::ostream &operator<<(std::ostream &o, const Vec &v)
    {
        return o << "(" << v.x << ", " << v.y << ", " << v.z << ")";
    }

    struct Float4
    {
        float x, y, z, w;
    };

    struct Matrix
    {
        Float4 rows[4];
    };

    struct Color
    {
        float r, g, b;

        Color() = default;
        Color(float r, float g, float b) : r(r), g(g), b(b) {}

        float &operator[](unsigned i) {
            switch (i) {
            case 0:  return r;
            case 1:  return g;
            default: return b;
            }
        }
        float operator[](unsigned i) const {
            switch (i) {
            case 0:  return r;
            case 1:  return g;
            default: return b;
            }
        }
    };

    struct TexCoord
    {
        float u, v;

        TexCoord() = default;
        TexCoord(float u, float v) : u(u), v(v) {}
    };

    struct Object
    {
        unsigned bvhRoot;
        Matrix o2w, w2o;
        bool swapsHandedness;
    };

    struct BBox
    {
        Point cmin, cmax;

        BBox() = default;
        BBox(const Point &p) : cmin(p), cmax(p) {}
        static BBox empty()
        {
            BBox b;
            b.cmin = Point(FLT_MAX, FLT_MAX, FLT_MAX);
            b.cmax = Point(FLT_MIN, FLT_MIN, FLT_MIN);
            return b;
        }

        Point centroid() const
        {
            return Point(0.5*cmin.x + 0.5*cmax.x, 0.5*cmin.y + 0.5*cmax.y, 0.5*cmin.z + 0.5*cmax.z);
        }
        BBox &extend(const Point &p)
        {
            cmin = Point(std::min(cmin.x, p.x), std::min(cmin.y, p.y), std::min(cmin.z, p.z));
            cmax = Point(std::max(cmax.x, p.x), std::max(cmax.y, p.y), std::max(cmax.z, p.z));
            return *this;
        }
        BBox &extend(const BBox &b)
        {
            return extend(b.cmin).extend(b.cmax);
        }
        static BBox unite(const BBox &b1, const BBox &b2)
        {
            return BBox(b1).extend(b2);
        }
        unsigned longestAxis() const
        {
            float xlen = cmax.x-cmin.x;
            float ylen = cmax.y-cmin.y;
            float zlen = cmax.z-cmin.z;
            if (xlen > ylen) {
                return xlen > zlen ? 0 : 2;
            }
            else {
                return ylen > zlen ? 1 : 2;
            }
        }
        float surface() const
        {
            float xlen = cmax.x-cmin.x, ylen = cmax.y-cmin.y, zlen = cmax.z-cmin.z;
            return 2*(xlen*ylen + xlen*zlen + ylen*zlen);
        }
    };

    struct BVHNode
    {
        BBox bbox;
        unsigned sndChildFirstPrim;
        uint16_t nPrim, axis;

        BVHNode() = default;
        BVHNode(const BBox &bbox) : bbox(bbox) {}
    };

    struct Noise
    {
        int ty;
        unsigned octaves;
        float amplitude;
        float freq;
        float persistence;
    };

    struct Texture
    {
        int ty;
        Color color1;
        Color color2;
        Noise noise;

        static Texture constant(const Color& c)
        {
            return (Texture) {
                .ty = -1,
                .color1 = c,
                .color2 = c,
            };
        }
    };

    struct Material
    {
        // diffuse
        unsigned diffuse;
        // specular (phong)
        unsigned specular;
        float specExp;
        // ambient / emissive
        unsigned emissive;

        static Material dummy()
        {
            return (Material) {
                .diffuse = 1,
                .specular = 0,
                .specExp = -1.0f,
                .emissive = 0,
            };
        }
    };

    struct Light; // opaque to C++

    struct Scene
    {
        BVHNode *bvhNodes;

        Point *verts;
        unsigned *triVerts; // 3 successive entries are the three indices of the vertices of a triangle

        Vec *normals;
        TexCoord *texcoords;
        Material *materials;
        Texture *textures;
        unsigned *triData; // 7 successive indices belong to one triangle: 3 normals, 2 texcoors, 1 material

        Object *objs;
        unsigned nObjs;

        Light *lights;
        unsigned nLights;
    };

    struct View
    {
        Point origin;
        Vec forward, up, right, originalUp;
        float rightFactor, upFactor;
    };

    struct Cam
    {
        View view;
        float param1, param2;
        int camtype;
    };

    struct Integrator
    {
        float minDist, maxDist;
        int mode;
        int itype;
    };

    struct State
    {
        float time;
        Cam cam;
        Integrator integrator;
        Scene scene;
        rt::Scene *sceneMgr;
    };


    extern "C" {
        void impala_init(State *state);
        void impala_update(State *state, float dt);

        void impala_init_bench1(State *state);
        void impala_init_bench2(State *state);

        void impala_render(unsigned *buf, int w, int h, bool measureTime, State *state);
    }

    // test that these are all POD
    inline static void test_for_pod()
    {
        static_assert(std::is_pod<impala::Point>::value, "impala::Point must be a POD");
        static_assert(std::is_pod<impala::Vec>::value, "impala::Vec must be a POD");
        static_assert(std::is_pod<impala::Float4>::value, "impala::Float4 must be a POD");
        static_assert(std::is_pod<impala::Matrix>::value, "impala::Matrix must be a POD");
        static_assert(std::is_pod<impala::Color>::value, "impala::Color must be a POD");
        static_assert(std::is_pod<impala::TexCoord>::value, "impala::TexCoord must be a POD");
        static_assert(std::is_pod<impala::Object>::value, "impala::Object must be a POD");
        static_assert(std::is_pod<impala::BBox>::value, "impala::BBox must be a POD");
        static_assert(std::is_pod<impala::BVHNode>::value, "impala::BVHNode must be a POD");
        static_assert(std::is_pod<impala::Texture>::value, "impala::Texture must be a POD");
        static_assert(std::is_pod<impala::Noise>::value, "impala::Noise must be a POD");
        static_assert(std::is_pod<impala::Material>::value, "impala::Material must be a POD");
        static_assert(std::is_pod<impala::Scene>::value, "impala::Scene must be a POD");
        static_assert(std::is_pod<impala::View>::value, "impala::View must be a POD");
        static_assert(std::is_pod<impala::Cam>::value, "impala::Cam must be a POD");
        static_assert(std::is_pod<impala::Integrator>::value, "impala::Integrator must be a POD");
        static_assert(std::is_pod<impala::State>::value, "impala::State must be a POD");
    }

}

#endif
