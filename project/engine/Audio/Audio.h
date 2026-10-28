#pragma once
#include <Windows.h>
#include <wrl.h>
#include <string>
#include <vector>
#include <xaudio2.h>

// 音声データを保持する構造体
// 読み込みと再生を分けるために使用します
struct SoundData {
	WAVEFORMATEX wfex;
	std::vector<BYTE> pBuffer;
	unsigned int bufferSize;
};

class Audio {
public:
	// 初期化
	void Initialize();
	// 終了処理
	void Finalize();

	// 音声ファイルの読み込み (.wav, .mp3, .aac等 対応)
	SoundData LoadAudio(const std::string& filename);

	// 音声再生
	// soundData: LoadAudioで読み込んだデータ
	// loop: ループ再生するかどうか
	// volume: 音量 (0.0f ~ 1.0f)
	void PlayWave(const SoundData& soundData, bool loop = false, float volume = 1.0f);

	// ミュート制御
	void SetMuted(bool isMuted) { isMuted_ = isMuted; }
	bool IsMuted() const { return isMuted_; }

private:
	Microsoft::WRL::ComPtr<IXAudio2> xAudio2_;
	IXAudio2MasteringVoice* masterVoice_ = nullptr;
	std::vector<IXAudio2SourceVoice*> activeVoices_;
	bool isMuted_ = true; // デフォルトでミュート(消音)状態
};
