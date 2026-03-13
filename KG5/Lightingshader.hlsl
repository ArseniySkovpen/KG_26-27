// ============================================================================
// LightingShader.hlsl — Lighting Pass
//
// Рисуем один полноэкранный треугольник (3 вершины, нет VB).
// Для каждого пикселя читаем GBuffer и считаем освещение от:
//   - Directional light (направленный, как солнце)
//   - Point lights (точечные, затухают по расстоянию)
//   - Spot lights (конические, как прожектор)
// ============================================================================

#define MAX_POINT_LIGHTS 16
#define MAX_SPOT_LIGHTS   4

// ---- Структуры источников света (должны совпадать с C++) ----
struct PointLight
{
    float4 Position; // xyz = позиция, w = радиус
    float4 Color; // xyz = цвет RGB, w = интенсивность
};

struct SpotLight
{
    float4 Position; // xyz = позиция,     w = cos(innerAngle)
    float4 Direction; // xyz = направление, w = cos(outerAngle)
    float4 Color; // xyz = цвет RGB,    w = интенсивность
};

cbuffer LightingCB : register(b0)
{
    float4 gEyePos;
    float4 gAmbientColor;
    float4 gDirLightDir; // w не используется
    float4 gDirLightColor; // w = интенсивность (0 = выключен)

    PointLight gPointLights[MAX_POINT_LIGHTS];
    SpotLight gSpotLights[MAX_SPOT_LIGHTS];

    int gNumPointLights;
    int gNumSpotLights;
    int gPad0, gPad1;
};

// GBuffer текстуры
Texture2D gAlbedoMap : register(t0); // RT0: цвет + specIntensity
Texture2D gNormalMap : register(t1); // RT1: нормаль + specPower
Texture2D gPositionMap : register(t2); // RT2: мировая позиция

SamplerState gSampler : register(s0); // POINT CLAMP

// ---- Vertex/Pixel структуры ----
struct VSOutput
{
    float4 PosH : SV_POSITION;
    float2 UV : TEXCOORD;
};

// ============================================================================
// Vertex Shader — fullscreen triangle trick
// Три вершины с SV_VertexID 0,1,2 покрывают весь экран одним треугольником.
// Это эффективнее квада: нет лишних пикселей по диагонали.
// ============================================================================
VSOutput VSMain(uint vertexID : SV_VertexID)
{
    // vertexID 0 → UV(0,0), 1 → UV(2,0), 2 → UV(0,2)
    // NDC X = UV.x * 2 - 1, NDC Y = -(UV.y * 2 - 1)
    float2 uv = float2((vertexID << 1) & 2, vertexID & 2);
    VSOutput o;
    o.UV = uv;
    o.PosH = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

// ============================================================================
// Вспомогательные функции освещения
// N = нормаль поверхности (нормализована)
// V = направление НА камеру (нормализовано)
// L = направление НА источник (нормализовано)
// ============================================================================

// Phong модель для одного источника
float3 PhongBRDF(float3 N, float3 V, float3 L,
                  float3 albedo, float specIntensity, float specPow,
                  float3 lightColor, float lightIntensity)
{
    // Diffuse: N·L (Lambert)
    float NdotL = max(dot(N, L), 0.0f);
    float3 diffuse = NdotL * albedo * lightColor;

    // Specular: (R·V)^n (Phong)
    float3 R = reflect(-L, N);
    float RdotV = max(dot(R, V), 0.0f);
    float3 specular = pow(RdotV, max(specPow, 1.0f)) * specIntensity * lightColor;

    return (diffuse + specular) * lightIntensity;
}

// ---- Directional Light ----
// Нет затухания — свет одинаковый по всей сцене (как солнце)
float3 CalcDirectional(float3 N, float3 V, float3 pos,
                        float3 albedo, float specIntensity, float specPow)
{
    if (gDirLightColor.w <= 0.0f)
        return 0; // выключен

    float3 L = normalize(-gDirLightDir.xyz);
    return PhongBRDF(N, V, L, albedo, specIntensity, specPow,
                     gDirLightColor.xyz, gDirLightColor.w);
}

// ---- Point Light ----
// Затухает по квадрату расстояния. Radius задаёт максимальный радиус влияния.
float3 CalcPoint(PointLight light, float3 N, float3 V, float3 pos,
                  float3 albedo, float specIntensity, float specPow)
{
    float3 toLight = light.Position.xyz - pos;
    float dist = length(toLight);
    float radius = light.Position.w;

    // За пределами радиуса — нет света
    if (dist > radius)
        return 0;

    float3 L = toLight / dist;

    // Плавное затухание: 1 у источника → 0 на границе радиуса
    float attenuation = 1.0f - smoothstep(0.0f, radius, dist);

    return PhongBRDF(N, V, L, albedo, specIntensity, specPow,
                     light.Color.xyz, light.Color.w) * attenuation;
}

// ---- Spot Light ----
// Конический свет как прожектор. Мягкий переход между innerAngle и outerAngle.
float3 CalcSpot(SpotLight light, float3 N, float3 V, float3 pos,
                 float3 albedo, float specIntensity, float specPow)
{
    float3 toLight = light.Position.xyz - pos;
    float dist = length(toLight);
    float3 L = toLight / dist;

    // Угол между направлением света и вектором к пикселю
    float cosAngle = dot(-L, normalize(light.Direction.xyz));
    float innerCos = light.Position.w;
    float outerCos = light.Direction.w;

    // smoothstep: 0 за конусом, плавно нарастает до 1 внутри innerAngle
    float spotFactor = smoothstep(outerCos, innerCos, cosAngle);
    if (spotFactor <= 0)
        return 0;

    // Простое линейное затухание по расстоянию
    float attenuation = 1.0f / (1.0f + 0.002f * dist);

    return PhongBRDF(N, V, L, albedo, specIntensity, specPow,
                     light.Color.xyz, light.Color.w) * attenuation * spotFactor;
}

// ============================================================================
// Pixel Shader
// ============================================================================
float4 PSMain(VSOutput pin) : SV_Target
{
    // Читаем GBuffer
    float4 albedoData = gAlbedoMap.Sample(gSampler, pin.UV);
    float4 normalData = gNormalMap.Sample(gSampler, pin.UV);
    float4 positionData = gPositionMap.Sample(gSampler, pin.UV);

    // Декодируем данные из GBuffer
    float3 albedo = albedoData.rgb;
    float specIntensity = albedoData.a; // из RT0 alpha
    float3 N = normalize(normalData.rgb * 2.0f - 1.0f); // из [0,1] → [-1,1]
    float specPow = max(normalData.a, 1.0f); // из RT1 alpha
    float3 pos = positionData.rgb;

    // Если позиция = 0 значит пиксель не покрыт геометрией (небо/фон)
    if (length(pos) < 0.001f)
        return float4(0, 0, 0, 1);

    float3 V = normalize(gEyePos.xyz - pos);

    // --- Суммируем вклад всех источников ---
    float3 lighting = gAmbientColor.rgb * albedo;

    // Directional
    lighting += CalcDirectional(N, V, pos, albedo, specIntensity, specPow);

    // Point lights
    for (int i = 0; i < gNumPointLights; ++i)
        lighting += CalcPoint(gPointLights[i], N, V, pos, albedo, specIntensity, specPow);

    // Spot lights
    for (int j = 0; j < gNumSpotLights; ++j)
        lighting += CalcSpot(gSpotLights[j], N, V, pos, albedo, specIntensity, specPow);

    return float4(lighting, 1.0f);
}
