// InstanceShader.hlsl
// Простой форвардный Phong-шейдер для инстансинга объектов.
// Мировая матрица передаётся как per-instance vertex data (4 float4 строки).

cbuffer FrameCB : register(b0)
{
    float4x4 gView;
    float4x4 gProj;
    float3 gEyePos;
    float gPad0;
    float3 gLightDir;
    float gPad1; // направление К источнику (нормализовано)
    float3 gLightColor;
    float gPad2;
    float3 gAmbient;
    float gPad3;
};

struct VSInput
{
    // Per-vertex (Stream 0)
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    // Per-instance (Stream 1)
    float4 World0 : INST_WORLD0; // строка 0 мировой матрицы
    float4 World1 : INST_WORLD1;
    float4 World2 : INST_WORLD2;
    float4 World3 : INST_WORLD3;
    float4 ColorPad : INST_COLOR0; // rgb = цвет, w = не используется
};

struct VSOutput
{
    float4 PosH : SV_POSITION;
    float3 PosW : POSITION;
    float3 NormW : NORMAL;
    float3 Color : COLOR;
};

VSOutput VSMain(VSInput vin)
{
    // Собираем мировую матрицу из строк (row-major)
    float4x4 world = float4x4(vin.World0, vin.World1, vin.World2, vin.World3);

    float4 posW = mul(float4(vin.Position, 1.0f), world);

    VSOutput vout;
    vout.PosW = posW.xyz;
    vout.PosH = mul(mul(posW, gView), gProj);
    // Нормаль: для равномерного масштабирования можно использовать ту же матрицу
    vout.NormW = normalize(mul(vin.Normal, (float3x3) world));
    vout.Color = vin.ColorPad.rgb;
    return vout;
}

float4 PSMain(VSOutput pin) : SV_Target
{
    float3 N = normalize(pin.NormW);
    float3 L = gLightDir; // уже нормализован
    float3 V = normalize(gEyePos - pin.PosW);
    float3 R = reflect(-L, N);

    float3 ambient = gAmbient * pin.Color;
    float3 diffuse = max(dot(N, L), 0.0f) * gLightColor * pin.Color;
    float3 specular = pow(max(dot(R, V), 0.0f), 32.0f) * 0.25f * gLightColor;

    return float4(saturate(ambient + diffuse + specular), 1.0f);
}
