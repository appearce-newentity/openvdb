// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

////////////////////////////////////////////////////////////////////////////////
///
/// @author Ken Museth
///
/// @file Tool.h
///
/// @brief Defines the Tool class, which chains together any sequence of high-level
///        OpenVDB operations exposed by the vdb_tool command-line utility.
///
/// @details Tool ties Parser (command-line action registry) and Geometry (polygon
///          mesh and point storage) together with the internal stacks of VDB grids
///          and Geometry instances. For example, it can convert a sequence of polygon
///          meshes and particles to level sets, perform a large number of operations
///          on these level set surfaces, generate adaptive polygon meshes from level
///          sets, render images, and write particles, meshes, or VDBs to disk.
///
/// @warning Human-readable output is written to the standard-error stream
///          (primarily std::clog, with std::cerr for errors), never to
///          std::cout, because std::cout is reserved for piping VDB / NanoVDB
///          grids to stdout (e.g. "-write stdout.vdb").
///
////////////////////////////////////////////////////////////////////////////////

#ifndef VDB_TOOL_HAS_BEEN_INCLUDED
#define VDB_TOOL_HAS_BEEN_INCLUDED

#include <openvdb/openvdb.h>
#include <openvdb/io/Stream.h>
#include <openvdb/util/CpuTimer.h>
#include <openvdb/util/Formats.h>
#include <openvdb/util/Assert.h>
#include <openvdb/tools/Composite.h>
#include <openvdb/tools/Count.h>// for tools::minMax (used by -print level=2)
#include <openvdb/tools/Diagnostics.h>
#include <openvdb/tools/Statistics.h>
#include <openvdb/tools/FastSweeping.h>
#include <openvdb/tools/LevelSetAdvect.h>
#include <openvdb/tools/LevelSetDilatedMesh.h>
#include <openvdb/tools/LevelSetSphere.h>
#include <openvdb/tools/LevelSetFilter.h>
#include <openvdb/tools/LevelSetMeasure.h>
#include <openvdb/tools/LevelSetMorph.h>
#include <openvdb/tools/LevelSetPlatonic.h>
#include <openvdb/tools/LevelSetRebuild.h>
#include <openvdb/tools/LevelSetUtil.h>
#include <openvdb/tools/RayIntersector.h>
#include <openvdb/tools/RayTracer.h>
#include <openvdb/tools/MeshToVolume.h>
#include <openvdb/tools/ParticlesToLevelSet.h>
#include <openvdb/tools/PointScatter.h>
#include <openvdb/tools/PointsToMask.h>
#include <openvdb/tools/VolumeToMesh.h>
#include <openvdb/tools/GridOperators.h>
#include <openvdb/tools/GridTransformer.h>
#include <openvdb/tools/Prune.h>
#include <openvdb/tools/Clip.h>
#include <openvdb/tools/Mask.h> // for tools::interiorMask()
#include <openvdb/tools/MultiResGrid.h>
#include <openvdb/tools/SignedFloodFill.h>
#include <openvdb/tools/PointIndexGrid.h>
#include <openvdb/points/PointConversion.h>
#include <openvdb/points/PointCount.h>

#ifdef VDB_TOOL_USE_NANO
#include <nanovdb/NanoVDB.h>
#include <nanovdb/io/IO.h>
#include <nanovdb/tools/CreateNanoGrid.h>
#include <nanovdb/tools/NanoToOpenVDB.h>
#endif

#ifdef VDB_TOOL_USE_EXR
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfOutputFile.h>
#include <OpenEXR/ImfPixelType.h>
#endif

#ifdef VDB_TOOL_USE_PNG
#include <png.h>
#endif

#ifdef VDB_TOOL_USE_PDAL
#include <pdal/pdal.hpp>
#endif

#ifdef VDB_TOOL_USE_JPG
#include <jpeglib.h>
#endif

#ifdef VDB_TOOL_USE_AX
#include <openvdb_ax/ax.h>// for openvdb::ax::run (the -ax action)
#endif

#include <tbb/blocked_range2d.h>
#include <tbb/enumerable_thread_specific.h>

#include "Calculator.h"
#include "Parser.h"
#include "Geometry.h"
#include "ShrinkWrap.h"

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#ifdef VDB_TOOL_USE_MPEG
#include <cstdlib>// for std::system
#endif

#ifndef VDB_TOOL_FFMPEG_PATH
#define VDB_TOOL_FFMPEG_PATH "ffmpeg"
#endif

namespace openvdb {
OPENVDB_USE_VERSION_NAMESPACE
namespace OPENVDB_VERSION_NAME {
namespace vdb_tool {

/// @brief Top-level command-line tool that chains OpenVDB high-level operations.
/// @details Owns the action parser, the stacks of in-flight Geometry and VDB grids,
///          and the log-redirection state. A Tool instance is constructed from argv,
///          which registers and parses all command-line actions; run() then executes
///          them in order. The class is non-copyable and non-movable.
class Tool
{
public:

    /// @brief Construct a Tool from command-line arguments.
    /// @param argc Argument count, as received by main().
    /// @param argv Argument vector, as received by main(). argv[0] is taken as the command name.
    /// @throw std::invalid_argument if parsing fails (unknown action, malformed option, etc.).
    Tool(int argc, char *argv[]);

    /// @brief Destructor; restores std::clog if a log file was opened.
    ~Tool() {this->endLog();}

    Tool(const Tool&) = delete;            ///< Copy construction is disabled.
    Tool(Tool&&) = delete;                 ///< Move construction is disabled.
    Tool& operator=(const Tool&) = delete; ///< Copy assignment is disabled.
    Tool& operator=(Tool&&) = delete;      ///< Move assignment is disabled.

    /// @brief Execute every action that was registered during construction, in order.
    /// @note  On a fatal exception inside an action this method writes the message to
    ///        std::cerr and calls std::exit(EXIT_FAILURE) rather than propagating.
    void run();

    /// @brief Redirect std::clog/std::cerr/std::cout to a log file for the remainder of this Tool's lifetime.
    /// @param logFile Path of the log file. If empty, a timestamped name is generated.
    /// @param append  If true, append to the existing file; otherwise truncate it (default).
    /// @param tee     If true (default), output is also written to the original terminal stream so the
    ///                user keeps interactive feedback while the file accumulates the same data. If false,
    ///                output is routed exclusively to the log file (the pre-#6 behaviour).
    /// @throw std::invalid_argument if the file cannot be opened or the standard stream buffers cannot be captured.
    /// @note Subsequent calls are no-ops while a redirection is already active.
    void startLog(std::string logFile, bool append = false, bool tee = true);

    /// @brief Restore std::clog/std::cerr/std::cout to their original buffers and close the log file (if any).
    void endLog() {
      if (mOldClogBuffer) std::clog.rdbuf(mOldClogBuffer);
      if (mOldCerrBuffer) std::cerr.rdbuf(mOldCerrBuffer);
      if (mOldCoutBuffer) std::cout.rdbuf(mOldCoutBuffer);
      if (mLogFile.is_open()) mLogFile.close();
      mOldClogBuffer = nullptr;
      mOldCerrBuffer = nullptr;
      mOldCoutBuffer = nullptr;
      mClogTee.reset();
      mCerrTee.reset();
      mCoutTee.reset();
    }

    /// @brief Print a summary of the current VDB-grid and Geometry stacks to @a os.
    void print(std::ostream& os = std::clog) const;

    /// @brief Print the canonical "-action option=value ..." form of every queued action to @a os.
    void print_args(std::ostream& os = std::clog) const;

    /// @brief Return the current version of this tool as "major.minor.patch".
    static std::string version() {return std::to_string(sMajor)+"."+std::to_string(sMinor)+"."+std::to_string(sPatch);}
    /// @brief Return the major version (incremented on incompatible option/file changes).
    static int major() {return sMajor;}
    /// @brief Return the minor version (incremented on backwards-compatible new features).
    static int minor() {return sMinor;}
    /// @brief Return the patch version (incremented on backwards-compatible bug fixes).
    static int patch() {return sPatch;}

private:

    static const int sMajor = 10; ///< Major version: incremented on incompatible option/file changes.
    static const int sMinor =  8; ///< Minor version: incremented on backwards-compatible new features.
    static const int sPatch =  0; ///< Patch version: incremented on backwards-compatible bug fixes.

    using GridT   = FloatGrid;                                       ///< Scalar grid type used by most level-set operations.
    using FilterT = std::unique_ptr<tools::LevelSetFilter<GridT>>;   ///< Owned pointer to a LevelSetFilter over GridT.
    struct Points; ///< Forward declaration of the helper points wrapper used by particlesToSdf.
    struct Header; ///< Forward declaration of the config-file header record.

    mutable util::CpuTimer   mTimer;          ///< Reusable timer for verbose timing reports.
    std::string              mCmdName;        ///< Base name of this command-line tool (argv[0]).
    std::string              mRawCmdLine;     ///< Verbatim argv joined by spaces — used in the log header.
    std::list<Geometry::Ptr> mGeom;           ///< Stack of Geometry instances owned by this tool (back = top).
    std::list<GridBase::Ptr> mGrid;           ///< Stack of VDB grids owned by this tool (back = top).
    Parser                   mParser;         ///< Command-line action parser and processor.
    bool                     mErrorOnWarning; ///< If true, warning() escalates to a fatal error.
    std::ofstream            mLogFile;        ///< Backing file used by startLog/endLog when active.
    std::streambuf          *mOldClogBuffer;  ///< Cached std::clog buffer for restoring after logging.
    std::streambuf          *mOldCerrBuffer;  ///< Cached std::cerr buffer for restoring after logging.
    std::streambuf          *mOldCoutBuffer;  ///< Cached std::cout buffer for restoring after logging.
    std::unique_ptr<TeeBuf>  mClogTee;        ///< Tee streambuf for std::clog (terminal + log file) when -log tee=true.
    std::unique_ptr<TeeBuf>  mCerrTee;        ///< Tee streambuf for std::cerr.
    std::unique_ptr<TeeBuf>  mCoutTee;        ///< Tee streambuf for std::cout.

    /// @brief Delete all queued Geometry, VDB grids, and local variables.
    void clear();

    /// @brief Deep-copy VDB grids and/or Geometry by index onto their respective stacks.
    void copy();

    /// @brief Run OpenVDB diagnostics checks on VDB grids on the stack.
    void diagnose();

    /// @brief Print value statistics (min, max, mean, std. dev.) for VDB grids on the stack.
    void stats();

    /// @brief Print an ASCII bar histogram of the active values of VDB grids on the stack.
    void histogram();

    /// @brief Rename a VDB grid and/or Geometry on the stack by age index.
    void rename();

    /// @brief Swap two VDB grids and/or two Geometry entries on their respective stacks.
    void swap();

    /// @brief Clip an input VDB grid against another grid, a bbox, or a frustum.
    /// @tparam GridType Type of the input grid being clipped.
    /// @param v     Numeric parameters defining the clipping region (interpretation depends on mode).
    /// @param age   Stack age of the secondary clipping grid, or sentinel meaning "use bbox/frustum".
    /// @param input The grid being clipped (left unchanged).
    /// @return Shared pointer to the clipped grid.
    /// @note  Defined in src/ToolGrid.cc rather than in this header, and instantiated there
    ///        only for FloatGrid and Vec3SGrid by Tool::clip(). Calling it from any other
    ///        translation unit will fail to link: move the definition back into Tool.h (or
    ///        add an explicit instantiation in ToolGrid.cc) before doing so.
    template <typename GridType>
    GridBase::Ptr clip(const VecF &v, int age, const GridType &input);
    /// @brief Action callback for "-clip"; dispatches to the templated clip() above.
    void clip();

    /// @brief Composite two grids using a binary op (min, max, or sum).
    void composite();

    /// @brief Generate a derived grid (e.g. gradient, curl, divergence) from another grid.
    void compute();

    /// @brief Import and process one or more configuration files.
    void config();

    /// @brief Perform CSG operations (union/intersection/difference) between two level-set surfaces.
    void csg();

    /// @brief Run the Enright advection benchmark on a level set.
    void enright();

    /// @brief Expand the narrow band of a level set.
    void expandLevelSet();

    /// @brief Perform filtering (convolution) of a level-set surface.
    void filterLevelSet();

    /// @brief Signed flood-fill of a level-set VDB.
    void floodLevelSet();

    /// @brief Print documentation for one, multiple, or all available actions.
    void help();

    /// @brief Convert an iso-surface of a scalar field into a level set (i.e. SDF).
    void isoToLevelSet();

    /// @brief Convert a volume into an adaptive polygon mesh.
    void volumeToMesh();

    /// @brief Create a level-set sphere, i.e. a narrow-band signed distance to a sphere.
    void levelSetSphere();

    /// @brief  Convert signed distance field into a unsigned distance field
    void sdf2udf();

    /// @brief Apply a simple function to each voxel in a grid
    void forValues();

    /// @brief Create a level-set platonic solid with the specified number of polygon faces.
    void levelSetPlatonic();

    /// @brief Convert a level-set VDB into a fog volume (normalized density).
    void levelSetToFog();

    /// @brief Convert a polygon mesh into a symmetric or asymmetric narrow-band level set.
    void meshToLevelSet();

    /// @brief Convert a polygon mesh into a symmetric narrow-band unsigned distance field.
    void meshToUnsignedDistanceField();

    /// @brief Convert an arbitrary (possibly non-watertight) polygon soup into a narrow-band level set.
    void shrinkWrap();

    /// @brief Generate a dx-offset surface from a polygon mesh or polygon soup.
    void meshToOffset();

#ifdef VDB_TOOL_USE_AX
    /// @brief Run an OpenVDB AX code snippet over one or more selected grids.
    /// @note  Gated behind VDB_TOOL_USE_AX (requires the openvdb_ax library + LLVM).
    void ax();
#endif

    /// @brief Convert every quad in the current mesh into two triangles.
    void quadsToTriangles();

    /// @brief Convert a sequence of image files into a single MPEG movie file.
    void movie();

    /// @brief Construct a level-of-detail sequence of VDB trees with powers-of-two refinements.
    void multires();

    /// @brief Perform morphological dilation/erosion on a level-set surface.
    void offsetLevelSet();

    /// @brief Convert geometry points into a narrow-band level set.
    void particlesToLevelSet();

    /// @brief Encode geometry points into a VDB PointDataGrid.
    void pointsToVdb();

    /// @brief Prune away inactive values in a VDB grid.
    void pruneLevelSet();

    /// @brief Read one or more geometry or VDB files from disk or STDIN.
    void read();
    /// @brief Read a geometry file (mesh or point cloud) and push it onto the geometry stack.
    void readGeo(  const std::string &fileName);
    /// @brief Read an OpenVDB file and push every selected grid onto the grid stack.
    void readVDB(  const std::string &fileName);
    /// @brief Read a NanoVDB file (.nvdb) and push every selected grid onto the grid stack.
    void readNVDB( const std::string &fileName);

    /// @brief Ray-trace level-set surfaces or volume-render fog volumes.
    void render();

    /// @brief Resample one VDB grid into another VDB grid or onto a transformed copy of itself.
    void resample();

    /// @brief Segment an input VDB into a list of topologically disconnected VDB grids.
    void segment();

    /// @brief Scatter points into the active values of an input VDB grid.
    void scatter();

    /// @brief Generate images of axis-aligned volume slices.
    void slice();

    /// @brief Apply affine transformations (uniform scale -> rotation -> translation) to VDB grids and geometry.
    void transform();

    /// @brief Extract points encoded in a VDB into geometry-format point lists.
    void vdbToPoints();

    /// @brief Write the list of geometries, VDB grids, or config files to disk or STDOUT.
    void write();
    /// @brief Write a single geometry to disk in the format implied by the file extension.
    void writeGeo( const std::string &fileName);
    /// @brief Write a single VDB grid to disk.
    void writeVDB( const std::string &fileName);
    /// @brief Write a single VDB grid as a NanoVDB (.nvdb) file.
    void writeNVDB(const std::string &fileName);
    /// @brief Write the currently parsed action list as a config file.
    void writeConf(const std::string &fileName);

    /// @brief Estimate the voxel size of a level set from a desired grid dimension.
    /// @param maxDimension Maximum voxel resolution along the longest axis of the bbox.
    /// @param exWidth      Exterior half-width of the narrow band, in voxel units.
    /// @param inWidth      Interior half-width of the narrow band, in voxel units.
    /// @param geo_age      Stack age of the geometry whose bbox drives the estimate.
    /// @return Voxel size in world units.
    float estimateVoxelSize(int maxDimension, float exWidth, float inWidth, int geo_age);
    /// @brief Convenience overload: symmetric narrow band (exWidth == inWidth == halfWidth).
    float estimateVoxelSize(int maxDim,  float halfWidth, int geo_age) {return this->estimateVoxelSize(maxDim, halfWidth, halfWidth, geo_age);}

    /// @brief Build a LevelSetFilter configured with the given spatial and temporal schemes.
    /// @param grid  Grid the filter will operate on.
    /// @param space Spatial discretization order.
    /// @param time  Temporal discretization order.
    FilterT createFilter(GridT &grid,  int space, int time);

    /// @brief Return a formatted string of usage examples (for the -examples action).
    std::string examples() const;

    /// @brief Emit a banner-framed warning to @a os. Escalates to error if mErrorOnWarning is true.
    void warning(const std::string &msg, std::ostream& os = std::clog) const;

    /// @brief Register every available action with the parser. Called from the constructor.
    void init();

    /// @brief Return an iterator to the VDB grid at stack age @a age (0 = most recent).
    /// @throw std::invalid_argument if @a age exceeds the current stack depth.
    inline std::list<GridBase::Ptr>::const_reverse_iterator getGrid(size_t age) const;
    /// @brief Return an iterator to the Geometry at stack age @a age (0 = most recent).
    /// @throw std::invalid_argument if @a age exceeds the current stack depth.
    inline std::list<Geometry::Ptr>::const_reverse_iterator getGeom(size_t age) const;

    /// @brief Translate one "vdb"/"geo" option value, replacing any non-numeric (name)
    ///        token with the stack age of the entry carrying that exact name.
    /// @details Installed as mParser.onGetOption so it runs on every read of a "vdb"/"geo"
    ///          option, letting each action's existing numeric parsing and error handling
    ///          (age lists, "*", out-of-range checks, etc.) operate unchanged downstream.
    ///          Resolution happens per-read against the CURRENT stack rather than being
    ///          written back into the option, so loop bodies re-resolve each iteration.
    /// @param optName Option name, either "vdb" or "geo" (selects which stack to search).
    /// @param raw     Fully expression-resolved option value (i.e. after {} substitution).
    /// @return The translated value: unchanged if empty or "*", otherwise every token is numeric.
    /// @throw std::out_of_range   if a name token matches no entry on the corresponding stack.
    /// @throw std::invalid_argument if a name token matches more than one entry. A name must
    ///        identify exactly one entry, since most actions read "vdb"/"geo" as a single age
    ///        and those that read a list may mutate the stack while consuming it.
    std::string resolveStackOption(const std::string &optName, const std::string &raw) const;

    /// @brief Convert the output of a VolumeToMesh pass into a Geometry instance.
    Geometry::Ptr mesherToGeometry(tools::VolumeToMesh&) const;
    /// @brief Adaptively mesh a scalar grid at @a isoValue and return the result as a Geometry.
    /// @param grid       Input scalar grid.
    /// @param isoValue   Iso-value at which to extract the surface (default 0 for SDFs).
    /// @param adaptivity Adaptivity parameter passed to VolumeToMesh (0 = uniform quads).
    Geometry::Ptr volumeToGeometry(const GridT &grid, float isoValue=0.0f, float adaptivity=0.0f) const;

};// Tool class

// ==============================================================================================================

inline std::list<GridBase::Ptr>::const_reverse_iterator Tool::getGrid(size_t age) const
{
    if (age>=mGrid.size()) {
      throw std::invalid_argument("-"+mParser.getAction().names[0]+" called getGrid("+std::to_string(age)+"), but grid count = "+std::to_string(mGrid.size()));
    }
    auto it = mGrid.crbegin();
    std::advance(it, age);
    return it;
}// Tool::getGrid

// ==============================================================================================================

inline std::list<Geometry::Ptr>::const_reverse_iterator Tool::getGeom(size_t age) const
{
    if (age>=mGeom.size()) {
      throw std::invalid_argument("-"+mParser.getAction().names[0]+" called getGeom("+std::to_string(age)+"), but geometry count = "+std::to_string(mGeom.size()));
    }
    auto it = mGeom.crbegin();
    std::advance(it, age);
    return it;
}// Tool::getGeom

// ==============================================================================================================

/// @brief Header record prepended to every vdb_tool config (.txt) file.
/// @details Identifies a config file with the magic string "vdb_tool" followed by
///          the major.minor.patch version that produced it. Used to gate loading
///          configs from incompatible tool versions.
struct Tool::Header {
    /// @brief Construct a header for the current tool version.
    Header() : mMagic("vdb_tool"), mMajor(sMajor), mMinor(sMinor), mPatch(sPatch) {}
    /// @brief Parse a header from the first line of a config file.
    /// @param line First line of the config file, e.g. "vdb_tool 10.8.0".
    /// @throw std::invalid_argument if @a line does not match "vdb_tool MAJOR.MINOR.PATCH".
    Header(const std::string &line) : mMagic("vdb_tool") {
      const VecS header = tokenize(line, " .");
      if (header.size()!=4 || header[0]!=mMagic ||
         !isInt(header[1], mMajor) ||
         !isInt(header[2], mMinor) ||
         !isInt(header[3], mPatch)) throw std::invalid_argument("Header: incompatible: \""+line+"\"");
    }
    /// @brief Format the header as the string written at the top of a config file.
    std::string str() const {
      return mMagic+" "+std::to_string(mMajor)+"."+std::to_string(mMinor)+"."+std::to_string(mPatch);
    }
    /// @brief Returns true if this header's major version matches the running tool.
    bool isCompatible() const {return mMajor == sMajor;}

    std::string mMagic; ///< Magic identifier; always "vdb_tool" for a valid header.
    int mMajor;         ///< Major version recorded in (or expected by) the config file.
    int mMinor;         ///< Minor version recorded in (or expected by) the config file.
    int mPatch;         ///< Patch version recorded in (or expected by) the config file.
};// Header struct

// ==============================================================================================================

/// @brief Lightweight adapter exposing a std::vector<Vec3s> as the point-source interface
///        expected by tools::ParticlesToLevelSet.
/// @details ParticlesToLevelSet requires the source to define a PosType alias and to provide
///          size() and getPos() member functions. This wrapper supplies them over a borrowed
///          vector of vertices without copying the data.
struct Tool::Points {
    using PosType = Vec3R; ///< Position type required by ParticlesToLevelSet (double precision).

    /// @brief Construct a Points adapter over an existing vertex array (stored by reference).
    Points(const std::vector<Vec3s> &vtx) : mPoints(vtx) {}
    /// @brief Number of points exposed by this adapter.
    size_t size() const { return mPoints.size(); }
    /// @brief Write the n'th point into @a p, converting from Vec3s to PosType (Vec3R).
    void getPos(size_t n, PosType &p) const { p = mPoints[n]; }

    const std::vector<Vec3s> &mPoints; ///< Borrowed reference to the underlying vertex array.
};// Points struct

} // namespace vdb_tool
} // namespace OPENVDB_VERSION_NAME
} // namespace openvdb

#endif// VDB_TOOL_TOOL_HAS_BEEN_INCLUDED
