// Posing of imported minis (M4): a humanoid rig (from the file's glTF skin,
// or fitted automatically), forward kinematics driven by the same pose angles
// as the parametric figures (figure::pose_names), and Dual Quaternion
// Skinning (Kavan, Collins, Zara, O'Sullivan 2008, ACM TOG 27(4)): blending
// unit dual quaternions instead of matrices keeps the skinned limb's volume
// at bent joints (no "candy wrapper" collapse of linear blend skinning).
//
// Pose convention (model frame: mm, Z up, front -Y, the figure's left on +X):
// angle 0 means arms hanging 10 degrees out from the body and straight
// (the rest pose of the file, T- or A-pose, is normalised to this), the legs,
// spine and head as in the file. Angles are relative to that.
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "anvil/mesh.hpp"

namespace anvil {
struct Skin;
struct ModelData;
}

namespace anvil::rig {

/// Unit quaternion (w, x, y, z).
struct Quat {
    double w = 1, x = 0, y = 0, z = 0;
    Quat operator*(const Quat& b) const {
        return {w * b.w - x * b.x - y * b.y - z * b.z, w * b.x + x * b.w + y * b.z - z * b.y,
                w * b.y - x * b.z + y * b.w + z * b.x, w * b.z + x * b.y - y * b.x + z * b.w};
    }
    Quat conj() const { return {w, -x, -y, -z}; }
    Vec3 rotate(const Vec3& v) const;
    static Quat axis_angle(const Vec3& axis, double angle);
    /// Shortest-arc rotation taking unit vector a to unit vector b.
    static Quat from_to(const Vec3& a, const Vec3& b);
};

/// Rigid transform x' = q x + t.
struct Xf {
    Quat q;
    Vec3 t;
    Vec3 apply(const Vec3& p) const { return q.rotate(p) + t; }
};

enum class Role : int { None, Hips, Spine, Neck, Head, Clavicle, UpperArm, Forearm, Hand, Thigh, Shin, Foot, Tail };

struct Rig {
    std::vector<std::string> names;
    std::vector<int> parent;    // -1: root; parents come before children
    std::vector<Vec3> pivot;    // rest position (mm, model frame)
    std::vector<Role> role;
    std::vector<int> side;      // +1 left (+X), -1 right, 0 centre
    std::vector<Vec3> tip;      // rest end of the joint's bone (child pivot or estimate)
    // per vertex, up to 4 influences (weights sum to 1; all zero = static)
    std::vector<std::array<std::uint16_t, 4>> joints;
    std::vector<std::array<float, 4>> weights;
    bool automatic = false;     // fitted by auto_rig (no skin in the file)

    int find(Role r, int s = 0) const;
    /// Uniform scale about the origin (the model stands on z = 0, centred).
    void scale(double s);
};

/// Rig from a glTF skin: joint roles from the bone names (Mixamo, Tripo,
/// Meshy, Unreal and Blender Rigify conventions), sides from names or
/// position. Joints are reordered so that parents precede children.
Rig from_skin(const Skin& skin, std::size_t vertex_count);
/// Humanoid rig fitted to a standing figure (T-, A- or arms-down pose,
/// facing -Y, feet on z = 0): joint positions from the proportions of the
/// figure and the mesh's own cross-sections, weights by distance to the
/// bones with an inside-the-mesh visibility test (generalized winding
/// number) and an exponential falloff between nearby bones.
Rig auto_rig(const Mesh& m);

/// Posed joint transforms (rest -> posed) for pose angles in radians
/// (missing angles are 0).
std::vector<Xf> pose_joints(const Rig& rig, const std::map<std::string, double>& pose);
/// Dual quaternion skinning of the rest mesh.
Mesh deform(const Mesh& rest, const Rig& rig, const std::vector<Xf>& joints);

/// Grip of a hand (side +1 left, -1 right): origin at the centre of the
/// closed fist, axes of the canonical forearm frame (arm hanging: Z up the
/// arm, palm facing the body), posed.
Xf hand_grip(const Rig& rig, const std::vector<Xf>& joints, int side);

/// An imported model ready for posing: rest mesh scaled to `height0`
/// (standing on z = 0, centred), its rig, provenance.
struct ModelAsset {
    Mesh mesh;
    Rig rig;
    double height0 = 0;
    std::string format;
    std::size_t parts = 0;
};
std::shared_ptr<const ModelAsset> make_asset(ModelData md, double height0);

}  // namespace anvil::rig
