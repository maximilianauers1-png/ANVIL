// STL import/export. Binary size is exactly 84 + 50*N bytes.
// STL has no unit: ANVIL writes "unit=<u>" into the 80-byte header and the
// export dialog shows the unit prominently.
#pragma once
#include <filesystem>
#include <string>

#include "anvil/mesh.hpp"
#include "anvil/units.hpp"

namespace anvil {

struct StlWriteOptions {
    bool ascii = false;
    LengthUnit unit = LengthUnit::Millimeter;  // file coordinates = mm / mm_per_unit
    std::string note;                          // appended to the header
};

/// Encodes the mesh (positions in mm) into STL bytes. Throws on empty mesh.
std::string encode_stl(const Mesh& m, const StlWriteOptions& opt);
/// Writes atomically (temp file + rename). Returns SHA-256 hex of the file content.
std::string write_stl(const std::filesystem::path& path, const Mesh& m, const StlWriteOptions& opt);

struct StlReadResult {
    Mesh mesh;              // vertices welded (exact float equality), positions in mm
    bool ascii = false;
    std::string header;
    std::size_t raw_triangles = 0;
    std::size_t dropped_degenerate = 0;  // triangles with repeated vertex indices after welding
};
/// Reads binary or ASCII STL. `unit` is the unit the file is assumed to be in.
StlReadResult read_stl(const std::filesystem::path& path, LengthUnit unit = LengthUnit::Millimeter);
StlReadResult decode_stl(const std::string& bytes, LengthUnit unit = LengthUnit::Millimeter);

/// SHA-256 of a byte string, lowercase hex (FIPS 180-4).
std::string sha256_hex(const std::string& bytes);

}  // namespace anvil
