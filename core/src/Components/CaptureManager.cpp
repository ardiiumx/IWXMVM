#include "StdInclude.hpp"
#include "CaptureManager.hpp"

#include "Mod.hpp"
#include "Configuration/PreferencesConfiguration.hpp"
#include "Components/Rewinding.hpp"
#include "Components/Playback.hpp"
#include "Graphics/Graphics.hpp"
#include "Utilities/PathUtils.hpp"
#include "D3D9.hpp"
#include "Events.hpp"

namespace IWXMVM::Components
{
    std::string_view CaptureManager::GetOutputFormatLabel(OutputFormat outputFormat)
    {
        switch (outputFormat)
        {
            case OutputFormat::Video:
                return "Video";
            case OutputFormat::CameraData:
                return "Camera Data";
            case OutputFormat::ImageSequence:
                return "Image Sequence";
            default:
                return "Unknown Output Format";
        }
    }

    std::string_view CaptureManager::GetVideoCodecLabel(VideoCodec codec)
    {
        switch (codec)
        {
            case VideoCodec::Prores4444XQ:
                return "Prores 4444 XQ";
            case VideoCodec::Prores4444:
                return "Prores 4444";
            case VideoCodec::Prores422HQ:
                return "Prores 422 HQ";
            case VideoCodec::Prores422:
                return "Prores 422";
            case VideoCodec::Prores422LT:
                return "Prores 422 LT";
            default:
                return "Unknown Video Codec";
        }
    }

    void CaptureManager::Initialize()
    {
        // Set r_smp_backend to 0. 
        // This should ensure that there are no separate threads for game logic and rendering
        // which frees us from having to do synchronizations for our recording code.
        auto r_smp_backend = Mod::GetGameInterface()->GetDvar("r_smp_backend");
        if (!r_smp_backend.has_value())
        {
            LOG_ERROR("Could not set r_smp_backend; dvar not found");
        }
        else
        {
            r_smp_backend.value().value->int32 = false;
        }

        IDirect3DDevice9* device = D3D9::GetDevice();

        if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)))
        {
            LOG_ERROR("Failed to capture backbuffer. Capture resolution not found.");
            return;
        }

        D3DSURFACE_DESC bbDesc = {};
        if (FAILED(backBuffer->GetDesc(&bbDesc)))
        {
            LOG_ERROR("Failed to get backbuffer description. Capture resolution not found.");
            return;
        }

        backBuffer->Release();
        backBuffer = nullptr;

        const Resolution gameResolution = {
            static_cast<std::int32_t>(bbDesc.Width),
            static_cast<std::int32_t>(bbDesc.Height),
        };
        for (std::ptrdiff_t i = 0; i < std::ssize(supportedResolutions); i++)
        {
            supportedResolutions[i] = {
                gameResolution.width / (i + 1),
                gameResolution.height / (i + 1),
            };
        }

        captureSettings = {
            0,
            0,
            OutputFormat::Video, 
            VideoCodec::Prores4444,
            gameResolution,
            250
        };

        auto& outputDirectory = PreferencesConfiguration::Get().captureOutputDirectory;
        if (outputDirectory.empty())
        {
            outputDirectory = std::filesystem::path(PathUtils::GetCurrentGameDirectory()) / "IWXMVM" / "recordings";
        }

        Events::RegisterListener(EventType::OnDemoBoundsDetermined, [&]() {
            if (captureSettings.startTick == 0 || captureSettings.endTick == 0)
            {
                auto endTick = Mod::GetGameInterface()->GetDemoInfo().endTick;
                captureSettings.startTick = static_cast<int32_t>(endTick * 0.1);
                captureSettings.endTick = static_cast<int32_t>(endTick * 0.9);
            }
        });

        Events::RegisterListener(EventType::OnFrame, [&]() { OnRenderFrame(); });
    }

    void CaptureManager::CaptureFrame()
    {
        framePrepared = false;

        FILE* outputPipe = pipe;
        if (MultiPassEnabled())
        {
            const auto passIndex = static_cast<std::size_t>(capturedFrameCount) % captureSettings.passes.size();
            GFX::GraphicsManager::Get().DrawShaderForPassIndex(passIndex);

            outputPipe = captureSettings.passes[passIndex].pipe;
        }

        IDirect3DDevice9* device = D3D9::GetDevice();

        if (FAILED(device->StretchRect(backBuffer, NULL, downsampledRenderTarget, NULL, D3DTEXF_NONE)))
        {
            LOG_ERROR("Failed to copy data from backbuffer to render target");
            StopCapture();
            return;
        }

        if (FAILED(device->GetRenderTargetData(downsampledRenderTarget, tempSurface)))
        {
            LOG_ERROR("Failed copy render target data to surface");
            StopCapture();
            return;
        }

        D3DLOCKED_RECT lockedRect = {};
        if (FAILED(tempSurface->LockRect(&lockedRect, nullptr, 0)))
        {
            LOG_ERROR("Failed to lock surface");
            StopCapture();
            return;
        }

        const auto surfaceByteSize = screenDimensions.width * screenDimensions.height * 4;
        std::fwrite(lockedRect.pBits, surfaceByteSize, 1, outputPipe);

        capturedFrameCount++;

        if (FAILED(tempSurface->UnlockRect()))
        {
            LOG_ERROR("Failed to unlock surface");
            StopCapture();
            return;
        }

        const auto currentTick = Playback::GetTimelineTick();
        if (!Rewinding::IsRewinding() && currentTick > captureSettings.endTick)
        {
            StopCapture();
        }
    }

    void CaptureManager::PrepareFrame()
    {
        if (!isCapturing.load())
        	return;

        if (MultiPassEnabled())
        {
            const auto passIndex = static_cast<std::size_t>(capturedFrameCount) % captureSettings.passes.size();
            const auto& pass = captureSettings.passes[passIndex];

            Rendering::SetVisibleElements(pass.elements);
        }

        framePrepared = true;
    }

    void CaptureManager::OnRenderFrame()
    {
        if (!isCapturing || Rewinding::IsRewinding())
            return;
    }

    int32_t CaptureManager::OnGameFrame()
    {
        if (MultiPassEnabled())
        {
            return capturedFrameCount % captureSettings.passes.size() == 0 ? 1000 / GetCaptureSettings().framerate : 0;
        }
        else
        {
            return 1000 / GetCaptureSettings().framerate;
        }
    }

    void CaptureManager::ToggleCapture()
    {
        if (!isCapturing)
        {
            StartCapture();
        }
        else
        {
            StopCapture();
        }
    }

    std::filesystem::path GetFFmpegPath()
    {
        auto appdataPath = std::filesystem::path(getenv("APPDATA"));
        return appdataPath / "codmvm_launcher" / "ffmpeg.exe";
    }

    std::string GetFFmpegCommand(const Components::CaptureSettings& captureSettings, const std::filesystem::path& outputDirectory, const Resolution screenDimensions, std::size_t passIndex)
    {
        auto path = GetFFmpegPath();
        char shortPathBuf[MAX_PATH];
        GetShortPathName(path.string().c_str(), shortPathBuf, MAX_PATH);
        std::string shortPath = shortPathBuf;
        switch (captureSettings.outputFormat)
        {
            case OutputFormat::ImageSequence:
                return std::format(
                    "{} -f rawvideo -pix_fmt bgra -s {}x{} -r {} -i - -q:v 0 "
                    "-vf scale={}:{} -y \"{}\\output_{}_%06d.tga\" 2>&1",
                    shortPath,
                    screenDimensions.width, screenDimensions.height, captureSettings.framerate,
                    captureSettings.resolution.width, captureSettings.resolution.height, outputDirectory.string(), passIndex);
            case OutputFormat::Video:
            {
                std::int32_t profile = 0;
                const char* pixelFormat = nullptr;
                switch (captureSettings.videoCodec.value())
                {
                    case VideoCodec::Prores4444XQ:
                        profile = 5;
                        pixelFormat = "yuv444p10le";
                        break;
                    case VideoCodec::Prores4444:
                        profile = 4;
                        pixelFormat = "yuv444p10le";
                        break;
                    case VideoCodec::Prores422HQ:
                        profile = 3;
                        pixelFormat = "yuv422p10le";
                        break;
                    case VideoCodec::Prores422:
                        profile = 2;
                        pixelFormat = "yuv422p10le";
                        break;
                    case VideoCodec::Prores422LT:
                        profile = 1;
                        pixelFormat = "yuv422p10le";
                        break;
                    default:
                        profile = 4;
                        pixelFormat = "yuv444p10le";
                        LOG_ERROR("Unsupported video codec. Choosing default ({})",
                                  static_cast<std::int32_t>(VideoCodec::Prores4444));
                        break;
                }

                std::string filename = std::format("Pass {}.mov", passIndex);
                auto i = 0;
                while (std::filesystem::exists(outputDirectory / filename))
                {
                    filename = std::format("Pass {}({}).mov", passIndex, ++i);
                }

                return std::format(
                    "{} -f rawvideo -pix_fmt bgra -s {}x{} -r {} -i - -c:v prores -profile:v {} -q:v 1 "
                    "-pix_fmt {} -vf scale={}:{} -y \"{}\\{}\" 2>&1",
                    shortPath, screenDimensions.width, screenDimensions.height, captureSettings.framerate, profile,
                    pixelFormat, captureSettings.resolution.width, captureSettings.resolution.height, outputDirectory.string(), filename);
            }
            default:
                LOG_ERROR("Output format not supported");
                return "";
        }
    }

    void CaptureManager::StartCapture()
    {
        if (captureSettings.startTick >= captureSettings.endTick)
        {
            LOG_ERROR("Start tick must be less than end tick");
            return;
        }

        // ensure output directory exists
        const auto& outputDirectory = PreferencesConfiguration::Get().captureOutputDirectory;
        if (!std::filesystem::exists(outputDirectory))
        {
            std::filesystem::create_directories(outputDirectory);
        }

        // skip to start tick
        auto currentTick = Playback::GetTimelineTick();
        Playback::SetTickDelta(captureSettings.startTick - currentTick, true);

        capturedFrameCount = 0;

        LOG_INFO("Starting capture at {0} ({1} fps)", captureSettings.resolution.ToString(), captureSettings.framerate);

        IDirect3DDevice9* device = D3D9::GetDevice();

        if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)))
        {
            LOG_ERROR("Failed to capture backbuffer");
            StopCapture();
            return;
        }

        D3DSURFACE_DESC bbDesc = {};
        if (FAILED(backBuffer->GetDesc(&bbDesc)))
        {
            LOG_ERROR("Failed to get backbuffer description");
            StopCapture();
            return;
        }
        if (FAILED(device->CreateOffscreenPlainSurface(bbDesc.Width, bbDesc.Height, bbDesc.Format, D3DPOOL_SYSTEMMEM,
                                                       &tempSurface, nullptr)))
        {
            LOG_ERROR("Failed to create temporary surface");
            StopCapture();
            return;
        }

        if (FAILED(device->CreateRenderTarget(bbDesc.Width, bbDesc.Height, bbDesc.Format, D3DMULTISAMPLE_NONE, 0, FALSE,
            &downsampledRenderTarget, NULL)))
        {
            LOG_ERROR("Failed to create render target");
            StopCapture();
            return;
        }

        screenDimensions.width = static_cast<std::int32_t>(bbDesc.Width);
        screenDimensions.height = static_cast<std::int32_t>(bbDesc.Height);

        if (!std::filesystem::exists(GetFFmpegPath()))
        {
            LOG_ERROR("ffmpeg is not present in the game directory");
            ffmpegNotFound = true;
            StopCapture();
            return;
        }
        ffmpegNotFound = false;

        if (captureSettings.passes.empty())
        {
            std::string ffmpegCommand = GetFFmpegCommand(captureSettings, outputDirectory, screenDimensions, 0);
            LOG_DEBUG("ffmpeg command: {}", ffmpegCommand);
            pipe = _popen(ffmpegCommand.c_str(), "wb");
            if (!pipe)
            {
                LOG_ERROR("ffmpeg pipe open error");
                StopCapture();
                return;
            }
        }
        else
        {
            for (std::size_t i = 0; i < captureSettings.passes.size(); i++)
            {
                auto& pass = captureSettings.passes[i];

                std::string ffmpegCommand = GetFFmpegCommand(captureSettings, outputDirectory, screenDimensions, i);
                LOG_DEBUG("ffmpeg command: {}", ffmpegCommand);
                pass.pipe = _popen(ffmpegCommand.c_str(), "wb");
                if (!pass.pipe)
                {
                    LOG_ERROR("ffmpeg pipe open error");
                    StopCapture();
                    return;
                }
            }
        }

        isCapturing.store(true);
    }

    void CaptureManager::StopCapture()
    {
        LOG_INFO("Stopped capture (wrote {0} frames)", capturedFrameCount);
        isCapturing.store(false);

        Rendering::ResetVisibleElements();
        framePrepared = false;

        if (pipe)
        {
            fflush(pipe);
            fclose(pipe);
            pipe = nullptr;
        }

        for (auto& pass : captureSettings.passes)
        {
            if (pass.pipe)
            {
                fflush(pass.pipe);
                fclose(pass.pipe);
                pass.pipe = nullptr;
            }
        }

        if (tempSurface)
        {
            tempSurface->Release();
            tempSurface = nullptr;
        }

        if (backBuffer)
        {
            backBuffer->Release();
            backBuffer = nullptr;
        }

        if (downsampledRenderTarget)
        {
            downsampledRenderTarget->Release();
            downsampledRenderTarget = nullptr;
        }

        if (depthSurface)
        {
            depthSurface->Release();
            depthSurface = nullptr;
        }

        if (depthShader)
        {
            depthShader->Release();
            depthShader = nullptr;
        }
    }
}