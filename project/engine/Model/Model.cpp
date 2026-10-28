#include "Model.h"
#include "DirectXCommon.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <unordered_map>
#include <windows.h>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include "SrvManager.h"
#include "GraphicsPipeline.h"

void Model::ReadNodeHierarchy(aiNode* node, Node& outNode) {
    aiMatrix4x4 aiLocalMatrix = node->mTransformation;
    aiLocalMatrix.Transpose();

    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            outNode.localMatrix.m[i][j] = aiLocalMatrix[i][j];
        }
    }

    outNode.name = node->mName.C_Str();
    outNode.children.resize(node->mNumChildren);
    for (uint32_t i = 0; i < node->mNumChildren; ++i) {
        ReadNodeHierarchy(node->mChildren[i], outNode.children[i]);
    }
}

std::unique_ptr<Model> Model::CreateParticleModel(ID3D12Device* device) {
    std::unique_ptr<Model> model = std::make_unique<Model>();
    ModelData modelData;

    modelData.vertices.push_back({ { -1.0f, 1.0f, 0.0f, 1.0f }, { 0.0f, 0.0f }, { 0.0f, 0.0f, -1.0f } });
    modelData.vertices.push_back({ { 1.0f, 1.0f, 0.0f, 1.0f },  { 1.0f, 0.0f }, { 0.0f, 0.0f, -1.0f } });
    modelData.vertices.push_back({ { -1.0f, -1.0f, 0.0f, 1.0f }, { 0.0f, 1.0f }, { 0.0f, 0.0f, -1.0f } });
    modelData.vertices.push_back({ { 1.0f, -1.0f, 0.0f, 1.0f }, { 1.0f, 1.0f }, { 0.0f, 0.0f, -1.0f } });
    modelData.indices = { 0, 1, 2, 2, 1, 3 };

    model->Initialize(modelData, device);
    return model;
}

std::unique_ptr<Model> Model::CreateRingModel(ID3D12Device* device) {
    std::unique_ptr<Model> model = std::make_unique<Model>();
    ModelData modelData;

    const uint32_t kRingDivide = 32;
    const float kOuterRadius = 1.0f;
    const float kInnerRadius = 0.2f;
    const float radianPerDivide = 2.0f * 3.14159265358979323846f / float(kRingDivide);

    for (uint32_t index = 0; index < kRingDivide; ++index) {
        float s = std::sin(index * radianPerDivide);
        float c = std::cos(index * radianPerDivide);
        float sNext = std::sin((index + 1) * radianPerDivide);
        float cNext = std::cos((index + 1) * radianPerDivide);
        float u = float(index) / float(kRingDivide);
        float uNext = float(index + 1) / float(kRingDivide);

        VertexData v1 = { {-s * kOuterRadius, c * kOuterRadius, 0.0f, 1.0f}, {u, 0.0f}, {0.0f, 0.0f, -1.0f} };
        VertexData v2 = { {-sNext * kOuterRadius, cNext * kOuterRadius, 0.0f, 1.0f}, {uNext, 0.0f}, {0.0f, 0.0f, -1.0f} };
        VertexData v3 = { {-s * kInnerRadius, c * kInnerRadius, 0.0f, 1.0f}, {u, 1.0f}, {0.0f, 0.0f, -1.0f} };
        VertexData v4 = { {-sNext * kInnerRadius, cNext * kInnerRadius, 0.0f, 1.0f}, {uNext, 1.0f}, {0.0f, 0.0f, -1.0f} };

        uint32_t start = (uint32_t)modelData.vertices.size();
        modelData.vertices.push_back(v1);
        modelData.vertices.push_back(v2);
        modelData.vertices.push_back(v3);
        modelData.vertices.push_back(v4);

        modelData.indices.push_back(start + 2);
        modelData.indices.push_back(start + 0);
        modelData.indices.push_back(start + 1);

        modelData.indices.push_back(start + 2);
        modelData.indices.push_back(start + 1);
        modelData.indices.push_back(start + 3);
    }

    model->Initialize(modelData, device);
    return model;
}

std::unique_ptr<Model> Model::CreateCylinderModel(ID3D12Device* device) {
    std::unique_ptr<Model> model = std::make_unique<Model>();
    ModelData modelData;

    const uint32_t kCylinderDivide = 32;
    const float kTopRadius = 1.0f;
    const float kBottomRadius = 1.0f;
    const float kHeight = 3.0f;
    const float radianPerDivide = 2.0f * 3.14159265358979323846f / float(kCylinderDivide);

    for (uint32_t index = 0; index < kCylinderDivide; ++index) {
        float s = std::sin(index * radianPerDivide);
        float c = std::cos(index * radianPerDivide);
        float sNext = std::sin((index + 1) * radianPerDivide);
        float cNext = std::cos((index + 1) * radianPerDivide);
        float u = float(index) / float(kCylinderDivide);
        float uNext = float(index + 1) / float(kCylinderDivide);

        VertexData v1 = { {-s * kTopRadius, kHeight, c * kTopRadius, 1.0f}, {u, 0.0f}, {-s, 0.0f, c} };
        VertexData v2 = { {-sNext * kTopRadius, kHeight, cNext * kTopRadius, 1.0f}, {uNext, 0.0f}, {-sNext, 0.0f, cNext} };
        VertexData v3 = { {-s * kBottomRadius, 0.0f, c * kBottomRadius, 1.0f}, {u, 1.0f}, {-s, 0.0f, c} };
        VertexData v4 = { {-sNext * kBottomRadius, 0.0f, cNext * kBottomRadius, 1.0f}, {uNext, 1.0f}, {-sNext, 0.0f, cNext} };

        uint32_t start = (uint32_t)modelData.vertices.size();
        modelData.vertices.push_back(v1);
        modelData.vertices.push_back(v2);
        modelData.vertices.push_back(v3);
        modelData.vertices.push_back(v4);

        modelData.indices.push_back(start + 2);
        modelData.indices.push_back(start + 1);
        modelData.indices.push_back(start + 0);

        modelData.indices.push_back(start + 2);
        modelData.indices.push_back(start + 3);
        modelData.indices.push_back(start + 1);
    }

    model->Initialize(modelData, device);
    return model;
}

std::unique_ptr<Model> Model::CreateSphereModel(ID3D12Device* device) {
    std::unique_ptr<Model> model = std::make_unique<Model>();
    ModelData modelData;
    const uint32_t kSubdivision = 16;
    const float kLatStep = 3.1415926535f / float(kSubdivision);
    const float kLonStep = 2.0f * 3.1415926535f / float(kSubdivision);

    for (uint32_t lat = 0; lat <= kSubdivision; ++lat) {
        float phi = lat * kLatStep;
        for (uint32_t lon = 0; lon <= kSubdivision; ++lon) {
            float theta = lon * kLonStep;
            Vector3 p = { std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta) };
            modelData.vertices.push_back({ {p.x, p.y, p.z, 1.0f}, {(float)lon / kSubdivision, (float)lat / kSubdivision}, p });
        }
    }
    for (uint32_t lat = 0; lat < kSubdivision; ++lat) {
        for (uint32_t lon = 0; lon < kSubdivision; ++lon) {
            uint32_t v1 = lat * (kSubdivision + 1) + lon;
            uint32_t v2 = v1 + 1;
            uint32_t v3 = (lat + 1) * (kSubdivision + 1) + lon;
            uint32_t v4 = v3 + 1;
            modelData.indices.push_back(v1); modelData.indices.push_back(v2); modelData.indices.push_back(v3);
            modelData.indices.push_back(v3); modelData.indices.push_back(v2); modelData.indices.push_back(v4);
        }
    }
    model->Initialize(modelData, device);
    return model;
}

std::unique_ptr<Model> Model::CreateBoxModel(ID3D12Device* device) {
    std::unique_ptr<Model> model = std::make_unique<Model>();
    ModelData modelData;
    // 0,0,0 から 0,0,1 に向かう 0.1x0.1 のボックス
    Vector3 p[8] = {
        {-0.05f,-0.05f, 0}, {0.05f,-0.05f, 0}, {-0.05f, 0.05f, 0}, {0.05f, 0.05f, 0},
        {-0.05f,-0.05f, 1}, {0.05f,-0.05f, 1}, {-0.05f, 0.05f, 1}, {0.05f, 0.05f, 1}
    };
    uint32_t indices[] = {
        0,2,1, 1,2,3, 4,5,6, 5,7,6, 0,1,4, 1,5,4,
        2,6,3, 3,6,7, 0,4,2, 4,6,2, 1,3,5, 3,7,5
    };
    for (int i = 0; i < 8; i++) {
        modelData.vertices.push_back({ {p[i].x, p[i].y, p[i].z, 1.0f}, {0,0}, {0,0,0} });
    }
    for (uint32_t i : indices) {
        modelData.indices.push_back(i);
    }
    model->Initialize(modelData, device);
    return model;
}

struct GLTFCacheData {
    ModelData modelData;
    Node rootNode;
    std::map<std::string, Bone> bones;
    std::shared_ptr<Animation> animation;
    Microsoft::WRL::ComPtr<ID3D12Resource> vertexResource;
    D3D12_VERTEX_BUFFER_VIEW vertexBufferView{};
    Microsoft::WRL::ComPtr<ID3D12Resource> indexResource;
    D3D12_INDEX_BUFFER_VIEW indexBufferView{};
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
};

static std::map<std::string, GLTFCacheData> s_GLTFCache;

namespace {
inline const char* SkipSpaces(const char* p, const char* end) {
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    return p;
}

inline const char* ParseFloatFast(const char* p, const char* end, float& outVal) {
    p = SkipSpaces(p, end);
    if (p >= end) { outVal = 0.0f; return p; }
    char* next = nullptr;
    outVal = std::strtof(p, &next);
    return (next && next > p) ? next : p;
}

inline const char* ParseIntFast(const char* p, const char* end, int& outVal) {
    p = SkipSpaces(p, end);
    int sign = 1;
    if (p < end && *p == '-') { sign = -1; ++p; }
    else if (p < end && *p == '+') { ++p; }
    int val = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        val = val * 10 + (*p - '0');
        ++p;
    }
    outVal = val * sign;
    return p;
}

struct ObjCorner {
    int v = 0;
    int vt = 0;
    int vn = 0;
    bool operator==(const ObjCorner& o) const {
        return v == o.v && vt == o.vt && vn == o.vn;
    }
};

struct ObjCornerHash {
    size_t operator()(const ObjCorner& c) const {
        size_t h = (size_t)c.v * 73856093u;
        h ^= (size_t)c.vt * 19349663u;
        h ^= (size_t)c.vn * 83492791u;
        return h;
    }
};

// 高速OBJローダー & 超高ポリゴンメッシュの空間グリッド簡略化 (LOD)
bool LoadObjFileFast(const std::string& filename, ModelData& outModelData, Node& outRootNode) {
    FILE* fp = nullptr;
    if (fopen_s(&fp, filename.c_str(), "rb") != 0 || !fp) {
        return false;
    }
    fseek(fp, 0, SEEK_END);
    long fileSize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (fileSize <= 0) {
        fclose(fp);
        return false;
    }

    std::vector<char> buffer((size_t)fileSize + 1);
    size_t bytesRead = fread(buffer.data(), 1, (size_t)fileSize, fp);
    fclose(fp);
    buffer[bytesRead] = '\0';

    std::vector<Vector3> positions;
    std::vector<Vector2> texcoords;
    std::vector<Vector3> normals;
    positions.reserve(bytesRead / 64 + 16);
    texcoords.reserve(bytesRead / 64 + 16);
    normals.reserve(bytesRead / 64 + 16);

    struct RawTri {
        ObjCorner c[3];
    };
    std::vector<RawTri> rawTriangles;
    rawTriangles.reserve(bytesRead / 48 + 16);

    Vector3 minPos = { 1e30f, 1e30f, 1e30f };
    Vector3 maxPos = { -1e30f, -1e30f, -1e30f };

    const char* ptr = buffer.data();
    const char* end = ptr + bytesRead;

    ObjCorner faceCorners[16];

    while (ptr < end) {
        const char* lineEnd = ptr;
        while (lineEnd < end && *lineEnd != '\n' && *lineEnd != '\r') ++lineEnd;

        const char* p = SkipSpaces(ptr, lineEnd);
        if (p < lineEnd && *p != '#') {
            if (p[0] == 'v') {
                if (p + 1 < lineEnd && (p[1] == ' ' || p[1] == '\t')) {
                    float x = 0.0f, y = 0.0f, z = 0.0f;
                    p += 2;
                    p = ParseFloatFast(p, lineEnd, x);
                    p = ParseFloatFast(p, lineEnd, y);
                    p = ParseFloatFast(p, lineEnd, z);
                    // Assimp の aiProcess_ConvertToLeftHanded と同一変換 (Z反転)
                    z = -z;
                    positions.push_back({ x, y, z });
                    if (x < minPos.x) minPos.x = x;
                    if (y < minPos.y) minPos.y = y;
                    if (z < minPos.z) minPos.z = z;
                    if (x > maxPos.x) maxPos.x = x;
                    if (y > maxPos.y) maxPos.y = y;
                    if (z > maxPos.z) maxPos.z = z;
                } else if (p + 2 < lineEnd && p[1] == 't' && (p[2] == ' ' || p[2] == '\t')) {
                    float u = 0.0f, v = 0.0f;
                    p += 3;
                    p = ParseFloatFast(p, lineEnd, u);
                    p = ParseFloatFast(p, lineEnd, v);
                    // Assimp の FlipUVs と同じ V 反転
                    texcoords.push_back({ u, 1.0f - v });
                } else if (p + 2 < lineEnd && p[1] == 'n' && (p[2] == ' ' || p[2] == '\t')) {
                    float nx = 0.0f, ny = 0.0f, nz = 0.0f;
                    p += 3;
                    p = ParseFloatFast(p, lineEnd, nx);
                    p = ParseFloatFast(p, lineEnd, ny);
                    p = ParseFloatFast(p, lineEnd, nz);
                    nz = -nz;
                    normals.push_back({ nx, ny, nz });
                }
            } else if (p[0] == 'f' && p + 1 < lineEnd && (p[1] == ' ' || p[1] == '\t')) {
                p += 2;
                int cornerCount = 0;
                while (p < lineEnd && cornerCount < 16) {
                    p = SkipSpaces(p, lineEnd);
                    if (p >= lineEnd) break;
                    ObjCorner corner{};
                    p = ParseIntFast(p, lineEnd, corner.v);
                    if (p < lineEnd && *p == '/') {
                        ++p;
                        if (p < lineEnd && *p != '/') {
                            p = ParseIntFast(p, lineEnd, corner.vt);
                        }
                        if (p < lineEnd && *p == '/') {
                            ++p;
                            p = ParseIntFast(p, lineEnd, corner.vn);
                        }
                    }
                    while (p < lineEnd && *p != ' ' && *p != '\t') ++p;

                    if (corner.v < 0) corner.v = (int)positions.size() + 1 + corner.v;
                    if (corner.vt < 0) corner.vt = (int)texcoords.size() + 1 + corner.vt;
                    if (corner.vn < 0) corner.vn = (int)normals.size() + 1 + corner.vn;
                    faceCorners[cornerCount++] = corner;
                }
                // ファン分割 + ワインディング反転 (aiProcess_FlipWindingOrder 相当: 0, i+1, i)
                for (int i = 1; i + 1 < cornerCount; ++i) {
                    RawTri tri;
                    tri.c[0] = faceCorners[0];
                    tri.c[1] = faceCorners[i + 1];
                    tri.c[2] = faceCorners[i];
                    rawTriangles.push_back(tri);
                }
            }
        }

        ptr = lineEnd;
        while (ptr < end && (*ptr == '\n' || *ptr == '\r')) ++ptr;
    }

    if (positions.empty() || rawTriangles.empty()) {
        return false;
    }

    outRootNode.name = "root";
    outRootNode.localMatrix = MakeIdentity4x4();
    outRootNode.children.clear();

    const size_t kSimplifyThreshold = 50000;
    if (rawTriangles.size() > kSimplifyThreshold) {
        // 超高ポリゴンモデル(Player.obj: 85万ポリゴン、Spider: 45万ポリゴン、SpiderArm: 24万ポリゴン×8本)を
        // 水密な空間グリッドクラスタリングで高速簡略化し、GPUのオーバードローと頂点負荷を90%以上削減
        const int kGridRes = 96;
        Vector3 extent = {
            (std::max)(maxPos.x - minPos.x, 0.0001f),
            (std::max)(maxPos.y - minPos.y, 0.0001f),
            (std::max)(maxPos.z - minPos.z, 0.0001f)
        };
        float maxExtent = (std::max)(extent.x, (std::max)(extent.y, extent.z));
        float invCellSize = (float)kGridRes / maxExtent;

        std::vector<int> posCluster(positions.size());
        struct ClusterAccum {
            Vector3 posSum{ 0.0f, 0.0f, 0.0f };
            Vector2 uvSum{ 0.0f, 0.0f };
            uint32_t count = 0;
            uint32_t uvCount = 0;
        };
        std::unordered_map<uint64_t, uint32_t> cellToCluster;
        cellToCluster.reserve(positions.size() / 8 + 64);
        std::vector<ClusterAccum> clusters;
        clusters.reserve(positions.size() / 8 + 64);

        for (size_t i = 0; i < positions.size(); ++i) {
            const Vector3& pos = positions[i];
            int gx = (int)((pos.x - minPos.x) * invCellSize);
            int gy = (int)((pos.y - minPos.y) * invCellSize);
            int gz = (int)((pos.z - minPos.z) * invCellSize);
            gx = (std::max)(0, (std::min)(kGridRes, gx));
            gy = (std::max)(0, (std::min)(kGridRes, gy));
            gz = (std::max)(0, (std::min)(kGridRes, gz));

            uint64_t key = ((uint64_t)(uint32_t)gx << 40) | ((uint64_t)(uint32_t)gy << 20) | (uint64_t)(uint32_t)gz;
            auto it = cellToCluster.find(key);
            uint32_t clusterIdx = 0;
            if (it == cellToCluster.end()) {
                clusterIdx = (uint32_t)clusters.size();
                cellToCluster[key] = clusterIdx;
                clusters.push_back(ClusterAccum{});
            } else {
                clusterIdx = it->second;
            }
            posCluster[i] = (int)clusterIdx;
            clusters[clusterIdx].posSum.x += pos.x;
            clusters[clusterIdx].posSum.y += pos.y;
            clusters[clusterIdx].posSum.z += pos.z;
            clusters[clusterIdx].count++;
        }

        for (const auto& tri : rawTriangles) {
            for (int k = 0; k < 3; ++k) {
                int vi = tri.c[k].v - 1;
                int vti = tri.c[k].vt - 1;
                if (vi >= 0 && vi < (int)positions.size() && vti >= 0 && vti < (int)texcoords.size()) {
                    uint32_t cIdx = (uint32_t)posCluster[vi];
                    clusters[cIdx].uvSum.x += texcoords[vti].x;
                    clusters[cIdx].uvSum.y += texcoords[vti].y;
                    clusters[cIdx].uvCount++;
                }
            }
        }

        outModelData.vertices.resize(clusters.size());
        for (size_t i = 0; i < clusters.size(); ++i) {
            float invCount = clusters[i].count > 0 ? (1.0f / (float)clusters[i].count) : 1.0f;
            float invUvCount = clusters[i].uvCount > 0 ? (1.0f / (float)clusters[i].uvCount) : 0.0f;
            VertexData& v = outModelData.vertices[i];
            v = {};
            v.position = {
                clusters[i].posSum.x * invCount,
                clusters[i].posSum.y * invCount,
                clusters[i].posSum.z * invCount,
                1.0f
            };
            v.texcoord = {
                clusters[i].uvSum.x * invUvCount,
                clusters[i].uvSum.y * invUvCount
            };
            v.normal = { 0.0f, 0.0f, 0.0f };
        }

        outModelData.indices.reserve(rawTriangles.size() / 2);
        for (const auto& tri : rawTriangles) {
            int v0 = tri.c[0].v - 1;
            int v1 = tri.c[1].v - 1;
            int v2 = tri.c[2].v - 1;
            if (v0 < 0 || v0 >= (int)positions.size() ||
                v1 < 0 || v1 >= (int)positions.size() ||
                v2 < 0 || v2 >= (int)positions.size()) {
                continue;
            }
            uint32_t c0 = (uint32_t)posCluster[v0];
            uint32_t c1 = (uint32_t)posCluster[v1];
            uint32_t c2 = (uint32_t)posCluster[v2];
            if (c0 == c1 || c1 == c2 || c2 == c0) {
                continue; // 縮退三角形を除去
            }
            outModelData.indices.push_back(c0);
            outModelData.indices.push_back(c1);
            outModelData.indices.push_back(c2);

            const Vector4& p0 = outModelData.vertices[c0].position;
            const Vector4& p1 = outModelData.vertices[c1].position;
            const Vector4& p2 = outModelData.vertices[c2].position;
            Vector3 e1 = { p1.x - p0.x, p1.y - p0.y, p1.z - p0.z };
            Vector3 e2 = { p2.x - p0.x, p2.y - p0.y, p2.z - p0.z };
            Vector3 fn = Cross(e1, e2);
            outModelData.vertices[c0].normal.x += fn.x;
            outModelData.vertices[c0].normal.y += fn.y;
            outModelData.vertices[c0].normal.z += fn.z;
            outModelData.vertices[c1].normal.x += fn.x;
            outModelData.vertices[c1].normal.y += fn.y;
            outModelData.vertices[c1].normal.z += fn.z;
            outModelData.vertices[c2].normal.x += fn.x;
            outModelData.vertices[c2].normal.y += fn.y;
            outModelData.vertices[c2].normal.z += fn.z;
        }

        for (auto& v : outModelData.vertices) {
            float lenSq = v.normal.x * v.normal.x + v.normal.y * v.normal.y + v.normal.z * v.normal.z;
            if (lenSq > 1e-12f) {
                float invLen = 1.0f / std::sqrt(lenSq);
                v.normal.x *= invLen;
                v.normal.y *= invLen;
                v.normal.z *= invLen;
            } else {
                v.normal = { 0.0f, 1.0f, 0.0f };
            }
        }
        return !outModelData.vertices.empty() && !outModelData.indices.empty();
    }

    // 通常サイズのOBJモデル: 同一頂点インデックスを結合して頂点バッファを最適化
    std::unordered_map<ObjCorner, uint32_t, ObjCornerHash> uniqueVertices;
    uniqueVertices.reserve(rawTriangles.size() + 16);
    outModelData.vertices.reserve(rawTriangles.size() + 16);
    outModelData.indices.reserve(rawTriangles.size() * 3);

    bool hasFileNormals = !normals.empty();

    for (const auto& tri : rawTriangles) {
        uint32_t triIdx[3];
        bool valid = true;
        for (int k = 0; k < 3; ++k) {
            const ObjCorner& corner = tri.c[k];
            if (corner.v <= 0 || corner.v > (int)positions.size()) {
                valid = false;
                break;
            }
            auto it = uniqueVertices.find(corner);
            if (it != uniqueVertices.end()) {
                triIdx[k] = it->second;
            } else {
                uint32_t newIdx = (uint32_t)outModelData.vertices.size();
                uniqueVertices[corner] = newIdx;
                VertexData vd{};
                const Vector3& pos = positions[corner.v - 1];
                vd.position = { pos.x, pos.y, pos.z, 1.0f };
                if (corner.vt > 0 && corner.vt <= (int)texcoords.size()) {
                    vd.texcoord = texcoords[corner.vt - 1];
                }
                if (corner.vn > 0 && corner.vn <= (int)normals.size()) {
                    vd.normal = normals[corner.vn - 1];
                }
                outModelData.vertices.push_back(vd);
                triIdx[k] = newIdx;
            }
        }
        if (!valid) continue;
        outModelData.indices.push_back(triIdx[0]);
        outModelData.indices.push_back(triIdx[1]);
        outModelData.indices.push_back(triIdx[2]);

        if (!hasFileNormals) {
            const Vector4& p0 = outModelData.vertices[triIdx[0]].position;
            const Vector4& p1 = outModelData.vertices[triIdx[1]].position;
            const Vector4& p2 = outModelData.vertices[triIdx[2]].position;
            Vector3 e1 = { p1.x - p0.x, p1.y - p0.y, p1.z - p0.z };
            Vector3 e2 = { p2.x - p0.x, p2.y - p0.y, p2.z - p0.z };
            Vector3 fn = Cross(e1, e2);
            for (int k = 0; k < 3; ++k) {
                outModelData.vertices[triIdx[k]].normal.x += fn.x;
                outModelData.vertices[triIdx[k]].normal.y += fn.y;
                outModelData.vertices[triIdx[k]].normal.z += fn.z;
            }
        }
    }

    if (!hasFileNormals) {
        for (auto& v : outModelData.vertices) {
            float lenSq = v.normal.x * v.normal.x + v.normal.y * v.normal.y + v.normal.z * v.normal.z;
            if (lenSq > 1e-12f) {
                float invLen = 1.0f / std::sqrt(lenSq);
                v.normal.x *= invLen;
                v.normal.y *= invLen;
                v.normal.z *= invLen;
            } else {
                v.normal = { 0.0f, 1.0f, 0.0f };
            }
        }
    }

    return !outModelData.vertices.empty();
}
} // namespace

std::unique_ptr<Model> Model::LoadGLTF(const std::string& filename, ID3D12Device* device) {
    std::unique_ptr<Model> model = std::make_unique<Model>();

    // キャッシュを検索 (GPU頂点/インデックスバッファも共有し、シーン遷移時の数百MBコピー＆再生成を回避)
    auto it = s_GLTFCache.find(filename);
    if (it != s_GLTFCache.end()) {
        const auto& cache = it->second;
        model->rootNode = cache.rootNode;
        model->bones_ = cache.bones;
        if (cache.animation) {
            model->animation_ = std::make_unique<Animation>(*cache.animation);
        }
        if (cache.vertexResource) {
            model->vertexResource_ = cache.vertexResource;
            model->vertexBufferView_ = cache.vertexBufferView;
            model->indexResource_ = cache.indexResource;
            model->indexBufferView_ = cache.indexBufferView;
            model->vertexCount_ = cache.vertexCount;
            model->indexCount_ = cache.indexCount;
            model->InitializeMaterialAndSkinning(device);
            return model;
        }
        model->Initialize(cache.modelData, device);
        return model;
    }

    ModelData modelData;

    bool isObjFile = (filename.size() >= 4 &&
        (filename.compare(filename.size() - 4, 4, ".obj") == 0 ||
         filename.compare(filename.size() - 4, 4, ".OBJ") == 0));

    if (isObjFile && LoadObjFileFast(filename, modelData, model->rootNode)) {
        model->Initialize(modelData, device);

        GLTFCacheData cache;
        cache.rootNode = model->rootNode;
        cache.vertexResource = model->vertexResource_;
        cache.vertexBufferView = model->vertexBufferView_;
        cache.indexResource = model->indexResource_;
        cache.indexBufferView = model->indexBufferView_;
        cache.vertexCount = model->vertexCount_;
        cache.indexCount = model->indexCount_;
        s_GLTFCache[filename] = std::move(cache);
        return model;
    }

    Assimp::Importer importer;
    // aiProcess_JoinIdenticalVertices を追加して重複頂点を結合する
    const aiScene* scene = importer.ReadFile(filename,
        aiProcess_Triangulate | aiProcess_GenNormals | aiProcess_JoinIdenticalVertices | aiProcess_LimitBoneWeights | aiProcess_ConvertToLeftHanded | aiProcess_PopulateArmatureData);

    if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode) {
        std::string errorMsg = "Assimp Error: " + std::string(importer.GetErrorString()) + "\n";
        OutputDebugStringA(errorMsg.c_str());
        return CreateParticleModel(device);
    }

    if (scene->mRootNode) {
        model->ReadNodeHierarchy(scene->mRootNode, model->rootNode);
    }

    // メッシュの解析
    uint32_t vertexOffset = 0;
    for (unsigned int i = 0; i < scene->mNumMeshes; i++) {
        aiMesh* mesh = scene->mMeshes[i];
        
        // 頂点ごとのウェイト情報を一時保存
        struct Weight {
            uint32_t index;
            float weight;
        };
        std::vector<std::vector<Weight>> vertexWeights(mesh->mNumVertices);

        for (unsigned int b = 0; b < mesh->mNumBones; b++) {
            aiBone* aiBone = mesh->mBones[b];
            std::string boneName = aiBone->mName.C_Str();
            
            Bone bone;
            bone.name = boneName;
            bone.index = b;
            
            aiMatrix4x4 aiOffset = aiBone->mOffsetMatrix;
            aiOffset.Transpose();
            for (int r = 0; r < 4; r++)
                for (int c = 0; c < 4; c++)
                    bone.offsetMatrix.m[r][c] = aiOffset[r][c];

            model->bones_[boneName] = bone;

            for (unsigned int w = 0; w < aiBone->mNumWeights; w++) {
                vertexWeights[aiBone->mWeights[w].mVertexId].push_back({ b, aiBone->mWeights[w].mWeight });
            }
        }

        // 頂点解析 (累積追加するように変更)
        size_t prevSize = modelData.vertices.size();
        modelData.vertices.resize(prevSize + mesh->mNumVertices);
        for (unsigned int v = 0; v < mesh->mNumVertices; v++) {
            aiVector3D& position = mesh->mVertices[v];
            
            // 法線とUVの存在チェックとデフォルト値
            aiVector3D normal = mesh->HasNormals() ? mesh->mNormals[v] : aiVector3D(0.0f, 1.0f, 0.0f);
            aiVector3D texcoord = mesh->HasTextureCoords(0) ? mesh->mTextureCoords[0][v] : aiVector3D(0.0f, 0.0f, 0.0f);
            
            VertexData& vertex = modelData.vertices[prevSize + v];
            vertex = {}; // ゼロ初期化

            // 元の座標系に戻す（Assimpのフラグに任せる）
            vertex.position = { position.x, position.y, position.z, 1.0f };
            vertex.normal = { normal.x, normal.y, normal.z };
            vertex.texcoord = { texcoord.x, texcoord.y };

            // ウェイト情報の書き込み
            const auto& weights = vertexWeights[v];
            for (size_t w = 0; w < weights.size() && w < 4; w++) {
                vertex.jointIndices[w] = weights[w].index;
                vertex.jointWeights[w] = weights[w].weight;
            }
        }

        // インデックス解析 (現在のメッシュの頂点オフセットを加算)
        for (unsigned int j = 0; j < mesh->mNumFaces; j++) {
            aiFace& face = mesh->mFaces[j];
            assert(face.mNumIndices == 3);
            for (unsigned int k = 0; k < face.mNumIndices; k++) {
                modelData.indices.push_back(face.mIndices[k] + vertexOffset);
            }
        }

        vertexOffset += mesh->mNumVertices;
    }

    if (modelData.vertices.empty()) {
        std::string errorMsg = "Model Error: No vertices found in model file: " + filename + "\n";
        OutputDebugStringA(errorMsg.c_str());
        return CreateParticleModel(device);
    }

    // アニメーションの読み込み（最初の1つ）
    if (scene->mNumAnimations > 0) {
        aiAnimation* aiAnim = scene->mAnimations[0];
        model->animation_ = std::make_unique<Animation>();
        model->animation_->name = aiAnim->mName.C_Str();
        float ticksPerSecond = (float)aiAnim->mTicksPerSecond != 0 ? (float)aiAnim->mTicksPerSecond : 24.0f;
        model->animation_->duration = (float)aiAnim->mDuration / ticksPerSecond;
        model->animation_->ticksPerSecond = ticksPerSecond;

        for (unsigned int i = 0; i < aiAnim->mNumChannels; i++) {
            aiNodeAnim* aiChannel = aiAnim->mChannels[i];
            AnimationChannel channel;
            channel.nodeName = aiChannel->mNodeName.C_Str();

            for (unsigned int k = 0; k < aiChannel->mNumPositionKeys; k++) {
                channel.positionKeys.push_back({ (float)aiChannel->mPositionKeys[k].mTime / ticksPerSecond, 
                    { aiChannel->mPositionKeys[k].mValue.x, aiChannel->mPositionKeys[k].mValue.y, aiChannel->mPositionKeys[k].mValue.z } });
            }
            for (unsigned int k = 0; k < aiChannel->mNumRotationKeys; k++) {
                channel.rotationKeys.push_back({ (float)aiChannel->mRotationKeys[k].mTime / ticksPerSecond, 
                    { aiChannel->mRotationKeys[k].mValue.x, aiChannel->mRotationKeys[k].mValue.y, aiChannel->mRotationKeys[k].mValue.z, aiChannel->mRotationKeys[k].mValue.w } });
            }
            for (unsigned int k = 0; k < aiChannel->mNumScalingKeys; k++) {
                channel.scaleKeys.push_back({ (float)aiChannel->mScalingKeys[k].mTime / ticksPerSecond, 
                    { aiChannel->mScalingKeys[k].mValue.x, aiChannel->mScalingKeys[k].mValue.y, aiChannel->mScalingKeys[k].mValue.z } });
            }
            model->animation_->channels.push_back(channel);
        }
    }

    model->Initialize(modelData, device);

    // 解析データとGPUバッファをキャッシュに保存
    GLTFCacheData cache;
    cache.rootNode = model->rootNode;
    cache.bones = model->bones_;
    if (model->animation_) {
        cache.animation = std::make_shared<Animation>(*model->animation_);
    }
    cache.vertexResource = model->vertexResource_;
    cache.vertexBufferView = model->vertexBufferView_;
    cache.indexResource = model->indexResource_;
    cache.indexBufferView = model->indexBufferView_;
    cache.vertexCount = model->vertexCount_;
    cache.indexCount = model->indexCount_;
    s_GLTFCache[filename] = std::move(cache);

    return model;
}

void Model::Initialize(ID3D12Device* device, ModelCommonData* commonData) {
    commonData_ = commonData;
    if (commonData_) {
        vertexCount_ = static_cast<uint32_t>(commonData_->vertices.size());
        indexCount_ = static_cast<uint32_t>(commonData_->indices.size());
    }

    materialResource_ = CreateBufferResource(device, sizeof(Material));
    Material* materialData = nullptr;
    materialResource_->Map(0, nullptr, reinterpret_cast<void**>(&materialData));
    materialData->color = { 1.0f, 1.0f, 1.0f, 1.0f };
    materialData->enableLighting = 1;
    materialData->uvTransform = MakeIdentity4x4();
    materialData->shininess = 10.0f;
    materialData->environmentCoefficient = environmentCoefficient;
    materialResource_->Unmap(0, nullptr);

    transform = { {1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f} };
}

// 頂点/インデックスバッファをDEFAULTヒープ（VRAM）に作成してデータをアップロードするヘルパー関数
Microsoft::WRL::ComPtr<ID3D12Resource> CreateDefaultBuffer(
    ID3D12Device* device,
    const void* initData,
    UINT64 byteSize,
    Microsoft::WRL::ComPtr<ID3D12Resource>& uploadBuffer) {

    if (!device || !initData || byteSize == 0) {
        return nullptr;
    }

    D3D12_HEAP_PROPERTIES defaultHeapProps{};
    defaultHeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC bufferDesc{};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = byteSize;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    Microsoft::WRL::ComPtr<ID3D12Resource> defaultBuffer;
    HRESULT hr = device->CreateCommittedResource(
        &defaultHeapProps,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(&defaultBuffer));
    if (FAILED(hr) || !defaultBuffer) {
        return nullptr;
    }

    D3D12_HEAP_PROPERTIES uploadHeapProps{};
    uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    hr = device->CreateCommittedResource(
        &uploadHeapProps,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&uploadBuffer));
    if (FAILED(hr) || !uploadBuffer) {
        return nullptr;
    }

    void* mappedData = nullptr;
    hr = uploadBuffer->Map(0, nullptr, &mappedData);
    if (FAILED(hr) || !mappedData) {
        return nullptr;
    }
    std::memcpy(mappedData, initData, byteSize);
    uploadBuffer->Unmap(0, nullptr);

    DirectXCommon* dxCommon = DirectXCommon::GetInstance();
    ID3D12CommandQueue* commandQueue = dxCommon ? dxCommon->GetCommandQueue() : nullptr;
    if (!commandQueue) {
        return defaultBuffer;
    }
    
    static ID3D12Device* s_cachedDevice = nullptr;
    static Microsoft::WRL::ComPtr<ID3D12CommandAllocator> s_tempAllocator;
    static Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> s_tempCmdList;
    static Microsoft::WRL::ComPtr<ID3D12Fence> s_fence;
    static UINT64 s_fenceValue = 0;
    static HANDLE s_fenceEvent = nullptr;

    if (s_cachedDevice != device || !s_tempAllocator || !s_tempCmdList || !s_fence) {
        s_tempCmdList.Reset();
        s_tempAllocator.Reset();
        s_fence.Reset();
        if (s_fenceEvent) {
            CloseHandle(s_fenceEvent);
            s_fenceEvent = nullptr;
        }
        s_fenceValue = 0;

        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s_tempAllocator));
        if (FAILED(hr) || !s_tempAllocator) {
            return defaultBuffer;
        }

        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s_tempAllocator.Get(), nullptr, IID_PPV_ARGS(&s_tempCmdList));
        if (FAILED(hr) || !s_tempCmdList) {
            return defaultBuffer;
        }
        s_tempCmdList->Close();

        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s_fence));
        if (FAILED(hr) || !s_fence) {
            return defaultBuffer;
        }
        s_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        s_cachedDevice = device;
    }

    s_tempAllocator->Reset();
    s_tempCmdList->Reset(s_tempAllocator.Get(), nullptr);

    s_tempCmdList->CopyBufferRegion(defaultBuffer.Get(), 0, uploadBuffer.Get(), 0, byteSize);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = defaultBuffer.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_GENERIC_READ;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    s_tempCmdList->ResourceBarrier(1, &barrier);

    s_tempCmdList->Close();

    ID3D12CommandList* cmdLists[] = { s_tempCmdList.Get() };
    commandQueue->ExecuteCommandLists(1, cmdLists);

    if (s_fence && s_fenceEvent) {
        ++s_fenceValue;
        commandQueue->Signal(s_fence.Get(), s_fenceValue);
        if (s_fence->GetCompletedValue() < s_fenceValue) {
            s_fence->SetEventOnCompletion(s_fenceValue, s_fenceEvent);
            WaitForSingleObject(s_fenceEvent, INFINITE);
        }
    }

    return defaultBuffer;
}

void Model::InitializeMaterialAndSkinning(ID3D12Device* device) {
    materialResource_ = CreateBufferResource(device, sizeof(Material));
    if (materialResource_) {
        Material* materialData = nullptr;
        materialResource_->Map(0, nullptr, reinterpret_cast<void**>(&materialData));
        if (materialData) {
            materialData->color = { 1.0f, 1.0f, 1.0f, 1.0f };
            materialData->enableLighting = 1;
            materialData->uvTransform = MakeIdentity4x4();
            materialData->shininess = 10.0f;
            materialData->environmentCoefficient = environmentCoefficient;
        }
        materialResource_->Unmap(0, nullptr);
    }

    // スキニング定数バッファの生成 (ボーンを持つモデルのみ生成してリソース節約)
    if (!bones_.empty() || animation_) {
        skinningResource_ = CreateBufferResource(device, sizeof(SkinningPalette));
        if (skinningResource_) {
            skinningResource_->Map(0, nullptr, reinterpret_cast<void**>(&skinningData_));
            if (skinningData_) {
                for (int i = 0; i < kMaxBones; i++)
                    skinningData_->boneMatrices[i] = MakeIdentity4x4();
            }
        }
    }

    transform = { {1.0f, 1.0f, 1.0f}, {0.0f, 3.1415f, 0.0f}, {0.0f, 0.0f, 0.0f} };
}

void Model::Initialize(const ModelData& modelData, ID3D12Device* device) {
    vertexCount_ = static_cast<uint32_t>(modelData.vertices.size());
    indexCount_ = static_cast<uint32_t>(modelData.indices.size());

    if (vertexCount_ == 0) {
        return;
    }

    Microsoft::WRL::ComPtr<ID3D12Resource> vertexUploadBuffer;
    vertexResource_ = CreateDefaultBuffer(device, modelData.vertices.data(), sizeof(VertexData) * vertexCount_, vertexUploadBuffer);
    if (!vertexResource_) {
        return;
    }
    vertexBufferView_.BufferLocation = vertexResource_->GetGPUVirtualAddress();
    vertexBufferView_.SizeInBytes = static_cast<UINT>(sizeof(VertexData) * vertexCount_);
    vertexBufferView_.StrideInBytes = sizeof(VertexData);

    Microsoft::WRL::ComPtr<ID3D12Resource> indexUploadBuffer;
    if (indexCount_ > 0) {
        indexResource_ = CreateDefaultBuffer(device, modelData.indices.data(), sizeof(uint32_t) * indexCount_, indexUploadBuffer);
        if (indexResource_) {
            indexBufferView_.BufferLocation = indexResource_->GetGPUVirtualAddress();
            indexBufferView_.SizeInBytes = static_cast<UINT>(sizeof(uint32_t) * indexCount_);
            indexBufferView_.Format = DXGI_FORMAT_R32_UINT;
        }
    }

    InitializeMaterialAndSkinning(device);
}

void Model::UpdateAnimation(float deltaTime) {
    if (!animation_) return;

    animationTime_ += deltaTime;
    animationTime_ = std::fmod(animationTime_, animation_->duration);

    // ノードのローカル行列を更新
    auto applyAnim = [&](Node& node, const auto& self) -> void {
        for (const auto& channel : animation_->channels) {
            if (channel.nodeName == node.name) {
                Vector3 translate = channel.positionKeys.empty() ? Vector3{0,0,0} : channel.positionKeys[0].value;
                Quaternion rotate = channel.rotationKeys.empty() ? Quaternion{0,0,0,1} : channel.rotationKeys[0].value;
                Vector3 scale = channel.scaleKeys.empty() ? Vector3{1,1,1} : channel.scaleKeys[0].value;

                // Position
                if (channel.positionKeys.size() > 1) {
                    for (size_t i = 0; i < channel.positionKeys.size() - 1; ++i) {
                        if (animationTime_ < channel.positionKeys[i + 1].time) {
                            float t = (animationTime_ - channel.positionKeys[i].time) / (channel.positionKeys[i + 1].time - channel.positionKeys[i].time);
                            translate = Lerp(channel.positionKeys[i].value, channel.positionKeys[i + 1].value, t);
                            break;
                        }
                    }
                    if (animationTime_ >= channel.positionKeys.back().time) translate = channel.positionKeys.back().value;
                }
                
                // Rotation
                if (channel.rotationKeys.size() > 1) {
                    for (size_t i = 0; i < channel.rotationKeys.size() - 1; ++i) {
                        if (animationTime_ < channel.rotationKeys[i + 1].time) {
                            float t = (animationTime_ - channel.rotationKeys[i].time) / (channel.rotationKeys[i + 1].time - channel.rotationKeys[i].time);
                            rotate = Slerp(channel.rotationKeys[i].value, channel.rotationKeys[i + 1].value, t);
                            break;
                        }
                    }
                    if (animationTime_ >= channel.rotationKeys.back().time) rotate = channel.rotationKeys.back().value;
                }

                // Scale
                if (channel.scaleKeys.size() > 1) {
                    for (size_t i = 0; i < channel.scaleKeys.size() - 1; ++i) {
                        if (animationTime_ < channel.scaleKeys[i + 1].time) {
                            float t = (animationTime_ - channel.scaleKeys[i].time) / (channel.scaleKeys[i + 1].time - channel.scaleKeys[i].time);
                            scale = Lerp(channel.scaleKeys[i].value, channel.scaleKeys[i + 1].value, t);
                            break;
                        }
                    }
                    if (animationTime_ >= channel.scaleKeys.back().time) scale = channel.scaleKeys.back().value;
                }

                Matrix4x4 scaleMatrix = Matrix4x4MakeScaleMatrix(scale);
                Matrix4x4 rotateMatrix = MakeRotateMatrixFromQuaternion(rotate);
                Matrix4x4 translateMatrix = MakeTranslateMatrix(translate);
                node.localMatrix = Multiply(Multiply(scaleMatrix, rotateMatrix), translateMatrix);
                
                break;
            }
        }
        for (auto& child : node.children) self(child, self);
    };
    applyAnim(rootNode, applyAnim);

    ComputeSkinningMatrices(rootNode, MakeIdentity4x4());
}

void Model::ComputeSkinningMatrices(Node& node, const Matrix4x4& parentMatrix) {
    Matrix4x4 globalMatrix = Multiply(node.localMatrix, parentMatrix);

    if (skinningData_ && bones_.count(node.name)) {
        const auto& bone = bones_[node.name];
        skinningData_->boneMatrices[bone.index] = Multiply(bone.offsetMatrix, globalMatrix);
    }

    for (auto& child : node.children) {
        ComputeSkinningMatrices(child, globalMatrix);
    }
}

void Model::Update() {
}

void Model::SetEnvironmentCoefficient(float coefficient) {
    environmentCoefficient = coefficient;
    if (materialResource_) {
        Material* materialData = nullptr;
        materialResource_->Map(0, nullptr, reinterpret_cast<void**>(&materialData));
        materialData->environmentCoefficient = environmentCoefficient;
        materialResource_->Unmap(0, nullptr);
    }
}

void Model::SetColor(const Vector4& color) {
    if (materialResource_) {
        Material* materialData = nullptr;
        materialResource_->Map(0, nullptr, reinterpret_cast<void**>(&materialData));
        materialData->color = color;
        materialResource_->Unmap(0, nullptr);
    }
}


void Model::Draw(ID3D12GraphicsCommandList* commandList) {
    D3D12_VERTEX_BUFFER_VIEW* vbView = nullptr;
    D3D12_INDEX_BUFFER_VIEW* ibView = nullptr;
    UINT indexCount = 0;

    if (commonData_) {
        vbView = &commonData_->vertexBufferView;
        ibView = &commonData_->indexBufferView;
        indexCount = (UINT)commonData_->indices.size();
    } else {
        vbView = &vertexBufferView_;
        ibView = &indexBufferView_;
        indexCount = indexCount_;
    }

    commandList->IASetVertexBuffers(0, 1, vbView);
    commandList->IASetIndexBuffer(ibView);
    commandList->SetGraphicsRootConstantBufferView(0, materialResource_->GetGPUVirtualAddress());
    commandList->DrawIndexedInstanced(indexCount, 1, 0, 0, 0);
}

void Model::Draw(
    ID3D12GraphicsCommandList* commandList,
    UINT instanceCount,
    D3D12_GPU_DESCRIPTOR_HANDLE textureSrvHandle,
    D3D12_GPU_DESCRIPTOR_HANDLE instancingSrvHandle) {

    D3D12_VERTEX_BUFFER_VIEW* vbView = nullptr;
    D3D12_INDEX_BUFFER_VIEW* ibView = nullptr;
    UINT indexCount = 0;

    if (commonData_) {
        vbView = &commonData_->vertexBufferView;
        ibView = &commonData_->indexBufferView;
        indexCount = (UINT)commonData_->indices.size();
    } else {
        vbView = &vertexBufferView_;
        ibView = &indexBufferView_;
        indexCount = indexCount_;
    }

    commandList->IASetVertexBuffers(0, 1, vbView);
    commandList->IASetIndexBuffer(ibView);
    commandList->SetGraphicsRootConstantBufferView(0, materialResource_->GetGPUVirtualAddress());
    commandList->SetGraphicsRootDescriptorTable(2, textureSrvHandle);
    commandList->SetGraphicsRootDescriptorTable(1, instancingSrvHandle);
    commandList->DrawIndexedInstanced(indexCount, instanceCount, 0, 0, 0);
}

void Model::DrawModel(
    ID3D12GraphicsCommandList* commandList,
    D3D12_GPU_DESCRIPTOR_HANDLE textureSrvHandle,
    D3D12_GPU_DESCRIPTOR_HANDLE environmentSrvHandle) {
    D3D12_VERTEX_BUFFER_VIEW* vbView = nullptr;
    D3D12_INDEX_BUFFER_VIEW* ibView = nullptr;
    UINT indexCount = 0;

    if (commonData_) {
        vbView = &commonData_->vertexBufferView;
        ibView = &commonData_->indexBufferView;
        indexCount = (UINT)commonData_->indices.size();
    } else {
        vbView = &vertexBufferView_;
        ibView = &indexBufferView_;
        indexCount = indexCount_;
    }

    commandList->IASetVertexBuffers(0, 1, vbView);
    commandList->IASetIndexBuffer(ibView);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->SetGraphicsRootConstantBufferView(0, materialResource_->GetGPUVirtualAddress());
    commandList->SetGraphicsRootDescriptorTable(2, textureSrvHandle);
    commandList->SetGraphicsRootDescriptorTable(3, environmentSrvHandle);
    
    // スキニング用バッファをセット
    if (skinningResource_) {
        commandList->SetGraphicsRootConstantBufferView(6, skinningResource_->GetGPUVirtualAddress());
    }

    commandList->DrawIndexedInstanced(indexCount, 1, 0, 0, 0);
}

void Model::PrepareDraw(
    ID3D12GraphicsCommandList* commandList,
    D3D12_GPU_DESCRIPTOR_HANDLE textureSrvHandle,
    D3D12_GPU_DESCRIPTOR_HANDLE environmentSrvHandle) {
    D3D12_VERTEX_BUFFER_VIEW* vbView = nullptr;
    D3D12_INDEX_BUFFER_VIEW* ibView = nullptr;

    if (commonData_) {
        vbView = &commonData_->vertexBufferView;
        ibView = &commonData_->indexBufferView;
    } else {
        vbView = &vertexBufferView_;
        ibView = &indexBufferView_;
    }

    commandList->IASetVertexBuffers(0, 1, vbView);
    commandList->IASetIndexBuffer(ibView);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->SetGraphicsRootConstantBufferView(0, materialResource_->GetGPUVirtualAddress());
    commandList->SetGraphicsRootDescriptorTable(2, textureSrvHandle);
    commandList->SetGraphicsRootDescriptorTable(3, environmentSrvHandle);

    if (skinningResource_) {
        commandList->SetGraphicsRootConstantBufferView(6, skinningResource_->GetGPUVirtualAddress());
    }
}

void Model::DrawPrepared(ID3D12GraphicsCommandList* commandList) {
    UINT indexCount = commonData_ ? (UINT)commonData_->indices.size() : indexCount_;
    commandList->DrawIndexedInstanced(indexCount, 1, 0, 0, 0);
}

void Model::DrawSkinningModel(
    ID3D12GraphicsCommandList* commandList,
    const AdvAnim::SkinCluster& skinCluster,
    D3D12_GPU_DESCRIPTOR_HANDLE textureSrvHandle,
    D3D12_GPU_DESCRIPTOR_HANDLE environmentSrvHandle,
    D3D12_GPU_VIRTUAL_ADDRESS transformationMatrixAddress,
    D3D12_GPU_VIRTUAL_ADDRESS directionalLightAddress,
    D3D12_GPU_VIRTUAL_ADDRESS cameraAddress) {

    // --- CSによるスキニング実行 ---
    SrvManager* srvManager = SrvManager::GetInstance();
    //srvManager->PreDraw(); // すでにセットされているはず

    // ルートシグネチャとPSOのセット（Compute用）
    GraphicsPipeline* pipeline = GraphicsPipeline::GetInstance();
    commandList->SetComputeRootSignature(pipeline->GetComputeRootSignature());
    commandList->SetPipelineState(pipeline->GetSkinningComputePipelineState());

    // [0] t0, t1, t2 (Descriptor Table)
    srvManager->SetComputeRootDescriptorTable(0, skinCluster.paletteSrvIndex);
    
    // [1] u0 (OutputVertices)
    srvManager->SetComputeRootDescriptorTable(1, skinCluster.outputVertexUavIndex);

    // [2] b0 (SkinningInformation)
    commandList->SetComputeRootConstantBufferView(2, skinCluster.infoResource->GetGPUVirtualAddress());

    // Dispatch
    uint32_t vCount = commonData_ ? (uint32_t)commonData_->vertices.size() : vertexCount_;
    commandList->Dispatch(UINT(vCount + 1023) / 1024, 1, 1);

    // バリア (UAV -> Vertex Buffer)
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = skinCluster.outputVertexResource.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &barrier);

    // --- 描画処理 ---
    const D3D12_INDEX_BUFFER_VIEW* ibView = nullptr;
    UINT indexCount = 0;

    if (commonData_) {
        ibView = &commonData_->indexBufferView;
        indexCount = (UINT)commonData_->indices.size();
    } else {
        ibView = &indexBufferView_;
        indexCount = indexCount_;
    }

    // ★ここでグラフィックス用のパイプラインとルートシグネチャを再セットする
    commandList->SetGraphicsRootSignature(pipeline->GetSkinningRootSignature());
    commandList->SetPipelineState(pipeline->GetSkinningPipelineState());

    commandList->IASetVertexBuffers(0, 1, &skinCluster.outputVertexBufferView);
    commandList->IASetIndexBuffer(ibView);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // 各種定数バッファとテクスチャの再セット（ルートシグネチャ変更によりクリアされるため）
    commandList->SetGraphicsRootConstantBufferView(0, materialResource_->GetGPUVirtualAddress());
    commandList->SetGraphicsRootConstantBufferView(1, transformationMatrixAddress);
    // [2] MatrixPalette (b0, VS) は CSで計算済みのため不要（または空のデスクリプタをセット）
    // [3] Texture
    commandList->SetGraphicsRootDescriptorTable(3, textureSrvHandle);
    // [4] Environment Map
    commandList->SetGraphicsRootDescriptorTable(4, environmentSrvHandle);
    // [5] Directional Light
    commandList->SetGraphicsRootConstantBufferView(5, directionalLightAddress);
    // [6] Camera
    commandList->SetGraphicsRootConstantBufferView(6, cameraAddress);

    commandList->DrawIndexedInstanced(indexCount, 1, 0, 0, 0);

    // バリアを戻す (Vertex Buffer -> UAV) 次回のDispatchのため
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    commandList->ResourceBarrier(1, &barrier);
}

void Model::SetLightingEnabled(bool enabled) {
    if (materialResource_) {
        Material* materialData = nullptr;
        materialResource_->Map(0, nullptr, reinterpret_cast<void**>(&materialData));
        materialData->enableLighting = enabled ? 1 : 0;
        materialResource_->Unmap(0, nullptr);
    }
}
