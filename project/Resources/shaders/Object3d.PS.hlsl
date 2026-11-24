#include "Object3d.hlsli"

ConstantBuffer<Material> gMaterial : register(b0);
ConstantBuffer<DirectionalLight> gDirectionalLight : register(b2);
ConstantBuffer<Camera> gCamera : register(b3);

Texture2D<float32_t4> gTexture : register(t0);
TextureCube<float32_t4> gEnvironmentTexture : register(t1); // 【追加】環境マップ用テクスチャ
SamplerState gSampler : register(s0);

struct PixelShaderOutput
{
    float32_t4 color : SV_TARGET0;
};

PixelShaderOutput main(VertexShaderOutput input)
{
    PixelShaderOutput output;
    
    // UVトランスフォームの適用
    float4 transformedUV = mul(float32_t4(input.texcoord, 0.0f, 1.0f), gMaterial.uvTransform);
    float32_t4 textureColor = gTexture.Sample(gSampler, transformedUV.xy);
    
    // 透過ピクセルの破棄（アルファクリッピング）
    if (textureColor.a < 0.05f)
    {
        discard;
    }

    // ── 【追加】ディザリングによる半透明（消滅）フェード ──
    // 4x4 のバイエル行列パターンを使ってピクセルを間引き、奥が透けない半透明化を実現する
    float dither[4][4] = {
        { 1.0f/17.0f,  9.0f/17.0f,  3.0f/17.0f, 11.0f/17.0f },
        { 13.0f/17.0f, 5.0f/17.0f, 15.0f/17.0f,  7.0f/17.0f },
        { 4.0f/17.0f,  12.0f/17.0f, 2.0f/17.0f, 10.0f/17.0f },
        { 16.0f/17.0f, 8.0f/17.0f, 14.0f/17.0f,  6.0f/17.0f }
    };
    int x = int(input.position.x) % 4;
    int y = int(input.position.y) % 4;
    if (input.color.a < dither[x][y])
    {
        discard;
    }
    
    // ライティング計算
    if (gMaterial.enableLighting != 0)
    {
        // --- 既存 of Punctual Light (Directional Light) の計算 ---
        float32_t3 dirLightingColor = float32_t3(0.0f, 0.0f, 0.0f);
        if (gDirectionalLight.intensity > 0.0f) {
            float NdotL = dot(normalize(input.normal), -gDirectionalLight.direction);
            float cos = pow(NdotL * 0.5f + 0.5f, 2.0f); // Half Lambert
            dirLightingColor = gDirectionalLight.color.rgb * gDirectionalLight.intensity * cos;
        }

        // --- スポットライト의 計算 ---
        float32_t3 spotLightingColor = float32_t3(0.0f, 0.0f, 0.0f);
        if (gDirectionalLight.enableSpotLight != 0)
        {
            float32_t3 spotLightDirection = normalize(gDirectionalLight.spotLightPos - input.worldPosition);
            float distance = length(gDirectionalLight.spotLightPos - input.worldPosition);
            float cosAngle = dot(-spotLightDirection, normalize(gDirectionalLight.spotLightDir));
            
            if (cosAngle > gDirectionalLight.spotLightCosAngle)
            {
                // 距離減衰
                float attenuation = saturate(1.0f - (distance / gDirectionalLight.spotLightRange));
                // コーンの境界付近を滑らかにする (フォールオフ)
                float angleFalloff = saturate((cosAngle - gDirectionalLight.spotLightCosAngle) / (1.0f - gDirectionalLight.spotLightCosAngle));
                
                // 拡散反射
                float NdotLSpot = dot(normalize(input.normal), spotLightDirection);
                float cosSpot = pow(NdotLSpot * 0.5f + 0.5f, 2.0f);
                
                spotLightingColor = gDirectionalLight.spotLightColor.rgb * gDirectionalLight.spotLightIntensity * cosSpot * attenuation * angleFalloff;
            }
        }
        
        float32_t3 lampLightingColor = float32_t3(0.0f, 0.0f, 0.0f);
        if (gDirectionalLight.numStreetLamps > 0u && gDirectionalLight.streetLampRange > 0.01f)
        {
            uint32_t lampCount = min(gDirectionalLight.numStreetLamps, 8u);
            float32_t3 N = normalize(input.normal);
            for (uint32_t li = 0u; li < lampCount; ++li)
            {
                float32_t3 toLamp = gDirectionalLight.streetLampPositions[li].xyz - input.worldPosition;
                float32_t dist = length(toLamp);
                if (dist < gDirectionalLight.streetLampRange)
                {
                    float32_t3 L = toLamp / max(dist, 0.001f);
                    float32_t normDist = dist / gDirectionalLight.streetLampRange;
                    float32_t atten = pow(saturate(1.0f - normDist), 2.0f);
                    float32_t ndotl = saturate(dot(N, L) * 0.65f + 0.35f);
                    lampLightingColor += gDirectionalLight.streetLampColor * gDirectionalLight.streetLampIntensity * atten * ndotl;
                }
            }
        }

        float32_t3 lightingColor = dirLightingColor + spotLightingColor + lampLightingColor;

        // 夜ステージ用：ビルの部屋の明かり / 街灯電球の自己発光
        float32_t3 nightEmissive = float32_t3(0.0f, 0.0f, 0.0f);
        if (gDirectionalLight.enableNightEmissive == 1u)
        {
            // ビルの暖かい窓明かり（RがBより高く明るいピクセル）を抽出し、夜でも光らせる
            float32_t warmth = saturate((textureColor.r - textureColor.b) * 3.5f);
            float32_t lum = dot(textureColor.rgb, float32_t3(0.299f, 0.587f, 0.114f));
            float32_t windowMask = warmth * smoothstep(0.14f, 0.45f, lum);
            nightEmissive = textureColor.rgb * windowMask * 1.35f;
        }
        else if (gDirectionalLight.enableNightEmissive == 2u)
        {
            // 街灯の電球カバー部分（明るいクリーム〜黄白色）を発光させる
            float32_t bulbLum = dot(textureColor.rgb, float32_t3(0.299f, 0.587f, 0.114f));
            float32_t bulbMask = smoothstep(0.38f, 0.70f, bulbLum);
            nightEmissive = textureColor.rgb * bulbMask * 2.2f;
        }
        else if (gDirectionalLight.enableNightEmissive == 3u)
        {
            // ボスの目・赤色エネルギーラインの自己発光と機能停止時の消灯減衰
            float32_t redness = saturate((textureColor.r - max(textureColor.g, textureColor.b)) * 3.0f - 0.25f);
            float32_t eyePower = saturate(gDirectionalLight.emissiveIntensity);
            nightEmissive = float32_t3(1.0f, 0.12f, 0.08f) * redness * 2.4f * eyePower;
            // 機能停止に伴い、赤い目部分のベース色自体も暗く落とす
            float32_t eyeDim = lerp(1.0f, 0.08f, (1.0f - eyePower) * redness);
            textureColor.rgb *= eyeDim;
        }

        output.color.rgb = textureColor.rgb * gMaterial.color.rgb * lightingColor + nightEmissive;
        output.color.a = textureColor.a * gMaterial.color.a;

        // --- 【追加】環境マップ (Environment Map) の計算 ---
        if (gMaterial.environmentCoefficient > 0.0f)
        {
            // カメラからピクセルへのベクトル
            float32_t3 cameraToPosition = normalize(input.worldPosition - gCamera.worldPosition);
            // 法線を使ってベクトルを反射
            float32_t3 reflectedVector = reflect(cameraToPosition, normalize(input.normal));
            // 環境マップからサンプリング
            float32_t4 environmentColor = gEnvironmentTexture.Sample(gSampler, reflectedVector);
            
            // 映り込みの強さ（スケール）を掛けて加算
            output.color.rgb += environmentColor.rgb * gMaterial.environmentCoefficient;
        }
        
    }
    else
    {
        // ライティングなし
        output.color = textureColor * gMaterial.color;
    }

    return output;
}