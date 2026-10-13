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
    // ロード完了後（ロード画面 -> ゲーム本編）の遷移演出パターン
    enum class ExitPattern {
        kReverseSlashSplitLeftRight, // パターン0: 右斜め下 -> 左斜め上へ1回斬って左右に開く
        kMultiSlashGlassShatter      // パターン1: 色んな方向から何度も斬りつけ、正面から突っ込んでガラスを割り、ゲーム内プレイヤーと重なって始まる
    };

    enum class TransitionPhase {
        kSlashTitle,             // 1. 左斜め上 -> 右斜め下へプレイヤー機が横断してタイトル画面を切る
        kSplitTitleUpDown,       // 2. 切れたタイトル画面が上下に移動し、隙間からロード画面が出る
        kLoadingProgress,        // 3. ロード画面表示＆ゲージ進行（裏で次シーン生成・初期化）
        kSlashLoadingReverse,    // [パターン0] 4A. 右斜め下 -> 左斜め上へプレイヤー機が逆横断してロード画面を切る
        kSplitLoadingLeftRight,  // [パターン0] 5A. 切れたロード画面が左右に移動し、隙間からゲーム画面が出る
        kMultiSlashLoading,      // [パターン1] 4B. 色んな方向から何度も横断して切れ目を入れた後、正面から突っ込む
        kGlassShatterAndOverlap, // [パターン1] 5B. ガラスが割れて破片が飛び散り、突っ込んだ機体がゲーム内プレイヤーと重なる
        kCompleted               // 6. 完全に画面外へ出たら完了（直後にPHASE 1開始）
    };

    static LoadingScreen* GetInstance();

    void Initialize();
    void Reset();
    void Update(float dt, float progress);
    void Draw();

    void SetExitPattern(ExitPattern pattern) { exitPattern_ = pattern; }
    ExitPattern GetExitPattern() const { return exitPattern_; }

    void SetProgress(float progress) { targetProgress_ = progress; }
    float GetProgress() const { return currentProgress_; }
    TransitionPhase GetPhase() const { return phase_; }
    bool IsReadyToLoadNextScene() const {
        return phase_ == TransitionPhase::kLoadingProgress ||
               phase_ == TransitionPhase::kSlashLoadingReverse ||
               phase_ == TransitionPhase::kSplitLoadingLeftRight ||
               phase_ == TransitionPhase::kMultiSlashLoading ||
               phase_ == TransitionPhase::kGlassShatterAndOverlap ||
               phase_ == TransitionPhase::kCompleted;
    }
    bool ShouldDrawNextSceneUnderneath() const {
        return phase_ == TransitionPhase::kSplitLoadingLeftRight ||
               phase_ == TransitionPhase::kGlassShatterAndOverlap;
    }
    bool IsTransitionCompleted() const { return isTransitionCompleted_; }

private:
    LoadingScreen() = default;
    ~LoadingScreen() = default;
    LoadingScreen(const LoadingScreen&) = delete;
    LoadingScreen& operator=(const LoadingScreen&) = delete;

    void BuildGlassShards();
    void CaptureCurrentBackBuffer(ID3D12GraphicsCommandList* commandList);
    void DrawLoadingBase(ID3D12GraphicsCommandList* commandList, const Matrix4x4& orthoProjection, Vector2 shakeOffset = { 0.0f, 0.0f });
    void DrawLineSprite(
        Sprite* sprite,
        ID3D12GraphicsCommandList* commandList,
        const Matrix4x4& orthoProjection,
        float x0, float y0, float x1, float y1,
        float thickness, const Vector4& color);
    void DrawSlashFighter3D(
        ID3D12GraphicsCommandList* commandList,
        float screenX, float screenY,
        float dirScreenX, float dirScreenY,
        float progress01);
    void DrawDiveAndOverlapFighter3D(
        ID3D12GraphicsCommandList* commandList,
        float diveProgress01,
        bool isShatterPhase,
        float shatterProgress01);
    void DrawDynamicBatch(
        ID3D12GraphicsCommandList* commandList,
        const Matrix4x4& orthoProjection,
        const std::vector<VertexData>& vertices,
        VertexData* mappedVertices,
        const D3D12_VERTEX_BUFFER_VIEW& vbView,
        ID3D12Resource* matRes,
        Material* matData,
        ID3D12Resource* wvpRes,
        TransformationMatrix* wvpData,
        D3D12_GPU_DESCRIPTOR_HANDLE srvHandle,
        const Vector4& color);

private:
    struct SlashCameraCB {
        Vector3 worldPosition;
        float padding;
    };

    struct GlassShard {
        Vector2 basePos[3];
        Vector2 uv[3];
        Vector2 center;
        Vector2 vel;
        float rotXSpeed = 0.0f;
        float rotYSpeed = 0.0f;
        float rotZSpeed = 0.0f;
        float scaleSpeed = 0.0f;
        float delay = 0.0f;
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

    // 複数回の斬撃ライン＆ガラスひび割れ一括描画用ダイナミックバッチ
    static constexpr uint32_t kMaxBatchVertices = 2400;
    Microsoft::WRL::ComPtr<ID3D12Resource> slashGlowBatchVbRes_;
    VertexData* slashGlowBatchMapped_ = nullptr;
    D3D12_VERTEX_BUFFER_VIEW slashGlowBatchVbView_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> slashGlowBatchMatRes_;
    Material* slashGlowBatchMatData_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> slashGlowBatchWvpRes_;
    TransformationMatrix* slashGlowBatchWvpData_ = nullptr;

    Microsoft::WRL::ComPtr<ID3D12Resource> slashCoreBatchVbRes_;
    VertexData* slashCoreBatchMapped_ = nullptr;
    D3D12_VERTEX_BUFFER_VIEW slashCoreBatchVbView_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> slashCoreBatchMatRes_;
    Material* slashCoreBatchMatData_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> slashCoreBatchWvpRes_;
    TransformationMatrix* slashCoreBatchWvpData_ = nullptr;

    // ガラス破片（112枚のポリゴン破片）描画用ダイナミックバッチ
    std::vector<GlassShard> glassShards_;
    static constexpr uint32_t kMaxShardVertices = 600;
    Microsoft::WRL::ComPtr<ID3D12Resource> shatterTexVbRes_;
    VertexData* shatterTexMapped_ = nullptr;
    D3D12_VERTEX_BUFFER_VIEW shatterTexVbView_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> shatterTexMatRes_;
    Material* shatterTexMatData_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> shatterTexWvpRes_;
    TransformationMatrix* shatterTexWvpData_ = nullptr;

    Microsoft::WRL::ComPtr<ID3D12Resource> shatterGlintVbRes_;
    VertexData* shatterGlintMapped_ = nullptr;
    D3D12_VERTEX_BUFFER_VIEW shatterGlintVbView_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> shatterGlintMatRes_;
    Material* shatterGlintMatData_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> shatterGlintWvpRes_;
    TransformationMatrix* shatterGlintWvpData_ = nullptr;

    // 画面を横断・突入して斬る3Dプレイヤー機モデル
    std::unique_ptr<Model> slashFighterModel_;
    Microsoft::WRL::ComPtr<ID3D12Resource> slashFighterTransformRes_;
    TransformationMatrix* slashFighterTransformData_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> slashFighterLightRes_;
    DirectionalLight* slashFighterLightData_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> slashFighterCameraRes_;
    SlashCameraCB* slashFighterCameraData_ = nullptr;

    ExitPattern exitPattern_ = ExitPattern::kMultiSlashGlassShatter;
    TransitionPhase phase_ = TransitionPhase::kSlashTitle;
    float phaseTimer_ = 0.0f;
    float time_ = 0.0f;
    float currentProgress_ = 0.0f;
    float targetProgress_ = 0.0f;
    bool hasCapturedTitle_ = false;
    bool hasCapturedLoading_ = false;
    bool hasCapturedCrackedLoading_ = false;
    bool isTransitionCompleted_ = false;
    bool isInitialized_ = false;
};


