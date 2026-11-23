#ifndef WINPUT_HPP
#define WINPUT_HPP

#include <windows.h>
#include <chrono>
#include <functional>
#include <random>
#include "tea_wraps.hpp"

constexpr float DEFAULT_RANDOM_MIN = 0.25;
constexpr float DEFAULT_RANDOM_MAX = 1.25;

struct Winput
{
    using Wait = std::function<bool(std::chrono::milliseconds)>;
    Winput(INPUT& i, POINT& mouse, std::random_device& rand_device);
    
    void RandomKeyPress(char c = 0, float min = DEFAULT_RANDOM_MIN,    //Float passed in represents time in seconds, truncated after 3 decimal points
        float max = DEFAULT_RANDOM_MAX);   

    void RandomMousePress(int pos = 0,  //Float passed in represents time in seconds, truncated after 3 decimal points
        float min = DEFAULT_RANDOM_MIN, float max = DEFAULT_RANDOM_MAX);

    bool RealisticMouseMove(const Coordinates& from,    //Realistically moves mouse, 'from' -> 'to'
        const Coordinates& to, const Wait& wait = {});

    void TypeString(const char* s); //No special chars, not case sensitive, not realistically typed

    Coordinates GetMousePos();
    static bool IsOnDisplay(const Coordinates& position);

private:
     INPUT& input;
     POINT& mousePos;
     std::random_device& rd;
     std::mt19937 gen;

    Winput() = delete;

    void KeyDown(char c);
    void KeyUp(char c);

    void MouseDown(int pos = 0);    //L-click = 0, R-click = 1
    void MouseUp(int pos = 0);      //L-click = 0, R-click = 1
};

#endif
