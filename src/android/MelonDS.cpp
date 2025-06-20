#include <oboe/Oboe.h>
#include "MelonDS.h"
#include "FileUtils.h"
#include "OboeCallback.h"
#include "MelonAudioStreamErrorCallback.h"
#include "MicInputOboeCallback.h"
#include "AndroidARCodeFile.h"
#include "OpenGLContext.h"
#include "MelonLog.h"
#include "../NDS.h"
#include "../GPU.h"
#include "../GPU3D.h"
#include "../GBACart.h"
#include "../SPU.h"
#include "../Platform.h"
#include "../AREngine.h"
#include "../FileSavestate.h"
#include "../DSi_I2C.h"
#include "Config.h"
#include "MemorySavestate.h"
#include "FrontendUtil.h"
#include "RewindManager.h"
#include "ROMManager.h"
#include "AndroidCameraHandler.h"
#include "LocalMultiplayer.h"
#include "ScreenshotRenderer.h"
#include "retroachievements/RetroAchievements.h"
#include "retroachievements/RACallback.h"
#include <android/asset_manager.h>
#include <cstring>

#define MIC_BUFFER_SIZE 2048

const char* MELONDS_TAG = "melonDS";

std::shared_ptr<oboe::AudioStream> audioStream;
oboe::AudioStreamErrorCallback *audioStreamErrorCallback;
std::shared_ptr<oboe::AudioStream> micInputStream;
OboeCallback *outputCallback;
MicInputOboeCallback *micInputCallback;
AndroidARCodeFile *arCodeFile;
FrameRenderedCallback* frameRenderedCallback;
OpenGLContext *openGlContext;
ScreenshotRenderer *screenshotRenderer;
bool isRenderConfigurationDirty = false;

namespace MelonDSAndroid
{
    GLuint softwareRenderingTexture;
    int frame = 0;
    int actualMicSource = 0;
    bool isMicInputEnabled = true;
    RetroAchievements::RACallback* retroAchievementsCallback;
    AAssetManager* assetManager;
    AndroidFileHandler* fileHandler;
    AndroidCameraHandler* cameraHandler;
    std::string internalFilesDir;
    int screenshotWidth = 256;
    int screenshotHeight = 192 * 2;
    EmulatorConfiguration currentConfiguration;

    // Variables used to keep the current state so that emulation can be reset
    char* currentRomPath = NULL;
    char* currentSramPath = NULL;
    RomGbaSlotConfig* currentGbaSlotConfig = nullptr;
    RunMode currentRunMode;

    void setupAudioOutputStream(int audioLatency, int volume);
    void cleanupAudioOutputStream();
    void setupMicInputStream();
    void cleanupMicInputStream();
    void resetAudioOutputStream();
    bool setupOpenGlContext(long sharedGlContext);
    void cleanupOpenGlContext();
    void updateCurrentGbaSlotConfig(RomGbaSlotConfig* newConfig);
    void copyString(char** dest, const char* source);

    /**
     * Used to set the emulator's initial configuration, before boot. To update the configuration during runtime, use @updateEmulatorConfiguration.
     *
     * @param emulatorConfiguration The emulator configuration during the next emulator run
     */
    void setConfiguration(EmulatorConfiguration emulatorConfiguration) {
        currentConfiguration = emulatorConfiguration;
        internalFilesDir = emulatorConfiguration.internalFilesDir;
        actualMicSource = emulatorConfiguration.micSource;
        isMicInputEnabled = true;

        Config::BIOS7Path = emulatorConfiguration.dsBios7Path ?: "";
        Config::BIOS9Path = emulatorConfiguration.dsBios9Path ?: "";
        Config::FirmwarePath = emulatorConfiguration.dsFirmwarePath ?: "";

        Config::DSiBIOS7Path = emulatorConfiguration.dsiBios7Path ?: "";
        Config::DSiBIOS9Path = emulatorConfiguration.dsiBios9Path ?: "";
        Config::DSiFirmwarePath = emulatorConfiguration.dsiFirmwarePath ?: "";
        Config::DSiNANDPath = emulatorConfiguration.dsiNandPath ?: "";

        // Internal BIOS and Firmware can only be used for DS
        if (emulatorConfiguration.userInternalFirmwareAndBios) {
            Config::FirmwareUsername = emulatorConfiguration.firmwareConfiguration.username;
            Config::FirmwareUsername =emulatorConfiguration.firmwareConfiguration.username;
            Config::FirmwareMessage = emulatorConfiguration.firmwareConfiguration.message;
            Config::FirmwareLanguage = emulatorConfiguration.firmwareConfiguration.language;
            Config::FirmwareBirthdayMonth = emulatorConfiguration.firmwareConfiguration.birthdayMonth;
            Config::FirmwareBirthdayDay = emulatorConfiguration.firmwareConfiguration.birthdayDay;
            Config::FirmwareFavouriteColour = emulatorConfiguration.firmwareConfiguration.favouriteColour;
            Config::FirmwareMAC = emulatorConfiguration.firmwareConfiguration.macAddress;
            Config::DSBatteryLevelOkay = true;
            Config::ConsoleType = 0;
            Config::ExternalBIOSEnable = false;
        } else {
            Config::ExternalBIOSEnable = true;
            Config::DirectBoot = !emulatorConfiguration.showBootScreen;

            if (emulatorConfiguration.consoleType == 0) {
                Config::DSBatteryLevelOkay = true;
                Config::ConsoleType = 0;
            } else {
                Config::DSiBatteryLevel = DSi_BPTWL::batteryLevel_Full;
                Config::DSiBatteryCharging = true;
                Config::ConsoleType = 1;
            }
        }

#ifdef JIT_ENABLED
        Config::JIT_Enable = emulatorConfiguration.useJit;
#endif

        Config::AudioBitrate = emulatorConfiguration.audioBitrate;
        Config::FirmwareOverrideSettings = false;
        Config::RandomizeMAC = emulatorConfiguration.firmwareConfiguration.randomizeMacAddress;
        Config::SocketBindAnyAddr = true;
        Config::DLDIEnable = false;
        Config::DSiSDEnable = false;

        Config::RewindEnabled = emulatorConfiguration.rewindEnabled;
        Config::RewindCaptureSpacingSeconds = emulatorConfiguration.rewindCaptureSpacingSeconds;
        Config::RewindLengthSeconds = emulatorConfiguration.rewindLengthSeconds;
        // Use 20MB per savestate
        RewindManager::SetRewindBufferSizes(1024 * 1024 * 20, 256 * 384 * 4);
    }

    void setup(AAssetManager* androidAssetManager, AndroidCameraHandler* androidCameraHandler, RetroAchievements::RACallback* raCallback, FrameRenderedCallback* androidFrameRenderedCallback, u32* screenshotBufferPointer, int screenshotWidthParam, int screenshotHeightParam, long glContext, bool isMasterInstance) {
        assetManager = androidAssetManager;
        cameraHandler = androidCameraHandler;
        retroAchievementsCallback = raCallback;
        frameRenderedCallback = androidFrameRenderedCallback;
        LocalMultiplayer::SetIsMasterInstance(isMasterInstance);
        setupOpenGlContext(glContext);
        screenshotWidth = screenshotWidthParam;
        screenshotHeight = screenshotHeightParam;
        screenshotRenderer = new ScreenshotRenderer(screenshotBufferPointer, screenshotWidth, screenshotHeight);
        RewindManager::SetRewindBufferSizes(1024 * 1024 * 20, screenshotWidth * screenshotHeight * 4);

        NDS::Init();

        if (currentConfiguration.soundEnabled) {
            setupAudioOutputStream(currentConfiguration.audioLatency, currentConfiguration.volume);
        }
        if (currentConfiguration.micSource == 2) {
            setupMicInputStream();
        }

        GPU::InitRenderer(currentConfiguration.renderer);
        GPU::SetRenderSettings(currentConfiguration.renderer, currentConfiguration.renderSettings);
        SPU::SetInterpolation(currentConfiguration.audioInterpolation);

        openGlContext->Release();
    }

    void setCodeList(std::list<Cheat> cheats)
    {
        if (arCodeFile == NULL) {
            arCodeFile = new AndroidARCodeFile();
            AREngine::SetCodeFile(arCodeFile);
        }

        ARCodeList codeList;

        for (std::list<Cheat>::iterator it = cheats.begin(); it != cheats.end(); it++)
        {
            Cheat& cheat = *it;

            ARCode code = {
                    .Enabled = true,
                    .CodeLen = cheat.codeLength
            };
            memcpy(code.Code, cheat.code, sizeof(code.Code));

            codeList.push_back(code);
        }

        arCodeFile->updateCodeList(codeList);
    }

    void setupAchievements(std::list<RetroAchievements::RAAchievement> achievements, std::string* richPresenceScript)
    {
        RetroAchievements::LoadAchievements(achievements);
        if (richPresenceScript != nullptr)
            RetroAchievements::SetupRichPresence(*richPresenceScript);
    }

    void unloadAchievements(std::list<RetroAchievements::RAAchievement> achievements)
    {
        RetroAchievements::UnloadAchievements(achievements);
    }

    std::string getRichPresenceStatus()
    {
        return RetroAchievements::GetRichPresenceStatus();
    }

    /**
     * Used to update the emulator's configuration during runtime. Will only update the configurations that can actually change during runtime without causing issues,
     *
     * @param emulatorConfiguration The new emulator configuration
     */
    void updateEmulatorConfiguration(EmulatorConfiguration emulatorConfiguration) {
        Config::AudioBitrate = emulatorConfiguration.audioBitrate;
        SPU::SetInterpolation(emulatorConfiguration.audioInterpolation);
        Config::RewindEnabled = emulatorConfiguration.rewindEnabled;
        Config::RewindCaptureSpacingSeconds = emulatorConfiguration.rewindCaptureSpacingSeconds;
        Config::RewindLengthSeconds = emulatorConfiguration.rewindLengthSeconds;
        isRenderConfigurationDirty = true;

        if (emulatorConfiguration.rewindEnabled) {
            RewindManager::TrimRewindWindowIfRequired();
        } else {
            RewindManager::Reset();
        }

        if (emulatorConfiguration.soundEnabled && currentConfiguration.volume > 0) {
            if (!audioStream) {
                setupAudioOutputStream(emulatorConfiguration.audioLatency, emulatorConfiguration.volume);
            } else if (currentConfiguration.audioLatency != emulatorConfiguration.audioLatency || currentConfiguration.volume != emulatorConfiguration.volume) {
                // Recreate audio stream with new settings
                cleanupAudioOutputStream();
                setupAudioOutputStream(emulatorConfiguration.audioLatency, emulatorConfiguration.volume);
            }
        } else if (audioStream) {
            cleanupAudioOutputStream();
        }

        int oldMicSource = actualMicSource;
        actualMicSource = emulatorConfiguration.micSource;

        if (oldMicSource == 2 && emulatorConfiguration.micSource != 2) {
            // No longer using device mic. Destroy stream
            cleanupMicInputStream();
        } else if (oldMicSource != 2 && emulatorConfiguration.micSource == 2) {
            // Now using device mic. Setup stream
            setupMicInputStream();
        }

        currentConfiguration = emulatorConfiguration;
    }

    int loadRom(char* romPath, char* sramPath, RomGbaSlotConfig* gbaSlotConfig)
    {
        copyString(&currentRomPath, romPath);
        copyString(&currentSramPath, sramPath);
        updateCurrentGbaSlotConfig(gbaSlotConfig);
        currentRunMode = ROM;

        bool loaded = ROMManager::LoadROM(romPath, sramPath, true);
        if (!loaded)
            return 2;

        // Slot 2 is not supported in DSi
        if (NDS::ConsoleType == 0)
        {
            if (gbaSlotConfig->type == GBA_ROM)
            {
                RomGbaSlotConfigGbaRom* gbaRomConfig = (RomGbaSlotConfigGbaRom*) gbaSlotConfig;
                if (!ROMManager::LoadGBAROM(gbaRomConfig->romPath, gbaRomConfig->savePath))
                    return 1;
            }
            else if (gbaSlotConfig->type == MEMORY_EXPANSION)
            {
                ROMManager::LoadGBAAddon(NDS::GBAAddon_RAMExpansion);
            }
        }

        NDS::Start();

        return 0;
    }

    int bootFirmware()
    {
        currentRunMode = FIRMWARE;
        ROMManager::SetupResult result = ROMManager::VerifySetup();
        if (result != ROMManager::SUCCESS)
        {
            return result;
        }

        bool successful = ROMManager::LoadBIOS();
        if (successful)
        {
            NDS::Start();
            return ROMManager::SUCCESS;
        }
        else
        {
            return ROMManager::FIRMWARE_NOT_BOOTABLE;
        }
    }

    void start()
    {
        if (audioStream)
            audioStream->requestStart();

        if (micInputStream && isMicInputEnabled)
            micInputStream->requestStart();

        openGlContext->Use();

        screenshotRenderer->init();
        RetroAchievements::Init(retroAchievementsCallback);
        frame = 0;
    }

    u32 loop()
    {
        if (isRenderConfigurationDirty) {
            GPU::SetRenderSettings(currentConfiguration.renderer, currentConfiguration.renderSettings);
            isRenderConfigurationDirty = false;
        }

        u32 nLines = NDS::RunFrame();
        RetroAchievements::FrameUpdate();

        if (ROMManager::NDSSave)
            ROMManager::NDSSave->CheckFlush();

        if (ROMManager::GBASave)
            ROMManager::GBASave->CheckFlush();

        int frontbuf = GPU::FrontBuffer;
        int targetTexture;
        if (GPU::Renderer == 0)
        {
            if (GPU::Framebuffer[frontbuf][0] && GPU::Framebuffer[frontbuf][1])
            {
                glBindTexture(GL_TEXTURE_2D, softwareRenderingTexture);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 192, GL_RGBA, GL_UNSIGNED_BYTE, GPU::Framebuffer[frontbuf][0]);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 192 + 2, 256, 192, GL_RGBA, GL_UNSIGNED_BYTE, GPU::Framebuffer[frontbuf][1]);
                glBindTexture(GL_TEXTURE_2D, 0);
            }
            targetTexture = softwareRenderingTexture;
        }
        else
        {
            targetTexture = GPU::CurGLCompositor->GetOutputTexture(frontbuf);
        }

        glFlush();
        frameRenderedCallback->onFrameRendered((int) targetTexture);

        // Capture screenshot
        screenshotRenderer->renderScreenshot();

        frame++;
        if (RewindManager::ShouldCaptureState(frame))
        {
            auto nextRewindState = RewindManager::GetNextRewindSaveState(frame);
            saveRewindState(nextRewindState);
        }

        return nLines;
    }

    void pause() {
        if (audioStream)
            audioStream->requestPause();

        if (micInputStream && isMicInputEnabled)
            micInputStream->requestStop();
    }

    void resume() {
        if (audioStream)
            audioStream->requestStart();

        if (micInputStream && isMicInputEnabled)
            micInputStream->requestStart();
    }

    void reset()
    {
        frame = 0;

        if (currentRunMode == ROM)
        {
            RetroAchievements::Reset();
            RewindManager::Reset();
        }
        ROMManager::Reset();
    }

    void enableMic()
    {
        isMicInputEnabled = true;
        if (actualMicSource == 2 && micInputStream)
        {
            micInputStream->requestStart();
        }
    }

    void disableMic()
    {
        isMicInputEnabled = false;
        if (actualMicSource == 2 && micInputStream)
        {
            micInputStream->requestStop();
        }
    }

    void updateMic()
    {
        if (!isMicInputEnabled)
        {
            Frontend::Mic_FeedSilence();
            return;
        }

        switch (actualMicSource)
        {
            case 0: // no mic
                Frontend::Mic_FeedSilence();
                break;
            case 1: // white noise
                Frontend::Mic_FeedNoise();
                break;
            case 2: // host mic
                Frontend::Mic_FeedExternalBuffer();
                break;
        }
    }

    bool saveState(const char* path)
    {
        FileSavestate* savestate = new FileSavestate(path, true);
        if (savestate->Error)
        {
            delete savestate;
            return false;
        }
        else
        {
            bool result = NDS::DoSavestate(savestate);
            if (result)
                result = RetroAchievements::DoSavestate(savestate);

            delete savestate;
            return result;
        }
    }

    bool loadState(const char* path)
    {
        bool success = true;
        char* backupPath = joinPaths(currentConfiguration.internalFilesDir, "backup.mln");

        FileSavestate* backup = new FileSavestate(backupPath, true);
        NDS::DoSavestate(backup);
        RetroAchievements::DoSavestate(backup);
        delete backup;

        FileSavestate* savestate = new FileSavestate(path, false);
        if (savestate->Error)
        {
            delete savestate;

            savestate = new FileSavestate(backupPath, false);
            success = false;
        }

        NDS::DoSavestate(savestate);
        RetroAchievements::DoSavestate(savestate);
        delete savestate;

        // Delete backup file
        remove(backupPath);

        delete[] backupPath;

        return success;
    }

    bool saveRewindState(RewindManager::RewindSaveState rewindSaveState)
    {
        MemorySavestate* savestate = new MemorySavestate(rewindSaveState.buffer, true);
        if (savestate->Error)
        {
            delete savestate;
            return false;
        }
        else
        {
            bool success = NDS::DoSavestate(savestate);
            if (success)
                success = RetroAchievements::DoSavestate(savestate);

            if (success)
                memcpy(rewindSaveState.screenshot, screenshotRenderer->getScreenshot(), screenshotWidth * screenshotHeight * 4);

            delete savestate;
            return success;
        }
    }

    bool loadRewindState(RewindManager::RewindSaveState rewindSaveState)
    {
        bool success = true;
        char* backupPath = joinPaths(currentConfiguration.internalFilesDir, "backup.mln");

        FileSavestate* backup = new FileSavestate(backupPath, true);
        NDS::DoSavestate(backup);
        RetroAchievements::DoSavestate(backup);
        delete backup;

        Savestate* savestate = new MemorySavestate(rewindSaveState.buffer, false);
        if (savestate->Error)
        {
            delete savestate;

            savestate = new FileSavestate(backupPath, false);
            success = false;
        }

        NDS::DoSavestate(savestate);
        RetroAchievements::DoSavestate(savestate);
        delete savestate;

        // Delete backup file
        remove(backupPath);
        // Restore frame
        frame = rewindSaveState.frame;
        RewindManager::OnRewindFromState(rewindSaveState);

        delete[] backupPath;

        return success;
    }

    RewindWindow getRewindWindow()
    {
        return RewindWindow {
            .currentFrame = frame,
            .rewindStates = RewindManager::GetRewindWindow()
        };
    }

    void stop()
    {
        RetroAchievements::DeInit();
        ROMManager::EjectCart();
        ROMManager::EjectGBACart();
        NDS::Stop();
        GPU::DeInitRenderer();
        NDS::DeInit();
        RewindManager::Reset();

        screenshotRenderer->cleanup();
    }

    void cleanup()
    {
        free(currentRomPath);
        free(currentSramPath);
        currentRomPath = NULL;
        currentSramPath = NULL;
        delete screenshotRenderer;

        cleanupAudioOutputStream();
        cleanupMicInputStream();
        cleanupOpenGlContext();

        if (arCodeFile != NULL) {
            delete arCodeFile;
            arCodeFile = NULL;
        }

        if (currentGbaSlotConfig != NULL)
        {
            delete currentGbaSlotConfig;
            currentGbaSlotConfig = NULL;
        }

        assetManager = NULL;
        frameRenderedCallback = NULL;
        screenshotRenderer = NULL;
    }

    void setupAudioOutputStream(int audioLatency, int volume)
    {
        oboe::PerformanceMode performanceMode;
        switch (audioLatency) {
            case 0:
                performanceMode = oboe::PerformanceMode::LowLatency;
                break;
            case 1:
                performanceMode = oboe::PerformanceMode::None;
                break;
            case 2:
                performanceMode = oboe::PerformanceMode::PowerSaving;
                break;
            default:
                performanceMode = oboe::PerformanceMode::None;
        }

        outputCallback = new OboeCallback(volume);
        audioStreamErrorCallback = new MelonAudioStreamErrorCallback(resetAudioOutputStream);
        oboe::AudioStreamBuilder streamBuilder;
        streamBuilder.setChannelCount(2);
        streamBuilder.setFramesPerCallback(1024);
        streamBuilder.setSampleRate(48000);
        streamBuilder.setFormat(oboe::AudioFormat::I16);
        streamBuilder.setDirection(oboe::Direction::Output);
        streamBuilder.setPerformanceMode(performanceMode);
        streamBuilder.setSharingMode(oboe::SharingMode::Shared);
        streamBuilder.setCallback(outputCallback);
        streamBuilder.setErrorCallback(audioStreamErrorCallback);

        oboe::Result result = streamBuilder.openStream(audioStream);
        if (result != oboe::Result::OK) {
            fprintf(stderr, "Failed to init audio stream");
            delete outputCallback;
            delete audioStreamErrorCallback;
            outputCallback = NULL;
            audioStreamErrorCallback = NULL;
        } else {
            Frontend::Init_Audio(audioStream->getSampleRate());
        }
    }

    void cleanupAudioOutputStream()
    {
        if (audioStream) {
            if (audioStream->getState() < oboe::StreamState::Closing) {
                audioStream->requestStop();
                audioStream->close();
            }
            delete outputCallback;
            delete audioStreamErrorCallback;
            audioStream = NULL;
            outputCallback = NULL;
            audioStreamErrorCallback = NULL;
        }
    }

    void setupMicInputStream()
    {
        micInputCallback = new MicInputOboeCallback(MIC_BUFFER_SIZE);
        oboe::AudioStreamBuilder micStreamBuilder;
        micStreamBuilder.setChannelCount(1);
        micStreamBuilder.setFramesPerCallback(1024);
        micStreamBuilder.setSampleRate(44100);
        micStreamBuilder.setFormat(oboe::AudioFormat::I16);
        micStreamBuilder.setDirection(oboe::Direction::Input);
        micStreamBuilder.setInputPreset(oboe::InputPreset::Generic);
        micStreamBuilder.setPerformanceMode(oboe::PerformanceMode::PowerSaving);
        micStreamBuilder.setSharingMode(oboe::SharingMode::Exclusive);
        micStreamBuilder.setCallback(micInputCallback);

        oboe::Result micResult = micStreamBuilder.openStream(micInputStream);
        if (micResult != oboe::Result::OK) {
            actualMicSource = 1;
            fprintf(stderr, "Failed to init mic audio stream");
            delete micInputCallback;
            micInputCallback = NULL;
        } else {
            Frontend::Mic_SetExternalBuffer(micInputCallback->buffer, MIC_BUFFER_SIZE);
        }
    }

    void cleanupMicInputStream()
    {
        if (micInputStream) {
            micInputStream->requestStop();
            micInputStream->close();
            delete micInputCallback;
            micInputStream = NULL;
            micInputCallback = NULL;
            Frontend::Mic_SetExternalBuffer(NULL, 0);
        }
    }

    void resetAudioOutputStream()
    {
        cleanupAudioOutputStream();
        setupAudioOutputStream(currentConfiguration.audioLatency, currentConfiguration.volume);
        if (audioStream) {
            audioStream->requestStart();
        }
    }

    bool setupOpenGlContext(long sharedGlContext)
    {
        if (openGlContext != nullptr)
            return true;

        openGlContext = new OpenGLContext();
        if (!openGlContext->InitContext(sharedGlContext))
        {
            LOG_ERROR(MELONDS_TAG, "Failed to init OpenGL context");
            openGlContext->DeInit();
            currentConfiguration.renderer = 0;
            return false;
        }
        else
        {
            LOG_DEBUG(MELONDS_TAG, "OpenGL context initialised");
            if (!openGlContext->Use())
            {
                LOG_ERROR(MELONDS_TAG, "Failed to use OpenGL context");
                cleanupOpenGlContext();
                return false;
            }
        }

        // Generate texture for software rendering
        glGenTextures(1, &softwareRenderingTexture);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, softwareRenderingTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        // Add 2 lines of spacing between the screens to match OpenGL rendering
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 192 * 2 + 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glBindTexture(GL_TEXTURE_2D, 0);

        return true;
    }

    void cleanupOpenGlContext()
    {
        if (openGlContext == nullptr)
            return;

        if (openGlContext->Use())
        {
            glDeleteTextures(1, &softwareRenderingTexture);
        }
        openGlContext->Release();
        openGlContext->DeInit();
        delete openGlContext;
        openGlContext = nullptr;
    }

    void updateCurrentGbaSlotConfig(RomGbaSlotConfig* newConfig)
    {
        if (currentGbaSlotConfig != nullptr)
            delete currentGbaSlotConfig;

        if (newConfig->type == RomGbaSlotConfigType::GBA_ROM)
        {
            RomGbaSlotConfigGbaRom* gbaRomConfig = new RomGbaSlotConfigGbaRom;
            gbaRomConfig->romPath = ((RomGbaSlotConfigGbaRom*) newConfig)->romPath;
            gbaRomConfig->savePath = ((RomGbaSlotConfigGbaRom*) newConfig)->savePath;

            currentGbaSlotConfig = (RomGbaSlotConfig*) gbaRomConfig;
        }
        else if (newConfig->type == RomGbaSlotConfigType::MEMORY_EXPANSION)
        {
            currentGbaSlotConfig = (RomGbaSlotConfig*) new RomGbaSlotConfigMemoryExpansion;
        }
        else
        {
            currentGbaSlotConfig = (RomGbaSlotConfig*) new RomGbaSlotConfigNone;
        }
    }

    void copyString(char** dest, const char* source)
    {
        if (source == nullptr)
        {
            if (*dest != nullptr)
            {
                free(*dest);
                *dest = nullptr;
            }

            return;
        }

        int length = strlen(source);
        if (*dest == nullptr)
        {
            *dest = (char*) malloc(length + 1);
        }
        else
        {
            *dest = (char*) realloc(*dest, length + 1);
        }

        strcpy(*dest, source);
    }
}

