#include "Particle.hlsli"

ConstantBuffer<Material> gMaterial : register(b0);

Texture2D<float32_t4> gTexture : register(t0);
SamplerState gSampler : register(s0);

struct PixelShaderOutput
{
    float32_t4 color : SV_Target0;
};

float Hash21(float2 p) {
    p = frac(p * float2(123.34f, 456.21f));
    p += dot(p, p + 45.32f);
    return frac(p.x * p.y);
}

float Noise(float2 p) {
    float2 i = floor(p);
    float2 f = frac(p);
    f = f * f * (3.0f - 2.0f * f);
    float a = Hash21(i);
    float b = Hash21(i + float2(1.0f, 0.0f));
    float c = Hash21(i + float2(0.0f, 1.0f));
    float d = Hash21(i + float2(1.0f, 1.0f));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

PixelShaderOutput main(VertexShaderOutput input)
{
    PixelShaderOutput output;

    float4 transformedUV = mul(float32_t4(input.texcoord, 0.0f, 1.0f), gMaterial.uvTransform);
    float32_t4 textureColor = gTexture.Sample(gSampler, transformedUV.xy);

    // 雲のような柔らかいもこもこ感を作る（UV中心からの距離 + 2オクターブノイズ）
    float2 centeredUV = input.texcoord - float2(0.5f, 0.5f);
    float dist = length(centeredUV) * 2.0f;

    float n1 = Noise(input.texcoord * 4.5f);
    float n2 = Noise(input.texcoord * 9.0f + float2(3.1f, 7.4f));
    float wisp = n1 * 0.65f + n2 * 0.35f;

    // 縁を柔らかくぼかしつつ雲の濃淡を出す
    float radial = saturate(1.0f - smoothstep(0.15f, 0.98f, dist + (wisp - 0.5f) * 0.35f));
    float softCore = exp(-dist * dist * 2.4f) * saturate(1.0f - smoothstep(0.65f, 1.0f, dist));
    float cloudShape = lerp(radial, softCore, 0.45f);

    float texLum = saturate(textureColor.a);
    float finalAlpha = saturate(cloudShape * (0.65f + 0.35f * texLum) * gMaterial.color.a * input.color.a);

    if (finalAlpha <= 0.002f) {
        discard;
    }

    // 上側が月光で少し明るく、下側がほんのり暗い立体的な雲の陰影
    float shade = saturate(0.75f + (0.5f - input.texcoord.y) * 0.45f);
    float3 cloudCol = gMaterial.color.rgb * input.color.rgb * shade;

    output.color = float4(cloudCol, finalAlpha);
    return output;
}
