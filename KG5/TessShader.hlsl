//тесселяция
cbuffer TessCB : register(b0)
{
    float4x4 gWorld;
    float4x4 gView;
    float4x4 gProj;
    float4x4 gWorldInvTranspose;
    float3 gEyePos;
    float gDisplacementScale;
    float gMinDist;
    float gMaxDist;
    float gMinTessFactor;
    float gMaxTessFactor;
    float gTotalTime;
    float3 gPad;
};

Texture2D gDisplacementMap : register(t0);
Texture2D gNormalMap : register(t1);
Texture2D gColorMap : register(t2);
SamplerState gSampler : register(s0);

struct VSInput
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float2 TexCoord : TEXCOORD;
};

struct VSOutput
{
    float3 PosW : POSITION;
    float3 NormalW : NORMAL;
    float2 TexCoord : TEXCOORD;
};

struct PatchTess
{
    float EdgeTess[3] : SV_TessFactor;
    float InsideTess[1] : SV_InsideTessFactor;
};

struct DSOutput
{
    float4 PosH : SV_POSITION;
    float3 PosW : POSITION;
    float3 NormalW : NORMAL;
    float2 TexCoord : TEXCOORD;
};

VSOutput VSMain(VSInput vin)
{
    VSOutput vout;
    vout.PosW = mul(float4(vin.Position, 1.0f), gWorld).xyz;
    vout.NormalW = mul(vin.Normal, (float3x3) gWorldInvTranspose);
    vout.TexCoord = vin.TexCoord;
    return vout;
}

// вызывается на каждый треугольник — вычисляет насколько сильно разбивать
PatchTess PatchHS(InputPatch<VSOutput, 3> patch, uint patchID : SV_PrimitiveID)
{
    PatchTess pt;

    float3 center = (patch[0].PosW + patch[1].PosW + patch[2].PosW) / 3.0f;
    float dist = distance(center, gEyePos);

    float t = saturate((dist - gMinDist) / (gMaxDist - gMinDist));
    float tess = lerp(gMaxTessFactor, gMinTessFactor, t);

    pt.EdgeTess[0] = tess;
    pt.EdgeTess[1] = tess;
    pt.EdgeTess[2] = tess;
    pt.InsideTess[0] = tess;

    return pt;
}

// Основная функция HS - передача контрольных точек
[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("PatchHS")]
VSOutput HSMain(InputPatch<VSOutput, 3> patch,
                uint i : SV_OutputControlPointID,
                uint patchID : SV_PrimitiveID)
{
    return patch[i];
}

// Domain Shader — получает интерполированную вершину, применяет displacement
[domain("tri")]
DSOutput DSMain(PatchTess patchTess,
                float3 bary : SV_DomainLocation,
                const OutputPatch<VSOutput, 3> patch)
{
    DSOutput dout;

    float3 posW = bary.x * patch[0].PosW
                + bary.y * patch[1].PosW
                + bary.z * patch[2].PosW;

    float3 normalW = bary.x * patch[0].NormalW
                   + bary.y * patch[1].NormalW
                   + bary.z * patch[2].NormalW;

    float2 uv = bary.x * patch[0].TexCoord
              + bary.y * patch[1].TexCoord
              + bary.z * patch[2].TexCoord;

    normalW = normalize(normalW);

    float height = gDisplacementMap.SampleLevel(gSampler, uv, 0).r;

    // Синусоидальная волна, бегущая вдоль X с небольшим наклоном по Z
    float wave = sin(posW.x * 0.006f + posW.z * 0.003f + gTotalTime * 2.0f)
               * 0.5f + 0.5f; // [0..1]

    posW += normalW * (height * wave - 0.5f) * gDisplacementScale;

    dout.PosW = posW;
    dout.NormalW = normalW;
    dout.TexCoord = uv;
    dout.PosH = mul(mul(float4(posW, 1.0f), gView), gProj);

    return dout;
}

// Нормал мап и освещение
float4 PSMain(DSOutput pin) : SV_Target
{
    float3 baseColor = gColorMap.Sample(gSampler, pin.TexCoord).rgb;

    float3 normalSample = gNormalMap.Sample(gSampler, pin.TexCoord).rgb;
    normalSample = normalSample * 2.0f - 1.0f;

    float3 N = normalize(pin.NormalW + normalSample * 0.5f);
    float3 L = normalize(float3(0.5f, 1.0f, 0.3f));
    float3 V = normalize(gEyePos - pin.PosW);
    float3 R = reflect(-L, N);

    float3 ambient = float3(0.1f, 0.1f, 0.15f) * baseColor;
    float3 diffuse = max(dot(N, L), 0.0f) * float3(1.0f, 0.92f, 0.75f) * baseColor;
    float3 specular = pow(max(dot(R, V), 0.0f), 32.0f) * 0.3f;

    return float4(ambient + diffuse + specular, 1.0f);
}
