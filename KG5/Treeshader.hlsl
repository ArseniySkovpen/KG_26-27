// TreeShader.hlsl
cbuffer TreeCB : register(b0)
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
    float3 LocalPos : POSITION;
    float3 Normal : NORMAL;
    float MatId : TEXCOORD;
    float3 WorldPos : INST_POS;
    float Height : INST_HEIGHT;
    float Width : INST_WIDTH;
    float3 Color : INST_COLOR;
};

struct VSOutput
{
    float4 PosH : SV_POSITION;
    float3 Color : COLOR;
};

VSOutput VSMain(VSInput vin)
{
    float4 baseV = mul(float4(vin.WorldPos, 1.f), gView);
    baseV.x += vin.LocalPos.x * vin.Width;
    baseV.y += vin.LocalPos.y * vin.Height;

    VSOutput vout;
    vout.PosH = mul(baseV, gProj);

    if (vin.MatId < 0.5f)
        vout.Color = float3(0.38f, 0.22f, 0.08f);
    else
        vout.Color = vin.Color;

    return vout;
}

float4 PSMain(VSOutput pin) : SV_Target
{
    return float4(pin.Color, 1.f);
}
