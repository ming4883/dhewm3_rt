// rtgl_heathaze.cpp
// dhewm3-owned, app-side heat-haze / screen-space refraction custom pass.
//
// This is the "app draws itself" implementation: it renders Doom 3's
// heatHazeWithMask.vfp screen-space quads directly into the RTGL1 composited
// frame, using Vulkan handles handed to it by RTGL1 through the
// RgDrawFrameCustomPassParams callback (see RTGL1.h / VulkanDevice.cpp).
// All game-specific logic (material classification, quad packing, POD layout,
// shader source) now lives in dhewm3; RTGL1 only supplies the injection point.
//
// Blueprint: RTGL1's former Source/CustomPass.cpp, adapted to use plain core
// Vulkan calls (RTGL1's MemoryAllocator / Utils / ShaderManager are private).
//
// Stage 3: the DLL CustomPass is gone; this callback is the only drawer.
// The payload comes from the global getter (rtgl_geometry.cpp); pUserData is
// unused by RTGL1 (kept NULL).

#include "sys/platform.h"

#ifdef D3_RTGL

#include "framework/Common.h"
#include "framework/CVarSystem.h"
#include "idlib/Str.h"
#include "renderer/rtgl/rtgl_internal.h"
#include "renderer/rtgl/rtgl_cvars.h"
#include "renderer/rtgl/rtgl_heathaze.h"

#ifndef RG_USE_SURFACE_WIN32
#define RG_USE_SURFACE_WIN32
#endif
#include <vulkan/vulkan.h>

#include <stdio.h>

// Matches RsHeatHaze.vert / .frag (3x vec4 = 48 bytes).
struct HeatHazePush
{
    float rect[ 4 ];   // x, y, w, h
    float anim[ 4 ];   // time, ior, waveSpeed, waveStrength
    float quad[ 4 ];   // phase, strength, maskFeather, aspect
};

namespace
{

static VkDevice         g_device          = VK_NULL_HANDLE;
static VkPhysicalDevice g_physicalDevice  = VK_NULL_HANDLE;

static VkSampler              g_sampler        = VK_NULL_HANDLE;
static VkDescriptorSetLayout  g_descSetLayout  = VK_NULL_HANDLE;
static VkDescriptorPool       g_descPool       = VK_NULL_HANDLE;
static VkDescriptorSet        g_descSet        = VK_NULL_HANDLE;
static VkRenderPass           g_renderPass     = VK_NULL_HANDLE;
static VkPipelineLayout       g_pipelineLayout = VK_NULL_HANDLE;
static VkPipeline             g_pipeline       = VK_NULL_HANDLE;
static VkShaderModule         g_vertModule     = VK_NULL_HANDLE;
static VkShaderModule         g_fragModule     = VK_NULL_HANDLE;

static VkImage         g_scratch       = VK_NULL_HANDLE;
static VkImageView     g_scratchView   = VK_NULL_HANDLE;
static VkDeviceMemory  g_scratchMemory = VK_NULL_HANDLE;
static uint32_t        g_scratchW = 0, g_scratchH = 0;
static VkFormat        g_scratchFormat = VK_FORMAT_UNDEFINED;
static VkFormat        g_renderPassFormat = VK_FORMAT_UNDEFINED;

static bool g_ready = false;

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------
static bool HZ_LoadSpv( const char* path, uint32_t** outCode, size_t* outWords )
{
    *outCode = NULL;
    *outWords = 0;
    FILE* f = NULL;
    if ( fopen_s( &f, path, "rb" ) != 0 || !f ) {
        common->Warning( "RTGL_HeatHaze: cannot open spv '%s'", path );
        return false;
    }
    fseek( f, 0, SEEK_END );
    long bytes = ftell( f );
    fseek( f, 0, SEEK_SET );
    if ( bytes <= 0 || ( bytes % 4 ) != 0 ) {
        fclose( f );
        common->Warning( "RTGL_HeatHaze: bad spv size for '%s'", path );
        return false;
    }
    size_t words = (size_t)bytes / 4;
    uint32_t* code = (uint32_t*)malloc( (size_t)bytes );
    if ( fread( code, 1, (size_t)bytes, f ) != (size_t)bytes ) {
        free( code );
        fclose( f );
        common->Warning( "RTGL_HeatHaze: short read on '%s'", path );
        return false;
    }
    fclose( f );
    *outCode = code;
    *outWords = words;
    return true;
}

static VkShaderModule HZ_CreateShaderModule( VkDevice device, const char* path )
{
    uint32_t* code = NULL;
    size_t words = 0;
    if ( !HZ_LoadSpv( path, &code, &words ) ) { return VK_NULL_HANDLE; }

    VkShaderModuleCreateInfo info = {};
    info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = words * 4;
    info.pCode    = code;
    VkShaderModule mod = VK_NULL_HANDLE;
    VkResult r = vkCreateShaderModule( device, &info, NULL, &mod );
    free( code );
    if ( r != VK_SUCCESS ) {
        common->Warning( "RTGL_HeatHaze: vkCreateShaderModule failed (%d) for '%s'", (int)r, path );
        return VK_NULL_HANDLE;
    }
    return mod;
}

static uint32_t HZ_FindMemoryType( uint32_t typeBits, VkMemoryPropertyFlags props )
{
    VkPhysicalDeviceMemoryProperties memProps = {};
    vkGetPhysicalDeviceMemoryProperties( g_physicalDevice, &memProps );
    for ( uint32_t i = 0; i < memProps.memoryTypeCount; i++ ) {
        if ( ( typeBits & ( 1u << i ) ) &&
             ( memProps.memoryTypes[ i ].propertyFlags & props ) == props ) {
            return i;
        }
    }
    // Fallback: first bit set (VMA would do the same kind of fallback).
    for ( uint32_t i = 0; i < memProps.memoryTypeCount; i++ ) {
        if ( typeBits & ( 1u << i ) ) { return i; }
    }
    return 0;
}

static void HZ_Barrier( VkCommandBuffer cmd, VkImage image,
                        VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                        VkImageLayout oldLayout, VkImageLayout newLayout )
{
    // Mirrors RTGL1 Utils::BarrierImage: ALL_COMMANDS -> ALL_COMMANDS.
    VkImageMemoryBarrier b = {};
    b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask       = srcAccess;
    b.dstAccessMask       = dstAccess;
    b.oldLayout           = oldLayout;
    b.newLayout           = newLayout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image               = image;
    b.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.baseMipLevel   = 0;
    b.subresourceRange.levelCount     = 1;
    b.subresourceRange.baseArrayLayer = 0;
    b.subresourceRange.layerCount     = 1;
    vkCmdPipelineBarrier( cmd,
                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          0, 0, NULL, 0, NULL, 1, &b );
}

// ---------------------------------------------------------------------------
// one-time resources (device-scope)
// ---------------------------------------------------------------------------
static bool HZ_CreateSampler()
{
    VkSamplerCreateInfo info = {};
    info.sType                   = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter               = VK_FILTER_LINEAR;
    info.minFilter               = VK_FILTER_LINEAR;
    info.mipmapMode              = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    info.addressModeU            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.borderColor             = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    info.unnormalizedCoordinates = VK_FALSE;
    return vkCreateSampler( g_device, &info, NULL, &g_sampler ) == VK_SUCCESS;
}

static bool HZ_CreateDescriptors()
{
    VkDescriptorSetLayoutBinding binding = {};
    binding.binding         = 0;
    binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo linfo = {};
    linfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    linfo.bindingCount = 1;
    linfo.pBindings    = &binding;
    if ( vkCreateDescriptorSetLayout( g_device, &linfo, NULL, &g_descSetLayout ) != VK_SUCCESS ) {
        return false;
    }

    VkDescriptorPoolSize poolSize = {};
    poolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 1;
    VkDescriptorPoolCreateInfo pinfo = {};
    pinfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pinfo.maxSets       = 1;
    pinfo.poolSizeCount = 1;
    pinfo.pPoolSizes    = &poolSize;
    if ( vkCreateDescriptorPool( g_device, &pinfo, NULL, &g_descPool ) != VK_SUCCESS ) {
        return false;
    }

    VkDescriptorSetAllocateInfo ainfo = {};
    ainfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ainfo.descriptorPool     = g_descPool;
    ainfo.descriptorSetCount = 1;
    ainfo.pSetLayouts        = &g_descSetLayout;
    return vkAllocateDescriptorSets( g_device, &ainfo, &g_descSet ) == VK_SUCCESS;
}

static bool HZ_CreateRenderPass( VkFormat colorFormat )
{
    if ( g_renderPass != VK_NULL_HANDLE && g_renderPassFormat == colorFormat ) {
        return true;
    }
    if ( g_renderPass != VK_NULL_HANDLE ) {
        vkDestroyRenderPass( g_device, g_renderPass, NULL );
        g_renderPass = VK_NULL_HANDLE;
    }
    // LOAD_OP_LOAD preserves the composited frame; GENERAL matches the layout
    // the framebuffers are left in after post-processing.
    VkAttachmentDescription color = {};
    color.format         = colorFormat;
    color.samples        = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout  = VK_IMAGE_LAYOUT_GENERAL;
    color.finalLayout    = VK_IMAGE_LAYOUT_GENERAL;

    VkAttachmentReference colorRef = {};
    colorRef.attachment = 0;
    colorRef.layout     = VK_IMAGE_LAYOUT_GENERAL;

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments    = &colorRef;

    VkSubpassDependency dep = {};
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass    = 0;
    dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo info = {};
    info.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 1;
    info.pAttachments    = &color;
    info.subpassCount    = 1;
    info.pSubpasses      = &subpass;
    info.dependencyCount = 1;
    info.pDependencies   = &dep;

    if ( vkCreateRenderPass( g_device, &info, NULL, &g_renderPass ) != VK_SUCCESS ) {
        return false;
    }
    g_renderPassFormat = colorFormat;
    return true;
}

static bool HZ_CreatePipeline()
{
    if ( g_pipeline != VK_NULL_HANDLE ) { return true; }

    char path[ MAX_PATH ];
    idStr::snPrintf( path, sizeof( path ), "%s/shaders/RsHeatHaze.vert.spv",
                     r_rtgl_overridePath.GetString() );
    g_vertModule = HZ_CreateShaderModule( g_device, path );
    idStr::snPrintf( path, sizeof( path ), "%s/shaders/RsHeatHaze.frag.spv",
                     r_rtgl_overridePath.GetString() );
    g_fragModule = HZ_CreateShaderModule( g_device, path );
    if ( g_vertModule == VK_NULL_HANDLE || g_fragModule == VK_NULL_HANDLE ) {
        return false;
    }

    VkPushConstantRange push = {};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push.offset     = 0;
    push.size       = sizeof( HeatHazePush );
    VkPipelineLayoutCreateInfo linfo = {};
    linfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    linfo.setLayoutCount         = 1;
    linfo.pSetLayouts            = &g_descSetLayout;
    linfo.pushConstantRangeCount = 1;
    linfo.pPushConstantRanges    = &push;
    if ( vkCreatePipelineLayout( g_device, &linfo, NULL, &g_pipelineLayout ) != VK_SUCCESS ) {
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[ 2 ] = {};
    stages[ 0 ].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[ 0 ].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[ 0 ].module = g_vertModule;
    stages[ 0 ].pName  = "main";
    stages[ 1 ].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[ 1 ].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[ 1 ].module = g_fragModule;
    stages[ 1 ].pName  = "main";

    VkPipelineVertexInputStateCreateInfo vertexInput = {};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
    inputAssembly.sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology               = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    inputAssembly.primitiveRestartEnable = VK_FALSE;
    VkPipelineViewportStateCreateInfo viewportState = {};
    viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount  = 1;
    VkPipelineRasterizationStateCreateInfo raster = {};
    raster.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode             = VK_POLYGON_MODE_FILL;
    raster.cullMode                = VK_CULL_MODE_NONE;
    raster.frontFace               = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth               = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisampling = {};
    multisampling.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depthStencil = {};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    VkPipelineColorBlendAttachmentState blend = {};
    blend.blendEnable         = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp        = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp        = VK_BLEND_OP_ADD;
    blend.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo colorBlend = {};
    colorBlend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.logicOpEnable   = VK_FALSE;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments    = &blend;
    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicInfo = {};
    dynamicInfo.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicInfo.dynamicStateCount = 2;
    dynamicInfo.pDynamicStates    = dynStates;

    VkGraphicsPipelineCreateInfo info = {};
    info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount          = 2;
    info.pStages             = stages;
    info.pVertexInputState   = &vertexInput;
    info.pInputAssemblyState = &inputAssembly;
    info.pViewportState      = &viewportState;
    info.pRasterizationState = &raster;
    info.pMultisampleState   = &multisampling;
    info.pDepthStencilState  = &depthStencil;
    info.pColorBlendState    = &colorBlend;
    info.pDynamicState       = &dynamicInfo;
    info.layout              = g_pipelineLayout;
    info.renderPass          = g_renderPass;
    info.subpass             = 0;
    return vkCreateGraphicsPipelines( g_device, VK_NULL_HANDLE, 1, &info, NULL, &g_pipeline ) == VK_SUCCESS;
}

static void HZ_DestroyPipeline()
{
    if ( g_pipeline != VK_NULL_HANDLE )       { vkDestroyPipeline( g_device, g_pipeline, NULL ); g_pipeline = VK_NULL_HANDLE; }
    if ( g_pipelineLayout != VK_NULL_HANDLE ) { vkDestroyPipelineLayout( g_device, g_pipelineLayout, NULL ); g_pipelineLayout = VK_NULL_HANDLE; }
    if ( g_vertModule != VK_NULL_HANDLE )     { vkDestroyShaderModule( g_device, g_vertModule, NULL ); g_vertModule = VK_NULL_HANDLE; }
    if ( g_fragModule != VK_NULL_HANDLE )     { vkDestroyShaderModule( g_device, g_fragModule, NULL ); g_fragModule = VK_NULL_HANDLE; }
}

static void HZ_DestroyScratch()
{
    if ( g_scratchView != VK_NULL_HANDLE )  { vkDestroyImageView( g_device, g_scratchView, NULL ); g_scratchView = VK_NULL_HANDLE; }
    if ( g_scratch != VK_NULL_HANDLE )      { vkDestroyImage( g_device, g_scratch, NULL ); g_scratch = VK_NULL_HANDLE; }
    if ( g_scratchMemory != VK_NULL_HANDLE ){ vkFreeMemory( g_device, g_scratchMemory, NULL ); g_scratchMemory = VK_NULL_HANDLE; }
    g_scratchW = g_scratchH = 0;
    g_scratchFormat = VK_FORMAT_UNDEFINED;
}

static bool HZ_EnsureScratch( uint32_t width, uint32_t height, VkFormat format )
{
    if ( g_scratch != VK_NULL_HANDLE && g_scratchW == width && g_scratchH == height &&
         g_scratchFormat == format ) {
        return true;
    }
    HZ_DestroyScratch();

    VkImageCreateInfo imageInfo = {};
    imageInfo.sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType   = VK_IMAGE_TYPE_2D;
    imageInfo.format      = format;
    imageInfo.extent      = { width, height, 1 };
    imageInfo.mipLevels   = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples     = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling      = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage       = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if ( vkCreateImage( g_device, &imageInfo, NULL, &g_scratch ) != VK_SUCCESS ) {
        return false;
    }

    VkMemoryRequirements req = {};
    vkGetImageMemoryRequirements( g_device, g_scratch, &req );
    VkMemoryAllocateInfo alloc = {};
    alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize  = req.size;
    alloc.memoryTypeIndex = HZ_FindMemoryType( req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT );
    if ( vkAllocateMemory( g_device, &alloc, NULL, &g_scratchMemory ) != VK_SUCCESS ) {
        vkDestroyImage( g_device, g_scratch, NULL );
        g_scratch = VK_NULL_HANDLE;
        return false;
    }
    vkBindImageMemory( g_device, g_scratch, g_scratchMemory, 0 );

    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image            = g_scratch;
    viewInfo.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format           = format;
    viewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel   = 0;
    viewInfo.subresourceRange.levelCount     = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount     = 1;
    if ( vkCreateImageView( g_device, &viewInfo, NULL, &g_scratchView ) != VK_SUCCESS ) {
        HZ_DestroyScratch();
        return false;
    }

    g_scratchW = width; g_scratchH = height; g_scratchFormat = format;

    VkDescriptorImageInfo imgInfo = {};
    imgInfo.sampler     = g_sampler;
    imgInfo.imageView   = g_scratchView;
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write = {};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = g_descSet;
    write.dstBinding      = 0;
    write.dstArrayElement = 0;
    write.descriptorCount = 1;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo      = &imgInfo;
    vkUpdateDescriptorSets( g_device, 1, &write, 0, NULL );
    return true;
}

static bool HZ_EnsureResources( VkDevice device, VkPhysicalDevice physicalDevice,
                                uint32_t width, uint32_t height, VkFormat colorFormat )
{
    // device/physicalDevice are stable for the lifetime of an RTGL instance;
    // if they change (re-init), tear everything down and rebuild.
    if ( g_ready && ( device != g_device || physicalDevice != g_physicalDevice ) ) {
        RTGL_HeatHaze_Shutdown();
    }
    bool fresh = false;
    if ( !g_ready ) {
        g_device = device;
        g_physicalDevice = physicalDevice;
        if ( !HZ_CreateSampler() || !HZ_CreateDescriptors() ) {
            RTGL_HeatHaze_Shutdown();
            return false;
        }
        fresh = true;
    }
    // renderPass / pipeline are format-dependent (attachment format).
    if ( g_renderPass == VK_NULL_HANDLE || g_renderPassFormat != colorFormat ) {
        if ( !HZ_CreateRenderPass( colorFormat ) ) {
            RTGL_HeatHaze_Shutdown();
            return false;
        }
        HZ_DestroyPipeline();  // pipeline references the old renderPass
    }
    if ( g_pipeline == VK_NULL_HANDLE && !HZ_CreatePipeline() ) {
        RTGL_HeatHaze_Shutdown();
        return false;
    }
    if ( !HZ_EnsureScratch( width, height, colorFormat ) ) {
        RTGL_HeatHaze_Shutdown();
        return false;
    }
    g_ready = true;
    (void)fresh;
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// public
// ---------------------------------------------------------------------------
void RTGL_HeatHaze_Shutdown( void )
{
    if ( g_device == VK_NULL_HANDLE ) { return; }
    HZ_DestroyScratch();
    HZ_DestroyPipeline();
    if ( g_renderPass != VK_NULL_HANDLE )     { vkDestroyRenderPass( g_device, g_renderPass, NULL ); g_renderPass = VK_NULL_HANDLE; }
    if ( g_descPool != VK_NULL_HANDLE )       { vkDestroyDescriptorPool( g_device, g_descPool, NULL ); g_descPool = VK_NULL_HANDLE; }
    if ( g_descSetLayout != VK_NULL_HANDLE )  { vkDestroyDescriptorSetLayout( g_device, g_descSetLayout, NULL ); g_descSetLayout = VK_NULL_HANDLE; }
    if ( g_sampler != VK_NULL_HANDLE )        { vkDestroySampler( g_device, g_sampler, NULL ); g_sampler = VK_NULL_HANDLE; }
    g_descSet = VK_NULL_HANDLE;
    g_renderPassFormat = VK_FORMAT_UNDEFINED;
    g_ready = false;
    g_device = VK_NULL_HANDLE;
    g_physicalDevice = VK_NULL_HANDLE;
}

void RGAPI_CALL RTGL_HeatHaze_CustomPass(
    void*    pCmd,
    uint32_t frameIndex,
    void*    pColorTargetView,
    void*    pDepthTargetView,
    void*    pCurrentRenderView,
    uint32_t targetWidth,
    uint32_t targetHeight,
    void*    pDevice,
    void*    pPhysicalDevice,
    void*    pColorImage,
    uint32_t colorFormat,
    void*    pUserData )
{
    (void)frameIndex; (void)pDepthTargetView; (void)pCurrentRenderView; (void)pUserData;

    const RgCustomPassHeatHazeFrame* payload = RTGL_GetHeatHazePayload();
    if ( payload == NULL ) { return; }
    if ( payload->magic != RG_CUSTOM_PASS_HEATHAZE_MAGIC ||
         payload->version != RG_CUSTOM_PASS_HEATHAZE_VERSION ||
         payload->quadCount == 0 ) {
        return;
    }

    VkCommandBuffer cmd          = (VkCommandBuffer)pCmd;
    VkDevice        device       = (VkDevice)pDevice;
    VkPhysicalDevice physDevice  = (VkPhysicalDevice)pPhysicalDevice;
    VkImage         colorImage   = (VkImage)pColorImage;
    VkImageView     colorView    = (VkImageView)pColorTargetView;
    VkFormat        format       = (VkFormat)colorFormat;
    if ( cmd == VK_NULL_HANDLE || device == VK_NULL_HANDLE || colorView == VK_NULL_HANDLE ||
         colorImage == VK_NULL_HANDLE || targetWidth == 0 || targetHeight == 0 ) {
        return;
    }

    if ( !HZ_EnsureResources( device, physDevice, targetWidth, targetHeight, format ) ) {
        return;
    }

    // 1) color (GENERAL) -> TRANSFER_SRC; scratch (UNDEFINED) -> TRANSFER_DST.
    HZ_Barrier( cmd, colorImage,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT,
                VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL );
    HZ_Barrier( cmd, g_scratch, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL );

    // 2) Copy the composited frame into the scratch image (breaks feedback loop).
    VkImageCopy copy = {};
    copy.srcSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.srcSubresource.mipLevel       = 0;
    copy.srcSubresource.baseArrayLayer = 0;
    copy.srcSubresource.layerCount     = 1;
    copy.dstSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.dstSubresource.mipLevel       = 0;
    copy.dstSubresource.baseArrayLayer = 0;
    copy.dstSubresource.layerCount     = 1;
    copy.extent = { targetWidth, targetHeight, 1 };
    vkCmdCopyImage( cmd, colorImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    g_scratch, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy );

    // 3) color back to GENERAL (render pass attachment); scratch -> SHADER_READ_ONLY.
    HZ_Barrier( cmd, colorImage,
                VK_ACCESS_TRANSFER_READ_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL );
    HZ_Barrier( cmd, g_scratch,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL );

    // 4) Render pass over the color attachment (LOAD_OP_LOAD, GENERAL->GENERAL).
    VkFramebufferCreateInfo fbInfo = {};
    fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass      = g_renderPass;
    fbInfo.attachmentCount = 1;
    fbInfo.pAttachments    = &colorView;
    fbInfo.width           = targetWidth;
    fbInfo.height          = targetHeight;
    fbInfo.layers          = 1;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    if ( vkCreateFramebuffer( device, &fbInfo, NULL, &framebuffer ) != VK_SUCCESS ) {
        return;
    }

    VkRenderPassBeginInfo beginInfo = {};
    beginInfo.sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    beginInfo.renderPass  = g_renderPass;
    beginInfo.framebuffer = framebuffer;
    beginInfo.renderArea  = { { 0, 0 }, { targetWidth, targetHeight } };
    vkCmdBeginRenderPass( cmd, &beginInfo, VK_SUBPASS_CONTENTS_INLINE );

    VkViewport viewport = { 0.0f, 0.0f, float( targetWidth ), float( targetHeight ), 0.0f, 1.0f };
    VkRect2D   scissor  = { { 0, 0 }, { targetWidth, targetHeight } };
    vkCmdSetViewport( cmd, 0, 1, &viewport );
    vkCmdSetScissor( cmd, 0, 1, &scissor );
    vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipeline );
    vkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipelineLayout,
                             0, 1, &g_descSet, 0, NULL );

    uint32_t n = payload->quadCount;
    if ( n > RG_CUSTOM_PASS_HEATHAZE_MAX_QUADS ) { n = RG_CUSTOM_PASS_HEATHAZE_MAX_QUADS; }
    for ( uint32_t i = 0; i < n; i++ )
    {
        const RgCustomPassHeatHazeQuad& q = payload->quads[ i ];
        if ( q.rectW <= 0.0f || q.rectH <= 0.0f ) { continue; }

        HeatHazePush pc = {};
        pc.rect[ 0 ] = q.rectX; pc.rect[ 1 ] = q.rectY;
        pc.rect[ 2 ] = q.rectW; pc.rect[ 3 ] = q.rectH;
        pc.anim[ 0 ] = payload->time;
        pc.anim[ 1 ] = payload->ior;
        pc.anim[ 2 ] = payload->waveSpeed;
        pc.anim[ 3 ] = payload->waveStrength;
        pc.quad[ 0 ] = q.phase;
        pc.quad[ 1 ] = q.strength;
        pc.quad[ 2 ] = q.maskFeather;
        pc.quad[ 3 ] = float( targetWidth ) / float( targetHeight > 0 ? targetHeight : 1 );

        vkCmdPushConstants( cmd, g_pipelineLayout,
                            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                            0, sizeof( pc ), &pc );
        vkCmdDraw( cmd, 4, 1, 0, 0 );
    }

    vkCmdEndRenderPass( cmd );
    vkDestroyFramebuffer( device, framebuffer, NULL );

    // Restore scratch to a neutral layout for the next frame's UNDEFINED transition.
    HZ_Barrier( cmd, g_scratch,
                VK_ACCESS_SHADER_READ_BIT, 0,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_IMAGE_LAYOUT_GENERAL );
}

#endif // D3_RTGL
