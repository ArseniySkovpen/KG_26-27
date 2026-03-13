#pragma once
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <utility> // std::pair

class InputDevice
{
public:
    // ---- Клавиатура ----
    void OnKeyDown(WPARAM key) { if (key < 256) m_keys[key] = true; }
    void OnKeyUp(WPARAM key) { if (key < 256) m_keys[key] = false; }

    // Зажата ли клавиша прямо сейчас
    bool IsKeyDown(int key) const { return key >= 0 && key < 256 && m_keys[key]; }

    // ---- Мышь ----
    void OnMouseMove(int x, int y)
    {
        if (m_firstMove)
        {
            // При первом движении просто запоминаем позицию без дельты
            m_mouseX = x; m_mouseY = y;
            m_firstMove = false;
            return;
        }
        m_mouseDX += x - m_mouseX;
        m_mouseDY += y - m_mouseY;
        m_mouseX = x;
        m_mouseY = y;
    }

    void OnMouseDown(int btn) { if (btn < 3) m_mouseButtons[btn] = true; }
    void OnMouseUp(int btn) { if (btn < 3) m_mouseButtons[btn] = false; }

    bool IsMouseDown(int btn) const { return btn >= 0 && btn < 3 && m_mouseButtons[btn]; }

    // Накопленная дельта мыши за кадр
    // Вызывай после использования, до EndFrame
    std::pair<float, float> GetMouseDelta() const
    {
        return { (float)m_mouseDX, (float)m_mouseDY };
    }

    // ---- Вызывать в конце каждого кадра ----
    void EndFrame()
    {
        m_mouseDX = 0;
        m_mouseDY = 0;
    }

private:
    bool m_keys[256] = {};
    bool m_mouseButtons[3] = {};
    int  m_mouseX = 0, m_mouseY = 0;
    int  m_mouseDX = 0, m_mouseDY = 0;
    bool m_firstMove = true;
};