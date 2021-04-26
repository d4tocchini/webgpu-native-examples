#include "example_base.h"
#include "examples.h"

#include <string.h>

/* -------------------------------------------------------------------------- *
 * WebGPU Example - Compute Boids
 *
 * Flocking boids example with gpu compute update pass.
 * Adapted from:
 * https://github.com/austinEng/webgpu-samples/blob/master/src/examples/computeBoids.ts
 *
 * A GPU compute particle simulation that mimics the flocking behavior of birds.
 * A compute shader updates two ping-pong buffers which store particle data. The
 * data is used to draw instanced particles.
 *
 * Ref:
 * https://github.com/gfx-rs/wgpu-rs/tree/master/examples/boids
 * -------------------------------------------------------------------------- */

// Number of boid particles to simulate
static const uint32_t NUM_PARTICLES = 1500;

// Number of single-particle calculations (invocations) in each gpu work group
static const uint32_t PARTICLES_PER_GROUP = 64;

typedef struct sim_params_t {
  float delta_t;        // deltaT
  float rule1_distance; // rule1Distance
  float rule2_distance; // rule2Distance
  float rule3_distance; // rule3Distance
  float rule1_scale;    // rule1Scale
  float rule2_scale;    // rule2Scale
  float rule3_scale;    // rule3Scale
} sim_params_t;

// WebGPU buffers
static WGPUBuffer sim_param_buffer; /* Simulation Parameter Buffer */
static WGPUBuffer particle_buffers[2];
static WGPUBuffer vertices_buffer;

// The pipeline layouts
static WGPUPipelineLayout compute_pipeline_layout;
static WGPUPipelineLayout render_pipeline_layout;

// Pipelines
static WGPUComputePipeline compute_pipeline;
static WGPURenderPipeline render_pipeline;

// Render pass descriptor for frame buffer writes
static WGPURenderPassColorAttachmentDescriptor rp_color_att_descriptors[1];
static WGPURenderPassDescriptor render_pass_desc;

// Bind groups and layouts
static WGPUBindGroup particle_bind_groups[2];
static WGPUBindGroupLayout compute_bind_group_layout;

// Other variables
static const char* example_title = "Compute Boids";
static bool prepared             = false;
static uint32_t work_group_count;

static float rand_float()
{
  return rand() / (float)RAND_MAX; /* [0, 1.0] */
}

// Prepare vertex buffers
static void prepare_vertices(wgpu_context_t* wgpu_context)
{
  // Buffer for the three 2d triangle vertices of each instance
  const float vertex_buffer[6] = {-0.01f, -0.02f, 0.01f, -0.02f, 0.00f, 0.02f};
  uint32_t vertex_buffer_size
    = (uint32_t)(ARRAY_SIZE(vertex_buffer) * sizeof(float));
  vertices_buffer = wgpu_create_buffer_from_data(
    wgpu_context, vertex_buffer, vertex_buffer_size, WGPUBufferUsage_Vertex);
}

static void setup_pipeline_layout(wgpu_context_t* wgpu_context)
{
  /* Compute pipeline layout */
  WGPUBindGroupLayoutEntry bgl_entries[3] = {
    [0] = (WGPUBindGroupLayoutEntry) {
      .binding = 0,
      .visibility = WGPUShaderStage_Compute,
      .buffer = (WGPUBufferBindingLayout) {
        .type = WGPUBufferBindingType_Uniform,
        .minBindingSize = sizeof(sim_params_t),
      },
      .sampler = {0},
    },
    [1] = (WGPUBindGroupLayoutEntry) {
      .binding = 1,
      .visibility = WGPUShaderStage_Compute,
      .buffer = (WGPUBufferBindingLayout) {
        .type = WGPUBufferBindingType_Storage,
        .minBindingSize = NUM_PARTICLES * 16,
      },
      .sampler = {0},
    },
    [2] = (WGPUBindGroupLayoutEntry) {
      .binding = 2,
      .visibility = WGPUShaderStage_Compute,
      .buffer = (WGPUBufferBindingLayout) {
        .type = WGPUBufferBindingType_Storage,
        .minBindingSize = NUM_PARTICLES * 16,
      },
      .sampler = {0},
    },
  };
  WGPUBindGroupLayoutDescriptor bgl_desc = {
    .entryCount = 3,
    .entries    = bgl_entries,
  };
  compute_bind_group_layout
    = wgpuDeviceCreateBindGroupLayout(wgpu_context->device, &bgl_desc);
  ASSERT(compute_bind_group_layout != NULL)

  WGPUPipelineLayoutDescriptor compute_pipeline_layout_desc = {
    .bindGroupLayoutCount = 1,
    .bindGroupLayouts     = &compute_bind_group_layout,
  };
  compute_pipeline_layout = wgpuDeviceCreatePipelineLayout(
    wgpu_context->device, &compute_pipeline_layout_desc);
  ASSERT(compute_pipeline_layout != NULL)

  /* Render pipeline layout (with empty bind group layout) */
  WGPUPipelineLayoutDescriptor render_pipeline_layout_desc = {0};
  render_pipeline_layout = wgpuDeviceCreatePipelineLayout(
    wgpu_context->device, &render_pipeline_layout_desc);
  ASSERT(render_pipeline_layout != NULL)
}

static void setup_render_pass(wgpu_context_t* wgpu_context)
{
  UNUSED_VAR(wgpu_context);

  // Color attachment
  rp_color_att_descriptors[0] = (WGPURenderPassColorAttachmentDescriptor) {
      .attachment = NULL,
      .loadOp = WGPULoadOp_Clear,
      .storeOp = WGPUStoreOp_Store,
      .clearColor = (WGPUColor) {
        .r = 0.0f,
        .g = 0.0f,
        .b = 0.0f,
        .a = 0.0f,
      },
  };

  // Render pass descriptor
  render_pass_desc = (WGPURenderPassDescriptor){
    .colorAttachmentCount = 1,
    .colorAttachments     = rp_color_att_descriptors,
  };
}

static void prepare_uniform_buffers(wgpu_example_context_t* context)
{
  // Buffer for simulation parameters uniform
  sim_params_t sim_param_data = {
    .delta_t        = 0.04f,  // deltaT
    .rule1_distance = 0.1f,   // rule1Distance
    .rule2_distance = 0.025f, // rule2Distance
    .rule3_distance = 0.025f, // rule3Distance
    .rule1_scale    = 0.02f,  // rule1Scale
    .rule2_scale    = 0.05f,  // rule2Scale
    .rule3_scale    = 0.005f, // rule3Scale
  };
  sim_param_buffer = wgpu_create_buffer_from_data(
    context->wgpu_context, &sim_param_data, sizeof(sim_params_t),
    WGPUBufferUsage_Uniform);

  // Buffer for all particles data of type [(posx,posy,velx,vely),...]
  float initial_particle_data[NUM_PARTICLES * 4];
  memset(initial_particle_data, 0.f, sizeof(initial_particle_data));
  srand((unsigned int)time(NULL)); // randomize seed
  for (uint32_t i = 0; i < NUM_PARTICLES; i += 4) {
    const size_t chunk               = i * 4;
    initial_particle_data[chunk + 0] = 2.f * (rand_float() - 0.5);       // posx
    initial_particle_data[chunk + 1] = 2.f * (rand_float() - 0.5);       // posy
    initial_particle_data[chunk + 2] = 2.f * (rand_float() - 0.5) * 0.1; // velx
    initial_particle_data[chunk + 3] = 2.f * (rand_float() - 0.5) * 0.1; // vely
  }

  // Creates two buffers of particle data each of size NUM_PARTICLES the two
  // buffers alternate as dst and src for each frame
  for (uint32_t i = 0; i < 2; ++i) {
    particle_buffers[i] = wgpu_create_buffer_from_data(
      context->wgpu_context, &initial_particle_data,
      sizeof(initial_particle_data),
      WGPUBufferUsage_Vertex | WGPUBufferUsage_Storage);
  }

  // Create two bind groups, one for each buffer as the src where the alternate
  // buffer is used as the dst
  for (uint32_t i = 0; i < 2; ++i) {
    WGPUBindGroupEntry bg_entries[3] = {
      [0] = (WGPUBindGroupEntry) {
        .binding = 0,
        .buffer = sim_param_buffer,
        .offset = 0,
        .size = sizeof(sim_params_t),
      },
      [1] = (WGPUBindGroupEntry) {
        .binding = 1,
        .buffer = particle_buffers[i],
        .offset = 0,
        .size = sizeof(initial_particle_data),
      },
      [2] = (WGPUBindGroupEntry) {
        .binding = 2,
        .buffer = particle_buffers[(i + 1) % 2],
        .offset = 0,
        .size = sizeof(initial_particle_data), // bind to opposite buffer
      },
    };
    WGPUBindGroupDescriptor bg_desc = {
      .layout     = compute_bind_group_layout,
      .entryCount = 3,
      .entries    = bg_entries,
    };
    particle_bind_groups[i]
      = wgpuDeviceCreateBindGroup(context->wgpu_context->device, &bg_desc);
  }

  // Calculates number of work groups from PARTICLES_PER_GROUP constant
  work_group_count
    = (uint32_t)ceilf((float)NUM_PARTICLES / (float)PARTICLES_PER_GROUP);
}

// Create the compute & graphics pipelines
static void prepare_pipelines(wgpu_context_t* wgpu_context)
{
  // Construct the different states making up the pipeline

  // Rasterization state
  WGPURasterizationStateDescriptor rasterization_state
    = wgpu_create_rasterization_state_descriptor(
      &(create_rasterization_state_desc_t){
        .front_face = WGPUFrontFace_CCW,
        .cull_mode  = WGPUCullMode_Back,
      });

  // Color blend state
  WGPUColorStateDescriptor color_state_desc
    = wgpu_create_color_state_descriptor(&(create_color_state_desc_t){
      .format       = wgpu_context->swap_chain.format,
      .enable_blend = true,
    });

  // Vertex state
  WGPUVertexAttributeDescriptor vert_buff_attrs_0[2] = {
    [0] = (WGPUVertexAttributeDescriptor) {
      // Attribute location 0: instance position
      .shaderLocation = 0,
      .offset = 0,
      .format = WGPUVertexFormat_Float32x2,
    },
    [1] = (WGPUVertexAttributeDescriptor) {
      // Attribute location 1: instance velocity
      .shaderLocation = 1,
      .offset = 2 * 4,
      .format = WGPUVertexFormat_Float32x2,
    },
  };
  WGPUVertexAttributeDescriptor vert_buff_attrs_1 = {
    // Attribute location 2: vertex positions
    .shaderLocation = 2,
    .offset         = 0,
    .format         = WGPUVertexFormat_Float32x2,
  };
  WGPUVertexBufferLayoutDescriptor vert_buf_desc[2] = {
    [0] = (WGPUVertexBufferLayoutDescriptor) {
      // instanced particles buffer
      .arrayStride = 4 * 4,
      .stepMode = WGPUInputStepMode_Instance,
      .attributeCount = 2,
      .attributes = vert_buff_attrs_0,
    },
    [1] = (WGPUVertexBufferLayoutDescriptor) {
      // vertex buffer
      .arrayStride = 2 * 4,
      .stepMode = WGPUInputStepMode_Vertex,
      .attributeCount = 1,
      .attributes = &vert_buff_attrs_1,
    },
  };
  WGPUVertexStateDescriptor vert_state_desc = {
    .vertexBufferCount = 2,
    .vertexBuffers     = vert_buf_desc,
  };

  // Shaders
  // Compute shader
  wgpu_shader_t boids_comp_shader = wgpu_shader_create(
    wgpu_context, &(wgpu_shader_desc_t){
                    // Compute shader SPIR-V
                    .file = "shaders/compute_boids/boids.comp.spv",
                  });

  // Vertex shader
  wgpu_shader_t vert_shader = wgpu_shader_create(
    wgpu_context, &(wgpu_shader_desc_t){
                    // Vertex shader SPIR-V
                    .file = "shaders/compute_boids/shader.vert.spv",
                  });
  // Fragment shader
  wgpu_shader_t frag_shader = wgpu_shader_create(
    wgpu_context, &(wgpu_shader_desc_t){
                    // Fragment shader SPIR-V
                    .file = "shaders/compute_boids/shader.frag.spv",
                  });

  // Compute pipeline
  compute_pipeline = wgpuDeviceCreateComputePipeline(
    wgpu_context->device,
    &(WGPUComputePipelineDescriptor){
      .layout       = compute_pipeline_layout,
      .computeStage = boids_comp_shader.programmable_stage_descriptor,
    });

  // Rendering pipeline
  render_pipeline = wgpuDeviceCreateRenderPipeline(
    wgpu_context->device,
    &(WGPURenderPipelineDescriptor){
      .layout = render_pipeline_layout,
      // Vertex shader
      .vertexStage = vert_shader.programmable_stage_descriptor,
      // Fragment shader
      .fragmentStage = &frag_shader.programmable_stage_descriptor,
      // Rasterization state: no culling, no depth bias
      .rasterizationState     = &rasterization_state,
      .primitiveTopology      = WGPUPrimitiveTopology_TriangleList,
      .colorStateCount        = 1,
      .colorStates            = &color_state_desc,
      .vertexState            = &vert_state_desc,
      .sampleCount            = 1,
      .sampleMask             = 0xFFFFFFFF,
      .alphaToCoverageEnabled = false,
    });

  // Partial clean-up
  wgpu_shader_release(&frag_shader);
  wgpu_shader_release(&vert_shader);
  wgpu_shader_release(&boids_comp_shader);
}

static int example_initialize(wgpu_example_context_t* context)
{
  if (context) {
    prepare_vertices(context->wgpu_context);
    setup_pipeline_layout(context->wgpu_context);
    prepare_uniform_buffers(context);
    prepare_pipelines(context->wgpu_context);
    setup_render_pass(context->wgpu_context);
    prepared = true;
    return 0;
  }

  return 1;
}

static WGPUCommandBuffer build_command_buffer(wgpu_example_context_t* context)
{
  wgpu_context_t* wgpu_context = context->wgpu_context;
  rp_color_att_descriptors[0].attachment
    = wgpu_context->swap_chain.frame_buffer;

  // Create command encoder
  wgpu_context->cmd_enc
    = wgpuDeviceCreateCommandEncoder(wgpu_context->device, NULL);

  // Compute pass
  {
    wgpu_context->cpass_enc
      = wgpuCommandEncoderBeginComputePass(wgpu_context->cmd_enc, NULL);
    wgpuComputePassEncoderSetPipeline(wgpu_context->cpass_enc,
                                      compute_pipeline);
    wgpuComputePassEncoderSetBindGroup(
      wgpu_context->cpass_enc, 0,
      particle_bind_groups[context->frame.index % 2], 0, NULL);
    wgpuComputePassEncoderDispatch(wgpu_context->cpass_enc, work_group_count, 1,
                                   1);
    wgpuComputePassEncoderEndPass(wgpu_context->cpass_enc);
    WGPU_RELEASE_RESOURCE(ComputePassEncoder, wgpu_context->cpass_enc)
  }

  // Render pass
  {
    wgpu_context->rpass_enc = wgpuCommandEncoderBeginRenderPass(
      wgpu_context->cmd_enc, &render_pass_desc);
    wgpuRenderPassEncoderSetPipeline(wgpu_context->rpass_enc, render_pipeline);
    // render dst particles
    wgpuRenderPassEncoderSetVertexBuffer(
      wgpu_context->rpass_enc, 0,
      particle_buffers[(context->frame.index + 1) % 2], 0, 0);
    // the three instance-local vertices
    wgpuRenderPassEncoderSetVertexBuffer(wgpu_context->rpass_enc, 1,
                                         vertices_buffer, 0, 0);
    wgpuRenderPassEncoderDraw(wgpu_context->rpass_enc, 3, NUM_PARTICLES, 0, 0);
    wgpuRenderPassEncoderEndPass(wgpu_context->rpass_enc);
    WGPU_RELEASE_RESOURCE(RenderPassEncoder, wgpu_context->rpass_enc)
  }

  // Get command buffer
  WGPUCommandBuffer command_buffer
    = wgpu_get_command_buffer(wgpu_context->cmd_enc);
  ASSERT(command_buffer != NULL)
  WGPU_RELEASE_RESOURCE(CommandEncoder, wgpu_context->cmd_enc)

  return command_buffer;
}

static int example_draw(wgpu_example_context_t* context)
{
  // Prepare frame
  prepare_frame(context);

  // Command buffer to be submitted to the queue
  wgpu_context_t* wgpu_context                   = context->wgpu_context;
  wgpu_context->submit_info.command_buffer_count = 1;
  wgpu_context->submit_info.command_buffers[0] = build_command_buffer(context);

  // Submit to queue
  submit_command_buffers(context);

  // Submit frame
  submit_frame(context);

  return 0;
}

static int example_render(wgpu_example_context_t* context)
{
  if (!prepared) {
    return 1;
  }
  return example_draw(context);
}

// Clean up used resources
static void example_destroy(wgpu_example_context_t* context)
{
  UNUSED_VAR(context);
  WGPU_RELEASE_RESOURCE(BindGroupLayout, compute_bind_group_layout)
  WGPU_RELEASE_RESOURCE(PipelineLayout, render_pipeline_layout)
  WGPU_RELEASE_RESOURCE(PipelineLayout, compute_pipeline_layout)
  WGPU_RELEASE_RESOURCE(BindGroup, particle_bind_groups[0])
  WGPU_RELEASE_RESOURCE(BindGroup, particle_bind_groups[1])
  WGPU_RELEASE_RESOURCE(Buffer, sim_param_buffer)
  WGPU_RELEASE_RESOURCE(Buffer, particle_buffers[0])
  WGPU_RELEASE_RESOURCE(Buffer, particle_buffers[1])
  WGPU_RELEASE_RESOURCE(Buffer, vertices_buffer)
  WGPU_RELEASE_RESOURCE(RenderPipeline, render_pipeline)
  WGPU_RELEASE_RESOURCE(ComputePipeline, compute_pipeline)
}

void example_compute_boids(int argc, char* argv[])
{
  // clang-format off
  example_run(argc, argv, &(refexport_t){
    .example_settings = (wgpu_example_settings_t){
     .title  = example_title,
    },
    .example_initialize_func      = &example_initialize,
    .example_render_func          = &example_render,
    .example_destroy_func         = &example_destroy,
  });
  // clang-format on
}
