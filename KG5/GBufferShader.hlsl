cbuffer GBufferCB : register(b0)
{
    float4x4 gWorld;
    float4x4 gView;
    float4x4 gProj;
    float4x4 gWorldInvTranspose;
    float4 gMaterialDiffuse;
    float4 gMaterialSpecular; 
    int gHasTexture;
    float gTexTilingX;
    float gTexTilingY;
    float gTotalTime;
    float gTexScrollX;
    float gTexScrollY;
    float2 gPad;
};

Texture2D gDiffuseMap : register(t0);
SamplerState gSampler : register(s0);

struct VSInput
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float2 TexCoord : TEXCOORD;
};

struct VSOutput
{
    float4 PosH : SV_POSITION;
    float3 PosW : POSITION;
    float3 NormalW : NORMAL;
    float2 TexCoord : TEXCOORD;
};

struct PSOutput
{
    float4 Albedo : SV_Target0;
    float4 Normal : SV_Target1;
    float4 Position : SV_Target2;
};

VSOutput VSMain(VSInput vin)
{
    VSOutput vout;
    float4 posW = mul(float4(vin.Position, 1.0f), gWorld);
    vout.PosW = posW.xyz;
    vout.PosH = mul(mul(posW, gView), gProj);
    vout.NormalW = mul(vin.Normal, (float3x3) gWorldInvTranspose);

    float2 uv = vin.TexCoord * float2(gTexTilingX, gTexTilingY);
    float scaleY = 1.0f + sin(gTotalTime * gTexScrollX) * gTexScrollY;
    uv.y *= scaleY;
    vout.TexCoord = uv;
    return vout;
}

PSOutput PSMain(VSOutput pin)
{
    PSOutput pout;
    float4 albedo = gHasTexture
        ? gDiffuseMap.Sample(gSampler, pin.TexCoord)
        : gMaterialDiffuse;
    float3 normal = normalize(pin.NormalW);
    pout.Albedo = float4(albedo.rgb, gMaterialSpecular.x);
    pout.Normal = float4(normal * 0.5f + 0.5f, gMaterialSpecular.w);
    pout.Position = float4(pin.PosW, 1.0f);
    return pout;
}
