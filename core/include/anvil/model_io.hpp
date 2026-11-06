// Import of AI-generated minis and other assets (Tripo, Meshy, ...):
// glTF 2.0 (.glb / .gltf) with node hierarchy and optional skin, Wavefront
// OBJ, STL. Everything is converted to ANVIL's frame: millimetres, Z up,
// front towards -Y (glTF: Y up, front +Z, metres -> (x, y, z) -> (x, -z, y)).
#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "anvil/mesh.hpp"

namespace anvil {

/// Column-major 4x4 affine matrix (glTF convention).
struct Mat4d {
    std::array<double, 16> m{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    static Mat4d identity() { return {}; }
    Vec3 point(const Vec3& p) const {
        return {m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12], m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
                m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
    }
    Vec3 dir(const Vec3& p) const {
        return {m[0] * p.x + m[4] * p.y + m[8] * p.z, m[1] * p.x + m[5] * p.y + m[9] * p.z, m[2] * p.x + m[6] * p.y + m[10] * p.z};
    }
    Mat4d operator*(const Mat4d& b) const;
    Mat4d inverse_affine() const;
    Vec3 translation() const { return {m[12], m[13], m[14]}; }
};

/// Skeleton and skin weights (rest pose), in ANVIL's frame.
struct Skin {
    std::vector<std::string> joint_names;
    std::vector<int> parent;                 // -1: root
    std::vector<Mat4d> rest_world;           // joint frame at rest (model space, mm)
    std::vector<std::array<std::uint16_t, 4>> joints;  // per vertex, up to 4 influences
    std::vector<std::array<float, 4>> weights;          // per vertex, sum 1
};

struct ModelData {
    Mesh mesh;                       // rest pose, mm, Z up; read_model welds equal positions
    std::shared_ptr<const Skin> skin;  // null if the file has none
    std::string format;              // "glb", "gltf", "obj", "stl"
    std::size_t parts = 0;           // meshes/primitives merged
};

/// Reads a model. The scale is left as stored (glTF: metres -> mm x1000; OBJ/STL: as mm).
ModelData read_model(const std::filesystem::path& path);
ModelData decode_glb(const std::string& bytes, const std::filesystem::path& base_dir);
ModelData decode_gltf_json(const std::string& json_text, const std::string& bin_chunk, const std::filesystem::path& base_dir);
ModelData decode_obj(const std::string& text);

/// Uniformly scales and moves the model so that its height (Z extent) is
/// `height_mm`, it stands on z = 0 and is centred on X/Y. Returns the scale.
double fit_to_height(ModelData& md, double height_mm);

}  // namespace anvil
