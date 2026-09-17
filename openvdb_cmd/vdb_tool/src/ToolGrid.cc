// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/// @file ToolGrid.cc
/// @brief Generic grid operations: transform, resample, multires, segment, clip, compute, forValues, ax. Split out of Tool.h; see Tool.h for the class.

#include "Tool.h"

namespace openvdb {
OPENVDB_USE_VERSION_NAMESPACE
namespace OPENVDB_VERSION_NAME {
namespace vdb_tool {

// ==============================================================================================================

void Tool::transform()
{
  OPENVDB_ASSERT(mParser.getAction().names[0] == "transform");
  mParser.printAction();
  const auto vdb_age = mParser.getVec<int>("vdb");
  const auto geo_age = mParser.getVec<int>("geo");
  const bool keep = mParser.get<bool>("keep");
  const Vec3d trans = mParser.getVec3<double>("translate");
  const Vec3d rot = mParser.getVec3<double>("rotate");
  const double scale = mParser.get<double>("scale");
  if (scale<=0.0) throw std::invalid_argument("transform: invalid scale: "+std::to_string(scale));

  for (int age : vdb_age) {
    auto it = this->getGrid(age);
    GridBase::Ptr grid(nullptr);
    if (keep) {
      grid = (*it)->copyGrid();// transform and tree are shared
      if (!grid->getName().empty()) grid->setName("xform_"+grid->getName());
      grid->setTransform((*it)->transform().copy());// new transform
      mGrid.push_back(grid);
    } else {
      grid = *it;
    }
    // Order of translations: scale -> rotate -> translate
    if (scale!=1.0)  grid->transform().postScale(scale);
    if (rot[0]!=0.0) grid->transform().postRotate(rot[0], math::X_AXIS);
    if (rot[1]!=0.0) grid->transform().postRotate(rot[1], math::Y_AXIS);
    if (rot[2]!=0.0) grid->transform().postRotate(rot[2], math::Z_AXIS);
    if (trans.length()>0.0) grid->transform().postTranslate(trans);
  }
  if (geo_age.empty()) return;

  // Order of translations: scale -> rotate -> translate
  math::Transform::Ptr xform = math::Transform::createLinearTransform(scale);
  if (rot[0]!=0.0) xform->postRotate(rot[0], math::X_AXIS);
  if (rot[1]!=0.0) xform->postRotate(rot[1], math::Y_AXIS);
  if (rot[2]!=0.0) xform->postRotate(rot[2], math::Z_AXIS);
  if (trans.length()>0.0) xform->postTranslate(trans);
  for (int age : geo_age) {
    auto it = this->getGeom(age);
    Geometry::Ptr geom(nullptr);
    if (keep) {
      geom = (*it)->deepCopy();
      if (!geom->getName().empty()) geom->setName("xform_"+geom->getName());
      mGeom.push_back(geom);
    } else {
      geom = *it;
    }
    geom->transform(*xform);
  }
}// Tool::transform

// ==============================================================================================================

#ifdef VDB_TOOL_USE_AX
void Tool::ax()
{
  OPENVDB_ASSERT(mParser.getAction().names[0] == "ax");
  mParser.printAction();
  // Code comes from either the "code" option or a "file"; file takes
  // precedence when both are given (useful for longer AX programs).
  std::string code = mParser.get<std::string>("code");
  const std::string file = mParser.get<std::string>("file");
  if (!file.empty()) code = readFileToString(file);
  if (code.empty()) return;// nothing to do

  // Optional attribute bindings: "axname:gridname,..." maps names used in
  // the AX code to actual grid names, so a kernel can be written against
  // generic names and retargeted without editing it.
  openvdb::ax::AttributeBindings bindings;
  const std::string bindStr = mParser.get<std::string>("bindings");
  if (!bindStr.empty()) {
    for (const std::string &pair : tokenize(bindStr, ",")) {
      const size_t c = pair.find(':');
      if (c == std::string::npos) {
        throw std::invalid_argument(
            "ax: bindings entry \"" + pair + "\" must be of the form axname:gridname");
      }
      const std::string axName   = trim(pair.substr(0, c));
      const std::string gridName = trim(pair.substr(c + 1));
      if (axName.empty() || gridName.empty()) {
        throw std::invalid_argument(
            "ax: bindings entry \"" + pair + "\" has an empty name");
      }
      bindings.set(axName, gridName);
    }
  }

  // Gather the selected grids. AX volume code addresses grids by name
  // (@gridname), so the whole selection is handed to a single AX run.
  const bool keep = mParser.get<bool>("keep");
  const std::string vdb = mParser.get<std::string>("vdb");
  GridPtrVec selected;
  if (vdb == "*") {
    for (auto it = mGrid.crbegin(); it != mGrid.crend(); ++it) selected.push_back(*it);
  } else {
    for (int a : mParser.getVec<int>("vdb")) selected.push_back(*this->getGrid(a));
  }
  if (selected.empty()) return;

  // With keep=false (default) AX edits the stack's grids in place (the
  // GridPtrVec holds the same shared pointers). With keep=true we run AX on
  // deep copies pushed onto the stack, leaving the originals untouched;
  // names are preserved so @gridname cross-references still resolve.
  GridPtrVec grids;
  if (keep) {
    for (const GridBase::Ptr &g : selected) {
      GridBase::Ptr copy = g->deepCopyGrid();
      mGrid.push_back(copy);
      grids.push_back(copy);
    }
  } else {
    grids = selected;
  }

  if (mParser.verbose) mTimer.start("AX");
  if (!openvdb::ax::isInitialized()) openvdb::ax::initialize();// lazily set up the LLVM JIT
  openvdb::ax::run(code.c_str(), grids, bindings);
  if (mParser.verbose) mTimer.stop();
}// Tool::ax

#endif// VDB_TOOL_USE_AX
// ==============================================================================================================

void Tool::compute()
{
  OPENVDB_ASSERT(findMatch(mParser.getAction().names[0], {"cpt","div","curl","length","grad","curvature"}));
  mParser.printAction();
  const std::string &action_name = mParser.getAction().names[0];
  const int age = mParser.get<int>("vdb");
  const bool keep = mParser.get<bool>("keep");
  auto it = this->getGrid(age);
  if (action_name == "cpt") {
    if (mParser.verbose) mTimer.start("CPT of SDF");
    auto sdf = gridPtrCast<FloatGrid>(*it);
    if (!sdf || sdf->getGridClass() != GRID_LEVEL_SET) throw std::invalid_argument("cpt: no level set with age "+std::to_string(age));
    auto grid = tools::cpt(*sdf);
    grid->setName("cpt_"+sdf->getName());
    if (!keep) mGrid.erase(std::next(it).base());
    mGrid.push_back(grid);
  } else if (action_name == "div") {
    if (mParser.verbose) mTimer.start("Divergence");
    auto vec = gridPtrCast<Vec3fGrid>(*it);
    if (!vec) throw std::invalid_argument("div: no vec3f grid with age "+std::to_string(age));
    auto grid = tools::divergence(*vec);
    grid->setName("div_"+vec->getName());
    if (!keep) mGrid.erase(std::next(it).base());
    mGrid.push_back(grid);
  } else if (action_name == "curl") {
    if (mParser.verbose) mTimer.start("Curl of Vec3");
    auto vec = gridPtrCast<Vec3fGrid>(*it);
    if (!vec) throw std::invalid_argument("curl: no vec3f grid with age "+std::to_string(age));
    auto grid = tools::curl(*vec);
    grid->setName("curl_"+vec->getName());
    if (!keep) mGrid.erase(std::next(it).base());
    mGrid.push_back(grid);
  } else if (action_name == "length") {
    if (mParser.verbose) mTimer.start("Length of Vec3");
    auto vec = gridPtrCast<Vec3fGrid>(*it);
    if (!vec) throw std::invalid_argument("length: no vec3f grid with age "+std::to_string(age));
    auto grid = tools::magnitude(*vec);
    grid->setName("length_"+vec->getName());
    if (!keep) mGrid.erase(std::next(it).base());
    mGrid.push_back(grid);
  } else if (action_name == "grad") {
    if (mParser.verbose) mTimer.start("Gradient");
    auto scalar = gridPtrCast<FloatGrid>(*it);
    if (!scalar) throw std::invalid_argument("grad: no float grid with age "+std::to_string(age));
    auto grid = tools::gradient(*scalar);
    grid->setName("grad_"+scalar->getName());
    if (!keep) mGrid.erase(std::next(it).base());
    mGrid.push_back(grid);
  } else if (action_name == "curvature") {
    if (mParser.verbose) mTimer.start("Curvature");
    auto scalar = gridPtrCast<FloatGrid>(*it);
    if (!scalar) throw std::invalid_argument("curv: no float grid with age "+std::to_string(age));
    auto grid = tools::meanCurvature(*scalar);
    grid->setName("curv_"+scalar->getName());
    if (!keep) mGrid.erase(std::next(it).base());
    mGrid.push_back(grid);
  } else {
    throw std::invalid_argument("csg: invalid type");
  }
  if (mParser.verbose) mTimer.stop();
}// Tool::compute

// ==============================================================================================================

void Tool::forValues()
{
  OPENVDB_ASSERT(findMatch(mParser.getAction().names[0], {"forAllValues", "forOnValues", "forOffValues"}));
  mParser.printAction();
  const std::string &action_name = mParser.getAction().names[0];
  const int mode = findMatch(action_name, {"forAllValues", "forOnValues", "forOffValues"});// 1-based index
  // Multi-grid: vdb= and use= accept comma-separated lists. The FIRST
  // grid in vdb= is the OUTPUT (iterated and written); the rest are
  // read-only inputs accessible through their respective use= names.
  std::vector<int> ages = mParser.getVec<int>("vdb");
  std::vector<std::string> voxel_vars = mParser.getVec<std::string>("use");
  const bool keep = mParser.get<bool>("keep");
  std::string kernel = mParser.get<std::string>("kernel");
  const std::string kernelFile = mParser.get<std::string>("file");
  if (!kernelFile.empty()) kernel = readFileToString(kernelFile);
  const std::string cls = mParser.get<std::string>("class");
  std::vector<float> back = mParser.getVec<float>("background");
  std::string grid_name = mParser.get<std::string>("name");
  if (ages.empty()) ages.push_back(0);
  if (voxel_vars.empty()) voxel_vars.push_back("v");
  if (ages.size() != voxel_vars.size()) {
      throw std::invalid_argument(action_name + ": vdb= and use= must have the "
          "same number of entries (got vdb=" + std::to_string(ages.size()) +
          " entries and use=" + std::to_string(voxel_vars.size()) + " entries)");
  }
  for (const std::string &v : voxel_vars) {
      if (v.empty()) {
          throw std::invalid_argument(action_name+": each use= entry must be a non-empty identifier");
      }
  }
  // The first (output) grid; secondary grids gathered below into `inputs`.
  auto it0 = this->getGrid(ages[0]);
  GridT::Ptr grid = gridPtrCast<GridT>(*it0);
  if (!grid) throw std::invalid_argument("no FloatGrid with age " + std::to_string(ages[0]));
  // Capture the output grid's ORIGINAL name; it's used for AX-style @gridname access.
  const std::string origName0 = grid->getName();

  // VALIDATION PHASE: Compile and bind the kernel BEFORE modifying the grid
  // stack or names. If validation fails, the exception unwinds without side effects.
  struct GridRef { std::string name; std::string alias; int age; GridT::Ptr grid; };
  Calculator calc;
  std::vector<GridRef> grids;  // filled during validation phase if kernel is non-empty

  if (!kernel.empty()) {
    // Gather every grid referenced via use=/vdb= as a "read source".
    // grids[0] is the OUTPUT (being iterated and written); grids[1..]
    // are read-only inputs.
    // `alias` is the grid's own name (AX-style @gridname access); it may
    // equal `name` (the use= binding) or be empty for an unnamed grid.
    grids.push_back({voxel_vars[0], origName0, ages[0], grid});
    for (size_t k = 1; k < ages.size(); ++k) {
        auto itK = this->getGrid(ages[k]);
        GridT::Ptr gK = gridPtrCast<GridT>(*itK);
        if (!gK) {
            throw std::invalid_argument(action_name +
                ": vdb=" + std::to_string(ages[k]) + " is not a FloatGrid");
        }
        // Disallow accidentally listing the same name twice (the second
        // would shadow the first and create confusing kernels).
        for (size_t j = 0; j < k; ++j) {
            if (voxel_vars[j] == voxel_vars[k]) {
                throw std::invalid_argument(action_name + ": duplicate use= "
                    "name \"" + voxel_vars[k] + "\" in use=...");
            }
        }
        grids.push_back({voxel_vars[k], gK->getName(), ages[k], gK});
    }

    // Configure the neighbor-function rewriter BEFORE compile so calls like
    // "x(1,0,0)" (use= name) or "sphere(1,0,0)" (grid name) are recognized
    // for ANY registered name and synthesized into "<name>(dx,dy,dz)" vars.
    std::vector<std::string> neighborNames;
    for (const GridRef &g : grids) {
        neighborNames.push_back(g.name);
        if (!g.alias.empty() && g.alias != g.name) neighborNames.push_back(g.alias);
    }
    calc.setNeighborFunctions(neighborNames);
    calc.compile(kernel);// throws on syntax error / unknown op
  }

  // NOW SAFE TO MODIFY STACK AND NAMES: compilation and binding have succeeded.
  if (keep) {
    GridT::Ptr tmp = grid->deepCopy();
    mGrid.push_back(tmp);
    grid = tmp;
  }
  if (grid_name.empty()) grid_name = action_name + "_" + grid->getName();
  grid->setName(grid_name);

  if (mParser.verbose) mTimer.start(action_name);
  if (!kernel.empty()) {

    const auto &mem = mParser.processor.memory();

    // Parse a synthesized neighbor name "<prefix>(dx,dy,dz)" into the
    // grid index (via prefix lookup) and integer offsets. Returns -1 if
    // the name doesn't match any registered grid prefix.
    auto parseNeighbor = [&grids](const std::string &name,
                                   int &gridIdx, int &dx, int &dy, int &dz) -> bool {
        for (size_t k = 0; k < grids.size(); ++k) {
            // A grid can be addressed by its use= name or by its own name
            // (@gridname); try both as the "<prefix>(dx,dy,dz)" prefix.
            for (const std::string &nm : {grids[k].name, grids[k].alias}) {
                if (nm.empty()) continue;
                const std::string prefix = nm + "(";
                if (name.size() <= prefix.size() + 1) continue;
                if (name.compare(0, prefix.size(), prefix) != 0) continue;
                if (name.back() != ')') continue;
                const std::string inner = name.substr(
                    prefix.size(), name.size() - prefix.size() - 1);
                int vals[3] = {0, 0, 0};
                int idx = 0;
                size_t start = 0;
                bool ok = true;
                for (size_t i = 0; i <= inner.size(); ++i) {
                    if (i == inner.size() || inner[i] == ',') {
                        if (idx >= 3) { ok = false; break; }
                        const std::string num = inner.substr(start, i - start);
                        try { vals[idx++] = std::stoi(num); }
                        catch (...) { ok = false; break; }
                        start = i + 1;
                    }
                }
                if (!ok || idx != 3) continue;
                gridIdx = static_cast<int>(k);
                dx = vals[0]; dy = vals[1]; dz = vals[2];
                return true;
            }
        }
        return false;
    };

    // Classify each Calculator input variable into one of three buckets:
    //   - center binding: bare name matches one of the use= names; bound
    //     per-voxel to grid[k]'s value at the current coord (or *it if k==0).
    //   - neighbor binding: synthesized "<name>(dx,dy,dz)"; bound via
    //     thread-local ConstAccessor to grid[k] at (i+dx, j+dy, k+dz).
    //   - constant binding: looked up once in Processor memory.
    struct CenterBinding   { int idx; int gridIdx; };
    struct NeighborBinding { int idx; int gridIdx; int dx, dy, dz; };
    std::vector<CenterBinding>   centers;
    std::vector<NeighborBinding> neighbors;
    std::vector<float> base(calc.variables().size());
    for (size_t i = 0; i < calc.variables().size(); ++i) {
        const std::string &name = calc.variables()[i];
        // Center reference: bare name == one of the registered grid names.
        int matchedGrid = -1;
        for (size_t k = 0; k < grids.size(); ++k) {
            // Match either the use= name or the grid's own name (@gridname).
            if (grids[k].name == name ||
                (!grids[k].alias.empty() && grids[k].alias == name)) {
                matchedGrid = static_cast<int>(k); break;
            }
        }
        if (matchedGrid >= 0) {
            centers.push_back({static_cast<int>(i), matchedGrid});
            continue;
        }
        // Neighbor reference: "<name>(dx,dy,dz)" for a known grid.
        int gIdx, dx, dy, dz;
        if (parseNeighbor(name, gIdx, dx, dy, dz)) {
            neighbors.push_back({static_cast<int>(i), gIdx, dx, dy, dz});
            continue;
        }
        // Otherwise: looked up once in Processor memory.
        if (!mem.isSet(name)) {
            std::string nameList;
            for (size_t k = 0; k < grids.size(); ++k) {
                if (k) nameList += ", ";
                nameList += "\"" + grids[k].name + "\"";
            }
            throw std::invalid_argument(
                action_name+": kernel references undefined variable \""+name+
                "\" (set it first with -eval / -calc, or use one of the grid "
                "names [" + nameList + "] for the current voxel value, or "
                "<name>(dx,dy,dz) for a relative neighbor)");
        }
        base[i] = strTo<float>(mem.get(name));
    }

    // Snapshot the OUTPUT grid if the kernel reads non-zero offsets from it
    // (directly or via any alias): otherwise parallel writes to the iterator's
    // grid would race with neighbor reads from the same grid. Check pointer
    // identity to catch all aliases, not just the primary gridIdx == 0.
    const GridT::ConstPtr outputGridPtr = grids[0].grid;
    const bool needSnapshot = std::any_of(neighbors.begin(), neighbors.end(),
        [outputGridPtr, &grids](const NeighborBinding &n) {
            return grids[n.gridIdx].grid.get() == outputGridPtr.get() &&
                   (n.dx || n.dy || n.dz);
        });
    GridT::Ptr out_snap = needSnapshot ? grid->deepCopy() : nullptr;

    // One thread-local ConstAccessor per grid. accessors[0] reads the
    // output's snapshot (or live grid if no snapshot is needed);
    // accessors[k>0] read each input grid directly.
    using ConstAcc = GridT::ConstAccessor;
    using AccTLS  = tbb::enumerable_thread_specific<ConstAcc>;
    std::vector<std::unique_ptr<AccTLS>> accessors;
    accessors.reserve(grids.size());
    for (size_t k = 0; k < grids.size(); ++k) {
        GridT::Ptr src = (k == 0 && out_snap) ? out_snap : grids[k].grid;
        accessors.push_back(std::make_unique<AccTLS>(
            [src]{ return src->getConstAccessor(); }));
    }

    // Per-thread value buffer, seeded once with `base` (the constant inputs
    // looked up in memory). There is no fixed cap on the number of Calculator
    // input variables: a wide neighbor stencil can easily exceed any small
    // fixed size, so the buffer is sized dynamically to base.size(). Only the
    // center/neighbor slots change per voxel and are overwritten below; the
    // constant entries persist from the seed, so no per-voxel copy is needed.
    using ValuesTLS = tbb::enumerable_thread_specific<std::vector<float>>;
    ValuesTLS valuesTLS([&base]{ return base; });

    // Per-voxel lambda. Each TBB worker reuses its thread-local cached
    // accessors via accessors[k]->local() for fast sequential reads.
    auto kernel_fn = [&calc, &centers, &neighbors, &accessors, &valuesTLS](auto &it) {
        std::vector<float> &values = valuesTLS.local();
        const openvdb::Coord c = it.getCoord();
        const float center0 = static_cast<float>(*it);
        // Bind bare-name center references: gridIdx==0 uses *it (no
        // accessor cost), others go through their accessor at the
        // current coord.
        for (const CenterBinding &cb : centers) {
            if (cb.gridIdx == 0) {
                values[cb.idx] = center0;
            } else {
                values[cb.idx] = static_cast<float>(
                    accessors[cb.gridIdx]->local().getValue(c));
            }
        }
        // Bind neighbor references via the corresponding accessor.
        for (const NeighborBinding &nb : neighbors) {
            if (nb.gridIdx == 0 && nb.dx == 0 && nb.dy == 0 && nb.dz == 0) {
                values[nb.idx] = center0;
            } else {
                values[nb.idx] = static_cast<float>(
                    accessors[nb.gridIdx]->local().getValue(
                        c.offsetBy(nb.dx, nb.dy, nb.dz)));
            }
        }
        it.setValue(calc.eval(values.data()));
    };
    switch (mode) {
    case 1: tools::foreach(grid->beginValueAll(), kernel_fn); break;
    case 2: tools::foreach(grid->beginValueOn(),  kernel_fn); break;
    case 3: tools::foreach(grid->beginValueOff(), kernel_fn); break;
    default:
      throw std::invalid_argument("forEachKenel: invalid mode = " + std::to_string(mode));
      break;
    }
  }
  if (int n = findMatch(cls, {"ls", "fog", "unknown"})) {
    auto class_tag = n==1 ? GRID_LEVEL_SET : n==2 ? GRID_FOG_VOLUME : GRID_UNKNOWN;
    grid->setGridClass(class_tag);
  }
  if (back.size()==1) {
    tools::changeBackground(grid->tree(), back[0]);// +/- outside (sign is preserved)
  } else if (back.size()==2) {
    tools::changeAsymmetricLevelSetBackground(grid->tree(), back[0], back[1]);// outside, inside
  }
  if (mParser.verbose) mTimer.stop();
}// Tool::forValues

// ==============================================================================================================

void Tool::multires()
{
  const std::string &action_name = mParser.getAction().names[0];
  OPENVDB_ASSERT(action_name == "multires");
  mParser.printAction();
  const int levels = mParser.get<int>("levels");
  const int age = mParser.get<int>("vdb");
  const bool keep = mParser.get<bool>("keep");
  auto it = this->getGrid(age);
  GridT::Ptr grid = gridPtrCast<GridT>(*it);
  if (!grid) throw std::invalid_argument(action_name + ": no VDB with age " + std::to_string(age));
  if (mParser.verbose) mTimer.start("MultiResGrid");
  if (keep) {
    tools::MultiResGrid<GridT::TreeType> mrg(levels+1, *grid);
    for (size_t level=1; level<mrg.numLevels(); ++level) mGrid.push_back(mrg.grid(level));
  } else {
    tools::MultiResGrid<GridT::TreeType> mrg(levels+1, grid);
    mGrid.erase(std::next(it).base());
    for (size_t level=1; level<mrg.numLevels(); ++level) mGrid.push_back(mrg.grid(level));
  }
  if (mParser.verbose) mTimer.stop();
}// Tool::multires

// ==============================================================================================================

void Tool::segment()
{
  const std::string &action_name = mParser.getAction().names[0];
  OPENVDB_ASSERT(action_name == "segment");
  mParser.printAction();
  const int age = mParser.get<int>("vdb");
  const bool keep = mParser.get<bool>("keep");
  auto it = this->getGrid(age);
  if (mParser.verbose) mTimer.start("Segmenting VDB");
  std::vector<GridBase::Ptr> grids;
  if (auto grid = gridPtrCast<GridT>(*it)) {
    std::vector<GridT::Ptr> segments;
    if (grid->getGridClass() == GRID_LEVEL_SET) {
      tools::segmentSDF(*grid, segments);
    } else {
      tools::segmentActiveVoxels(*grid, segments);
    }
    for (auto g : segments) grids.push_back(g);
  } else {
    throw std::invalid_argument(action_name + ": no VDB with age " + std::to_string(age));
  }
  if (!keep) mGrid.erase(std::next(it).base());
  for (auto g : grids) mGrid.push_back(g);
  if (mParser.verbose) mTimer.stop();
}// Tool::segment

// ==============================================================================================================

// for simplicity we are restricting this resampler to only work on float grids!
void Tool::resample()
{
  const std::string &action_name = mParser.getAction().names[0];
  OPENVDB_ASSERT(action_name == "resample");
  mParser.printAction();
  const VecI age = mParser.getVec<int>("vdb");
  const float scale = mParser.get<float>("scale");
  const Vec3d translate = mParser.getVec3<double>("translate");
  const int order = mParser.get<int>("order");
  const bool keep = mParser.get<bool>("keep");

  if (age.size()!=1 && age.size()!=2) throw std::invalid_argument("resample: expected one or two arguments to \"vdb\"");
  auto itIn = this->getGrid(age[0]);
  FloatGrid::Ptr inGrid = gridPtrCast<FloatGrid>(*itIn), outGrid;
  if (age.size()==2) {
    auto itOut = this->getGrid(age[1]);
    outGrid = gridPtrCast<FloatGrid>(*itOut);
    if (!outGrid) throw std::invalid_argument(action_name+": no reference grid of type float with age "+std::to_string(age[1]));
  } else {
    if (scale<=0.0f) throw std::invalid_argument("resample: invalid scale: "+std::to_string(scale));
    auto map = math::MapBase::Ptr(new math::UniformScaleTranslateMap(scale, translate));
    auto xform = math::Transform::Ptr(new math::Transform(map));
    outGrid = FloatGrid::create();
    outGrid->setTransform(xform);
  }

  if (!inGrid) throw std::invalid_argument(action_name+": no grid of type float with age "+std::to_string(age[0]));

  if (mParser.verbose) mTimer.start("Resampling VDB");
  switch (order) {
  case 0:
    tools::resampleToMatch<tools::PointSampler>(*inGrid, *outGrid);
    break;
  case 1:
    tools::resampleToMatch<tools::BoxSampler>(*inGrid, *outGrid);
    break;
  case 2:
    tools::resampleToMatch<tools::QuadraticSampler>(*inGrid, *outGrid);
    break;
  default:
    throw std::invalid_argument("resample: invalid interpolation order: "+std::to_string(order));
  }
  if (!keep) mGrid.erase(std::next(itIn).base());
  if (age.size()==1) mGrid.push_back(outGrid);
  if (mParser.verbose) mTimer.stop();
}// Tool::resample

// ==============================================================================================================

template <typename GridType>
GridBase::Ptr Tool::clip(const VecF &v, int age, const GridType &input)
{
  using Vec3T = Vec3d;
  GridBase::Ptr output;
  switch (v.size()) {
  case 0: {// clip against a mask
    auto it = this->getGrid(age);
    if (auto mask = gridPtrCast<FloatGrid>(*it)) {
      output = tools::clip(input, *mask);
    } else if (auto mask = gridPtrCast<Vec3fGrid>(*it)) {
      output = tools::clip(input, *mask);
    } else if (auto tmp = gridPtrCast<points::PointDataGrid>(*it)) {
      output = tools::clip(input, *mask);
    } else {
      throw std::invalid_argument("clip: unsupported mask type with "+std::to_string(age));
    }
    break;
  } case 6: {// clip against a bbox
    if (age>=0) throw std::invalid_argument("clip: both mask and bbox were specified");
    BBoxd bbox(Vec3T(v[0],v[1],v[2]), Vec3T(v[3],v[4],v[5]));
    output = tools::clip(input, bbox);
    break;
  } case 8: {// clip against a frustrum
  if (age>=0) throw std::invalid_argument("clip: both mask and frustrum were specified");
    BBoxd bbox(Vec3T(v[0],v[1],v[2]), Vec3T(v[3],v[4],v[5]));
    math::NonlinearFrustumMap frustum(bbox,v[6],v[7]);
    output = tools::clip(input, frustum);
    break;
  } default:
    throw std::invalid_argument("clip: expected either a mask, bbox or frustum");
  }
  return output;
}// Tool::clip

// ==============================================================================================================

void Tool::clip()
{
  OPENVDB_ASSERT(mParser.getAction().names[0] == "clip");
  mParser.printAction();
  const int age = mParser.get<int>("vdb");
  const bool keep = mParser.get<bool>("keep");
  VecF vec = mParser.getVec<float>("bbox");
  float tmp;
  if ((tmp = mParser.get<float>("taper")) > 0.0f) vec.push_back(tmp);
  if ((tmp = mParser.get<float>("depth")) > 0.0f) vec.push_back(tmp);
  const int mask = mParser.get<int>("mask");
  auto it = this->getGrid(age);
  GridBase::Ptr grid;
  if (mParser.verbose) mTimer.start("Clip VDB grid");
  if (auto floatGrid = gridPtrCast<FloatGrid>(*it)) {
    grid =this->clip(vec, mask, *floatGrid);
  } else if (auto vec3Grid = gridPtrCast<Vec3fGrid>(*it)) {
    grid = this->clip(vec, mask, *vec3Grid);
  } else {
    throw std::invalid_argument(mParser.getAction().names[0] + ": unsupported grid type with " + std::to_string(age));
  }
  if (!(*it)->getName().empty()) grid->setName("clip_"+(*it)->getName());
  if (!keep) mGrid.erase(std::next(it).base());
  mGrid.push_back(grid);
  if (mParser.verbose) mTimer.stop();
}

} // namespace vdb_tool
} // namespace OPENVDB_VERSION_NAME
} // namespace openvdb
