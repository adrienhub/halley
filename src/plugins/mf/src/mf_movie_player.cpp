#include "mf_movie_player.h"
#include "halley/resources/resource_data.h"
#include "halley/core/api/audio_api.h"
#include "halley/core/api/video_api.h"
#include "resource_data_byte_stream.h"
#include "halley/core/graphics/texture_descriptor.h"
#include "halley/concurrency/concurrent.h"
#include "halley/core/resources/resources.h"
#include "halley/core/graphics/material/material_definition.h"

using namespace Halley;

/*
#include <D3D9.h>
#pragma comment(lib, "Mfuuid.lib")
#pragma comment(lib, "Mf.lib")
#pragma comment(lib, "strmiids.lib")
*/

MFMoviePlayer::MFMoviePlayer(VideoAPI& video, AudioAPI& audio, std::shared_ptr<ResourceDataStream> data)
	: video(video)
	, audio(audio)
	, data(std::move(data))
{
	init();
}

MFMoviePlayer::~MFMoviePlayer() noexcept
{
	deInit();
}

void MFMoviePlayer::play()
{
	if (state == MoviePlayerState::Paused) {
		state = MoviePlayerState::Playing;

		for (auto& s: streams) {
			s.playing = true;
		}
	}
}

void MFMoviePlayer::pause()
{
	if (state == MoviePlayerState::Playing) {
		state = MoviePlayerState::Paused;

		for (auto& s: streams) {
			s.playing = false;
		}
	}
}

void MFMoviePlayer::reset()
{
	state = MoviePlayerState::Paused;
	time = 0;

	PROPVARIANT pos;
	pos.vt = VT_I8;
	pos.hVal.QuadPart = 0;
	reader->SetCurrentPosition(GUID_NULL, pos);
}

void MFMoviePlayer::update(Time t)
{
	if (state == MoviePlayerState::Playing) {
		time += t;

		MoviePlayerStream* videoStream = nullptr;
		for (auto& s: streams) {
			if (s.type == MoviePlayerStreamType::Video) {
				videoStream = &s;
				break;
			}
		}

		if (videoStream) {
			if (pendingFrames.size() < 5 && !videoStream->eof) {
				const DWORD controlFlags = 0;
				DWORD streamIndex;
				DWORD streamFlags;
				LONGLONG timestamp;
				IMFSample* sample;
				auto hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, controlFlags, &streamIndex, &streamFlags, &timestamp, &sample);
				onReadSample(hr, streamIndex, streamFlags, timestamp, sample);
			}

			if (!pendingFrames.empty()) {
				auto& next = pendingFrames.front();
				if (!currentTexture || time >= next.time) {
					currentTexture = next.texture;
					pendingFrames.pop_front();
				}
			}

			if (pendingFrames.empty() && videoStream->eof) {
				state = MoviePlayerState::Finished;
			}
		}
	}
}

Sprite MFMoviePlayer::getSprite(Resources& resources)
{
	auto matDef = resources.get<MaterialDefinition>("Halley/Sprite");
	return Sprite().setImage(currentTexture, matDef).setTexRect(Rect4f(0, 0, 1, 1)).setSize(Vector2f(videoSize));
}

MoviePlayerState MFMoviePlayer::getState() const
{
	return state;
}

Vector2i MFMoviePlayer::getSize() const
{
	return videoSize;
}

void MFMoviePlayer::init()
{
	inputByteStream = new ResourceDataByteStream(data);
	inputByteStream->AddRef();

	IMFAttributes* attributes = nullptr;
	HRESULT hr = MFCreateAttributes(&attributes, 1);
	if (!SUCCEEDED(hr)) {
		throw Exception("Unable to create attributes");
	}

	/*
	constexpr bool useAsync = false;
	if (useAsync) {
		sampleReceiver = new MoviePlayerSampleReceiver(*this);
		sampleReceiver->AddRef();
		attributes->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, sampleReceiver);
	}
	*/

	// DX11 acceleration
	auto dx11Device = static_cast<IUnknown*>(video.getImplementationPointer("ID3D11Device"));
	IMFDXGIDeviceManager* deviceManager = nullptr;
	if (dx11Device) {
		UINT resetToken;
		hr = MFCreateDXGIDeviceManager(&resetToken, &deviceManager);
		if (!SUCCEEDED(hr)) {
			throw Exception("Unable to create DXGI Device Manager");
		}
		hr = deviceManager->ResetDevice(dx11Device, resetToken);
		if (!SUCCEEDED(hr)) {
			throw Exception("Unable to reset DXGI Device Manager with device");
		}
		attributes->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, deviceManager);
	}

	hr = MFCreateSourceReaderFromByteStream(inputByteStream, attributes, &reader);
	if (!SUCCEEDED(hr)) {
		throw Exception("Unable to create source reader");
	}

	// Release these temporaries
	if (attributes) {
		attributes->Release();
	}
	if (deviceManager) {
		deviceManager->Release();
	}

	reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, false);
	reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, true);
	reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, true);

	// Setup the right decoding formats
	bool hasMoreStreams = true;
	for (int streamIndex = 0; hasMoreStreams; ++streamIndex) {
		streams.emplace_back();
		auto& curStream = streams.back();

		for (int mediaTypeIndex = 0; ; ++mediaTypeIndex) {
			IMFMediaType *nativeType = nullptr;
			hr = reader->GetNativeMediaType(streamIndex, mediaTypeIndex, &nativeType);

			if (hr == MF_E_INVALIDSTREAMNUMBER) {
				hasMoreStreams = false;
				break;
			} else if (hr == MF_E_NO_MORE_TYPES) {
				break;
			} else if (SUCCEEDED(hr)) {
				GUID majorType;
				GUID subType;
				IMFMediaType *targetType = nullptr;

				try {
					hr = nativeType->GetGUID(MF_MT_MAJOR_TYPE, &majorType);
					if (!SUCCEEDED(hr)) {
						throw Exception("Unable to read major type");
					}

					MFCreateMediaType(&targetType);
					hr = targetType->SetGUID(MF_MT_MAJOR_TYPE, majorType);
					if (!SUCCEEDED(hr)) {
						throw Exception("Unable to write major type");
					}

					if (majorType == MFMediaType_Video) {
						UINT64 frameSize;
						nativeType->GetUINT64(MF_MT_FRAME_SIZE, &frameSize);
						videoSize = Vector2i(int(frameSize >> 32), int(frameSize & 0xFFFFFFFFull));
						UINT32 strideRaw;
						nativeType->GetUINT32(MF_MT_DEFAULT_STRIDE, &strideRaw);
						INT32 stride = static_cast<INT32>(strideRaw);
						UINT64 aspectRatioRaw;
						nativeType->GetUINT64(MF_MT_PIXEL_ASPECT_RATIO, &aspectRatioRaw);
						float par = float(aspectRatioRaw >> 32) / float(aspectRatioRaw & 0xFFFFFFFFull);
						std::cout << "Video is " << videoSize.x << " x " << videoSize.y << ", stride: " << stride << ", PAR: " << par << std::endl;

						curStream.type = MoviePlayerStreamType::Video;
						subType = MFVideoFormat_NV12; // NV12 is the only format supported by DX accelerated decoding
					} else if (majorType == MFMediaType_Audio) {
						UINT32 sampleRate;
						UINT32 numChannels;
						nativeType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sampleRate);
						nativeType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &numChannels);
						std::cout << "Audio has " << numChannels << " channels at " << sampleRate << " Hz." << std::endl;

						curStream.type = MoviePlayerStreamType::Audio;
						subType = MFAudioFormat_PCM;
					}

					hr = targetType->SetGUID(MF_MT_SUBTYPE, subType);
					if (!SUCCEEDED(hr)) {
						throw Exception("Unable to write subtype");
					}

					hr = reader->SetCurrentMediaType(streamIndex, nullptr, targetType);
					if (!SUCCEEDED(hr)) {
						throw Exception("Unable to set current media type");
					}
				} catch (...) {
					if (targetType) {
						targetType->Release();
					}
					nativeType->Release();
					throw;
				}

				targetType->Release();
				nativeType->Release();
			} else {
				throw Exception("Error reading stream info");
			}
		}
	}

	reset();
}

void MFMoviePlayer::deInit()
{
	if (sampleReceiver) {
		sampleReceiver->Release();
		sampleReceiver = nullptr;
	}
	if (reader) {
		reader->Release();
		reader = nullptr;
	}
	if (inputByteStream) {
		inputByteStream->Release();
		inputByteStream = nullptr;
	}
}

HRESULT MFMoviePlayer::onReadSample(HRESULT hr, DWORD streamIndex, DWORD streamFlags, LONGLONG timestamp, IMFSample* sample)
{
	auto& curStream = streams.at(streamIndex);
	if (streamFlags & MF_SOURCE_READERF_ENDOFSTREAM) {
		curStream.eof = true;
	}

	if (sample) {
		Time sampleTime = Time(timestamp) / 10000000.0;

		DWORD bufferCount;
		sample->GetBufferCount(&bufferCount);
		for (int i = 0; i < int(bufferCount); ++i) {
			IMFMediaBuffer* buffer;
			sample->GetBufferByIndex(i, &buffer);

			if (curStream.type == MoviePlayerStreamType::Video) {
				/*
				IDirect3DSurface9 *surface = nullptr;
				auto hr = MFGetService(buffer, MR_BUFFER_SERVICE, __uuidof(IDirect3DSurface9), reinterpret_cast<void**>(&surface));
				if (SUCCEEDED(hr)) {
					std::cout << Release();
				}"Got DX9 surface!\n";
				surface->Release();
				*/
				
				IMF2DBuffer* buffer2d;
				hr = buffer->QueryInterface(__uuidof(IMF2DBuffer), reinterpret_cast<void**>(&buffer2d));
				if (!SUCCEEDED(hr)) {
					throw Exception("Unable to read video frame");
				}
				
				BYTE* src;
				LONG pitch;
				buffer2d->Lock2D(&src, &pitch);
				readVideoSample(sampleTime, gsl::as_bytes(gsl::span<const BYTE>(src, pitch * videoSize.y)), pitch);
				buffer2d->Unlock2D();
				buffer2d->Release();
			}

			if (curStream.type == MoviePlayerStreamType::Audio) {
				DWORD length;
				buffer->GetCurrentLength(&length);
				BYTE* data;
				DWORD maxLen;
				DWORD curLen;
				buffer->Lock(&data, &maxLen, &curLen);
				readAudioSample(sampleTime, gsl::as_bytes(gsl::span<const BYTE>(data, curLen)));
				buffer->Unlock();
			}

			buffer->Release();
		}

		sample->Release();
	}

	return S_OK;
}

void MFMoviePlayer::readVideoSample(Time time, gsl::span<const gsl::byte> data, int stride)
{
	Vector2i texSize = videoSize;

	Bytes myData(data.size());
	memcpy(myData.data(), data.data(), data.size());

	std::shared_ptr<Texture> tex = video.createTexture(videoSize);
	tex->startLoading();
	pendingFrames.push_back({tex, time});

	Concurrent::execute(Executors::getVideoAux(), [tex, texSize, stride, data = std::move(myData)] () mutable
	{
		TextureDescriptor descriptor;
		descriptor.format = TextureFormat::Indexed;
		descriptor.pixelFormat = PixelDataFormat::Image;
		descriptor.size = texSize;
		descriptor.pixelData = TextureDescriptorImageData(std::move(data), stride);
		
		tex->load(std::move(descriptor));
	});
}

void MFMoviePlayer::readAudioSample(Time time, gsl::span<const gsl::byte> data)
{
	//std::cout << "Audio at " << time << " (" << data.size_bytes() << " bytes).\n";
}

/*
MoviePlayerSampleReceiver::MoviePlayerSampleReceiver(MFMoviePlayer& player)
	: player(player)
{
}


HRESULT MoviePlayerSampleReceiver::OnReadSample(HRESULT hr, DWORD streamIndex, DWORD streamFlags, LONGLONG timestamp, IMFSample* sample)
{
	return player.onReadSample(hr, streamIndex, streamFlags, timestamp, sample);
}

HRESULT MoviePlayerSampleReceiver::QueryInterface(const IID& riid, void** ppvObject)
{
	if (!ppvObject) {
		return E_INVALIDARG;
	}
	*ppvObject = nullptr;

	if (riid == __uuidof(IMFSourceReaderCallback)) {
		AddRef();
		*ppvObject = static_cast<IMFSourceReaderCallback*>(this);
		return NOERROR;
	} else if (riid == __uuidof(IMFSourceReaderCallback2)) {
		AddRef();
		*ppvObject = static_cast<IMFSourceReaderCallback2*>(this);
		return NOERROR;
	} else {
		return E_NOINTERFACE;
	}
}

ULONG MoviePlayerSampleReceiver::AddRef()
{
	return InterlockedIncrement(&refCount);
}

ULONG MoviePlayerSampleReceiver::Release()
{
	ULONG uCount = InterlockedDecrement(&refCount);
    if (uCount == 0) {
		delete this;
    }
    return uCount;
}

HRESULT MoviePlayerSampleReceiver::OnFlush(DWORD dwStreamIndex)
{
	return S_OK;
}

HRESULT MoviePlayerSampleReceiver::OnEvent(DWORD dwStreamIndex, IMFMediaEvent* pEvent)
{
	return S_OK;
}

HRESULT MoviePlayerSampleReceiver::OnTransformChange()
{
	return S_OK;
}

HRESULT MoviePlayerSampleReceiver::OnStreamError(DWORD dwStreamIndex, HRESULT hrStatus)
{
	return S_OK;
}
*/
