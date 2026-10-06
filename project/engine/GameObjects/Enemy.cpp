#include "Enemy.h"

namespace Engine {

    Enemy::Enemy(const Enemy& other) {
        *this = other;
    }

    Enemy& Enemy::operator=(const Enemy& other) {
        if (this != &other) {
            GameObject::operator=(other);
            radius_ = other.radius_;
            hp_ = other.hp_;
            maxHP_ = other.maxHP_;
            collider_ = other.collider_;
            currentStateType_ = other.currentStateType_;
            groupIndex_ = other.groupIndex_;
            memberIndex_ = other.memberIndex_;
            localOffset_ = other.localOffset_;
            stateTimer_ = other.stateTimer_;
            wanderAnchor_ = other.wanderAnchor_;
            diveDirection_ = other.diveDirection_;
            wanderPhase_ = other.wanderPhase_;
            speed_ = other.speed_;
            relativeZ_ = other.relativeZ_;
            appearStartPos_ = other.appearStartPos_;
            railProgress_ = other.railProgress_;
            wanderAnchorRailProgress_ = other.wanderAnchorRailProgress_;
            wanderAnchorRelX_ = other.wanderAnchorRelX_;
            wanderAnchorRelY_ = other.wanderAnchorRelY_;

            switch (currentStateType_) {
            case StateType::kSideWait: ChangeState(std::make_unique<EnemyStateSideWait>()); break;
            case StateType::kAppear:   ChangeState(std::make_unique<EnemyStateAppear>()); break;
            case StateType::kWander:   ChangeState(std::make_unique<EnemyStateWander>()); break;
            case StateType::kDive:     ChangeState(std::make_unique<EnemyStateDive>()); break;
            default:                   ChangeState(std::make_unique<EnemyStateSideWait>()); break;
            }
        }
        return *this;
    }

    void Enemy::Initialize(const Vector3& position, float hp, float radius) {
        position_ = position;
        hp_ = hp;
        maxHP_ = hp;
        radius_ = radius;
        isAlive_ = true;
        stateTimer_ = 0.0f;
        wanderPhase_ = 0.0f;
        speed_ = 0.0f;
        relativeZ_ = 120.0f;

        // 初期状態を EnemyStateSideWait に設定 (State Pattern)
        ChangeState(std::make_unique<EnemyStateSideWait>());
    }

    void Enemy::ChangeState(std::unique_ptr<IEnemyState> newState) {
        if (!newState) return;

        if (currentState_) {
            currentState_->Exit(this);
        }

        currentState_ = std::move(newState);

        if (currentState_) {
            currentState_->Enter(this);
        }
    }

    void Enemy::OnUpdate(float deltaTime) {
        // 現在の状態クラスに更新処理を多態的に委譲 (State Pattern)
        if (currentState_) {
            currentState_->Update(this, deltaTime);
        }
    }

    void Enemy::TakeDamage(float damage) {
        if (!isAlive_) return;
        hp_ -= damage;
        if (hp_ <= 0.0f) {
            hp_ = 0.0f;
            isAlive_ = false;
        }
    }

}
