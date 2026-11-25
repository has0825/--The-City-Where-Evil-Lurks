#include "Object3d.hlsli"

struct TransformationMatrix
{
    float32_t4x4 WVP;
    float32_t4x4 World;
    float32_t4 color; // モデルごとのカラー(アルファフェード等)
};
ConstantBuffer<TransformationMatrix> gTransformationMatrix : register(b1);

struct VertexShaderInput
{
    float32_t4 position : POSITION0;
    float32_t2 texcoord : TEXCOORD0;
    float32_t3 normal : NORMAL0;
    uint32_t4 jointIndices : BONEINDICES0;
    float32_t4 jointWeights : BONEWEIGHTS0;
};

ConstantBuffer<SkinningPalette> gSkinningPalette : register(b4);
ConstantBuffer<DirectionalLight> gDirectionalLight : register(b2);

VertexShaderOutput main(VertexShaderInput input)
{
    VertexShaderOutput output;
    
    float32_t4 skinnedPosition = input.position;
    float32_t3 skinnedNormal = input.normal;

    // ボーンウェイトが定義されているオブジェクト（歩行モデル等）のみスキニング計算を行う (非スキニングモデルの頂点処理を極限まで軽量化)
    if (input.jointWeights[0] > 0.0f)
    {
        skinnedPosition = float32_t4(0, 0, 0, 0);
        skinnedNormal = float32_t3(0, 0, 0);
        for (int i = 0; i < 4; i++) {
            if (input.jointWeights[i] > 0) {
                skinnedPosition += mul(input.position, gSkinningPalette.boneMatrices[input.jointIndices[i]]) * input.jointWeights[i];
                skinnedNormal += mul(input.normal, (float32_t3x3)gSkinningPalette.boneMatrices[input.jointIndices[i]]) * input.jointWeights[i];
            }
        }
    }

    // 雑魚敵（little spider）の羽ばたき頂点アニメーション
    if (gDirectionalLight.enableNightEmissive == 5u)
    {
        float32_t wingX = saturate((abs(skinnedPosition.x) - 0.11f) / 0.30f);
        float32_t wingY = saturate((skinnedPosition.y - 0.15f) / 0.15f);
        float32_t wingMask = wingX * wingY;
        if (wingMask > 0.001f)
        {
            float32_t flap = sin(gDirectionalLight.emissiveIntensity * 30.0f);
            float32_t sideSign = (skinnedPosition.x >= 0.0f) ? 1.0f : -1.0f;
            float32_t angle = flap * 0.52f * wingMask * sideSign;
            float32_t px = skinnedPosition.x - sideSign * 0.11f;
            float32_t py = skinnedPosition.y - 0.22f;
            float32_t c = cos(angle);
            float32_t s = sin(angle);
            skinnedPosition.x = sideSign * 0.11f + px * c - py * s;
            skinnedPosition.y = 0.22f + px * s + py * c;
            float32_t nx = skinnedNormal.x;
            float32_t ny = skinnedNormal.y;
            skinnedNormal.x = nx * c - ny * s;
            skinnedNormal.y = nx * s + ny * c;
        }
    }

    output.position = mul(skinnedPosition, gTransformationMatrix.WVP);
    output.texcoord = input.texcoord;
    
    // 法線とワールド座標の計算（環境マップに必須）
    output.normal = normalize(mul(skinnedNormal, (float32_t3x3) gTransformationMatrix.World));
    output.worldPosition = mul(skinnedPosition, gTransformationMatrix.World).xyz;
    output.color = gTransformationMatrix.color; // モデル単位のカラーを出力
    
    return output;
}