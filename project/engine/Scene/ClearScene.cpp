#include "ClearScene.h"
#include "SceneManager.h"
#include "WinApp.h"
#include "TextureManager.h"
#include "MathUtil.h"
#include "DirectXCommon.h"
#include <algorithm>
#include <cstdlib>

void ClearScene::Initialize() {
    // 前のシーンのリソースを即座に解放
    SceneManager::GetInstance()->ClearPreviousScene();

    input_ = Input::GetInstance();
    graphicsPipeline_ = GraphicsPipeline::GetInstance();
    DirectXCommon* dxCommon = DirectXCommon::GetInstance();
    ID3D12Device* device = dxCommon->GetDevice();

    // クリアカラーを夜空の色に設定
    dxCommon->SetClearColor(0.002f, 0.004f, 0.015f, 1.0f);

    camera_ = std::make_unique<Camera>(WinApp::kClientWidth, WinApp::kClientHeight);
    camera_->SetTranslate({ 0.0f, 0.0f, -40.0f });

    // テクスチャロード
    TextureManager::GetInstance()->LoadTexture("Clear/Clear.png");
    TextureManager::GetInstance()->LoadTexture("Player2/Player_basecolor.JPEG");
    TextureManager::GetInstance()->LoadTexture("test.dds"); // 環境マップ用
    TextureManager::GetInstance()->LoadTexture("dds/qwantani_night_puresky_2k.dds"); // 夜空環境マップ用
    TextureManager::GetInstance()->LoadTexture("circle2.png");
    TextureManager::GetInstance()->LoadTexture("gradationLine.png");
    TextureManager::GetInstance()->LoadTexture("text1.png");

    if (TextureManager::GetInstance()->GetMetaData("dds/qwantani_night_puresky_2k.dds").IsCubemap()) {
        playerEnvMapName_ = "dds/qwantani_night_puresky_2k.dds";
    } else {
        playerEnvMapName_ = "test.dds";
    }

    textureSrvHandle_ = TextureManager::GetInstance()->GetSrvHandleGPU("circle2.png");
    gradationSrvHandle_ = TextureManager::GetInstance()->GetSrvHandleGPU("gradationLine.png");
    textSrvHandle_ = TextureManager::GetInstance()->GetSrvHandleGPU("text1.png");

    // パーティクルマネージャーとモデルの初期化
    particleManager_ = std::make_unique<ParticleManager>();
    particleManager_->Initialize(device);

    particleModel_ = std::unique_ptr<Model>(Model::CreateParticleModel(device));
    ringModel_ = std::unique_ptr<Model>(Model::CreateRingModel(device));
    cylinderModel_ = std::unique_ptr<Model>(Model::CreateCylinderModel(device));

    // モデル読み込み
    clearModel_ = Model::LoadGLTF("Resources/Clear/Clear.obj", device);
    clearModel_->transform.scale = { 0.4f, 0.4f, 0.4f }; // サイズを小さく
    clearModel_->transform.rotate = { 0.0f, 3.14159265f, 0.0f };
    clearModel_->transform.translate = { 0.0f, 0.0f, 20.0f }; // 遠く、中央付近に配置

    // 背景を飛ぶプレイヤーモデルのロード（遠くの夜空を真っ直ぐ飛行し、風景に溶け込むように調整）
    playerModel_ = Model::LoadGLTF("Resources/Player2/Player.obj", device);
    if (playerModel_) {
        playerModel_->transform.scale = { 2.2f, 2.2f, 2.2f };
        playerModel_->transform.rotate = { 0.0f, 1.57079632f, 0.0f }; // 右向き水平
        playerModel_->transform.translate = { -95.0f, 8.5f, 75.0f }; // 遠くの奥空を飛行
        playerModel_->SetColor({ 0.58f, 0.66f, 0.84f, 1.0f });
        playerModel_->SetEnvironmentCoefficient(0.08f);
        playerModel_->SetLightingEnabled(true);
    }

    // 定数バッファの作成
    transformResource_ = CreateBufferResource(device, sizeof(TransformationMatrix));
    transformResource_->Map(0, nullptr, reinterpret_cast<void**>(&transformData_));
    transformData_->WVP = MakeIdentity4x4();
    transformData_->World = MakeIdentity4x4();
    transformData_->color = { 1.0f, 1.0f, 1.0f, 1.0f };

    playerTransformResource_ = CreateBufferResource(device, sizeof(TransformationMatrix));
    playerTransformResource_->Map(0, nullptr, reinterpret_cast<void**>(&playerTransformData_));
    playerTransformData_->WVP = MakeIdentity4x4();
    playerTransformData_->World = MakeIdentity4x4();
    playerTransformData_->color = { 1.0f, 1.0f, 1.0f, 1.0f };

    directionalLightResource_ = CreateBufferResource(device, sizeof(DirectionalLight));
    directionalLightResource_->Map(0, nullptr, reinterpret_cast<void**>(&directionalLightData_));
    directionalLightData_->color = { 1.0f, 1.0f, 1.0f, 1.0f };
    directionalLightData_->direction = Normalize({ 0.0f, -1.0f, 1.0f });
    directionalLightData_->intensity = 1.0f;
    directionalLightData_->enableSpotLight = 0; // スポットライト無効
    directionalLightData_->enableNightEmissive = 0;
    directionalLightData_->numStreetLamps = 0;

    // プレイヤー専用夜空ライト（右上奥の月光＋花火爆発時の色照り返しスポットライト）
    playerLightResource_ = CreateBufferResource(device, sizeof(DirectionalLight));
    playerLightResource_->Map(0, nullptr, reinterpret_cast<void**>(&playerLightData_));
    playerLightData_->color = { 0.44f, 0.58f, 0.94f, 1.0f };
    playerLightData_->direction = Normalize({ -0.55f, -0.45f, 0.70f });
    playerLightData_->intensity = 0.65f;
    playerLightData_->enableSpotLight = 1;
    playerLightData_->spotLightPos = { 0.0f, 0.0f, 20.0f };
    playerLightData_->spotLightDir = { 0.0f, -1.0f, 0.0f };
    playerLightData_->spotLightRange = 80.0f;
    playerLightData_->spotLightCosAngle = std::cos(75.0f * 3.14159265f / 180.0f);
    playerLightData_->spotLightColor = { 1.0f, 0.8f, 0.4f };
    playerLightData_->spotLightIntensity = 0.0f;
    playerLightData_->enableNightEmissive = 0;
    playerLightData_->numStreetLamps = 0;

    cameraResource_ = CreateBufferResource(device, sizeof(CameraDataCB));
    cameraResource_->Map(0, nullptr, reinterpret_cast<void**>(&cameraDataCB_));
    cameraDataCB_->worldPosition = camera_->GetTransform().translate;

    // 夜空背景用定数バッファの作成
    nightSkyResource_ = CreateBufferResource(device, sizeof(NightSkyCB));
    nightSkyResource_->Map(0, nullptr, reinterpret_cast<void**>(&nightSkyData_));
    nightSkyData_->time = 0.0f;
    nightSkyData_->aspectRatio = static_cast<float>(WinApp::kClientWidth) / static_cast<float>(WinApp::kClientHeight);
    nightSkyData_->padding[0] = 0.0f;
    nightSkyData_->padding[1] = 0.0f;
}

void ClearScene::Finalize() {
}

void ClearScene::Update() {
    float kDeltaTime = 1.0f / 60.0f;

    // ── 打ち上げ花火の生成・更新 ──
    fireworkSpawnTimer_ -= kDeltaTime;
    if (fireworkSpawnTimer_ <= 0.0f) {
        // 0.4秒〜1.0秒に1回打ち上げる
        fireworkSpawnTimer_ = 0.4f + static_cast<float>(rand()) / RAND_MAX * 0.6f;

        ActiveFirework fw;
        // 画面下部のランダムな位置から
        fw.position = {
            -26.0f + static_cast<float>(rand()) / RAND_MAX * 52.0f,
            -18.0f,
            12.0f + static_cast<float>(rand()) / RAND_MAX * 22.0f
        };
        // 上方へのランダムな速度（頂点付近で綺麗に減速して開くように調整）
        fw.velocity = {
            -2.5f + static_cast<float>(rand()) / RAND_MAX * 5.0f,
            20.0f + static_cast<float>(rand()) / RAND_MAX * 6.5f,
            -1.0f + static_cast<float>(rand()) / RAND_MAX * 2.0f
        };
        fw.timer = 0.0f;
        // 1.35秒〜1.85秒後（上昇が落ち着く頂点付近）で爆発
        fw.maxTime = 1.35f + static_cast<float>(rand()) / RAND_MAX * 0.50f;

        // 本物の花火らしい鮮やかな炎色反応パレット
        static const Vector3 kFireworkPalette[] = {
            { 1.00f, 0.22f, 0.26f }, // 紅 (ストロンチウム)
            { 0.20f, 0.95f, 0.48f }, // 翠 (バリウム)
            { 0.25f, 0.62f, 1.00f }, // 蒼 (銅)
            { 1.00f, 0.84f, 0.30f }, // 黄金 (ナトリウム/鉄)
            { 0.88f, 0.32f, 1.00f }, // 紫
            { 1.00f, 0.52f, 0.78f }, // 桜
            { 0.92f, 0.96f, 1.00f }, // 白銀 (マグネシウム)
            { 1.00f, 0.50f, 0.16f }, // 橙
        };
        int palIdx = rand() % (sizeof(kFireworkPalette) / sizeof(kFireworkPalette[0]));
        fw.color = kFireworkPalette[palIdx];

        activeFireworks_.push_back(fw);
    }

    // 花火の更新
    for (auto it = activeFireworks_.begin(); it != activeFireworks_.end(); ) {
        it->position.x += it->velocity.x * kDeltaTime;
        it->position.y += it->velocity.y * kDeltaTime;
        it->position.z += it->velocity.z * kDeltaTime;
        
        // 少し重力を受けて減速
        it->velocity.y -= 9.8f * kDeltaTime;

        // 軌跡の火の粉を放出
        if (particleManager_) {
            particleManager_->EmitFireworkTrail(it->position, it->color);
        }

        it->timer += kDeltaTime;
        if (it->timer >= it->maxTime) {
            // 頂点で爆発！
            if (particleManager_) {
                particleManager_->EmitFirework(it->position, it->color);
            }
            // プレイヤーへの花火の照り返しフラッシュを記録
            fireworkFlashPos_ = it->position;
            fireworkFlashColor_ = it->color;
            fireworkFlashTimer_ = 0.65f;

            it = activeFireworks_.erase(it);
        } else {
            ++it;
        }
    }

    // 行列更新
    Matrix4x4 worldMatrix = MakeAffineMatrix(clearModel_->transform.scale, clearModel_->transform.rotate, clearModel_->transform.translate);
    Matrix4x4 viewMatrix = camera_->GetViewMatrix();
    Matrix4x4 projectionMatrix = camera_->GetProjectionMatrix();
    Matrix4x4 viewProj = Multiply(viewMatrix, projectionMatrix);
    Matrix4x4 wvpMatrix = Multiply(worldMatrix, viewProj);

    transformData_->World = worldMatrix;
    transformData_->WVP = wvpMatrix;

    // 背景を飛ぶプレイヤーの更新（遠くの夜空をまっすぐ安定して飛行）
    time_ += kDeltaTime;
    if (playerModel_ && playerTransformData_) {
        // 11秒で遠くの空を左から右へ真っ直ぐ飛び去る
        float duration = 11.0f;
        float t = std::fmod(time_, duration) / duration;

        float startX = -95.0f;
        float endX = 95.0f;
        playerModel_->transform.translate.x = startX + (endX - startX) * t;
        playerModel_->transform.translate.y = 8.5f;
        playerModel_->transform.translate.z = 75.0f;

        // 揺れをなくしてまっすぐ水平に飛行させる
        playerModel_->transform.rotate.x = 0.0f;
        playerModel_->transform.rotate.y = 1.57079632f;
        playerModel_->transform.rotate.z = 0.0f;

        Matrix4x4 playerWorldMatrix = MakeAffineMatrix(playerModel_->transform.scale, playerModel_->transform.rotate, playerModel_->transform.translate);
        Matrix4x4 playerWvpMatrix = Multiply(playerWorldMatrix, viewProj);
        playerTransformData_->World = playerWorldMatrix;
        playerTransformData_->WVP = playerWvpMatrix;

        // 機体尾部（左後ろ）からジェットアフターバーナー炎を噴射
        if (particleManager_) {
            Vector3 tailPos = {
                playerModel_->transform.translate.x - 1.8f,
                playerModel_->transform.translate.y,
                playerModel_->transform.translate.z
            };
            particleManager_->EmitJetAfterburner(tailPos, { -1.0f, 0.0f, 0.0f });
        }

        // プレイヤーへの花火照り返しスポットライト更新
        if (playerLightData_) {
            if (fireworkFlashTimer_ > 0.0f) {
                fireworkFlashTimer_ = (std::max)(0.0f, fireworkFlashTimer_ - kDeltaTime);
                float flashRatio = fireworkFlashTimer_ / 0.65f;
                Vector3 toPlayer = {
                    playerModel_->transform.translate.x - fireworkFlashPos_.x,
                    playerModel_->transform.translate.y - fireworkFlashPos_.y,
                    playerModel_->transform.translate.z - fireworkFlashPos_.z
                };
                playerLightData_->spotLightPos = fireworkFlashPos_;
                playerLightData_->spotLightDir = Normalize(toPlayer);
                playerLightData_->spotLightColor = fireworkFlashColor_;
                playerLightData_->spotLightIntensity = 3.2f * flashRatio * flashRatio;
            } else {
                playerLightData_->spotLightIntensity = 0.0f;
            }
        }
    }

    // パーティクルマネージャーの更新
    if (particleManager_) {
        particleManager_->Update(
            viewProj,
            camera_->GetBillboardMatrix(),
            kDeltaTime,
            camera_->GetTransform().translate.z,
            { 0,0,0 },
            false,
            false,
            0,
            { 0,0,0 }
        );
    }

    cameraDataCB_->worldPosition = camera_->GetTransform().translate;
    if (nightSkyData_) {
        nightSkyData_->time = time_;
    }

    // スペースキーでタイトルに戻る
    if (input_->IsKeyTriggered(DIK_SPACE)) {
        if (particleManager_) {
            particleManager_->Clear();
        }
        activeFireworks_.clear();
        SceneManager::GetInstance()->ChangeScene("TITLE");
    }
}

void ClearScene::Draw() {
    ID3D12GraphicsCommandList* commandList = DirectXCommon::GetInstance()->GetCommandList();

    // ── 夜空背景の描画 (最背面・深度テストなし) ──
    if (graphicsPipeline_ && graphicsPipeline_->GetNightSkyPipelineState() && graphicsPipeline_->GetNightSkyRootSignature()) {
        commandList->SetPipelineState(graphicsPipeline_->GetNightSkyPipelineState());
        commandList->SetGraphicsRootSignature(graphicsPipeline_->GetNightSkyRootSignature());
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if (nightSkyResource_) {
            commandList->SetGraphicsRootConstantBufferView(0, nightSkyResource_->GetGPUVirtualAddress());
        }
        commandList->DrawInstanced(3, 1, 0, 0);
    }

    // デスクリプタヒープのセット
    ID3D12DescriptorHeap* modelHeaps[] = { TextureManager::GetInstance()->GetSrvHeap() };
    commandList->SetDescriptorHeaps(1, modelHeaps);

    if (graphicsPipeline_ && graphicsPipeline_->GetObject3dPipelineState() && graphicsPipeline_->GetObject3dRootSignature()) {
        commandList->SetPipelineState(graphicsPipeline_->GetObject3dPipelineState());
        commandList->SetGraphicsRootSignature(graphicsPipeline_->GetObject3dRootSignature());

        // 行列、ライト、カメラの定数バッファをセット
        if (transformResource_) commandList->SetGraphicsRootConstantBufferView(1, transformResource_->GetGPUVirtualAddress());
        if (directionalLightResource_) commandList->SetGraphicsRootConstantBufferView(4, directionalLightResource_->GetGPUVirtualAddress());
        if (cameraResource_) commandList->SetGraphicsRootConstantBufferView(5, cameraResource_->GetGPUVirtualAddress());

        if (clearModel_) {
            clearModel_->DrawModel(
                commandList,
                TextureManager::GetInstance()->GetSrvHandleGPU("Clear/Clear.png"),
                TextureManager::GetInstance()->GetSrvHandleGPU("test.dds")
            );
        }

        // ── プレイヤーモデルの描画（夜空専用ライト＆夜空環境マップ） ──
        if (playerTransformResource_) commandList->SetGraphicsRootConstantBufferView(1, playerTransformResource_->GetGPUVirtualAddress());
        if (playerLightResource_) commandList->SetGraphicsRootConstantBufferView(4, playerLightResource_->GetGPUVirtualAddress());
        if (playerModel_) {
            playerModel_->DrawModel(
                commandList,
                TextureManager::GetInstance()->GetSrvHandleGPU("Player2/Player_basecolor.JPEG"),
                TextureManager::GetInstance()->GetSrvHandleGPU(playerEnvMapName_)
            );
        }

        // ── パーティクルの描画 (花火専用ソフトグロー加算合成パイプライン) ──
        if (particleManager_ && graphicsPipeline_ && graphicsPipeline_->GetRootSignature()) {
            commandList->SetGraphicsRootSignature(graphicsPipeline_->GetRootSignature());
            commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            ID3D12PipelineState* fwPso = graphicsPipeline_->GetFireworkParticlePipelineState();
            if (!fwPso) {
                fwPso = graphicsPipeline_->GetPipelineState(kBlendModeAdd);
            }
            if (fwPso) {
                commandList->SetPipelineState(fwPso);
                
                particleManager_->Draw(
                    commandList,
                    particleModel_.get(),
                    ringModel_.get(),
                    cylinderModel_.get(),
                    textureSrvHandle_,
                    gradationSrvHandle_,
                    textSrvHandle_
                );
            }
        }
    }
}
