#pragma once
#include "GameObject.h"
#include "Enemy.h" // For BoxCollider definition
#include <vector>

namespace Engine {

    /// <summary>
    /// ビルオブジェクトクラス (GameObjectを継承したポリモーフィズム構造)
    /// </summary>
    class Building : public GameObject {
    public:
        Building() = default;
        ~Building() override = default;

        void Initialize(const Vector3& position, const Vector3& scale, int floors);
        
        /// <summary>
        /// 毎フレームの更新ロジック (Template Method Pattern)
        /// </summary>
        void OnUpdate(float deltaTime) override;

        void DestroyBuilding(const Vector3& initialVelocity, const Vector3& rotSpeed);

        // --- カプセル化された Getter / Setter ---
        int GetFloors() const { return floors_; }
        void SetFloors(int f) { floors_ = f; }

        bool IsDestroyed() const { return isDestroyed_; }
        void SetIsDestroyed(bool destroyed) { isDestroyed_ = destroyed; }

        const BoxCollider& GetCollider() const { return collider_; }
        void SetCollider(const BoxCollider& col) { collider_ = col; }

        const Vector3& GetVelocity() const { return velocity_; }
        void SetVelocity(const Vector3& vel) { velocity_ = vel; }

        const Vector3& GetRotationSpeed() const { return rotationSpeed_; }
        void SetRotationSpeed(const Vector3& rs) { rotationSpeed_ = rs; }

        float GetDestroyTimer() const { return destroyTimer_; }
        void SetDestroyTimer(float t) { destroyTimer_ = t; }
        void AddDestroyTimer(float delta) { destroyTimer_ += delta; }

        float GetOriginalX() const { return originalX_; }
        void SetOriginalX(float x) { originalX_ = x; }

        float GetOriginalY() const { return originalY_; }
        void SetOriginalY(float y) { originalY_ = y; }

        int GetOriginalFloors() const { return originalFloors_; }
        void SetOriginalFloors(int f) { originalFloors_ = f; }

        int GetCbIndexStart() const { return cbIndexStart_; }
        void SetCbIndexStart(int idx) { cbIndexStart_ = idx; }

        int GetCbCount() const { return cbCount_; }
        void SetCbCount(int c) { cbCount_ = c; }

        bool IsNearCourseColumn() const { return isNearCourseColumn_; }
        void SetIsNearCourseColumn(bool nearCol) { isNearCourseColumn_ = nearCol; }

        float GetRailProgress() const { return railProgress_; }
        void SetRailProgress(float rp) { railProgress_ = rp; }

        // 行列キャッシュのカプセル化
        const std::vector<Matrix4x4>& GetCachedDetailWorldMatrices() const { return cachedDetailWorldMatrices_; }
        std::vector<Matrix4x4>& GetCachedDetailWorldMatrices() { return cachedDetailWorldMatrices_; }

        const Matrix4x4& GetCachedIntegratedWorldMatrix() const { return cachedIntegratedWorldMatrix_; }
        void SetCachedIntegratedWorldMatrix(const Matrix4x4& mat) { cachedIntegratedWorldMatrix_ = mat; }

        bool IsWorldMatrixCached() const { return isWorldMatrixCached_; }
        void SetIsWorldMatrixCached(bool cached) { isWorldMatrixCached_ = cached; }

    private:
        int floors_ = 0;
        bool isDestroyed_ = false;
        BoxCollider collider_;
        Vector3 velocity_ = { 0.0f, 0.0f, 0.0f };
        Vector3 rotationSpeed_ = { 0.0f, 0.0f, 0.0f };
        float destroyTimer_ = 0.0f;
        float originalX_ = 0.0f;
        float originalY_ = -20.0f;
        int originalFloors_ = 0;
        int cbIndexStart_ = -1;
        int cbCount_ = 0;
        bool isNearCourseColumn_ = false;
        float railProgress_ = -1.0f;

        std::vector<Matrix4x4> cachedDetailWorldMatrices_;
        Matrix4x4 cachedIntegratedWorldMatrix_;
        bool isWorldMatrixCached_ = false;
    };

}
