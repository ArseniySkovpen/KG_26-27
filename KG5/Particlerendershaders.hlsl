// ============================================================
// ParticleRenderShaders.hlsl
// VS reads particle by SV_VertexID from SRV.
// GS expands point to camera-facing billboard.
// PS opaque, discard outside circle.
// ============================================================

struct Particle
{
    float3 Position;
    float Life;
    float3 Velocity;
    float Size;
    float4 Color;
};

StructuredBuffer<Particle> Particles : register(t0);

cbuffer ParticleRenderCB : register(b0)
{
    float4x4 gView;
    float4x4 gProj;
    float3 gEyePos;
    float gPad0;
};

struct VSOutput
{
    float3 WorldPos : POSITION;
    float Size : PSIZE;
    float4 Color : COLOR;
};

struct GSOutput
{
    float4 PosH : SV_POSITION;
    float2 UV : TEXCOORD;
    float4 Color : COLOR;
};

VSOutput VSMain(uint vid : SV_VertexID)
{
    Particle p = Particles[vid];
    VSOutput o;
    o.WorldPos = p.Position;
    o.Size = max(p.Size, 0.0001f);
    o.Color = p.Color;
    return o;
}

[maxvertexcount(4)]
void GSMain(point VSOutput input[1], inout TriangleStream<GSOutput> stream)
{
    float3 c = input[0].WorldPos;
    float halfSize = input[0].Size * 0.5f;
    float4 col = input[0].Color;

    float3 look = normalize(gEyePos - c);
    float3 right = normalize(cross(float3(0, 1, 0), look));
    float3 up = cross(look, right);

    float4x4 vp = mul(gView, gProj);

    float3 v0 = c + (-right - up) * halfSize;
    float3 v1 = c + (right - up) * halfSize;
    float3 v2 = c + (-right + up) * halfSize;
    float3 v3 = c + (right + up) * halfSize;

    GSOutput o;
    o.Color = col;

    o.PosH = mul(float4(v0, 1.0f), vp);
    o.UV = float2(0, 1);
    stream.Append(o);
    o.PosH = mul(float4(v1, 1.0f), vp);
    o.UV = float2(1, 1);
    stream.Append(o);
    o.PosH = mul(float4(v2, 1.0f), vp);
    o.UV = float2(0, 0);
    stream.Append(o);
    o.PosH = mul(float4(v3, 1.0f), vp);
    o.UV = float2(1, 0);
    stream.Append(o);
}

float4 PSMain(GSOutput pin) : SV_Target
{
    float2 p = pin.UV * 2.0f - 1.0f;
    float r = length(p);
    if (r > 1.0f)
        discard;

    float glow = 1.0f - smoothstep(0.15f, 1.0f, r);
    float3 col = pin.Color.rgb * (0.25f + 0.85f * glow);

    if (pin.Color.a < 0.05f)
        discard;

    return float4(col, 1.0f);
}
