// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/// @file ToolLevelSet.cc
/// @brief Level-set operations: filtering, CSG, composite, primitives, sdf2udf, enright. Split out of Tool.h; see Tool.h for the class.

#include "Tool.h"

namespace openvdb {
OPENVDB_USE_VERSION_NAMESPACE
namespace OPENVDB_VERSION_NAME {
namespace vdb_tool {

// ==============================================================================================================

void Tool::levelSetToFog()
{
  OPENVDB_ASSERT(mParser.getAction().names[0] == "ls2fog");
  mParser.printAction();
  const int age = mParser.get<int>("vdb");
  const bool keep = mParser.get<bool>("keep");
  const float cutoff = mParser.get<float>("cutoff");
  std::string grid_name = mParser.get<std::string>("name");
  auto it = this->getGrid(age);
  auto sdf = gridPtrCast<FloatGrid>(*it);
  if (!sdf || sdf->getGridClass() != GRID_LEVEL_SET) {
    throw std::invalid_argument("no Level Set with age " + std::to_string(age));
  }
  if (mParser.verbose) mTimer.start("SDF to FOG");
  FloatGrid::Ptr fog = keep ? sdf->deepCopy() : sdf;
  const float cutoffDistance = cutoff <= 0.0f ? sdf->background() : cutoff * float(sdf->voxelSize()[0]);
  tools::sdfToFogVolume(*fog, cutoffDistance);// fog <- sdf > 0 ? 0 : -sdf / |cutoffDistance|
  if (!keep) mGrid.erase(std::next(it).base());
  if (grid_name.empty()) grid_name = "ls2fog_"+sdf->getName();
  fog->setName(grid_name);
  mGrid.push_back(fog);
  if (mParser.verbose) mTimer.stop();
}// Tool::levelSetToFog

// ==============================================================================================================

void Tool::isoToLevelSet()
{
  OPENVDB_ASSERT(mParser.getAction().names[0] == "iso2ls");
  mParser.printAction();
  const VecI age = mParser.getVec<int>("vdb");
  if (age.size()!=1 && age.size()!=2) throw std::invalid_argument("iso2ls: expected one or two vdb grids, not "+std::to_string(age.size()));
  const float isoValue = mParser.get<float>("iso");
  const float voxel = mParser.get<float>("voxel");
  const float width = mParser.get<float>("width");
  const bool keep = mParser.get<bool>("keep");
  std::string grid_name = mParser.get<std::string>("name");
  auto it = this->getGrid(age[0]);
  auto grid = gridPtrCast<FloatGrid>(*it);
  if (!grid) throw std::invalid_argument("no VDB with age " + std::to_string(age[0]));
  if (mParser.verbose) mTimer.start("Iso to SDF");
  math::Transform::Ptr xform(nullptr);
  if (age.size()==2) {
    auto it = this->getGrid(age[1]);
    xform = (*it)->transform().copy();
  } else if (voxel>0.0f) {
    xform = math::Transform::createLinearTransform(voxel);
  }
  auto sdf = tools::levelSetRebuild(*grid, isoValue, width, xform.get());
  if (!keep) mGrid.erase(std::next(it).base());
  if (grid_name.empty()) grid_name = "iso2ls_"+grid->getName();
  sdf->setName(grid_name);
  mGrid.push_back(sdf);
  if (mParser.verbose) mTimer.stop();
}// Tool::isoToLevelSet

// ==============================================================================================================

typename Tool::FilterT Tool::createFilter(GridT &grid, int space, int time)
{
  auto filter = std::make_unique<tools::LevelSetFilter<GridT>>(grid);

  switch (space) {
  case 1:
    filter->setSpatialScheme(math::FIRST_BIAS);
    break;
  case 2:
    filter->setSpatialScheme(math::SECOND_BIAS);
    break;
  case 3:
    filter->setSpatialScheme(math::THIRD_BIAS);
    break;
  case 5:
#if 0
    filter->setSpatialScheme(math::WENO5_BIAS);
#else
    filter->setSpatialScheme(math::HJWENO5_BIAS);
#endif
    break;
  default:
    throw std::invalid_argument("createFilter: invalid space discretization scheme \""+std::to_string(space)+"\"");
  }

  switch (time) {
  case 1:
    filter->setTemporalScheme(math::TVD_RK1);
    break;
  case 2:
    filter->setTemporalScheme(math::TVD_RK2);
    break;
  case 3:
    filter->setTemporalScheme(math::TVD_RK3);
    break;
  default:
    throw std::invalid_argument("createFilter: invalid time discretization scheme \""+std::to_string(time)+"\"");
  }
  return filter;
}// Tool::createFilter

// ==============================================================================================================

void Tool::offsetLevelSet()
{
  OPENVDB_ASSERT(findMatch(mParser.getAction().names[0], {"dilate", "erode", "open", "close"}));
  mParser.printAction();
  const std::string &action_name = mParser.getAction().names[0];
  float radius = mParser.get<float>("radius");
  const int space = mParser.get<int>("space");
  const int time = mParser.get<int>("time");
  const int age = mParser.get<int>("vdb");
  if (radius<0) throw std::invalid_argument("offsetLevelSet: invalid radius");
  if (radius==0) return;
  auto it = this->getGrid(age);
  GridT::Ptr grid = gridPtrCast<GridT>(*it);
  if (!grid || grid->getGridClass() != GRID_LEVEL_SET) {
    throw std::invalid_argument("no level set with age " + std::to_string(age));
  }
  auto filter = this->createFilter(*grid, space, time);
  radius *= static_cast<float>((*it)->voxelSize()[0]);// voxel to world units
  if (action_name == "dilate") {
    if (mParser.verbose) mTimer.start("Dilate  SDF");
    filter->offset(-radius);
  } else if (action_name == "erode") {
    if (mParser.verbose) mTimer.start("Erode   SDF");
    filter->offset( radius);
  } else if (action_name == "open") {
    if (mParser.verbose) mTimer.start("Open   SDF");
    filter->offset( radius);
    filter->offset(-radius);
  } else if (action_name == "close") {
    if (mParser.verbose) mTimer.start("Close   SDF");
    filter->offset(-radius);
    filter->offset( radius);
  } else {
    throw std::invalid_argument("offsetLevelSet: invalid operation type");
  }
  grid->setName(action_name + "_" + grid->getName());
  if (mParser.verbose) mTimer.stop();
}// Tool::offsetLevelSet

// ==============================================================================================================

void Tool::filterLevelSet()
{
  OPENVDB_ASSERT(findMatch(mParser.getAction().names[0], {"gauss", "mean", "median"}));
  mParser.printAction();
  const std::string &action_name = mParser.getAction().names[0];
  const int nIter = mParser.get<int>("iter");
  const int space = mParser.get<int>("space");
  const int time = mParser.get<int>("time");
  const int age = mParser.get<int>("vdb");
  const int size = mParser.get<int>("size");
  if (size<0) throw std::invalid_argument("filterLevelSet: invalid filter size");
  if (size==0) return;
  auto it = this->getGrid(age);
  GridT::Ptr grid = gridPtrCast<GridT>(*it);
  if (!grid || grid->getGridClass() != GRID_LEVEL_SET) {
    throw std::invalid_argument(action_name + ": no level set with age " + std::to_string(age));
  }
  auto filter = this->createFilter(*grid, space, time);

  if (action_name == "gauss") {
    if (mParser.verbose) mTimer.start("Gauss   SDF");
    for (int i=0; i<nIter; ++i) filter->gaussian(size);
  } else if (action_name == "mean") {
    if (mParser.verbose) mTimer.start("Mean SDF ");
    for (int i=0; i<nIter; ++i) filter->mean(size);
  } else if (action_name == "median") {
    if (mParser.verbose) mTimer.start("Median SDF");
    for (int i=0; i<nIter; ++i) filter->median(size);
  } else {
    throw std::invalid_argument("filterLevelSet: invalid filter type");
  }
  grid->setName(action_name + "_" + grid->getName());
  if (mParser.verbose) mTimer.stop();
}// Tool::filterLevelSet

// ==============================================================================================================

void Tool::pruneLevelSet()
{
  OPENVDB_ASSERT(mParser.getAction().names[0] == "prune");
  mParser.printAction();
  const int age = mParser.get<int>("vdb");
  auto it = this->getGrid(age);
  GridT::Ptr grid = gridPtrCast<GridT>(*it);
  if (!grid || grid->getGridClass() != GRID_LEVEL_SET) {
    throw std::invalid_argument(mParser.getAction().names[0] + ": no level set with age " + std::to_string(age));
  }
  if (mParser.verbose) mTimer.start("Prune   SDF");
  tools::pruneLevelSet(grid->tree());
  grid->setName("prune_"+grid->getName());
  if (mParser.verbose) mTimer.stop();
}// Tool::pruneLevelSet

// ==============================================================================================================

void Tool::floodLevelSet()
{
  OPENVDB_ASSERT(mParser.getAction().names[0] == "flood");
  mParser.printAction();
  const int age = mParser.get<int>("vdb");
  auto it = this->getGrid(age);
  GridT::Ptr grid = gridPtrCast<GridT>(*it);
  if (!grid || grid->getGridClass() != GRID_LEVEL_SET) {
    throw std::invalid_argument("flood: no level set with age " + std::to_string(age));
  }
  if (mParser.verbose) mTimer.start("Flood   SDF");
  tools::signedFloodFill(grid->tree());
  grid->setName("flood_"+grid->getName());
  if (mParser.verbose) mTimer.stop();
}// Tool::floodLevelSet

// ==============================================================================================================

void Tool::composite()
{
  OPENVDB_ASSERT(findMatch(mParser.getAction().names[0], {"min","max","sum","multiply","divide"}));
  mParser.printAction();
  const std::string &action_name = mParser.getAction().names[0];
  const VecI ij = mParser.getVec<int>("vdb");
  const bool keep = mParser.get<bool>("keep");
  if (ij.size()!=2) throw std::invalid_argument(action_name+": expected two vdb ages, but got "+std::to_string(ij.size()));
  if (ij[0] == ij[1]) throw std::invalid_argument(action_name+": identical inputs: volume1=volume2="+std::to_string(ij[0]));
  auto itA = this->getGrid(ij[0]), itB = this->getGrid(ij[1]);
  GridT::Ptr gridA = gridPtrCast<GridT>(*itA);
  if (!gridA) throw std::invalid_argument(action_name + ": no float grid with age " + std::to_string(ij[0]));
  GridT::Ptr gridB = gridPtrCast<GridT>(*itB);
  if (!gridB) throw std::invalid_argument(action_name + ": no float grid with age " + std::to_string(ij[1]));
  if (gridA->transform() != gridB->transform()) throw std::invalid_argument(action_name+": grids have different transforms");
  GridT::Ptr tmpA, tmpB;
  if (keep) {
    tmpA = gridA->deepCopy();
    tmpB = gridB->deepCopy();
    mGrid.push_back(tmpA);
  } else {
    tmpA = gridA;
    tmpB = gridB;
    mGrid.erase(std::next(itB).base());// remove B from mGrids since it will be destroyed
  }
  tmpA->setName(action_name+"_"+tmpA->getName());
  if (mParser.verbose) mTimer.start(action_name);
  if (action_name == "min") {
    tools::compMin(*tmpA, *tmpB);// Store the result in the A grid and leave the B grid empty.
  } else if (action_name == "max") {
    tools::compMax(*tmpA, *tmpB);// Store the result in the A grid and leave the B grid empty.
  } else if (action_name == "sum") {
    tools::compSum(*tmpA, *tmpB);// Store the result in the A grid and leave the B grid empty.
  } else if (action_name == "multiply") {
    tools::compMul(*tmpA, *tmpB);// Store the result in the A grid and leave the B grid empty.
  } else if (action_name == "divide") {
    tools::compDiv(*tmpA, *tmpB);// Store the result in the A grid and leave the B grid empty.
  } else {
    throw std::invalid_argument(action_name+": invalid operation");
  }
  if (mParser.verbose) mTimer.stop();
}// Tool::composite

// ==============================================================================================================

void Tool::csg()
{
  OPENVDB_ASSERT(findMatch(mParser.getAction().names[0], {"union", "intersection", "difference"}));
  mParser.printAction();
  const std::string &action_name = mParser.getAction().names[0];
  const VecI ij = mParser.getVec<int>("vdb");
  const bool keep = mParser.get<bool>("keep");
  const bool prune = mParser.get<bool>("prune");
  const bool rebuild = mParser.get<bool>("rebuild");
  if (ij.size()!=2) throw std::invalid_argument("csg: expected two vdb ages, but got "+std::to_string(ij.size()));
  if (ij[0] == ij[1]) throw std::invalid_argument("csg: identical inputs: volume1=volume2="+std::to_string(ij[0]));
  auto itA = this->getGrid(ij[0]), itB = this->getGrid(ij[1]);
  GridT::Ptr gridA = gridPtrCast<GridT>(*itA);
  if (!gridA || gridA->getGridClass() != GRID_LEVEL_SET) {
    throw std::invalid_argument(action_name + ": no level set with age " + std::to_string(ij[0]));
  }
  GridT::Ptr gridB = gridPtrCast<GridT>(*itB);
  if (!gridB || gridB->getGridClass() != GRID_LEVEL_SET) {
    throw std::invalid_argument(action_name + ": no level set with age " + std::to_string(ij[1]));
  }
  if (gridA->transform() != gridB->transform()) {
    if (gridA->voxelSize()[0]<gridB->voxelSize()[0]) {// use the smallest voxel size
      const float halfWidth = static_cast<float>(gridA->background()/gridA->voxelSize()[0]);
      if (mParser.verbose) mTimer.start("Rebuilding "+std::to_string(ij[1]));
      gridB = tools::levelSetRebuild(*gridB, 0.0f, halfWidth, &(gridA->transform()));
    } else {
      const float halfWidth = static_cast<float>(gridB->background()/gridB->voxelSize()[0]);
      if (mParser.verbose) mTimer.start("Rebuilding "+std::to_string(ij[0]));
      gridA = tools::levelSetRebuild(*gridA, 0.0f, halfWidth, &(gridB->transform()));
    }
    if (mParser.verbose) mTimer.stop();
  }
  if (action_name == "union") {
    if (mParser.verbose) mTimer.start("Union");
    if (keep) {
      GridT::Ptr grid = tools::csgUnionCopy(*gridA, *gridB);
      if (rebuild) grid = tools::sdfToSdf(*grid);
      grid->setName("union_"+gridA->getName());
      mGrid.push_back(grid);// A and B are unchanged!
    } else {
      tools::csgUnion(*gridA, *gridB, prune);// overwrites A and cannibalizes B
      if (rebuild) gridA = tools::sdfToSdf(*gridA);
      gridA->setName("union_"+gridA->getName());
    }
  } else if (action_name == "intersection") {
    if (mParser.verbose) mTimer.start("Intersection");
    if (keep) {
      GridT::Ptr grid = tools::csgIntersectionCopy(*gridA, *gridB);
      if (rebuild) grid = tools::sdfToSdf(*grid);
      grid->setName("intersection_"+gridA->getName());
      mGrid.push_back(grid);// A and B are unchanged!
    } else {
      tools::csgIntersection(*gridA, *gridB, prune);// overwrites A and cannibalizes B
      if (rebuild) gridA = tools::sdfToSdf(*gridA);
      gridA->setName("intersection_"+gridA->getName());
    }
  } else if (action_name == "difference") {
    if (mParser.verbose) mTimer.start("Difference");
    if (keep) {
      GridT::Ptr grid = tools::csgDifferenceCopy(*gridA, *gridB);
      if (rebuild) grid = tools::sdfToSdf(*grid);
      grid->setName("difference_"+gridA->getName());
      mGrid.push_back(grid);// A and B are unchanged!
    } else {
      tools::csgDifference(*gridA, *gridB, prune);// overwrites A and deletes B
      if (rebuild) gridA = tools::sdfToSdf(*gridA);
      gridA->setName("difference_"+gridA->getName());
    }
  } else {
    throw std::invalid_argument("csg: invalid type");
  }
  if (!keep) mGrid.erase(std::next(itB).base());// remove B since it was corrupted
  if (mParser.verbose) mTimer.stop();
}// Tool::csg

// ==============================================================================================================

void Tool::sdf2udf()
{
  const std::string &action_name = mParser.getAction().names[0];
  OPENVDB_ASSERT(findMatch(action_name, {"sdf2udf"}));// mode = 0 for no match
  try {
    const int age = mParser.get<int>("vdb");
    const bool keep = mParser.get<bool>("keep");
    std::string grid_name = mParser.get<std::string>("name");

    /// get the relevant grid to be processed
    auto it = this->getGrid(age);// will throw if grid doesn't exist
    GridT::Ptr grid = gridPtrCast<GridT>(*it);
    if (!grid) throw std::invalid_argument("no FloatGrid with age " + std::to_string(age));
    if (keep) {
      GridT::Ptr tmp = grid->deepCopy();
      mGrid.push_back(tmp);
      grid = tmp;
    }
    if (grid_name.empty()) grid_name = action_name + "_" + grid->getName();
    grid->setName(grid_name);
    if (grid->getGridClass() != GRID_LEVEL_SET) throw std::invalid_argument("no level set with age "+std::to_string(age));
    grid->setGridClass(GRID_UNKNOWN);// GRID_LEVEL_SET -> GRID_UNKNOW

    if (mParser.verbose) mTimer.start(action_name);
    tools::foreach(grid->beginValueAll(), [](auto &it){it.setValue(math::Abs(*it));});
    if (mParser.verbose) mTimer.stop();

  } catch (const std::exception& e) {
    if (mErrorOnWarning) {
      throw std::invalid_argument(action_name+": "+e.what());
    } else {
      std::clog << action_name << ": skipping due to " << e.what() << std::endl;
    }
  }
}// Tool::sdf2udf

// ==============================================================================================================

void Tool::levelSetSphere()
{
  const std::string &action_name = mParser.getAction().names[0];
  OPENVDB_ASSERT(action_name == "sphere");
  mParser.printAction();
  const int dim = mParser.get<int>("dim");
  float voxel = mParser.get<float>("voxel");
  const float radius = mParser.get<float>("radius");
  const Vec3f center = mParser.getVec3<float>("center");
  const float width = mParser.get<float>("width");
  const std::string grid_name = mParser.get<std::string>("name");
  if (voxel == 0.0f) voxel = 2.0f*radius/(static_cast<float>(dim) - 2.0f*width);
  if (mParser.verbose) mTimer.start(action_name);
  GridT::Ptr grid = tools::createLevelSetSphere<GridT>(radius, center, voxel, width);
  if (mParser.verbose) mTimer.stop();
  grid->setName(grid_name);
  mGrid.push_back(grid);
}// Tool::levelSetSphere

// ==============================================================================================================

void Tool::levelSetPlatonic()
{
  OPENVDB_ASSERT(mParser.getAction().names[0] == "platonic");
  mParser.printAction();
  const int dim = mParser.get<int>("dim");
  float voxel = mParser.get<float>("voxel");
  const int faces = mParser.get<int>("faces");
  const float scale = mParser.get<float>("scale");
  const Vec3f center = mParser.getVec3<float>("center");
  const float width = mParser.get<float>("width");
  const std::string grid_name = mParser.get<std::string>("name");
  if (voxel == 0.0f) voxel = 2.0f*scale/(static_cast<float>(dim) - 2*width);
  std::string shape;
  switch (faces) {// TETRAHEDRON=4, CUBE=6, OCTAHEDRON=8, DODECAHEDRON=12, ICOSAHEDRON=20
    case  4: shape = "Tetrahedron"; break;
    case  6: shape = "Cube"; break;
    case  8: shape = "Octahedron"; break;
    case 12: shape = "Dodecahedron"; break;
    case 20: shape = "Icosahedron"; break;
    default: throw std::invalid_argument("levelSetPlatonic: invalid face count: "+std::to_string(faces));
  }
  if (mParser.verbose) mTimer.start("Create "+shape);
  GridT::Ptr grid = tools::createLevelSetPlatonic<GridT>(faces, scale, center, voxel, width);
  if (mParser.verbose) mTimer.stop();
  if (grid_name.empty()) {
    grid->setName(shape);
  } else {
    grid->setName(grid_name);
  }
  mGrid.push_back(grid);
}// Tool::levelSetPlatonic

// ==============================================================================================================

void Tool::expandLevelSet()
{
  const std::string &action_name = mParser.getAction().names[0];
  OPENVDB_ASSERT(action_name == "expand");
  mParser.printAction();
  const int dilate = mParser.get<int>("dilate");
  const int iter = mParser.get<int>("iter");
  const int age = mParser.get<int>("vdb");
  const bool keep = mParser.get<bool>("keep");
  auto it = this->getGrid(age);
  GridT::Ptr sdf = gridPtrCast<GridT>(*it);
  if (!sdf || sdf->getGridClass() != GRID_LEVEL_SET) {
    throw std::invalid_argument(action_name + ": no level set with age " + std::to_string(age));
  }
  if (mParser.verbose) mTimer.start("Expand SDF");
  auto grid = tools::dilateSdf(*sdf, dilate, tools::NN_FACE, iter);
  if (!keep) mGrid.erase(std::next(it).base());
  grid->setName("expand_"+grid->getName());
  mGrid.push_back(grid);
  if (mParser.verbose) mTimer.stop();
}// Tool::expandLevelSet

// ==============================================================================================================
// LeVeque, R., High-Resolution Conservative Algorithms For Advection In Incompressible Flow, SIAM J. Numer. Anal. 33, 627–665 (1996)
// https://faculty.washington.edu/rjl/pubs/hiresadv/0733033.pdf
void Tool::enright()
{
  const std::string &action_name = mParser.getAction().names[0];
  OPENVDB_ASSERT(action_name == "enright");
  mParser.printAction();
  const Vec3d translate = mParser.getVec3<double>("translate");
  const float scale = mParser.get<float>("scale");
  const float dt = mParser.get<float>("dt");
  const int age = mParser.get<int>("vdb");
  const bool keep = mParser.get<bool>("keep");
  math::ScaleTranslateMap::Ptr map(new math::ScaleTranslateMap(Vec3d(scale) ,translate));
  const math::Transform xform(map);
  struct LeVequeField {
    using VectorType = Vec3f;
    const math::Transform xform;
    LeVequeField(const math::Transform &_xform) : xform(_xform) {}
    const math::Transform& transform() const { return xform; }
    Vec3f operator() (const Vec3d& xyz, float time) const {
      static const float pi = math::pi<float>(), phase = pi / 3.0f;
      const Vec3d p = xform.worldToIndex(xyz);
      const float Px =  pi * float(p[0]), Py = pi * float(p[1]), Pz = pi * float(p[2]);
      const float tr =  math::Cos(time * phase);
      const float a  =  math::Sin(2.0f*Py);
      const float b  = -math::Sin(2.0f*Px);
      const float c  =  math::Sin(2.0f*Pz);
      return tr * Vec3f(2.0f * math::Pow2(math::Sin(Px)) * a * c,
                                    b * math::Pow2(math::Sin(Py)) * c,
                                b * a * math::Pow2(math::Sin(Pz)) );
    }
    Vec3f operator() (const Coord& ijk, float time) const {return (*this)(ijk.asVec3d(), time);}
  } field(xform);
  auto it = this->getGrid(age);
  GridT::Ptr grid = gridPtrCast<GridT>(*it);
  if (keep) {
    auto tmp = grid->deepCopy();
    mGrid.push_back(tmp);
    grid = tmp;
  }
  if (!grid || grid->getGridClass() != GRID_LEVEL_SET) {
    throw std::invalid_argument(action_name + ": no level set with age " + std::to_string(age));
  }
  if (mParser.verbose) mTimer.start("Enright SDF");
  tools::LevelSetAdvection<GridT, LeVequeField> advect(*grid, field);
  advect.setSpatialScheme(math::HJWENO5_BIAS);
  advect.setTemporalScheme(math::TVD_RK2);
  advect.setTrackerSpatialScheme(math::HJWENO5_BIAS);
  advect.setTrackerTemporalScheme(math::TVD_RK1);
  advect.advect(0.0f, dt);
  if (mParser.verbose) mTimer.stop();
}// Tool::enright

} // namespace vdb_tool
} // namespace OPENVDB_VERSION_NAME
} // namespace openvdb
