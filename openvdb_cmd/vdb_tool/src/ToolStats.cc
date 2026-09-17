// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/// @file ToolStats.cc
/// @brief Diagnostics, statistics, histogram and the -print output. Split out of Tool.h; see Tool.h for the class.

#include "Tool.h"
#include <openvdb/tools/Count.h>// tools::minMax
#include <openvdb/tools/Diagnostics.h>
#include <openvdb/tools/LevelSetMeasure.h>
#include <openvdb/tools/Statistics.h>
#include <openvdb/util/Formats.h>

namespace openvdb {
OPENVDB_USE_VERSION_NAMESPACE
namespace OPENVDB_VERSION_NAME {
namespace vdb_tool {

// ==============================================================================================================

void Tool::diagnose()
{
  OPENVDB_ASSERT(mParser.getAction().names[0] == "diagnose");
  mParser.printAction();
  const std::string vdb_str = mParser.get<std::string>("vdb");
  const bool fatal    = mParser.get<bool>("fatal");
  const size_t checks = static_cast<size_t>(mParser.get<int>("checks"));

  std::vector<GridBase::Ptr> grids;
  if (vdb_str == "*") {
    for (auto it = mGrid.crbegin(); it != mGrid.crend(); ++it) grids.push_back(*it);
  } else {
    for (int a : mParser.getVec<int>("vdb")) {
      if (size_t(a) >= mGrid.size())
        throw std::out_of_range("diagnose: vdb index " + std::to_string(a) +
                                " is out of range (stack size " + std::to_string(mGrid.size()) + ")");
      grids.push_back(*this->getGrid(a));
    }
  }

  if (grids.empty()) { std::clog << "diagnose: no grids on stack\n"; return; }

  bool anyFailed = false;
  for (auto& base : grids) {
    const std::string& name = base->getName();
    auto grid = gridPtrCast<FloatGrid>(base);
    if (!grid) {
      std::clog << "diagnose: [" << name << "] skipped (not a FloatGrid)\n";
      continue;
    }
    std::string msg;
    if (base->getGridClass() == GRID_LEVEL_SET) {
      msg = tools::checkLevelSet(*grid, checks);
    } else if (base->getGridClass() == GRID_FOG_VOLUME) {
      msg = tools::checkFogVolume(*grid, checks);
    } else {
      // For generic float grids run NaN and infinity checks.
      tools::Diagnose<FloatGrid> d(*grid);
      tools::CheckNan<FloatGrid> checkNan;
      tools::CheckInf<FloatGrid> checkInf;
      msg += d.check(checkNan, /*updateMask*/false, /*voxels*/true, /*tiles*/true, /*background*/true);
      msg += d.check(checkInf, /*updateMask*/false, /*voxels*/true, /*tiles*/true, /*background*/true);
    }
    if (msg.empty()) {
      std::clog << "diagnose: [" << name << "] OK\n";
    } else {
      anyFailed = true;
      std::clog << "diagnose: [" << name << "] FAILED\n";
      // indent each line of the diagnostic message
      std::istringstream ss(msg);
      std::string line;
      while (std::getline(ss, line))
        if (!line.empty()) std::clog << "  " << line << "\n";
    }
  }
  if (fatal && anyFailed)
    throw std::runtime_error("diagnose: one or more grids failed validation");
}// Tool::diagnose

// ==============================================================================================================

void Tool::stats()
{
  OPENVDB_ASSERT(mParser.getAction().names[0] == "stats");
  mParser.printAction();
  const std::string vdb_str = mParser.get<std::string>("vdb");

  std::vector<GridBase::Ptr> grids;
  std::vector<int> ages;
  if (vdb_str == "*") {
    int age = 0;
    for (auto it = mGrid.crbegin(); it != mGrid.crend(); ++it, ++age) {
      grids.push_back(*it);
      ages.push_back(age);
    }
  } else {
    for (int a : mParser.getVec<int>("vdb")) {
      if (size_t(a) >= mGrid.size())
        throw std::out_of_range("stats: vdb index " + std::to_string(a) +
                                " is out of range (stack size " + std::to_string(mGrid.size()) + ")");
      grids.push_back(*this->getGrid(a));
      ages.push_back(a);
    }
  }

  if (grids.empty()) { std::clog << "stats: no grids on stack\n"; return; }

  const int aw = 5, w = 14;
  int nw = 4;// minimum width for "name" header
  for (auto& g : grids) nw = std::max(nw, (int)g->getName().size());
  nw += 2;// two-space gap after the longest name
  std::clog << std::right << std::setw(aw) << "age"
            << "  "
            << std::left  << std::setw(nw) << "name"
            << std::right << std::setw(w)  << "background"
                          << std::setw(w)  << "min"
                          << std::setw(w)  << "max"
                          << std::setw(w)  << "mean"
                          << std::setw(w)  << "stddev"
                          << std::setw(w)  << "area"
                          << std::setw(w)  << "volume"
            << "\n" << std::string(aw + 2 + nw + 7*w, '-') << "\n";

  // Background column: numeric grid types print their scalar background, vector
  // grids print the whole vector as "[x, y, z]" (NOT its magnitude, unlike the
  // value columns below), anything else is "(n/a)".
  auto backgroundStr = [](const GridBase::Ptr& base) -> std::string {
    std::stringstream ss;
    if      (auto p = gridPtrCast<FloatGrid>(base))  ss << p->background();
    else if (auto p = gridPtrCast<DoubleGrid>(base)) ss << p->background();
    else if (auto p = gridPtrCast<Int32Grid>(base))  ss << p->background();
    else if (auto p = gridPtrCast<Int64Grid>(base))  ss << p->background();
    else if (auto p = gridPtrCast<BoolGrid>(base))   ss << p->background();
    else if (auto p = gridPtrCast<Vec3SGrid>(base))  ss << p->background();
    else if (auto p = gridPtrCast<Vec3DGrid>(base))  ss << p->background();
    else if (auto p = gridPtrCast<Vec3IGrid>(base))  ss << p->background();
    else return "(n/a)";
    return ss.str();
  };

  /// @brief True if @a base is a vector-valued grid, whose value columns therefore
  ///        report magnitudes rather than the values themselves.
  auto isVectorGrid = [](const GridBase::Ptr& base) {
    return gridPtrCast<Vec3SGrid>(base) || gridPtrCast<Vec3DGrid>(base) || gridPtrCast<Vec3IGrid>(base);
  };

  // min/max/mean/stddev of active voxels, dispatched by value type. tools::statistics()
  // returns a (non-templated) math::Stats regardless of the grid's value type, so this
  // covers every scalar type the file formats can round-trip. For vector grids it
  // reduces each value to its magnitude (see stats_internal::GetValImpl), so these
  // four columns report statistics of |v| -- flagged by a legend under the table.
  auto valueStatsStr = [&](const GridBase::Ptr& base) -> std::string {
    std::stringstream ss;
    ss << std::fixed << std::setprecision(6);
    auto printFromStats = [&](const math::Stats& s) {
      // An empty grid (no active voxels) has no min/max/mean/stddev to report;
      // math::Stats would otherwise print the ±inf/NaN sentinels it initializes to.
      if (s.size() == 0) {
        ss << std::setw(w) << "(empty)" << std::setw(w) << "(empty)"
           << std::setw(w) << "(empty)" << std::setw(w) << "(empty)";
      } else {
        ss << std::setw(w) << s.min() << std::setw(w) << s.max()
           << std::setw(w) << s.mean() << std::setw(w) << s.stdDev();
      }
    };
    if      (auto p = gridPtrCast<FloatGrid>(base))  printFromStats(tools::statistics(p->cbeginValueOn()));
    else if (auto p = gridPtrCast<DoubleGrid>(base)) printFromStats(tools::statistics(p->cbeginValueOn()));
    else if (auto p = gridPtrCast<Int32Grid>(base))  printFromStats(tools::statistics(p->cbeginValueOn()));
    else if (auto p = gridPtrCast<Int64Grid>(base))  printFromStats(tools::statistics(p->cbeginValueOn()));
    else if (auto p = gridPtrCast<BoolGrid>(base))   printFromStats(tools::statistics(p->cbeginValueOn()));
    else if (auto p = gridPtrCast<Vec3SGrid>(base))  printFromStats(tools::statistics(p->cbeginValueOn()));
    else if (auto p = gridPtrCast<Vec3DGrid>(base))  printFromStats(tools::statistics(p->cbeginValueOn()));
    else if (auto p = gridPtrCast<Vec3IGrid>(base))  printFromStats(tools::statistics(p->cbeginValueOn()));
    else ss << std::setw(w) << "(n/a)" << std::setw(w) << "(n/a)" << std::setw(w) << "(n/a)" << std::setw(w) << "(n/a)";
    return ss.str();
  };

  bool anyVector = false;
  for (size_t i = 0; i < grids.size(); ++i) {
    auto& base = grids[i];
    const int age = ages[i];
    anyVector |= isVectorGrid(base);
    std::clog << std::right << std::setw(aw) << age << "  "
              << std::left  << std::setw(nw) << base->getName()
              << std::right << std::setw(w)  << backgroundStr(base)
                            << valueStatsStr(base);
    // area and volume are only defined for float level sets
    auto grid = gridPtrCast<FloatGrid>(base);
    if (grid && base->getGridClass() == GRID_LEVEL_SET) {
      try { std::clog << std::setw(w) << tools::levelSetArea(*grid); }
      catch (...) { std::clog << std::setw(w) << "(n/a)"; }
      try { std::clog << std::setw(w) << tools::levelSetVolume(*grid); }
      catch (...) { std::clog << std::setw(w) << "(n/a)"; }
    } else {
      std::clog << std::setw(w) << "(n/a)" << std::setw(w) << "(n/a)";
    }
    std::clog << "\n";
  }
  if (anyVector) {
    std::clog << "(for vector grids min/max/mean/stddev are of the vector magnitude |v|,"
                 " while background is the vector itself)\n";
  }
}// Tool::stats

// ==============================================================================================================

void Tool::histogram()
{
  OPENVDB_ASSERT(mParser.getAction().names[0] == "histogram");
  mParser.printAction();
  const std::string vdb_str = mParser.get<std::string>("vdb");
  const std::string min_str = mParser.get<std::string>("min");
  const std::string max_str = mParser.get<std::string>("max");
  const int  bins   = mParser.get<int>("bins");
  const int  cols   = mParser.get<int>("cols");
  const bool useLog = mParser.get<bool>("log");

  if (bins < 1) throw std::invalid_argument("histogram: \"bins\" must be at least 1, got "+std::to_string(bins));
  if (cols < 1) throw std::invalid_argument("histogram: \"cols\" must be at least 1, got "+std::to_string(cols));
  // An explicit range is optional: either bound may be given on its own, in which
  // case the other is still derived from the grid's own extrema.
  const bool hasMin = !min_str.empty(), hasMax = !max_str.empty();
  const double userMin = hasMin ? mParser.get<double>("min") : 0.0;
  const double userMax = hasMax ? mParser.get<double>("max") : 0.0;
  if (hasMin && hasMax && userMin >= userMax) {
    throw std::invalid_argument("histogram: \"min\" must be less than \"max\", got min="+min_str+" max="+max_str);
  }

  std::vector<GridBase::Ptr> grids;
  std::vector<int> ages;
  if (vdb_str == "*") {
    int age = 0;
    for (auto it = mGrid.crbegin(); it != mGrid.crend(); ++it, ++age) {
      grids.push_back(*it);
      ages.push_back(age);
    }
  } else {
    for (int a : mParser.getVec<int>("vdb")) {
      if (size_t(a) >= mGrid.size())
        throw std::out_of_range("histogram: vdb index " + std::to_string(a) +
                                " is out of range (stack size " + std::to_string(mGrid.size()) + ")");
      grids.push_back(*this->getGrid(a));
      ages.push_back(a);
    }
  }

  if (grids.empty()) { std::clog << "histogram: no grids on stack\n"; return; }

  // Bin the active values of one typed grid.
  // Returns null when there is nothing to plot; the caller uses @a n and @a constant
  // to distinguish cases: n==0 → no active voxels; n>0 && constant → every active
  // voxel shares a single value; n>0 && !constant → user-supplied bounds exclude all
  // grid values (tools::histogram requires a non-empty range).
  auto build = [&](const auto &grid, double &lo, double &hi, uint64_t &n, bool &constant)
      -> std::unique_ptr<math::Histogram> {
    const math::Extrema ex = tools::extrema(grid.cbeginValueOn());
    n = ex.size();
    if (n == 0) return nullptr;
    lo = hasMin ? userMin : ex.min();
    hi = hasMax ? userMax : ex.max();
    if (lo >= hi) {
      // If neither bound came from the user, lo/hi equal ex.min()/ex.max(), so lo>=hi
      // means the grid is truly constant.  If at least one bound is user-supplied the
      // grid may span a valid range that simply doesn't intersect [lo, hi).
      constant = !hasMin && !hasMax;
      if (constant) lo = ex.min(); // report the actual constant, not a user value
      return nullptr;
    }
    return std::make_unique<math::Histogram>(
        tools::histogram(grid.cbeginValueOn(), lo, hi, static_cast<size_t>(bins)));
  };

  // Every value below is printed with an explicit format, since the per-bin rows
  // switch std::clog to fixed/low precision and that state would otherwise leak
  // into the next grid's summary line (and into whatever action runs next).
  const std::ios_base::fmtflags oldFlags = std::clog.flags();
  const std::streamsize oldPrecision = std::clog.precision();
  auto summaryFmt = [](std::ostream &os) -> std::ostream& {
    return os << std::defaultfloat << std::setprecision(6);
  };

  for (size_t i = 0; i < grids.size(); ++i) {
    auto &base = grids[i];
    if (i) std::clog << "\n";
    std::clog << "histogram: [" << ages[i] << "] \"" << base->getName() << "\" ("
              << base->valueType() << ")\n";

    double lo = 0.0, hi = 0.0;
    uint64_t n = 0;
    bool isVec = false, constant = false;
    std::unique_ptr<math::Histogram> hist;
    if      (auto p = gridPtrCast<FloatGrid>(base))  hist = build(*p, lo, hi, n, constant);
    else if (auto p = gridPtrCast<DoubleGrid>(base)) hist = build(*p, lo, hi, n, constant);
    else if (auto p = gridPtrCast<Int32Grid>(base))  hist = build(*p, lo, hi, n, constant);
    else if (auto p = gridPtrCast<Int64Grid>(base))  hist = build(*p, lo, hi, n, constant);
    else if (auto p = gridPtrCast<BoolGrid>(base))   hist = build(*p, lo, hi, n, constant);
    // tools::histogram reduces a vector value to its magnitude (see
    // stats_internal::GetValImpl), so vector grids are binned by |v|. This is what
    // makes "-grad -histogram" a useful check of the Eikonal condition |grad(phi)|=1.
    else if (auto p = gridPtrCast<Vec3SGrid>(base)) { hist = build(*p, lo, hi, n, constant); isVec = true; }
    else if (auto p = gridPtrCast<Vec3DGrid>(base)) { hist = build(*p, lo, hi, n, constant); isVec = true; }
    else if (auto p = gridPtrCast<Vec3IGrid>(base)) { hist = build(*p, lo, hi, n, constant); isVec = true; }
    else {
      std::clog << "  (unsupported value type, skipped)\n";
      continue;
    }
    const char *unit = isVec ? " (by vector magnitude)" : "";
    if (n == 0) { std::clog << "  (no active voxels)\n"; continue; }
    if (!hist && constant) {
      summaryFmt(std::clog) << "  all " << n << " active voxels have the constant "
                            << (isVec ? "magnitude " : "value ") << lo << "\n";
      continue;
    }
    if (!hist || hist->size() == 0) {// user-supplied range excludes all grid values
      summaryFmt(std::clog) << "  none of the " << n << " active voxels fall within the requested range ["
                            << lo << ", " << hi << "]\n";
      continue;
    }

    uint64_t peak = 0;
    for (size_t b = 0; b < hist->numBins(); ++b) peak = std::max(peak, hist->count(b));

    summaryFmt(std::clog) << "  " << hist->size() << " of " << n << " active voxels" << unit
                          << " in " << hist->numBins() << " bins over [" << lo << ", " << hi << "]"
                          << (useLog ? ", log scale" : "") << "\n";

    for (size_t b = 0; b < hist->numBins(); ++b) {
      const uint64_t c = hist->count(b);
      // Scale bars against the tallest bin so the plot always spans "cols" characters.
      // log1p keeps empty bins at zero length while compressing very peaked
      // distributions (common for level sets and fog volumes) into a readable range.
      int len = 0;
      if (peak > 0 && c > 0) {
        const double frac = useLog ? std::log1p(double(c)) / std::log1p(double(peak))
                                   : double(c) / double(peak);
        len = std::max(1, static_cast<int>(frac * cols + 0.5));
      }
      std::clog << "  [" << std::right << std::setw(12) << std::fixed << std::setprecision(4) << hist->min(b)
                << ", " << std::setw(12) << hist->max(b)
                << (b + 1 == hist->numBins() ? "] " : ") ")
                << std::setw(10) << c << " "
                << std::setw(5) << std::setprecision(1) << (100.0 * double(c) / double(hist->size())) << "% "
                << std::defaultfloat << std::string(size_t(len), '#') << "\n";
    }
  }
  std::clog.flags(oldFlags);
  std::clog.precision(oldPrecision);
}// Tool::histogram

// ==============================================================================================================

void Tool::print_args(std::ostream& os) const
{
  os << "\n" << std::setw(40) << std::setfill('=') << "> Actions <" << std::setw(40) << "\n";
  mParser.print(os);
  os << std::setw(80) << std::setfill('=') << "\n" << std::endl;
}// Tool::print_args

// ==============================================================================================================

// ----- Helpers for the pretty-printed -print output ------------------------
// Helpers used only by Tool::print() below: keep them local to this TU.
namespace {
namespace print_detail {

enum Align { LEFT, RIGHT };

// Print a centered title between U+2550 (BOX DRAWINGS DOUBLE HORIZONTAL) chars
// to a fixed visible width (default 80 columns).
void printBanner(std::ostream& os, const std::string& title, int width = 80)
{
    static const std::string kBar = "═";// U+2550, 3 bytes in UTF-8
    const int titleLen = static_cast<int>(title.size());
    const int left  = std::max(0, (width - titleLen) / 2);
    const int right = std::max(0, width - titleLen - left);
    for (int i = 0; i < left;  ++i) os << kBar;
    os << title;
    for (int i = 0; i < right; ++i) os << kBar;
    os << "\n";
}

// 1234567 -> "1,234,567" — thousands separators for big counts.
std::string formatCommas(uint64_t n)
{
    std::string s = std::to_string(n);
    int pos = static_cast<int>(s.size()) - 3;
    while (pos > 0) { s.insert(pos, ","); pos -= 3; }
    return s;
}

// Wrap util::printBytes to produce a trim string ("15.326 MB").
std::string formatBytes(uint64_t bytes)
{
    std::stringstream ss;
    util::printBytes(ss, bytes, /*head=*/"", /*tail=*/"", /*exact=*/false,
                     /*width=*/0, /*precision=*/3);
    return ss.str();
}

// Integer index-space bbox, formatted as a compact "[a,b,c]->[d,e,f]" with an
// ASCII arrow (so std::setw byte-counting lines up the column). Used for the
// VDB grid table's index-space active-voxel bbox column.
std::string formatBBox(const openvdb::CoordBBox& bbox)
{
    std::stringstream ss;
    const auto& mn = bbox.min();
    const auto& mx = bbox.max();
    ss << "[" << mn.x() << "," << mn.y() << "," << mn.z() << "]->["
       << mx.x() << "," << mx.y() << "," << mx.z() << "]";
    return ss.str();
}

// Float-valued bbox, formatted like formatBBox but trimmed to 3 decimals to
// avoid seven-significant-digit float bloat. Used for the Geometry table
// (world-space vertex bbox) and the VDB grid table's world-space bbox column.
template <typename BBoxT>
std::string formatBBoxd(const BBoxT& bbox)
{
    std::stringstream ss;
    ss << std::fixed << std::setprecision(3);
    const auto& mn = bbox.min();
    const auto& mx = bbox.max();
    ss << "[" << mn[0] << "," << mn[1] << "," << mn[2] << "]->["
       << mx[0] << "," << mx[1] << "," << mx[2] << "]";
    return ss.str();
}

// Per-level node counts, top-down, e.g. "2->10->58" = 2 upper-internal nodes,
// 10 lower-internal nodes, 58 leaf nodes. TreeBase::nodeCount() returns counts
// bottom-up (index 0 = leaf) sized treeDepth, with the last element being the
// single root node — we drop the root and reverse the rest so the order
// matches the "32->16->8" node-dimension column header. Works on any value
// type since it goes through the virtual TreeBase interface.
std::string formatNodes(const openvdb::GridBase& grid)
{
    const auto counts = grid.baseTree().nodeCount();// bottom-up, includes root at back
    if (counts.size() < 2) return "(n/a)";
    std::stringstream ss;
    for (std::size_t i = counts.size() - 1; i-- > 0; ) {// skip root (back), down to leaf
        ss << counts[i];
        if (i > 0) ss << "->";
    }
    return ss.str();
}

// Print a table with column-width auto-sizing. Cells must be pure ASCII
// (column widths use byte size = visible width). The header separator uses
// U+2500 BOX DRAWINGS LIGHT HORIZONTAL and is emitted directly so the
// multibyte glyphs don't interact with std::setw byte-counting.
void printTable(std::ostream& os,
                       const std::vector<std::string>& headers,
                       const std::vector<std::vector<std::string>>& rows,
                       const std::vector<Align>& aligns,
                       const std::string& indent = "  ")
{
    const std::size_t nc = headers.size();
    std::vector<std::size_t> w(nc, 0);
    for (std::size_t c = 0; c < nc; ++c) w[c] = headers[c].size();
    for (const auto& r : rows) {
        for (std::size_t c = 0; c < nc && c < r.size(); ++c) {
            w[c] = std::max(w[c], r[c].size());
        }
    }
    auto emit = [&](const std::vector<std::string>& r) {
        os << indent;
        for (std::size_t c = 0; c < nc; ++c) {
            const std::string& cell = c < r.size() ? r[c] : std::string();
            os << (aligns[c] == RIGHT ? std::right : std::left)
               << std::setw(static_cast<int>(w[c])) << cell;
            if (c + 1 < nc) os << "  ";
        }
        os << "\n";
    };
    emit(headers);
    // Separator row — repeat U+2500 to match each column's width.
    os << indent;
    for (std::size_t c = 0; c < nc; ++c) {
        for (std::size_t i = 0; i < w[c]; ++i) os << "─";
        if (c + 1 < nc) os << "  ";
    }
    os << "\n";
    for (const auto& r : rows) emit(r);
}

}// namespace print_detail
} // anonymous namespace
// ----- end helpers ---------------------------------------------------------

void Tool::print(std::ostream& os) const
{
  using namespace print_detail;
  OPENVDB_ASSERT(mParser.getAction().names[0] == "print");
  const int level = mParser.get<int>("level");

  if (mParser.verbose>1) {
    os << "\n";
    printBanner(os, " Actions ");
    mParser.print(os);
    printBanner(os, "");
    os << "\n";
    printBanner(os, " Variables ");
    mParser.processor.memory().print(os);
    printBanner(os, "");
    os << std::endl;
  }

  if (mParser.verbose>0) {
    os << "\n";
    printBanner(os, " Primitives ");

    // Geometry — same column-aligned table style as the VDB grid table below.
    std::vector<std::string> geomHeaders = {"age", "name", "vtx", "tri", "quad", "size"};
    std::vector<Align>       geomAligns  = {RIGHT, LEFT,   RIGHT, RIGHT, RIGHT,  RIGHT};
    if (level >= 1) { geomHeaders.push_back("bbox"); geomAligns.push_back(LEFT); }

    auto buildGeomRow = [&](int age, const Geometry& geom) {
        const std::uint64_t mem = sizeof(geom)
            + geom.vtx().size()  * sizeof(openvdb::Vec3s)
            + geom.tri().size()  * sizeof(openvdb::Vec3I)
            + geom.quad().size() * sizeof(openvdb::Vec4I)
            + geom.rgb().size()  * sizeof(openvdb::Vec3s);
        std::vector<std::string> row = {
            std::to_string(age),
            geom.getName(),
            formatCommas(geom.vtx().size()),
            formatCommas(geom.tri().size()),
            formatCommas(geom.quad().size()),
            formatBytes(mem),
        };
        if (level >= 1) row.push_back(formatBBoxd(geom.bbox()));
        return row;
    };

    std::vector<std::vector<std::string>> geomRows;
    if (mParser.getStr("geo")=="*") {
      for (auto begin = mGeom.crbegin(), it = begin, end = mGeom.crend(); it != end; ++it) {
        geomRows.push_back(buildGeomRow(static_cast<int>(std::distance(begin, it)), **it));
      }
    } else {
      for (int age : mParser.getVec<int>("geo")) {
        geomRows.push_back(buildGeomRow(age, **this->getGeom(age)));
      }
    }
    if (geomRows.empty()) {
      os << "Geometry: none\n";
    } else {
      os << "Geometry:\n";
      printTable(os, geomHeaders, geomRows, geomAligns);
    }

    // VDB grids — column-aligned table.
    std::vector<std::string> headers = {"age", "name", "type", "class", "dim", "voxels", "dx", "size"};
    std::vector<Align>       aligns  = {RIGHT, LEFT,   LEFT,   LEFT,    LEFT,  RIGHT,    RIGHT, RIGHT};
    if (level >= 1) { headers.push_back("bbox(index)"); aligns.push_back(LEFT); }
    if (level >= 1) { headers.push_back("bbox(world)"); aligns.push_back(LEFT); }
    if (level >= 1) { headers.push_back("32->16->8");   aligns.push_back(RIGHT); }

    auto buildRow = [&](int age, const GridBase& grid) {
        const auto bbox = grid.evalActiveVoxelBoundingBox();
        const auto dim  = bbox.dim();
        std::stringstream dimSS;
        dimSS << "[" << dim.x() << ", " << dim.y() << ", " << dim.z() << "]";
        std::stringstream dxSS;
        dxSS << grid.voxelSize()[0];
        std::vector<std::string> row = {
            std::to_string(age),
            grid.getName(),
            grid.valueType(),
            GridBase::gridClassToString(grid.getGridClass()),
            dimSS.str(),
            formatCommas(grid.activeVoxelCount()),
            dxSS.str(),
            formatBytes(grid.memUsage()),
        };
        // Both the index-space active-voxel bbox and its world-space transform.
        if (level >= 1) row.push_back(formatBBox(bbox));
        if (level >= 1) row.push_back(formatBBoxd(grid.transform().indexToWorld(bbox)));
        if (level >= 1) row.push_back(formatNodes(grid));
        return row;
    };

    std::vector<std::vector<std::string>> rows;
    if (mParser.getStr("vdb")=="*") {
      for (auto begin = mGrid.crbegin(), it = begin, end = mGrid.crend(); it != end; ++it) {
        rows.push_back(buildRow(static_cast<int>(std::distance(begin, it)), **it));
      }
    } else {
      for (int age : mParser.getVec<int>("vdb")) {
        rows.push_back(buildRow(age, **this->getGrid(age)));
      }
    }
    if (rows.empty()) {
      os << "VDB grids: none\n";
    } else {
      os << "VDB grids:\n";
      printTable(os, headers, rows, aligns);
    }

    if (mParser.get<bool>("mem")) {
      os << "\n";
      printBanner(os, " Variables ");
      mParser.processor.memory().print(os);
    }

    printBanner(os, "");
    os << std::endl;
  }
}// Tool::print

} // namespace vdb_tool
} // namespace OPENVDB_VERSION_NAME
} // namespace openvdb
