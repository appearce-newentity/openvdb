// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/// @file ToolMesh.cc
/// @brief Mesh <-> volume conversion, shrink-wrap, mesh offsetting. Split out of Tool.h; see Tool.h for the class.

#include "Tool.h"

namespace openvdb {
OPENVDB_USE_VERSION_NAMESPACE
namespace OPENVDB_VERSION_NAME {
namespace vdb_tool {

// ==============================================================================================================

float Tool::estimateVoxelSize(int maxDim,  float exWidth, float inWidth, int geo_age)
{
  auto it = this->getGeom(geo_age);
  const auto bbox = (*it)->bbox();
  if (!bbox) {
    throw std::invalid_argument("estimateVoxelSize: invalid bbox");
  } else if (maxDim <= 0) {
    throw std::invalid_argument("estimateVoxelSize: invalid maxDim");
  }
  const auto d = bbox.extents()[bbox.maxExtent()];// longest extent of bbox along any coordinate axis
  return static_cast<float>(static_cast<double>(d)/static_cast<double>(maxDim - static_cast<int>(exWidth + inWidth)));
}// Tool::estimateVoxelSize

// ==============================================================================================================

void Tool::quadsToTriangles()
{
  OPENVDB_ASSERT(mParser.getAction().names[0] == "quad2tri");
  mParser.printAction();
  const int geo_age = mParser.get<int>("geo");
  const bool keep = mParser.get<bool>("keep");
  Geometry::Ptr mesh = *this->getGeom(geo_age);
  if (mesh->isPoints()) throw std::invalid_argument("called on points, i.e. no quads!");
  if (keep) {
    Geometry::Ptr meshCopy = mesh->deepCopy();
    mGeom.push_back(meshCopy);
    mesh = meshCopy;
  }
  if (mParser.verbose) mTimer.start("Quads -> Triangles");
  mesh->triangulateQuads();
  if (mParser.verbose) mTimer.stop();
}// Tool::quadsToTriangles

// ==============================================================================================================

void Tool::meshToLevelSet()
{
  OPENVDB_ASSERT(mParser.getAction().names[0] == "mesh2ls");
  mParser.printAction();
  const int dim = mParser.get<int>("dim");
  float voxel = mParser.get<float>("voxel");
  const float width = mParser.get<float>("width");
  const float exWidth = mParser.get<float>("exWidth");
  const float inWidth = mParser.get<float>("inWidth");
  const int geo_age = mParser.get<int>("geo");
  const int vdb_age = mParser.get<int>("vdb");
  const bool keep = mParser.get<bool>("keep");
  std::string grid_name = mParser.get<std::string>("name");

  math::Transform::Ptr xform(nullptr);
  if (vdb_age>=0) {// use xform from reference VDB
    auto it = this->getGrid(vdb_age);
    xform = (*it)->transform().copy();
  } else if (exWidth <= 0.0 || inWidth <= 0.0) {
    if (voxel == 0.0f) voxel = this->estimateVoxelSize(dim, width, geo_age);
    xform = math::Transform::createLinearTransform(voxel);
  } else {
    if (voxel == 0.0f) voxel = this->estimateVoxelSize(dim, exWidth, inWidth, geo_age);
    xform = math::Transform::createLinearTransform(voxel);
  }
  auto it = this->getGeom(geo_age);
  const Geometry &mesh = **it;
  if (mesh.isPoints()) throw std::invalid_argument("Warning: -mesh2ls/mesh2sdf was called on points, not a mesh! Hint: use -points2ls instead!");
  if (exWidth <= 0.0 || inWidth <= 0.0) {// symmetric narrow-band
      if (mParser.verbose) mTimer.start("Mesh -> LS");
      auto grid  = tools::meshToLevelSet<GridT>(*xform, mesh.vtx(), mesh.tri(), mesh.quad(), width);
      if (grid_name.empty()) grid_name = "mesh2ls_" + mesh.getName();
      grid->setName(grid_name);
      mGrid.push_back(grid);
  } else {// asymmetric narrow-band
      if (mParser.verbose) mTimer.start("Mesh -> SDF");
      auto grid  = tools::meshToSignedDistanceField<GridT>(*xform, mesh.vtx(), mesh.tri(), mesh.quad(), exWidth, inWidth);
      if (grid_name.empty()) grid_name = "mesh2sdf_" + mesh.getName();
      grid->setName(grid_name);
      mGrid.push_back(grid);
  }
  if (!keep) mGeom.erase(std::next(it).base());
  if (mParser.verbose) mTimer.stop();
}// Tool::meshToLevelSet

// ==============================================================================================================

void Tool::meshToUnsignedDistanceField()
{
  OPENVDB_ASSERT(mParser.getAction().names[0] == "mesh2udf");
  if (mParser.getAction().matchedName == "soup2udf") {
    std::cerr << "Warning: action \"-soup2udf\" is deprecated; use \"-mesh2udf\" instead.\n";
  }
  mParser.printAction();
  const int dim = mParser.get<int>("dim");
  float voxel = mParser.get<float>("voxel");
  const float width = mParser.get<float>("width");
  const int geo_age = mParser.get<int>("geo");
  const int vdb_age = mParser.get<int>("vdb");
  const bool keep = mParser.get<bool>("keep");
  std::string grid_name = mParser.get<std::string>("name");

  math::Transform::Ptr xform(nullptr);
  if (vdb_age>=0) {// use xform from reference VDB
    auto it = this->getGrid(vdb_age);
    xform = (*it)->transform().copy();
  } else {
    if (voxel == 0.0f) voxel = this->estimateVoxelSize(dim, width, geo_age);
    xform = math::Transform::createLinearTransform(voxel);
  }
  auto it = this->getGeom(geo_age);
  const Geometry &mesh = **it;
  if (mesh.isPoints()) throw std::invalid_argument("only points, expected mesh! Hint: use -points2ls instead!");
  if (mParser.verbose) mTimer.start("Mesh -> UDF");
  auto grid = tools::meshToUnsignedDistanceField<GridT>(*xform, mesh.vtx(), mesh.tri(), mesh.quad(), width);
  if (grid_name.empty()) grid_name = "mesh2udf_" + mesh.getName();
  grid->setName(grid_name);
  mGrid.push_back(grid);
  if (!keep) mGeom.erase(std::next(it).base());
  if (mParser.verbose) mTimer.stop();
}// Tool::meshToUnsignedDistanceField

// ==============================================================================================================

void Tool::shrinkWrap()
{
  OPENVDB_ASSERT(mParser.getAction().names[0] == "shrinkwrap");
  const std::string &matched = mParser.getAction().matchedName;
  if (matched == "soup2ls" || matched == "soup2sdf") {
    std::cerr << "Warning: action \"-" << matched << "\" is deprecated; use \"-shrinkwrap\" instead.\n";
  }
  mParser.printAction();
  const int dim = mParser.get<int>("dim");// final dimension
  float voxel = mParser.get<float>("voxel");// final voxel size
  const float width = mParser.get<float>("width");
  const int offset_mode = mParser.get<int>("mode");
  const int geo_age = mParser.get<int>("geo");
  const float erode = mParser.get<float>("erode");
  const float thres = mParser.get<float>("thres");
  const bool keep = mParser.get<bool>("keep");
  std::string grid_name = mParser.get<std::string>("name");
  // Output selector (NB: selects outputs, not inputs): "*" for all generated
  // grids, otherwise a list of resolution levels (0 = finest). Defaults to "0".
  const std::string level_sel = mParser.get<std::string>("levels");

  auto it = this->getGeom(geo_age);
  Geometry::Ptr mesh = *it;
  if (mesh->isPoints()) {
    if (!keep) mGeom.erase(std::next(it).base());
    throw std::invalid_argument("got points, expected mesh! Hint: use -points2ls instead!");
  }
  if (keep) mesh = mesh->deepCopy();// deep copy since mesh will be modified below
  if (mParser.verbose) mTimer.start("ShrinkWrap -> SDF");

  Spinner spin, *progress = mParser.verbose ? &spin : nullptr;
  const ShrinkWrapLimit D(erode, thres);
  PolySoup poly{std::move(mesh->vtx()), std::move(mesh->tri()), std::move(mesh->quad()), mesh->bbox()};

  // Build the LOD hierarchy directly (rather than the single-grid free function)
  // so we can output more than just the finest level. grids() is ordered
  // finest(0) -> coarsest(size-1), matching the "levels" indices below.
  using SW = ShrinkWrap<GridT>;
  auto sw = voxel > 0.0f ? std::make_unique<SW>(std::move(poly), voxel, width)
                         : std::make_unique<SW>(std::move(poly), dim, width);
  sw->process(D, progress, offset_mode);

  if (mParser.verbose) mTimer.stop();

  const std::vector<GridT::Ptr> grids = sw->grids();// finest(0) -> coarsest
  const int count = static_cast<int>(grids.size());

  // Resolve the selector into a list of level indices. "*" -> every level.
  std::vector<int> levels;
  if (level_sel == "*") {
    for (int i = 0; i < count; ++i) levels.push_back(i);
  } else {
    levels = mParser.getVec<int>("levels");
    if (levels.empty()) levels.push_back(0);
  }

  // Validate every requested level BEFORE pushing any grid, so an out-of-range
  // index produces a clean error with no partial output on the stack.
  for (const int lvl : levels) {
    if (lvl < 0 || lvl >= count) {
      throw std::invalid_argument("shrinkwrap: requested output grid levels=" + std::to_string(lvl) +
          " does not exist; the shrink-wrap hierarchy generated " + std::to_string(count) +
          " grid(s), so valid levels are 0 (finest) .. " + std::to_string(count - 1) + " (coarsest)");
    }
  }

  if (grid_name.empty()) grid_name = "shrinkwrap_" + mesh->getName();
  const bool multi = levels.size() > 1;
  for (const int lvl : levels) {
    GridT::Ptr g = grids[lvl];
    // Disambiguate names only when several grids are output; a single output
    // keeps the plain name (preserving the historical single-grid behavior).
    g->setName(multi ? grid_name + "_" + std::to_string(lvl) : grid_name);
    mGrid.push_back(g);
  }
  if (!keep) mGeom.erase(std::next(it).base());
}// Tool::shrinkWrap

// ==============================================================================================================

void Tool::meshToOffset()
{
  OPENVDB_ASSERT(mParser.getAction().names[0] == "mesh2offset");
  if (mParser.getAction().matchedName == "soup2offset") {
    std::cerr << "Warning: action \"-soup2offset\" is deprecated; use \"-mesh2offset\" instead.\n";
  }
  mParser.printAction();
  const int dim = mParser.get<int>("dim");// final dimension
  float voxel = mParser.get<float>("voxel");// final voxel size
  const float width = mParser.get<float>("width");
  //const float offset = mParser.get<float>("offset");
  const int offset_mode = mParser.get<int>("mode");
  const int geo_age = mParser.get<int>("geo");
  const bool keep = mParser.get<bool>("keep");
  std::string grid_name = mParser.get<std::string>("name");

  auto it = this->getGeom(geo_age);
  if (voxel == 0.0f) voxel = this->estimateVoxelSize(dim, width, geo_age);
  Geometry::Ptr mesh = *it;
  if (mesh->isPoints()) {
    if (!keep) mGeom.erase(std::next(it).base());
    throw std::invalid_argument("got points, expected mesh! Hint: use -points2ls instead!");
  }
  if (keep) mesh = mesh->deepCopy();// deep copy since mesh will be modified below
  if (mParser.verbose) mTimer.start("Mesh -> Offset");

  PolySoup poly{std::move(mesh->vtx()), std::move(mesh->tri()), std::move(mesh->quad()), mesh->bbox()};
  ShrinkWrap<GridT> tmp(std::move(poly), voxel, width);
  auto grid = tmp.offset(voxel, offset_mode);

  if (mParser.verbose) mTimer.stop();

  if (grid_name.empty()) grid_name = "mesh2offset_" + mesh->getName();
  grid->setName(grid_name);
  mGrid.push_back(grid);
  if (!keep) mGeom.erase(std::next(it).base());
}// Tool::meshToOffset

// ==============================================================================================================

void Tool::volumeToMesh()
{
  const int mode = findMatch(mParser.getAction().names[0], {"ls2mesh", "fog2mesh", "vol2mesh"});// 1-based index
  OPENVDB_ASSERT(mode);// mode = 0 for no match
  mParser.printAction();
  const std::string &action_name = mParser.getAction().names[0];
  const double adaptivity = mParser.get<float>("adapt");
  const double iso = mParser.get<float>("iso");
  const int age = mParser.get<int>("vdb");
  const int mask = mParser.get<int>("mask");
  const bool invert = mParser.get<bool>("invert");
  const bool keep = mParser.get<bool>("keep");
  std::string grid_name = mParser.get<std::string>("name");

  auto it = this->getGrid(age);// will throw if grid doesn't exist
  GridT::Ptr grid = gridPtrCast<GridT>(*it);
  if (!grid) throw std::invalid_argument("no FloatGrid with age " + std::to_string(age));
  if (mode==1 && grid->getGridClass() != GRID_LEVEL_SET) {
    throw std::invalid_argument("no level set with age "+std::to_string(age));
  } else if (mode==2 && grid->getGridClass() != GRID_FOG_VOLUME) {
    throw std::invalid_argument("no fog volume with age "+std::to_string(age));
  }

  if (mParser.verbose) mTimer.start(action_name);

  tools::VolumeToMesh mesher(iso, adaptivity, /*relaxDisorientedTriangles*/true);
  if (mask >= 0) {
    auto base = *this->getGrid(mask);// might throw
    if (base->isType<BoolGrid>()) {
      mesher.setSurfaceMask(base, invert);
    } else if (base->isType<FloatGrid>()) {
      mesher.setSurfaceMask(tools::interiorMask(*gridPtrCast<FloatGrid>(base), 0.0), invert);
    } else if (base->isType<Vec3fGrid>()) {
      mesher.setSurfaceMask(tools::interiorMask(*gridPtrCast<Vec3fGrid>(base)), invert);
    } else {
      throw std::invalid_argument("unsupported mask type with age "+std::to_string(mask));
    }
  }
  mesher(*grid);
  Geometry::Ptr geom = this->mesherToGeometry(mesher);

  if (!keep) mGrid.erase(std::next(it).base());
  if (grid_name.empty()) grid_name = action_name + "_" + grid->getName();
  geom->setName(grid_name);
  mGeom.push_back(geom);

  if (mParser.verbose) mTimer.stop();
}// Tool::volumeToMesh

// ==============================================================================================================

Geometry::Ptr Tool::mesherToGeometry(tools::VolumeToMesh &mesher) const
{
  Geometry::Ptr geom(new Geometry());

  {// allocate and copy vertices
    auto &vtx = geom->vtx();
    vtx.resize(mesher.pointListSize());
    tools::volume_to_mesh_internal::PointListCopy ptnCpy(mesher.pointList(), vtx);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, vtx.size()), ptnCpy);
    mesher.pointList().reset(nullptr);
  }

  {// allocate and copy polygons
    auto& polygonPoolList = mesher.polygonPoolList();
    size_t numQuad = 0, numTri = 0;
    for (size_t i = 0, N = mesher.polygonPoolListSize(); i < N; ++i) {
      auto &polygons = polygonPoolList[i];
      numTri  += polygons.numTriangles();
      numQuad += polygons.numQuads();
    }
    auto &tri  = geom->tri();
    auto &quad = geom->quad();
    tri.resize(numTri);
    quad.resize(numQuad);
    size_t qIdx = 0, tIdx = 0;
    for (size_t n = 0, N = mesher.polygonPoolListSize(); n < N; ++n) {
      auto &poly = polygonPoolList[n];
      for (size_t i = 0, I = poly.numQuads(); i < I; ++i) quad[qIdx++] = poly.quad(i);
      for (size_t i = 0, I = poly.numTriangles(); i < I; ++i) tri[tIdx++] = poly.triangle(i);
    }
  }
  return geom;
}// Tool::mesherToGeometry

// ==============================================================================================================

Geometry::Ptr Tool::volumeToGeometry(const GridT &grid, float isoValue, float adaptivity) const
{
  tools::VolumeToMesh mesher(isoValue, adaptivity, /*relaxDisorientedTriangles*/true);
  mesher(grid);
  return this->mesherToGeometry(mesher);
}

} // namespace vdb_tool
} // namespace OPENVDB_VERSION_NAME
} // namespace openvdb
