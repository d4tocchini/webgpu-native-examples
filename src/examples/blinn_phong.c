#include "example_base.h"

#include <string.h>

/* -------------------------------------------------------------------------- *
 * WebGPU Example - Blinn-Phong Lighting example
 *
 * Ref:
 * https://github.com/jack1232/ebook-webgpu-lighting/tree/main/src/examples/ch04
 *
 * Note:
 * https://learnopengl.com/Advanced-Lighting/Advanced-Lighting
 * -------------------------------------------------------------------------- */

/* -------------------------------------------------------------------------- *
 * webgpu-simplified - Enums
 * -------------------------------------------------------------------------- */

/** The enumeration for specifying the type of a GPU buffer. */
typedef enum bufer_type_enum {
  BufferType_Uniform          = 0,
  BufferType_Vertex           = 1,
  BufferType_Index            = 2,
  BufferType_Storage          = 3,
  BufferType_Vertex_Storage   = 4,
  BufferType_Index_Storage    = 5,
  BufferType_Indirect         = 6,
  BufferType_Indirect_Storage = 7,
  BufferType_Read             = 8,
  BufferType_Write            = 9,
} bufer_type_enum;

/** The enumeration for specifying the type of input data. */
typedef enum array_data_type_enum {
  DataType_Float32Array = 0,
  DataType_Float64Array = 1,
  DataType_Uint16Array  = 2,
  DataType_Uint32Array  = 3,
} array_data_type_enum;

/* -------------------------------------------------------------------------- *
 * webgpu-simplified - Data types
 * -------------------------------------------------------------------------- */

/**
 * @brief Interface as output of the `initWebGPU` function.
 */
typedef struct iweb_gpu_init_t {
  /** The GPU device */
  WGPUDevice device;
  /** The GPU texture format */
  WGPUTextureFormat format;
  /** The canvas size */
  WGPUExtent2D size;
  /** The background color for the scene */
  WGPUColor background;
  /** MSAA count (1 or 4) */
  uint32_t msaa_count;
} iweb_gpu_init_t;

#define PIPELINE_COUNT 4
#define VERTEX_BUFFER_COUNT 4
#define UNIFORM_BUFFER_COUNT 4
#define UNIFORM_BIND_GROUP_COUNT 4
#define GPU_TEXTURE_COUNT 1
#define DEPTH_TEXTURE_COUNT 1

typedef struct iPipeline_t {
  /** The render pipelines */
  WGPURenderPipeline pipelines[PIPELINE_COUNT];
  /** The vertex buffer array */
  WGPUBuffer vertex_buffers[VERTEX_BUFFER_COUNT];
  /** The uniform buffer array */
  WGPUBuffer uniform_buffers[UNIFORM_BUFFER_COUNT];
  /** The uniform bind group array */
  WGPUBindGroup uniform_bind_groups[UNIFORM_BIND_GROUP_COUNT];
  /** The GPU texture array */
  WGPUTexture gpu_textures[GPU_TEXTURE_COUNT];
  WGPUTexture gpu_texture_views[GPU_TEXTURE_COUNT];
  /** The depth texture array */
  WGPUTexture depth_textures[DEPTH_TEXTURE_COUNT];
  WGPUTexture depth_texture_views[DEPTH_TEXTURE_COUNT];
} iPipeline_t;

typedef struct range_t {
  const void* ptr;
  size_t size;
} range_t;

typedef struct ivertex_data_t {
  range_t positions;
  range_t colors;
  range_t normals;
  range_t uvs;
  range_t indices;
  range_t indices2;
} ivertex_data_t;

/* -------------------------------------------------------------------------- *
 * webgpu-simplified - Functions
 * -------------------------------------------------------------------------- */

/**
 * @brief This function is used to initialize the WebGPU apps. It returns the
 * iweb_gpu_init_t interface.
 *
 * @param wgpu_context the WebGPU context
 * @param msaa_count the MSAA count
 * @param iweb_gpu_init_t object
 */
iweb_gpu_init_t init_web_gpu(wgpu_context_t* wgpu_context, uint32_t msaa_count)
{
  return (iweb_gpu_init_t) {
    .device = wgpu_context->device,
    .format = wgpu_context->swap_chain.format,
      .size = (WGPUExtent2D) {
      .width = wgpu_context->surface.width,
      .height = wgpu_context->surface.height,
    },
    .background = (WGPUColor) {
      .r = 0.009f,
      .g = 0.0125,
      .b = 0.0164f,
      .a = 1.0f
    },
    .msaa_count = msaa_count,
  };
}

WGPUBufferUsageFlags
get_buffer_usage_flags_from_buffer_type(bufer_type_enum buffer_type)
{
  WGPUBufferUsageFlags common_flags
    = WGPUBufferUsage_CopyDst | WGPUBufferUsage_CopySrc;
  WGPUBufferUsageFlags flag = WGPUBufferUsage_Uniform | common_flags;
  if (buffer_type == BufferType_Vertex) {
    flag = WGPUBufferUsage_Vertex | common_flags;
  }
  else if (buffer_type == BufferType_Index) {
    flag = WGPUBufferUsage_Index | common_flags;
  }
  else if (buffer_type == BufferType_Storage) {
    flag = WGPUBufferUsage_Storage | common_flags;
  }
  else if (buffer_type == BufferType_Vertex_Storage) {
    flag = WGPUBufferUsage_Vertex | WGPUBufferUsage_Storage | common_flags;
  }
  else if (buffer_type == BufferType_Index_Storage) {
    flag = WGPUBufferUsage_Index | WGPUBufferUsage_Storage | common_flags;
  }
  else if (buffer_type == BufferType_Indirect) {
    flag = WGPUBufferUsage_Indirect | common_flags;
  }
  else if (buffer_type == BufferType_Indirect_Storage) {
    flag = WGPUBufferUsage_Indirect | WGPUBufferUsage_Storage | common_flags;
  }
  else if (buffer_type == BufferType_Read) {
    flag = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
  }
  else if (buffer_type == BufferType_Write) {
    flag = WGPUBufferUsage_MapWrite | common_flags;
  }
  return flag;
}

/**
 * @brief This function can be used to create vertex, uniform, or storage GPU
 * buffer. The default is a uniform buffer.
 * @param device GPU device
 * @param bufferSize Buffer size.
 * @param buffer_type Of the `buffer_type` enum.
 */
WGPUBuffer create_buffer(WGPUDevice device, size_t buffer_size,
                         bufer_type_enum buffer_type)
{
  return wgpuDeviceCreateBuffer(
    device, &(WGPUBufferDescriptor){
              .usage = get_buffer_usage_flags_from_buffer_type(buffer_type),
              .size  = buffer_size,
              .mappedAtCreation = false,
            });
}

/**
 * @brief This function creats a GPU buffer with data to initialize it. If the
 * input data is a type of `Float32Array` or `Float64Array`, it returns a
 * vertex, uniform, or storage buffer specified by the enum `bufferType`.
 * Otherwise, if the input data has a `Uint16Array` or `Uint32Array`, this
 * function will return an index buffer.
 * @param device GPU device
 * @param data Input data that should be one of four data types: `Float32Array`,
 * `Float64Array`, `Uint16Array`, and `Uint32Array`
 * @param bufferType Type of enum `BufferType`. It is used to specify the type
 * of the returned buffer. The default is vertex buffer
 */
WGPUBuffer create_buffer_with_data(WGPUDevice device, const void* data,
                                   size_t data_byte_length,
                                   array_data_type_enum array_data_type,
                                   bufer_type_enum buffer_type)
{
  WGPUBufferUsageFlags flag
    = get_buffer_usage_flags_from_buffer_type(buffer_type);
  if (buffer_type == BufferType_Vertex
      && (array_data_type == DataType_Uint16Array
          || array_data_type == DataType_Uint32Array)) {
    flag = WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst
           | WGPUBufferUsage_CopySrc;
  }
  WGPUBuffer buffer = wgpuDeviceCreateBuffer(device, &(WGPUBufferDescriptor){
                                                       .usage = flag,
                                                       .size = data_byte_length,
                                                       .mappedAtCreation = true,
                                                     });
  ASSERT(buffer != NULL);
  if (array_data_type == DataType_Uint32Array) {
    uint32_t* mapping
      = (uint32_t*)wgpuBufferGetMappedRange(buffer, 0, data_byte_length);
    memcpy(mapping, data, data_byte_length);
  }
  else if (array_data_type == DataType_Uint16Array) {
    uint16_t* mapping
      = (uint16_t*)wgpuBufferGetMappedRange(buffer, 0, data_byte_length);
    memcpy(mapping, data, data_byte_length);
  }
  else if (array_data_type == DataType_Float64Array) {
    double* mapping
      = (double*)wgpuBufferGetMappedRange(buffer, 0, data_byte_length);
    memcpy(mapping, data, data_byte_length);
  }
  else {
    float* mapping
      = (float*)wgpuBufferGetMappedRange(buffer, 0, data_byte_length);
    memcpy(mapping, data, data_byte_length);
  }

  wgpuBufferUnmap(buffer);
  return buffer;
}

/* -------------------------------------------------------------------------- *
 * Blinn-Phong Lighting example
 * -------------------------------------------------------------------------- */

static iPipeline_t prepare_render_pipelines(iweb_gpu_init_t* init,
                                            ivertex_data_t* data)
{
  /* create vertex and index buffers */
  WGPUBuffer position_buffer = create_buffer_with_data(
    init->device, data->positions.ptr, data->positions.size,
    DataType_Float32Array, BufferType_Vertex);
  WGPUBuffer normal_buffer = create_buffer_with_data(
    init->device, data->normals.ptr, data->normals.size, DataType_Float32Array,
    BufferType_Vertex);
  WGPUBuffer index_buffer = create_buffer_with_data(
    init->device, data->indices.ptr, data->indices.size, DataType_Uint32Array,
    BufferType_Vertex);
  WGPUBuffer index_buffer_2 = create_buffer_with_data(
    init->device, data->indices2.ptr, data->indices2.size, DataType_Uint32Array,
    BufferType_Vertex);

  /* uniform buffer for model-matrix, vp-matrix, and normal-matrix */
  WGPUBuffer view_uniform_buffer
    = create_buffer(init->device, 192, BufferType_Uniform);

  /* light uniform buffers for shape and wireframe */
  WGPUBuffer shape_uniform_buffer
    = create_buffer(init->device, 64, BufferType_Uniform);
  WGPUBuffer wireframe_uniform_buffer
    = create_buffer(init->device, 64, BufferType_Uniform);

  /* uniform buffer for material */
  WGPUBuffer material_uniform_buffer
    = create_buffer(init->device, 16, BufferType_Uniform);

  return (iPipeline_t){
    .vertex_buffers
    = {position_buffer, normal_buffer, index_buffer, index_buffer_2},
    .uniform_buffers = {view_uniform_buffer, shape_uniform_buffer,
                        wireframe_uniform_buffer, material_uniform_buffer},
  };
}
