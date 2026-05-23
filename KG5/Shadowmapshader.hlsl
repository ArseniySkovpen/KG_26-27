// ============================================================
// ShadowMapShader.hlsl
// Depth-only VS for cascaded shadow maps.
// No pixel shader - we only need depth.
// ============================================================

cbuffer ShadowVsCB : register(b0)
{
    float4x4 gWorldViewProj;
};

struct VSInput
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float2 TexCoord : TEXCOORD;
};

float4 VSMain(VSInput vin) : SV_POSITION
{
    return mul(float4(vin.Position, 1.0f), gWorldViewProj);
}
