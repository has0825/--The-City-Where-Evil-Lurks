#pragma once
#include "Bullet.h"
#include <vector>
#include <memory>

namespace Engine {

    /// <summary>
    /// [Object Pool Pattern]
    /// 弾オブジェクトのメモリ確保・解放オーバーヘッドを削減する管理プール
    /// </summary>
    class BulletPool {
    public:
        BulletPool() = default;
        ~BulletPool() = default;

        /// <summary>
        /// プールの初期化 (あらかじめ最大数を確保)
        /// </summary>
        /// <param name="capacity">事前確保サイズ</param>
        void Initialize(size_t capacity = 100);

        /// <summary>
        /// 非アクティブな弾を取得し再利用 (Object Pool Pattern)
        /// </summary>
        Bullet* Acquire(const Vector3& position, const Vector3& velocity, float radius = 0.5f, int type = 0);

        /// <summary>
        /// アクティブなすべての弾を更新
        /// </summary>
        void Update(float deltaTime);

        /// <summary>
        /// アクティブなすべての弾のコンテナ参照を取得
        /// </summary>
        const std::vector<Bullet>& GetActiveBullets() const { return pool_; }
        std::vector<Bullet>& GetActiveBullets() { return pool_; }

    private:
        std::vector<Bullet> pool_;
        size_t capacity_ = 100;
    };

}
