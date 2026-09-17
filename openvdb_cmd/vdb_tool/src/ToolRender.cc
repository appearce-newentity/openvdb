// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/// @file ToolRender.cc
/// @brief Rendering, slicing and movie output, plus the PNG/JPG/EXR image writers. Split out of Tool.h; see Tool.h for the class.

#include "Tool.h"

namespace openvdb {
OPENVDB_USE_VERSION_NAMESPACE
namespace OPENVDB_VERSION_NAME {
namespace vdb_tool {

// ==============================================================================================================

void Tool::slice()
{
  using RangeT = tbb::blocked_range2d<int>;
  struct Axis {
    const std::string label;// string name of this axis
    const VecF        slices;// fractional slices along the current axis
    const Vec3I       abc;// indics of the three axis
    Axis(const Parser &p, char c, int i, int j, int k) : label(1,c), slices(p.getVec<float>(label)), abc(i,j,k) {}
  };

  OPENVDB_ASSERT(mParser.getAction().names[0] == "slice");
  mParser.printAction();
  const int age = mParser.get<int>("vdb");
  const bool keep = mParser.get<bool>("keep");
  const bool force = mParser.get<bool>("force");
  const std::string file = mParser.get<std::string>("file");
  const VecI scale = mParser.getVec<int>("scale", "x");
  const std::vector<Axis> axes = {{mParser, 'X', 0, 1, 2}, {mParser, 'Y', 1, 0, 2}, {mParser, 'Z', 2, 0, 1}};

  auto it = this->getGrid(age);
  GridT::Ptr grid = gridPtrCast<GridT>(*it);
  if (!grid) throw std::invalid_argument("slice: no VDB with age " + std::to_string(age));
  const auto &tree = grid->tree();
  if (mParser.verbose) mTimer.start("slice");

  // color LUT from https://gist.github.com/mikhailov-work/6a308c20e494d9e0ccc29036b28faa7a (Apache-2.0)
  const unsigned char LUT[256][3] = {{48,18,59},{50,21,67},{51,24,74},{52,27,81},{53,30,88},{54,33,95},{55,36,102},{56,39,109},{57,42,115},{58,45,121},{59,47,128},{60,50,134},{61,53,139},{62,56,145},{63,59,151},{63,62,156},{64,64,162},{65,67,167},{65,70,172},{66,73,177},{66,75,181},{67,78,186},{68,81,191},{68,84,195},{68,86,199},{69,89,203},{69,92,207},{69,94,211},{70,97,214},{70,100,218},{70,102,221},{70,105,224},{70,107,227},{71,110,230},{71,113,233},{71,115,235},{71,118,238},{71,120,240},{71,123,242},{70,125,244},{70,128,246},{70,130,248},{70,133,250},{70,135,251},{69,138,252},{69,140,253},{68,143,254},{67,145,254},{66,148,255},{65,150,255},{64,153,255},{62,155,254},{61,158,254},{59,160,253},{58,163,252},{56,165,251},{55,168,250},{53,171,248},{51,173,247},{49,175,245},{47,178,244},{46,180,242},{44,183,240},{42,185,238},{40,188,235},{39,190,233},{37,192,231},{35,195,228},{34,197,226},{32,199,223},{31,201,221},{30,203,218},{28,205,216},{27,208,213},{26,210,210},{26,212,208},{25,213,205},{24,215,202},{24,217,200},{24,219,197},{24,221,194},{24,222,192},{24,224,189},{25,226,187},{25,227,185},{26,228,182},{28,230,180},{29,231,178},{31,233,175},{32,234,172},{34,235,170},{37,236,167},{39,238,164},{42,239,161},{44,240,158},{47,241,155},{50,242,152},{53,243,148},{56,244,145},{60,245,142},{63,246,138},{67,247,135},{70,248,132},{74,248,128},{78,249,125},{82,250,122},{85,250,118},{89,251,115},{93,252,111},{97,252,108},{101,253,105},{105,253,102},{109,254,98},{113,254,95},{117,254,92},{121,254,89},{125,255,86},{128,255,83},{132,255,81},{136,255,78},{139,255,75},{143,255,73},{146,255,71},{150,254,68},{153,254,66},{156,254,64},{159,253,63},{161,253,61},{164,252,60},{167,252,58},{169,251,57},{172,251,56},{175,250,55},{177,249,54},{180,248,54},{183,247,53},{185,246,53},{188,245,52},{190,244,52},{193,243,52},{195,241,52},{198,240,52},{200,239,52},{203,237,52},{205,236,52},{208,234,52},{210,233,53},{212,231,53},{215,229,53},{217,228,54},{219,226,54},{221,224,55},{223,223,55},{225,221,55},{227,219,56},{229,217,56},{231,215,57},{233,213,57},{235,211,57},{236,209,58},{238,207,58},{239,205,58},{241,203,58},{242,201,58},{244,199,58},{245,197,58},{246,195,58},{247,193,58},{248,190,57},{249,188,57},{250,186,57},{251,184,56},{251,182,55},{252,179,54},{252,177,54},{253,174,53},{253,172,52},{254,169,51},{254,167,50},{254,164,49},{254,161,48},{254,158,47},{254,155,45},{254,153,44},{254,150,43},{254,147,42},{254,144,41},{253,141,39},{253,138,38},{252,135,37},{252,132,35},{251,129,34},{251,126,33},{250,123,31},{249,120,30},{249,117,29},{248,114,28},{247,111,26},{246,108,25},{245,105,24},{244,102,23},{243,99,21},{242,96,20},{241,93,19},{240,91,18},{239,88,17},{237,85,16},{236,83,15},{235,80,14},{234,78,13},{232,75,12},{231,73,12},{229,71,11},{228,69,10},{226,67,10},{225,65,9},{223,63,8},{221,61,8},{220,59,7},{218,57,7},{216,55,6},{214,53,6},{212,51,5},{210,49,5},{208,47,5},{206,45,4},{204,43,4},{202,42,4},{200,40,3},{197,38,3},{195,37,3},{193,35,2},{190,33,2},{188,32,2},{185,30,2},{183,29,2},{180,27,1},{178,26,1},{175,24,1},{172,23,1},{169,22,1},{167,20,1},{164,19,1},{161,18,1},{158,16,1},{155,15,1},{152,14,1},{149,13,1},{146,11,1},{142,10,1},{139,9,2},{136,8,2},{133,7,2},{129,6,2},{126,5,2},{122,4,3}};

  const CoordBBox bbox = grid->evalActiveVoxelBoundingBox();
  const Coord dim = bbox.dim();
  math::Extrema ex;
  if (!force && grid->getGridClass() == GRID_LEVEL_SET) {
    ex.add( tree.background());
    ex.add(-tree.background());
  } else if (!force && grid->getGridClass() == GRID_FOG_VOLUME) {
    ex.add(0.0f);
    ex.add(1.0f);
  } else {
    ex = tools::extrema(grid->cbeginValueOn());
  }

  for (const Axis &axis : axes) {
    tools::Film image(scale[0], scale.size()==2 ? scale[1] : scale[0]*dim[axis.abc[2]]/dim[axis.abc[1]]);
    for (const float slice : axis.slices) {
      tbb::parallel_for(RangeT(0, int(image.width()), 0, int(image.height())), [&](const RangeT &range){
        const int a = axis.abc[0], b = axis.abc[1], c = axis.abc[2];
        Vec3R xyz;
        // Compute in double (Vec3R's element type) so the Int32 dim/bbox
        // operands widen losslessly; mixing them with float triggers
        // -Wimplicit-int-float-conversion under -Werror.
        xyz[a] = double(slice) * (dim[a]+1) + bbox.min()[a];
        auto acc = grid->getAccessor();// thread local copy
        for (auto row=range.rows().begin(); row!=range.rows().end(); ++row) {
          xyz[b] = row/double(image.width())*(dim[b]+1) + bbox.min()[b];
          for (int col=range.cols().begin(); col<range.cols().end(); ++col) {
            xyz[c] = col/double(image.height())*(dim[c]+1) + bbox.min()[c];
            const float v = tools::BoxSampler::sample(acc, xyz);
            // Clamp before the integer cast: float-to-uint8_t conversion is
            // UB when the value falls outside [0,255], and clang on ARM64
            // emits an unmasked fcvtzu — values outside ex's range (e.g. the
            // background of a level set whose active values have been
            // rewritten by forOnValues) yield huge indices and an OOB read.
            const float t = float((v - ex.min()) / (ex.max() - ex.min()));
            const int   k = int(255.0f * math::Clamp(t, 0.0f, 1.0f));
            const unsigned char *p = LUT[k];
            image.pixel(row,col) = tools::Film::RGBA(p[0]/255.0f, p[1]/255.0f, p[2]/255.0f);
          }// loop over colums in image
        }// loop over rows in image
      });// end parallel_for
      image.savePPM(file + "_" + axis.label + "_" + std::to_string(slice)+ ".ppm");
    }// loop over slices within an axis (singular)
  }// loop over axes (plural)

  if (!keep) mGrid.erase(std::next(it).base());
  if (mParser.verbose) mTimer.stop();
}// Tool::slice

// ==============================================================================================================

/// @brief Convert multiple image files to a mpeg movie file
// vdb_tool -sphere -for x=0,1,0.01 -slice X='{$x}' -end -img2mpeg input="slice_*.ppm" output=slices.mp4
// vdb_tool -sphere -for x=0,1,0.01 -slice X='{$x}' -end -img2mpeg && open slices.mp4
void Tool::movie()
{
#ifdef VDB_TOOL_USE_MPEG
  OPENVDB_ASSERT(mParser.getAction().names[0] == "movie");
  mParser.printAction();
  const int fps = mParser.get<int>("fps");
  const std::string input = mParser.get<std::string>("input");
  const std::string output = mParser.get<std::string>("output");
  const VecI scale = mParser.getVec<int>("scale", "x");
  //const bool keep = mParser.get<bool>("keep");
  const std::string flip = mParser.get<std::string>("flip");
  if (mParser.verbose) mTimer.start("movie");

  std::string cmd("\"" VDB_TOOL_FFMPEG_PATH "\""), log("log.txt");
  cmd += " -loglevel error";// only log error messages
  if (contains(input, '*')) cmd += " -pattern_type glob";// support expanding shell-like wildcard patterns (globbing)
  cmd += " -i \'" + input + "\'";// specify multiple input files as "input_*.ppm"
  cmd += " -vf \"fps=" + std::to_string(fps);
  if (findMatch(flip,{"vertical",  "180"})) cmd += ",vflip";// flip vertical (up/down)
  if (findMatch(flip,{"horizontal","180"})) cmd += ",hflip";// flip horizontal (left/right)
  if (flip!="" && !contains(cmd,"flip")) throw std::invalid_argument("Tool::movie: invalid argument flip=\""+flip+"\", expected \"vertical\", \"horizontal\" or \"180\"");
  if (scale.size()==2) {
    cmd += ",scale=" + std::to_string(scale[0]) + ":" + std::to_string(scale[1]) + ":flags=lanczos";
  } else if (scale.size()==1) {
    cmd += ",scale=" + std::to_string(scale[0]) + ":-1:flags=lanczos";
  }
  cmd += "\"";// end "-vf \"fps=..."
  if (getExt(output) == "gif") cmd += " -c:v gif";// create animated gif
  cmd += " -y " + output;// overwrite output files without asking
  cmd += " > " + log + " 2>&1";// redirect stdout and stderr to log file
  if (mParser.verbose>1) std::clog << cmd << std::endl;
  auto mySystem = [&](const std::string &cmd){
    if (int code = std::system(cmd.c_str())) {
      std::stringstream ss;
      ss << code << "\n\"" << cmd << "\"\n" << std::ifstream(log).rdbuf();
      throw std::runtime_error(ss.str());
    }
  };
  mySystem(cmd);
  mySystem("rm " + log);
  if (mParser.verbose) mTimer.stop();
#else
  throw std::runtime_error("MPEG support was disabled during compilation!");
#endif
}// Tool::movie

// ==============================================================================================================

// Image writers used only by Tool::render() below: keep them local to this TU.
namespace {

#ifdef VDB_TOOL_USE_PNG
void savePNG(const std::string& fname, const tools::Film& film)
{
  png_structp png = png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  if (!png) OPENVDB_THROW(RuntimeError, "png_create_write_struct failed");
  png_infop info = info = png_create_info_struct(png);
  if (!info) OPENVDB_THROW(RuntimeError, "png_create_info_struct failed.")

  FILE* fp = std::fopen(fname.c_str(), "wb");
  if (!fp) {
    OPENVDB_THROW(IoError,"Unable to open '" + fname + "' for writing");
  }

  if (setjmp(png_jmpbuf(png))) {
    OPENVDB_THROW(IoError, "Error during initialization of PNG I/O.");
  }
  png_init_io(png, fp);

  if (setjmp(png_jmpbuf(png))) {
    OPENVDB_THROW(IoError, "Error writing PNG file header.");
  }
  // Output is 8bit depth, RGB format.
  png_set_IHDR(png, info,
            int(film.width()), int(film.height()),
            8,
            PNG_COLOR_TYPE_RGB,
            PNG_INTERLACE_NONE,
            PNG_COMPRESSION_TYPE_DEFAULT,
            PNG_FILTER_TYPE_DEFAULT);
  png_write_info(png, info);

  const size_t channels = 3; // 3 = RGB, 4 = RGBA
  auto buffer = film.convertToBitBuffer<uint8_t>(/*alpha=*/false);
  uint8_t* tmp = buffer.get();

  std::unique_ptr<png_bytep[]> row_pointers(new png_bytep[film.height()]);

  for (size_t row = 0; row < film.height(); row++) {
    row_pointers[row] = tmp + (row * film.width() * channels);
  }

  if (setjmp(png_jmpbuf(png))) {
    OPENVDB_THROW(IoError, "Error writing PNG data buffers.");
  }
  /* write out the entire image data in one call */
  png_write_image(png, row_pointers.get());
  png_write_end(png, nullptr);

  std::fclose(fp);
  png_destroy_write_struct(&png, &info);
}// savePNG
#else
void savePNG(const std::string&, const tools::Film&)
{
  OPENVDB_THROW(RuntimeError, "vdb_tool has not been compiled with .png support.");
}// savePNG
#endif

// ==============================================================================================================

#ifdef VDB_TOOL_USE_JPG
void saveJPG(const std::string& fname, const tools::Film& film)
{
  jpeg_error_mgr jerr;
  jpeg_compress_struct cinfo;
  jpeg_create_compress(&cinfo);
  cinfo.err = jpeg_std_error(&jerr);
  FILE* fp = std::fopen(fname.c_str(), "wb");
  if (!fp) OPENVDB_THROW(IoError,"Unable to open '" + fname + "' for writing");
  jpeg_stdio_dest(&cinfo, fp);
  cinfo.image_width      = static_cast<JDIMENSION>(film.width());
  cinfo.image_height     = static_cast<JDIMENSION>(film.height());
  cinfo.input_components = 3;
  cinfo.in_color_space   = JCS_RGB;
  jpeg_set_defaults(&cinfo);
  jpeg_start_compress(&cinfo, TRUE);
  auto buf = film.convertToBitBuffer<uint8_t>(/*alpha=*/false);
  uint8_t *row = buf.get();
  const uint32_t stride = static_cast<uint32_t>(film.width() * 3);
  for (int y = 0; y < film.height(); ++y) {
      jpeg_write_scanlines(&cinfo, &row, 1);
      row += stride;
  }
  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
  std::fclose(fp);
}// saveJPG
#else
void saveJPG(const std::string&, const tools::Film&)
{
  OPENVDB_THROW(RuntimeError, "vdb_tool has not been compiled with .jpg support.");
}// saveJPG
#endif

// ==============================================================================================================

#ifdef VDB_TOOL_USE_EXR
void saveEXR(const std::string& filename, const tools::Film& film, const std::string &compression = "zip")
{
    Imf::setGlobalThreadCount(8);
    Imf::Header header(int(film.width()), int(film.height()));
    if (compression == "none") {
        header.compression() = Imf::NO_COMPRESSION;
    } else if (compression == "rle") {
        header.compression() = Imf::RLE_COMPRESSION;
    } else if (compression == "zip") {
        header.compression() = Imf::ZIP_COMPRESSION;
    } else {
        OPENVDB_THROW(ValueError,
            "expected none, rle or zip compression, got \"" << compression << "\"");
    }
    header.channels().insert("R", Imf::Channel(Imf::FLOAT));
    header.channels().insert("G", Imf::Channel(Imf::FLOAT));
    header.channels().insert("B", Imf::Channel(Imf::FLOAT));
    header.channels().insert("A", Imf::Channel(Imf::FLOAT));
    using RGBA = tools::Film::RGBA;
    const size_t pixelBytes = sizeof(RGBA), rowBytes = pixelBytes * film.width();
    RGBA& pixel0 = const_cast<RGBA*>(film.pixels())[0];
    Imf::FrameBuffer framebuffer;
    framebuffer.insert("R",
        Imf::Slice(Imf::FLOAT, reinterpret_cast<char*>(&pixel0.r), pixelBytes, rowBytes));
    framebuffer.insert("G",
        Imf::Slice(Imf::FLOAT, reinterpret_cast<char*>(&pixel0.g), pixelBytes, rowBytes));
    framebuffer.insert("B",
        Imf::Slice(Imf::FLOAT, reinterpret_cast<char*>(&pixel0.b), pixelBytes, rowBytes));
    framebuffer.insert("A",
        Imf::Slice(Imf::FLOAT, reinterpret_cast<char*>(&pixel0.a), pixelBytes, rowBytes));

    Imf::OutputFile imgFile(filename.c_str(), header);
    imgFile.setFrameBuffer(framebuffer);
    imgFile.writePixels(int(film.height()));
}// saveEXR
#else
void saveEXR(const std::string&, const tools::Film&, const std::string& = "zip")
{
    OPENVDB_THROW(RuntimeError, "vdb_tool has not been compiled with .exr support.");
}// saveEXR
#endif
} // anonymous namespace

// ==============================================================================================================

void Tool::render()
{
  const std::string &action_name = mParser.getAction().names[0];
  OPENVDB_ASSERT(action_name == "render");
  const VecS fileNames = mParser.getVec<std::string>("files");
  const int age = mParser.get<int>("vdb");
  const bool keep = mParser.get<bool>("keep");
  const std::string camType = mParser.get<std::string>("camera");
  const float aperture = mParser.get<float>("aperture");
  const float focal = mParser.get<float>("focal");
  const float isovalue = mParser.get<float>("isovalue");
  const int samples = mParser.get<int>("samples");
  const VecI image = mParser.getVec<int>("image", "x");
  Vec3d translate = mParser.getVec3<double>("translate");
  const Vec3d rotate = mParser.getVec3<double>("rotate");
  Vec3d target = mParser.getVec3<double>("target");
  const Vec3d up = mParser.getVec3<double>("up");
  const bool lookat = mParser.get<bool>("lookat");
  const float znear = mParser.get<float>("near");
  const float zfar = mParser.get<float>("far");
  const std::string shader = mParser.get<std::string>("shader");
  VecF light = mParser.getVec<float>("light");
  const float frame = mParser.get<float>("frame");
  const  float cutoff = mParser.get<float>("cutoff");
  const float gain = mParser.get<float>("gain");
  const Vec3f absorb = mParser.getVec3<float>("absorb");
  const Vec3f scatter = mParser.getVec3<float>("scatter");
  const VecF step = mParser.getVec<float>("step");
  const int colorgrid = mParser.get<int>("colorgrid");

  if (light.size()==3) {
    for (size_t i=0; i<3; ++i) light.push_back(0.7f);
  } else if (light.size()!=6) {
    throw std::invalid_argument("render: \"light\" option expected 3 or 6 values, got "+std::to_string(light.size()));
  }
  if (image.size()!=2) throw std::invalid_argument("render: expected width and height,  e.g. image=1920x1080");
  auto it = this->getGrid(age);
  GridT::Ptr grid = gridPtrCast<GridT>(*it);
  if (!grid || grid->getGridClass() != GRID_LEVEL_SET) {
    throw std::invalid_argument(action_name + ": no level set with age " + std::to_string(age));
  }
  if (step.size()!=2) throw std::invalid_argument("render: \"step\" option expected 2 values, but got "+std::to_string(step.size()));

  tools::Film film(image[0], image[1]);
  const CoordBBox bbox = grid->evalActiveVoxelBoundingBox();
  const math::BBox<Vec3d> bboxIndex(bbox.min().asVec3d(), bbox.max().asVec3d());
  const math::BBox<Vec3R> bboxWorld = bboxIndex.applyMap(*(grid->transform().baseMap()));

  if (lookat && target == Vec3d(0.0) && translate == Vec3d(0.0)) {
    target = bboxWorld.getCenter();
    translate = 3.0*bboxWorld.max();
  }
  Vec3SGrid::Ptr colorgridPtr;
  if (colorgrid>=0) {
    auto it2 = this->getGrid(colorgrid);
    colorgridPtr = gridPtrCast<Vec3SGrid>(*it2);
    if (!colorgridPtr) throw std::invalid_argument("render: no colorgrid of type Vec3f with age "+std::to_string(colorgrid));
  }
  std::unique_ptr<tools::BaseCamera> camera;
  if (startsWith(camType, "persp")) {
    camera.reset(new tools::PerspectiveCamera(film, rotate, translate, focal, aperture, znear, zfar));
  } else if (startsWith(camType, "ortho")) {
    camera.reset(new tools::OrthographicCamera(film, rotate, translate, frame, znear, zfar));
  } else {
    throw std::invalid_argument("render: expected perspective or orthographic camera, got \""+camType+"\"");
  }
  if (lookat) camera->lookAt(target, up);

  // Define the shader for level set rendering.  The default shader is a diffuse shader.
  std::unique_ptr<tools::BaseShader> shaderPtr;
  if (shader == "matte") {
    if (colorgridPtr) {
      shaderPtr.reset(new tools::MatteShader<Vec3SGrid>(*colorgridPtr));
    } else {
      shaderPtr.reset(new tools::MatteShader<>());
    }
  } else if (shader == "normal") {
    if (colorgridPtr) {
      shaderPtr.reset(new tools::NormalShader<Vec3SGrid>(*colorgridPtr));
    } else {
      shaderPtr.reset(new tools::NormalShader<>());
    }
  } else if (shader == "position") {
    if (colorgridPtr) {
      shaderPtr.reset(new tools::PositionShader<Vec3SGrid>(bboxWorld, *colorgridPtr));
    } else {
      shaderPtr.reset(new tools::PositionShader<>(bboxWorld));
    }
  } else if (shader == "diffuse") { // default
    if (colorgridPtr) {
      shaderPtr.reset(new tools::DiffuseShader<Vec3SGrid>(*colorgridPtr));
    } else {
      shaderPtr.reset(new tools::DiffuseShader<>());
    }
  } else {
    throw std::invalid_argument("render: unsupported value of shader=\""+shader+"\"");
  }

  if (grid->getGridClass() == GRID_LEVEL_SET) {
    if (mParser.verbose) mTimer.start("ray-tracing");
    tools::LevelSetRayIntersector<GridT> intersector(*grid, static_cast<GridT::ValueType>(isovalue));
    tools::rayTrace(*grid, intersector, *shaderPtr, *camera, samples, 0, true);
  } else {// volume rendering
    if (mParser.verbose) mTimer.start("volumerendering");
    using IntersectorT = tools::VolumeRayIntersector<GridT>;
    IntersectorT intersector(*grid);
    tools::VolumeRender<IntersectorT> renderer(intersector, *camera);
    renderer.setLightDir(  light[0], light[1], light[2]);
    renderer.setLightColor(light[3], light[4], light[5]);
    renderer.setPrimaryStep(step[0]);
    renderer.setShadowStep(step[1]);
    renderer.setScattering(scatter[0], scatter[1], scatter[2]);
    renderer.setAbsorption(absorb[0], absorb[1], absorb[2]);
    renderer.setLightGain(gain);
    renderer.setCutOff(cutoff);
    renderer.render(true);
  }

#ifdef VDB_TOOL_USE_JPG
  std::string fileName("test.jpg");
#elif VDB_TOOL_USE_PNG
  std::string fileName("test.png");
#else
  std::string fileName("test.ppm");
#endif

  if (fileNames.empty()) {
    if (!grid->getName().empty()) fileName = grid->getName() + "." + getExt(fileName);
  } else {
    fileName = fileNames[0];
  }

  switch (findFileExt(fileName, {"ppm", "png", "jpg", "exr"})) {
  case 1:
    film.savePPM(fileName);
    break;
  case 2:
    savePNG(fileName, film);
    break;
  case 3:
    saveJPG(fileName, film);
    break;
  case 4:
    saveEXR(fileName, film);
    break;
  default:
    throw std::invalid_argument("Image file \""+fileName+"\" has an unrecognized extension");
    break;
  }

  if (!keep) mGrid.erase(std::next(it).base());
  if (mParser.verbose) mTimer.stop();
}

} // namespace vdb_tool
} // namespace OPENVDB_VERSION_NAME
} // namespace openvdb
