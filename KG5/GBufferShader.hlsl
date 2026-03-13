// ============================================================================
// GBufferShader.hlsl — Geometry Pass
// Записывает данные геометрии в три GBuffer текстуры:
//   SV_Target0 (Albedo)   : RGB = цвет, A = specular intensity
//   SV_Target1 (Normal)   : RGB = нормаль (world space, закодирована в [0,1]), A = specPow
//   SV_Target2 (Position) : RGB = позиция в мировых координатах
// ============================================================================

cbuffer GBufferCB : register(b0)
{
    float4x4 gWorld;
    float4x4 gView;
    float4x4 gProj;
    float4x4 gWorldInvTranspose;
    float4 gMaterialDiffuse;
    float4 gMaterialSpecular; // w = specular power
    // int(4) + float(4) + float(4) + float(4) = 16 байт, один регистр
    int gHasTexture;
    float gTexTilingX;
    float gTexTilingY;
    float gTotalTime;
    // float(4) + float(4) + pad(4) + pad(4) = 16 байт, один регистр
    float gTexScrollX;
    float gTexScrollY;
    float2 gPad;
};

Texture2D gDiffuseMap : register(t0);
SamplerState gSampler : register(s0);

// ---- Vertex Shader Input/Output ----
struct VSInput
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float2 TexCoord : TEXCOORD;
};

struct VSOutput
{
    float4 PosH : SV_POSITION; // позиция в clip space (для растеризатора)
    float3 PosW : POSITION; // позиция в world space (для GBuffer)
    float3 NormalW : NORMAL; // нормаль в world space
    float2 TexCoord : TEXCOORD;
};

// ---- Output структура для MRT (Multiple Render Targets) ----
struct PSOutput
{
    float4 Albedo : SV_Target0; // цвет + specular intensity
    float4 Normal : SV_Target1; // нормаль + specular power
    float4 Position : SV_Target2; // мировые координаты
};

// ============================================================================
// Vertex Shader
// ============================================================================
VSOutput VSMain(VSInput vin)
{
    VSOutput vout;

    // Переводим позицию: Object → World → View → Clip
    float4 posW = mul(float4(vin.Position, 1.0f), gWorld);
    vout.PosW = posW.xyz;
    float4 posV = mul(posW, gView);
    vout.PosH = mul(posV, gProj);

    // WorldInvTranspose корректно трансформирует нормали при наличии масштабирования
    vout.NormalW = mul(vin.Normal, (float3x3) gWorldInvTranspose);

    // UV: тайлинг
    float2 uv = vin.TexCoord * float2(gTexTilingX, gTexTilingY);
    // Вертикальный скейл: gTexScrollX = скорость синуса, gTexScrollY = амплитуда
    float scaleY = 1.0f + sin(gTotalTime * gTexScrollX) * gTexScrollY;
    uv.y *= scaleY;
    vout.TexCoord = uv;

    return vout;
}

// ============================================================================
// Pixel Shader — записывает в три GBuffer RT
// ============================================================================
PSOutput PSMain(VSOutput pin)
{
    PSOutput pout;

    // Базовый цвет: из текстуры или из материала
    float4 albedo = gHasTexture
        ? gDiffuseMap.Sample(gSampler, pin.TexCoord)
        : gMaterialDiffuse;

    // Нормализуем нормаль (интерполяция при растеризации немного денормализует)
    float3 normal = normalize(pin.NormalW);

    // RT0: Albedo (RGB) + specular intensity в Alpha
    pout.Albedo = float4(albedo.rgb, gMaterialSpecular.x);

    // RT1: Нормаль закодирована из [-1,1] в [0,1] чтобы влезла в RGBA16F
    //      Alpha = specular power (чем больше — тем уже и резче блик)
    pout.Normal = float4(normal * 0.5f + 0.5f, gMaterialSpecular.w);

    // RT2: Точная мировая позиция пикселя (нужна для расчёта освещения)
    pout.Position = float4(pin.PosW, 1.0f);

    return pout;
}
