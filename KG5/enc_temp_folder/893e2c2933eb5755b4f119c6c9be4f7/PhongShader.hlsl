cbuffer ConstantBuffer : register(b0)
{
    float4x4 gWorld;
    float4x4 gView;
    float4x4 gProj;
    float4x4 gWorldInvTranspose;

    float4 gLightDir;
    float4 gLightColor;
    float4 gAmbientColor;
    float4 gEyePos;
    float4 gMaterialDiffuse;
    float4 gMaterialSpecular;
    float gSpecularPower;
    float gTotalTime; 
    float2 gTexTiling;
    float2 gTexScroll;
    float2 gPad;
    int gHasTexture; 
    int gPad2;
};

Texture2D gDiffuseMap : register(t0);
SamplerState gSampler : register(s0);

struct VSInput
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float2 TexCoord : TEXCOORD;
};

struct PSInput
{
    float4 PositionH : SV_POSITION;
    float3 PositionW : TEXCOORD0;
    float3 NormalW : TEXCOORD1;
    float2 TexCoord : TEXCOORD2;
};

PSInput VSMain(VSInput vin)
{
    PSInput vout;

    float4 posW = mul(float4(vin.Position, 1.0f), gWorld);
    vout.PositionW = posW.xyz;
    vout.PositionH = mul(mul(posW, gView), gProj);
    vout.NormalW = mul(vin.Normal, (float3x3) gWorldInvTranspose);

    // Тайлинг
    float2 uv = vin.TexCoord * gTexTiling;
    // Скейлинг по вертикали: Y растягивается со временем
    float scaleY = 1.0f + sin(gTotalTime * gTexScroll.x) * gTexScroll.y;
    uv.y *= scaleY;
    vout.TexCoord = uv;

    return vout;
}

float4 PSMain(PSInput pin) : SV_TARGET
{
    float3 N = normalize(pin.NormalW);
    float3 L = normalize(-gLightDir.xyz);
    float3 V = normalize(gEyePos.xyz - pin.PositionW);
    float3 R = reflect(-L, N);

    float4 baseColor = gMaterialDiffuse;
    if (gHasTexture)
        baseColor *= gDiffuseMap.Sample(gSampler, pin.TexCoord);

    float3 ambient = gAmbientColor.rgb * baseColor.rgb;
    float3 diffuse = max(dot(N, L), 0.0f) * gLightColor.rgb * baseColor.rgb;
    float3 specular = pow(max(dot(R, V), 0.0f), gSpecularPower)
                    * gLightColor.rgb * gMaterialSpecular.rgb;

    return float4(ambient + diffuse + specular, baseColor.a);
}
