// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/// @file ToolInit.cc
/// @brief Tool::init(): registration of every command-line action. Split out of Tool.h; see Tool.h for the class.

#include "Tool.h"

namespace openvdb {
OPENVDB_USE_VERSION_NAMESPACE
namespace OPENVDB_VERSION_NAME {
namespace vdb_tool {

// ==============================================================================================================

void Tool::init()
{
  // note, the following actions were added when mParser was constructed: -quiet,-verbose,-debug,-default,-for,-each,-end

  //  mParser.addAction({"name",.. "alias"}, "documentation of action",
  //                    {{"option name", "default value", "expected values", "documentation of option"}},
  //                     {more options}...});
  mParser.addAction(
     {"config", "c"}, "Import and process one or more configuration files",
    {{"files", "",  "config1.txt,config2.txt...", "list of configuration files to load and execute"},
     {"execute", "true", "1|0|true|false", "toggle wether to execute the actions in the config file"},
     {"update", "false", "1|0|true|false", "toggle wether to update the version number of the config file"}},
     [&](){this->config();}, [](){}, 0); // anonymous options are appended to "files"

  mParser.addAction(
     {"help", "h"}, "Print documentation for one, multiple or all available actions",
    {{"actions", "", "read,write,...", "list of actions to document. If the list is empty documentation is printed for all available actions and if other actions proceed this action, documentation is printed for those actions only"},
     {"exit", "true", "1|0|true|false", "toggle wether to terminate after this action or not"},
     {"brief", "false", "1|0|true|false", "toggle brief or detailed documentation"},
     {"search", "", "mesh", "case-insensitive keyword: list every action whose name or documentation contains it (e.g. search=mesh). Useful for discovering the right action without scrolling the full help."},
     {"format", "text", "text|md", "output format. 'text' (default) is the usual human-readable help; 'md' emits a single Markdown table of action names + descriptions, used to regenerate the action list in README.md so it can't drift from the registered actions."}},
     [](){}, [&](){this->help();}, 0); // anonymous options are appended to "actions"

  mParser.addAction(
     {"read", "import", "load", "i"}, "Read one or more geometry or VDB files from disk or STDIN.",
    {{"files", "", "{file|stdin}.{obj|ply|abc|stl|off|pts|xyz|e57|vdb|nvdb|gltf|glb|geo|usd|usda|usdc|usdz}", "list of files or the input stream, e.g. file.vdb,stdin.vdb. Note that \"files=\" is optional since any argument without \"=\" is intrepreted as a file and appended to \"files\""},
     {"grids", "*", "*|grid_name,...", "list of VDB grids name to be imported (defaults to \"*\", i.e. import all available grids)"},
     {"delayed", "true", "1|0|true|false", "toggle delayed loading of VDB grids (enabled by default). This option is ignored by other file types"}},
     [](){}, [&](){this->read();}, 0);//  anonymous options are treated as to the first option,i.e. "files"

  mParser.addAction(
     {"write", "export", "save", "o"}, "Write list of geometry, VDB or config files to disk or STDOUT",
    {{"files", "", "{file|stdout}.{obj|ply|stl|off|geo|abc|vdb|nvdb|txt}", "list of files or the output stream, e.g. file.vdb or stdin.vdb. Note that \"files=\" is optional since any argument without the \"=\" character is intrepreted as a file and appended to \"files\"."},
     {"geo", "0", "0|1...", "geometry to write (defaults to \"0\" which is the latest)."},
     {"vdb", "*", "0,1,...", "list of VDB grids to write (defaults to \"*\", i.e. all available grids)."},
     {"keep", "", "1|0|true|false", "toggle wether to preserved or deleted geometry and grids after they have been written."},
     {"codec", "", "none|zip|blosc|active", "compression codec for the file or stream"},
     {"bits", "32", "32|16|8|4|N", "bit-width of floating point numbers during quantization of VDB and NanoVDB grids, i.e. 32 is full, 16, is half (defaults to 32). NanoVDB also supports 8, 4 and N which is adaptive bit-width"},// VDB: 32, 16 + for NVDB 8, 4 or N
     {"dither", "false", "1|0|true|false", "toggle dithering of quantized NanoVDB grids (disabled by default)"},
     {"absolute", "true", "1|0|true|false", "toggle absolute or relative error tolerance during quantization of NanoVDBs. Only used if bits=N. Defaults to absolute"},// absolute or relative error for N bits in NVDB
     {"tolerance", "-1", "1.0", "absolute or relative error tolerance used during quantization of NanoVDBs. Only used if bits=N."},// error tolerance for N bits in NVDB
     {"stats", "", "none|bbox|extrema|all", "specify the statistics to compute for NanoVDBs."},
     {"ascii", "false", "1|0|true|false", "for ascii vs binary output format when available (e.g. for ply files). Defaults to false, i.e. binary is preferred over ascii when available"},
     {"checksum", "", "none|partial|full", "specify the type of checksum to compute for NanoVDBs"}},
     [&](){mParser.setDefaults();}, [&](){this->write();}, 0);// anonymous options are treated as to the first option,i.e. "files"

  mParser.addAction(
     {"rename"}, "Rename a VDB grid and/or Geometry on the stack by age index",
    {{"vdb",  "", "0",      "age index of the VDB grid to rename (omit to skip VDB)"},
     {"geo",  "", "0",      "age index of the Geometry to rename (omit to skip Geometry)"},
     {"name", "", "sphere", "new name to assign"}},
     [](){}, [&](){this->rename();});

  mParser.addAction(
     {"swap"}, "Swap two VDB grids and/or two Geometry entries on their respective stacks",
    {{"vdb", "", "0,1", "comma-separated pair of age indices of VDB grids to swap (omit to skip VDB)"},
     {"geo", "", "0,1", "comma-separated pair of age indices of Geometry to swap (omit to skip Geometry)"}},
     [](){}, [&](){this->swap();});

  mParser.addAction(
     {"copy"}, "Deep-copy VDB grids and/or Geometry by index onto the top of their respective stacks",
    {{"vdb",    "", "0|0,1,2|*", "comma-separated age index/indices of VDB grids to copy, or \"*\" for all (omit to skip VDB)"},
     {"geo",    "",  "0|0,1|*",  "comma-separated age index/indices of Geometry to copy, or \"*\" for all (omit to skip Geometry)"},
     {"prefix", "", "copy_",     "prefix prepended to the name of each copy (default is empty, preserving the original name)"}},
     [&](){mParser.setDefaults();}, [&](){this->copy();});

  mParser.addAction(
     {"clear"}, "Deletes geometry, VDB grids and local variables",
    {{"geo", "*", "*|0,1,...", "list of geometries to delete (defaults to all)"},
     {"vdb", "*", "*|0,1,...", "list of VDB grids to delete (defaults to all)"},
     {"variables", "0", "1|0|true|false", "clear all the local variables (defaults to off)"}},
     [](){}, [&](){this->clear();});

  mParser.addAction(
     {"sphere"}, "Create a level set sphere, i.e. a narrow-band signed distance to a sphere",
    {{"dim", "", "256", "largest dimension in voxel units of the sphere (defaults to 256). If \"voxel\" is defined \"dim\" is ignored"},
     {"voxel", "", "0.0", "voxel size in world units (by defaults \"dim\" is used to derive \"voxel\"). If specified this option takes precedence over \"dim\". Defaults to 0.0, i.e. this option is disabled"},
     {"radius", "1.0", "1.0", "radius of sphere in world units"},
     {"center", "(0,0,0)", "(0.0,0.0,0.0)", "center of sphere in world units"},
     {"signed", "true", "1|0|true|false", "toggle wether the output volume should be a signed vs unsigned distance field"},
     {"width", "", "3.0", "half-width in voxel units of the output narrow-band level set (defaults to 3 units on either side of the zero-crossing)"},
     {"name", "sphere", "sphere", "name assigned to the level set sphere"}},
     [&](){mParser.setDefaults();}, [&](){this->levelSetSphere();});

  // Register forAllValues/forOnValues/forOffValues in a loop to prevent drift
  // (they share identical options and implementation)
  const std::vector<std::pair<std::vector<std::string>, std::string>> forValuesVariants = {
      {{"forAllValues"}, "Applied a simple computational kernel to ALL values in a grid."},
      {{"forOnValues"}, "Applied a simple computational kernel to ON values in a grid."},
      {{"forOffValues"}, "Applied a simple computational kernel to OFF values in a grid."}
  };
  const std::vector<Option> forValuesOptions = {
      {"keep", "", "1|0|true|false", "toggle wether the input volume is preserved or deleted after the conversion"},
      {"vdb", "0", "0|0,1", "age(s) (i.e. stack index) of grid(s) to be processed. Defaults to 0, i.e. most recently inserted VDB. Accepts a comma-separated list to use multiple grids in the kernel: the FIRST grid is written (iterated), the rest are read-only inputs. Length must match use=."},
      {"kernel", "", "sin(v)+2*v*v", "user-defined math expression to apply to each value. The \"kernel=\" prefix is OPTIONAL; the kernel may also be supplied as a bare positional argument, e.g. \"-forOnValues 'sin(v)+1'\" or \"-forOnValues 'sin(v)+1' keep=true\" — other named options of the same action still parse normally. Supports infix (e.g. \"sin(v)+2*v*v\"), RPN (e.g. \"$v:sin:$v:pow2:2:*:+\"), and infix multi-statement programs with assignment (e.g. \"t = v*v; t + sin(t)\"). The variable that holds the current voxel value is configurable via the \"use\" option (defaults to \"v\"). Stencil kernels: write \"v(dx,dy,dz)\" with integer-literal offsets to read a relative neighbor voxel through a per-thread ConstAccessor (e.g. \"v(1,0,0)-v(-1,0,0)\" computes a finite-difference x-derivative). The grid is internally deep-copied so reads come from a stable snapshot. Any other identifier in the kernel is looked up once in the Processor's string memory (the same namespace used by -eval / -calc) and used as a per-voxel constant — kernels like \"a*v + b\" therefore require -eval / -calc to have set \"a\" and \"b\" beforehand, else an error is thrown. An empty kernel is a no-op."},
      {"use", "v", "v|x|x,y", "name(s) of the kernel variable(s) bound to the voxel value(s) of the input grid(s). Defaults to \"v\". Accepts a comma-separated list matching vdb=: use=x,y vdb=0,1 makes \"x\" the output grid and \"y\" a read-only input. Each name is excluded from the Processor-memory lookup and may be called as a function (e.g. \"x(1,0,0)\") to read a relative neighbor through a per-thread ConstAccessor."},
      {"class", "", "ls", "class label of the output volume."},
      {"background", "", "1.5,2.0", "background value(s) of the output volume. If two values are provided they are assumed to be outside, inside"},
      {"name", "", "foo-bar", "name assigned to the output volume"},
      {"file", "", "prog.txt", "read the kernel from a file instead of the \"kernel\" option (useful for longer kernels). If both are given, \"file\" takes precedence."}
  };
  for (const auto& variant : forValuesVariants) {
      mParser.addAction(
          std::vector<std::string>(variant.first), std::string(variant.second),
          std::vector<Option>(forValuesOptions),
          [&](){mParser.setDefaults();}, [&](){this->forValues();},
          /*anonymous=*/2, /*greedy=*/true);
  }

#ifdef VDB_TOOL_USE_AX
  mParser.addAction(
     {"ax"}, "run an OpenVDB AX expression over selected grids (requires the openvdb_ax library and LLVM)",
    {{"vdb", "*", "*|0|0,1", "age(s) (i.e. stack index) of grid(s) to process, or \"*\" for all (default). AX volume code references grids by name via @gridname, so all selected grids are passed to the executable together and may be read or written."},
     {"keep", "", "1|0|true|false", "toggle whether the input grid(s) are preserved. By default (keep=false) the selected grids are edited in place; keep=true instead deep-copies each selected grid, runs AX on the copies (which are pushed onto the stack), and leaves the originals untouched."},
     {"code", "", "@v += 1;", "AX code snippet to parse, compile and execute. The \"code=\" prefix is OPTIONAL; the snippet may be given as a bare positional argument, e.g. -ax '@density += 1;'. See the OpenVDB AX documentation for the language."},
     {"file", "", "prog.ax", "read the AX code from a file instead of the \"code\" option (useful for longer programs). If both are given, \"file\" takes precedence."},
     {"bindings", "", "x:density,y:temp", "comma-separated axname:gridname pairs that remap AX attribute/grid names to actual grid names, e.g. bindings=x:density makes @x in the code operate on the grid named \"density\". Lets a kernel be written against generic names and retargeted without editing the code."}},
     [&](){mParser.setDefaults();}, [&](){this->ax();},
     /*anonymous=*/2, /*greedy=*/true);// code (index 2) contains spaces/';'/'='; accept bare "@v+=1;" alongside code='...'
#endif

  mParser.addAction(
     {"sdf2udf"}, "Converts a signed distance field into an unsigned distance field, i.e. performs the Abs of all values and changes GridClass to UNKNOWN.",
    {{"keep", "", "1|0|true|false", "toggle wether the input volume is preserved or deleted after the conversion"},
     {"vdb", "0", "0|0,1", "age(s) (i.e. stack index) of grid(s) to be processed. Defaults to 0, i.e. most recently inserted VDB. Accepts a comma-separated list to use multiple grids in the kernel: the FIRST grid is written (iterated), the rest are read-only inputs. Length must match use=."},
     {"name", "sphere", "sphere", "name assigned to the output volume"}},
     [&](){mParser.setDefaults();}, [&](){this->sdf2udf();});

  mParser.addAction(
     {"quad2tri", "q2t"}, "Convert all quads in mesh to triangles, assuming they are both planar and convex",
    {{"geo", "0", "0", "age (i.e. stack index) of the geometry to be processed. Defaults to 0, i.e. most recently inserted geometry."},
     {"keep", "", "1|0|true|false", "toggle wether the input geometry is preserved or deleted after the conversion"}},
     [&](){mParser.setDefaults();}, [&](){this->quadsToTriangles();});

  mParser.addAction(
     {"movie", "img2mpeg", "mov2mpeg", "mov2gif", "img2gif"}, "Convert image and movie files to mpeg or animated gif files",
    {{"fps", "24", "24", "desired frame rate of mpeg movie"},
     {"input", "slice_*.ppm", "slice_*.ppm|input.avi", "input image files or movie file to get converted"},
     {"output", "slices.mp4", "output.mp4|output.gif", "name of output mpeg or gif file"},
     {"scale", "", "1280x720|640", "scale of the output movie or gif."},
//     {"keep", "true", "1|0|true|false", "toggle wether the input images are preserved or deleted after the conversion"},
     {"flip", "", "vertical|horizontal|180", "flip output video vertical or horizontal or rotate it by 180"}},
     [&](){mParser.setDefaults();}, [&](){this->movie();});

  mParser.addAction(
     {"mesh2ls", "mesh2sdf"}, "Convert a watertight polygon surface into a narrow-band level set, i.e. a narrow-band signed distance to a polygon mesh",
    {{"dim", "", "256", "largest dimension in voxel units of the mesh bbox (defaults to 256). If \"vdb\" or \"voxel\" is defined then \"dim\" is ignored"},
     {"voxel", "", "0.01", "voxel size in world units (by defaults \"dim\" is used to derive \"voxel\"). If specified this option takes precedence over \"dim\""},
     {"width", "", "3.0", "half-width in voxel units of the output narrow-band level set (defaults to 3 units on either side of the zero-crossing)"},
     {"exWidth", "0.0", "3.0", "half-width in voxel units of the output narrow-band level set (disabled by default)"},
     {"inWidth", "0.0", "3.0", "half-width in voxel units of the input narrow-band level set (disabled by default)"},
     {"geo", "0", "0", "age (i.e. stack index) of the geometry to be processed. Defaults to 0, i.e. most recently inserted geometry."},
     {"vdb", "-1", "0", "age (i.e. stack index) of reference grid used to define the transform. Defaults to -1, i.e. disabled. If specified this option takes precedence over \"dim\" and \"voxel\"!"},
     {"keep", "", "1|0|true|false", "toggle wether the input geometry is preserved or deleted after the conversion"},
     {"name", "", "mesh2ls_input", "specify the name of the resulting vdb (by default it's derived from the input geometry)"}},
     [&](){mParser.setDefaults();}, [&](){this->meshToLevelSet();});

  mParser.addAction(
     {"mesh2udf", "soup2udf"}, "Convert a polygon mesh or polygon soup into a to a unsigned distance field with an symmetrical narrow band",
    {{"dim", "", "256", "largest dimension in voxel units of the mesh bbox (defaults to 256). If \"vdb\" or \"voxel\" is defined then \"dim\" is ignored"},
     {"voxel", "", "0.01", "voxel size in world units (by defaults \"dim\" is used to derive \"voxel\"). If specified this option takes precedence over \"dim\""},
     {"width", "", "3.0", "half-width in voxel units of the output narrow-band level set (defaults to 3 units on either side of the zero-crossing)"},
     {"geo", "0", "0", "age (i.e. stack index) of the geometry to be processed. Defaults to 0, i.e. most recently inserted geometry."},
     {"vdb", "-1", "0", "age (i.e. stack index) of reference grid used to define the transform. Defaults to -1, i.e. disabled. If specified this option takes precedence over \"dim\" and \"voxel\"!"},
     {"keep", "", "1|0|true|false", "toggle wether the input geometry is preserved or deleted after the conversion"},
     {"name", "", "mesh2udf_input", "specify the name of the resulting vdb (by default it's derived from the input geometry)"}},
     [&](){mParser.setDefaults();}, [&](){this->meshToUnsignedDistanceField();});

  mParser.addAction(
     {"shrinkwrap", "soup2ls", "soup2sdf"}, "Convert a polygon soup into a narrow-band level set, i.e. a narrow-band signed distance to a polygon mesh",
    {{"dim", "", "256", "largest dimension in voxel units of the mesh bbox (defaults to 256). If \"voxel\" is defined then \"dim\" is ignored"},
     {"voxel", "", "0.01", "voxel size in world units (by defaults \"dim\" is used to derive \"voxel\"). If specified this option takes precedence over \"dim\""},
     {"width", "", "3.0", "half-width in voxel units of the output narrow-band level set (defaults to 3 units on either side of the zero-crossing)"},
     {"mode", "0", "0", "mode of offset operator: 0) old method (using mesh -> UDF -> mesh -> SDF), 1) Mihai's signed-flood-fill and 2) Greg's createLevelSetDilatedMesh. Defaults to 0, i.e. paper."},
     {"geo", "0", "0", "age (i.e. stack index) of the geometry to be processed. Defaults to 0, i.e. most recently inserted geometry."},
     {"erode", "8", "2", "maximum constrained-erosion distance, in units of the 2-voxel erosion step used per shrink-wrap iteration (need not be an integer). Defaults to 8."},
     {"thres", "0", "0.01", "closing (or engineering) threshold. Defaults to 0, i.e. it\'s diabled."},
     {"levels", "0", "0|0,1,2|*", "selects which of the level-set grids generated by the hierarchical shrink-wrap algorithm to output, by resolution level: 0 is the finest (highest-resolution) grid, 1 the next-finest, and so on. Accepts a single index (default 0, i.e. only the finest grid), a comma-separated list (e.g. \"0,1,2\" outputs the three finest grids), or \"*\" to output every generated grid. A runtime error is thrown if a requested level does not exist. Note that these are resolution levels of the output hierarchy, not ages on the VDB stack, which is why this option is named \"levels\" rather than \"vdb\"."},
     {"keep", "", "1|0|true|false", "toggle wether the input geometry is preserved or deleted after the conversion"},
     {"name", "", "shrinkwrap_input", "specify the name of the resulting vdb (by default it's derived from the input geometry)"}},
     [&](){mParser.setDefaults();}, [&](){this->shrinkWrap();});

  mParser.addAction(
     {"mesh2offset", "soup2offset"}, "Convert a polygon mesh or polygon soup into an offset narrow-band level set, i.e. a narrow-band signed distance to a polygon mesh",
    {{"dim", "", "256", "largest dimension in voxel units of the mesh bbox (defaults to 256). If \"vdb\" or \"voxel\" is defined then \"dim\" is ignored"},
     {"voxel", "", "0.01", "voxel size in world units (by defaults \"dim\" is used to derive \"voxel\"). If specified this option takes precedence over \"dim\""},
     //{"offset", "1.0", "1.0", "Offset in voxel units. Defaults to one, i.e. offset surface corresponds to one voxel dilation from mesh."},
     {"width", "", "3.0", "half-width in voxel units of the output narrow-band level set (defaults to 3 units on either side of the zero-crossing)"},
     {"mode", "0", "0", "mode of offset operator: 0) old method (using mesh -> UDF -> mesh -> SDF), 1) Mihai's signed-flood-fill and 2) Greg's createLevelSetDilatedMesh. Defaults to 0, i.e. paper."},
     {"geo", "0", "0", "age (i.e. stack index) of the geometry to be processed. Defaults to 0, i.e. most recently inserted geometry."},
     {"keep", "", "1|0|true|false", "toggle wether the input geometry is preserved or deleted after the conversion"},
     {"name", "", "mesh2offset_input", "specify the name of the resulting vdb (by default it's derived from the input geometry)"}},
     [&](){mParser.setDefaults();}, [&](){this->meshToOffset();});

  mParser.addAction(
     {"vol2mesh", "vdb2mesh"}, "Convert a scalar volume to an adaptive polygon mesh",
    {{"adapt", "0.0", "0.005", "normalized metric for the adaptive meshing. 0 is uniform and 1 is extreme adaptivity. Defaults to 0."},
     {"iso", "0.0", "0.1", "iso-value used to define the implicit surface. Defaults to zero."},
     {"vdb", "0", "0", "age (i.e. stack index) of the level set VDB grid to be meshed. Defaults to 0, i.e. most recently inserted VDB."},
     {"mask","-1", "1", "age (i.e. stack index) of the level set VDB grid used as a surface mask during meshing. Defaults to -1, i.e. it's disabled."},
     {"invert", "false", "1|0|true|false", "boolean toggle to mesh the complement of the mask. Defaults to false and ignored if no mask is specified."},
     {"keep", "", "1|0|true|false", "toggle wether the input VDB is preserved or deleted after the processing. The mask is never removed!"},
     {"name", "", "vol2mesh_input", "specify the name of the resulting vdb (by default it's derived from the input VDB)"}},
     [&](){mParser.setDefaults();}, [&](){this->volumeToMesh();});

  mParser.addAction(
     {"ls2mesh", "sdf2mesh"}, "Convert a level set to an adaptive polygon mesh",
    {{"adapt", "0.0", "0.005", "normalized metric for the adaptive meshing. 0 is uniform and 1 is extreme adaptivity. Defaults to 0."},
     {"iso", "0.0", "0.1", "iso-value used to define the implicit surface. Defaults to zero."},
     {"vdb", "0", "0", "age (i.e. stack index) of the level set VDB grid to be meshed. Defaults to 0, i.e. most recently inserted VDB."},
     {"mask","-1", "1", "age (i.e. stack index) of the level set VDB grid used as a surface mask during meshing. Defaults to -1, i.e. it's disabled."},
     {"invert", "false", "1|0|true|false", "boolean toggle to mesh the complement of the mask. Defaults to false and ignored if no mask is specified."},
     {"keep", "", "1|0|true|false", "toggle wether the input VDB is preserved or deleted after the processing. The mask is never removed!"},
     {"name", "", "ls2mesh_input", "specify the name of the resulting vdb (by default it's derived from the input VDB)"}},
     [&](){mParser.setDefaults();}, [&](){this->volumeToMesh();});

  mParser.addAction(
     {"fog2mesh"}, "Convert a fog volume to an adaptive polygon mesh",
    {{"adapt", "0.0", "0.005", "normalized metric for the adaptive meshing. 0 is uniform and 1 is extreme adaptivity. Defaults to 0."},
     {"iso", "0.5", "0.5", "iso-value used to define the implicit surface. Defaults to zero."},
     {"vdb", "0", "0", "age (i.e. stack index) of the level set VDB grid to be meshed. Defaults to 0, i.e. most recently inserted VDB."},
     {"mask","-1", "1", "age (i.e. stack index) of the level set VDB grid used as a surface mask during meshing. Defaults to -1, i.e. it's disabled."},
     {"invert", "false", "1|0|true|false", "boolean toggle to mesh the complement of the mask. Defaults to false and ignored if no mask is specified."},
     {"keep", "", "1|0|true|false", "toggle wether the input VDB is preserved or deleted after the processing. The mask is never removed!"},
     {"name", "", "fog2mesh_input", "specify the name of the resulting vdb (by default it's derived from the input VDB)"}},
     [&](){mParser.setDefaults();}, [&](){this->volumeToMesh();});

  mParser.addAction(
     {"ls2fog", "l2f", "sdf2fog"}, "Convert a level set VDB into a VDB with a fog volume, i.e. normalized density.",
    {{"vdb", "0", "0", "age (i.e. stack index) of the VDB grid to be processed. Defaults to 0, i.e. most recently inserted VDB."},
     {"cutoff", "0.0", "3.0", "cut-off in voxel units so fog = sdf >=0 ? 0 : -sdf/|cutoff|*dx (defaults to 0, i.e. cutoff = max for smoothest ramp"},
     {"keep", "", "1|0|true|false", "toggle wether the input VDB is preserved or deleted after the processing"},
     {"name", "", "ls2fog_input", "specify the name of the resulting VDB (by default it's derived from the input VDB)"}},
     [&](){mParser.setDefaults();}, [&](){this->levelSetToFog();});

  mParser.addAction(
     {"points2ls", "points2sdf", "p2l", "pts2sdf"}, "Convert geometry points into a narrow-band level set",
    {{"dim", "", "256", "largest dimension in voxel units of the bbox of all the points (defaults to 256). If \"voxel\" is defined \"dim\" is ignored"},
     {"voxel", "", "0.01", "voxel size in world units (by defaults \"dim\" is used to derive \"voxel\"). If specified this option takes precedence over \"dim\""},
     {"width", "", "3.0", "half-width in voxel units of the output narrow-band level set (defaults to 3 units on either side of the zero-crossing)"},
     {"radius", "2.0", "2.0", "radius in voxel units of the input points"},
     {"geo", "0", "0", "age (i.e. stack index) of the geometry to be processed. Defaults to 0, i.e. most recently inserted geometry."},
     {"keep", "", "1|0|true|false", "toggle wether the input points are preserved or deleted after the processing"},
     {"name", "", "points2ls_input", "specify the name of the resulting VDB (by default it's derived from the input points)"}},
     [&](){mParser.setDefaults();}, [&](){this->particlesToLevelSet();});

  mParser.addAction(
     {"iso2ls", "lsRebuild", "i2l"}, "Convert an iso-surface of a scalar field into a level set (i.e. SDF)",
    {{"vdb", "0", "0,1", "age (i.e. stack index) of the VDB grid to be processed and an optional reference grid. Defaults to 0, i.e. most recently inserted VDB."},
     {"iso", "0.0", "0.0", "value of the iso-surface from which to compute the level set"},
     {"voxel", "", "0.0", "voxel size in world units (defaults to zero, i.e the transform out the output matches the input)"},
     {"width", "", "3.0", "half-width in voxel units of the output narrow-band level set (defaults to 3 units on either side of the zero-crossing)"},
     {"keep", "", "1|0|true|false", "toggle wether the input VDB is preserved or deleted after the processing"},
     {"name", "", "iso2ls_input", "specify the name of the resulting VDB (by default it's derived from the input VDB)"}},
     [&](){mParser.setDefaults();}, [&](){this->isoToLevelSet();});

  mParser.addAction(
     {"points2vdb", "p2v"}, "Encode geometry points into a VDB grid",
    {{"geo", "0", "0", "age (i.e. stack index) of the geometry to be processed. Defaults to 0, i.e. most recently inserted geometry."},
     {"keep", "", "1|0|true|false", "toggle wether the input points are preserved or deleted after the processing"},
     {"ppv", "8", "8", "the number of points per voxel in the output VDB grid (defaults to 8)"},
     {"bits", "16", "16|8|32", "the number of bits used to represent a single point in the VDB grid (defaults to 16, i.e. half precision)"},
     {"name", "", "points_2vdb_input", "specify the name of the resulting VDB (by default it's derived from the input geometry)"}},
     [&](){mParser.setDefaults();}, [&](){this->pointsToVdb();});

  mParser.addAction(
     {"vdb2points", "v2p"}, "Extract points encoded in a VDB to points in a geometry format",
    {{"vdb", "0", "0", "age (i.e. stack index) of the VDB grid to be processed. Defaults to 0, i.e. most recently inserted VDB."},
     {"keep", "", "1|0|true|false", "toggle wether the input VDB is preserved or deleted after the processing"},
     {"name", "", "vdb2points_input", "specify the name of the resulting points (by default it's derived from the input VDB)"}},
     [&](){mParser.setDefaults();}, [&](){this->vdbToPoints();});

  mParser.addAction(
     {"scatter"}, "Scatter point into the active values of an input VDB grid",
    {{"count", "0", "0", "fixed number of points to randomly scatter (disabled by default)"},
     {"density", "0.0", "0.0", "uniform density of points per active voxel (disabled by default)"},
     {"ppv", "8", "8", "number of points per active voxel (defaults to 8)"},
     {"vdb", "0", "0", "age (i.e. stack index) of the VDB grid to be scatter points into. Defaults to 0, i.e. most recently inserted VDB"},
     {"keep", "", "1|0|true|false", "toggle wether the input VDB is preserved or deleted after the processing"},
     {"name", "", "scatter_input", "specify the name of the resulting points (by default it's derived from the input VDB)"}},
     [&](){mParser.setDefaults();}, [&](){this->scatter();});

  mParser.addAction(
     {"platonic"}, "Create a level set shape with the specified number of polygon faces",
    {{"dim", "", "256", "largest dimension in voxel units of the bbox of all the shape (defaults to 256). In \"voxel\" is defined \"dim\" is ignored"},
     {"voxel", "", "0.01", "voxel size in world units (by defaults \"dim\" is used to derive \"voxel\"). If specified this option takes precedence over \"dim\""},
     {"faces", "4", "{4|6|8|12|20}", "number of polygon faces of the shape to generate the level set VDB from"},
     {"scale", "1.0", "1.0", "scale of the shape in world units. E.g. if faces=6 and scale=1.0 the result is a unit cube"},
     {"center", "(0,0,0)", "(0.0,0.0,0.0)", "center of the shape in world units. defaults to the origin"},
     {"width", "", "3.0", "half-width in voxel units of the output narrow-band level set (defaults to 3 units on either side of the zero-crossing)"},
     {"name", "", "Tetrahedron", "specify the name of the resulting VDB (by default it's derived from face count)"}},
     [&](){mParser.setDefaults();}, [&](){this->levelSetPlatonic();});

  mParser.addAction(
     {"enright"}, "Performs Enright advection benchmark test on a level set",
    {{"translate", "(0,0,0)", "(0.0,0.0,0.0)", "defines the origin of the Enright velocity field"},
     {"scale", "1.0", "1.0", "defined the scale of the Enright velocity field"},
     {"dt", "0.05", "0.05", "time-step the input level set is advected"},
     {"vdb", "0", "0", "age (i.e. stack index) of the VDB grid to be processed. Defaults to 0, i.e. most recently inserted VDB."},
     {"keep", "", "1|0|true|false", "toggle wether the input VDB is preserved or deleted after the processing"}},
     [&](){mParser.setDefaults();}, [&](){this->enright();});

  mParser.addAction(
     {"dilate", "dilateLS"}, "dilate level set surface by a fixed radius",
    {{"radius", "1.0", "1.0", "radius in voxel units by which the surface is dilated"},
     {"space", "", "1|2|3|5", "order of the spatial discretization (defaults to 5, i.e. WENO)"},
     {"time", "", "1|2|3", "order of the temporal discretization (defaults to 1, i.e. explicit Euler)"},
     {"vdb", "0", "0", "age (i.e. stack index) of the VDB grid to be processed. Defaults to 0, i.e. most recently inserted VDB."}},
     [&](){mParser.setDefaults();}, [&](){this->offsetLevelSet();});

  mParser.addAction(
     {"erode", "erodeLS"}, "erode level set surface by a fixed radius",
    {{"radius", "1.0", "1.0", "radius in voxel units by which the surface is eroded"},
     {"space", "", "1|2|3|5", "order of the spatial discretization (defaults to 5, i.e. WENO)"},
     {"time", "", "1|2|3", "order of the temporal discretization (defaults to 1, i.e. explicit Euler)"},
     {"vdb", "0", "0", "age (i.e. stack index) of the VDB grid to be processed. Defaults to 0, i.e. most recently inserted VDB."}},
     [&](){mParser.setDefaults();}, [&](){this->offsetLevelSet();});

  mParser.addAction(
     {"open", "openLS"}, "morphological opening, i.e. erosion followed by dilation, of a level set surface by a fixed radius",
    {{"radius", "1.0", "1.0", "radius in voxel units by which the surface is opened"},
     {"space", "", "1|2|3|5", "order of the spatial discretization (defaults to 5, i.e. WENO)"},
     {"time", "", "1|2|3", "order of the temporal discretization (defaults to 1, i.e. explicit Euler)"},
     {"vdb", "0", "0", "age (i.e. stack index) of the VDB grid to be processed. Defaults to 0, i.e. most recently inserted VDB."}},
     [&](){mParser.setDefaults();}, [&](){this->offsetLevelSet();});

  mParser.addAction(
     {"close", "closeLS"}, "morphological closing, i.e. dilation followed by erosion, of level set surface by a fixed radius",
    {{"radius", "1.0", "1.0", "radius in voxel units by which the surface is closed"},
     {"space", "", "1|2|3|5", "order of the spatial discretization (defaults to 5, i.e. WENO)"},
     {"time", "", "1|2|3", "order of the temporal discretization (defaults to 1, i.e. explicit Euler)"},
     {"vdb", "0", "0", "age (i.e. stack index) of the VDB grid to be processed. Defaults to 0, i.e. most recently inserted VDB."}},
     [&](){mParser.setDefaults();}, [&](){this->offsetLevelSet();});

  mParser.addAction(
     {"gauss", "gaussLS"}, "gaussian convolution of a level set surface",
    {{"iter",  "1", "1", "number of iterations are that the filter is applied"},
     {"space", "", "1|2|3|5", "order of the spatial discretization (defaults to 5, i.e. WENO)"},
     {"time", "", "1|2|3", "order of the temporal discretization (defaults to 1, i.e. explicit Euler)"},
     {"vdb", "0", "0", "age (i.e. stack index) of the VDB grid to be processed. Defaults to 0, i.e. most recently inserted VDB."},
     {"size", "1", "1", "size of filter in voxel units"}},
     [&](){mParser.setDefaults();}, [&](){this->filterLevelSet();});

  mParser.addAction(
     {"mean", "meanLS"}, "mean value filtering of a level set surface",
    {{"iter",  "1",  "1", "number of iterations are that the filter is applied"},
     {"space", "", "1|2|3|5", "order of the spatial discretization (defaults to 5, i.e. WENO)"},
     {"time", "", "1|2|3", "order of the temporal discretization (defaults to 1, i.e. explicit Euler)"},
     {"vdb", "0", "0", "age (i.e. stack index) of the VDB grid to be processed. Defaults to 0, i.e. most recently inserted VDB."},
     {"size", "1", "1", "size of filter in voxel units"}},
     [&](){mParser.setDefaults();}, [&](){this->filterLevelSet();});

  mParser.addAction(
     {"median", "medianLS"}, "median value filtering of a level set surface",
    {{"iter",  "1",  "1", "number of iterations are that the filter is applied"},
     {"space", "", "1|2|3|5", "order of the spatial discretization (defaults to 5, i.e. WENO)"},
     {"time", "", "1|2|3", "order of the temporal discretization (defaults to 1, i.e. explicit Euler)"},
     {"vdb", "0", "0", "age (i.e. stack index) of the VDB grid to be processed. Defaults to 0, i.e. most recently inserted VDB."},
     {"size", "1", "1", "size of filter in voxel units"}},
     [&](){mParser.setDefaults();}, [&](){this->filterLevelSet();});

  mParser.addAction(
     {"cpt"}, "generate a vector grid with the closest-point-transform to a level set surface",
    {{"vdb", "0", "0", "age (i.e. stack index) of the VDB grid to be processed. Defaults to 0, i.e. most recently inserted VDB."},
     {"keep", "", "1|0|true|false", "toggle wether the input VDB is preserved or deleted after the processing"}},
     [&](){mParser.setDefaults();}, [&](){this->compute();});

  mParser.addAction(
     {"div"}, "generate a scalar grid with the divergence of a vector grid",
    {{"vdb", "0", "0", "age (i.e. stack index) of the VDB grid to be processed. Defaults to 0, i.e. most recently inserted VDB."},
     {"keep", "", "1|0|true|false", "toggle wether the input VDB is preserved or deleted after the processing"}},
     [&](){mParser.setDefaults();}, [&](){this->compute();});

  mParser.addAction(
     {"curl"}, "generate a vector grid with the curl of another vector grid",
    {{"vdb", "0", "0", "age (i.e. stack index) of the VDB grid to be processed. Defaults to 0, i.e. most recently inserted VDB."},
     {"keep", "", "1|0|true|false", "toggle wether the input VDB is preserved or deleted after the processing"}},
     [&](){mParser.setDefaults();}, [&](){this->compute();});

  mParser.addAction(
     {"grad"}, "generate a vector grid with the gradient of a scalar grid",
    {{"vdb", "0", "0", "age (i.e. stack index) of the VDB grid to be processed. Defaults to 0, i.e. most recently inserted VDB."},
     {"keep", "", "1|0|true|false", "toggle wether the input VDB is preserved or deleted after the processing"}},
     [&](){mParser.setDefaults();}, [&](){this->compute();});

  mParser.addAction(
     {"curvature"}, "generate scalar grid with the mean curvature of a level set surface",
    {{"vdb", "0", "0", "age (i.e. stack index) of the VDB grid to be processed. Defaults to 0, i.e. most recently inserted VDB."},
     {"keep", "", "1|0|true|false", "toggle wether the input VDB is preserved or deleted after the processing"}},
     [&](){mParser.setDefaults();}, [&](){this->compute();});

  mParser.addAction(
     {"length"}, "generate a scalar grid with the magnitude of a vector grid",
    {{"vdb", "0", "0", "age (i.e. stack index) of the VDB grid to be processed. Defaults to 0, i.e. most recently inserted VDB."},
     {"keep", "", "1|0|true|false", "toggle wether the input VDB is preserved or deleted after the processing"}},
     [&](){mParser.setDefaults();}, [&](){this->compute();});

  mParser.addAction(
     {"union"}, "CSG union of two level sets surfaces",
    {{"vdb", "0,1", "0,1", "ages (i.e. stack indices) of the two VDB grids to union. Defaults to 0,1, i.e. two most recently inserted VDBs."},
     {"keep", "", "1|0|true|false", "toggle wether the input VDB is preserved or deleted after the processing"},
     {"prune", "true", "true", "toggle wether to prune the tree after the boolean operation (enabled by default)"},
     {"rebuild", "true", "true", "toggle wether to re-build the level set after the boolean operation (enabled by default)"}},
     [&](){mParser.setDefaults();}, [&](){this->csg();});

  mParser.addAction(
     {"intersection"}, "CSG intersection of two level sets surfaces",
    {{"vdb", "0,1", "0,1", "ages (i.e. stack indices) of the two VDB grids to intersect. Defaults to 0,1, i.e. two most recently inserted VDBs."},
     {"keep", "", "1|0|true|false", "toggle wether the input VDB is preserved or deleted after the processing"},
     {"prune", "true", "true", "toggle wether to prune the tree after the boolean operation (enabled by default)"},
     {"rebuild", "true", "true", "toggle wether to re-build the level set after the boolean operation (enabled by default)"}},
     [&](){mParser.setDefaults();}, [&](){this->csg();});

  mParser.addAction(
     {"difference"}, "CSG difference of two level sets surfaces",
    {{"vdb", "0,1", "0,1", "ages (i.e. stack indices) of the two VDB grids to difference. Defaults to 0,1, i.e. two most recently inserted VDBs."},
     {"keep", "", "1|0|true|false", "toggle wether the input VDB is preserved or deleted after the processing"},
     {"prune", "true", "true", "toggle wether to prune the tree after the boolean operation (enabled by default)"},
     {"rebuild", "true",  "true", "toggle wether to re-build the level set after the boolean operation (enabled by default)"}},
     [&](){mParser.setDefaults();}, [&](){this->csg();});

  mParser.addAction(
     {"min"}, "Given grids A and B, compute min(a, b) per voxel",
    {{"vdb", "0,1", "0,1", "ages (i.e. stack indices) of the two VDB grids to composit. Defaults to 0,1, i.e. two most recently inserted VDBs."},
     {"keep", "", "1|0|true|false", "toggle wether the input VDBs is preserved or deleted after the processing"}},
     [&](){mParser.setDefaults();}, [&](){this->composite();});

  mParser.addAction(
     {"max"}, "Given grids A and B, compute max(a, b) per voxel",
    {{"vdb", "0,1", "0,1", "ages (i.e. stack indices) of the two VDB grids to composit. Defaults to 0,1, i.e. two most recently inserted VDBs."},
     {"keep", "", "1|0|true|false", "toggle wether the input VDBs is preserved or deleted after the processing"}},
     [&](){mParser.setDefaults();}, [&](){this->composite();});

  mParser.addAction(
     {"sum"}, "Given grids A and B, compute sum(a, b) per voxel",
    {{"vdb", "0,1", "0,1", "ages (i.e. stack indices) of the two VDB grids to composit. Defaults to 0,1, i.e. two most recently inserted VDBs."},
     {"keep", "", "1|0|true|false", "toggle wether the input VDBs is preserved or deleted after the processing"}},
     [&](){mParser.setDefaults();}, [&](){this->composite();});

  mParser.addAction(
     {"multiply", "mul"}, "Given grids A and B, compute a * b per voxel",
    {{"vdb", "0,1", "0,1", "ages (i.e. stack indices) of the two VDB grids to composit. Defaults to 0,1, i.e. two most recently inserted VDBs."},
     {"keep", "", "1|0|true|false", "toggle wether the input VDBs is preserved or deleted after the processing"}},
     [&](){mParser.setDefaults();}, [&](){this->composite();});

  mParser.addAction(
     {"divide"}, "Given grids A and B, compute a / b per voxel",
    {{"vdb", "0,1", "0,1", "ages (i.e. stack indices) of the two VDB grids to composit. Defaults to 0,1, i.e. two most recently inserted VDBs."},
     {"keep", "", "1|0|true|false", "toggle wether the input VDBs is preserved or deleted after the processing"}},
     [&](){mParser.setDefaults();}, [&](){this->composite();});

  mParser.addAction(
     {"multires"}, "construct a LoD sequences of VDB trees with powers of two refinements",
    {{"levels", "2", "2", "number of multi-resolution grids in the output LoD sequence"},
     {"vdb", "0", "0", "age (i.e. stack index) of the VDB grid to be processed. Defaults to 0, i.e. most recently inserted VDB."},
     {"keep", "", "1|0|true|false", "toggle wether the input VDB is preserved or deleted after the processing"}},
     [&](){mParser.setDefaults();}, [&](){this->multires();});

  mParser.addAction(
     {"resample"}, "resample one VDB grid into another VDB grid or a transformation of the input grid",
    {{"vdb", "0,1", "0,1", "pair of input and optional output grids (i.e. stack index) to be processed. Defaults to 0,1, i.e. most recent VDB is resampled to match the transform of the second to most recent VDB."},
     {"scale", "0", "0", "scale use to transform the input grid (ignored if two grids are specified with vdb)"},
     {"translate", "(0,0,0)", "(0,0,0)", "translation use to transform the input grid (ignored if two grids are specified with vdb)"},
     {"order", "1", "1", "order of the polynomial interpolation kernel used during resampling"},
     {"keep", "", "1|0|true|false", "toggle wether the input VDB is preserved or deleted after the processing"}},
     [&](){mParser.setDefaults();}, [&](){this->resample();});

  mParser.addAction(
     {"clip"}, "Clip a VDB grid against another grid, a bbox or frustum",
    {{"vdb", "0", "0", "age (i.e. stack index) of the VDB grid to be processed. Defaults to 0, i.e. most recently inserted VDB."},
     {"keep", "", "1|0|true|false", "toggle wether the input VDB is preserved or deleted after the processing"},
     {"bbox", "", "(0,0,0),(1,1,1)", "min and max of the world-space bounding-box used for clipping. Defaults to empty, i.e. disabled"},
     {"taper", "-1", "1", "taper of the frustum (requires bbox and depth to be specified). Defaults to -1, i.e. disabled"},
     {"depth", "-1", "1", "depth in world units of the frustum (requires bbox and taper to be specified). Defaults to -1, i.e. disabled"},
     {"mask", "-1", "1", "age (i.e. stack index) of a mask VDB used for clipping. Defaults to -1, i.e. disabled"}},
     [&](){mParser.setDefaults();}, [&](){this->clip();});

  mParser.addAction(
     {"slice"}, "Generate images of slices of a VDB grid",
    {{"vdb", "0", "0", "age (i.e. stack index) of the VDB grid to be processed. Defaults to 0, i.e. most recently inserted VDB."},
     {"keep", "true", "1|0|true|false", "toggle wether the input VDB is preserved or deleted after the processing"},
     {"file", "slice", "slice", "name of ppm file(s) of slices"},
     {"force", "0", "1|0|true|false", "force computations of min/max, else use expected values for LS and FOG volumes (default)"},
     {"scale", "512", "1920x1080", "pixel size of image (aspect ratio is derived from the vdb unless both dimensions are given)"},
     {"X", "0.5", "1", "One or more X-slices in range 0 -> 1. Defaults to 0.5, i.e. mid-point"},
     {"Y", "", "1", "One or more Y-slices in range 0 -> 1. Defaults to 0.5, i.e. mid-point"},
     {"Z", "", "1", "One or more Z-slices in range 0 -> 1. Defaults to 0.5, i.e. mid-point"}},
     [&](){mParser.setDefaults();}, [&](){this->slice();});

  mParser.addAction(
    {"prune"}, "prune away inactive values in a VDB grid",
   {{"vdb", "0", "0", "age (i.e. stack index) of the VDB grid to be processed. Defaults to 0, i.e. most recently inserted VDB"}},
     [](){},[&](){this->pruneLevelSet();});

  mParser.addAction(
     {"flood"}, "signed-flood filling of a level set VDB",
    {{"vdb", "0", "0", "age (i.e. stack index) of the VDB grid to be processed. Defaults to 0, i.e. most recently inserted VDB"}},
     [](){},[&](){this->floodLevelSet();});

  mParser.addAction(
     {"expand"}, "expand narrow band of level set",
    {{"dilate", "1", "1", "number of integer voxels that the narrow band of the input SDF will be dilated"},
     {"iter", "1", "1", "number of iterations of the fast sweeping algorithm (each using 8 sweeps)"},
     {"vdb", "0", "0", "age (i.e. stack index) of the VDB grid to be processed. Defaults to 0, i.e. most recently inserted VDB."},
     {"keep", "", "1|0|true|false", "toggle wether the input VDB is preserved or deleted after the processing"}},
     [&](){mParser.setDefaults();}, [&](){this->expandLevelSet();});

  mParser.addAction(
     {"segment"}, "segment an input VDB into a list if topologically disconnected VDB grids",
    {{"vdb", "0", "0", "age (i.e. stack index) of the VDB grid to be processed. Defaults to 0, i.e. most recently inserted VDB."},
     {"keep", "", "1|0|true|false", "toggle wether the input VDB is preserved or deleted after the processing"}},
     [&](){mParser.setDefaults();}, [&](){this->segment();});

  mParser.addAction(
     {"transform"}, "apply affine transformations (uniform scale -> rotation -> translation) to a VDB grids and geometry",
    {{"rotate", "(0.0,0.0,0.0)", "(0.0,0.0,0.0)", "rotation in radians around x,y,z axis"},
     {"translate", "(0.0,0.0,0.0)", "(0.0,0.0,0.0)", "translation in world units along x,y,z axis"},
     {"scale", "1.0", "1.0", "uniform scaling in world units"},
     {"vdb", "", "0,2,..", "age (i.e. stack index) of the VDB grid to be processed. Defaults to empty."},
     {"geo", "", "0,2,..", "age (i.e. stack index) of the Geometry to be processed. Defaults to empty."},
     {"keep", "", "1|0|true|false", "toggle wether the input VDB is preserved or overwritten"}},
     [&](){mParser.setDefaults();}, [&](){this->transform();});

  mParser.addAction(
     {"render"}, "ray-tracing of level set surfaces and volume rendering of fog volumes",
    {{"files", "", "output.{jpg|png|ppm|exr}", "file used to save the rendered image to disk"},
     {"vdb", "0", "0", "age (i.e. stack index) of the VDB grid to be processed. Defaults to 0, i.e. most recently inserted VDB."},
     {"keep", "", "1|0|true|false", "toggle wether the input VDB is preserved or deleted after the rendering"},
     {"camera", "perspective", "persp|ortho", "perspective or orthographic camera"},
     {"aperture", "41.2136", "41.2136", "width in mm of the frame of a perspective camera, i.e., the visible field (defaults to 41.2136mm)"},
     {"focal", "50", "50", "focal length of a perspective camera in mm (defaults to 50mm)"},
     {"isovalue", "0.0", "0.0", "iso-value use during ray-intersection of level set surfaces"},
     {"samples", "1", "1", "number of samples (rays) per pixel"},
     {"image", "1920x1080", "1920x1080", "image size defined in terms of pixel resolution"},
     {"translate", "(0,0,0)", "(0,0,0)", "translation of the camera in world-space units, applied after rotation"},
     {"rotate", "(0,0,0)", "(0,0,0)", "rotation in degrees of the camera in world space (applied in x, y, z order)"},
     {"target", "(0,0,0)", "", "target point in world pace that the camera will point at (if undefined target is set to the center of the bbox of the grid)"},
     {"up", "(0,1,0)", "(0,1,0)", "vector that should point up after rotation with lookat"},
     {"lookat", "true", "true", "rotate the camera so it looks at the center of the shape uses up as the horizontal direction"},
     {"near", "0.001", "0.001", "depth of the near clipping plane in world-space units"},
     {"far",  "3.4e+38", "3.4e+38", "depth of the far clipping plane in world-space units"},
     {"shader", "diffuse", "diffuse|normal|position|matte", "shader type; either \"diffuse\", \"matte\", \"normal\" or \"position\""},
     {"light", "(0.3,0.3,0.0),(0.7,0.7,0.7)", "(0.3,0.3,0.0),(0.7,0.7,0.7)", "light source direction and optional color"},
     {"frame", "1.0", "1.0", "orthographic camera frame width in world units"},
     {"cutoff", "0.005", "0.005", "density and transmittance cutoff value (ignored for level sets)"},
     {"gain", "0.2", "0.2", "amount of scatter along the shadow ray (ignored for level sets)"},
     {"absorb", "(0.1,0.1,0.1)", "(0.1,0.1,0.1)", "absorption coefficients for RGB (ignored for level sets)"},
     {"scatter", "(1.5,1.5,1.5)", "(1.5,1.5,1.5)", "scattering coefficients for RGB (ignored for level sets)"},
     {"step", "1.0,3.0", "1.0,3.0", "step size in voxels for integration along the primary ray (ignored for level sets)"},
     {"colorgrid", "-1", "1", "age of a vec3s VDB grid to be used to set material colors. Defaults to -1, i.e. disabled"}},
     [&](){mParser.setDefaults();}, [&](){this->render();}, 0);

  mParser.addAction(
       {"print", "p"}, "prints information to the terminal about the current stack of VDB grids and Geometry",
      {{"vdb", "*", "*", "print information about VDB grids"},
       {"geo", "*", "*", "print information about geometries"},
       {"mem", "0", "0|1|false|true", "print a list of all stored variables"},
       {"level", "0", "0|1", "detail level: 0=base table, 1=+bbox and node-count columns"}},
      [](){}, [&](){this->print();});

  mParser.addAction(
      {"diagnose"}, "Run OpenVDB diagnostics checks on one or more VDB grids",
     {{"vdb",    "*", "*|0|0,1,2", "comma-separated age indices of VDB grids to check, or \"*\" for all (default)"},
      {"checks", "9", "1|...|9",   "number of checks to run (level set: 1-9, fog volume: 1-6). Lower values skip slower or stricter checks"},
      {"fatal",  "0", "1|0|true|false", "if true, throw an exception when any check fails (default false)"}},
     [&](){mParser.setDefaults();}, [&](){this->diagnose();});

  mParser.addAction(
      {"stats"}, "Print value statistics (min, max, mean, std. dev.) of active voxels for one or more VDB grids",
     {{"vdb", "*", "*|0|0,1,2", "comma-separated age indices of VDB grids to analyze, or \"*\" for all (default)"}},
     [](){}, [&](){this->stats();});

  mParser.addAction(
      {"histogram", "hist"}, "Print an ASCII bar histogram of the distribution of active values for one or more VDB grids",
     {{"vdb",  "*",  "*|0|0,1,2", "comma-separated age indices of VDB grids to analyze, or \"*\" for all (default)"},
      {"bins", "10", "10|20|50",  "number of histogram bins (defaults to 10)"},
      {"min",  "",   "0.0",       "lower bound of the histogram range (defaults to the grid's smallest active value)"},
      {"max",  "",   "1.0",       "upper bound of the histogram range (defaults to the grid's largest active value)"},
      {"cols", "40", "40",        "width in characters of the longest bar (defaults to 40)"},
      {"log",  "false", "1|0|true|false", "use a logarithmic scale for the bar lengths, which makes highly peaked distributions (common for level sets and fog volumes) readable. Defaults to false, i.e. a linear scale"}},
     [](){}, [&](){this->histogram();});

  mParser.addAction(
      {"version"}, "write timing information to the terminal", {},
      [&](){std::clog << mCmdName << ": version " << Tool::version() << std::endl;std::exit(EXIT_SUCCESS);}, [](){});

  mParser.addAction(
      {"examples"}, "print examples to the terminal and terminate", {},
      [&](){std::clog << this->examples() << std::endl; std::exit(EXIT_SUCCESS);}, [](){});

  mParser.addAction(
      {"errorOnWarning", "stopOnWarning"}, "stop on warnings, i.e. treat warnings as errors", {},
      [&](){mErrorOnWarning = true;}, [](){});

  mParser.addAction(
      {"log"}, "enable logging to file",
      {{"file",   "",      "vdb_tool.log",   "file used for logging. Use \"watch -n 0.5 vdb_tool.log\" to see updates in real-time."},
       {"append", "false", "0|1|false|true", "if true, append to the existing log file instead of truncating it"},
       {"tee",    "true",  "0|1|false|true", "if true (default), also write to the original terminal stream so interactive feedback is preserved"}},
      [&](){this->startLog(mParser.get<std::string>("file"),
                            mParser.get<bool>("append"),
                            mParser.get<bool>("tee"));}, [](){}, 0);

  Processor &proc = mParser.processor;

  // operations related to VDB grids
  proc.add("voxelSize", "voxel size of specified vdb grid, e.g. {0:voxelSize} -> {0.01}",
      [&](){auto it = this->getGrid(strToInt(proc.get()));
            proc.set((*it)->voxelSize()[0]);});

  proc.add("voxelCount", "number of active voxels of specified VDB grid, e.g. {0:voxelCount} -> {3269821}",
      [&](){auto it = this->getGrid(strToInt(proc.get()));
            proc.set((*it)->activeVoxelCount());});

  proc.add("gridCount", "push the number of loaded VDB grids onto the stack, e.g. {gridCount} -> {1}",
      [&](){proc.push(mGrid.size());});

  proc.add("gridName", "name of a specified VDB grid, e.g. {0:gridName} -> {sphere}",
      [&](){auto it = this->getGrid(strToInt(proc.get()));
            proc.set((*it)->getName());});

  proc.add("isGridEmpty", "test if a specified VDB grid is empty or not, e.g. {0:isGridEmpty} -> {0}",
      [&](){auto it = this->getGrid(strToInt(proc.get()));
            proc.set((*it)->empty());});

  proc.add("gridType", "value type of a specified VDB grid, e.g. {0:gridType} -> {float}",
      [&](){auto it = this->getGrid(strToInt(proc.get()));
            proc.set((*it)->valueType());});

  proc.add("gridClass", "class of a specified VDB grid, e.g. {0:gridClass} -> {ls}",
      [&](){auto it = this->getGrid(strToInt(proc.get()));
            switch ((*it)->getGridClass()) {
                case GRID_LEVEL_SET: proc.set("ls"); break;
                case GRID_FOG_VOLUME: proc.set("fog"); break;
                default: proc.set("unknown");}});

  proc.add("isLS", "test if a specified VDB grid is a level set or not, e.g. {0:isLS} -> {1}",
      [&](){auto it = this->getGrid(strToInt(proc.get()));
            proc.set((*it)->getGridClass()==GRID_LEVEL_SET);});

  proc.add("isFOG", "test if a specified VDB grid is a fog volume or not, e.g. {0:isFOG} -> {0}",
      [&](){auto it = this->getGrid(strToInt(proc.get()));
            proc.set((*it)->getGridClass()==GRID_FOG_VOLUME);});

  proc.add("gridDim", "voxel dimension of specified VDB grid, e.g. {0:gridDim} -> {[255,255,255]}",
      [&](){auto it = this->getGrid(strToInt(proc.get()));
            const CoordBBox bbox = (*it)->evalActiveVoxelBoundingBox();
            std::stringstream ss;
            ss << bbox.dim();
            proc.set(ss.str());});

  proc.add("gridBBox", "world space bounding box of specified VDB grid, e.g. {0:gridBBox} -> {[-1.016,-1.016,-1.016] [1.016,1.016,1.016]}",
      [&](){auto it = this->getGrid(strToInt(proc.get()));
            const CoordBBox bbox = (*it)->evalActiveVoxelBoundingBox();
            const math::BBox<Vec3d> bboxIndex(bbox.min().asVec3d(), bbox.max().asVec3d());
            const math::BBox<Vec3R> bboxWorld = bboxIndex.applyMap(*((*it)->transform().baseMap()));
            const auto &min = bboxWorld.min(), &max = bboxWorld.max();
            std::stringstream ss;
            ss << "["<<min[0]<<","<<min[1]<<","<<min[2]<<"] "
               << "["<<max[0]<<","<<max[1]<<","<<max[2]<<"]";
            proc.set(ss.str());});

  proc.add("gridCenter", "world space center of bounding box of specified VDB grid, e.g. {0:gridCenter} -> {[0.0,0.0,0.0]}",
      [&](){auto it = this->getGrid(strToInt(proc.get()));
            const CoordBBox bbox = (*it)->evalActiveVoxelBoundingBox();
            const math::BBox<Vec3d> bboxIndex(bbox.min().asVec3d(), bbox.max().asVec3d());
            const math::BBox<Vec3R> bboxWorld = bboxIndex.applyMap(*((*it)->transform().baseMap()));
            const auto center = 0.5*(bboxWorld.max() + bboxWorld.min());
            std::stringstream ss;
            ss << "["<<center[0]<<","<<center[1]<<","<<center[2]<<"]";
            proc.set(ss.str());});

  proc.add("gridRadius", "world space radius of bounding box of specified VDB grid, e.g. {0:gridRadius} -> {1.73}",
      [&](){auto it = this->getGrid(strToInt(proc.get()));
            const CoordBBox bbox = (*it)->evalActiveVoxelBoundingBox();
            const math::BBox<Vec3d> bboxIndex(bbox.min().asVec3d(), bbox.max().asVec3d());
            const math::BBox<Vec3R> bboxWorld = bboxIndex.applyMap(*((*it)->transform().baseMap()));
            proc.set(0.5*(bboxWorld.max() - bboxWorld.min()).length());});

  // operations related to geometry
  proc.add("vtxCount", "number of voxels of a specified geometry, e.g. {0:vtxCount} -> {2461023}",
      [&](){auto it = this->getGeom(strToInt(proc.get()));
            proc.set((*it)->vtxCount());});

  proc.add("polyCount", "number of polygons of a specified geometry, e.g. {0:polyCount} -> {23560}",
      [&](){auto it = this->getGeom(strToInt(proc.get()));
            proc.set((*it)->polyCount());});

  proc.add("geomCount", "push the number of loaded geometries onto the stack, e.g. {geomCount} -> {1}",
      [&](){proc.push(mGrid.size());});

  proc.add("geomName", "name of a specified geometry, e.g. {0:geomName} -> {bunny}",
      [&](){auto it = this->getGeom(strToInt(proc.get()));
            proc.set((*it)->getName());});

  proc.add("isGeomEmpty", "test if a specified VDB grid is empty or not, e.g. {0:isGridEmpty} -> {0}",
      [&](){auto it = this->getGeom(strToInt(proc.get()));
            proc.set((*it)->isEmpty());});

  proc.add("geomClass", "class of a specified geometry, e.g. {0:geomClass} -> {points}",
      [&](){auto it = this->getGeom(strToInt(proc.get()));
            if ((*it)->isPoints()) {
                proc.set("points");
            } else if ((*it)->isMesh()) {
                proc.set("mesh");
            } else {
                proc.set("unknown");}});

  proc.add("geomBBox", "world space bounding box of specified geometry, e.g. {0:geomBBox} -> {[-1.016,-1.016,-1.016] [1.016,1.016,1.016]}",
      [&](){auto it = this->getGeom(strToInt(proc.get()));
            const auto &min = (*it)->bbox().min(), &max = (*it)->bbox().max();
            std::stringstream ss;
            ss << "["<<min[0]<<","<<min[1]<<","<<min[2]<<"] "
               << "["<<max[0]<<","<<max[1]<<","<<max[2]<<"]";
            proc.set(ss.str());});

  proc.add("geomCenter", "world space center of bounding box of specified geometry, e.g. {0:geomCenter} -> {[0.0,0.0,0.0]}",
      [&](){auto it = this->getGeom(strToInt(proc.get()));
            const auto center = 0.5*((*it)->bbox().max() + (*it)->bbox().min());
            std::stringstream ss;
            ss << "["<<center[0]<<","<<center[1]<<","<<center[2]<<"]";
            proc.set(ss.str());});

  proc.add("geomRadius", "world space radius of bounding box of specified geometry, e.g. {0:geomRadius} -> {1.73}",
      [&](){auto it = this->getGeom(strToInt(proc.get()));
            proc.set(0.5*((*it)->bbox().max() - (*it)->bbox().min()).length());});

}// Tool::init()

} // namespace vdb_tool
} // namespace OPENVDB_VERSION_NAME
} // namespace openvdb
