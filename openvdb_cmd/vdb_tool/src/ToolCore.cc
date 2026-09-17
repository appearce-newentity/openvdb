// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/// @file ToolCore.cc
/// @brief Tool lifecycle, logging, grid/geometry stack management, help text. Split out of Tool.h; see Tool.h for the class.

#include "Tool.h"

namespace openvdb {
OPENVDB_USE_VERSION_NAMESPACE
namespace OPENVDB_VERSION_NAME {
namespace vdb_tool {

// ==============================================================================================================

Tool::Tool(int argc, char *argv[])
    : mTimer(std::clog)
    , mCmdName(getBase(argv[0]))// name of executable
    , mRawCmdLine([&]{
        std::string s;
        for (int i = 0; i < argc; ++i) {
            if (i > 0) s += ' ';
            s += argv[i];
        }
        return s;
      }())
    , mParser({{"dim", "256", "256", "default grid resolution along the longest axis"},
               {"voxel", "0.0", "0.01", "default voxel size in world units. A value of zero indicates that dim is used to derive the voxel size."},
               {"width", "3.0", "3.0", "default narrow-band width of level sets in voxel units"},
               {"time", "1", "1|2|3", "default temporal discretization order"},
               {"space", "5", "1|2|3|5", "default spatial discretization order"},
               {"keep", "false", "1|0|true|false", "by default delete the input"}})
    , mErrorOnWarning(false)
    , mLogFile()
    , mOldClogBuffer(nullptr)
    , mOldCerrBuffer(nullptr)
    , mOldCoutBuffer(nullptr)
{
    openvdb::initialize();
    this->init();// fast: less than 1 ms
    try {
        mParser.finalize();
        // Set up centralized error handler for action execution
        mParser.onActionError = [this](const std::string&, const std::string&) {
            return mErrorOnWarning; // true = fatal (re-throw), false = skip
        };
        // Translate "vdb"/"geo" name tokens into stack ages on every read of those
        // options. Every option so named refers to an entry on one of the two stacks,
        // so no action needs to opt out of this.
        mParser.onGetOption = [this](const std::string &name, std::string &value) {
            if (name != "vdb" && name != "geo") return;
            value = this->resolveStackOption(name, value);
        };
        mParser.parse(argc, argv);// extremely fast, but might throw
    } catch (const std::exception& e) {
        this->endLog();
        throw std::invalid_argument(e.what());
    }
}// Tool::Tool

// ==============================================================================================================

void Tool::startLog(std::string logFile, bool append, bool tee)
{
    if (mOldClogBuffer != nullptr) return;// handles repeated calls
    if (logFile.empty()) logFile = "vdb_tool_" + dateStamp() + ".log";
    const auto mode = std::ios::out | (append ? std::ios::app : std::ios::trunc);
    mLogFile.open(logFile, mode);
    if (!mLogFile.is_open()) {
        throw std::invalid_argument("startLog: failed to open log file \"" + logFile + "\"");
    }
    // Unit-buffered output so `watch -n 0.5 vdb_tool.log` (and tail -f) see
    // each line as soon as it's written, instead of waiting for the 4 KB
    // block buffer to flush. The help text recommends this workflow.
    mLogFile.setf(std::ios::unitbuf);

    // Redirect all three text streams so warnings/errors (cerr) and any
    // stdout-bound output also land in the log — not just clog.
    mOldClogBuffer = std::clog.rdbuf();
    mOldCerrBuffer = std::cerr.rdbuf();
    mOldCoutBuffer = std::cout.rdbuf();
    if (mOldClogBuffer == nullptr || mOldCerrBuffer == nullptr || mOldCoutBuffer == nullptr) {
        throw std::invalid_argument("startLog: failed to cache standard stream buffers");
    }
    if (tee) {
        // Each TeeBuf fans output to (original terminal stream, log file) so
        // the user keeps live console feedback while the log accumulates.
        mClogTee = std::make_unique<TeeBuf>(mOldClogBuffer, mLogFile.rdbuf());
        mCerrTee = std::make_unique<TeeBuf>(mOldCerrBuffer, mLogFile.rdbuf());
        mCoutTee = std::make_unique<TeeBuf>(mOldCoutBuffer, mLogFile.rdbuf());
        std::clog.rdbuf(mClogTee.get());
        std::cerr.rdbuf(mCerrTee.get());
        std::cout.rdbuf(mCoutTee.get());
    } else {
        // Exclusive log mode (tee=false): nothing goes to the terminal.
        std::clog.rdbuf(mLogFile.rdbuf());
        std::cerr.rdbuf(mLogFile.rdbuf());
        std::cout.rdbuf(mLogFile.rdbuf());
    }

    // Self-describing log header — timestamp, vdb_tool version, and the full
    // command line. Makes a stored log readable days later without needing
    // to remember what was invoked. In append mode a blank line separates
    // this run's header from the previous run's output.
    if (append) mLogFile << "\n";
    mLogFile << "# vdb_tool log\n"
             << "# date     : " << dateStamp() << "\n"
             << "# version  : " << Tool::version() << "\n"
             << "# command  : " << mRawCmdLine << "\n"
             << std::flush;
}

// ==============================================================================================================

std::string Tool::resolveStackOption(const std::string &optName, const std::string &raw) const
{
    if (raw.empty() || raw == "*") return raw;// unchanged: every action already special-cases these

    const bool isVdb = optName == "vdb";
    const size_t stackSize = isVdb ? mGrid.size() : mGeom.size();
    const std::string &actionName = mParser.getAction().names[0];

    VecS out;
    for (const std::string &tok : tokenize(raw, "(),")) {
        int age;
        if (isInt(tok, age)) { out.push_back(tok); continue; }// already a stack age
        // Name lookup: scan age 0 (top/most-recent) upward, collecting every match.
        std::vector<int> matches;
        int a = 0;
        if (isVdb) {
            for (auto it = mGrid.crbegin(); it != mGrid.crend(); ++it, ++a)
                if ((*it)->getName() == tok) matches.push_back(a);
        } else {
            for (auto it = mGeom.crbegin(); it != mGeom.crend(); ++it, ++a)
                if ((*it)->getName() == tok) matches.push_back(a);
        }
        const std::string what = isVdb ? "VDB grid" : "Geometry";
        if (matches.empty()) {
            throw std::out_of_range(actionName + ": no " + what + " named \"" + tok +
                                    "\" on the stack (stack size " + std::to_string(stackSize) + ")");
        }
        // A name must identify exactly one entry. Expanding it to several ages would be
        // unsafe: most actions read "vdb"/"geo" as a single age via get<int>() and cannot
        // parse a list at all, and those that do read a list may erase entries by index
        // while consuming it, so a multi-age expansion can delete the wrong grid.
        if (matches.size() > 1) {
            std::string ages;
            for (size_t k = 0; k < matches.size(); ++k) ages += (k ? "," : "") + std::to_string(matches[k]);
            throw std::invalid_argument(actionName + ": the name \"" + tok + "\" is ambiguous -- it matches " +
                                        std::to_string(matches.size()) + " " + what + " entries at ages " +
                                        ages + ". Use an explicit age index to select one.");
        }
        out.push_back(std::to_string(matches[0]));
    }

    std::string result;
    for (size_t i = 0; i < out.size(); ++i) {
        if (i) result += ",";
        result += out[i];
    }
    return result;
}// Tool::resolveStackOption

// ==============================================================================================================

void Tool::run()
{
    if (mParser.verbose>1) this->print_args();
    try {
        mParser.run();
    } catch (const std::exception& e) {
        std::cerr << "Fatal error in Tool::run: " << e.what() << std::endl;
        std::exit(EXIT_FAILURE);
    }
}// Tool::run

// ==============================================================================================================

void Tool::warning(const std::string &msg, std::ostream& os) const
{
    if (mParser.verbose) {
        os << "\n" << std::setw(static_cast<int>(msg.size())) << std::setfill('*') << "\n" << msg
           << "\n" << std::setw(static_cast<int>(msg.size())) << std::setfill('*') << "\n";
    }
}// Tool::warning

// ==============================================================================================================

void Tool::help()
{
  OPENVDB_ASSERT(mParser.getAction().names[0] == "help");
  mParser.printAction();
  const VecS actions = mParser.getVec<std::string>("actions");
  const bool stop = mParser.get<bool>("exit");
  const bool brief = mParser.get<bool>("brief");
  const std::string format = mParser.get<std::string>("format");
  const std::string search = mParser.get<std::string>("search");

  if (!search.empty()) {
    // Discovery aid: list every action whose primary/alias name OR its
    // documentation contains the (case-insensitive) keyword. Detailed usage
    // is printed for each hit so the user sees the options too.
    const std::string key = toLowerCase(search);
    VecS hits;
    for (const auto &a : mParser.available) {
      bool match = contains(toLowerCase(a.documentation), key);
      for (const auto &n : a.names) match = match || contains(toLowerCase(n), key);
      if (match) hits.push_back(a.names[0]);
    }
    if (hits.empty()) {
      std::clog << "help: no action matches keyword \"" << search << "\"\n";
    } else {
      std::clog << "Actions matching \"" << search << "\":\n";
      mParser.usage(hits, brief);
    }
    if (stop) std::exit(EXIT_SUCCESS);
    return;
  }

  if (format == "md") {
    // Emit a single Markdown table of registered actions for the README's
    // big "action list" section. Sorted alphabetically by primary name
    // (matches the order Parser::finalize() establishes). Generated output
    // should be diffed against README.md so it can't silently drift.
    std::clog << "| Action | Description |\n";
    std::clog << "|---|---|\n";
    for (const auto &a : mParser.available) {
      std::string desc = a.documentation;
      // Markdown table cells can't contain raw newlines or unescaped '|'.
      for (char &c : desc) if (c == '\n' || c == '\r') c = ' ';
      size_t bar = 0;
      while ((bar = desc.find('|', bar)) != std::string::npos) {
        desc.replace(bar, 1, "\\|");
        bar += 2;
      }
      std::clog << "| **" << a.names[0] << "**";
      if (a.names.size() > 1) {
        std::clog << " (alias" << (a.names.size() > 2 ? "es" : "") << ": ";
        for (size_t i = 1; i < a.names.size(); ++i) std::clog << (i>1 ? ", " : "") << a.names[i];
        std::clog << ")";
      }
      std::clog << " | " << desc << " |\n";
    }
    if (stop) std::exit(EXIT_SUCCESS);
    return;
  }

  if (actions.empty()) {
    if (mParser.actions.size()==1) {// ./vdb_tool -help
      if (!brief) {
        std::clog << "\nThis command-line tool can perform a use-defined, and possibly\n"
                  << "non-linear, sequence of high-level tasks available in openvdb.\n"
                  << "For instance, it can convert polygon meshes and particles to level\n"
                  << "sets, and subsequently perform a large number of operations on these\n"
                  << "level set surfaces. It can also generate adaptive polygon meshes from\n"
                  << "level sets, write them to disk and even render them to image files.\n\n"
                  << "Version: " + Tool::version() + "\n" + this->examples() + "\n";
      }
      mParser.usage_all(brief);
      if (!brief) {
        std::clog << "\nNote that actions always start with one or more \"-\", and (except for file names)\n"
                  << "its options always contain a \"=\" and an optional number of characters\n"
                  << "used for identification, e.g. \"-erode r=2\" is identical to \"-erode radius=2.0\"\n"
                  << "but \"-erode rr=2\" will produce an error since \"rr\" does not match\n"
                  << "the first two character of \"radius\". Also note that this tool maintains two\n"
                  << "lists of primitives, namely geometry (i.e. points and meshes) and level sets.\n"
                  << "They can be referenced with \"geo=n\" and \"vdb=n\" where the integer \"n\" refers\n"
                  << "to age (i.e stack index) of the primitive with \"n=0\" meaning most recent. E.g.\n"
                  << "-mesh2ls g=1\" means convert the second to last geometry (here polygon mesh) to a\n"
                  << "level set. Likewise \"-gauss v=0\" means perform a gaussian filter on the most\n"
                  << "recent level set (default).\n";
      }
    } else {// e.g. ./vdb_tool -sphere -dilate -help
      mParser.usage(brief);
    }
  } else {// ./vdb_tool -help sphere dilate
    mParser.usage(actions, brief);
  }

  if (stop) std::exit(EXIT_SUCCESS);
}// Tool::help()

// ==============================================================================================================

std::string Tool::examples() const
{
    const int w = 16;
    std::stringstream ss;
    ss << std::left << std::setw(w) << "Surface points:" << mCmdName << " -read points.[obj/ply/stl/off/pts] -points2ls d=256 r=2.0 w=3 -dilate r=2 -gauss i=1 w=1 -erode r=2 -ls2m a=0.25 -write output.[ply/obj/stl]\n";
    ss << std::left << std::setw(w) << "Convert mesh:  " << mCmdName << " -read mesh.[ply/obj/off] -mesh2ls d=256 -write output.vdb config.txt\n";
    ss << std::left << std::setw(w) << "Config example:" << mCmdName << " -config config.txt\n";
    return ss.str();
}

// ==============================================================================================================

void Tool::clear()
{
  OPENVDB_ASSERT(mParser.getAction().names[0] == "clear");
  // Resolve every requested age to a stable list iterator BEFORE erasing anything.
  // Erasing inside the loop instead would (a) leave earlier deletions in place when
  // a later age turns out to be out of range, and (b) shift the remaining entries,
  // since an age is a distance from the back of the list -- so "-clear vdb=0,2" on a
  // three-grid stack erased age 0 and then failed on age 2, having already destroyed
  // a grid. Duplicate ages are removed first: resolving one twice would yield the
  // same iterator twice, and erasing it twice is undefined behaviour. This is safe
  // because mGrid/mGeom are std::lists, whose iterators stay valid when *other*
  // elements are erased.
  auto resolveVictims = [](VecI ages, auto &stack, auto &&get) {
    std::sort(ages.begin(), ages.end());
    ages.erase(std::unique(ages.begin(), ages.end()), ages.end());
    std::vector<typename std::decay_t<decltype(stack)>::const_iterator> victims;
    victims.reserve(ages.size());
    for (int a : ages) victims.push_back(std::next(get(a)).base());// throws if out of range
    return victims;
  };
  if (mParser.get<std::string>("geo") == "*") {
    mGeom.clear();
  } else {
    const auto victims = resolveVictims(mParser.getVec<int>("geo"), mGeom,
                                        [this](int a){ return this->getGeom(a); });
    for (auto it : victims) mGeom.erase(it);
  }
  if (mParser.get<std::string>("vdb")  == "*") {
    mGrid.clear();
  } else {
    const auto victims = resolveVictims(mParser.getVec<int>("vdb"), mGrid,
                                        [this](int a){ return this->getGrid(a); });
    for (auto it : victims) mGrid.erase(it);
  }
  if (mParser.get<bool>("variables")) {
    mParser.processor.memory().clear();
  }
}// Tool::clear

// ==============================================================================================================

void Tool::copy()
{
  OPENVDB_ASSERT(mParser.getAction().names[0] == "copy");
  mParser.printAction();
  const std::string vdb_str = mParser.get<std::string>("vdb");
  const std::string geo_str = mParser.get<std::string>("geo");
  const std::string prefix  = mParser.get<std::string>("prefix");
  if (vdb_str.empty() && geo_str.empty()) {
    throw std::invalid_argument("copy: at least one of \"vdb\" or \"geo\" must be specified");
  }
  auto applyPrefix = [&](const std::string& name) { return prefix + name; };
  if (!vdb_str.empty()) {
    std::vector<GridBase::Ptr> copies;
    if (vdb_str == "*") {
      for (auto it = mGrid.crbegin(); it != mGrid.crend(); ++it)
        copies.push_back((*it)->deepCopyGrid());
    } else {
      const auto indices = mParser.getVec<int>("vdb");
      for (int a : indices) {
        if (size_t(a) >= mGrid.size()) {
          throw std::out_of_range("copy: vdb index " + std::to_string(a) +
                                  " is out of range (stack size " + std::to_string(mGrid.size()) + ")");
        }
      }
      copies.reserve(indices.size());
      for (int a : indices) copies.push_back((*this->getGrid(a))->deepCopyGrid());
    }
    for (auto& c : copies) {
      if (!prefix.empty()) c->setName(applyPrefix(c->getName()));
      mGrid.push_back(c);
    }
  }
  if (!geo_str.empty()) {
    std::vector<Geometry::Ptr> copies;
    if (geo_str == "*") {
      for (auto it = mGeom.crbegin(); it != mGeom.crend(); ++it)
        copies.push_back((*it)->deepCopy());
    } else {
      const auto indices = mParser.getVec<int>("geo");
      for (int a : indices) {
        if (size_t(a) >= mGeom.size()) {
          throw std::out_of_range("copy: geo index " + std::to_string(a) +
                                  " is out of range (stack size " + std::to_string(mGeom.size()) + ")");
        }
      }
      copies.reserve(indices.size());
      for (int a : indices) copies.push_back((*this->getGeom(a))->deepCopy());
    }
    for (auto& c : copies) {
      if (!prefix.empty()) c->setName(applyPrefix(c->getName()));
      mGeom.push_back(c);
    }
  }
}// Tool::copy

// ==============================================================================================================

void Tool::rename()
{
  OPENVDB_ASSERT(mParser.getAction().names[0] == "rename");
  mParser.printAction();
  const std::string vdb_str  = mParser.get<std::string>("vdb");
  const std::string geo_str  = mParser.get<std::string>("geo");
  const std::string new_name = mParser.get<std::string>("name");
  if (vdb_str.empty() && geo_str.empty()) {
    throw std::invalid_argument("rename: at least one of \"vdb\" or \"geo\" must be specified");
  }
  if (new_name.empty()) {
    throw std::invalid_argument("rename: \"name\" must not be empty");
  }
  if (!vdb_str.empty()) {
    const int a = mParser.get<int>("vdb");
    if (size_t(a) >= mGrid.size()) {
      throw std::out_of_range("rename: vdb index " + std::to_string(a) +
                              " is out of range (stack size " + std::to_string(mGrid.size()) + ")");
    }
    (*this->getGrid(a))->setName(new_name);
  }
  if (!geo_str.empty()) {
    const int a = mParser.get<int>("geo");
    if (size_t(a) >= mGeom.size()) {
      throw std::out_of_range("rename: geo index " + std::to_string(a) +
                              " is out of range (stack size " + std::to_string(mGeom.size()) + ")");
    }
    (*this->getGeom(a))->setName(new_name);
  }
}// Tool::rename

// ==============================================================================================================

void Tool::swap()
{
  OPENVDB_ASSERT(mParser.getAction().names[0] == "swap");
  mParser.printAction();
  const std::string vdb_str = mParser.get<std::string>("vdb");
  const std::string geo_str = mParser.get<std::string>("geo");
  if (vdb_str.empty() && geo_str.empty()) {
    throw std::invalid_argument("swap: at least one of \"vdb\" or \"geo\" must be specified");
  }
  if (!vdb_str.empty()) {
    const auto idx = mParser.getVec<int>("vdb");
    if (idx.size() != 2) {
      throw std::invalid_argument("swap: \"vdb\" requires exactly two indices, e.g. vdb=0,1");
    }
    if (size_t(idx[0]) >= mGrid.size() || size_t(idx[1]) >= mGrid.size()) {
      throw std::out_of_range("swap: vdb index out of range (stack size " + std::to_string(mGrid.size()) + ")");
    }
    if (idx[0] != idx[1]) {
      auto it0 = mGrid.rbegin(); std::advance(it0, idx[0]);
      auto it1 = mGrid.rbegin(); std::advance(it1, idx[1]);
      std::iter_swap(it0, it1);
    }
  }
  if (!geo_str.empty()) {
    const auto idx = mParser.getVec<int>("geo");
    if (idx.size() != 2) {
      throw std::invalid_argument("swap: \"geo\" requires exactly two indices, e.g. geo=0,1");
    }
    if (size_t(idx[0]) >= mGeom.size() || size_t(idx[1]) >= mGeom.size()) {
      throw std::out_of_range("swap: geo index out of range (stack size " + std::to_string(mGeom.size()) + ")");
    }
    if (idx[0] != idx[1]) {
      auto it0 = mGeom.rbegin(); std::advance(it0, idx[0]);
      auto it1 = mGeom.rbegin(); std::advance(it1, idx[1]);
      std::iter_swap(it0, it1);
    }
  }
}// Tool::swap

} // namespace vdb_tool
} // namespace OPENVDB_VERSION_NAME
} // namespace openvdb
