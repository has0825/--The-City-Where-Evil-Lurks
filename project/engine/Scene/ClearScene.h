#pragma once
#include "BaseScene.h"
#include "Input.h"
#include "Model.h"
#include "Camera.h"
#include "GraphicsPipeline.h"
#include "D3D12Util.h"
#include "DataTypes.h"
#include "ParticleManager.h"
#include <memory>
#include <wrl.h>
#include <vector>

// ゲームクリアシーン
class ClearScene : public BaseScene {
public:
    void Initialize() override;
    void Finalize() override;
    void Update() override;
    void Draw() override;

private:
    Input* input_ = nullptr;
    GraphicsPipeline* graphicsPipeline_ = nullptr;
    std::unique_ptr<Camera> camera_;
    std::unique_ptr<Model> clearModel_;

    Microsoft::WRL::ComPtr<ID3D12Resource> transformResource_;
    TransformationMatrix* transformData_ = nullptr;

    Microsoft::WRL::ComPtr<ID3D12Resource> directionalLightResource_;
    DirectionalLight* directionalLightData_ = nullptr;

    struct CameraDataCB {
        Vector3 worldPosition;
        float padding;
    };
    Microsoft::WRL::ComPtr<ID3D12Resource> cameraResource_;
    CameraDataCB* cameraDataCB_ = nullptr;

    // パーティクル管理用
    std::unique_ptr<ParticleManager> particleManager_;
    std::unique_ptr<Model> particleModel_;
    std::unique_ptr<Model> ringModel_;
    std::unique_ptr<Model> cylinderModel_;
    D3D12_GPU_DESCRIPTOR_HANDLE textureSrvHandle_{};
    D3D12_GPU_DESCRIPTOR_HANDLE gradationSrvHandle_{};
    D3D12_GPU_DESCRIPTOR_HANDLE textSrvHandle_{};

    // 打ち上げ花火管理用
    struct ActiveFirework {
        Vector3 position;
        Vector3 velocity;
        float timer;
        float maxTime;
        Vector3 color;
    };
    std::vector<ActiveFirework> activeFireworks_;
    float fireworkSpawnTimer_ = 0.0f;

    // 背景飛行プレイヤー用モデル
    std::unique_ptr<Model> playerModel_;
    Microsoft::WRL::ComPtr<ID3D12Resource> playerTransformResource_;
    TransformationMatrix* playerTransformData_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> playerLightResource_;
    DirectionalLight* playerLightData_ = nullptr;
    std::string playerEnvMapName_ = "test.dds";
    float time_ = 0.0f;

    // タイトル遷移時のプレイヤー演出用
    bool isReturningToTitle_ = false;
    bool hasTriggeredStarFlash_ = false;
    float returnTimer_ = 0.0f;
    Vector3 returnStartPos_{};
    Vector3 returnStartRot_{};

    // 花火爆発のプレイヤーへの照り返し演出用
    Vector3 fireworkFlashPos_ = { 0.0f, 0.0f, 20.0f };
    Vector3 fireworkFlashColor_ = { 1.0f, 0.8f, 0.4f };
    float fireworkFlashTimer_ = 0.0f;

    // 夜空背景用定数バッファ
    struct NightSkyCB {
        float time;
        float aspectRatio;
        float padding[2];
    };
    Microsoft::WRL::ComPtr<ID3D12Resource> nightSkyResource_;
    NightSkyCB* nightSkyData_ = nullptr;
};
