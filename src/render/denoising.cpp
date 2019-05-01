/*
 * Copyright 2011-2018 Blender Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "render/denoising.h"

#include "kernel/filter/filter_defines.h"

#include "util/util_foreach.h"
#include "util/util_map.h"
#include "util/util_system.h"
#include "util/util_time.h"

#include <OpenImageIO/filesystem.h>

CCL_NAMESPACE_BEGIN

/* Utility Functions */

static void print_progress(int num, int total, int frame, int num_frames)
{
  const char *label = "Denoise Frame ";
  int cols = system_console_width();

  cols -= strlen(label);

  int len = 1;
  for (int x = total; x > 9; x /= 10) {
    len++;
  }

  int bars = cols - 2 * len - 6;

  printf("\r%s", label);

  if (num_frames > 1) {
    int frame_len = 1;
    for (int x = num_frames - 1; x > 9; x /= 10) {
      frame_len++;
    }
    bars -= frame_len + 2;
    printf("%*d ", frame_len, frame);
  }

  int v = int(float(num) * bars / total);
  printf("[");
  for (int i = 0; i < v; i++) {
    printf("=");
  }
  if (v < bars) {
    printf(">");
  }
  for (int i = v + 1; i < bars; i++) {
    printf(" ");
  }
  printf(string_printf("] %%%dd / %d", len, total).c_str(), num);
  fflush(stdout);
}

/* Splits in at its last dot, setting suffix to the part after the dot and in to the part before
 * it. Returns whether a dot was found. */
static bool split_last_dot(string &in, string &suffix)
{
  size_t pos = in.rfind(".");
  if (pos == string::npos) {
    return false;
  }
  suffix = in.substr(pos + 1);
  in = in.substr(0, pos);
  return true;
}

/* Separate channel names as generated by Blender.
 * If views is true:
 *   Inputs are expected in the form RenderLayer.Pass.View.Channel, sets renderlayer to
 *   "RenderLayer.View" Otherwise: Inputs are expected in the form RenderLayer.Pass.Channel */
static bool parse_channel_name(
    string name, string &renderlayer, string &pass, string &channel, bool multiview_channels)
{
  if (!split_last_dot(name, channel)) {
    return false;
  }
  string view;
  if (multiview_channels && !split_last_dot(name, view)) {
    return false;
  }
  if (!split_last_dot(name, pass)) {
    return false;
  }
  renderlayer = name;

  if (multiview_channels) {
    renderlayer += "." + view;
  }

  return true;
}

/* Channel Mapping */

struct ChannelMapping {
  int channel;
  string name;
};

static void fill_mapping(vector<ChannelMapping> &map, int pos, string name, string channels)
{
  for (const char *chan = channels.c_str(); *chan; chan++) {
    map.push_back({pos++, name + "." + *chan});
  }
}

static const int INPUT_NUM_CHANNELS = 15;
static const int INPUT_DENOISING_DEPTH = 0;
static const int INPUT_DENOISING_NORMAL = 1;
static const int INPUT_DENOISING_SHADOWING = 4;
static const int INPUT_DENOISING_ALBEDO = 5;
static const int INPUT_NOISY_IMAGE = 8;
static const int INPUT_DENOISING_VARIANCE = 11;
static const int INPUT_DENOISING_INTENSITY = 14;
static vector<ChannelMapping> input_channels()
{
  vector<ChannelMapping> map;
  fill_mapping(map, INPUT_DENOISING_DEPTH, "Denoising Depth", "Z");
  fill_mapping(map, INPUT_DENOISING_NORMAL, "Denoising Normal", "XYZ");
  fill_mapping(map, INPUT_DENOISING_SHADOWING, "Denoising Shadowing", "X");
  fill_mapping(map, INPUT_DENOISING_ALBEDO, "Denoising Albedo", "RGB");
  fill_mapping(map, INPUT_NOISY_IMAGE, "Noisy Image", "RGB");
  fill_mapping(map, INPUT_DENOISING_VARIANCE, "Denoising Variance", "RGB");
  fill_mapping(map, INPUT_DENOISING_INTENSITY, "Denoising Intensity", "X");
  return map;
}

static const int OUTPUT_NUM_CHANNELS = 3;
static vector<ChannelMapping> output_channels()
{
  vector<ChannelMapping> map;
  fill_mapping(map, 0, "Combined", "RGB");
  return map;
}

/* Renderlayer Handling */

bool DenoiseImageLayer::detect_denoising_channels()
{
  /* Map device input to image channels. */
  input_to_image_channel.clear();
  input_to_image_channel.resize(INPUT_NUM_CHANNELS, -1);

  foreach (const ChannelMapping &mapping, input_channels()) {
    vector<string>::iterator i = find(channels.begin(), channels.end(), mapping.name);
    if (i == channels.end()) {
      return false;
    }

    size_t input_channel = mapping.channel;
    size_t layer_channel = i - channels.begin();
    input_to_image_channel[input_channel] = layer_to_image_channel[layer_channel];
  }

  /* Map device output to image channels. */
  output_to_image_channel.clear();
  output_to_image_channel.resize(OUTPUT_NUM_CHANNELS, -1);

  foreach (const ChannelMapping &mapping, output_channels()) {
    vector<string>::iterator i = find(channels.begin(), channels.end(), mapping.name);
    if (i == channels.end()) {
      return false;
    }

    size_t output_channel = mapping.channel;
    size_t layer_channel = i - channels.begin();
    output_to_image_channel[output_channel] = layer_to_image_channel[layer_channel];
  }

  /* Check that all buffer channels are correctly set. */
  for (int i = 0; i < INPUT_NUM_CHANNELS; i++) {
    assert(input_to_image_channel[i] >= 0);
  }
  for (int i = 0; i < OUTPUT_NUM_CHANNELS; i++) {
    assert(output_to_image_channel[i] >= 0);
  }

  return true;
}

bool DenoiseImageLayer::match_channels(int neighbor,
                                       const std::vector<string> &channelnames,
                                       const std::vector<string> &neighbor_channelnames)
{
  neighbor_input_to_image_channel.resize(neighbor + 1);
  vector<int> &mapping = neighbor_input_to_image_channel[neighbor];

  assert(mapping.size() == 0);
  mapping.resize(input_to_image_channel.size(), -1);

  for (int i = 0; i < input_to_image_channel.size(); i++) {
    const string &channel = channelnames[input_to_image_channel[i]];
    std::vector<string>::const_iterator frame_channel = find(
        neighbor_channelnames.begin(), neighbor_channelnames.end(), channel);

    if (frame_channel == neighbor_channelnames.end()) {
      return false;
    }

    mapping[i] = frame_channel - neighbor_channelnames.begin();
  }

  return true;
}

/* Denoise Task */

DenoiseTask::DenoiseTask(Device *device,
                         Denoiser *denoiser,
                         int frame,
                         const vector<int> &neighbor_frames)
    : denoiser(denoiser),
      device(device),
      frame(frame),
      neighbor_frames(neighbor_frames),
      current_layer(0),
      input_pixels(device, "filter input buffer", MEM_READ_ONLY),
      num_tiles(0)
{
  image.samples = denoiser->samples_override;
}

DenoiseTask::~DenoiseTask()
{
  free();
}

/* Device callbacks */

bool DenoiseTask::acquire_tile(Device *device, Device *tile_device, RenderTile &tile)
{
  thread_scoped_lock tile_lock(tiles_mutex);

  if (tiles.empty()) {
    return false;
  }

  tile = tiles.front();
  tiles.pop_front();

  device->map_tile(tile_device, tile);

  print_progress(num_tiles - tiles.size(), num_tiles, frame, denoiser->num_frames);

  return true;
}

/* Mapping tiles is required for regular rendering since each tile has its separate memory
 * which may be allocated on a different device.
 * For standalone denoising, there is a single memory that is present on all devices, so the only
 * thing that needs to be done here is to specify the surrounding tile geometry.
 *
 * However, since there is only one large memory, the denoised result has to be written to
 * a different buffer to avoid having to copy an entire horizontal slice of the image. */
void DenoiseTask::map_neighboring_tiles(RenderTile *tiles, Device *tile_device)
{
  /* Fill tile information. */
  for (int i = 0; i < 9; i++) {
    if (i == 4) {
      continue;
    }

    int dx = (i % 3) - 1;
    int dy = (i / 3) - 1;
    tiles[i].x = clamp(tiles[4].x + dx * denoiser->tile_size.x, 0, image.width);
    tiles[i].w = clamp(tiles[4].x + (dx + 1) * denoiser->tile_size.x, 0, image.width) - tiles[i].x;
    tiles[i].y = clamp(tiles[4].y + dy * denoiser->tile_size.y, 0, image.height);
    tiles[i].h = clamp(tiles[4].y + (dy + 1) * denoiser->tile_size.y, 0, image.height) -
                 tiles[i].y;

    tiles[i].buffer = tiles[4].buffer;
    tiles[i].offset = tiles[4].offset;
    tiles[i].stride = image.width;
  }

  /* Allocate output buffer. */
  device_vector<float> *output_mem = new device_vector<float>(
      tile_device, "denoising_output", MEM_READ_WRITE);
  output_mem->alloc(OUTPUT_NUM_CHANNELS * tiles[4].w * tiles[4].h);

  /* Fill output buffer with noisy image, assumed by kernel_filter_finalize
   * when skipping denoising of some pixels. */
  float *result = output_mem->data();
  float *in = &image.pixels[image.num_channels * (tiles[4].y * image.width + tiles[4].x)];

  const DenoiseImageLayer &layer = image.layers[current_layer];
  const int *input_to_image_channel = layer.input_to_image_channel.data();

  for (int y = 0; y < tiles[4].h; y++) {
    for (int x = 0; x < tiles[4].w; x++, result += OUTPUT_NUM_CHANNELS) {
      for (int i = 0; i < OUTPUT_NUM_CHANNELS; i++) {
        result[i] = in[image.num_channels * x + input_to_image_channel[INPUT_NOISY_IMAGE + i]];
      }
    }
    in += image.num_channels * image.width;
  }

  output_mem->copy_to_device();

  /* Fill output tile info. */
  tiles[9] = tiles[4];
  tiles[9].buffer = output_mem->device_pointer;
  tiles[9].stride = tiles[9].w;
  tiles[9].offset -= tiles[9].x + tiles[9].y * tiles[9].stride;

  thread_scoped_lock output_lock(output_mutex);
  assert(output_pixels.count(tiles[4].tile_index) == 0);
  output_pixels[tiles[9].tile_index] = output_mem;
}

void DenoiseTask::unmap_neighboring_tiles(RenderTile *tiles)
{
  thread_scoped_lock output_lock(output_mutex);
  assert(output_pixels.count(tiles[4].tile_index) == 1);
  device_vector<float> *output_mem = output_pixels[tiles[9].tile_index];
  output_pixels.erase(tiles[4].tile_index);
  output_lock.unlock();

  /* Copy denoised pixels from device. */
  output_mem->copy_from_device(0, OUTPUT_NUM_CHANNELS * tiles[9].w, tiles[9].h);

  float *result = output_mem->data();
  float *out = &image.pixels[image.num_channels * (tiles[9].y * image.width + tiles[9].x)];

  const DenoiseImageLayer &layer = image.layers[current_layer];
  const int *output_to_image_channel = layer.output_to_image_channel.data();

  for (int y = 0; y < tiles[9].h; y++) {
    for (int x = 0; x < tiles[9].w; x++, result += OUTPUT_NUM_CHANNELS) {
      for (int i = 0; i < OUTPUT_NUM_CHANNELS; i++) {
        out[image.num_channels * x + output_to_image_channel[i]] = result[i];
      }
    }
    out += image.num_channels * image.width;
  }

  /* Free device buffer. */
  output_mem->free();
  delete output_mem;
}

void DenoiseTask::release_tile()
{
}

bool DenoiseTask::get_cancel()
{
  return false;
}

void DenoiseTask::create_task(DeviceTask &task)
{
  /* Callback functions. */
  task.acquire_tile = function_bind(&DenoiseTask::acquire_tile, this, device, _1, _2);
  task.map_neighbor_tiles = function_bind(&DenoiseTask::map_neighboring_tiles, this, _1, _2);
  task.unmap_neighbor_tiles = function_bind(&DenoiseTask::unmap_neighboring_tiles, this, _1);
  task.release_tile = function_bind(&DenoiseTask::release_tile, this);
  task.get_cancel = function_bind(&DenoiseTask::get_cancel, this);

  /* Denoising parameters. */
  task.denoising = denoiser->params;
  task.denoising_do_filter = true;
  task.denoising_write_passes = false;
  task.denoising_from_render = false;

  task.denoising_frames.resize(neighbor_frames.size());
  for (int i = 0; i < neighbor_frames.size(); i++) {
    task.denoising_frames[i] = neighbor_frames[i] - frame;
  }

  /* Buffer parameters. */
  task.pass_stride = INPUT_NUM_CHANNELS;
  task.target_pass_stride = OUTPUT_NUM_CHANNELS;
  task.pass_denoising_data = 0;
  task.pass_denoising_clean = -1;
  task.frame_stride = image.width * image.height * INPUT_NUM_CHANNELS;

  /* Create tiles. */
  thread_scoped_lock tile_lock(tiles_mutex);
  thread_scoped_lock output_lock(output_mutex);

  tiles.clear();
  assert(output_pixels.empty());
  output_pixels.clear();

  int tiles_x = divide_up(image.width, denoiser->tile_size.x);
  int tiles_y = divide_up(image.height, denoiser->tile_size.y);

  for (int ty = 0; ty < tiles_y; ty++) {
    for (int tx = 0; tx < tiles_x; tx++) {
      RenderTile tile;
      tile.x = tx * denoiser->tile_size.x;
      tile.y = ty * denoiser->tile_size.y;
      tile.w = min(image.width - tile.x, denoiser->tile_size.x);
      tile.h = min(image.height - tile.y, denoiser->tile_size.y);
      tile.start_sample = 0;
      tile.num_samples = image.layers[current_layer].samples;
      tile.sample = 0;
      tile.offset = 0;
      tile.stride = image.width;
      tile.tile_index = ty * tiles_x + tx;
      tile.task = RenderTile::DENOISE;
      tile.buffers = NULL;
      tile.buffer = input_pixels.device_pointer;
      tiles.push_back(tile);
    }
  }

  num_tiles = tiles.size();
}

/* Denoiser Operations */

bool DenoiseTask::load_input_pixels(int layer)
{
  int w = image.width;
  int h = image.height;
  int num_pixels = image.width * image.height;
  int frame_stride = num_pixels * INPUT_NUM_CHANNELS;

  /* Load center image */
  DenoiseImageLayer &image_layer = image.layers[layer];

  float *buffer_data = input_pixels.data();
  image.read_pixels(image_layer, buffer_data);
  buffer_data += frame_stride;

  /* Load neighbor images */
  for (int i = 0; i < image.in_neighbors.size(); i++) {
    if (!image.read_neighbor_pixels(i, image_layer, buffer_data)) {
      error = "Failed to read neighbor frame pixels";
      return false;
    }
    buffer_data += frame_stride;
  }

  /* Preprocess */
  buffer_data = input_pixels.data();
  for (int neighbor = 0; neighbor < image.in_neighbors.size() + 1; neighbor++) {
    /* Clamp */
    if (denoiser->params.clamp_input) {
      for (int i = 0; i < num_pixels * INPUT_NUM_CHANNELS; i++) {
        buffer_data[i] = clamp(buffer_data[i], -1e8f, 1e8f);
      }
    }

    /* Box blur */
    int r = 5 * denoiser->params.radius;
    float *data = buffer_data + 14;
    array<float> temp(num_pixels);

    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        int n = 0;
        float sum = 0.0f;
        for (int dx = max(x - r, 0); dx < min(x + r + 1, w); dx++, n++) {
          sum += data[INPUT_NUM_CHANNELS * (y * w + dx)];
        }
        temp[y * w + x] = sum / n;
      }
    }

    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        int n = 0;
        float sum = 0.0f;

        for (int dy = max(y - r, 0); dy < min(y + r + 1, h); dy++, n++) {
          sum += temp[dy * w + x];
        }

        data[INPUT_NUM_CHANNELS * (y * w + x)] = sum / n;
      }
    }

    buffer_data += frame_stride;
  }

  /* Copy to device */
  input_pixels.copy_to_device();

  return true;
}

/* Task stages */

bool DenoiseTask::load()
{
  string center_filepath = denoiser->input[frame];
  if (!image.load(center_filepath, error)) {
    return false;
  }

  if (!image.load_neighbors(denoiser->input, neighbor_frames, error)) {
    return false;
  }

  if (image.layers.empty()) {
    error = "No image layers found to denoise in " + center_filepath;
    return false;
  }

  /* Allocate device buffer. */
  int num_frames = image.in_neighbors.size() + 1;
  input_pixels.alloc(image.width * INPUT_NUM_CHANNELS, image.height * num_frames);
  input_pixels.zero_to_device();

  /* Read pixels for first layer. */
  current_layer = 0;
  if (!load_input_pixels(current_layer)) {
    return false;
  }

  return true;
}

bool DenoiseTask::exec()
{
  for (current_layer = 0; current_layer < image.layers.size(); current_layer++) {
    /* Read pixels for secondary layers, first was already loaded. */
    if (current_layer > 0) {
      if (!load_input_pixels(current_layer)) {
        return false;
      }
    }

    /* Run task on device. */
    DeviceTask task(DeviceTask::RENDER);
    create_task(task);
    device->task_add(task);
    device->task_wait();

    printf("\n");
  }

  return true;
}

bool DenoiseTask::save()
{
  bool ok = image.save_output(denoiser->output[frame], error);
  free();
  return ok;
}

void DenoiseTask::free()
{
  image.free();
  input_pixels.free();
  assert(output_pixels.empty());
}

/* Denoise Image Storage */

DenoiseImage::DenoiseImage()
{
  width = 0;
  height = 0;
  num_channels = 0;
  samples = 0;
}

DenoiseImage::~DenoiseImage()
{
  free();
}

void DenoiseImage::close_input()
{
  in_neighbors.clear();
}

void DenoiseImage::free()
{
  close_input();
  pixels.clear();
}

bool DenoiseImage::parse_channels(const ImageSpec &in_spec, string &error)
{
  const std::vector<string> &channels = in_spec.channelnames;
  const ParamValue *multiview = in_spec.find_attribute("multiView");
  const bool multiview_channels = (multiview && multiview->type().basetype == TypeDesc::STRING &&
                                   multiview->type().arraylen >= 2);

  layers.clear();

  /* Loop over all the channels in the file, parse their name and sort them
   * by RenderLayer.
   * Channels that can't be parsed are directly passed through to the output. */
  map<string, DenoiseImageLayer> file_layers;
  for (int i = 0; i < channels.size(); i++) {
    string layer, pass, channel;
    if (parse_channel_name(channels[i], layer, pass, channel, multiview_channels)) {
      file_layers[layer].channels.push_back(pass + "." + channel);
      file_layers[layer].layer_to_image_channel.push_back(i);
    }
  }

  /* Loop over all detected RenderLayers, check whether they contain a full set of input channels.
   * Any channels that won't be processed internally are also passed through. */
  for (map<string, DenoiseImageLayer>::iterator i = file_layers.begin(); i != file_layers.end();
       ++i) {
    const string &name = i->first;
    DenoiseImageLayer &layer = i->second;

    /* Check for full pass set. */
    if (!layer.detect_denoising_channels()) {
      continue;
    }

    layer.name = name;
    layer.samples = samples;

    /* If the sample value isn't set yet, check if there is a layer-specific one in the input file.
     */
    if (layer.samples < 1) {
      string sample_string = in_spec.get_string_attribute("cycles." + name + ".samples", "");
      if (sample_string != "") {
        if (!sscanf(sample_string.c_str(), "%d", &layer.samples)) {
          error = "Failed to parse samples metadata: " + sample_string;
          return false;
        }
      }
    }

    if (layer.samples < 1) {
      error = string_printf(
          "No sample number specified in the file for layer %s or on the command line",
          name.c_str());
      return false;
    }

    layers.push_back(layer);
  }

  return true;
}

void DenoiseImage::read_pixels(const DenoiseImageLayer &layer, float *input_pixels)
{
  /* Pixels from center file have already been loaded into pixels.
   * We copy a subset into the device input buffer with channels reshuffled. */
  const int *input_to_image_channel = layer.input_to_image_channel.data();

  for (int i = 0; i < width * height; i++) {
    for (int j = 0; j < INPUT_NUM_CHANNELS; j++) {
      int image_channel = input_to_image_channel[j];
      input_pixels[i * INPUT_NUM_CHANNELS + j] =
          pixels[((size_t)i) * num_channels + image_channel];
    }
  }
}

bool DenoiseImage::read_neighbor_pixels(int neighbor,
                                        const DenoiseImageLayer &layer,
                                        float *input_pixels)
{
  /* Load pixels from neighboring frames, and copy them into device buffer
   * with channels reshuffled. */
  size_t num_pixels = (size_t)width * (size_t)height;
  array<float> neighbor_pixels(num_pixels * num_channels);
  if (!in_neighbors[neighbor]->read_image(TypeDesc::FLOAT, neighbor_pixels.data())) {
    return false;
  }

  const int *input_to_image_channel = layer.neighbor_input_to_image_channel[neighbor].data();

  for (int i = 0; i < width * height; i++) {
    for (int j = 0; j < INPUT_NUM_CHANNELS; j++) {
      int image_channel = input_to_image_channel[j];
      input_pixels[i * INPUT_NUM_CHANNELS + j] =
          neighbor_pixels[((size_t)i) * num_channels + image_channel];
    }
  }

  return true;
}

bool DenoiseImage::load(const string &in_filepath, string &error)
{
  if (!Filesystem::is_regular(in_filepath)) {
    error = "Couldn't find file: " + in_filepath;
    return false;
  }

  unique_ptr<ImageInput> in(ImageInput::open(in_filepath));
  if (!in) {
    error = "Couldn't open file: " + in_filepath;
    return false;
  }

  in_spec = in->spec();
  width = in_spec.width;
  height = in_spec.height;
  num_channels = in_spec.nchannels;

  if (!parse_channels(in_spec, error)) {
    return false;
  }

  if (layers.size() == 0) {
    error = "Could not find a render layer containing denoising info";
    return false;
  }

  size_t num_pixels = (size_t)width * (size_t)height;
  pixels.resize(num_pixels * num_channels);

  /* Read all channels into buffer. Reading all channels at once is faster
   * than individually due to interleaved EXR channel storage. */
  if (!in->read_image(TypeDesc::FLOAT, pixels.data())) {
    error = "Failed to read image: " + in_filepath;
    return false;
  }

  return true;
}

bool DenoiseImage::load_neighbors(const vector<string> &filepaths,
                                  const vector<int> &frames,
                                  string &error)
{
  if (frames.size() > DENOISE_MAX_FRAMES - 1) {
    error = string_printf("Maximum number of neighbors (%d) exceeded\n", DENOISE_MAX_FRAMES - 1);
    return false;
  }

  for (int neighbor = 0; neighbor < frames.size(); neighbor++) {
    int frame = frames[neighbor];
    const string &filepath = filepaths[frame];

    if (!Filesystem::is_regular(filepath)) {
      error = "Couldn't find neighbor frame: " + filepath;
      return false;
    }

    unique_ptr<ImageInput> in_neighbor(ImageInput::open(filepath));
    if (!in_neighbor) {
      error = "Couldn't open neighbor frame: " + filepath;
      return false;
    }

    const ImageSpec &neighbor_spec = in_neighbor->spec();
    if (neighbor_spec.width != width || neighbor_spec.height != height) {
      error = "Neighbor frame has different dimensions: " + filepath;
      return false;
    }

    foreach (DenoiseImageLayer &layer, layers) {
      if (!layer.match_channels(neighbor, in_spec.channelnames, neighbor_spec.channelnames)) {
        error = "Neighbor frame misses denoising data passes: " + filepath;
        return false;
      }
    }

    in_neighbors.push_back(std::move(in_neighbor));
  }

  return true;
}

bool DenoiseImage::save_output(const string &out_filepath, string &error)
{
  /* Save image with identical dimensions, channels and metadata. */
  ImageSpec out_spec = in_spec;

  /* Ensure that the output frame contains sample information even if the input didn't. */
  for (int i = 0; i < layers.size(); i++) {
    string name = "cycles." + layers[i].name + ".samples";
    if (!out_spec.find_attribute(name, TypeDesc::STRING)) {
      out_spec.attribute(name, TypeDesc::STRING, string_printf("%d", layers[i].samples));
    }
  }

  /* We don't need input anymore at this point, and will possibly
   * overwrite the same file. */
  close_input();

  /* Write to temporary file path, so we denoise images in place and don't
   * risk destroying files when something goes wrong in file saving. */
  string extension = OIIO::Filesystem::extension(out_filepath);
  string unique_name = ".denoise-tmp-" + OIIO::Filesystem::unique_path();
  string tmp_filepath = out_filepath + unique_name + extension;
  unique_ptr<ImageOutput> out(ImageOutput::create(tmp_filepath));

  if (!out) {
    error = "Failed to open temporary file " + tmp_filepath + " for writing";
    return false;
  }

  /* Open temporary file and write image buffers. */
  if (!out->open(tmp_filepath, out_spec)) {
    error = "Failed to open file " + tmp_filepath + " for writing: " + out->geterror();
    return false;
  }

  bool ok = true;
  if (!out->write_image(TypeDesc::FLOAT, pixels.data())) {
    error = "Failed to write to file " + tmp_filepath + ": " + out->geterror();
    ok = false;
  }

  if (!out->close()) {
    error = "Failed to save to file " + tmp_filepath + ": " + out->geterror();
    ok = false;
  }

  out.reset();

  /* Copy temporary file to outputput filepath. */
  string rename_error;
  if (ok && !OIIO::Filesystem::rename(tmp_filepath, out_filepath, rename_error)) {
    error = "Failed to move denoised image to " + out_filepath + ": " + rename_error;
    ok = false;
  }

  if (!ok) {
    OIIO::Filesystem::remove(tmp_filepath);
  }

  return ok;
}

/* File pattern handling and outer loop over frames */

Denoiser::Denoiser(DeviceInfo &device_info)
{
  samples_override = 0;
  tile_size = make_int2(64, 64);

  num_frames = 0;

  /* Initialize task scheduler. */
  TaskScheduler::init();

  /* Initialize device. */
  DeviceRequestedFeatures req;
  device = Device::create(device_info, stats, profiler, true);
  device->load_kernels(req);
}

Denoiser::~Denoiser()
{
  delete device;
  TaskScheduler::exit();
}

bool Denoiser::run()
{
  assert(input.size() == output.size());

  num_frames = output.size();

  for (int frame = 0; frame < num_frames; frame++) {
    /* Skip empty output paths. */
    if (output[frame].empty()) {
      continue;
    }

    /* Determine neighbor frame numbers that should be used for filtering. */
    vector<int> neighbor_frames;
    for (int f = frame - params.neighbor_frames; f <= frame + params.neighbor_frames; f++) {
      if (f >= 0 && f < num_frames && f != frame) {
        neighbor_frames.push_back(f);
      }
    }

    /* Execute task. */
    DenoiseTask task(device, this, frame, neighbor_frames);
    if (!task.load()) {
      error = task.error;
      return false;
    }

    if (!task.exec()) {
      error = task.error;
      return false;
    }

    if (!task.save()) {
      error = task.error;
      return false;
    }

    task.free();
  }

  return true;
}

CCL_NAMESPACE_END
