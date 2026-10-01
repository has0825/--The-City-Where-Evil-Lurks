#pragma once
#include <memory>
#include <array>
#include <vector>
#include <string>
#include "Sprite.h"
#include "MathUtil.h"

class LoadingScreen {
public:
    static LoadingScreen* GetInstance();

    void Initialize();
    void Reset();
    void Update(float dt, float progress);
    void Draw();

    void SetProgress(float progress) { targetProgress_ = progress; }
    float GetProgress() const { return currentProgress_; }
    bool IsTransitionCompleted() const { return isTransitionCompleted_; }

private:
    LoadingScreen() = default;
    ~LoadingScreen() = default;
    LoadingScreen(const LoadingScreen&) = delete;
    LoadingScreen& operator=(const LoadingScreen&) = delete;

private:
    std::unique_ptr<Sprite> background_;
    std::unique_ptr<Sprite> textBg_;
    std::unique_ptr<Sprite> textFill_;
    std::array<std::unique_ptr<Sprite>, 6> dots_;

    float time_ = 0.0f;
    float currentProgress_ = 0.0f;
    float targetProgress_ = 0.0f;
    float fadeAlpha_ = 1.0f;
    bool isFadingOut_ = false;
    bool isTransitionCompleted_ = false;
    bool isInitialized_ = false;
};
