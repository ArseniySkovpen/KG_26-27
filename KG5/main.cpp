#include "Window.h"
#include "RenderingSystem.h"
#include "Timer.h"
#include "InputDevice.h"
#include <cmath>

// ============================================================================
// Камера от первого лица
// ============================================================================
class FPSCamera
{
public:
    XMFLOAT3 position = { 0.f, 150.f, -500.f };
    float    yaw = 0.f;    // поворот влево/вправо (градусы)
    float    pitch = -10.f;  // поворот вверх/вниз (градусы, зажат в [-89, 89])

    float moveSpeed = 300.f;  // единиц в секунду
    float mouseSens = 0.15f;  // градусов на пиксель

    // Обновить позицию и угол по вводу
    void Update(const InputDevice& input, float dt)
    {
        // --- Поворот мышью (только когда зажата ПКМ) ---
        if (input.IsMouseDown(1))
        {
            auto [dx, dy] = input.GetMouseDelta();
            yaw += dx * mouseSens;
            pitch -= dy * mouseSens;
            // Ограничиваем pitch чтобы не перевернуться
            if (pitch > 89.f) pitch = 89.f;
            if (pitch < -89.f) pitch = -89.f;
        }

        // --- Вычисляем forward/right векторы из углов ---
        float yawRad = XMConvertToRadians(yaw);
        float pitchRad = XMConvertToRadians(pitch);

        // forward — куда смотрит камера
        XMFLOAT3 forward = {
            sinf(yawRad) * cosf(pitchRad),
            sinf(pitchRad),
            cosf(yawRad) * cosf(pitchRad)
        };

        // right — вправо от камеры (перпендикулярно forward и мировому up)
        XMVECTOR fwd = XMLoadFloat3(&forward);
        XMVECTOR worldUp = XMVectorSet(0, 1, 0, 0);
        XMVECTOR right = XMVector3Normalize(XMVector3Cross(fwd, worldUp));
        XMFLOAT3 r; XMStoreFloat3(&r, right);

        // --- Движение WASD ---
        float speed = dt * moveSpeed;

        // Shift — ускорение x5
        if (input.IsKeyDown(VK_SHIFT)) speed *= 5.f;

        XMVECTOR pos = XMLoadFloat3(&position);

        if (input.IsKeyDown('W'))
            pos = XMVectorAdd(pos, XMVectorScale(fwd, speed));
        if (input.IsKeyDown('S'))
            pos = XMVectorAdd(pos, XMVectorScale(fwd, -speed));
        if (input.IsKeyDown('D'))
            pos = XMVectorAdd(pos, XMVectorScale(right, speed));
        if (input.IsKeyDown('A'))
            pos = XMVectorAdd(pos, XMVectorScale(right, -speed));
        if (input.IsKeyDown('E'))
            pos = XMVectorAdd(pos, XMVectorScale(worldUp, speed));
        if (input.IsKeyDown('Q'))
            pos = XMVectorAdd(pos, XMVectorScale(worldUp, -speed));

        XMStoreFloat3(&position, pos);

        // Запоминаем forward для target
        m_forward = forward;
    }

    // Получить eye/target/up для передачи в RenderingSystem
    void GetViewVectors(XMFLOAT3& eye, XMFLOAT3& target, XMFLOAT3& up) const
    {
        eye = position;
        target = {
            position.x + m_forward.x,
            position.y + m_forward.y,
            position.z + m_forward.z
        };
        up = { 0.f, 1.f, 0.f };
    }

private:
    XMFLOAT3 m_forward = { 0.f, 0.f, 1.f };
};

// ============================================================================
// App
// ============================================================================
class App
{
public:
    bool Init(HINSTANCE hInstance)
    {
        if (!m_window.Init(hInstance, 1280, 720, L"DX12 Deferred Rendering - Sponza | ПКМ + WASD"))
            return false;

        m_window.SetResizeCallback([this](int w, int h) {
            m_renderer.OnResize(w, h);
            });

        if (!m_renderer.Init(m_window.GetHWND(),
            m_window.GetWidth(),
            m_window.GetHeight()))
            return false;

        if (!m_renderer.LoadObj("sponza.obj"))
        {
            MessageBoxA(nullptr,
                "Не удалось загрузить sponza.obj\n"
                "Файлы должны лежать рядом с .exe:\n"
                "sponza.obj, sponza.mtl, папка textures/",
                "Ошибка", MB_OK | MB_ICONWARNING);
        }

        m_renderer.SetTexTiling(1.0f, 1.0f);
        // gTexScrollX = скорость синуса, gTexScrollY = амплитуда скейла
        // scaleY = 1.0 + sin(time * 1.0) * 0.3 → от 0.7x до 1.3x по вертикали
        m_renderer.SetTexScroll(1.0f, 0.3f);

        // Ambient почти чёрный — чтобы свет от каждого источника был чётко виден
        m_renderer.SetAmbient({ 0.01f, 0.01f, 0.01f });

        // ---- DIRECTIONAL — синий, сбоку ----
        // Освещает ВСЕ поверхности одинаково независимо от расстояния
        m_renderer.SetDirectionalLight(
            { 1.f, -0.1f, 0.f },
            { 0.2f, 0.4f, 1.0f },     // холодный синий
            4.0f);                     // интенсивность высокая

        // ---- POINT — оранжевый, центр ----
        // Затухает с расстоянием, круглое пятно
        m_renderer.AddPointLight(
            { 0.f, 120.f, 0.f },
            { 1.0f, 0.45f, 0.0f },
            600.f, 10.0f);

        // ---- SPOT — зелёный, низко над полом ----
        // Конус с чёткой границей
        m_renderer.AddSpotLight(
            { 0.f, 180.f, 0.f },      // низко — 180 единиц над полом
            { 0.f, -1.f, 0.f },
            { 0.1f, 1.0f, 0.2f },     // зелёный
            12.f,                      // inner
            25.f,                      // outer
            20.0f);                    // очень яркий

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

            // Обновляем камеру
            m_camera.Update(m_input, dt);

            // Передаём позицию/направление в renderer
            XMFLOAT3 eye, target, up;
            m_camera.GetViewVectors(eye, target, up);
            m_renderer.SetCamera(eye, target, up);

            // ESC — выход
            if (m_input.IsKeyDown(VK_ESCAPE))
                PostQuitMessage(0);

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