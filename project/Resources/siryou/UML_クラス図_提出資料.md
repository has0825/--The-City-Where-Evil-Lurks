# 【日本工学院 課題提出資料】UMLクラス図 & 設計フロー図

本資料は、就職作品プロジェクト（3Dルート固定シューティング & Blender Sync）におけるクラス設計および振る舞いを、UMLクラス図とフロー図（Mermaid記法）でまとめたものです。

---

## 1. 添付画像の日本語化フロー図 (参考)

ご提示いただいたフロー図の構成・デザインをそのまま日本語化した図です。

```mermaid
flowchart TD
    Start[" "] -->|お金を手に入れる| Shopping["買い物に行く"]
    Shopping --> Decision{"何を買うか考える"}
    Decision -->|候補 1| Laptop["ノートPC"]
    Decision -->|候補 2| iPhone["スマートフォン (iPhone)"]
    Decision -->|候補 3| Car["🚗 自動車"]

    classDef default fill:#ffffff,stroke:#1e1e38,stroke-width:2px,color:#1e1e38;
    classDef highlight fill:#f0f4ff,stroke:#3b5998,stroke-width:2px,color:#1e1e38;
    class Shopping,Decision highlight;
```

---

## 2. プロジェクトのゲーム処理フロー図 (添付画像と同スタイル)

提示画像のスタイル（開始ノード ➔ 処理 ➔ ひし形分岐 ➔ 分岐先カード）を踏襲し、本プロジェクトの「エネミー行動制御AI（Stateパターン）」の処理フローを日本語で視覚化したものです。

```mermaid
flowchart TD
    Start[" "] -->|敵生成・配置| Init["エネミー初期化"]
    Init --> StateCheck{"現在の行動状態を判定"}
    
    StateCheck -->|待機フェーズ| SideWait["画面外待機 (SideWait)<br/>編隊合流タイミングを監視"]
    StateCheck -->|出現フェーズ| Appear["画面内合流 (Appear)<br/>イージング曲線で隊列へ進入"]
    StateCheck -->|交戦フェーズ| Wander["回遊・射撃 (Wander)<br/>サイン波で浮遊しつつ弾発射"]
    StateCheck -->|突撃フェーズ| Dive["急降下特攻 (Dive)<br/>自機座標へ向けて高速突撃"]

    classDef default fill:#ffffff,stroke:#232742,stroke-width:2px,color:#1a1d2e;
    classDef decision fill:#ffffff,stroke:#232742,stroke-width:2px,color:#1a1d2e;
    classDef stateNode fill:#fcfcff,stroke:#2d3748,stroke-width:2px,color:#1a202c;
    class Init,SideWait,Appear,Wander,Dive stateNode;
    class StateCheck decision;
```

---

## 3. 【課題提出用】UMLクラス図 (01_04. デザインパターン / 01_02. ポリモーフィズム)

授業課題の提出要件を満たす正式なUMLクラス図です。

### 判定基準チェック項目
- [x] **継承 (Generalization / Inheritance)**: 三角矢印記号（`<|--`）を使用
- [x] **コンポジション (Composition - 強い所有)**: 塗りつぶし菱形記号（`*--`）を使用
- [x] **集約 (Aggregation - 弱い所有)**: 白抜き菱形記号（`o--`）を使用
- [x] **依存 (Dependency)**: 破線矢印記号（`..>`）を使用
- [x] **関連 (Association)**: 実線矢印記号（`-->`）を使用

---

### パターンA：エネミーAI状態管理（Stateパターン & ポリモーフィズム）※推奨

エネミーの挙動を `IEnemyState` インターフェースを通じてポリモーフィズムで切り替える設計です。

```mermaid
classDiagram
    direction TB

    class GameObject {
        #Vector3 position_
        #Vector3 rotation_
        #Vector3 scale_
        +Initialize()*
        +Update(deltaTime)*
        +Draw()*
    }

    class BoxCollider {
        +bool hasCollider
        +Vector3 center
        +Vector3 size
    }

    class Enemy {
        -float hp_
        -float radius_
        -BoxCollider collider_
        -unique_ptr~IEnemyState~ currentState_
        +Initialize(position, hp, radius)
        +OnUpdate(deltaTime)
        +ChangeState(newState)
        +TakeDamage(damage)
    }

    class IEnemyState {
        <<interface>>
        +Enter(enemy)*
        +Update(enemy, deltaTime)*
        +Exit(enemy)*
    }

    class EnemyStateSideWait {
        +Enter(enemy)
        +Update(enemy, deltaTime)
    }

    class EnemyStateAppear {
        +Enter(enemy)
        +Update(enemy, deltaTime)
    }

    class EnemyStateWander {
        +Enter(enemy)
        +Update(enemy, deltaTime)
    }

    class EnemyStateDive {
        +Enter(enemy)
        +Update(enemy, deltaTime)
    }

    class EnemyFactory {
        +CreateEnemy(position, hp, radius)$ Enemy
    }

    %% 継承関係 (三角形矢印)
    GameObject <|-- Enemy : 継承 (extends)
    IEnemyState <|-- EnemyStateSideWait : 実装 (implements)
    IEnemyState <|-- EnemyStateAppear : 実装 (implements)
    IEnemyState <|-- EnemyStateWander : 実装 (implements)
    IEnemyState <|-- EnemyStateDive : 実装 (implements)

    %% コンポジション (黒い菱形: 強い所有 / 寿命が連動)
    Enemy *-- BoxCollider : コンポジション (値所有)
    Enemy *-- IEnemyState : コンポジション (unique_ptr所有)

    %% 依存関係 (破線矢印: 引数参照や一時生成)
    IEnemyState ..> Enemy : 引数として一時参照 (depends)
    EnemyFactory ..> Enemy : 生成して返却 (creates)
```

#### 設計のポイント（就活用アピール内容）
1. **State パターンによる状態遷移の分離**:
   - 待機（`SideWait`）、出現（`Appear`）、回遊（`Wander`）、特攻（`Dive`）という異なる行動ルーチンを各Stateクラスへカプセル化。`switch-case` の肥大化を防ぎ、開放閉鎖原則（OCP）を遵守しています。
2. **所有関係（コンポジション）の明確化**:
   - `Enemy` は自身のコリジョン情報 `BoxCollider` を値型として所有（コンポジション）し、状態インスタンス `currentState_` を `std::unique_ptr` で排他的に所有します。
3. **相互参照の疎結合化（依存関係）**:
   - `IEnemyState::Update` 等は引数として `Enemy*` を受け取る一時的な「依存（`..>`）」関係とし、双方向所有による循環参照を回避しています。

---

### パターンB：シーン管理機構（Abstract Factory & Singleton パターン）

シーンの動的切り替えと依存性逆転（DIP）を実現する設計です。

```mermaid
classDiagram
    direction TB

    class AbstractSceneFactory {
        <<abstract>>
        +CreateScene(sceneName)* unique_ptr~BaseScene~
    }

    class SceneFactory {
        +CreateScene(sceneName) unique_ptr~BaseScene~
    }

    class BaseScene {
        <<abstract>>
        +Initialize()*
        +Finalize()*
        +Update()*
        +Draw()*
    }

    class TitleScene {
        +Initialize()
        +Update()
        +Draw()
    }

    class GamePlayScene {
        +Initialize()
        +Update()
        +Draw()
    }

    class ClearScene {
        +Initialize()
        +Update()
        +Draw()
    }

    class GameOverScene {
        +Initialize()
        +Update()
        +Draw()
    }

    class SceneManager {
        -unique_ptr~BaseScene~ currentScene_
        -AbstractSceneFactory* sceneFactory_
        +GetInstance()$ SceneManager*
        +ChangeScene(sceneName)
        +SetFactory(factory)
        +Update()
        +Draw()
    }

    %% 継承関係
    AbstractSceneFactory <|-- SceneFactory : 継承 (extends)
    BaseScene <|-- TitleScene : 継承 (extends)
    BaseScene <|-- GamePlayScene : 継承 (extends)
    BaseScene <|-- ClearScene : 継承 (extends)
    BaseScene <|-- GameOverScene : 継承 (extends)

    %% コンポジション (強い所有)
    SceneManager *-- BaseScene : コンポジション (unique_ptr所有)

    %% 集約 (白い菱形: 弱い所有 / 外部から注入)
    SceneManager o-- AbstractSceneFactory : 集約 (ポインタ参照)

    %% 依存関係 (一時的な生成)
    SceneFactory ..> BaseScene : 生成 (creates)
```

#### 設計のポイント
- **集約（Aggregation: `o--`）**: `SceneManager` は `AbstractSceneFactory` を外部から注入（DI）されてポインタとして保持するため、生存期間が独立した「集約」関係となります。
- **コンポジション（Composition: `*--`）**: 現在アクティブなシーン（`currentScene_`）は `unique_ptr` で完全管理され、切り替え時に破棄されるため「コンポジション」となります。

---

## 4. Mermaid Live Editor での画像出力手順

課題提出物として `.png` 画像が必要な場合の手順です：

1. [Mermaid Live Editor](https://mermaid.live/) にアクセスします。
2. 左側の「Code」ペインに、上記いずれかの ```mermaid ブロック内のコードを貼り付けます。
3. 右側のプレビューに日本語付きの美しいクラス図が表示されます。
4. 画面上部（または下部）の **「Actions」➔「PNG」** をクリックして画像を保存します。
   （または画面をスクリーンショット撮影して保存します）
