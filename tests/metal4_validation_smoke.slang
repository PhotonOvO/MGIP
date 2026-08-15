struct SmokeVertexOutput
{
    float4 position : SV_Position;
};

struct IndexedSmokeVertex
{
    [[vk::location(0)]] float2 position : POSITION;
};

struct IcbTextureArguments
{
    Texture2D<float4> texture;
    SamplerState sampler;
};

[shader("vertex")]
SmokeVertexOutput offscreenSmokeVertex(uint vertexId : SV_VertexID)
{
    const float2 positions[3] = {
        float2(-1.0f, -1.0f),
        float2( 3.0f, -1.0f),
        float2(-1.0f,  3.0f),
    };
    SmokeVertexOutput output;
    output.position = float4(positions[vertexId], 0.0f, 1.0f);
    return output;
}

[shader("fragment")]
float4 offscreenSmokeFragment(
    SmokeVertexOutput input,
    StructuredBuffer<uint> packedColor) : SV_Target0
{
    const uint color = packedColor[0];
    return float4(
        float(color & 0xffu),
        float((color >> 8u) & 0xffu),
        float((color >> 16u) & 0xffu),
        float((color >> 24u) & 0xffu)) / 255.0f;
}

[shader("vertex")]
SmokeVertexOutput indexedSmokeVertex(
    IndexedSmokeVertex input,
    uint baseInstance : SV_StartInstanceLocation)
{
    float4 position = float4(input.position, 0.0f, 1.0f);
    if(baseInstance != 7u)
        position.xy = float2(4.0f, 4.0f);
    SmokeVertexOutput output;
    output.position = position;
    return output;
}

[shader("fragment")]
float4 indexedSmokeFragment(SmokeVertexOutput input) : SV_Target0
{
    return float4(0.0f, 1.0f, 0.0f, 1.0f);
}

[shader("fragment")]
float4 icbTopLevelTextureSmokeFragment(
    SmokeVertexOutput input,
    Texture2D<float4> texture,
    SamplerState sampler) : SV_Target0
{
    return texture.Sample(sampler, float2(0.5f, 0.5f));
}

[shader("fragment")]
float4 icbArgumentBufferSmokeFragment(
    SmokeVertexOutput input,
    ParameterBlock<IcbTextureArguments> arguments) : SV_Target0
{
    return arguments.texture.Sample(arguments.sampler, float2(0.5f, 0.5f));
}

[shader("vertex")]
SmokeVertexOutput depthWriteSmokeVertex(uint vertexId : SV_VertexID)
{
    const float2 positions[3] = {
        float2(-1.0f, -1.0f),
        float2( 3.0f, -1.0f),
        float2(-1.0f,  3.0f),
    };
    SmokeVertexOutput output;
    output.position = float4(positions[vertexId], 0.5f, 1.0f);
    return output;
}

[shader("vertex")]
SmokeVertexOutput depthRejectSmokeVertex(uint vertexId : SV_VertexID)
{
    const float2 positions[3] = {
        float2(-1.0f, -1.0f),
        float2( 3.0f, -1.0f),
        float2(-1.0f,  3.0f),
    };
    SmokeVertexOutput output;
    output.position = float4(positions[vertexId], 0.25f, 1.0f);
    return output;
}

[shader("fragment")]
float4 depthWriteSmokeFragment(SmokeVertexOutput input) : SV_Target0
{
    return float4(1.0f, 0.0f, 0.0f, 1.0f);
}

[shader("fragment")]
float4 depthRejectSmokeFragment(SmokeVertexOutput input) : SV_Target0
{
    return float4(0.0f, 1.0f, 0.0f, 1.0f);
}

[shader("fragment")]
float4 depthSampleSmokeFragment(
    SmokeVertexOutput input,
    Texture2D<float> depthTexture) : SV_Target0
{
    const uint2 coordinate = uint2(input.position.xy);
    const float depth = depthTexture.Load(int3(coordinate, 0));
    return float4(depth, depth, depth, 1.0f);
}

[numthreads(1, 1, 1)]
[shader("compute")]
void writeIndexedIndirectSmoke(
    RWStructuredBuffer<uint> indirectArguments,
    uint3 dispatchThreadId : SV_DispatchThreadID)
{
    [ForceUnroll]
    for(uint command = 1u; command < 3u; ++command)
    {
        const uint base = command * 5u;
        indirectArguments[base + 0u] = 3u;
        indirectArguments[base + 1u] = 1u;
        indirectArguments[base + 2u] = 1u;
        indirectArguments[base + 3u] = 2u;
        indirectArguments[base + 4u] = 7u;
    }
}

[numthreads(1, 1, 1)]
[shader("compute")]
void argumentTableSnapshotSmoke(
    StructuredBuffer<uint> snapshotSource,
    RWStructuredBuffer<uint> snapshotDestination,
    uint3 dispatchThreadId : SV_DispatchThreadID)
{
    snapshotDestination[0] = snapshotSource[0];
}
