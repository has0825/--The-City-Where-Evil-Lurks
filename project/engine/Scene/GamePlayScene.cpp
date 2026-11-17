#include "GamePlayScene.h"
#include "WinApp.h"
#include "SceneManager.h"
#include "LoadingScreen.h"
#include "D3D12Util.h"
#include "MathUtil.h"
#include "DataTypes.h"
#include "TextureManager.h"
#include "SrvManager.h"
#include "DirectXCommon.h"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <filesystem>
#include <cmath>

#ifdef USE_IMGUI
#include "externals/imgui/imgui.h"
#include "externals/imgui/imgui_impl_dx12.h"
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif


namespace {
    const std::string kSkyboxTextures[] = {
        "dds/moonless_golf_2k.dds",
        "dds/qwantani_night_puresky_2k.dds"
    };
    const size_t kNumSkyboxTextures = sizeof(kSkyboxTextures) / sizeof(kSkyboxTextures[0]);
    const float kWorldShiftX = 0.0f;

    Matrix4x4 MakeLookRotationMatrix(const Vector3& direction) {
        Vector3 forward = Normalize(direction);
        if (std::abs(forward.x) < 0.0001f && std::abs(forward.y) < 0.0001f && std::abs(forward.z) < 0.0001f) {
            return MakeIdentity4x4();
        }
        Vector3 upTemp = { 0.0f, 1.0f, 0.0f };
        if (std::abs(forward.y) > 0.99f) {
            upTemp = { 0.0f, 0.0f, 1.0f };
        }
        Vector3 right = Normalize(Cross(upTemp, forward));
        Vector3 up = Cross(forward, right);

        Matrix4x4 rot = MakeIdentity4x4();
        rot.m[0][0] = right.x;   rot.m[0][1] = right.y;   rot.m[0][2] = right.z;
        rot.m[1][0] = up.x;      rot.m[1][1] = up.y;      rot.m[1][2] = up.z;
        rot.m[2][0] = forward.x; rot.m[2][1] = forward.y; rot.m[2][2] = forward.z;
        return rot;
    }
}

// staticメンバの定義
int GamePlayScene::blenderSyncCounter_ = 0;

static bool IsCollidingOBBAndSphere(const Vector3& spherePos, float sphereRadius,
                                     const Vector3& boxPos, const Vector3& boxScale, float boxRotY,
                                     const Vector3& colCenter, const Vector3& colSize) {
    // 1. 球の位置をボックスの位置・Y回転のローカル空間に変換する（スケールは適用しない）
    Vector3 diff = { spherePos.x - boxPos.x, spherePos.y - boxPos.y, spherePos.z - boxPos.z };
    
    // Y軸回転 of 逆回転
    float cosRot = std::cos(-boxRotY);
    float sinRot = std::sin(-boxRotY);
    Vector3 localSpherePos;
    localSpherePos.x = diff.x * cosRot - diff.z * sinRot;
    localSpherePos.y = diff.y;
    localSpherePos.z = diff.x * sinRot + diff.z * cosRot;
    
    // 2. ボックスのスケールを適用したワールド空間でのコライダーAABBの中心と半サイズを計算
    // Blender側では colSize が半幅(ハーフサイズ)として扱われている
    Vector3 halfSize = { colSize.x * boxScale.x, colSize.y * boxScale.y, colSize.z * boxScale.z };
    Vector3 center = { colCenter.x * boxScale.x, colCenter.y * boxScale.y, colCenter.z * boxScale.z };
    
    // 3. ローカル空間でのAABBと球の衝突判定
    Vector3 closestPoint;
    closestPoint.x = (std::max)(center.x - halfSize.x, (std::min)(localSpherePos.x, center.x + halfSize.x));
    closestPoint.y = (std::max)(center.y - halfSize.y, (std::min)(localSpherePos.y, center.y + halfSize.y));
    closestPoint.z = (std::max)(center.z - halfSize.z, (std::min)(localSpherePos.z, center.z + halfSize.z));
    
    float distSq = (localSpherePos.x - closestPoint.x) * (localSpherePos.x - closestPoint.x) +
                   (localSpherePos.y - closestPoint.y) * (localSpherePos.y - closestPoint.y) +
                   (localSpherePos.z - closestPoint.z) * (localSpherePos.z - closestPoint.z);
                   
    return distSq <= (sphereRadius * sphereRadius);
}

static Vector3 Add(const Vector3& v1, const Vector3& v2) {
    return { v1.x + v2.x, v1.y + v2.y, v1.z + v2.z };
}
static Vector3 Subtract(const Vector3& v1, const Vector3& v2) {
    return { v1.x - v2.x, v1.y - v2.y, v1.z - v2.z };
}
static float Length(const Vector3& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}
static Vector3 Scale(const Vector3& v, float s) {
    return { v.x * s, v.y * s, v.z * s };
}
static Vector3 TransformNormal(const Vector3& v, const Matrix4x4& m) {
    Vector3 result;
    result.x = v.x * m.m[0][0] + v.y * m.m[1][0] + v.z * m.m[2][0];
    result.y = v.x * m.m[0][1] + v.y * m.m[1][1] + v.z * m.m[2][1];
    result.z = v.x * m.m[0][2] + v.y * m.m[1][2] + v.z * m.m[2][2];
    return result;
}

void GamePlayScene::Initialize() {
    isBossFightInitialized_ = false;
    // 既存のリプレイ録画ファイルを全て削除
    try {
        if (std::filesystem::exists("Resources")) {
            for (const auto& entry : std::filesystem::directory_iterator("Resources")) {
                if (entry.is_regular_file()) {
                    std::string filename = entry.path().filename().string();
                    if (filename == "replay_frames.csv" || 
                        (filename.rfind("replay_frames_", 0) == 0 && filename.rfind(".csv") == filename.length() - 4)) {
                        std::filesystem::remove(entry.path());
                    }
                }
            }
        }
    } catch (const std::exception&) {
        // エラー無視
    }
    replaySessionIndex_ = 0;

    // クリアカラーのリセット (デフォルトの青緑系に戻す)
    DirectXCommon::GetInstance()->SetClearColor(0.1f, 0.25f, 0.5f, 1.0f);

    // プレイヤーパラメータをファイルから読み込む
    std::ifstream paramFile("Resources/player_params.txt");
    if (paramFile.is_open()) {
        std::string line;
        while (std::getline(paramFile, line)) {
            if (line.empty() || line[0] == '#') continue;
            size_t pos = line.find('=');
            if (pos == std::string::npos) continue;
            std::string key = line.substr(0, pos);
            std::string val = line.substr(pos + 1);
            if (key == "LIMIT_X") playerLimitX_ = std::stof(val);
            else if (key == "LIMIT_Y") playerLimitY_ = std::stof(val);
            else if (key == "COLLISION_RADIUS") playerCollisionRadius_ = std::stof(val);
            else if (key == "SPEED_X") playerSpeedX_ = std::stof(val);
            else if (key == "SPEED_Y") playerSpeedY_ = std::stof(val);
        }
        paramFile.close();
    } else {
        // ファイルが無ければデフォルト値で作成しておく
        std::ofstream outfile("Resources/player_params.txt");
        if (outfile.is_open()) {
            outfile << "LIMIT_X=36.0\n";
            outfile << "LIMIT_Y=12.0\n";
            outfile << "COLLISION_RADIUS=2.0\n";
            outfile << "SPEED_X=35.0\n";
            outfile << "SPEED_Y=50.0\n";
            outfile.close();
        }
    }

    dxCommon_ = DirectXCommon::GetInstance();
    input_ = Input::GetInstance();
    ID3D12Device* device = dxCommon_->GetDevice();
    ID3D12GraphicsCommandList* commandList = dxCommon_->GetCommandList();

    audio_ = std::make_unique<Audio>();
    audio_->Initialize();
    bgmData_ = audio_->LoadAudio("resources/result.mp3");
    jumpSE_ = audio_->LoadAudio("resources/damage.mp3");

    graphicsPipeline_ = GraphicsPipeline::GetInstance();
    if (graphicsPipeline_) {
        graphicsPipeline_->Initialize(device);
    }

    srvDescriptorHeap_ = SrvManager::GetInstance()->GetDescriptorHeap();
    descriptorSizeSRV_ = SrvManager::GetInstance()->GetDescriptorSize();

    std::random_device seedGenerator;
    randomEngine_.seed(seedGenerator());

    camera_ = std::make_unique<Camera>(WinApp::kClientWidth, WinApp::kClientHeight);
    camera_->SetTranslate({ 0.0f, 0.0f, -15.0f });

    particleModel_ = std::unique_ptr<Model>(Model::CreateParticleModel(device));
    TextureManager::GetInstance()->LoadTexture("isihahen.png");
    ringModel_ = std::unique_ptr<Model>(Model::CreateRingModel(device));
    cylinderModel_ = std::unique_ptr<Model>(Model::CreateCylinderModel(device));

    particleManager_ = std::make_unique<ParticleManager>();
    particleManager_->Initialize(device);

    transformResource_ = CreateBufferResource(device, sizeof(TransformationMatrix));
    transformResource_->Map(0, nullptr, reinterpret_cast<void**>(&transformData_));
    transformData_->WVP = MakeIdentity4x4();
    transformData_->World = MakeIdentity4x4();
    transformData_->color = { 1.0f, 1.0f, 1.0f, 1.0f };

    directionalLightResource_ = CreateBufferResource(device, sizeof(DirectionalLight));
    directionalLightResource_->Map(0, nullptr, reinterpret_cast<void**>(&directionalLightData_));
    directionalLightData_->color = { 1.0f, 1.0f, 1.0f, 1.0f };
    directionalLightData_->direction = Normalize({ 0.0f, -1.0f, 1.0f });
    directionalLightData_->intensity = 1.0f;

    cameraResource_ = CreateBufferResource(device, sizeof(CameraDataCB));
    cameraResource_->Map(0, nullptr, reinterpret_cast<void**>(&cameraDataCB_));
    cameraDataCB_->worldPosition = { 0.0f, 0.0f, 0.0f };

    vignetteParamResource_ = CreateBufferResource(device, sizeof(VignetteParameter));
    vignetteParamResource_->Map(0, nullptr, reinterpret_cast<void**>(&vignetteParamData_));
    vignetteParamData_->scale = 16.0f;
    vignetteParamData_->power = 0.8f;

    boxFilterParamResource_ = CreateBufferResource(device, sizeof(BoxFilterParameter));
    boxFilterParamResource_->Map(0, nullptr, reinterpret_cast<void**>(&boxFilterParamData_));
    boxFilterParamData_->kernelSize = 2; // 5x5 default

    radialBlurParamResource_ = CreateBufferResource(device, sizeof(RadialBlurParameter));
    radialBlurParamResource_->Map(0, nullptr, reinterpret_cast<void**>(&radialBlurParamData_));
    radialBlurParamData_->center = { 0.5f, 0.5f };
    radialBlurParamData_->blurWidth = 0.01f;

    spriteInstancingResource_ = CreateBufferResource(device, sizeof(ParticleForGPU) * kSpriteInstanceCount);
    spriteInstancingResource_->Map(0, nullptr, reinterpret_cast<void**>(&spriteInstancingData_));

    DirectX::ScratchImage mipImages = LoadTexture("resources/circle2.png");
    const DirectX::TexMetadata& metadata = mipImages.GetMetadata();
    textureResource_ = CreateTextureResource(device, metadata);
    intermediateResource_ = UploadTextureData(textureResource_.Get(), mipImages, device, commandList);

    DirectX::ScratchImage noiseImages = LoadTexture("resources/noise1.png");
    const DirectX::TexMetadata& noiseMetadata = noiseImages.GetMetadata();
    noise1Resource_ = CreateTextureResource(device, noiseMetadata);
    noise1IntermediateResource_ = UploadTextureData(noise1Resource_.Get(), noiseImages, device, commandList);
    noise1SrvIndex_ = SrvManager::GetInstance()->Allocate();
    SrvManager::GetInstance()->CreateSRVforTexture2D(noise1SrvIndex_, noise1Resource_.Get(), noiseMetadata.format, 1);

    DirectX::ScratchImage noise0Images = LoadTexture("resources/noise0.png");
    const DirectX::TexMetadata& noise0Metadata = noise0Images.GetMetadata();
    noise0Resource_ = CreateTextureResource(device, noise0Metadata);
    noise0IntermediateResource_ = UploadTextureData(noise0Resource_.Get(), noise0Images, device, commandList);
    noise0SrvIndex_ = SrvManager::GetInstance()->Allocate();
    SrvManager::GetInstance()->CreateSRVforTexture2D(noise0SrvIndex_, noise0Resource_.Get(), noise0Metadata.format, 1);

    // 初期は noise1 を使用
    activeNoiseSrvIndex_ = noise1SrvIndex_;
    selectedNoiseIndex_ = 1;

    dissolveParamResource_ = CreateBufferResource(device, sizeof(DissolveParameter));
    dissolveParamResource_->Map(0, nullptr, reinterpret_cast<void**>(&dissolveParamData_));
    dissolveParamData_->threshold = 1.0f;
    dissolveParamData_->edgeColor = { 1.0f, 1.0f, 1.0f };
    dissolveParamData_->edgeRange = 0.03f;

    randomParamResource_ = CreateBufferResource(device, sizeof(RandomParameter));
    randomParamResource_->Map(0, nullptr, reinterpret_cast<void**>(&randomParamData_));
    randomParamData_->time = 0.0f;
    randomParamData_->noiseScale = 100.0f;
    randomParamData_->noiseStrength = 1.0f;
    randomParamData_->isColorNoise = 0.0f;
    randomParamData_->isMultiplyNoise = 0.0f;
    randomEffectTime_ = 0.0f;
    randomNoiseScale_ = 100.0f;
    randomNoiseStrength_ = 1.0f;
    randomSpeed_ = 1.0f;
    randomIsColorNoise_ = false;
    randomNoiseType_ = 0;
    
    isTransitioning_ = false;
    transitionThreshold_ = 0.0f;
    activePostProcess_ = kNone;
    hasStartedInitialPhase_ = false;
    phaseIntroTimer_ = -1.0f;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = metadata.format;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = UINT(metadata.mipLevels);

    uint32_t textureSrvIndex = SrvManager::GetInstance()->Allocate();
    D3D12_CPU_DESCRIPTOR_HANDLE textureSrvHandleCPU = SrvManager::GetInstance()->GetCPUDescriptorHandle(textureSrvIndex);
    textureSrvHandleGPU_ = SrvManager::GetInstance()->GetGPUDescriptorHandle(textureSrvIndex);
    device->CreateShaderResourceView(textureResource_.Get(), &srvDesc, textureSrvHandleCPU);



    DirectX::ScratchImage textMipImages = LoadTexture("resources/text1.png");
    const DirectX::TexMetadata& textMetadata = textMipImages.GetMetadata();
    textTextureResource_ = CreateTextureResource(device, textMetadata);
    textIntermediateResource_ = UploadTextureData(textTextureResource_.Get(), textMipImages, device, commandList);

    D3D12_SHADER_RESOURCE_VIEW_DESC textSrvDesc{};
    textSrvDesc.Format = textMetadata.format;
    textSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    textSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    textSrvDesc.Texture2D.MipLevels = UINT(textMetadata.mipLevels);

    uint32_t textSrvIndex = SrvManager::GetInstance()->Allocate();
    D3D12_CPU_DESCRIPTOR_HANDLE textSrvHandleCPU = SrvManager::GetInstance()->GetCPUDescriptorHandle(textSrvIndex);
    textSrvHandleGPU_ = SrvManager::GetInstance()->GetGPUDescriptorHandle(textSrvIndex);
    device->CreateShaderResourceView(textTextureResource_.Get(), &textSrvDesc, textSrvHandleCPU);

    D3D12_SHADER_RESOURCE_VIEW_DESC spriteInstancingSrvDesc{};
    spriteInstancingSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
    spriteInstancingSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    spriteInstancingSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    spriteInstancingSrvDesc.Buffer.FirstElement = 0;
    spriteInstancingSrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    spriteInstancingSrvDesc.Buffer.NumElements = kSpriteInstanceCount;
    spriteInstancingSrvDesc.Buffer.StructureByteStride = sizeof(ParticleForGPU);

    uint32_t spriteInstancingSrvIndex = SrvManager::GetInstance()->Allocate();
    D3D12_CPU_DESCRIPTOR_HANDLE spriteInstancingSrvHandleCPU = SrvManager::GetInstance()->GetCPUDescriptorHandle(spriteInstancingSrvIndex);
    spriteInstancingSrvHandleGPU_ = SrvManager::GetInstance()->GetGPUDescriptorHandle(spriteInstancingSrvIndex);
    device->CreateShaderResourceView(spriteInstancingResource_.Get(), &spriteInstancingSrvDesc, spriteInstancingSrvHandleCPU);



    // gradationLine.png の読み込みとSRV作成
    DirectX::ScratchImage gradationMipImages = LoadTexture("resources/gradationLine.png");
    const DirectX::TexMetadata& gradationMetadata = gradationMipImages.GetMetadata();
    gradationTextureResource_ = CreateTextureResource(device, gradationMetadata);
    gradationIntermediateResource_ = UploadTextureData(gradationTextureResource_.Get(), gradationMipImages, device, commandList);

    D3D12_SHADER_RESOURCE_VIEW_DESC gradationSrvDesc{};
    gradationSrvDesc.Format = gradationMetadata.format;
    gradationSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    gradationSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    gradationSrvDesc.Texture2D.MipLevels = UINT(gradationMetadata.mipLevels);

    uint32_t gradationSrvIndex = SrvManager::GetInstance()->Allocate();
    D3D12_CPU_DESCRIPTOR_HANDLE gradationSrvHandleCPU = SrvManager::GetInstance()->GetCPUDescriptorHandle(gradationSrvIndex);
    gradationSrvHandleGPU_ = SrvManager::GetInstance()->GetGPUDescriptorHandle(gradationSrvIndex);
    device->CreateShaderResourceView(gradationTextureResource_.Get(), &gradationSrvDesc, gradationSrvHandleCPU);



    if (!isTitleMode_) {
        audio_->PlayWave(bgmData_, true, 0.5f);
    }

    TextureManager::GetInstance()->Initialize(device, "Resources/");
    TextureManager::GetInstance()->LoadTexture("test.dds");
    TextureManager::GetInstance()->LoadTexture("circle2.png");
    TextureManager::GetInstance()->LoadTexture("gradationLine.png");
    TextureManager::GetInstance()->LoadTexture("douro.jpg");
    TextureManager::GetInstance()->LoadTexture("human/white.png");
    TextureManager::GetInstance()->LoadTexture("aiming.png");
    TextureManager::GetInstance()->LoadTexture("Player2/Player_basecolor.JPEG");
    TextureManager::GetInstance()->LoadTexture("Player/player.png");
    TextureManager::GetInstance()->LoadTexture("cobblestone_street_night_2k.dds");
    TextureManager::GetInstance()->LoadTexture("rostock_laage_airport_4k.dds");
    TextureManager::GetInstance()->LoadTexture("dds/moonless_golf_2k.dds");
    TextureManager::GetInstance()->LoadTexture("dds/qwantani_night_puresky_2k.dds");
    TextureManager::GetInstance()->LoadTexture("test.dds");
    TextureManager::GetInstance()->LoadTexture("tesuto.dds");

    skybox_ = std::make_unique<Skybox>();
    skybox_->Initialize(device);

    playerModel_ = Model::LoadGLTF("Resources/human/walk.gltf", device);
    fighterModel_ = Model::LoadGLTF("Resources/Player2/Player.obj", device);
    if (fighterModel_) {
        fighterModel_->transform.scale = { 10.0f, 10.0f, 10.0f };
    }
    enemyModel_ = Model::LoadGLTF("Resources/Player/player.obj", device);
    if (enemyModel_) {
        enemyModel_->transform.scale = { 10.0f, 10.0f, 10.0f };
    }
    
    // 戦闘機用トランスフォーム
    fighterTransformResource_ = CreateBufferResource(device, sizeof(TransformationMatrix));
    fighterTransformResource_->Map(0, nullptr, reinterpret_cast<void**>(&fighterTransformData_));
    fighterTransformData_->WVP = MakeIdentity4x4();
    fighterTransformData_->World = MakeIdentity4x4();
    fighterTransformData_->color = { 1.0f, 1.0f, 1.0f, 1.0f };

    // 弾用トランスフォーム
    playerBullets_.resize(kMaxBullets);
    for (uint32_t i = 0; i < kMaxBullets; ++i) {
        bulletTransformResources_[i] = CreateBufferResource(device, sizeof(TransformationMatrix));
        bulletTransformResources_[i]->Map(0, nullptr, reinterpret_cast<void**>(&bulletTransformData_[i]));
        bulletTransformData_[i]->WVP = MakeIdentity4x4();
        bulletTransformData_[i]->World = MakeIdentity4x4();
        bulletTransformData_[i]->color = { 1.0f, 1.0f, 1.0f, 1.0f };
    }

    // トドメ巨大弾用トランスフォーム
    defeatBulletTransformResource_ = CreateBufferResource(device, sizeof(TransformationMatrix));
    defeatBulletTransformResource_->Map(0, nullptr, reinterpret_cast<void**>(&defeatBulletTransformData_));
    defeatBulletTransformData_->WVP = MakeIdentity4x4();
    defeatBulletTransformData_->World = MakeIdentity4x4();
    defeatBulletTransformData_->color = { 1.0f, 1.0f, 1.0f, 1.0f };

    // 死亡演出フラグの初期化
    isBossDefeatedSequence_ = false;
    bossDefeatTimer_ = 0.0f;
    isDefeatBulletActive_ = false;
    isBossModelVisible_ = true;
    hasHitBoss_ = false;
    bossDefeatHitTimer_ = 0.0f;

    // 敵小隊と敵の初期化（オブジェクトプール・無限フォーメーションスポーン式）

    enemies_.clear();
    std::uniform_int_distribution<int> distForm(0, (int)FormationType::kCount - 1);
    std::uniform_real_distribution<float> distX(-10.0f, 10.0f);
    std::uniform_real_distribution<float> distY(-5.0f, 10.0f);

    for (int g = 0; g < kNumGroups; ++g) {
        enemyGroups_[g].formation = (FormationType)distForm(randomEngine_);
        enemyGroups_[g].centerX = distX(randomEngine_);
        enemyGroups_[g].centerY = distY(randomEngine_);
        enemyGroups_[g].centerZ = 250.0f + (float)g * 300.0f; // 250m, 550m, 850m
        enemyGroups_[g].centerRailProgress = enemyGroups_[g].centerZ; // レール空間でも同じ初期値
    }

    for (int i = 0; i < kMaxEnemies; ++i) {
        enemyTransformResources_[i] = CreateBufferResource(device, sizeof(TransformationMatrix));
        enemyTransformResources_[i]->Map(0, nullptr, reinterpret_cast<void**>(&enemyTransformData_[i]));
        enemyTransformData_[i]->WVP = MakeIdentity4x4();
        enemyTransformData_[i]->World = MakeIdentity4x4();
        enemyTransformData_[i]->color = { 1.0f, 1.0f, 1.0f, 1.0f };

        Enemy enemy;
        enemy.groupIndex = i / kEnemiesPerGroup;
        enemy.memberIndex = i % kEnemiesPerGroup;
        enemy.scale = { 3.8f, 3.8f, 3.8f };
        enemy.rotate = { 0.0f, 0.0f, 0.0f }; // モデルの向きを180度反転して修正
        enemy.isAlive = false;
        enemy.radius = 5.0f;
        enemy.hp = 30.0f;
        enemy.maxHP = 30.0f;
        enemies_.push_back(enemy);
    }

    // 最初はすべての小隊メンバーを生存（Alive）にして初期フォーメーション位置を適用
    // (Z距離がそれぞれ 250m, 550m に分散されます)
    for (int i = 0; i < kMaxEnemies; ++i) {
        enemies_[i].isAlive = true;
    }
    activeGroupIndex_ = 0;
    for (int g = 0; g < kNumGroups; ++g) {
        ApplyGroupFormation(g);
    }

    // ── ビル(Building)の初期化とバッファ生成 ──
    buildingModel_ = std::unique_ptr<Model>(Model::LoadGLTF("Resources/building/building.obj", device));
    TextureManager::GetInstance()->LoadTexture("building/buillding_uv.png");

    // ビル用定数バッファの一括生成（30,000個のリソース割り当てを1回にまとめて爆速化）
    const UINT cbAlignment = 256;
    buildingTransformBufferResource_ = CreateBufferResource(device, cbAlignment * kMaxBuildingCBs);
    uint8_t* buildingMappedPtr = nullptr;
    buildingTransformBufferResource_->Map(0, nullptr, reinterpret_cast<void**>(&buildingMappedPtr));

    for (int i = 0; i < kMaxBuildingCBs; ++i) {
        buildingTransformData_[i] = reinterpret_cast<TransformationMatrix*>(buildingMappedPtr + i * cbAlignment);
        buildingTransformData_[i]->WVP = MakeIdentity4x4();
        buildingTransformData_[i]->World = MakeIdentity4x4();
        buildingTransformData_[i]->color = { 1.0f, 1.0f, 1.0f, 1.0f };
    }

    buildings_.clear();
    float kBuildingInterval = 80.0f;
    for (int i = 0; i < kMaxBuildings; ++i) {
        Building building;
        
        // 4列に並べる: 0:内側左, 1:内側右, 2:外側左, 3:外側右
        int columnType = i % 4;
        if (columnType == 0) building.position.x = -45.0f;
        else if (columnType == 1) building.position.x = 45.0f;
        else if (columnType == 2) building.position.x = -85.0f;
        else building.position.x = 85.0f;
        
        // 積み重ねる階数をランダム設定（内側は1〜5階、外側は5〜10階）
        if (columnType == 2 || columnType == 3) {
            building.floors = 5 + (randomEngine_() % 6); // 外側のビルは高め
        } else {
            building.floors = 1 + (randomEngine_() % 5); // 内側のビル
        }
        building.scale = { 10.0f, 10.0f, 10.0f }; // ビル1階分の等倍スケール
        building.rotate = { 0.0f, 0.0f, 0.0f };
        
        int groupIndex = i / 4;
        building.position.z = 50.0f + (float)groupIndex * kBuildingInterval;
        // 積み重ねるため、個々のY座標は Update 内で計算されるため、基準値のみ設定
        building.position.y = kFloorY; 

        // 破壊演出用変数の初期化
        building.isDestroyed = false;
        building.velocity = { 0.0f, 0.0f, 0.0f };
        building.rotationSpeed = { 0.0f, 0.0f, 0.0f };
        building.destroyTimer = 0.0f;

        buildings_.push_back(building);
    }

    // ── 床(Plane)の初期化とバッファ生成（4,000個のリソース割り当てを1回にまとめて爆速化）──
    floorModel_ = std::unique_ptr<Model>(Model::LoadGLTF("Resources/plane.obj", device));
    TextureManager::GetInstance()->LoadTexture("douro.jpg");
    TextureManager::GetInstance()->LoadTexture("juziro.jpg");

    floorTransformBufferResource_ = CreateBufferResource(device, cbAlignment * kNumFloors);
    uint8_t* floorMappedPtr = nullptr;
    floorTransformBufferResource_->Map(0, nullptr, reinterpret_cast<void**>(&floorMappedPtr));

    for (int i = 0; i < kNumFloors; ++i) {
        floorTransformData_[i] = reinterpret_cast<TransformationMatrix*>(floorMappedPtr + i * cbAlignment);
        floorTransformData_[i]->WVP   = MakeIdentity4x4();
        floorTransformData_[i]->World = MakeIdentity4x4();
        floorTransformData_[i]->color = { 1.0f, 1.0f, 1.0f, 1.0f };

        floorPositions_[i] = { 0.0f, -10000.0f, 0.0f };
        floorRotations_[i] = { 0.0f, 0.0f, 0.0f };
        floorLanes_[i] = -1;
        floorProgresses_[i] = 999999.0f;
    }

    // 広域地面ベース用定数バッファの生成とマッピング
    groundBaseTransformResource_ = CreateBufferResource(device, sizeof(TransformationMatrix));
    groundBaseTransformResource_->Map(0, nullptr, reinterpret_cast<void**>(&groundBaseTransformData_));
    groundBaseTransformData_->WVP   = MakeIdentity4x4();
    groundBaseTransformData_->World = MakeIdentity4x4();
    groundBaseTransformData_->color = { 1.0f, 1.0f, 1.0f, 1.0f };

    // ── 天球(SkyDome)の初期化とバッファ生成 ──
    // 球の内側からテクスチャを見る形式のため、カリングが逆向き of モデルを使用
    TextureManager::GetInstance()->LoadTexture("haikei/Green.png");
    TextureManager::GetInstance()->LoadTexture("haikei/Red.png");
    skydomeModel_ = std::unique_ptr<Model>(Model::LoadGLTF("Resources/skydome/SkyDome.obj", device));
    
    // 天球のライティングを無効化 (Unlit)
    ID3D12Resource* skydomeMaterialRes = skydomeModel_->GetMaterialResource();
    if (skydomeMaterialRes) {
        ::Material* materialData = nullptr;
        skydomeMaterialRes->Map(0, nullptr, reinterpret_cast<void**>(&materialData));
        if (materialData) {
            materialData->enableLighting = 0;
        }
        skydomeMaterialRes->Unmap(0, nullptr);
    }

    skydomeTransformRes_ = CreateBufferResource(device, sizeof(TransformationMatrix));
    skydomeTransformRes_->Map(0, nullptr, reinterpret_cast<void**>(&skydomeTransformData_));
    skydomeTransformData_->WVP   = MakeIdentity4x4();
    skydomeTransformData_->World = MakeIdentity4x4();
    skydomeTransformData_->color = { 1.0f, 1.0f, 1.0f, 1.0f };
    activeBackgroundTex_ = 0;
    bgUvScrollX_ = 0.0f;
    bgUvScrollY_ = 0.0f;

    // レティクル(エイミング)用バッファ
    aimingInstancingResource_ = CreateBufferResource(device, sizeof(ParticleForGPU));
    aimingInstancingResource_->Map(0, nullptr, reinterpret_cast<void**>(&aimingInstancingData_));
    aimingInstancingData_->WVP = MakeIdentity4x4();
    aimingInstancingData_->World = MakeIdentity4x4();
    aimingInstancingData_->uvTransform = MakeIdentity4x4();
    aimingInstancingData_->color = { 1.0f, 1.0f, 1.0f, 1.0f };
    
    D3D12_SHADER_RESOURCE_VIEW_DESC aimingInstancingSrvDesc{};
    aimingInstancingSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
    aimingInstancingSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    aimingInstancingSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    aimingInstancingSrvDesc.Buffer.FirstElement = 0;
    aimingInstancingSrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    aimingInstancingSrvDesc.Buffer.NumElements = 1;
    aimingInstancingSrvDesc.Buffer.StructureByteStride = sizeof(ParticleForGPU);

    uint32_t aimingInstancingSrvIndex = SrvManager::GetInstance()->Allocate();
    D3D12_CPU_DESCRIPTOR_HANDLE aimingInstancingSrvHandleCPU = SrvManager::GetInstance()->GetCPUDescriptorHandle(aimingInstancingSrvIndex);
    aimingInstancingSrvHandleGPU_ = SrvManager::GetInstance()->GetGPUDescriptorHandle(aimingInstancingSrvIndex);
    device->CreateShaderResourceView(aimingInstancingResource_.Get(), &aimingInstancingSrvDesc, aimingInstancingSrvHandleCPU);
    
    reticleMaterialResource_ = CreateBufferResource(device, sizeof(Material));
    reticleMaterialResource_->Map(0, nullptr, reinterpret_cast<void**>(&reticleMaterialData_));
    reticleMaterialData_->color = {1.0f, 1.0f, 1.0f, 1.0f};
    reticleMaterialData_->uvTransform = MakeIdentity4x4();

    // HPバー用のバッファ生成とマッピング
    hpBarInstancingResource_ = CreateBufferResource(device, sizeof(ParticleForGPU) * kHpBarInstanceCount);
    hpBarInstancingResource_->Map(0, nullptr, reinterpret_cast<void**>(&hpBarInstancingData_));
    for (uint32_t i = 0; i < kHpBarInstanceCount; ++i) {
        hpBarInstancingData_[i].WVP = MakeIdentity4x4();
        hpBarInstancingData_[i].World = MakeIdentity4x4();
        hpBarInstancingData_[i].color = { 1.0f, 1.0f, 1.0f, 1.0f };
        hpBarInstancingData_[i].uvTransform = MakeIdentity4x4();
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC hpBarInstancingSrvDesc{};
    hpBarInstancingSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
    hpBarInstancingSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    hpBarInstancingSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    hpBarInstancingSrvDesc.Buffer.FirstElement = 0;
    hpBarInstancingSrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    hpBarInstancingSrvDesc.Buffer.NumElements = kHpBarInstanceCount;
    hpBarInstancingSrvDesc.Buffer.StructureByteStride = sizeof(ParticleForGPU);

    uint32_t hpBarInstancingSrvIndex = SrvManager::GetInstance()->Allocate();
    D3D12_CPU_DESCRIPTOR_HANDLE hpBarInstancingSrvHandleCPU = SrvManager::GetInstance()->GetCPUDescriptorHandle(hpBarInstancingSrvIndex);
    hpBarInstancingSrvHandleGPU_ = SrvManager::GetInstance()->GetGPUDescriptorHandle(hpBarInstancingSrvIndex);
    device->CreateShaderResourceView(hpBarInstancingResource_.Get(), &hpBarInstancingSrvDesc, hpBarInstancingSrvHandleCPU);



    // HP関連変数の初期化
    playerHP_ = 100.0f;
    playerMaxHP_ = 100.0f;
    bossHP_ = 500.0f;
    bossMaxHP_ = 500.0f;
    bossCollisionRadius_ = 27.0f;
    bossWebFireOffset_ = { 0.0f, 27.0f, 36.0f }; // 糸レーザーをお尻（上・奥）から発射するための初期オフセット

    // 【重要】モデルのスケール・座標設定
    // ※もし画面に見えない場合は、ここ(scale)を 10.0f や 100.0f など大きくしてみてください。
    playerModel_->transform.scale = { 10.0f, 10.0f, 10.0f };
    playerModel_->transform.translate = { 0.0f, 0.0f, 0.0f };
    playerModel_->transform.rotate = { 0.0f, 0.0f, 0.0f };
    playerModel_->SetEnvironmentCoefficient(modelEnvCoefficient_);

    // simpleSkinの初期化
    cubeModel_ = AdvAnim::LoadModelFile("Resources/simpleSkin", "simpleSkin.gltf");
    cubeAnimation_ = AdvAnim::LoadAnimationFile("Resources/simpleSkin", "simpleSkin.gltf");
    cubeSkeleton_ = AdvAnim::CreateSkeleton(cubeModel_.rootNode);
    cubeSkinCluster_ = AdvAnim::CreateSkinCluster(device, cubeSkeleton_, cubeModel_.modelData, TextureManager::GetInstance()->GetSrvHeap(), dxCommon_->GetDescriptorSizeSRV());

    cubeRenderModel_ = std::make_unique<Model>();
    cubeRenderModel_->Initialize(cubeModel_.modelData, device);
    cubeRenderModel_->bones_ = cubeModel_.bones;
    
    // 定数バッファの生成
    cubeTransformResource_ = CreateBufferResource(device, sizeof(TransformationMatrix));
    cubeTransformResource_->Map(0, nullptr, reinterpret_cast<void**>(&cubeTransformData_));
    cubeTransformData_->WVP = MakeIdentity4x4();
    cubeTransformData_->World = MakeIdentity4x4();
    cubeTransformData_->color = { 1.0f, 1.0f, 1.0f, 1.0f };

    // AnimatedCubeの初期化
    animatedCubeModel_ = AdvAnim::LoadModelFile("Resources/AnimatedCube", "AnimatedCube.gltf");
    animatedCubeAnimation_ = AdvAnim::LoadAnimationFile("Resources/AnimatedCube", "AnimatedCube.gltf");
    animatedCubeSkeleton_ = AdvAnim::CreateSkeleton(animatedCubeModel_.rootNode);

    animatedCubeRenderModel_ = std::make_unique<Model>();
    animatedCubeRenderModel_->Initialize(animatedCubeModel_.modelData, device);
    animatedCubeRenderModel_->bones_ = animatedCubeModel_.bones;
    
    animatedCubeTransformResource_ = CreateBufferResource(device, sizeof(TransformationMatrix));
    animatedCubeTransformResource_->Map(0, nullptr, reinterpret_cast<void**>(&animatedCubeTransformData_));
    animatedCubeTransformData_->WVP = MakeIdentity4x4();
    animatedCubeTransformData_->World = MakeIdentity4x4();
    animatedCubeTransformData_->color = { 1.0f, 1.0f, 1.0f, 1.0f };

    // テクスチャの読み込み
    TextureManager::GetInstance()->LoadTexture("AnimatedCube/AnimatedCube_BaseColor.png");

    // デバッグ描画用の初期化
    debugSphereModel_ = Model::CreateSphereModel(device);
    debugSphereModel_->SetColor({ 0.0f, 1.0f, 1.0f, 1.0f }); // ネオンシアン (高視認性)
    debugBoxModel_ = Model::CreateBoxModel(device);
    debugBoxModel_->SetLightingEnabled(false);
    debugBoxModel_->SetColor({ 0.45f, 1.0f, 0.10f, 1.0f }); // 青色
    for (uint32_t i = 0; i < kMaxDebugInstances; ++i) {
        debugTransformResources_[i] = CreateBufferResource(device, sizeof(TransformationMatrix));
        debugTransformResources_[i]->Map(0, nullptr, reinterpret_cast<void**>(&debugTransformData_[i]));
        debugTransformData_[i]->WVP = MakeIdentity4x4();
        debugTransformData_[i]->World = MakeIdentity4x4();
        debugTransformData_[i]->color = { 1.0f, 1.0f, 1.0f, 1.0f };
    }

    // removed isCameraMode_ = false;

    gpuParticleManager_ = std::make_unique<GpuParticleManager>();
    gpuParticleManager_->Initialize(device);

    postProcess_ = std::make_unique<PostProcess>();
    postProcess_->Initialize(dxCommon_, WinApp::kClientWidth, WinApp::kClientHeight);

    // 深度バッファ用のSRVを生成
    depthSrvIndex_ = SrvManager::GetInstance()->Allocate();
    SrvManager::GetInstance()->CreateSRVforTexture2D(
        depthSrvIndex_, 
        dxCommon_->GetDepthStencilResource(), 
        DXGI_FORMAT_R24_UNORM_X8_TYPELESS, // 深度値のサンプリング用フォーマット
        1
    );

    gpuParticleManager_->SetTranslate({ -10.0f, 0.0f, 0.0f });

    // プレイヤーのワールドZ座標を初期化（カメラ初期Z=-15より65ユニット前に配置）
    fighterWorldZ_ = camera_->GetTransform().translate.z + 65.0f;

    // ── 蜘蛛ボス（Big Spider）の初期化 ──
    // テクスチャのロード
    TextureManager::GetInstance()->LoadTexture("big Spider/big+Spider_basecolor.jpg");
    TextureManager::GetInstance()->LoadTexture("big Spider/big+spider+arm_basecolor.jpg");

    // ボスモデルのロード (元の高画質オリジナルモデルをロード)
    bossBodyModel_ = Model::LoadGLTF("Resources/big Spider/big+Spider.obj", device);
    bossLegModel_ = Model::LoadGLTF("Resources/big Spider/big+spider+arm.obj", device);

    // 胴体用の定数バッファ生成とマップ
    bossBodyTransformResource_ = CreateBufferResource(device, sizeof(TransformationMatrix));
    bossBodyTransformResource_->Map(0, nullptr, reinterpret_cast<void**>(&bossBodyTransformData_));
    bossBodyTransformData_->WVP = MakeIdentity4x4();
    bossBodyTransformData_->World = MakeIdentity4x4();
    bossBodyTransformData_->color = { 1.0f, 1.0f, 1.0f, 1.0f };

    // 8本の足用の定数バッファ生成とマップ
    for (int i = 0; i < 8; ++i) {
        bossLegTransformResources_[i] = CreateBufferResource(device, sizeof(TransformationMatrix));
        bossLegTransformResources_[i]->Map(0, nullptr, reinterpret_cast<void**>(&bossLegTransformData_[i]));
        bossLegTransformData_[i]->WVP = MakeIdentity4x4();
        bossLegTransformData_[i]->World = MakeIdentity4x4();
        bossLegTransformData_[i]->color = { 1.0f, 1.0f, 1.0f, 1.0f };
    }

    // 蜘蛛の巣テクスチャのロードとSRV取得
    TextureManager::GetInstance()->LoadTexture("spider web.png");
    spiderWebSrvHandleGPU_ = TextureManager::GetInstance()->GetSrvHandleGPU("spider web.png");

    // 画面蜘蛛の巣用バッファ生成とマッピング
    screenWebTransformResource_ = CreateBufferResource(device, sizeof(ParticleForGPU));
    screenWebTransformResource_->Map(0, nullptr, reinterpret_cast<void**>(&screenWebTransformData_));
    screenWebTransformData_->WVP = MakeIdentity4x4();
    screenWebTransformData_->World = MakeIdentity4x4();
    screenWebTransformData_->color = { 1.0f, 1.0f, 1.0f, 0.0f }; // 初期は完全透明
    screenWebTransformData_->uvTransform = MakeIdentity4x4();

    // 画面蜘蛛の巣用のSRV作成
    D3D12_SHADER_RESOURCE_VIEW_DESC screenWebSrvDesc{};
    screenWebSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
    screenWebSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    screenWebSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    screenWebSrvDesc.Buffer.FirstElement = 0;
    screenWebSrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    screenWebSrvDesc.Buffer.NumElements = 1;
    screenWebSrvDesc.Buffer.StructureByteStride = sizeof(ParticleForGPU);

    uint32_t screenWebSrvIndex = SrvManager::GetInstance()->Allocate();
    D3D12_CPU_DESCRIPTOR_HANDLE screenWebSrvHandleCPU = SrvManager::GetInstance()->GetCPUDescriptorHandle(screenWebSrvIndex);
    screenWebSrvHandleGPU_ = SrvManager::GetInstance()->GetGPUDescriptorHandle(screenWebSrvIndex);
    device->CreateShaderResourceView(screenWebTransformResource_.Get(), &screenWebSrvDesc, screenWebSrvHandleCPU);

    // 蜘蛛の巣弾（3D）用バッファ生成とマッピング
    webBulletInstancingResource_ = CreateBufferResource(device, sizeof(ParticleForGPU) * kMaxWebBullets);
    webBulletInstancingResource_->Map(0, nullptr, reinterpret_cast<void**>(&webBulletInstancingData_));
    for (int i = 0; i < kMaxWebBullets; ++i) {
        webBulletInstancingData_[i].WVP = MakeIdentity4x4();
        webBulletInstancingData_[i].World = MakeIdentity4x4();
        webBulletInstancingData_[i].color = { 1.0f, 1.0f, 1.0f, 1.0f };
        webBulletInstancingData_[i].uvTransform = MakeIdentity4x4();
    }

    // 蜘蛛の巣弾用のSRV作成
    D3D12_SHADER_RESOURCE_VIEW_DESC webBulletSrvDesc{};
    webBulletSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
    webBulletSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    webBulletSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    webBulletSrvDesc.Buffer.FirstElement = 0;
    webBulletSrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    webBulletSrvDesc.Buffer.NumElements = kMaxWebBullets;
    webBulletSrvDesc.Buffer.StructureByteStride = sizeof(ParticleForGPU);

    uint32_t webBulletSrvIndex = SrvManager::GetInstance()->Allocate();
    D3D12_CPU_DESCRIPTOR_HANDLE webBulletSrvHandleCPU = SrvManager::GetInstance()->GetCPUDescriptorHandle(webBulletSrvIndex);
    webBulletInstancingSrvHandleGPU_ = SrvManager::GetInstance()->GetGPUDescriptorHandle(webBulletSrvIndex);
    device->CreateShaderResourceView(webBulletInstancingResource_.Get(), &webBulletSrvDesc, webBulletSrvHandleCPU);

    // 蜘蛛の巣弾オブジェクトプール初期化
    bossWebBullets_.resize(kMaxWebBullets);
    for (int i = 0; i < kMaxWebBullets; ++i) {
        bossWebBullets_[i].isAlive = false;
        bossWebBullets_[i].radius = 8.0f; // 巨大な蜘蛛の巣
    }

    // HP表示補間用初期化
    playerHPVisual_ = playerHP_;
    bossHPVisual_ = bossHP_;

    // 初期フェーズはフェーズ1スタート
    currentPhase_ = GamePhase::kPhase1;
    phaseTimer_ = 0.0f;
    bossTime_ = 0.0f;
    // タイトルモード時はPHASE演出を出さない。通常モードはディゾルブ完了後に呼ぶ
    if (isTitleMode_) {
        phaseIntroTimer_ = -1.0f; // 演出無効
    }

    /*
    // 現在の配置をBlender用にファイル出力する
    std::ofstream layoutFile("Resources/scene_layout.txt");
    if (layoutFile.is_open()) {
        float kFloorHeight = 10.0f;
        for (int i = 0; i < kMaxBuildings; ++i) {
            for (int f = 0; f < buildings_[i].floors; ++f) {
                Vector3 floorPos = buildings_[i].position;
                floorPos.y = -20.0f + (float)f * kFloorHeight + kFloorHeight * 0.5f;
                layoutFile << "BUILDING," 
                           << floorPos.x << "," << floorPos.y << "," << floorPos.z << ","
                           << buildings_[i].scale.x << "," << buildings_[i].scale.y << "," << buildings_[i].scale.z << ","
                           << buildings_[i].rotate.x << "," << buildings_[i].rotate.y << "," << buildings_[i].rotate.z << "\n";
            }
        }
        for (int i = 0; i < kNumFloors; ++i) {
            layoutFile << "FLOOR," 
                       << floorPositions_[i].x << "," << floorPositions_[i].y << "," << floorPositions_[i].z << ","
                       << 300.0f << "," << 1.0f << "," << kFloorSizeZ << "\n";
        }
        // 自機初期位置の出力
        layoutFile << "PLAYER," 
                   << 0.0f << "," << kFighterYOffset << "," << fighterWorldZ_ << ","
                   << 10.0f << "," << 10.0f << "," << 10.0f << ","
                   << 0.0f << "," << 0.0f << "," << 0.0f << "\n";
        // 敵の初期位置の出力
        for (int i = 0; i < kMaxEnemies; ++i) {
            if (enemies_[i].isAlive) {
                layoutFile << "ENEMY," 
                           << enemies_[i].position.x << "," << enemies_[i].position.y << "," << enemies_[i].position.z << ","
                           << enemies_[i].scale.x << "," << enemies_[i].scale.y << "," << enemies_[i].scale.z << ","
                           << enemies_[i].rotate.x << "," << enemies_[i].rotate.y << "," << enemies_[i].rotate.z << "\n";
            }
        }
        layoutFile.close();
    }
    */

    // ── scene_layout.txt のインポート処理 ──
    int floorCount = 0;
    std::ifstream layoutInFile("Resources/scene_layout.txt");
    if (layoutInFile.is_open()) {
        buildings_.clear();

        // すべての敵を一旦非アクティブ化
        for (int i = 0; i < kMaxEnemies; ++i) {
            enemies_[i].isAlive = false;
        }

        // すべての床を一旦画面外へ（描画非表示用）および回転クリア
        for (int i = 0; i < kNumFloors; ++i) {
            floorPositions_[i] = { 0.0f, -10000.0f, 0.0f };
            floorRotations_[i] = { 0.0f, 0.0f, 0.0f };
            floorLanes_[i] = -1;
            floorProgresses_[i] = 999999.0f;
        }

        waypoints_.clear();

        std::string line;
        int buildingCount = 0;
        int enemyCount = 0;

        while (std::getline(layoutInFile, line)) {
            if (line.empty()) continue;
            std::stringstream ss(line);
            std::string type;
            std::getline(ss, type, ',');

            if (type == "BUILDING") {
                std::string s_gx, s_gy, s_gz, s_sx, s_sy, s_sz, s_rx, s_ry, s_rz;
                std::getline(ss, s_gx, ',');
                std::getline(ss, s_gy, ',');
                std::getline(ss, s_gz, ',');
                std::getline(ss, s_sx, ',');
                std::getline(ss, s_sy, ',');
                std::getline(ss, s_sz, ',');
                std::getline(ss, s_rx, ',');
                std::getline(ss, s_ry, ',');
                std::getline(ss, s_rz, ',');

                float gx = std::stof(s_gx);
                float gy = std::stof(s_gy);
                float gz = std::stof(s_gz);
                float sx = std::stof(s_sx);
                float sz = std::stof(s_sz);
                float rx = s_rx.empty() ? 0.0f : std::stof(s_rx);
                float ry = s_ry.empty() ? 0.0f : std::stof(s_ry);
                float rz = s_rz.empty() ? 0.0f : std::stof(s_rz);

                // コライダー情報があれば読み取る
                std::string s_col_type, s_cx, s_cy, s_cz, s_csx, s_csy, s_csz;
                bool hasCollider = false;
                Vector3 colCenter = { 0.0f, 0.0f, 0.0f };
                Vector3 colSize = { 2.0f, 2.0f, 2.0f };
                
                // 無効フラグ
                bool isDisabled = false;
                std::string s_disabled;
                
                if (std::getline(ss, s_col_type, ',')) {
                    if (s_col_type == "BOX" &&
                        std::getline(ss, s_cx, ',') &&
                        std::getline(ss, s_cy, ',') &&
                        std::getline(ss, s_cz, ',') &&
                        std::getline(ss, s_csx, ',') &&
                        std::getline(ss, s_csy, ',') &&
                        std::getline(ss, s_csz, ',')) {
                        
                        hasCollider = true;
                        colCenter = { std::stof(s_cx), std::stof(s_cz), std::stof(s_cy) };
                        colSize = { std::stof(s_csx), std::stof(s_csz), std::stof(s_csy) };
                    } else if (s_col_type == "NONE") {
                        std::string dummy;
                        for (int d = 0; d < 6; ++d) {
                            std::getline(ss, dummy, ',');
                        }
                    }
                    
                    if (std::getline(ss, s_disabled, ',')) {
                        if (!s_disabled.empty() && std::stoi(s_disabled) == 1) {
                            isDisabled = true;
                        }
                    }
                }
                
                if (isDisabled) {
                    continue;
                }

                // 既にずれている座標と比較するため、読み込んだ gx にも kWorldShiftX を加算
                float shiftedGx = gx + kWorldShiftX;
                int foundIdx = -1;
                for (int i = 0; i < buildingCount; ++i) {
                    if (std::abs(buildings_[i].position.x - shiftedGx) < 1.0f &&
                        std::abs(buildings_[i].position.z - gz) < 1.0f) {
                        foundIdx = i;
                        break;
                    }
                }

                if (foundIdx != -1) {
                    buildings_[foundIdx].floors++;
                    buildings_[foundIdx].originalFloors = buildings_[foundIdx].floors;
                    if (hasCollider) {
                        buildings_[foundIdx].collider.hasCollider = true;
                        buildings_[foundIdx].collider.center = colCenter;
                        buildings_[foundIdx].collider.size = colSize;
                    }
                } else if (buildingCount < kMaxBuildings) {
                    Building b;
                    b.position = { shiftedGx, -20.0f, gz }; // 底面を基準高さ -20.0f
                    b.scale = { sx, 10.0f, sz };     
                    b.rotate = { rx, ry, rz };
                    b.floors = 1;
                    b.originalX = shiftedGx;
                    b.originalY = -20.0f;
                    b.originalFloors = 1;
                    b.isDestroyed = false;
                    b.velocity = { 0.0f, 0.0f, 0.0f };
                    b.rotationSpeed = { 0.0f, 0.0f, 0.0f };
                    b.destroyTimer = 0.0f;
                    b.collider.hasCollider = hasCollider;
                    b.collider.center = colCenter;
                    b.collider.size = colSize;

                    buildings_.push_back(b);
                    buildingCount = static_cast<int>(buildings_.size());
                }
            } else if (type == "ENEMY") {
                std::string s_gx, s_gy, s_gz;
                std::getline(ss, s_gx, ',');
                std::getline(ss, s_gy, ',');
                std::getline(ss, s_gz, ',');

                float gx = std::stof(s_gx);
                float gy = std::stof(s_gy);
                float gz = std::stof(s_gz);

                // 敵のBoxコライダー用の読み込み (3.8,3.8,3.8,0.0,0.0,0.0 の6列をスキップ)
                std::string dummy;
                for (int d = 0; d < 6; ++d) {
                    std::getline(ss, dummy, ',');
                }
                
                std::string s_col_type, s_cx, s_cy, s_cz, s_csx, s_csy, s_csz;
                bool hasCollider = false;
                Vector3 colCenter = { 0.0f, 0.0f, 0.0f };
                Vector3 colSize = { 2.0f, 2.0f, 2.0f };
                
                bool isDisabled = false;
                std::string s_disabled;
                
                if (std::getline(ss, s_col_type, ',')) {
                    if (s_col_type == "BOX" &&
                        std::getline(ss, s_cx, ',') &&
                        std::getline(ss, s_cy, ',') &&
                        std::getline(ss, s_cz, ',') &&
                        std::getline(ss, s_csx, ',') &&
                        std::getline(ss, s_csy, ',') &&
                        std::getline(ss, s_csz, ',')) {
                        
                        hasCollider = true;
                        colCenter = { std::stof(s_cx), std::stof(s_cz), std::stof(s_cy) };
                        colSize = { std::stof(s_csx), std::stof(s_csz), std::stof(s_csy) };
                    } else if (s_col_type == "NONE") {
                        std::string dummy_col;
                        for (int d = 0; d < 6; ++d) {
                            std::getline(ss, dummy_col, ',');
                        }
                    }
                    
                    if (std::getline(ss, s_disabled, ',')) {
                        if (!s_disabled.empty() && std::stoi(s_disabled) == 1) {
                            isDisabled = true;
                        }
                    }
                }

                if (isDisabled) {
                    continue;
                }

                if (enemyCount < kMaxEnemies) {
                    enemies_[enemyCount].position = { gx + kWorldShiftX, gy, gz };
                    enemies_[enemyCount].isAlive = true;
                    enemies_[enemyCount].hp = 3.0f;
                    enemies_[enemyCount].radius = 3.8f;
                    enemies_[enemyCount].collider.hasCollider = hasCollider;
                    enemies_[enemyCount].collider.center = colCenter;
                    enemies_[enemyCount].collider.size = colSize;
                    enemyCount++;
                }
            } else if (type == "FLOOR") {
                std::string s_gx, s_gy, s_gz;
                std::getline(ss, s_gx, ',');
                std::getline(ss, s_gy, ',');
                std::getline(ss, s_gz, ',');

                // 不要なスケールやダミー回転を読み飛ばす (300.0, 1.0, 200.0, 0.0, 0.0, 0.0)
                std::string dummy;
                for (int d = 0; d < 6; ++d) {
                    std::getline(ss, dummy, ',');
                }

                std::string s_rx, s_ry, s_rz, s_juziro;
                std::getline(ss, s_rx, ',');
                std::getline(ss, s_ry, ',');
                std::getline(ss, s_rz, ',');
                std::getline(ss, s_juziro, ',');

                float gx = std::stof(s_gx);
                float gy = std::stof(s_gy);
                float gz = std::stof(s_gz);
                float rx = s_rx.empty() ? 0.0f : std::stof(s_rx);
                float ry = s_ry.empty() ? 0.0f : std::stof(s_ry);
                float rz = s_rz.empty() ? 0.0f : std::stof(s_rz);
                int isJuziro = s_juziro.empty() ? -1 : std::stoi(s_juziro);

                if (floorCount < kNumFloors) {
                    floorPositions_[floorCount] = { gx + kWorldShiftX, kFloorY, gz };
                    floorRotations_[floorCount] = { rx, ry, rz };
                    floorIsJuziro_[floorCount] = isJuziro;
                    floorCount++;
                }
            } else if (type == "WAYPOINT") {
                std::string s_gx, s_gy, s_gz;
                std::getline(ss, s_gx, ',');
                std::getline(ss, s_gy, ',');
                std::getline(ss, s_gz, ',');

                float gx = std::stof(s_gx);
                float gy = std::stof(s_gy);
                float gz = std::stof(s_gz);

                waypoints_.push_back({ gx + kWorldShiftX, gy, gz });
            }
        }
        layoutInFile.close();
    }
    numLoadedFloors_ = floorCount;

    if (waypoints_.empty()) {
        for (float z = 0.0f; z <= 10000.0f; z += 10.0f) {
            waypoints_.push_back({ kWorldShiftX, -20.0f, z });
        }
    }

    // ── 累積距離（アークレングス）の事前計算 ──
    waypointDistances_.clear();
    if (!waypoints_.empty()) {
        waypointDistances_.resize(waypoints_.size());
        waypointDistances_[0] = 0.0f;
        for (size_t i = 1; i < waypoints_.size(); ++i) {
            Vector3 diff = Subtract(waypoints_[i], waypoints_[i - 1]);
            float dist = std::sqrt(diff.x * diff.x + diff.y * diff.y + diff.z * diff.z);
            waypointDistances_[i] = waypointDistances_[i - 1] + dist;
        }

        // ── 床タイルの縦・横300m置き十字路(juziro.jpg)判定および横道(douro.jpg)の向き・間隔設定 ──
        for (int i = 0; i < numLoadedFloors_; ++i) {
            if (floorIsJuziro_[i] == -1) {
                size_t bestIdx = 0;
                float bestDistSq = 1e18f;
                for (size_t w = 0; w < waypoints_.size(); ++w) {
                    float dx = floorPositions_[i].x - waypoints_[w].x;
                    float dz = floorPositions_[i].z - waypoints_[w].z;
                    float d2 = dx * dx + dz * dz;
                    if (d2 < bestDistSq) {
                        bestDistSq = d2;
                        bestIdx = w;
                    }
                }
                float sVal = waypointDistances_[bestIdx];
                float rem = std::fmod(std::abs(sVal), 300.0f);
                bool isJuziroRow = (rem < 45.0f || rem > 255.0f);

                float dLat = std::sqrt(bestDistSq);
                int colIdx = static_cast<int>(std::round(dLat / kRoadColumnXOffset));
                bool isJuziroCol = ((std::abs(colIdx) % 3) == 0);

                floorIsJuziro_[i] = (isJuziroRow && isJuziroCol) ? 1 : 0;
                if (isJuziroRow && !isJuziroCol) {
                    floorRotations_[i].y += 1.57079632f;
                }
            }
        }

        // ── juziro.jpg の隣にある douro.jpg を重ならないギリギリ（隙間0m・重なり0m）でぴったり接続 ──
        for (int i = 0; i < numLoadedFloors_; ++i) {
            if (floorIsJuziro_[i] == 1) {
                floorPositions_[i].y = kFloorY + 0.002f;
                continue;
            }

            // 最も近い十字路タイル (juziro.jpg) を探す
            int bestJ = -1;
            float bestDistSq = 1e18f;
            for (int j = 0; j < numLoadedFloors_; ++j) {
                if (floorIsJuziro_[j] != 1) continue;
                float dx = floorPositions_[i].x - floorPositions_[j].x;
                float dz = floorPositions_[i].z - floorPositions_[j].z;
                float d2 = dx * dx + dz * dz;
                if (d2 < bestDistSq) {
                    bestDistSq = d2;
                    bestJ = j;
                }
            }

            if (bestJ >= 0) {
                float jRy = floorRotations_[bestJ].y;
                float fwdX = std::sin(jRy);
                float fwdZ = std::cos(jRy);
                float rightX = std::cos(jRy);
                float rightZ = -std::sin(jRy);

                float dx = floorPositions_[i].x - floorPositions_[bestJ].x;
                float dz = floorPositions_[i].z - floorPositions_[bestJ].z;
                float dFwd = dx * fwdX + dz * fwdZ;
                float dLat = dx * rightX + dz * rightZ;

                if (std::abs(dFwd) < 40.0f) {
                    // 十字路と同じ横並びの列にある横道 douro.jpg (2 = 横道: 80m×80m、中心距離±80mでぴったり接続)
                    floorIsJuziro_[i] = 2;
                    if (std::abs(dLat) >= 70.0f && std::abs(dLat) <= 90.0f) {
                        float targetLat = (dLat >= 0.0f) ? 80.0f : -80.0f;
                        float shift = targetLat - dLat;
                        floorPositions_[i].x += rightX * shift;
                        floorPositions_[i].z += rightZ * shift;
                    }
                } else {
                    // 縦方向の douro.jpg (0 = 縦道: 長さ110m×幅80m、十字路から中心距離±95mで 40m〜150m / 150m〜260m にぴったり接続)
                    floorIsJuziro_[i] = 0;
                    if (std::abs(dFwd) >= 70.0f && std::abs(dFwd) <= 115.0f) {
                        float targetFwd = (dFwd >= 0.0f) ? 95.0f : -95.0f;
                        float shift = targetFwd - dFwd;
                        floorPositions_[i].x += fwdX * shift;
                        floorPositions_[i].z += fwdZ * shift;
                    }
                }
            }

            // 境界線の同一ピクセルZファイティング防止用の極小オフセット
            floorPositions_[i].y = kFloorY + 0.0002f * static_cast<float>(i % 7);
        }

        // カーブ内側等で中心距離が極端に近い重複タイルがあれば非表示化
        for (int i = 0; i < numLoadedFloors_; ++i) {
            if (floorPositions_[i].y < -5000.0f) continue;
            for (int k = 0; k < i; ++k) {
                if (floorPositions_[k].y < -5000.0f) continue;
                float dx = floorPositions_[i].x - floorPositions_[k].x;
                float dz = floorPositions_[i].z - floorPositions_[k].z;
                if (dx * dx + dz * dz < 55.0f * 55.0f) {
                    floorPositions_[i].y = -10000.0f;
                    break;
                }
            }
        }

        // ── プレイヤー進行ルート（コース中央脇）へのビル増量・高密度配置 ──
        if (!waypointDistances_.empty() && waypointDistances_.back() > 0.0f) {
            float totalDist = waypointDistances_.back();
            float buildingPitch = 40.0f; // 40m間隔で進行方向の左右にビルを高密度補填

            for (float s = 20.0f; s < totalDist; s += buildingPitch) {
                Vector3 railPos = GetRailPosition(s);
                Vector3 railDir = GetRailDirection(s);
                Vector3 railRight = CalculateRailRight(railDir);

                // コース脇の左右4レーン (±85m, ±125m：十字路の縦通り±52mを避けた街区列)
                const float sideOffsets[] = { -85.0f, +85.0f, -125.0f, +125.0f };
                for (int k = 0; k < 4; ++k) {
                    float offset = sideOffsets[k];
                    Vector3 bPos = Add(railPos, Scale(railRight, offset));
                    bPos.y = -20.0f; // 地面基準高さ

                    // 既存のビルとの重なりチェック (20m以内なら配置スキップ)
                    bool isOverlap = false;
                    for (size_t bIdx = 0; bIdx < buildings_.size(); ++bIdx) {
                        if (buildings_[bIdx].floors <= 0) continue;
                        float dx = buildings_[bIdx].position.x - bPos.x;
                        float dz = buildings_[bIdx].position.z - bPos.z;
                        if (dx * dx + dz * dz < 20.0f * 20.0f) {
                            isOverlap = true;
                            break;
                        }
                    }

                    if (!isOverlap && buildings_.size() < kMaxBuildings) {
                        Building b;
                        b.position = bPos;
                        b.scale = { 10.0f, 10.0f, 10.0f };
                        b.isNearCourseColumn = true; // 中央＆すぐ隣の列フラグ（詳細描画用）

                        // 近列(±85m)は中層(3~8階)、外列(±125m)は高層(6~12階)
                        if (std::abs(offset) < 100.0f) {
                            b.floors = 3 + (randomEngine_() % 6);
                        } else {
                            b.floors = 6 + (randomEngine_() % 7);
                        }

                        float rotY = std::atan2(railDir.x, railDir.z);
                        b.rotate = { 0.0f, rotY, 0.0f };

                        b.originalX = bPos.x;
                        b.originalY = -20.0f;
                        b.originalFloors = b.floors;
                        b.isDestroyed = false;
                        b.velocity = { 0.0f, 0.0f, 0.0f };
                        b.rotationSpeed = { 0.0f, 0.0f, 0.0f };
                        b.destroyTimer = 0.0f;

                        buildings_.push_back(b);
                    }
                }
            }

            // ── 曲がり角（カーブ手前の直線および直進延長）での高密度背景ビル補填 ──
            float stepCheck = 40.0f; // 40mごとにチェック
            for (float s = 40.0f; s < totalDist - 40.0f; s += stepCheck) {
                float prevS = (s - 30.0f < 0.0f) ? 0.0f : (s - 30.0f);
                float nextS = (s + 30.0f > totalDist) ? totalDist : (s + 30.0f);
                Vector3 dirPrev = GetRailDirection(prevS);
                Vector3 dirNext = GetRailDirection(nextS);

                // 進行方向の変化量（カーブ判定）
                float dotVal = dirPrev.x * dirNext.x + dirPrev.z * dirNext.z;
                if (dotVal < 0.985f) { // カーブしているポイント
                    Vector3 dirStraight = dirPrev;
                    Vector3 rightStraight = CalculateRailRight(dirStraight);
                    Vector3 pStart = GetRailPosition(s);

                    // 直進方向（ダミー道路が伸びている方向）へ 15 ステップ（各 80m）高密度に伸ばす（十字路の縦通り 0m, ±240m を避けた街区列）
                    const float dummyCols[] = {
                        -85.0f, 85.0f, -125.0f, 125.0f, -165.0f, 165.0f,
                        -325.0f, 325.0f, -365.0f, 365.0f, -405.0f, 405.0f
                    };
                    for (int d = 1; d <= 15; ++d) {
                        float dDist = d * 80.0f;
                        Vector3 pDummy = Add(pStart, Scale(dirStraight, dDist));

                        for (int k = 0; k < 12; ++k) {
                            float offset = dummyCols[k];
                            Vector3 bPos = Add(pDummy, Scale(rightStraight, offset));
                            bPos.y = -20.0f;

                            // 既存ビルとの重なりチェック (20m以内)
                            bool isOverlap = false;
                            for (size_t bIdx = 0; bIdx < buildings_.size(); ++bIdx) {
                                if (buildings_[bIdx].floors <= 0) continue;
                                float dx = buildings_[bIdx].position.x - bPos.x;
                                float dz = buildings_[bIdx].position.z - bPos.z;
                                if (dx * dx + dz * dz < 20.0f * 20.0f) {
                                    isOverlap = true;
                                    break;
                                }
                            }

                            if (!isOverlap && buildings_.size() < kMaxBuildings) {
                                Building b;
                                b.position = bPos;
                                b.scale = { 10.0f, 10.0f, 10.0f };
                                b.floors = 3 + (randomEngine_() % 8);
                                b.isNearCourseColumn = (std::abs(offset) < 90.0f); // 中央＆隣接列のみ詳細描画
                                float rotY = std::atan2(dirStraight.x, dirStraight.z);
                                b.rotate = { 0.0f, rotY, 0.0f };
                                b.originalX = bPos.x;
                                b.originalY = -20.0f;
                                b.originalFloors = b.floors;
                                b.isDestroyed = false;
                                b.velocity = { 0.0f, 0.0f, 0.0f };
                                b.rotationSpeed = { 0.0f, 0.0f, 0.0f };
                                b.destroyTimer = 0.0f;

                                buildings_.push_back(b);
                            }
                        }
                    }
                }
            }
        }
    }

    // ── 空いた寂しいスペースへのダミービル自動生成 ──
    if (!waypoints_.empty() && !waypointDistances_.empty() && waypointDistances_.back() > 0.0f) {
        float totalDist = waypointDistances_.back();
        float checkStep = 20.0f; // 進行ライン上の判定を網羅するステップ
        const float kMinDistToRoute = 45.0f; // コース中心から45m以内は配置禁止（道路を塞がない）
        const float kMinDistToRouteSq = kMinDistToRoute * kMinDistToRoute;

        // X範囲: -360m から +360m まで 80m 刻み
        // Z範囲: 40m から totalDist まで 80m 刻み
        for (float z = 40.0f; z < totalDist; z += 80.0f) {
            for (float x = -360.0f; x <= 360.0f; x += 80.0f) {
                if (buildings_.size() >= kMaxBuildings) {
                    break; // 最大上限に達したら終了
                }

                // グリッド座標に kWorldShiftX を加味
                Vector3 bPos = { x + kWorldShiftX, -20.0f, z };

                // 1. コース（進行レール）との距離チェック
                bool isNearRoute = false;
                for (float s = 0.0f; s <= totalDist; s += checkStep) {
                    Vector3 railPos = GetRailPosition(s);
                    float dx = bPos.x - railPos.x;
                    float dz = bPos.z - railPos.z;
                    if (dx * dx + dz * dz < kMinDistToRouteSq) {
                        isNearRoute = true;
                        break;
                    }
                }
                if (isNearRoute) continue;

                // 2. 既存のビルとの重なりチェック (20m以内)
                bool isOverlap = false;
                for (size_t bIdx = 0; bIdx < buildings_.size(); ++bIdx) {
                    if (buildings_[bIdx].floors <= 0) continue;
                    float dx = buildings_[bIdx].position.x - bPos.x;
                    float dz = buildings_[bIdx].position.z - bPos.z;
                    if (dx * dx + dz * dz < 20.0f * 20.0f) {
                        isOverlap = true;
                        break;
                    }
                }
                if (isOverlap) continue;

                // 配置実行
                Building b;
                b.position = bPos;
                b.scale = { 10.0f, 10.0f, 10.0f };
                // 3〜10階のランダムな高さ
                b.floors = 3 + (randomEngine_() % 8); 
                b.isNearCourseColumn = false; // ルートから遠いため、LODによって遠方は自動的に荒いモデル（超軽量化）になる
                b.rotate = { 0.0f, 0.0f, 0.0f };
                b.originalX = bPos.x;
                b.originalY = -20.0f;
                b.originalFloors = b.floors;
                b.isDestroyed = false;
                b.velocity = { 0.0f, 0.0f, 0.0f };
                b.rotationSpeed = { 0.0f, 0.0f, 0.0f };
                b.destroyTimer = 0.0f;

                buildings_.push_back(b);
            }
        }
    }

    // ── プレイヤーが通る中央タイル/レールおよび十字路の縦道・横道と重なっているビルを自動削除 ──
    if (!waypoints_.empty() && !waypointDistances_.empty() && waypointDistances_.back() > 0.0f) {
        float totalDist = waypointDistances_.back();
        float checkStep = 10.0f; // 10m間隔で進行ライン上の判定を網羅
        const float kCenterClearRadius = 40.0f; // 進行ルート中心から40m以内（道路通行域）のビルは削除対象

        for (size_t i = 0; i < buildings_.size(); ++i) {
            if (buildings_[i].floors <= 0 || buildings_[i].isDestroyed) continue;

            bool isOverlappingWithCenter = false;
            for (float s = 0.0f; s <= totalDist; s += checkStep) {
                Vector3 railPos = GetRailPosition(s);
                float dx = buildings_[i].position.x - railPos.x;
                float dz = buildings_[i].position.z - railPos.z;
                if (dx * dx + dz * dz < kCenterClearRadius * kCenterClearRadius) {
                    isOverlappingWithCenter = true;
                    break;
                }
            }

            // 十字路(juziro.jpg)がある「横方向の通り」および「縦方向の通り」にあるビルも削除
            if (!isOverlappingWithCenter) {
                for (int f = 0; f < numLoadedFloors_; ++f) {
                    if (floorIsJuziro_[f] != 1) continue;
                    float dx = buildings_[i].position.x - floorPositions_[f].x;
                    float dz = buildings_[i].position.z - floorPositions_[f].z;
                    if (dx * dx + dz * dz > 260.0f * 260.0f) continue;

                    float ry = floorRotations_[f].y;
                    float sinY = std::sin(ry);
                    float cosY = std::cos(ry);
                    float distForward = std::abs(dx * sinY + dz * cosY);
                    float distLateral = std::abs(dx * cosY - dz * sinY);
                    // 1. 十字路がある横方向の通り (前後±52m、左右±200m)
                    // 2. 十字路がある縦方向の通り (左右±52m、前後±200m)
                    if ((distForward < 52.0f && distLateral < 200.0f) ||
                        (distLateral < 52.0f && distForward < 200.0f)) {
                        isOverlappingWithCenter = true;
                        break;
                    }
                }
            }

            if (isOverlappingWithCenter) {
                // 進行ルートまたは十字路の縦横通りと重なっているビルを削除 (floors = 0)
                buildings_[i].floors = 0;
                buildings_[i].originalFloors = 0;
            }
        }

        // ── プレイヤーが通るルート（コース/レール）の近くにあるビルの自動判定 ──
        float routeCheckStep = 10.0f; // 10m間隔でルート上の点を判定
        const float kNearRouteDist = 130.0f; // ルート中心から130m以内（コース両脇および直近ビル列）
        const float kNearRouteDistSq = kNearRouteDist * kNearRouteDist;

        for (size_t i = 0; i < buildings_.size(); ++i) {
            if (buildings_[i].floors <= 0) continue;

            float minSqDist = 1e9f;
            for (float s = 0.0f; s <= totalDist; s += routeCheckStep) {
                Vector3 railPos = GetRailPosition(s);
                float dx = buildings_[i].position.x - railPos.x;
                float dz = buildings_[i].position.z - railPos.z;
                float sqDist = dx * dx + dz * dz;
                if (sqDist < minSqDist) {
                    minSqDist = sqDist;
                }
            }

            // ルートの近くにあるビル、または直進延長上のビルは isNearCourseColumn = true （高詳細描画対象）
            // 元の isNearCourseColumn（曲がり角延長上の高密度補テンフラグ等）を保持しつつ判定
            buildings_[i].isNearCourseColumn = buildings_[i].isNearCourseColumn || (minSqDist <= kNearRouteDistSq);
        }
    }

    // ── デモ用データの初期化 ──
    shotBeams_.resize(kMaxShotBeams);
    for (int i = 0; i < kMaxShotBeams; ++i) {
        shotBeams_[i].isAlive = false;
    }

    // フラッシュ演出用の定数バッファ生成とマッピング
    flashMaterialResource_ = CreateBufferResource(device, sizeof(Material));
    flashMaterialResource_->Map(0, nullptr, reinterpret_cast<void**>(&flashMaterialData_));
    flashMaterialData_->color = { 1.0f, 1.0f, 1.0f, 0.0f }; // 初期は完全透明
    flashMaterialData_->uvTransform = MakeIdentity4x4();

    flashTransformResource_ = CreateBufferResource(device, sizeof(TransformationMatrix));
    flashTransformResource_->Map(0, nullptr, reinterpret_cast<void**>(&flashTransformData_));
    flashTransformData_->WVP = MakeIdentity4x4();
    flashTransformData_->World = MakeIdentity4x4();
    flashTransformData_->color = { 1.0f, 1.0f, 1.0f, 1.0f };

    // デモ用テクスチャのロード（フラッシュやヒットエフェクトで使うテクスチャを予め確保）
    TextureManager::GetInstance()->LoadTexture("aiming.png");
    flashSrvHandleGPU_ = TextureManager::GetInstance()->GetSrvHandleGPU("aiming.png");
    
    // 近接突撃用メンバの初期化
    currentFighterPos_ = playerPos_;
    attackMode_ = AttackMode::kShooting;
    meleeState_ = MeleeState::kIdle;
    digitalGlitchTimer_ = 0.0f;

    // ボス登場WARNING演出用スプライトの事前ロード・生成
    TextureManager::GetInstance()->LoadTexture("warning_text.png");
    TextureManager::GetInstance()->LoadTexture("warning_tape_single.png");
    TextureManager::GetInstance()->LoadTexture("human/white.png");
    TextureManager::GetInstance()->LoadTexture("phase.png");
    TextureManager::GetInstance()->LoadTexture("number/big1.png");
    TextureManager::GetInstance()->LoadTexture("number/big2.png");
    TextureManager::GetInstance()->LoadTexture("circle2.png");
    warningTextSprite_ = Sprite::Create("warning_text.png", { 0.0f, 0.0f });
    for (int i = 0; i < 3; ++i) {
        warningTextGhostSprites_[i] = Sprite::Create("warning_text.png", { 0.0f, 0.0f });
    }
    warningTapeTopSprite_ = Sprite::Create("warning_tape_single.png", { 0.0f, 0.0f });
    warningTapeBottomSprite_ = Sprite::Create("warning_tape_single.png", { 0.0f, 0.0f });
    for (int i = 0; i < 4; ++i) {
        warningTapeTopSprites_[i] = Sprite::Create("warning_tape_single.png", { 0.0f, 0.0f });
        warningTapeBottomSprites_[i] = Sprite::Create("warning_tape_single.png", { 0.0f, 0.0f });
    }
    warningRedOverlaySprite_ = Sprite::Create("human/white.png", { 0.0f, 0.0f });
    TextureManager::GetInstance()->LoadTexture("boss_defeat_text.png");
    bossDefeatTextSprite_ = Sprite::Create("boss_defeat_text.png", { 0.0f, 0.0f });
    for (int i = 0; i < 2; ++i) {
        bossDefeatTextGhostSprites_[i] = Sprite::Create("boss_defeat_text.png", { 0.0f, 0.0f });
    }

    // フェーズ演出（高速ブースト＆ソニックブーム）用スプライト群の事前生成
    phaseIntroSprite_ = Sprite::Create("phase.png", { 0.0f, 0.0f });
    numberIntroSprite_ = Sprite::Create("number/big1.png", { 0.0f, 0.0f });
    for (int i = 0; i < 2; ++i) {
        phaseIntroGhostSprites_[i] = Sprite::Create("phase.png", { 0.0f, 0.0f });
        numberIntroGhostSprites_[i] = Sprite::Create("number/big1.png", { 0.0f, 0.0f });
        phaseIntroGlowSprites_[i] = Sprite::Create("circle2.png", { 0.0f, 0.0f });
    }
    for (int i = 0; i < 6; ++i) {
        phaseIntroBandSprites_[i] = Sprite::Create("human/white.png", { 0.0f, 0.0f });
    }
    for (int i = 0; i < 8; ++i) {
        phaseIntroStreakSprites_[i] = Sprite::Create("human/white.png", { 0.0f, 0.0f });
    }
    for (int i = 0; i < 48; ++i) {
        cornerUiSprites_[i] = Sprite::Create("human/white.png", { 0.0f, 0.0f });
    }
    for (int i = 0; i < 24; ++i) {
        cornerSpeedLineSprites_[i] = Sprite::Create("human/white.png", { 0.0f, 0.0f });
    }

    // ── ルート上の曲がり角（A / D 長押し選択ポイント）の自動検出 ──
    corners_.clear();
    cornerState_ = CornerTurnState::kNone;
    activeCornerIndex_ = -1;
    cornerHoldingDir_ = 0;
    cornerChosenDir_ = 0;
    cornerHoldProgress_ = 0.0f;
    cornerTimer_ = 0.0f;
    wrongRouteDarkenAlpha_ = 0.0f;
    isOpeningIntro_ = true;

    if (!waypoints_.empty() && !waypointDistances_.empty() && waypointDistances_.back() > 250.0f) {
        float totalDist = waypointDistances_.back();
        float lastCornerS = -500.0f;
        for (float s = 120.0f; s < totalDist - 120.0f; s += 10.0f) {
            if (s - lastCornerS < 250.0f) continue;
            Vector3 dirBefore = GetRailDirection(s - 55.0f);
            Vector3 dirAfter  = GetRailDirection(s + 55.0f);
            float dotVal = dirBefore.x * dirAfter.x + dirBefore.z * dirAfter.z;
            if (dotVal < 0.35f) { // 約70度〜90度の曲がり角を検出
                float bestS = s;
                float minDot = dotVal;
                for (float probe = s - 25.0f; probe <= s + 25.0f; probe += 2.0f) {
                    Vector3 dB = GetRailDirection(probe - 55.0f);
                    Vector3 dA = GetRailDirection(probe + 55.0f);
                    float d = dB.x * dA.x + dB.z * dA.z;
                    if (d < minDot) {
                        minDot = d;
                        bestS = probe;
                    }
                }
                Vector3 finalBefore = GetRailDirection(bestS - 60.0f);
                Vector3 finalAfter  = GetRailDirection(bestS + 60.0f);
                Vector3 rightBefore = CalculateRailRight(finalBefore);
                float sideDot = rightBefore.x * finalAfter.x + rightBefore.z * finalAfter.z;

                CornerInfo info;
                info.cornerS = bestS;
                info.triggerS = bestS - 95.0f; // 角の95m手前で曲がる演出を開始
                info.correctDir = (sideDot >= 0.0f) ? 1 : -1; // +1: Dキー(右), -1: Aキー(左)
                info.cornerPos = GetRailPosition(bestS);
                info.dirBefore = finalBefore;
                info.dirAfter = finalAfter;
                info.rightBefore = rightBefore;
                info.completed = false;
                corners_.push_back(info);
                lastCornerS = bestS;
            }
        }
    }

    // ── ゲーム開始演出（地球 -> 日本 -> 東京の夜景）用 3Dモデル＆テクスチャの初期化 ──
    TextureManager::GetInstance()->LoadTexture("Earth/earth.png");
    TextureManager::GetInstance()->LoadTexture("Japan/japan.png");
    TextureManager::GetInstance()->LoadTexture("Tokyo/tokyo.png");

    earthModel_ = Model::LoadGLTF("Resources/Earth/earth.obj", device);
    japanModel_ = Model::LoadGLTF("Resources/Japan/japan.obj", device);
    tokyoModel_ = Model::LoadGLTF("Resources/Tokyo/tokyo.obj", device);

    if (earthModel_) {
        earthModel_->SetLightingEnabled(false);
        earthModel_->SetEnvironmentCoefficient(0.0f);
    }
    if (japanModel_) {
        japanModel_->SetLightingEnabled(false);
        japanModel_->SetEnvironmentCoefficient(0.0f);
    }
    if (tokyoModel_) {
        tokyoModel_->SetLightingEnabled(false);
        tokyoModel_->SetEnvironmentCoefficient(0.0f);
    }

    earthTransformResource_ = CreateBufferResource(device, sizeof(TransformationMatrix));
    earthTransformResource_->Map(0, nullptr, reinterpret_cast<void**>(&earthTransformData_));
    earthTransformData_->WVP   = MakeIdentity4x4();
    earthTransformData_->World = MakeIdentity4x4();
    earthTransformData_->color = { 1.0f, 1.0f, 1.0f, 1.0f };

    japanTransformResource_ = CreateBufferResource(device, sizeof(TransformationMatrix));
    japanTransformResource_->Map(0, nullptr, reinterpret_cast<void**>(&japanTransformData_));
    japanTransformData_->WVP   = MakeIdentity4x4();
    japanTransformData_->World = MakeIdentity4x4();
    japanTransformData_->color = { 1.0f, 1.0f, 1.0f, 1.0f };

    tokyoTransformResource_ = CreateBufferResource(device, sizeof(TransformationMatrix));
    tokyoTransformResource_->Map(0, nullptr, reinterpret_cast<void**>(&tokyoTransformData_));
    tokyoTransformData_->WVP   = MakeIdentity4x4();
    tokyoTransformData_->World = MakeIdentity4x4();
    tokyoTransformData_->color = { 1.0f, 1.0f, 1.0f, 1.0f };

    // ── 通常プレイ開始時は最初からステージ遠方(S=1100m)にボスがいる状態でスタートし、
    //    ロード画面明けは地球→日本→東京夜景を省いて、ボスの登場演出(t=6.4s〜)から直接開始する ──
    if (!isTitleMode_) {
        currentPhase_ = GamePhase::kPhase1;
        bossAppearanceTimer_ = kBossIntroTotalDuration - 6.4f; // 地球・日本・東京夜景をスキップし、ボスの登場演出のみを再生
        hasStartedInitialPhase_ = false; // 冒頭演出終了後に「PHASE 1」を開始
        phaseIntroTimer_ = -1.0f;
        warningScrollOffset_ = 0.0f;
        warningLastBeatIndex_ = -1;
        bossZOffset_ = 1100.0f; // 最初ボスとプレイヤーはある程度離れた位置(1100m前方)に配置
        bossTime_ = 0.0f;
        isBossLanded_ = false;
        isBossRoared_ = false;
        hasEmittedJumpStart_ = false;
        hasEmittedJumpEnd_ = false;
        bossActionState_ = BossActionState::kIdle;
        bossActionTimer_ = 0.0f;
    }
}

void GamePlayScene::StartPhaseIntro(int phaseNum) {
    phaseIntroNum_ = phaseNum;
    phaseIntroTimer_ = 0.0f;
    hasTriggeredPhaseImpact_ = false;
    hasTriggeredPhaseExit_ = false;

    // フェーズ画像と数字画像をセットアップ
    if (!phaseIntroSprite_) {
        phaseIntroSprite_ = Sprite::Create("phase.png", { 0.0f, 0.0f });
    }
    std::string numberTex = "number/big" + std::to_string(phaseNum) + ".png";
    numberIntroSprite_ = Sprite::Create(numberTex, { 0.0f, 0.0f });
    for (int i = 0; i < 2; ++i) {
        if (!phaseIntroGhostSprites_[i]) {
            phaseIntroGhostSprites_[i] = Sprite::Create("phase.png", { 0.0f, 0.0f });
        }
        numberIntroGhostSprites_[i] = Sprite::Create(numberTex, { 0.0f, 0.0f });
        if (!phaseIntroGlowSprites_[i]) {
            phaseIntroGlowSprites_[i] = Sprite::Create("circle2.png", { 0.0f, 0.0f });
        }
    }
    for (int i = 0; i < 6; ++i) {
        if (!phaseIntroBandSprites_[i]) {
            phaseIntroBandSprites_[i] = Sprite::Create("human/white.png", { 0.0f, 0.0f });
        }
    }
    for (int i = 0; i < 8; ++i) {
        if (!phaseIntroStreakSprites_[i]) {
            phaseIntroStreakSprites_[i] = Sprite::Create("human/white.png", { 0.0f, 0.0f });
        }
    }

    // 演出中に前進・ブーストするため、残存している雑魚敵や弾を一旦クリア（演出終了直後に前方へ再出現）
    Vector3 vanishColor = (phaseNum == 1) ? Vector3{ 0.2f, 0.9f, 1.0f } : Vector3{ 1.0f, 0.6f, 0.15f };
    for (int i = 0; i < kMaxEnemies; ++i) {
        if (enemies_[i].isAlive) {
            if (particleManager_) {
                particleManager_->EmitCustomSparks(enemies_[i].position, 45.0f, 10, vanishColor, 0.4f);
            }
            enemies_[i].isAlive = false;
        }
    }
    for (auto& b : playerBullets_) {
        b.currentTime = b.lifeTime;
    }

    // 突入開始のチャージ効果音
    if (audio_) {
        PlaySE(jumpSE_, false, 1.2f);
    }
}

void GamePlayScene::Finalize() {
    if (audio_) {
        audio_->Finalize();
    }
}

void GamePlayScene::Update() {
    float kDeltaTime = 1.0f / 60.0f;

    if (isDemoMode_) {
        UpdateDemo(kDeltaTime);
        return;
    }

    // 0. 数字キーによるプリセット切り替え（通常プレイ用）
    if (input_->IsKeyTriggered(DIK_1)) ApplyPreset(0);
    else if (input_->IsKeyTriggered(DIK_2)) ApplyPreset(1);
    else if (input_->IsKeyTriggered(DIK_3)) ApplyPreset(2);
    else if (input_->IsKeyTriggered(DIK_4)) ApplyPreset(3);
    else if (input_->IsKeyTriggered(DIK_5)) ApplyPreset(4);
    else if (input_->IsKeyTriggered(DIK_6)) ApplyPreset(5);
    else if (input_->IsKeyTriggered(DIK_7)) ApplyPreset(6);
    else if (input_->IsKeyTriggered(DIK_8)) ApplyPreset(7);
    else if (input_->IsKeyTriggered(DIK_9)) ApplyPreset(8);
    else if (input_->IsKeyTriggered(DIK_0)) ApplyPreset(9);

    // 0.1. Tキーによる背景（スカイボックス）の切り替え
    if (input_->IsKeyTriggered(DIK_T)) {
        skyboxType_ = (skyboxType_ + 1) % static_cast<int>(kNumSkyboxTextures);
    }

    // 0.5. 各種演出タイマーの更新と時間スケール（ヒットストップ）処理
    if (hitstopTimer_ > 0.0f) {
        hitstopTimer_ -= kDeltaTime;
        kDeltaTime = kDeltaTime * 0.05f; // スローモーション化
    }

    if (cameraShakeTimer_ > 0.0f) {
        cameraShakeTimer_ -= kDeltaTime;
        if (cameraShakeTimer_ < 0.0f) cameraShakeTimer_ = 0.0f;
        
        float progress = cameraShakeTimer_ / (cameraShakeTimeMax_ > 0.0f ? cameraShakeTimeMax_ : 1.0f);
        progress = std::clamp(progress, 0.0f, 1.0f);
        float currentIntensity = activeShakeIntensity_ * (progress * progress);
        
        std::uniform_real_distribution<float> distShake(-1.0f, 1.0f);
        if (selectedEffectPreset_ == 7) {
            cameraShakeOffset_ = {
                distShake(randomEngine_) * currentIntensity,
                0.0f,
                0.0f
            };
        } else {
            cameraShakeOffset_ = {
                distShake(randomEngine_) * currentIntensity,
                distShake(randomEngine_) * currentIntensity,
                distShake(randomEngine_) * currentIntensity
            };
        }
    } else {
        cameraShakeOffset_ = { 0.0f, 0.0f, 0.0f };
    }

    if (flashAlpha_ > 0.0f) {
        float fadeSpeed = (selectedEffectPreset_ == 8) ? 1.5f : 3.5f;
        flashAlpha_ -= kDeltaTime * fadeSpeed;
        if (flashAlpha_ < 0.0f) flashAlpha_ = 0.0f;
    }

    if (useRadialBlur_ && blurIntensity_ > 0.0f) {
        blurIntensity_ -= kDeltaTime * (maxBlurWidth_ / 0.25f);
        if (blurIntensity_ < 0.0f) blurIntensity_ = 0.0f;
        
        activePostProcess_ = kRadialBlur;
        if (radialBlurParamData_) {
            radialBlurParamData_->center = { 0.5f, 0.5f };
            radialBlurParamData_->blurWidth = -blurIntensity_;
        }
    } else {
        if (activePostProcess_ == kRadialBlur) {
            activePostProcess_ = kNone;
        }
    }

    if (digitalGlitchTimer_ > 0.0f) {
        digitalGlitchTimer_ -= kDeltaTime;
        if (digitalGlitchTimer_ < 0.0f) digitalGlitchTimer_ = 0.0f;
        
        activePostProcess_ = kRandom;
        if (randomParamResource_ && randomParamData_) {
            randomParamData_->time = randomEffectTime_;
            randomParamData_->noiseScale = 100.0f;
            randomParamData_->noiseStrength = 0.85f;
            randomParamData_->isColorNoise = false;
            randomParamData_->isMultiplyNoise = false;
        }
    } else {
        if (activePostProcess_ == kRandom) {
            activePostProcess_ = kNone;
        }
    }

    // ゲームクリア/ゲームオーバー遷移判定
    if (bossHP_ <= 0.0f && !isBossDefeatedSequence_) {
        isBossDefeatedSequence_ = true;
        bossDefeatTimer_ = 0.0f;
        hasHitBoss_ = false;
        isBossModelVisible_ = true;
        isDefeatBulletActive_ = false;
        bossDefeatHitTimer_ = 0.0f;
        
        // パーティクルを一旦すべてクリア（土・石の破片バースト予約も解除）
        if (particleManager_) {
            particleManager_->Clear();
        }
        triggerDebrisEmit_ = false;
        
        // プレイヤーの速度を通常に戻し、バレルロールやブーストを強制解除
        isBarrelRolling_ = false;
        isBoosting_ = false;
        boostBlurWidth_ = 0.0f;
        boostForwardSpeed_ = kNormalSpeed;
        
        // プレイヤーの弾をすべて消去（寿命切れにする）
        for (auto& b : playerBullets_) {
            b.currentTime = b.lifeTime;
        }
        // ボスの蜘蛛の巣弾などをクリア
        bossWebBullets_.clear();
    }

    if (isBossDefeatedSequence_) {
        bossDefeatTimer_ += kDeltaTime;
        
        // カメラトランスフォームの参照を取得
        EulerTransform& camTrans = camera_->GetTransform();
        
        // 1. 自機を画面中央（X=0, Y=0）に滑らかに補間移動
        if (fighterModel_) {
            fighterModel_->transform.translate.x = std::lerp(fighterModel_->transform.translate.x, 0.0f, 0.1f);
            fighterModel_->transform.translate.y = std::lerp(fighterModel_->transform.translate.y, 0.0f, 0.1f);
            fighterModel_->transform.rotate.x = std::lerp(fighterModel_->transform.rotate.x, 0.0f, 0.1f);
            fighterModel_->transform.rotate.y = std::lerp(fighterModel_->transform.rotate.y, 0.0f, 0.1f);
            fighterModel_->transform.rotate.z = std::lerp(fighterModel_->transform.rotate.z, 0.0f, 0.1f);
            
            // プレイヤーのロール・ピッチもリセット
            playerRotationRoll_ = std::lerp(playerRotationRoll_, 0.0f, 0.1f);
            playerRotationPitch_ = std::lerp(playerRotationPitch_, 0.0f, 0.1f);
        }
        
        // 演出中はプレイヤーとボスの前進・移動を完全に停止（その場で静止して演出）
        // fighterWorldZ_ の前進処理を停止
        
        // 自機のワールド位置（曲がり角を曲がった後のレール座標系にも対応）
        Vector3 defeatPlayerRailPos = GetRailPosition(fighterWorldZ_);
        Vector3 defeatPlayerRailDir = GetRailDirection(fighterWorldZ_);
        Vector3 defeatPlayerRailRight = CalculateRailRight(defeatPlayerRailDir);
        Vector3 defeatPlayerRailUp = CalculateRailUp(defeatPlayerRailDir, defeatPlayerRailRight);
        float defeatPlayerRelX = fighterModel_ ? fighterModel_->transform.translate.x : 0.0f;
        float defeatPlayerRelY = (fighterModel_ ? fighterModel_->transform.translate.y : 0.0f) + 17.0f;
        Vector3 fighterWorldPos = Add(
            defeatPlayerRailPos,
            Add(Scale(defeatPlayerRailRight, defeatPlayerRelX), Scale(defeatPlayerRailUp, defeatPlayerRelY))
        );
        
        // ボスのワールド位置（演出中はバウンスも停止し、完全に静止）
        float bodyBounce = 0.0f;
        Vector3 bossPos = GetBossPosition(bodyBounce);
        
        // ==========================================
        // 【カメラワーク】背後65m、高さ26m、ボス注視アングル（常に維持）
        // ==========================================
        float camProgress = fighterWorldZ_ - 65.0f;
        Vector3 camRailPos = GetRailPosition(camProgress);
        Vector3 camRailDir = GetRailDirection(camProgress);
        Vector3 camRailRight = CalculateRailRight(camRailDir);
        Vector3 camRailUp = CalculateRailUp(camRailDir, camRailRight);
        Vector3 targetCamPos = Add(camRailPos, Scale(camRailUp, 26.0f)); // 通常時 Yオフセット 26.0f
        
        // 開始フレーム（最初のフレーム）は即座にワープ、それ以外はスムーズに補間
        bool isStartFrame = (bossDefeatTimer_ <= kDeltaTime);
        camTrans.translate = isStartFrame ? targetCamPos : Lerp(camTrans.translate, targetCamPos, 0.08f);
        
        // カメラからボスへの方向ベクトルから回転角を計算して注視させる
        Vector3 lookDir = Subtract(bossPos, camTrans.translate);
        float lookDist = Length(lookDir);
        if (lookDist > 0.001f) {
            lookDir = Scale(lookDir, 1.0f / lookDist);
        } else {
            lookDir = camRailDir;
        }
        
        float targetRotY = -std::atan2(lookDir.x, lookDir.z);
        float targetRotX = std::atan2(-lookDir.y, std::sqrt(lookDir.x * lookDir.x + lookDir.z * lookDir.z));
        
        float rotLerpFactor = isStartFrame ? 1.0f : 0.08f;
        
        float diffY = targetRotY - camTrans.rotate.y;
        while (diffY < -static_cast<float>(M_PI)) diffY += 2.0f * static_cast<float>(M_PI);
        while (diffY > static_cast<float>(M_PI)) diffY -= 2.0f * static_cast<float>(M_PI);
        camTrans.rotate.y += diffY * rotLerpFactor;

        float diffX = targetRotX - camTrans.rotate.x;
        while (diffX < -static_cast<float>(M_PI)) diffX += 2.0f * static_cast<float>(M_PI);
        while (diffX > static_cast<float>(M_PI)) diffX -= 2.0f * static_cast<float>(M_PI);
        camTrans.rotate.x += diffX * rotLerpFactor;
        
        camTrans.rotate.z = isStartFrame ? 0.0f : std::lerp(camTrans.rotate.z, 0.0f, rotLerpFactor);
        
        // ==========================================
        // 【ボス撃破演出エフェクト】火花（Sparks）のみの洗練された演出
        // ==========================================
        if (bossDefeatTimer_ < 0.8f) {
            // --- 溜め期の火花スパーク ---
            if (particleManager_) {
                particleManager_->EmitCustomSparks(fighterWorldPos, 60.0f, 12, { 1.0f, 0.8f, 0.2f }, 1.0f);
            }
            // 空間の微振動
            cameraShakeTimer_ = 0.05f;
            cameraShakeIntensity_ = 0.8f;
            activeShakeIntensity_ = 0.8f;
            cameraShakeTimeMax_ = 0.05f;
        }
        else if (isDefeatBulletActive_) {
            // トドメ弾飛行中の火花スパーク
            if (particleManager_) {
                particleManager_->EmitCustomSparks(defeatBulletPos_, 80.0f, 15, { 1.0f, 0.7f, 0.1f }, 1.2f);
            }
        }
        else if (hasHitBoss_) {
            // --- 着弾後のボス全身火花スパークバースト ---
            if (particleManager_) {
                // 0.1秒おきにボスの胴体・関節から激しい火花が噴出
                static float lastSparkTime = -1.0f;
                if (bossDefeatHitTimer_ - lastSparkTime >= 0.1f) {
                    lastSparkTime = bossDefeatHitTimer_;
                    
                    std::uniform_real_distribution<float> offsetDist(-18.0f, 18.0f);
                    Vector3 sparkPos = {
                        bossPos.x + offsetDist(randomEngine_),
                        bossPos.y + offsetDist(randomEngine_),
                        bossPos.z + offsetDist(randomEngine_)
                    };
                    
                    // 高速度・高輝度の純粋な火花スパークのみを放射
                    particleManager_->EmitCustomSparks(sparkPos, 180.0f, 40, { 1.0f, 0.85f, 0.3f }, 2.0f);
                    particleManager_->EmitCustomSparks(sparkPos, 240.0f, 25, { 1.0f, 0.4f, 0.05f }, 2.5f);
                }
                
                // 毎フレーム降り注ぐ連続スパーク（土と石が飛び散るGPUパーティクルは出さず、純粋な火花スパークのみにする）
                particleManager_->EmitCustomSparks(bossPos, 140.0f, 20, { 1.0f, 0.9f, 0.4f }, 1.5f);
            }
        }

        // 2. トドメの巨大弾の自動発射 (0.8秒時点)
        if (bossDefeatTimer_ >= 0.8f && !isDefeatBulletActive_ && !hasHitBoss_) {
            isDefeatBulletActive_ = true;
            defeatBulletPos_ = fighterWorldPos;
            Vector3 dir = Normalize(Subtract(bossPos, defeatBulletPos_));
            float bulletSpeed = 380.0f;
            defeatBulletVel_ = Scale(dir, bulletSpeed);
            defeatBulletSize_ = 3.5f;
            
            // 発射瞬間の火花スパークバースト
            if (particleManager_) {
                particleManager_->EmitCustomSparks(fighterWorldPos, 120.0f, 50, { 1.0f, 0.8f, 0.2f }, 1.5f);
            }
            if (audio_) {
                PlaySE(jumpSE_, false, 1.5f);
            }
        }
        
        // 3. トドメの巨大弾の更新とヒット判定
        if (isDefeatBulletActive_) {
            defeatBulletPos_ = Add(defeatBulletPos_, Scale(defeatBulletVel_, kDeltaTime));
            
            // 衝突判定 (ボスの衝突半径または弾の進行方向でボスに到達・追い抜いたか)
            Vector3 diff = Subtract(bossPos, defeatBulletPos_);
            float dist = Length(diff);
            float forwardRemaining = diff.x * defeatBulletVel_.x + diff.y * defeatBulletVel_.y + diff.z * defeatBulletVel_.z;
            if (dist <= bossCollisionRadius_ || forwardRemaining <= 0.0f) {
                isDefeatBulletActive_ = false;
                hasHitBoss_ = true;
                isBossModelVisible_ = false; // ボスを消滅させる
                bossDefeatHitTimer_ = 0.0f;  // ヒットした瞬間からタイマーを開始
                
                // ★★★ スローモーション（ヒットストップ）の発動 ★★★
                hitstopTimer_ = 0.65f; // 0.65秒間超スローモーション化
                
                // 大きく白い十字Particleエフェクトを発生 (ボスのレール空間を考慮)
                float bossProgress = fighterWorldZ_ + bossZOffset_;
                Vector3 bRailDir = GetRailDirection(bossProgress);
                Vector3 bRailRight = CalculateRailRight(bRailDir);
                Vector3 bRailUp = CalculateRailUp(bRailDir, bRailRight);
                particleManager_->EmitWhiteCross(bossPos, bRailRight, bRailUp);
                
                // 白い超巨大多重衝撃波リングと巨大シリンダーエフェクトを追加してかっこよく演出
                particleManager_->EmitMegaRing(bossPos, {1.0f, 1.0f, 1.0f});
                particleManager_->EmitMegaCylinder(bossPos, {1.0f, 1.0f, 1.0f});
                
                // 周囲に飛び散る純白の激しいスパークエフェクトを追加
                particleManager_->EmitFlame(bossPos, 20.0f, 30, {1.0f, 1.0f, 1.0f});
                
                // ボスから地面・天に向けて四方八方に拡散する巨大稲妻を8本落とす
                Vector3 lightningColor = { 1.0f, 1.0f, 1.0f };
                for (int i = 0; i < 8; ++i) {
                    float theta = (i / 8.0f) * 2.0f * static_cast<float>(M_PI);
                    Vector3 strikeEnd = {
                        bossPos.x + std::cos(theta) * 60.0f,
                        bossPos.y - 40.0f, // 地面付近
                        bossPos.z + std::sin(theta) * 60.0f
                    };
                    particleManager_->EmitLSystemLightning(bossPos, strikeEnd, 4, 1.2f, lightningColor);
                }
                
                // 大爆発SE
                if (audio_) {
                    PlaySE(jumpSE_, false, 2.0f);
                }
                
                // 画面インパクトフラッシュ (真っ白に)
                flashAlpha_ = 1.0f;
                flashColor_ = { 1.0f, 1.0f, 1.0f, 1.0f };
                
                // 強烈なラジアルブラーによる爆発の衝撃波画面の歪み表現
                useRadialBlur_ = true;
                blurIntensity_ = 0.12f;
                maxBlurWidth_ = 0.12f;
                
                // 画面シェイクを2倍に強化して重厚な爆発振動を表現
                cameraShakeTimer_ = 0.80f;
                cameraShakeIntensity_ = 12.0f;
                activeShakeIntensity_ = 12.0f;
                cameraShakeTimeMax_ = 0.80f;
            }
        }
        
        // 被弾後タイマーのカウントを進める
        if (hasHitBoss_) {
            bossDefeatHitTimer_ += kDeltaTime;
        }
        
        // フラッシュの減衰速度をトドメ演出中はさらにゆっくりにする（かつ最初は最大で維持）
        if (flashAlpha_ > 0.0f) {
            float originalFade = kDeltaTime * ((selectedEffectPreset_ == 8) ? 1.5f : 3.5f);
            
            // 命中後0.4秒間はフラッシュを完全に1.0f（最大値）にロックして輝かせる
            if (bossDefeatHitTimer_ < 0.4f) {
                flashAlpha_ = 1.0f;
            } else {
                // その後、非常にゆっくりフェードアウトさせる（減衰速度 0.6f）
                float desiredFade = kDeltaTime * 0.6f;
                flashAlpha_ += (originalFade - desiredFade);
                flashAlpha_ = (std::min)(flashAlpha_, 1.0f);
            }
        }

        // 4. クリアシーンへの遷移判定 (撃破ヒット後、さらに2.0秒演出を持たせる)
        if (hasHitBoss_ && bossDefeatHitTimer_ >= 2.0f) {
            SceneManager::GetInstance()->ChangeScene("CLEAR");
            return;
        }
    }
    
    if (playerHP_ <= 0.0f && !isTitleMode_) {
        SceneManager::GetInstance()->ChangeScene("GAMEOVER");
        return;
    }

    // HPVisualの補間更新（滑らかなHPバー減少用）
    playerHPVisual_ = std::lerp(playerHPVisual_, playerHP_, 0.1f);
    bossHPVisual_ = std::lerp(bossHPVisual_, bossHP_, 0.1f);

    // デバフおよび画面効果タイマーの更新
    if (playerSpeedDebuffTimer_ > 0.0f) {
        playerSpeedDebuffTimer_ -= kDeltaTime;
    }
    if (screenWebTimer_ > 0.0f) {
        screenWebTimer_ -= kDeltaTime;
    }

    // ── 切れたロード画面が左右へ完全に画面外へ出た瞬間に「PHASE 1」を開始 ──
    bool isWaitingForTransition = (!isTitleMode_ && (SceneManager::GetInstance()->IsLoading() || bossAppearanceTimer_ > 0.0f));
    if (!isTitleMode_ && !isWaitingForTransition && !hasStartedInitialPhase_) {
        hasStartedInitialPhase_ = true;
        StartPhaseIntro(1);
    }

    // ── 「PHASE」表示演出（高速ブースト＆ソニックブーム）タイマーの更新 ──
    if (phaseIntroTimer_ >= 0.0f) {
        phaseIntroTimer_ += kDeltaTime;
        float t = phaseIntroTimer_;

        // フェーズごとのテーマカラー（PHASE 1: サイバーシアン / PHASE 2: オーバードライブ・アンバーゴールド）
        Vector3 primary3D = (phaseIntroNum_ == 1) ? Vector3{ 0.15f, 0.88f, 1.0f } : Vector3{ 1.0f, 0.48f, 0.10f };
        Vector3 accent3D  = (phaseIntroNum_ == 1) ? Vector3{ 0.55f, 0.96f, 1.0f } : Vector3{ 1.0f, 0.90f, 0.28f };

        // 現在の自機ワールド座標とレール基底を取得
        Vector3 pRailPos = GetRailPosition(fighterWorldZ_);
        Vector3 pRailDir = GetRailDirection(fighterWorldZ_);
        Vector3 pRailRight = CalculateRailRight(pRailDir);
        Vector3 pRailUp = CalculateRailUp(pRailDir, pRailRight);
        float pOffsetUp = (fighterModel_ ? fighterModel_->transform.translate.y : 0.0f) + 17.0f;
        float pOffsetRight = (fighterModel_ ? fighterModel_->transform.translate.x : 0.0f);
        Vector3 currentFighterPos = Add(pRailPos, Add(Scale(pRailRight, pOffsetRight), Scale(pRailUp, pOffsetUp)));

        if (t < 0.42f) {
            // 1. チャージ＆クロス突入フェーズ (0.0s 〜 0.42s): 機体周囲に気流とエネルギー火花が収束
            if (particleManager_) {
                particleManager_->EmitAeroWind(currentFighterPos, 85.0f, 3, primary3D);
                if (static_cast<int>(t * 60.0f) % 2 == 0) {
                    particleManager_->EmitCustomSparks(currentFighterPos, 70.0f, 6, accent3D, 0.2f);
                }
            }
        } else if (t < 1.75f) {
            // 2. ソニックブーム炸裂＆高速ブースト巡航フェーズ (0.42s 〜 1.75s)
            if (!hasTriggeredPhaseImpact_) {
                hasTriggeredPhaseImpact_ = true;

                if (particleManager_) {
                    // 多重ソニックブーム衝撃波リング＆シリンダー
                    particleManager_->EmitMegaRing(currentFighterPos, primary3D);
                    particleManager_->EmitRing(currentFighterPos, accent3D);
                    particleManager_->EmitCylinder(currentFighterPos, primary3D);
                    particleManager_->EmitCustomSparks(currentFighterPos, 165.0f, 50, accent3D, 0.35f);
                    particleManager_->EmitSlash(currentFighterPos, 110.0f, 24, primary3D);

                    // 機体から周囲の空間へ走る6本の稲妻放電
                    for (int i = 0; i < 6; ++i) {
                        float angle = (static_cast<float>(i) / 6.0f) * 2.0f * static_cast<float>(M_PI) + 0.3f;
                        Vector3 radialDir = Add(Scale(pRailRight, std::cos(angle) * 42.0f), Scale(pRailUp, std::sin(angle) * 32.0f));
                        Vector3 boltEnd = Add(currentFighterPos, Add(radialDir, Scale(pRailDir, 15.0f)));
                        particleManager_->EmitLSystemLightning(currentFighterPos, boltEnd, 3, 1.0f, accent3D);
                    }
                }

                if (audio_) {
                    PlaySE(jumpSE_, false, 1.6f);
                }
            }

            // ブースト巡航中の連続ソニック気流とスパーク
            if (particleManager_) {
                particleManager_->EmitAeroWind(currentFighterPos, 130.0f, 4, primary3D);
                if (static_cast<int>(t * 60.0f) % 3 == 0) {
                    particleManager_->EmitCustomSparks(currentFighterPos, 95.0f, 8, accent3D, 0.3f);
                }
            }
        } else if (t < kPhaseIntroDuration) {
            // 3. オーバードライブ斬撃＆ワープスプリット退場フェーズ (1.75s 〜 2.40s)
            if (!hasTriggeredPhaseExit_) {
                hasTriggeredPhaseExit_ = true;

                if (particleManager_) {
                    particleManager_->EmitRing(currentFighterPos, accent3D);
                    particleManager_->EmitSlash(currentFighterPos, 125.0f, 28, accent3D);
                    particleManager_->EmitCustomSparks(currentFighterPos, 135.0f, 35, primary3D, 0.3f);
                }

                if (audio_) {
                    PlaySE(jumpSE_, false, 1.35f);
                }
            }
        } else {
            // 演出完了: PHASE 1 / PHASE 2 のブースト演出が完了した瞬間に、現在の自機位置の前方へ新しく雑魚敵小隊を出現させる
            phaseIntroTimer_ = -1.0f;
            for (int g = 0; g < kNumGroups; ++g) {
                RespawnEnemyGroup(g, fighterWorldZ_);
            }
        }
    }

    // ── フェーズ自動遷移（決められたルートをたどりながら雑魚敵を倒してボスを探す。角を曲がる演出中はフェーズタイマーを一時停止） ──
    if (!isTitleMode_ && !isWaitingForTransition && !(phaseIntroTimer_ >= 0.0f) &&
        cornerState_ == CornerTurnState::kNone && currentPhase_ != GamePhase::kBossFight) {
        phaseTimer_ += kDeltaTime;
        float phaseProgress = std::clamp(phaseTimer_ / kPhaseDuration, 0.0f, 1.0f);
        if (currentPhase_ == GamePhase::kPhase1) {
            bossZOffset_ = std::lerp(1100.0f, 600.0f, phaseProgress);
        } else if (currentPhase_ == GamePhase::kPhase2) {
            bossZOffset_ = std::lerp(600.0f, 169.0f, phaseProgress);
        }

        if (phaseTimer_ >= kPhaseDuration) {
            phaseTimer_ = 0.0f;
            // フェーズ切り替え時に残っている雑魚敵を綺麗にクリア
            for (int eIdx = 0; eIdx < kMaxEnemies; ++eIdx) {
                enemies_[eIdx].isAlive = false;
            }
            if (currentPhase_ == GamePhase::kPhase1) {
                currentPhase_ = GamePhase::kPhase2;
                bossZOffset_ = 600.0f;
                StartPhaseIntro(2); // PHASE 2の表示演出
            } else if (currentPhase_ == GamePhase::kPhase2) {
                // ボス戦突入！ボスの姿を現し、再びボスが迫力の咆哮（Roar）を行う！
                currentPhase_ = GamePhase::kBossFight;
                isOpeningIntro_ = false; // ゲーム開始時の上空移動ではなく、ボス戦開始時の咆哮演出
                bossAppearanceTimer_ = kBossIntroTotalDuration - 9.6f; // t_total = 9.6s から開始し、直後の 9.8s〜11.2s で咆哮！
                isBossRoared_ = false;   // 再び咆哮エフェクト・衝撃波・SEを発動させる
                bossZOffset_ = 169.0f;
                bossActionState_ = BossActionState::kIdle;
                bossActionTimer_ = 0.0f;
            }
        }
    }

    // ── ゲーム開始演出（地球 -> 日本 -> 東京の夜景 -> ステージ俯瞰 -> ボス登場演出）タイマーの更新 ──
    if (bossAppearanceTimer_ > 0.0f) {
        // ロード画面がまだ開いている最中はタイマーを0秒(地球全体ビュー)に維持し、ロード画面が開き始めた瞬間からズーム開始
        bool holdIntroForLoading = (!isTitleMode_ && SceneManager::GetInstance()->IsLoading() && !LoadingScreen::GetInstance()->ShouldDrawNextSceneUnderneath());
        if (!holdIntroForLoading) {
            float currentT = kBossIntroTotalDuration - bossAppearanceTimer_;
            float timeScale = (!isOpeningIntro_ && currentT >= 11.2f) ? 2.4f : 1.0f;
            bossAppearanceTimer_ -= kDeltaTime * timeScale;
            warningScrollOffset_ += kDeltaTime;
        }
        float t = kBossIntroTotalDuration - bossAppearanceTimer_; // 0.0s ～ 12.5s

        // 0. 宇宙・上空からのズームイン期 (0.0s ～ 7.8s): 画面揺れやブラーを無効化してクリアに見せる
        if (t < 7.8f && !isTitleMode_) {
            useRadialBlur_ = false;
            blurIntensity_ = 0.0f;
            cameraShakeTimer_ = 0.0f;
            activeShakeIntensity_ = 0.0f;
        }

        // ① 7.8秒〜9.8秒: ボスに接近完了＆WARNINGアラートビート演出 + 地面踏み締め衝撃波 (7.8s時点で1回)
        if (t >= 7.8f && !isBossLanded_) {
            isBossLanded_ = true;
            cameraShakeTimer_ = 0.45f;
            cameraShakeIntensity_ = 3.0f;
            activeShakeIntensity_ = 3.0f;
            cameraShakeTimeMax_ = 0.45f;
            if (audio_) {
                PlaySE(jumpSE_, false, 2.0f);
            }
            Vector3 landPos = GetBossPosition(0.0f);
            for (int k = 0; k < 12; ++k) {
                particleManager_->EmitHit(landPos);
            }
            particleManager_->EmitRing(landPos);
            particleManager_->EmitCylinder(landPos);
        }

        if (t >= 7.8f && t < 9.8f && !isTitleMode_) {
            float warnT = t - 7.8f; // 0.0s ～ 2.0s
            int beatIdx = static_cast<int>(warnT / 0.50f);
            if (beatIdx != warningLastBeatIndex_) {
                warningLastBeatIndex_ = beatIdx;
                bool isMajorImpact = (beatIdx == 0 || beatIdx == 3);
                Vector3 bossPos = GetBossPosition(0.0f);
                Vector3 alertRed   = { 1.0f, 0.15f, 0.12f };
                Vector3 alertAmber = { 1.0f, 0.78f, 0.15f };
                if (particleManager_) {
                    particleManager_->EmitRing(bossPos, (beatIdx % 2 == 0) ? alertRed : alertAmber);
                    particleManager_->EmitCustomSparks(bossPos, 95.0f, 20, alertAmber, 0.25f);
                }
                if (audio_) {
                    PlaySE(jumpSE_, false, isMajorImpact ? 1.55f : 1.15f);
                }
            }
        }

        // ② 9.8秒時点: ボス咆哮・ビル吹き飛ばし (1回限り)
        if (t >= 9.8f && !isBossRoared_) {
            isBossRoared_ = true;
            cameraShakeTimer_ = 1.25f;
            cameraShakeIntensity_ = 0.9f;
            activeShakeIntensity_ = 0.9f;
            cameraShakeTimeMax_ = 1.25f;
            
            // 画面全体を赤いフラッシュで染める
            flashColor_ = { 1.0f, 0.15f, 0.15f };
            flashAlpha_ = 0.95f;

            if (audio_) {
                PlaySE(jumpSE_, false, 3.0f); // 大音量の咆哮SE
            }
            
            // ボス位置から赤い高威力エネルギー衝撃波群を大放出
            Vector3 bossPos = GetBossPosition(0.0f);
            particleManager_->EmitMegaRing(bossPos, {1.0f, 0.1f, 0.1f});
            particleManager_->EmitMegaCylinder(bossPos, {1.0f, 0.1f, 0.1f});
            particleManager_->EmitLightning(bossPos, 65.0f, 30, { 1.0f, 0.2f, 0.2f });
            particleManager_->EmitGravityVortex(bossPos, 35.0f, 40, { 0.8f, 0.1f, 0.1f });
            particleManager_->EmitChaosVoid(bossPos, 45.0f, 30, { 0.6f, 0.0f, 0.9f });
            particleManager_->EmitCustomSparks(bossPos, 90.0f, 50, { 1.0f, 0.4f, 0.1f }, 1.5f);

            // 周囲のビルを衝撃波で吹き飛ばす
            Vector3 roarPos = GetBossPosition(0.0f, 0.0f, 0.0f);
            float bossProgress = fighterWorldZ_ + bossZOffset_;
            Vector3 bRailDir = GetRailDirection(bossProgress);
            Vector3 bRailRight = CalculateRailRight(bRailDir);
            for (size_t i = 0; i < buildings_.size(); ++i) {
                if (!buildings_[i].isDestroyed) {
                    Vector3 diff = Subtract(buildings_[i].position, roarPos);
                    float distSq = diff.x * diff.x + diff.y * diff.y + diff.z * diff.z;
                    if (distSq < 180.0f * 180.0f) { // 180m 以内のビルを吹き飛ばす
                        buildings_[i].isDestroyed = true;
                        buildings_[i].destroyTimer = 0.0f;
                        
                        float sideDot = diff.x * bRailRight.x + diff.z * bRailRight.z;
                        float signSide = (sideDot >= 0.0f) ? 1.0f : -1.0f;
                        Vector3 outDir = Scale(bRailRight, signSide * 85.0f);
                        buildings_[i].velocity = { outDir.x, 70.0f, outDir.z - 20.0f + (float)(randomEngine_() % 30) };
                        buildings_[i].rotationSpeed = {
                            ((float)(randomEngine_() % 200) / 100.0f) - 1.0f,
                            ((float)(randomEngine_() % 200) / 100.0f) - 1.0f,
                            signSide * 4.0f
                        };
                    }
                }
            }
        }

        // ③ 9.8秒〜11.2秒: ボスの咆哮エネルギー連続噴出演出 ＆ 画面揺れ・空気の振動ブラー(集中線) ＆ FOVズーム
        if (t >= 9.8f && t < 11.2f) {
            float roarProgress = std::clamp((t - 9.8f) / 1.4f, 0.0f, 1.0f);
            float easeRoar = std::sin(roarProgress * static_cast<float>(M_PI)); // 0 -> 1 -> 0

            float roarBlur = 0.005f * easeRoar;
            useRadialBlur_ = true;
            blurIntensity_ = roarBlur;
            maxBlurWidth_ = 0.005f;
            activePostProcess_ = kRadialBlur;
            if (radialBlurParamData_) {
                radialBlurParamData_->center = { 0.5f, 0.5f };
                radialBlurParamData_->blurWidth = -roarBlur;
            }

            cameraShakeTimer_ = 0.15f;
            cameraShakeTimeMax_ = 0.15f;
            cameraShakeIntensity_ = 0.9f * easeRoar;
            activeShakeIntensity_ = 0.9f * easeRoar;
            std::uniform_real_distribution<float> distRoarShake(-1.0f, 1.0f);
            cameraShakeOffset_ = {
                distRoarShake(randomEngine_) * activeShakeIntensity_,
                distRoarShake(randomEngine_) * activeShakeIntensity_,
                distRoarShake(randomEngine_) * activeShakeIntensity_
            };

            float fov = 0.45f;
            if (roarProgress < 0.25f) {
                float zoomT = roarProgress / 0.25f;
                fov = std::lerp(0.45f, 0.28f, zoomT * zoomT);
            } else {
                float zoomT = (roarProgress - 0.25f) / 0.75f;
                if (zoomT < 0.5f) {
                    float backT = zoomT / 0.5f;
                    fov = std::lerp(0.28f, 0.52f, backT);
                } else {
                    float backT = (zoomT - 0.5f) / 0.5f;
                    fov = std::lerp(0.52f, 0.45f, backT);
                }
            }
            camera_->SetFov(fov);

            static int emitCounter = 0;
            emitCounter++;
            if (emitCounter % 3 == 0) {
                Vector3 bossPos = GetBossPosition(0.0f);
                float bossProgress = fighterWorldZ_ + bossZOffset_;
                Vector3 bossRailDir = GetRailDirection(bossProgress);
                Vector3 bossRailUp = Cross(bossRailDir, Normalize(Vector3{ -bossRailDir.z, 0.0f, bossRailDir.x }));
                Vector3 mouthPos = Add(bossPos, Add(Scale(bossRailUp, 6.0f), Scale(bossRailDir, -22.0f)));
                particleManager_->EmitFlame(mouthPos, 70.0f, 6, { 1.0f, 0.1f, 0.1f });
                particleManager_->EmitCustomSparks(mouthPos, 80.0f, 10, { 1.0f, 0.5f, 0.1f }, 1.0f);
            }
        } else if (t >= 11.2f) {
            camera_->SetFov(0.45f);
            useRadialBlur_ = false;
            blurIntensity_ = 0.0f;
            cameraShakeTimer_ = 0.0f;
            activeShakeIntensity_ = 0.0f;
            cameraShakeOffset_ = { 0.0f, 0.0f, 0.0f };
        }

        // ④ 演出終了時のリセットとクリーンアップ
        if (bossAppearanceTimer_ <= 0.0f) {
            bossAppearanceTimer_ = -1.0f;
            earthAlpha_ = 0.0f;
            japanAlpha_ = 0.0f;
            tokyoAlpha_ = 0.0f;
            camera_->ClearTarget(); // 演出終了時に確実に注視ターゲットを解除
            camera_->SetFov(0.45f); // FOVを確実に初期値に戻す
            useRadialBlur_ = false;
            blurIntensity_ = 0.0f;
            cameraShakeTimer_ = 0.0f;
            cameraShakeOffset_ = { 0.0f, 0.0f, 0.0f };
            activePostProcess_ = kNone; // ラジアルブラーを解除
        }
    }

    // ボスの足アニメーション時間更新（最初からステージ上にいるため常時更新、撃破演出中は停止）
    if (!isTitleMode_ && !isBossDefeatedSequence_) {
        bossTime_ += kDeltaTime * bossLegSwingSpeed_;

        // ── ボス通過痕（地面ひび割れ＆破片スパーク）演出 ──
        if (currentPhase_ == GamePhase::kBossFight && bossAppearanceTimer_ <= 0.0f) {
            bossStepCrackTimer_ += kDeltaTime;
            if (bossStepCrackTimer_ >= 0.15f) { // 0.15秒周期で足元にひび割れ破片を放出
                bossStepCrackTimer_ = 0.0f;
                float bodyBounce = 0.0f;
                Vector3 bossPos = GetBossPosition(bodyBounce);
                if (particleManager_) {
                    std::uniform_real_distribution<float> crackDist(-18.0f, 18.0f);
                    Vector3 crackPos = { bossPos.x + crackDist(randomEngine_), kFloorY + 0.3f, bossPos.z + crackDist(randomEngine_) };
                    // 地面が裂けて砕けるひび割れショックウェーブ & 破片火花
                    particleManager_->EmitCustomSparks(crackPos, 50.0f, 8, { 0.95f, 0.65f, 0.25f }, 1.2f);
                    particleManager_->EmitMegaRing(crackPos, { 0.85f, 0.45f, 0.15f });
                }
            }
        }
    }

    // ボス戦時またはタイトル背景時は雑魚敵をすべて非生存にする
    if (currentPhase_ == GamePhase::kBossFight || isTitleMode_) {
        for (auto& enemy : enemies_) {
            enemy.isAlive = false;
        }
    }

    randomEffectTime_ += kDeltaTime * randomSpeed_;
    if (randomParamData_) {
        randomParamData_->time = randomEffectTime_;
        randomParamData_->noiseScale = randomNoiseScale_;
        randomParamData_->noiseStrength = randomNoiseStrength_;
        randomParamData_->isColorNoise = randomIsColorNoise_ ? 1.0f : 0.0f;
        randomParamData_->isMultiplyNoise = (randomNoiseType_ == 1) ? 1.0f : 0.0f;
    }

    if (isTransitioning_) {
        transitionThreshold_ -= kDeltaTime * 0.5f; // Fade in over 2 seconds
        if (transitionThreshold_ <= 0.0f) {
            transitionThreshold_ = 0.0f;
            isTransitioning_ = false;
            activePostProcess_ = kNone;
            // 前のシーンのメモリを完全に解放
            SceneManager::GetInstance()->ClearPreviousScene();
            // ディゾルブ完了後にPHASE 1演出を開始
            if (!isTitleMode_) {
                StartPhaseIntro(1);
            }
        }
        dissolveParamData_->threshold = transitionThreshold_;
    }

    // ── プレイヤーのワールド位置の共通計算 ──
    EulerTransform& camTransForPos = camera_->GetTransform();
    Vector3 fighterWorldPos = {
        camTransForPos.translate.x + fighterModel_->transform.translate.x,
        camTransForPos.translate.y - 3.0f + fighterModel_->transform.translate.y,
        fighterWorldZ_
    };

#ifdef USE_IMGUI
    // ── ボス足攻撃のデバッグ用オーバーレイ表示 ──
    ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f); // 半透明背景
    ImGui::Begin("Boss Leg Debug", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::SetWindowFontScale(2.0f); // 文字サイズを大きく
    
    int currentAttackLeg = 5;
    
    if (bossActionState_ == BossActionState::kLegAttack) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "LEG ATTACK ACTIVE!");
        ImGui::Text("Active Leg Index: %d", currentAttackLeg);
        ImGui::Text("Timer: %.2fs", bossActionTimer_);
    } else {
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "Boss State: Normal");
        ImGui::Text("Target Leg if attack starts: %d", currentAttackLeg);
    }
    ImGui::End();

    ImGui::SetNextWindowSize(ImVec2(500, 600), ImGuiCond_FirstUseEver);
    ImGui::Begin("GamePlay Control");

    // ── ゲームフェーズ制御 ──
    ImGui::Separator();
    ImGui::Text("Game Phase Control");
    const char* phaseNames[] = { "1 Phase (Enemies)", "2 Phase (Enemies)", "Boss Fight (Big Spider)" };
    int currentPhaseInt = static_cast<int>(currentPhase_);
    if (ImGui::Combo("Game Phase", &currentPhaseInt, phaseNames, IM_ARRAYSIZE(phaseNames))) {
        currentPhase_ = static_cast<GamePhase>(currentPhaseInt);
        phaseTimer_ = 0.0f; // 切り替え時にタイマーをリセット
        if (currentPhase_ == GamePhase::kPhase1) {
            StartPhaseIntro(1);
        } else if (currentPhase_ == GamePhase::kPhase2) {
            StartPhaseIntro(2);
        }
    }
    if (ImGui::Button("Replay PHASE 1 Intro")) {
        currentPhase_ = GamePhase::kPhase1;
        phaseTimer_ = 0.0f;
        StartPhaseIntro(1);
    }
    ImGui::SameLine();
    if (ImGui::Button("Replay PHASE 2 Intro")) {
        currentPhase_ = GamePhase::kPhase2;
        phaseTimer_ = 0.0f;
        StartPhaseIntro(2);
    }
    if (currentPhase_ != GamePhase::kBossFight) {
        ImGui::ProgressBar(phaseTimer_ / kPhaseDuration, ImVec2(0.0f, 0.0f));
        ImGui::Text("Time remaining: %.1fs", kPhaseDuration - phaseTimer_);
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "BOSS FIGHT ACTIVE");
    }

    // ── 背景切り替えとパラメータ調整 ──
    ImGui::Separator();
    ImGui::Text("SkyDome Control");
    const char* bgNames[] = { "Green.png (Green)", "Red.png (Red)" };
    ImGui::Combo("Sky Texture", &activeBackgroundTex_, bgNames, IM_ARRAYSIZE(bgNames));
    if (ImGui::Button("Switch Sky Texture (T key)")) {
        activeBackgroundTex_ = (activeBackgroundTex_ + 1) % kNumSkyTextures;
    }
    ImGui::DragFloat("Sky UV Scroll X", &bgUvScrollX_, 0.001f, 0.0f, 1.0f, "%.3f");
    ImGui::DragFloat("Sky UV Scroll Y", &bgUvScrollY_, 0.001f, 0.0f, 1.0f, "%.3f");

    // ── プレイヤーHP調整 ──
    ImGui::Separator();
    ImGui::Text("Player HP Status");
    ImGui::SliderFloat("Player HP", &playerHP_, 0.0f, playerMaxHP_, "%.1f");

    // ── 蜘蛛ボス調整項目 ──
    if (currentPhase_ == GamePhase::kBossFight) {
        ImGui::Separator();
        ImGui::DragFloat("Boss Collision Radius", &bossCollisionRadius_, 0.1f, 1.0f, 100.0f, "%.1f");
        ImGui::DragFloat("Boss Body Scale (big+Spider)", &bossBodyScale_, 0.1f, 0.01f, 100.0f, "%.2f");
        ImGui::DragFloat("Boss Leg Scale (big+spider+arm)", &bossLegScale_, 0.1f, 0.01f, 100.0f, "%.2f");
        ImGui::DragFloat("Boss Y Height", &bossYOffset_, 0.5f, -200.0f, 100.0f, "%.1f");
        ImGui::DragFloat("Boss Z Offset", &bossZOffset_, 1.0f, 10.0f, 1000.0f, "%.1f");
        ImGui::DragFloat("Boss Body RotY", &bossBodyRotY_, 1.0f, 0.0f, 360.0f, "%.1f");
        ImGui::DragFloat("Body Bounce Range", &bossBodyBounceRange_, 0.01f, 0.0f, 5.0f, "%.2f");
        ImGui::DragFloat("Body Roll Range (deg)", &bossBodyRollRange_, 0.05f, 0.0f, 15.0f, "%.1f");
        ImGui::DragFloat3("Boss Web Fire Offset (Ass)", &bossWebFireOffset_.x, 0.1f, -100.0f, 100.0f, "%.2f");
        
        ImGui::Text("--- Symmetrical Leg Pairs (X, Y, Z Offsets) ---");
        ImGui::DragFloat3("Pair 0 (Front) Offset", &bossLegPairPos0_.x, 0.05f, -20.0f, 20.0f, "%.2f");
        ImGui::DragFloat("Pair 0 (Front) RotY", &bossLegPairRotY0_, 1.0f, -180.0f, 180.0f, "%.1f");
        
        ImGui::DragFloat3("Pair 1 (Mid-Front) Offset", &bossLegPairPos1_.x, 0.05f, -20.0f, 20.0f, "%.2f");
        ImGui::DragFloat("Pair 1 (Mid-Front) RotY", &bossLegPairRotY1_, 1.0f, -180.0f, 180.0f, "%.1f");
        
        ImGui::DragFloat3("Pair 2 (Mid-Back) Offset", &bossLegPairPos2_.x, 0.05f, -20.0f, 20.0f, "%.2f");
        ImGui::DragFloat("Pair 2 (Mid-Back) RotY", &bossLegPairRotY2_, 1.0f, -180.0f, 180.0f, "%.1f");
        
        ImGui::DragFloat3("Pair 3 (Back) Offset", &bossLegPairPos3_.x, 0.05f, -20.0f, 20.0f, "%.2f");
        ImGui::DragFloat("Pair 3 (Back) RotY", &bossLegPairRotY3_, 1.0f, -180.0f, 180.0f, "%.1f");

        ImGui::Text("--- Leg Local Pivot Offset ---");
        ImGui::DragFloat("Pivot Y (Up/Down)", &bossLegPivotY_, 0.01f, -1.0f, 1.0f, "%.2f");
        ImGui::DragFloat("Pivot Z (Front/Back)", &bossLegPivotZ_, 0.01f, -1.0f, 1.0f, "%.2f");

        ImGui::Text("--- Walk Animation Motion ---");
        ImGui::DragFloat("Walk Speed", &bossLegSwingSpeed_, 0.1f, 0.0f, 20.0f, "%.1f");
        ImGui::DragFloat("Walk Swing Range", &bossLegSwingRange_, 0.01f, 0.0f, 2.0f, "%.2f");
        ImGui::DragFloat("Walk Step Lift Range", &bossLegLiftRange_, 0.05f, 0.0f, 10.0f, "%.2f");
    }

    ImGui::Separator();
    ImGui::Checkbox("Show SimpleSkin", &showSimpleSkin_);
    ImGui::Checkbox("Show AnimatedCube", &showAnimatedCube_);
    ImGui::Checkbox("Show Particles", &showParticles_);
    ImGui::Checkbox("Show Skybox", &showSkybox_);
    if (showSkybox_) {
        const char* skyboxItems[] = {
            "Moonless Golf (Night)",
            "Qwantani Night Puresky"
        };
        ImGui::Combo("Skybox Type", &skyboxType_, skyboxItems, IM_ARRAYSIZE(skyboxItems));
    }
    ImGui::Checkbox("Show Enemies", &showEnemies_);
    ImGui::DragFloat2("Sprite Position", &spritePos_.x, 1.0f, -2000.0f, 2000.0f, "%.1f");
    if (ImGui::SliderFloat("Model Reflection", &modelEnvCoefficient_, 0.0f, 1.0f)) {
        if (playerModel_) playerModel_->SetEnvironmentCoefficient(modelEnvCoefficient_);
    }


    if (fighterModel_) {
        if (ImGui::DragFloat("Fighter Model Scale", &fighterModel_->transform.scale.x, 0.1f, 0.1f, 100.0f, "%.1f")) {
            fighterModel_->transform.scale.y = fighterModel_->transform.scale.x;
            fighterModel_->transform.scale.z = fighterModel_->transform.scale.x;
        }
    }
    
    ImGui::Text("F1 Key: Toggle Debug Camera (Free Camera)");
    if (sceneMode_ == SceneMode::kCamera) {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "  [WASDQE] Move Camera (LShift: Turbo)");
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "  [ZC] Rotate Camera Left/Right (Yaw)");
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "  [Right-Click Drag] Look Around");
        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "  (Mouse cursor is active for ImGui!)");
    }
    ImGui::Text("TAB Key: Switch Scene Mode");
    ImGui::Text("Current Mode: %s", (sceneMode_ == SceneMode::kMouse ? "Mouse" : (sceneMode_ == SceneMode::kCamera ? "Camera" : "Fighter")));

    // ── Blender連携パネル ──
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.6f, 1.0f), "[Blender Sync]");
    ImGui::Text("game_state.json: Updating every 3 frames");
    if (replayRecording_) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.1f, 0.1f, 1.0f));
        if (ImGui::Button("[REC] Stop Replay (R key)")) {
            replayRecording_ = false;
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Frame: %d", replayFrameCounter_);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.6f, 0.2f, 1.0f));
        if (ImGui::Button("[REC] Start Replay (R key)")) {
            replayRecording_ = true;
            replayFrameCounter_ = 0;
            replaySessionIndex_++;
            std::string filename = "Resources/replay_frames_" + std::to_string(replaySessionIndex_) + ".csv";
            std::ofstream repFileHdr(filename, std::ios::trunc);
            if (repFileHdr.is_open()) {
                repFileHdr << "frame,px,py,pz,proll,ppitch";
                for (int i = 0; i < kMaxEnemies; ++i) {
                    repFileHdr << ",e" << i << "_alive,e" << i << "_x,e" << i << "_y,e" << i << "_z,e" << i << "_rz";
                }
                for (int i = 0; i < 20; ++i) {
                    repFileHdr << ",b" << i << "_alive,b" << i << "_x,b" << i << "_y,b" << i << "_z";
                }
                repFileHdr << ",boss_active,boss_x,boss_y,boss_z,boss_ry\n";
                repFileHdr.close();
            }
        }
        ImGui::PopStyleColor();
    }
    ImGui::Text("Blender RT : blender_realtime_viewer.py");
    ImGui::Text("Replay     : blender_replay_viewer.py");
    ImGui::Text("LevelEditor: blender_level_editor.py");
    ImGui::Text("ParamEdit  : blender_param_editor.py");
    ImGui::End();
#endif

    // マウスホイールによる環境マップ反射強度の調整
    int32_t wheel = input_->GetWheelDelta();
    if (wheel != 0) {
        modelEnvCoefficient_ += static_cast<float>(wheel) / 12000.0f; // 感度調整 (120ごとに0.01動く程度)
        modelEnvCoefficient_ = std::clamp(modelEnvCoefficient_, 0.0f, 1.0f);
        if (playerModel_) playerModel_->SetEnvironmentCoefficient(modelEnvCoefficient_);
    }

    if (input_->IsKeyPressed(DIK_1)) currentEffect_ = kTypeExplosion;
    if (input_->IsKeyPressed(DIK_2)) currentEffect_ = kTypeFountain;
    if (input_->IsKeyPressed(DIK_3)) currentEffect_ = kTypeSpiral;
    if (input_->IsKeyPressed(DIK_4)) currentEffect_ = kTypeRain;
    
    // キー5を押した瞬間のみ、ヒットエフェクトとリングエフェクトを発生させる
    if (input_->IsKeyTriggered(DIK_5)) {
        particleManager_->EmitHit(emitterPos_);
        particleManager_->EmitRing(emitterPos_);
    }

    if (input_->IsKeyTriggered(DIK_6)) {
        particleManager_->EmitCylinder(emitterPos_);
    }

    if (input_->IsKeyTriggered(DIK_G)) useGravity_ = !useGravity_;

    // SPACEキーを押した瞬間にSEを再生（戦闘機モード以外のみ。戦闘機モードはブーストSE対応予定）
    if (input_->IsKeyTriggered(DIK_SPACE) && sceneMode_ != SceneMode::kFighter) {
        PlaySE(jumpSE_, false, 1.0f);
    }
    EulerTransform& camTrans = camera_->GetTransform();
    HWND hwnd = WinApp::GetInstance()->GetHwnd();

    // F1キーでデバッグカメラ（第三者視点フリーカメラ）を即時切り替え
    if (input_->IsKeyTriggered(DIK_F1)) {
        if (sceneMode_ == SceneMode::kCamera) {
            // 現在デバッグカメラ（Camera）モードなら、元のモードに戻す
            sceneMode_ = preDebugSceneMode_;

            // 元の操作に戻る際は、ImGuiなどを快適に操作できるよう必ずマウスカーソルを確実に再表示する
            // ShowCursorの内部カウンタが負に累積して消えたままになるのを防ぐため、表示(>=0)されるまで呼び出す
            while (ShowCursor(TRUE) < 0);

            // ── 元の操作に戻る際、カメラの回転・位置を一瞬で戦闘機背後に完全リセット！ ──
            if (sceneMode_ == SceneMode::kFighter) {
                // 1. 回転を正面(0,0,0)にリセット（デバッグ時の首振り角を引き継がない）
                camTrans.rotate = { 0.0f, 0.0f, 0.0f };

                // 2. 位置（X, Y, Z）を戦闘機追従のジャストな初期背後位置に一瞬でワープ
                float targetCamX = 0.0f;
                float targetCamY = 0.0f;
                if (fighterModel_) {
                    targetCamX = fighterModel_->transform.translate.x * 0.5f;
                    targetCamY = fighterModel_->transform.translate.y * 0.5f;
                }
                camTrans.translate.x = targetCamX;
                camTrans.translate.y = targetCamY;
                camTrans.translate.z = fighterWorldZ_ - 65.0f;
            }
        } else {
            // それ以外なら、現在のモードを保存してデバッグカメラ（Camera）に切り替える
            preDebugSceneMode_ = sceneMode_;
            sceneMode_ = SceneMode::kCamera;

            // デバッグカメラに入る際は最初はImGuiをいじるため、マウスカーソルを確実に表示状態にする
            while (ShowCursor(TRUE) < 0);

            // カメラの回転を正面にリセット（切り替え直後に迷子になるのを防止）
            camTrans.rotate = { 0.0f, 0.0f, 0.0f };
        }
    }

    // TABキーでモード切替
    if (input_->IsKeyTriggered(DIK_TAB) && !isTitleMode_) {
        if (sceneMode_ == SceneMode::kMouse) {
            sceneMode_ = SceneMode::kCamera;
            ShowCursor(FALSE);
        } else if (sceneMode_ == SceneMode::kCamera) {
            sceneMode_ = SceneMode::kFighter;
            ShowCursor(FALSE);
        } else {
            sceneMode_ = SceneMode::kMouse;
            ShowCursor(TRUE);
        }
    }

    // Bキーでボスシーン（kBossFightフェーズ）へ即座にスキップ / リプレイ
    if (input_->IsKeyTriggered(DIK_B)) {
        currentPhase_ = GamePhase::kBossFight;
        bossAppearanceTimer_ = kBossIntroTotalDuration - 6.4f;
        warningScrollOffset_ = 0.0f; // WARNINGアニメーション用スクロールタイマーリセット
        warningLastBeatIndex_ = -1;
        phaseTimer_ = 0.0f;          // 通常フェーズのタイマーをリセット
        bossZOffset_ = 169.0f;       // ボスZオフセットを初期位置にリセット
        bossTime_ = 0.0f;            // ボスタイマーのリセット
        isBossLanded_ = false;       // 着地フラグのリセット
        isBossRoared_ = false;       // 咆哮フラグのリセット
        hasEmittedJumpStart_ = false;
        hasEmittedJumpEnd_ = false;
        bossActionState_ = BossActionState::kIdle; // アクションステートのリセット
        bossActionTimer_ = 0.0f;     // アクションタイマーのリセット
        for (auto& enemy : enemies_) {
            enemy.isAlive = false;
        }
    }

    if (sceneMode_ == SceneMode::kCamera) {
        // 右クリックが押されている間（ドラッグ中）だけカメラを回転させる（カーソルロック）
        if (input_->IsMousePressed(1)) {
            // カメラ回転中はマウスカーソルを確実に非表示にする
            while (ShowCursor(FALSE) >= 0);

            RECT rect;
            GetWindowRect(hwnd, &rect);
            int centerX = rect.left + (rect.right - rect.left) / 2;
            int centerY = rect.top + (rect.bottom - rect.top) / 2;
            POINT currentPos;
            GetCursorPos(&currentPos);

            float deltaX = static_cast<float>(currentPos.x - centerX);
            float deltaY = static_cast<float>(currentPos.y - centerY);

            SetCursorPos(centerX, centerY);

            camTrans.rotate.y += deltaX * mouseSensitivity_;
            camTrans.rotate.x += deltaY * mouseSensitivity_;

            const float pitchLimit = static_cast<float>(M_PI / 2.0 - 0.01);
            camTrans.rotate.x = std::clamp(camTrans.rotate.x, -pitchLimit, pitchLimit);
        } else {
            // 右クリックを離している間は、ImGuiの操作ができるようにマウスカーソルを確実に表示する
            while (ShowCursor(TRUE) < 0);
        }
        
        // Z / C キーによるカメラの左右旋回（横回転）
        float keyboardRotSpeed = 1.5f; // 秒間の回転速度(ラジアン)
        if (input_->IsKeyPressed(DIK_Z)) {
            camTrans.rotate.y -= keyboardRotSpeed * kDeltaTime;
        }
        if (input_->IsKeyPressed(DIK_C)) {
            camTrans.rotate.y += keyboardRotSpeed * kDeltaTime;
        }
        
        Vector3 moveDir = { 0.0f, 0.0f, 0.0f };
        if (input_->IsKeyPressed(DIK_W)) moveDir.z += 1.0f;
        if (input_->IsKeyPressed(DIK_S)) moveDir.z -= 1.0f;
        if (input_->IsKeyPressed(DIK_D)) moveDir.x += 1.0f;
        if (input_->IsKeyPressed(DIK_A)) moveDir.x -= 1.0f;
        if (input_->IsKeyPressed(DIK_E)) moveDir.y += 1.0f;
        if (input_->IsKeyPressed(DIK_Q)) moveDir.y -= 1.0f;
        
        if (moveDir.x != 0.0f || moveDir.y != 0.0f || moveDir.z != 0.0f) {
            // フリーカメラの基本移動速度を大幅に向上（元の 5.0f は遅すぎたため 80.0f に設定）
            // さらに左Shiftまたは右Shiftを押している間は、高速ターボ移動（300.0f）できるように拡張
            float cameraSpeed = 80.0f;
            if (input_->IsKeyPressed(DIK_LSHIFT) || input_->IsKeyPressed(DIK_RSHIFT)) {
                cameraSpeed = 300.0f;
            }
            Matrix4x4 cameraRotY = MakeRotateYMatrix(camTrans.rotate.y);
            Vector3 rotatedMoveDir = TransformNormal(moveDir, cameraRotY);
            rotatedMoveDir = Normalize(rotatedMoveDir);
            rotatedMoveDir = Scale(rotatedMoveDir, cameraSpeed * kDeltaTime);
            camTrans.translate = Add(camTrans.translate, rotatedMoveDir);
        }
    } else if (sceneMode_ == SceneMode::kFighter) {
        // --- 戦闘機（レールシューター）モード ---

        // ── ブースト処理（LSHIFTで発動、バレルロール終了と同時に自動で戻る） ────────────────
        bool isBossIntro = (bossAppearanceTimer_ > 0.0f);
        if (input_->IsKeyTriggered(DIK_LSHIFT) && !(phaseIntroTimer_ >= 0.0f) && !isBossIntro && !isWaitingForTransition && !isBossDefeatedSequence_ && !isTitleMode_) {
            if (!isBarrelRolling_) {
                isBoosting_ = true;
                isBarrelRolling_ = true;
                barrelRollTimer_ = 0.0f;
            }
        }

        if (isTitleMode_ && isTitleTransitioning_) {
            if (titleTransitionTimer_ < 1.2f) {
                boostForwardSpeed_ = kNormalSpeed; // 前半1.2秒は通常速度でカメラ戻しを待つ
                boostBlurWidth_ = 0.0f;           // ブラーは発生させない
            } else {
                boostForwardSpeed_ = 750.0f;       // 後半は勢いよくブーストで飛び去る
                boostBlurWidth_ = kBoostBlurMax * 0.25f; // 弱いラジアルブラーを適用
            }
        } else if (phaseIntroTimer_ >= 0.0f && !isTitleMode_) {
            // フェーズ演出中のスピード制御（画面ブラーは発生させない）
            float pt = phaseIntroTimer_;
            boostBlurWidth_ = 0.0f;
            if (pt < 0.42f) {
                boostForwardSpeed_ = 115.0f;
            } else if (pt < 1.75f) {
                float u = (pt - 0.42f) / (1.75f - 0.42f);
                boostForwardSpeed_ = std::lerp(245.0f, 155.0f, u);
            } else {
                float u = std::clamp((pt - 1.75f) / (kPhaseIntroDuration - 1.75f), 0.0f, 1.0f);
                boostForwardSpeed_ = std::lerp(155.0f, kNormalSpeed, u);
            }
        } else if (isBoosting_) {
            boostBlurWidth_ += kBoostBlurFadeIn * kDeltaTime * kBoostBlurMax;
            boostBlurWidth_ = (std::min)(boostBlurWidth_, kBoostBlurMax);
            boostForwardSpeed_ += (kBoostSpeedMax - boostForwardSpeed_) * 0.05f;
        } else {
            boostBlurWidth_ -= kBoostBlurFadeOut * kDeltaTime * kBoostBlurMax;
            boostBlurWidth_ = (std::max)(boostBlurWidth_, 0.0f);
            boostForwardSpeed_ += (kNormalSpeed - boostForwardSpeed_) * 0.05f;
        }

        // RadialBlur の適用制御（単発のuseRadialBlur_演出中はそちらの強度と大きい方を優先）
        float effectiveBlur = boostBlurWidth_;
        if (useRadialBlur_ && blurIntensity_ > effectiveBlur) {
            effectiveBlur = blurIntensity_;
        }
        if (effectiveBlur > 0.0001f) {
            if (!isTransitioning_) activePostProcess_ = kRadialBlur;
            if (radialBlurParamData_) {
                radialBlurParamData_->center = { 0.5f, 0.5f };
                radialBlurParamData_->blurWidth = -effectiveBlur;
            }
        } else {
            if (activePostProcess_ == kRadialBlur && !isTransitioning_) {
                activePostProcess_ = kNone;
            }
        }

        // ── 0. 曲がり角（A / D 長押しルート選択）イベントの開始判定と状態更新 ──────────────────
        bool isBossIntroCam = isBossIntro;
        if (!isTitleMode_ && !isWaitingForTransition && !isBossIntroCam && !(phaseIntroTimer_ >= 0.0f) && !isBossDefeatedSequence_) {
            if (cornerState_ == CornerTurnState::kNone) {
                for (size_t cIdx = 0; cIdx < corners_.size(); ++cIdx) {
                    if (!corners_[cIdx].completed &&
                        fighterWorldZ_ >= corners_[cIdx].triggerS &&
                        fighterWorldZ_ < corners_[cIdx].cornerS + 15.0f) {
                        activeCornerIndex_ = static_cast<int>(cIdx);
                        cornerState_ = CornerTurnState::kPromptAndHold;
                        cornerHoldingDir_ = 0;
                        cornerChosenDir_ = 0;
                        cornerHoldProgress_ = 0.0f;
                        cornerTimer_ = 0.0f;
                        wrongRouteDarkenAlpha_ = 0.0f;
                        isBoosting_ = false;
                        isBarrelRolling_ = false;

                        // 先にいる雑魚敵が正しい曲がり角の方向へ逃げていく姿をプレイヤーに見せるため、生存中の敵を角の先へ誘導配置
                        // （※PHASE 2前やボス戦前の残り3.5秒以内は、直後の演出で消えて見えるのを防ぐため新規出現させない）
                        bool isNearPhaseTransition = (kPhaseDuration - phaseTimer_) <= 3.5f;
                        if (currentPhase_ != GamePhase::kBossFight && !isNearPhaseTransition) {
                            int aliveCount = 0;
                            for (int eIdx = 0; eIdx < kMaxEnemies; ++eIdx) {
                                if (enemies_[eIdx].isAlive) aliveCount++;
                            }
                            if (aliveCount == 0) {
                                RespawnEnemyGroup(0, fighterWorldZ_);
                            }
                            for (int eIdx = 0; eIdx < kMaxEnemies; ++eIdx) {
                                if (enemies_[eIdx].isAlive) {
                                    enemies_[eIdx].state = Enemy::State::kWander;
                                    enemies_[eIdx].stateTimer = 0.0f;
                                    enemies_[eIdx].relativeZ = (corners_[cIdx].cornerS - fighterWorldZ_) + 45.0f + (eIdx % 5) * 10.0f;
                                }
                            }
                        }
                        break;
                    }
                }
            }

            if (cornerState_ == CornerTurnState::kPromptAndHold && activeCornerIndex_ >= 0 && activeCornerIndex_ < static_cast<int>(corners_.size())) {
                const CornerInfo& corner = corners_[activeCornerIndex_];
                cornerTimer_ += kDeltaTime;

                // Aキー（左）または Dキー（右）の長押し入力判定
                bool holdA = input_->IsKeyPressed(DIK_A) && !input_->IsKeyPressed(DIK_D);
                bool holdD = input_->IsKeyPressed(DIK_D) && !input_->IsKeyPressed(DIK_A);
                int newHoldDir = holdA ? -1 : (holdD ? 1 : 0);

                if (newHoldDir != 0) {
                    if (newHoldDir != cornerHoldingDir_) {
                        cornerHoldingDir_ = newHoldDir;
                        cornerHoldProgress_ = 0.0f; // 押すキーを変えた場合は0からチャージ開始
                    }
                    // 0.78秒の長押しでゲージ100%到達
                    cornerHoldProgress_ = std::clamp(cornerHoldProgress_ + kDeltaTime / 0.78f, 0.0f, 1.0f);

                    // ── 長押ししている間は集中線（プレイヤーが見づらくならないよう弱めのラジアルブラー＋外周の2D集中線）を出す！ ──
                    useRadialBlur_ = true;
                    blurIntensity_ = 0.002f + 0.004f * cornerHoldProgress_;
                    activePostProcess_ = kRadialBlur;
                    if (radialBlurParamData_) {
                        radialBlurParamData_->center = { 0.5f, 0.5f };
                        radialBlurParamData_->blurWidth = -blurIntensity_;
                    }
                } else {
                    cornerHoldingDir_ = 0;
                    cornerHoldProgress_ = (std::max)(0.0f, cornerHoldProgress_ - kDeltaTime * 1.8f);
                    useRadialBlur_ = false;
                    blurIntensity_ = 0.0f;
                }

                // 先にいる雑魚敵を曲がり角の奥（正解ルート側）へ滑らかに進ませて「敵を追う」視覚誘導を行う
                if (currentPhase_ != GamePhase::kBossFight) {
                    for (int eIdx = 0; eIdx < kMaxEnemies; ++eIdx) {
                        if (enemies_[eIdx].isAlive) {
                            enemies_[eIdx].state = Enemy::State::kWander;
                            enemies_[eIdx].stateTimer = 0.0f;
                            float targetRelZ = (corner.cornerS - fighterWorldZ_) + 55.0f + (eIdx % 5) * 10.0f;
                            enemies_[eIdx].relativeZ = std::lerp(enemies_[eIdx].relativeZ, targetRelZ, kDeltaTime * 2.5f);
                        }
                    }
                }

                // 長押しゲージが100%に達したか、制限時間(4.5秒)が経過したらルート確定！
                const float kMaxPromptTime = 4.5f;
                if (cornerHoldProgress_ >= 1.0f || cornerTimer_ >= kMaxPromptTime) {
                    if (cornerHoldProgress_ >= 1.0f) {
                        cornerChosenDir_ = cornerHoldingDir_;
                    } else if (cornerHoldingDir_ == corner.correctDir && cornerHoldProgress_ >= 0.55f) {
                        cornerChosenDir_ = corner.correctDir;
                    } else {
                        cornerChosenDir_ = cornerHoldingDir_; // 未入力(0)または逆方向
                    }

                    cornerTimer_ = 0.0f;
                    if (cornerChosenDir_ == corner.correctDir) {
                        // 正しいルートを選択！高速バンク旋回へ
                        cornerState_ = CornerTurnState::kTurningCorrect;
                        if (audio_) {
                            PlaySE(jumpSE_, false, 1.6f);
                        }
                    } else {
                        // 間違えたルートを選択（または時間切れ）！徐々に暗くしてゲームオーバーへ
                        cornerState_ = CornerTurnState::kTurningWrong;
                        wrongRouteStartPos_ = GetRailPosition(fighterWorldZ_);
                        if (cornerChosenDir_ != 0) {
                            // 逆方向の道へ曲がる
                            wrongRouteMoveDir_ = Scale(corner.rightBefore, static_cast<float>(cornerChosenDir_));
                        } else {
                            // 曲がらずに直進方向へ突っ込む
                            wrongRouteMoveDir_ = corner.dirBefore;
                        }
                        useRadialBlur_ = false;
                        blurIntensity_ = 0.0f;
                    }
                }
            } else if (cornerState_ == CornerTurnState::kTurningCorrect && activeCornerIndex_ >= 0 && activeCornerIndex_ < static_cast<int>(corners_.size())) {
                cornerTimer_ += kDeltaTime;
                const float kTurnDuration = 0.90f;
                float u = std::clamp(cornerTimer_ / kTurnDuration, 0.0f, 1.0f);

                // 旋回中も集中線（弱めのラジアルブラー）を維持して疾走感を演出
                useRadialBlur_ = true;
                blurIntensity_ = 0.006f * (1.0f - u * 0.7f);
                activePostProcess_ = kRadialBlur;
                if (radialBlurParamData_) {
                    radialBlurParamData_->center = { 0.5f, 0.5f };
                    radialBlurParamData_->blurWidth = -blurIntensity_;
                }

                if (cornerTimer_ >= kTurnDuration) {
                    corners_[activeCornerIndex_].completed = true;
                    cornerState_ = CornerTurnState::kNone;
                    activeCornerIndex_ = -1;
                    cornerHoldingDir_ = 0;
                    cornerHoldProgress_ = 0.0f;
                    useRadialBlur_ = false;
                    blurIntensity_ = 0.0f;
                }
            } else if (cornerState_ == CornerTurnState::kTurningWrong && activeCornerIndex_ >= 0 && activeCornerIndex_ < static_cast<int>(corners_.size())) {
                cornerTimer_ += kDeltaTime;
                // 1.5秒かけて画面を徐々に暗くし、1.8秒時点でゲームオーバー画面へ遷移
                wrongRouteDarkenAlpha_ = std::clamp(cornerTimer_ / 1.50f, 0.0f, 1.0f);
                if (cornerTimer_ >= 1.80f) {
                    SceneManager::GetInstance()->ChangeScene("GAMEOVER");
                    return;
                }
            }
        }

        // ── 1. プレイヤーのワールドZ座標を自律前進（ボス登場演出中はその場で静止、曲がり角演出中は専用速度で進行） ──────────────────
        if (!isWaitingForTransition && !isBossIntroCam && !isBossDefeatedSequence_) {
            if (cornerState_ == CornerTurnState::kPromptAndHold && activeCornerIndex_ >= 0) {
                // 角の手前（cornerS - 20m）までゆっくり減速接近しながらA/D長押しを待つ
                float stopS = corners_[activeCornerIndex_].cornerS - 20.0f;
                if (fighterWorldZ_ < stopS) {
                    fighterWorldZ_ = (std::min)(stopS, fighterWorldZ_ + 22.0f * kDeltaTime);
                }
            } else if (cornerState_ == CornerTurnState::kTurningCorrect) {
                // 正解ルート選択後は高速（230m/s）で一気に角を曲がり抜ける
                fighterWorldZ_ += 230.0f * kDeltaTime;
            } else if (cornerState_ == CornerTurnState::kTurningWrong) {
                // 間違えたルート選択時は正解レール(fighterWorldZ_)を進めず、下で別方向へ移動させる
            } else {
                fighterWorldZ_ += boostForwardSpeed_ * kDeltaTime;
            }
        }

        // ── 2. カメラは等速追従をベースにしつつ、ブースト時やフェーズ演出時はG遅延を適用 ──
        float currentDist = 65.0f;
        if (phaseIntroTimer_ >= 0.0f && !isTitleMode_) {
            float pt = phaseIntroTimer_;
            if (pt < 0.42f) {
                // チャージ期: カメラが機体へグッと寄って溜めを作る (65m ➔ 46m)
                float u = pt / 0.42f;
                float ease = u * u * (3.0f - 2.0f * u);
                currentDist = std::lerp(65.0f, 46.0f, ease);
            } else if (pt < 0.72f) {
                // ソニックブーム炸裂: 強烈な加速Gでカメラが後方へ一気に引き離される (46m ➔ 98m)
                float u = (pt - 0.42f) / 0.30f;
                float easeOut = 1.0f - (1.0f - u) * (1.0f - u);
                currentDist = std::lerp(46.0f, 98.0f, easeOut);
            } else {
                // ブースト巡航から定位置(65m)へ滑らかに復帰
                float u = std::clamp((pt - 0.72f) / (kPhaseIntroDuration - 0.72f), 0.0f, 1.0f);
                float ease = u * u * (3.0f - 2.0f * u);
                currentDist = std::lerp(98.0f, 65.0f, ease);
            }
        } else if (isBoosting_) {
            // バレルロール進行度を正弦波（0〜1〜0）にマップしてカメラを後方に引き離す
            float t = barrelRollTimer_ / kBarrelRollDuration;
            float wave = std::sin(t * static_cast<float>(M_PI));

            // ブースト時カメラ引き離しG演出（通常65m ➔ 最大120m引き離す）
            currentDist = 65.0f + wave * 55.0f;
        }

        float camProgress = fighterWorldZ_ - currentDist;
        Vector3 camRailPos = GetRailPosition(camProgress);
        Vector3 camRailDir = GetRailDirection(camProgress);
        Vector3 camRailRight = CalculateRailRight(camRailDir);
        Vector3 camRailUp = CalculateRailUp(camRailDir, camRailRight);

        // ── 3. 自機の横/縦移動（画面内相対）─────────────────────────
        if (fighterModel_) {
            Vector3 inputDir = {0,0,0};
            if ((phaseIntroTimer_ >= 0.0f || isBossIntro || cornerState_ != CornerTurnState::kNone) && !isTitleMode_) {
                // フェーズ演出中・ボス登場演出中・曲がり角A/D選択演出中は自機を画面中央へ滑らかにセンタリング固定
                fighterModel_->transform.translate.x = std::lerp(fighterModel_->transform.translate.x, 0.0f, 0.12f);
                fighterModel_->transform.translate.y = std::lerp(fighterModel_->transform.translate.y, 0.0f, 0.12f);
            } else if (!isWaitingForTransition && !isBossDefeatedSequence_ && !isTitleMode_) {
                if (input_->IsKeyPressed(DIK_W)) inputDir.y += 1.0f;
                if (input_->IsKeyPressed(DIK_S)) inputDir.y -= 1.0f;
                if (input_->IsKeyPressed(DIK_A)) inputDir.x -= 1.0f;
                if (input_->IsKeyPressed(DIK_D)) inputDir.x += 1.0f;
            }

            if (inputDir.x != 0 || inputDir.y != 0) {
                inputDir = Normalize(inputDir);
                float speedFactor = (playerSpeedDebuffTimer_ > 0.0f) ? 0.5f : 1.0f;
                
                // 横移動：傾き（ロール）に応じて移動量を決定し、動き出しをワンテンポ遅らせる
                float rollFactor = 0.0f;
                if (std::abs(playerRotationRoll_) > 0.001f) {
                    rollFactor = -playerRotationRoll_ / kMaxRollAngle;
                }
                rollFactor = std::clamp(rollFactor, -1.0f, 1.0f);
                
                fighterModel_->transform.translate.x += rollFactor * playerSpeedX_ * speedFactor * kDeltaTime;
                fighterModel_->transform.translate.y += inputDir.y * playerSpeedY_ * speedFactor * kDeltaTime;
            }

            fighterModel_->transform.translate.x = std::clamp(fighterModel_->transform.translate.x, -playerLimitX_, playerLimitX_);
            fighterModel_->transform.translate.y = std::clamp(fighterModel_->transform.translate.y, -playerLimitY_, playerLimitY_);

            int currentAngleIndex = 0;
            float localTime = 0.0f;
            bool isCut = false;
            if (isTitleMode_) {
                int numAngles = 4;
                float angleDuration = 6.0f;
                currentAngleIndex = static_cast<int>(randomEffectTime_ / angleDuration) % numAngles;
                localTime = std::fmod(randomEffectTime_, angleDuration);

                static int lastAngleIndex = -1;
                isCut = (currentAngleIndex != lastAngleIndex);
                lastAngleIndex = currentAngleIndex;

                // アングル切り替えの瞬間、プレイヤーのZ座標をリセットしてループさせる（ビルや道路の消失を防ぐ）
                if (isCut && !isTitleTransitioning_) {
                    fighterWorldZ_ = 100.0f;
                }
            }

            // ── プレイヤーのワールド座標（Z はfighterWorldZ_を直接使う。間違えたルート選択時は別方向へ進む）──
            Vector3 playerRailPos = GetRailPosition(fighterWorldZ_);
            Vector3 playerRailDir = GetRailDirection(fighterWorldZ_);
            if (cornerState_ == CornerTurnState::kTurningWrong && activeCornerIndex_ >= 0 && activeCornerIndex_ < static_cast<int>(corners_.size())) {
                const CornerInfo& corner = corners_[activeCornerIndex_];
                float turnBlend = std::clamp(cornerTimer_ / 0.55f, 0.0f, 1.0f);
                float easedTurn = turnBlend * turnBlend * (3.0f - 2.0f * turnBlend);
                playerRailDir = Normalize(Lerp(corner.dirBefore, wrongRouteMoveDir_, easedTurn));
                // 交差点に向かって進みつつ、間違えた方向へ逸れていく軌跡
                float fwdDist = (std::min)(cornerTimer_, 0.55f) * 95.0f + (std::max)(0.0f, cornerTimer_ - 0.55f) * 35.0f;
                float sideDist = (cornerChosenDir_ != 0) ? (easedTurn * cornerTimer_ * 85.0f) : (cornerTimer_ * 75.0f);
                Vector3 sideVec = (cornerChosenDir_ != 0) ? wrongRouteMoveDir_ : corner.dirBefore;
                playerRailPos = Add(wrongRouteStartPos_, Add(Scale(corner.dirBefore, fwdDist * (cornerChosenDir_ != 0 ? (1.0f - 0.5f * easedTurn) : 1.0f)), Scale(sideVec, sideDist)));
            }
            Vector3 playerRailRight = CalculateRailRight(playerRailDir);
            Vector3 playerRailUp = CalculateRailUp(playerRailDir, playerRailRight);

            // 基準高さは路面 -20.0f からプレイヤーは通常 -3.0f (つまり相対 +17.0f)
            float playerOffsetUp = fighterModel_->transform.translate.y + 17.0f;
            Vector3 fighterWorldPos = Add(playerRailPos, Add(Scale(playerRailRight, fighterModel_->transform.translate.x), Scale(playerRailUp, playerOffsetUp)));

            if (isTitleMode_) {
                if (isTitleTransitioning_) {
                    // 1.2秒かけてタイトルアングルから「いつものアングル（プレイヤー真後ろ）」へスムーズに戻す
                    titleTransitionTimer_ += kDeltaTime;
                    float t = std::clamp(titleTransitionTimer_ / 1.2f, 0.0f, 1.0f);

                    // いつものアングル（プレイヤー真後ろ）の目標位置・回転を計算
                    float targetCamX = fighterModel_->transform.translate.x * 0.5f;
                    float targetCamY = fighterModel_->transform.translate.y * 0.5f;
                    float camOffsetUp = targetCamY + 26.0f;
                    Vector3 camBackVector = Scale(playerRailDir, -65.0f);
                    Vector3 targetCamPos = Add(playerRailPos, Add(camBackVector, Add(Scale(playerRailRight, targetCamX), Scale(playerRailUp, camOffsetUp))));

                    Vector3 lookAheadTarget = Add(fighterWorldPos, Scale(playerRailDir, 20.0f));
                    Vector3 cameraTarget = Add(lookAheadTarget, Scale(playerRailUp, 5.0f));
                    Vector3 lookDir = Subtract(cameraTarget, targetCamPos);
                    float lookDist = std::sqrt(lookDir.x * lookDir.x + lookDir.y * lookDir.y + lookDir.z * lookDir.z);
                    if (lookDist > 0.001f) {
                        lookDir = Scale(lookDir, 1.0f / lookDist);
                    } else {
                        lookDir = playerRailDir;
                    }
                    float targetRotY = -std::atan2(lookDir.x, lookDir.z);
                    float targetRotX = std::atan2(-lookDir.y, std::sqrt(lookDir.x * lookDir.x + lookDir.z * lookDir.z));

                    // 開始アングルから目標アングルへと滑らかに補間
                    camTrans.translate = Lerp(transitionStartCamPos_, targetCamPos, t);

                    // 角度補間（最短角度差で補間）
                    float diffY = targetRotY - transitionStartCamRot_.y;
                    while (diffY < -static_cast<float>(M_PI)) diffY += 2.0f * static_cast<float>(M_PI);
                    while (diffY > static_cast<float>(M_PI)) diffY -= 2.0f * static_cast<float>(M_PI);
                    camTrans.rotate.y = transitionStartCamRot_.y + diffY * t;

                    float diffX = targetRotX - transitionStartCamRot_.x;
                    while (diffX < -static_cast<float>(M_PI)) diffX += 2.0f * static_cast<float>(M_PI);
                    while (diffX > static_cast<float>(M_PI)) diffX -= 2.0f * static_cast<float>(M_PI);
                    camTrans.rotate.x = transitionStartCamRot_.x + diffX * t;

                    camTrans.rotate.z = std::lerp(transitionStartCamRot_.z, 0.0f, t);
                } else {
                    // ── タイトル画面専用シネマティックカメラワーク ──
                    Vector3 targetCamPos = { 0,0,0 };
                    Vector3 cameraTarget = { 0,0,0 };

                if (currentAngleIndex == 0) {
                    // アングル1: 右斜め後ろの近い低い位置から見上げる（自機を画面中央〜やや下に大きく映す）
                    targetCamPos = Add(playerRailPos, Add(Scale(playerRailRight, 15.0f), Add(Scale(playerRailDir, -26.0f), Scale(playerRailUp, 9.5f))));
                    cameraTarget = Add(fighterWorldPos, Scale(playerRailUp, 5.2f));
                }
                else if (currentAngleIndex == 1) {
                    // アングル2: 正面斜め前クローズアップ（自機を近くでダイナミックに捉えつつ上部にタイトル空間を確保）
                    targetCamPos = Add(playerRailPos, Add(Scale(playerRailRight, -9.0f), Add(Scale(playerRailDir, 28.0f), Scale(playerRailUp, 19.5f))));
                    cameraTarget = Add(fighterWorldPos, Scale(playerRailUp, 4.5f));
                }
                else if (currentAngleIndex == 2) {
                    // アングル3: 後方斜め上からの近接ダイナミック旋回（遠すぎず自機がはっきり見える距離に調整）
                    float angle = localTime * 0.25f - 0.5f;
                    Vector3 offset = Add(Scale(playerRailDir, -32.0f), Scale(playerRailRight, std::sin(angle) * 20.0f));
                    targetCamPos = Add(playerRailPos, Add(offset, Scale(playerRailUp, 27.0f)));
                    cameraTarget = Add(fighterWorldPos, Add(Scale(playerRailDir, 6.0f), Scale(playerRailUp, 4.2f)));
                }
                else {
                    // アングル4: サイド近接並走カメラ（真横〜斜め横の至近距離で自機を大きく流し撮り）
                    float forwardOffset = (localTime - 3.0f) * 4.2f;
                    targetCamPos = Add(playerRailPos, Add(Scale(playerRailDir, forwardOffset), Add(Scale(playerRailRight, -26.0f), Scale(playerRailUp, 17.5f))));
                    cameraTarget = Add(fighterWorldPos, Scale(playerRailUp, 4.5f));
                }

                if (isCut) {
                    camTrans.translate = targetCamPos;
                } else {
                    // 遷移演出中ならカメラ追従を極端に遅くして、プレイヤーだけを飛び去らせる
                    float posLerpFactor = isTitleTransitioning_ ? 0.005f : 0.2f;
                    camTrans.translate = Lerp(camTrans.translate, targetCamPos, posLerpFactor);
                }

                // カメラの回転（注視点への方向）
                Vector3 lookDir = Subtract(cameraTarget, camTrans.translate);
                float lookDist = std::sqrt(lookDir.x * lookDir.x + lookDir.y * lookDir.y + lookDir.z * lookDir.z);
                if (lookDist > 0.001f) {
                    lookDir = Scale(lookDir, 1.0f / lookDist);
                } else {
                    lookDir = playerRailDir;
                }

                float baseCamRotY = -std::atan2(lookDir.x, lookDir.z);
                float baseCamRotX = std::atan2(-lookDir.y, std::sqrt(lookDir.x * lookDir.x + lookDir.z * lookDir.z));

                if (isCut) {
                    camTrans.rotate.y = baseCamRotY;
                    camTrans.rotate.x = baseCamRotX;
                    camTrans.rotate.z = 0.0f;
                } else {
                    float rotLerpFactor = isTitleTransitioning_ ? 0.01f : 0.15f;
                    float diffY = baseCamRotY - camTrans.rotate.y;
                    while (diffY < -static_cast<float>(M_PI)) diffY += 2.0f * static_cast<float>(M_PI);
                    while (diffY > static_cast<float>(M_PI)) diffY -= 2.0f * static_cast<float>(M_PI);
                    camTrans.rotate.y += diffY * rotLerpFactor;

                    float diffX = baseCamRotX - camTrans.rotate.x;
                    while (diffX < -static_cast<float>(M_PI)) diffX += 2.0f * static_cast<float>(M_PI);
                    while (diffX > static_cast<float>(M_PI)) diffX -= 2.0f * static_cast<float>(M_PI);
                    camTrans.rotate.x += diffX * rotLerpFactor;

                    camTrans.rotate.z = std::lerp(camTrans.rotate.z, 0.0f, rotLerpFactor);
                }
            }
        }
        else if (!isBossDefeatedSequence_) {
                // スターフォックス風のカメラX/Y追従
                float cameraLag = 0.08f;
                float targetCamX = fighterModel_->transform.translate.x * 0.5f;
                float targetCamY = fighterModel_->transform.translate.y * 0.5f;
                camRelativeX_ = std::lerp(camRelativeX_, targetCamX, cameraLag);
                camRelativeY_ = std::lerp(camRelativeY_, targetCamY, cameraLag);

                // カメラの目標位置を計算（プレイヤーモデルの真後ろから追従）
                // 基準高さは路面 -20.0f から通常 +6.0f (つまり相対 +26.0f)
                float camOffsetUp = camRelativeY_ + 26.0f;
                Vector3 camBackVector = Scale(playerRailDir, -currentDist);
                Vector3 targetCamPos = Add(playerRailPos, Add(camBackVector, Add(Scale(playerRailRight, camRelativeX_), Scale(playerRailUp, camOffsetUp))));

                // カメラ位置のスムーズ追従 (Smooth Follow、遷移待機中は即座に初期定位置へ固定)
                float posLerpFactor = isWaitingForTransition ? 1.0f : 0.12f;
                Vector3 camDiff = Subtract(targetCamPos, camTrans.translate);
                float camDistSq = camDiff.x * camDiff.x + camDiff.y * camDiff.y + camDiff.z * camDiff.z;
                if (camDistSq > 10000.0f) { // 100m以上離れている場合は即座にワープ
                    camTrans.translate = targetCamPos;
                } else {
                    camTrans.translate = Lerp(camTrans.translate, targetCamPos, posLerpFactor);
                }

                // ── カメラの注視ターゲット（LookAhead & LookAt）の計算 ──
                // プレイヤーの少し先（20.0m 先）を注視点とする
                float lookAheadDist = 20.0f;
                Vector3 lookAheadTarget = Add(fighterWorldPos, Scale(playerRailDir, lookAheadDist));
                // プレイヤーの少し上（5.0m 上）を狙う
                Vector3 cameraTarget = Add(lookAheadTarget, Scale(playerRailUp, 5.0f));

                // カメラの現在位置から注視ターゲットへの方向ベクトル
                Vector3 lookDir = Subtract(cameraTarget, camTrans.translate);
                float lookDist = std::sqrt(lookDir.x * lookDir.x + lookDir.y * lookDir.y + lookDir.z * lookDir.z);
                if (lookDist > 0.001f) {
                    lookDir = Scale(lookDir, 1.0f / lookDist);
                } else {
                    lookDir = camRailDir; // フォールバック
                }

                // 方向ベクトルからオイラー角を計算
                float baseCamRotY = -std::atan2(lookDir.x, lookDir.z);
                float baseCamRotX = std::atan2(-lookDir.y, std::sqrt(lookDir.x * lookDir.x + lookDir.z * lookDir.z));

                float targetCamRotateY = fighterModel_->transform.translate.x * 0.003f;
                float targetCamRotateX = -fighterModel_->transform.translate.y * 0.003f;

                float targetRotY = baseCamRotY + targetCamRotateY;
                float targetRotX = baseCamRotX + targetCamRotateX;

                // カメラの回転角のスムーズ首振り（最短角度差による補間）
                float rotLerpFactor = isWaitingForTransition ? 1.0f : 0.15f;

                float diffY = targetRotY - camTrans.rotate.y;
                while (diffY < -static_cast<float>(M_PI)) diffY += 2.0f * static_cast<float>(M_PI);
                while (diffY > static_cast<float>(M_PI)) diffY -= 2.0f * static_cast<float>(M_PI);
                camTrans.rotate.y += diffY * rotLerpFactor;

                float diffX = targetRotX - camTrans.rotate.x;
                while (diffX < -static_cast<float>(M_PI)) diffX += 2.0f * static_cast<float>(M_PI);
                while (diffX > static_cast<float>(M_PI)) diffX -= 2.0f * static_cast<float>(M_PI);
                camTrans.rotate.x += diffX * rotLerpFactor;

                // ロール回転は入力とバレルロールに追従
                float targetRotZ = -inputDir.x * 0.03f;
                if (isBarrelRolling_) {
                    float rollT = barrelRollTimer_ / kBarrelRollDuration;
                    targetRotZ += std::sin(rollT * static_cast<float>(M_PI)) * 0.15f;
                }
                camTrans.rotate.z = std::lerp(camTrans.rotate.z, targetRotZ, rotLerpFactor);
            }

            // ── 4. バレルロール開始（LSHIFTのブースト切り替え時に連動） ──

            // ── 5. 機体の傾き（通常ロール/ピッチ + バレルロール合成 + 曲がり角A/D長押しバンク） ──
            float baseRoll  = inputDir.x * -kMaxRollAngle;
            float basePitch = inputDir.y * 0.4f;
            if (isBossIntro) { baseRoll = 0.0f; basePitch = 0.0f; }
            if (cornerState_ == CornerTurnState::kPromptAndHold && cornerHoldingDir_ != 0) {
                // A/D長押し中は選んだ方向へ機体をグッとバンク（傾け）させ、周囲に気流と火花を発生させる
                baseRoll = -static_cast<float>(cornerHoldingDir_) * (0.35f + 0.65f * cornerHoldProgress_);
                basePitch = -0.08f * cornerHoldProgress_;
                if (particleManager_ && static_cast<int>(cornerTimer_ * 60.0f) % 4 == 0) {
                    Vector3 windCol = (cornerHoldingDir_ == corners_[activeCornerIndex_].correctDir)
                        ? Vector3{ 0.20f, 0.90f, 1.0f } : Vector3{ 1.0f, 0.35f, 0.20f };
                    particleManager_->EmitAeroWind(fighterWorldPos, 95.0f, 1, windCol);
                }
            } else if (cornerState_ == CornerTurnState::kTurningCorrect && activeCornerIndex_ >= 0) {
                float u = std::clamp(cornerTimer_ / 0.90f, 0.0f, 1.0f);
                baseRoll = -static_cast<float>(corners_[activeCornerIndex_].correctDir) * 1.15f * std::sin(u * static_cast<float>(M_PI));
                basePitch = -0.12f * std::sin(u * static_cast<float>(M_PI));
                if (particleManager_ && static_cast<int>(cornerTimer_ * 60.0f) % 3 == 0) {
                    particleManager_->EmitAeroWind(fighterWorldPos, 110.0f, 1, { 0.25f, 0.92f, 1.0f });
                }
            } else if (cornerState_ == CornerTurnState::kTurningWrong) {
                baseRoll = -static_cast<float>(cornerChosenDir_) * 0.85f;
            }
            if (phaseIntroTimer_ >= 0.0f && !isTitleMode_) {
                // フェーズ演出中のソニックブーム加速に伴う軽やかな上昇ピッチ演出
                float pt = phaseIntroTimer_;
                if (pt >= 0.35f && pt < 1.25f) {
                    float u = (pt - 0.35f) / 0.90f;
                    basePitch = -0.14f * std::sin(u * static_cast<float>(M_PI));
                }
            }
            playerRotationRoll_  = std::lerp(playerRotationRoll_,  baseRoll,  0.1f);
            playerRotationPitch_ = std::lerp(playerRotationPitch_, basePitch, 0.1f);

            float rollAngle = playerRotationRoll_;

            // フェーズ演出中の高速バレルロール＆退場スナップロール演出
            if (phaseIntroTimer_ >= 0.0f && !isTitleMode_) {
                float pt = phaseIntroTimer_;
                if (pt >= 0.16f && pt < 1.16f) {
                    // ソニックブーム突入バレルロール（PHASE 1は1回転、PHASE 2は勢いよく2回転）
                    float u = (pt - 0.16f) / 1.00f;
                    float easedU = 0.5f - 0.5f * std::cos(static_cast<float>(M_PI) * u);
                    float spins = (phaseIntroNum_ == 1) ? 1.0f : 2.0f;
                    rollAngle += easedU * spins * 2.0f * static_cast<float>(M_PI);
                } else if (pt >= 1.75f && pt < 2.30f) {
                    // 退場時のキレのあるエルロンロール
                    float u = (pt - 1.75f) / 0.55f;
                    float easedU = 0.5f - 0.5f * std::cos(static_cast<float>(M_PI) * u);
                    rollAngle -= easedU * 2.0f * static_cast<float>(M_PI);
                }
            } else if (isBarrelRolling_) {
                barrelRollTimer_ += kDeltaTime;
                float t = barrelRollTimer_ / kBarrelRollDuration; // 0.0 → 1.0
                if (t >= 1.0f) {
                    t = 1.0f;
                    isBarrelRolling_ = false;
                    barrelRollTimer_ = 0.0f;
                    isBoosting_ = false; // バレルロール終了と同時にブーストも自動解除
                }
                // easeInOut: 0.5 - 0.5 * cos(π*t) で0から1へスムーズに変化
                float easedT = 0.5f - 0.5f * std::cos(static_cast<float>(M_PI) * t);
                rollAngle += easedT * 2.0f * static_cast<float>(M_PI); // ← ベース回転に上乗せ（360度一回転）
            }

            // タイトル遷移後半（ブースト開始後）は連続スピン（クルクル回転）
            if (isTitleMode_ && isTitleTransitioning_ && titleTransitionTimer_ >= 1.2f) {
                float spinTime = titleTransitionTimer_ - 1.2f; // 後半開始からの経過時間
                rollAngle = spinTime * 4.0f * static_cast<float>(M_PI); // 1秒で2回転
            }

            fighterModel_->transform.rotate.z = rollAngle;

            // 機体自体の向きをレールの進行方向に滑らかに合わせる
            float basePlayerRotY = std::atan2(playerRailDir.x, playerRailDir.z);
            float basePlayerRotX = std::atan2(-playerRailDir.y, std::sqrt(playerRailDir.x * playerRailDir.x + playerRailDir.z * playerRailDir.z));

            // 進行方向に向かせるための回転角（符号を反転して180度オフセット）
            float targetPlayerRotY = -basePlayerRotY + static_cast<float>(M_PI);
            float diffPlayerY = targetPlayerRotY - fighterModel_->transform.rotate.y;
            while (diffPlayerY < -static_cast<float>(M_PI)) diffPlayerY += 2.0f * static_cast<float>(M_PI);
            while (diffPlayerY > static_cast<float>(M_PI)) diffPlayerY -= 2.0f * static_cast<float>(M_PI);
            fighterModel_->transform.rotate.y += diffPlayerY * 0.15f;

            // ピッチはレールの傾きを除いた入力分のみ
            float targetPlayerRotX = playerRotationPitch_;
            float diffPlayerX = targetPlayerRotX - fighterModel_->transform.rotate.x;
            while (diffPlayerX < -static_cast<float>(M_PI)) diffPlayerX += 2.0f * static_cast<float>(M_PI);
            while (diffPlayerX > static_cast<float>(M_PI)) diffPlayerX -= 2.0f * static_cast<float>(M_PI);
            fighterModel_->transform.rotate.x += diffPlayerX * 0.15f;

            // レールに沿った回転行列を構築 (r = playerRailRight, u = playerRailUp, d = playerRailDir)
            Matrix4x4 R_rail = MakeIdentity4x4();
            R_rail.m[0][0] = playerRailRight.x; R_rail.m[0][1] = playerRailRight.y; R_rail.m[0][2] = playerRailRight.z;
            R_rail.m[1][0] = playerRailUp.x;    R_rail.m[1][1] = playerRailUp.y;    R_rail.m[1][2] = playerRailUp.z;
            R_rail.m[2][0] = playerRailDir.x;   R_rail.m[2][1] = playerRailDir.y;   R_rail.m[2][2] = playerRailDir.z;

            // モデルのローカル回転 (Z:ロール -> X:ピッチ -> Y:180度反転)
            Matrix4x4 rotateY = MakeRotateYMatrix(static_cast<float>(M_PI));
            Matrix4x4 rotateX = MakeRotateXMatrix(playerRotationPitch_);
            Matrix4x4 rotateZ = MakeRotateZMatrix(rollAngle);
            Matrix4x4 R_local = Multiply(Multiply(rotateZ, rotateX), rotateY);

            // 最終姿勢行列の算出とワールド行列の組み立て
            Matrix4x4 R_final = Multiply(R_local, R_rail);
            Matrix4x4 scaleMatrix = MakeScaleMatrix(fighterModel_->transform.scale);
            Matrix4x4 translateMatrix = MakeTranslateMatrix(fighterWorldPos);
            fighterTransformData_->World = Multiply(Multiply(scaleMatrix, R_final), translateMatrix);


            // ── ゲーム開始演出（ボスの登場演出のみ）中のカメラワーク更新 ──
            earthAlpha_ = 0.0f;
            japanAlpha_ = 0.0f;
            tokyoAlpha_ = 0.0f;

            if (!isBossDefeatedSequence_ && bossAppearanceTimer_ > 0.0f) {
                float t_total = kBossIntroTotalDuration - bossAppearanceTimer_; // 6.4s ～ 13.6s (ボスの登場演出のみ)
                Vector3 bossPos = GetBossPosition(0.0f, 0.0f);

                // 1. 地球・日本・東京の夜景 3Dモデルは非表示のまま維持 (0.0f)

                // 2. カメラ位置・回転のタイムライン制御
                if (t_total < 6.4f) {
                    // ── フェーズA (0.0s ～ 6.4s): 真上からの連続ズームイン（地球 -> 日本 -> 東京の夜景 -> ステージを上から見下ろして徐々にボスへ寄る）──
                    camera_->ClearTarget();

                    float camY = 3000.0f;
                    if (t_total < 1.5f) {
                        // 地球へ寄る (3000m -> 2100m)
                        float u = t_total / 1.5f;
                        float ease = u * u * (3.0f - 2.0f * u);
                        camY = std::lerp(3000.0f, 2100.0f, ease);
                    } else if (t_total < 3.2f) {
                        // 日本へ寄る (2100m -> 1300m)
                        float u = (t_total - 1.5f) / 1.7f;
                        float ease = u * u * (3.0f - 2.0f * u);
                        camY = std::lerp(2100.0f, 1300.0f, ease);
                    } else if (t_total < 4.7f) {
                        // 東京の夜景へ寄る (1300m -> 720m)
                        float u = (t_total - 3.2f) / 1.5f;
                        float ease = u * u * (3.0f - 2.0f * u);
                        camY = std::lerp(1300.0f, 720.0f, ease);
                    } else {
                        // 現在のステージを真上から見下ろし、徐々にボスに向かって寄っていく (720m -> 220m)
                        float u = (t_total - 4.7f) / 1.7f;
                        float ease = u * u * (3.0f - 2.0f * u);
                        camY = std::lerp(720.0f, 220.0f, ease);
                    }

                    camTrans.translate = { bossPos.x, camY, bossPos.z };
                    camTrans.rotate = { static_cast<float>(M_PI) * 0.5f, 0.0f, 0.0f }; // 真下(-Y)を真っ直ぐ見下ろす
                }
                else if (t_total < 7.8f) {
                    // ── フェーズB (6.4s ～ 7.8s): ステージ俯瞰から引き続きボスへ寄りつつ、ボスの正面アングルへ滑らかに回り込む ──
                    camera_->ClearTarget();

                    float u = (t_total - 6.4f) / 1.4f;
                    float ease = u * u * (3.0f - 2.0f * u);

                    float bossProgress = fighterWorldZ_ + bossZOffset_;
                    Vector3 bRailDir = GetRailDirection(bossProgress);
                    Vector3 bRailRight = CalculateRailRight(bRailDir);
                    Vector3 bRailUp = CalculateRailUp(bRailDir, bRailRight);

                    Vector3 startPos = { bossPos.x, 220.0f, bossPos.z };
                    Vector3 endPos = Add(bossPos, Add(Scale(bRailDir, -115.0f), Scale(bRailUp, 12.0f)));

                    // 降下中は少し手前に弧を描きながらボスの正面へ滑り込む
                    Vector3 curPos = {
                        std::lerp(startPos.x, endPos.x, ease),
                        std::lerp(startPos.y, endPos.y, std::pow(ease, 0.85f)),
                        std::lerp(startPos.z, endPos.z, ease)
                    };
                    camTrans.translate = curPos;

                    // 視線を真下(+PI/2)からボスの中心へと滑らかにピッチアップ
                    Vector3 lookTarget = Add(bossPos, Scale(bRailUp, 6.0f));
                    Vector3 dir = Subtract(lookTarget, curPos);
                    float horizDist = std::sqrt(dir.x * dir.x + dir.z * dir.z);
                    float targetRotY = (horizDist > 0.1f) ? -std::atan2(dir.x, dir.z) : 0.0f;
                    float targetRotX = std::atan2(-dir.y, (std::max)(0.001f, horizDist));

                    float pitchBlend = ease * ease;
                    camTrans.rotate.x = std::lerp(static_cast<float>(M_PI) * 0.5f, targetRotX, pitchBlend);
                    camTrans.rotate.y = targetRotY * pitchBlend;
                    camTrans.rotate.z = 0.0f;
                }
                else if (t_total < 11.2f) {
                    // ── フェーズC (7.8s ～ 11.2s): ボスの登場演出（7.8s～9.8s: 360度ダイナミック公転カメラ ＆ 9.8s～11.2s: 迫力の咆哮カメラ）──
                    camera_->SetTarget(&bossPos);

                    float bossProgress = fighterWorldZ_ + bossZOffset_;
                    Vector3 bossRailDir = GetRailDirection(bossProgress);
                    Vector3 bossRailRight = Normalize(Vector3{ bossRailDir.z, 0.0f, -bossRailDir.x });
                    Vector3 bossRailUp = Cross(bossRailDir, bossRailRight);

                    if (t_total < 9.8f) {
                        // 7.8s ～ 9.8s: ボスの周囲を360度ぐるりと回り込む公転カメラ
                        float u = (t_total - 7.8f) / 2.0f;
                        float easedT = u * u * (3.0f - 2.0f * u);
                        float yawAngle = (1.0f - easedT) * 2.0f * static_cast<float>(M_PI);
                        float dist = std::lerp(115.0f, 135.0f, easedT);
                        float height = std::lerp(12.0f, 28.0f, easedT);

                        camTrans.translate = Add(bossPos, Add(
                            Scale(bossRailRight, -std::sin(yawAngle) * dist),
                            Add(Scale(bossRailUp, height), Scale(bossRailDir, -std::cos(yawAngle) * dist))
                        ));
                    } else {
                        // 9.8s ～ 11.2s: 咆哮（Roar）を正面〜ややローアングルから捉えるダイナミックカメラ
                        float u = (t_total - 9.8f) / 1.4f;
                        float ease = u * u * (3.0f - 2.0f * u);
                        float dist = std::lerp(135.0f, 148.0f, ease);
                        float height = std::lerp(28.0f, 38.0f, ease);
                        camTrans.translate = Add(bossPos, Add(Scale(bossRailDir, -dist), Scale(bossRailUp, height)));
                    }
                }
                else {
                    // ── フェーズD (11.2s ～ 13.6s):
                    //    ・ゲーム開始時(isOpeningIntro_ == true): ビルを貫通しないよう、一度ビル群の上空(Y=340m)へ上がってから上空を移動し、プレイヤーの背後へ降りる
                    //    ・ボス戦開始咆哮後(isOpeningIntro_ == false): ボスは目の前(169m先)の同じ道路上にいるため、そのまま道路に沿ってプレイヤー背後へ滑らかに戻る ──
                    camera_->ClearTarget();

                    float durationD = kBossIntroTotalDuration - 11.2f; // 2.4s
                    float u = std::clamp((t_total - 11.2f) / durationD, 0.0f, 1.0f);
                    auto smoothstep = [](float x) {
                        x = std::clamp(x, 0.0f, 1.0f);
                        return x * x * (3.0f - 2.0f * x);
                    };

                    float bossProgress = fighterWorldZ_ + bossZOffset_;
                    Vector3 bRailDir = GetRailDirection(bossProgress);
                    Vector3 bRailRight = CalculateRailRight(bRailDir);
                    Vector3 bRailUp = CalculateRailUp(bRailDir, bRailRight);

                    Vector3 playerRailPos = GetRailPosition(fighterWorldZ_);
                    Vector3 playerRailDir = GetRailDirection(fighterWorldZ_);
                    Vector3 playerRailRight = CalculateRailRight(playerRailDir);
                    Vector3 playerRailUp = CalculateRailUp(playerRailDir, playerRailRight);

                    float targetCamX = fighterModel_ ? fighterModel_->transform.translate.x * 0.5f : 0.0f;
                    float targetY = fighterModel_ ? fighterModel_->transform.translate.y * 0.5f : 0.0f;

                    Vector3 startPos = Add(bossPos, Add(Scale(bRailDir, -148.0f), Scale(bRailUp, 38.0f)));
                    Vector3 targetPos = Add(playerRailPos, Add(Scale(playerRailRight, targetCamX), Add(Scale(playerRailUp, targetY + 26.0f), Scale(playerRailDir, -65.0f))));

                    // ビル最高高度(約140m)を余裕で越える上空高度(340m)
                    const float highAltitudeY = 340.0f;

                    // 水平移動(X, Z):
                    // ・u = 0.00 ～ 0.26 (上昇中) はボス前の道路上からほぼ動かず真上に上がる
                    // ・u = 0.26 ～ 0.74 (上空340m飛行中) にボス上空からプレイヤー上空へ一気に移動する
                    // ・u = 0.74 ～ 1.00 (降下中) はプレイヤー背後の道路上で真下に降りる
                    float horizT = 0.0f;
                    if (u < 0.26f) {
                        horizT = 0.03f * smoothstep(u / 0.26f);
                    } else if (u < 0.74f) {
                        horizT = std::lerp(0.03f, 0.97f, smoothstep((u - 0.26f) / 0.48f));
                    } else {
                        horizT = std::lerp(0.97f, 1.00f, smoothstep((u - 0.74f) / 0.26f));
                    }

                    // 垂直移動(Y):
                    // ・u = 0.00 ～ 0.28 で startPos.y から上空 340m へ上昇
                    // ・u = 0.28 ～ 0.72 は上空 340m～365m のアーチを維持してビルの上を移動
                    // ・u = 0.72 ～ 1.00 で上空 340m からプレイヤー背後の targetPos.y へ降下
                    float camY = startPos.y;
                    if (u < 0.28f) {
                        float upT = smoothstep(u / 0.28f);
                        camY = std::lerp(startPos.y, highAltitudeY, upT);
                    } else if (u < 0.72f) {
                        float midU = (u - 0.28f) / 0.44f;
                        camY = highAltitudeY + 25.0f * std::sin(midU * static_cast<float>(M_PI));
                    } else {
                        float downT = smoothstep((u - 0.72f) / 0.28f);
                        camY = std::lerp(highAltitudeY, targetPos.y, downT);
                    }

                    if (!isOpeningIntro_) {
                        float directT = smoothstep(u);
                        horizT = directT;
                        camY = std::lerp(startPos.y, targetPos.y, directT);
                    }

                    Vector3 camPos = {
                        std::lerp(startPos.x, targetPos.x, horizT),
                        camY,
                        std::lerp(startPos.z, targetPos.z, horizT)
                    };
                    camTrans.translate = camPos;

                    // 開始時のカメラ角度（ボスを注視していた向き）
                    Vector3 startDir = Normalize(Subtract(bossPos, startPos));
                    float startRotY = -std::atan2(startDir.x, startDir.z);
                    float startRotX = std::atan2(-startDir.y, std::sqrt(startDir.x * startDir.x + startDir.z * startDir.z));

                    // 終了時のカメラ角度（通常プレイ時のプレイヤー背後カメラの向きと完全一致させる）
                    Vector3 lookAheadTarget = Add(fighterWorldPos, Scale(playerRailDir, 20.0f));
                    Vector3 playerCamTarget = Add(lookAheadTarget, Scale(playerRailUp, 5.0f));
                    Vector3 endLookDir = Normalize(Subtract(playerCamTarget, targetPos));
                    float baseCamRotY = -std::atan2(endLookDir.x, endLookDir.z);
                    float baseCamRotX = std::atan2(-endLookDir.y, std::sqrt(endLookDir.x * endLookDir.x + endLookDir.z * endLookDir.z));

                    float targetCamRotateY = baseCamRotY + (fighterModel_ ? fighterModel_->transform.translate.x * 0.003f : 0.0f);
                    float targetCamRotateX = baseCamRotX + (fighterModel_ ? -fighterModel_->transform.translate.y * 0.003f : 0.0f);

                    // ヨー回転(Y): 上空移動中に滑らかにプレイヤー進行方向へ回頭
                    float yawT = smoothstep((u - 0.12f) / 0.72f);
                    float diffRotY = targetCamRotateY - startRotY;
                    while (diffRotY < -static_cast<float>(M_PI)) diffRotY += 2.0f * static_cast<float>(M_PI);
                    while (diffRotY > static_cast<float>(M_PI)) diffRotY -= 2.0f * static_cast<float>(M_PI);
                    camTrans.rotate.y = startRotY + diffRotY * yawT;

                    // ピッチ回転(X): ゲーム開始時は上昇とともに下向き(ステージ見下ろし角 1.18rad)になり、ボス戦開始咆哮時は正面のまま滑らかに補間
                    const float overheadPitch = 1.18f;
                    if (!isOpeningIntro_) {
                        camTrans.rotate.x = std::lerp(startRotX, targetCamRotateX, smoothstep(u));
                    } else if (u < 0.28f) {
                        float pitchDownT = smoothstep(u / 0.28f);
                        camTrans.rotate.x = std::lerp(startRotX, overheadPitch, pitchDownT);
                    } else if (u < 0.68f) {
                        camTrans.rotate.x = overheadPitch;
                    } else {
                        float pitchUpT = smoothstep((u - 0.68f) / 0.32f);
                        camTrans.rotate.x = std::lerp(overheadPitch, targetCamRotateX, pitchUpT);
                    }
                    camTrans.rotate.z = 0.0f;
                }

                // 3. 地球・日本・東京の夜景 3Dモデルのトランスフォーム・マテリアル更新
                // ※寄りすぎて荒く見えないよう、ズーム倍率を軽め（1.00倍 -> 1.18〜1.22倍）に抑えて高精細に見せる
                const float fovHalfTan = std::tan(0.45f * 0.5f); // tan(0.225) ≈ 0.22887f
                if (earthModel_ && earthTransformData_ && earthAlpha_ > 0.001f) {
                    float u = std::clamp(t_total / 2.2f, 0.0f, 1.0f);
                    float ease = u * u * (3.0f - 2.0f * u);
                    float planeY = 480.0f;
                    float distToPlane = (std::max)(10.0f, camTrans.translate.y - planeY);
                    // カメラ下降に合わせて3D地球モデルへ軽めにズームイン
                    float zoomFactor = std::lerp(1.00f, 1.20f, ease);
                    float scaleVal = distToPlane * fovHalfTan * zoomFactor;
                    float scaleY = scaleVal * 0.16f;
                    float shiftX = std::lerp(0.0f, 0.02f, ease) * scaleVal;
                    float shiftZ = std::lerp(0.0f, 0.02f, ease) * scaleVal;
                    Matrix4x4 worldMat = MakeAffineMatrix(Vector3{ scaleVal, scaleY, scaleVal }, Vector3{ 0.0f, 0.0f, 0.0f }, { camTrans.translate.x + shiftX, planeY, camTrans.translate.z + shiftZ });
                    earthTransformData_->World = worldMat;
                    earthTransformData_->color = { 1.0f, 1.0f, 1.0f, 1.0f };
                    earthModel_->SetColor({ 1.0f, 1.0f, 1.0f, earthAlpha_ });
                }
                if (japanModel_ && japanTransformData_ && japanAlpha_ > 0.001f) {
                    float u = std::clamp((t_total - 1.5f) / 2.4f, 0.0f, 1.0f);
                    float ease = u * u * (3.0f - 2.0f * u);
                    float planeY = 470.0f;
                    float distToPlane = (std::max)(10.0f, camTrans.translate.y - planeY);
                    // 3D日本列島モデルから関東・東京方面へ軽めにズームイン
                    float zoomFactor = std::lerp(1.00f, 1.22f, ease);
                    float scaleVal = distToPlane * fovHalfTan * zoomFactor;
                    float scaleY = scaleVal * 0.16f;
                    float shiftX = std::lerp(0.0f, -0.06f, ease) * scaleVal;
                    float shiftZ = std::lerp(0.0f, +0.06f, ease) * scaleVal;
                    Matrix4x4 worldMat = MakeAffineMatrix(Vector3{ scaleVal, scaleY, scaleVal }, Vector3{ 0.0f, 0.0f, 0.0f }, { camTrans.translate.x + shiftX, planeY, camTrans.translate.z + shiftZ });
                    japanTransformData_->World = worldMat;
                    japanTransformData_->color = { 1.0f, 1.0f, 1.0f, 1.0f };
                    japanModel_->SetColor({ 1.0f, 1.0f, 1.0f, japanAlpha_ });
                }
                if (tokyoModel_ && tokyoTransformData_ && tokyoAlpha_ > 0.001f) {
                    float u = std::clamp((t_total - 3.2f) / 2.4f, 0.0f, 1.0f);
                    float ease = u * u * (3.0f - 2.0f * u);
                    float planeY = 460.0f;
                    float distToPlane = (std::max)(10.0f, camTrans.translate.y - planeY);
                    // 3D東京夜景モデルへ軽めに寄りながら、真下のステージへシームレスに繋がる
                    float zoomFactor = std::lerp(1.00f, 1.18f, ease);
                    float scaleVal = distToPlane * fovHalfTan * zoomFactor;
                    float scaleY = scaleVal * 0.16f;
                    float shiftX = std::lerp(0.0f, 0.01f, ease) * scaleVal;
                    float shiftZ = std::lerp(0.0f, -0.01f, ease) * scaleVal;
                    Matrix4x4 worldMat = MakeAffineMatrix(Vector3{ scaleVal, scaleY, scaleVal }, Vector3{ 0.0f, 0.0f, 0.0f }, { camTrans.translate.x + shiftX, planeY, camTrans.translate.z + shiftZ });
                    tokyoTransformData_->World = worldMat;
                    tokyoTransformData_->color = { 1.0f, 1.0f, 1.0f, 1.0f };
                    tokyoModel_->SetColor({ 1.0f, 1.0f, 1.0f, tokyoAlpha_ });
                }
            }

            // ── 背景テクスチャの切り替え（Tキー）──
            if (input_->IsKeyTriggered(DIK_T) && !isTitleMode_) {
                activeBackgroundTex_ = (activeBackgroundTex_ + 1) % kNumSkyTextures;
            }



            // ── Blender同期: game_state.jsonの出力（3フレームに1回）──
            if (++blenderSyncCounter_ % 3 == 0) {
                std::ostringstream oss;
                oss << "{\n";
                // フェーズ情報
                oss << "  \"phase\": " << static_cast<int>(currentPhase_) << ",\n";
                // プレイヤー情報
                oss << "  \"player\": {";
                oss << "\"x\": " << fighterWorldPos.x << ", ";
                oss << "\"y\": " << fighterWorldPos.y << ", ";
                oss << "\"z\": " << fighterWorldPos.z << ", ";
                oss << "\"roll\": " << (fighterModel_ ? fighterModel_->transform.rotate.z : 0.0f) << ", ";
                oss << "\"pitch\": " << (fighterModel_ ? fighterModel_->transform.rotate.x : 0.0f) << ", ";
                oss << "\"hp\": " << playerHP_ << ", ";
                oss << "\"maxhp\": " << playerMaxHP_ << ", ";
                oss << "\"boosting\": " << (isBoosting_ ? "true" : "false");
                oss << "},\n";
                // 敵情報 (10体分)
                oss << "  \"enemies\": [\n";
                for (int i = 0; i < kMaxEnemies; ++i) {
                    oss << "    {";
                    oss << "\"alive\": " << (enemies_[i].isAlive ? "true" : "false") << ", ";
                    oss << "\"x\": " << enemies_[i].position.x << ", ";
                    oss << "\"y\": " << enemies_[i].position.y << ", ";
                    oss << "\"z\": " << enemies_[i].position.z << ", ";
                    oss << "\"rx\": " << enemies_[i].rotate.x << ", ";
                    oss << "\"ry\": " << enemies_[i].rotate.y << ", ";
                    oss << "\"rz\": " << enemies_[i].rotate.z << ", ";
                    oss << "\"hp\": " << enemies_[i].hp << ", ";
                    oss << "\"state\": " << static_cast<int>(enemies_[i].state);
                    oss << "}";
                    if (i < kMaxEnemies - 1) oss << ",";
                    oss << "\n";
                }
                oss << "  ],\n";
                // ボス情報
                float bodyBounceForJson = 0.0f;
                if (bossLegSwingSpeed_ > 0.0f) {
                    bodyBounceForJson = std::sin(bossTime_ * 2.0f) * bossBodyBounceRange_;
                }
                float dropOffsetForJson = 0.0f;
                oss << "  \"boss\": {";
                oss << "\"active\": " << (currentPhase_ == GamePhase::kBossFight ? "true" : "false") << ", ";
                oss << "\"visible\": " << (isBossModelVisible_ ? "true" : "false") << ", ";
                oss << "\"x\": 0.0, ";
                oss << "\"y\": " << (bossYOffset_ + bodyBounceForJson + dropOffsetForJson) << ", ";
                oss << "\"z\": " << (fighterWorldZ_ + bossZOffset_) << ", ";
                oss << "\"rot_y\": " << bossBodyRotY_ << ", ";
                oss << "\"hp\": " << bossHP_ << ", ";
                oss << "\"maxhp\": " << bossMaxHP_ << ", ";
                oss << "\"action\": " << static_cast<int>(bossActionState_);
                oss << "},\n";
                // 弾情報 (全60発・固定インデックス・aliveフラグ付き)
                // ※インデックスを固定することでBlender側が同じ弾を追跡できる
                static const int kMaxDisplayBullets = 20; // 先頭20発のみ出力(JSON軽量化)
                oss << "  \"bullets\": [\n";
                for (int i = 0; i < kMaxDisplayBullets; ++i) {
                    bool alive = (playerBullets_[i].currentTime < playerBullets_[i].lifeTime);
                    oss << "    {\"alive\": " << (alive ? "true" : "false") << ", ";
                    oss << "\"x\": " << playerBullets_[i].position.x << ", ";
                    oss << "\"y\": " << playerBullets_[i].position.y << ", ";
                    oss << "\"z\": " << playerBullets_[i].position.z << "}";
                    if (i < kMaxDisplayBullets - 1) oss << ",";
                    oss << "\n";
                }
                oss << "  ],\n";
                // 追加情報
                oss << "  \"recording\": " << (replayRecording_ ? "true" : "false") << "\n";
                oss << "}\n";

                std::ofstream posFile("Resources/game_state.json", std::ios::trunc);
                if (posFile.is_open()) {
                    posFile << oss.str();
                    posFile.close();
                }
            }

            // ── リプレイ録画 (RキーでON/OFF, 録画中は毎フレームCSVに追記) ──
            if (input_->IsKeyTriggered(DIK_R) && !(phaseIntroTimer_ >= 0.0f) && !isTitleMode_) {
                replayRecording_ = !replayRecording_;
                if (replayRecording_) {
                    replayFrameCounter_ = 0;
                    replaySessionIndex_++;
                    std::string filename = "Resources/replay_frames_" + std::to_string(replaySessionIndex_) + ".csv";
                    // ヘッダー行を一度書いて初期化
                    std::ofstream repFile(filename, std::ios::trunc);
                    if (repFile.is_open()) {
                        repFile << "frame,px,py,pz,proll,ppitch";
                        for (int i = 0; i < kMaxEnemies; ++i) {
                            repFile << ",e" << i << "_alive,e" << i << "_x,e" << i << "_y,e" << i << "_z,e" << i << "_rz";
                        }
                        for (int i = 0; i < 20; ++i) {
                            repFile << ",b" << i << "_alive,b" << i << "_x,b" << i << "_y,b" << i << "_z";
                        }
                        repFile << ",boss_active,boss_x,boss_y,boss_z,boss_ry\n";
                        repFile.close();
                    }
                }
            }

            if (replayRecording_) {
                std::string filename = "Resources/replay_frames_" + std::to_string(replaySessionIndex_) + ".csv";
                std::ofstream repFile(filename, std::ios::app);
                if (repFile.is_open()) {
                    float bodyBounceRec = 0.0f;
                    if (bossLegSwingSpeed_ > 0.0f) {
                        bodyBounceRec = std::sin(bossTime_ * 2.0f) * bossBodyBounceRange_;
                    }
                    float dropOffsetRec = 0.0f;
                    repFile << replayFrameCounter_++
                            << "," << fighterWorldPos.x
                            << "," << fighterWorldPos.y
                            << "," << fighterWorldPos.z
                            << "," << (fighterModel_ ? fighterModel_->transform.rotate.z : 0.0f)
                            << "," << (fighterModel_ ? fighterModel_->transform.rotate.x : 0.0f);
                    for (int i = 0; i < kMaxEnemies; ++i) {
                        repFile << "," << (enemies_[i].isAlive ? 1 : 0)
                                << "," << enemies_[i].position.x
                                << "," << enemies_[i].position.y
                                << "," << enemies_[i].position.z
                                << "," << enemies_[i].rotate.z;
                    }
                    // 弾の情報 (最大20発)
                    for (int i = 0; i < 20; ++i) {
                        bool bulletAlive = (playerBullets_[i].currentTime < playerBullets_[i].lifeTime);
                        repFile << "," << (bulletAlive ? 1 : 0)
                                << "," << playerBullets_[i].position.x
                                << "," << playerBullets_[i].position.y
                                << "," << playerBullets_[i].position.z;
                    }
                    bool bossActiveRec = (currentPhase_ == GamePhase::kBossFight);
                    repFile << "," << (bossActiveRec ? 1 : 0)
                            << ",0.0"
                            << "," << (bossYOffset_ + bodyBounceRec + dropOffsetRec)
                            << "," << (fighterWorldZ_ + bossZOffset_)
                            << "," << bossBodyRotY_ << "\n";
                    repFile.close();
                }
            }

            // ── Blenderパラメータ読み込み (60フレームに1回) ──
            if (++blenderParamReadCounter_ >= 60) {
                blenderParamReadCounter_ = 0;
                std::ifstream paramIn("Resources/blender_params.txt");
                if (paramIn.is_open()) {
                    std::string line;
                    while (std::getline(paramIn, line)) {
                        if (line.empty() || line[0] == '#') continue;
                        size_t pos = line.find('=');
                        if (pos == std::string::npos) continue;
                        std::string key = line.substr(0, pos);
                        std::string val = line.substr(pos + 1);
                        try {
                            float v = std::stof(val);
                            if (key == "boss_z_offset")       bossZOffset_      = v;
                            else if (key == "boss_y_offset") bossYOffset_      = v;
                            else if (key == "boss_scale")    bossScale_        = v;
                            else if (key == "player_speed_x")playerSpeedX_     = v;
                            else if (key == "player_speed_y")playerSpeedY_     = v;
                            else if (key == "player_limit_x")playerLimitX_     = v;
                            else if (key == "player_limit_y")playerLimitY_     = v;
                            else if (key == "boss_hp")        bossHP_           = v;
                            else if (key == "player_hp")      playerHP_         = v;
                        } catch (...) {}
                    }
                    paramIn.close();
                }
            }

            // ── 弾の発射（SPACE）＆ターゲット(クロスヘア)の更新 ──────────────────────
            // 曲がり角を曲がった後も常にプレイヤーの正面にターゲット(クロスヘア)が正対して表示されるよう、
            // レティクル位置はプレイヤーのローカルレール空間 (Right, Up, Forward) で計算・補間する
            Vector3 targetReticleLocal = { 0.0f, 0.0f, 120.0f }; // デフォルトはプレイヤー正面 120m 先

            if (currentPhase_ == GamePhase::kBossFight) {
                float dropOffset = 0.0f;
                float bodyBounce = 0.0f;
                if (bossLegSwingSpeed_ > 0.0f) {
                    bodyBounce = std::sin(bossTime_ * 2.0f) * bossBodyBounceRange_;
                }
                
                // ボスの攻撃による降下オフセットを反映
                float bossYAttackOffset = 0.0f;
                if (bossActionState_ == BossActionState::kLegAttack) {
                    if (bossActionTimer_ <= 5.0f) {
                        float t = bossActionTimer_ / 5.0f;
                        bossYAttackOffset = std::lerp(0.0f, -4.0f, t);
                    } else if (bossActionTimer_ <= 5.3f) {
                        float t = (bossActionTimer_ - 5.0f) / 0.3f;
                        bossYAttackOffset = std::lerp(-4.0f, -6.0f, t);
                    } else if (bossActionTimer_ <= 6.8f) {
                        float t = (bossActionTimer_ - 5.3f) / 1.5f;
                        bossYAttackOffset = std::lerp(-6.0f, 0.0f, t);
                    }
                }

                Vector3 bossTargetWorld = GetBossPosition(bodyBounce, dropOffset, bossYAttackOffset);

                // 弾が上空に飛びすぎるのを防ぐため、ターゲット位置をUP方向に沿って12.0m引き下げる
                float bossProgress = fighterWorldZ_ + bossZOffset_;
                Vector3 bossRailDir = GetRailDirection(bossProgress);
                Vector3 bossRailRight = CalculateRailRight(bossRailDir);
                Vector3 bossRailUp = CalculateRailUp(bossRailDir, bossRailRight);
                bossTargetWorld = Subtract(bossTargetWorld, Scale(bossRailUp, 12.0f));

                Vector3 toBoss = Subtract(bossTargetWorld, fighterWorldPos);
                float forwardDot = toBoss.x * playerRailDir.x + toBoss.y * playerRailDir.y + toBoss.z * playerRailDir.z;
                if (forwardDot > 5.0f) {
                    float rx = toBoss.x * playerRailRight.x + toBoss.y * playerRailRight.y + toBoss.z * playerRailRight.z;
                    float ry = toBoss.x * playerRailUp.x    + toBoss.y * playerRailUp.y    + toBoss.z * playerRailUp.z;
                    targetReticleLocal = { rx, ry, forwardDot };
                }
            } else {
                float bestDist2D = 30.0f;
                for (auto& enemy : enemies_) {
                    if (!enemy.isAlive) continue;
                    // レールの進行方向に対して「前方」にいる敵のみロックオン対象
                    Vector3 toEnemy = Subtract(enemy.position, fighterWorldPos);
                    float forwardDot = toEnemy.x * playerRailDir.x + toEnemy.y * playerRailDir.y + toEnemy.z * playerRailDir.z;
                    if (forwardDot > 5.0f) {
                        // プレイヤーのローカル空間（右・上）に投影して、画面上での2D距離を計算
                        float rx = toEnemy.x * playerRailRight.x + toEnemy.y * playerRailRight.y + toEnemy.z * playerRailRight.z;
                        float ry = toEnemy.x * playerRailUp.x    + toEnemy.y * playerRailUp.y    + toEnemy.z * playerRailUp.z;
                        float dist2D = std::sqrt(rx * rx + ry * ry);
                        if (dist2D < bestDist2D) {
                            bestDist2D = dist2D;
                            targetReticleLocal = { rx, ry, forwardDot };
                        }
                    }
                }
            }

            float aimLerpSpeed = 0.15f;
            if (aimReticlePos_.z < 1.0f) {
                aimReticlePos_ = targetReticleLocal;
            } else {
                aimReticlePos_.x = std::lerp(aimReticlePos_.x, targetReticleLocal.x, aimLerpSpeed);
                aimReticlePos_.y = std::lerp(aimReticlePos_.y, targetReticleLocal.y, aimLerpSpeed);
                aimReticlePos_.z = std::lerp(aimReticlePos_.z, targetReticleLocal.z, aimLerpSpeed);
            }

            // ローカル空間で補間したレティクル位置を、現在のプレイヤー正面基底(playerRailRight, playerRailUp, playerRailDir)でワールド座標へ変換
            Vector3 reticlePos = Add(
                fighterWorldPos,
                Add(
                    Scale(playerRailRight, aimReticlePos_.x),
                    Add(Scale(playerRailUp, aimReticlePos_.y), Scale(playerRailDir, aimReticlePos_.z))
                )
            );

            // ボス登場演出中は射撃を禁止
            if (input_->IsKeyTriggered(DIK_SPACE) && !(phaseIntroTimer_ >= 0.0f) && !isBossDefeatedSequence_ && !isBossIntro && !isTitleMode_) {
                // 翼の発射位置をレールのright/up方向で計算
                Vector3 leftWing  = Add(fighterWorldPos, Add(Scale(playerRailRight, -2.5f), Scale(playerRailUp, 0.8f)));
                Vector3 rightWing = Add(fighterWorldPos, Add(Scale(playerRailRight,  2.5f), Scale(playerRailUp, 0.8f)));

                Vector3 dirLeft  = Normalize({ reticlePos.x - leftWing.x,  reticlePos.y - leftWing.y,  reticlePos.z - leftWing.z });
                Vector3 dirRight = Normalize({ reticlePos.x - rightWing.x, reticlePos.y - rightWing.y, reticlePos.z - rightWing.z });
                float bulletSpeed = 150.0f + boostForwardSpeed_;

                for (auto& b : playerBullets_) {
                    if (b.currentTime >= b.lifeTime) {
                        b.position = leftWing; b.velocity = Scale(dirLeft, bulletSpeed);
                        b.lifeTime = 2.0f; b.currentTime = 0.0f; break;
                    }
                }
                for (auto& b : playerBullets_) {
                    if (b.currentTime >= b.lifeTime) {
                        b.position = rightWing; b.velocity = Scale(dirRight, bulletSpeed);
                        b.lifeTime = 2.0f; b.currentTime = 0.0f; break;
                    }
                }
                PlaySE(jumpSE_, false, 1.0f);
            }

            // エイミング(レティクル)のワールド行列更新：
            // 固定回転(0,0,0)ではなくプレイヤーの進行方向基底(R_reticle)を適用し、曲がった後もクロスヘアが横向きにならず常にプレイヤー正面を向くようにする
            Matrix4x4 R_reticle = MakeIdentity4x4();
            R_reticle.m[0][0] = playerRailRight.x; R_reticle.m[0][1] = playerRailRight.y; R_reticle.m[0][2] = playerRailRight.z;
            R_reticle.m[1][0] = playerRailUp.x;    R_reticle.m[1][1] = playerRailUp.y;    R_reticle.m[1][2] = playerRailUp.z;
            R_reticle.m[2][0] = playerRailDir.x;   R_reticle.m[2][1] = playerRailDir.y;   R_reticle.m[2][2] = playerRailDir.z;
            Matrix4x4 reticleWorld = Multiply(Multiply(MakeScaleMatrix(Vector3{ 8.0f, 8.0f, 8.0f }), R_reticle), MakeTranslateMatrix(reticlePos));
            aimingInstancingData_->World = reticleWorld;

            // ── 自機と敵・ボスの衝突判定 ────────────────────────
            // ボス登場演出中は衝突判定を行わない
            if (currentPhase_ == GamePhase::kBossFight && !(bossAppearanceTimer_ > 0.0f)) {
                // 落下登場演出時のオフセット
                float dropOffset = 0.0f;
                if (bossAppearanceTimer_ > 0.0f) {
                    float appRate = (3.0f - bossAppearanceTimer_) / 3.0f;
                    dropOffset = 120.0f * (1.0f - appRate) * (1.0f - appRate);
                }

                // ボスとの衝突
                float bodyBounce = 0.0f;
                if (bossLegSwingSpeed_ > 0.0f) {
                    bodyBounce = std::sin(bossTime_ * 2.0f) * bossBodyBounceRange_;
                }
                Vector3 bossPos = GetBossPosition(bodyBounce, dropOffset);
                Vector3 diff = Subtract(fighterWorldPos, bossPos);
                float dist = Length(diff);
                if (dist <= (bossCollisionRadius_ + playerCollisionRadius_)) { // 自機半径
                    // 接触中は毎フレーム少しずつダメージを受ける
                    playerHP_ -= 0.5f;
                    if (playerHP_ < 0.0f) playerHP_ = 0.0f;

                    // 一定頻度で被弾エフェクトとSEを発生
                    static uint32_t hitTimer = 0;
                    if (++hitTimer % 15 == 0) {
                        particleManager_->EmitHit(fighterWorldPos);
                        PlaySE(jumpSE_, false, 0.8f);
                    }
                }


            } else if (!(phaseIntroTimer_ >= 0.0f) && cornerState_ != CornerTurnState::kTurningWrong) {
                // 自機とビルの衝突判定（Boxコライダー対応）
                for (size_t i = 0; i < buildings_.size(); ++i) {
                    auto& b = buildings_[i];
                    if (b.floors <= 0 || b.isDestroyed) continue;
                    if (!b.collider.hasCollider) continue;
                    
                    Vector3 boxScale = { b.scale.x, b.scale.y * b.floors, b.scale.z };
                    if (IsCollidingOBBAndSphere(fighterWorldPos, playerCollisionRadius_, b.position, boxScale, b.rotate.y, b.collider.center, b.collider.size)) {
                        playerHP_ -= 0.5f; // 毎フレームダメージを受ける
                        if (playerHP_ < 0.0f) playerHP_ = 0.0f;
                        
                        static uint32_t bHitTimer = 0;
                        if (++bHitTimer % 15 == 0) {
                            particleManager_->EmitHit(fighterWorldPos);
                            PlaySE(jumpSE_, false, 0.8f);
                        }
                    }
                }

                // 雑魚敵との衝突
                for (auto& enemy : enemies_) {
                    if (!enemy.isAlive) continue;

                    bool hit = false;
                    if (enemy.collider.hasCollider) {
                        hit = IsCollidingOBBAndSphere(fighterWorldPos, playerCollisionRadius_, enemy.position, enemy.scale, enemy.rotate.y, enemy.collider.center, enemy.collider.size);
                    } else {
                        Vector3 diff = Subtract(fighterWorldPos, enemy.position);
                        float dist = Length(diff);
                        
                        if (dist <= (enemy.radius + playerCollisionRadius_)) {
                            hit = true;
                        }
                        // 特攻状態におけるすり抜け防止判定（曲がった後のレール方向にも対応）
                        else if (enemy.state == Enemy::State::kDive) {
                            float frameMovement = enemy.speed * kDeltaTime;
                            float fwdDiff = diff.x * playerRailDir.x + diff.y * playerRailDir.y + diff.z * playerRailDir.z;
                            if (std::abs(fwdDiff) < (frameMovement * 0.75f + 3.0f)) {
                                float rightDiff = diff.x * playerRailRight.x + diff.y * playerRailRight.y + diff.z * playerRailRight.z;
                                float upDiff    = diff.x * playerRailUp.x    + diff.y * playerRailUp.y    + diff.z * playerRailUp.z;
                                float distRightUp = std::sqrt(rightDiff * rightDiff + upDiff * upDiff);
                                if (distRightUp <= (enemy.radius + playerCollisionRadius_)) {
                                    hit = true;
                                }
                            }
                        }
                    }

                    if (hit) {
                        // 敵を撃破（雑魚敵の突進の威力を弱めに調整: 10.0f -> 3.0f）
                        enemy.isAlive = false;
                        playerHP_ -= 3.0f;
                        if (playerHP_ < 0.0f) playerHP_ = 0.0f;

                        // 被弾エフェクトとSE
                        EmitHitEffect(enemy.position);
                        PlaySE(jumpSE_, false, 1.2f);
                    }
                }
            }
        }
    }

    // ── 弾の更新（全モード共通）と敵・ボスとの衝突判定 ──
    if (!(phaseIntroTimer_ >= 0.0f)) {
        for (int i = 0; i < kMaxBullets; ++i) {
            if (playerBullets_[i].currentTime < playerBullets_[i].lifeTime) {
                playerBullets_[i].position = Add(playerBullets_[i].position, Scale(playerBullets_[i].velocity, kDeltaTime));
                playerBullets_[i].currentTime += kDeltaTime;

                // ボス登場演出中は弾との衝突判定を行わない
                if (currentPhase_ == GamePhase::kBossFight && !(bossAppearanceTimer_ > 0.0f)) {
                // 落下登場演出時のオフセット
                float dropOffset = 0.0f;
                if (bossAppearanceTimer_ > 0.0f) {
                    float appRate = (3.0f - bossAppearanceTimer_) / 3.0f;
                    dropOffset = 120.0f * (1.0f - appRate) * (1.0f - appRate);
                }

                float bodyBounce = 0.0f;
                if (bossLegSwingSpeed_ > 0.0f) {
                    bodyBounce = std::sin(bossTime_ * 2.0f) * bossBodyBounceRange_;
                }
                Vector3 bossPos = GetBossPosition(bodyBounce, dropOffset);

                Vector3 diff = Subtract(playerBullets_[i].position, bossPos);
                float dist = Length(diff);
                if (dist <= (bossCollisionRadius_ + 2.5f)) { // 弾の半径2.5f
                    // 弾を消去
                    playerBullets_[i].currentTime = playerBullets_[i].lifeTime;

                    // ボスにダメージ（1発あたりのダメージを減らして歯ごたえのあるボス戦に調整: 10.0f -> 4.0f）
                    bossHP_ -= 4.0f;
                    if (bossHP_ < 0.0f) bossHP_ = 0.0f;

                    // 被弾エフェクトとSE
                    EmitHitEffect(playerBullets_[i].position);
                    PlaySE(jumpSE_, false, 1.2f);
                }
            }
            // 非ボスフェーズ時の敵との当たり判定 (球衝突判定)
            else {
                for (auto& enemy : enemies_) {
                    if (!enemy.isAlive) continue;

                    bool hit = false;
                    if (enemy.collider.hasCollider) {
                        hit = IsCollidingOBBAndSphere(playerBullets_[i].position, 2.5f, enemy.position, enemy.scale, enemy.rotate.y, enemy.collider.center, enemy.collider.size);
                    } else {
                        Vector3 diff = Subtract(playerBullets_[i].position, enemy.position);
                        float dist = Length(diff);
                        // 弾の当たり判定半径を広げて（0.5f -> 2.5f）、すり抜けや近距離で当たらない現象を緩和
                        if (dist <= (enemy.radius + 2.5f)) {
                            hit = true;
                        }
                    }

                    if (hit) {
                        // 弾を消去
                        playerBullets_[i].currentTime = playerBullets_[i].lifeTime;

                        // 敵のHPを減算
                        enemy.hp -= 10.0f;
                        if (enemy.hp <= 0.0f) {
                            // 敵を撃破
                            enemy.isAlive = false;
                            Vector3 deathPos = enemy.position; // 爆発エフェクト発生位置を記録

                            // 全滅リポップはUpdateの最後で一括判定するため、ここではリポップ処理を行わない

                            // ★★★ 超ド派手爆破エフェクト！ ★★★
                            EmitHitEffect(deathPos);
                            // プリセット5(氷結)・8(神聖)以外のときは属性色のシリンダーとリングを重ね合わせて撃破を強調
                            if (selectedEffectPreset_ != 5 && selectedEffectPreset_ != 8) {
                                particleManager_->EmitCylinder(deathPos, effectBaseColor_);
                                particleManager_->EmitRing(deathPos, effectBaseColor_);
                            }

                            // 爆破音を再生
                            PlaySE(jumpSE_, false, 1.5f);
                        } else {
                            // 生存時は小規模な被弾エフェクトとSE
                            EmitHitEffect(playerBullets_[i].position);
                            PlaySE(jumpSE_, false, 0.6f);
                        }
                        break;
                    }
                }
            }
        }
    }
}

    // ── 蜘蛛ボス（Big Spider）の攻撃AIと攻撃更新 ──
    if (currentPhase_ == GamePhase::kBossFight && !isBossDefeatedSequence_ && bossAppearanceTimer_ <= 0.0f) {
        bossActionTimer_ += kDeltaTime;

        // 攻撃周期：2.5秒ごとに攻撃を切り替える（高速化）
        if (bossActionState_ == BossActionState::kIdle && bossActionTimer_ >= 2.5f) {
            bossActionTimer_ = 0.0f;
            // デバッグ確認用：常に近接攻撃（100%確率）を行う
            bossActionState_ = BossActionState::kLegAttack;
        }

        // 各攻撃パターンに応じたボスの物理的な移動やアニメーション設定
        if (bossActionState_ == BossActionState::kLegAttack) {
            // 足殴り：ボスが一時的にプレイヤーに急接近する（6.8秒で往復：5.0秒接近・振りかぶり溜め、0.3秒叩きつけ、1.5秒後退に調整）
            static bool hasShaken = false;
            Vector3 lightningColor = { 0.68f, 0.05f, 1.0f }; // カッコいいネオンパープル（紫色の雷）
            
            float bodyBounce = std::sin(bossTime_ * 2.0f) * bossBodyBounceRange_;
            Vector3 currentBossPos = GetBossPosition(bodyBounce, 0.0f);

            if (bossActionTimer_ <= 5.0f) {
                // 1. 接近・振りかぶり溜め (0.0〜5.0秒)
                float t = bossActionTimer_ / 5.0f;
                bossZOffset_ = std::lerp(169.0f, 65.0f, t);
                hasShaken = false; // 振りかぶりフェーズではシェイクフラグをリセット
                
                // 振りかぶり中にプレイヤーの位置を追従し、ターゲットエリアを決定（レールローカル横位置で判定するように修正）
                float playerLocalX = fighterModel_ ? fighterModel_->transform.translate.x : 0.0f;
                if (playerLocalX < -15.0f) {
                    bossAttackTargetArea_ = 0; // 左
                } else if (playerLocalX > 15.0f) {
                    bossAttackTargetArea_ = 2; // 右
                } else {
                    bossAttackTargetArea_ = 1; // 中央
                }
                isBossAttackTargetLocked_ = false;

                // ★★★ 紫の雷の溜めエフェクト ★★★
                // ボス本体の周囲に雷オーラを発生
                particleManager_->EmitLightning(currentBossPos, 8.0f, 2, lightningColor);
                
                // 振りかぶる右前脚（i=5）自体に強烈な紫の電撃オーラを纏わせる
                if (bossLegTransformData_[5]) {
                    Matrix4x4 w = bossLegTransformData_[5]->World;
                    Vector3 legDir = { w.m[2][0], w.m[2][1], w.m[2][2] };
                    Vector3 legRoot = { w.m[3][0], w.m[3][1], w.m[3][2] };
                    
                    float dirLen = std::sqrt(legDir.x * legDir.x + legDir.y * legDir.y + legDir.z * legDir.z);
                    if (dirLen > 0.001f) {
                        legDir = { legDir.x / dirLen, legDir.y / dirLen, legDir.z / dirLen };
                    }
                    
                    float legLen = bossLegScale_ * 0.55f;
                    Vector3 legTip = {
                        legRoot.x + legDir.x * legLen,
                        legRoot.y + legDir.y * legLen,
                        legRoot.z + legDir.z * legLen
                    };
                    
                    Vector3 legMid = {
                        (legRoot.x + legTip.x) * 0.5f,
                        (legRoot.y + legTip.y) * 0.5f,
                        (legRoot.z + legTip.z) * 0.5f
                    };

                    // 1. 脚全体を包む極太の這いずり電撃オーラ（スケールを負の巨大値 -1.85f に拡張）
                    // 毎フレーム3組重複して放出することで、脚に絡みつく無数のうねる巨大電撃を生成
                    for (int k = 0; k < 3; ++k) {
                        particleManager_->EmitLSystemLightning(legRoot, legTip, 3, -1.85f, lightningColor);
                    }

                    // 2. 脚の先端・中間・根本から周囲の空間へ大きく弾け飛ぶ放射スパーク
                    // 半径 15.0f〜24.0f の非常に大きな紫の火花を空間へ放出する
                    particleManager_->EmitLightning(legTip, 24.0f, 3, lightningColor);
                    particleManager_->EmitLightning(legMid, 18.0f, 2, lightningColor);
                    particleManager_->EmitLightning(legRoot, 14.0f, 1, lightningColor);
                }

                // 狙っている地面（ターゲットエリア）にバチバチと警告予兆の雷を落とす
                float targetX = 0.0f;
                if (bossAttackTargetArea_ == 0) targetX = -30.0f;
                else if (bossAttackTargetArea_ == 2) targetX = 30.0f;

                // ターゲット位置のレール座標系を算出
                float targetProgress = fighterWorldZ_ + 65.0f;
                Vector3 targetRailPos = GetRailPosition(targetProgress);
                Vector3 targetRailDir = GetRailDirection(targetProgress);
                Vector3 targetRailRight = Normalize(Vector3{ targetRailDir.z, 0.0f, -targetRailDir.x });
                Vector3 targetRailUp = Cross(targetRailDir, targetRailRight);
                float targetRelY = bossYOffset_ + 14.0f; // -20m路面に対する相対Y

                Vector3 warningPos = Add(targetRailPos, Add(Scale(targetRailRight, targetX), Scale(targetRailUp, targetRelY)));
                particleManager_->EmitLightning(warningPos, 5.0f, 1, lightningColor);

            } else if (bossActionTimer_ <= 5.3f) {
                // 2. 振り下ろし叩きつけ (5.0〜5.3秒)
                bossZOffset_ = 65.0f;
                bossLegSwingRange_ = 1.2f; // 足を激しく動かす
                
                // 振り下ろし開始時にターゲットエリアをロックオン
                isBossAttackTargetLocked_ = true;

                // 振り下ろし中も激しいスパークをボスとターゲット先に落とす
                float targetX = 0.0f;
                if (bossAttackTargetArea_ == 0) targetX = -30.0f;
                else if (bossAttackTargetArea_ == 2) targetX = 30.0f;

                float targetProgress = fighterWorldZ_ + 65.0f;
                Vector3 targetRailPos = GetRailPosition(targetProgress);
                Vector3 targetRailDir = GetRailDirection(targetProgress);
                Vector3 targetRailRight = Normalize(Vector3{ targetRailDir.z, 0.0f, -targetRailDir.x });
                Vector3 targetRailUp = Cross(targetRailDir, targetRailRight);
                float targetRelY = bossYOffset_ + 14.0f;

                Vector3 warningPos = Add(targetRailPos, Add(Scale(targetRailRight, targetX), Scale(targetRailUp, targetRelY)));
                particleManager_->EmitLightning(currentBossPos, 12.0f, 2, lightningColor);
                particleManager_->EmitLightning(warningPos, 12.0f, 2, lightningColor);

            } else if (bossActionTimer_ <= 6.8f) {
                // 3. 戻り (5.3〜6.8秒)
                // ─── 巨大な「1本の雷」の3フェーズタイムライン (先行微光➔本落雷➔残光明滅) ───
                float strikeElapsed = bossActionTimer_ - 5.3f; // 激突からの経過時間

                // ターゲットレール情報の準備
                float targetProgress = fighterWorldZ_ + 65.0f;
                Vector3 targetRailPos = GetRailPosition(targetProgress);
                Vector3 targetRailDir = GetRailDirection(targetProgress);
                Vector3 targetRailRight = Normalize(Vector3{ targetRailDir.z, 0.0f, -targetRailDir.x });
                Vector3 targetRailUp = Cross(targetRailDir, targetRailRight);
                float targetRelY = bossYOffset_ + 14.0f;

                // ボス手元の電撃開始レール情報の準備
                float bossProgress = fighterWorldZ_ + bossZOffset_;
                Vector3 bossRailPos = GetRailPosition(bossProgress);
                Vector3 bossRailDir = GetRailDirection(bossProgress);
                Vector3 bossRailRight = Normalize(Vector3{ bossRailDir.z, 0.0f, -bossRailDir.x });
                Vector3 bossRailUp = Cross(bossRailDir, bossRailRight);

                if (strikeElapsed <= 0.05f) {
                    // ① 先行微光 (Leader) [0.00s 〜 0.05s / 約3フレーム]
                    float targetX = 0.0f;
                    if (bossAttackTargetArea_ == 0) targetX = -30.0f;
                    else if (bossAttackTargetArea_ == 2) targetX = 30.0f;

                    Vector3 impactPos = Add(targetRailPos, Add(Scale(targetRailRight, targetX), Scale(targetRailUp, targetRelY)));
                    Vector3 lightningStart = Add(bossRailPos, Add(Scale(bossRailUp, bossYOffset_ + 16.0f), Scale(bossRailDir, -15.0f)));
                    
                    // 暗めの紫色 (-0.18fスケールで細い枝として生成)
                    Vector3 leaderColor = { 0.35f, 0.02f, 0.5f };
                    particleManager_->EmitLSystemLightning(lightningStart, impactPos, 3, -0.18f, leaderColor);
                    
                    // ポストプロセスフラッシュもまだ極小
                    activePostProcess_ = kVignette;
                    // 雷のX座標をUV座標にマッピング (-30➔0.16, 0➔0.5, 30➔0.84)
                    float targetUvX = 0.5f;
                    if (bossAttackTargetArea_ == 0) targetUvX = 0.16f;
                    else if (bossAttackTargetArea_ == 2) targetUvX = 0.84f;
                    vignetteParamData_->scale = targetUvX;
                    vignetteParamData_->power = 10.0f + 0.15f; // ごく僅かな明かり

                } else if (strikeElapsed <= 0.10f) {
                    // ② 本落雷 (Return Stroke) [0.05s 〜 0.10s / 約3フレーム]
                    float targetX = 0.0f;
                    if (bossAttackTargetArea_ == 0) targetX = -30.0f;
                    else if (bossAttackTargetArea_ == 2) targetX = 30.0f;

                    Vector3 impactPos = Add(targetRailPos, Add(Scale(targetRailRight, targetX), Scale(targetRailUp, targetRelY)));
                    Vector3 lightningStart = Add(bossRailPos, Add(Scale(bossRailUp, bossYOffset_ + 16.0f), Scale(bossRailDir, -15.0f)));

                    if (!hasShaken) {
                        cameraShakeTimer_ = 0.78f;
                        cameraShakeIntensity_ = 10.5f; // 最大震度
                        activeShakeIntensity_ = 10.5f;
                        cameraShakeTimeMax_ = 0.78f;
                        hasShaken = true;

                        // 地面の破片を飛び散らせる
                        triggerDebrisEmit_ = true;
                        debrisEmitPos_ = Add(targetRailPos, Scale(targetRailRight, targetX));
                        debrisEmitCount_ = 30000;

                        // 極太の主幹雷撃(1.6fスケール)を走らせる
                        particleManager_->EmitLSystemLightning(lightningStart, impactPos, 4, 1.6f, lightningColor);

                        // 周囲の爆発エフェクトと大音響SE
                        particleManager_->EmitCylinder(impactPos, lightningColor);
                        particleManager_->EmitRing(impactPos, lightningColor);
                        particleManager_->EmitCustomSparks(impactPos, 25.0f, 15, { 0.5f, 0.5f, 0.5f }, 1.0f);
                        PlaySE(jumpSE_, false, 1.7f);

                        // ダメージ判定
                        // レール横方向（右方向ベクトル）の内積などからプレイヤーの左右エリアを判定
                        // 簡略化のため、プレイヤーの相対X (fighterModel_->transform.translate.x) を使用する
                        float playerRelX = fighterModel_ ? fighterModel_->transform.translate.x : 0.0f;
                        int playerArea = 1;
                        if (playerRelX < -15.0f) playerArea = 0;
                        else if (playerRelX > 15.0f) playerArea = 2;
                        if (playerArea == bossAttackTargetArea_) {
                            playerHP_ -= 25.0f;
                            if (playerHP_ < 0.0f) playerHP_ = 0.0f;
                            particleManager_->EmitHit(fighterWorldPos);
                            PlaySE(jumpSE_, false, 1.2f);
                        }
                    }

                    // 全画面フラッシュバースト（最大露出）
                    activePostProcess_ = kVignette;
                    float targetUvX = 0.5f;
                    if (bossAttackTargetArea_ == 0) targetUvX = 0.16f;
                    else if (bossAttackTargetArea_ == 2) targetUvX = 0.84f;
                    vignetteParamData_->scale = targetUvX;
                    vignetteParamData_->power = 11.0f; // 10.0 + 1.0 (最大フラッシュ)

                } else {
                    // ③ 残光・明滅 (Flicker) [0.10s 〜 1.5s]
                    float currentIntensity = 1.0f - (strikeElapsed - 0.10f) / 1.4f; // 1.0 ➔ 0.0
                    currentIntensity = (std::max)(0.0f, currentIntensity);

                    // 高速なバチバチ明滅
                    float flicker = (std::sin(strikeElapsed * 48.0f) * 0.5f + 0.5f); // 0.0 〜 1.0
                    float flashIntensity = currentIntensity * (0.3f + 0.7f * flicker);

                    if (currentIntensity <= 0.0f) {
                        activePostProcess_ = kNone;
                        vignetteParamData_->power = 0.8f;
                    } else {
                        activePostProcess_ = kVignette;
                        float targetUvX = 0.5f;
                        if (bossAttackTargetArea_ == 0) targetUvX = 0.16f;
                        else if (bossAttackTargetArea_ == 2) targetUvX = 0.84f;
                        vignetteParamData_->scale = targetUvX;
                        vignetteParamData_->power = 10.0f + flashIntensity;

                        // 残光中のバチバチスパーク
                        if (flicker > 0.7f) {
                            float targetX = 0.0f;
                            if (bossAttackTargetArea_ == 0) targetX = -30.0f;
                            else if (bossAttackTargetArea_ == 2) targetX = 30.0f;
                            Vector3 impactPos = Add(targetRailPos, Add(Scale(targetRailRight, targetX), Scale(targetRailUp, targetRelY)));
                            particleManager_->EmitLightning(impactPos, 8.0f, 1, lightningColor);
                        }
                    }
                }

                float t = (bossActionTimer_ - 5.3f) / 1.5f;
                bossZOffset_ = std::lerp(65.0f, 169.0f, t);
                isBossAttackTargetLocked_ = true;
            } else {
                // 戻りフェーズ中、または攻撃が終わったら徐々に全画面雷撃フラッシュエフェクトをフェードアウトさせる
                if (activePostProcess_ == kVignette && vignetteParamData_->power >= 10.0f) {
                    float currentIntensity = vignetteParamData_->power - 10.0f;
                    currentIntensity -= kDeltaTime * 1.5f;
                    if (currentIntensity <= 0.0f) {
                        activePostProcess_ = kNone;
                        vignetteParamData_->power = 0.8f;
                    } else {
                        vignetteParamData_->power = 10.0f + currentIntensity;
                    }
                }

                bossZOffset_ = 169.0f;
                bossLegSwingRange_ = 0.3f; // 元に戻す
                bossActionState_ = BossActionState::kIdle;
                bossActionTimer_ = 0.0f;
                isBossAttackTargetLocked_ = false;
            }
        } 
        else if (bossActionState_ == BossActionState::kLaserAttack) {
            // 糸レーザー：3秒間、高密度で直進パーティクルを射出する
            if (bossActionTimer_ <= 3.0f) {
                bossLaserTimer_ += kDeltaTime;
                if (bossLaserTimer_ >= 0.08f) { // 0.08秒ごとに2発
                    bossLaserTimer_ = 0.0f;
                    
                    float bodyBounce = std::sin(bossTime_ * 2.0f) * bossBodyBounceRange_;
                    Vector3 bossWebFirePos = {
                        bossWebFireOffset_.x,
                        bossYOffset_ + bodyBounce + bossWebFireOffset_.y,
                        fighterWorldZ_ + bossZOffset_ + bossWebFireOffset_.z
                    };
                    
                    EulerTransform& camTransForLaser = camera_->GetTransform();
                    fighterWorldPos = {
                        camTransForLaser.translate.x + fighterModel_->transform.translate.x,
                        camTransForLaser.translate.y - 3.0f + fighterModel_->transform.translate.y,
                        fighterWorldZ_
                    };
                    
                    particleManager_->EmitLaserThread(bossWebFirePos, fighterWorldPos);
                    PlaySE(jumpSE_, false, 0.3f);
                }
            } else {
                bossActionState_ = BossActionState::kIdle;
                bossActionTimer_ = 0.0f;
            }
        } 
        else if (bossActionState_ == BossActionState::kWebAttack) {
            // 蜘蛛の巣弾：プレイヤーに向けて巨大な蜘蛛の巣弾を1発射出する
            float bodyBounce = std::sin(bossTime_ * 2.0f) * bossBodyBounceRange_;
            Vector3 bossWebFirePos = {
                bossWebFireOffset_.x,
                bossYOffset_ + bodyBounce + bossWebFireOffset_.y,
                fighterWorldZ_ + bossZOffset_ + bossWebFireOffset_.z
            };
            
            EulerTransform& camTransForWeb = camera_->GetTransform();
            fighterWorldPos = {
                camTransForWeb.translate.x + fighterModel_->transform.translate.x,
                camTransForWeb.translate.y - 3.0f + fighterModel_->transform.translate.y,
                fighterWorldZ_
            };

            for (auto& web : bossWebBullets_) {
                if (!web.isAlive) {
                    web.isAlive = true;
                    web.position = bossWebFirePos;
                    
                    Vector3 dir = { fighterWorldPos.x - bossWebFirePos.x, fighterWorldPos.y - bossWebFirePos.y, fighterWorldPos.z - bossWebFirePos.z };
                    float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
                    if (len > 0.01f) {
                        dir = { dir.x / len, dir.y / len, dir.z / len };
                    } else {
                        dir = { 0.0f, 0.0f, -1.0f };
                    }
                    float speed = 100.0f; // 中速
                    web.velocity = { dir.x * speed, dir.y * speed, dir.z * speed };
                    
                    PlaySE(jumpSE_, false, 1.4f);
                    break;
                }
            }
            
            bossActionState_ = BossActionState::kIdle;
            bossActionTimer_ = 0.0f;
        }

        // ── 蜘蛛の巣弾の更新処理と自機との衝突・回避判定 ──
        EulerTransform& camTrans = camera_->GetTransform();
        fighterWorldPos = {
            camTrans.translate.x + fighterModel_->transform.translate.x,
            camTrans.translate.y - 3.0f + fighterModel_->transform.translate.y,
            fighterWorldZ_
        };

        for (auto& web : bossWebBullets_) {
            if (web.isAlive) {
                // 弾の移動
                web.position = {
                    web.position.x + web.velocity.x * kDeltaTime,
                    web.position.y + web.velocity.y * kDeltaTime,
                    web.position.z + web.velocity.z * kDeltaTime
                };

                // 自機との衝突判定
                Vector3 diff = { fighterWorldPos.x - web.position.x, fighterWorldPos.y - web.position.y, fighterWorldPos.z - web.position.z };
                float dist = std::sqrt(diff.x * diff.x + diff.y * diff.y + diff.z * diff.z);
                if (web.position.z < fighterWorldPos.z + 5.0f && web.position.z > fighterWorldPos.z - 5.0f && dist <= (web.radius + playerCollisionRadius_)) { // 自機半径
                    web.isAlive = false;
                    playerHP_ -= 20.0f; // 大ダメージ
                    if (playerHP_ < 0.0f) playerHP_ = 0.0f;
                    
                    // デバフ発動：3秒間の移動速度半減
                    playerSpeedDebuffTimer_ = 3.0f;

                    particleManager_->EmitHit(fighterWorldPos);
                    PlaySE(jumpSE_, false, 1.5f);
                }
                // 自機を通り越して回避成功したか判定
                else if (web.position.z < fighterWorldPos.z - 5.0f) {
                    web.isAlive = false;
                    screenWebTimer_ = 5.0f; // 画面蜘蛛の巣効果タイマー設定（5秒間）
                }
            }
        }

        // ── 糸レーザーと自機との衝突判定 ──
        if (bossActionState_ == BossActionState::kLaserAttack && bossActionTimer_ <= 3.0f) {
            static float laserHitCooldown = 0.0f;
            laserHitCooldown += kDeltaTime;
            if (laserHitCooldown >= 0.2f) {
                laserHitCooldown = 0.0f;
                
                float dx = fighterWorldPos.x - 0.0f;
                float dy = fighterWorldPos.y - (bossYOffset_ - 2.0f);
                float dist2D = std::sqrt(dx * dx + dy * dy);
                
                // 避けていなければ小ダメージ
                if (dist2D < 15.0f) {
                    playerHP_ -= 1.5f;
                    if (playerHP_ < 0.0f) playerHP_ = 0.0f;
                    
                    particleManager_->EmitHit(fighterWorldPos);
                    PlaySE(jumpSE_, false, 0.5f);
                }
            }
        }
    }

    // ── 蜘蛛ボス（Big Spider）のトランスフォームとアニメーション更新 ──
    if (!isTitleMode_ && bossBodyTransformData_ && isBossModelVisible_) {
        // 歩行速度が0より大きい時のみ、8本の足のステップに連動した真ん中の胴体の動き（上下バウンス・左右重心揺れ・前後うねり・ロール/ピッチ/ヨー）を計算
        float bodyBounce = 0.0f;
        float bodyRoll = 0.0f;
        float bodyPitchWave = 0.0f;
        float bodyYawWave = 0.0f;
        float bodySwayRight = 0.0f;
        float bodySurgeDir = 0.0f;
        float wholeSpiderLateralSway = 0.0f;
        if (bossLegSwingSpeed_ > 0.0f) {
            // 4脚ずつの交互ステップ(1周期に2回接地)に合わせた重厚な上下バウンス
            bodyBounce = std::sin(bossTime_ * 2.0f - 0.35f) * bossBodyBounceRange_;
            // 左右の足踏み交代に合わせた胴体のロール角・頭腹部のピッチ角・首振りヨー角
            bodyRoll = std::cos(bossTime_) * (bossBodyRollRange_ * static_cast<float>(M_PI) / 180.0f);
            bodyPitchWave = std::sin(bossTime_ * 2.0f - 0.5f) * 0.11f;
            bodyYawWave = std::sin(bossTime_) * 0.095f;
            // 真ん中の胴体自体の左右重心スウェイ＆前後サージ
            bodySwayRight = std::cos(bossTime_) * 3.0f;
            bodySurgeDir = std::sin(bossTime_ * 2.0f) * 1.8f;
            // ボス全体もその場で足踏みするだけでなく、軽く左右へ這うように横移動させる
            wholeSpiderLateralSway = std::sin(bossTime_ * 0.45f) * 6.5f;
        }

        // ── 振りかぶり近接攻撃のアニメーション計算 ──
        camTrans = camera_->GetTransform();
        fighterWorldPos = {
            camTrans.translate.x + fighterModel_->transform.translate.x,
            camTrans.translate.y - 3.0f + fighterModel_->transform.translate.y,
            fighterWorldZ_
        };
        // 常に右前脚の Pair 1 (i=5) の足で殴る
        int attackLegIdx = 5;

        float bossYAttackOffset = 0.0f;

        if (currentPhase_ == GamePhase::kBossFight && bossActionState_ == BossActionState::kLegAttack) {
            if (bossActionTimer_ <= 5.0f) {
                // 1. 接近・振りかぶり溜め (0.0〜5.0秒)
                float t = bossActionTimer_ / 5.0f;
                bossYAttackOffset = std::lerp(0.0f, -4.0f, t);
            } else if (bossActionTimer_ <= 5.3f) {
                // 2. 叩きつけ (5.0〜5.3秒)
                float t = (bossActionTimer_ - 5.0f) / 0.3f;
                bossYAttackOffset = std::lerp(-4.0f, -6.0f, t);
            } else if (bossActionTimer_ <= 6.8f) {
                // 3. 戻り (5.3〜6.8秒)
                float t = (bossActionTimer_ - 5.3f) / 1.5f;
                bossYAttackOffset = std::lerp(-6.0f, 0.0f, t);
            }
        }

        // ボスは最初からステージ上に配置されているため落下オフセットは0
        float dropOffset = 0.0f;

        // 胴体のワールド行列計算
        // ボスはプレイヤーの前方 bossZOffset_ の位置に進み、高さは接地高さ bossYOffset_
        Vector3 bossPos = GetBossPosition(bodyBounce, dropOffset, bossYAttackOffset);
        
        // ── ボスジャンプ時の土煙・石の飛び散りエフェクトトリガー ──
        if (currentPhase_ == GamePhase::kBossFight) {
            float bossProgress = fighterWorldZ_ + bossZOffset_;
            float cornerS = 800.0f;
            float jumpStart = cornerS - 55.0f;  // 745m (踏み切り)
            float jumpEnd   = cornerS + 110.0f; // 910m (着地)
            float landEnd   = cornerS + 150.0f; // 950m (復帰終了)

            if (bossProgress >= jumpStart && bossProgress < jumpEnd) {
                if (!hasEmittedJumpStart_) {
                    hasEmittedJumpStart_ = true;
                    // 踏み切り！石や土の飛び散りエフェクト
                    Vector3 jumpStartPos = GetBossPosition(0.0f, 0.0f, 0.0f);
                    EmitHitEffect(jumpStartPos);
                    for (int k = 0; k < 20; ++k) {
                        particleManager_->EmitHit(jumpStartPos);
                    }
                    particleManager_->EmitRing(jumpStartPos, {0.8f, 0.5f, 0.2f}); // 土煙リング
                    particleManager_->EmitCylinder(jumpStartPos, {0.6f, 0.4f, 0.2f});
                    if (audio_) {
                        PlaySE(jumpSE_, false, 1.8f); // 踏み切り音
                    }
                }
            }
            else if (bossProgress >= jumpEnd && bossProgress < landEnd) {
                if (!hasEmittedJumpEnd_) {
                    hasEmittedJumpEnd_ = true;
                    // 着地！激しい土砂飛び散り
                    Vector3 jumpEndPos = GetBossPosition(0.0f, 0.0f, 0.0f);
                    EmitHitEffect(jumpEndPos);
                    for (int k = 0; k < 25; ++k) {
                        particleManager_->EmitHit(jumpEndPos);
                    }
                    particleManager_->EmitRing(jumpEndPos, {0.8f, 0.5f, 0.2f});
                    particleManager_->EmitCylinder(jumpEndPos, {0.6f, 0.4f, 0.2f});
                    if (audio_) {
                        PlaySE(jumpSE_, false, 2.0f); // 着地音
                    }
                }
            }
            else if (bossProgress > landEnd) {
                // ジャンプと着地が終わったら、次回のジャンプのためにフラグをリセット
                hasEmittedJumpStart_ = false;
                hasEmittedJumpEnd_ = false;
            }
        }

        Vector3 bossBodyScale = { bossBodyScale_, bossBodyScale_, bossBodyScale_ }; // 胴体専用スケール
        // 蜘蛛メッシュ(big+Spider.obj)のローカル軸に合わせて、X軸(左右ロール)・Y軸(対面向き＋首振りヨー)・Z軸(頭と腹の上下ピッチうねり)を合成
        Vector3 bossRotate = {
            bodyRoll,
            bossBodyRotY_ * static_cast<float>(M_PI) / 180.0f + bodyYawWave,
            bodyPitchWave
        };

        float roarBlend = 0.0f;
        // ボス登場カメラ完了後の咆哮姿勢（9.8秒から11.2秒の咆哮フェーズで上を向いて吠えるポーズと微振動を適用）
        if (bossAppearanceTimer_ > 0.0f) {
            float t = kBossIntroTotalDuration - bossAppearanceTimer_;
            if (t >= 9.8f && t < 11.2f) {
                float roarProgress = std::clamp((t - 9.8f) / 1.4f, 0.0f, 1.0f);
                float easeRoar = std::sin(roarProgress * static_cast<float>(M_PI)); // 0 -> 1 -> 0
                roarBlend = easeRoar;

                // 咆哮で天を仰ぐのけぞり
                bossRotate.x = -0.5f * easeRoar;
                bossRotate.z = 0.35f * easeRoar;

                // 胴体を大きく上方に持ち上げ、身を起こす威嚇動作 (最大 12.0m 上昇、8.0m 後退)
                bossPos.y += 12.0f * easeRoar;
                bossPos.z += 8.0f * easeRoar;

                // 咆哮の全身超激震 (高周波激震)
                float shakeFreq = 65.0f;
                bossRotate.y += std::sin(t * shakeFreq) * 0.08f;
                bossRotate.z += std::cos(t * shakeFreq) * 0.08f;
                bossPos.y += std::sin(t * shakeFreq) * 0.45f;
                bossPos.z += std::cos(t * shakeFreq) * 0.30f;
            }
        }

        // ボス位置におけるレール方向から姿勢行列を構築
        float bossProgress = fighterWorldZ_ + bossZOffset_;
        Vector3 bossRailDir = GetRailDirection(bossProgress);
        Vector3 bossRailRight = CalculateRailRight(bossRailDir);
        Vector3 bossRailUp = CalculateRailUp(bossRailDir, bossRailRight);

        // ボス全体の緩やかな左右横移動(wholeSpiderLateralSway)と、真ん中の胴体自身の重心移動(bodySwayRight / bodySurgeDir)を適用
        float swayScale = 1.0f - roarBlend;
        Vector3 baseSpiderPos = Add(bossPos, Scale(bossRailRight, wholeSpiderLateralSway * swayScale));
        // 胴体と8本の足の親フレームが絶対に離れないよう、上下バウンス・左右スウェイ・前後サージの基準座標(spiderSharedPos)を統一する
        Vector3 spiderSharedPos = Add(baseSpiderPos, Add(Scale(bossRailRight, bodySwayRight * swayScale), Scale(bossRailDir, bodySurgeDir * swayScale)));
        // 胴体モデル(big+Spider.obj)の太い胸腹部が8本の足の真ん中にピタッと収まるよう、胴体を前方(プレイヤー側: -bossRailDir)へ 8.8m 寄せ、高さを -4.8m 密着させる
        Vector3 bossBodyPos = Add(spiderSharedPos, Add(Scale(bossRailDir, -8.8f), Scale(bossRailUp, -4.8f)));
        Vector3 bossLegsPos = spiderSharedPos;

        Matrix4x4 R_rail = MakeIdentity4x4();
        R_rail.m[0][0] = bossRailRight.x; R_rail.m[0][1] = bossRailRight.y; R_rail.m[0][2] = bossRailRight.z;
        R_rail.m[1][0] = bossRailUp.x;    R_rail.m[1][1] = bossRailUp.y;    R_rail.m[1][2] = bossRailUp.z;
        R_rail.m[2][0] = bossRailDir.x;   R_rail.m[2][1] = bossRailDir.y;   R_rail.m[2][2] = bossRailDir.z;

        // 真ん中の胴体および8本の足の共通ローカル回転行列 (うねり・ロール・ピッチ・ヨーを一体として反映し、胴体と足の付け根の分離をゼロにする)
        Matrix4x4 rotateX = MakeRotateXMatrix(bossRotate.x);
        Matrix4x4 rotateY = MakeRotateYMatrix(bossRotate.y);
        Matrix4x4 rotateZ = MakeRotateZMatrix(bossRotate.z);
        Matrix4x4 R_localBody = Multiply(Multiply(rotateZ, rotateX), rotateY);
        Matrix4x4 R_finalBody = Multiply(R_localBody, R_rail);

        Matrix4x4 bossWorld = Multiply(Multiply(MakeScaleMatrix(bossBodyScale), R_finalBody), MakeTranslateMatrix(bossBodyPos));
        bossBodyTransformData_->World = bossWorld;

        // 8本の足の親フレームにも胴体と完全に同一の回転行列(R_finalBody)を適用し、ソケットが常に胴体内部にロックされるようにする
        Matrix4x4 R_final = R_finalBody;

        // 胴体の「スケールなし」の足基準行列
        Matrix4x4 bossWorldNoScale = Multiply(R_final, MakeTranslateMatrix(bossLegsPos));

        // 4組の左右対称な足ペアパラメータを配列化してアクセス
        Vector3 legPairPos[4] = { bossLegPairPos0_, bossLegPairPos1_, bossLegPairPos2_, bossLegPairPos3_ };
        float legPairRotY[4] = { bossLegPairRotY0_, bossLegPairRotY1_, bossLegPairRotY2_, bossLegPairRotY3_ };

        // プレイヤーの現在のレール位置と軸方向を取得（足の攻撃ターゲット計算用）
        Vector3 playerRailPos = GetRailPosition(fighterWorldZ_);
        Vector3 playerRailDir = GetRailDirection(fighterWorldZ_);
        Vector3 playerRailRight = CalculateRailRight(playerRailDir);

        for (int i = 0; i < 8; ++i) {
            if (!bossLegTransformData_[i]) continue;

            int pairIdx = (i < 4) ? i : (i - 4); // 0,1,2,3 ➔ 左足(前→後)、4,5,6,7 ➔ 右足(前→後)

            // ── 本物の蜘蛛らしい8脚テトラポッド歩行（交互4脚＋前後への波状位相差）と力強い足踏み（Step & Stance）計算 ──
            // Group A (i = 0, 2, 5, 7) と Group B (i = 1, 3, 4, 6) を半周期(0.5)ずらしつつ、
            // 前後の脚ペアごとにわずかな波状位相差(pairIdx * 0.06f)を与えて生き物らしい多脚の足踏み感を作る
            bool isGroupB = (i == 1 || i == 3 || i == 4 || i == 6);
            float phaseCycle = (bossTime_ / (2.0f * static_cast<float>(M_PI))) + (isGroupB ? 0.5f : 0.0f) + static_cast<float>(pairIdx) * 0.06f;
            float stepU = phaseCycle - std::floor(phaseCycle); // [0.0, 1.0)

            const float kAerialRatio = 0.46f; // 46%: 足を上げて横・前方へ踏み出す空中フェーズ / 54%: 地面を掻く接地フェーズ
            float stride = 0.0f;
            float liftCurve = 0.0f;
            float lateralWave = 0.0f;
            float stompDip = 0.0f;

            if (bossLegSwingSpeed_ > 0.0f) {
                if (stepU < kAerialRatio) {
                    // 1. 空中ステップフェーズ: 膝と足先を持ち上げながら、外側(横)へ張り出しつつ後方(-1)から前方(+1)へ脚を運ぶ
                    float su = stepU / kAerialRatio;
                    stride = -1.0f + 2.0f * (su * su * (3.0f - 2.0f * su));
                    liftCurve = std::pow(std::sin(su * static_cast<float>(M_PI)), 0.75f);
                    lateralWave = std::sin(su * static_cast<float>(M_PI));
                } else {
                    // 2. 地面踏みしめ(Stance)フェーズ: 足先を地面に着地させ、路面を横に掻きながら前方(+1)から後方(-1)へ送る
                    float su = (stepU - kAerialRatio) / (1.0f - kAerialRatio);
                    stride = 1.0f - 2.0f * su;
                    liftCurve = 0.0f;
                    lateralWave = -0.35f * std::sin(su * static_cast<float>(M_PI));
                    // 着地直後の力強い踏みしめ沈み込み
                    if (su < 0.25f) {
                        stompDip = -0.035f * std::sin((su / 0.25f) * static_cast<float>(M_PI));
                    }
                }
            }

            // 隣り合う足と絶対にぶつかって貫通しないよう、スイング角を安全な上限(0.26rad ≈ 15度)以内にクランプ
            float safeSwingRange = std::clamp(bossLegSwingRange_, 0.0f, 0.26f);
            float swing = stride * safeSwingRange;

            // 上下のステップ角と回転のみによって足を動かし、ソケット位置(付け根)自体は胴体内部から一切移動させないことで胴体と足の分離を防ぐ
            float maxLiftRad = bossLegLiftRange_ / (bossLegScale_ * 0.5f);
            float liftAngleRad = liftCurve * maxLiftRad + stompDip;
            float stepSocketLiftY = 0.0f;
            float stepLateralReach = 0.0f;

            // 蜘蛛胴体メッシュ(big+Spider.obj)のローカル座標系では +X が頭(前)、-X が腹(後)、+Z が右、-Z が左
            // そのため左足(i < 4)と右足(i >= 4)の真の左右対称は Z 座標の反転(-Z / +Z)と Y 回転の (180度 - baseRotY) になる
            Vector3 finalOffset = legPairPos[pairIdx];
            float baseRotY = legPairRotY[pairIdx];
            if (baseRotY < 0.0f) {
                baseRotY += 360.0f; // [-180, 180] を [0, 360] に正規化して左右対称(180 - baseRotY)を正確に保つ
            }

            Vector3 legRotate{};
            if (i < 4) {
                // 左足 (i = 0:第1前脚, 1:第2中前脚, 2:第3中後脚, 3:第4後脚)
                finalOffset.z = -finalOffset.z; // 左側 (-Z)
                
                // 通常の回転姿勢（+liftAngleRad で左足も膝・足先が上方向 +Y に持ち上がる）
                float normalRotY = (180.0f - baseRotY) * static_cast<float>(M_PI) / 180.0f + swing;
                float normalPitch = -0.06f + liftAngleRad;
                
                float currentRotY = normalRotY;
                float currentPitch = normalPitch;
                
                // 攻撃中の左前足(i==1)の軌道を補間
                if (i == 1 && attackLegIdx == 1 && bossActionState_ == BossActionState::kLegAttack) {
                    float targetX = 0.0f;
                    if (bossAttackTargetArea_ == 0) targetX = -30.0f;
                    else if (bossAttackTargetArea_ == 2) targetX = 30.0f;
                    Vector3 targetWorldPos = Add(playerRailPos, Scale(playerRailRight, targetX));
                    targetWorldPos.y = fighterWorldPos.y;

                    Vector3 diff = Subtract(targetWorldPos, bossPos);
                    Vector3 playerLocal = {
                        diff.x * R_final.m[0][0] + diff.y * R_final.m[0][1] + diff.z * R_final.m[0][2],
                        diff.x * R_final.m[1][0] + diff.y * R_final.m[1][1] + diff.z * R_final.m[1][2],
                        diff.x * R_final.m[2][0] + diff.y * R_final.m[2][1] + diff.z * R_final.m[2][2]
                    };
                    
                    Vector3 scaledJointLocal = Scale(finalOffset, bossScale_);
                    Vector3 dirLocal = Subtract(playerLocal, scaledJointLocal);
                    
                    float len = std::sqrt(dirLocal.x * dirLocal.x + dirLocal.y * dirLocal.y + dirLocal.z * dirLocal.z);
                    if (len > 0.01f) {
                        // 足モデル(先端 -Z)が dirLocal 方向を向く正確なヨー角
                        float targetRotY = std::atan2(dirLocal.x, -dirLocal.z);
                        float upPitch = 1.25f; // 天高く振りかぶる角度
                        float horizDist = std::sqrt(dirLocal.x * dirLocal.x + dirLocal.z * dirLocal.z);
                        float strikePitch = -0.15f;
                        if (horizDist > 0.01f) {
                            strikePitch = std::clamp(std::atan2(dirLocal.y, horizDist), -1.1f, 0.2f);
                        }
                        
                        float diffRotY = targetRotY - normalRotY;
                        while (diffRotY > static_cast<float>(M_PI)) diffRotY -= 2.0f * static_cast<float>(M_PI);
                        while (diffRotY < -static_cast<float>(M_PI)) diffRotY += 2.0f * static_cast<float>(M_PI);

                        if (bossActionTimer_ <= 5.0f) {
                            float t = bossActionTimer_ / 5.0f;
                            currentRotY = normalRotY + diffRotY * t;
                            currentPitch = std::lerp(normalPitch, upPitch, t);
                        } else if (bossActionTimer_ <= 5.3f) {
                            float t = (bossActionTimer_ - 5.0f) / 0.3f;
                            currentRotY = targetRotY;
                            currentPitch = std::lerp(upPitch, strikePitch, t);
                        } else if (bossActionTimer_ <= 6.8f) {
                            float t = (bossActionTimer_ - 5.3f) / 1.5f;
                            currentRotY = targetRotY - diffRotY * t;
                            currentPitch = std::lerp(strikePitch, normalPitch, t);
                        }
                    }
                }

                // 登場カメラ完了後の咆哮演出中 (9.8s ～ 11.2s) の前脚威嚇モーション
                if (bossAppearanceTimer_ > 0.0f) {
                    float t_appearance = kBossIntroTotalDuration - bossAppearanceTimer_;
                    if (t_appearance >= 9.8f && t_appearance < 11.2f) {
                        float roarProgress = std::clamp((t_appearance - 9.8f) / 1.4f, 0.0f, 1.0f);
                        float easeRoar = std::sin(roarProgress * static_cast<float>(M_PI));

                        if (i == 0 || i == 1) { // 左側の前脚2本：天高く持ち上げて威嚇
                            currentPitch = std::lerp(normalPitch, 1.35f, easeRoar);
                            currentRotY += std::sin(t_appearance * 80.0f) * 0.08f * easeRoar;
                        } else {
                            // 左側の後ろ脚2本：地面を力強く踏みしめる
                            currentPitch = std::lerp(normalPitch, -0.30f, easeRoar);
                        }
                    }
                }
                
                legRotate = { 
                    currentPitch, 
                    currentRotY, 
                    0.0f 
                };
            } else {
                // 右足 (i = 4:第1前脚, 5:第2中前脚, 6:第3中後脚, 7:第4後脚)
                // 通常の回転姿勢（+liftAngleRad で右足も膝・足先が上方向 +Y に持ち上がる）
                float normalRotY = baseRotY * static_cast<float>(M_PI) / 180.0f - swing;
                float normalPitch = -0.06f + liftAngleRad;
                
                float currentRotY = normalRotY;
                float currentPitch = normalPitch;
                
                // 攻撃中の右前足(i==5)の軌道を補間
                if (i == 5 && attackLegIdx == 5 && bossActionState_ == BossActionState::kLegAttack) {
                    float targetX = 0.0f;
                    if (bossAttackTargetArea_ == 0) targetX = -30.0f;
                    else if (bossAttackTargetArea_ == 2) targetX = 30.0f;
                    Vector3 targetWorldPos = Add(playerRailPos, Scale(playerRailRight, targetX));
                    targetWorldPos.y = fighterWorldPos.y;

                    Vector3 diff = Subtract(targetWorldPos, bossPos);
                    Vector3 playerLocal = {
                        diff.x * R_final.m[0][0] + diff.y * R_final.m[0][1] + diff.z * R_final.m[0][2],
                        diff.x * R_final.m[1][0] + diff.y * R_final.m[1][1] + diff.z * R_final.m[1][2],
                        diff.x * R_final.m[2][0] + diff.y * R_final.m[2][1] + diff.z * R_final.m[2][2]
                    };
                    
                    Vector3 scaledJointLocal = Scale(finalOffset, bossScale_);
                    Vector3 dirLocal = Subtract(playerLocal, scaledJointLocal);
                    
                    float len = std::sqrt(dirLocal.x * dirLocal.x + dirLocal.y * dirLocal.y + dirLocal.z * dirLocal.z);
                    if (len > 0.01f) {
                        // 足モデル(先端 -Z)が dirLocal 方向を向く正確なヨー角
                        float targetRotY = std::atan2(dirLocal.x, -dirLocal.z);
                        float upPitch = 1.25f; // 天高く振りかぶる角度
                        float horizDist = std::sqrt(dirLocal.x * dirLocal.x + dirLocal.z * dirLocal.z);
                        float strikePitch = -0.15f;
                        if (horizDist > 0.01f) {
                            strikePitch = std::clamp(std::atan2(dirLocal.y, horizDist), -1.1f, 0.2f);
                        }
                        
                        float diffRotY = targetRotY - normalRotY;
                        while (diffRotY > static_cast<float>(M_PI)) diffRotY -= 2.0f * static_cast<float>(M_PI);
                        while (diffRotY < -static_cast<float>(M_PI)) diffRotY += 2.0f * static_cast<float>(M_PI);

                        if (bossActionTimer_ <= 5.0f) {
                            float t = bossActionTimer_ / 5.0f;
                            currentRotY = normalRotY + diffRotY * t;
                            currentPitch = std::lerp(normalPitch, upPitch, t);
                        } else if (bossActionTimer_ <= 5.3f) {
                            float t = (bossActionTimer_ - 5.0f) / 0.3f;
                            currentRotY = targetRotY;
                            currentPitch = std::lerp(upPitch, strikePitch, t);
                        } else if (bossActionTimer_ <= 6.8f) {
                            float t = (bossActionTimer_ - 5.3f) / 1.5f;
                            currentRotY = targetRotY - diffRotY * t;
                            currentPitch = std::lerp(strikePitch, normalPitch, t);
                        }
                    }
                }

                // 登場カメラ完了後の咆哮演出中 (9.8s ～ 11.2s) の前脚威嚇モーション
                if (bossAppearanceTimer_ > 0.0f) {
                    float t_appearance = kBossIntroTotalDuration - bossAppearanceTimer_;
                    if (t_appearance >= 9.8f && t_appearance < 11.2f) {
                        float roarProgress = std::clamp((t_appearance - 9.8f) / 1.4f, 0.0f, 1.0f);
                        float easeRoar = std::sin(roarProgress * static_cast<float>(M_PI));

                        if (i == 4 || i == 5) { // 右側の前脚2本：天高く持ち上げて威嚇
                            currentPitch = std::lerp(normalPitch, 1.35f, easeRoar);
                            currentRotY += std::sin(t_appearance * 80.0f) * 0.08f * easeRoar;
                        } else {
                            // 右側の後ろ脚2本：地面を力強く踏みしめる
                            currentPitch = std::lerp(normalPitch, -0.30f, easeRoar);
                        }
                    }
                }
                
                legRotate = { 
                    currentPitch, 
                    currentRotY, 
                    0.0f 
                };
            }

            // 足モデル自体のスケール（独立した足専用スケールを適用）
            Vector3 legScale = { bossLegScale_, bossLegScale_, bossLegScale_ };

            // 現在の回転行列 (R) を作成
            Matrix4x4 mRotate = Multiply(Multiply(MakeRotateXMatrix(legRotate.x), MakeRotateYMatrix(legRotate.y)), MakeRotateZMatrix(legRotate.z));

            // スケールされたローカルピボット位置 (根本)
            Vector3 scaledPivot = { 0.0f, bossLegPivotY_ * bossLegScale_, bossLegPivotZ_ * bossLegScale_ };
            Vector4 pivotV4 = { scaledPivot.x, scaledPivot.y, scaledPivot.z, 1.0f };

            // 回転後のピボット変位を相殺し、足の付け根(scaledPivot)が頭胸部のソケット座標に固定されつつ、ステップに合わせて横方向へも伸縮するようにする
            Vector4 rotV4 = Multiply(pivotV4, mRotate);
            Vector3 finalJointPos = Scale(finalOffset, bossScale_);
            finalJointPos.x -= rotV4.x;
            finalJointPos.y += stepSocketLiftY - rotV4.y;
            finalJointPos.z += ((i < 4) ? -stepLateralReach : stepLateralReach) - rotV4.z;

            // 以前の正常な配置方式をベースにしつつ、ズレを打ち消した座標を適用
            Matrix4x4 legLocal = MakeAffineMatrix(legScale, legRotate, finalJointPos);

            // スケールなしの胴体行列と合成することで、胴体スケール変更が足の大きさに影響するのを防止
            Matrix4x4 legWorld = Multiply(legLocal, bossWorldNoScale);

            bossLegTransformData_[i]->World = legWorld;
        }
    }

    // 画面シェイク（カメラの振動）の適用
    Vector3 originalCamTranslate = camera_->GetTransform().translate;
    camera_->SetTranslate(Add(originalCamTranslate, cameraShakeOffset_));

    camera_->Update();
    Matrix4x4 viewProjectionMatrix = camera_->GetViewProjectionMatrix();

    camera_->SetTranslate(originalCamTranslate); // 描画後に座標を戻す

    // 蜘蛛ボスのWVP行列更新 (最新のカメラ行列を使用)
    if (bossBodyTransformData_) {
        bossBodyTransformData_->WVP = Multiply(bossBodyTransformData_->World, viewProjectionMatrix);
        for (int i = 0; i < 8; ++i) {
            if (bossLegTransformData_[i]) {
                bossLegTransformData_[i]->WVP = Multiply(bossLegTransformData_[i]->World, viewProjectionMatrix);
            }
        }
    }

    // 地球・日本・東京の夜景 3DモデルのWVP行列更新
    if (earthTransformData_) {
        earthTransformData_->WVP = Multiply(earthTransformData_->World, viewProjectionMatrix);
    }
    if (japanTransformData_) {
        japanTransformData_->WVP = Multiply(japanTransformData_->World, viewProjectionMatrix);
    }
    if (tokyoTransformData_) {
        tokyoTransformData_->WVP = Multiply(tokyoTransformData_->World, viewProjectionMatrix);
    }

    // ── 天球(SkyDome)のワールド・WVP行列を計算 ──
    // ビュー行列から平行移動成分をゼロにすることで「常に無限遠に見える」真のスカイドームを実現
    if (skydomeModel_ && skydomeTransformData_) {
        Vector3 skyScale  = { kSkydomeScale, kSkydomeScale, kSkydomeScale };
        Vector3 skyRotate = { 0.0f, 0.0f, 0.0f };
        Vector3 skyPos    = { 0.0f, 0.0f, 0.0f }; // 原点固定（ビューの平行移動除去で常にカメラ中心になる）

        // ビュー行列の平行移動成分をゼロに除去し、回転のみ残した ViewProjection を作成
        Matrix4x4 skyViewMatrix = camera_->GetViewMatrix();
        skyViewMatrix.m[3][0] = 0.0f;
        skyViewMatrix.m[3][1] = 0.0f;
        skyViewMatrix.m[3][2] = 0.0f;
        Matrix4x4 skyViewProjMatrix = Multiply(skyViewMatrix, camera_->GetProjectionMatrix());

        Matrix4x4 skyWorld = MakeAffineMatrix(skyScale, skyRotate, skyPos);
        skydomeTransformData_->World = skyWorld;
        skydomeTransformData_->WVP   = Multiply(skyWorld, skyViewProjMatrix);

        // UVスケールを適用してテクスチャを細かくタイリングする
        // これにより、星や雲が小さく（＝より遠くに）見え、テクスチャの荒さが大幅に改善されます
        float uvScale = 4.0f; 
        Matrix4x4 uvTranslate = MakeScaleMatrix({ uvScale, uvScale, 1.0f });

        // 天球マテリアルのUVトランスフォームを更新
        ID3D12Resource* materialRes = skydomeModel_->GetMaterialResource();
        if (materialRes) {
            ::Material* materialData = nullptr;
            materialRes->Map(0, nullptr, reinterpret_cast<void**>(&materialData));
            if (materialData) {
                materialData->uvTransform = uvTranslate;
            }
            materialRes->Unmap(0, nullptr);
        }
    }

    // 敵キャラのワールド行列・WVPの更新
    EulerTransform& camTransForEnemy = camera_->GetTransform();

    // 各グループの画面外判定とリポップチェック
    if (currentPhase_ != GamePhase::kBossFight) {
        // 自機の現在位置を取得
        Vector3 fighterWorldPos = { 0.0f, 0.0f, 0.0f };
        if (fighterModel_) {
            Vector3 playerRailPos = GetRailPosition(fighterWorldZ_);
            Vector3 playerRailDir = GetRailDirection(fighterWorldZ_);
            Vector3 playerRailRight = CalculateRailRight(playerRailDir);
            Vector3 playerRailUp = CalculateRailUp(playerRailDir, playerRailRight);
            float playerOffsetUp = fighterModel_->transform.translate.y + 17.0f;
            fighterWorldPos = Add(playerRailPos, Add(Scale(playerRailRight, fighterModel_->transform.translate.x), Scale(playerRailUp, playerOffsetUp)));
        }

        // グループ中心Zに基づく画面外判定と強制リポップ処理は、一括全滅リポップ制御に移行したため削除しました

        bool isPhaseIntroActive = (phaseIntroTimer_ >= 0.0f) || isWaitingForTransition;

        // ── 雑魚敵の移動・状態更新 ──
        for (int i = 0; i < kMaxEnemies; ++i) {
            if (!enemies_[i].isAlive) continue;

            Enemy& enemy = enemies_[i];
            
            // フェーズ演出中（isPhaseIntroActive == true）は敵の動きやタイマー更新をストップ
            if (!isPhaseIntroActive) {
                enemy.stateTimer += kDeltaTime;

                if (enemy.state == Enemy::State::kSideWait) {
                    // PHASE 2前およびボス戦前の残り4.5秒以内は、直後の演出でパッと消えてしまうのを防ぐため新しい雑魚敵が飛び出してこないようにする
                    bool isNearPhaseEnd = (kPhaseDuration - phaseTimer_) <= 4.5f;
                    // 出現判定: レール累積距離でプレイヤーより前方300m以内に入ったら出現開始
                    float distAlongRail = enemy.railProgress - fighterWorldZ_;
                    if (!isNearPhaseEnd && distAlongRail > 0.0f && distAlongRail < 300.0f) {
                        enemy.state = Enemy::State::kAppear;
                        enemy.stateTimer = 0.0f;
                        // 出現時のレール相対距離を記録 (最大120m)
                        enemy.relativeZ = (std::min)(120.0f, distAlongRail);
                        // 出現開始時の位置を記憶
                        enemy.appearStartPos = enemy.position;
                    } else if (isNearPhaseEnd) {
                        // フェーズ終了間際にまだ待機中の敵はそのまま非アクティブ化する
                        enemy.isAlive = false;
                    }
                }
                else if (enemy.state == Enemy::State::kAppear) {
                    // 2. 中央へ移動: 待機位置からフォーメーション目標位置(wanderAnchor)に向けてイージング＋カーブで合流
                    // wanderAnchorのレール累積距離を更新（プレイヤーとの相対累積距離を保持）
                    enemy.wanderAnchorRailProgress = fighterWorldZ_ + enemy.relativeZ;
                    // レール空間からワールド座標を再計算
                    {
                        Vector3 aRailPos   = GetRailPosition(enemy.wanderAnchorRailProgress);
                        Vector3 aRailDir   = GetRailDirection(enemy.wanderAnchorRailProgress);
                        Vector3 aRailRight = CalculateRailRight(aRailDir);
                        Vector3 aRailUp    = CalculateRailUp(aRailDir, aRailRight);
                        enemy.wanderAnchor = Add(aRailPos, Add(Scale(aRailRight, enemy.wanderAnchorRelX), Scale(aRailUp, enemy.wanderAnchorRelY)));
                    }

                    float kAppearDuration = 1.2f;
                    float t = std::clamp(enemy.stateTimer / kAppearDuration, 0.0f, 1.0f);
                    
                    // EaseOutQuad による滑らかな合流
                    float rate = 1.0f - (1.0f - t) * (1.0f - t);

                    // Z軸は徐々にプレイヤーとのキープ相対Zに近づける
                    float currentZ = std::lerp(enemy.appearStartPos.z, enemy.wanderAnchor.z, rate);
                    
                    // X軸は滑らかに目標フォーメーション位置へ
                    float currentX = std::lerp(enemy.appearStartPos.x, enemy.wanderAnchor.x, rate);

                    // Y軸は「上へ膨らむ山なりのカーブ」を加えてビルから飛び出す挙動にする
                    float arcHeight = 15.0f; // 飛び出しの最高到達高度
                    float heightOffset = arcHeight * std::sin(t * static_cast<float>(M_PI));
                    float currentY = std::lerp(enemy.appearStartPos.y, enemy.wanderAnchor.y, rate) + heightOffset;

                    enemy.position = { currentX, currentY, currentZ };

                    // 飛び出す方向（左から右か、右から左か）に応じて機体をローリング（傾き）させる演出
                    float rollDir = (enemy.appearStartPos.x < enemy.wanderAnchor.x) ? -0.8f : 0.8f; // 約45度傾く
                    enemy.rotate.z = rollDir * std::sin(t * static_cast<float>(M_PI)); // 合流完了で水平に戻る
                    
                    // 進行方向を向くヨー（rotate.y）
                    float yawDir = (enemy.appearStartPos.x < enemy.wanderAnchor.x) ? 0.4f : -0.4f;
                    enemy.rotate.y = yawDir * (1.0f - rate); // 合流完了で正面に戻る

                    if (t >= 1.0f) {
                        enemy.position = enemy.wanderAnchor;
                        enemy.rotate = { 0.0f, 0.0f, 0.0f }; // 回転をリセット
                        enemy.state = Enemy::State::kWander;
                        enemy.stateTimer = 0.0f;
                        enemy.wanderPhase = (float)(rand() % 100) / 10.0f;
                    }
                }
                else if (enemy.state == Enemy::State::kWander) {
                    // 3. 徘徊しながら後退: wanderAnchor の周囲を動きつつ、全体がZ軸手前(プレイヤー側)へ下がっていく
                    enemy.wanderPhase += kDeltaTime * 2.5f;
                    
                    // 相対Z距離を徐々に減らす (プレイヤーへ向けてゆっくり近づいてくる＝下がっていくように見せる)
                    // 秒速 15m で距離が縮まる (3秒で 45m 接近)
                    enemy.relativeZ -= 15.0f * kDeltaTime;
                    enemy.wanderAnchorRailProgress = fighterWorldZ_ + enemy.relativeZ;
                    // レール空間からwanderAnchorワールド座標を再計算
                    {
                        Vector3 wRailPos   = GetRailPosition(enemy.wanderAnchorRailProgress);
                        Vector3 wRailDir   = GetRailDirection(enemy.wanderAnchorRailProgress);
                        Vector3 wRailRight = CalculateRailRight(wRailDir);
                        Vector3 wRailUp    = CalculateRailUp(wRailDir, wRailRight);
                        enemy.wanderAnchor = Add(wRailPos, Add(Scale(wRailRight, enemy.wanderAnchorRelX), Scale(wRailUp, enemy.wanderAnchorRelY)));
                    }
                    
                    float radiusX = 6.0f;
                    float radiusY = 4.0f;
                    enemy.position.x = enemy.wanderAnchor.x + std::cos(enemy.wanderPhase) * radiusX;
                    enemy.position.y = enemy.wanderAnchor.y + std::sin(enemy.wanderPhase * 1.3f) * radiusY;
                    enemy.position.z = enemy.wanderAnchor.z + std::sin(enemy.wanderPhase * 0.7f) * 3.0f;

                    // 中央出現から一定時間 (3.0秒) 経過したら特攻開始（フェーズ終了直前は早めに突進して画面内から離脱させる）
                    float diveTriggerTime = ((kPhaseDuration - phaseTimer_) <= 2.2f) ? 0.5f : 3.0f;
                    if (enemy.stateTimer >= diveTriggerTime) {
                        enemy.state = Enemy::State::kDive;
                        enemy.stateTimer = 0.0f;
                        // 自機位置に向けて特攻方向を計算 (Z追従を解除してその瞬間の位置へ突進)
                        Vector3 playerTarget = fighterWorldPos;
                        Vector3 toPlayer = Subtract(playerTarget, enemy.position);

                        enemy.diveDirection = Normalize(toPlayer);
                        enemy.speed = 85.0f + boostForwardSpeed_; // プレイヤーの速度に合わせて特攻速度を上げる (ブースト対応)
                    }
                }
                else if (enemy.state == Enemy::State::kDive) {
                    // 4. 特攻状態: プレイヤーに向けて直線的に高速突進 (Z追従なし)
                    enemy.position = Add(enemy.position, Scale(enemy.diveDirection, enemy.speed * kDeltaTime));
                    
                    // 特攻中の回転
                    enemy.rotate.z += kDeltaTime * 18.0f;
                    enemy.rotate.x += kDeltaTime * 6.0f;
                    enemy.rotate.y += kDeltaTime * 4.0f;
                    
                    // プレイヤーを通り過ぎてレール後方20m超えたら非生存化してリポップ対象に
                    {
                        Vector3 localRailDir = GetRailDirection(fighterWorldZ_);
                        Vector3 toFighterRail = Subtract(enemy.position, fighterWorldPos);
                        float behindDot = toFighterRail.x * (-localRailDir.x) + toFighterRail.y * (-localRailDir.y) + toFighterRail.z * (-localRailDir.z);
                        if (behindDot > 20.0f) {
                            enemy.isAlive = false;
                        }
                    }
                }
            }
        }

        // ── 雑魚敵のグループごと個別全滅時のリポップ制御 ──
        // PHASE 2前およびボス戦前の残り4.5秒以内は新しい雑魚敵をリポップさせない（演出開始時に出てきてすぐ消えるのを防止）
        bool canRespawnEnemies = ((kPhaseDuration - phaseTimer_) > 4.5f);
        if (currentPhase_ != GamePhase::kBossFight && !isPhaseIntroActive && canRespawnEnemies) {
            for (int g = 0; g < kNumGroups; ++g) {
                bool anyEnemyAliveInGroup = false;
                for (int idx = 0; idx < kEnemiesPerGroup; ++idx) {
                    int enemyIdx = g * kEnemiesPerGroup + idx;
                    if (enemies_[enemyIdx].isAlive) {
                        anyEnemyAliveInGroup = true;
                        break;
                    }
                }
                // そのグループの敵が1体もいなくなったら、そのグループを前方にリポップする
                if (!anyEnemyAliveInGroup) {
                    RespawnEnemyGroup(g, fighterWorldZ_);
                }
            }
        }

        for (int i = 0; i < kMaxEnemies; ++i) {
            if (enemies_[i].isAlive) {
                // 敵の進行方向の決定 (特攻中は特攻方向、それ以外はレール進行方向の逆＝プレイヤーと対面する方向)
                Vector3 eDir = {0.0f, 0.0f, -1.0f};
                if (enemies_[i].state == Enemy::State::kDive) {
                    eDir = enemies_[i].diveDirection;
                } else {
                    eDir = Scale(GetRailDirection(enemies_[i].wanderAnchorRailProgress), -1.0f);
                }
                
                float eDirLen = std::sqrt(eDir.x * eDir.x + eDir.y * eDir.y + eDir.z * eDir.z);
                if (eDirLen > 0.001f) {
                    eDir = Scale(eDir, 1.0f / eDirLen);
                } else {
                    eDir = {0.0f, 0.0f, -1.0f};
                }

                // モデルの初期姿勢が後ろ向きのため、正面(Zマイナス)をeDirに向かせるための基準姿勢を構築
                Vector3 d = eDir;
                Vector3 r = CalculateRailRight(d);
                Vector3 u = CalculateRailUp(d, r);

                Matrix4x4 R_enemy_base = MakeIdentity4x4();
                R_enemy_base.m[0][0] = r.x; R_enemy_base.m[0][1] = r.y; R_enemy_base.m[0][2] = r.z;
                R_enemy_base.m[1][0] = u.x; R_enemy_base.m[1][1] = u.y; R_enemy_base.m[1][2] = u.z;
                R_enemy_base.m[2][0] = d.x; R_enemy_base.m[2][1] = d.y; R_enemy_base.m[2][2] = d.z;

                // 180度反転して前を向かせる回転
                Matrix4x4 rotateY = MakeRotateYMatrix(static_cast<float>(M_PI));

                // 敵のローカル回転 (合流ローリングや特攻時スピンなど)
                Matrix4x4 rotateX_e = MakeRotateXMatrix(enemies_[i].rotate.x);
                Matrix4x4 rotateY_e = MakeRotateYMatrix(enemies_[i].rotate.y);
                Matrix4x4 rotateZ_e = MakeRotateZMatrix(enemies_[i].rotate.z);
                Matrix4x4 R_local = Multiply(Multiply(Multiply(rotateZ_e, rotateX_e), rotateY_e), rotateY);

                // ワールド行列の組み立て
                Matrix4x4 R_final = Multiply(R_local, R_enemy_base);
                Matrix4x4 scaleMatrix = MakeScaleMatrix(enemies_[i].scale);
                Matrix4x4 translateMatrix = MakeTranslateMatrix(enemies_[i].position);
                Matrix4x4 worldMatrix = Multiply(Multiply(scaleMatrix, R_final), translateMatrix);

                enemyTransformData_[i]->World = worldMatrix;
                enemyTransformData_[i]->WVP = Multiply(worldMatrix, viewProjectionMatrix);
            } else {
                // 撃破された敵は非表示にする
                enemyTransformData_[i]->World = MakeIdentity4x4();
                enemyTransformData_[i]->WVP = MakeIdentity4x4();
            }
        }
    } else {
        // ボス戦フェーズではすべての雑魚敵を完全に非表示にする
        for (int i = 0; i < kMaxEnemies; ++i) {
            enemyTransformData_[i]->World = MakeIdentity4x4();
            enemyTransformData_[i]->WVP = MakeIdentity4x4();
        }
    }

    // 戦闘機とエイミングのWVPの更新
    if (fighterModel_ && fighterTransformData_) {
        fighterTransformData_->WVP = Multiply(fighterTransformData_->World, viewProjectionMatrix);
    }
    if (sceneMode_ == SceneMode::kFighter) {
        if (aimingInstancingData_) {
            aimingInstancingData_->WVP = Multiply(aimingInstancingData_->World, viewProjectionMatrix);
            aimingInstancingData_->color = { 1.0f, 1.0f, 1.0f, 1.0f };
            aimingInstancingData_->uvTransform = MakeIdentity4x4();
        }
    }

    // 弾の描画用行列の更新（スター・ウォーズ風レーザー状に進行方向へ正しく向かせる）
    for (int i = 0; i < kMaxBullets; ++i) {
        if (playerBullets_[i].currentTime < playerBullets_[i].lifeTime) {
            Vector3 vel = playerBullets_[i].velocity;
            Vector3 dir = Normalize(vel);
            Matrix4x4 rotMatrix = MakeLookRotationMatrix(dir);
            Vector3 scaleOuter = { 2.4f, 2.4f, 4.8f };

            Matrix4x4 bulletWorld = Multiply(Multiply(MakeScaleMatrix(scaleOuter), rotMatrix), MakeTranslateMatrix(playerBullets_[i].position));
            bulletTransformData_[i]->World = bulletWorld;
            bulletTransformData_[i]->WVP = Multiply(bulletWorld, viewProjectionMatrix);
        } else {
            bulletTransformData_[i]->World = MakeIdentity4x4();
            bulletTransformData_[i]->WVP = MakeIdentity4x4();
        }
    }

    // トドメ巨大弾のWVPの更新
    if (defeatBulletTransformData_) {
        if (isDefeatBulletActive_) {
            Matrix4x4 bulletWorld = MakeAffineMatrix(
                Vector3{ defeatBulletSize_, defeatBulletSize_, defeatBulletSize_ },
                Vector3{ 0.0f, 0.0f, 0.0f },
                defeatBulletPos_
            );
            defeatBulletTransformData_->World = bulletWorld;
            defeatBulletTransformData_->WVP = Multiply(bulletWorld, viewProjectionMatrix);
        } else {
            defeatBulletTransformData_->World = MakeIdentity4x4();
            defeatBulletTransformData_->WVP = MakeIdentity4x4();
        }
    }

    if (playerModel_) {
        // アニメーションの更新 (DeltaTimeは固定60fpsとする)
        playerModel_->UpdateAnimation(kDeltaTime);
        playerModel_->Update();

        // モデル自体のトランスフォームを適用したWVP/Worldを計算
        Matrix4x4 modelWorldMatrix = MakeAffineMatrix(playerModel_->transform.scale, playerModel_->transform.rotate, playerModel_->transform.translate);
        
        transformData_->WVP = Multiply(modelWorldMatrix, viewProjectionMatrix);
        transformData_->World = modelWorldMatrix;

        if (cameraDataCB_) {
            cameraDataCB_->worldPosition = camTrans.translate;
        }
    }

    // simpleSkinのスケルトンアニメーション更新
    cubeAnimationTime_ += kDeltaTime;
    cubeAnimationTime_ = std::fmod(cubeAnimationTime_, cubeAnimation_.duration);

    // 1. アニメーションをスケルトンに適用
    AdvAnim::ApplyAnimation(cubeSkeleton_, cubeAnimation_, cubeAnimationTime_);
    // 2. スケルトンの階層行列を更新
    AdvAnim::Update(cubeSkeleton_);
    // 3. スキニングクラスターを更新
    AdvAnim::Update(cubeSkinCluster_, cubeSkeleton_);

    // 描画用のワールド行列を計算（スケルトン全体を移動させるための行列）
    cubeRenderModel_->transform.translate = Vector3{ 20.0f, 0.0f, 0.0f };
    cubeRenderModel_->transform.scale = Vector3{ 1.0f, 1.0f, 1.0f };
    Matrix4x4 cubeBaseWorldMatrix = MakeAffineMatrix(cubeRenderModel_->transform.scale, cubeRenderModel_->transform.rotate, cubeRenderModel_->transform.translate);

    // 資料の指示により、スキニングモデルのWVPにはルートジョイントの行列を含めない
    cubeTransformData_->WVP = Multiply(cubeBaseWorldMatrix, viewProjectionMatrix);
    cubeTransformData_->World = cubeBaseWorldMatrix;

    // AnimatedCubeのアニメーション更新
    animatedCubeAnimationTime_ += kDeltaTime;
    animatedCubeAnimationTime_ = std::fmod(animatedCubeAnimationTime_, animatedCubeAnimation_.duration);

    animatedCubeRenderModel_->transform.rotate.y += 0.02f;
    AdvAnim::ApplyAnimation(animatedCubeSkeleton_, animatedCubeAnimation_, animatedCubeAnimationTime_);
    AdvAnim::Update(animatedCubeSkeleton_);

    if (animatedCubeRenderModel_ && animatedCubeRenderModel_->skinningData_) {
        for (const auto& joint : animatedCubeSkeleton_.joints) {
            if (animatedCubeRenderModel_->bones_.count(joint.name)) {
                const auto& bone = animatedCubeRenderModel_->bones_.at(joint.name);
                animatedCubeRenderModel_->skinningData_->boneMatrices[bone.index] = Multiply(bone.offsetMatrix, joint.skeletonSpaceMatrix);
            }
        }
    }

    animatedCubeRenderModel_->transform.translate = Vector3{ -20.0f, 0.0f, 0.0f }; // 左側にずらす
    animatedCubeRenderModel_->transform.scale = Vector3{ 1.0f, 1.0f, 1.0f };
    Matrix4x4 animatedCubeBaseWorldMatrix = MakeAffineMatrix(animatedCubeRenderModel_->transform.scale, animatedCubeRenderModel_->transform.rotate, animatedCubeRenderModel_->transform.translate);

    animatedCubeTransformData_->WVP = Multiply(animatedCubeBaseWorldMatrix, viewProjectionMatrix);
    animatedCubeTransformData_->World = animatedCubeBaseWorldMatrix;

    // ── ビルと床(Plane)の更新・再配置（オブジェクトプール） ──
    // ボス戦中もビルの物理シミュレーション・衝突判定・行列計算を更新する。
    if (sceneMode_ == SceneMode::kFighter) {
        bool isBossIntro = (bossAppearanceTimer_ > 0.0f);
        float cameraZ = (isBossIntro || isTitleMode_) ? (fighterWorldZ_ - 65.0f) : camera_->GetTransform().translate.z;

        // ビルの画面外再配置（自動直進コースのときのみオブジェクトプールによる再配置を行う）
        if (waypoints_.empty()) {
            for (size_t i = 0; i < buildings_.size(); ++i) {
                // カメラの後方を超えたら再配置（タイトル時は視界外になるよう判定境界を遥か後方に下げる）
                float cullDist = isTitleMode_ ? 800.0f : kBuildingInterval;
                if (buildings_[i].position.z < cameraZ - cullDist) {
                    buildings_[i].position.z += (float)kMaxBuildings * kBuildingInterval * 0.25f;
                    
                    // 破壊されているビルを元のきれいな状態にリセット
                    buildings_[i].position.x = buildings_[i].originalX;
                    buildings_[i].position.y = buildings_[i].originalY;
                    buildings_[i].rotate = { 0.0f, 0.0f, 0.0f };
                    buildings_[i].isDestroyed = false;
                    buildings_[i].velocity = { 0.0f, 0.0f, 0.0f };
                    buildings_[i].rotationSpeed = { 0.0f, 0.0f, 0.0f };
                    buildings_[i].destroyTimer = 0.0f;

                    buildings_[i].floors = buildings_[i].originalFloors;
                    buildings_[i].isWorldMatrixCached = false;
                }
            }
        }

        // ── ビルの物理シミュレーションとボス衝突判定 ──
        {
            Vector3 bossPos = { 0.0f, 0.0f, 0.0f };
            Vector3 bRailRight = { 1.0f, 0.0f, 0.0f };
            Vector3 bRailDir = { 0.0f, 0.0f, 1.0f };
            bool isBossActive = (!isTitleMode_ && !isBossDefeatedSequence_ && (bossAppearanceTimer_ > 0.0f || currentPhase_ == GamePhase::kBossFight));
            if (isBossActive) {
                float bodyBounce = std::sin(bossTime_ * 2.0f) * bossBodyBounceRange_;
                bossPos = GetBossPosition(bodyBounce, 0.0f, 0.0f); // 正しいレール空間のワールド座標を取得

                float bossProgress = fighterWorldZ_ + bossZOffset_;
                bRailDir = GetRailDirection(bossProgress);
                bRailRight = CalculateRailRight(bRailDir);
            }

            // ボス戦開始時のビル累積距離の一括初期化（自動直進コース waypoints_.empty() のときのみ実行し、都市コースでは既存の街並み座標を維持する）
            if (waypoints_.empty() && currentPhase_ == GamePhase::kBossFight && !isBossFightInitialized_) {
                isBossFightInitialized_ = true;
                float L = 2000.0f; // ループの全体長
                for (size_t i = 0; i < buildings_.size(); ++i) {
                    float relativeS = std::fmod(static_cast<float>(i) * 12.5f, L);
                    buildings_[i].railProgress = fighterWorldZ_ - 200.0f + relativeS;
                    
                    // 初期座標を計算して配置
                    Vector3 railPos = GetRailPosition(buildings_[i].railProgress);
                    Vector3 railDir = GetRailDirection(buildings_[i].railProgress);
                    Vector3 railRight = CalculateRailRight(railDir);
                    
                    float offsetX = 45.0f;
                    int lane = i % 4;
                    if (lane == 0) offsetX = -85.0f;
                    else if (lane == 1) offsetX = -45.0f;
                    else if (lane == 2) offsetX = 45.0f;
                    else offsetX = 85.0f;
                    
                    float noiseX = std::sin(static_cast<float>(i) * 0.5f) * 3.0f;
                    float noiseZ = std::cos(static_cast<float>(i) * 0.3f) * 3.0f;
                    
                    Vector3 targetPos = Add(railPos, Scale(railRight, offsetX + noiseX));
                    targetPos.y = -20.0f;
                    targetPos = Add(targetPos, Scale(railDir, noiseZ));
                    
                    buildings_[i].position = targetPos;
                    float rotY = std::atan2(railDir.x, railDir.z);
                    buildings_[i].rotate = { 0.0f, rotY, 0.0f };
                    
                    buildings_[i].isDestroyed = false;
                    buildings_[i].velocity = { 0.0f, 0.0f, 0.0f };
                    buildings_[i].rotationSpeed = { 0.0f, 0.0f, 0.0f };
                    buildings_[i].destroyTimer = 0.0f;
                    
                    if (std::abs(offsetX) < 60.0f) {
                        buildings_[i].floors = 3 + (i % 6);
                    } else {
                        buildings_[i].floors = 6 + (i % 7);
                    }
                    buildings_[i].originalFloors = buildings_[i].floors;
                    buildings_[i].isWorldMatrixCached = false;
                }
            }

            for (size_t i = 0; i < buildings_.size(); ++i) {
                // ── ボス戦中（自動直進コース時のみ）のビル位置ループ再配置 ──
                if (waypoints_.empty() && currentPhase_ == GamePhase::kBossFight) {
                    float L = 2000.0f; // ループの全体長
                    // プレイヤーの後方に流れたビルを前方へワープ
                    if (buildings_[i].railProgress < fighterWorldZ_ - 200.0f) {
                        buildings_[i].railProgress += L;
                        
                        // 破壊状態のリセット
                        buildings_[i].isDestroyed = false;
                        buildings_[i].velocity = { 0.0f, 0.0f, 0.0f };
                        buildings_[i].rotationSpeed = { 0.0f, 0.0f, 0.0f };
                        buildings_[i].destroyTimer = 0.0f;
                        
                        float offsetX = 45.0f;
                        int lane = i % 4;
                        if (lane == 0) offsetX = -85.0f;
                        else if (lane == 1) offsetX = -45.0f;
                        else if (lane == 2) offsetX = 45.0f;
                        else offsetX = 85.0f;
                        
                        if (std::abs(offsetX) < 60.0f) {
                            buildings_[i].floors = 3 + (i % 6);
                        } else {
                            buildings_[i].floors = 6 + (i % 7);
                        }
                        buildings_[i].originalFloors = buildings_[i].floors;
                        buildings_[i].isWorldMatrixCached = false;
                    }
                    
                    // 破壊されていないときのみ、現在設定されている railProgress の静止座標に固定する
                    // （破壊されているときは、物理シミュレーションによる吹き飛び移動を優先する）
                    if (!buildings_[i].isDestroyed) {
                        Vector3 railPos = GetRailPosition(buildings_[i].railProgress);
                        Vector3 railDir = GetRailDirection(buildings_[i].railProgress);
                        Vector3 railRight = CalculateRailRight(railDir);
                        
                        float offsetX = 45.0f;
                        int lane = i % 4;
                        if (lane == 0) offsetX = -85.0f;
                        else if (lane == 1) offsetX = -45.0f;
                        else if (lane == 2) offsetX = 45.0f;
                        else offsetX = 85.0f;
                        
                        float noiseX = std::sin(static_cast<float>(i) * 0.5f) * 3.0f;
                        float noiseZ = std::cos(static_cast<float>(i) * 0.3f) * 3.0f;
                        
                        Vector3 targetPos = Add(railPos, Scale(railRight, offsetX + noiseX));
                        targetPos.y = -20.0f;
                        targetPos = Add(targetPos, Scale(railDir, noiseZ));
                        
                        buildings_[i].position = targetPos;
                        float rotY = std::atan2(railDir.x, railDir.z);
                        buildings_[i].rotate = { 0.0f, rotY, 0.0f };
                    }
                }

                if (buildings_[i].isDestroyed) {
                    // すでに破壊されているビルの物理更新
                    buildings_[i].destroyTimer += kDeltaTime;

                    // 速度に従って移動
                    buildings_[i].position.x += buildings_[i].velocity.x * kDeltaTime;
                    buildings_[i].position.y += buildings_[i].velocity.y * kDeltaTime;
                    buildings_[i].position.z += buildings_[i].velocity.z * kDeltaTime;

                    // 重力を適用（落下）
                    buildings_[i].velocity.y -= 9.8f * 5.0f * kDeltaTime;

                    // 回転の更新
                    buildings_[i].rotate.x += buildings_[i].rotationSpeed.x * kDeltaTime;
                    buildings_[i].rotate.y += buildings_[i].rotationSpeed.y * kDeltaTime;
                    buildings_[i].rotate.z += buildings_[i].rotationSpeed.z * kDeltaTime;
                } else {
                    // ボスとの衝突判定（レールローカル空間での3D判定を行い、道路脇のビル誤爆を防ぐ）
                    if (isBossActive) {
                        Vector3 diff = Subtract(buildings_[i].position, bossPos);
                        diff.y = 0.0f; // 高低差を無視して判定を行う
                        float localX = diff.x * bRailRight.x + diff.y * bRailRight.y + diff.z * bRailRight.z; // 左右方向（道路幅方向）
                        float localZ = diff.x * bRailDir.x   + diff.y * bRailDir.y   + diff.z * bRailDir.z;   // 前後方向（進行方向）

                        // 進行方向（前後）に30m、道路幅（左右）に55mの範囲にあるビルを体当たり破壊（左右の外側二列目 X=±85m を誤爆から保護）
                        if (std::abs(localZ) < 30.0f && std::abs(localX) < 55.0f) {
                            buildings_[i].isDestroyed = true;
                            buildings_[i].destroyTimer = 0.0f;

                            // 吹き飛び方向
                            float signX = (localX >= 0.0f) ? 1.0f : -1.0f;

                            if (i % 2 == 0) {
                                // なぎ倒れタイプ (回転中心は底面。外側へ倒れるように回転符号を修正)
                                buildings_[i].velocity = { signX * 25.0f, 15.0f, 10.0f };
                                buildings_[i].rotationSpeed = { 0.0f, 0.0f, signX * 3.5f };
                            } else {
                                // 吹き飛びタイプ (上空に激しく吹き飛ぶ。外側へ回転するように符号を修正)
                                buildings_[i].velocity = { signX * 45.0f, 60.0f, 25.0f };
                                float rx = ((float)(randomEngine_() % 200) / 100.0f) - 1.0f;
                                float ry = ((float)(randomEngine_() % 200) / 100.0f) - 1.0f;
                                float rz = signX * (1.5f + ((float)(randomEngine_() % 100) / 100.0f));
                                buildings_[i].rotationSpeed = { rx * 3.0f, ry * 3.0f, rz * 3.0f };
                            }

                            // 衝突時エフェクトの発生
                            EmitHitEffect(buildings_[i].position);
                            if (particleManager_) {
                                particleManager_->EmitCustomSparks(buildings_[i].position, 25.0f, 15, { 1.0f, 0.5f, 0.1f }, 1.0f);
                                particleManager_->EmitFlame(buildings_[i].position, 15.0f, 10, { 1.0f, 0.3f, 0.0f });
                            }
                        }
                    }
                }
            }
        }

        // 各ビルのワールド行列とWVP行列を計算（距離カリング & 1棟1ドロー統合最適化）
        {
            int cbIndex = 0;
            bool isBossIntro = (bossAppearanceTimer_ > 0.0f);
            float t_app = kBossIntroTotalDuration - bossAppearanceTimer_;
            Vector3 refCamPos = isBossIntro
                ? ((t_app < 11.2f) ? GetBossPosition(0.0f, 0.0f) : camera_->GetTransform().translate)
                : (isTitleMode_ ? GetRailPosition((std::max)(0.0f, fighterWorldZ_ - 65.0f)) : camera_->GetTransform().translate);
            float camZ = refCamPos.z;
            float camX = refCamPos.x;

            // ボス登場演出中のカメラ位置・ボス位置を事前計算（公転旋回中にボスを遮るビルを半透明化する）
            Vector3 introCamPos = camera_->GetTransform().translate;
            Vector3 introBossPos = { 0.0f, 0.0f, 0.0f };
            if (isBossIntro) {
                introBossPos = GetBossPosition(0.0f, 0.0f);
            }

            for (size_t i = 0; i < buildings_.size(); ++i) {
                buildings_[i].cbIndexStart = -1;
                buildings_[i].cbCount = 0;

                if (buildings_[i].floors <= 0) continue;

                // ── 破壊されて1.0秒以上経過したビルは即時消去（カリング） ──
                if (buildings_[i].isDestroyed && buildings_[i].destroyTimer > 1.0f) {
                    continue;
                }

                float dz = buildings_[i].position.z - camZ;
                float dx = buildings_[i].position.x - camX;
                float distSq = dx * dx + dz * dz;

                // 2D全方位の距離カリング（ルートが横方向や逆方向に曲がっても視界内のビルが消えないようにする）
                if (distSq > kBuildingCullFarDist * kBuildingCullFarDist) {
                    continue;
                }

                // ── 破壊時およびボス登場カメラ演出中のビル透過率（alpha）を算出（詳細描画・統合描画の両方に適用） ──
                float alpha = 1.0f;
                if (buildings_[i].isDestroyed) {
                    // 1.0秒かけてアルファを 1.0 -> 0.0 に減少させる
                    alpha = 1.0f - std::clamp(buildings_[i].destroyTimer / 1.0f, 0.0f, 1.0f);
                    alpha = alpha * alpha; // イージング
                } else if (isBossIntro && t_app >= 7.2f && t_app < 11.5f) {
                    // カメラからボスへの視線コーン（手前側のみ）に現在重なってボスを隠しているビルだけをピンポイントでその瞬間だけ半透明にする
                    float bx = buildings_[i].position.x;
                    float bz = buildings_[i].position.z;
                    float cx = introCamPos.x;
                    float cy = introCamPos.y;
                    float cz = introCamPos.z;
                    float tx = introBossPos.x;
                    float ty = introBossPos.y;
                    float tz = introBossPos.z;

                    float abx = tx - cx;
                    float abz = tz - cz;
                    float apx = bx - cx;
                    float apz = bz - cz;
                    float abLenSq2D = abx * abx + abz * abz;

                    if (abLenSq2D > 0.01f) {
                        // u: カメラ位置(0.0) ～ ボス位置(1.0) の前後投影位置（1.0より奥の背景ビルや0.0より後ろのビルは一切透過させない）
                        float u = (apx * abx + apz * abz) / abLenSq2D;
                        if (u > -0.06f && u < 1.02f) {
                            float u_seg = std::clamp(u, 0.0f, 1.0f);
                            float nearX = cx + abx * u_seg;
                            float nearZ = cz + abz * u_seg;
                            float segDx = bx - nearX;
                            float segDz = bz - nearZ;
                            float perpDist = std::sqrt(segDx * segDx + segDz * segDz);

                            // 視線コーンの横幅（カメラ直近では22m、ボス本体＋8本脚付近では42m）
                            float coneRadius = std::lerp(22.0f, 42.0f, u_seg);
                            if (perpDist < coneRadius) {
                                // 高さの重なり判定（ビルの屋上がカメラ〜ボス間の視線ラインより下なら、視界を遮らないため透過しない）
                                float rayY = std::lerp(cy, ty, u_seg);
                                float vertMargin = std::lerp(8.0f, 22.0f, u_seg);
                                float bldgTopY = buildings_[i].position.y + (float)buildings_[i].floors * kFloorHeight;

                                if (bldgTopY >= rayY - vertMargin) {
                                    // 前後方向フェード（カメラ手前端・ボス直前で滑らかにイン・アウト）
                                    float alongWeight = 1.0f;
                                    if (u < 0.04f) {
                                        alongWeight = std::clamp((u - (-0.06f)) / 0.10f, 0.0f, 1.0f);
                                    } else if (u > 0.90f) {
                                        alongWeight = std::clamp((1.02f - u) / 0.12f, 0.0f, 1.0f);
                                    }

                                    // 横方向フェード（視線の中央に入った瞬間だけ透明になり、外れたらすぐ不透明へ戻る）
                                    float coreRadius = coneRadius * 0.58f;
                                    float perpWeight = 1.0f;
                                    if (perpDist > coreRadius) {
                                        float r = std::clamp((perpDist - coreRadius) / (coneRadius - coreRadius), 0.0f, 1.0f);
                                        perpWeight = 1.0f - r * r * (3.0f - 2.0f * r);
                                    }

                                    // 高さ方向フェード
                                    float vertWeight = std::clamp((bldgTopY - (rayY - vertMargin)) / 10.0f, 0.0f, 1.0f);

                                    float occludeWeight = alongWeight * perpWeight * vertWeight;
                                    alpha = std::lerp(1.0f, 0.12f, occludeWeight);
                                }
                            }
                        }
                    }
                }

                // プレイヤーが通るルート近傍および直進延長上のビル(isNearCourseColumn)は視界限界(kBuildingCullFarDist)まで常に詳細描画（荒くしない）
                bool isDetailView = buildings_[i].isNearCourseColumn ||
                                    buildings_[i].isDestroyed ||
                                    (distSq <= 120.0f * 120.0f);

                if (isDetailView) {
                    // 各階個別のマルチドロー（詳細描画）
                    buildings_[i].cbIndexStart = cbIndex;
                    int count = 0;

                    // ワールド行列の初期化キャッシュ（破壊されていない静的ビル用）
                    if (!buildings_[i].isWorldMatrixCached || (int)buildings_[i].cachedDetailWorldMatrices.size() != buildings_[i].floors) {
                        buildings_[i].cachedDetailWorldMatrices.resize(buildings_[i].floors);
                        for (int f = 0; f < buildings_[i].floors; ++f) {
                            Vector3 floorPos = buildings_[i].position;
                            floorPos.y = buildings_[i].position.y + (float)f * kFloorHeight + kFloorHeight * 0.5f;
                            buildings_[i].cachedDetailWorldMatrices[f] = MakeAffineMatrix(buildings_[i].scale, buildings_[i].rotate, floorPos);
                        }
                        Vector3 integratedScale = buildings_[i].scale;
                        integratedScale.y = kFloorHeight * (float)buildings_[i].floors;
                        Vector3 centerPos = buildings_[i].position;
                        centerPos.y = buildings_[i].position.y + integratedScale.y * 0.5f;
                        buildings_[i].cachedIntegratedWorldMatrix = MakeAffineMatrix(integratedScale, buildings_[i].rotate, centerPos);
                        buildings_[i].isWorldMatrixCached = true;
                    }

                    for (int f = 0; f < buildings_[i].floors; ++f) {
                        if (cbIndex >= kMaxBuildingCBs) break;

                        Matrix4x4 worldMatrix;
                        if (buildings_[i].isDestroyed) {
                            Vector3 floorPos = buildings_[i].position;
                            floorPos.y = buildings_[i].position.y + (float)f * kFloorHeight + kFloorHeight * 0.5f;
                            worldMatrix = MakeAffineMatrix(buildings_[i].scale, buildings_[i].rotate, floorPos);
                        } else {
                            worldMatrix = buildings_[i].cachedDetailWorldMatrices[f];
                        }

                        buildingTransformData_[cbIndex]->World = worldMatrix;
                        buildingTransformData_[cbIndex]->WVP = Multiply(worldMatrix, viewProjectionMatrix);
                        buildingTransformData_[cbIndex]->color = { 1.0f, 1.0f, 1.0f, alpha };
                        cbIndex++;
                        count++;
                    }
                    buildings_[i].cbCount = count;
                } else {
                    // 遠距離または背景ダミービル: 1棟につき1ドローコールに統合（Yスケールを一括拡大して超軽量化）
                    if (cbIndex >= kMaxBuildingCBs) break;

                    buildings_[i].cbIndexStart = cbIndex;
                    buildings_[i].cbCount = 1;

                    if (!buildings_[i].isWorldMatrixCached) {
                        Vector3 integratedScale = buildings_[i].scale;
                        integratedScale.y = kFloorHeight * (float)buildings_[i].floors;
                        Vector3 centerPos = buildings_[i].position;
                        centerPos.y = buildings_[i].position.y + integratedScale.y * 0.5f;
                        buildings_[i].cachedIntegratedWorldMatrix = MakeAffineMatrix(integratedScale, buildings_[i].rotate, centerPos);

                        buildings_[i].cachedDetailWorldMatrices.resize(buildings_[i].floors);
                        for (int f = 0; f < buildings_[i].floors; ++f) {
                            Vector3 floorPos = buildings_[i].position;
                            floorPos.y = buildings_[i].position.y + (float)f * kFloorHeight + kFloorHeight * 0.5f;
                            buildings_[i].cachedDetailWorldMatrices[f] = MakeAffineMatrix(buildings_[i].scale, buildings_[i].rotate, floorPos);
                        }
                        buildings_[i].isWorldMatrixCached = true;
                    }

                    Matrix4x4 worldMatrix = buildings_[i].cachedIntegratedWorldMatrix;
                    buildingTransformData_[cbIndex]->World = worldMatrix;
                    buildingTransformData_[cbIndex]->WVP = Multiply(worldMatrix, viewProjectionMatrix);
                    buildingTransformData_[cbIndex]->color = { 1.0f, 1.0f, 1.0f, alpha };
                    cbIndex++;
                }
            }
        }

        // 床(Plane)のワールド・WVP行列を計算
        {
            if (!floorWorldMatricesInitialized_ || (int)floorWorldMatrices_.size() != numLoadedFloors_) {
                floorWorldMatrices_.resize(numLoadedFloors_);
                // 十字路(juziro.jpg: 80m×80m)、横道(douro.jpg: 80m×80m)、縦道(douro.jpg: 長さ110m×幅80m、中心95m/205m) で隙間0m・重なり0mでぴったり接続
                const Matrix4x4 scaleJuziro = Matrix4x4MakeScaleMatrix({ 40.0f, 40.0f, 1.0f });
                const Matrix4x4 scaleCross  = Matrix4x4MakeScaleMatrix({ 40.0f, 40.0f, 1.0f });
                const Matrix4x4 scaleLong   = Matrix4x4MakeScaleMatrix({ 55.0f, 40.0f, 1.0f });
                const Matrix4x4 baseRotX    = MakeRotateXMatrix(1.57079632f);
                const Matrix4x4 baseRotY    = MakeRotateYMatrix(1.57079632f);
                const Matrix4x4 baseRotMat  = Multiply(baseRotX, baseRotY);

                for (int i = 0; i < numLoadedFloors_; ++i) {
                    const Matrix4x4& scaleMatrix = (floorIsJuziro_[i] == 1) ? scaleJuziro : ((floorIsJuziro_[i] == 2) ? scaleCross : scaleLong);
                    Matrix4x4 courseRotX   = MakeRotateXMatrix(floorRotations_[i].x);
                    Matrix4x4 courseRotY   = MakeRotateYMatrix(floorRotations_[i].y);
                    Matrix4x4 courseRotZ   = MakeRotateZMatrix(floorRotations_[i].z);
                    Matrix4x4 courseRotMat = Multiply(Multiply(courseRotX, courseRotY), courseRotZ);
                    Matrix4x4 rotMat       = Multiply(baseRotMat, courseRotMat);

                    Matrix4x4 translateMatrix = MakeTranslateMatrix(floorPositions_[i]);
                    floorWorldMatrices_[i]    = Multiply(Multiply(scaleMatrix, rotMat), translateMatrix);
                    if (floorTransformData_[i]) {
                        floorTransformData_[i]->World = floorWorldMatrices_[i];
                    }
                }
                floorWorldMatricesInitialized_ = true;
            }

            bool isBossIntro = (bossAppearanceTimer_ > 0.0f);
            float t_app = kBossIntroTotalDuration - bossAppearanceTimer_;
            Vector3 refCamPos = isBossIntro
                ? ((t_app < 11.2f) ? GetBossPosition(0.0f, 0.0f) : camera_->GetTransform().translate)
                : (isTitleMode_ ? GetRailPosition((std::max)(0.0f, fighterWorldZ_ - 65.0f)) : camera_->GetTransform().translate);
            float camX = refCamPos.x;
            float camZ = refCamPos.z;
            const float kFloorFarDistSq = kFloorCullFarDist * kFloorCullFarDist;

            for (int i = 0; i < numLoadedFloors_; ++i) {
                float dx = floorPositions_[i].x - camX;
                float dz = floorPositions_[i].z - camZ;
                if (dx * dx + dz * dz > kFloorFarDistSq) continue;

                if (floorTransformData_[i]) {
                    floorTransformData_[i]->WVP = Multiply(floorWorldMatrices_[i], viewProjectionMatrix);
                }
            }
        }

        // 広域地面ベース (Ground Base Plane) のワールド・WVP行列を計算 ($Y = -20.2\text{m}$ に配置してタイルの空白空間を完全隠蔽)
        if (groundBaseTransformData_) {
            const Vector3 groundScale   = { 10000.0f, 10000.0f, 1.0f };
            const Matrix4x4 scaleMatrix = Matrix4x4MakeScaleMatrix(groundScale);
            const Matrix4x4 baseRotX    = MakeRotateXMatrix(1.57079632f);
            const Matrix4x4 baseRotY    = MakeRotateYMatrix(1.57079632f);
            const Matrix4x4 rotMat      = Multiply(baseRotX, baseRotY);
            const Matrix4x4 translateMatrix = MakeTranslateMatrix({ 0.0f, -20.2f, 5000.0f });

            Matrix4x4 worldMatrix = Multiply(Multiply(scaleMatrix, rotMat), translateMatrix);
            groundBaseTransformData_->World = worldMatrix;
            groundBaseTransformData_->WVP   = Multiply(worldMatrix, viewProjectionMatrix);
        }

        // 地形(Terrain)のワールド・WVP行列を計算
        if (hasTerrain_ && terrainTransformData_) {
            // Blenderスケール 0.1 で出力されているため、ゲーム上では等倍(1.0f)のスケールにするために10倍にする
            const Vector3 terrainScale = { 10.0f, 10.0f, 10.0f };
            const Vector3 terrainRotate = { 0.0f, 0.0f, 0.0f };
            const Vector3 terrainTranslate = { 0.0f, 0.0f, 0.0f };
            
            Matrix4x4 worldMatrix = MakeAffineMatrix(terrainScale, terrainRotate, terrainTranslate);
            terrainTransformData_->World = worldMatrix;
            terrainTransformData_->WVP   = Multiply(worldMatrix, viewProjectionMatrix);
        }



    }

    // 戦闘機モードの場合のジェット噴射エミッター位置の計算
    Vector3 leftJetPos = { 0.0f, 0.0f, 0.0f };
    Vector3 rightJetPos = { 0.0f, 0.0f, 0.0f };
    Vector3 jetDirection = { 0.0f, 0.0f, 1.0f };
    fighterWorldPos = { 0.0f, 0.0f, 0.0f };
    if (sceneMode_ == SceneMode::kFighter) {
        Vector3 playerRailPos = GetRailPosition(fighterWorldZ_);
        Vector3 playerRailDir = GetRailDirection(fighterWorldZ_);
        jetDirection = playerRailDir;
        Vector3 playerRailRight = CalculateRailRight(playerRailDir);
        Vector3 playerRailUp = CalculateRailUp(playerRailDir, playerRailRight);
        
        float playerOffsetUp = fighterModel_->transform.translate.y + 17.0f;
        fighterWorldPos = Add(playerRailPos, Add(Scale(playerRailRight, fighterModel_->transform.translate.x), Scale(playerRailUp, playerOffsetUp)));
        
        // 自機の現在のロール・ピッチ角度から R_local を再構築して機体姿勢に同期させる
        float pitch = fighterModel_->transform.rotate.x;
        float yaw_180 = static_cast<float>(M_PI);
        float roll = fighterModel_->transform.rotate.z;

        Matrix4x4 rotateY = MakeRotateYMatrix(yaw_180);
        Matrix4x4 rotateX = MakeRotateXMatrix(pitch);
        Matrix4x4 rotateZ = MakeRotateZMatrix(roll);
        Matrix4x4 R_local = Multiply(Multiply(rotateZ, rotateX), rotateY);

        Matrix4x4 R_rail = MakeIdentity4x4();
        R_rail.m[0][0] = playerRailRight.x; R_rail.m[0][1] = playerRailRight.y; R_rail.m[0][2] = playerRailRight.z;
        R_rail.m[1][0] = playerRailUp.x;    R_rail.m[1][1] = playerRailUp.y;    R_rail.m[1][2] = playerRailUp.z;
        R_rail.m[2][0] = playerRailDir.x;   R_rail.m[2][1] = playerRailDir.y;   R_rail.m[2][2] = playerRailDir.z;

        Matrix4x4 R_final = Multiply(R_local, R_rail);

        // 機体のローカル回転に追従した Right, Up, Dir を抽出
        Vector3 finalRight = { R_final.m[0][0], R_final.m[0][1], R_final.m[0][2] };
        Vector3 finalUp    = { R_final.m[1][0], R_final.m[1][1], R_final.m[1][2] };
        Vector3 finalDir   = { R_final.m[2][0], R_final.m[2][1], R_final.m[2][2] };

        // 姿勢に追従した位置でジェット位置を計算
        Vector3 localOffsetLeft = Add(Scale(finalRight, -1.0f), Add(Scale(finalUp, 1.4f), Scale(finalDir, 6.8f)));
        Vector3 localOffsetRight = Add(Scale(finalRight, 0.2f), Add(Scale(finalUp, 1.4f), Scale(finalDir, 6.8f)));
        
        leftJetPos  = Add(fighterWorldPos, localOffsetLeft);
        rightJetPos = Add(fighterWorldPos, localOffsetRight);
    }

    // ── パーティクル/エフェクトエンジンの更新（画面外からの突入配置が完了するまではジェット噴射を出さない） ──
    bool hidePlayerForTransition = !isTitleMode_ && LoadingScreen::GetInstance()->ShouldHideInGamePlayer();
    bool isPhaseIntroBoost = !isTitleMode_ && (phaseIntroTimer_ >= 0.35f && phaseIntroTimer_ < 2.15f);
    particleManager_->Update(
        viewProjectionMatrix,
        camera_->GetBillboardMatrix(),
        kDeltaTime,
        camera_->GetTransform().translate.z,
        fighterWorldPos,
        isBoosting_ || isPhaseIntroBoost,
        (sceneMode_ == SceneMode::kFighter && !isBossDefeatedSequence_ && !hidePlayerForTransition),
        currentEffect_,
        emitterPos_,
        leftJetPos,
        rightJetPos,
        jetDirection,
        (isTitleMode_ && !isTitleTransitioning_) ? 0.0f : boostForwardSpeed_
    );

    {
        D3D12_RESOURCE_DESC desc = textTextureResource_->GetDesc();
        float imageWidth = (float)desc.Width;
        float imageHeight = (float)desc.Height;
        Vector3 scale = { imageWidth, imageHeight, 1.0f };
        Vector3 rotate = { 0.0f, 0.0f, 0.0f };
        float halfClientW = (float)WinApp::kClientWidth / 2.0f;
        float halfClientH = (float)WinApp::kClientHeight / 2.0f;
        Vector3 translate = {
            -halfClientW + (imageWidth / 2.0f) + spritePos_.x,
            -halfClientH + (imageHeight / 2.0f) + spritePos_.y,
            0.0f
        };
        Matrix4x4 worldSprite = MakeAffineMatrix(scale, rotate, translate);
        Matrix4x4 projectionSprite = MakeOrthographicMatrix(-halfClientW, halfClientH, halfClientW, -halfClientH, 0.0f, 100.0f);
        Matrix4x4 viewSprite = MakeIdentity4x4();
        Matrix4x4 viewProjSprite = Multiply(viewSprite, projectionSprite);

        spriteInstancingData_[0].World = worldSprite;
        spriteInstancingData_[0].WVP = Multiply(worldSprite, viewProjSprite);
        spriteInstancingData_[0].color = { 1.0f, 1.0f, 1.0f, 1.0f };
    }

    // ── HPバーの更新 ──
    if (hpBarInstancingData_) {
        float halfClientW = (float)WinApp::kClientWidth / 2.0f;
        float halfClientH = (float)WinApp::kClientHeight / 2.0f;
        Matrix4x4 projectionSprite = MakeOrthographicMatrix(-halfClientW, halfClientH, halfClientW, -halfClientH, 0.0f, 100.0f);
        Matrix4x4 viewProjSprite = Multiply(MakeIdentity4x4(), projectionSprite);

        // 共通設定
        for (uint32_t i = 0; i < kHpBarInstanceCount; ++i) {
            hpBarInstancingData_[i].uvTransform = MakeIdentity4x4();
        }

        // 1. プレイヤーHPバー背景（インデックス0）
        {
            Vector3 scale = { 200.0f, 20.0f, 1.0f };
            // 左上基準配置 (左端を -halfClientW + 30px に固定)
            Vector3 translate = { -halfClientW + 30.0f + scale.x, halfClientH - 30.0f - (scale.y / 2.0f), 0.1f };
            Matrix4x4 world = MakeAffineMatrix(scale, Vector3{0, 0, 0}, translate);
            hpBarInstancingData_[0].World = world;
            hpBarInstancingData_[0].WVP = Multiply(world, viewProjSprite);
            hpBarInstancingData_[0].color = { 0.1f, 0.1f, 0.1f, 0.8f }; // ダークグレー
        }

        // 2. プレイヤーHPバー前景（インデックス1）
        {
            // 滑らかなHPVisual_を使用
            float hpRatio = std::clamp(playerHPVisual_ / playerMaxHP_, 0.0f, 1.0f);
            Vector3 scale = { 196.0f * hpRatio, 16.0f, 1.0f };
            // 背景の左端から4px内側に左端を固定
            Vector3 translate = { -halfClientW + 34.0f + scale.x, halfClientH - 32.0f - (scale.y / 2.0f), 0.0f };
            if (scale.x <= 0.0f) scale.x = 0.0001f;
            Matrix4x4 world = MakeAffineMatrix(scale, Vector3{0, 0, 0}, translate);
            hpBarInstancingData_[1].World = world;
            hpBarInstancingData_[1].WVP = Multiply(world, viewProjSprite);
            hpBarInstancingData_[1].color = { 0.0f, 1.0f, 0.2f, 1.0f }; // 鮮やかな緑
        }

        // 3. ボスHPバー外枠（インデックス2）
        {
            Vector3 scale = { 350.0f, 24.0f, 1.0f };
            // 右上基準配置 (右端を halfClientW - 30px に固定)
            Vector3 translate = { halfClientW - 30.0f - scale.x, halfClientH - 30.0f - (scale.y / 2.0f), 0.2f }; // 最背面
            Matrix4x4 world = MakeAffineMatrix(scale, Vector3{0, 0, 0}, translate);
            hpBarInstancingData_[2].World = world;
            hpBarInstancingData_[2].WVP = Multiply(world, viewProjSprite);
            hpBarInstancingData_[2].color = { 0.7f, 0.7f, 0.7f, 1.0f }; // ライトグレーの枠線
        }

        // 4. ボスHPバー内側背景（インデックス3）
        {
            Vector3 scale = { 346.0f, 20.0f, 1.0f };
            // 外枠から内側に2pxずつマージンをとって配置
            Vector3 translate = { halfClientW - 32.0f - scale.x, halfClientH - 32.0f - (scale.y / 2.0f), 0.1f }; // 中間
            Matrix4x4 world = MakeAffineMatrix(scale, Vector3{0, 0, 0}, translate);
            hpBarInstancingData_[3].World = world;
            hpBarInstancingData_[3].WVP = Multiply(world, viewProjSprite);
            hpBarInstancingData_[3].color = { 0.05f, 0.05f, 0.05f, 0.95f }; // ほぼ黒のダークグレー（減少部分＝後ろの背景を消す目的）
        }

        // 5. ボスHPバー前景 (残HP)（インデックス4）
        {
            // 滑らかなbossHPVisual_を使用
            float hpRatio = std::clamp(bossHPVisual_ / bossMaxHP_, 0.0f, 1.0f);
            Vector3 scale = { 346.0f * hpRatio, 20.0f, 1.0f };
            // 背景の右端から4px内側に右端を固定 (左端が右へ向けて縮む ＝ 左から減る)
            Vector3 translate = { (halfClientW - 32.0f) - scale.x, halfClientH - 32.0f - (scale.y / 2.0f), 0.0f }; // 最前面
            if (scale.x <= 0.0f) scale.x = 0.0001f;
            Matrix4x4 world = MakeAffineMatrix(scale, Vector3{0, 0, 0}, translate);
            hpBarInstancingData_[4].World = world;
            hpBarInstancingData_[4].WVP = Multiply(world, viewProjSprite);
            hpBarInstancingData_[4].color = { 0.8f, 0.1f, 0.1f, 1.0f }; // 赤 (残HP)
        }
    }

    // ── 蜘蛛の巣弾（3Dビルボード）の更新・行列計算 ──
    if (webBulletInstancingData_) {
        Matrix4x4 viewProj = camera_->GetViewProjectionMatrix();
        Matrix4x4 billboard = camera_->GetBillboardMatrix();

        for (int i = 0; i < kMaxWebBullets; ++i) {
            if (bossWebBullets_[i].isAlive) {
                Vector3 scale = { bossWebBullets_[i].radius, bossWebBullets_[i].radius, 1.0f };
                Matrix4x4 world = Multiply(MakeScaleMatrix(scale), billboard);
                world.m[3][0] = bossWebBullets_[i].position.x;
                world.m[3][1] = bossWebBullets_[i].position.y;
                world.m[3][2] = bossWebBullets_[i].position.z;

                webBulletInstancingData_[i].World = world;
                webBulletInstancingData_[i].WVP = Multiply(world, viewProj);
                webBulletInstancingData_[i].color = { 1.0f, 1.0f, 1.0f, 1.0f };
                webBulletInstancingData_[i].uvTransform = MakeIdentity4x4();
            } else {
                webBulletInstancingData_[i].World = MakeIdentity4x4();
                webBulletInstancingData_[i].WVP = MakeIdentity4x4();
                webBulletInstancingData_[i].color.w = 0.0f;
            }
        }
    }

    // ── 画面蜘蛛の巣（2Dスプライト）の更新・行列計算 ──
    if (screenWebTransformData_) {
        if (screenWebTimer_ > 0.0f) {
            float halfClientW = (float)WinApp::kClientWidth / 2.0f;
            float halfClientH = (float)WinApp::kClientHeight / 2.0f;
            Matrix4x4 projectionSprite = MakeOrthographicMatrix(-halfClientW, halfClientH, halfClientW, -halfClientH, 0.0f, 100.0f);
            Matrix4x4 viewProjSprite = Multiply(MakeIdentity4x4(), projectionSprite);

            Vector3 scale = { 650.0f, 650.0f, 1.0f };
            Vector3 translate = { 0.0f, 0.0f, 0.0f }; 
            Matrix4x4 world = MakeAffineMatrix(scale, Vector3{0,0,0}, translate);

            float alpha = std::clamp(screenWebTimer_ / 5.0f, 0.0f, 0.8f);

            screenWebTransformData_->World = world;
            screenWebTransformData_->WVP = Multiply(world, viewProjSprite);
            screenWebTransformData_->color = { 1.0f, 1.0f, 1.0f, alpha };
            screenWebTransformData_->uvTransform = MakeIdentity4x4();
        } else {
            screenWebTransformData_->World = MakeIdentity4x4();
            screenWebTransformData_->WVP = MakeIdentity4x4();
            screenWebTransformData_->color = { 1.0f, 1.0f, 1.0f, 0.0f };
        }
    }

    if (gpuParticleManager_) {
        gpuParticleManager_->Update(camera_->GetViewProjectionMatrix(), camera_->GetBillboardMatrix(), kDeltaTime);
    }

#ifdef USE_IMGUI
    ImGui::Begin("PostProcess");
    const char* items[] = { "None", "Grayscale", "Sepia", "Vignette", "BoxFilter", "Outline", "RadialBlur", "Dissolve", "Random" };
    int currentItem = static_cast<int>(activePostProcess_);
    if (ImGui::Combo("Effect", &currentItem, items, IM_ARRAYSIZE(items))) {
        activePostProcess_ = static_cast<PostProcessType>(currentItem);
    }
    if (activePostProcess_ == kRandom) {
        ImGui::SliderFloat("Noise Scale", &randomNoiseScale_, 1.0f, 1000.0f);
        ImGui::SliderFloat("Noise Strength", &randomNoiseStrength_, 0.0f, 1.0f);
        ImGui::SliderFloat("Time Speed", &randomSpeed_, 0.0f, 10.0f);
        ImGui::Checkbox("Color Noise", &randomIsColorNoise_);
        
        const char* noiseTypeItems[] = { "TV Static", "Multiply" };
        ImGui::Combo("Noise Type", &randomNoiseType_, noiseTypeItems, IM_ARRAYSIZE(noiseTypeItems));
        
        ImGui::Text("Random Effect Time: %.2f", randomEffectTime_);
    }
    if (activePostProcess_ == kVignette) {
        ImGui::SliderFloat("Vignette Scale", &vignetteParamData_->scale, 0.0f, 32.0f);
        ImGui::SliderFloat("Vignette Power", &vignetteParamData_->power, 0.0f, 5.0f);
    }
    if (activePostProcess_ == kBoxFilter) {
        ImGui::SliderInt("Kernel Size (k)", &boxFilterParamData_->kernelSize, 1, 10);
    }
    if (activePostProcess_ == kRadialBlur) {
        ImGui::SliderFloat2("Center", &radialBlurParamData_->center.x, 0.0f, 1.0f);
        ImGui::SliderFloat("Blur Width", &radialBlurParamData_->blurWidth, 0.0f, 0.1f);
    }
    if (activePostProcess_ == kDissolve) {
        ImGui::SliderFloat("Threshold", &dissolveParamData_->threshold, 0.0f, 1.0f);
        ImGui::ColorEdit3("EdgeColor", &dissolveParamData_->edgeColor.x);
        ImGui::SliderFloat("EdgeRange", &dissolveParamData_->edgeRange, 0.0f, 0.1f);
        const char* noiseItems[] = { "noise0", "noise1" };
        if (ImGui::Combo("Mask Texture", &selectedNoiseIndex_, noiseItems, IM_ARRAYSIZE(noiseItems))) {
            activeNoiseSrvIndex_ = (selectedNoiseIndex_ == 0) ? noise0SrvIndex_ : noise1SrvIndex_;
        }
    }
    ImGui::End();
#endif
}

void GamePlayScene::Draw() {
    if (isDemoMode_) {
        DrawDemo();
        return;
    }

    ID3D12GraphicsCommandList* commandList = dxCommon_->GetCommandList();

    // --- 1. RenderTextureへの描画開始 ---
    postProcess_->PreDraw();

    // スカイボックスの描画 (dds天球を完全に非表示にする)
    /*
    if (skybox_ && showSkybox_) {
        ID3D12DescriptorHeap* heaps[] = { TextureManager::GetInstance()->GetSrvHeap() };
        commandList->SetDescriptorHeaps(1, heaps);

        if (graphicsPipeline_ && graphicsPipeline_->GetSkyboxPipelineState()) {
            commandList->SetPipelineState(graphicsPipeline_->GetSkyboxPipelineState());
            commandList->SetGraphicsRootSignature(graphicsPipeline_->GetRootSignature());

            // カメラのビュー行列を取得し、平行移動成分をゼロにする
            Matrix4x4 viewMatrix = camera_->GetViewMatrix();
            viewMatrix.m[3][0] = 0.0f;
            viewMatrix.m[3][1] = 0.0f;
            viewMatrix.m[3][2] = 0.0f;

            // 進行感を出すために、時間経過でゆっくりY軸周りに自動回転させる
            float rotationAngle = randomEffectTime_ * 0.03f;
            Matrix4x4 rotateY = MakeRotateYMatrix(rotationAngle);
            Matrix4x4 skyboxViewMatrix = Multiply(rotateY, viewMatrix);

            Matrix4x4 projectionMatrix = camera_->GetProjectionMatrix();
            Matrix4x4 wvpMatrix = Multiply(skyboxViewMatrix, projectionMatrix);

            int index = std::clamp(skyboxType_, 0, static_cast<int>(kNumSkyboxTextures) - 1);
            std::string skyboxTexName = kSkyboxTextures[index];
            // 安全対策: 指定されたテクスチャがキューブマップでない場合は、有効なキューブマップにフォールバックする
            if (!TextureManager::GetInstance()->GetMetaData(skyboxTexName).IsCubemap()) {
                skyboxTexName = "rostock_laage_airport_4k.dds";
            }
            D3D12_GPU_DESCRIPTOR_HANDLE skyboxSrvHandle = TextureManager::GetInstance()->GetSrvHandleGPU(skyboxTexName);

            skybox_->Draw(commandList, wvpMatrix, skyboxSrvHandle);
        }
    }
    */

    // 1. キャラクターモデルの描画 (playerModel_)
    if (showSimpleSkin_ && playerModel_) {
        ID3D12DescriptorHeap* modelHeaps[] = { TextureManager::GetInstance()->GetSrvHeap() };
        commandList->SetDescriptorHeaps(1, modelHeaps);

        // パイプラインが生成されているか念のためチェック
        if (graphicsPipeline_ && graphicsPipeline_->GetObject3dPipelineState() && graphicsPipeline_->GetObject3dRootSignature()) {
            commandList->SetPipelineState(graphicsPipeline_->GetObject3dPipelineState());
            commandList->SetGraphicsRootSignature(graphicsPipeline_->GetObject3dRootSignature());

            // 行列、ライト、カメラの定数バッファをセット
            if (transformResource_) commandList->SetGraphicsRootConstantBufferView(1, transformResource_->GetGPUVirtualAddress());
            if (directionalLightResource_) commandList->SetGraphicsRootConstantBufferView(4, directionalLightResource_->GetGPUVirtualAddress());
            if (cameraResource_) commandList->SetGraphicsRootConstantBufferView(5, cameraResource_->GetGPUVirtualAddress());

            playerModel_->DrawModel(
                commandList,
                TextureManager::GetInstance()->GetSrvHandleGPU("human/white.png"),
                TextureManager::GetInstance()->GetSrvHandleGPU("test.dds")
            );
        }
    }

    // --- 3Dオブジェクト（戦闘機/デバッグカメラモード共通）の描画 ---
    if (sceneMode_ == SceneMode::kFighter || sceneMode_ == SceneMode::kCamera) {
        ID3D12DescriptorHeap* modelHeaps[] = { TextureManager::GetInstance()->GetSrvHeap() };
        commandList->SetDescriptorHeaps(1, modelHeaps);
        if (graphicsPipeline_ && graphicsPipeline_->GetObject3dPipelineState() && graphicsPipeline_->GetObject3dRootSignature()) {
            commandList->SetPipelineState(graphicsPipeline_->GetObject3dPipelineState());
            commandList->SetGraphicsRootSignature(graphicsPipeline_->GetObject3dRootSignature());

            // 地球・日本・東京の夜景フェーズは廃止し、最初からステージとボスの登場演出を描画する
            bool isIntroMapPhase = false;

            // 自機(Fighter)描画（画面外から突っ込んできたプレイヤー機が定位置に配置されるまでは元から立っている方を非表示にする）
            bool hidePlayerForTransition = !isTitleMode_ && LoadingScreen::GetInstance()->ShouldHideInGamePlayer();
            if (fighterModel_ && !hidePlayerForTransition && !isIntroMapPhase) {
                if (fighterTransformResource_) commandList->SetGraphicsRootConstantBufferView(1, fighterTransformResource_->GetGPUVirtualAddress());
                if (directionalLightResource_) commandList->SetGraphicsRootConstantBufferView(4, directionalLightResource_->GetGPUVirtualAddress());
                if (cameraResource_) commandList->SetGraphicsRootConstantBufferView(5, cameraResource_->GetGPUVirtualAddress());
                fighterModel_->DrawModel(commandList, TextureManager::GetInstance()->GetSrvHandleGPU("Player2/Player_basecolor.JPEG"), TextureManager::GetInstance()->GetSrvHandleGPU("test.dds"));
            }

            // 床(Plane)描画 (標準 douro.jpg & 300m置き十字路 juziro.jpg)
            if (floorModel_ && !isIntroMapPhase) {
                if (directionalLightResource_) commandList->SetGraphicsRootConstantBufferView(4, directionalLightResource_->GetGPUVirtualAddress());
                if (cameraResource_) commandList->SetGraphicsRootConstantBufferView(5, cameraResource_->GetGPUVirtualAddress());

                D3D12_GPU_DESCRIPTOR_HANDLE douroSrv = TextureManager::GetInstance()->GetSrvHandleGPU("douro.jpg");
                D3D12_GPU_DESCRIPTOR_HANDLE juziroSrv = TextureManager::GetInstance()->GetSrvHandleGPU("juziro.jpg");
                D3D12_GPU_DESCRIPTOR_HANDLE testSrv = TextureManager::GetInstance()->GetSrvHandleGPU("test.dds");
                D3D12_GPU_DESCRIPTOR_HANDLE whiteSrv = TextureManager::GetInstance()->GetSrvHandleGPU("human/white.png");

                // 0. 広域地面ベース（上空からステージを見下ろした際に道路外の隙間が見えないようにダークアスファルト色で敷く）
                if (groundBaseTransformResource_) {
                    floorModel_->SetColor({ 0.05f, 0.05f, 0.07f, 1.0f });
                    floorModel_->PrepareDraw(commandList, whiteSrv, testSrv);
                    commandList->SetGraphicsRootConstantBufferView(1, groundBaseTransformResource_->GetGPUVirtualAddress());
                    floorModel_->DrawPrepared(commandList);
                    floorModel_->SetColor({ 1.0f, 1.0f, 1.0f, 1.0f });
                }

                bool isBossIntro = (bossAppearanceTimer_ > 0.0f);
                float t_app = kBossIntroTotalDuration - bossAppearanceTimer_;
                Vector3 refCamPos = isBossIntro
                    ? ((t_app < 11.2f) ? GetBossPosition(0.0f, 0.0f) : camera_->GetTransform().translate)
                    : (isTitleMode_ ? GetRailPosition((std::max)(0.0f, fighterWorldZ_ - 65.0f)) : camera_->GetTransform().translate);
                float camX = refCamPos.x;
                float camZ = refCamPos.z;
                const float kFloorFarDistSq = kFloorCullFarDist * kFloorCullFarDist;

                // 1. 通常道路 (douro.jpg)
                floorModel_->PrepareDraw(commandList, douroSrv, testSrv);
                for (int i = 0; i < numLoadedFloors_; ++i) {
                    if (floorIsJuziro_[i] == 1) continue;
                    if (floorPositions_[i].y < -5000.0f) continue;
                    float dx = floorPositions_[i].x - camX;
                    float dz = floorPositions_[i].z - camZ;
                    if (dx * dx + dz * dz > kFloorFarDistSq) continue;

                    D3D12_GPU_VIRTUAL_ADDRESS cbAddr = floorTransformBufferResource_ ? 
                        (floorTransformBufferResource_->GetGPUVirtualAddress() + i * 256) : 
                        floorTransformResources_[i]->GetGPUVirtualAddress();
                    commandList->SetGraphicsRootConstantBufferView(1, cbAddr);
                    floorModel_->DrawPrepared(commandList);
                }

                // 2. 十字路 (juziro.jpg)
                floorModel_->PrepareDraw(commandList, juziroSrv, testSrv);
                for (int i = 0; i < numLoadedFloors_; ++i) {
                    if (floorIsJuziro_[i] != 1) continue;
                    if (floorPositions_[i].y < -5000.0f) continue;
                    float dx = floorPositions_[i].x - camX;
                    float dz = floorPositions_[i].z - camZ;
                    if (dx * dx + dz * dz > kFloorFarDistSq) continue;

                    D3D12_GPU_VIRTUAL_ADDRESS cbAddr = floorTransformBufferResource_ ? 
                        (floorTransformBufferResource_->GetGPUVirtualAddress() + i * 256) : 
                        floorTransformResources_[i]->GetGPUVirtualAddress();
                    commandList->SetGraphicsRootConstantBufferView(1, cbAddr);
                    floorModel_->DrawPrepared(commandList);
                }
            }

            // 地形(Terrain)描画
            if (hasTerrain_ && terrainModel_ && terrainTransformResource_ && !isIntroMapPhase) {
                if (directionalLightResource_) commandList->SetGraphicsRootConstantBufferView(4, directionalLightResource_->GetGPUVirtualAddress());
                if (cameraResource_) commandList->SetGraphicsRootConstantBufferView(5, cameraResource_->GetGPUVirtualAddress());
                commandList->SetGraphicsRootConstantBufferView(1, terrainTransformResource_->GetGPUVirtualAddress());
                terrainModel_->DrawModel(commandList, TextureManager::GetInstance()->GetSrvHandleGPU("human/white.png"), TextureManager::GetInstance()->GetSrvHandleGPU("test.dds"));
            }

            // 天球(SkyDome)描画 ── 専用パイプライン（Zバッファ書き込みなし・常に最遠面・ぼかしサンプラー）で描画
            if (skydomeModel_ && skydomeTransformRes_ && graphicsPipeline_ && graphicsPipeline_->GetSkydomePipelineState() && !isIntroMapPhase) {
                // 天球専用パイプラインとルートシグネチャに切り替える
                commandList->SetPipelineState(graphicsPipeline_->GetSkydomePipelineState());
                commandList->SetGraphicsRootSignature(graphicsPipeline_->GetSkydomeRootSignature());
                if (directionalLightResource_) commandList->SetGraphicsRootConstantBufferView(4, directionalLightResource_->GetGPUVirtualAddress());
                if (cameraResource_) commandList->SetGraphicsRootConstantBufferView(5, cameraResource_->GetGPUVirtualAddress());
                commandList->SetGraphicsRootConstantBufferView(1, skydomeTransformRes_->GetGPUVirtualAddress());
                int index = std::clamp(skyboxType_, 0, static_cast<int>(kNumSkyboxTextures) - 1);
                std::string skyTex = kSkyboxTextures[index];
                skydomeModel_->DrawModel(commandList, TextureManager::GetInstance()->GetSrvHandleGPU(skyTex), TextureManager::GetInstance()->GetSrvHandleGPU("test.dds"));
                // 天球描画後に Object3d パイプラインに戻す
                commandList->SetPipelineState(graphicsPipeline_->GetObject3dPipelineState());
                commandList->SetGraphicsRootSignature(graphicsPipeline_->GetObject3dRootSignature());
            }

            // ビル(Building)描画（距離カリング & 1棟1ドローコール統合により超軽量化）
            if (buildingModel_ && !isIntroMapPhase) {
                if (directionalLightResource_) commandList->SetGraphicsRootConstantBufferView(4, directionalLightResource_->GetGPUVirtualAddress());
                if (cameraResource_) commandList->SetGraphicsRootConstantBufferView(5, cameraResource_->GetGPUVirtualAddress());

                D3D12_GPU_DESCRIPTOR_HANDLE bldgSrv = TextureManager::GetInstance()->GetSrvHandleGPU("building/buillding_uv.png");
                D3D12_GPU_DESCRIPTOR_HANDLE testSrv = TextureManager::GetInstance()->GetSrvHandleGPU("test.dds");
                buildingModel_->PrepareDraw(commandList, bldgSrv, testSrv);

                for (size_t i = 0; i < buildings_.size(); ++i) {
                    if (buildings_[i].cbIndexStart < 0 || buildings_[i].cbCount <= 0) continue;

                    for (int c = 0; c < buildings_[i].cbCount; ++c) {
                        int cbIdx = buildings_[i].cbIndexStart + c;
                        if (cbIdx >= kMaxBuildingCBs) break;
                        D3D12_GPU_VIRTUAL_ADDRESS bldgCbAddr = buildingTransformBufferResource_ ?
                            (buildingTransformBufferResource_->GetGPUVirtualAddress() + cbIdx * 256) :
                            buildingTransformResources_[cbIdx]->GetGPUVirtualAddress();
                        commandList->SetGraphicsRootConstantBufferView(1, bldgCbAddr);
                        buildingModel_->DrawPrepared(commandList);
                    }
                }
            }

            // 弾の描画（スター・ウォーズ風 蛍光ブラスターレーザー：鮮やかな黄緑色）
            if (debugBoxModel_) {
                debugBoxModel_->SetLightingEnabled(false); // 発光ビームのためライティングをOFF（黒ずみ解消）
                D3D12_GPU_DESCRIPTOR_HANDLE whiteSrv = TextureManager::GetInstance()->GetSrvHandleGPU("human/white.png");
                D3D12_GPU_DESCRIPTOR_HANDLE testSrv = TextureManager::GetInstance()->GetSrvHandleGPU("test.dds");

                // 1. 外殻（鮮やかな蛍光黄緑色オーラ）
                debugBoxModel_->SetColor({ 0.45f, 1.0f, 0.10f, 1.0f });
                debugBoxModel_->PrepareDraw(commandList, whiteSrv, testSrv);

                for (int i = 0; i < kMaxBullets; ++i) {
                    if (playerBullets_[i].currentTime < playerBullets_[i].lifeTime) {
                        commandList->SetGraphicsRootConstantBufferView(1, bulletTransformResources_[i]->GetGPUVirtualAddress());
                        debugBoxModel_->DrawPrepared(commandList);
                    }
                }

                // 2. 内核（中心の明るい黄緑ホワイト発光コア）
                debugBoxModel_->SetColor({ 0.85f, 1.0f, 0.65f, 1.0f });
                debugBoxModel_->PrepareDraw(commandList, whiteSrv, testSrv);

                for (int i = 0; i < kMaxBullets; ++i) {
                    if (playerBullets_[i].currentTime < playerBullets_[i].lifeTime) {
                        Matrix4x4 origWorld = bulletTransformData_[i]->World;
                        Vector3 vel = playerBullets_[i].velocity;
                        Vector3 dir = Normalize(vel);
                        Matrix4x4 rotMatrix = MakeLookRotationMatrix(dir);
                        Vector3 scaleInner = { 1.1f, 1.1f, 5.0f }; // 内核用スケール
                        Matrix4x4 worldInner = Multiply(Multiply(MakeScaleMatrix(scaleInner), rotMatrix), MakeTranslateMatrix(playerBullets_[i].position));

                        bulletTransformData_[i]->World = worldInner;
                        bulletTransformData_[i]->WVP = Multiply(worldInner, camera_->GetViewProjectionMatrix());

                        commandList->SetGraphicsRootConstantBufferView(1, bulletTransformResources_[i]->GetGPUVirtualAddress());
                        debugBoxModel_->DrawPrepared(commandList);

                        // 元の外殻行列に戻す
                        bulletTransformData_[i]->World = origWorld;
                        bulletTransformData_[i]->WVP = Multiply(origWorld, camera_->GetViewProjectionMatrix());
                    }
                }

                // トドメ巨大弾の描画（黄緑色に発光）
                if (isDefeatBulletActive_ && defeatBulletTransformResource_ && debugSphereModel_) {
                    debugSphereModel_->SetColor({ 0.0f, 1.0f, 1.0f, 1.0f }); // 元のフィニッシュ弾色(シアン)
                    debugSphereModel_->PrepareDraw(commandList, whiteSrv, testSrv);
                    commandList->SetGraphicsRootConstantBufferView(1, defeatBulletTransformResource_->GetGPUVirtualAddress());
                    debugSphereModel_->DrawPrepared(commandList);
                }
            }

            // 敵の描画（プレイヤーと同じ戦闘機モデルを使用。冒頭演出中およびPHASE演出中は非表示、Phase 1 / Phase 2 で描画）
            if (enemyModel_ && showEnemies_ && currentPhase_ != GamePhase::kBossFight && !(bossAppearanceTimer_ > 0.0f) && !(phaseIntroTimer_ >= 0.0f)) {
                D3D12_GPU_DESCRIPTOR_HANDLE enemySrv = TextureManager::GetInstance()->GetSrvHandleGPU("Player/player.png");
                D3D12_GPU_DESCRIPTOR_HANDLE testSrv = TextureManager::GetInstance()->GetSrvHandleGPU("test.dds");
                enemyModel_->PrepareDraw(commandList, enemySrv, testSrv);

                for (int i = 0; i < kMaxEnemies; ++i) {
                    if (enemies_[i].isAlive && enemies_[i].state != Enemy::State::kSideWait) {
                        commandList->SetGraphicsRootConstantBufferView(1, enemyTransformResources_[i]->GetGPUVirtualAddress());
                        enemyModel_->DrawPrepared(commandList);
                    }
                }
            }

            // 蜘蛛ボス（Big Spider）の描画（冒頭演出中およびボス戦フェーズのみ描画し、雑魚敵を追っているPhase 1 / Phase 2では映らないようにする）
            if (!isTitleMode_ && isBossModelVisible_) {
                bool isBossIntro = (bossAppearanceTimer_ > 0.0f);
                bool canDrawBoss = !isIntroMapPhase && (isBossIntro || currentPhase_ == GamePhase::kBossFight);

                if (canDrawBoss) {
                    if (directionalLightResource_) commandList->SetGraphicsRootConstantBufferView(4, directionalLightResource_->GetGPUVirtualAddress());
                    if (cameraResource_) commandList->SetGraphicsRootConstantBufferView(5, cameraResource_->GetGPUVirtualAddress());

                    // 胴体の描画 (常にオリジナル高画質モデルを使用)
                    if (bossBodyModel_ && bossBodyTransformResource_) {
                        commandList->SetGraphicsRootConstantBufferView(1, bossBodyTransformResource_->GetGPUVirtualAddress());
                        bossBodyModel_->DrawModel(
                            commandList,
                            TextureManager::GetInstance()->GetSrvHandleGPU("big Spider/big+Spider_basecolor.jpg"),
                            TextureManager::GetInstance()->GetSrvHandleGPU("test.dds")
                        );
                    }

                    // 足の描画（常にオリジナル高画質モデルを使用 ＆ バインドを1回に集約して高速化）
                    if (bossLegModel_) {
                        D3D12_VERTEX_BUFFER_VIEW vbView = bossLegModel_->GetVertexBufferView();
                        D3D12_INDEX_BUFFER_VIEW ibView = bossLegModel_->GetIndexBufferView();
                        UINT indexCount = bossLegModel_->GetIndexCount();
                        
                        commandList->IASetVertexBuffers(0, 1, &vbView);
                        commandList->IASetIndexBuffer(&ibView);
                        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                        commandList->SetGraphicsRootConstantBufferView(0, bossLegModel_->GetMaterialResource()->GetGPUVirtualAddress());
                        
                        D3D12_GPU_DESCRIPTOR_HANDLE texHandle = TextureManager::GetInstance()->GetSrvHandleGPU("big Spider/big+spider+arm_basecolor.jpg");
                        D3D12_GPU_DESCRIPTOR_HANDLE envHandle = TextureManager::GetInstance()->GetSrvHandleGPU("test.dds");
                        commandList->SetGraphicsRootDescriptorTable(2, texHandle);
                        commandList->SetGraphicsRootDescriptorTable(3, envHandle);

                        for (int i = 0; i < 8; ++i) {
                            if (bossLegTransformResources_[i]) {
                                commandList->SetGraphicsRootConstantBufferView(1, bossLegTransformResources_[i]->GetGPUVirtualAddress());
                                commandList->DrawIndexedInstanced(indexCount, 1, 0, 0, 0);
                            }
                        }
                    }
                }
            }

            // ── イントロ演出用 地球・日本・東京の夜景 3Dモデルの描画 ──
            if (!isTitleMode_ && (earthAlpha_ > 0.001f || japanAlpha_ > 0.001f || tokyoAlpha_ > 0.001f) &&
                graphicsPipeline_ && graphicsPipeline_->GetObject3dBlendNormalPipelineState()) {
                commandList->SetPipelineState(graphicsPipeline_->GetObject3dBlendNormalPipelineState());
                commandList->SetGraphicsRootSignature(graphicsPipeline_->GetObject3dRootSignature());
                if (directionalLightResource_) commandList->SetGraphicsRootConstantBufferView(4, directionalLightResource_->GetGPUVirtualAddress());
                if (cameraResource_) commandList->SetGraphicsRootConstantBufferView(5, cameraResource_->GetGPUVirtualAddress());

                D3D12_GPU_DESCRIPTOR_HANDLE envSrv = TextureManager::GetInstance()->GetSrvHandleGPU("test.dds");

                // 各3Dモデルの描画直前に深度バッファをクリアすることで、
                // 地球(Y=480)の深度が日本(Y=470)や東京(Y=460)の描画を遮蔽してしまう現象を完全に防ぎ、
                // かつ各3Dモデル内部の凹凸・3Dビル群・球体ドームの前後関係は正確にZソートさせる！
                if (earthModel_ && earthTransformResource_ && earthAlpha_ > 0.001f) {
                    commandList->ClearDepthStencilView(dxCommon_->GetDsvHandle(), D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
                    commandList->SetGraphicsRootConstantBufferView(1, earthTransformResource_->GetGPUVirtualAddress());
                    earthModel_->DrawModel(
                        commandList,
                        TextureManager::GetInstance()->GetSrvHandleGPU("Earth/earth.png"),
                        envSrv
                    );
                }
                if (japanModel_ && japanTransformResource_ && japanAlpha_ > 0.001f) {
                    commandList->ClearDepthStencilView(dxCommon_->GetDsvHandle(), D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
                    commandList->SetGraphicsRootConstantBufferView(1, japanTransformResource_->GetGPUVirtualAddress());
                    japanModel_->DrawModel(
                        commandList,
                        TextureManager::GetInstance()->GetSrvHandleGPU("Japan/japan.png"),
                        envSrv
                    );
                }
                if (tokyoModel_ && tokyoTransformResource_ && tokyoAlpha_ > 0.001f) {
                    commandList->ClearDepthStencilView(dxCommon_->GetDsvHandle(), D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
                    commandList->SetGraphicsRootConstantBufferView(1, tokyoTransformResource_->GetGPUVirtualAddress());
                    tokyoModel_->DrawModel(
                        commandList,
                        TextureManager::GetInstance()->GetSrvHandleGPU("Tokyo/tokyo.png"),
                        envSrv
                    );
                }

                // パイプラインを通常のObject3dに戻す
                commandList->SetPipelineState(graphicsPipeline_->GetObject3dPipelineState());
            }
        }
        
        // エイミング(レティクル)の描画 (ボス登場演出中および画面外からの突入配置中は非表示)
        bool isBossIntro = (bossAppearanceTimer_ > 0.0f);
        bool hideReticleForTransition = !isTitleMode_ && LoadingScreen::GetInstance()->ShouldHideInGamePlayer();
        if (!isBossIntro && !isTitleMode_ && !hideReticleForTransition && graphicsPipeline_ && graphicsPipeline_->GetRootSignature()) {
            commandList->SetGraphicsRootSignature(graphicsPipeline_->GetRootSignature());
            commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            if (graphicsPipeline_->GetPipelineState(kBlendModeNormal)) {
                commandList->SetPipelineState(graphicsPipeline_->GetPipelineState(kBlendModeNormal));
                if (particleModel_) {
                    particleModel_->Draw(commandList, 1, TextureManager::GetInstance()->GetSrvHandleGPU("aiming.png"), aimingInstancingSrvHandleGPU_);
                }
            }
        }

        // 蜘蛛の巣弾（3Dビルボード）の描画
        if (currentPhase_ == GamePhase::kBossFight && graphicsPipeline_ && graphicsPipeline_->GetRootSignature()) {
            commandList->SetGraphicsRootSignature(graphicsPipeline_->GetRootSignature());
            commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            if (graphicsPipeline_->GetPipelineState(kBlendModeNormal)) {
                commandList->SetPipelineState(graphicsPipeline_->GetPipelineState(kBlendModeNormal));
                if (particleModel_) {
                    particleModel_->Draw(commandList, kMaxWebBullets, spiderWebSrvHandleGPU_, webBulletInstancingSrvHandleGPU_);
                }
            }
        }
    }

    // simpleSkinの描画
    if (showSimpleSkin_ && cubeRenderModel_ && graphicsPipeline_ && graphicsPipeline_->GetSkinningPipelineState()) {
        ID3D12DescriptorHeap* modelHeaps[] = { TextureManager::GetInstance()->GetSrvHeap() };
        commandList->SetDescriptorHeaps(1, modelHeaps);
        commandList->SetPipelineState(graphicsPipeline_->GetSkinningPipelineState());
        commandList->SetGraphicsRootSignature(graphicsPipeline_->GetSkinningRootSignature());

        if (cubeTransformResource_) commandList->SetGraphicsRootConstantBufferView(1, cubeTransformResource_->GetGPUVirtualAddress());
        if (directionalLightResource_) commandList->SetGraphicsRootConstantBufferView(5, directionalLightResource_->GetGPUVirtualAddress());
        if (cameraResource_) commandList->SetGraphicsRootConstantBufferView(6, cameraResource_->GetGPUVirtualAddress());

        cubeRenderModel_->DrawSkinningModel(
            commandList,
            cubeSkinCluster_,
            TextureManager::GetInstance()->GetSrvHandleGPU("simpleSkin/uvChecker.png"),
            TextureManager::GetInstance()->GetSrvHandleGPU("test.dds"),
            cubeTransformResource_->GetGPUVirtualAddress(),
            directionalLightResource_->GetGPUVirtualAddress(),
            cameraResource_->GetGPUVirtualAddress()
        );
    }

    // AnimatedCubeの描画
    if (showAnimatedCube_ && animatedCubeRenderModel_ && graphicsPipeline_ && graphicsPipeline_->GetObject3dPipelineState()) {
        ID3D12DescriptorHeap* modelHeaps[] = { TextureManager::GetInstance()->GetSrvHeap() };
        commandList->SetDescriptorHeaps(1, modelHeaps);
        commandList->SetPipelineState(graphicsPipeline_->GetObject3dPipelineState());
        commandList->SetGraphicsRootSignature(graphicsPipeline_->GetObject3dRootSignature());

        if (animatedCubeTransformResource_) commandList->SetGraphicsRootConstantBufferView(1, animatedCubeTransformResource_->GetGPUVirtualAddress());
        if (directionalLightResource_) commandList->SetGraphicsRootConstantBufferView(4, directionalLightResource_->GetGPUVirtualAddress());
        if (cameraResource_) commandList->SetGraphicsRootConstantBufferView(5, cameraResource_->GetGPUVirtualAddress());

        animatedCubeRenderModel_->DrawModel(
            commandList,
            TextureManager::GetInstance()->GetSrvHandleGPU("AnimatedCube/AnimatedCube_BaseColor.png"),
            TextureManager::GetInstance()->GetSrvHandleGPU("test.dds")
        );
    }

    // --- スケルトンのデバッグ描画 ---
    if (showSimpleSkin_ && graphicsPipeline_ && graphicsPipeline_->GetObject3dPipelineState() && graphicsPipeline_->GetObject3dRootSignature()) {
        commandList->SetPipelineState(graphicsPipeline_->GetObject3dPipelineState());
        commandList->SetGraphicsRootSignature(graphicsPipeline_->GetObject3dRootSignature());
        if (directionalLightResource_) commandList->SetGraphicsRootConstantBufferView(4, directionalLightResource_->GetGPUVirtualAddress());
        if (cameraResource_) commandList->SetGraphicsRootConstantBufferView(5, cameraResource_->GetGPUVirtualAddress());
        Matrix4x4 debugBaseWorld = MakeAffineMatrix(cubeRenderModel_->transform.scale, cubeRenderModel_->transform.rotate, { 40.0f, 0.0f, 0.0f });
        DrawSkeleton(cubeSkeleton_, debugBaseWorld);
    }

    // 3. パーティクルの描画（半透明なので最後に描画）
    if (showParticles_) {
        // パイプラインとルートシグネチャのセット
        if (graphicsPipeline_ && graphicsPipeline_->GetRootSignature()) {
            commandList->SetGraphicsRootSignature(graphicsPipeline_->GetRootSignature());
            commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            BlendMode blendMode = useAdditiveBlend_ ? kBlendModeAdd : kBlendModeNormal;
            if (graphicsPipeline_->GetPipelineState(blendMode)) {
                commandList->SetPipelineState(graphicsPipeline_->GetPipelineState(blendMode));
                
                // 自作パーティクルエンジンの描画呼び出し
                particleManager_->Draw(
                    commandList,
                    particleModel_.get(),
                    ringModel_.get(),
                    cylinderModel_.get(),
                    textureSrvHandleGPU_,
                    gradationSrvHandleGPU_,
                    textSrvHandleGPU_
                );
            }

            // スプライト（テキスト）の描画
            if (graphicsPipeline_->GetPipelineState(kBlendModeNormal)) {
                commandList->SetPipelineState(graphicsPipeline_->GetPipelineState(kBlendModeNormal));
                if (particleModel_) {
                    particleModel_->Draw(commandList, kSpriteInstanceCount, textSrvHandleGPU_, spriteInstancingSrvHandleGPU_);
                }
            }

            // HPバーの描画（冒頭の地球→日本→東京夜景→ボス登場演出中は非表示）
            if (!isTitleMode_ && !(bossAppearanceTimer_ > 0.0f) && graphicsPipeline_->GetPipelineState(kBlendModeNormal)) {
                commandList->SetPipelineState(graphicsPipeline_->GetPipelineState(kBlendModeNormal));
                if (particleModel_) {
                    // ボスフェーズのときはボスHPバーも含めた5インスタンス、それ以外はプレイヤーHPバーのみの2インスタンスを描画
                    UINT hpBarDrawCount = (currentPhase_ == GamePhase::kBossFight) ? 5 : 2;
                    particleModel_->Draw(
                        commandList, 
                        hpBarDrawCount, 
                        TextureManager::GetInstance()->GetSrvHandleGPU("human/white.png"), 
                        hpBarInstancingSrvHandleGPU_
                    );
                }
            }

            // 画面蜘蛛の巣の描画（2D）
            if (screenWebTimer_ > 0.0f && !isTitleMode_ && graphicsPipeline_->GetPipelineState(kBlendModeNormal)) {
                commandList->SetPipelineState(graphicsPipeline_->GetPipelineState(kBlendModeNormal));
                if (particleModel_) {
                    particleModel_->Draw(commandList, 1, spiderWebSrvHandleGPU_, screenWebSrvHandleGPU_);
                }
            }

            // ── フェーズ演出スプライトの描画 (PHASE 1 / 2 高速ブースト＆ソニックブーム演出) ──
            if (phaseIntroTimer_ >= 0.0f && !isTitleMode_) {
                float halfClientW = (float)WinApp::kClientWidth / 2.0f;
                float halfClientH = (float)WinApp::kClientHeight / 2.0f;
                float clientW = (float)WinApp::kClientWidth;
                float clientH = (float)WinApp::kClientHeight;
                Matrix4x4 projectionSprite = MakeOrthographicMatrix(-halfClientW, halfClientH, halfClientW, -halfClientH, 0.0f, 100.0f);
                Matrix4x4 viewProjSprite = Multiply(MakeIdentity4x4(), projectionSprite);

                if (graphicsPipeline_ && graphicsPipeline_->GetSpriteRootSignature() && graphicsPipeline_->GetSpritePipelineState()) {
                    commandList->SetGraphicsRootSignature(graphicsPipeline_->GetSpriteRootSignature());
                    commandList->SetPipelineState(graphicsPipeline_->GetSpritePipelineState());

                    // 中心基準＆任意のZ回転でスプライトを描画するヘルパーラムダ
                    auto drawCenteredSprite = [&](Sprite* spr, float cx, float cy, float w, float h, float rotZ, const Vector4& col) {
                        if (!spr || w <= 0.1f || h <= 0.1f || col.w <= 0.001f) return;
                        float sx = w;
                        float sy = -h; // 正射影(+Yが上)での上下反転補正
                        float cosR = std::cos(rotZ);
                        float sinR = std::sin(rotZ);
                        spr->transform.scale = { sx, sy, 1.0f };
                        spr->transform.rotate = { 0.0f, 0.0f, rotZ };
                        spr->transform.translate = {
                            cx - (0.5f * sx * cosR + 0.5f * sy * sinR),
                            cy - (-0.5f * sx * sinR + 0.5f * sy * cosR),
                            0.0f
                        };
                        spr->SetColor(col);
                        spr->Draw(commandList, viewProjSprite);
                    };

                    float t = phaseIntroTimer_;
                    // PHASE 1: サイバーシアン / PHASE 2: オーバードライブ・アンバーゴールド
                    Vector3 themePrimary = (phaseIntroNum_ == 1) ? Vector3{ 0.10f, 0.88f, 1.0f } : Vector3{ 1.0f, 0.45f, 0.08f };
                    Vector3 themeAccent  = (phaseIntroNum_ == 1) ? Vector3{ 0.65f, 0.98f, 1.0f } : Vector3{ 1.0f, 0.92f, 0.30f };

                    // 帯の開閉率 (0.0s〜0.30sで展開、退場時は縦につぶさず左右スライド＆フェードで消去)
                    float barOpen = 1.0f;
                    float exitFade = 1.0f;
                    float exitSplitX = 0.0f;
                    if (t < 0.30f) {
                        float u = t / 0.30f;
                        barOpen = 1.0f - (1.0f - u) * (1.0f - u);
                    } else if (t >= 1.75f) {
                        float u = std::clamp((t - 1.75f) / (kPhaseIntroDuration - 1.75f), 0.0f, 1.0f);
                        exitFade = std::clamp(1.0f - std::pow(u, 1.45f), 0.0f, 1.0f);
                        exitSplitX = 560.0f * (0.40f * u + 0.60f * u * u);
                    }

                    // 1. 上下シネマティック・レターボックス帯＆ネオンエッジライン
                    float letterboxH = 52.0f * barOpen * exitFade;
                    if (letterboxH > 0.5f) {
                        drawCenteredSprite(phaseIntroBandSprites_[3].get(), 0.0f,  halfClientH - letterboxH * 0.5f, clientW + 40.0f, letterboxH, 0.0f, { 0.02f, 0.04f, 0.09f, 0.86f * barOpen * exitFade });
                        drawCenteredSprite(phaseIntroBandSprites_[4].get(), 0.0f, -halfClientH + letterboxH * 0.5f, clientW + 40.0f, letterboxH, 0.0f, { 0.02f, 0.04f, 0.09f, 0.86f * barOpen * exitFade });
                        drawCenteredSprite(phaseIntroStreakSprites_[6].get(), 0.0f,  halfClientH - letterboxH, clientW + 40.0f, 3.5f, 0.0f, { themePrimary.x, themePrimary.y, themePrimary.z, 0.95f * barOpen * exitFade });
                        drawCenteredSprite(phaseIntroStreakSprites_[7].get(), 0.0f, -halfClientH + letterboxH, clientW + 40.0f, 3.5f, 0.0f, { themePrimary.x, themePrimary.y, themePrimary.z, 0.95f * barOpen * exitFade });
                    }

                    // 2. 中央タクティカル・サイバーバイザー帯（退場時は縦につぶれずフェードアウト）
                    float visorTilt = -0.032f; // 約 -1.8度
                    float visorH = 152.0f * barOpen;
                    if (t >= 0.42f && t < 0.58f) {
                        // ソニックブーム着弾の瞬間にバイザー帯も一瞬広がる
                        float impactBump = 1.0f - (t - 0.42f) / 0.16f;
                        visorH += 36.0f * impactBump * impactBump;
                    }
                    drawCenteredSprite(phaseIntroBandSprites_[0].get(), 0.0f, 0.0f, clientW + 120.0f, (std::max)(2.0f, visorH), visorTilt, { 0.01f, 0.03f, 0.08f, 0.72f * barOpen * exitFade });

                    // バイザー上下のレーザーレール（突入時は左右からスライドイン、退場時は左右へ分かれて移動）
                    float railSlide = (t < 0.35f) ? (1.0f - std::pow(1.0f - t / 0.35f, 3.0f)) : 1.0f;
                    float railOffsetX = (1.0f - railSlide) * 900.0f + exitSplitX;
                    float halfVisorH = visorH * 0.5f;
                    drawCenteredSprite(phaseIntroBandSprites_[1].get(), -railOffsetX,  halfVisorH, clientW + 120.0f, 4.5f, visorTilt, { themeAccent.x, themeAccent.y, themeAccent.z, 0.95f * barOpen * exitFade });
                    drawCenteredSprite(phaseIntroBandSprites_[2].get(),  railOffsetX, -halfVisorH, clientW + 120.0f, 4.5f, visorTilt, { themePrimary.x, themePrimary.y, themePrimary.z, 0.95f * barOpen * exitFade });

                    // 3. バイザー内を高速で駆け抜けるソニック・スピードストリーク（4本）＆左右の回転サイバーダイヤモンド（2個）
                    const float streakOffsetsY[4] = { 48.0f, -34.0f, 18.0f, -56.0f };
                    const float streakWidths[4]   = { 420.0f, 540.0f, 310.0f, 480.0f };
                    const float streakSpeeds[4]   = { 2400.0f, -2700.0f, 3100.0f, -2200.0f };
                    for (int i = 0; i < 4; ++i) {
                        float travel = std::fmod(t * streakSpeeds[i] + i * 370.0f, clientW + 600.0f);
                        float sx = (streakSpeeds[i] > 0.0f) ? (-halfClientW - 300.0f + travel) : (halfClientW + 300.0f - travel);
                        float sy = streakOffsetsY[i] * barOpen + sx * std::tan(-visorTilt);
                        Vector3 sCol = (i % 2 == 0) ? themeAccent : themePrimary;
                        drawCenteredSprite(phaseIntroStreakSprites_[i].get(), sx, sy, streakWidths[i], 3.0f, visorTilt, { sCol.x, sCol.y, sCol.z, 0.65f * barOpen * exitFade });
                    }

                    // 左右の回転サイバーダイヤモンド・エンブレム（退場時は左右へ一緒に移動して消える）
                    if (barOpen * exitFade > 0.05f) {
                        float diaRot = static_cast<float>(M_PI) * 0.25f + t * 4.5f;
                        float diaScale = 22.0f * barOpen * (1.0f + 0.15f * std::sin(t * 18.0f));
                        float diaOffsetX = 370.0f + (1.0f - barOpen) * 180.0f + exitSplitX;
                        drawCenteredSprite(phaseIntroStreakSprites_[4].get(), -diaOffsetX, -diaOffsetX * std::tan(-visorTilt), diaScale, diaScale, diaRot, { themeAccent.x, themeAccent.y, themeAccent.z, 0.85f * barOpen * exitFade });
                        drawCenteredSprite(phaseIntroStreakSprites_[5].get(),  diaOffsetX,  diaOffsetX * std::tan(-visorTilt), diaScale, diaScale, -diaRot, { themeAccent.x, themeAccent.y, themeAccent.z, 0.85f * barOpen * exitFade });
                    }

                    // 4. 「PHASE」＆「1 / 2」本体ロゴと残像オーラスプライトの計算・描画
                    if (phaseIntroSprite_ && numberIntroSprite_) {
                        float w1 = (float)phaseIntroSprite_->GetMetadata().width;
                        float h1 = (float)phaseIntroSprite_->GetMetadata().height;
                        float w2 = (float)numberIntroSprite_->GetMetadata().width;
                        float h2 = (float)numberIntroSprite_->GetMetadata().height;
                        float gap = 24.0f;
                        float baseTotalW = w1 + gap + w2;

                        // 毎フレーム最初にテクスチャ切り抜き範囲を全体へリセット（フェーズCでの分割切り抜きをクリア）
                        phaseIntroSprite_->SetTextureRect(0.0f, 0.0f, w1, h1);
                        numberIntroSprite_->SetTextureRect(0.0f, 0.0f, w2, h2);
                        for (int g = 0; g < 2; ++g) {
                            if (phaseIntroGhostSprites_[g])  phaseIntroGhostSprites_[g]->SetTextureRect(0.0f, 0.0f, w1, h1);
                            if (numberIntroGhostSprites_[g]) numberIntroGhostSprites_[g]->SetTextureRect(0.0f, 0.0f, w2, h2);
                        }

                        // 合体時のそれぞれの基準中心座標
                        float baseCenter1X = -baseTotalW * 0.5f + w1 * 0.5f;
                        float baseCenter2X =  baseTotalW * 0.5f - w2 * 0.5f;
                        float baseCenter1Y = 0.0f;
                        float baseCenter2Y = 0.0f;

                        float scaleX = 1.0f;
                        float scaleY = 1.0f;
                        float alpha  = 1.0f;
                        float offset1X = 0.0f, offset1Y = 0.0f;
                        float offset2X = 0.0f, offset2Y = 0.0f;
                        float textRot = 0.0f;

                        if (t < 0.42f) {
                            // フェーズA: 左右から超高速クロス突入 (0.0s 〜 0.42s)
                            float u = t / 0.42f;
                            float easeOut = 1.0f - std::pow(1.0f - u, 4.0f);
                            offset1X = std::lerp(-860.0f, 0.0f, easeOut);
                            offset2X = std::lerp( 860.0f, 0.0f, easeOut);
                            offset1Y = std::lerp( 35.0f,  0.0f, easeOut);
                            offset2Y = std::lerp(-35.0f,  0.0f, easeOut);
                            scaleX = std::lerp(1.85f, 1.0f, easeOut);
                            scaleY = std::lerp(0.65f, 1.0f, easeOut);
                            alpha = std::clamp(u * 2.2f, 0.0f, 1.0f);
                            textRot = (1.0f - easeOut) * -0.06f;

                            // 突入時の2重残像ブラー
                            for (int g = 0; g < 2; ++g) {
                                float lag = (float)(g + 1) * 75.0f * (1.0f - easeOut);
                                Vector3 gCol = (g == 0) ? themeAccent : themePrimary;
                                float gAlpha = alpha * (0.55f - g * 0.20f);
                                drawCenteredSprite(phaseIntroGhostSprites_[g].get(),  baseCenter1X + offset1X - lag, baseCenter1Y + offset1Y, w1 * scaleX, h1 * scaleY, textRot, { gCol.x, gCol.y, gCol.z, gAlpha });
                                drawCenteredSprite(numberIntroGhostSprites_[g].get(), baseCenter2X + offset2X + lag, baseCenter2Y + offset2Y, w2 * scaleX, h2 * scaleY, textRot, { gCol.x, gCol.y, gCol.z, gAlpha });
                            }

                            drawCenteredSprite(phaseIntroSprite_.get(),  baseCenter1X + offset1X, baseCenter1Y + offset1Y, w1 * scaleX, h1 * scaleY, textRot, { 1.0f, 1.0f, 1.0f, alpha });
                            drawCenteredSprite(numberIntroSprite_.get(), baseCenter2X + offset2X, baseCenter2Y + offset2Y, w2 * scaleX, h2 * scaleY, textRot, { 1.0f, 1.0f, 1.0f, alpha });
                        } else if (t < 1.75f) {
                            // フェーズB: ソニックブーム合体ロック＆ブースト巡航 (0.42s 〜 1.75s)
                            float elapsed = t - 0.42f;
                            float impactBounce = 0.0f;
                            if (elapsed < 0.20f) {
                                float bu = 1.0f - (elapsed / 0.20f);
                                impactBounce = 0.34f * bu * bu;
                            }
                            float slowZoom = 0.06f * (elapsed / (1.75f - 0.42f));
                            scaleX = 1.0f + impactBounce + slowZoom;
                            scaleY = 1.0f + impactBounce + slowZoom;

                            // 揺れなしで中央にピタッと固定
                            float shakeX = 0.0f;
                            float shakeY = 0.0f;

                            // 鼓動するソニックオーラ残像（周期的に背面から拡大拡散）
                            for (int g = 0; g < 2; ++g) {
                                float pulsePhase = std::fmod(elapsed * 2.2f + g * 0.5f, 1.0f);
                                float auraScale = scaleX * (1.05f + 0.42f * pulsePhase);
                                float auraAlpha = (1.0f - pulsePhase) * 0.50f;
                                Vector3 gCol = (g == 0) ? themePrimary : themeAccent;
                                drawCenteredSprite(phaseIntroGhostSprites_[g].get(),  (baseCenter1X * auraScale) + shakeX, baseCenter1Y + shakeY, w1 * auraScale, h1 * auraScale, 0.0f, { gCol.x, gCol.y, gCol.z, auraAlpha });
                                drawCenteredSprite(numberIntroGhostSprites_[g].get(), (baseCenter2X * auraScale) + shakeX, baseCenter2Y + shakeY, w2 * auraScale, h2 * auraScale, 0.0f, { gCol.x, gCol.y, gCol.z, auraAlpha });
                            }

                            Vector4 mainColor = { 1.0f, 1.0f, 1.0f, 1.0f };
                            if (phaseIntroNum_ == 2 && t >= 0.55f) {
                                float glowWave = 0.5f + 0.5f * std::sin(t * 16.0f);
                                mainColor = { 1.0f, std::lerp(0.92f, 1.0f, glowWave), std::lerp(0.75f, 1.0f, glowWave), 1.0f };
                            }

                            drawCenteredSprite(phaseIntroSprite_.get(),  baseCenter1X * scaleX + shakeX, baseCenter1Y + shakeY, w1 * scaleX, h1 * scaleY, 0.0f, mainColor);
                            drawCenteredSprite(numberIntroSprite_.get(), baseCenter2X * scaleX + shakeX, baseCenter2Y + shakeY, w2 * scaleX, h2 * scaleY, 0.0f, mainColor);
                        } else {
                            // フェーズC: 中央から真っ二つに斬られて左右へスライドしながら消える演出 (1.75s 〜 2.40s)
                            // ※縦につぶしたり横に引き伸ばしたりせず、1.06倍の美しい比率を維持したまま中央(X=0)で切断して左右へ移動
                            scaleX = 1.06f;
                            scaleY = 1.06f;
                            alpha  = exitFade;

                            float sw1 = w1 * scaleX;
                            float sh1 = h1 * scaleY;
                            float sw2 = w2 * scaleX;
                            float sh2 = h2 * scaleY;
                            float c1BaseX = baseCenter1X * scaleX;
                            float c2BaseX = baseCenter2X * scaleX;

                            // 「PHASE 1 / 2」全体の中心(X = 0.0f)で「PHASE」画像を左右に2分割
                            float x1Left  = c1BaseX - sw1 * 0.5f;
                            float leftRatio = std::clamp((0.0f - x1Left) / sw1, 0.05f, 0.95f);
                            float texLeftW  = w1 * leftRatio;
                            float texRightW = w1 - texLeftW;
                            float screenLeftW  = sw1 * leftRatio;
                            float screenRightW = sw1 - screenLeftW;

                            // 左半分（左へ移動）と右半分（右へ移動）の中心座標
                            float leftPieceCenterX  = x1Left + screenLeftW * 0.5f - exitSplitX;
                            float rightPieceCenterX = 0.0f   + screenRightW * 0.5f + exitSplitX;
                            float numPieceCenterX   = c2BaseX + exitSplitX;

                            // 移動軌跡の残像（左右へ移動する後ろ側に薄く残す）
                            float trailShift = (std::min)(55.0f, exitSplitX * 0.22f);
                            float ghostAlpha = alpha * 0.45f;
                            if (phaseIntroGhostSprites_[1]) {
                                phaseIntroGhostSprites_[1]->SetTextureRect(0.0f, 0.0f, texLeftW, h1);
                                drawCenteredSprite(phaseIntroGhostSprites_[1].get(), leftPieceCenterX + trailShift, 0.0f, screenLeftW, sh1, 0.0f, { themeAccent.x, themeAccent.y, themeAccent.z, ghostAlpha });
                            }
                            if (numberIntroGhostSprites_[0]) {
                                numberIntroGhostSprites_[0]->SetTextureRect(0.0f, 0.0f, w2, h2);
                                drawCenteredSprite(numberIntroGhostSprites_[0].get(), numPieceCenterX - trailShift, 0.0f, sw2, sh2, 0.0f, { themeAccent.x, themeAccent.y, themeAccent.z, ghostAlpha });
                            }

                            // 本体：左半分（「PHASE」の左側）を左へスライド描画
                            phaseIntroSprite_->SetTextureRect(0.0f, 0.0f, texLeftW, h1);
                            drawCenteredSprite(phaseIntroSprite_.get(), leftPieceCenterX, 0.0f, screenLeftW, sh1, 0.0f, { 1.0f, 1.0f, 1.0f, alpha });

                            // 本体：右半分（「PHASE」の右側 ＋ 数字「1 / 2」）を右へスライド描画
                            if (phaseIntroGhostSprites_[0]) {
                                phaseIntroGhostSprites_[0]->SetTextureRect(texLeftW, 0.0f, texRightW, h1);
                                drawCenteredSprite(phaseIntroGhostSprites_[0].get(), rightPieceCenterX, 0.0f, screenRightW, sh1, 0.0f, { 1.0f, 1.0f, 1.0f, alpha });
                            }
                            drawCenteredSprite(numberIntroSprite_.get(), numPieceCenterX, 0.0f, sw2, sh2, 0.0f, { 1.0f, 1.0f, 1.0f, alpha });
                        }
                    }

                    // 5. ソニックブーム合体閃光＆中央一閃スラッシュ（最前面）
                    if (t >= 0.40f && t < 0.70f) {
                        float u = (t - 0.40f) / 0.30f;
                        float beamH = 44.0f * (1.0f - u) * (1.0f - u);
                        float beamAlpha = (1.0f - u);
                        drawCenteredSprite(phaseIntroBandSprites_[5].get(), 0.0f, 0.0f, clientW + 200.0f, beamH, visorTilt, { 1.0f, 1.0f, 1.0f, beamAlpha });
                    } else if (t >= 1.70f && t < 2.15f) {
                        // 中央(X=0)を縦に斬り裂くスラッシュ閃光
                        float u = std::clamp((t - 1.70f) / 0.45f, 0.0f, 1.0f);
                        float slashLen = std::lerp(160.0f, 520.0f, (std::min)(1.0f, u * 3.5f));
                        float slashThickness = 22.0f * (1.0f - u) * (1.0f - u);
                        float slashRot = static_cast<float>(M_PI) * 0.5f - 0.08f; // ほぼ垂直の鋭い斬撃角
                        drawCenteredSprite(phaseIntroBandSprites_[5].get(), 0.0f, 0.0f, slashLen, slashThickness, slashRot, { 1.0f, 1.0f, 1.0f, 1.0f - u });
                    }
                }
            }

            // ── 曲がり角（A / D 長押しルート選択）演出の描画（周囲の薄い赤・長押し中の集中線・A/D長押しUI・間違い時の暗転） ──
            if (cornerState_ != CornerTurnState::kNone && !isTitleMode_ && activeCornerIndex_ >= 0 && activeCornerIndex_ < static_cast<int>(corners_.size())) {
                float clientW = (float)WinApp::kClientWidth;
                float clientH = (float)WinApp::kClientHeight;
                float halfClientW = clientW * 0.5f;
                float halfClientH = clientH * 0.5f;
                Matrix4x4 projectionSprite = MakeOrthographicMatrix(-halfClientW, halfClientH, halfClientW, -halfClientH, 0.0f, 100.0f);
                Matrix4x4 viewProjSprite = Multiply(MakeIdentity4x4(), projectionSprite);

                if (graphicsPipeline_ && graphicsPipeline_->GetSpriteRootSignature() && graphicsPipeline_->GetSpritePipelineState()) {
                    commandList->SetGraphicsRootSignature(graphicsPipeline_->GetSpriteRootSignature());
                    commandList->SetPipelineState(graphicsPipeline_->GetSpritePipelineState());

                    auto drawCornerCentered = [&](Sprite* sp, float cx, float cy, float w, float h, float rotZ, const Vector4& col) {
                        if (!sp || w <= 0.1f || h <= 0.1f || col.w <= 0.001f) return;
                        float sx = w;
                        float sy = -h;
                        float cosR = std::cos(rotZ);
                        float sinR = std::sin(rotZ);
                        sp->transform.scale = { sx, sy, 1.0f };
                        sp->transform.rotate = { 0.0f, 0.0f, rotZ };
                        sp->transform.translate = {
                            cx - (0.5f * sx * cosR + 0.5f * sy * sinR),
                            cy - (-0.5f * sx * sinR + 0.5f * sy * cosR),
                            0.0f
                        };
                        sp->SetColor(col);
                        sp->Draw(commandList, viewProjSprite);
                    };

                    const CornerInfo& corner = corners_[activeCornerIndex_];
                    float pulse = 0.5f + 0.5f * std::sin(cornerTimer_ * 10.0f);

                    // 1. 緊急感を出すため、画面の周りを薄く赤っぽく染める（上下左右の赤いフレーム帯＋極薄の赤オーバーレイ）
                    if (cornerState_ == CornerTurnState::kPromptAndHold || cornerState_ == CornerTurnState::kTurningWrong) {
                        float edgeAlpha = 0.28f + 0.12f * pulse;
                        float innerAlpha = 0.14f + 0.06f * pulse;
                        drawCornerCentered(cornerUiSprites_[0].get(), 0.0f, 0.0f, clientW, clientH, 0.0f, { 1.0f, 0.05f, 0.05f, 0.09f + 0.04f * pulse });
                        // 外周の赤いフレーム帯 (上下左右)
                        drawCornerCentered(cornerUiSprites_[1].get(), 0.0f,  halfClientH - 24.0f, clientW, 48.0f, 0.0f, { 1.0f, 0.06f, 0.06f, edgeAlpha });
                        drawCornerCentered(cornerUiSprites_[2].get(), 0.0f, -halfClientH + 24.0f, clientW, 48.0f, 0.0f, { 1.0f, 0.06f, 0.06f, edgeAlpha });
                        drawCornerCentered(cornerUiSprites_[3].get(), -halfClientW + 28.0f, 0.0f, 56.0f, clientH, 0.0f, { 1.0f, 0.06f, 0.06f, edgeAlpha });
                        drawCornerCentered(cornerUiSprites_[4].get(),  halfClientW - 28.0f, 0.0f, 56.0f, clientH, 0.0f, { 1.0f, 0.06f, 0.06f, edgeAlpha });
                        // 内側のさらなるソフト赤グラデーション段差
                        drawCornerCentered(cornerUiSprites_[5].get(), 0.0f,  halfClientH - 64.0f, clientW, 36.0f, 0.0f, { 1.0f, 0.08f, 0.08f, innerAlpha });
                        drawCornerCentered(cornerUiSprites_[6].get(), 0.0f, -halfClientH + 64.0f, clientW, 36.0f, 0.0f, { 1.0f, 0.08f, 0.08f, innerAlpha });
                        drawCornerCentered(cornerUiSprites_[7].get(), -halfClientW + 76.0f, 0.0f, 44.0f, clientH, 0.0f, { 1.0f, 0.08f, 0.08f, innerAlpha });
                        drawCornerCentered(cornerUiSprites_[8].get(),  halfClientW - 76.0f, 0.0f, 44.0f, clientH, 0.0f, { 1.0f, 0.08f, 0.08f, innerAlpha });
                    }

                    // 2. A / D を長押ししている間（および旋回突破中）はプレイヤーの視界を妨げない控えめな「集中線」を外周に描画！
                    bool showSpeedLines = (cornerState_ == CornerTurnState::kPromptAndHold && cornerHoldingDir_ != 0) ||
                                          (cornerState_ == CornerTurnState::kTurningCorrect);
                    if (showSpeedLines) {
                        float lineIntensity = (cornerState_ == CornerTurnState::kTurningCorrect)
                            ? 0.85f : (0.35f + 0.50f * cornerHoldProgress_);
                        for (int i = 0; i < 24; ++i) {
                            float baseAngle = (static_cast<float>(i) / 24.0f) * 2.0f * static_cast<float>(M_PI);
                            float animPhase = std::fmod(cornerTimer_ * 8.0f + static_cast<float>(i) * 0.37f, 1.0f);
                            float angle = baseAngle + 0.03f * std::sin(static_cast<float>(i) * 3.1f);
                            // 画面中央のプレイヤーには絶対にかぶらないよう、外周付近（半径520〜720px）に控えめに配置
                            float radius = std::lerp(720.0f, 520.0f, animPhase);
                            float lineLen = std::lerp(110.0f, 185.0f, lineIntensity) * (0.75f + 0.25f * std::sin(static_cast<float>(i) * 1.9f));
                            float lineThick = (i % 3 == 0) ? 2.2f : 1.4f;
                            float lx = std::cos(angle) * radius;
                            float ly = std::sin(angle) * radius;
                            Vector4 lineCol = (i % 2 == 0)
                                ? Vector4{ 1.0f, 0.96f, 0.92f, 0.28f * lineIntensity * (1.0f - animPhase * 0.5f) }
                                : Vector4{ 0.55f, 0.95f, 1.0f, 0.24f * lineIntensity * (1.0f - animPhase * 0.5f) };
                            drawCornerCentered(cornerSpeedLineSprites_[i].get(), lx, ly, lineLen, lineThick, angle, lineCol);
                        }
                    }

                    // 3. 左右の「[A] 左へ曲がる」「[D] 右へ曲がる」長押しUIとゲージの描画
                    if (cornerState_ == CornerTurnState::kPromptAndHold) {
                        const float leftX  = -350.0f;
                        const float rightX =  350.0f;
                        const float uiY    = -20.0f;

                        bool isLeftCorrect  = (corner.correctDir < 0);
                        bool isRightCorrect = (corner.correctDir > 0);
                        float leftHold  = (cornerHoldingDir_ < 0) ? cornerHoldProgress_ : 0.0f;
                        float rightHold = (cornerHoldingDir_ > 0) ? cornerHoldProgress_ : 0.0f;

                        // 左パネル [A] のカラー（正解ルート側は明るく発光して敵/ボスの逃走方向を示唆）
                        Vector4 leftBaseCol = isLeftCorrect
                            ? Vector4{ 0.15f, 0.92f, 1.0f, 0.85f + 0.15f * pulse }
                            : Vector4{ 0.75f, 0.78f, 0.85f, 0.55f };
                        if (leftHold > 0.0f) leftBaseCol = { 1.0f, 0.92f, 0.25f, 1.0f };

                        // 右パネル [D] のカラー
                        Vector4 rightBaseCol = isRightCorrect
                            ? Vector4{ 0.15f, 0.92f, 1.0f, 0.85f + 0.15f * pulse }
                            : Vector4{ 0.75f, 0.78f, 0.85f, 0.55f };
                        if (rightHold > 0.0f) rightBaseCol = { 1.0f, 0.92f, 0.25f, 1.0f };

                        // ── 左側 [A] キーボックス＆文字「A」＆左矢印「<<」＆長押しゲージ ──
                        float lScale = 1.0f + 0.12f * leftHold + (isLeftCorrect ? 0.05f * pulse : 0.0f);
                        drawCornerCentered(cornerUiSprites_[9].get(),  leftX, uiY, 96.0f * lScale, 96.0f * lScale, 0.0f, { 0.04f, 0.07f, 0.12f, 0.84f });
                        drawCornerCentered(cornerUiSprites_[10].get(), leftX, uiY + 46.0f * lScale, 96.0f * lScale, 4.5f, 0.0f, leftBaseCol);
                        drawCornerCentered(cornerUiSprites_[11].get(), leftX, uiY - 46.0f * lScale, 96.0f * lScale, 4.5f, 0.0f, leftBaseCol);
                        drawCornerCentered(cornerUiSprites_[12].get(), leftX - 46.0f * lScale, uiY, 4.5f, 96.0f * lScale, 0.0f, leftBaseCol);
                        drawCornerCentered(cornerUiSprites_[13].get(), leftX + 46.0f * lScale, uiY, 4.5f, 96.0f * lScale, 0.0f, leftBaseCol);

                        // 文字「A」(左斜め線・右斜め線・中央横線)
                        drawCornerCentered(cornerUiSprites_[14].get(), leftX - 10.0f * lScale, uiY + 2.0f, 8.0f * lScale, 48.0f * lScale,  0.24f, leftBaseCol);
                        drawCornerCentered(cornerUiSprites_[15].get(), leftX + 10.0f * lScale, uiY + 2.0f, 8.0f * lScale, 48.0f * lScale, -0.24f, leftBaseCol);
                        drawCornerCentered(cornerUiSprites_[16].get(), leftX, uiY - 5.0f * lScale, 24.0f * lScale, 7.5f * lScale, 0.0f, leftBaseCol);

                        // 左シェブロン矢印「<<」（敵・ボスが左へ曲がった場合は大きくスライドアニメーション）
                        float lArrowSlide = isLeftCorrect ? (-14.0f * std::fmod(cornerTimer_ * 2.4f, 1.0f)) : 0.0f;
                        drawCornerCentered(cornerUiSprites_[17].get(), leftX - 82.0f + lArrowSlide, uiY + 13.0f, 34.0f, 7.5f,  0.65f, leftBaseCol);
                        drawCornerCentered(cornerUiSprites_[18].get(), leftX - 82.0f + lArrowSlide, uiY - 13.0f, 34.0f, 7.5f, -0.65f, leftBaseCol);
                        drawCornerCentered(cornerUiSprites_[19].get(), leftX - 112.0f + lArrowSlide, uiY + 13.0f, 34.0f, 7.5f,  0.65f, leftBaseCol);
                        drawCornerCentered(cornerUiSprites_[20].get(), leftX - 112.0f + lArrowSlide, uiY - 13.0f, 34.0f, 7.5f, -0.65f, leftBaseCol);

                        // 左長押しプログレスバー（ボックスの下）
                        const float gaugeW = 140.0f;
                        const float gaugeH = 16.0f;
                        drawCornerCentered(cornerUiSprites_[21].get(), leftX, uiY - 72.0f, gaugeW, gaugeH, 0.0f, { 0.08f, 0.10f, 0.15f, 0.85f });
                        if (leftHold > 0.001f) {
                            float fillW = (gaugeW - 4.0f) * leftHold;
                            drawCornerCentered(cornerUiSprites_[22].get(), leftX - (gaugeW - 4.0f) * 0.5f + fillW * 0.5f, uiY - 72.0f, fillW, gaugeH - 4.0f, 0.0f, { 1.0f, 0.88f, 0.20f, 0.98f });
                        }

                        // ── 右側 [D] キーボックス＆文字「D」＆右矢印「>>」＆長押しゲージ ──
                        float rScale = 1.0f + 0.12f * rightHold + (isRightCorrect ? 0.05f * pulse : 0.0f);
                        drawCornerCentered(cornerUiSprites_[23].get(), rightX, uiY, 96.0f * rScale, 96.0f * rScale, 0.0f, { 0.04f, 0.07f, 0.12f, 0.84f });
                        drawCornerCentered(cornerUiSprites_[24].get(), rightX, uiY + 46.0f * rScale, 96.0f * rScale, 4.5f, 0.0f, rightBaseCol);
                        drawCornerCentered(cornerUiSprites_[25].get(), rightX, uiY - 46.0f * rScale, 96.0f * rScale, 4.5f, 0.0f, rightBaseCol);
                        drawCornerCentered(cornerUiSprites_[26].get(), rightX - 46.0f * rScale, uiY, 4.5f, 96.0f * rScale, 0.0f, rightBaseCol);
                        drawCornerCentered(cornerUiSprites_[27].get(), rightX + 46.0f * rScale, uiY, 4.5f, 96.0f * rScale, 0.0f, rightBaseCol);

                        // 文字「D」(左縦線・上下横線・右縦線)
                        drawCornerCentered(cornerUiSprites_[28].get(), rightX - 13.0f * rScale, uiY, 8.0f * rScale, 46.0f * rScale, 0.0f, rightBaseCol);
                        drawCornerCentered(cornerUiSprites_[29].get(), rightX - 1.0f * rScale,  uiY + 19.0f * rScale, 24.0f * rScale, 7.5f * rScale, -0.12f, rightBaseCol);
                        drawCornerCentered(cornerUiSprites_[30].get(), rightX - 1.0f * rScale,  uiY - 19.0f * rScale, 24.0f * rScale, 7.5f * rScale,  0.12f, rightBaseCol);
                        drawCornerCentered(cornerUiSprites_[31].get(), rightX + 12.0f * rScale, uiY, 8.0f * rScale, 34.0f * rScale, 0.0f, rightBaseCol);

                        // 右シェブロン矢印「>>」
                        float rArrowSlide = isRightCorrect ? (14.0f * std::fmod(cornerTimer_ * 2.4f, 1.0f)) : 0.0f;
                        drawCornerCentered(cornerUiSprites_[32].get(), rightX + 82.0f + rArrowSlide, uiY + 13.0f, 34.0f, 7.5f, -0.65f, rightBaseCol);
                        drawCornerCentered(cornerUiSprites_[33].get(), rightX + 82.0f + rArrowSlide, uiY - 13.0f, 34.0f, 7.5f,  0.65f, rightBaseCol);
                        drawCornerCentered(cornerUiSprites_[34].get(), rightX + 112.0f + rArrowSlide, uiY + 13.0f, 34.0f, 7.5f, -0.65f, rightBaseCol);
                        drawCornerCentered(cornerUiSprites_[35].get(), rightX + 112.0f + rArrowSlide, uiY - 13.0f, 34.0f, 7.5f,  0.65f, rightBaseCol);

                        // 右長押しプログレスバー（ボックスの下）
                        drawCornerCentered(cornerUiSprites_[36].get(), rightX, uiY - 72.0f, gaugeW, gaugeH, 0.0f, { 0.08f, 0.10f, 0.15f, 0.85f });
                        if (rightHold > 0.001f) {
                            float fillW = (gaugeW - 4.0f) * rightHold;
                            drawCornerCentered(cornerUiSprites_[37].get(), rightX - (gaugeW - 4.0f) * 0.5f + fillW * 0.5f, uiY - 72.0f, fillW, gaugeH - 4.0f, 0.0f, { 1.0f, 0.88f, 0.20f, 0.98f });
                        }

                        // 正解ルート（敵・ボスが逃げた方向）を示すターゲットマーカー（ひし形ビーコン）
                        float targetSideX = isLeftCorrect ? leftX : rightX;
                        float beaconRot = static_cast<float>(M_PI) * 0.25f + cornerTimer_ * 4.0f;
                        drawCornerCentered(cornerUiSprites_[38].get(), targetSideX, uiY + 82.0f, 24.0f, 24.0f, beaconRot, { 1.0f, 0.30f, 0.20f, 0.95f });
                        drawCornerCentered(cornerUiSprites_[39].get(), targetSideX, uiY + 82.0f, 12.0f, 12.0f, -beaconRot, { 1.0f, 0.92f, 0.30f, 0.98f });
                    }

                    // 4. 間違えたルートを選択した際、画面全体を徐々に暗くしてゲームオーバーへ遷移する暗転オーバーレイ
                    if (cornerState_ == CornerTurnState::kTurningWrong && wrongRouteDarkenAlpha_ > 0.001f) {
                        drawCornerCentered(cornerUiSprites_[47].get(), 0.0f, 0.0f, clientW + 40.0f, clientH + 40.0f, 0.0f, { 0.0f, 0.0f, 0.0f, wrongRouteDarkenAlpha_ });
                    }
                }
            }

            // ── ボスに接近した際のWARNING演出描画 (7.8秒〜9.8秒の2.0秒間表示) ──
            if (bossAppearanceTimer_ > 0.0f && !isTitleMode_) {
                float t_app = kBossIntroTotalDuration - bossAppearanceTimer_; // 0.0s ～ 12.5s

                // ボスへカメラが寄った後のWARNINGパート（7.8s ～ 9.8s）のみ描画！
                if (t_app >= 7.8f && t_app < 9.8f) {
                    // 既存の2.5秒WARNINGアニメーション(0.0s～2.5s)にスケーリングして再生
                    float t = (t_app - 7.8f) * (2.5f / 2.0f);
                    float clientW = (float)WinApp::kClientWidth;
                    float clientH = (float)WinApp::kClientHeight;
                    float halfClientW = clientW / 2.0f;
                    float halfClientH = clientH / 2.0f;
                    Matrix4x4 projectionSprite = MakeOrthographicMatrix(-halfClientW, halfClientH, halfClientW, -halfClientH, 0.0f, 100.0f);
                    Matrix4x4 viewProjSprite = Multiply(MakeIdentity4x4(), projectionSprite);

                    if (graphicsPipeline_ && graphicsPipeline_->GetSpriteRootSignature() && graphicsPipeline_->GetSpritePipelineState()) {
                        commandList->SetGraphicsRootSignature(graphicsPipeline_->GetSpriteRootSignature());
                        commandList->SetPipelineState(graphicsPipeline_->GetSpritePipelineState());

                        // 中心座標(centerX, centerY)・幅・高さ・回転角(rotZ)から2Dスプライトを正確に中央基準で描画するヘルパー
                        auto drawWarnCentered = [&](Sprite* sp, float cx, float cy, float w, float h, float rotZ, const Vector4& col) {
                            if (!sp || w <= 0.01f || h <= 0.01f || col.w <= 0.001f) return;
                            float cosR = std::cos(rotZ);
                            float sinR = std::sin(rotZ);
                            float tx = cx - (w * 0.5f) * cosR - (h * 0.5f) * sinR;
                            float ty = cy - (w * 0.5f) * sinR + (h * 0.5f) * cosR;
                            sp->transform.translate = { tx, ty, 0.0f };
                            sp->transform.scale = { w, -h, 1.0f };
                            sp->transform.rotate = { 0.0f, 0.0f, rotZ };
                            sp->SetColor(col);
                            sp->Draw(commandList, viewProjSprite);
                        };

                        // 2.0秒～2.5秒の0.5秒間で中央から左右へ斬られてスライド退場
                        float exitU = (t >= 2.0f) ? std::clamp((t - 2.0f) / 0.5f, 0.0f, 1.0f) : 0.0f;
                        float exitEase = exitU * exitU * (2.2f - 1.2f * exitU);
                        float exitSplitX = exitEase * 900.0f;
                        float fadeAlpha = (t >= 2.0f) ? (std::max)(0.0f, 1.0f - std::pow(exitU, 1.5f)) : 1.0f;

                        // 上下警戒枠および中央バイザー帯のスライドインイージング (最初の0.28秒間)
                        float slideIn = (t < 0.28f) ? (t / 0.28f) : 1.0f;
                        slideIn = std::sin(slideIn * static_cast<float>(M_PI) * 0.5f); // 0 -> 1

                        // 0.5秒周期のサイレンビート波形 (0.0s, 0.5s, 1.0s, 1.5s で鋭く跳ねる)
                        float beatPhase = std::fmod(t, 0.50f) / 0.50f;
                        float beatImpact = (t < 2.0f) ? std::pow(1.0f - beatPhase, 3.0f) : 0.0f;

                        // ① 画面全体の赤い緊急パルス背景（ビートに合わせて鋭く明滅）
                        if (warningRedOverlaySprite_) {
                            float redPulse = (0.20f + 0.22f * beatImpact + 0.08f * std::sin(warningScrollOffset_ * 18.0f)) * fadeAlpha;
                            warningRedOverlaySprite_->transform.translate = { -halfClientW, halfClientH, 0.0f };
                            warningRedOverlaySprite_->transform.scale = { clientW, -clientH, 1.0f };
                            warningRedOverlaySprite_->transform.rotate = { 0.0f, 0.0f, 0.0f };
                            warningRedOverlaySprite_->SetColor({ 1.0f, 0.03f, 0.03f, redPulse });
                            warningRedOverlaySprite_->Draw(commandList, viewProjSprite);
                        }

                        // ② 上下の黄色＆黒ストライプバー（4枚の独立Spriteタイルで画面端から端まで途切れなく高速スクロール）
                        {
                            float tapeHeight = 65.0f * slideIn;
                            float tapeWidth = 640.0f;
                            float scrollSpeed = 520.0f; // 高速連続スクロール

                            float topOffset = std::fmod(warningScrollOffset_ * scrollSpeed, tapeWidth);
                            float bottomOffset = -std::fmod(warningScrollOffset_ * scrollSpeed, tapeWidth);

                            // 上部バー (右へスルスル移動) & 下部バー (左へスルスル移動)
                            for (int k = 0; k < 4; ++k) {
                                float topX = -halfClientW - tapeWidth + topOffset + k * tapeWidth;
                                if (warningTapeTopSprites_[k]) {
                                    warningTapeTopSprites_[k]->SetColor({ 1.0f, 1.0f, 1.0f, 1.0f * fadeAlpha });
                                    warningTapeTopSprites_[k]->transform.translate = { topX, halfClientH, 0.0f };
                                    warningTapeTopSprites_[k]->transform.scale = { tapeWidth, -tapeHeight, 1.0f };
                                    warningTapeTopSprites_[k]->Draw(commandList, viewProjSprite);
                                }

                                float botX = -halfClientW - tapeWidth + bottomOffset + k * tapeWidth;
                                if (warningTapeBottomSprites_[k]) {
                                    warningTapeBottomSprites_[k]->SetColor({ 1.0f, 1.0f, 1.0f, 1.0f * fadeAlpha });
                                    warningTapeBottomSprites_[k]->transform.translate = { botX, -halfClientH + tapeHeight, 0.0f };
                                    warningTapeBottomSprites_[k]->transform.scale = { tapeWidth, -tapeHeight, 1.0f };
                                    warningTapeBottomSprites_[k]->Draw(commandList, viewProjSprite);
                                }
                            }

                            // 上下ストライプの内側エッジを彩る赤＆ゴールドの発光レーザーライン (bandSprites 3, 4)
                            float edgeGlow = (0.78f + 0.22f * beatImpact) * fadeAlpha;
                            drawWarnCentered(phaseIntroBandSprites_[3].get(), 0.0f,  halfClientH - tapeHeight - 3.0f, clientW, 4.5f, 0.0f, { 1.0f, 0.22f, 0.10f, edgeGlow });
                            drawWarnCentered(phaseIntroBandSprites_[4].get(), 0.0f, -halfClientH + tapeHeight + 3.0f, clientW, 4.5f, 0.0f, { 1.0f, 0.22f, 0.10f, edgeGlow });
                        }

                        // ③ 中央の緊急タクティカルバイザー帯・高速ストリーク・左右ハザードエンブレム
                        float bandH = (168.0f + 16.0f * beatImpact) * slideIn;
                        if (bandH > 1.0f && fadeAlpha > 0.01f) {
                            // 背面のダーククリムゾン帯 (bandSprites 0)
                            drawWarnCentered(phaseIntroBandSprites_[0].get(), 0.0f, 0.0f, clientW + 160.0f, bandH, 0.0f, { 0.08f, 0.01f, 0.01f, 0.78f * fadeAlpha });

                            // 帯の上下を走るネオンレーザー枠（クリムゾンレッド＆アンバーゴールド）(bandSprites 1, 2)
                            float railAlpha = (0.85f + 0.15f * beatImpact) * fadeAlpha;
                            drawWarnCentered(phaseIntroBandSprites_[1].get(), -exitSplitX,  bandH * 0.5f, clientW + 160.0f, 5.5f, 0.0f, { 1.0f, 0.18f, 0.10f, railAlpha });
                            drawWarnCentered(phaseIntroBandSprites_[2].get(),  exitSplitX, -bandH * 0.5f, clientW + 160.0f, 5.5f, 0.0f, { 1.0f, 0.82f, 0.18f, railAlpha });

                            // 帯の内部を左右に高速疾走する4本のレッド＆ゴールド光条ストリーク (streakSprites 0..3)
                            const float streakY[4]   = { 64.0f, -64.0f, 42.0f, -42.0f };
                            const float streakSpd[4] = { 3100.0f, -3300.0f, -2600.0f, 2900.0f };
                            const float streakW[4]   = { 440.0f, 500.0f, 320.0f, 380.0f };
                            for (int i = 0; i < 4; ++i) {
                                float rawX = std::fmod(t * std::abs(streakSpd[i]) + i * 410.0f, clientW + 900.0f) - (halfClientW + 450.0f);
                                float sx = (streakSpd[i] >= 0.0f) ? rawX : -rawX;
                                sx += (sx < 0.0f) ? -exitSplitX : exitSplitX;
                                Vector4 sCol = (i % 2 == 0) ? Vector4{ 1.0f, 0.85f, 0.20f, 0.80f * fadeAlpha } : Vector4{ 1.0f, 0.18f, 0.12f, 0.80f * fadeAlpha };
                                drawWarnCentered(phaseIntroStreakSprites_[i].get(), sx, streakY[i], streakW[i], 3.5f, 0.0f, sCol);
                            }

                            // 「WARNING」の両脇で回転・脈動する2重ハザード・ダイヤモンドエンブレム (streakSprites 4..7)
                            float diaRot = static_cast<float>(M_PI) * 0.25f + t * 5.0f;
                            float diaOuter = (28.0f + 8.0f * beatImpact) * slideIn;
                            float diaInner = diaOuter * 0.52f;
                            float diaX = 415.0f + (1.0f - slideIn) * 160.0f + exitSplitX;
                            drawWarnCentered(phaseIntroStreakSprites_[4].get(), -diaX, 0.0f, diaOuter, diaOuter,  diaRot, { 1.0f, 0.18f, 0.12f, 0.90f * fadeAlpha });
                            drawWarnCentered(phaseIntroStreakSprites_[5].get(),  diaX, 0.0f, diaOuter, diaOuter, -diaRot, { 1.0f, 0.18f, 0.12f, 0.90f * fadeAlpha });
                            drawWarnCentered(phaseIntroStreakSprites_[6].get(), -diaX, 0.0f, diaInner, diaInner, -diaRot * 1.5f, { 1.0f, 0.88f, 0.22f, 0.95f * fadeAlpha });
                            drawWarnCentered(phaseIntroStreakSprites_[7].get(),  diaX, 0.0f, diaInner, diaInner,  diaRot * 1.5f, { 1.0f, 0.88f, 0.22f, 0.95f * fadeAlpha });
                        }

                        // ④ 画面中央の「WARNING」ロゴ本体＆衝撃波オーラ＆グリッチ＆中央一閃分割退場
                        // ※warning_text.png (800x200) 内で文字が [X:159..512, Y:32..119] に偏っているため、
                        //   文字領域ジャスト [left=156, top=29, width=360, height=94] をクロップして画面の完全な真ん中(0,0)に配置！
                        if (warningTextSprite_) {
                            const float kCropLeft = 156.0f;
                            const float kCropTop  = 29.0f;
                            const float kCropW    = 360.0f;
                            const float kCropH    = 94.0f;

                            // 基準表示サイズ（元の文字比率 360:94 を正確に保ったまま迫力あるサイズで表示）
                            const float kBaseTextW = 650.0f;
                            const float kBaseTextH = 170.0f;

                            // 毎フレーム最初にクロップ領域を文字全体へセット
                            warningTextSprite_->SetTextureRect(kCropLeft, kCropTop, kCropW, kCropH);
                            for (int g = 0; g < 3; ++g) {
                                if (warningTextGhostSprites_[g]) {
                                    warningTextGhostSprites_[g]->SetTextureRect(kCropLeft, kCropTop, kCropW, kCropH);
                                }
                            }

                            if (t < 0.28f) {
                                // フェーズA (0.0s ～ 0.28s): 左右からの高速クロス残像収束＆ズームスラムイン
                                float u = t / 0.28f;
                                float easeOut = 1.0f - std::pow(1.0f - u, 4.0f);
                                float zoomScale = std::lerp(2.25f, 1.0f, easeOut);
                                float alpha = std::clamp(u * 2.5f, 0.0f, 1.0f);

                                float w = kBaseTextW * zoomScale;
                                float h = kBaseTextH * zoomScale;

                                // 左右から中央(0,0)へ高速収束するクリムゾン＆アンバー残像
                                float ghostShift = (1.0f - easeOut) * 640.0f;
                                if (warningTextGhostSprites_[0]) {
                                    drawWarnCentered(warningTextGhostSprites_[0].get(), -ghostShift, 0.0f, w, h, 0.0f, { 1.0f, 0.15f, 0.10f, alpha * 0.65f });
                                }
                                if (warningTextGhostSprites_[1]) {
                                    drawWarnCentered(warningTextGhostSprites_[1].get(),  ghostShift, 0.0f, w, h, 0.0f, { 1.0f, 0.85f, 0.20f, alpha * 0.65f });
                                }

                                drawWarnCentered(warningTextSprite_.get(), 0.0f, 0.0f, w, h, 0.0f, { 1.0f, 1.0f, 1.0f, alpha });
                            } else if (t < 2.00f) {
                                // フェーズB (0.28s ～ 2.00s): サイレン鼓動バウンス・拡散ショックウェーブ残像・サイバーグリッチ
                                float scale = 1.0f + 0.14f * beatImpact;
                                float w = kBaseTextW * scale;
                                float h = kBaseTextH * scale;

                                // 揺れなしで中央にピタッと固定
                                float shakeX = 0.0f;
                                float shakeY = 0.0f;

                                // 1. 背面から周期的に拡大拡散するクリムゾン＆ゴールド衝撃波オーラ
                                if (warningTextGhostSprites_[0]) {
                                    float auraScale = 1.04f + 0.58f * beatPhase;
                                    float auraAlpha = (1.0f - beatPhase) * 0.65f;
                                    drawWarnCentered(warningTextGhostSprites_[0].get(), shakeX, shakeY, kBaseTextW * auraScale, kBaseTextH * auraScale, 0.0f, { 1.0f, 0.22f, 0.12f, auraAlpha });
                                }

                                // 2. サイバーグリッチ残像（横方向へ瞬間的にズレる高輝度アンバーゴースト）
                                if (warningTextGhostSprites_[1]) {
                                    float glitchWave = std::sin(t * 48.0f) * std::cos(t * 31.0f);
                                    float glitchOffsetX = (std::abs(glitchWave) > 0.62f) ? (glitchWave * 28.0f) : (beatImpact * 14.0f);
                                    float glitchOffsetY = (std::abs(glitchWave) > 0.62f) ? -4.0f : 0.0f;
                                    drawWarnCentered(warningTextGhostSprites_[1].get(), shakeX + glitchOffsetX, shakeY + glitchOffsetY, w * 1.02f, h * 1.02f, 0.0f, { 1.0f, 0.85f, 0.20f, 0.48f });
                                }

                                // 3. 本体ロゴ（画面ど真ん中 (0,0) に完全ロック！）
                                float glowTint = 0.88f + 0.12f * beatImpact;
                                drawWarnCentered(warningTextSprite_.get(), shakeX, shakeY, w, h, 0.0f, { 1.0f, glowTint, glowTint, 1.0f });
                            } else {
                                // フェーズC (2.00s ～ 2.50s): 中央(X=0)から真っ二つに斬られて左右へスライドしながら消える演出
                                // ※縦につぶさず美しい比率(1.06倍)を維持したまま、左半分と右半分に分割して左右へ移動
                                float scale = 1.06f;
                                float fullW = kBaseTextW * scale;
                                float fullH = kBaseTextH * scale;
                                float halfScreenW = fullW * 0.5f;
                                float halfTexW    = kCropW * 0.5f;

                                float leftPieceCX  = -halfScreenW * 0.5f - exitSplitX;
                                float rightPieceCX =  halfScreenW * 0.5f + exitSplitX;

                                // 移動軌跡の残像（ghostSprites 1, 2 にそれぞれ独立して割り当て）
                                float trailShift = (std::min)(55.0f, exitSplitX * 0.22f);
                                if (warningTextGhostSprites_[1]) {
                                    warningTextGhostSprites_[1]->SetTextureRect(kCropLeft, kCropTop, halfTexW, kCropH);
                                    drawWarnCentered(warningTextGhostSprites_[1].get(), leftPieceCX + trailShift, 0.0f, halfScreenW, fullH, 0.0f, { 1.0f, 0.25f, 0.12f, fadeAlpha * 0.50f });
                                }
                                if (warningTextGhostSprites_[2]) {
                                    warningTextGhostSprites_[2]->SetTextureRect(kCropLeft + halfTexW, kCropTop, halfTexW, kCropH);
                                    drawWarnCentered(warningTextGhostSprites_[2].get(), rightPieceCX - trailShift, 0.0f, halfScreenW, fullH, 0.0f, { 1.0f, 0.25f, 0.12f, fadeAlpha * 0.50f });
                                }

                                // 本体：左半分（「WAR」）を左へスライド描画
                                warningTextSprite_->SetTextureRect(kCropLeft, kCropTop, halfTexW, kCropH);
                                drawWarnCentered(warningTextSprite_.get(), leftPieceCX, 0.0f, halfScreenW, fullH, 0.0f, { 1.0f, 1.0f, 1.0f, fadeAlpha });

                                // 本体：右半分（「NING」）を右へスライド描画
                                if (warningTextGhostSprites_[0]) {
                                    warningTextGhostSprites_[0]->SetTextureRect(kCropLeft + halfTexW, kCropTop, halfTexW, kCropH);
                                    drawWarnCentered(warningTextGhostSprites_[0].get(), rightPieceCX, 0.0f, halfScreenW, fullH, 0.0f, { 1.0f, 1.0f, 1.0f, fadeAlpha });
                                }
                            }
                        }

                        // ⑤ ロックオン合体閃光＆退場時の中央一閃スラッシュ（最前面: bandSprites 5）
                        if (t >= 0.22f && t < 0.48f) {
                            float u = (t - 0.22f) / 0.26f;
                            float beamH = 40.0f * (1.0f - u) * (1.0f - u);
                            drawWarnCentered(phaseIntroBandSprites_[5].get(), 0.0f, 0.0f, clientW + 200.0f, beamH, 0.0f, { 1.0f, 0.92f, 0.75f, 1.0f - u });
                        } else if (t >= 1.98f && t < 2.38f) {
                            // 中央(X=0)を縦に斬り裂くクリムゾン＆ホワイトの一閃スラッシュ
                            float u = std::clamp((t - 1.98f) / 0.40f, 0.0f, 1.0f);
                            float slashLen = std::lerp(180.0f, 540.0f, (std::min)(1.0f, u * 3.5f));
                            float slashThickness = 24.0f * (1.0f - u) * (1.0f - u);
                            float slashRot = static_cast<float>(M_PI) * 0.5f - 0.06f;
                            drawWarnCentered(phaseIntroBandSprites_[5].get(), 0.0f, 0.0f, slashLen, slashThickness, slashRot, { 1.0f, 0.90f, 0.80f, 1.0f - u });
                        }
                    }
                }

                // ── WARNING終了後、ボス咆哮に合わせて「ボスを倒せ！」カットイン表示（9.8s ～ 12.3s） ──
                if (t_app >= 9.8f && t_app < 12.3f && bossDefeatTextSprite_) {
                    float t_text = t_app - 9.8f; // 0.0s ～ 2.5s
                    float clientW = (float)WinApp::kClientWidth;
                    float clientH = (float)WinApp::kClientHeight;
                    float halfClientW = clientW / 2.0f;
                    float halfClientH = clientH / 2.0f;
                    Matrix4x4 projectionSprite = MakeOrthographicMatrix(-halfClientW, halfClientH, halfClientW, -halfClientH, 0.0f, 100.0f);
                    Matrix4x4 viewProjSprite = Multiply(MakeIdentity4x4(), projectionSprite);

                    if (graphicsPipeline_ && graphicsPipeline_->GetSpriteRootSignature() && graphicsPipeline_->GetSpritePipelineState()) {
                        commandList->SetGraphicsRootSignature(graphicsPipeline_->GetSpriteRootSignature());
                        commandList->SetPipelineState(graphicsPipeline_->GetSpritePipelineState());

                        auto drawCentered = [&](Sprite* sp, float cx, float cy, float w, float h, float rotZ, const Vector4& col) {
                            if (!sp || w <= 0.01f || h <= 0.01f || col.w <= 0.001f) return;
                            float cosR = std::cos(rotZ);
                            float sinR = std::sin(rotZ);
                            float tx = cx - (w * 0.5f) * cosR - (h * 0.5f) * sinR;
                            float ty = cy - (w * 0.5f) * sinR + (h * 0.5f) * cosR;
                            sp->transform.translate = { tx, ty, 0.0f };
                            sp->transform.scale = { w, -h, 1.0f };
                            sp->transform.rotate = { 0.0f, 0.0f, rotZ };
                            sp->SetColor(col);
                            sp->Draw(commandList, viewProjSprite);
                        };

                        // 基準サイズ: 1280x360 の比率を維持した迫力サイズ
                        const float kBaseW = 900.0f;
                        const float kBaseH = 253.0f;
                        const float textPosY = 30.0f; // 画面中央やや上

                        float alpha = 1.0f;
                        float scale = 1.0f;
                        float slideX = 0.0f;

                        if (t_text < 0.25f) {
                            // ① スラムイン (0.0s ～ 0.25s): 巨大スケールから一気に着弾
                            float u = t_text / 0.25f;
                            float ease = 1.0f - std::pow(1.0f - u, 4.0f);
                            scale = std::lerp(2.4f, 1.0f, ease);
                            alpha = std::clamp(u * 3.5f, 0.0f, 1.0f);

                            // 左右から収束するショックウェーブ残像
                            float ghostShift = (1.0f - ease) * 550.0f;
                            if (bossDefeatTextGhostSprites_[0]) {
                                drawCentered(bossDefeatTextGhostSprites_[0].get(), -ghostShift, textPosY, kBaseW * scale, kBaseH * scale, 0.0f, { 1.0f, 0.15f, 0.10f, alpha * 0.65f });
                            }
                            if (bossDefeatTextGhostSprites_[1]) {
                                drawCentered(bossDefeatTextGhostSprites_[1].get(),  ghostShift, textPosY, kBaseW * scale, kBaseH * scale, 0.0f, { 1.0f, 0.85f, 0.20f, alpha * 0.65f });
                            }
                        } else if (t_text < 1.80f) {
                            // ② ボス咆哮と呼応する微細な脈動＆衝撃波オーラ拡散 (0.25s ～ 1.80s)
                            float pulse = std::sin((t_text - 0.25f) * 8.0f);
                            scale = 1.0f + 0.04f * (std::max)(0.0f, pulse);
                            alpha = 1.0f;

                            float auraPhase = std::fmod(t_text - 0.25f, 0.55f) / 0.55f;
                            float auraScale = 1.02f + 0.28f * auraPhase;
                            float auraAlpha = (1.0f - auraPhase) * 0.45f;
                            if (bossDefeatTextGhostSprites_[0]) {
                                drawCentered(bossDefeatTextGhostSprites_[0].get(), 0.0f, textPosY, kBaseW * auraScale, kBaseH * auraScale, 0.0f, { 1.0f, 0.20f, 0.10f, auraAlpha });
                            }
                        } else {
                            // ③ 退場・右へ疾走スライドアウト (1.80s ～ 2.50s)
                            float u = (t_text - 1.80f) / 0.70f;
                            alpha = (std::max)(0.0f, 1.0f - u * u);
                            scale = 1.0f + 0.15f * u;
                            slideX = u * u * 450.0f;
                        }

                        // 背面の半透明ダークシネマ帯
                        if (phaseIntroBandSprites_[0]) {
                            float bandAlpha = 0.52f * alpha;
                            drawCentered(phaseIntroBandSprites_[0].get(), 0.0f, textPosY, clientW + 80.0f, 185.0f * (std::min)(1.0f, scale), 0.0f, { 0.02f, 0.0f, 0.0f, bandAlpha });
                        }
                        // 上下の発光ネオンライン（ゴールド＆クリムゾン）
                        if (phaseIntroBandSprites_[1]) {
                            drawCentered(phaseIntroBandSprites_[1].get(), 0.0f, textPosY + 92.0f, clientW + 80.0f, 3.5f, 0.0f, { 1.0f, 0.85f, 0.20f, 0.85f * alpha });
                        }
                        if (phaseIntroBandSprites_[2]) {
                            drawCentered(phaseIntroBandSprites_[2].get(), 0.0f, textPosY - 92.0f, clientW + 80.0f, 3.5f, 0.0f, { 1.0f, 0.22f, 0.12f, 0.85f * alpha });
                        }

                        // 「ボスを倒せ！」本体描画
                        drawCentered(bossDefeatTextSprite_.get(), slideX, textPosY, kBaseW * scale, kBaseH * scale, 0.0f, { 1.0f, 1.0f, 1.0f, alpha });
                    }
                }
            }
        }

    // ボス死亡演出中は土と石の破片（isihahen.png）を描画しない
    if (gpuParticleManager_ && !isBossDefeatedSequence_) {
        SrvManager::GetInstance()->PreDraw();
        if (triggerDebrisEmit_) {
            gpuParticleManager_->TriggerEmit(debrisEmitPos_, debrisEmitCount_);
            triggerDebrisEmit_ = false;
        }
        gpuParticleManager_->UpdateCS();

        // 頂点バッファとインデックスバッファのポインタを取得してDrawに渡す (安全なバインド順序)
        D3D12_VERTEX_BUFFER_VIEW* pVbView = nullptr;
        D3D12_INDEX_BUFFER_VIEW* pIbView = nullptr;
        D3D12_VERTEX_BUFFER_VIEW vbView{};
        D3D12_INDEX_BUFFER_VIEW ibView{};
        if (particleModel_) {
            vbView = particleModel_->GetVertexBufferView();
            ibView = particleModel_->GetIndexBufferView();
            pVbView = &vbView;
            pIbView = &ibView;
        }

        gpuParticleManager_->Draw(
            commandList, 
            TextureManager::GetInstance()->GetSrvHandleGPU("isihahen.png"),
            pVbView,
            pIbView
        );
    }
    } // End of if (showParticles_)

    // --- 2. RenderTextureから画面（Swapchain）へのコピー ---
    postProcess_->PostDraw();

    // 描画先をバックバッファに戻す
    D3D12_CPU_DESCRIPTOR_HANDLE backBufferHandle = dxCommon_->GetCurrentBackBufferRtvHandle();
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = dxCommon_->GetDsvHandle();
    commandList->OMSetRenderTargets(1, &backBufferHandle, false, &dsvHandle);

    if (isTransitioning_) {
        // 深度バッファをDEPTH_WRITE状態へ安全に遷移
        dxCommon_->TransitionDepthStencilState(D3D12_RESOURCE_STATE_DEPTH_WRITE);
        // 前シーンの3D描画が、今シーンのオブジェクトの深度値で遮蔽されるのを防ぐために深度バッファをクリア
        commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        SceneManager::GetInstance()->DrawPreviousScene();
        // 前のシーンの描画でコンテキスト状態が変更されている可能性があるため再設定
        commandList->OMSetRenderTargets(1, &backBufferHandle, false, &dsvHandle);
    }

    // SrvManagerのデスクリプタヒープをセット
    SrvManager::GetInstance()->PreDraw();

    // 適切なパイプラインを選択
    ID3D12PipelineState* pso = nullptr;
    switch (activePostProcess_) {
    case kNone:
    default:
        pso = graphicsPipeline_->GetFullscreenPipelineState();
        break;
    case kGrayscale:
        pso = graphicsPipeline_->GetGrayscalePipelineState();
        break;
    case kSepia:
        pso = graphicsPipeline_->GetSepiaPipelineState();
        break;
    case kVignette:
        pso = graphicsPipeline_->GetVignettePipelineState();
        break;
    case kBoxFilter:
        pso = graphicsPipeline_->GetBoxFilterPipelineState();
        break;
    case kOutline:
        pso = graphicsPipeline_->GetDepthOutlinePipelineState();
        break;
    case kRadialBlur:
        pso = graphicsPipeline_->GetRadialBlurPipelineState();
        break;
    case kDissolve:
        pso = graphicsPipeline_->GetDissolvePipelineState();
        break;
    case kRandom:
        pso = graphicsPipeline_->GetRandomPipelineState();
        break;
    }

    // Fullscreenパイプラインで描画
    commandList->SetPipelineState(pso);
    if (activePostProcess_ == kDissolve) {
        commandList->SetGraphicsRootSignature(graphicsPipeline_->GetDissolveRootSignature());
        SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(0, postProcess_->GetSrvIndex());
        SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(1, activeNoiseSrvIndex_);
        commandList->SetGraphicsRootConstantBufferView(2, dissolveParamResource_->GetGPUVirtualAddress());
    } else if (activePostProcess_ == kOutline) {
        // 深度バッファをPIXEL_SHADER_RESOURCE状態へ安全に遷移
        dxCommon_->TransitionDepthStencilState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        commandList->SetGraphicsRootSignature(graphicsPipeline_->GetDepthOutlineRootSignature());
        SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(0, postProcess_->GetSrvIndex()); // t0: カラー
        SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(1, depthSrvIndex_);               // t1: 深度
    } else {
        commandList->SetGraphicsRootSignature(graphicsPipeline_->GetFullscreenRootSignature());
        SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(0, postProcess_->GetSrvIndex());
        if (activePostProcess_ == kVignette && vignetteParamResource_) {
            commandList->SetGraphicsRootConstantBufferView(1, vignetteParamResource_->GetGPUVirtualAddress());
        } else if (activePostProcess_ == kBoxFilter && boxFilterParamResource_) {
            commandList->SetGraphicsRootConstantBufferView(1, boxFilterParamResource_->GetGPUVirtualAddress());
        } else if (activePostProcess_ == kRadialBlur && radialBlurParamResource_) {
            commandList->SetGraphicsRootConstantBufferView(1, radialBlurParamResource_->GetGPUVirtualAddress());
        } else if (activePostProcess_ == kRandom && randomParamResource_) {
            commandList->SetGraphicsRootConstantBufferView(1, randomParamResource_->GetGPUVirtualAddress());
        }
    }
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->DrawInstanced(3, 1, 0, 0);
}



void GamePlayScene::DrawSkeleton(const AdvAnim::Skeleton& skeleton, const Matrix4x4& baseWorldMatrix) {
    debugTransformIndex_ = 0;
    Matrix4x4 viewProjectionMatrix = camera_->GetViewProjectionMatrix();
    ID3D12GraphicsCommandList* commandList = dxCommon_->GetCommandList();

    for (const auto& joint : skeleton.joints) {
        if (debugTransformIndex_ >= kMaxDebugInstances) break;

        // Jointの球体描画
        Matrix4x4 sphereWorld = Multiply(joint.skeletonSpaceMatrix, baseWorldMatrix);
        // 必ず見えるように大きくする (0.3 -> 1.0)
        Matrix4x4 sphereFinal = Multiply(MakeScaleMatrix({ 1.0f, 1.0f, 1.0f }), sphereWorld);

        debugTransformData_[debugTransformIndex_]->WVP = Multiply(sphereFinal, viewProjectionMatrix);
        debugTransformData_[debugTransformIndex_]->World = sphereFinal;
        commandList->SetGraphicsRootConstantBufferView(1, debugTransformResources_[debugTransformIndex_]->GetGPUVirtualAddress());
        debugSphereModel_->DrawModel(commandList, TextureManager::GetInstance()->GetSrvHandleGPU("human/white.png"), TextureManager::GetInstance()->GetSrvHandleGPU("test.dds"));
        debugTransformIndex_++;

        // ボーン（線）の描画
        if (joint.parent.has_value() && debugTransformIndex_ < kMaxDebugInstances) {
            const auto& parentJoint = skeleton.joints[*joint.parent];
            Vector3 p1 = { parentJoint.skeletonSpaceMatrix.m[3][0], parentJoint.skeletonSpaceMatrix.m[3][1], parentJoint.skeletonSpaceMatrix.m[3][2] };
            Vector3 p2 = { joint.skeletonSpaceMatrix.m[3][0], joint.skeletonSpaceMatrix.m[3][1], joint.skeletonSpaceMatrix.m[3][2] };

            // baseWorldMatrixを適用した座標を計算
            Vector4 p1W = Multiply(Vector4{ p1.x, p1.y, p1.z, 1.0f }, baseWorldMatrix);
            Vector4 p2W = Multiply(Vector4{ p2.x, p2.y, p2.z, 1.0f }, baseWorldMatrix);
            Vector3 start = { p1W.x, p1W.y, p1W.z };
            Vector3 end = { p2W.x, p2W.y, p2W.z };

            Vector3 diff = { end.x - start.x, end.y - start.y, end.z - start.z };
            float length = std::sqrt(diff.x * diff.x + diff.y * diff.y + diff.z * diff.z);
            if (length > 0.01f) {
                Vector3 direction = { diff.x / length, diff.y / length, diff.z / length };

                // Z軸をdirectionに向け、原点をstartに置く行列を作成
                Matrix4x4 rotateMatrix = MakeIdentity4x4();
                Vector3 up = (std::abs(direction.y) < 0.999f) ? Vector3{ 0, 1, 0 } : Vector3{ 1, 0, 0 };
                Vector3 right = Normalize(Cross(up, direction));
                Vector3 realUp = Cross(direction, right);
                rotateMatrix.m[0][0] = right.x; rotateMatrix.m[0][1] = right.y; rotateMatrix.m[0][2] = right.z;
                rotateMatrix.m[1][0] = realUp.x; rotateMatrix.m[1][1] = realUp.y; rotateMatrix.m[1][2] = realUp.z;
                rotateMatrix.m[2][0] = direction.x; rotateMatrix.m[2][1] = direction.y; rotateMatrix.m[2][2] = direction.z;

                Matrix4x4 boxWorld = Multiply(Multiply(MakeScaleMatrix({ 1.0f, 1.0f, length }), rotateMatrix), MakeTranslateMatrix(start));

                debugTransformData_[debugTransformIndex_]->WVP = Multiply(boxWorld, viewProjectionMatrix);
                debugTransformData_[debugTransformIndex_]->World = boxWorld;
                commandList->SetGraphicsRootConstantBufferView(1, debugTransformResources_[debugTransformIndex_]->GetGPUVirtualAddress());
                debugBoxModel_->DrawModel(commandList, TextureManager::GetInstance()->GetSrvHandleGPU("human/white.png"), TextureManager::GetInstance()->GetSrvHandleGPU("test.dds"));
                debugTransformIndex_++;
            }
        }
    }
}

void GamePlayScene::ApplyGroupFormation(int groupIndex) {
    EnemyGroup& group = enemyGroups_[groupIndex];
    
    for (int idx = 0; idx < kEnemiesPerGroup; ++idx) {
        int enemyIdx = groupIndex * kEnemiesPerGroup + idx;
        Enemy& enemy = enemies_[enemyIdx];
        
        // フォーメーションに応じたレール左右(relX)・上下(relY)・先後(railOfs)オフセット
        float relX = 0.0f, relY = 0.0f, railOfs = 0.0f;
        
        switch (group.formation) {
        case FormationType::kVShape:
            if (idx == 0) { relX =   0.0f; relY = 0.0f; railOfs =   0.0f; }
            else if (idx == 1) { relX = -10.0f; relY = 0.0f; railOfs = -15.0f; }
            else if (idx == 2) { relX =  10.0f; relY = 0.0f; railOfs = -15.0f; }
            else if (idx == 3) { relX = -20.0f; relY = 0.0f; railOfs = -30.0f; }
            else if (idx == 4) { relX =  20.0f; relY = 0.0f; railOfs = -30.0f; }
            break;
        case FormationType::kCircle: {
            float angle = ((float)idx / kEnemiesPerGroup) * 2.0f * (float)M_PI;
            relX = std::cos(angle) * 14.0f;
            relY = std::sin(angle) * 14.0f;
            railOfs = 0.0f;
            break;
        }
        case FormationType::kLineX:
            relX = ((float)idx - (kEnemiesPerGroup - 1) * 0.5f) * 12.0f;
            relY = 0.0f;
            railOfs = 0.0f;
            break;
        case FormationType::kSlant:
            relX = ((float)idx - (kEnemiesPerGroup - 1) * 0.5f) * 12.0f;
            relY = ((float)idx - (kEnemiesPerGroup - 1) * 0.5f) * 6.0f;
            railOfs = ((float)idx - (kEnemiesPerGroup - 1) * 0.5f) * -10.0f;
            break;
        }
        
        enemy.localOffset = { relX, relY, railOfs };
        
        // wanderAnchorをレール空間で記録
        enemy.wanderAnchorRailProgress = group.centerRailProgress + railOfs;
        enemy.wanderAnchorRelX = group.centerX + relX;
        enemy.wanderAnchorRelY = group.centerY + relY;
        if (enemy.wanderAnchorRelY < 5.0f) {
            enemy.wanderAnchorRelY = 5.0f; // 地面に潜るのを防止
        }
        
        // 初期ワールド座標をレールから計算
        Vector3 railPos  = GetRailPosition(enemy.wanderAnchorRailProgress);
        Vector3 railDir  = GetRailDirection(enemy.wanderAnchorRailProgress);
        Vector3 railRight = CalculateRailRight(railDir);
        Vector3 railUp   = CalculateRailUp(railDir, railRight);
        enemy.wanderAnchor = Add(railPos, Add(Scale(railRight, enemy.wanderAnchorRelX), Scale(railUp, enemy.wanderAnchorRelY)));
        
        // railProgress をセット
        enemy.railProgress = enemy.wanderAnchorRailProgress;
        
        // 初期状態は横側待機 (kSideWait)
        enemy.state = Enemy::State::kSideWait;
        enemy.stateTimer = 0.0f;
        enemy.rotate = { 0.0f, 0.0f, 0.0f };
        enemy.relativeZ = 220.0f;
        
        // 「ビルに隠れた状態から真ん中に現れる」を表現するため、初期位置をレール左右の极端(+/-45m)に設定
        float spawnRelX = (idx % 2 == 0) ? -45.0f : 45.0f;
        enemy.position = Add(railPos, Add(Scale(railRight, spawnRelX), Scale(railUp, enemy.wanderAnchorRelY)));
    }
}

void GamePlayScene::RespawnEnemyGroup(int groupIndex, float playerZ) {
    EnemyGroup& group = enemyGroups_[groupIndex];
    
    // フォーメーション形状をランダム決定
    std::uniform_int_distribution<int> distForm(0, (int)FormationType::kCount - 1);
    group.formation = (FormationType)distForm(randomEngine_);
    
    // レール左右方向の相対位置と高さをランダム決定
    std::uniform_real_distribution<float> distX(-10.0f, 10.0f);
    std::uniform_real_distribution<float> distY(5.0f, 20.0f);
    
    // レール累積距離に対する前方オフセット（グループごとに50mずらす）
    float minProgress = 160.0f + (float)groupIndex * 50.0f;
    float maxProgress = 220.0f + (float)groupIndex * 50.0f;
    std::uniform_real_distribution<float> distProgress(minProgress, maxProgress);
    
    group.centerX = distX(randomEngine_);
    group.centerY = distY(randomEngine_);
    group.centerRailProgress = playerZ + distProgress(randomEngine_);
    group.centerZ = group.centerRailProgress; // 互換性のため残留
    
    // 小隊メンバー全員を生存状態（Alive）にして再配置
    for (int idx = 0; idx < kEnemiesPerGroup; ++idx) {
        int enemyIdx = groupIndex * kEnemiesPerGroup + idx;
        enemies_[enemyIdx].isAlive = true;
        enemies_[enemyIdx].hp = 30.0f;
        enemies_[enemyIdx].maxHP = 30.0f;
    }
    
    ApplyGroupFormation(groupIndex);
}

void GamePlayScene::UpdateDemo(float deltaTime) {
    // 0. 数字キーによるプリセット切り替え
    if (input_->IsKeyTriggered(DIK_1)) ApplyPreset(0);
    else if (input_->IsKeyTriggered(DIK_2)) ApplyPreset(1);
    else if (input_->IsKeyTriggered(DIK_3)) ApplyPreset(2);
    else if (input_->IsKeyTriggered(DIK_4)) ApplyPreset(3);
    else if (input_->IsKeyTriggered(DIK_5)) ApplyPreset(4);
    else if (input_->IsKeyTriggered(DIK_6)) ApplyPreset(5);
    else if (input_->IsKeyTriggered(DIK_7)) ApplyPreset(6);
    else if (input_->IsKeyTriggered(DIK_8)) ApplyPreset(7);
    else if (input_->IsKeyTriggered(DIK_9)) ApplyPreset(8);
    else if (input_->IsKeyTriggered(DIK_0)) ApplyPreset(9);

    // 1. ヒットストップ処理
    if (hitstopTimer_ > 0.0f) {
        hitstopTimer_ -= deltaTime;
        // ヒットストップ中は時間の進行を極端に遅くする（スローモーション化）
        deltaTime = deltaTime * 0.05f;
    }

    // 2. カメラシェイク処理
    if (cameraShakeTimer_ > 0.0f) {
        cameraShakeTimer_ -= deltaTime;
        if (cameraShakeTimer_ < 0.0f) cameraShakeTimer_ = 0.0f;
        
        float progress = cameraShakeTimer_ / (cameraShakeTimeMax_ > 0.0f ? cameraShakeTimeMax_ : 1.0f);
        progress = std::clamp(progress, 0.0f, 1.0f);
        // 自乗でイージングアウトさせる
        float currentIntensity = activeShakeIntensity_ * (progress * progress);
        
        std::uniform_real_distribution<float> distShake(-1.0f, 1.0f);
        if (selectedEffectPreset_ == 7) {
            // 風竜巻は横揺れ（Xのみ）で煽られる表現
            cameraShakeOffset_ = {
                distShake(randomEngine_) * currentIntensity,
                0.0f,
                0.0f
            };
        } else {
            cameraShakeOffset_ = {
                distShake(randomEngine_) * currentIntensity,
                distShake(randomEngine_) * currentIntensity,
                distShake(randomEngine_) * currentIntensity
            };
        }
    } else {
        cameraShakeOffset_ = { 0.0f, 0.0f, 0.0f };
    }

    // カメラの座標と角度を設定して更新
    camera_->GetTransform().translate = Add(cameraBasePos_, cameraShakeOffset_);
    camera_->GetTransform().rotate = cameraBaseRot_;
    camera_->Update();

    // 3. インパクトフラッシュの更新
    if (flashAlpha_ > 0.0f) {
        // 神聖属性(Preset 8)はゆっくり余韻を残してフェードアウト、他は高速フェード
        float fadeSpeed = (selectedEffectPreset_ == 8) ? 1.5f : 3.5f;
        flashAlpha_ -= deltaTime * fadeSpeed;
        if (flashAlpha_ < 0.0f) flashAlpha_ = 0.0f;
    }

    // 4. ラジアルブラーの更新
    if (useRadialBlur_ && blurIntensity_ > 0.0f) {
        blurIntensity_ -= deltaTime * (maxBlurWidth_ / 0.25f);
        if (blurIntensity_ < 0.0f) blurIntensity_ = 0.0f;
        
        activePostProcess_ = kRadialBlur;
        if (radialBlurParamData_) {
            radialBlurParamData_->center = { 0.5f, 0.5f };
            radialBlurParamData_->blurWidth = -blurIntensity_;
        }
    } else {
        if (activePostProcess_ == kRadialBlur) {
            activePostProcess_ = kNone;
        }
    }

    // 4.5 デジタルバグ（グリッチノイズポストプロセス）の更新
    if (digitalGlitchTimer_ > 0.0f) {
        digitalGlitchTimer_ -= deltaTime;
        if (digitalGlitchTimer_ < 0.0f) digitalGlitchTimer_ = 0.0f;
        
        activePostProcess_ = kRandom;
        if (randomParamResource_ && randomParamData_) {
            randomParamData_->time = randomEffectTime_;
            randomParamData_->noiseScale = 100.0f;
            randomParamData_->noiseStrength = 0.85f; // 強めのノイズ
            randomParamData_->isColorNoise = false;
            randomParamData_->isMultiplyNoise = false;
        }
    } else {
        if (activePostProcess_ == kRandom) {
            activePostProcess_ = kNone;
        }
    }

    // 5. 攻撃モード制御 ＆ 近接突撃・ビーム射撃処理
    if (input_->IsKeyTriggered(DIK_TAB)) {
        attackMode_ = (attackMode_ == AttackMode::kShooting) ? AttackMode::kMelee : AttackMode::kShooting;
        meleeState_ = MeleeState::kIdle;
        meleeTimer_ = 0.0f;
        currentFighterPos_ = playerPos_;
    }

    bool attackTrigger = input_->IsKeyTriggered(DIK_SPACE);
    
    // オートデモタイマー更新
    if (autoPlay_) {
        autoPlayTimer_ += deltaTime;
        if (autoPlayTimer_ >= autoPlayInterval_) {
            attackTrigger = true;
            autoPlayTimer_ = 0.0f;
        }
    }

    if (attackMode_ == AttackMode::kShooting) {
        // ── 射撃モード ──
        float hoverY = std::sin(randomEffectTime_ * 3.0f) * 0.15f;
        currentFighterPos_ = playerPos_;
        currentFighterPos_.y += hoverY;
        fighterModel_->transform.rotate.z = 0.0f;
        
        if (attackTrigger) {
            for (int i = 0; i < kMaxShotBeams; ++i) {
                if (!shotBeams_[i].isAlive) {
                    shotBeams_[i].position = playerPos_;
                    Vector3 toTarget = Subtract(targetPos_, playerPos_);
                    shotBeams_[i].velocity = Scale(Normalize(toTarget), 150.0f);
                    shotBeams_[i].isAlive = true;
                    if (audio_) {
                        PlaySE(jumpSE_, false, 0.35f);
                    }
                    break;
                }
            }
        }
    } else {
        // ── 近接突撃モード ──
        const float kMeleeDashDuration = 0.12f; // より高速に（7〜8フレーム）
        const float kMeleeHitDuration = 0.06f;
        const float kMeleeReturnDuration = 0.30f;

        if (meleeState_ == MeleeState::kIdle) {
            currentFighterPos_ = playerPos_;
            float hoverY = std::sin(randomEffectTime_ * 3.0f) * 0.15f;
            currentFighterPos_.y += hoverY;
            fighterModel_->transform.rotate.z = 0.0f;

            if (attackTrigger) {
                meleeState_ = MeleeState::kDash;
                meleeTimer_ = 0.0f;
                if (audio_) {
                    PlaySE(jumpSE_, false, 0.7f);
                }
            }
        }
        
        if (meleeState_ == MeleeState::kDash) {
            meleeTimer_ += deltaTime;
            float t = std::clamp(meleeTimer_ / kMeleeDashDuration, 0.0f, 1.0f);
            
            // イージング付き突進
            currentFighterPos_.x = std::lerp(playerPos_.x, targetPos_.x, t * t);
            currentFighterPos_.y = std::lerp(playerPos_.y, targetPos_.y, t * t);
            currentFighterPos_.z = std::lerp(playerPos_.z, targetPos_.z - 1.5f, t * t);
            
            // バレルスピン
            fighterModel_->transform.rotate.z = t * static_cast<float>(M_PI) * 4.0f;
            
            if (t >= 1.0f) {
                meleeState_ = MeleeState::kHit;
                meleeTimer_ = 0.0f;
            }
        }
        
        if (meleeState_ == MeleeState::kHit) {
            currentFighterPos_ = targetPos_;
            currentFighterPos_.z -= 1.5f;
            
            if (meleeTimer_ == 0.0f) {
                EmitHitEffect(targetPos_);
            }
            
            meleeTimer_ += deltaTime;
            if (meleeTimer_ >= kMeleeHitDuration) {
                meleeState_ = MeleeState::kReturn;
                meleeTimer_ = 0.0f;
            }
        }
        
        if (meleeState_ == MeleeState::kReturn) {
            meleeTimer_ += deltaTime;
            float t = std::clamp(meleeTimer_ / kMeleeReturnDuration, 0.0f, 1.0f);
            
            // 元の位置に戻る
            currentFighterPos_.x = std::lerp(targetPos_.x, playerPos_.x, t);
            currentFighterPos_.y = std::lerp(targetPos_.y, playerPos_.y, t);
            currentFighterPos_.z = std::lerp(targetPos_.z - 1.5f, playerPos_.z, t);
            
            // 回転を徐々に戻す
            fighterModel_->transform.rotate.z = (1.0f - t) * static_cast<float>(M_PI) * 4.0f;
            
            if (t >= 1.0f) {
                meleeState_ = MeleeState::kIdle;
                meleeTimer_ = 0.0f;
                fighterModel_->transform.rotate.z = 0.0f;
            }
        }
    }

    // ビーム前進と着弾判定
    for (int i = 0; i < kMaxShotBeams; ++i) {
        if (shotBeams_[i].isAlive) {
            shotBeams_[i].position = Add(shotBeams_[i].position, Scale(shotBeams_[i].velocity, deltaTime));
            
            if (shotBeams_[i].position.z >= targetPos_.z - 0.5f) {
                shotBeams_[i].isAlive = false;
                EmitHitEffect(targetPos_);
            }

            if (bulletTransformResources_[i]) {
                Vector3 vel = shotBeams_[i].velocity;
                Vector3 dir = Normalize(vel);
                Matrix4x4 rotMatrix = MakeLookRotationMatrix(dir);
                Vector3 scaleOuter = { 2.4f, 2.4f, 4.8f };

                bulletTransformData_[i]->World = Multiply(Multiply(MakeScaleMatrix(scaleOuter), rotMatrix), MakeTranslateMatrix(shotBeams_[i].position));
                bulletTransformData_[i]->WVP = Multiply(bulletTransformData_[i]->World, camera_->GetViewProjectionMatrix());
            }
        }
    }

    // 6. パーティクル更新
    if (particleManager_) {
        particleManager_->Update(
            camera_->GetViewProjectionMatrix(),
            camera_->GetBillboardMatrix(),
            deltaTime,
            camera_->GetTransform().translate.z,
            playerPos_,
            false,
            false,
            0,
            targetPos_
        );
    }

    if (gpuParticleManager_) {
        gpuParticleManager_->Update(
            camera_->GetViewProjectionMatrix(),
            camera_->GetBillboardMatrix(),
            deltaTime
        );
    }

    // 7. 自機と標的の行列更新
    if (fighterModel_ && fighterTransformResource_) {
        fighterModel_->transform.translate = currentFighterPos_;
        
        float recoilPitch = 0.0f;
        if (attackMode_ == AttackMode::kShooting && attackTrigger) recoilPitch = 0.12f;
        else if (meleeState_ == MeleeState::kDash) recoilPitch = -0.2f;
        
        fighterModel_->transform.rotate.x = std::lerp(fighterModel_->transform.rotate.x, recoilPitch, 0.1f);
        
        Matrix4x4 worldMatrix;
        if (sceneMode_ == SceneMode::kFighter) {
            Vector3 playerRailPos = GetRailPosition(fighterWorldZ_);
            Vector3 playerRailDir = GetRailDirection(fighterWorldZ_);
            Vector3 playerRailRight = CalculateRailRight(playerRailDir);
            Vector3 playerRailUp = CalculateRailUp(playerRailDir, playerRailRight);

            // ローカルの回転（ロール、ピッチ、ヨウは180度反転）
            float pitch = fighterModel_->transform.rotate.x;
            float yaw_180 = static_cast<float>(M_PI);
            float roll = fighterModel_->transform.rotate.z;

            Matrix4x4 rotateY = MakeRotateYMatrix(yaw_180);
            Matrix4x4 rotateX = MakeRotateXMatrix(pitch);
            Matrix4x4 rotateZ = MakeRotateZMatrix(roll);
            Matrix4x4 R_local = Multiply(Multiply(rotateZ, rotateX), rotateY);

            // レールの回転
            Matrix4x4 R_rail = MakeIdentity4x4();
            R_rail.m[0][0] = playerRailRight.x; R_rail.m[0][1] = playerRailRight.y; R_rail.m[0][2] = playerRailRight.z;
            R_rail.m[1][0] = playerRailUp.x;    R_rail.m[1][1] = playerRailUp.y;    R_rail.m[1][2] = playerRailUp.z;
            R_rail.m[2][0] = playerRailDir.x;   R_rail.m[2][1] = playerRailDir.y;   R_rail.m[2][2] = playerRailDir.z;

            // 最終回転行列
            Matrix4x4 R_final = Multiply(R_local, R_rail);

            // ワールド行列を構築（スケールは 0.5f 固定、回転は R_final、平行移動は currentFighterPos_）
            Matrix4x4 scaleMatrix = Matrix4x4MakeScaleMatrix({0.5f, 0.5f, 0.5f});
            Matrix4x4 translateMatrix = MakeTranslateMatrix(currentFighterPos_);
            worldMatrix = Multiply(Multiply(scaleMatrix, R_final), translateMatrix);
        } else {
            // 通常のアフィン行列
            worldMatrix = MakeAffineMatrix(
                Vector3{0.5f, 0.5f, 0.5f},
                fighterModel_->transform.rotate,
                currentFighterPos_
            );
        }

        fighterTransformData_->World = worldMatrix;
        fighterTransformData_->WVP = Multiply(fighterTransformData_->World, camera_->GetViewProjectionMatrix());
    }

    if (enemyModel_ && enemyTransformResources_[0]) {
        float hoverY = std::cos(randomEffectTime_ * 2.0f) * 0.25f;
        Vector3 renderTargetPos = targetPos_;
        renderTargetPos.y += hoverY;
        
        Vector3 targetRot = { randomEffectTime_ * 0.5f, randomEffectTime_ * 0.8f, 0.0f };
        enemyTransformData_[0]->World = MakeAffineMatrix(
            Vector3{1.2f, 1.2f, 1.2f},
            targetRot,
            renderTargetPos
        );
        enemyTransformData_[0]->WVP = Multiply(enemyTransformData_[0]->World, camera_->GetViewProjectionMatrix());
    }
    
    // スプライト（HPバー等）を非表示化するため、WVPを0行列にする
    for (uint32_t i = 0; i < kHpBarInstanceCount; ++i) {
        hpBarInstancingData_[i].WVP = MakeIdentity4x4();
        hpBarInstancingData_[i].WVP.m[0][0] = 0.0f;
        hpBarInstancingData_[i].WVP.m[1][1] = 0.0f;
        hpBarInstancingData_[i].WVP.m[2][2] = 0.0f;
        hpBarInstancingData_[i].WVP.m[3][3] = 0.0f;
    }
}

void GamePlayScene::EmitHitEffect(const Vector3& pos) {
    bool isMelee = (attackMode_ == AttackMode::kMelee);

    if (isMelee) {
        // ── 近接（打撃・斬撃）モード専用の豪華複合エフェクト ──
        // 基本的な打撃衝撃波リングとシリンダーの重ね合わせ
        particleManager_->EmitRing(pos);
        particleManager_->EmitRing(Add(pos, { 0.0f, 0.0f, 0.8f })); // 二重衝撃波
        particleManager_->EmitCylinder(pos);

        switch (selectedEffectPreset_) {
        case 0: // 通常近接
            // 巨大な十字・X字斬撃軌跡と、重みのあるスパーク
            particleManager_->EmitSlash(pos, effectParticleSpeed_ * 1.5f, static_cast<int>(effectParticleCount_ * 1.2f), effectBaseColor_);
            particleManager_->EmitSlash(pos, effectParticleSpeed_ * 1.2f, static_cast<int>(effectParticleCount_ * 0.8f), { 1.0f, 1.0f, 1.0f });
            particleManager_->EmitCustomSparks(pos, effectParticleSpeed_ * 1.8f, static_cast<int>(effectParticleCount_ * 1.5f), effectBaseColor_, effectGravity_ + 0.5f);
            break;

        case 1: // 火炎近接
            // 巨大炎スラッシュ ＋ 炎バースト ＋ 飛び散る残り火
            particleManager_->EmitFlame(pos, effectParticleSpeed_ * 1.2f, static_cast<int>(effectParticleCount_ * 1.3f), effectBaseColor_);
            particleManager_->EmitSlash(pos, effectParticleSpeed_ * 1.6f, effectParticleCount_, { 1.0f, 0.3f, 0.0f });
            particleManager_->EmitSlash(pos, effectParticleSpeed_ * 1.4f, static_cast<int>(effectParticleCount_ * 0.8f), { 1.0f, 0.8f, 0.2f });
            particleManager_->EmitCylinder(pos);
            particleManager_->EmitCustomSparks(pos, effectParticleSpeed_ * 1.6f, effectParticleCount_, effectBaseColor_, 0.6f);
            break;

        case 2: // 雷撃近接
            // 雷刃十字斬 ＋ 四方からの放電スレッド収縮 ＋ 超高圧放電リング
            particleManager_->EmitLightning(pos, effectParticleSpeed_ * 2.0f, static_cast<int>(effectParticleCount_ * 1.3f), effectBaseColor_);
            particleManager_->EmitSlash(pos, effectParticleSpeed_ * 1.6f, effectParticleCount_, { 0.2f, 0.9f, 1.0f });
            particleManager_->EmitLaserThread(Add(pos, { -12.0f, 0.0f, 0.0f }), pos);
            particleManager_->EmitLaserThread(Add(pos, { 12.0f, 0.0f, 0.0f }), pos);
            particleManager_->EmitLaserThread(Add(pos, { 0.0f, -12.0f, 0.0f }), pos);
            particleManager_->EmitLaserThread(Add(pos, { 0.0f, 12.0f, 0.0f }), pos);
            particleManager_->EmitCylinder(pos);
            break;

        case 3: // 真空斬撃近接
            // 鋭く交叉する真空の風刃（4枚刃） ＋ 多重真空リング
            particleManager_->EmitSlash(pos, effectParticleSpeed_ * 1.8f, static_cast<int>(effectParticleCount_ * 1.5f), effectBaseColor_);
            particleManager_->EmitSlash(pos, effectParticleSpeed_ * 1.5f, effectParticleCount_, { 0.7f, 1.0f, 0.8f });
            particleManager_->EmitSlash(pos, effectParticleSpeed_ * 1.3f, static_cast<int>(effectParticleCount_ * 0.8f), { 1.0f, 1.0f, 1.0f });
            particleManager_->EmitRing(Add(pos, { 0.0f, 0.0f, -0.8f }));
            particleManager_->EmitRing(Add(pos, { 0.0f, 0.0f, 0.8f }));
            break;

        case 4: // 特異点近接
            // 重力吸い込みから瞬時に空間を切り裂く重力崩壊斬撃
            particleManager_->EmitGravityVortex(pos, effectParticleSpeed_ * 0.9f, static_cast<int>(effectParticleCount_ * 1.3f), effectBaseColor_);
            particleManager_->EmitGravityOut(pos, static_cast<int>(effectParticleCount_ * 1.3f), effectBaseColor_);
            particleManager_->EmitSlash(pos, effectParticleSpeed_ * 1.6f, effectParticleCount_, { 0.6f, 0.1f, 0.9f });
            particleManager_->EmitCylinder(pos);
            break;

        case 5: // 氷結近接
            // 巨大氷華の破砕 ＋ 氷の鋭いクロス斬撃
            particleManager_->EmitGlacial(pos, effectParticleSpeed_ * 1.2f, static_cast<int>(effectParticleCount_ * 1.4f), effectBaseColor_);
            particleManager_->EmitSlash(pos, effectParticleSpeed_ * 1.6f, effectParticleCount_, { 0.5f, 0.8f, 1.0f });
            particleManager_->EmitRing(pos);
            particleManager_->EmitRing(Add(pos, { 0.0f, 0.0f, -1.0f }));
            break;

        case 6: // デジタル近接
            // デジタルバグ ＋ マゼンタとグリーンの交差スライスカッター
            particleManager_->EmitDigitalGlitch(pos, effectParticleSpeed_ * 1.3f, static_cast<int>(effectParticleCount_ * 1.4f), effectBaseColor_);
            particleManager_->EmitSlash(pos, effectParticleSpeed_ * 1.5f, effectParticleCount_, { 0.0f, 1.0f, 0.4f });
            particleManager_->EmitSlash(pos, effectParticleSpeed_ * 1.5f, effectParticleCount_, { 1.0f, 0.0f, 0.8f });
            particleManager_->EmitCylinder(pos);
            break;

        case 7: // 風竜巻近接
            // 竜巻渦巻き ＋ 高速回転する風の斬撃
            particleManager_->EmitAeroWind(pos, effectParticleSpeed_ * 1.2f, static_cast<int>(effectParticleCount_ * 1.3f), effectBaseColor_);
            particleManager_->EmitSlash(pos, effectParticleSpeed_ * 1.8f, effectParticleCount_, { 0.8f, 1.0f, 0.9f });
            particleManager_->EmitCylinder(pos);
            particleManager_->EmitRing(pos);
            break;

        case 8: // 神聖近接
            // 天から降り注ぐ極太の光の柱 ＋ 黄金の十字斬撃 ＋ 聖なる光輪
            particleManager_->EmitHolyLight(pos, effectParticleSpeed_ * 1.2f, static_cast<int>(effectParticleCount_ * 1.3f), effectBaseColor_);
            particleManager_->EmitSlash(pos, effectParticleSpeed_ * 1.6f, effectParticleCount_, { 1.0f, 0.95f, 0.7f });
            particleManager_->EmitLaserThread(Add(pos, { 0.0f, 25.0f, 0.0f }), pos);
            particleManager_->EmitLaserThread(Add(pos, { -5.0f, 20.0f, -3.0f }), pos);
            particleManager_->EmitLaserThread(Add(pos, { 5.0f, 20.0f, 3.0f }), pos);
            particleManager_->EmitCylinder(pos);
            break;

        case 9: // カオス近接
            // 混沌物質の炸裂 ＋ 闇の稲妻 ＋ 混沌の衝撃斬撃 ＋ 特異点渦
            particleManager_->EmitChaosVoid(pos, effectParticleSpeed_ * 1.2f, static_cast<int>(effectParticleCount_ * 1.3f), effectBaseColor_);
            particleManager_->EmitLightning(pos, effectParticleSpeed_ * 1.8f, static_cast<int>(effectParticleCount_ * 0.8f), { 0.8f, 0.0f, 0.9f });
            particleManager_->EmitSlash(pos, effectParticleSpeed_ * 1.6f, effectParticleCount_, { 0.4f, 0.0f, 0.6f });
            particleManager_->EmitGravityVortex(pos, effectParticleSpeed_ * 0.8f, static_cast<int>(effectParticleCount_ * 0.6f), { 0.1f, 0.0f, 0.2f });
            particleManager_->EmitCylinder(pos);
            break;
        }
    } else {
        // ── 射撃（ビームヒット）モードのエフェクト（既存） ──
        switch (selectedEffectPreset_) {
        case 0: // 通常スパーク
            particleManager_->EmitCustomSparks(pos, effectParticleSpeed_, effectParticleCount_, effectBaseColor_, effectGravity_);
            particleManager_->EmitRing(pos, effectBaseColor_);
            break;
            
        case 1: // 火炎バースト
            particleManager_->EmitFlame(pos, effectParticleSpeed_ * 0.8f, effectParticleCount_, effectBaseColor_);
            particleManager_->EmitCylinder(pos, effectBaseColor_);
            particleManager_->EmitRing(pos, effectBaseColor_);
            particleManager_->EmitCustomSparks(pos, effectParticleSpeed_ * 1.5f, effectParticleCount_ / 2, effectBaseColor_, 0.45f);
            break;
            
        case 2: // 超高圧放電
            particleManager_->EmitLightning(pos, effectParticleSpeed_ * 1.8f, effectParticleCount_, effectBaseColor_);
            particleManager_->EmitLaserThread(Add(pos, {-8.0f,0.0f,0.0f}), pos);
            particleManager_->EmitLaserThread(Add(pos, {8.0f,0.0f,0.0f}), pos);
            particleManager_->EmitLaserThread(Add(pos, {0.0f,-8.0f,0.0f}), pos);
            particleManager_->EmitLaserThread(Add(pos, {0.0f,8.0f,0.0f}), pos);
            particleManager_->EmitCylinder(pos, effectBaseColor_);
            break;
            
        case 3: // 真空斬撃
            particleManager_->EmitSlash(pos, effectParticleSpeed_ * 1.5f, effectParticleCount_, effectBaseColor_);
            particleManager_->EmitRing(pos, effectBaseColor_);
            particleManager_->EmitRing(Add(pos, {0.0f, 0.0f, 1.0f}), effectBaseColor_);
            break;
            
        case 4: // 特異点爆発
            particleManager_->EmitGravityVortex(pos, effectParticleSpeed_ * 0.8f, effectParticleCount_, effectBaseColor_);
            particleManager_->EmitCylinder(pos, effectBaseColor_);
            particleManager_->EmitRing(pos, effectBaseColor_);
            particleManager_->EmitGravityOut(pos, effectParticleCount_, effectBaseColor_);
            break;

        case 5: // 氷結・氷華
            particleManager_->EmitGlacial(pos, effectParticleSpeed_, effectParticleCount_, effectBaseColor_);
            break;
            
        case 6: // デジタルバグ
            particleManager_->EmitDigitalGlitch(pos, effectParticleSpeed_, effectParticleCount_, effectBaseColor_);
            break;

        case 7: // 風・竜巻
            particleManager_->EmitAeroWind(pos, effectParticleSpeed_, effectParticleCount_, effectBaseColor_);
            particleManager_->EmitRing(pos, effectBaseColor_);
            particleManager_->EmitCylinder(pos, effectBaseColor_);
            break;
            
        case 8: // 神聖・天光
            particleManager_->EmitHolyLight(pos, effectParticleSpeed_, effectParticleCount_, effectBaseColor_);
            break;
            
        case 9: // カオスボイド・闇物質
            particleManager_->EmitChaosVoid(pos, effectParticleSpeed_, effectParticleCount_, effectBaseColor_);
            particleManager_->EmitLightning(pos, effectParticleSpeed_ * 1.5f, effectParticleCount_ / 2, {0.8f, 0.0f, 0.9f});
            particleManager_->EmitGravityVortex(pos, effectParticleSpeed_ * 0.7f, effectParticleCount_ / 2, {0.1f, 0.0f, 0.2f});
            particleManager_->EmitCylinder(pos, effectBaseColor_);
            particleManager_->EmitRing(pos, effectBaseColor_);
            break;
        }
    }

    // ── ダイナミック画面演出（カメラ揺れ・ラジアルブラー・フラッシュ等）はデモモード時のみ発動（通常プレイでの敵被弾・撃破時は画面を揺らしたりぼかしたりしない） ──
    if (isDemoMode_) {
        float shakeMultiplier = isMelee ? 1.6f : 0.8f;
        float hitstopMultiplier = isMelee ? 1.8f : 1.0f;

        activeShakeIntensity_ = cameraShakeIntensity_ * (isMelee ? 1.6f : 0.20f);
        cameraShakeTimer_ = cameraShakeTimeMax_ * shakeMultiplier;
        hitstopTimer_ = isMelee ? (hitstopTimeMax_ * hitstopMultiplier) : 0.0f;

        if (useRadialBlur_) {
            blurIntensity_ = maxBlurWidth_ * (isMelee ? 1.5f : 1.0f);
        }
        if (useImpactFlash_) {
            flashAlpha_ = isMelee ? 0.80f : 0.55f; // 近接はフラッシュ強め
            if (selectedEffectPreset_ == 1) flashColor_ = {1.0f, 0.4f, 0.1f, 1.0f};
            else if (selectedEffectPreset_ == 2) flashColor_ = {0.2f, 0.8f, 1.0f, 1.0f};
            else if (selectedEffectPreset_ == 4) flashColor_ = {0.15f, 0.0f, 0.25f, 1.0f}; // 特異点は暗い黒紫色
            else if (selectedEffectPreset_ == 5) flashColor_ = {0.4f, 0.75f, 1.0f, 1.0f};
            else if (selectedEffectPreset_ == 8) flashColor_ = {1.0f, 0.95f, 0.5f, 1.0f};
            else if (selectedEffectPreset_ == 9) flashColor_ = {0.35f, 0.0f, 0.55f, 1.0f};
            else flashColor_ = {1.0f, 1.0f, 1.0f, 1.0f};
        } else {
            flashAlpha_ = 0.0f;
        }
    }
}

void GamePlayScene::DrawDemo() {
    ID3D12GraphicsCommandList* commandList = dxCommon_->GetCommandList();

    // 1. RenderTextureへの描画開始
    postProcess_->PreDraw();

    // 3Dモデル用パイプライン設定
    if (graphicsPipeline_ && graphicsPipeline_->GetObject3dPipelineState() && graphicsPipeline_->GetObject3dRootSignature()) {
        commandList->SetPipelineState(graphicsPipeline_->GetObject3dPipelineState());
        commandList->SetGraphicsRootSignature(graphicsPipeline_->GetObject3dRootSignature());

        ID3D12DescriptorHeap* heaps[] = { TextureManager::GetInstance()->GetSrvHeap() };
        commandList->SetDescriptorHeaps(1, heaps);

        if (directionalLightResource_) commandList->SetGraphicsRootConstantBufferView(4, directionalLightResource_->GetGPUVirtualAddress());
        if (cameraResource_) commandList->SetGraphicsRootConstantBufferView(5, cameraResource_->GetGPUVirtualAddress());

        // プレイヤー戦闘機の描画
        if (fighterModel_ && fighterTransformResource_ && !LoadingScreen::GetInstance()->ShouldHideInGamePlayer()) {
            commandList->SetGraphicsRootConstantBufferView(1, fighterTransformResource_->GetGPUVirtualAddress());
            fighterModel_->DrawModel(commandList, TextureManager::GetInstance()->GetSrvHandleGPU("Player2/Player_basecolor.JPEG"), TextureManager::GetInstance()->GetSrvHandleGPU("test.dds"));
        }

        // 標的の描画
        if (enemyModel_ && enemyTransformResources_[0]) {
            commandList->SetGraphicsRootConstantBufferView(1, enemyTransformResources_[0]->GetGPUVirtualAddress());
            enemyModel_->DrawModel(commandList, TextureManager::GetInstance()->GetSrvHandleGPU("Player/player.png"), TextureManager::GetInstance()->GetSrvHandleGPU("test.dds"));
        }

        // ビームの描画（スター・ウォーズ風 蛍光ブラスター：鮮やかな黄緑色）
        if (debugBoxModel_) {
            debugBoxModel_->SetLightingEnabled(false);
            D3D12_GPU_DESCRIPTOR_HANDLE whiteSrv = TextureManager::GetInstance()->GetSrvHandleGPU("human/white.png");
            D3D12_GPU_DESCRIPTOR_HANDLE testSrv = TextureManager::GetInstance()->GetSrvHandleGPU("test.dds");

            // 1. 外殻（鮮やかな蛍光黄緑色オーラ）
            debugBoxModel_->SetColor({ 0.45f, 1.0f, 0.10f, 1.0f });
            for (int i = 0; i < kMaxShotBeams; ++i) {
                if (shotBeams_[i].isAlive) {
                    commandList->SetGraphicsRootConstantBufferView(1, bulletTransformResources_[i]->GetGPUVirtualAddress());
                    debugBoxModel_->DrawModel(commandList, whiteSrv, testSrv);
                }
            }

            // 2. 内核（明るい黄緑ホワイト発光コア）
            debugBoxModel_->SetColor({ 0.85f, 1.0f, 0.65f, 1.0f });
            for (int i = 0; i < kMaxShotBeams; ++i) {
                if (shotBeams_[i].isAlive) {
                    Matrix4x4 origWorld = bulletTransformData_[i]->World;
                    Vector3 vel = shotBeams_[i].velocity;
                    Vector3 dir = Normalize(vel);
                    Matrix4x4 rotMatrix = MakeLookRotationMatrix(dir);
                    Vector3 scaleInner = { 1.1f, 1.1f, 5.0f };
                    Matrix4x4 worldInner = Multiply(Multiply(MakeScaleMatrix(scaleInner), rotMatrix), MakeTranslateMatrix(shotBeams_[i].position));

                    bulletTransformData_[i]->World = worldInner;
                    bulletTransformData_[i]->WVP = Multiply(worldInner, camera_->GetViewProjectionMatrix());

                    commandList->SetGraphicsRootConstantBufferView(1, bulletTransformResources_[i]->GetGPUVirtualAddress());
                    debugBoxModel_->DrawModel(commandList, whiteSrv, testSrv);

                    bulletTransformData_[i]->World = origWorld;
                    bulletTransformData_[i]->WVP = Multiply(origWorld, camera_->GetViewProjectionMatrix());
                }
            }
        }
    }

    // パーティクルの描画（半透明）
    if (showParticles_ && graphicsPipeline_ && graphicsPipeline_->GetRootSignature()) {
        commandList->SetGraphicsRootSignature(graphicsPipeline_->GetRootSignature());
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        BlendMode blendMode = useAdditiveBlend_ ? kBlendModeAdd : kBlendModeNormal;
        if (graphicsPipeline_->GetPipelineState(blendMode)) {
            commandList->SetPipelineState(graphicsPipeline_->GetPipelineState(blendMode));
            
            particleManager_->Draw(
                commandList,
                particleModel_.get(),
                ringModel_.get(),
                cylinderModel_.get(),
                textureSrvHandleGPU_,
                gradationSrvHandleGPU_,
                textSrvHandleGPU_
            );
        }
    }

    // 画面インパクトフラッシュの描画（半透明ブレンド）
    if (flashAlpha_ > 0.0f && graphicsPipeline_ && graphicsPipeline_->GetRootSignature()) {
        commandList->SetGraphicsRootSignature(graphicsPipeline_->GetRootSignature());
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if (graphicsPipeline_->GetPipelineState(kBlendModeNormal)) {
            commandList->SetPipelineState(graphicsPipeline_->GetPipelineState(kBlendModeNormal));
            
            Vector4 colorWithAlpha = { flashColor_.x, flashColor_.y, flashColor_.z, flashAlpha_ };
            
            float flashZ = camera_->GetTransform().translate.z + 1.5f;
            Matrix4x4 worldMatrix = MakeAffineMatrix(Vector3{120.0f, 120.0f, 1.0f}, Vector3{0.0f, 0.0f, 0.0f}, Vector3{0.0f, 0.0f, flashZ});
            
            aimingInstancingData_->World = worldMatrix;
            aimingInstancingData_->WVP = Multiply(worldMatrix, camera_->GetViewProjectionMatrix());
            aimingInstancingData_->color = colorWithAlpha;
            aimingInstancingData_->uvTransform = MakeIdentity4x4();
            
            if (particleModel_) {
                particleModel_->Draw(commandList, 1, TextureManager::GetInstance()->GetSrvHandleGPU("human/white.png"), aimingInstancingSrvHandleGPU_);
            }
        }
    }

    // 2. RenderTextureへの描画終了 ＆ ポストプロセス適用
    postProcess_->PostDraw();

    // 描画先をバックバッファに戻す
    D3D12_CPU_DESCRIPTOR_HANDLE backBufferHandle = dxCommon_->GetCurrentBackBufferRtvHandle();
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = dxCommon_->GetDsvHandle();
    commandList->OMSetRenderTargets(1, &backBufferHandle, false, &dsvHandle);

    // SrvManagerのデスクリプタヒープをセット
    SrvManager::GetInstance()->PreDraw();

    // 適切なパイプラインを選択
    ID3D12PipelineState* pso = nullptr;
    switch (activePostProcess_) {
    case kNone:
    default:
        pso = graphicsPipeline_->GetFullscreenPipelineState();
        break;
    case kGrayscale:
        pso = graphicsPipeline_->GetGrayscalePipelineState();
        break;
    case kSepia:
        pso = graphicsPipeline_->GetSepiaPipelineState();
        break;
    case kVignette:
        pso = graphicsPipeline_->GetVignettePipelineState();
        break;
    case kBoxFilter:
        pso = graphicsPipeline_->GetBoxFilterPipelineState();
        break;
    case kOutline:
        pso = graphicsPipeline_->GetDepthOutlinePipelineState();
        break;
    case kRadialBlur:
        pso = graphicsPipeline_->GetRadialBlurPipelineState();
        break;
    case kDissolve:
        pso = graphicsPipeline_->GetDissolvePipelineState();
        break;
    case kRandom:
        pso = graphicsPipeline_->GetRandomPipelineState();
        break;
    }

    // Fullscreenパイプラインで描画
    commandList->SetPipelineState(pso);
    if (activePostProcess_ == kDissolve) {
        commandList->SetGraphicsRootSignature(graphicsPipeline_->GetDissolveRootSignature());
        SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(0, postProcess_->GetSrvIndex());
        SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(1, activeNoiseSrvIndex_);
        commandList->SetGraphicsRootConstantBufferView(2, dissolveParamResource_->GetGPUVirtualAddress());
    } else if (activePostProcess_ == kOutline) {
        // 深度バッファをPIXEL_SHADER_RESOURCE状態へ安全に遷移
        dxCommon_->TransitionDepthStencilState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        commandList->SetGraphicsRootSignature(graphicsPipeline_->GetDepthOutlineRootSignature());
        SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(0, postProcess_->GetSrvIndex()); // t0: カラー
        SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(1, depthSrvIndex_);               // t1: 深度
    } else {
        commandList->SetGraphicsRootSignature(graphicsPipeline_->GetFullscreenRootSignature());
        SrvManager::GetInstance()->SetGraphicsRootDescriptorTable(0, postProcess_->GetSrvIndex());
        if (activePostProcess_ == kVignette && vignetteParamResource_) {
            commandList->SetGraphicsRootConstantBufferView(1, vignetteParamResource_->GetGPUVirtualAddress());
        } else if (activePostProcess_ == kBoxFilter && boxFilterParamResource_) {
            commandList->SetGraphicsRootConstantBufferView(1, boxFilterParamResource_->GetGPUVirtualAddress());
        } else if (activePostProcess_ == kRadialBlur && radialBlurParamResource_) {
            commandList->SetGraphicsRootConstantBufferView(1, radialBlurParamResource_->GetGPUVirtualAddress());
        } else if (activePostProcess_ == kRandom && randomParamResource_) {
            commandList->SetGraphicsRootConstantBufferView(1, randomParamResource_->GetGPUVirtualAddress());
        }
    }
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->DrawInstanced(3, 1, 0, 0);

    // 3. ImGuiデモパネル
#ifdef USE_IMGUI
    ImGui::SetNextWindowSize(ImVec2(480, 580), ImGuiCond_FirstUseEver);
    ImGui::Begin("SPECTACULAR HIT EFFECT SHOWCASE", nullptr, ImGuiWindowFlags_NoCollapse);

    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "CG4 HIT EFFECT PREVIEWER");
    ImGui::Separator();

    // 攻撃モード切り替えUI
    ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f), "Attack Mode Selection: [TAB key to toggle]");
    const char* modeNames[] = { "Shooting Mode (射撃ビーム)", "Melee Mode (打撃・斬撃近接突撃)" };
    int currentModeInt = static_cast<int>(attackMode_);
    if (ImGui::Combo("Attack Mode", &currentModeInt, modeNames, IM_ARRAYSIZE(modeNames))) {
        attackMode_ = static_cast<AttackMode>(currentModeInt);
        meleeState_ = MeleeState::kIdle;
        meleeTimer_ = 0.0f;
        currentFighterPos_ = playerPos_;
    }

    if (attackMode_ == AttackMode::kMelee) {
        const char* stateStr = "Unknown";
        ImVec4 stateColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        switch (meleeState_) {
        case MeleeState::kIdle:   stateStr = "Idle (待機中)"; stateColor = ImVec4(0.7f, 0.7f, 0.7f, 1.0f); break;
        case MeleeState::kDash:   stateStr = "Dash (超高速突進中)"; stateColor = ImVec4(1.0f, 0.5f, 0.0f, 1.0f); break;
        case MeleeState::kHit:    stateStr = "Hit (衝突・エフェクト発生)"; stateColor = ImVec4(1.0f, 0.2f, 0.2f, 1.0f); break;
        case MeleeState::kReturn: stateStr = "Return (元の位置へ帰還中)"; stateColor = ImVec4(0.2f, 0.8f, 1.0f, 1.0f); break;
        }
        ImGui::Text("Melee State: ");
        ImGui::SameLine();
        ImGui::TextColored(stateColor, "%s", stateStr);
    } else {
        ImGui::Text("Shooting Status: Ready");
    }
    ImGui::Separator();

    ImGui::Text("Effect Presets:");
    const char* presetNames[] = {
        "Standard Sparks (通常火花) [1]",
        "Flame Burst (火炎バースト) [2]",
        "Volt Lightning Strike (超高圧放電) [3]",
        "Sonic Blade Slash (真空斬撃) [4]",
        "Gravity Singularity (特異点爆発) [5]",
        "Glacial Freeze (氷結・氷華) [6]",
        "Cyber Spiral (サイバースパイラル) [7]",
        "Aero Wind (風・竜巻) [8]",
        "Holy Light (神聖・天光) [9]",
        "Chaos Void (カオスボイド・闇物質) [0]"
    };
    if (ImGui::Combo("Preset", &selectedEffectPreset_, presetNames, IM_ARRAYSIZE(presetNames))) {
        ApplyPreset(selectedEffectPreset_);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("Sparks Parameters:");
    ImGui::SliderInt("Particle Count", &effectParticleCount_, 10, 150);
    ImGui::SliderFloat("Particle Speed", &effectParticleSpeed_, 5.0f, 40.0f, "%.1f");
    ImGui::SliderFloat("Gravity Multiplier", &effectGravity_, 0.0f, 1.5f, "%.2f");
    ImGui::ColorEdit3("Base Color", &effectBaseColor_.x);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("Dynamic Screen Impact Effects:");
    ImGui::SliderFloat("Shake Intensity", &cameraShakeIntensity_, 0.0f, 5.0f, "%.1f");
    ImGui::SliderFloat("Shake Duration", &cameraShakeTimeMax_, 0.1f, 0.8f, "%.2f");

    ImGui::SliderFloat("Hitstop Duration", &hitstopTimeMax_, 0.0f, 0.4f, "%.2f");

    ImGui::Spacing();
    ImGui::Checkbox("Enable Radial Blur", &useRadialBlur_);
    ImGui::SliderFloat("Max Blur Width", &maxBlurWidth_, 0.0f, 0.15f, "%.3f");

    ImGui::Checkbox("Enable Impact Flash", &useImpactFlash_);

    ImGui::Spacing();
    ImGui::Separator();

    ImGui::Checkbox("Auto-play Mode (自動連続再生)", &autoPlay_);
    if (autoPlay_) {
        ImGui::SliderFloat("Auto-play Interval", &autoPlayInterval_, 0.3f, 4.0f, "%.1f s");
    }

    ImGui::Spacing();
    
    // モードによってラベルと挙動を変更
    std::string buttonLabel = (attackMode_ == AttackMode::kShooting) 
        ? "EMIT BEAM SHOT! (SPACE key)" 
        : "EXECUTE MELEE DASH! (SPACE key)";
        
    if (ImGui::Button(buttonLabel.c_str(), ImVec2(-1, 40))) {
        if (attackMode_ == AttackMode::kShooting) {
            for (int i = 0; i < kMaxShotBeams; ++i) {
                if (!shotBeams_[i].isAlive) {
                    shotBeams_[i].position = playerPos_;
                    Vector3 toTarget = Subtract(targetPos_, playerPos_);
                    shotBeams_[i].velocity = Scale(Normalize(toTarget), 150.0f);
                    shotBeams_[i].isAlive = true;
                    if (audio_) {
                        PlaySE(jumpSE_, false, 0.35f);
                    }
                    break;
                }
            }
        } else {
            if (meleeState_ == MeleeState::kIdle) {
                meleeState_ = MeleeState::kDash;
                meleeTimer_ = 0.0f;
                if (audio_) {
                    PlaySE(jumpSE_, false, 0.7f);
                }
            }
        }
    }

    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), 
        "Controls:\n"
        "[TAB] key: Toggle Attack Mode (Shooting / Melee)\n"
        "[SPACE] key: Trigger Attack / Dash\n"
        "[1] - [0] keys: Select Effect Preset\n"
        "[Z/C] key: Rotate Free Camera Left/Right");
    
    ImGui::End();
#endif
}

void GamePlayScene::ApplyPreset(int presetIndex) {
    selectedEffectPreset_ = presetIndex;
    
    switch (presetIndex) {
    case 0: // 通常スパーク
        effectBaseColor_ = {1.0f, 0.6f, 0.1f};
        effectParticleCount_ = 60;
        effectParticleSpeed_ = 15.0f;
        effectGravity_ = 0.0f;
        cameraShakeIntensity_ = 1.5f;
        cameraShakeTimeMax_ = 0.30f;
        hitstopTimeMax_ = 0.08f;
        useRadialBlur_ = true;
        maxBlurWidth_ = 0.04f;
        useImpactFlash_ = true;
        flashColor_ = {1.0f, 1.0f, 1.0f, 1.0f};
        break;
    case 1: // 火炎バースト
        effectBaseColor_ = {1.0f, 0.35f, 0.0f};
        effectParticleCount_ = 100;
        effectParticleSpeed_ = 12.0f;
        effectGravity_ = -0.1f;
        cameraShakeIntensity_ = 2.8f;
        cameraShakeTimeMax_ = 0.45f;
        hitstopTimeMax_ = 0.12f;
        useRadialBlur_ = true;
        maxBlurWidth_ = 0.06f;
        useImpactFlash_ = true;
        flashColor_ = {1.0f, 0.4f, 0.1f, 1.0f};
        break;
    case 2: // 超高圧放電
        effectBaseColor_ = {0.1f, 0.8f, 1.0f};
        effectParticleCount_ = 75;
        effectParticleSpeed_ = 28.0f;
        effectGravity_ = 0.0f;
        cameraShakeIntensity_ = 2.0f;
        cameraShakeTimeMax_ = 0.15f;
        hitstopTimeMax_ = 0.03f;
        useRadialBlur_ = true;
        maxBlurWidth_ = 0.08f;
        useImpactFlash_ = true;
        flashColor_ = {0.2f, 0.8f, 1.0f, 1.0f};
        break;
    case 3: // 真空斬撃 (揺れなし・フラッシュなし・ストップなし)
        effectBaseColor_ = {0.2f, 1.0f, 0.4f};
        effectParticleCount_ = 50;
        effectParticleSpeed_ = 22.0f;
        effectGravity_ = 0.0f;
        cameraShakeIntensity_ = 0.0f;
        cameraShakeTimeMax_ = 0.0f;
        hitstopTimeMax_ = 0.0f;
        useRadialBlur_ = false;
        maxBlurWidth_ = 0.0f;
        useImpactFlash_ = false;
        flashColor_ = {1.0f, 1.0f, 1.0f, 1.0f};
        break;
    case 4: // 特異点重力爆発 (時間差重揺れ・暗黒フラッシュ)
        effectBaseColor_ = {0.6f, 0.1f, 0.9f};
        effectParticleCount_ = 120;
        effectParticleSpeed_ = 18.0f;
        effectGravity_ = 0.8f;
        cameraShakeIntensity_ = 2.5f;
        cameraShakeTimeMax_ = 0.60f;
        hitstopTimeMax_ = 0.20f;
        useRadialBlur_ = true;
        maxBlurWidth_ = 0.12f;
        useImpactFlash_ = true;
        flashColor_ = {0.15f, 0.0f, 0.25f, 1.0f}; // 暗い黒紫色
        break;
    case 5: // 氷結・氷華 (揺れなし・最大フリーズ・冷気フラッシュ)
        effectBaseColor_ = {0.7f, 0.9f, 1.0f};
        effectParticleCount_ = 80;
        effectParticleSpeed_ = 8.0f;
        effectGravity_ = 0.02f;
        cameraShakeIntensity_ = 0.0f;
        cameraShakeTimeMax_ = 0.0f;
        hitstopTimeMax_ = 0.30f;
        useRadialBlur_ = false;
        maxBlurWidth_ = 0.0f;
        useImpactFlash_ = true;
        flashColor_ = {0.4f, 0.75f, 1.0f, 1.0f};
        break;
    case 6: // デジタルバグ (物理シェイクなし・フラッシュなし・ノイズ画面バグ)
        effectBaseColor_ = {0.0f, 1.0f, 0.3f};
        effectParticleCount_ = 90;
        effectParticleSpeed_ = 25.0f;
        effectGravity_ = 0.0f;
        cameraShakeIntensity_ = 0.0f;
        cameraShakeTimeMax_ = 0.0f;
        hitstopTimeMax_ = 0.0f;
        useRadialBlur_ = false;
        maxBlurWidth_ = 0.0f;
        useImpactFlash_ = false;
        flashColor_ = {0.0f, 1.0f, 0.2f, 1.0f};
        break;
    case 7: // 風・竜巻 (緩い横揺れ・フラッシュなし)
        effectBaseColor_ = {0.6f, 0.95f, 0.75f};
        effectParticleCount_ = 70;
        effectParticleSpeed_ = 18.0f;
        effectGravity_ = -0.05f;
        cameraShakeIntensity_ = 0.8f;
        cameraShakeTimeMax_ = 0.30f;
        hitstopTimeMax_ = 0.04f;
        useRadialBlur_ = true;
        maxBlurWidth_ = 0.045f;
        useImpactFlash_ = false;
        flashColor_ = {0.7f, 1.0f, 0.8f, 1.0f};
        break;
    case 8: // 神聖・天光 (揺れなし・神々しい黄金フラッシュ)
        effectBaseColor_ = {1.0f, 0.95f, 0.6f};
        effectParticleCount_ = 80;
        effectParticleSpeed_ = 14.0f;
        effectGravity_ = 0.05f;
        cameraShakeIntensity_ = 0.0f;
        cameraShakeTimeMax_ = 0.0f;
        hitstopTimeMax_ = 0.10f;
        useRadialBlur_ = true;
        maxBlurWidth_ = 0.09f;
        useImpactFlash_ = true;
        flashColor_ = {1.0f, 0.95f, 0.5f, 1.0f};
        break;
    case 9: // カオスボイド・闇物質 (最大全軸揺れ・長ヒットストップ)
        effectBaseColor_ = {0.45f, 0.05f, 0.75f};
        effectParticleCount_ = 130;
        effectParticleSpeed_ = 20.0f;
        effectGravity_ = 0.5f;
        cameraShakeIntensity_ = 4.0f;
        cameraShakeTimeMax_ = 0.75f;
        hitstopTimeMax_ = 0.25f;
        useRadialBlur_ = true;
        maxBlurWidth_ = 0.14f;
        useImpactFlash_ = true;
        flashColor_ = {0.35f, 0.0f, 0.55f, 1.0f};
        break;
    }
}


static Vector3 CatmullRom(const Vector3& p0, const Vector3& p1, const Vector3& p2, const Vector3& p3, float t) {
    float t2 = t * t;
    float t3 = t2 * t;

    Vector3 a = Scale(p1, 2.0f);
    Vector3 b = Subtract(p2, p0);
    Vector3 c = Subtract(Add(Scale(p0, 2.0f), Scale(p2, 4.0f)), Add(Scale(p1, 5.0f), p3));
    Vector3 d = Add(Subtract(Scale(p1, 3.0f), p0), Subtract(p3, Scale(p2, 3.0f)));

    return Scale(Add(Add(a, Scale(b, t)), Add(Scale(c, t2), Scale(d, t3))), 0.5f);
}

static Vector3 CatmullRomDerivative(const Vector3& p0, const Vector3& p1, const Vector3& p2, const Vector3& p3, float t) {
    float t2 = t * t;

    Vector3 b = Subtract(p2, p0);
    Vector3 c = Subtract(Add(Scale(p0, 2.0f), Scale(p2, 4.0f)), Add(Scale(p1, 5.0f), p3));
    Vector3 d = Add(Subtract(Scale(p1, 3.0f), p0), Subtract(p3, Scale(p2, 3.0f)));

    return Scale(Add(b, Add(Scale(c, 2.0f * t), Scale(d, 3.0f * t2))), 0.5f);
}

Vector3 GamePlayScene::GetRailPosition(float progress) {
    if (waypoints_.empty()) {
        return { kWorldShiftX, -20.0f, progress };
    }
    if (waypoints_.size() == 1) {
        return waypoints_[0];
    }
    
    auto it = std::lower_bound(waypointDistances_.begin(), waypointDistances_.end(), progress);
    
    if (it == waypointDistances_.begin()) {
        Vector3 dir = Normalize(Subtract(waypoints_[1], waypoints_[0]));
        float dist = progress - waypointDistances_[0];
        return Add(waypoints_[0], Scale(dir, dist));
    }
    
    if (it == waypointDistances_.end()) {
        size_t size = waypoints_.size();
        Vector3 dir = Normalize(Subtract(waypoints_[size - 1], waypoints_[size - 2]));
        float dist = progress - waypointDistances_[size - 1];
        return Add(waypoints_[size - 1], Scale(dir, dist));
    }
    
    size_t index = std::distance(waypointDistances_.begin(), it);
    const Vector3& p2 = waypoints_[index];
    const Vector3& p1 = waypoints_[index - 1];
    float d2 = waypointDistances_[index];
    float d1 = waypointDistances_[index - 1];
    
    float denom = d2 - d1;
    if (std::abs(denom) < 0.001f) {
        return p1;
    }
    
    float t = (progress - d1) / denom;

    // Catmull-Rom スプライン補間
    const Vector3& p0 = (index > 1) ? waypoints_[index - 2] : Add(p1, Subtract(p1, p2));
    const Vector3& p3 = (index < waypoints_.size() - 1) ? waypoints_[index + 1] : Add(p2, Subtract(p2, p1));

    return CatmullRom(p0, p1, p2, p3, t);
}

Vector3 GamePlayScene::GetRailDirection(float progress) {
    if (waypoints_.empty()) {
        return { 0.0f, 0.0f, 1.0f };
    }
    if (waypoints_.size() == 1) {
        return { 0.0f, 0.0f, 1.0f };
    }
    
    auto it = std::lower_bound(waypointDistances_.begin(), waypointDistances_.end(), progress);
    
    if (it == waypointDistances_.begin()) {
        return Normalize(Subtract(waypoints_[1], waypoints_[0]));
    }
    
    if (it == waypointDistances_.end()) {
        size_t size = waypoints_.size();
        return Normalize(Subtract(waypoints_[size - 1], waypoints_[size - 2]));
    }
    
    size_t index = std::distance(waypointDistances_.begin(), it);
    const Vector3& p2 = waypoints_[index];
    const Vector3& p1 = waypoints_[index - 1];
    float d2 = waypointDistances_[index];
    float d1 = waypointDistances_[index - 1];

    float denom = d2 - d1;
    if (std::abs(denom) < 0.001f) {
        return Normalize(Subtract(p2, p1));
    }

    float t = (progress - d1) / denom;

    const Vector3& p0 = (index > 1) ? waypoints_[index - 2] : Add(p1, Subtract(p1, p2));
    const Vector3& p3 = (index < waypoints_.size() - 1) ? waypoints_[index + 1] : Add(p2, Subtract(p2, p1));

    Vector3 derivative = CatmullRomDerivative(p0, p1, p2, p3, t);
    float len = Length(derivative);
    if (len > 0.0001f) {
        return Scale(derivative, 1.0f / len);
    }
    
    return Normalize(Subtract(p2, p1));
}

Vector3 GamePlayScene::CalculateRailRight(const Vector3& dir) {
    Vector3 up = { 0.0f, 1.0f, 0.0f };
    if (std::abs(dir.y) > 0.999f) {
        up = { 0.0f, 0.0f, -1.0f };
    }
    return Normalize(Cross(up, dir));
}

Vector3 GamePlayScene::CalculateRailUp(const Vector3& dir, const Vector3& right) {
    return Cross(dir, right);
}

Vector3 GamePlayScene::GetBossPosition(float bodyBounce, float dropOffset, float yAttackOffset) {
    float bossProgress = fighterWorldZ_ + bossZOffset_;
    Vector3 railPos = GetRailPosition(bossProgress);
    Vector3 railDir = GetRailDirection(bossProgress);
    Vector3 railRight = Normalize(Vector3{ railDir.z, 0.0f, -railDir.x });
    Vector3 railUp = Cross(railDir, railRight);
    
    // ── 曲がり角（S=800m）でのアクロバティックジャンプ ──
    float yJumpOffset = 0.0f;
    float cornerS = 800.0f;
    
    // フェーズ定義
    float prepStart = cornerS - 100.0f; // 700m地点から溜め開始
    float jumpStart = cornerS - 55.0f;  // 745m地点から踏み切り
    float jumpEnd   = cornerS + 110.0f; // 910m地点着地 (曲がり切ってから遠くに降りる)
    float landEnd   = cornerS + 150.0f; // 950m地点通常高度に復帰
    
    float maxSink = -18.0f; // 最大沈み込み量
    
    if (bossProgress >= prepStart && bossProgress < jumpStart) {
        // 1. 溜め（沈み込み）フェーズ
        float u = (bossProgress - prepStart) / (jumpStart - prepStart);
        yJumpOffset = maxSink * std::sin(u * static_cast<float>(M_PI) * 0.5f);
    } 
    else if (bossProgress >= jumpStart && bossProgress <= jumpEnd) {
        // 2. 大ジャンプフェーズ (頂点である cornerS=800m で最高高度120mに達する非対称放物線)
        float maxJumpHeight = 120.0f;
        if (bossProgress < cornerS) {
            // 上昇中 (745m 〜 800m、幅55m)
            float t = (bossProgress - cornerS) / 55.0f; // -1.0 ➔ 0.0
            yJumpOffset = maxJumpHeight * (1.0f - t * t * t * t) + maxSink * (t * t);
        } else {
            // 下降中 (800m 〜 910m、幅110m、長くゆっくり滞空する)
            float t = (bossProgress - cornerS) / 110.0f; // 0.0 ➔ 1.0
            yJumpOffset = maxJumpHeight * (1.0f - t * t * t * t) + maxSink * (t * t);
        }
    } 
    else if (bossProgress > jumpEnd && bossProgress <= landEnd) {
        // 3. 着地衝撃吸収・復帰フェーズ
        float v = (bossProgress - jumpEnd) / (landEnd - jumpEnd);
        yJumpOffset = maxSink * (1.0f - v) * (1.0f - v);
    }
    
    float relativeY = (bossYOffset_ + 20.0f) + bodyBounce + dropOffset + yAttackOffset + yJumpOffset;
    
    return Add(railPos, Scale(railUp, relativeY));
}

void GamePlayScene::PlaySE(const SoundData& soundData, bool loop, float volume) {
    if (!isTitleMode_ && audio_) {
        audio_->PlayWave(soundData, loop, volume);
    }
}

void GamePlayScene::TriggerTitleTransitionBoost() {
    isTitleTransitioning_ = true;
    isBoosting_ = false;

    // ブラーやポストプロセスはかけない
    useRadialBlur_ = false;
    activePostProcess_ = kNone;

    // 決定音（SE）を通常音量で再生
    if (audio_) {
        audio_->PlayWave(jumpSE_, false, 1.0f);
    }

    // 遷移開始時のカメラ座標と回転を保存（滑らかな戻し補間用）
    transitionStartCamPos_ = camera_->GetTransform().translate;
    transitionStartCamRot_ = camera_->GetTransform().rotate;
    titleTransitionTimer_ = 0.0f;
}

bool GamePlayScene::GetTransitionPlayerTarget(
    Matrix4x4& outFighterWorld,
    Matrix4x4& outViewProj,
    Vector3& outCamPos,
    Vector3& outCamForward,
    Vector3& outCamUp,
    Vector3& outFighterPos) const {
    if (!camera_ || !fighterTransformData_ || !fighterModel_) return false;
    outFighterWorld = fighterTransformData_->World;
    outViewProj     = camera_->GetViewProjectionMatrix();
    outCamPos       = camera_->GetTransform().translate;
    Matrix4x4 camWorld = Inverse(camera_->GetViewMatrix());
    outCamForward = Normalize({ camWorld.m[2][0], camWorld.m[2][1], camWorld.m[2][2] });
    outCamUp      = Normalize({ camWorld.m[1][0], camWorld.m[1][1], camWorld.m[1][2] });
    outFighterPos = {
        outFighterWorld.m[3][0],
        outFighterWorld.m[3][1],
        outFighterWorld.m[3][2]
    };
    return true;
}


