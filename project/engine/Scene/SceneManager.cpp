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

    // ロード画面演出の開始
    isLoading_ = true;
    loadProgress_ = 0.05f;
    isLoadCompleted_ = false;

    LoadingScreen::GetInstance()->Initialize();
    LoadingScreen::GetInstance()->Reset();
}

void SceneManager::Update() {
    if (isLoading_) {
        // ロード画面のアニメーションと進捗の更新
        if (!nextScene_) {
            loadProgress_ = 0.20f;
            nextScene_ = sceneFactory_->CreateScene(currentSceneName_);
            loadProgress_ = 0.40f;
        } else if (!isLoadCompleted_) {
            loadProgress_ = 0.75f;
            // 新しいシーンの初期化
            if (nextScene_) {
                nextScene_->Initialize();
            }
            loadProgress_ = 1.00f;
            isLoadCompleted_ = true;
        }

        LoadingScreen::GetInstance()->Update(1.0f / 60.0f, loadProgress_);

        // 背景シーンの更新（ロード完了後は準備できた nextScene_ を更新、完了前は currentScene_ を更新）
        if (isLoadCompleted_ && nextScene_) {
            nextScene_->Update();
        } else if (currentScene_) {
            currentScene_->Update();
        }

        // ロード完了後の画面遷移（フェードアウト）演出が完了したらシーンを本切り替え
        if (isLoadCompleted_ && LoadingScreen::GetInstance()->IsTransitionCompleted()) {
            ClearPreviousScene();
            if (currentScene_) {
                previousScene_ = std::move(currentScene_);
            }
            currentScene_ = std::move(nextScene_);
            isLoading_ = false;
        }

        return;
    }

    // 現在のシーンの更新
    if (currentScene_) {
        currentScene_->Update();
    }
}

void SceneManager::Draw() {
    if (isLoading_ && isLoadCompleted_ && nextScene_) {
        nextScene_->Draw();
    } else if (currentScene_) {
        currentScene_->Draw();
    }

    // ロード中画面の重ね描画
    if (isLoading_) {
        LoadingScreen::GetInstance()->Draw();
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