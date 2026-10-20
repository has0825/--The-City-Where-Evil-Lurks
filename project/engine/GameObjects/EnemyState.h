#pragma once
#include "MathTypes.h"
#include <memory>

namespace Engine {

    class Enemy; // 前方宣言

    /// <summary>
    /// エネミー行動状態の抽象基底クラス (State Pattern)
    /// </summary>
    class IEnemyState {
    public:
        virtual ~IEnemyState() = default;

        virtual void Enter(Enemy* enemy) {}
        virtual void Update(Enemy* enemy, float deltaTime) = 0;
        virtual void Exit(Enemy* enemy) {}
    };

    /// <summary>
    /// 横側で待機する状態
    /// </summary>
    class EnemyStateSideWait : public IEnemyState {
    public:
        void Enter(Enemy* enemy) override;
        void Update(Enemy* enemy, float deltaTime) override;
    };

    /// <summary>
    /// 中央へ出現・合流する状態
    /// </summary>
    class EnemyStateAppear : public IEnemyState {
    public:
        void Enter(Enemy* enemy) override;
        void Update(Enemy* enemy, float deltaTime) override;
    };

    /// <summary>
    /// 回遊・射撃しつつ後退する状態
    /// </summary>
    class EnemyStateWander : public IEnemyState {
    public:
        void Enter(Enemy* enemy) override;
        void Update(Enemy* enemy, float deltaTime) override;
    };

    /// <summary>
    /// プレイヤーへ特攻する状態
    /// </summary>
    class EnemyStateDive : public IEnemyState {
    public:
        void Enter(Enemy* enemy) override;
        void Update(Enemy* enemy, float deltaTime) override;
    };

}
