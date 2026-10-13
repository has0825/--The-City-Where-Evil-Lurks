#pragma once
#include <memory>
#include <array>
#include <vector>
#include <string>
#include <wrl.h>
#include <d3d12.h>
#include "Sprite.h"
#include "Model.h"
#include "MathUtil.h"
#include "DataTypes.h"

class LoadingScreen {
public:
    enum class TransitionPhase {
        kSlashTitle,            // 1. 左斜め上 -> 右斜め下へプレイヤー機が横断して画面を切る
        kSplitTitleUpDown,      // 2. 切れたタイトル画面が上下に移動し、隙間からロード画面が出る
        kLoadingProgress,       // 3. ロード画面表示＆ゲージ進行（裏で次シーン生成・初期化）
        kSlashLoadingReverse,   // 4. 右斜め下 -> 左斜め上へプレイヤー機が逆横断してロード画面を切る
        kSplitLoadingLeftRight, // 5. 切れたロード画面が左右に移動し、隙間からゲーム画面が出る
        kCompleted              // 6. 切れたロード画面が完全に画面外へ出たら完了（PHASE 1開始）
    };

    static LoadingScreen* GetInstance();

    void Initialize();
    void Reset();
    void Update(float dt, float progress);
    void Draw();

    void SetProgress(float progress) { targetProgress_ = progress; }
    float GetProgress() const { return currentProgress_; }
    TransitionPhase GetPhase() const { return phase_; }
    bool IsReadyToLoadNextScene() const {
        return phase_ == TransitionPhase::kLoadingProgress ||
               phase_ == TransitionPhase::kSlashLoadingReverse ||
               phase_ == TransitionPhase::kSplitLoadingLeftRight ||
               phase_ == TransitionPhase::kCompleted;
    }
    bool IsTransitionCompleted() const { return isTransitionCompleted_; }

private:
    LoadingScreen() = default;
    ~LoadingScreen() = default;
    LoadingScreen(const LoadingScreen&) = delete;
    LoadingScreen& operator=(const LoadingScreen&) = delete;

    void CaptureCurrentBackBuffer(ID3D12GraphicsCommandList* commandList);
    void DrawLoadingBase(ID3D12GraphicsCommandList* commandList, const Matrix4x4& orthoProjection);
    void DrawLineSprite(
        Sprite* sprite,
        ID3D12GraphicsCommandList* commandList,
        const Matrix4x4& orthoProjection,
        float x0, float y0, float x1, float y1,
        float thickness, const Vector4& color);
    void DrawSlashFighter3D(
        ID3D12GraphicsCommandList* commandList,
        float screenX, float screenY,
        bool isTopLeftToBottomRight,
        float progress01);

private:
    struct SlashCameraCB {
        Vector3 worldPosition;
        float padding;
    };

    std::unique_ptr<Sprite> background_;
    std::unique_ptr<Sprite> textBg_;
    std::unique_ptr<Sprite> textFill_;
    std::array<std::unique_ptr<Sprite>, 6> dots_;

    // 画面キャプチャ＆分割描画用リソース
    Microsoft::WRL::ComPtr<ID3D12Resource> capturedTextureResource_;
    uint32_t capturedSrvIndex_ = 0;
    D3D12_GPU_DESCRIPTOR_HANDLE capturedSrvHandleGPU_{};
    std::unique_ptr<Sprite> capturedFullSprite_;
    std::unique_ptr<Sprite> capturedUpperRightSprite_;
    std::unique_ptr<Sprite> capturedLowerLeftSprite_;

    // 斬撃ライン・切断面エッジ発光用スプライト
    std::unique_ptr<Sprite> slashGlowOuterSprite_;
    std::unique_ptr<Sprite> slashGlowMidSprite_;
    std::unique_ptr<Sprite> slashCoreSprite_;
    std::unique_ptr<Sprite> slashEdgeSpriteA_;
    std::unique_ptr<Sprite> slashEdgeSpriteB_;
    std::unique_ptr<Sprite> slashEdgeCoreA_;
    std::unique_ptr<Sprite> slashEdgeCoreB_;
    std::unique_ptr<Sprite> slashFlashSprite_;

    // 画面を横断して斬る3Dプレイヤー機モデル
    std::unique_ptr<Model> slashFighterModel_;
    Microsoft::WRL::ComPtr<ID3D12Resource> slashFighterTransformRes_;
    TransformationMatrix* slashFighterTransformData_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> slashFighterLightRes_;
    DirectionalLight* slashFighterLightData_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> slashFighterCameraRes_;
    SlashCameraCB* slashFighterCameraData_ = nullptr;

    TransitionPhase phase_ = TransitionPhase::kSlashTitle;
    float phaseTimer_ = 0.0f;
    float time_ = 0.0f;
    float currentProgress_ = 0.0f;
    float targetProgress_ = 0.0f;
    bool hasCapturedTitle_ = false;
    bool hasCapturedLoading_ = false;
    bool isTransitionCompleted_ = false;
    bool isInitialized_ = false;
};

