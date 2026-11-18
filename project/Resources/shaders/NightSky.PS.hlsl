struct VertexShaderOutput {
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD0;
};

struct NightSkyParams {
    float time;
    float aspectRatio;
    float camYaw;
    float camPitch;
};

ConstantBuffer<NightSkyParams> gParams : register(b0);

struct PixelShaderOutput {
    float4 color : SV_TARGET0;
};

// ハッシュ関数 (2D -> 1D)
float Hash21(float2 p) {
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}

// ハッシュ関数 (2D -> 2D)
float2 Hash22(float2 p) {
    float3 p3 = frac(float3(p.xyx) * float3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.xx + p3.yz) * p3.zy);
}

// 滑らかなバリューノイズ
float Noise(float2 p) {
    float2 i = floor(p);
    float2 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);

    float a = Hash21(i);
    float b = Hash21(i + float2(1.0, 0.0));
    float c = Hash21(i + float2(0.0, 1.0));
    float d = Hash21(i + float2(1.0, 1.0));

    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

// FBM (5オクターブ)
float Fbm5(float2 p) {
    float value = 0.0;
    float amp = 0.5;
    float2x2 rot = float2x2(0.80, 0.60, -0.60, 0.80);
    for (int i = 0; i < 5; ++i) {
        value += amp * Noise(p);
        p = mul(rot, p) * 2.03 + float2(17.1, 31.7);
        amp *= 0.5;
    }
    return value;
}

// 雲用のドメインワーピング付きFBM (もこもこした自然な雲の輪郭)
float CloudDensity(float2 uv, float time) {
    float2 warp = float2(
        Fbm5(uv * 0.65 + float2(time * 0.015, 2.3)),
        Fbm5(uv * 0.65 + float2(5.2, -time * 0.010))
    );
    float base = Fbm5(uv + warp * 0.75);
    float detail = Fbm5(uv * 2.6 - warp * 0.4 + float2(time * 0.02, 0.0));
    float d = base * 0.78 + detail * 0.22;
    return d;
}

// 星空レイヤー生成
float3 StarLayer(float2 uv, float scale, float time, float densityThreshold, float flareStrength) {
    float2 gv = frac(uv * scale) - 0.5;
    float2 id = floor(uv * scale);

    float3 col = float3(0.0, 0.0, 0.0);

    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float2 offset = float2(x, y);
            float2 cellId = id + offset;
            float n = Hash21(cellId);

            if (n > densityThreshold) {
                float2 rnd = Hash22(cellId + 13.7);
                float2 starPos = offset + (rnd - 0.5) * 0.82;
                float d = length(gv - starPos);

                float size = lerp(0.022, 0.088, frac(n * 37.3));
                float starCore = smoothstep(size, 0.0, d);
                float starGlow = exp(-d * 20.0) * 0.22;

                float flare = 0.0;
                if (flareStrength > 0.0 && frac(n * 91.7) > 0.68) {
                    float2 delta = gv - starPos;
                    float rays = max(0.0, 1.0 - abs(delta.x * delta.y * 950.0));
                    rays *= smoothstep(0.42, 0.0, d);
                    flare = rays * flareStrength;
                }

                float twinkleSpeed = lerp(1.4, 4.2, frac(n * 19.1));
                float twinklePhase = n * 62.8318;
                float twinkle = 0.65 + 0.35 * sin(time * twinkleSpeed + twinklePhase);

                float temp = frac(n * 53.9);
                float3 starColor;
                if (temp < 0.35) {
                    starColor = float3(0.55, 0.75, 1.0);
                } else if (temp < 0.75) {
                    starColor = float3(0.95, 0.97, 1.0);
                } else if (temp < 0.92) {
                    starColor = float3(1.0, 0.88, 0.62);
                } else {
                    starColor = float3(1.0, 0.72, 0.52);
                }

                float brightness = (starCore * 1.35 + starGlow + flare) * twinkle;
                col += starColor * brightness;
            }
        }
    }
    return col;
}

// 流れ星
float3 ShootingStar(float2 uv, float time) {
    float cycle = 6.0;
    float t = fmod(time + 1.8, cycle);
    float periodId = floor((time + 1.8) / cycle);

    if (t > 0.85) {
        return float3(0.0, 0.0, 0.0);
    }

    float progress = t / 0.85;
    float rnd1 = Hash21(float2(periodId, 11.3));
    float rnd2 = Hash21(float2(periodId, 27.9));

    float2 startPos = float2(lerp(-0.5, 0.6, rnd1), lerp(0.14, 0.44, rnd2));
    float2 dir = normalize(float2(0.85, -0.45));
    float travel = 0.68;

    float2 head = startPos + dir * (progress * travel);
    float tailLength = 0.18 * sin(progress * 3.14159265);

    float2 pa = uv - head;
    float proj = clamp(dot(pa, -dir), 0.0, tailLength);
    float dist = length(pa - (-dir) * proj);

    float tailFade = 1.0 - (proj / max(tailLength, 0.0001));
    float alpha = smoothstep(0.0042, 0.0, dist) * pow(tailFade, 2.0);
    float headGlow = exp(-length(pa) * 145.0) * 1.25;

    float fadeInOut = sin(progress * 3.14159265);
    return float3(0.75, 0.88, 1.0) * (alpha * 1.6 + headGlow) * fadeInOut;
}

PixelShaderOutput main(VertexShaderOutput input) {
    PixelShaderOutput output;

    float2 uv = input.texcoord;
    float2 p = float2((uv.x - 0.5) * gParams.aspectRatio, 0.5 - uv.y);

    // カメラ空間の視線ベクトルをピッチ・ヨー回転してワールド空間の視線ベクトルに変換
    float3 rawDir = normalize(float3(p.x, p.y + 0.14, 1.05));
    float cPitch = cos(gParams.camPitch);
    float sPitch = sin(gParams.camPitch);
    float3 pitchedDir = float3(
        rawDir.x,
        rawDir.y * cPitch - rawDir.z * sPitch,
        rawDir.y * sPitch + rawDir.z * cPitch
    );
    float cYaw = cos(gParams.camYaw);
    float sYaw = sin(gParams.camYaw);
    float3 rayDir = normalize(float3(
        pitchedDir.x * cYaw - pitchedDir.z * sYaw,
        pitchedDir.y,
        pitchedDir.x * sYaw + pitchedDir.z * cYaw
    ));

    float heightFactor = saturate((rayDir.y + 0.30) / 0.80); // 0: 地平線側, 1: 天頂側
    float2 skyP = float2(atan2(rayDir.x, rayDir.z), asin(clamp(rayDir.y, -0.999, 0.999)) - 0.13);

    // 1. 夜空のベースグラデーション（天頂に向かって深く暗く落ちていくグラデーション）
    float3 zenithColor  = float3(0.00025, 0.0006, 0.0035); // 天頂：漆黒に近い極深の紺
    float3 upperColor   = float3(0.0010,  0.0026, 0.0130); // 上空：深いミッドナイトブルー
    float3 midSkyColor  = float3(0.0028,  0.0072, 0.0280); // 中空：夜の濃紺
    float3 horizonColor = float3(0.0095,  0.0210, 0.0560); // 地平線：夜の淡い蒼み

    float3 skyColor = lerp(horizonColor, midSkyColor, smoothstep(0.0, 0.30, heightFactor));
    skyColor = lerp(skyColor, upperColor, smoothstep(0.28, 0.68, heightFactor));
    skyColor = lerp(skyColor, zenithColor, smoothstep(0.60, 1.0, heightFactor));

    // 画面上端に向かうにつれてさらに暗く引き締めるビネット/大気減衰
    float topDarken = lerp(1.0, 0.55, smoothstep(0.55, 1.0, heightFactor));
    skyColor *= topDarken;

    // 2. 天の川 (Milky Way)
    float2 mwUV = float2(skyP.x * 0.75 + skyP.y * 0.65, -skyP.x * 0.65 + skyP.y * 0.75);
    float mwBand = exp(-pow((mwUV.y - 0.05 + sin(mwUV.x * 2.2) * 0.06) * 4.4, 2.0));
    float mwNoise = Fbm5(mwUV * 3.8 + float2(2.4, 1.1));
    float mwDust  = Fbm5(mwUV * 7.5 + float2(8.3, 4.7));
    float mwIntensity = mwBand * smoothstep(0.28, 0.85, mwNoise) * (1.0 - smoothstep(0.45, 0.85, mwDust) * 0.58);
    mwIntensity *= smoothstep(0.05, 0.35, heightFactor);

    float3 milkyWayColor = lerp(float3(0.005, 0.010, 0.036), float3(0.016, 0.024, 0.068), mwNoise);
    skyColor += milkyWayColor * mwIntensity * 0.85;

    // 3. 星空 (複数レイヤー + ゆっくりした日周運動 + 瞬き)
    float starVisibility = smoothstep(0.02, 0.25, heightFactor);
    float2 starCoord = skyP + float2(gParams.time * 0.0014, gParams.time * 0.00035);

    float3 stars = float3(0.0, 0.0, 0.0);
    stars += StarLayer(starCoord, 44.0, gParams.time, 0.72, 0.0) * (0.28 + mwIntensity * 0.65);
    stars += StarLayer(starCoord + float2(5.2, 3.1), 22.0, gParams.time, 0.80, 0.0) * 0.62;
    stars += StarLayer(starCoord + float2(11.4, 7.8), 9.5, gParams.time, 0.86, 0.35) * 1.15;

    skyColor += stars * starVisibility;

    // 4. 流れ星
    skyColor += ShootingStar(skyP, gParams.time) * starVisibility;

    // 5. 月は描画せず、雲のライティング基準方向のみ定義
    float3 moonDir = normalize(float3(0.58, 0.42, 1.05));
    float moonAngleDist = acos(clamp(dot(rayDir, moonDir), -1.0, 1.0));

    // 6. 透視投影によるリアルな夜の雲 (2レイヤー・パララックス移動・月光ライティング)
    // 空の上向き成分が正の領域で、雲平面への射影座標を求める
    float projY = max(rayDir.y + 0.06, 0.08);
    float distanceFade = smoothstep(0.01, 0.16, rayDir.y + 0.06); // 地平線ギリギリは霞に溶ける

    // --- 奥の高層薄雲 (空全体に大きく広がる薄雲) ---
    float2 highCloudUV = rayDir.xz / (projY * 1.25) * 1.75;
    highCloudUV += float2(gParams.time * 0.012, gParams.time * 0.004);
    float highDensity = CloudDensity(highCloudUV + float2(12.3, 4.1), gParams.time * 0.5);
    float highAlpha = smoothstep(0.20, 0.56, highDensity) * 0.68 * distanceFade;

    float moonScatter = exp(-moonAngleDist * 3.2);
    float3 highCloudCol = lerp(float3(0.012, 0.022, 0.052), float3(0.095, 0.135, 0.24), moonScatter);
    skyColor = lerp(skyColor, highCloudCol, highAlpha);

    // --- 手前の低層立体雲 (空を覆う豊かな立体雲) ---
    float2 lowCloudUV = rayDir.xz / projY * 1.45;
    lowCloudUV += float2(-gParams.time * 0.024, gParams.time * 0.006); // 少しずつ動いて見える速度

    float lowRaw = CloudDensity(lowCloudUV, gParams.time);
    float lowAlpha = smoothstep(0.22, 0.52, lowRaw) * 0.94 * distanceFade;

    if (lowAlpha > 0.001) {
        // 月の方向へ少しオフセットして雲の厚みの差(光の透過・輪郭の輝き)を計算
        float2 toMoon = normalize(moonDir.xz - rayDir.xz + float2(0.0001, 0.0001));
        float2 sampleDir = toMoon * 0.08;
        float shadowRaw = CloudDensity(lowCloudUV + sampleDir, gParams.time);

        // 光の当たり具合: 手前が薄く、月側に向かって開けているほど明るい
        float lightDiff = saturate((lowRaw - shadowRaw) * 3.8 + 0.48);
        // 雲の薄い縁が月光で銀色に光る (シルバーライニング)
        float edgeGlow = smoothstep(0.22, 0.35, lowRaw) * (1.0 - smoothstep(0.35, 0.54, lowRaw));
        float silverLining = edgeGlow * exp(-moonAngleDist * 2.2) * 1.65;

        // 雲の暗い底色 〜 月光に照らされた表層色
        float3 cloudDark = float3(0.005, 0.010, 0.028);
        float3 cloudLit  = lerp(float3(0.032, 0.054, 0.115), float3(0.15, 0.22, 0.36), exp(-moonAngleDist * 2.4));
        float3 lowCloudCol = lerp(cloudDark, cloudLit, lightDiff);
        lowCloudCol += float3(0.22, 0.30, 0.48) * silverLining;

        // 上空に行くほど雲も少しずつ暗く馴染む
        lowCloudCol *= lerp(1.0, 0.72, smoothstep(0.55, 1.0, heightFactor));

        skyColor = lerp(skyColor, lowCloudCol, lowAlpha);
    }

    output.color = float4(skyColor, 1.0);
    return output;
}
