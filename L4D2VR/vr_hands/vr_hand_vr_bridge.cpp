#include "vr.h"

#include "game.h"
#include "sdk.h"
#include "vr_hand_system.h"
#include "vr_hand_math.h"

#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <string>
#include <memory>
#include <vector>

VR::VR() = default;

VR::~VR() = default;

namespace
{
    const std::string kNoManualReloadMagazineGlbPath;
}

bool VR::DrawVrHandsForEye(const CViewSetup& view, int eyeIndex, VrHandDrawPass drawPass)
{
    // Raw D3D9 commands issued directly from Source RenderView are safe only in
    // single-threaded rendering. The queued path needs a DXVK-side submission point.
    if (!m_VrHandsEnabled || !m_IsVREnabled || !m_Input || !m_Game || m_Game->GetMatQueueMode() != 0)
        return false;

    IDirect3DSurface9* surface = (eyeIndex == 0) ? m_D9LeftEyeSurface : m_D9RightEyeSurface;
    if (!surface)
        return false;

    IDirect3DDevice9* device = nullptr;
    if (FAILED(surface->GetDevice(&device)) || !device)
        return false;

    if (!m_VrHands)
        m_VrHands = std::make_unique<VrHandSystem>();

    float sceneLightScale = 1.0f;
    if (m_AutoFlashlightHasScreenLuma)
    {
        const float localLuma = std::max(
            m_AutoFlashlightCenterMedianLuma,
            m_AutoFlashlightPeripheralMedianLuma * 0.35f);
        sceneLightScale = std::clamp(localLuma / 110.0f, 0.08f, 1.0f);
    }

    // The detachable magazine is rendered by repeating the native Source
    // viewmodel draw with every non-clip bone moved out of view. This reuses
    // the weapon model's active materials, shader path and lighting exactly.
    // Do not also draw the old standalone D3D9 GLB visual.
    const VrHandMatrix4* manualReloadMagazineWorldPtr = nullptr;
    const bool manualReloadMagazineUseViewmodelLayer = false;
    const Vector currentViewmodelPosition = GetRecommendedViewmodelAbsPos();
    const QAngle currentViewmodelAngles = GetRecommendedViewmodelAbsAngle();

    const bool drewAny = m_VrHands->DrawForEye(
        device,
        m_Input,
        view,
        eyeIndex,
        m_VRScale,
        m_VrHandsModelScale,
        m_VrHandsMotionRangeWithoutController,
        m_VrHandsRightUseViewmodelPose,
        m_MouseModeEnabled,
        m_VrHandsDebugLog,
        sceneLightScale,
        m_LeftControllerPosAbs,
        m_LeftControllerAngAbs,
        m_RightControllerPosAbs,
        m_RightControllerAngAbs,
        currentViewmodelPosition,
        currentViewmodelAngles,
        m_VrHandsLeftPoseOffsetMeters,
        m_VrHandsLeftPoseRotationOffsetDeg,
        m_VrHandsRightPoseOffsetMeters,
        m_VrHandsRightPoseRotationOffsetDeg,
        kNoManualReloadMagazineGlbPath,
        manualReloadMagazineWorldPtr,
        manualReloadMagazineUseViewmodelLayer,
        drawPass);
    device->Release();
    return drewAny;
}

void VR::BeginVrHandsEyeRender(const CViewSetup& view, int eyeIndex)
{
    m_VrHandsActiveEyeView = nullptr;
    m_VrHandsActiveEyeIndex = -1;
    m_VrHandsWorldMaskDrawn = false;
    if (!m_VrHandsEnabled || !m_IsVREnabled || !m_Input || !m_Game || m_Game->GetMatQueueMode() != 0)
        return;

    m_VrHandsActiveEyeView = &view;
    m_VrHandsActiveEyeIndex = eyeIndex;
}

void VR::DrawVrHandsWorldDepthMaskBeforeViewmodel()
{
    if (!m_VrHandsActiveEyeView || m_VrHandsActiveEyeIndex < 0 || m_VrHandsWorldMaskDrawn)
        return;

    IDirect3DSurface9* surface = (m_VrHandsActiveEyeIndex == 0) ? m_D9LeftEyeSurface : m_D9RightEyeSurface;
    if (!surface)
        return;

    IDirect3DDevice9* device = nullptr;
    if (FAILED(surface->GetDevice(&device)) || !device)
        return;

    if (!m_VrHands)
        m_VrHands = std::make_unique<VrHandSystem>();

    const bool stencilReady = m_VrHands->ClearViewmodelOcclusionStencil(device);
    device->Release();
    if (!stencilReady)
        return;

    m_VrHandsWorldMaskDrawn = DrawVrHandsForEye(
        *m_VrHandsActiveEyeView,
        m_VrHandsActiveEyeIndex,
        VrHandDrawPass::WorldVisibilityMask);
}

void VR::FinishVrHandsEyeRender()
{
    const CViewSetup* view = m_VrHandsActiveEyeView;
    const int eyeIndex = m_VrHandsActiveEyeIndex;
    const bool worldMaskDrawn = m_VrHandsWorldMaskDrawn;
    m_VrHandsActiveEyeView = nullptr;
    m_VrHandsActiveEyeIndex = -1;
    m_VrHandsWorldMaskDrawn = false;

    if (!view || eyeIndex < 0)
        return;

    DrawVrHandsForEye(
        *view,
        eyeIndex,
        worldMaskDrawn ? VrHandDrawPass::ViewmodelComposite : VrHandDrawPass::WorldDepth);
}

void VR::ReleaseVrHandsD3DResources()
{
    if (m_VrHands)
        m_VrHands->OnDeviceLost();
}

namespace
{
    constexpr int kManualReloadViewmodelCycleOffset = 0x65C;
    constexpr int kManualReloadViewmodelPlaybackRateOffset = 0x660;
    constexpr int kManualReloadViewmodelSequenceOffset = 0x8A4;
    constexpr float kManualReloadMinimumPostInsertTailSeconds = 0.75f;
    constexpr float kManualReloadPostInsertBoundaryWaitTimeoutSeconds = 8.25f;

    bool ManualReloadWeaponUsesDetachableMagazine(C_WeaponCSBase::WeaponID weaponId)
    {
        switch (weaponId)
        {
        case C_WeaponCSBase::WeaponID::PISTOL:
        case C_WeaponCSBase::WeaponID::UZI:
        case C_WeaponCSBase::WeaponID::M16A1:
        case C_WeaponCSBase::WeaponID::HUNTING_RIFLE:
        case C_WeaponCSBase::WeaponID::MAC10:
        case C_WeaponCSBase::WeaponID::SCAR:
        case C_WeaponCSBase::WeaponID::SNIPER_MILITARY:
        case C_WeaponCSBase::WeaponID::AK47:
        case C_WeaponCSBase::WeaponID::MAGNUM:
        case C_WeaponCSBase::WeaponID::MP5:
        case C_WeaponCSBase::WeaponID::SG552:
        case C_WeaponCSBase::WeaponID::AWP:
        case C_WeaponCSBase::WeaponID::SCOUT:
            return true;
        default:
            return false;
        }
    }

    Vector ManualReloadGetInsertionAxisLocal(const VR* vr)
    {
        if (!vr)
            return Vector(0.0f, -1.0f, 0.0f);
        return vr->m_ManualReloadResolvedInsertionAxisValid
            ? vr->m_ManualReloadResolvedInsertionAxisLocal
            : vr->m_ManualReloadMagazineInsertionAxisLocal;
    }

    float ManualReloadVectorLength(const Vector& value)
    {
        return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
    }

    Vector ManualReloadNormalize(const Vector& value)
    {
        const float length = ManualReloadVectorLength(value);
        if (!(length > 0.000001f))
            return Vector(0.0f, 0.0f, 1.0f);
        return value * (1.0f / length);
    }

    Vector ManualReloadMatrixTranslation(const VrHandMatrix4& matrix)
    {
        return Vector(
            VrHandMath::Get(matrix, 0, 3),
            VrHandMath::Get(matrix, 1, 3),
            VrHandMath::Get(matrix, 2, 3));
    }

    Vector ManualReloadTransformDirection(const VrHandMatrix4& matrix, const Vector& value)
    {
        return Vector(
            VrHandMath::Get(matrix, 0, 0) * value.x + VrHandMath::Get(matrix, 0, 1) * value.y + VrHandMath::Get(matrix, 0, 2) * value.z,
            VrHandMath::Get(matrix, 1, 0) * value.x + VrHandMath::Get(matrix, 1, 1) * value.y + VrHandMath::Get(matrix, 1, 2) * value.z,
            VrHandMath::Get(matrix, 2, 0) * value.x + VrHandMath::Get(matrix, 2, 1) * value.y + VrHandMath::Get(matrix, 2, 2) * value.z);
    }

    VrHandMatrix4 ManualReloadStripScale(const VrHandMatrix4& matrix)
    {
        VrHandMatrix4 out = matrix;
        for (int column = 0; column < 3; ++column)
        {
            Vector axis(
                VrHandMath::Get(out, 0, column),
                VrHandMath::Get(out, 1, column),
                VrHandMath::Get(out, 2, column));
            axis = ManualReloadNormalize(axis);
            VrHandMath::Set(out, 0, column, axis.x);
            VrHandMath::Set(out, 1, column, axis.y);
            VrHandMath::Set(out, 2, column, axis.z);
        }
        VrHandMath::Set(out, 3, 0, 0.0f);
        VrHandMath::Set(out, 3, 1, 0.0f);
        VrHandMath::Set(out, 3, 2, 0.0f);
        VrHandMath::Set(out, 3, 3, 1.0f);
        return out;
    }

    VrHandMatrix4 ManualReloadBuildLocalTransform(
        float sourceUnitsPerMeter,
        float modelScale,
        const Vector& localPositionOffsetMeters,
        const Vector& localRotationOffsetDeg,
        bool includeModelScale)
    {
        constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
        const float rx = localRotationOffsetDeg.x * kDegToRad;
        const float ry = localRotationOffsetDeg.y * kDegToRad;
        const float rz = localRotationOffsetDeg.z * kDegToRad;
        const float sx = std::sin(rx), cx = std::cos(rx);
        const float sy = std::sin(ry), cy = std::cos(ry);
        const float sz = std::sin(rz), cz = std::cos(rz);

        VrHandMatrix4 local = VrHandMath::Identity();
        VrHandMath::Set(local, 0, 0, cz * cy);
        VrHandMath::Set(local, 0, 1, cz * sy * sx - sz * cx);
        VrHandMath::Set(local, 0, 2, cz * sy * cx + sz * sx);
        VrHandMath::Set(local, 1, 0, sz * cy);
        VrHandMath::Set(local, 1, 1, sz * sy * sx + cz * cx);
        VrHandMath::Set(local, 1, 2, sz * sy * cx - cz * sx);
        VrHandMath::Set(local, 2, 0, -sy);
        VrHandMath::Set(local, 2, 1, cy * sx);
        VrHandMath::Set(local, 2, 2, cy * cx);
        VrHandMath::Set(local, 0, 3, localPositionOffsetMeters.x * sourceUnitsPerMeter);
        VrHandMath::Set(local, 1, 3, localPositionOffsetMeters.y * sourceUnitsPerMeter);
        VrHandMath::Set(local, 2, 3, localPositionOffsetMeters.z * sourceUnitsPerMeter);

        if (!includeModelScale)
            return local;

        VrHandMatrix4 scale = VrHandMath::Identity();
        const float sourceScale = sourceUnitsPerMeter * modelScale;
        VrHandMath::Set(scale, 0, 0, sourceScale);
        VrHandMath::Set(scale, 1, 1, sourceScale);
        VrHandMath::Set(scale, 2, 2, sourceScale);
        return VrHandMath::Multiply(local, scale);
    }

    bool ManualReloadReadViewmodelAnimation(void* entity, int& sequence, float& cycle, float& playbackRate)
    {
        if (!entity)
            return false;
#if defined(_MSC_VER)
        __try
        {
#endif
            const auto* base = reinterpret_cast<const unsigned char*>(entity);
            sequence = *reinterpret_cast<const int*>(base + kManualReloadViewmodelSequenceOffset);
            cycle = *reinterpret_cast<const float*>(base + kManualReloadViewmodelCycleOffset);
            playbackRate = *reinterpret_cast<const float*>(base + kManualReloadViewmodelPlaybackRateOffset);
            return std::isfinite(cycle) && std::isfinite(playbackRate);
#if defined(_MSC_VER)
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
#endif
    }

    bool ManualReloadWriteViewmodelAnimation(void* entity, int sequence, float cycle, float playbackRate)
    {
        if (!entity)
            return false;
#if defined(_MSC_VER)
        __try
        {
#endif
            auto* base = reinterpret_cast<unsigned char*>(entity);
            *reinterpret_cast<int*>(base + kManualReloadViewmodelSequenceOffset) = sequence;
            *reinterpret_cast<float*>(base + kManualReloadViewmodelCycleOffset) = cycle;
            *reinterpret_cast<float*>(base + kManualReloadViewmodelPlaybackRateOffset) = playbackRate;
            return true;
#if defined(_MSC_VER)
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
#endif
    }

    bool ManualReloadSoundLooksWeaponRelated(const char* sample)
    {
        if (!sample || !*sample)
            return false;

        std::string lower(sample);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lower.find("weapon") != std::string::npos ||
            lower.find("reload") != std::string::npos ||
            lower.find("clip") != std::string::npos ||
            lower.find("mag") != std::string::npos ||
            lower.find("bolt") != std::string::npos ||
            lower.find("slide") != std::string::npos ||
            lower.find("rifle") != std::string::npos ||
            lower.find("smg") != std::string::npos ||
            lower.find("pistol") != std::string::npos ||
            lower.find("sniper") != std::string::npos;
    }

    bool ManualReloadSoundStartsInsertTail(const char* sample)
    {
        if (!sample || !*sample)
            return false;

        std::string lower(sample);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lower.find("clip_in") != std::string::npos ||
            lower.find("clip-in") != std::string::npos ||
            lower.find("clipin") != std::string::npos ||
            lower.find("clip.insert") != std::string::npos ||
            lower.find("mag_in") != std::string::npos ||
            lower.find("mag-in") != std::string::npos ||
            lower.find("magin") != std::string::npos ||
            lower.find("magazine_in") != std::string::npos ||
            lower.find("magazine-in") != std::string::npos ||
            lower.find("magazinein") != std::string::npos ||
            lower.find("mag.insert") != std::string::npos ||
            lower.find("insert") != std::string::npos ||
            lower.find("clip_locked") != std::string::npos ||
            lower.find("mag_locked") != std::string::npos ||
            lower.find("magazine_locked") != std::string::npos;
    }

    enum class ManualReloadDelayedSoundStage
    {
        Other,
        Insert,
        Lock,
        Ready,
        SlideBack,
        SlideForward
    };

    std::string ManualReloadLowerSoundSample(const std::string& sample)
    {
        std::string lower(sample);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lower;
    }

    ManualReloadDelayedSoundStage ManualReloadClassifyDelayedSound(const std::string& sample)
    {
        const std::string lower = ManualReloadLowerSoundSample(sample);
        if (lower.find("clip_in") != std::string::npos ||
            lower.find("clip-in") != std::string::npos ||
            lower.find("clipin") != std::string::npos ||
            lower.find("clip.insert") != std::string::npos ||
            lower.find("mag_in") != std::string::npos ||
            lower.find("mag-in") != std::string::npos ||
            lower.find("magin") != std::string::npos ||
            lower.find("magazine_in") != std::string::npos ||
            lower.find("magazine-in") != std::string::npos ||
            lower.find("magazinein") != std::string::npos ||
            lower.find("mag.insert") != std::string::npos ||
            lower.find("insert") != std::string::npos)
        {
            return ManualReloadDelayedSoundStage::Insert;
        }
        if (lower.find("clip_locked") != std::string::npos ||
            lower.find("clip-locked") != std::string::npos ||
            lower.find("cliplocked") != std::string::npos ||
            lower.find("mag_locked") != std::string::npos ||
            lower.find("mag-locked") != std::string::npos ||
            lower.find("maglocked") != std::string::npos ||
            lower.find("magazine_locked") != std::string::npos ||
            lower.find("magazine-locked") != std::string::npos ||
            lower.find("magazinelocked") != std::string::npos)
        {
            return ManualReloadDelayedSoundStage::Lock;
        }
        if (lower.find("slideback") != std::string::npos ||
            lower.find("slide_back") != std::string::npos ||
            lower.find("slide-back") != std::string::npos ||
            lower.find("boltback") != std::string::npos ||
            lower.find("bolt_back") != std::string::npos ||
            lower.find("bolt-back") != std::string::npos)
        {
            return ManualReloadDelayedSoundStage::SlideBack;
        }
        if (lower.find("slideforward") != std::string::npos ||
            lower.find("slide_forward") != std::string::npos ||
            lower.find("slide-forward") != std::string::npos ||
            lower.find("boltforward") != std::string::npos ||
            lower.find("bolt_forward") != std::string::npos ||
            lower.find("bolt-forward") != std::string::npos)
        {
            return ManualReloadDelayedSoundStage::SlideForward;
        }
        if (lower.find("ready") != std::string::npos)
            return ManualReloadDelayedSoundStage::Ready;
        return ManualReloadDelayedSoundStage::Other;
    }

    int ManualReloadSoundSpecificityScore(const std::string& sample)
    {
        const std::string lower = ManualReloadLowerSoundSample(sample);
        if (lower.find("weapons/rifle/gunother/") != std::string::npos ||
            lower.find("weapons\\rifle\\gunother\\") != std::string::npos ||
            lower.find("weapons/smg/gunother/") != std::string::npos ||
            lower.find("weapons\\smg\\gunother\\") != std::string::npos ||
            lower.find("weapons/pistol/gunother/") != std::string::npos ||
            lower.find("weapons\\pistol\\gunother\\") != std::string::npos ||
            lower.find("weapons/sniper/gunother/") != std::string::npos ||
            lower.find("weapons\\sniper\\gunother\\") != std::string::npos)
        {
            return 0;
        }
        return lower.find("weapon") != std::string::npos ? 1 : 0;
    }

    size_t ManualReloadNormalizeDelayedSoundsForReplay(std::vector<ManualReloadDelayedSound>& sounds)
    {
        if (sounds.empty())
            return 0;

        size_t selectedInsertIndex = sounds.size();
        int selectedInsertScore = -1;
        for (size_t i = 0; i < sounds.size(); ++i)
        {
            if (ManualReloadClassifyDelayedSound(sounds[i].sample) != ManualReloadDelayedSoundStage::Insert)
                continue;

            const int score = ManualReloadSoundSpecificityScore(sounds[i].sample);
            if (selectedInsertIndex == sounds.size() || score > selectedInsertScore ||
                (score == selectedInsertScore && i > selectedInsertIndex))
            {
                selectedInsertIndex = i;
                selectedInsertScore = score;
            }
        }

        if (selectedInsertIndex == sounds.size())
            return 0;

        struct StageChoice
        {
            ManualReloadDelayedSoundStage stage = ManualReloadDelayedSoundStage::Other;
            size_t sourceIndex = 0;
            int specificity = -1;
        };

        std::vector<StageChoice> classifiedChoices;
        std::vector<size_t> otherChoices;
        for (size_t i = selectedInsertIndex; i < sounds.size(); ++i)
        {
            const ManualReloadDelayedSoundStage stage = ManualReloadClassifyDelayedSound(sounds[i].sample);
            if (stage == ManualReloadDelayedSoundStage::Other)
            {
                bool duplicate = false;
                for (size_t selected : otherChoices)
                {
                    if (sounds[selected].sample == sounds[i].sample)
                    {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate)
                    otherChoices.push_back(i);
                continue;
            }

            const int specificity = ManualReloadSoundSpecificityScore(sounds[i].sample);
            StageChoice* choice = nullptr;
            for (StageChoice& existing : classifiedChoices)
            {
                if (existing.stage == stage)
                {
                    choice = &existing;
                    break;
                }
            }
            if (!choice)
            {
                classifiedChoices.push_back({ stage, i, specificity });
                continue;
            }
            if (specificity > choice->specificity)
            {
                choice->sourceIndex = i;
                choice->specificity = specificity;
            }
        }

        std::vector<size_t> selectedIndices;
        selectedIndices.reserve(classifiedChoices.size() + otherChoices.size());
        for (const StageChoice& choice : classifiedChoices)
            selectedIndices.push_back(choice.sourceIndex);
        selectedIndices.insert(selectedIndices.end(), otherChoices.begin(), otherChoices.end());
        std::sort(selectedIndices.begin(), selectedIndices.end());

        const float rebaseSeconds = sounds[selectedInsertIndex].offsetSeconds;
        std::vector<ManualReloadDelayedSound> normalized;
        normalized.reserve(selectedIndices.size());
        for (size_t selected : selectedIndices)
        {
            ManualReloadDelayedSound event = sounds[selected];
            event.offsetSeconds = std::max(0.0f, event.offsetSeconds - rebaseSeconds);
            normalized.push_back(std::move(event));
        }

        const size_t removedCount = sounds.size() - normalized.size();
        sounds.swap(normalized);
        return removedCount;
    }

    std::string ManualReloadPrepareConsoleSoundSample(const std::string& rawSample)
    {
        size_t start = 0;
        while (start < rawSample.size())
        {
            const char c = rawSample[start];
            if (c == '*' || c == '#' || c == '@' || c == '>' || c == '<' ||
                c == '^' || c == ')' || c == '}' || c == '$' || c == '?' || c == '!')
            {
                ++start;
                continue;
            }
            break;
        }

        std::string sample = rawSample.substr(start);
        if (sample.rfind("sound/", 0) == 0 || sample.rfind("sound\\", 0) == 0)
            sample.erase(0, 6);

        std::string escaped;
        escaped.reserve(sample.size());
        for (char c : sample)
        {
            if (c == '"' || c == '\\')
                escaped.push_back('\\');
            if (c == '\r' || c == '\n' || c == ';')
                continue;
            escaped.push_back(c);
        }
        return escaped;
    }

}

bool VR::IsManualReloadActive() const
{
    return m_ManualReloadState != ManualReloadState::Idle;
}

bool VR::IsManualReloadBlockingFire() const
{
    // The gameplay reload command is intentionally decoupled from the physical insertion.
    // Block fire from the first reload frame until the delayed visual/audio tail has finished.
    return IsManualReloadActive();
}

bool VR::ShouldHideManualReloadNativeClip() const
{
    return m_ManualReloadHideNativeClip;
}

bool VR::CaptureManualReloadSound(int entityIndex, const char* sample, float volume, int flags, int pitch)
{
    // Do not swallow updates or stops for sounds that may already be playing. Clip-out audio belongs
    // to the visible removal phase and stays live. Delay only the hidden post-insert tail, starting
    // from the first actual magazine insertion sound.
    constexpr int kSoundChangeVolume = (1 << 0);
    constexpr int kSoundChangePitch = (1 << 1);
    constexpr int kSoundStop = (1 << 2);
    constexpr int kSoundStopLooping = (1 << 5);
    constexpr int kNonStartFlags = kSoundChangeVolume | kSoundChangePitch | kSoundStop | kSoundStopLooping;
    if ((flags & kNonStartFlags) != 0)
        return false;

    if (!sample || !*sample || !m_Game ||
        (m_ManualReloadState != ManualReloadState::WaitingForFreshMagazineGrab &&
            m_ManualReloadState != ManualReloadState::HoldingFreshMagazine &&
            m_ManualReloadState != ManualReloadState::AwaitingNativePostInsertBoundary))
    {
        return false;
    }

    const int localPlayerIndex = (m_Game->m_EngineClient != nullptr)
        ? m_Game->m_EngineClient->GetLocalPlayer()
        : -1;
    const bool fromViewmodel = m_ManualReloadViewmodelEntityIndex > 0 &&
        entityIndex == m_ManualReloadViewmodelEntityIndex;
    const bool fromLocalWeaponPath = (entityIndex == -1 || entityIndex == localPlayerIndex) &&
        ManualReloadSoundLooksWeaponRelated(sample);
    if (!fromViewmodel && !fromLocalWeaponPath)
        return false;

    const auto now = std::chrono::steady_clock::now();
    ManualReloadDelayedSound event;
    bool queued = false;
    bool insertTailStartedNow = false;
    {
        std::lock_guard<std::mutex> lock(m_ManualReloadSoundMutex);
        if (!m_ManualReloadSoundInsertTailStarted)
        {
            if (!ManualReloadSoundStartsInsertTail(sample))
                return false;

            m_ManualReloadSoundInsertTailStarted = true;
            m_ManualReloadSoundCaptureStarted = now;
            m_ManualReloadAudioInsertVisualOffsetSeconds = std::max(
                0.0f,
                m_ManualReloadVisualResumeDurationSeconds);
            m_ManualReloadAudioInsertVisualOffsetValid = true;
            insertTailStartedNow = true;
        }

        event.sample = sample;
        event.offsetSeconds = std::clamp(
            std::chrono::duration<float>(now - m_ManualReloadSoundCaptureStarted).count(),
            0.0f,
            3.0f);
        event.volume = std::clamp(volume, 0.0f, 1.0f);
        event.pitch = std::clamp(pitch, 1, 255);

        if (m_ManualReloadDelayedSounds.size() < 64)
        {
            const bool duplicate = !m_ManualReloadDelayedSounds.empty() &&
                m_ManualReloadDelayedSounds.back().sample == event.sample &&
                std::fabs(m_ManualReloadDelayedSounds.back().offsetSeconds - event.offsetSeconds) < 0.015f;
            if (!duplicate)
            {
                m_ManualReloadDelayedSounds.push_back(event);
                queued = true;
            }
        }
    }

    if (insertTailStartedNow)
    {
        Game::logMsg(
            "[VR][ManualReload][Audio] insert-tail capture started ent=%d visualOffset=%.3fs sample=%s",
            entityIndex,
            m_ManualReloadAudioInsertVisualOffsetSeconds,
            sample);
    }
    if (queued)
    {
        Game::logMsg(
            "[VR][ManualReload][Audio] delayed hidden-tail sound ent=%d offset=%.3fs sample=%s",
            entityIndex,
            event.offsetSeconds,
            sample);
    }
    return true;
}

void VR::ReplayManualReloadDelayedSounds()
{
    if (m_ManualReloadState != ManualReloadState::ResumingNativeReloadWithMagazine ||
        !m_Game ||
        m_ManualReloadSoundReplayStarted.time_since_epoch().count() == 0)
    {
        return;
    }

    const float elapsedSeconds = std::chrono::duration<float>(
        std::chrono::steady_clock::now() - m_ManualReloadSoundReplayStarted).count();
    std::vector<ManualReloadDelayedSound> due;
    {
        std::lock_guard<std::mutex> lock(m_ManualReloadSoundMutex);
        while (m_ManualReloadDelayedSoundReplayIndex < m_ManualReloadDelayedSounds.size() &&
            m_ManualReloadDelayedSounds[m_ManualReloadDelayedSoundReplayIndex].offsetSeconds <= elapsedSeconds)
        {
            due.push_back(m_ManualReloadDelayedSounds[m_ManualReloadDelayedSoundReplayIndex]);
            ++m_ManualReloadDelayedSoundReplayIndex;
        }
    }

    for (const ManualReloadDelayedSound& event : due)
    {
        const std::string sample = ManualReloadPrepareConsoleSoundSample(event.sample);
        if (sample.empty())
            continue;

        char volume[32] = {};
        std::snprintf(volume, sizeof(volume), "%.3f", static_cast<double>(event.volume));
        const std::string command = "playvol \"" + sample + "\" " + volume;
        m_Game->ClientCmd_Unrestricted(command.c_str());
        Game::logMsg(
            "[VR][ManualReload][Audio] replayed delayed sound offset=%.3fs sample=%s",
            event.offsetSeconds,
            event.sample.c_str());
    }
}

float VR::GetManualReloadReplayDurationSeconds() const
{
    const float replayStartOffsetSeconds = m_ManualReloadVisualReplayStartOffsetValid
        ? std::max(0.0f, m_ManualReloadVisualReplayStartOffsetSeconds)
        : 0.0f;
    const float remainingVisualSeconds = std::max(
        0.0f,
        m_ManualReloadVisualResumeDurationSeconds - replayStartOffsetSeconds);
    float durationSeconds = std::clamp(
        std::max(0.10f, remainingVisualSeconds + 0.05f),
        0.10f,
        3.0f);
    std::lock_guard<std::mutex> lock(m_ManualReloadSoundMutex);
    if (!m_ManualReloadDelayedSounds.empty())
        durationSeconds = std::max(durationSeconds, m_ManualReloadDelayedSounds.back().offsetSeconds + 0.05f);
    return std::clamp(durationSeconds, 0.10f, 3.0f);
}

void VR::StartManualReloadPostInsertReplay(const char* reason)
{
    if (!m_ManualReloadVisualReplayStartOffsetValid)
        return;

    m_ManualReloadState = ManualReloadState::ResumingNativeReloadWithMagazine;
    m_ManualReloadResumeStarted = std::chrono::steady_clock::now();
    m_ManualReloadSoundReplayStarted = m_ManualReloadResumeStarted;
    m_ManualReloadPostInsertBoundaryWaitStarted = {};
    size_t prunedSoundCount = 0;
    size_t replaySoundCount = 0;
    {
        std::lock_guard<std::mutex> lock(m_ManualReloadSoundMutex);
        prunedSoundCount = ManualReloadNormalizeDelayedSoundsForReplay(m_ManualReloadDelayedSounds);
        replaySoundCount = m_ManualReloadDelayedSounds.size();
        m_ManualReloadDelayedSoundReplayIndex = 0;
    }
    if (prunedSoundCount > 0)
    {
        Game::logMsg(
            "[VR][ManualReload][Audio] normalized insert-tail replay kept=%u pruned=%u",
            static_cast<unsigned int>(replaySoundCount),
            static_cast<unsigned int>(prunedSoundCount));
    }
    m_ManualReloadViewmodelFrozen = false;
    Game::logMsg(
        "[VR][ManualReload] native magazine restored; replaying post-insert tail reason=%s offset=%.3fs duration=%.3fs",
        reason ? reason : "unknown",
        m_ManualReloadVisualReplayStartOffsetSeconds,
        GetManualReloadReplayDurationSeconds());
}

bool VR::TryStartManualReloadPostInsertReplay(const char* reason)
{
    float audioInsertVisualOffsetSeconds = 0.0f;
    bool audioInsertVisualOffsetValid = false;
    {
        std::lock_guard<std::mutex> lock(m_ManualReloadSoundMutex);
        audioInsertVisualOffsetSeconds = m_ManualReloadAudioInsertVisualOffsetSeconds;
        audioInsertVisualOffsetValid = m_ManualReloadAudioInsertVisualOffsetValid;
    }

    // Workshop replacement models often return their visible magazine bone to the socket only
    // after the useful hand/bolt tail has already elapsed. The first clip-in sound is emitted at
    // the real insertion boundary, so prefer it whenever it moves the visual replay point earlier.
    // SnapManualReloadNativeMagazineToSocket keeps the magazine subtree fixed at the socket while
    // these earlier poses replay, preventing a second visible insertion.
    if (audioInsertVisualOffsetValid &&
        (!m_ManualReloadVisualReplayStartOffsetValid ||
            audioInsertVisualOffsetSeconds < m_ManualReloadVisualReplayStartOffsetSeconds))
    {
        const bool hadVisualBoundary = m_ManualReloadVisualReplayStartOffsetValid;
        const float previousOffsetSeconds = m_ManualReloadVisualReplayStartOffsetSeconds;
        m_ManualReloadVisualReplayStartOffsetSeconds = std::max(0.0f, audioInsertVisualOffsetSeconds);
        m_ManualReloadVisualReplayStartOffsetValid = true;
        Game::logMsg(
            "[VR][ManualReload] post-insert visual replay aligned to audio insert boundary offset=%.3fs previousVisualBoundary=%s%.3fs",
            m_ManualReloadVisualReplayStartOffsetSeconds,
            hadVisualBoundary ? "" : "unavailable/",
            previousOffsetSeconds);
    }

    if (!m_ManualReloadVisualReplayStartOffsetValid)
        return false;

    const float capturedSeconds = std::max(0.0f, m_ManualReloadVisualResumeDurationSeconds);
    float remainingSeconds = std::max(
        0.0f,
        capturedSeconds - std::max(0.0f, m_ManualReloadVisualReplayStartOffsetSeconds));

    // Do not start playback as soon as the native magazine first reaches the socket.
    // At that instant the cache usually contains no post-insert frames yet. Wait until
    // a visible tail has actually been sampled, or until the hidden native sequence ends.
    if (!m_ManualReloadTailCaptureComplete &&
        remainingSeconds < kManualReloadMinimumPostInsertTailSeconds)
    {
        return false;
    }

    // Some replacement models report the visual magazine boundary only at the very end
    // of their native sequence. Preserve a useful final window instead of jumping to idle.
    // The native magazine subtree is pinned to the socket while this window replays, so
    // shifting the replay point slightly earlier cannot redraw a second magazine insertion.
    if (remainingSeconds < kManualReloadMinimumPostInsertTailSeconds && capturedSeconds > 0.0f)
    {
        const float previousOffset = m_ManualReloadVisualReplayStartOffsetSeconds;
        m_ManualReloadVisualReplayStartOffsetSeconds = std::max(
            0.0f,
            capturedSeconds - kManualReloadMinimumPostInsertTailSeconds);
        remainingSeconds = std::max(
            0.0f,
            capturedSeconds - m_ManualReloadVisualReplayStartOffsetSeconds);
        Game::logMsg(
            "[VR][ManualReload] post-insert replay boundary widened offset=%.3fs->%.3fs captured=%.3fs preservedTail=%.3fs",
            previousOffset,
            m_ManualReloadVisualReplayStartOffsetSeconds,
            capturedSeconds,
            remainingSeconds);
    }

    StartManualReloadPostInsertReplay(reason);
    return true;
}

void VR::UseManualReloadPostInsertFallbackBoundary(const char* reason)
{
    if (m_ManualReloadVisualReplayStartOffsetValid)
        return;

    const float capturedSeconds = std::max(0.0f, m_ManualReloadVisualResumeDurationSeconds);
    // Preserve a real tail instead of jumping straight to idle. This fallback is used only when
    // the replacement model never exposes a reliable visual reinsertion boundary.
    const float preservedTailSeconds = std::clamp(capturedSeconds * 0.25f, 0.35f, 0.75f);
    m_ManualReloadVisualReplayStartOffsetSeconds = std::max(0.0f, capturedSeconds - preservedTailSeconds);
    m_ManualReloadVisualReplayStartOffsetValid = true;
    Game::logMsg(
        "[VR][ManualReload] post-insert boundary fallback reason=%s captured=%.3fs offset=%.3fs preservedTail=%.3fs",
        reason ? reason : "unknown",
        capturedSeconds,
        m_ManualReloadVisualReplayStartOffsetSeconds,
        std::max(0.0f, capturedSeconds - m_ManualReloadVisualReplayStartOffsetSeconds));
}

void VR::BeginManualReload(C_BasePlayer* localPlayer)
{
    if (!m_ManualReloadEnabled || !m_VrHandsEnabled || !m_Game || m_Game->GetMatQueueMode() != 0 || !localPlayer)
        return;

    C_WeaponCSBase* weapon = reinterpret_cast<C_WeaponCSBase*>(localPlayer->GetActiveWeapon());
    if (!weapon || !ManualReloadWeaponUsesDetachableMagazine(weapon->GetWeaponID()))
        return;

    CancelManualReload();
    m_ManualReloadWeapon = weapon;
    m_ManualReloadWeaponId = static_cast<int>(weapon->GetWeaponID());
    m_ManualReloadMagazineModelName.clear();
    m_ManualReloadMagazineBoneIndex = -1;
    m_ManualReloadMagazineMotionBoneIndex = -1;
    m_ManualReloadViewmodelEntityIndex = -1;
    m_ManualReloadMotionProbeValid = false;
    {
        std::lock_guard<std::mutex> lock(m_ManualReloadSoundMutex);
        m_ManualReloadDelayedSounds.clear();
        m_ManualReloadDelayedSoundReplayIndex = 0;
        m_ManualReloadSoundInsertTailStarted = false;
        m_ManualReloadSoundCaptureStarted = {};
        m_ManualReloadAudioInsertVisualOffsetSeconds = 0.0f;
        m_ManualReloadAudioInsertVisualOffsetValid = false;
    }
    m_ManualReloadSoundReplayStarted = {};
    m_ManualReloadState = ManualReloadState::WatchingNativeClipRemoval;
    m_ManualReloadStarted = std::chrono::steady_clock::now();
    m_ManualReloadResolvedInsertionAxisValid = false;
    m_ManualReloadVisualResumeDurationSeconds = 0.0f;
    m_ManualReloadVisualReplayStartOffsetSeconds = 0.0f;
    m_ManualReloadVisualReplayStartOffsetValid = false;
    m_ManualReloadNativeVisualClipWasAway = false;
    m_ManualReloadNativeVisualClipMaxDistanceMeters = 0.0f;
    m_ManualReloadNativeMotionProbeMaxDistanceMeters = 0.0f;
    m_ManualReloadPostInsertBoundaryWaitStarted = {};
    Game::logMsg(
        "[VR][ManualReload] begin weaponId=%d; scanning current viewmodel for detachable magazine bone",
        static_cast<int>(weapon->GetWeaponID()));
}

void VR::CancelManualReload()
{
    if (m_ManualReloadViewmodelFrozen && m_ManualReloadViewmodelEntity)
    {
        const float playbackRate = (m_ManualReloadOriginalPlaybackRate > 0.0001f) ? m_ManualReloadOriginalPlaybackRate : 1.0f;
        ManualReloadWriteViewmodelAnimation(
            m_ManualReloadViewmodelEntity,
            m_ManualReloadFrozenSequence,
            m_ManualReloadFrozenCycle,
            playbackRate);
    }

    if (m_ManualReloadMouseTestReloadPulseOwned && m_Game)
    {
        m_Game->ClientCmd_Unrestricted("-reload");
        m_ManualReloadMouseTestReloadPulseOwned = false;
    }

    m_ManualReloadState = ManualReloadState::Idle;
    m_ManualReloadWeapon = nullptr;
    m_ManualReloadWeaponId = 0;
    m_ManualReloadViewmodelEntity = nullptr;
    m_ManualReloadViewmodelEntityIndex = -1;
    m_ManualReloadMagazineModelName.clear();
    m_ManualReloadMagazineBoneIndex = -1;
    m_ManualReloadMagazineMotionBoneIndex = -1;
    m_ManualReloadSocketValid = false;
    m_ManualReloadMotionProbeValid = false;
    m_ManualReloadHideNativeClip = false;
    m_ManualReloadMagazineInsertionArmed = false;
    m_ManualReloadFrozenSequence = -1;
    m_ManualReloadFrozenCycle = 0.0f;
    m_ManualReloadOriginalPlaybackRate = 1.0f;
    m_ManualReloadViewmodelFrozen = false;
    m_ManualReloadResolvedInsertionAxisLocal = { 0.0f, -1.0f, 0.0f };
    m_ManualReloadResolvedInsertionAxisValid = false;
    m_ManualReloadVisualResumeDurationSeconds = 0.0f;
    m_ManualReloadVisualReplayStartOffsetSeconds = 0.0f;
    m_ManualReloadVisualReplayStartOffsetValid = false;
    m_ManualReloadTailCaptureComplete = false;
    m_ManualReloadNativeVisualClipWasAway = false;
    m_ManualReloadNativeVisualClipMaxDistanceMeters = 0.0f;
    m_ManualReloadNativeMotionProbeMaxDistanceMeters = 0.0f;
    m_ManualReloadStarted = {};
    m_ManualReloadResumeStarted = {};
    m_ManualReloadPostInsertBoundaryWaitStarted = {};
    {
        std::lock_guard<std::mutex> lock(m_ManualReloadSoundMutex);
        m_ManualReloadDelayedSounds.clear();
        m_ManualReloadDelayedSoundReplayIndex = 0;
        m_ManualReloadSoundInsertTailStarted = false;
        m_ManualReloadSoundCaptureStarted = {};
        m_ManualReloadAudioInsertVisualOffsetSeconds = 0.0f;
        m_ManualReloadAudioInsertVisualOffsetValid = false;
    }
    m_ManualReloadSoundReplayStarted = {};
    m_ManualReloadMouseTestMagazineLocalOffsetMeters = { 0.0f, 0.0f, 0.0f };
    m_ManualReloadMouseTestMagazineLocalRotationOffsetDeg = { 0.0f, 0.0f, 0.0f };
    m_ManualReloadMouseTestLastUpdate = {};
}

bool VR::GetManualReloadMagazineWorld(VrHandMatrix4& outWorld) const
{
    if (!m_ManualReloadEnabled || !m_ManualReloadSocketValid)
        return false;

    if (m_ManualReloadState == ManualReloadState::HoldingFreshMagazine)
    {
        if (m_ManualReloadMouseTestMode && m_MouseModeEnabled)
        {
            const VrHandMatrix4 socketCorrection = ManualReloadBuildLocalTransform(
                m_VRScale,
                1.0f,
                m_ManualReloadMagazineSocketOffsetMeters,
                m_ManualReloadMagazineSocketRotationOffsetDeg,
                false);
            const VrHandMatrix4 local = ManualReloadBuildLocalTransform(
                m_VRScale,
                1.0f,
                m_ManualReloadMouseTestMagazineLocalOffsetMeters,
                m_ManualReloadMouseTestMagazineLocalRotationOffsetDeg,
                true);
            outWorld = VrHandMath::Multiply(
                VrHandMath::Multiply(m_ManualReloadSocketWorld, socketCorrection),
                local);
            return true;
        }

        outWorld = VrHandMath::BuildControllerWorld(
            m_LeftControllerPosAbs,
            m_LeftControllerAngAbs,
            m_VRScale,
            1.0f,
            m_ManualReloadMagazineHandOffsetMeters,
            m_ManualReloadMagazineHandRotationOffsetDeg);
        return true;
    }

    if (m_ManualReloadState == ManualReloadState::ResumingNativeReloadWithMagazine)
    {
        const VrHandMatrix4 local = ManualReloadBuildLocalTransform(
            m_VRScale,
            1.0f,
            m_ManualReloadMagazineSocketOffsetMeters,
            m_ManualReloadMagazineSocketRotationOffsetDeg,
            true);
        outWorld = VrHandMath::Multiply(m_ManualReloadSocketWorld, local);
        return true;
    }

    return false;
}

void VR::UpdateManualReloadMouseTestKeyboard(C_BasePlayer* localPlayer)
{
    auto keyDown = [](int virtualKey) -> bool
        {
            return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
        };

    const bool reloadKeyDown = keyDown(VK_F6);
    const bool grabKeyDown = keyDown(VK_F7);
    const bool dropKeyDown = keyDown(VK_DELETE);
    const bool resetKeyDown = keyDown(VK_HOME);
    const bool cancelKeyDown = keyDown(VK_F9);

    const bool reloadJustPressed = reloadKeyDown && !m_ManualReloadMouseTestReloadKeyDownPrev;
    const bool grabJustPressed = grabKeyDown && !m_ManualReloadMouseTestGrabKeyDownPrev;
    const bool dropJustPressed = dropKeyDown && !m_ManualReloadMouseTestDropKeyDownPrev;
    const bool resetJustPressed = resetKeyDown && !m_ManualReloadMouseTestResetKeyDownPrev;
    const bool cancelJustPressed = cancelKeyDown && !m_ManualReloadMouseTestCancelKeyDownPrev;

    m_ManualReloadMouseTestReloadKeyDownPrev = reloadKeyDown;
    m_ManualReloadMouseTestGrabKeyDownPrev = grabKeyDown;
    m_ManualReloadMouseTestDropKeyDownPrev = dropKeyDown;
    m_ManualReloadMouseTestResetKeyDownPrev = resetKeyDown;
    m_ManualReloadMouseTestCancelKeyDownPrev = cancelKeyDown;

    // Keep the synthetic reload pulse alive for one ProcessInput frame so Source sees
    // a normal button press rather than +reload and -reload in the same command batch.
    if (m_ManualReloadMouseTestReloadPulseOwned && m_Game)
    {
        m_Game->ClientCmd_Unrestricted("-reload");
        m_ManualReloadMouseTestReloadPulseOwned = false;
    }

    if (!m_ManualReloadEnabled || !m_ManualReloadMouseTestMode || !m_MouseModeEnabled)
    {
        m_ManualReloadMouseTestLastUpdate = {};
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    float deltaSeconds = 0.0f;
    if (m_ManualReloadMouseTestLastUpdate.time_since_epoch().count() != 0)
    {
        deltaSeconds = std::chrono::duration<float>(now - m_ManualReloadMouseTestLastUpdate).count();
        deltaSeconds = std::clamp(deltaSeconds, 0.0f, 0.10f);
    }
    m_ManualReloadMouseTestLastUpdate = now;

    if (cancelJustPressed)
    {
        if (IsManualReloadActive())
            Game::logMsg("[VR][ManualReload][MouseTest] canceled by F9");
        CancelManualReload();
        return;
    }

    if (!IsManualReloadActive())
    {
        if (!reloadJustPressed || !localPlayer || !m_Game)
            return;

        BeginManualReload(localPlayer);
        if (!IsManualReloadActive())
        {
            Game::logMsg("[VR][ManualReload][MouseTest] F6 ignored: equip a detachable-magazine firearm and keep single-threaded rendering enabled");
            return;
        }

        m_Game->ClientCmd_Unrestricted("+reload");
        m_ManualReloadMouseTestReloadPulseOwned = true;
        Game::logMsg("[VR][ManualReload][MouseTest] F6 started native reload; after pause press F7, then hold PageDown to push the native Source magazine into the socket");
        Game::logMsg("[VR][ManualReload][MouseTest] controls: F7=grab  Home=reset alignment  arrows=lateral move  PageUp/PageDown=pull/push  numpad 8/2 4/6 7/9=rotate  Delete=drop  F9=cancel");
        return;
    }

    const Vector insertionAxis = ManualReloadNormalize(ManualReloadGetInsertionAxisLocal(this));
    auto resetMagazineToGuideStart = [&]()
        {
            m_ManualReloadMouseTestMagazineLocalOffsetMeters = insertionAxis * m_ManualReloadMagazineGuideStartDepthMeters;
            m_ManualReloadMouseTestMagazineLocalRotationOffsetDeg = { 0.0f, 0.0f, 0.0f };
            m_ManualReloadMagazineInsertionArmed = false;
        };

    if (m_ManualReloadState == ManualReloadState::WaitingForFreshMagazineGrab)
    {
        if (grabJustPressed)
        {
            resetMagazineToGuideStart();
            m_ManualReloadState = ManualReloadState::HoldingFreshMagazine;
            VrHandMatrix4 previewWorld{};
            if (GetManualReloadMagazineWorld(previewWorld))
            {
                const Vector socketWorld = ManualReloadMatrixTranslation(m_ManualReloadSocketWorld);
                const Vector guideWorld = ManualReloadMatrixTranslation(previewWorld);
                Game::logMsg(
                    "[VR][ManualReload][MouseTest] F7 spawned aligned native magazine socket=(%.2f %.2f %.2f) guide=(%.2f %.2f %.2f); hold PageDown to insert",
                    socketWorld.x, socketWorld.y, socketWorld.z,
                    guideWorld.x, guideWorld.y, guideWorld.z);
            }
            else
            {
                Game::logMsg("[VR][ManualReload][MouseTest] F7 spawned aligned native magazine at guide start; hold PageDown to insert");
            }
        }
        return;
    }

    if (m_ManualReloadState != ManualReloadState::HoldingFreshMagazine)
        return;

    if (dropJustPressed)
    {
        m_ManualReloadState = ManualReloadState::WaitingForFreshMagazineGrab;
        m_ManualReloadMagazineInsertionArmed = false;
        Game::logMsg("[VR][ManualReload][MouseTest] native magazine dropped; press F7 to spawn another one");
        return;
    }

    if (resetJustPressed)
    {
        resetMagazineToGuideStart();
        Game::logMsg("[VR][ManualReload][MouseTest] native magazine reset to aligned guide start");
    }

    const float moveStep = 0.12f * deltaSeconds;
    if (keyDown(VK_LEFT))
        m_ManualReloadMouseTestMagazineLocalOffsetMeters.x -= moveStep;
    if (keyDown(VK_RIGHT))
        m_ManualReloadMouseTestMagazineLocalOffsetMeters.x += moveStep;
    if (keyDown(VK_UP))
        m_ManualReloadMouseTestMagazineLocalOffsetMeters.y += moveStep;
    if (keyDown(VK_DOWN))
        m_ManualReloadMouseTestMagazineLocalOffsetMeters.y -= moveStep;
    if (keyDown(VK_PRIOR)) // PageUp: pull away from the socket.
        m_ManualReloadMouseTestMagazineLocalOffsetMeters += insertionAxis * moveStep;
    if (keyDown(VK_NEXT)) // PageDown: push into the socket.
        m_ManualReloadMouseTestMagazineLocalOffsetMeters -= insertionAxis * moveStep;

    const float rotationStep = 90.0f * deltaSeconds;
    if (keyDown(VK_NUMPAD8))
        m_ManualReloadMouseTestMagazineLocalRotationOffsetDeg.x += rotationStep;
    if (keyDown(VK_NUMPAD2))
        m_ManualReloadMouseTestMagazineLocalRotationOffsetDeg.x -= rotationStep;
    if (keyDown(VK_NUMPAD6))
        m_ManualReloadMouseTestMagazineLocalRotationOffsetDeg.y += rotationStep;
    if (keyDown(VK_NUMPAD4))
        m_ManualReloadMouseTestMagazineLocalRotationOffsetDeg.y -= rotationStep;
    if (keyDown(VK_NUMPAD9))
        m_ManualReloadMouseTestMagazineLocalRotationOffsetDeg.z += rotationStep;
    if (keyDown(VK_NUMPAD7))
        m_ManualReloadMouseTestMagazineLocalRotationOffsetDeg.z -= rotationStep;
}

void VR::UpdateManualReload(C_BasePlayer* localPlayer, bool leftGripDown, bool leftGripJustPressed)
{
    if (!IsManualReloadActive())
        return;

    ReplayManualReloadDelayedSounds();

    if (!m_ManualReloadEnabled || !m_VrHandsEnabled || !m_Game || m_Game->GetMatQueueMode() != 0 || !localPlayer)
    {
        CancelManualReload();
        return;
    }

    C_WeaponCSBase* weapon = reinterpret_cast<C_WeaponCSBase*>(localPlayer->GetActiveWeapon());
    if (!weapon || weapon != m_ManualReloadWeapon || !ManualReloadWeaponUsesDetachableMagazine(weapon->GetWeaponID()))
    {
        CancelManualReload();
        return;
    }

    if (m_ManualReloadState == ManualReloadState::WatchingNativeClipRemoval &&
        m_ManualReloadStarted.time_since_epoch().count() != 0 &&
        std::chrono::duration<float>(std::chrono::steady_clock::now() - m_ManualReloadStarted).count() >= 5.0f)
    {
        Game::logMsg("[VR][ManualReload] native clip did not leave weapon; manual reload canceled");
        CancelManualReload();
        return;
    }

    if (m_ManualReloadState == ManualReloadState::WaitingForFreshMagazineGrab)
    {
        if (m_ManualReloadMouseTestMode && m_MouseModeEnabled)
            return;

        Vector bodyForward = m_HmdForward;
        bodyForward.z = 0.0f;
        bodyForward = ManualReloadNormalize(bodyForward);
        const Vector worldUp(0.0f, 0.0f, 1.0f);
        Vector bodyRight(
            bodyForward.y * worldUp.z - bodyForward.z * worldUp.y,
            bodyForward.z * worldUp.x - bodyForward.x * worldUp.z,
            bodyForward.x * worldUp.y - bodyForward.y * worldUp.x);
        bodyRight = ManualReloadNormalize(bodyRight);
        const Vector bodyOrigin = m_CameraAnchor
            + bodyForward * (m_InventoryBodyOriginOffset.x * m_VRScale)
            + bodyRight * (m_InventoryBodyOriginOffset.y * m_VRScale)
            + worldUp * (m_InventoryBodyOriginOffset.z * m_VRScale);
        const Vector pouch = bodyOrigin
            + bodyForward * (m_InventoryLeftWaistOffset.x * m_VRScale)
            + bodyRight * (m_InventoryLeftWaistOffset.y * m_VRScale)
            + worldUp * (m_InventoryLeftWaistOffset.z * m_VRScale);

        if (leftGripJustPressed && ManualReloadVectorLength(m_LeftControllerPosAbs - pouch) <= m_ManualReloadMagazineGrabRangeMeters * m_VRScale)
        {
            m_ManualReloadState = ManualReloadState::HoldingFreshMagazine;
            m_ManualReloadMagazineInsertionArmed = false;
            Game::logMsg("[VR][ManualReload] fresh native magazine grabbed from left waist");
        }
        return;
    }

    if (m_ManualReloadState != ManualReloadState::HoldingFreshMagazine)
        return;

    if (!leftGripDown)
    {
        m_ManualReloadState = ManualReloadState::WaitingForFreshMagazineGrab;
        m_ManualReloadMagazineInsertionArmed = false;
        return;
    }

    VrHandMatrix4 heldWorld{};
    if (!GetManualReloadMagazineWorld(heldWorld))
        return;

    const VrHandMatrix4 socketCorrection = ManualReloadBuildLocalTransform(
        m_VRScale,
        1.0f,
        m_ManualReloadMagazineSocketOffsetMeters,
        m_ManualReloadMagazineSocketRotationOffsetDeg,
        false);
    const VrHandMatrix4 socketRigid = VrHandMath::Multiply(m_ManualReloadSocketWorld, socketCorrection);
    VrHandMatrix4 inverseSocket{};
    if (!VrHandMath::Invert4x4(socketRigid, inverseSocket))
        return;

    const VrHandMatrix4 heldRigid = ManualReloadStripScale(heldWorld);
    const VrHandMatrix4 magazineInSocket = VrHandMath::Multiply(inverseSocket, heldRigid);
    const Vector localPositionMeters = ManualReloadMatrixTranslation(magazineInSocket) * (1.0f / std::max(0.001f, m_VRScale));
    const Vector insertionAxis = ManualReloadNormalize(ManualReloadGetInsertionAxisLocal(this));
    const float axial = VrHandMath::Dot(localPositionMeters, insertionAxis);
    const Vector lateralVector = localPositionMeters - insertionAxis * axial;
    const float lateral = ManualReloadVectorLength(lateralVector);

    const Vector heldAxis = ManualReloadNormalize(ManualReloadTransformDirection(magazineInSocket, insertionAxis));
    const float dot = std::clamp(VrHandMath::Dot(heldAxis, insertionAxis), -1.0f, 1.0f);
    const float angleDeg = std::acos(dot) * (180.0f / 3.14159265358979323846f);
    const bool aligned = lateral <= m_ManualReloadMagazineCaptureRadiusMeters &&
        angleDeg <= m_ManualReloadMagazineCaptureAngleDeg;

    if (aligned && axial >= m_ManualReloadMagazineFullInsertDepthMeters && axial <= m_ManualReloadMagazineGuideStartDepthMeters)
        m_ManualReloadMagazineInsertionArmed = true;

    if (m_ManualReloadMagazineInsertionArmed && aligned &&
        axial <= m_ManualReloadMagazineFullInsertDepthMeters && axial >= -0.02f)
    {
        m_ManualReloadMagazineInsertionArmed = false;
        // The detached Source-material copy disappears immediately. Restore the native magazine
        // at the socket now, but do not jump to idle when the hidden animation has not reached
        // its post-insert boundary yet. In that case the visible gun remains frozen briefly while
        // DrawModelExecute keeps sampling the hidden Source tail.
        m_ManualReloadHideNativeClip = false;
        m_ManualReloadViewmodelFrozen = false;

        m_ManualReloadState = ManualReloadState::AwaitingNativePostInsertBoundary;
        m_ManualReloadPostInsertBoundaryWaitStarted = std::chrono::steady_clock::now();
        if (!TryStartManualReloadPostInsertReplay("captured-boundary-ready-at-player-insert"))
        {
            const float capturedSeconds = std::max(0.0f, m_ManualReloadVisualResumeDurationSeconds);
            const float replayOffsetSeconds = m_ManualReloadVisualReplayStartOffsetValid
                ? std::max(0.0f, m_ManualReloadVisualReplayStartOffsetSeconds)
                : 0.0f;
            Game::logMsg(
                "[VR][ManualReload] player inserted magazine; detached copy removed and native magazine restored at socket; waiting for hidden post-insert tail samples captured=%.3fs offset=%.3fs remaining=%.3fs",
                capturedSeconds,
                replayOffsetSeconds,
                std::max(0.0f, capturedSeconds - replayOffsetSeconds));
        }
    }
}

void VR::OnManualReloadViewmodelPose(
    const char* modelName,
    void* viewmodelEntity,
    int viewmodelEntityIndex,
    const VrHandMatrix4& modelAnchorWorld,
    const VrHandMatrix4& nativeClipWorld,
    const VrHandMatrix4& nativeMotionProbeWorld)
{
    (void)modelName;
    if (!IsManualReloadActive() || !viewmodelEntity)
        return;

    m_ManualReloadViewmodelEntity = viewmodelEntity;
    if (viewmodelEntityIndex > 0)
        m_ManualReloadViewmodelEntityIndex = viewmodelEntityIndex;
    VrHandMatrix4 inverseModelAnchor{};
    if (!VrHandMath::Invert4x4(modelAnchorWorld, inverseModelAnchor))
        return;
    const VrHandMatrix4 currentClipLocal = VrHandMath::Multiply(inverseModelAnchor, nativeClipWorld);
    const VrHandMatrix4 currentMotionProbeLocal = VrHandMath::Multiply(inverseModelAnchor, nativeMotionProbeWorld);

    if (!m_ManualReloadSocketValid)
    {
        m_ManualReloadSocketLocal = currentClipLocal;
        m_ManualReloadSocketWorld = VrHandMath::Multiply(modelAnchorWorld, m_ManualReloadSocketLocal);
        m_ManualReloadSocketValid = true;
    }
    else
    {
        m_ManualReloadSocketWorld = VrHandMath::Multiply(modelAnchorWorld, m_ManualReloadSocketLocal);
    }

    if (!m_ManualReloadMotionProbeValid)
    {
        m_ManualReloadMotionProbeLocal = currentMotionProbeLocal;
        m_ManualReloadMotionProbeValid = true;
    }

    int sequence = -1;
    float cycle = 0.0f;
    float playbackRate = 1.0f;
    if (!ManualReloadReadViewmodelAnimation(viewmodelEntity, sequence, cycle, playbackRate))
        return;

    if (m_ManualReloadState == ManualReloadState::WatchingNativeClipRemoval)
    {
        const Vector currentProbe = ManualReloadMatrixTranslation(currentMotionProbeLocal);
        const Vector initialProbe = ManualReloadMatrixTranslation(m_ManualReloadMotionProbeLocal);
        const float movedMeters = ManualReloadVectorLength(currentProbe - initialProbe) / std::max(0.001f, m_VRScale);
        if (movedMeters >= m_ManualReloadNativeClipLeaveDistanceMeters)
        {
            const Vector outwardModelLocal = currentProbe - initialProbe;
            if (ManualReloadVectorLength(outwardModelLocal) > 0.0001f)
            {
                VrHandMatrix4 inverseSocketLocal{};
                if (VrHandMath::Invert4x4(ManualReloadStripScale(m_ManualReloadSocketLocal), inverseSocketLocal))
                {
                    m_ManualReloadResolvedInsertionAxisLocal = ManualReloadNormalize(
                        ManualReloadTransformDirection(inverseSocketLocal, outwardModelLocal));
                    m_ManualReloadResolvedInsertionAxisValid = true;
                }
            }
            m_ManualReloadVisualResumeDurationSeconds = 0.0f;
            m_ManualReloadVisualReplayStartOffsetSeconds = 0.0f;
            m_ManualReloadVisualReplayStartOffsetValid = false;
            m_ManualReloadTailCaptureComplete = false;
            const Vector currentVisualClip = ManualReloadMatrixTranslation(currentClipLocal);
            const Vector initialVisualClip = ManualReloadMatrixTranslation(m_ManualReloadSocketLocal);
            const float visualClipMovedMeters = ManualReloadVectorLength(currentVisualClip - initialVisualClip) /
                std::max(0.001f, m_VRScale);
            const float visualAwayThresholdMeters = std::clamp(
                m_ManualReloadNativeClipLeaveDistanceMeters * 0.50f,
                0.012f,
                0.040f);
            m_ManualReloadNativeVisualClipMaxDistanceMeters = visualClipMovedMeters;
            m_ManualReloadNativeVisualClipWasAway = visualClipMovedMeters >= visualAwayThresholdMeters;
            m_ManualReloadNativeMotionProbeMaxDistanceMeters = movedMeters;
            m_ManualReloadPostInsertBoundaryWaitStarted = {};
            {
                std::lock_guard<std::mutex> lock(m_ManualReloadSoundMutex);
                m_ManualReloadDelayedSounds.clear();
                m_ManualReloadDelayedSoundReplayIndex = 0;
                m_ManualReloadSoundInsertTailStarted = false;
                m_ManualReloadSoundCaptureStarted = {};
                m_ManualReloadAudioInsertVisualOffsetSeconds = 0.0f;
                m_ManualReloadAudioInsertVisualOffsetValid = false;
            }
            m_ManualReloadSoundReplayStarted = {};
            m_ManualReloadFrozenSequence = sequence;
            m_ManualReloadFrozenCycle = cycle;
            m_ManualReloadOriginalPlaybackRate = playbackRate;
            // Do not freeze Source's internal animation state. DrawModelExecute freezes only the visible
            // submitted bones while the hidden native sequence keeps progressing so its tail can be sampled.
            m_ManualReloadViewmodelFrozen = false;
            m_ManualReloadHideNativeClip = true;
            m_ManualReloadState = ManualReloadState::WaitingForFreshMagazineGrab;
            const Vector resolvedAxis = ManualReloadGetInsertionAxisLocal(this);
            Game::logMsg(
                "[VR][ManualReload] native clip left weapon %.3fm; animation paused; inferred insertion axis=(%.3f %.3f %.3f)",
                movedMeters,
                resolvedAxis.x,
                resolvedAxis.y,
                resolvedAxis.z);
        }
        return;
    }

    if (m_ManualReloadState == ManualReloadState::WaitingForFreshMagazineGrab ||
        m_ManualReloadState == ManualReloadState::HoldingFreshMagazine ||
        m_ManualReloadState == ManualReloadState::AwaitingNativePostInsertBoundary)
    {
        const Vector currentProbe = ManualReloadMatrixTranslation(currentMotionProbeLocal);
        const Vector initialProbe = ManualReloadMatrixTranslation(m_ManualReloadMotionProbeLocal);
        const float motionMovedMeters = ManualReloadVectorLength(currentProbe - initialProbe) /
            std::max(0.001f, m_VRScale);
        m_ManualReloadNativeMotionProbeMaxDistanceMeters = std::max(
            m_ManualReloadNativeMotionProbeMaxDistanceMeters,
            motionMovedMeters);

        const Vector currentVisualClip = ManualReloadMatrixTranslation(currentClipLocal);
        const Vector initialVisualClip = ManualReloadMatrixTranslation(m_ManualReloadSocketLocal);
        const float visualClipMovedMeters = ManualReloadVectorLength(currentVisualClip - initialVisualClip) /
            std::max(0.001f, m_VRScale);
        m_ManualReloadNativeVisualClipMaxDistanceMeters = std::max(
            m_ManualReloadNativeVisualClipMaxDistanceMeters,
            visualClipMovedMeters);

        const float visualAwayThresholdMeters = std::clamp(
            m_ManualReloadNativeClipLeaveDistanceMeters * 0.50f,
            0.012f,
            0.040f);
        const float visualReturnedThresholdMeters = std::clamp(
            m_ManualReloadNativeClipLeaveDistanceMeters * 0.35f,
            0.006f,
            0.018f);
        if (visualClipMovedMeters >= visualAwayThresholdMeters)
            m_ManualReloadNativeVisualClipWasAway = true;

        if (!m_ManualReloadVisualReplayStartOffsetValid &&
            m_ManualReloadFrozenSequence >= 0 &&
            m_ManualReloadNativeVisualClipWasAway &&
            visualClipMovedMeters <= visualReturnedThresholdMeters)
        {
            m_ManualReloadVisualReplayStartOffsetSeconds =
                std::max(0.0f, m_ManualReloadVisualResumeDurationSeconds + (1.0f / 90.0f));
            m_ManualReloadVisualReplayStartOffsetValid = true;
            Game::logMsg(
                "[VR][ManualReload] hidden native visual magazine reinsertion boundary captured offset=%.3fs visualDistance=%.3fm motionDistance=%.3fm",
                m_ManualReloadVisualReplayStartOffsetSeconds,
                visualClipMovedMeters,
                motionMovedMeters);
        }

        if (!m_ManualReloadTailCaptureComplete &&
            m_ManualReloadFrozenSequence >= 0 &&
            sequence != m_ManualReloadFrozenSequence)
        {
            m_ManualReloadTailCaptureComplete = true;
            Game::logMsg(
                "[VR][ManualReload] hidden native reload tail capture finished duration=%.3fs sequence=%d->%d",
                m_ManualReloadVisualResumeDurationSeconds,
                m_ManualReloadFrozenSequence,
                sequence);
        }

        if (m_ManualReloadState == ManualReloadState::AwaitingNativePostInsertBoundary)
        {
            if (!TryStartManualReloadPostInsertReplay("visual-native-reinsertion-boundary"))
            {
                const float waitingSeconds =
                    (m_ManualReloadPostInsertBoundaryWaitStarted.time_since_epoch().count() != 0)
                    ? std::chrono::duration<float>(std::chrono::steady_clock::now() - m_ManualReloadPostInsertBoundaryWaitStarted).count()
                    : 0.0f;
                if (!m_ManualReloadVisualReplayStartOffsetValid &&
                    (m_ManualReloadTailCaptureComplete ||
                        waitingSeconds >= kManualReloadPostInsertBoundaryWaitTimeoutSeconds))
                {
                    UseManualReloadPostInsertFallbackBoundary(
                        m_ManualReloadTailCaptureComplete ? "tail-finished-without-visual-boundary" : "boundary-wait-timeout");
                    TryStartManualReloadPostInsertReplay("fallback-preserved-tail");
                }
            }
        }
        return;
    }

    if (m_ManualReloadState == ManualReloadState::ResumingNativeReloadWithMagazine)
    {
        const auto now = std::chrono::steady_clock::now();
        const float resumedSeconds = std::chrono::duration<float>(now - m_ManualReloadResumeStarted).count();
        ReplayManualReloadDelayedSounds();
        const float replayDurationSeconds = GetManualReloadReplayDurationSeconds();
        if (resumedSeconds >= replayDurationSeconds)
        {
            m_ManualReloadHideNativeClip = false;
            m_ManualReloadState = ManualReloadState::Idle;
            m_ManualReloadWeapon = nullptr;
            m_ManualReloadWeaponId = 0;
            m_ManualReloadViewmodelEntity = nullptr;
            m_ManualReloadViewmodelEntityIndex = -1;
            m_ManualReloadMagazineModelName.clear();
            m_ManualReloadMagazineBoneIndex = -1;
            m_ManualReloadMagazineMotionBoneIndex = -1;
            m_ManualReloadSocketValid = false;
            m_ManualReloadMotionProbeValid = false;
            m_ManualReloadMagazineInsertionArmed = false;
            m_ManualReloadFrozenSequence = -1;
            m_ManualReloadFrozenCycle = 0.0f;
            m_ManualReloadOriginalPlaybackRate = 1.0f;
            m_ManualReloadViewmodelFrozen = false;
            m_ManualReloadResolvedInsertionAxisValid = false;
            m_ManualReloadVisualResumeDurationSeconds = 0.0f;
            m_ManualReloadVisualReplayStartOffsetSeconds = 0.0f;
            m_ManualReloadVisualReplayStartOffsetValid = false;
            m_ManualReloadTailCaptureComplete = false;
            m_ManualReloadNativeVisualClipWasAway = false;
            m_ManualReloadNativeVisualClipMaxDistanceMeters = 0.0f;
            m_ManualReloadNativeMotionProbeMaxDistanceMeters = 0.0f;
            m_ManualReloadStarted = {};
            m_ManualReloadResumeStarted = {};
            m_ManualReloadPostInsertBoundaryWaitStarted = {};
            {
                std::lock_guard<std::mutex> lock(m_ManualReloadSoundMutex);
                m_ManualReloadDelayedSounds.clear();
                m_ManualReloadDelayedSoundReplayIndex = 0;
                m_ManualReloadSoundInsertTailStarted = false;
                m_ManualReloadSoundCaptureStarted = {};
                m_ManualReloadAudioInsertVisualOffsetSeconds = 0.0f;
                m_ManualReloadAudioInsertVisualOffsetValid = false;
            }
            m_ManualReloadSoundReplayStarted = {};
            Game::logMsg("[VR][ManualReload] captured reload tail finished; native viewmodel returned to live idle pose");
        }
    }
}
