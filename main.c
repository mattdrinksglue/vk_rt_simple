#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include "SDL.h"
#include "SDL2/SDL_vulkan.h"

#include "vulkan/vulkan.h"

static uint32_t window_width = 1280;
static uint32_t window_height = 720;

#define FRAME_OVERLAP 2

#include "vk_mem_alloc.h"
#define VK_WRAP_IMPL
#include "vk_wrap.h"

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#define CIMGUI_USE_SDL2
#define CIMGUI_USE_VULKAN
#include "cimgui.h"
#include "cimgui_impl.h"

#include "HandmadeMath.h"

#define CGLTF_IMPLEMENTATION
#include "cgltf/cgltf.h"

#include "vk_rt_mesh.h"

typedef struct {
  float e[4];
  float view[16];
  float proj[16];
  uint32_t frame_no;
} push_constants_t;

typedef struct {
  v3 pos;
  v3 norm;
  float u, v;
} vertex_t;

typedef struct {
  vertex_t *vertices;
  uint32_t vertex_count;
  uint32_t *indices;
  uint32_t index_count;
} mesh_t;

vkrt_geom_data load_gltf_mesh(const char *fp, uint32_t mesh_idx) {
  cgltf_options options = {};
  cgltf_data *data = NULL;
  cgltf_result res = cgltf_parse_file(&options, fp, &data);
  if (res != cgltf_result_success) {
    fprintf(stderr, "Failed to load gltf file %s\n", fp);
    exit(1);
  }
  res = cgltf_load_buffers(&options, data, fp);
  if (res != cgltf_result_success) {
    fprintf(stderr, "Failed to load gltf file %s\n", fp);
    exit(1);
  }
  assert(mesh_idx < data->meshes_count && "Invalid mesh index");

  cgltf_mesh m = data->meshes[mesh_idx];

  size_t index_count = 0;
  size_t vertex_count = 0;
  // iterate through primitives to get sizes
  for (size_t i = 0; i < m.primitives_count; ++i) {
    cgltf_primitive p = m.primitives[i];
    index_count += cgltf_accessor_unpack_indices(p.indices, NULL, 1, 1);
    for (size_t j = 0; j < p.attributes_count; ++j) {
      if (p.attributes[j].type == cgltf_attribute_type_position) {
	vertex_count += p.attributes[j].data->count;
      }
    }
  }

  uint32_t *indices = malloc(sizeof(*indices) * index_count);
  vertex_t *vertices = malloc(sizeof(*vertices) * vertex_count);
  assert(indices);
  assert(vertices);
  size_t vertex_offset = 0;
  size_t index_offset = 0;
  
  for (size_t i = 0; i < m.primitives_count; ++i) {
    cgltf_primitive p = m.primitives[i];
    size_t index_cnt = cgltf_accessor_unpack_indices(p.indices, NULL, 1, 1);
    cgltf_accessor_unpack_indices(p.indices, &indices[index_offset], 4, p.indices->count);
    index_offset += index_cnt;

    size_t vertex_cnt = 0;

    for (size_t j = 0; j < p.attributes_count; ++j) {
      if (p.attributes[j].type == cgltf_attribute_type_position) {
	cgltf_accessor *attr = p.attributes[j].data;
	float *p_vertices = (float *)attr->buffer_view->buffer->data +
	  attr->buffer_view->offset/sizeof(float) +
	  attr->offset/sizeof(float);
	size_t n = 0;
	vertex_cnt = attr->count;
	
	for(size_t k = 0; k < attr->count; ++k) {
	  vertices[vertex_offset + k].pos = (v3) {
	    p_vertices[n], p_vertices[n+1], p_vertices[n+2]
	  };
	  n += attr->stride/sizeof(float);
	}
      } else if (p.attributes[j].type == cgltf_attribute_type_normal) {
	cgltf_accessor *attr = p.attributes[j].data;
	float *p_normals = (float *)attr->buffer_view->buffer->data +
	  attr->buffer_view->offset/sizeof(float) +
	  attr->offset/sizeof(float);
	size_t n = 0;
	for(size_t k = 0; k < attr->count; ++k) {
	  vertices[vertex_offset + k].norm = (v3) {
	    p_normals[n], p_normals[n+1], p_normals[n+2]
	  };
	  n += attr->stride/sizeof(float);
	}
      } else if (p.attributes[j].type == cgltf_attribute_type_texcoord) {
	cgltf_accessor *attr = p.attributes[j].data;
	float *p_coords = (float *)attr->buffer_view->buffer->data +
	  attr->buffer_view->offset/sizeof(float) +
	  attr->offset/sizeof(float);
	size_t n = 0;
	for(size_t k = 0; k < attr->count; ++k) {
	  vertices[vertex_offset + k].u = p_coords[n];
	  vertices[vertex_offset + k].v = p_coords[n+1];
	  n += attr->stride/sizeof(float);
	}
      }
    }
    vertex_offset += vertex_cnt;
  }
  printf("Mesh loaded with: %lu indices, %lu vertices\n", index_count, vertex_count);
  
  vkrt_geom_data mesh = {
    .vertex_count = vertex_count,
    .vertex_data = vertices,
    .sizeof_vertex = sizeof(vertex_t),
    .index_count = index_count,
    .index_data = indices,
    .sizeof_index = sizeof(uint32_t),
    .primitive_count = index_count / 3,
    .transform = (VkTransformMatrixKHR) {
      .matrix = {
	1, 0, 0, 0,
	0, 1, 0, 0,
	0, 0, 1, 0,
      }
    }
  };
  
  return mesh;
}

typedef struct {
  uint64_t vertex_buffer_address;
  uint64_t index_buffer_address;
  uint32_t material_index;
} geometry_node;

vki_swapchain build_swapchain(VkDevice device,
			      VkPhysicalDevice physical_device,
			      VkSurfaceKHR surface) {
  vki_swapchain_builder builder = {
    .physical_device = physical_device,
    .device = device,
    .surface = surface,
    .desired_width = window_width,
    .desired_height = window_height,
  };
  vki_set_desired_format(&builder, (VkSurfaceFormatKHR) {
      .format = VK_FORMAT_B8G8R8A8_UNORM,
      .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
    });

  vki_set_desired_present_mode(&builder, VK_PRESENT_MODE_FIFO_KHR);
  //vki_set_desired_present_mode(&builder, VK_PRESENT_MODE_IMMEDIATE_KHR);  
  vki_add_image_usage_flags(&builder, VK_IMAGE_USAGE_TRANSFER_DST_BIT);

  return vki_swapchain_build(builder);
}

int main(void) {  
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    printf("Failed to initialize SDL");
    exit(1);
  }
  SDL_Vulkan_LoadLibrary(NULL);
  SDL_Window *win = SDL_CreateWindow("window", SDL_WINDOWPOS_UNDEFINED,
				     SDL_WINDOWPOS_UNDEFINED, window_width,
				     window_height,
				     SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);


  uint32_t sdl_extension_count = 0;
  SDL_Vulkan_GetInstanceExtensions(win, &sdl_extension_count, NULL);
  char **sdl_extensions = malloc(sdl_extension_count * sizeof(char*));
  SDL_Vulkan_GetInstanceExtensions(win, &sdl_extension_count,
				   (const char **)sdl_extensions);

  VkInstance instance;
  VkDebugUtilsMessengerEXT debug_messenger;
  
  {
    vki_instance_builder builder = vki_new_instance_builder();
    vki_set_api_version(&builder, VK_API_VERSION_1_3);
    vki_enable_debug_messenger(&builder, NULL);
    vki_enable_extensions(&builder, sdl_extension_count, sdl_extensions);
    vki_enable_extension(&builder, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    vki_enable_validation(&builder);
    vki_instance vki_inst = vki_instance_build(builder);
    free(sdl_extensions);

    instance = vki_inst.instance;
    debug_messenger = vki_inst.messenger;
  }
  
  VkSurfaceKHR surface;
  SDL_Vulkan_CreateSurface(win, instance, &surface);

  VkDevice device;
  VkPhysicalDevice physical_device;

  uint32_t graphics_queue_family;
  VkQueue graphics_queue;
  {
    vki_physical_device pd = vki_physical_device_init(instance,
						      VK_API_VERSION_1_3);
    vki_set_surface(&pd, surface);  
    pd.features13 = (VkPhysicalDeviceVulkan13Features) {
      .dynamicRendering = true, .synchronization2 = true,
      .maintenance4 = true,
    };
    pd.features12 = (VkPhysicalDeviceVulkan12Features) {
      .bufferDeviceAddress = true,
      .descriptorIndexing = true,
      .shaderSampledImageArrayNonUniformIndexing = VK_TRUE,
      .runtimeDescriptorArray = VK_TRUE,
      .descriptorBindingVariableDescriptorCount = VK_TRUE,
    };

    vki_set_features(&pd, (VkPhysicalDeviceFeatures2) {
	.features = (VkPhysicalDeviceFeatures) {
	  .shaderInt64 = VK_TRUE,
	}
      });
  
    vki_physical_device_select(&pd);

    vki_enable_device_extension(&pd, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
    vki_enable_device_extension(&pd, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
    // required by acceleration_structure extension
    vki_enable_device_extension(&pd, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
    vki_enable_device_extension(&pd, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    vki_enable_device_extension(&pd, VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
    
    vki_enable_device_extension(&pd, VK_KHR_SPIRV_1_4_EXTENSION_NAME);
    vki_enable_device_extension(&pd, VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME);

    VkPhysicalDeviceRayTracingPipelineFeaturesKHR pd_rt_pipeline_features = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR,
      .rayTracingPipeline = VK_TRUE,
    };
    VkPhysicalDeviceAccelerationStructureFeaturesKHR pd_as_features = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
      .accelerationStructure = VK_TRUE,
      .pNext = &pd_rt_pipeline_features
    };

    vki_enable_features_pnext(&pd, &pd_as_features);
    
    vki_device vki_device = vki_device_create(pd);

    graphics_queue = vki_device_get_queue(vki_device, VK_QUEUE_GRAPHICS_BIT,
					  &graphics_queue_family);
    
    if (graphics_queue == VK_NULL_HANDLE) {
      fprintf(stderr, "Failed to get graphics queue\n");
      exit(1);
    }
    device = vki_device.device;
    physical_device = vki_device.physical_device;
    vki_device_cleanup(vki_device);
  }

  VmaAllocator allocator;
  {
    VmaAllocatorCreateInfo allocator_info = {
      .physicalDevice = physical_device,
      .device = device,
      .instance = instance,
      .flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
    };
    vmaCreateAllocator(&allocator_info, &allocator);
  }

  vki_swapchain swapchain = build_swapchain(device, physical_device, surface);

  VkExtent2D draw_extent = { window_width, window_height };
  VkExtent3D draw_extent3 = { window_width, window_height, 1 };
  VkImageUsageFlags flags = VK_IMAGE_USAGE_TRANSFER_SRC_BIT     |
                            VK_IMAGE_USAGE_TRANSFER_DST_BIT     |
                            VK_IMAGE_USAGE_STORAGE_BIT          |
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  vkw_image draw_image = vkw_image_create(device, allocator, draw_extent3,
					  VK_FORMAT_R32G32B32A32_SFLOAT,
					  flags, false);
  
  // make a descriptor allocator
  vkw_pool_size_ratio ratios[1] = {
    (vkw_pool_size_ratio) { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}
  };
  vkw_descriptor_allocator ds_alloc =
    vkw_descriptor_allocator_init(device, 10, 1, ratios);

  vkw_immediate_submit_buffer immediate_buf;
  immediate_buf = vkw_immediate_submit_buffer_create(device,
						     graphics_queue_family);
  
  // initialize ImGui
  // first make descriptor pool for imgui
  VkDescriptorPool imgui_pool;
  {
    VkDescriptorPoolSize pool_sizes[] = {
      { VK_DESCRIPTOR_TYPE_SAMPLER, 1000 },
      { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
      { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000 },
      { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000 },
      { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000 },
      { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000 },
      { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 },
      { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000 },
      { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 },
      { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000 },
      { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000 }
    };
    VkDescriptorPoolCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
      .maxSets = 1000,
      .poolSizeCount = sizeof(pool_sizes)/sizeof(pool_sizes[0]),
      .pPoolSizes = pool_sizes,
    };
    VK_CHECK(vkCreateDescriptorPool(device, &info, NULL, &imgui_pool));
  }
  
  igCreateContext(NULL);

  ImGui_ImplSDL2_InitForVulkan(win);
  ImGui_ImplVulkan_InitInfo imgui_init_info = {
    .Instance = instance,
    .PhysicalDevice = physical_device,
    .Device = device,
    .Queue = graphics_queue,
    .DescriptorPool = imgui_pool,
    .ImageCount = 3,
    .MinImageCount = 3,
    .UseDynamicRendering = true,
    .ColorAttachmentFormat = VK_FORMAT_B8G8R8A8_UNORM,
    .MSAASamples = VK_SAMPLE_COUNT_1_BIT,
  };
  ImGui_ImplVulkan_Init(&imgui_init_info, VK_NULL_HANDLE);

  (void)vkw_immediate_begin(device, immediate_buf);
  ImGui_ImplVulkan_CreateFontsTexture();
  vkw_immediate_end(device, immediate_buf, graphics_queue);


  vkrt_get_device_functions(device);
  
  VkPhysicalDeviceRayTracingPipelinePropertiesKHR rt_pipeline_props = {
    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR,
  };
  VkPhysicalDeviceAccelerationStructureFeaturesKHR as_features = {
    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
  };

  VkPhysicalDeviceProperties2 dev_props = {
    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
    .pNext = &rt_pipeline_props,
  };
  vkGetPhysicalDeviceProperties2(physical_device, &dev_props);
    
  VkPhysicalDeviceFeatures2 dev_features = {
    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
    .pNext = &as_features,
  };

  vkGetPhysicalDeviceFeatures2(physical_device, &dev_features);

  //const char *asset_path = "./assets/spheres_cube_material.glb";
  //const char *asset_path = "./assets/sponza/Sponza.gltf";
  //const char *asset_path = "./assets/structure.glb";
  const char *asset_path = "./assets/sponza_glb.glb";
  vkrt_model model = vkrt_load_gltf_model(device, allocator, graphics_queue,
					  immediate_buf, asset_path);
  
  size_t geom_count = 0;
  for (size_t i = 0; i < model.mesh_count; ++i) {
    geom_count += model.meshes[i].primitive_count;
  }

  geometry_node *geom_nodes = calloc(sizeof(geometry_node), geom_count);

  size_t idx = 0;
  for (uint32_t i = 0; i < model.mesh_count; ++i) {
    vkrt_mesh mesh = model.meshes[i];
    for (uint32_t j = 0; j < mesh.primitive_count; ++j) {      
      geom_nodes[idx++] = (geometry_node) {
	mesh.primitives[j].vertex_buffer.device_address,
	mesh.primitives[j].index_buffer.device_address,
	mesh.primitives[j].material_index,
      };
    }
  }

  VkBufferUsageFlagBits usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  
  vkrt_memory geometry_nodes =
    vkrt_allocate_memory(device, allocator, geom_count * sizeof(*geom_nodes),
			 geom_nodes, usage);
      
  printf("Loaded: %lu meshes\n", geom_count);
  fflush(stdout);

  free(geom_nodes);
  
  vkrt_as *blases = calloc(sizeof(vkrt_as), geom_count);

  idx = 0;
  for (uint32_t i = 0; i < model.mesh_count; ++i) {
    vkrt_mesh mesh = model.meshes[i];
    vkrt_memory transform_buffer = mesh.transform_buffer;
    for (uint32_t j = 0; j < mesh.primitive_count; ++j) {
      vkrt_primitive p = mesh.primitives[j];
      blases[idx++] = vkrt_create_blas3(device, allocator, graphics_queue,
      					immediate_buf, p.vertex_buffer,
      					p.index_buffer, p.primitive_count,
					p.vertex_count, transform_buffer);
    }
  }

  // create tlas
  vkrt_as tlas;

  VkTransformMatrixKHR transform = {
    .matrix = {
      1, 0, 0, 0,
      0, 1, 0, 0,
      0, 0, 1, 0,
    },
  };

  tlas = vkrt_create_tlas(device, allocator, graphics_queue, immediate_buf,
			  geom_count, blases, transform);

  // descriptor set layout
  VkDescriptorSetLayout rt_layout;
  VkDescriptorSet rt_set;
  {
    vkw_descriptor_layout_builder b = {};
    vkw_descriptor_layout_builder_add(&b, 0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR);
    vkw_descriptor_layout_builder_add(&b, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    vkw_descriptor_layout_builder_add(&b, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    vkw_descriptor_layout_builder_add(&b, 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    vkw_descriptor_layout_builder_add2(&b, 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				       model.texture_count);
    rt_layout = vkw_descriptor_layout_build(&b, device, VK_SHADER_STAGE_RAYGEN_BIT_KHR
					    | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR);

    rt_set = 
      vkw_descriptor_allocator_alloc(&ds_alloc, device, rt_layout);

    // HACK
    // since we only want to write 5 things, but need auxilliary space for
    // 4 + model.texture_count items, this is the only way to do this with the
    // current, naive api
    // TODO FIXME
    vkrt_ds_writer writer = vkrt_ds_writer_create(4 + model.texture_count, rt_set);
    writer.ds_count = 5;
    vkrt_ds_writer_add_as(&writer, 0, &tlas.as);
    vkrt_ds_writer_add_image(&writer, 1, draw_image.view);
    vkrt_ds_writer_add_buffer(&writer, 2, geometry_nodes.buffer, 0, VK_WHOLE_SIZE);
    vkrt_ds_writer_add_buffer(&writer, 3, model.materials_buffer.buffer, 0, VK_WHOLE_SIZE);
    vkrt_ds_writer_add_sampled_images(&writer, 4, model.texture_count, model.textures);

    vkrt_ds_writer_write(device, writer);

    vkrt_ds_writer_free(&writer);
  }
  // pipeline layout
  VkPipelineLayout rt_pipeline_layout;
  {
    VkPipelineLayoutCreateInfo pipeline_layout_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &rt_layout,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &(VkPushConstantRange) {
	.offset = 0,
	.size = sizeof(push_constants_t),
	.stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR
	| VK_SHADER_STAGE_RAYGEN_BIT_KHR,
      }
    };

    VK_CHECK(vkCreatePipelineLayout(device, &pipeline_layout_info, NULL,
				    &rt_pipeline_layout));
  }
  // pipeline
  VkPipeline rt_pipeline;
  {
    VkShaderModule raygen_sh;
    VkShaderModule closest_hit_sh;
    VkShaderModule miss_sh;
    if (!vkh_load_shader_module("./shaders/ray_gen.spv",     device, &raygen_sh) ||
	!vkh_load_shader_module("./shaders/closest_hit.spv", device, &closest_hit_sh) ||
	!vkh_load_shader_module("./shaders/miss.spv",        device, &miss_sh)) {
      fprintf(stderr, "Failed to load a raytracing shader - please check they exist\n");
      exit(1);
    }
	
    VkPipelineShaderStageCreateInfo rgen_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR,
      .module = raygen_sh,
      .pName = "main",
    };

    VkPipelineShaderStageCreateInfo rchit_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
      .module = closest_hit_sh,
      .pName = "main",
    };

    VkPipelineShaderStageCreateInfo rmiss_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = VK_SHADER_STAGE_MISS_BIT_KHR,
      .module = miss_sh,
      .pName = "main",
    };

    VkPipelineShaderStageCreateInfo stages[3] = {rgen_info, rmiss_info, rchit_info};

    VkRayTracingShaderGroupCreateInfoKHR rgen_group = {
      .sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
      .type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR,
      .generalShader = 0, // index into stages
      .closestHitShader = VK_SHADER_UNUSED_KHR,
      .anyHitShader = VK_SHADER_UNUSED_KHR,
      .intersectionShader = VK_SHADER_UNUSED_KHR,
    };

    VkRayTracingShaderGroupCreateInfoKHR rmiss_group = {
      .sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
      .type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR,
      .generalShader = 1, // index into stages
      .closestHitShader = VK_SHADER_UNUSED_KHR,
      .anyHitShader = VK_SHADER_UNUSED_KHR,
      .intersectionShader = VK_SHADER_UNUSED_KHR,
    };

    VkRayTracingShaderGroupCreateInfoKHR rhit_group = {
      .sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
      .type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR,
      .generalShader = VK_SHADER_UNUSED_KHR,
      .closestHitShader = 2,
      .anyHitShader = VK_SHADER_UNUSED_KHR,
      .intersectionShader = VK_SHADER_UNUSED_KHR,
    };

    VkRayTracingShaderGroupCreateInfoKHR shader_groups[3] = {
      rgen_group, rmiss_group, rhit_group
    };

    VkRayTracingPipelineCreateInfoKHR pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR,
      .stageCount = 3,
      .pStages = stages,
      .groupCount = 3,
      .pGroups = shader_groups,
      .maxPipelineRayRecursionDepth = 2,
      .layout = rt_pipeline_layout,
    };

    VK_CHECK(vkCreateRayTracingPipelinesKHRp(device, NULL, NULL, 1, &pipeline_info,
					    NULL, &rt_pipeline));
    vkDestroyShaderModule(device, raygen_sh, NULL);
    vkDestroyShaderModule(device, miss_sh, NULL);
    vkDestroyShaderModule(device, closest_hit_sh, NULL);
  }
  // shader binding table
  uint64_t sbt_handle_size = rt_pipeline_props.shaderGroupHandleSize;
  uint64_t sbt_handle_alignment = rt_pipeline_props.shaderGroupHandleAlignment;
  uint64_t sbt_handle_size_aligned =
    (sbt_handle_size + sbt_handle_alignment - 1) & ~(sbt_handle_alignment - 1);
  uint64_t sbt_size = 3 * sbt_handle_size_aligned; // 3 from the number of shader groups

  vkrt_memory sbt_rgen_buffer;
  vkrt_memory sbt_rmiss_buffer;
  vkrt_memory sbt_rchit_buffer;

  VkStridedDeviceAddressRegionKHR rgen_sbt;
  VkStridedDeviceAddressRegionKHR rmiss_sbt;
  VkStridedDeviceAddressRegionKHR rchit_sbt;
  VkStridedDeviceAddressRegionKHR rcall_sbt = {};
  {
    uint8_t *sbt_results = calloc(sbt_size, 1);
    VkBufferUsageFlagBits usage = VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR |
      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    VK_CHECK(vkGetRayTracingShaderGroupHandlesKHRp(device, rt_pipeline, 0, 3,
						  sbt_size, sbt_results));

    // NOTE: these are in the same order as our shader groups above!
    sbt_rgen_buffer = vkrt_allocate_memory(device, allocator, sbt_handle_size,
					   sbt_results, usage);
    rgen_sbt.deviceAddress = sbt_rgen_buffer.device_address;
    rgen_sbt.size = sbt_handle_size_aligned;
    rgen_sbt.stride = sbt_handle_size_aligned;

    sbt_rmiss_buffer = vkrt_allocate_memory(device, allocator, sbt_handle_size,
					   sbt_results + sbt_handle_size_aligned, usage);
    rmiss_sbt.deviceAddress = sbt_rmiss_buffer.device_address;
    rmiss_sbt.size = sbt_handle_size_aligned;
    rmiss_sbt.stride = sbt_handle_size_aligned;

    sbt_rchit_buffer = vkrt_allocate_memory(device, allocator, sbt_handle_size,
					   sbt_results + sbt_handle_size_aligned * 2, usage);
    rchit_sbt.deviceAddress = sbt_rchit_buffer.device_address;
    rchit_sbt.size = sbt_handle_size_aligned;
    rchit_sbt.stride = sbt_handle_size_aligned;
  }

  vkw_frame_data frames[FRAME_OVERLAP];
  {
    for (uint32_t i = 0; i < FRAME_OVERLAP; ++i) {
      frames[i] = vkw_frame_data_create(device, graphics_queue_family);
    }
  }

  push_constants_t push_constants = {
    .e = {20, 20, 10, 0},
  };

  HMM_Vec3 camera_pos = {0, 2, 5};
  HMM_Mat4 view, proj, inv_proj;
  float theta = 0, phi = 0;
  /*
  HMM_Mat4 view = HMM_Translate((HMM_Vec3){0, 2, 3});
  //view = HMM_MulM4(view, HMM_Rotate_RH(M_PI, (HMM_Vec3){0, 1, 0}));
  HMM_Mat4 proj = HMM_Perspective_RH_ZO(90.f * M_PI / 180.f,
					(float)draw_extent.width/draw_extent.height,
					100.f, 0.1f);
  
  HMM_Mat4 inv_proj = HMM_InvPerspective_RH(proj);
  view.Elements[1][1] *= -1;

  memcpy(&push_constants.view, &view, 16*sizeof(float));
  memcpy(&push_constants.proj, &inv_proj, 16*sizeof(float));
  */  
  bool done = false;
  uint32_t frame_number = 0;

  uint32_t timeout = 1000000000;

  bool swapchain_resize = false;
  bool reset_accumulation = false;
  
  uint32_t ticks_frame = 0, ticks_prev = 0;
  for (;!done; frame_number++) {
    if (reset_accumulation) {
      frame_number = 0;
      reset_accumulation = false;
    }
    ticks_prev = ticks_frame;
    ticks_frame = SDL_GetTicks();

    if (swapchain_resize) {
      vkDeviceWaitIdle(device);
      vki_destroy_swapchain(device, swapchain);
      swapchain = build_swapchain(device, physical_device, surface);
      swapchain_resize = false;
    }
    
    SDL_Event e;
    while(SDL_PollEvent(&e)) {
      ImGui_ImplSDL2_ProcessEvent(&e);
      switch (e.type) {
      case SDL_QUIT: {done = true;} break;
      case SDL_WINDOWEVENT: {
	if (e.window.event == SDL_WINDOWEVENT_RESIZED) {
	  SDL_GetWindowSize(win, &window_width, &window_height);
	}
      } break;
      }
    } 
    
    vkw_frame_data curr = frames[frame_number % 2];
    
    // imgui
    {
      ImGui_ImplVulkan_NewFrame();
      ImGui_ImplSDL2_NewFrame();
      igNewFrame();
      if (igBegin("background", NULL, 0)) {
	igText("Frame time: %d", ticks_frame - ticks_prev);
	igText("Average frame time: %f", (float)ticks_frame/frame_number);
	igInputFloat4("color", push_constants.e, NULL, 0);
	igInputFloat3("pos", camera_pos.Elements, NULL, 0);
	igInputFloat("theta", &theta, 0.01, 0.1, NULL, 0);
	igInputFloat("phi", &phi, 0.01, 0.1, NULL, 0);
	igCheckbox("reset", &reset_accumulation);
	igEnd();
      }
      igRender();
    }

    // update camera matrices
    view = HMM_Translate(camera_pos);
    view = HMM_MulM4(view, HMM_Rotate_RH(theta, (HMM_Vec3){0, 1, 0}));
    view = HMM_MulM4(view, HMM_Rotate_RH(phi, (HMM_Vec3){1, 0, 0}));    
    proj = HMM_Perspective_RH_ZO(90.f * M_PI / 180.f,
					  (float)draw_extent.width/draw_extent.height,
					  100.f, 0.1f);
  
    inv_proj = HMM_InvPerspective_RH(proj);
    view.Elements[1][1] *= -1;

    memcpy(&push_constants.view, &view, 16*sizeof(float));
    memcpy(&push_constants.proj, &inv_proj, 16*sizeof(float));


    vkw_frame_cmd_begin(device, curr, timeout);
    //printf("Starting frame: %u\n", frame_number);
    //fflush(stdout);
    uint32_t image_index;
    VkResult res = vkAcquireNextImageKHR(device, swapchain.swapchain, timeout,
				   curr.swap_sem, NULL, &image_index);
    if (res == VK_ERROR_OUT_OF_DATE_KHR) {
      swapchain_resize = true;
      continue;
    }
    
    VkCommandBuffer cmd = curr.buf;
    vkh_transition_image(cmd, draw_image.image, VK_IMAGE_LAYOUT_UNDEFINED,
			 VK_IMAGE_LAYOUT_GENERAL);

    // raytracing
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rt_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
			    rt_pipeline_layout, 0, 1, &rt_set, 0, 0);

    push_constants.frame_no = frame_number;
    vkCmdPushConstants(cmd, rt_pipeline_layout, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
		       VK_SHADER_STAGE_RAYGEN_BIT_KHR, 0, sizeof(push_constants_t),
		       &push_constants);

    vkCmdTraceRaysKHRp(cmd, &rgen_sbt, &rmiss_sbt, &rchit_sbt, &rcall_sbt,
		      draw_image.extent.width, draw_image.extent.height, 1);

    vkh_transition_image(cmd, draw_image.image, VK_IMAGE_LAYOUT_GENERAL,
			 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vkh_transition_image(cmd, swapchain.images[image_index],
			 VK_IMAGE_LAYOUT_UNDEFINED,
			 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkh_copy_image_to_image(cmd, draw_image.image,
			    swapchain.images[image_index], draw_extent,
			    swapchain.extent);
    vkh_transition_image(cmd, swapchain.images[image_index],
			 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        
    // imgui
    {
      VkRenderingAttachmentInfo attinf = {
	.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
	.imageView = swapchain.image_views[image_index],
	.imageLayout = VK_IMAGE_LAYOUT_GENERAL,
	.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
	.storeOp = VK_ATTACHMENT_STORE_OP_STORE
      };
      VkRenderingInfo reninf = {
	.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
	.renderArea.extent = swapchain.extent,
	.layerCount = 1,
	.colorAttachmentCount = 1,
	.pColorAttachments = &attinf,
      };
      vkCmdBeginRendering(cmd, &reninf);

      ImGui_ImplVulkan_RenderDrawData(igGetDrawData(), cmd,
				      VK_NULL_HANDLE);

      vkCmdEndRendering(cmd);
    }

    vkh_transition_image(cmd, swapchain.images[image_index],
			 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			 VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    
    vkw_frame_end_and_submit(device, graphics_queue, curr);

    VkPresentInfoKHR present_info = {
      .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
      .pSwapchains = &swapchain.swapchain,
      .swapchainCount = 1,
      .pWaitSemaphores = &curr.render_sem,
      .waitSemaphoreCount = 1,
      .pImageIndices = &image_index,
    };
    
    res = vkQueuePresentKHR(graphics_queue, &present_info);
    if (res == VK_ERROR_OUT_OF_DATE_KHR) {
      swapchain_resize = true;
    }
  }

  vkDeviceWaitIdle(device);

  vkrt_destroy_as(device, allocator, tlas);
  for(uint32_t i = 0; i < geom_count; ++i) {
    vkrt_destroy_as(device, allocator, blases[i]);
  }
  free(blases);
  vkrt_memory_free(allocator, sbt_rgen_buffer);
  vkrt_memory_free(allocator, sbt_rmiss_buffer);
  vkrt_memory_free(allocator, sbt_rchit_buffer);
  vkrt_memory_free(allocator, geometry_nodes);

  vkrt_free_model(device, allocator, model);
    
  vkDestroyPipeline(device, rt_pipeline, NULL);
  vkDestroyPipelineLayout(device, rt_pipeline_layout, NULL);

  vkDestroyDescriptorSetLayout(device, rt_layout, NULL);
  
  for (uint32_t i = 0; i < FRAME_OVERLAP; ++i) {
    vkw_frame_data_destroy(device, frames[i]);
  }

  ImGui_ImplVulkan_DestroyFontsTexture();
  vkDestroyDescriptorPool(device, imgui_pool, NULL);
  ImGui_ImplVulkan_Shutdown();

  vkw_immediate_submit_buffer_destroy(device, immediate_buf);
  
  vkw_image_destroy(device, allocator, draw_image);

  vkw_descriptor_allocator_destroy(device, &ds_alloc);

  vmaDestroyAllocator(allocator);
  
  vki_destroy_swapchain(device, swapchain);
  
  vkDestroySurfaceKHR(instance, surface, NULL);  
  vkDestroyDevice(device, NULL);
  
  vkDestroyDebugUtilsMessengerEXT(instance, debug_messenger, NULL);
  vkDestroyInstance(instance, NULL);
  
  SDL_DestroyWindow(win);
  SDL_Vulkan_UnloadLibrary();

  SDL_Quit();
  
  return 0;
}
