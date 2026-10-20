#include "EnemyState.h"
#include "Enemy.h"
#include <cmath>

namespace Engine {

    // --- EnemyStateSideWait ---
    void EnemyStateSideWait::Enter(Enemy* enemy) {
        if (enemy) {
            enemy->SetStateType(Enemy::StateType::kSideWait);
        }
    }

    void EnemyStateSideWait::Update(Enemy* enemy, float deltaTime) {
        if (!enemy) return;
        enemy->AddStateTimer(deltaTime);

        // 2.0秒待機したら出現フェーズ (Appear) へ自己状態遷移
        if (enemy->GetStateTimer() >= 2.0f) {
            enemy->ChangeState(std::make_unique<EnemyStateAppear>());
        }
    }

    // --- EnemyStateAppear ---
    void EnemyStateAppear::Enter(Enemy* enemy) {
        if (enemy) {
            enemy->SetStateType(Enemy::StateType::kAppear);
            enemy->SetStateTimer(0.0f);
        }
    }

    void EnemyStateAppear::Update(Enemy* enemy, float deltaTime) {
        if (!enemy) return;
        enemy->AddStateTimer(deltaTime);

        // 1.5秒かけて中央へ移動後、回遊フェーズ (Wander) へ自己状態遷移
        if (enemy->GetStateTimer() >= 1.5f) {
            enemy->ChangeState(std::make_unique<EnemyStateWander>());
        }
    }

    // --- EnemyStateWander ---
    void EnemyStateWander::Enter(Enemy* enemy) {
        if (enemy) {
            enemy->SetStateType(Enemy::StateType::kWander);
            enemy->SetStateTimer(0.0f);
        }
    }

    void EnemyStateWander::Update(Enemy* enemy, float deltaTime) {
        if (!enemy) return;
        enemy->AddStateTimer(deltaTime);
        enemy->AddWanderPhase(deltaTime * 2.0f);

        // 4.0秒回遊後、プレイヤーへダイブ特攻 (Dive) へ自己状態遷移
        if (enemy->GetStateTimer() >= 4.0f) {
            enemy->ChangeState(std::make_unique<EnemyStateDive>());
        }
    }

    // --- EnemyStateDive ---
    void EnemyStateDive::Enter(Enemy* enemy) {
        if (enemy) {
            enemy->SetStateType(Enemy::StateType::kDive);
            enemy->SetStateTimer(0.0f);
        }
    }

    void EnemyStateDive::Update(Enemy* enemy, float deltaTime) {
        if (!enemy) return;
        enemy->AddStateTimer(deltaTime);
        // 特攻状態の処理（位置の高速更新等）
    }

}
