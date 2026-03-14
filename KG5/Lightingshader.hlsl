#define MAX_POINT_LIGHTS 16
#define MAX_SPOT_LIGHTS   4

struct PointLight
{
    float4 Position;
    float4 Color;
};
struct SpotLight
{
    float4 Position;
    float4 Direction;
    float4 Color;
};

cbuffer LightingCB : register(b0)
{
    float4 gEyePos;
    float4 gAmbientColor;
    float4 gDirLightDir;
    float4 gDirLightColor;
    PointLight gPointLights[MAX_POINT_LIGHTS];
    SpotLight gSpotLights[MAX_SPOT_LIGHTS];
    int gNumPointLights;
    int gNumSpotLights;
    int gPad0, gPad1;
};

Texture2D gAlbedoMap : register(t0);
Texture2D gNormalMap : register(t1);
Texture2D gPositionMap : register(t2);
SamplerState gSampler : register(s0);

struct VSOutput
{
    float4 PosH : SV_POSITION;
    float2 UV : TEXCOORD;
};

VSOutput VSMain(uint id : SV_VertexID)
{
    float2 uv = float2((id << 1) & 2, id & 2);
    VSOutput o;
    o.UV = uv;
    o.PosH = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float3 PhongBRDF(float3 N, float3 V, float3 L,
                  float3 albedo, float specI, float specP,
                  float3 lColor, float lIntensity)
{
    float NdotL = max(dot(N, L), 0.0f);
    float3 R = reflect(-L, N);
    float RdotV = max(dot(R, V), 0.0f);
    return (NdotL * albedo * lColor
          + pow(RdotV, max(specP, 1.0f)) * specI * lColor) * lIntensity;
}

float3 CalcDirectional(float3 N, float3 V, float3 albedo, float specI, float specP)
{
    if (gDirLightColor.w <= 0.0f)
        return 0;
    float3 L = normalize(-gDirLightDir.xyz);
    return PhongBRDF(N, V, L, albedo, specI, specP, gDirLightColor.xyz, gDirLightColor.w);
}

float3 CalcPoint(PointLight light, float3 N, float3 V, float3 pos,
                  float3 albedo, float specI, float specP)
{
    float3 toLight = light.Position.xyz - pos;
    float dist = length(toLight);
    float radius = light.Position.w;
    if (dist > radius)
        return 0;
    float3 L = toLight / dist;
    float att = 1.0f - smoothstep(0.0f, radius, dist);
    return PhongBRDF(N, V, L, albedo, specI, specP, light.Color.xyz, light.Color.w) * att;
}

float3 CalcSpot(SpotLight light, float3 N, float3 V, float3 pos,
                 float3 albedo, float specI, float specP)
{
    float3 toLight = light.Position.xyz - pos;
    float dist = length(toLight);
    float3 L = toLight / dist;
    float cosAngle = dot(-L, normalize(light.Direction.xyz));
    float innerCos = light.Position.w;
    float outerCos = light.Direction.w;
    float spot = smoothstep(outerCos, innerCos, cosAngle);
    if (spot <= 0)
        return 0;
    float att = 1.0f / (1.0f + 0.002f * dist);
    return PhongBRDF(N, V, L, albedo, specI, specP, light.Color.xyz, light.Color.w) * att * spot;
}

float4 PSMain(VSOutput pin) : SV_Target
{
    float4 albedoData = gAlbedoMap.Sample(gSampler, pin.UV);
    float4 normalData = gNormalMap.Sample(gSampler, pin.UV);
    float3 pos = gPositionMap.Sample(gSampler, pin.UV).rgb;

    if (length(pos) < 0.001f)
        return float4(0, 0, 0, 1);

    float3 albedo = albedoData.rgb;
    float specI = albedoData.a;
    float3 N = normalize(normalData.rgb * 2.0f - 1.0f);
    float specP = max(normalData.a, 1.0f);
    float3 V = normalize(gEyePos.xyz - pos);

    float3 light = gAmbientColor.rgb * albedo;
    light += CalcDirectional(N, V, albedo, specI, specP);
    for (int i = 0; i < gNumPointLights; ++i)
        light += CalcPoint(gPointLights[i], N, V, pos, albedo, specI, specP);
    for (int j = 0; j < gNumSpotLights; ++j)
        light += CalcSpot(gSpotLights[j], N, V, pos, albedo, specI, specP);

    return float4(light, 1.0f);
}
