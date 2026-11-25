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
        else if (gDirectionalLight.enableNightEmissive == 4u)
        {
            // 道路・地面用：巨大ボスの歩行によるひび割れ＆陥没（streetLampPositions[8..15] に xyz=中心, w=半径）
            float32_t maxCrack = 0.0f;
            float32_t maxCrackEdge = 0.0f;
            float32_t centerDent = 0.0f;
            float2 totalNormalOffset = float2(0.0f, 0.0f);
            float32_t rimHeight = 0.0f;

            float32_t3 viewDirW = normalize(gCamera.worldPosition - input.worldPosition);
            float2 lightDirXZ = normalize(float2(0.35f, -0.65f));

            for (uint32_t ci = 8u; ci < 16u; ++ci)
            {
                float32_t4 crack = gDirectionalLight.streetLampPositions[ci];
                if (crack.w <= 0.1f) continue;
                float2 rawDXZ = input.worldPosition.xz - crack.xz;
                float rawDistSq = dot(rawDXZ, rawDXZ);
                if (rawDistSq >= crack.w * crack.w) continue;

                float rawDist = sqrt(rawDistSq);
                float rawNormR = rawDist / crack.w;

                // すり鉢状の凹み深度プロファイル（軽く自然な凹み）
                float32_t bowl = pow(saturate(1.0f - rawNormR / 0.58f), 1.6f);
                float32_t rim = sin(saturate((rawNormR - 0.52f) / 0.42f) * 3.14159265f) * 0.10f;

                // 視線方向に応じたごく軽い擬似パララックス
                float2 dXZ = rawDXZ - viewDirW.xz * (bowl * 0.35f);
                float dist = length(dXZ);
                float normR = saturate(dist / crack.w);
                float angle = atan2(dXZ.y, dXZ.x);

                float jitter1 = sin(input.worldPosition.x * 0.45f + input.worldPosition.z * 0.37f) * 0.28f;
                float jitter2 = cos(input.worldPosition.x * 1.15f - input.worldPosition.z * 0.95f) * 0.14f;
                float spoke = abs(sin((angle + jitter1 + jitter2) * 4.5f + crack.x * 0.13f));
                float ringCrack = abs(sin(normR * 10.5f + jitter1 * 3.2f));

                float mainLine = 1.0f - smoothstep(0.02f, 0.15f + normR * 0.10f, spoke);
                float subLine = (1.0f - smoothstep(0.02f, 0.11f, ringCrack)) * 0.60f;
                float fadeOut = 1.0f - smoothstep(0.35f, 1.0f, normR);

                float32_t crackVal = saturate((mainLine + subLine) * fadeOut);
                maxCrack = max(maxCrack, crackVal);

                // ひびの縁（少し外側の欠け・ベベルハイライト用）
                float edgeMask = saturate((1.0f - smoothstep(0.12f, 0.28f + normR * 0.10f, spoke)) - mainLine) * fadeOut;
                maxCrackEdge = max(maxCrackEdge, edgeMask);

                // 中心部の軽い影（環境遮蔽）
                centerDent = max(centerDent, bowl * 0.24f);
                rimHeight = max(rimHeight, rim);

                // 斜面の法線傾き（軽く凹んだ陰影）
                if (rawDist > 0.001f)
                {
                    float2 radialDir = rawDXZ / rawDist;
                    float32_t innerSlope = -smoothstep(0.0f, 0.45f, rawNormR) * (1.0f - smoothstep(0.45f, 0.72f, rawNormR));
                    float32_t outerSlope = (1.0f - smoothstep(0.65f, 0.95f, rawNormR)) * smoothstep(0.55f, 0.72f, rawNormR) * 0.25f;
                    totalNormalOffset += radialDir * (innerSlope * 0.45f + outerSlope);
                }
            }
            if (maxCrack > 0.001f || centerDent > 0.001f || rimHeight > 0.001f)
            {
                // クレーター斜面の陰影計算（軽く立体感が出る程度）
                float32_t slopeLit = dot(totalNormalOffset, lightDirXZ);
                float32_t shadeFactor = saturate(1.0f + slopeLit * 0.32f + rimHeight * 0.12f - centerDent);
                textureColor.rgb *= shadeFactor;

                // 欠けたひびの縁をわずかに明るくし、ひびの溝の中を暗く落とす
                textureColor.rgb = lerp(textureColor.rgb, textureColor.rgb * 1.15f + float32_t3(0.015f, 0.015f, 0.02f), maxCrackEdge * 0.25f);
                textureColor.rgb = lerp(textureColor.rgb, float32_t3(0.018f, 0.018f, 0.024f), maxCrack * 0.92f);
            }
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