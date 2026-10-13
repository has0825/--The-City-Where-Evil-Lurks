#include "SceneManager.h"
#include "LoadingScreen.h"
#include <cassert>

SceneManager* SceneManager::GetInstance() {
    static SceneManager instance;
    return &instance;
}

SceneManager::~SceneManager() {
    ClearPreviousScene();
    if (currentScene_) {
        currentScene_->Finalize();
    }
}

void SceneManager::ChangeScene(const std::string& sceneName) {
    assert(sceneFactory_);
    assert(nextScene_ == nullptr);

    previousSceneName_ = currentSceneName_;
    currentSceneName_ = sceneName;

    // 初回起動時（currentScene_ がまだ存在しない状態）は即座に生成・初期化して開始
    if (!currentScene_) {
        currentScene_ = sceneFactory_->CreateScene(currentSceneName_);
        if (currentScene_) {
            currentScene_->Initialize();
        }
        isLoading_ = false;
        isLoadCompleted_ = true;
        return;
    }

    // ロード画面演出の開始
    isLoading_ = true;
    loadProgress_ = 0.05f;
    isLoadCompleted_ = false;

    LoadingScreen::GetInstance()->Initialize();
    LoadingScreen::GetInstance()->Reset();
}

void SceneManager::Update() {
    if (isLoading_) {
        LoadingScreen* loading = LoadingScreen::GetInstance();

        // タイトル画面の斜め斬撃＆上下分割が完了してロード画面が完全に現れてから次シーンの重いロードを実行（斬撃・分割アニメのカクつき防止）
        if (loading->IsReadyToLoadNextScene()) {
            if (!nextScene_) {
                loadProgress_ = 0.35f;
                nextScene_ = sceneFactory_->CreateScene(currentSceneName_);
                loadProgress_ = 0.60f;
            } else if (!isLoadCompleted_) {
                loadProgress_ = 0.85f;
                if (nextScene_) {
                    nextScene_->Initialize();
                }
                loadProgress_ = 1.00f;
                isLoadCompleted_ = true;
            }
        }

        loading->Update(1.0f / 60.0f, loadProgress_);

        // 背景シーンの更新（ロード完了後は準備できた nextScene_ を更新して初期カメラ・行列を確定させる）
        if (isLoadCompleted_ && nextScene_) {
            nextScene_->Update();
        } else if (currentScene_ && loading->GetPhase() == LoadingScreen::TransitionPhase::kSlashTitle) {
            currentScene_->Update();
        }

        // 切れたロード画面が左右へ完全に画面外へ出たらシーンを本切り替え
        if (isLoadCompleted_ && loading->IsTransitionCompleted()) {
            ClearPreviousScene();
            if (currentScene_) {
                previousScene_ = std::move(currentScene_);
            }
            currentScene_ = std::move(nextScene_);
            isLoading_ = false;
            // 切り替わった直後のフレームで新シーンを更新（PHASE 1演出を即時開始）
            if (currentScene_) {
                currentScene_->Update();
            }
        }

        return;
    }

    // 現在のシーンの更新
    if (currentScene_) {
        currentScene_->Update();
    }
}

void SceneManager::Draw() {
    if (isLoading_) {
        LoadingScreen* loading = LoadingScreen::GetInstance();
        auto phase = loading->GetPhase();

        if (phase == LoadingScreen::TransitionPhase::kSlashTitle) {
            // 1. タイトル画面を斬るフェーズでは現在のシーンを描画（キャプチャ元）
            if (currentScene_) {
                currentScene_->Draw();
            }
        } else if (loading->ShouldDrawNextSceneUnderneath()) {
            // 5. 切れた/割れたロード画面の奥に新シーン（ゲーム画面）を描画
            if (isLoadCompleted_ && nextScene_) {
                nextScene_->Draw();
            }
        }

        // 斬撃・分割・ロード画面のオーバーレイ描画
        loading->Draw();
        return;
    }

    if (currentScene_) {
        currentScene_->Draw();
    }
}

void SceneManager::DrawPreviousScene() {
    if (previousScene_) {
        previousScene_->Draw();
    }
}

void SceneManager::ClearPreviousScene() {
    if (previousScene_) {
        previousScene_->Finalize();
        previousScene_.reset();
    }
}