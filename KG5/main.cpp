#include "Window.h"
#include "RenderingSystem.h"
#include "Timer.h"
#include "InputDevice.h"
#include <cmath>
#include <vector>
#include <cstdlib>

class FPSCamera
{
public:
    XMFLOAT3 position = { 0.f, 150.f, -500.f };
    float    yaw = 0.f;
    float    pitch = -10.f;
    float    moveSpeed = 300.f;
    float    mouseSens = 0.15f;
    void Update(const InputDevice& input, float dt)
    {
        if (input.IsMouseDown(1))
        {
            auto [dx, dy] = input.GetMouseDelta();
            yaw += dx * mouseSens;
            pitch -= dy * mouseSens;
            if (pitch > 89.f) pitch = 89.f;
            if (pitch < -89.f) pitch = -89.f;
        }

        float yawRad = XMConvertToRadians(yaw);
        float pitchRad = XMConvertToRadians(pitch);

        XMFLOAT3 forward = {
            sinf(yawRad) * cosf(pitchRad),
            sinf(pitchRad),
            cosf(yawRad) * cosf(pitchRad)
        };

        XMVECTOR fwd = XMLoadFloat3(&forward);
        XMVECTOR worldUp = XMVectorSet(0, 1, 0, 0);
        XMVECTOR right = XMVector3Normalize(XMVector3Cross(fwd, worldUp));

        float speed = dt * moveSpeed;
        if (input.IsKeyDown(VK_SHIFT)) speed *= 5.f;

        XMVECTOR pos = XMLoadFloat3(&position);
        if (input.IsKeyDown('W')) pos = XMVectorAdd(pos, XMVectorScale(fwd, speed));
        if (input.IsKeyDown('S')) pos = XMVectorAdd(pos, XMVectorScale(fwd, -speed));
        if (input.IsKeyDown('D')) pos = XMVectorAdd(pos, XMVectorScale(right, speed));
        if (input.IsKeyDown('A')) pos = XMVectorAdd(pos, XMVectorScale(right, -speed));
        if (input.IsKeyDown('E')) pos = XMVectorAdd(pos, XMVectorScale(worldUp, speed));
        if (input.IsKeyDown('Q')) pos = XMVectorAdd(pos, XMVectorScale(worldUp, -speed));
        XMStoreFloat3(&position, pos);
        m_forward = forward;
    }

    void GetViewVectors(XMFLOAT3& eye, XMFLOAT3& target, XMFLOAT3& up) const
    {
        eye = position;
        target = { position.x + m_forward.x,
                   position.y + m_forward.y,
                   position.z + m_forward.z };
        up = { 0.f, 1.f, 0.f };
    }

private:
    XMFLOAT3 m_forward = { 0.f, 0.f, 1.f };
};

struct FallingLight
{
    XMFLOAT3 pos;
    XMFLOAT3 color;
    float    velY;
    float    radius;
    float    intensity;
    bool     landed;
};

class FallingStar
{
public:
    static constexpr float CEILING_Y = 430.f;
    static constexpr float FLOOR_Y = 5.f;
    static constexpr float GRAVITY = -25.f;
    static constexpr float SPAWN_INTERVAL = 0.5f;
    static constexpr int   MAX_LIGHTS = 13;
    static constexpr float SLOT_Z[2] = { 100.f, -100.f };
    void Update(float dt)
    {
        m_spawnTimer += dt;
        if (m_spawnTimer >= SPAWN_INTERVAL && (int)m_lights.size() < MAX_LIGHTS)
        {
            m_spawnTimer = 0.f;
            Spawn();
        }

        for (auto& l : m_lights)
        {
            if (l.landed) continue;

            l.velY += GRAVITY * dt;
            l.pos.y += l.velY * dt;

            if (l.pos.y <= FLOOR_Y)
            {
                l.pos.y = FLOOR_Y;
                l.landed = true;
                l.velY = 0.f;
            }
        }
    }

    void SubmitLights(RenderingSystem& rs) const
    {
        for (const auto& l : m_lights)
            rs.AddPointLight(l.pos, l.color, l.radius, l.intensity);
    }

private:
    void Spawn()
    {
        FallingLight l;
        float t = (float)rand() / RAND_MAX;
        l.pos.x = -380.f + t * 760.f;
        int   slot = rand() % 2;
        float jitter = ((float)rand() / RAND_MAX - 0.5f) * 30.f;
        l.pos.z = SLOT_Z[slot] + jitter;
        l.pos.y = CEILING_Y;
        l.velY = -5.f;
        l.landed = false;
        float r = 0.8f + (float)rand() / RAND_MAX * 0.2f;
        float g = 0.2f + (float)rand() / RAND_MAX * 0.5f;
        float b = 0.0f + (float)rand() / RAND_MAX * 0.15f;
        l.color = { r, g, b };
        l.radius = 300.f + (float)rand() / RAND_MAX * 300.f;
        l.intensity = 15.0f;
        m_lights.push_back(l);
    }
    std::vector<FallingLight> m_lights;
    float m_spawnTimer = 0.f;

};

class App
{
public:
    bool Init(HINSTANCE hInstance)
    {
        srand(12345);
        if (!m_window.Init(hInstance, 1280, 720, L"DX12 Deferred - Falling Lights"))
            return false;

        m_window.SetResizeCallback([this](int w, int h) {
            m_renderer.OnResize(w, h);
            });

        if (!m_renderer.Init(m_window.GetHWND(),
            m_window.GetWidth(),
            m_window.GetHeight()))
            return false;

        if (!m_renderer.LoadObj("sponza.obj"))
            MessageBoxA(nullptr, "Не удалось загрузить sponza.obj",
                "Ошибка", MB_OK | MB_ICONWARNING);

        m_renderer.SetTexTiling(1.0f, 1.0f);
        m_renderer.SetTexScroll(1.0f, 0.3f);
        m_timer.Reset();
        return true;
    }

    void Show(int nCmdShow) { m_window.Show(nCmdShow); }

    int Run()
    {
        MSG msg{};
        while (true)
        {
            while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                if (msg.message == WM_QUIT) return (int)msg.wParam;
                switch (msg.message)
                {
                case WM_KEYDOWN:     m_input.OnKeyDown(msg.wParam); break;
                case WM_KEYUP:       m_input.OnKeyUp(msg.wParam);   break;
                case WM_MOUSEMOVE:   m_input.OnMouseMove(LOWORD(msg.lParam), HIWORD(msg.lParam)); break;
                case WM_LBUTTONDOWN: m_input.OnMouseDown(0); break;
                case WM_LBUTTONUP:   m_input.OnMouseUp(0);   break;
                case WM_RBUTTONDOWN: m_input.OnMouseDown(1); break;
                case WM_RBUTTONUP:   m_input.OnMouseUp(1);   break;
                }
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }

            m_timer.Tick();
            float dt = m_timer.DeltaTime();

            if (m_input.IsKeyDown(VK_ESCAPE)) PostQuitMessage(0);

            m_camera.Update(m_input, dt);
            m_fallingLights.Update(dt);

            XMFLOAT3 eye, target, up;
            m_camera.GetViewVectors(eye, target, up);
            m_renderer.SetCamera(eye, target, up);

            m_renderer.ClearLights();
            m_renderer.SetAmbient({ 0.02f, 0.02f, 0.03f });
            m_renderer.SetDirectionalLight(
                { 1.f, -0.1f, 0.f },
                { 0.2f, 0.4f, 1.0f },
                4.0f);
            m_renderer.AddPointLight({ 0.f, 120.f, 0.f }, { 1.0f, 0.45f, 0.0f }, 600.f, 10.0f);
            m_renderer.AddSpotLight(
                { 0.f, 180.f, 0.f }, { 0.f, -1.f, 0.f },
                { 0.1f, 1.0f, 0.2f }, 12.f, 25.f, 20.0f);
            m_fallingLights.SubmitLights(m_renderer);

            const float clear[] = { 0.f, 0.f, 0.f, 1.f };
            m_renderer.BeginFrame(clear);
            m_renderer.DrawScene(m_timer.TotalTime(), dt);
            m_renderer.EndFrame();
            m_input.EndFrame();
        }
    }

private:
    Window          m_window;
    RenderingSystem m_renderer;
    Timer           m_timer;
    InputDevice     m_input;
    FPSCamera       m_camera;
    FallingStar     m_fallingLights;
};

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    App app;
    if (!app.Init(hInstance))
    {
        MessageBox(nullptr, L"Init failed!", L"Error", MB_OK | MB_ICONERROR);
        return -1;
    }
    app.Show(nCmdShow);
    return app.Run();
}