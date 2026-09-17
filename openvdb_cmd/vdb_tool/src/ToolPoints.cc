// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/// @file ToolPoints.cc
/// @brief Point cloud <-> VDB conversion, particle rasterization and scattering. Split out of Tool.h; see Tool.h for the class.

#include "Tool.h"

namespace openvdb {
OPENVDB_USE_VERSION_NAMESPACE
namespace OPENVDB_VERSION_NAME {
namespace vdb_tool {

// ==============================================================================================================

void Tool::vdbToPoints()
{
  OPENVDB_ASSERT(mParser.getAction().names[0] == "vdb2points");
  mParser.printAction();
  const int age = mParser.get<int>("vdb");
  const bool keep = mParser.get<bool>("keep");
  std::string grid_name = mParser.get<std::string>("name");
  auto it = this->getGrid(age);
  auto grid = gridPtrCast<points::PointDataGrid>(*it);
  if (!grid || grid->getGridClass() != GRID_UNKNOWN) {
    throw std::invalid_argument("no PointDataGrid with age " + std::to_string(age));
  }
  if (mParser.verbose) mTimer.start("VDB to points");
  const size_t count = points::pointCount(grid->tree());
  if (count==0) throw std::invalid_argument("empty PointDataGrid with age "+std::to_string(age));
  Geometry::Ptr geom(new Geometry());
  geom->vtx().resize(count);
  Vec3s *points = geom->vtx().data();
  for (auto leafIter = grid->tree().cbeginLeaf(); leafIter; ++leafIter) {
    const points::AttributeArray& array = leafIter->constAttributeArray("P");
    points::AttributeHandle<Vec3f> positionHandle(array);
    for (auto indexIter = leafIter->beginIndexOn(); indexIter; ++indexIter) {
      Vec3f voxelPosition = positionHandle.get(*indexIter);
      const Vec3d xyz = indexIter.getCoord().asVec3d();
      Vec3f worldPosition = grid->transform().indexToWorld(voxelPosition + xyz);
      *points++ = worldPosition;
    }// loop over points in leaf node
  }// loop over leaf nodes
  if (!keep) mGrid.erase(std::next(it).base());
  if (geom->isPoints()) {
    if (grid_name.empty()) grid_name = "vdb2points_"+grid->getName();
    geom->setName(grid_name);
    mGeom.push_back(geom);
  }
  if (mParser.verbose) mTimer.stop();
}// Tool::vdbToPoints

// ==============================================================================================================

void Tool::pointsToVdb()
{
  const std::string &name = mParser.getAction().names[0];
  OPENVDB_ASSERT(name == "points2vdb");
  try {
    mParser.printAction();
    const int age = mParser.get<int>("geo");
    const bool keep = mParser.get<bool>("keep");
    const int pointsPerVoxel = mParser.get<int>("ppv");
    const int bits = mParser.get<int>("bits");
    std::string grid_name = mParser.get<std::string>("name");
    using GridT = points::PointDataGrid;
    using IdGridT = tools::PointIndexGrid;
    if (mParser.verbose) mTimer.start("Points to VDB");
    auto it = this->getGeom(age);
    Points points((*it)->vtx());
    const float voxelSize = points::computeVoxelSize(points, pointsPerVoxel);
    auto xform = math::Transform::createLinearTransform(voxelSize);

    GridT::Ptr grid;
    IdGridT::Ptr indexGrid;

    points::PointAttributeVector<openvdb::Vec3s> positionsWrapper((*it)->vtx());
    openvdb::NamePair rgbAttribute ;
    switch (bits) {
    case 8:
      indexGrid = tools::createPointIndexGrid<tools::PointIndexGrid>(positionsWrapper, *xform);
      grid = points::createPointDataGrid<points::FixedPointCodec</*1-byte=*/true>, GridT>(*indexGrid, positionsWrapper, *xform);
      openvdb::points::TypedAttributeArray<Vec3s, points::NullCodec>::registerType();
      rgbAttribute =
        openvdb::points::TypedAttributeArray<Vec3s, points::NullCodec>::attributeType();
      openvdb::points::appendAttribute(grid->tree(), "Cd", rgbAttribute);
      break;
    case 16:
      indexGrid = tools::createPointIndexGrid<tools::PointIndexGrid>(positionsWrapper, *xform);
      grid = points::createPointDataGrid<points::FixedPointCodec</*1-byte=*/false>, GridT>(*indexGrid, positionsWrapper, *xform);
      openvdb::points::TypedAttributeArray<Vec3s, points::NullCodec>::registerType();
      rgbAttribute =
        openvdb::points::TypedAttributeArray<Vec3s, points::NullCodec>::attributeType();
      openvdb::points::appendAttribute(grid->tree(), "Cd", rgbAttribute);
      break;
    case 32:
      indexGrid = tools::createPointIndexGrid<tools::PointIndexGrid>(positionsWrapper, *xform);
      grid = points::createPointDataGrid<points::NullCodec, GridT>(*indexGrid, positionsWrapper, *xform);

      openvdb::points::TypedAttributeArray<Vec3s, points::NullCodec>::registerType();
      rgbAttribute =
        openvdb::points::TypedAttributeArray<Vec3s, points::NullCodec>::attributeType();
      openvdb::points::appendAttribute(grid->tree(), "Cd", rgbAttribute);
      break;
    default:
      throw std::invalid_argument("pointsToVdb: unsupported bit-width: "+std::to_string(bits));
    }

    if ((*it)->rgb().size() == (*it)->vtx().size()) {

      points::PointAttributeVector<Vec3s> rgbWrapper((*it)->rgb());
      points::populateAttribute<openvdb::points::PointDataTree,
        openvdb::tools::PointIndexTree, openvdb::points::PointAttributeVector<Vec3s>>(
            grid->tree(), indexGrid->tree(), "Cd", rgbWrapper);

    }
    if (grid_name.empty()) grid_name = "points2vdb_"+(*it)->getName();
    grid->setName(grid_name);
    mGrid.push_back(grid);
    if (!keep) mGeom.erase(std::next(it).base());
    if (mParser.verbose) mTimer.stop();
  } catch (const std::exception& e) {
    throw std::invalid_argument(name+": "+e.what());
  }
}// Tool::pointsToVdb

// ==============================================================================================================

void Tool::particlesToLevelSet()
{
  OPENVDB_ASSERT(mParser.getAction().names[0] == "points2ls");
  mParser.printAction();
  const int dim = mParser.get<int>("dim");
  float voxel = mParser.get<float>("voxel");
  const float width = mParser.get<float>("width");
  const float radius = mParser.get<float>("radius");
  const int age = mParser.get<int>("geo");
  const bool keep = mParser.get<bool>("keep");
  std::string grid_name = mParser.get<std::string>("name");
  if (voxel == 0.0f) voxel = this->estimateVoxelSize(dim, width, age);
  auto it = this->getGeom(age);
  const Geometry &points = **it;
  if (points.isMesh()) throw std::invalid_argument("got mesh, expected points! Hint: use -mesh2ls instead!");
  if (mParser.verbose) mTimer.start("Points->SDF");
  GridT::Ptr grid = createLevelSet<GridT>(voxel, width);
  if (grid_name.empty()) grid_name = "points2ls_"+points.getName();
  grid->setName(grid_name);
  tools::particlesToSdf(Points(points.vtx()), *grid, voxel*radius);
  mGrid.push_back(grid);
  if (!keep) mGeom.erase(std::next(it).base());
  if (mParser.verbose) mTimer.stop();
}// Tool::particlesToLevelSet

// ==============================================================================================================

void Tool::scatter()
{
  const std::string &action_name = mParser.getAction().names[0];
  OPENVDB_ASSERT(action_name == "scatter");
  mParser.printAction();
  const Index64 count = mParser.get<int>("count");
  const float density = mParser.get<float>("density");
  const int pointsPerVoxel = mParser.get<int>("ppv");
  const int age = mParser.get<int>("vdb");
  const bool keep = mParser.get<bool>("keep");
  std::string grid_name = mParser.get<std::string>("name");
  auto it = this->getGrid(age);
  GridT::Ptr grid = gridPtrCast<GridT>(*it);
  if (!grid) throw std::invalid_argument(action_name + ": no VDB with age " + std::to_string(age));
  if (mParser.verbose) mTimer.start("SDF -> mesh");
  Geometry::Ptr geom(new Geometry());
  struct PointWrapper {
    std::vector<Vec3f> &xyz;
    PointWrapper(std::vector<Vec3f> &_xyz) : xyz(_xyz) {}
    Index64 size() const { return Index64(xyz.size()); }
    void add(const Vec3d &p) { xyz.emplace_back(float(p[0]), float(p[1]), float(p[2])); }
  } points(geom->vtx());
  using RandGenT = std::mersenne_twister_engine<uint32_t, 32, 351, 175, 19,
        0xccab8ee7, 11, 0xffffffff, 7, 0x31b6ab00, 15, 0xffe50000, 17, 1812433253>; // mt11213b
  RandGenT mtRand;

  if (count>0) {// fixed point count scattering
    tools::UniformPointScatter<PointWrapper, RandGenT> tmp(points, count, mtRand);
    tmp(*grid);
  } else if (density>0.0f) {// uniform density scattering
    tools::UniformPointScatter<PointWrapper, RandGenT> tmp(points, density, mtRand);
    tmp(*grid);
  } else if (pointsPerVoxel>0) {// dense uniform scattering
    tools::DenseUniformPointScatter<PointWrapper, RandGenT> tmp(points, static_cast<float>(pointsPerVoxel), mtRand);
    tmp(*grid);
  } else {
    throw std::invalid_argument("scatter: internal error");
  }
  if (!keep) mGrid.erase(std::next(it).base());
  if (grid_name.empty()) grid_name = "scatter_"+grid->getName();
  geom->setName(grid_name);
  mGeom.push_back(geom);
  if (mParser.verbose) mTimer.stop();
}// Tool::scatter

} // namespace vdb_tool
} // namespace OPENVDB_VERSION_NAME
} // namespace openvdb
