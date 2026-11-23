#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include "winput.hpp"
#include "tea_wraps.hpp"

using namespace std;

namespace
{
int RoundCursorCoordinate(double coordinate)
{
    constexpr double minimum = std::numeric_limits<int>::min();
    constexpr double maximum = std::numeric_limits<int>::max();
    return static_cast<int>(std::lround(std::clamp(coordinate, minimum, maximum)));
}
}

Winput::Winput(INPUT& i, POINT& mouse, random_device& rand_device) : input(i), 
    mousePos(mouse), rd(rand_device), gen(rd()) {}

void Winput::KeyUp(char c)
{
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = toupper(c);

    input.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &input, sizeof(INPUT));
}

void Winput::KeyDown(char c)
{
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = toupper(c);

    input.ki.dwFlags = 0;
    SendInput(1, &input, sizeof(INPUT));
}

void Winput::MouseUp(int pos)
{
    if (pos != 0 && pos != 1)
    {
        cerr << "Invalid 'MouseUp()' parameter.\n";
        return;
    }

    input.mi.dwFlags = pos == 0 ? MOUSEEVENTF_LEFTUP : MOUSEEVENTF_RIGHTUP;
    input.type = INPUT_MOUSE;
    SendInput(1, &input, sizeof(INPUT));
}

void Winput::MouseDown(int pos)
{
    if (pos != 0 && pos != 1)
    {
        cerr << "Invalid 'MouseDown()' parameter.\n";
        return;
    }

    input.mi.dwFlags = pos == 0 ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_RIGHTDOWN;
    input.type = INPUT_MOUSE;
    SendInput(1, &input, sizeof(INPUT));
}

void Winput::RandomKeyPress(char c, float min ,float max)
{
    uniform_real_distribution<float> distrib(min, max);
    SleepThread(distrib(gen));
    KeyDown(c);
    SleepThread(distrib(gen));
    KeyUp(c);
}

void Winput::RandomMousePress(int pos, float min, float max)
{
    uniform_real_distribution<float> distrib(min, max);
    SleepThread(distrib(gen));
    MouseDown(pos);
    SleepThread(distrib(gen));
    MouseUp(pos);
}

void Winput::TypeString(const char* s)
{
    while (*s)
    {
        RandomKeyPress(*s, 0.001, 0.005);
        ++s;
    }
}

Coordinates Winput::GetMousePos()
{
    if (!GetCursorPos(&mousePos))
        throw runtime_error("GetCursorPos failed.");

    return Coordinates(mousePos.x, mousePos.y);
}

bool Winput::IsOnDisplay(const Coordinates& position)
{
    return MonitorFromPoint({position.pos_x, position.pos_y}, MONITOR_DEFAULTTONULL) != nullptr;
}

bool Winput::RealisticMouseMove(const Coordinates& from,
    const Coordinates& to, const Wait& wait)
{
    if (from == to)
        return !wait || wait(chrono::milliseconds(0));

    auto random = [&](double min, double max) {
        return uniform_real_distribution<double>(min, max)(gen);
    };

    auto move = [&](const Coordinates& begin, const Coordinates& end,
                    double seconds) -> bool {
        if (begin == end)
            return true;

        const double dx = double(end.pos_x) - begin.pos_x;
        const double dy = double(end.pos_y) - begin.pos_y;
        const double distance = hypot(dx, dy);
        const double bend1 = random(-0.15, 0.15);
        const double bend2 = random(-0.15, 0.15);
        const double x1 = begin.pos_x + dx * 0.3 - dy * bend1;
        const double y1 = begin.pos_y + dy * 0.3 + dx * bend1;
        const double x2 = begin.pos_x + dx * 0.7 - dy * bend2;
        const double y2 = begin.pos_y + dy * 0.7 + dx * bend2;
        const double jitterAt = random(0.25, 0.75);
        const double jitterSize = bernoulli_distribution(0.35)(gen)
            ? fmin(1.5, distance * 0.02) * random(-1.0, 1.0) : 0.0;
        const auto start = chrono::steady_clock::now();
        double elapsed = 0;

        while (true)
        {
            if (wait && !wait(chrono::milliseconds(0))) return false;
            if (!wait) elapsed = chrono::duration<double>(chrono::steady_clock::now() - start).count();
            const double progress = fmin(1.0, elapsed / seconds);
            const double t = progress * progress *
                (6.0 - 8.0 * progress + 3.0 * progress * progress);
            const double u = 1.0 - t;
            const double w0 = u * u * u;
            const double w1 = 3.0 * u * u * t;
            const double w2 = 3.0 * u * t * t;
            const double w3 = t * t * t;
            const double phase = (progress - jitterAt) / 0.1;
            const double envelope = fmax(0.0, 1.0 - phase * phase);
            const double jitter = jitterSize * envelope * envelope;
            const double x = w0 * begin.pos_x + w1 * x1 + w2 * x2
                           + w3 * end.pos_x - dy / distance * jitter;
            const double y = w0 * begin.pos_y + w1 * y1 + w2 * y2
                           + w3 * end.pos_y + dx / distance * jitter;

            if (!SetCursorPos(RoundCursorCoordinate(x), RoundCursorCoordinate(y)))
            {
                cerr << "SetCursorPos failed.\n";
                return false;
            }

            if (progress >= 1.0)
                return true;

            if (wait)
            {
                if (!wait(chrono::milliseconds(8))) return false;
                elapsed += 0.008;
            }
            else SleepThread(0.008f);
        }
    };

    const double dx = double(to.pos_x) - from.pos_x;
    const double dy = double(to.pos_y) - from.pos_y;
    const double distance = hypot(dx, dy);
    const double seconds =
        fmin(0.8, 0.12 + distance * 0.0004) * random(0.85, 1.15);
    const int overshootChance = uniform_int_distribution<int>(13, 67)(gen);
    Coordinates overshoot = to;

    if (bernoulli_distribution(overshootChance / 100.0)(gen))
    {
        const double amount = fmin(12.0, distance * random(0.02, 0.06));
        overshoot.pos_x = RoundCursorCoordinate(to.pos_x + dx / distance * amount);
        overshoot.pos_y = RoundCursorCoordinate(to.pos_y + dy / distance * amount);
    }

    if (!move(from, overshoot, seconds)) return false;

    return overshoot == to || move(overshoot, to, random(0.06, 0.14));
}
