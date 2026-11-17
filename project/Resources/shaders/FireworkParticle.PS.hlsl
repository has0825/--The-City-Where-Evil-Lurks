#include "Particle.hlsli"

ConstantBuffer<Material> gMaterial : register(b0);

Texture2D<float32_t4> gTexture : register(t0);
SamplerState gSampler : register(s0);

struct PixelShaderOutput
{
    float32_t4 color : SV_Target0;
};

PixelShaderOutput main(VertexShaderOutput input)
{
    PixelShaderOutput output;

    float4 transformedUV = mul(float32_t4(input.texcoord, 0.0f, 1.0f), gMaterial.uvTransform);
    float32_t4 textureColor = gTexture.Sample(gSampler, transformedUV.xy);

    // UV中心(0.5, 0.5)からの距離に基づいた滑らかな光学グロー
    // 四角い板ポリゴンの端で確実に0.0になるようにフェードアウトさせ、ポリゴン境界を完全に消す
    float2 centeredUV = input.texcoord - float2(0.5f, 0.5f);
    float dist = length(centeredUV) * 2.0f; // 中心=0.0, 辺=1.0
    float edgeMask = saturate(1.0f - smoothstep(0.65f, 1.0f, dist));

    // 中心の白熱コア + 周囲の柔らかな大気散乱グロー
    float core = exp(-dist * dist * 16.0f);
    float halo = exp(-dist * dist * 3.8f) * 0.55f;
    float glow = (core + halo) * edgeMask;

    // テクスチャの輝度も活かしつつ滑らかにブレンド
    float texLum = dot(textureColor.rgb, float3(0.299f, 0.587f, 0.114f)) * textureColor.a;
    float intensity = lerp(glow, glow * (0.5f + 0.5f * texLum), 0.35f);

    // 中心の白熱部(core)は少し白に近づけて高温の火花らしさを出す
    float3 baseColor = gMaterial.color.rgb * input.color.rgb;
    float3 hotColor = lerp(baseColor, float3(1.0f, 0.98f, 0.94f), saturate(core * 0.72f));

    float finalAlpha = saturate(intensity * gMaterial.color.a * input.color.a);
    if (finalAlpha <= 0.001f) {
        discard;
    }

    // 加算合成用に輝度を出力 (SrcBlend = SRC_ALPHA なので rgb * alpha で加算される)
    output.color = float4(hotColor, finalAlpha);
    return output;
}
