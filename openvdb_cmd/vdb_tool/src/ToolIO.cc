// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/// @file ToolIO.cc
/// @brief Reading and writing VDB, geometry and config files. Split out of Tool.h; see Tool.h for the class.
///        NanoVDB read/write lives in ToolNanoVDB.cc.

#include "Tool.h"
#include <openvdb/io/Archive.h>
#include <openvdb/io/Compression.h>
#include <openvdb/io/File.h>
#include <openvdb/io/Stream.h>
#ifdef VDB_TOOL_USE_PDAL
#include <pdal/pdal.hpp>
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

void Tool::read()
{
  OPENVDB_ASSERT(mParser.getAction().names[0] == "read");
  for (auto &fileName : mParser.getVec<std::string>("files")) {
    switch (findFileExt(fileName, {"geo,obj,ply,abc,pts,off,stl,xyz,usd,usda,usdc,usdz,gltf,glb", "vdb", "nvdb"})) {
    case 1:
      this->readGeo(fileName);
      break;
    case 2:
      this->readVDB(fileName);
      break;
    case 3:
      this->readNVDB(fileName);
      break;
    default:
#if VDB_TOOL_USE_PDAL
      pdal::StageFactory factory;
      if (factory.inferReaderDriver(fileName) != "") {
        this->readGeo(fileName);
        break;
      }
#endif
      throw std::invalid_argument("File \""+fileName+"\" has an invalid extension");
    }// end switch
  }// end for loop over files
}// Tool::read

// ==============================================================================================================

void Tool::readGeo(const std::string &fileName)
{
  OPENVDB_ASSERT(mParser.getAction().names[0] == "read");
  if (mParser.verbose>1) std::clog << "Reading geometry from \"" << fileName << "\"\n";
  if (mParser.verbose) mTimer.start("Read geometry file \"" + fileName + "\"");
  Geometry::Ptr geom(new Geometry());
  geom->read(fileName, mParser.verbose);
  if (geom->vtxCount()) {
    geom->setName(getBase(fileName));
    mGeom.push_back(geom);
  }
  if (mParser.verbose) {
    mTimer.stop();
    if (mParser.verbose>1) geom->print();
  }
}// Tool::readGeo

// ==============================================================================================================

void Tool::readVDB(const std::string &fileName)
{
  OPENVDB_ASSERT(mParser.getAction().names[0] == "read");
  const VecS gridNames = mParser.getVec<std::string>("grids");
  if (gridNames.empty()) throw std::invalid_argument("readVDB: no grids names specified");
  GridPtrVecPtr grids;
  if (fileName=="stdin.vdb") {
    if (isatty(fileno(stdin))) throw std::invalid_argument("readVDB: stdin is not connected to the terminal!");
    if (mParser.verbose) mTimer.start("Reading VDB grid(s) from input stream");
    io::Stream s(std::cin);
    grids = s.getGrids();
  } else {
    if (mParser.verbose) mTimer.start("Reading VDB grid(s) from file named \""+fileName+"\"");
    io::File file(fileName);
    file.open(mParser.get<bool>("delayed"));
    grids = file.getGrids();
  }
  const size_t count = mGrid.size();
  if (grids) {
    for (GridBase::Ptr grid : *grids) {
      if (gridNames[0]=="*" || findMatch(grid->getName(), gridNames)) mGrid.push_back(grid);
    }
  } else if (mParser.verbose) {
    std::clog << "readVDB: no vdb grids in \"" << fileName << "\"";
  }
  if (mParser.verbose) {
    mTimer.stop();
    if (mGrid.size() == count) std::clog << "readVDB: no vdb grids were loaded\n";
    if (mParser.verbose>1) for (GridBase::Ptr grid : *grids) grid->print();
  }
}// Tool::readVDB

// ==============================================================================================================

void Tool::config()
{
    OPENVDB_ASSERT(mParser.getAction().names[0] == "config");
    const bool update  = mParser.get<bool>("update");
    const bool execute = mParser.get<bool>("execute");
    std::string line;
    for (auto &fileName : mParser.getVec<std::string>("files")) {
        if (update) {
            std::fstream file(fileName, std::fstream::in | std::fstream::out);
            if (!file.is_open() || !getline (file, line)) throw std::invalid_argument("updateConf: failed to open file \""+fileName+"\"");
            const Header old_header(line), new_header;
            if (!old_header.isCompatible()) {
                std::stringstream ss;
                ss << new_header.str() << std::endl;
                ss << file.rdbuf();// load the rest of the config file
                file.clear();
                file.seekg(0);// rewind to start
                file << ss.rdbuf();// write back to the config file
            }
            file.close();
        }
        if (execute) {
            std::ifstream file(fileName);
            if (!file.is_open()) throw std::invalid_argument("readConf: unable to open \""+fileName+"\"");
            if (mParser.verbose>1) std::clog << "Reading configuration from \"" << fileName << "\"\n";
            if (mParser.verbose) mTimer.start("Read config file \"" + fileName + "\"");
            if (!getline (file,line)) throw std::invalid_argument("readConf: empty file \""+fileName+"\"");
            Header header(line);
            if (!header.isCompatible()) throw std::invalid_argument("readConf: incompatible version \""+line+"\"");
            std::vector<char*> args({&header.mMagic[0]});//parser is expecting first argument to the name of the executable
            std::string accum;
            while (getline(file, line)) {
                const size_t start = line.find_first_not_of(" \t"), stop = line.find_first_of("#%");
                // A blank or comment-only line is skipped without touching accum, so a
                // comment line in the middle of a backslash continuation is transparently
                // absorbed rather than breaking (or being appended to) the continued line.
                if (start >= stop) continue;// line is empty or starts with a comment
                line = line.substr(start, stop - start);// remove leading whitespaces and tailing comments
                line = line.substr(0, line.find_last_not_of(" \t") + 1);// remove tailing whitespaces
                if (!line.empty() && line.back() == '\\') {
                    accum += line.substr(0, line.size() - 1);// strip \ and accumulate
                    accum += ' ';// separate this line's tokens from the next line's
                    continue;
                }
                line = accum + line;
                accum.clear();
                VecS tmp = vdb_tool::tokenize(line, " ");
                tmp[0].insert (0, 1, '-');// first token is an action
                std::transform(tmp.begin(), tmp.end(), std::back_inserter(args), [](const std::string &s){
                    char *c = new char[s.size()+1];
                    std::strcpy(c, s.c_str());
                    return c;
                });
            }
            if (!accum.empty()) {
                throw std::invalid_argument("readConf: unterminated line continuation at end of file \""+fileName+"\"");
            }
            file.close();
            mParser.parse(static_cast<int>(args.size()), args.data());
            if (mParser.verbose) mTimer.stop();
        }
    }
}// Tool::config

// ==============================================================================================================

void Tool::write()
{
  OPENVDB_ASSERT(mParser.getAction().names[0] == "write");
  for (std::string &fileName : mParser.getVec<std::string>("files")) {
    switch (findFileExt(fileName, {"geo,obj,ply,stl,off,abc", "vdb", "nvdb", "txt"})) {
    case 1:
      this->writeGeo(fileName);
      break;
    case 2:
      this->writeVDB(fileName);
      break;
    case 3:
      this->writeNVDB(fileName);
      break;
    case 4:
      this->writeConf(fileName);
      break;
    default:
      throw std::invalid_argument("File \""+fileName+"\" has an invalid extension");
      break;
    }
  }
}// Tool::write

// ==============================================================================================================

void Tool::writeVDB(const std::string &fileName)
{
  OPENVDB_ASSERT(mParser.getAction().names[0] == "write");
  try {
    mParser.printAction();
    const std::string age = mParser.get<std::string>("vdb");
    const bool keep = mParser.get<bool>("keep");
    const std::string codec = toLowerCase(mParser.get<std::string>("codec"));
    bool half;
    switch (mParser.get<int>("bits")) {
      case 16:
        half = true; break;
      case 32:
        half = false; break;
      default:
        throw std::invalid_argument("writeVDB: bits should either be 32 or 16, not "+mParser.get<std::string>("bits"));
    }
    GridPtrVec grids;// vector of grids to be written and possibly removed from mGrid
    if (age == "*") {
      for (auto it = mGrid.crbegin(); it != mGrid.crend(); ++it) grids.push_back(*it);
      if (!keep) mGrid.clear();
    } else {
      for (int a : vectorize<int>(age, ",")) grids.push_back(*this->getGrid(a));
      if (!keep) for (auto &g : grids) mGrid.remove(g);
    }

    if (grids.empty()) throw std::invalid_argument("no vdb grids to write");

    auto setCodec = [&](io::Archive &a) {
      if (codec=="zip") {
        a.setCompression(io::COMPRESS_ZIP | io::COMPRESS_ACTIVE_MASK);
      } else if (codec=="blosc") {
        a.setCompression(io::COMPRESS_BLOSC | io::COMPRESS_ACTIVE_MASK);
      } else if (codec=="active") {
        a.setCompression(io::COMPRESS_ACTIVE_MASK);
      } else if (codec=="none") {
        a.setCompression(io::COMPRESS_NONE);
      } else if (!codec.empty()) {
        throw std::invalid_argument("writeVDB: unsupported codec \""+codec+"\"");
      }
    };
    for (size_t i=0; half && i<grids.size(); ++i) grids[i]->setSaveFloatAsHalf(true);
    if (fileName=="stdout.vdb") {
      if (isatty(fileno(stdout)))  throw std::invalid_argument("writeVDB: stdout is not connected to the terminal");
      if (mParser.verbose) mTimer.start("Streaming VDB grid(s) to output stream");
      io::Stream stream(std::cout);
      setCodec(stream);
      stream.write(grids);
    } else {
      if (mParser.verbose) mTimer.start("Writing VDB grid(s) to file named \""+fileName+"\"");
      io::File file(fileName);
      setCodec(file);
      file.write(grids);
      file.close();
    }
    for (size_t i=0; half && i<grids.size(); ++i) grids[i]->setSaveFloatAsHalf(false);
    if (mParser.verbose) mTimer.stop();
  } catch (const std::exception& e) {
    throw std::invalid_argument(std::string("writeVDB: ") + e.what());// catch in Tool::write
  }
}// Tool::writeVDB

// ==============================================================================================================

void Tool::writeGeo(const std::string &fileName)
{
  OPENVDB_ASSERT(mParser.getAction().names[0] == "write");
  const int age = mParser.get<int>("geo");
  const bool keep = mParser.get<bool>("keep");
  const bool ascii = mParser.get<bool>("ascii");
  if (mParser.verbose>1) std::clog << "Writing geometry to \"" << fileName << "\"\n";
  auto it = this->getGeom(age);
  if (mParser.verbose) mTimer.start("Write geometry file \"" + fileName + "\"");
  (*it)->write(fileName, ascii);
  if (!keep) mGeom.erase(std::next(it).base());
  if (mParser.verbose) mTimer.stop();
}// Tool::writeGeo

// ==============================================================================================================

void Tool::writeConf(const std::string &fileName)
{
  OPENVDB_ASSERT(mParser.getAction().names[0] == "write");
  if (mParser.verbose>1) std::clog << "Writing configuration to \"" << fileName << "\"\n";
  std::ofstream file(fileName);
  if (!file.is_open()) throw std::invalid_argument("writeConf: unable to open \""+fileName+"\"");
  if (mParser.verbose) mTimer.start("Write config file \"" + fileName + "\"");
  const Header header;
  file << header.str() << std::endl;
  for (auto &a : mParser.actions) if (a.names[0] != "config") a.print(file);// exclude "-config" to avoid infinite loop
  file.close();
  if (mParser.verbose) mTimer.stop();
}// Tool::writeConf

} // namespace vdb_tool
} // namespace OPENVDB_VERSION_NAME
} // namespace openvdb
