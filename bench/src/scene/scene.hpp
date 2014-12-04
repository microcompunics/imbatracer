#ifndef IMBA_SCENE_HPP
#define IMBA_SCENE_HPP

#include <unordered_set>
#include <memory>

#include "image.hpp"
#include "triangle_mesh.hpp"
#include "material.hpp"

namespace imba {

template <typename T>
struct SceneObjectId {
    SceneObjectId(int i) : id_(i) {}
    int id_;
};

typedef SceneObjectId<TriangleMesh> TriangleMeshId;
typedef SceneObjectId<Texture>      TextureId;
typedef SceneObjectId<Material>     MaterialId;

/// Scene represented as a collection of renderable objects, which can be
/// triangle mesh instances, CSG primitives, and so on.
class Scene {
public:
    ~Scene() {
        for (auto mesh : meshes_) { delete mesh; }
        for (auto tex : textures_) { delete tex; }
    }

    TriangleMeshId add_triangle_mesh(TriangleMesh* mesh) {
        meshes_.push_back(mesh);
        return TriangleMeshId(meshes_.size() - 1);
    }

    TextureId add_texture(Texture* texture) {
        textures_.push_back(texture);
        return TextureId(textures_.size() - 1);
    }

    int triangle_mesh_count() const { return meshes_.size(); }
    int texture_count() const { return textures_.size(); }

    const TriangleMesh* triangle_mesh(TriangleMeshId id) const { return meshes_[id.id_]; }
    TriangleMesh* triangle_mesh(TriangleMeshId id) { return meshes_[id.id_]; }

    const Texture* texture(TextureId id) const { return textures_[id.id_]; }
    Texture* texture(TextureId id) { return textures_[id.id_]; }

private:
    std::vector<TriangleMesh*> meshes_;
    std::vector<Texture*>      textures_;
    std::vector<Material>      materials_;

    ThorinUniquePtr<::Scene>         scene_data_;
    ThorinUniquePtr<::CompiledScene> compiled_scene_;
};

} // namespace imba

#endif // IMBA_SCENE_HPP

