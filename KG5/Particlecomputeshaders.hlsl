// ============================================================
// ParticleComputeShaders.hlsl
// Append/Consume StructuredBuffer + GPU physics.
// ============================================================

struct Particle
{
    float3 Position;
    float Life;
    float3 Velocity;
    float Size;
    float4 Color;
};

cbuffer ParticleCB : register(b0)
{
    float gDeltaTime;
    float gTotalTime;
    uint gEmitCount;
    uint gSrcCount;
    float4 gEmitterPos;
    float3 gGravity;
    float gPad0;
};

ConsumeStructuredBuffer<Particle> SrcParticles : register(u0);
AppendStructuredBuffer<Particle> DstParticles : register(u1);

// ----- RNG -----
uint WangHash(uint s)
{
    s = (s ^ 61u) ^ (s >> 16u);
    s *= 9u;
    s = s ^ (s >> 4u);
    s *= 0x27d4eb2du;
    s = s ^ (s >> 15u);
    return s;
}

float Rnd(inout uint s)
{
    s = WangHash(s);
    return (float) s * (1.0f / 4294967295.0f);
}

// ----- UpdateCS -----
[numthreads(64, 1, 1)]
void UpdateCS(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= gSrcCount)
        return;

    Particle p = SrcParticles.Consume();

    p.Velocity += gGravity * gDeltaTime;
    p.Position += p.Velocity * gDeltaTime;
    p.Life -= gDeltaTime;

    // Color fades over life - bright yellow -> red
    float fade = saturate(p.Life * 0.4f);
    p.Color = float4(1.0f, 0.85f * fade, 0.4f * fade, 1.0f);

    if (p.Life > 0.0f)
        DstParticles.Append(p);
}

// ----- EmitCS -----
[numthreads(64, 1, 1)]
void EmitCS(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= gEmitCount)
        return;

    uint seed = WangHash(DTid.x * 73856093u
                       ^ asuint(gTotalTime + (float) DTid.x * 0.001f) * 19349663u
                       ^ 83492791u);

    Particle p;
    p.Position = gEmitterPos.xyz;

    // Cone upwards
    float u = Rnd(seed);
    float v = Rnd(seed);
    float theta = 6.2831853f * u;
    float phi = acos(0.5f + v * 0.4f);

    float3 dir;
    dir.x = sin(phi) * cos(theta);
    dir.y = cos(phi);
    dir.z = sin(phi) * sin(theta);

    // Strong upwards velocity, modest gravity = beautiful fountain
    float speed = 150.0f + Rnd(seed) * 50.0f;
    p.Velocity = dir * speed;
    p.Life = 2.5f;
    p.Size = 4.0f + Rnd(seed) * 3.0f;
    p.Color = float4(1.0f, 0.85f, 0.4f, 1.0f);

    DstParticles.Append(p);
}
