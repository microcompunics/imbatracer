#include <iostream>
#include <float.h>
#include <limits.h>
#include <core/util.h>
#include <core/assert.h>
#include "objloader.h"

extern "C"
{
    // Debugging
    void print_s(const char *s)
    {
      std::cout << "Impala print: " << s << std::endl;
    }
    void print_si(const char *s, int x)
    {
      std::cout << "Impala print: " << s << " " << x << std::endl;
    }
    void print_sii(const char *s, int x, int y)
    {
      std::cout << "Impala print: " << s << " " << x << ", " << y << std::endl;
    }
    void print_sf(const char *s, float x)
    {
      std::cout << "Impala print: " << s << " " << x << std::endl;
    }
    void print_sfff(const char *s, float x, float y, float z)
    {
      std::cout << "Impala print: " << s << " " << x << ", " << y << ", " << z << std::endl;
    }

    void assert_failed(const char *str)
    {
        std::cerr << "Impala assertion failed: " << str << std::endl;
        rt::debugAbort();
    }


    //void load_file(const char *path, const char *filename, unsigned flags, impala::DynList *overrideMaterials,
    //               impala::DynList *vertices, impala::DynList *normals, impala::DynList *texCoords, impala::DynList *materials, impala::DynList *textures,
    //               impala::DynList *triVerts, impala::DynList *triData);
}
