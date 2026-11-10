#pragma once
#include "BaseScene.h"
#include "Input.h"
#include "Model.h"
#include "Camera.h"
#include "GraphicsPipeline.h"
#include "D3D12Util.h"
#include "DataTypes.h"
#include "Skybox.h"
#include "Sprite.h"
#include <memory>
#include <wrl.h>
#include "GamePlayScene.h"

// タイトルシーン
class TitleScene : public BaseScene {
public:
    void Initialize() override;
    void Finalize() override;
    void Update() override;
    void Draw() override;

private:
    Input* input_ = nullptr;
    GraphicsPipeline* graphicsPipeline_ = nullptr;
    std::unique_ptr<Camera> camera_;
    std::unique_ptr<Model> titleModel_;           // メイン前面3D文字（紅蓮＆光沢反射）
    std::unique_ptr<Model> titleRimModel_;        // 黄金メタリック・ベベル縁取り3D文字
    std::unique_ptr<Model> titleShadowModel_;     // 重厚ダークシルエット＆立体シャドウ3D文字
    std::unique_ptr<Model> titleAuraModels_[2];   // 背面へ鼓動拡散する3Dオーラ残像
    std::unique_ptr<Skybox> skybox_;
    std::unique_ptr<GamePlayScene> gameplayScene_;
    uint32_t inputDelay_ = 24; // シーン開始時の誤判定防止用のディレイカウンタ

    bool isTransitioningToGame_ = false;       // 遷移演出中フラグ
    float transitionTimer_ = 0.0f;             // 遷移演出タイマー
    const float kTransitionDuration = 1.3f;    // ブースト加速演出時間 (1.3s)
    float titleAnimTimer_ = 0.0f;              // タイトル演出・モーション用タイマー

    // 3Dタイトル各レイヤー用の定数バッファ
    Microsoft::WRL::ComPtr<ID3D12Resource> transformResource_;
    TransformationMatrix* transformData_ = nullptr;

    Microsoft::WRL::ComPtr<ID3D12Resource> rimTransformResource_;
    TransformationMatrix* rimTransformData_ = nullptr;

    Microsoft::WRL::ComPtr<ID3D12Resource> shadowTransformResource_;
    TransformationMatrix* shadowTransformData_ = nullptr;

    Microsoft::WRL::ComPtr<ID3D12Resource> auraTransformResources_[2];
    TransformationMatrix* auraTransformData_[2] = { nullptr, nullptr };

    Microsoft::WRL::ComPtr<ID3D12Resource> directionalLightResource_;
    DirectionalLight* directionalLightData_ = nullptr;

    struct CameraDataCB {
        Vector3 worldPosition;
        float padding;
    };
    Microsoft::WRL::ComPtr<ID3D12Resource> cameraResource_;
    CameraDataCB* cameraDataCB_ = nullptr;

    // 2Dタクティカル・バックドロップ＆サイバー演出スプライト群
    std::unique_ptr<Sprite> letterboxSprites_[2];      // 上下シネマティック帯
    std::unique_ptr<Sprite> letterboxEdgeSprites_[2];  // 上下ネオンエッジライン
    std::unique_ptr<Sprite> visorBandSprite_;          // タイトル背面ダークバイザー帯
    std::unique_ptr<Sprite> visorInnerBandSprite_;     // バイザー内側の紅蓮コア帯
    std::unique_ptr<Sprite> visorRailSprites_[4];      // バイザー上下の2重レーザーレール
    std::unique_ptr<Sprite> backdropGlowSprites_[2];   // タイトル背面のラジアル発光グロー
    std::unique_ptr<Sprite> streakSprites_[6];         // 高速スピードストリーク光線
    std::unique_ptr<Sprite> diamondSprites_[4];        // 左右の回転サイバーダイヤモンド紋章
    std::unique_ptr<Sprite> emberSprites_[12];         // 舞い上がる火の粉・スパーク粒子
    std::unique_ptr<Sprite> impactFlashSprite_;        // 登場合体時の閃光スラッシュビーム
};