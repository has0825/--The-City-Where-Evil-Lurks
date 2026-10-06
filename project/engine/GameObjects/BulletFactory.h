#pragma once
#include "Bullet.h"

namespace Engine {

    /// <summary>
    /// [Factory Method Pattern]
    /// 弾オブジェクトの生成・初期化を隠蔽・カプセル化する工場クラス
    /// </summary>
    class BulletFactory {
    public:
        BulletFactory() = default;
        ~BulletFactory() = default;

        /// <summary>
        /// 弾オブジェクトを生成して初期化 (Factory Method Pattern)
        /// </summary>
        static Bullet CreateBullet(const Vector3& position, const Vector3& velocity, float radius = 0.5f, int type = 0);
    };

}
