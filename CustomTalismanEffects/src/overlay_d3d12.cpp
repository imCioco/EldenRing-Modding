#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "log.hpp"
#include "overlay_d3d12.hpp"
#include "overlay_frame.hpp"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace cte::overlay::d3d12 {
namespace {

using Microsoft::WRL::ComPtr;

void* com_identity(IUnknown* object) noexcept {
    if (!object) return nullptr;
    IUnknown* identity = nullptr;
    if (FAILED(object->QueryInterface(IID_PPV_ARGS(&identity))) || !identity)
        return nullptr;
    void* result = identity;
    identity->Release();
    return result;
}

bool supported_format(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return true;
    default:
        return false;
    }
}

const char* color_mode_name(ColorMode mode) noexcept {
    switch (mode) {
    case ColorMode::Sdr: return "SDR";
    case ColorMode::ScRgb: return "scRGB";
    case ColorMode::Hdr10: return "HDR10";
    default: return "unknown";
    }
}

const char* color_evidence_name(ColorEvidence evidence) noexcept {
    switch (evidence) {
    case ColorEvidence::Dxgi: return "DXGI";
    case ColorEvidence::NvidiaNvapi: return "NVIDIA_NVAPI";
    default: return "unknown";
    }
}

D3D12_HEAP_PROPERTIES heap_properties(D3D12_HEAP_TYPE type) noexcept {
    D3D12_HEAP_PROPERTIES result{};
    result.Type = type;
    result.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    result.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    result.CreationNodeMask = 1;
    result.VisibleNodeMask = 1;
    return result;
}

D3D12_RESOURCE_DESC buffer_desc(UINT64 size) noexcept {
    D3D12_RESOURCE_DESC result{};
    result.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    result.Alignment = 0;
    result.Width = size;
    result.Height = 1;
    result.DepthOrArraySize = 1;
    result.MipLevels = 1;
    result.Format = DXGI_FORMAT_UNKNOWN;
    result.SampleDesc.Count = 1;
    result.SampleDesc.Quality = 0;
    result.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    result.Flags = D3D12_RESOURCE_FLAG_NONE;
    return result;
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource,
                                  D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after) noexcept {
    D3D12_RESOURCE_BARRIER result{};
    result.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    result.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    result.Transition.pResource = resource;
    result.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    result.Transition.StateBefore = before;
    result.Transition.StateAfter = after;
    return result;
}

bool compile_shader(const char* source, const char* entry, const char* target,
                    ComPtr<ID3DBlob>& bytecode) {
    ComPtr<ID3DBlob> errors;
    const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS |
                       D3DCOMPILE_OPTIMIZATION_LEVEL3;
    const HRESULT hr = D3DCompile(source, std::strlen(source), "CustomTalismanEffects-v2",
                                  nullptr, nullptr, entry, target, flags, 0,
                                  &bytecode, &errors);
    if (FAILED(hr)) {
        if (errors && errors->GetBufferPointer()) {
            flog("[overlay-v2] shader compile failed: %.*s",
                 static_cast<int>(errors->GetBufferSize()),
                 static_cast<const char*>(errors->GetBufferPointer()));
        }
        return false;
    }
    return true;
}

constexpr const char* kShaderSource = R"hlsl(
cbuffer vertexBuffer : register(b0)
{
    float4x4 ProjectionMatrix;
    uint ColorMode;
    float PaperWhiteNits;
    float2 Padding;
};

struct VS_INPUT
{
    float2 pos : POSITION;
    float2 uv  : TEXCOORD0;
    float4 col : COLOR0;
};

struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
    float4 col : COLOR0;
};

PS_INPUT VSMain(VS_INPUT input)
{
    PS_INPUT output;
    output.pos = mul(ProjectionMatrix, float4(input.pos.xy, 0.0f, 1.0f));
    output.uv = input.uv;
    output.col = input.col;
    return output;
}

Texture2D texture0 : register(t0);
SamplerState sampler0 : register(s0);

float3 SrgbToLinear(float3 value)
{
    float3 low = value / 12.92f;
    float3 high = pow((value + 0.055f) / 1.055f, 2.4f);
    return lerp(high, low, step(value, 0.04045f));
}

float3 Linear709To2020(float3 value)
{
    return mul(float3x3(
        0.6274040f, 0.3292820f, 0.0433136f,
        0.0690970f, 0.9195400f, 0.0113612f,
        0.0163916f, 0.0880132f, 0.8955950f), value);
}

float3 PqEncode(float3 normalizedNits)
{
    const float m1 = 2610.0f / 16384.0f;
    const float m2 = 2523.0f / 32.0f;
    const float c1 = 3424.0f / 4096.0f;
    const float c2 = 2413.0f / 128.0f;
    const float c3 = 2392.0f / 128.0f;
    float3 p = pow(saturate(normalizedNits), m1);
    return pow((c1 + c2 * p) / (1.0f + c3 * p), m2);
}

float4 PSMain(PS_INPUT input) : SV_Target
{
    float4 value = texture0.Sample(sampler0, input.uv) * input.col;
    if (ColorMode == 0)
        return value;

    float3 linear709 = SrgbToLinear(saturate(value.rgb));
    if (ColorMode == 1)
        return float4(linear709 * (PaperWhiteNits / 80.0f), value.a);

    // HDR10 target: UI colors are authored in sRGB/Rec.709.  Convert to
    // Rec.2020, place diffuse white at PaperWhiteNits, then ST.2084 encode.
    float3 linear2020 = Linear709To2020(linear709);
    return float4(PqEncode(linear2020 * (PaperWhiteNits / 10000.0f)), value.a);
}
)hlsl";

constexpr const char* kComposeShaderSource = R"hlsl(
struct PS_INPUT { float4 pos : SV_POSITION; };

PS_INPUT VSMain(uint vertex_id : SV_VertexID)
{
    float2 positions[3] = {
        float2(-1.0f, -1.0f),
        float2(-1.0f,  3.0f),
        float2( 3.0f, -1.0f)
    };
    PS_INPUT output;
    output.pos = float4(positions[vertex_id], 0.0f, 1.0f);
    return output;
}

Texture2D<float4> background_texture : register(t0);
Texture2D<float4> overlay_texture : register(t1);

float3 PqDecode(float3 encoded)
{
    const float m1 = 2610.0f / 16384.0f;
    const float m2 = 2523.0f / 32.0f;
    const float c1 = 3424.0f / 4096.0f;
    const float c2 = 2413.0f / 128.0f;
    const float c3 = 2392.0f / 128.0f;
    float3 p = pow(saturate(encoded), 1.0f / m2);
    return pow(max(p - c1, 0.0f) / max(c2 - c3 * p, 1.0e-6f), 1.0f / m1);
}

float3 PqEncode(float3 normalizedNits)
{
    const float m1 = 2610.0f / 16384.0f;
    const float m2 = 2523.0f / 32.0f;
    const float c1 = 3424.0f / 4096.0f;
    const float c2 = 2413.0f / 128.0f;
    const float c3 = 2392.0f / 128.0f;
    float3 p = pow(max(normalizedNits, 0.0f), m1);
    return pow((c1 + c2 * p) / (1.0f + c3 * p), m2);
}

float3 Linear709To2020(float3 value)
{
    return mul(float3x3(
        0.6274040f, 0.3292820f, 0.0433136f,
        0.0690970f, 0.9195400f, 0.0113612f,
        0.0163916f, 0.0880132f, 0.8955950f), value);
}

float4 PSMain(PS_INPUT input) : SV_Target
{
    const int2 pixel = int2(input.pos.xy);
    const float4 background = background_texture.Load(int3(pixel, 0));
    // RGB in the private FP16 target is already premultiplied, linear Rec.709
    // in scRGB units (1.0 == 80 nits). Alpha is accumulated conventionally.
    const float4 overlay = overlay_texture.Load(int3(pixel, 0));
    if (overlay.a <= 0.0f)
        return background;
    const float3 background2020 = PqDecode(background.rgb);
    const float3 overlay2020 =
        Linear709To2020(overlay.rgb) * (80.0f / 10000.0f);
    const float3 composed =
        overlay2020 + background2020 * (1.0f - saturate(overlay.a));
    return float4(PqEncode(composed), background.a);
}
)hlsl";

struct ShaderCache {
    ComPtr<ID3DBlob> imgui_vertex;
    ComPtr<ID3DBlob> imgui_pixel;
    ComPtr<ID3DBlob> compose_vertex;
    ComPtr<ID3DBlob> compose_pixel;
    bool ready = false;
};

ShaderCache g_shader_cache;
std::once_flag g_shader_once;

bool ensure_shaders() noexcept {
    try {
        std::call_once(g_shader_once, [] {
            g_shader_cache.ready =
                compile_shader(kShaderSource, "VSMain", "vs_5_0",
                               g_shader_cache.imgui_vertex) &&
                compile_shader(kShaderSource, "PSMain", "ps_5_0",
                               g_shader_cache.imgui_pixel) &&
                compile_shader(kComposeShaderSource, "VSMain", "vs_5_0",
                               g_shader_cache.compose_vertex) &&
                compile_shader(kComposeShaderSource, "PSMain", "ps_5_0",
                               g_shader_cache.compose_pixel);
        });
    } catch (...) {
        return false;
    }
    return g_shader_cache.ready;
}

struct RootConstants {
    float projection[16]{};
    uint32_t color_mode = 0;
    float paper_white_nits = 203.0f;
    float padding[2]{};
};
static_assert(sizeof(RootConstants) == 20u * sizeof(uint32_t));

bool create_root_signature(ID3D12Device* device,
                           ComPtr<ID3D12RootSignature>& root_signature) {
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    range.RegisterSpace = 0;
    range.OffsetInDescriptorsFromTableStart = 0;

    std::array<D3D12_ROOT_PARAMETER, 2> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.ShaderRegister = 0;
    parameters[0].Constants.RegisterSpace = 0;
    parameters[0].Constants.Num32BitValues = 20;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1].DescriptorTable.pDescriptorRanges = &range;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MipLODBias = 0.0f;
    sampler.MaxAnisotropy = 0;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MinLOD = 0.0f;
    sampler.MaxLOD = 0.0f;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = static_cast<UINT>(parameters.size());
    desc.pParameters = parameters.data();
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &sampler;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                 D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                 D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                 D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                           &serialized, &errors)) || !serialized)
        return false;
    return SUCCEEDED(device->CreateRootSignature(
        0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&root_signature)));
}

bool create_pipeline(ID3D12Device* device, ID3D12RootSignature* root_signature,
                     DXGI_FORMAT render_target_format,
                     ComPtr<ID3D12PipelineState>& pipeline) {
    if (!ensure_shaders())
        return false;

    const std::array<D3D12_INPUT_ELEMENT_DESC, 3> input_elements{{
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0,
         static_cast<UINT>(offsetof(ImDrawVert, pos)),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0,
         static_cast<UINT>(offsetof(ImDrawVert, uv)),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0,
         static_cast<UINT>(offsetof(ImDrawVert, col)),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    }};

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root_signature;
    desc.VS = {g_shader_cache.imgui_vertex->GetBufferPointer(),
               g_shader_cache.imgui_vertex->GetBufferSize()};
    desc.PS = {g_shader_cache.imgui_pixel->GetBufferPointer(),
               g_shader_cache.imgui_pixel->GetBufferSize()};

    desc.BlendState.AlphaToCoverageEnable = FALSE;
    desc.BlendState.IndependentBlendEnable = FALSE;
    D3D12_RENDER_TARGET_BLEND_DESC& blend = desc.BlendState.RenderTarget[0];
    blend.BlendEnable = TRUE;
    blend.LogicOpEnable = FALSE;
    blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.FrontCounterClockwise = FALSE;
    desc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    desc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    desc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.RasterizerState.MultisampleEnable = FALSE;
    desc.RasterizerState.AntialiasedLineEnable = FALSE;
    desc.RasterizerState.ForcedSampleCount = 0;
    desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    desc.DepthStencilState.DepthEnable = FALSE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.DepthStencilState.StencilEnable = FALSE;
    desc.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    desc.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    desc.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    desc.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    desc.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    desc.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.DepthStencilState.BackFace = desc.DepthStencilState.FrontFace;

    desc.InputLayout = {input_elements.data(),
                        static_cast<UINT>(input_elements.size())};
    desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = render_target_format;
    desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.NodeMask = 0;
    desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    return SUCCEEDED(device->CreateGraphicsPipelineState(
        &desc, IID_PPV_ARGS(&pipeline)));
}

bool create_compose_root_signature(
    ID3D12Device* device, ComPtr<ID3D12RootSignature>& root_signature) {
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 2;
    range.BaseShaderRegister = 0;
    range.RegisterSpace = 0;
    range.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 1;
    desc.pParameters = &parameter;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
                 D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                 D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                 D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                           &serialized, &errors)) || !serialized)
        return false;
    return SUCCEEDED(device->CreateRootSignature(
        0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&root_signature)));
}

bool create_compose_pipeline(ID3D12Device* device,
                             ID3D12RootSignature* root_signature,
                             DXGI_FORMAT render_target_format,
                             ComPtr<ID3D12PipelineState>& pipeline) {
    if (!ensure_shaders())
        return false;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root_signature;
    desc.VS = {g_shader_cache.compose_vertex->GetBufferPointer(),
               g_shader_cache.compose_vertex->GetBufferSize()};
    desc.PS = {g_shader_cache.compose_pixel->GetBufferPointer(),
               g_shader_cache.compose_pixel->GetBufferSize()};
    desc.BlendState.AlphaToCoverageEnable = FALSE;
    desc.BlendState.IndependentBlendEnable = FALSE;
    D3D12_RENDER_TARGET_BLEND_DESC& blend = desc.BlendState.RenderTarget[0];
    blend.BlendEnable = FALSE;
    blend.LogicOpEnable = FALSE;
    blend.SrcBlend = D3D12_BLEND_ONE;
    blend.DestBlend = D3D12_BLEND_ZERO;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.FrontCounterClockwise = FALSE;
    desc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    desc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    desc.RasterizerState.SlopeScaledDepthBias =
        D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.RasterizerState.MultisampleEnable = FALSE;
    desc.RasterizerState.AntialiasedLineEnable = FALSE;
    desc.RasterizerState.ForcedSampleCount = 0;
    desc.RasterizerState.ConservativeRaster =
        D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    desc.DepthStencilState.DepthEnable = FALSE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.DepthStencilState.StencilEnable = FALSE;
    desc.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    desc.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    desc.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    desc.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    desc.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    desc.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.DepthStencilState.BackFace = desc.DepthStencilState.FrontFace;
    desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = render_target_format;
    desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.NodeMask = 0;
    desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    return SUCCEEDED(device->CreateGraphicsPipelineState(
        &desc, IID_PPV_ARGS(&pipeline)));
}

struct UploadBuffer {
    ComPtr<ID3D12Resource> resource;
    uint8_t* mapped = nullptr;
    size_t capacity = 0;

    void reset() {
        if (resource && mapped) resource->Unmap(0, nullptr);
        mapped = nullptr;
        capacity = 0;
        resource.Reset();
    }

    bool ensure(ID3D12Device* device, size_t needed) {
        if (needed <= capacity && resource && mapped) return true;
        reset();
        if (needed == 0) return true;

        const size_t grown = needed + needed / 2 + 4096;
        if (grown < needed || grown > std::numeric_limits<UINT64>::max()) return false;
        const D3D12_HEAP_PROPERTIES heap = heap_properties(D3D12_HEAP_TYPE_UPLOAD);
        const D3D12_RESOURCE_DESC desc = buffer_desc(static_cast<UINT64>(grown));
        if (FAILED(device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&resource))) || !resource)
            return false;
        D3D12_RANGE read_range{0, 0};
        void* data = nullptr;
        if (FAILED(resource->Map(0, &read_range, &data)) || !data) {
            resource.Reset();
            return false;
        }
        mapped = static_cast<uint8_t*>(data);
        capacity = grown;
        return true;
    }

    ~UploadBuffer() { reset(); }
};

struct FrameSlot {
    ComPtr<ID3D12Resource> backbuffer;
    ComPtr<ID3D12Resource> hdr_background;
    ComPtr<ID3D12Resource> hdr_overlay;
    ComPtr<ID3D12CommandAllocator> allocator;
    UploadBuffer vertex_buffer;
    UploadBuffer index_buffer;
    uint64_t fence_value = 0;
    uint64_t descriptor_font_generation = 0;
    std::vector<ComPtr<ID3D12Resource>> transient_uploads;
};

struct RetiredResource {
    ComPtr<ID3D12Resource> resource;
    uint64_t retire_after_fence = 0;
};

} // namespace

bool prepare_shaders() {
    return ensure_shaders();
}

struct Session::Impl {
    void* swapchain_identity = nullptr;
    void* queue_identity = nullptr;
    void* device_identity = nullptr;

    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    ComPtr<ID3D12DescriptorHeap> srv_heap;
    ComPtr<ID3D12GraphicsCommandList> command_list;
    ComPtr<ID3D12RootSignature> root_signature;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12PipelineState> linear_overlay_pipeline;
    ComPtr<ID3D12RootSignature> compose_root_signature;
    ComPtr<ID3D12PipelineState> compose_pipeline;
    ComPtr<ID3D12Fence> fence;
    HANDLE fence_event = nullptr;

    std::vector<std::unique_ptr<FrameSlot>> slots;
    UINT rtv_stride = 0;
    UINT srv_stride = 0;
    UINT srv_descriptors_per_slot = 1;
    uint32_t buffer_count = 0;
    uint32_t backbuffer_width = 0;
    uint32_t backbuffer_height = 0;
    DXGI_FORMAT backbuffer_format = DXGI_FORMAT_UNKNOWN;
    bool hdr10_intermediate_available = false;
    ColorMode initialized_color_mode = ColorMode::Unknown;

    ComPtr<ID3D12Resource> font_texture;
    uint64_t font_generation = 0;
    std::vector<RetiredResource> retired_resources;

    uint64_t next_fence_value = 1;
    uint64_t last_signaled_fence = 0;
    bool initialized = false;

    ~Impl() {
        release_all();
        if (fence_event) {
            CloseHandle(fence_event);
            fence_event = nullptr;
        }
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtv_cpu(uint32_t index) const noexcept {
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            rtv_heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(index) * rtv_stride;
        return handle;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu(uint32_t slot_index,
                                        uint32_t within_slot = 0) const noexcept {
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            srv_heap->GetCPUDescriptorHandleForHeapStart();
        const SIZE_T descriptor_index =
            static_cast<SIZE_T>(slot_index) * srv_descriptors_per_slot + within_slot;
        handle.ptr += descriptor_index * srv_stride;
        return handle;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu(uint32_t slot_index,
                                        uint32_t within_slot = 0) const noexcept {
        D3D12_GPU_DESCRIPTOR_HANDLE handle =
            srv_heap->GetGPUDescriptorHandleForHeapStart();
        const UINT64 descriptor_index =
            static_cast<UINT64>(slot_index) * srv_descriptors_per_slot + within_slot;
        handle.ptr += descriptor_index * srv_stride;
        return handle;
    }

    void release_backbuffer_state() {
        command_list.Reset(); // recorded lists can retain backbuffer references
        slots.clear();
        rtv_heap.Reset();
        srv_heap.Reset();
        font_texture.Reset();
        retired_resources.clear();
        font_generation = 0;
        hdr10_intermediate_available = false;
        initialized_color_mode = ColorMode::Unknown;
    }

    void release_all() {
        release_backbuffer_state();
        pipeline.Reset();
        linear_overlay_pipeline.Reset();
        compose_pipeline.Reset();
        compose_root_signature.Reset();
        root_signature.Reset();
        fence.Reset();
        queue.Reset();
        device.Reset();
        initialized = false;
    }

    bool create_descriptors_and_slots(IDXGISwapChain3* swapchain,
                                      const DXGI_SWAP_CHAIN_DESC1& desc,
                                      bool make_hdr_intermediates) {
        D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
        rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtv_desc.NumDescriptors = desc.BufferCount *
            (make_hdr_intermediates ? 2u : 1u);
        rtv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(device->CreateDescriptorHeap(&rtv_desc,
                                                IID_PPV_ARGS(&rtv_heap))) || !rtv_heap)
            return false;

        D3D12_DESCRIPTOR_HEAP_DESC srv_desc{};
        srv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        // One immutable descriptor slot per backbuffer.  Updating a descriptor
        // still referenced by an in-flight command list is invalid; tying it to
        // the frame-slot fence prevents that race during font rebuilds.
        srv_descriptors_per_slot = make_hdr_intermediates ? 3u : 1u;
        srv_desc.NumDescriptors = desc.BufferCount * srv_descriptors_per_slot;
        srv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&srv_desc,
                                                IID_PPV_ARGS(&srv_heap))) || !srv_heap)
            return false;

        rtv_stride = device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        srv_stride = device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        slots.reserve(desc.BufferCount);
        for (UINT index = 0; index < desc.BufferCount; ++index) {
            auto slot = std::make_unique<FrameSlot>();
            if (FAILED(device->CreateCommandAllocator(
                    D3D12_COMMAND_LIST_TYPE_DIRECT,
                    IID_PPV_ARGS(&slot->allocator))) || !slot->allocator)
                return false;
            if (FAILED(swapchain->GetBuffer(index,
                                            IID_PPV_ARGS(&slot->backbuffer))) ||
                !slot->backbuffer)
                return false;
            const D3D12_RESOURCE_DESC buffer = slot->backbuffer->GetDesc();
            if (buffer.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
                buffer.DepthOrArraySize != 1 || buffer.MipLevels != 1 ||
                buffer.Width != desc.Width || buffer.Height != desc.Height ||
                buffer.Format != desc.Format || buffer.SampleDesc.Count != 1 ||
                (buffer.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0)
                return false;
            device->CreateRenderTargetView(slot->backbuffer.Get(), nullptr,
                                           rtv_cpu(index));

            if (make_hdr_intermediates) {
                D3D12_RESOURCE_DESC background_desc = slot->backbuffer->GetDesc();
                background_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
                const D3D12_HEAP_PROPERTIES default_heap =
                    heap_properties(D3D12_HEAP_TYPE_DEFAULT);
                if (FAILED(device->CreateCommittedResource(
                        &default_heap, D3D12_HEAP_FLAG_NONE, &background_desc,
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                        IID_PPV_ARGS(&slot->hdr_background))) ||
                    !slot->hdr_background)
                    return false;

                D3D12_RESOURCE_DESC overlay_desc{};
                overlay_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                overlay_desc.Width = background_desc.Width;
                overlay_desc.Height = background_desc.Height;
                overlay_desc.DepthOrArraySize = 1;
                overlay_desc.MipLevels = 1;
                overlay_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                overlay_desc.SampleDesc.Count = 1;
                overlay_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
                overlay_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                D3D12_CLEAR_VALUE overlay_clear{};
                overlay_clear.Format = overlay_desc.Format;
                if (FAILED(device->CreateCommittedResource(
                        &default_heap, D3D12_HEAP_FLAG_NONE, &overlay_desc,
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                        &overlay_clear, IID_PPV_ARGS(&slot->hdr_overlay))) ||
                    !slot->hdr_overlay)
                    return false;

                device->CreateRenderTargetView(
                    slot->hdr_overlay.Get(), nullptr,
                    rtv_cpu(desc.BufferCount + index));

                D3D12_SHADER_RESOURCE_VIEW_DESC background_srv{};
                background_srv.Shader4ComponentMapping =
                    D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                background_srv.Format = desc.Format;
                background_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                background_srv.Texture2D.MipLevels = 1;
                device->CreateShaderResourceView(
                    slot->hdr_background.Get(), &background_srv,
                    srv_cpu(index, 1));

                D3D12_SHADER_RESOURCE_VIEW_DESC overlay_srv = background_srv;
                overlay_srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                device->CreateShaderResourceView(
                    slot->hdr_overlay.Get(), &overlay_srv, srv_cpu(index, 2));
            }
            slots.push_back(std::move(slot));
        }
        hdr10_intermediate_available = make_hdr_intermediates;
        return true;
    }

    bool upload_font(FrameSlot& slot, uint32_t slot_index,
                     const frame::font_atlas_packet& font) {
        if (font.width == 0 || font.height == 0 || font.rgba32.empty() ||
            static_cast<uint64_t>(font.width) * font.height * 4ull !=
                font.rgba32.size())
            return false;

        if (font_generation != font.generation) {
            D3D12_RESOURCE_DESC texture_desc{};
            texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            texture_desc.Alignment = 0;
            texture_desc.Width = font.width;
            texture_desc.Height = font.height;
            texture_desc.DepthOrArraySize = 1;
            texture_desc.MipLevels = 1;
            texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            texture_desc.SampleDesc.Count = 1;
            texture_desc.SampleDesc.Quality = 0;
            texture_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            texture_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

            ComPtr<ID3D12Resource> next_font;
            const D3D12_HEAP_PROPERTIES default_heap =
                heap_properties(D3D12_HEAP_TYPE_DEFAULT);
            if (FAILED(device->CreateCommittedResource(
                    &default_heap, D3D12_HEAP_FLAG_NONE, &texture_desc,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                    IID_PPV_ARGS(&next_font))) || !next_font)
                return false;

            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            UINT rows = 0;
            UINT64 row_bytes = 0;
            UINT64 upload_size = 0;
            device->GetCopyableFootprints(&texture_desc, 0, 1, 0, &footprint,
                                          &rows, &row_bytes, &upload_size);
            if (upload_size == 0 || rows != font.height ||
                row_bytes < static_cast<UINT64>(font.width) * 4)
                return false;

            ComPtr<ID3D12Resource> upload;
            const D3D12_HEAP_PROPERTIES upload_heap =
                heap_properties(D3D12_HEAP_TYPE_UPLOAD);
            const D3D12_RESOURCE_DESC upload_desc = buffer_desc(upload_size);
            if (FAILED(device->CreateCommittedResource(
                    &upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                    IID_PPV_ARGS(&upload))) || !upload)
                return false;

            uint8_t* mapped = nullptr;
            D3D12_RANGE read_range{0, 0};
            if (FAILED(upload->Map(0, &read_range,
                                   reinterpret_cast<void**>(&mapped))) || !mapped)
                return false;
            const size_t source_pitch = static_cast<size_t>(font.width) * 4;
            for (UINT row = 0; row < rows; ++row) {
                std::memcpy(mapped + footprint.Offset +
                                static_cast<size_t>(row) * footprint.Footprint.RowPitch,
                            font.rgba32.data() + static_cast<size_t>(row) * source_pitch,
                            source_pitch);
            }
            upload->Unmap(0, nullptr);

            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = upload.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = footprint;
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = next_font.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.SubresourceIndex = 0;
            command_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            const D3D12_RESOURCE_BARRIER barrier = transition(
                next_font.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            command_list->ResourceBarrier(1, &barrier);

            if (font_texture) {
                retired_resources.push_back(
                    {font_texture, last_signaled_fence});
            }
            font_texture = std::move(next_font);
            font_generation = font.generation;
            slot.transient_uploads.push_back(std::move(upload));
        }

        if (!font_texture) return false;
        if (slot.descriptor_font_generation != font_generation) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Texture2D.MipLevels = 1;
            srv.Texture2D.MostDetailedMip = 0;
            srv.Texture2D.ResourceMinLODClamp = 0.0f;
            device->CreateShaderResourceView(font_texture.Get(), &srv,
                                             srv_cpu(slot_index));
            slot.descriptor_font_generation = font_generation;
        }
        return true;
    }

    void collect_retired() {
        if (!fence) return;
        const uint64_t complete = fence->GetCompletedValue();
        retired_resources.erase(
            std::remove_if(retired_resources.begin(), retired_resources.end(),
                [complete](const RetiredResource& item) {
                    return item.retire_after_fence <= complete;
                }),
            retired_resources.end());
    }

    void setup_render_state(const frame::frame_packet& packet,
                            uint32_t slot_index, ColorMode color_mode,
                            ID3D12PipelineState* selected_pipeline) {
        const float left = packet.display_pos.x;
        const float right = packet.display_pos.x + packet.display_size.x;
        const float top = packet.display_pos.y;
        const float bottom = packet.display_pos.y + packet.display_size.y;

        RootConstants constants{};
        const float matrix[16] = {
            2.0f / (right - left), 0.0f, 0.0f, 0.0f,
            0.0f, 2.0f / (top - bottom), 0.0f, 0.0f,
            0.0f, 0.0f, 0.5f, 0.0f,
            (right + left) / (left - right),
            (top + bottom) / (bottom - top), 0.5f, 1.0f,
        };
        std::memcpy(constants.projection, matrix, sizeof(matrix));
        constants.color_mode = static_cast<uint32_t>(color_mode);

        command_list->SetPipelineState(selected_pipeline);
        command_list->SetGraphicsRootSignature(root_signature.Get());
        ID3D12DescriptorHeap* heaps[] = {srv_heap.Get()};
        command_list->SetDescriptorHeaps(1, heaps);
        command_list->SetGraphicsRoot32BitConstants(
            0, 20, &constants, 0);
        command_list->SetGraphicsRootDescriptorTable(1, srv_gpu(slot_index));
        command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        D3D12_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(backbuffer_width);
        viewport.Height = static_cast<float>(backbuffer_height);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        command_list->RSSetViewports(1, &viewport);
    }

    D3D12_RECT draw_bounds(const frame::frame_packet& packet) const noexcept {
        float left = static_cast<float>(backbuffer_width);
        float top = static_cast<float>(backbuffer_height);
        float right = 0.0f;
        float bottom = 0.0f;
        bool any = false;
        for (const frame::draw_command& command : packet.commands) {
            if (command.kind != frame::draw_command_kind::draw) continue;
            const float command_left =
                (command.clip_rect.x - packet.display_pos.x) *
                packet.framebuffer_scale.x;
            const float command_top =
                (command.clip_rect.y - packet.display_pos.y) *
                packet.framebuffer_scale.y;
            const float command_right =
                (command.clip_rect.z - packet.display_pos.x) *
                packet.framebuffer_scale.x;
            const float command_bottom =
                (command.clip_rect.w - packet.display_pos.y) *
                packet.framebuffer_scale.y;
            if (command_right <= command_left || command_bottom <= command_top)
                continue;
            left = std::min(left, command_left);
            top = std::min(top, command_top);
            right = std::max(right, command_right);
            bottom = std::max(bottom, command_bottom);
            any = true;
        }
        if (!any) return {};
        D3D12_RECT result{};
        result.left = static_cast<LONG>(std::max(0.0f, std::floor(left)));
        result.top = static_cast<LONG>(std::max(0.0f, std::floor(top)));
        result.right = static_cast<LONG>(std::min(
            static_cast<float>(backbuffer_width), std::ceil(right)));
        result.bottom = static_cast<LONG>(std::min(
            static_cast<float>(backbuffer_height), std::ceil(bottom)));
        return result;
    }

    void record_imgui_draws(const frame::frame_packet& packet,
                            uint32_t slot_index, ColorMode color_mode,
                            ID3D12PipelineState* selected_pipeline,
                            D3D12_CPU_DESCRIPTOR_HANDLE target,
                            const D3D12_VERTEX_BUFFER_VIEW& vertex_view,
                            const D3D12_INDEX_BUFFER_VIEW& index_view) {
        command_list->OMSetRenderTargets(1, &target, FALSE, nullptr);
        command_list->IASetVertexBuffers(0, 1, &vertex_view);
        command_list->IASetIndexBuffer(&index_view);
        setup_render_state(packet, slot_index, color_mode, selected_pipeline);

        const ImVec2 clip_offset = packet.display_pos;
        const ImVec2 clip_scale = packet.framebuffer_scale;
        for (const frame::draw_command& command : packet.commands) {
            if (command.kind == frame::draw_command_kind::reset_render_state) {
                setup_render_state(packet, slot_index, color_mode,
                                   selected_pipeline);
                command_list->OMSetRenderTargets(1, &target, FALSE, nullptr);
                command_list->IASetVertexBuffers(0, 1, &vertex_view);
                command_list->IASetIndexBuffer(&index_view);
                continue;
            }

            const float clip_left =
                (command.clip_rect.x - clip_offset.x) * clip_scale.x;
            const float clip_top =
                (command.clip_rect.y - clip_offset.y) * clip_scale.y;
            const float clip_right =
                (command.clip_rect.z - clip_offset.x) * clip_scale.x;
            const float clip_bottom =
                (command.clip_rect.w - clip_offset.y) * clip_scale.y;
            if (clip_right <= clip_left || clip_bottom <= clip_top) continue;

            D3D12_RECT scissor{};
            scissor.left = static_cast<LONG>(std::max(0.0f, std::floor(clip_left)));
            scissor.top = static_cast<LONG>(std::max(0.0f, std::floor(clip_top)));
            scissor.right = static_cast<LONG>(std::min(
                static_cast<float>(backbuffer_width), std::ceil(clip_right)));
            scissor.bottom = static_cast<LONG>(std::min(
                static_cast<float>(backbuffer_height), std::ceil(clip_bottom)));
            if (scissor.right <= scissor.left || scissor.bottom <= scissor.top)
                continue;
            command_list->RSSetScissorRects(1, &scissor);
            if (command.vtx_offset >
                static_cast<uint32_t>(std::numeric_limits<INT>::max()))
                continue;
            command_list->DrawIndexedInstanced(
                command.elem_count, 1, command.idx_offset,
                static_cast<INT>(command.vtx_offset), 0);
        }
    }
};

Session::Session() : impl_(std::make_unique<Impl>()) {}
Session::~Session() = default;

bool Session::initialize(IDXGISwapChain3* swapchain,
                         ID3D12CommandQueue* queue,
                         ColorMode color_mode,
                         ColorEvidence color_evidence) {
    if (!swapchain || !queue) return false;
    impl_->release_all();

    ComPtr<ID3D12Device> swap_device;
    ComPtr<ID3D12Device> queue_device;
    if (FAILED(swapchain->GetDevice(IID_PPV_ARGS(&swap_device))) || !swap_device ||
        FAILED(queue->GetDevice(IID_PPV_ARGS(&queue_device))) || !queue_device)
        return false;
    if (com_identity(swap_device.Get()) != com_identity(queue_device.Get()))
        return false;
    // The renderer's heaps/resources intentionally target node zero.  A
    // linked-adapter implementation needs per-node creation masks and the
    // ResizeBuffers1 queue array tracked per backbuffer; fail closed here.
    if (swap_device->GetNodeCount() != 1)
        return false;

    DXGI_SWAP_CHAIN_DESC1 desc{};
    ComPtr<IDXGISwapChain1> swapchain1;
    if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&swapchain1))) || !swapchain1 ||
        FAILED(swapchain1->GetDesc1(&desc)) || desc.BufferCount < 2 ||
        desc.BufferCount > 8 || desc.SampleDesc.Count != 1 ||
        !supported_format(desc.Format))
        return false;
    if ((desc.BufferUsage & DXGI_USAGE_RENDER_TARGET_OUTPUT) == 0)
        return false;
    constexpr UINT kUnsupportedFlags =
        DXGI_SWAP_CHAIN_FLAG_RESTRICTED_CONTENT |
        DXGI_SWAP_CHAIN_FLAG_DISPLAY_ONLY |
        DXGI_SWAP_CHAIN_FLAG_HW_PROTECTED;
    if ((desc.Flags & kUnsupportedFlags) != 0)
        return false;

    // The coordinator derives the color mode from an observed DXGI or
    // target-specific vendor transition, but the renderer remains its own
    // trust boundary. Never create a PSO for an ambiguous or nonsensical
    // format/mode combination.
    const bool valid_color_contract =
        (color_mode == ColorMode::Hdr10 &&
         desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM) ||
        (color_mode == ColorMode::ScRgb &&
         desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) ||
        (color_mode == ColorMode::Sdr &&
         desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT);
    if (!valid_color_contract)
        return false;

    if (desc.Width == 0 || desc.Height == 0) {
        ComPtr<ID3D12Resource> backbuffer;
        if (FAILED(swapchain->GetBuffer(swapchain->GetCurrentBackBufferIndex(),
                                        IID_PPV_ARGS(&backbuffer))) || !backbuffer)
            return false;
        const D3D12_RESOURCE_DESC resource_desc = backbuffer->GetDesc();
        desc.Width = static_cast<UINT>(resource_desc.Width);
        desc.Height = resource_desc.Height;
    }

    impl_->device = swap_device;
    impl_->queue = queue;
    impl_->swapchain_identity = com_identity(swapchain);
    impl_->queue_identity = com_identity(queue);
    impl_->device_identity = com_identity(swap_device.Get());
    impl_->buffer_count = desc.BufferCount;
    impl_->backbuffer_width = desc.Width;
    impl_->backbuffer_height = desc.Height;
    impl_->backbuffer_format = desc.Format;

    const bool make_hdr_intermediates = color_mode == ColorMode::Hdr10;
    if (!impl_->create_descriptors_and_slots(swapchain, desc,
                                             make_hdr_intermediates) ||
        !create_root_signature(impl_->device.Get(), impl_->root_signature) ||
        !create_pipeline(impl_->device.Get(), impl_->root_signature.Get(),
                         desc.Format, impl_->pipeline)) {
        impl_->release_all();
        return false;
    }

    if (impl_->hdr10_intermediate_available &&
        (!create_pipeline(impl_->device.Get(), impl_->root_signature.Get(),
                          DXGI_FORMAT_R16G16B16A16_FLOAT,
                          impl_->linear_overlay_pipeline) ||
         !create_compose_root_signature(impl_->device.Get(),
                                        impl_->compose_root_signature) ||
         !create_compose_pipeline(impl_->device.Get(),
                                  impl_->compose_root_signature.Get(),
                                  desc.Format, impl_->compose_pipeline))) {
        impl_->release_all();
        return false;
    }

    if (FAILED(impl_->device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            impl_->slots[0]->allocator.Get(), impl_->pipeline.Get(),
            IID_PPV_ARGS(&impl_->command_list))) || !impl_->command_list ||
        FAILED(impl_->command_list->Close())) {
        impl_->release_all();
        return false;
    }
    if (FAILED(impl_->device->CreateFence(
            0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&impl_->fence))) ||
        !impl_->fence) {
        impl_->release_all();
        return false;
    }
    impl_->fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!impl_->fence_event) {
        impl_->release_all();
        return false;
    }

    impl_->initialized = true;
    impl_->initialized_color_mode = color_mode;
    flog("[overlay-v2] D3D12 renderer initialized (%ux%u, %u buffers, "
         "format %u, swapchain_color=%s, evidence=%s)",
         desc.Width, desc.Height, desc.BufferCount,
         static_cast<unsigned>(desc.Format), color_mode_name(color_mode),
         color_evidence_name(color_evidence));
    return true;
}

bool Session::matches(IDXGISwapChain3* swapchain,
                      ID3D12CommandQueue* queue,
                      ColorMode color_mode) const {
    if (!impl_->initialized || !swapchain || !queue) return false;
    return impl_->swapchain_identity == com_identity(swapchain) &&
           impl_->queue_identity == com_identity(queue) &&
           impl_->initialized_color_mode == color_mode;
}

RenderResult Session::render(
    IDXGISwapChain3* swapchain,
    const frame::frame_packet* packet,
    const frame::font_atlas_packet* font,
    ColorMode color_mode) {
    if (!impl_->initialized || !swapchain || !packet || !font ||
        packet->font_generation != font->generation ||
        packet->display_size.x <= 0.0f || packet->display_size.y <= 0.0f)
        return RenderResult::Skipped;
    if (packet->vertices.empty() || packet->indices.empty())
        return RenderResult::Skipped;

    bool has_draw = false;
    for (const frame::draw_command& command : packet->commands) {
        if (command.kind != frame::draw_command_kind::draw ||
            command.elem_count == 0)
            continue;
        const size_t index_offset = command.idx_offset;
        const size_t element_count = command.elem_count;
        if (index_offset > packet->indices.size() ||
            element_count > packet->indices.size() - index_offset ||
            command.vtx_offset >= packet->vertices.size())
            return RenderResult::RecoverableFailure;
        has_draw = true;
    }
    if (!has_draw) return RenderResult::Skipped;

    const uint32_t slot_index = swapchain->GetCurrentBackBufferIndex();
    if (slot_index >= impl_->slots.size()) return RenderResult::RecoverableFailure;
    FrameSlot& slot = *impl_->slots[slot_index];

    D3D12_RECT hdr_bounds{};
    if (color_mode == ColorMode::Hdr10) {
        if (!impl_->hdr10_intermediate_available || !slot.hdr_background ||
            !slot.hdr_overlay || !impl_->linear_overlay_pipeline ||
            !impl_->compose_pipeline || !impl_->compose_root_signature)
            return RenderResult::RecoverableFailure;
        hdr_bounds = impl_->draw_bounds(*packet);
        if (hdr_bounds.right <= hdr_bounds.left ||
            hdr_bounds.bottom <= hdr_bounds.top)
            return RenderResult::Skipped;
    }

    // Never wait in Present.  If the game outruns our tiny overlay queue work,
    // omit one UI frame and preserve game pacing instead of stalling the CPU.
    if (slot.fence_value != 0 &&
        impl_->fence->GetCompletedValue() < slot.fence_value)
        return RenderResult::Skipped;

    slot.transient_uploads.clear();
    impl_->collect_retired();

    if (FAILED(slot.allocator->Reset()) ||
        FAILED(impl_->command_list->Reset(slot.allocator.Get(), impl_->pipeline.Get()))) {
        return FAILED(impl_->device->GetDeviceRemovedReason())
            ? RenderResult::DeviceLost : RenderResult::RecoverableFailure;
    }

    if (packet->vertices.size() >
            std::numeric_limits<size_t>::max() / sizeof(ImDrawVert) ||
        packet->indices.size() >
            std::numeric_limits<size_t>::max() / sizeof(ImDrawIdx)) {
        impl_->command_list->Close();
        return RenderResult::RecoverableFailure;
    }
    const size_t vertex_bytes = packet->vertices.size() * sizeof(ImDrawVert);
    const size_t index_bytes = packet->indices.size() * sizeof(ImDrawIdx);
    if (vertex_bytes > std::numeric_limits<UINT>::max() ||
        index_bytes > std::numeric_limits<UINT>::max() ||
        !slot.vertex_buffer.ensure(impl_->device.Get(), vertex_bytes) ||
        !slot.index_buffer.ensure(impl_->device.Get(), index_bytes)) {
        impl_->command_list->Close();
        return RenderResult::RecoverableFailure;
    }
    if (vertex_bytes)
        std::memcpy(slot.vertex_buffer.mapped, packet->vertices.data(), vertex_bytes);
    if (index_bytes)
        std::memcpy(slot.index_buffer.mapped, packet->indices.data(), index_bytes);

    if (!impl_->upload_font(slot, slot_index, *font)) {
        impl_->command_list->Close();
        return RenderResult::RecoverableFailure;
    }

    D3D12_VERTEX_BUFFER_VIEW vertex_view{};
    vertex_view.BufferLocation = slot.vertex_buffer.resource->GetGPUVirtualAddress();
    vertex_view.SizeInBytes = static_cast<UINT>(vertex_bytes);
    vertex_view.StrideInBytes = sizeof(ImDrawVert);
    D3D12_INDEX_BUFFER_VIEW index_view{};
    index_view.BufferLocation = slot.index_buffer.resource->GetGPUVirtualAddress();
    index_view.SizeInBytes = static_cast<UINT>(index_bytes);
    index_view.Format = sizeof(ImDrawIdx) == 2 ? DXGI_FORMAT_R16_UINT
                                              : DXGI_FORMAT_R32_UINT;

    const D3D12_CPU_DESCRIPTOR_HANDLE backbuffer_target =
        impl_->rtv_cpu(slot_index);

    if (color_mode == ColorMode::Hdr10) {
        const D3D12_RECT bounds = hdr_bounds;

        std::array<D3D12_RESOURCE_BARRIER, 3> prepare{{
            transition(slot.backbuffer.Get(), D3D12_RESOURCE_STATE_PRESENT,
                       D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition(slot.hdr_background.Get(),
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_COPY_DEST),
            transition(slot.hdr_overlay.Get(),
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_RENDER_TARGET),
        }};
        impl_->command_list->ResourceBarrier(
            static_cast<UINT>(prepare.size()), prepare.data());
        D3D12_TEXTURE_COPY_LOCATION copy_source{};
        copy_source.pResource = slot.backbuffer.Get();
        copy_source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        copy_source.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION copy_destination{};
        copy_destination.pResource = slot.hdr_background.Get();
        copy_destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        copy_destination.SubresourceIndex = 0;
        const D3D12_BOX copy_bounds{
            static_cast<UINT>(bounds.left),
            static_cast<UINT>(bounds.top), 0,
            static_cast<UINT>(bounds.right),
            static_cast<UINT>(bounds.bottom), 1};
        impl_->command_list->CopyTextureRegion(
            &copy_destination, bounds.left, bounds.top, 0,
            &copy_source, &copy_bounds);
        const D3D12_RESOURCE_BARRIER background_ready = transition(
            slot.hdr_background.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        impl_->command_list->ResourceBarrier(1, &background_ready);

        const D3D12_CPU_DESCRIPTOR_HANDLE overlay_target =
            impl_->rtv_cpu(impl_->buffer_count + slot_index);
        constexpr float transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        impl_->command_list->ClearRenderTargetView(
            overlay_target, transparent, 1, &bounds);
        impl_->record_imgui_draws(
            *packet, slot_index, ColorMode::ScRgb,
            impl_->linear_overlay_pipeline.Get(), overlay_target,
            vertex_view, index_view);

        std::array<D3D12_RESOURCE_BARRIER, 2> compose_ready{{
            transition(slot.hdr_overlay.Get(),
                       D3D12_RESOURCE_STATE_RENDER_TARGET,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            transition(slot.backbuffer.Get(),
                       D3D12_RESOURCE_STATE_COPY_SOURCE,
                       D3D12_RESOURCE_STATE_RENDER_TARGET),
        }};
        impl_->command_list->ResourceBarrier(
            static_cast<UINT>(compose_ready.size()), compose_ready.data());

        impl_->command_list->SetPipelineState(impl_->compose_pipeline.Get());
        impl_->command_list->SetGraphicsRootSignature(
            impl_->compose_root_signature.Get());
        ID3D12DescriptorHeap* heaps[] = {impl_->srv_heap.Get()};
        impl_->command_list->SetDescriptorHeaps(1, heaps);
        impl_->command_list->SetGraphicsRootDescriptorTable(
            0, impl_->srv_gpu(slot_index, 1));
        impl_->command_list->IASetPrimitiveTopology(
            D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D12_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(impl_->backbuffer_width);
        viewport.Height = static_cast<float>(impl_->backbuffer_height);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        impl_->command_list->RSSetViewports(1, &viewport);
        impl_->command_list->RSSetScissorRects(1, &bounds);
        impl_->command_list->OMSetRenderTargets(
            1, &backbuffer_target, FALSE, nullptr);
        impl_->command_list->DrawInstanced(3, 1, 0, 0);

        const D3D12_RESOURCE_BARRIER to_present = transition(
            slot.backbuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PRESENT);
        impl_->command_list->ResourceBarrier(1, &to_present);
    } else {
        const D3D12_RESOURCE_BARRIER to_target = transition(
            slot.backbuffer.Get(), D3D12_RESOURCE_STATE_PRESENT,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        impl_->command_list->ResourceBarrier(1, &to_target);
        impl_->record_imgui_draws(
            *packet, slot_index, color_mode, impl_->pipeline.Get(),
            backbuffer_target, vertex_view, index_view);
        const D3D12_RESOURCE_BARRIER to_present = transition(
            slot.backbuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PRESENT);
        impl_->command_list->ResourceBarrier(1, &to_present);
    }

    if (FAILED(impl_->command_list->Close()))
        return FAILED(impl_->device->GetDeviceRemovedReason())
            ? RenderResult::DeviceLost : RenderResult::RecoverableFailure;

    ID3D12CommandList* lists[] = {impl_->command_list.Get()};
    impl_->queue->ExecuteCommandLists(1, lists);

    const uint64_t fence_value = impl_->next_fence_value++;
    const HRESULT signal_result = impl_->queue->Signal(impl_->fence.Get(), fence_value);
    if (FAILED(signal_result))
        return RenderResult::DeviceLost;
    slot.fence_value = fence_value;
    impl_->last_signaled_fence = fence_value;
    return RenderResult::Submitted;
}

bool Session::before_resize() {
    if (!impl_->initialized) return true;

    // ResizeBuffers requires every direct and indirect backbuffer reference to
    // be gone.  D3D12 command lists do not keep all referenced COM objects
    // alive, so a timeout is never permission to release GPU-live resources.
    // Resize is already a host synchronization transaction; waiting here is
    // allowed, while the normal Present path remains strictly non-blocking.
    if (impl_->last_signaled_fence != 0 && impl_->fence &&
        impl_->fence->GetCompletedValue() < impl_->last_signaled_fence) {
        if (!impl_->device || !impl_->fence_event) return false;

        const HRESULT arm_result = impl_->fence->SetEventOnCompletion(
            impl_->last_signaled_fence, impl_->fence_event);
        if (FAILED(arm_result)) {
            if (FAILED(impl_->device->GetDeviceRemovedReason())) {
                impl_->release_backbuffer_state();
                impl_->initialized = false;
                return true;
            }
            flog("[overlay-v2] [WARN] could not arm resize fence (0x%08X); retaining session",
                 static_cast<unsigned>(arm_result));
            return false;
        }

        constexpr ULONGLONG kResizeFenceBudgetMs = 2000;
        const ULONGLONG deadline = GetTickCount64() + kResizeFenceBudgetMs;
        for (;;) {
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) {
                flog("[overlay-v2] [WARN] resize fence exceeded %llu ms; retaining session",
                     static_cast<unsigned long long>(kResizeFenceBudgetMs));
                return false;
            }
            const DWORD wait_slice = static_cast<DWORD>(
                std::min<ULONGLONG>(100, deadline - now));
            const DWORD wait_result =
                WaitForSingleObject(impl_->fence_event, wait_slice);
            // SetEventOnCompletion registrations cannot be cancelled.  If an
            // earlier resize attempt timed out, its eventual signal can still
            // be pending on this auto-reset event.  The event is therefore
            // only a wake-up hint: the fence value itself is the authority.
            if (impl_->fence->GetCompletedValue() >= impl_->last_signaled_fence)
                break;
            if (FAILED(impl_->device->GetDeviceRemovedReason()))
                break; // the removed device can no longer execute this work
            if (wait_result == WAIT_OBJECT_0)
                continue; // consumed a stale completion from an older wait
            if (wait_result == WAIT_FAILED) {
                flog("[overlay-v2] [WARN] resize fence wait failed (%lu); retaining session",
                     GetLastError());
                return false;
            }
            if (wait_result != WAIT_TIMEOUT) {
                flog("[overlay-v2] [WARN] unexpected resize fence wait result (%lu); retaining session",
                     wait_result);
                return false;
            }
            // A timeout never authorizes releasing GPU-live objects.  The
            // finite total budget keeps a broken queue from hanging resize.
        }
    }

    impl_->release_backbuffer_state();
    impl_->initialized = false;
    return true;
}

bool Session::gpu_idle() const {
    if (!impl_->initialized || impl_->last_signaled_fence == 0)
        return true;
    if (!impl_->fence) return false;
    if (impl_->fence->GetCompletedValue() >= impl_->last_signaled_fence)
        return true;
    return impl_->device && FAILED(impl_->device->GetDeviceRemovedReason());
}

HRESULT Session::device_removed_reason() const {
    return impl_->device ? impl_->device->GetDeviceRemovedReason() : E_FAIL;
}

} // namespace cte::overlay::d3d12
