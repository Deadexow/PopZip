#pragma once

#include <windows.h>

#include <vector>

struct SkyStar {
    float x = 0.0f;
    float y = 0.0f;
    float vx = 0.0f;
    float vy = 0.0f;
    int size = 1;
    BYTE alpha = 200;
};

struct SkyMeteor {
    float x = 0.0f;
    float y = 0.0f;
    float vx = 0.0f;
    float vy = 0.0f;
};

struct SkyPetal {
    float x = 0.0f;
    float y = 0.0f;
    float vx = 0.0f;
    float vy = 0.0f;
    float angle = 0.0f;
    float spin = 0.0f;
    float size = 5.0f;
    float phase = 0.0f;
    int shade = 0;
};

struct SkyState {
    std::vector<SkyStar> stars;
    std::vector<SkyMeteor> meteors;
    std::vector<SkyPetal> petals;
    float diskAngle = 0.0f;
    float time = 0.0f;
    int kind = -1;
    bool initialized = false;
};

void ResetSky(SkyState& state);
void EnsureSky(SkyState& state, int width, int height);
void UpdateSky(SkyState& state, int width, int height);
void DrawSky(HDC dc, const SkyState& state, int width, int height);