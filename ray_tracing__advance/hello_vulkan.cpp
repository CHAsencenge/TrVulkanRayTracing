/*
 * Copyright (c) 2014-2021, NVIDIA CORPORATION.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-FileCopyrightText: Copyright (c) 2014-2021 NVIDIA CORPORATION
 * SPDX-License-Identifier: Apache-2.0
 */


#include <sstream>


#define VMA_IMPLEMENTATION

#define STB_IMAGE_IMPLEMENTATION
#include "obj_loader.h"
#include "stb_image.h"


#include "hello_vulkan.h"
#include "nvh/cameramanipulator.hpp"
#include "nvvk/descriptorsets_vk.hpp"
#include "nvvk/images_vk.hpp"
#include "nvvk/pipeline_vk.hpp"

#include "nvh/fileoperations.hpp"
#include "nvvk/commands_vk.hpp"
#include "nvvk/renderpasses_vk.hpp"


extern std::vector<std::string> defaultSearchPaths;


// Holding the camera matrices
struct CameraMatrices
{
  nvmath::mat4f view;
  nvmath::mat4f proj;
  nvmath::mat4f viewInverse;
  // #VKRay
  nvmath::mat4f projInverse;
};


//--------------------------------------------------------------------------------------------------
// Keep the handle on the device
// Initialize the tool to do all our allocations: buffers, images
//
void HelloVulkan::setup(const VkInstance& instance, const VkDevice& device, const VkPhysicalDevice& physicalDevice, uint32_t queueFamily)
{
  AppBaseVk::setup(instance, device, physicalDevice, queueFamily);
  m_alloc.init(instance, device, physicalDevice);
  m_debug.setup(m_device);


  m_offscreen.setup(device, physicalDevice, &m_alloc, queueFamily);
  m_raytrace.setup(device, physicalDevice, &m_alloc, queueFamily);
}

//--------------------------------------------------------------------------------------------------
// Called at each frame to update the camera matrix
//
void HelloVulkan::updateUniformBuffer(const VkCommandBuffer& cmdBuf)
{
  // Prepare new UBO contents on host.
  const float    aspectRatio = m_size.width / static_cast<float>(m_size.height);
  CameraMatrices hostUBO     = {};
  hostUBO.view               = CameraManip.getMatrix();
  hostUBO.proj               = nvmath::perspectiveVK(CameraManip.getFov(), aspectRatio, 0.1f, 1000.0f);
  // hostUBO.proj[1][1] *= -1;  // Inverting Y for Vulkan (not needed with perspectiveVK).
  hostUBO.viewInverse = nvmath::invert(hostUBO.view);
  // #VKRay
  hostUBO.projInverse = nvmath::invert(hostUBO.proj);

  // UBO on the device, and what stages access it.
  VkBuffer deviceUBO      = m_cameraMat.buffer;
  auto     uboUsageStages = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;

  // Ensure that the modified UBO is not visible to previous frames.
  VkBufferMemoryBarrier beforeBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  beforeBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
  beforeBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  beforeBarrier.buffer        = deviceUBO;
  beforeBarrier.offset        = 0;
  beforeBarrier.size          = sizeof(hostUBO);
  vkCmdPipelineBarrier(cmdBuf, uboUsageStages, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_DEPENDENCY_DEVICE_GROUP_BIT, 0,
                       nullptr, 1, &beforeBarrier, 0, nullptr);


  // Schedule the host-to-device upload. (hostUBO is copied into the cmd
  // buffer so it is okay to deallocate when the function returns).
  vkCmdUpdateBuffer(cmdBuf, m_cameraMat.buffer, 0, sizeof(CameraMatrices), &hostUBO);

  // Making sure the updated UBO will be visible.
  VkBufferMemoryBarrier afterBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  afterBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  afterBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  afterBarrier.buffer        = deviceUBO;
  afterBarrier.offset        = 0;
  afterBarrier.size          = sizeof(hostUBO);
  vkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_TRANSFER_BIT, uboUsageStages, VK_DEPENDENCY_DEVICE_GROUP_BIT, 0,
                       nullptr, 1, &afterBarrier, 0, nullptr);
}

//--------------------------------------------------------------------------------------------------
// Describing the layout pushed when rendering
//
void HelloVulkan::createDescriptorSetLayout()
{
  auto nbTxt = static_cast<uint32_t>(m_textures.size());
  auto nbObj = static_cast<uint32_t>(m_objModel.size());

  // Camera matrices (binding = 0)
  m_descSetLayoutBind.addBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR);
  // Materials (binding = 1)
  m_descSetLayoutBind.addBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nbObj + 1,
                                 VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
                                     | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR);
  // Scene description (binding = 2)
  m_descSetLayoutBind.addBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                 VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
                                     | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR);
  // Textures (binding = 3)
  m_descSetLayoutBind.addBinding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nbTxt,
                                 VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR);
  // Materials (binding = 4)
  m_descSetLayoutBind.addBinding(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nbObj,
                                 VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR);
  // Storing vertices (binding = 5)
  m_descSetLayoutBind.addBinding(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nbObj, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR);
  // Storing indices (binding = 6)
  m_descSetLayoutBind.addBinding(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nbObj, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR);
  // Storing implicit obj (binding = 7)
  m_descSetLayoutBind.addBinding(7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                 VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_INTERSECTION_BIT_KHR
                                     | VK_SHADER_STAGE_ANY_HIT_BIT_KHR);


  m_descSetLayout = m_descSetLayoutBind.createLayout(m_device);
  m_descPool      = m_descSetLayoutBind.createPool(m_device, 1);
  m_descSet       = nvvk::allocateDescriptorSet(m_device, m_descPool, m_descSetLayout);
}

//--------------------------------------------------------------------------------------------------
// Setting up the buffers in the descriptor set
//
void HelloVulkan::updateDescriptorSet()
{
  std::vector<VkWriteDescriptorSet> writes;

  // Camera matrices and scene description
  VkDescriptorBufferInfo dbiUnif{m_cameraMat.buffer, 0, VK_WHOLE_SIZE};
  writes.emplace_back(m_descSetLayoutBind.makeWrite(m_descSet, 0, &dbiUnif));
  VkDescriptorBufferInfo dbiSceneDesc{m_sceneDesc.buffer, 0, VK_WHOLE_SIZE};
  writes.emplace_back(m_descSetLayoutBind.makeWrite(m_descSet, 2, &dbiSceneDesc));

  // All material buffers, 1 buffer per OBJ
  std::vector<VkDescriptorBufferInfo> dbiMat;
  std::vector<VkDescriptorBufferInfo> dbiMatIdx;
  std::vector<VkDescriptorBufferInfo> dbiVert;
  std::vector<VkDescriptorBufferInfo> dbiIdx;
  for(auto& m : m_objModel)
  {
    dbiMat.push_back({m.matColorBuffer.buffer, 0, VK_WHOLE_SIZE});
    dbiMatIdx.push_back({m.matIndexBuffer.buffer, 0, VK_WHOLE_SIZE});
    dbiVert.push_back({m.vertexBuffer.buffer, 0, VK_WHOLE_SIZE});
    dbiIdx.push_back({m.indexBuffer.buffer, 0, VK_WHOLE_SIZE});
  }
  dbiMat.push_back({m_implObjects.implMatBuf.buffer, 0, VK_WHOLE_SIZE});  // Adding implicit mat
  writes.emplace_back(m_descSetLayoutBind.makeWriteArray(m_descSet, 1, dbiMat.data()));
  writes.emplace_back(m_descSetLayoutBind.makeWriteArray(m_descSet, 4, dbiMatIdx.data()));
  writes.emplace_back(m_descSetLayoutBind.makeWriteArray(m_descSet, 5, dbiVert.data()));
  writes.emplace_back(m_descSetLayoutBind.makeWriteArray(m_descSet, 6, dbiIdx.data()));

  // All texture samplers
  std::vector<VkDescriptorImageInfo> diit;
  for(auto& texture : m_textures)
  {
    diit.emplace_back(texture.descriptor);
  }
  writes.emplace_back(m_descSetLayoutBind.makeWriteArray(m_descSet, 3, diit.data()));

  VkDescriptorBufferInfo dbiImplDesc{m_implObjects.implBuf.buffer, 0, VK_WHOLE_SIZE};
  writes.emplace_back(m_descSetLayoutBind.makeWrite(m_descSet, 7, &dbiImplDesc));

  // Writing the information
  vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}


//--------------------------------------------------------------------------------------------------
// Creating the pipeline layout
//
void HelloVulkan::createGraphicsPipeline()
{
  VkPushConstantRange pushConstantRanges = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ObjPushConstants)};

  // Creating the Pipeline Layout
  VkPipelineLayoutCreateInfo createInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  createInfo.setLayoutCount         = 1;
  createInfo.pSetLayouts            = &m_descSetLayout;
  createInfo.pushConstantRangeCount = 1;
  createInfo.pPushConstantRanges    = &pushConstantRanges;
  vkCreatePipelineLayout(m_device, &createInfo, nullptr, &m_pipelineLayout);


  // Creating the Pipeline
  std::vector<std::string>                paths = defaultSearchPaths;
  nvvk::GraphicsPipelineGeneratorCombined gpb(m_device, m_pipelineLayout, m_offscreen.renderPass());
  gpb.depthStencilState.depthTestEnable = true;
  gpb.addShader(nvh::loadFile("spv/vert_shader.vert.spv", true, paths, true), VK_SHADER_STAGE_VERTEX_BIT);
  gpb.addShader(nvh::loadFile("spv/frag_shader.frag.spv", true, paths, true), VK_SHADER_STAGE_FRAGMENT_BIT);
  gpb.addBindingDescription({0, sizeof(VertexObj)});
  gpb.addAttributeDescriptions({
      {0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(VertexObj, pos))},
      {1, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(VertexObj, nrm))},
      {2, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(VertexObj, color))},
      {3, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(VertexObj, texCoord))},
  });

  m_graphicsPipeline = gpb.createPipeline();
  m_debug.setObjectName(m_graphicsPipeline, "Graphics");
}

//--------------------------------------------------------------------------------------------------
// Loading the OBJ file and setting up all buffers
//
void HelloVulkan::loadModel(const std::string& filename, nvmath::mat4f transform)
{
  LOGI("Loading File:  %s \n", filename.c_str());
  ObjLoader loader;
  loader.loadModel(filename);

  // Converting from Srgb to linear
  for(auto& m : loader.m_materials)
  {
    m.ambient  = nvmath::pow(m.ambient, 2.2f);
    m.diffuse  = nvmath::pow(m.diffuse, 2.2f);
    m.specular = nvmath::pow(m.specular, 2.2f);
  }

  ObjInstance instance;
  instance.objIndex    = static_cast<uint32_t>(m_objModel.size());
  instance.transform   = transform;
  instance.transformIT = nvmath::transpose(nvmath::invert(transform));
  instance.txtOffset   = static_cast<uint32_t>(m_textures.size());

  ObjModel model;
  model.nbIndices  = static_cast<uint32_t>(loader.m_indices.size());
  model.nbVertices = static_cast<uint32_t>(loader.m_vertices.size());

  // Create the buffers on Device and copy vertices, indices and materials
  nvvk::CommandPool  cmdBufGet(m_device, m_graphicsQueueIndex);
  VkCommandBuffer    cmdBuf  = cmdBufGet.createCommandBuffer();
  VkBufferUsageFlags rtUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
                               | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
  model.vertexBuffer   = m_alloc.createBuffer(cmdBuf, loader.m_vertices, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | rtUsage);
  model.indexBuffer    = m_alloc.createBuffer(cmdBuf, loader.m_indices, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | rtUsage);
  model.matColorBuffer = m_alloc.createBuffer(cmdBuf, loader.m_materials, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  model.matIndexBuffer = m_alloc.createBuffer(cmdBuf, loader.m_matIndx, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  // Creates all textures found
  createTextureImages(cmdBuf, loader.m_textures);
  cmdBufGet.submitAndWait(cmdBuf);
  m_alloc.finalizeAndReleaseStaging();

  std::string objNb = std::to_string(instance.objIndex);
  m_debug.setObjectName(model.vertexBuffer.buffer, (std::string("vertex_" + objNb).c_str()));
  m_debug.setObjectName(model.indexBuffer.buffer, (std::string("index_" + objNb).c_str()));
  m_debug.setObjectName(model.matColorBuffer.buffer, (std::string("mat_" + objNb).c_str()));
  m_debug.setObjectName(model.matIndexBuffer.buffer, (std::string("matIdx_" + objNb).c_str()));

  m_objModel.emplace_back(model);
  m_objInstance.emplace_back(instance);
}


//--------------------------------------------------------------------------------------------------
// Creating the uniform buffer holding the camera matrices
// - Buffer is host visible
//
void HelloVulkan::createUniformBuffer()
{
  m_cameraMat = m_alloc.createBuffer(sizeof(CameraMatrices), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  m_debug.setObjectName(m_cameraMat.buffer, "cameraMat");
}

//--------------------------------------------------------------------------------------------------
// Create a storage buffer containing the description of the scene elements
// - Which geometry is used by which instance
// - Transformation
// - Offset for texture
//
void HelloVulkan::createSceneDescriptionBuffer()
{
  nvvk::CommandPool cmdGen(m_device, m_graphicsQueueIndex);

  auto cmdBuf = cmdGen.createCommandBuffer();
  m_sceneDesc = m_alloc.createBuffer(cmdBuf, m_objInstance, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  cmdGen.submitAndWait(cmdBuf);
  m_alloc.finalizeAndReleaseStaging();
  m_debug.setObjectName(m_sceneDesc.buffer, "sceneDesc");
}

//--------------------------------------------------------------------------------------------------
// Creating all textures and samplers
//
void HelloVulkan::createTextureImages(const VkCommandBuffer& cmdBuf, const std::vector<std::string>& textures)
{
  VkSamplerCreateInfo samplerCreateInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  samplerCreateInfo.minFilter  = VK_FILTER_LINEAR;
  samplerCreateInfo.magFilter  = VK_FILTER_LINEAR;
  samplerCreateInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  samplerCreateInfo.maxLod     = FLT_MAX;

  VkFormat format = VK_FORMAT_R8G8B8A8_SRGB;

  // If no textures are present, create a dummy one to accommodate the pipeline layout
  if(textures.empty() && m_textures.empty())
  {
    nvvk::Texture texture;

    std::array<uint8_t, 4> color{255u, 255u, 255u, 255u};
    VkDeviceSize           bufferSize      = sizeof(color);
    auto                   imgSize         = VkExtent2D{1, 1};
    auto                   imageCreateInfo = nvvk::makeImage2DCreateInfo(imgSize, format);

    // Creating the dummy texture
    nvvk::Image           image  = m_alloc.createImage(cmdBuf, bufferSize, color.data(), imageCreateInfo);
    VkImageViewCreateInfo ivInfo = nvvk::makeImageViewCreateInfo(image.image, imageCreateInfo);
    texture                      = m_alloc.createTexture(image, ivInfo, samplerCreateInfo);

    // The image format must be in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    nvvk::cmdBarrierImageLayout(cmdBuf, texture.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    m_textures.push_back(texture);
  }
  else
  {
    // Uploading all images
    for(const auto& texture : textures)
    {
      std::stringstream o;
      int               texWidth, texHeight, texChannels;
      o << "media/textures/" << texture;
      std::string txtFile = nvh::findFile(o.str(), defaultSearchPaths, true);

      stbi_uc* stbi_pixels = stbi_load(txtFile.c_str(), &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);

      std::array<stbi_uc, 4> color{255u, 0u, 255u, 255u};

      stbi_uc* pixels = stbi_pixels;
      // Handle failure
      if(!stbi_pixels)
      {
        texWidth = texHeight = 1;
        texChannels          = 4;
        pixels               = reinterpret_cast<stbi_uc*>(color.data());
      }

      VkDeviceSize bufferSize      = static_cast<uint64_t>(texWidth) * texHeight * sizeof(uint8_t) * 4;
      auto         imgSize         = VkExtent2D{(uint32_t)texWidth, (uint32_t)texHeight};
      auto         imageCreateInfo = nvvk::makeImage2DCreateInfo(imgSize, format, VK_IMAGE_USAGE_SAMPLED_BIT, true);

      {
        nvvk::Image image = m_alloc.createImage(cmdBuf, bufferSize, pixels, imageCreateInfo);
        nvvk::cmdGenerateMipmaps(cmdBuf, image.image, format, imgSize, imageCreateInfo.mipLevels);
        VkImageViewCreateInfo ivInfo  = nvvk::makeImageViewCreateInfo(image.image, imageCreateInfo);
        nvvk::Texture         texture = m_alloc.createTexture(image, ivInfo, samplerCreateInfo);

        m_textures.push_back(texture);
      }

      stbi_image_free(stbi_pixels);
    }
  }
}

//--------------------------------------------------------------------------------------------------
// Destroying all allocations
//
void HelloVulkan::destroyResources()
{
  vkDestroyPipeline(m_device, m_graphicsPipeline, nullptr);
  vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
  vkDestroyDescriptorPool(m_device, m_descPool, nullptr);
  vkDestroyDescriptorSetLayout(m_device, m_descSetLayout, nullptr);

  m_alloc.destroy(m_cameraMat);
  m_alloc.destroy(m_sceneDesc);
  m_alloc.destroy(m_implObjects.implBuf);
  m_alloc.destroy(m_implObjects.implMatBuf);

  for(auto& m : m_objModel)
  {
    m_alloc.destroy(m.vertexBuffer);
    m_alloc.destroy(m.indexBuffer);
    m_alloc.destroy(m.matColorBuffer);
    m_alloc.destroy(m.matIndexBuffer);
  }

  for(auto& t : m_textures)
  {
    m_alloc.destroy(t);
  }

  //#Post
  m_offscreen.destroy();

  // #VKRay
  m_raytrace.destroy();

  m_alloc.deinit();
}

//--------------------------------------------------------------------------------------------------
// Drawing the scene in raster mode
//
void HelloVulkan::rasterize(const VkCommandBuffer& cmdBuf)
{
  VkDeviceSize offset{0};

  m_debug.beginLabel(cmdBuf, "Rasterize");

  // Dynamic Viewport
  setViewport(cmdBuf);

  // Drawing all triangles
  vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_graphicsPipeline);
  vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &m_descSet, 0, nullptr);


  for(int i = 0; i < m_objInstance.size(); ++i)
  {
    auto& inst                 = m_objInstance[i];
    auto& model                = m_objModel[inst.objIndex];
    m_pushConstants.instanceId = i;  // Telling which instance is drawn

    vkCmdPushConstants(cmdBuf, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(ObjPushConstants), &m_pushConstants);
    vkCmdBindVertexBuffers(cmdBuf, 0, 1, &model.vertexBuffer.buffer, &offset);
    vkCmdBindIndexBuffer(cmdBuf, model.indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmdBuf, model.nbIndices, 1, 0, 0, 0);
  }
  m_debug.endLabel(cmdBuf);
}

//--------------------------------------------------------------------------------------------------
// Handling resize of the window
//
void HelloVulkan::onResize(int /*w*/, int /*h*/)
{
  m_offscreen.createFramebuffer(m_size);
  m_offscreen.updateDescriptorSet();
  m_raytrace.updateRtDescriptorSet(m_offscreen.colorTexture().descriptor.imageView);
  resetFrame();
}

//--------------------------------------------------------------------------------------------------
// Initialize offscreen rendering
//
void HelloVulkan::initOffscreen()
{
  m_offscreen.createFramebuffer(m_size);
  m_offscreen.createDescriptor();
  m_offscreen.createPipeline(m_renderPass);
  m_offscreen.updateDescriptorSet();
}

//--------------------------------------------------------------------------------------------------
// Initialize Vulkan ray tracing
//
void HelloVulkan::initRayTracing()
{
  m_raytrace.createBottomLevelAS(m_objModel, m_implObjects);
  m_raytrace.createTopLevelAS(m_objInstance, m_implObjects);
  m_raytrace.createRtDescriptorSet(m_offscreen.colorTexture().descriptor.imageView);
  m_raytrace.createRtPipeline(m_descSetLayout);
}

//--------------------------------------------------------------------------------------------------
// Ray trace the scene
//
void HelloVulkan::raytrace(const VkCommandBuffer& cmdBuf, const nvmath::vec4f& clearColor)
{
  updateFrame();
  if(m_pushConstants.frame >= m_maxFrames)
    return;

  m_raytrace.raytrace(cmdBuf, clearColor, m_descSet, m_size, m_pushConstants);
}

//--------------------------------------------------------------------------------------------------
// If the camera matrix has changed, resets the frame.
// otherwise, increments frame.
//
void HelloVulkan::updateFrame()
{
  static nvmath::mat4f refCamMatrix;
  static float         refFov{CameraManip.getFov()};

  const auto& m   = CameraManip.getMatrix();
  const auto  fov = CameraManip.getFov();

  if(memcmp(&refCamMatrix.a00, &m.a00, sizeof(nvmath::mat4f)) != 0 || refFov != fov)
  {
    resetFrame();
    refCamMatrix = m;
    refFov       = fov;
  }
  m_pushConstants.frame++;
}

void HelloVulkan::resetFrame()
{
  m_pushConstants.frame = -1;
}


void HelloVulkan::addImplSphere(nvmath::vec3f center, float radius, int matId)
{
  ObjImplicit impl;
  impl.minimum = center - radius;
  impl.maximum = center + radius;
  impl.objType = EObjType::eSphere;
  impl.matId   = matId;
  m_implObjects.objImpl.push_back(impl);
}

void HelloVulkan::addImplCube(nvmath::vec3f minumum, nvmath::vec3f maximum, int matId)
{
  ObjImplicit impl;
  impl.minimum = minumum;
  impl.maximum = maximum;
  impl.objType = EObjType::eCube;
  impl.matId   = matId;
  m_implObjects.objImpl.push_back(impl);
}

void HelloVulkan::addImplMaterial(const MaterialObj& mat)
{
  m_implObjects.implMat.push_back(mat);
}

//--------------------------------------------------------------------------------------------------
// Create a storage buffer containing the description of the scene elements
// - Which geometry is used by which instance
// - Transformation
// - Offset for texture
//
void HelloVulkan::createImplictBuffers()
{
  using vkBU = VkBufferUsageFlagBits;
  nvvk::CommandPool cmdGen(m_device, m_graphicsQueueIndex);

  // Not allowing empty buffers
  if(m_implObjects.objImpl.empty())
    m_implObjects.objImpl.push_back({});
  if(m_implObjects.implMat.empty())
    m_implObjects.implMat.push_back({});

  auto cmdBuf              = cmdGen.createCommandBuffer();
  m_implObjects.implBuf    = m_alloc.createBuffer(cmdBuf, m_implObjects.objImpl,
                                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR
                                                   | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
  m_implObjects.implMatBuf = m_alloc.createBuffer(cmdBuf, m_implObjects.implMat, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  cmdGen.submitAndWait(cmdBuf);
  m_alloc.finalizeAndReleaseStaging();
  m_debug.setObjectName(m_implObjects.implBuf.buffer, "implicitObj");
  m_debug.setObjectName(m_implObjects.implMatBuf.buffer, "implicitMat");
}
