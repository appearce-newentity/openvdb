// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/// @file ToolNanoVDB.cc
/// @brief NanoVDB read (-read *.nvdb) and write (-write *.nvdb) actions. Kept in their own TU because
///        nanovdb::tools::createNanoGrid is instantiated here for every supported grid/quantization type,
///        which makes this the most expensive file in vdb_tool to compile. Split out of Tool.h.

#include "Tool.h"
#ifdef VDB_TOOL_USE_NANO
#include <nanovdb/NanoVDB.h>
#include <nanovdb/io/IO.h>
#include <nanovdb/tools/CreateNanoGrid.h>
#include <nanovdb/tools/NanoToOpenVDB.h>
#endif
#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace openvdb {
OPENVDB_USE_VERSION_NAMESPACE
namespace OPENVDB_VERSION_NAME {
namespace vdb_tool {

// ==============================================================================================================

#ifdef VDB_TOOL_USE_NANO
void Tool::readNVDB(const std::string &fileName)
{
  OPENVDB_ASSERT(mParser.getAction().names[0] == "read");
  const VecS gridNames = mParser.getVec<std::string>("grids");
  if (gridNames.empty()) throw std::invalid_argument("readNVDB: no grids names specified");
  std::vector<nanovdb::GridHandle<>> grids;
  if (fileName=="stdin.nvdb") {
    if (isatty(fileno(stdin))) throw std::invalid_argument("readNVDB: stdin is not connected to the terminal!");
    if (mParser.verbose) mTimer.start("Reading NanoVDB grid(s) from input stream");
    grids = nanovdb::io::readGrids(std::cin);
    throw std::runtime_error("Not implemented");
  } else {
    if (mParser.verbose) mTimer.start("Reading NanoVDB grid(s) from file named \""+fileName+"\"");
    grids = nanovdb::io::readGrids(fileName);
  }
  const size_t count = mGrid.size();
  if (grids.size()) {
    for (const auto& gridHandle : grids) {
      if (gridNames[0]=="*" || findMatch(gridHandle.gridMetaData()->shortGridName(), gridNames)) mGrid.push_back(nanovdb::tools::nanoToOpenVDB(gridHandle));
    }
  } else if (mParser.verbose>0) {
    std::clog << "readVDB: no vdb grids in \"" << fileName << "\"";
  }
  if (mParser.verbose) {
    mTimer.stop();
    if (mGrid.size() == count) std::clog << "readNVDB: no NanoVDB grids were loaded\n";
    if (mParser.verbose>1) for (auto it = std::next(mGrid.cbegin(), count); it != mGrid.cend(); ++it) (*it)->print();
  }
}// Tool::readNVDB
#else
void Tool::readNVDB(const std::string&)
{
    throw std::runtime_error("NanoVDB support was disabled during compilation!");
}// Tool::readNVDB
#endif

// ==============================================================================================================

#ifdef VDB_TOOL_USE_NANO
void Tool::writeNVDB(const std::string &fileName)
{
  const std::string &action_name = mParser.getAction().names[0];
  OPENVDB_ASSERT(action_name == "write");
  try {
    mParser.printAction();
    const std::string age = mParser.get<std::string>("vdb");
    const bool keep = mParser.get<bool>("keep");
    const std::string codec_str = toLowerCase(mParser.get<std::string>("codec"));
    const std::string bits = mParser.get<std::string>("bits");
    const bool dither = mParser.get<bool>("dither");
    const bool absolute = mParser.get<bool>("absolute");
    const float tolerance = mParser.get<float>("tolerance");// negative values means derive it from the grid class (eg ls or fog)
    const std::string stats = mParser.get<std::string>("stats");
    const std::string checksum = mParser.get<std::string>("checksum");
    const int verbose = mParser.verbose ? 1 : 0;

    nanovdb::io::Codec codec = nanovdb::io::Codec::NONE;// compression codec for the file
    if (codec_str == "zip") {
      codec = nanovdb::io::Codec::ZIP;
    } else if (codec_str == "blosc") {
      codec = nanovdb::io::Codec::BLOSC;
    } else if (!codec_str.empty() && codec_str != "none") {
      throw std::invalid_argument("writeNVDB: unsupported codec \""+codec_str+"\"");
    }

    nanovdb::GridType qMode = nanovdb::GridType::Unknown;// output grid type defaults to input grid type
    if (bits == "4") {
      qMode = nanovdb::GridType::Fp4;
    } else if (bits == "8") {
      qMode = nanovdb::GridType::Fp8;
    } else if (bits == "16") {
      qMode = nanovdb::GridType::Fp16;
    } else if (bits == "N") {
      qMode = nanovdb::GridType::FpN;
    } else if (bits != "" && bits != "32") {
      throw std::invalid_argument("writeNVDB: unsupported bits \""+bits+"\"");
    }

    nanovdb::tools::StatsMode sMode = nanovdb::tools::StatsMode::Default;
    if (stats == "none") {
      sMode = nanovdb::tools::StatsMode::Disable;
    } else if (stats == "bbox") {
      sMode = nanovdb::tools::StatsMode::BBox;
    } else if (stats == "extrema") {
      sMode = nanovdb::tools::StatsMode::MinMax;
    } else if (stats == "all") {
      sMode = nanovdb::tools::StatsMode::All;
    } else if (stats != "") {
      throw std::invalid_argument("writeNVDB: unsupported stats \""+stats+"\"");
    }

    nanovdb::CheckMode cMode = nanovdb::CheckMode::Default;
    if (checksum == "none") {
      cMode = nanovdb::CheckMode::Disable;
    } else if (checksum == "partial") {
      cMode = nanovdb::CheckMode::Partial;
    } else if (checksum == "full") {
      cMode = nanovdb::CheckMode::Full;
    } else if (checksum != "") {
      throw std::invalid_argument("writeNVDB: unsupported checksum \""+checksum+"\"");
    }

    GridPtrVec grids;// vector of grids to be written and possibly removed from mGrid
    if (age == "*") {
      for (auto it = mGrid.crbegin(); it != mGrid.crend(); ++it) grids.push_back(*it);
      if (!keep) mGrid.clear();
    } else {
      for (int a : vectorize<int>(age, ",")) grids.push_back(*this->getGrid(a));
      if (!keep) for (auto &g : grids) mGrid.remove(g);
    }

    if (grids.empty()) throw std::invalid_argument(action_name+": no vdb grids to write");

    auto openToNano = [&](const GridBase::Ptr& base) {
      if (auto floatGrid = GridBase::grid<FloatGrid>(base)) {
        using SrcGridT = openvdb::FloatGrid;
        switch (qMode){
        case nanovdb::GridType::Fp4:
          return nanovdb::tools::createNanoGrid<SrcGridT, nanovdb::Fp4>(*floatGrid, sMode, cMode, dither, verbose);
        case nanovdb::GridType::Fp8:
          return nanovdb::tools::createNanoGrid<SrcGridT, nanovdb::Fp8>(*floatGrid, sMode, cMode, dither, verbose);
        case nanovdb::GridType::Fp16:
          return nanovdb::tools::createNanoGrid<SrcGridT, nanovdb::Fp16>(*floatGrid, sMode, cMode, dither, verbose);
        case nanovdb::GridType::FpN:
          if (absolute) {
            return nanovdb::tools::createNanoGrid<SrcGridT, nanovdb::FpN>(*floatGrid, sMode, cMode, dither, verbose, nanovdb::tools::AbsDiff(tolerance));
          } else {
            return nanovdb::tools::createNanoGrid<SrcGridT, nanovdb::FpN>(*floatGrid, sMode, cMode, dither, verbose, nanovdb::tools::RelDiff(tolerance));
          }
        default: break;// 32 bit float grids are handled below
        }// end of switch
      }
      return nanovdb::tools::openToNanoVDB(base, sMode, cMode, verbose);// float and other grids
    };// openToNano

    if (fileName=="stdout.nvdb") {
      if (isatty(fileno(stdout)))  throw std::invalid_argument("writeNVDB: stdout is not connected to the terminal");
      if (mParser.verbose) mTimer.start("Streaming NanoVDB to stdout");
      for (auto grid: grids) {
        auto handle = openToNano(grid);
        nanovdb::io::writeGrid(std::cout, handle, codec);
      }
    } else {
      if (mParser.verbose) mTimer.start("Writing NanoVDB to file");
      std::ofstream os(fileName, std::ios::out | std::ios::binary);
      for (auto grid: grids) {
        auto handle = openToNano(grid);
        nanovdb::io::writeGrid(os, handle, codec);
      }
    }
    if (mParser.verbose) mTimer.stop();
  } catch (const std::exception& e) {
    throw std::invalid_argument(action_name+": "+e.what());
  }
}// Tool::writeNVDB
#else
void Tool::writeNVDB(const std::string&)
{
    throw std::runtime_error("NanoVDB support was disabled during compilation!");
}// Tool::writeNVDB
#endif

} // namespace vdb_tool
} // namespace OPENVDB_VERSION_NAME
} // namespace openvdb
