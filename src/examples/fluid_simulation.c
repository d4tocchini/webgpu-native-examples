#include "example_base.h"
#include "examples.h"

#include <string.h>

/* -------------------------------------------------------------------------- *
 * WebGPU Example - Fluid Simulation
 *
 * WebGPU demo featuring an implementation of Jos Stam's "Real-Time Fluid
 * Dynamics for Games" paper.
 *
 * Ref:
 * JavaScript version: https://github.com/indiana-dev/WebGPU-Fluid-Simulation
 * Jos Stam Paper :
 * https://www.dgp.toronto.edu/public_user/stam/reality/Research/pdf/GDC03.pdf
 * Nvidia GPUGem's Chapter 38 :
 * https://developer.nvidia.com/gpugems/gpugems/part-vi-beyond-triangles/chapter-38-fast-fluid-dynamics-simulation-gpu
 * Jamie Wong's Fluid simulation :
 * https://jamie-wong.com/2016/08/05/webgl-fluid-simulation/ PavelDoGreat's
 * Fluid simulation : https://github.com/PavelDoGreat/WebGL-Fluid-Simulation
 * -------------------------------------------------------------------------- */

#define MAX_DIMENSIONS 3

static struct {
  uint32_t grid_size;
  uint32_t grid_w;
  uint32_t grid_h;
  uint32_t dye_size;
  uint32_t dye_w;
  uint32_t dye_h;
  uint32_t rdx;
  uint32_t dye_rdx;
  float dx;
  uint32_t sim_speed;
  bool contain_fluid;
  float velocity_add_intensity;
  float velocity_add_radius;
  float velocity_diffusion;
  float dye_add_intensity;
  float dye_add_radius;
  float dye_diffusion;
  float viscosity;
  uint32_t vorticity;
  uint32_t pressure_iterations;
} settings = {
  .grid_size              = 512,
  .dye_size               = 2048,
  .sim_speed              = 5,
  .contain_fluid          = true,
  .velocity_add_intensity = 0.1f,
  .velocity_add_radius    = 0.0001f,
  .velocity_diffusion     = 0.9999f,
  .dye_add_intensity      = 4.0f,
  .dye_add_radius         = 0.001f,
  .dye_diffusion          = 0.994f,
  .viscosity              = 0.8f,
  .vorticity              = 2,
  .pressure_iterations    = 100,
};

static struct {
  vec2 current;
  vec2 last;
  vec2 velocity;
} mouse_infos = {
  .current  = GLM_VEC2_ZERO_INIT,
  .last     = GLM_VEC2_ZERO_INIT,
  .velocity = GLM_VEC2_ZERO_INIT,
};

/* -------------------------------------------------------------------------- *
 * Dynamic buffer
 * -------------------------------------------------------------------------- */

/**
 * Creates and manage multi-dimensional buffers by creating a buffer for each
 * dimension
 */
typedef struct {
  wgpu_context_t* wgpu_context;          /* The WebGPU context*/
  uint32_t dims;                         /* Number of dimensions */
  uint32_t buffer_size;                  /* Size of the buffer in bytes */
  uint32_t w;                            /* Buffer width */
  uint32_t h;                            /* Buffer height */
  wgpu_buffer_t buffers[MAX_DIMENSIONS]; /* Multi-dimensional buffers */
} dynamic_buffer_t;

static void dynamic_buffer_init_defaults(dynamic_buffer_t* this)
{
  memset(this, 0, sizeof(*this));
}

static void dynamic_buffer_init(dynamic_buffer_t* this,
                                wgpu_context_t* wgpu_context, uint32_t dims,
                                uint32_t w, uint32_t h)
{
  dynamic_buffer_init_defaults(this);

  this->wgpu_context = wgpu_context;
  this->dims         = dims;
  this->buffer_size  = w * h * 4;
  this->w            = w;
  this->h            = h;

  assert(dims <= MAX_DIMENSIONS);
  for (uint32_t dim = 0; dim < dims; ++dim) {
    WGPUBufferDescriptor buffer_desc = {
      .usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc
               | WGPUBufferUsage_CopyDst,
      .size             = this->buffer_size,
      .mappedAtCreation = false,
    };
    this->buffers[dim] = (wgpu_buffer_t){
      .buffer = wgpuDeviceCreateBuffer(wgpu_context->device, &buffer_desc),
      .usage  = buffer_desc.usage,
      .size   = buffer_desc.size,
    };
  }
}

static void dynamic_buffer_destroy(dynamic_buffer_t* this)
{
  for (uint32_t i = 0; i < this->dims; ++i) {
    wgpu_destroy_buffer(&this->buffers[i]);
  }
}

/**
 * Copy each buffer to another DynamicBuffer's buffers.
 * If the dimensions don't match, the last non-empty dimension will be copied
 * instead
 */
static void dynamic_buffer_copy_to(dynamic_buffer_t* this,
                                   dynamic_buffer_t* buffer,
                                   WGPUCommandEncoder command_encoder)
{
  for (uint32_t i = 0; i < MAX(this->dims, buffer->dims); ++i) {
    wgpuCommandEncoderCopyBufferToBuffer(
      command_encoder, this->buffers[MIN(i, this->dims - 1)].buffer, 0,
      buffer->buffers[MIN(i, buffer->dims - 1)].buffer, 0, this->buffer_size);
  }
}

/* Reset all the buffers */
static void dynamic_buffer_clear(dynamic_buffer_t* this)
{
  float* empty_buffer = (float*)malloc(this->buffer_size);
  memset(empty_buffer, 0, this->buffer_size);

  for (uint32_t i = 0; i < this->dims; ++i) {
    wgpu_queue_write_buffer(this->wgpu_context, this->buffers[i].buffer, 0,
                            empty_buffer, this->buffer_size);
  }

  free(empty_buffer);
}

static struct {
  dynamic_buffer_t velocity;
  dynamic_buffer_t velocity0;

  dynamic_buffer_t dye;
  dynamic_buffer_t dye0;

  dynamic_buffer_t divergence;
  dynamic_buffer_t divergence0;

  dynamic_buffer_t pressure;
  dynamic_buffer_t pressure0;

  dynamic_buffer_t vorticity;

  /* The r,g,b buffer containing the data to render */
  dynamic_buffer_t rgb_buffer;
} dynamic_buffers;

/* Initialize dynamic buffers */
static void dynamic_buffers_init(wgpu_context_t* wgpu_context)
{
  dynamic_buffer_init(&dynamic_buffers.velocity, wgpu_context, 2,
                      settings.grid_w, settings.grid_h);
  dynamic_buffer_init(&dynamic_buffers.velocity0, wgpu_context, 2,
                      settings.grid_w, settings.grid_h);

  dynamic_buffer_init(&dynamic_buffers.dye, wgpu_context, 3, settings.dye_w,
                      settings.dye_h);
  dynamic_buffer_init(&dynamic_buffers.dye0, wgpu_context, 4, settings.dye_w,
                      settings.dye_h);

  dynamic_buffer_init(&dynamic_buffers.divergence, wgpu_context, 1,
                      settings.grid_w, settings.grid_h);
  dynamic_buffer_init(&dynamic_buffers.divergence0, wgpu_context, 1,
                      settings.grid_w, settings.grid_h);

  dynamic_buffer_init(&dynamic_buffers.pressure, wgpu_context, 1,
                      settings.grid_w, settings.grid_h);
  dynamic_buffer_init(&dynamic_buffers.pressure0, wgpu_context, 1,
                      settings.grid_w, settings.grid_h);

  dynamic_buffer_init(&dynamic_buffers.vorticity, wgpu_context, 1,
                      settings.grid_w, settings.grid_h);
}

/* -------------------------------------------------------------------------- *
 * Uniforms
 * -------------------------------------------------------------------------- */

typedef enum {
  UNIFORM_TIME,                   /* time */
  UNIFORM_DT,                     /* dt */
  UNIFORM_MOUSE_INFOS,            /* mouseInfos */
  UNIFORM_GRID_SIZE,              /* gridSize */
  UNIFORM_SIM_SPEED,              /* sim_speed */
  UNIFORM_VELOCITY_ADD_INTENSITY, /* velocity_add_intensity */
  UNIFORM_VELOCITY_ADD_RADIUS,    /* velocity_add_radius */
  UNIFORM_VELOCITY_DIFFUSION,     /* velocity_diffusion */
  UNIFORM_DYE_ADD_INTENSITY,      /* dye_add_intensity */
  UNIFORM_DYE_ADD_RADIUS,         /* dye_add_radius */
  UNIFORM_DYE_ADD_DIFFUSION,      /* dye_diffusion */
  UNIFORM_VISCOSITY,              /* viscosity */
  UNIFORM_VORTICITY,              /* vorticity */
  UNIFORM_CONTAIN_FLUID,          /* contain_fluid */
  UNIFORM_MOUSE_TYPE,             /* mouse_type */
  UNIFORM_RENDER_INTENSITY,       /* render_intensity_multiplier */
  UNIFORM_RENDER_DYE,             /* render_dye_buffer */
  UNIFORM_COUNT,
} uniform_type_t;

/* Manage uniform buffers relative to the compute shaders & the gui */
typedef struct {
  uniform_type_t type;
  size_t size;
  bool always_update;
  bool needs_update;
  wgpu_buffer_t buffer;
} uniform_t;

static void uniform_init_defaults(uniform_t* this)
{
  memset(this, 0, sizeof(*this));
}

static void uniform_init(uniform_t* this, wgpu_context_t* wgpu_context,
                         uniform_type_t type, uint32_t size, float* value)
{
  this->type         = type;
  this->size         = size;
  this->needs_update = false;

  this->always_update = (size == 1);

  if (this->size > 0 && value != NULL) {
    this->buffer
      = wgpu_create_buffer(wgpu_context, &(wgpu_buffer_desc_t){
                                           .usage = WGPUBufferUsage_Uniform
                                                    | WGPUBufferUsage_CopyDst,
                                           .size         = this->size * 4,
                                           .initial.data = value,
                                         });
  }
}

/* Update the GPU buffer if the value has changed */
static void uniform_update(uniform_t* this, wgpu_context_t* wgpu_context,
                           float* value)
{
  if (this->needs_update || this->always_update || value != NULL) {
    wgpu_queue_write_buffer(wgpu_context, this->buffer.buffer, 0, value,
                            this->size);
    this->needs_update = false;
  }
}

static struct {
  uniform_t time;
  uniform_t dt;
  uniform_t mouse;
  uniform_t grid;
  uniform_t sim_speed;
  uniform_t vel_force;
  uniform_t vel_radius;
  uniform_t vel_diff;
  uniform_t dye_force;
  uniform_t dye_radius;
  uniform_t dye_diff;
  uniform_t viscosity;
  uniform_t u_vorticity;
  uniform_t contain_fluid;
  uniform_t u_symmetry;
  uniform_t u_render_intensity;
  uniform_t u_render_dye;
} uniforms;

/* -------------------------------------------------------------------------- *
 * Initialization
 * -------------------------------------------------------------------------- */

/* Downscale if necessary to prevent crashes */
static WGPUExtent3D get_valid_dimensions(uint32_t w, uint32_t h,
                                         uint64_t max_buffer_size,
                                         uint64_t max_canvas_size)
{
  float down_ratio = 1.0f;

  /* Prevent buffer size overflow */
  if (w * h * 4 >= max_buffer_size) {
    down_ratio = sqrt(max_buffer_size / (float)(w * h * 4));
  }

  /* Prevent canvas size overflow */
  if (w > max_canvas_size) {
    down_ratio = max_canvas_size / (float)w;
  }
  else if (h > max_canvas_size) {
    down_ratio = max_canvas_size / (float)h;
  }

  return (WGPUExtent3D){
    .width  = floor(w * down_ratio),
    .height = floor(h * down_ratio),
  };
}

/* Fit to screen while keeping the aspect ratio */
static WGPUExtent3D get_preferred_dimensions(uint32_t size,
                                             wgpu_context_t* wgpu_context,
                                             uint64_t max_buffer_size,
                                             uint64_t max_canvas_size)
{
  const float aspect_ratio
    = (float)wgpu_context->surface.width / (float)wgpu_context->surface.height;

  uint32_t w = 0, h = 0;

  if (wgpu_context->surface.height < wgpu_context->surface.width) {
    w = floor(size * aspect_ratio);
    h = size;
  }
  else {
    w = size;
    h = floor(size / aspect_ratio);
  }

  return get_valid_dimensions(w, h, max_buffer_size, max_canvas_size);
}

static void init_sizes(wgpu_context_t* wgpu_context)
{
  uint64_t max_buffer_size          = 0;
  uint64_t max_canvas_size          = 0;
  WGPUSupportedLimits device_limits = {0};
  if (wgpuAdapterGetLimits(wgpu_context->adapter, &device_limits)) {
    max_buffer_size = device_limits.limits.maxStorageBufferBindingSize;
    max_canvas_size = device_limits.limits.maxTextureDimension2D;
  }

  /* Calculate simulation buffer dimensions */
  WGPUExtent3D grid_size = get_preferred_dimensions(
    settings.grid_size, wgpu_context, max_buffer_size, max_canvas_size);
  settings.grid_w = grid_size.width;
  settings.grid_h = grid_size.height;

  /* Calculate dye & canvas buffer dimensions */
  WGPUExtent3D dye_size = get_preferred_dimensions(
    settings.dye_size, wgpu_context, max_buffer_size, max_canvas_size);
  settings.dye_w = dye_size.width;
  settings.dye_h = dye_size.height;

  /* Useful values for the simulation */
  settings.rdx     = settings.grid_size * 4;
  settings.dye_rdx = settings.dye_size * 4;
  settings.dx      = 1.0f / settings.rdx;
}

/* -------------------------------------------------------------------------- *
 * Render
 * -------------------------------------------------------------------------- */

/* Renders 3 (r, g, b) storage buffers to the canvas */
static struct {
  /* Vertex buffer */
  wgpu_buffer_t vertex_buffer;

  /* Render pipeline */
  WGPURenderPipeline render_pipeline;

  /* Bind groups stores the resources bound to the binding points in a shader */
  WGPUBindGroup render_bind_group;

  /* Render pass descriptor for frame buffer writes */
  struct {
    WGPURenderPassColorAttachment color_attachments[1];
    WGPURenderPassDescriptor descriptor;
  } render_pass;
} render_program = {0};

// Shaders
// clang-format off
static const char* shader_wgsl = CODE(
  struct GridSize {
    w : f32,
    h : f32,
    dyeW: f32,
    dyeH: f32,
    dx : f32,
    rdx : f32,
    dyeRdx : f32
  }

  struct VertexOut {
    @builtin(position) position : vec4<f32>,
    @location(1) uv : vec2<f32>,
  };

  @group(0) @binding(0) var<storage, read_write> fieldX : array<f32>;
  @group(0) @binding(1) var<storage, read_write> fieldY : array<f32>;
  @group(0) @binding(2) var<storage, read_write> fieldZ : array<f32>;
  @group(0) @binding(3) var<uniform> uGrid : GridSize;
  @group(0) @binding(4) var<uniform> multiplier : f32;
  @group(0) @binding(5) var<uniform> isRenderingDye : f32;

  @vertex
  fn vertex_main(@location(0) position: vec4<f32>) -> VertexOut
  {
    var output : VertexOut;
    output.position = position;
    output.uv = position.xy*.5+.5;
    return output;
  }

  @fragment
  fn fragment_main(fragData : VertexOut) -> @location(0) vec4<f32>
  {
    var w = uGrid.dyeW;
    var h = uGrid.dyeH;

    if (isRenderingDye != 1.) {
      w = uGrid.w;
      h = uGrid.h;
    }

    let fuv = vec2<f32>((floor(fragData.uv*vec2(w, h))));
    let id = u32(fuv.x + fuv.y * w);

    let r = fieldX[id];
    let g = fieldY[id];
    let b = fieldZ[id];
    var col = vec3(r, g, b);

    if (r == g && r == b) {
      if (r < 0.) {col = mix(vec3(0.), vec3(0., 0., 1.), abs(r));}
      else {col = mix(vec3(0.), vec3(1., 0., 0.), r);}
    }
    return vec4(col, 1) * multiplier;
  }
);
// clang-format on

static void render_program_prepare_vertex_buffer(wgpu_context_t* wgpu_context)
{
  const float vertices[24] = {
    -1, -1, 0, 1, -1, 1, 0, 1, 1, -1, 0, 1,
    1,  -1, 0, 1, -1, 1, 0, 1, 1, 1,  0, 1,
  };

  render_program.vertex_buffer = wgpu_create_buffer(
    wgpu_context, &(wgpu_buffer_desc_t){
                    .usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_Vertex,
                    .size  = sizeof(vertices),
                    .initial.data = vertices,
                  });
}

static void render_program_destroy()
{
  wgpu_destroy_buffer(&render_program.vertex_buffer);
  WGPU_RELEASE_RESOURCE(RenderPipeline, render_program.render_pipeline)
  WGPU_RELEASE_RESOURCE(BindGroup, render_program.render_bind_group)
}

static void render_program_prepare_pipelines(wgpu_context_t* wgpu_context)
{
  /* Primitive state */
  WGPUPrimitiveState primitive_state = {
    .topology  = WGPUPrimitiveTopology_TriangleList,
    .frontFace = WGPUFrontFace_CCW,
    .cullMode  = WGPUCullMode_None,
  };

  /* Color target state */
  WGPUBlendState blend_state              = wgpu_create_blend_state(false);
  WGPUColorTargetState color_target_state = (WGPUColorTargetState){
    .format    = wgpu_context->swap_chain.format,
    .blend     = &blend_state,
    .writeMask = WGPUColorWriteMask_All,
  };

  /* Vertex buffer layout */
  WGPU_VERTEX_BUFFER_LAYOUT(
    fluid_simulation, 16, WGPU_VERTATTR_DESC(0, WGPUVertexFormat_Float32x4, 0))

  /* Vertex state */
  WGPUVertexState vertex_state = wgpu_create_vertex_state(
                wgpu_context, &(wgpu_vertex_state_t){
                .shader_desc = (wgpu_shader_desc_t){
                  // Vertex shader WGSL
                  .label            = "vertex_shader_wgsl",
                  .wgsl_code.source = shader_wgsl,
                  .entry            = "vertex_main",
                },
                .buffer_count = 1,
                .buffers      = &fluid_simulation_vertex_buffer_layout,
              });

  /* Fragment state */
  WGPUFragmentState fragment_state = wgpu_create_fragment_state(
                wgpu_context, &(wgpu_fragment_state_t){
                .shader_desc = (wgpu_shader_desc_t){
                  // Fragment shader WGSL
                  .label            = "fragment_shader_wgsl",
                  .wgsl_code.source = shader_wgsl,
                  .entry            = "fragment_main",
                },
                .target_count = 1,
                .targets      = &color_target_state,
              });

  // Multisample state
  WGPUMultisampleState multisample_state
    = wgpu_create_multisample_state_descriptor(
      &(create_multisample_state_desc_t){
        .sample_count = 1,
      });

  // Create rendering pipeline using the specified states
  render_program.render_pipeline = wgpuDeviceCreateRenderPipeline(
    wgpu_context->device, &(WGPURenderPipelineDescriptor){
                            .label       = "fluid_simulation_render_pipeline",
                            .primitive   = primitive_state,
                            .vertex      = vertex_state,
                            .fragment    = &fragment_state,
                            .multisample = multisample_state,
                          });
  ASSERT(render_program.render_pipeline != NULL);

  // Partial cleanup
  WGPU_RELEASE_RESOURCE(ShaderModule, vertex_state.module);
  WGPU_RELEASE_RESOURCE(ShaderModule, fragment_state.module);
}

static void render_program_setup_bind_group(wgpu_context_t* wgpu_context)
{
  WGPUBindGroupEntry bg_entries[6] = {
    /* Binding 0 : fieldX */
    [0] = (WGPUBindGroupEntry) {
      .binding = 0,
      .buffer  = dynamic_buffers.rgb_buffer.buffers[0].buffer,
      .size    = dynamic_buffers.rgb_buffer.buffer_size,
    },
    /* Binding 1 : fieldY */
    [1] = (WGPUBindGroupEntry) {
      .binding = 1,
      .buffer  = dynamic_buffers.rgb_buffer.buffers[1].buffer,
      .size    = dynamic_buffers.rgb_buffer.buffer_size,
    },
    /* Binding 2 : fieldZ */
    [2] = (WGPUBindGroupEntry) {
      .binding = 2,
      .buffer  = dynamic_buffers.rgb_buffer.buffers[2].buffer,
      .size    = dynamic_buffers.rgb_buffer.buffer_size,
    },
    /* Binding 3 : uGrid */
    [3] = (WGPUBindGroupEntry) {
      .binding = 3,
      .buffer  = uniforms.grid.buffer.buffer,
      .size    = uniforms.grid.buffer.size,
    },
    /* Binding 4 : multiplier */
    [4] = (WGPUBindGroupEntry) {
      .binding = 4,
      .buffer  = uniforms.u_render_intensity.buffer.buffer,
      .size    = uniforms.u_render_intensity.buffer.size,
    },
    /* Binding 4 : isRenderingDye */
    [5] = (WGPUBindGroupEntry) {
      .binding = 5,
      .buffer  = uniforms.u_render_dye.buffer.buffer,
      .size    = uniforms.u_render_dye.buffer.size,
    },
  };
  WGPUBindGroupDescriptor bg_desc = {
    .label = "render bind group",
    .layout
    = wgpuRenderPipelineGetBindGroupLayout(render_program.render_pipeline, 0),
    .entryCount = (uint32_t)ARRAY_SIZE(bg_entries),
    .entries    = bg_entries,
  };
  render_program.render_bind_group
    = wgpuDeviceCreateBindGroup(wgpu_context->device, &bg_desc);
  ASSERT(render_program.render_bind_group != NULL);
}

/* The r,g,b buffer containing the data to render */
static void render_program_setup_rgb_buffer(wgpu_context_t* wgpu_context)
{
  dynamic_buffer_init(&dynamic_buffers.rgb_buffer, wgpu_context, /* dims: */ 3,
                      /* w: */ settings.dye_w, /* h: */ settings.dye_h);
}

/* Uniforms */
static void render_program_setup_render_uniforms(wgpu_context_t* wgpu_context)
{
  float value = 1;

  uniform_init(&uniforms.u_render_intensity, wgpu_context,
               UNIFORM_RENDER_INTENSITY, 1, &value);
  uniform_init(&uniforms.u_render_dye, wgpu_context, UNIFORM_RENDER_DYE, 1,
               &value);
}

static void render_program_setup_render_pass()
{
  /* Color attachment */
  render_program.render_pass.color_attachments[0] = (WGPURenderPassColorAttachment) {
      .view       = NULL, /* Assigned later */
      .loadOp     = WGPULoadOp_Clear,
      .storeOp    = WGPUStoreOp_Store,
      .clearColor = (WGPUColor) {
        .r = 0.0f,
        .g = 0.0f,
        .b = 0.0f,
        .a = 1.0f,
      },
  };

  /* Render pass descriptor */
  render_program.render_pass.descriptor = (WGPURenderPassDescriptor){
    .colorAttachmentCount   = 1,
    .colorAttachments       = render_program.render_pass.color_attachments,
    .depthStencilAttachment = NULL,
  };
}

/* Dispatch a draw command to render on the canvas */
static void render_program_dispatch(wgpu_context_t* wgpu_context,
                                    WGPUCommandEncoder command_encoder)
{
  render_program.render_pass.color_attachments[0].view
    = wgpu_context->swap_chain.frame_buffer;

  WGPURenderPassEncoder render_pass_encoder = wgpuCommandEncoderBeginRenderPass(
    command_encoder, &render_program.render_pass.descriptor);

  wgpuRenderPassEncoderSetPipeline(render_pass_encoder,
                                   render_program.render_pipeline);
  wgpuRenderPassEncoderSetBindGroup(render_pass_encoder, 0,
                                    render_program.render_bind_group, 0, 0);
  wgpuRenderPassEncoderSetVertexBuffer(render_pass_encoder, 0,
                                       render_program.vertex_buffer.buffer, 0,
                                       WGPU_WHOLE_SIZE);
  wgpuRenderPassEncoderDraw(render_pass_encoder, 6, 1, 0, 0);
  wgpuRenderPassEncoderEnd(render_pass_encoder);
  WGPU_RELEASE_RESOURCE(RenderPassEncoder, render_pass_encoder)
}
