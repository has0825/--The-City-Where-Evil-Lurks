#pragma once
#include "Enemy.h"
#include <memory>

namespace Engine {

    /// <summary>
    /// [Factory Method Pattern]
    /// エネミーオブジェクトの生成ロジックをカプセル化する工場クラス
    /// </summary>
    class EnemyFactory {
    public:
        EnemyFactory() = default;
        ~EnemyFactory() = default;

        /// <summary>
        /// 雑魚エネミーの生成と初期化 (Factory Method Pattern)
        /// </summary>
        static Enemy CreateEnemy(const Vector3& position, float hp = 10.0f, float radius = 1.5f, int groupIdx = 0, int memberIdx = 0);
    };

}
