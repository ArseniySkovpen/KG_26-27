// BillboardShader.hlsl
cbuffer BillboardCB : register(b0)
{
    float4x4 gView;
    float4x4 gProj;
    float3 gEyePos;
    float gPad0;
    float3 gAmbient;
    float gPad1;
    float3 gSunDir;
    float gPad2;
    float3 gSunColor;
    float gPad3;
};

struct VSInput
{
    float2 CornerOffset : POSITION;
    float2 UV : TEXCOORD;
    float3 WorldPos : INST_POS;
    float Height : INST_HEIGHT;
    float Width : INST_WIDTH;
    float3 Color : INST_COLOR;
};

struct VSOutput
{
    float4 PosH : SV_POSITION;
    float2 UV : TEXCOORD;
    float3 Color : COLOR;
};

VSOutput VSMain(VSInput vin)
{
    float3 center = vin.WorldPos + float3(0, vin.Height * 0.5f, 0);
    float4 centerV = mul(float4(center, 1.0f), gView);

    centerV.x += vin.CornerOffset.x * vin.Width * 0.5f;
    centerV.y += vin.CornerOffset.y * vin.Height * 0.5f;

    VSOutput vout;
    vout.PosH = mul(centerV, gProj);
    vout.UV = vin.UV;
    vout.Color = vin.Color;
    return vout;
}

float4 PSMain(VSOutput pin) : SV_Target
{
    float2 uv = pin.UV;

    float2 crownUV = float2(uv.x, (uv.y - 0.25f) / 0.75f);
    float2 crownPos = crownUV * 2.0f - 1.0f;
    float crownR = length(crownPos);

    bool isTrunk = (uv.y < 0.25f) && (abs(uv.x - 0.5f) < 0.12f);
    bool isCrown = (uv.y >= 0.25f) && (crownR < 1.0f);

    if (!isTrunk && !isCrown)
        discard;

    float3 color;
    if (isTrunk)
        color = float3(0.38f, 0.22f, 0.08f);
    else
    {
        float edge = 1.0f - smoothstep(0.6f, 1.0f, crownR);
        color = pin.Color * edge;
    }

    float3 N = float3(0, 0, -1);
    float NdotL = max(dot(N, gSunDir), 0.0f);
    float3 lit = gAmbient * color + NdotL * gSunColor * color;

    return float4(saturate(lit), 1.0f);
}
