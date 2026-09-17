// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

////////////////////////////////////////////////////////////////////////////////
///
/// @author Ken Museth
///
/// @file Geometry.h
///
/// @brief Polygon-mesh / point-cloud container used by vdb_tool. Holds vertex
///        positions plus optional per-vertex RGB colors and any combination
///        of triangle / quad face lists, with a lazily-cached world-space
///        bounding box. Provides stream- and file-based readers and writers
///        for the OBJ, PLY, STL, OFF, GEO, PTS, and XYZ formats (binary and
///        ASCII where applicable), and optional read/write paths for
///        Alembic (.abc), OpenUSD (.usd / .usda / .usdc / .usdz), glTF
///        (.gltf / .glb), OpenVDB (.vdb) and NanoVDB (.nvdb) backends. Also
///        exposes a static fan-triangulate helper used by the readers to
///        convert convex N-gons into triangles.
///
////////////////////////////////////////////////////////////////////////////////

#ifndef VDB_TOOL_GEOMETRY_HAS_BEEN_INCLUDED
#define VDB_TOOL_GEOMETRY_HAS_BEEN_INCLUDED

#include <functional>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cstdlib> // for std::malloc and std::free

#include <tbb/parallel_for.h>
#include <tbb/parallel_reduce.h>
#include <tbb/blocked_range.h>

#include <openvdb/openvdb.h>
#include <openvdb/points/PointCount.h>
#include <openvdb/util/Assert.h>

#ifdef VDB_TOOL_USE_NANO
#include <nanovdb/NanoVDB.h>
#include <nanovdb/io/IO.h>
#endif

#ifdef VDB_TOOL_USE_ABC
#include <Alembic/Abc/TypedPropertyTraits.h>
#include <Alembic/AbcCoreAbstract/All.h>
#include <Alembic/AbcCoreFactory/All.h>
#include <Alembic/AbcCoreOgawa/All.h>
#include <Alembic/AbcGeom/All.h>
#include <Alembic/Util/All.h>
#endif

#ifdef VDB_TOOL_USE_USD
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/points.h>
#include <pxr/usd/usdGeom/xformCache.h>
#endif

// tinygltf is included by Geometry.cc only: it is a single-header library whose
// implementation (TINYGLTF_IMPLEMENTATION) must be compiled into exactly one TU,
// and nothing in the class declaration below needs its types.

#ifdef VDB_TOOL_USE_PDAL
#include "pdal/pdal.hpp"
#include "pdal/PipelineManager.hpp"
#include "pdal/PipelineReaderJSON.hpp"
#include "pdal/util/FileUtils.hpp"
#endif

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include "Util.h"

namespace openvdb {
OPENVDB_USE_VERSION_NAMESPACE
namespace OPENVDB_VERSION_NAME {
namespace vdb_tool {

/// @brief Class that encapsulates (explicit) geometry, i.e. vertices/points,
///        triangles and quads. It is used to represent points and polygon meshes.
/// @details A Geometry instance owns four parallel containers: vertex positions,
///          triangle indices, quad indices, and optional per-vertex RGB colors.
///          It is non-copyable and non-movable; use deepCopy() to duplicate an
///          instance explicitly. Geometry supports reading and writing several
///          common mesh formats (OBJ, PLY, STL, OFF, ABC) as well as a compact
///          native binary format (.geo) defined by the embedded Header struct.
class Geometry
{
public:

    using PosT  = Vec3f;                           ///< Vertex position type (single-precision Vec3f).
    using BBoxT = math::BBox<PosT>;                ///< Axis-aligned bounding box type over PosT.
    using Ptr   = std::shared_ptr<Geometry>;       ///< Shared-pointer alias for heap-allocated Geometry.
    struct Header;                                 ///< Forward declaration of the native .geo file header.

    /// @brief Default constructor: produces an empty Geometry with an invalid bbox.
    Geometry() = default;

    /// @brief Default destructor.
    ~Geometry() = default;

    Geometry(const Geometry&) = delete;            ///< Copy construction is disabled; use deepCopy().
    Geometry(Geometry&&) = delete;                 ///< Move construction is disabled.
    Geometry& operator=(const Geometry&) = delete; ///< Copy assignment is disabled.
    Geometry& operator=(Geometry&&) = delete;      ///< Move assignment is disabled.

    /// @brief Explicitly produce a deep copy of this Geometry.
    /// @return Shared pointer to a newly allocated Geometry containing duplicated
    ///         vertices, triangles, quads, colors, name, and bbox.
    Ptr deepCopy() const;

    /// @brief Read-only access to the vertex array.
    const std::vector<Vec3s>& vtx() const  { return mVtx; }
    /// @brief Read-only access to the triangle index array.
    const std::vector<Vec3I>& tri() const  { return mTri; }
    /// @brief Read-only access to the quad index array.
    const std::vector<Vec4I>& quad() const { return mQuad; }
    /// @brief Read-only access to the optional per-vertex RGB color array.
    const std::vector<Vec3s>& rgb() const  { return mRGB; }

    /// @brief Mutable access to the vertex array.
    /// @warning Modifying vertices invalidates the cached bbox; call clear()
    ///          or otherwise reset mBBox if downstream code relies on it.
    std::vector<Vec3s>& vtx()  { return mVtx; }
    /// @brief Mutable access to the triangle index array.
    std::vector<Vec3I>& tri()  { return mTri; }
    /// @brief Mutable access to the quad index array.
    std::vector<Vec4I>& quad() { return mQuad; }
    /// @brief Mutable access to the per-vertex RGB color array.
    std::vector<Vec3s>& rgb()  { return mRGB; }

    /// @brief Returns the axis-aligned bounding box of the vertices.
    /// @return Reference to the cached bbox. The bbox is computed lazily on the
    ///         first call (in parallel via TBB) and cached until clear() is invoked.
    const BBoxT& bbox() const;

    /// @brief Returns the maximum extent (longest side) of the vertex bbox.
    float maxLength() const;

    /// @brief Erases all vertices, triangles, quads, name, and invalidates the cached bbox.
    void clear();

    /// @brief Write the geometry to file, dispatching on the filename extension.
    /// @param fileName Output file path; extension selects the format (geo, obj, ply, stl, abc, off).
    /// @param ascii    If true and the format supports it (PLY), write in ASCII rather than binary.
    /// @throw std::invalid_argument if the extension is not recognized.
    void write(const std::string &fileName, bool ascii = false) const;
    /// @brief Write the mesh as a Wavefront OBJ file.
    /// @throw std::invalid_argument if the file cannot be opened for writing.
    void writeOBJ(const std::string &fileName) const;
    /// @brief Write the mesh as an OFF (Object File Format) file.
    /// @throw std::invalid_argument if the file cannot be opened for writing.
    void writeOFF(const std::string &fileName) const;
    /// @brief Write the mesh as a PLY file (binary by default, ASCII if @a ascii is true).
    /// @throw std::invalid_argument if the file cannot be opened or if binary buffer allocation fails.
    void writePLY(const std::string &fileName, bool ascii = false) const;
    /// @brief Write the triangulated mesh as a binary STL file.
    /// @throw std::invalid_argument if the file cannot be opened, the host is big-endian,
    ///        or the mesh contains quads (call triangulateQuads() first).
    void writeSTL(const std::string &fileName) const;
    /// @brief Write the geometry in the native compact binary .geo format.
    /// @throw std::invalid_argument if the file cannot be opened for writing.
    void writeGEO(const std::string &fileName) const;
    /// @brief Write the mesh as an Alembic (.abc) file (requires VDB_TOOL_USE_ABC).
    /// @throw std::runtime_error if Alembic support was not enabled at compile time.
    void writeABC(const std::string &fileName) const;

    /// @brief Stream version of writeGEO; serializes this Geometry to @a os.
    /// @return Number of bytes written (matches Header::size()).
    size_t writeGEO(std::ostream &os) const;
    /// @brief Deprecated alias for writeGEO(std::ostream&).
    OPENVDB_DEPRECATED size_t write(std::ostream &os) {return this->writeGEO(os);}
    /// @brief Stream version of writeOBJ.
    void   writeOBJ(std::ostream &os) const;
    /// @brief Stream version of writeOFF.
    void   writeOFF(std::ostream &os) const;
    /// @brief Stream version of writePLY (binary or ASCII based on @a ascii).
    /// @throw std::invalid_argument if binary buffer allocation fails.
    void   writePLY(std::ostream &os, bool ascii = false) const;
    /// @brief Stream version of writeSTL.
    /// @throw std::invalid_argument if the host is big-endian or the mesh contains quads.
    void   writeSTL(std::ostream &os) const;

    /// @brief Read geometry from file, dispatching on the filename extension.
    /// @param fileName Input file path; extension selects the parser.
    /// @param verbose  Verbosity level for diagnostic output (0 = quiet).
    /// @throw std::invalid_argument on unrecognized extension or I/O failure.
    void read(const std::string &fileName, int verbose = 0);
    /// @brief Read a Wavefront OBJ file.
    /// @throw std::invalid_argument if the file cannot be opened.
    void readOBJ(const std::string &fileName);
    /// @brief Read an OFF (Object File Format) file.
    /// @throw std::invalid_argument if the file cannot be opened, the "OFF" header is missing,
    ///        or a face has more vertices than the supported maximum (n-gons beyond quads).
    void readOFF(const std::string &fileName);
    /// @brief Read a PLY file (binary or ASCII auto-detected from the header).
    /// @throw std::invalid_argument if the file cannot be opened, the header is malformed,
    ///        binary buffer allocation fails, or polygons exceed the supported maximum.
    void readPLY(const std::string &fileName);
    /// @brief Read a binary or ASCII STL file (format auto-detected).
    /// @throw std::runtime_error if the file cannot be opened or is unexpectedly empty.
    /// @throw std::invalid_argument if the binary file is malformed, host is big-endian,
    ///        or the ASCII file contains unsupported n-gons.
    void readSTL(const std::string &fileName);
    /// @brief Read a PTS point-cloud file (one or more clouds, ASCII).
    /// @throw std::runtime_error if the file cannot be opened.
    /// @throw std::invalid_argument on a malformed coordinate line.
    void readPTS(const std::string &fileName);
    /// @brief Read a native .geo file (compact binary format).
    /// @throw std::invalid_argument if the file cannot be opened.
    void readGEO(const std::string &fileName);
    /// @brief Read an Alembic (.abc) file (requires VDB_TOOL_USE_ABC).
    /// @throw std::runtime_error if Alembic support was not enabled at compile time
    ///        or if a polygon with more than 4 vertices is encountered.
    void readABC(const std::string &fileName);
    /// @brief Read a USD geometry file (.usd, .usda, .usdc, or .usdz). Traverses every
    ///        UsdGeomMesh and UsdGeomPoints prim and accumulates their points and faces
    ///        into this Geometry, baking each prim's world transform into the vertex
    ///        positions.
    /// @throw std::runtime_error if USD support was not enabled at compile time.
    /// @throw std::invalid_argument if the file cannot be opened as a USD stage.
    /// @note  Requires VDB_TOOL_USE_USD. Triangles and quads are preserved as-is;
    ///        n-gons (n>4) are fan-triangulated. UsdGeomPoints prims contribute only
    ///        vertex positions (no face data). Instancing, subdivision, animation,
    ///        and per-point widths are not supported by this minimal reader.
    void readUSD(const std::string &fileName);
    /// @brief Read a point cloud via PDAL (e.g. LAS/LAZ/E57).
    /// @return true on success, false if PDAL could not parse the file.
    /// @note  Requires VDB_TOOL_USE_PDAL.
    /// @throw std::runtime_error if PDAL support was not enabled at compile time
    ///        or if the underlying PDAL pipeline fails.
    bool readPDAL(const std::string &fileName);
    /// @brief Read a glTF / glb file. Walks every mesh primitive and appends
    ///        POSITION + index data to this Geometry. Non-indexed primitives
    ///        and primitives whose index buffers use UBYTE/USHORT/UINT are all
    ///        supported. Only TRIANGLES mode is consumed; other primitive
    ///        modes (POINTS, LINES, STRIP, FAN) are skipped with a warning.
    /// @throw std::runtime_error if glTF support was not enabled at compile time.
    /// @throw std::invalid_argument if the file cannot be parsed.
    /// @note  Requires VDB_TOOL_USE_GLTF. The node-graph transform stack is
    ///        currently NOT applied — meshes are loaded in their local space.
    ///        Materials, normals, UVs, vertex colors, animation, and skinning
    ///        are also ignored by this minimal reader.
    void readGLTF(const std::string &fileName);
    /// @brief Read an ASCII XYZ point file (x y z per line).
    /// @throw std::invalid_argument if the file cannot be opened or a line is malformed.
    void readXYZ(const std::string &fileName);
    /// @brief Read points from an OpenVDB file (.vdb).
    void readVDB(const std::string &fileName);
    /// @brief Read points from a NanoVDB file (.nvdb). Requires VDB_TOOL_USE_NANO.
    /// @throw std::runtime_error if NanoVDB support was not enabled at compile time.
    void readNVDB(const std::string &fileName);

    /// @brief Stream version of readGEO; deserializes from @a is.
    /// @return Number of bytes consumed, or 0 if the magic header did not match
    ///         (in which case the stream is rewound to its start).
    size_t readGEO(std::istream &is);
    /// @brief Deprecated alias for readGEO(std::istream&).
    OPENVDB_DEPRECATED size_t read(std::istream &is) {return this->readGEO(is);}
    /// @brief Stream version of readOBJ.
    void   readOBJ(std::istream &is);
    /// @brief Stream version of readOFF.
    /// @throw std::invalid_argument on a malformed header or unsupported n-gons.
    void   readOFF(std::istream &is);
    /// @brief Stream version of readPLY.
    /// @throw std::invalid_argument on a malformed header, buffer-allocation failure,
    ///        or unsupported n-gons.
    void   readPLY(std::istream &is);
    /// @brief Stream version of readXYZ.
    /// @throw std::invalid_argument on a malformed coordinate line.
    void   readXYZ(std::istream &is);

    /// @brief Number of vertices in this Geometry.
    size_t vtxCount() const { return mVtx.size(); }
    /// @brief Number of triangles.
    size_t triCount() const { return mTri.size(); }
    /// @brief Number of quads.
    size_t quadCount() const { return mQuad.size(); }
    /// @brief Total polygon count (triangles + quads).
    size_t polyCount() const { return mTri.size() + mQuad.size(); }

    /// @brief Apply an affine transformation to every vertex in place.
    /// @param xform OpenVDB transform whose indexToWorld mapping is applied to each vertex.
    /// @note Invalidates the cached bbox.
    void transform(const math::Transform &xform);

    /// @brief Triangulates each quad into two triangles, using the shortest diagonal.
    /// @return Number of new triangles appended.
    /// @note The quads are removed while the vertex list is unchanged.
    size_t triangulateQuads();

    /// @brief Returns true if this Geometry contains no vertices and no polygons.
    bool isEmpty() const { return mVtx.empty() && mTri.empty() && mQuad.empty(); }
    /// @brief Returns true if this Geometry is a pure point cloud (vertices but no polygons).
    bool isPoints() const { return !mVtx.empty() && mTri.empty() && mQuad.empty(); }
    /// @brief Returns true if this Geometry contains a polygon mesh (vertices plus tris and/or quads).
    bool isMesh() const { return !mVtx.empty() && (!mTri.empty() || !mQuad.empty()); }

    /// @brief Returns this Geometry's user-assigned name (e.g. for stack display).
    const std::string getName() const { return mName; }
    /// @brief Assigns a human-readable name to this Geometry.
    void setName(const std::string &name) { mName = name; }

    /// @brief Print a one-line summary of this Geometry to @a os.
    /// @param n  Optional stack index, printed alongside the summary for context.
    /// @param os Output stream (defaults to std::clog).
    void print(size_t n = 0, std::ostream& os = std::clog) const;

    /// @brief Static method to fan-triangulate a planar and convex N-gon.
    /// @param nGon List of vertex indices for an N-gon.
    /// @return Vector of triangles, as triplets of vertex indices, that make up the N-gon.
    /// @warning The triangulation is naive (fan from @c nGon[0]) and assumes the input N-gon
    ///          is both planar and convex; non-convex polygons need an ear-clip routine instead.
    static std::vector<Vec3I> triangulate(const std::vector<int> &nGon);

    /// @brief Append the fan-triangulation of an N-gon to an existing triangle vector.
    /// @details Avoids the per-face allocation of the return-by-value overload, so it's the
    ///          right choice for use inside reader loops that triangulate many faces.
    /// @param indices    Pointer to the N-gon's vertex indices.
    /// @param n          Number of indices in the N-gon (n < 3 is a no-op).
    /// @param out        Destination vector; the N-2 triangles are appended.
    /// @param indexOffset Constant added to every emitted index (e.g. -1 to convert
    ///                    OBJ's 1-based indices to 0-based, or @c base to translate USD's
    ///                    per-prim indices into Geometry-wide indices).
    static void triangulate(const int *indices, std::size_t n,
                            std::vector<Vec3I> &out, int indexOffset = 0);

private:

    /// @brief Use AD dot (AB cross AC) = 0 to test if all points of a quad are
    ///        in the same plane.
    /// @param quad Quad to be tested.
    /// @return true if all the points of the quad are coplanar.
    bool isPlanar(const Vec4I &quad) const {
        auto q = [&](int i)->const PosT&{ return mVtx[quad[i]]; };
        return math::isApproxZero((q(0)-q(3)).dot((q(0)-q(1)).cross(q(0)-q(2))), 1e-5f);
    }

    std::vector<PosT>  mVtx;     ///< Vertex positions in world space.
    std::vector<Vec3I> mTri;     ///< Triangle indices (zero-based, three per triangle).
    std::vector<Vec4I> mQuad;    ///< Quad indices (zero-based, four per quad).
    std::vector<Vec3s> mRGB;     ///< Optional per-vertex RGB colors (not written to .geo file).
    mutable BBoxT      mBBox;    ///< Lazily computed bbox of mVtx (not written to .geo file).
    std::string        mName;    ///< User-assigned name (e.g. "bunny", "dragon").
    int                mVerbose; ///< Verbosity flag (not written to .geo file).

};// Geometry class

/// @brief Header record prepended to every native .geo file.
/// @details Begins with a magic number identifying the format and contains the
///          byte sizes of the variable-length payload that follows: name, vertex
///          array, triangle array, and quad array. The bbox is also serialized
///          immediately after the name. Used by Geometry::readGEO/writeGEO.
struct Geometry::Header
{
    /// @brief Magic number identifying a vdb_tool .geo file ("vdb_geo1" in ASCII).
    const static uint64_t sMagic = 0x7664625f67656f31UL;
    uint64_t magic; ///< Magic identifier; must equal sMagic on read.
    uint64_t name;  ///< Length in bytes of the geometry name that follows the header.
    uint64_t vtx;   ///< Number of vertices in the payload.
    uint64_t tri;   ///< Number of triangles in the payload.
    uint64_t quad;  ///< Number of quads in the payload.

    /// @brief Default constructor producing a valid magic but empty counts.
    Header() : magic(sMagic), name(0), vtx(0), tri(0), quad(0) {}
    /// @brief Construct a header populated from the given Geometry.
    Header(const Geometry &g) : magic(sMagic), name(g.getName().size()), vtx(g.vtxCount()), tri(g.triCount()), quad(g.quadCount()) {}
    /// @brief Total size in bytes of the header plus its payload on disk.
    uint64_t size() const { return sizeof(*this) + name + sizeof(BBoxT) + sizeof(PosT)*vtx + sizeof(Vec3I)*tri + sizeof(Vec4I)*quad;}
};// Geometry::Header

} // namespace vdb_tool
} // namespace OPENVDB_VERSION_NAME
} // namespace openvdb

#endif// VDB_TOOL_GEOMETRY_HAS_BEEN_INCLUDED
