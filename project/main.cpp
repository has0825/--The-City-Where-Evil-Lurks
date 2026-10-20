#include "Game.h"
#include <Windows.h>
#include <dxgidebug.h>
#include <memory> 
#include <filesystem>
#include <vector>

// D3D12のリソースリークをチェックするための構造体
struct D3DResourceLeakChecker {
    ~D3DResourceLeakChecker() {
        Microsoft::WRL::ComPtr<IDXGIDebug1> debug;
        if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&debug)))) {
            debug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_ALL);
        }
    }
};

int WINAPI WinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPSTR, _In_ int) {
    try {
        // 実行ファイルの場所を取得
        wchar_t exePath[MAX_PATH] = { 0 };
        GetModuleFileNameW(NULL, exePath, MAX_PATH);
        std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();

        // Resources ディレクトリが存在する場所を探索してカレントディレクトリを設定
        const std::vector<std::filesystem::path> searchCandidates = {
            exeDir,                                         // exeと同じフォルダ（配布時・単体実行時）
            std::filesystem::current_path(),                // 現在のカレントディレクトリ
            exeDir.parent_path().parent_path() / "project", // generated/outputs/Development から見た project
            exeDir.parent_path() / "project",               // 親の project
            exeDir / "project"                              // サブディレクトリの project
        };

        for (const auto& candidate : searchCandidates) {
            std::error_code ec;
            if (std::filesystem::exists(candidate / "Resources", ec)) {
                std::filesystem::current_path(candidate, ec);
                break;
            }
        }

        D3DResourceLeakChecker leakChecker;

        std::unique_ptr<Game> game = std::make_unique<Game>();

        // 実行
        game->Run();
    }
    catch (const std::exception& e) {
        MessageBoxA(NULL, e.what(), "Error", MB_OK | MB_ICONERROR);
        return -1;
    }
    catch (...) {
        MessageBoxA(NULL, "Unknown error occurred.", "Error", MB_OK | MB_ICONERROR);
        return -1;
    }

    return 0;
}