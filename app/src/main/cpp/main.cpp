#include <android/log.h>
#include <android_native_app_glue.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

constexpr float kPi = 3.1415926535f;

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

const std::array<Vec3, 6> kOutpostPositions{{
    {-125, 0, -82}, {-62, 0, 116}, {74, 0, -126},
    {128, 0, 48}, {-142, 0, 66}, {38, 0, 112}
}};

Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float length(Vec3 a) { return std::sqrt(dot(a, a)); }
Vec3 normalized(Vec3 a) {
    const float magnitude = length(a);
    return magnitude > 0.0001f ? a * (1.0f / magnitude) : Vec3{0, 0, -1};
}
Vec3 lookDirection(float yaw, float pitch) {
    const float horizontal = std::cos(pitch);
    return {std::sin(yaw) * horizontal, std::sin(pitch), -std::cos(yaw) * horizontal};
}

struct Mat4 {
    float v[16]{};
};

Mat4 identity() {
    Mat4 result{};
    result.v[0] = result.v[5] = result.v[10] = result.v[15] = 1;
    return result;
}

Mat4 multiply(const Mat4& a, const Mat4& b) {
    Mat4 result{};
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            for (int i = 0; i < 4; ++i) {
                result.v[column * 4 + row] += a.v[i * 4 + row] * b.v[column * 4 + i];
            }
        }
    }
    return result;
}

Mat4 translation(Vec3 p) {
    Mat4 result = identity();
    result.v[12] = p.x;
    result.v[13] = p.y;
    result.v[14] = p.z;
    return result;
}

Mat4 scaling(Vec3 s) {
    Mat4 result{};
    result.v[0] = s.x;
    result.v[5] = s.y;
    result.v[10] = s.z;
    result.v[15] = 1;
    return result;
}

Mat4 rotationY(float angle) {
    Mat4 result = identity();
    result.v[0] = std::cos(angle);
    result.v[2] = std::sin(angle);
    result.v[8] = -std::sin(angle);
    result.v[10] = std::cos(angle);
    return result;
}

Mat4 rotationX(float angle) {
    Mat4 result = identity();
    result.v[5] = std::cos(angle);
    result.v[6] = std::sin(angle);
    result.v[9] = -std::sin(angle);
    result.v[10] = std::cos(angle);
    return result;
}

Mat4 perspective(float fov, float aspect, float nearPlane, float farPlane) {
    Mat4 result{};
    const float f = 1.0f / std::tan(fov * 0.5f);
    result.v[0] = f / aspect;
    result.v[5] = f;
    result.v[10] = (farPlane + nearPlane) / (nearPlane - farPlane);
    result.v[11] = -1;
    result.v[14] = (2 * farPlane * nearPlane) / (nearPlane - farPlane);
    return result;
}

Mat4 lookAt(Vec3 eye, Vec3 target, Vec3 up) {
    const Vec3 forward = normalized(target - eye);
    const Vec3 side = normalized({forward.y * up.z - forward.z * up.y,
                                  forward.z * up.x - forward.x * up.z,
                                  forward.x * up.y - forward.y * up.x});
    const Vec3 correctedUp{side.y * forward.z - side.z * forward.y,
                           side.z * forward.x - side.x * forward.z,
                           side.x * forward.y - side.y * forward.x};
    Mat4 result = identity();
    result.v[0] = side.x; result.v[4] = side.y; result.v[8] = side.z;
    result.v[1] = correctedUp.x; result.v[5] = correctedUp.y; result.v[9] = correctedUp.z;
    result.v[2] = -forward.x; result.v[6] = -forward.y; result.v[10] = -forward.z;
    result.v[12] = -dot(side, eye);
    result.v[13] = -dot(correctedUp, eye);
    result.v[14] = dot(forward, eye);
    return result;
}

struct Vertex {
    float x, y, z;
    float r, g, b;
};

struct Bot {
    Vec3 position;
    Vec3 velocity;
    float health = 100;
    float shotTimer = 1;
    float strafeTimer = 0;
    float strafeDirection = 1;
    int appearance = 0;
    bool alive = true;
    bool isActive = true; // Added to track if the bot is active
};

enum class LootType { Ammo, Medkit };

struct LootDrop {
    Vec3 position;
    Vec3 velocity;
    LootType type = LootType::Ammo;
    bool active = false;
    bool collected = false;
    bool airborne = false;
};

struct Projectile {
    Vec3 position;
    Vec3 velocity;
    float damage = 0;
    float lifetime = 0;
    bool playerOwned = false;
    bool active = false;
};

struct WeaponStats {
    const char* name;
    const char* role;
    int magazineSize;
    int reserveAmmo;
    float damage;
    float fireInterval;
    float projectileSpeed;
    float hipSpread;
    float adsSpread;
    float recoil;
    float reloadSeconds;
};

enum HudControl : size_t { MoveHud, CrouchHud, AimHud, JumpHud, MedHud, ReloadHud, FireHud, HudControlCount };

struct HudAnchor {
    float x;
    float y;
};

const std::array<WeaponStats, 3> kWeaponStats{{
    {"RIFLE", "BALANCED", 30, 120, 35, 0.18f, 78, 0.015f, 0.004f, 0.035f, 1.35f},
    {"SMG", "FAST FIRE", 40, 160, 20, 0.095f, 68, 0.027f, 0.012f, 0.022f, 1.2f},
    {"SNIPER", "LONG RANGE", 5, 40, 100, 0.72f, 140, 0.006f, 0.0015f, 0.11f, 1.9f}
}};

const WeaponStats& weaponStats(int selection) {
    return kWeaponStats[std::clamp(selection, 0, static_cast<int>(kWeaponStats.size()) - 1)];
}

enum class SoundEffect {
    PlayerShot,
    EnemyShot,
    Hit,
    Reload,
    Pickup,
    Heal,
    Aim,
    Damage,
    Storm,
    Footstep,
    Jump,
    Elimination,
    Victory,
    Defeat
};

class AudioSystem {
public:
    ~AudioSystem() { shutdown(); }

    bool initialize() {
        if (slCreateEngine(&engineObject_, 0, nullptr, 0, nullptr, nullptr) != SL_RESULT_SUCCESS ||
            (*engineObject_)->Realize(engineObject_, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS ||
            (*engineObject_)->GetInterface(engineObject_, SL_IID_ENGINE, &engine_) != SL_RESULT_SUCCESS) {
            shutdown();
            return false;
        }
        if ((*engine_)->CreateOutputMix(engine_, &outputMix_, 0, nullptr, nullptr) != SL_RESULT_SUCCESS ||
            (*outputMix_)->Realize(outputMix_, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) {
            shutdown();
            return false;
        }

        for (Channel& channel : channels_) {
            channel.samples.resize(kMaxSamples);
            SLDataLocator_AndroidSimpleBufferQueue queueLocator{
                SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 1
            };
            SLDataFormat_PCM format{
                SL_DATAFORMAT_PCM, 1, SL_SAMPLINGRATE_22_05,
                SL_PCMSAMPLEFORMAT_FIXED_16, SL_PCMSAMPLEFORMAT_FIXED_16,
                SL_SPEAKER_FRONT_CENTER, SL_BYTEORDER_LITTLEENDIAN
            };
            SLDataSource source{&queueLocator, &format};
            SLDataLocator_OutputMix outputLocator{SL_DATALOCATOR_OUTPUTMIX, outputMix_};
            SLDataSink sink{&outputLocator, nullptr};
            SLInterfaceID interfaceIds[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE};
            SLboolean required[] = {SL_BOOLEAN_TRUE};
            if ((*engine_)->CreateAudioPlayer(engine_, &channel.player, &source, &sink, 1,
                                              interfaceIds, required) != SL_RESULT_SUCCESS ||
                (*channel.player)->Realize(channel.player, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS ||
                (*channel.player)->GetInterface(channel.player, SL_IID_PLAY, &channel.play) !=
                    SL_RESULT_SUCCESS ||
                (*channel.player)->GetInterface(channel.player, SL_IID_ANDROIDSIMPLEBUFFERQUEUE,
                                                &channel.queue) != SL_RESULT_SUCCESS) {
                shutdown();
                return false;
            }
        }
        initialized_ = true;
        return true;
    }

    void shutdown() {
        initialized_ = false;
        for (Channel& channel : channels_) {
            if (channel.player) (*channel.player)->Destroy(channel.player);
            channel.player = nullptr;
            channel.play = nullptr;
            channel.queue = nullptr;
        }
        if (outputMix_) (*outputMix_)->Destroy(outputMix_);
        outputMix_ = nullptr;
        if (engineObject_) (*engineObject_)->Destroy(engineObject_);
        engineObject_ = nullptr;
        engine_ = nullptr;
    }

    void play(SoundEffect effect) {
        if (!initialized_) return;
        const int channelIndex = channelFor(effect);
        Channel& channel = channels_[channelIndex];
        const SoundProfile sound = profileFor(effect);
        const size_t count = std::min(kMaxSamples,
            static_cast<size_t>(sound.duration * kSampleRate));
        if (count == 0) return;

        (*channel.play)->SetPlayState(channel.play, SL_PLAYSTATE_STOPPED);
        (*channel.queue)->Clear(channel.queue);
        uint32_t noiseState = 0x9e3779b9u + static_cast<uint32_t>(effect) * 7919u;
        for (size_t i = 0; i < count; ++i) {
            const float time = static_cast<float>(i) / kSampleRate;
            const float progress = time / sound.duration;
            const float frequency = sound.startHz + (sound.endHz - sound.startHz) * progress;
            const float phase = 2 * kPi * (sound.startHz * time +
                0.5f * (sound.endHz - sound.startHz) * time * progress);
            noiseState = noiseState * 1664525u + 1013904223u;
            const float noise = static_cast<float>((noiseState >> 16) & 0x7fff) / 16384.0f - 1.0f;
            float envelope = std::exp(-sound.decay * time) * std::clamp(1.0f - progress, 0.0f, 1.0f);
            float sample = std::sin(phase) * (1.0f - sound.noiseMix) + noise * sound.noiseMix;
            if (effect == SoundEffect::Reload) {
                const float click = std::fmod(time, 0.15f) < 0.018f ? 0.65f : 0.0f;
                sample = sample * 0.2f + click;
            } else if (effect == SoundEffect::Pickup || effect == SoundEffect::Heal ||
                       effect == SoundEffect::Victory) {
                const float note = effect == SoundEffect::Victory
                    ? (progress < 0.34f ? 523.0f : (progress < 0.67f ? 659.0f : 784.0f))
                    : frequency * 1.25f;
                sample = 0.65f * std::sin(2 * kPi * note * time) + 0.2f * std::sin(phase);
            }
            channel.samples[i] = static_cast<int16_t>(
                std::clamp(sample * envelope * sound.gain, -1.0f, 1.0f) * 32767.0f);
        }

        (*channel.queue)->Enqueue(channel.queue, channel.samples.data(),
                                  static_cast<SLuint32>(count * sizeof(int16_t)));
        (*channel.play)->SetPlayState(channel.play, SL_PLAYSTATE_PLAYING);
    }

private:
    static constexpr SLuint32 kSampleRate = 22050;
    static constexpr size_t kMaxSamples = 16537;
    struct Channel {
        SLObjectItf player = nullptr;
        SLPlayItf play = nullptr;
        SLAndroidSimpleBufferQueueItf queue = nullptr;
        std::vector<int16_t> samples;
    };
    struct SoundProfile {
        float duration;
        float startHz;
        float endHz;
        float gain;
        float noiseMix;
        float decay;
    };

    SLObjectItf engineObject_ = nullptr;
    SLEngineItf engine_ = nullptr;
    SLObjectItf outputMix_ = nullptr;
    std::array<Channel, 5> channels_{};
    bool initialized_ = false;

    static int channelFor(SoundEffect effect) {
        switch (effect) {
            case SoundEffect::PlayerShot: return 0;
            case SoundEffect::EnemyShot: return 1;
            case SoundEffect::Footstep: return 2;
            case SoundEffect::Hit:
            case SoundEffect::Damage:
            case SoundEffect::Storm:
            case SoundEffect::Elimination:
            case SoundEffect::Victory:
            case SoundEffect::Defeat: return 4;
            default: return 3;
        }
    }

    static SoundProfile profileFor(SoundEffect effect) {
        switch (effect) {
            case SoundEffect::PlayerShot: return {0.16f, 150, 48, 0.8f, 0.62f, 16};
            case SoundEffect::EnemyShot: return {0.14f, 110, 42, 0.58f, 0.65f, 17};
            case SoundEffect::Hit: return {0.11f, 720, 260, 0.5f, 0.12f, 20};
            case SoundEffect::Reload: return {0.42f, 170, 90, 0.35f, 0.35f, 5};
            case SoundEffect::Pickup: return {0.24f, 580, 920, 0.35f, 0.02f, 3};
            case SoundEffect::Heal: return {0.48f, 360, 860, 0.33f, 0.01f, 2};
            case SoundEffect::Aim: return {0.1f, 920, 560, 0.2f, 0.08f, 22};
            case SoundEffect::Damage: return {0.2f, 180, 62, 0.5f, 0.3f, 13};
            case SoundEffect::Storm: return {0.3f, 120, 55, 0.36f, 0.28f, 6};
            case SoundEffect::Footstep: return {0.085f, 95, 48, 0.18f, 0.72f, 24};
            case SoundEffect::Jump: return {0.16f, 180, 420, 0.2f, 0.06f, 8};
            case SoundEffect::Elimination: return {0.2f, 460, 780, 0.36f, 0.03f, 4};
            case SoundEffect::Victory: return {0.62f, 523, 784, 0.28f, 0.01f, 1.2f};
            case SoundEffect::Defeat: return {0.5f, 310, 72, 0.3f, 0.03f, 3};
        }
        return {0.1f, 300, 180, 0.2f, 0.1f, 5};
    }
};

struct Game {
    AudioSystem audio;
    Vec3 player{0, 0, 0};
    Vec3 playerVelocity{};
    Vec3 aim{0, 0, -1};
    float yaw = 0;
    float pitch = 0;
    float targetYaw = 0;
    float targetPitch = 0;
    float aimSensitivity = 1.0f;
    std::array<HudAnchor, HudControlCount> hudAnchors{{
        {0.12f, 0.78f}, {0.64f, 0.55f}, {0.77f, 0.55f}, {0.92f, 0.43f},
        {0.61f, 0.9f}, {0.76f, 0.9f}, {0.91f, 0.9f}
    }};
    std::array<Bot, 8> bots{};
    std::array<LootDrop, 20> loot{};
    std::array<Projectile, 64> projectiles{};
    float health = 100;
    Vec3 zoneCenter{0, 0, 0};
    float zoneRadius = 160;
    float fireCooldown = 0;
    float footstepTimer = 0;
    float hitMarkerTimer = 0;
    float recoil = 0;
    float reloadTimer = 0;
    float healTimer = 0;
    float stormTimer = 0;
    float roundTime = 0;
    int selectedWeapon = 0;
    int selectedMode = 0;
    int selectedSkin = 0;
    int selectedStoreSkin = 1;
    int coins = 300;
    std::array<bool, 4> ownedSkins{{true, false, false, false}};
    int magazine = 30;
    int reserveAmmo = 120;
    int medkits = 1;
    int kills = 0;
    int width = 1;
    int height = 1;
    int movePointer = -1;
    int aimPointer = -1;
    int firePointer = -1;
    int healPointer = -1;
    int aimAssistTarget = -1;
    int hudDragPointer = -1;
    int draggedHudControl = -1;
    int sensitivityPointer = -1;
    float moveX = 0;
    float moveY = 0;
    float moveOriginX = 0;
    float moveOriginY = 0;
    float aimStartX = 0;
    float aimStartY = 0;
    bool reloading = false;
    bool healing = false;
    bool aiming = false;
    bool crouching = false;
    bool grounded = true;
    bool started = false;
    bool modeSelectOpen = false;
    bool loadoutOpen = false;
    bool skinSelectOpen = false;
    bool storeOpen = false;
    bool settingsOpen = false;
    bool hudEditMode = false;
    bool gameOver = false;
    bool won = false;
};

std::array<float, 3> playerSkinColor(int skin) {
    static const std::array<std::array<float, 3>, 4> colors{{
        {0.13f, 0.32f, 0.78f}, {0.2f, 0.52f, 0.31f},
        {0.82f, 0.3f, 0.16f}, {0.32f, 0.62f, 0.72f}
    }};
    return colors[std::clamp(skin, 0, static_cast<int>(colors.size()) - 1)];
}

float playerEyeHeight(const Game& game) {
    return game.crouching ? 1.2f : 1.62f;
}

int hitHudControl(const Game& game, float x, float y, float width, float height) {
    int closest = -1;
    float bestDistance = 1.7f;
    for (size_t i = 0; i < HudControlCount; ++i) {
        const HudAnchor& anchor = game.hudAnchors[i];
        const float radiusX = i == MoveHud ? width * 0.07f
                                : (i == FireHud ? width * 0.075f : width * 0.055f);
        const float radiusY = i == MoveHud || i == FireHud ? height * 0.15f : height * 0.105f;
        const float dx = (x - anchor.x * width) / radiusX;
        const float dy = (y - anchor.y * height) / radiusY;
        const float distance = dx * dx + dy * dy;
        if (distance < bestDistance) {
            bestDistance = distance;
            closest = static_cast<int>(i);
        }
    }
    return closest;
}

void setAimSensitivity(Game& game, float x, float width) {
    const float fraction = std::clamp((x / width - 0.25f) / 0.5f, 0.0f, 1.0f);
    game.aimSensitivity = 0.35f + fraction * 2.05f;
}

void selectWeapon(Game& game, int selection) {
    game.selectedWeapon = std::clamp(selection, 0, static_cast<int>(kWeaponStats.size()) - 1);
    const WeaponStats& weapon = weaponStats(game.selectedWeapon);
    game.magazine = weapon.magazineSize;
    game.reserveAmmo = weapon.reserveAmmo;
    game.reloading = false;
}

class Renderer {
public:
    bool initialize(ANativeWindow* window) {
        const EGLint configAttributes[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
            EGL_DEPTH_SIZE, 16, EGL_NONE
        };
        display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (display_ == EGL_NO_DISPLAY || !eglInitialize(display_, nullptr, nullptr)) return false;
        EGLConfig config{};
        EGLint count = 0;
        eglChooseConfig(display_, configAttributes, &config, 1, &count);
        const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
        context_ = eglCreateContext(display_, config, EGL_NO_CONTEXT, contextAttributes);
        surface_ = eglCreateWindowSurface(display_, config, window, nullptr);
        if (context_ == EGL_NO_CONTEXT || surface_ == EGL_NO_SURFACE ||
            !eglMakeCurrent(display_, surface_, surface_, context_)) return false;
        width_ = ANativeWindow_getWidth(window);
        height_ = ANativeWindow_getHeight(window);
        if (!createProgram()) return false;
        glGenBuffers(1, &buffer_);
        glEnable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glViewport(0, 0, width_, height_);
        return true;
    }

    void shutdown() {
        if (display_ != EGL_NO_DISPLAY) {
            eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (surface_ != EGL_NO_SURFACE) eglDestroySurface(display_, surface_);
            if (context_ != EGL_NO_CONTEXT) eglDestroyContext(display_, context_);
            eglTerminate(display_);
        }
        display_ = EGL_NO_DISPLAY;
        surface_ = EGL_NO_SURFACE;
        context_ = EGL_NO_CONTEXT;
    }

    void render(Game& game) {
        if (display_ == EGL_NO_DISPLAY) return;
        game.width = width_;
        game.height = height_;
        glViewport(0, 0, width_, height_);
        glClearColor(game.started ? 0.35f : 0.025f,
                     game.started ? 0.64f : 0.055f,
                     game.started ? 0.75f : 0.075f, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glUseProgram(program_);
        if (!game.started) {
            glUniform1f(lightLocation_, 1.0f);
            if (game.settingsOpen) drawSettings(game);
            else if (game.modeSelectOpen) drawModeSelect(game);
            else if (game.loadoutOpen) drawWeaponSelect(game);
            else if (game.skinSelectOpen) drawSkinSelect(game);
            else if (game.storeOpen) drawStore(game);
            else {
                drawMenuScene(game);
                drawSplash(game);
            }
            eglSwapBuffers(display_, surface_);
            return;
        }
        drawSkyGradient();
        glClear(GL_DEPTH_BUFFER_BIT);
        const float aspect = static_cast<float>(width_) / std::max(height_, 1);
        const Vec3 eye = game.player + Vec3{0, playerEyeHeight(game), 0};
        const Vec3 target = eye + game.aim * 100.0f;
        const float fieldOfView = game.aiming ? 0.58f : 1.12f;
        const Mat4 viewProjection = multiply(perspective(fieldOfView, aspect, 0.1f, 700.0f),
                               lookAt(eye, target, {0, 1, 0}));
        glUniformMatrix4fv(matrixLocation_, 1, GL_FALSE, viewProjection.v);
        glUniform1f(lightLocation_, 0.78f);
        drawBox(viewProjection, {0, -0.22f, 0}, {400, 0.35f, 400}, {0.24f, 0.43f, 0.25f});
        drawArena(game, viewProjection);
        drawSkyClouds(viewProjection);
        drawZone(game, viewProjection);
        for (const LootDrop& drop : game.loot) {
            if (!drop.active || drop.collected) continue;
            const bool isMedkit = drop.type == LootType::Medkit;
            const auto color = isMedkit ? std::array<float, 3>{0.78f, 0.84f, 0.68f}
                        : std::array<float, 3>{0.93f, 0.65f, 0.16f};
            drawBox(viewProjection, drop.position + Vec3{0, 0.35f, 0}, {0.72f, 0.62f, 0.72f}, color);
            if (isMedkit) {
            drawBox(viewProjection, drop.position + Vec3{0, 0.36f, 0}, {0.12f, 0.4f, 0.06f},
                {0.8f, 0.16f, 0.12f});
            drawBox(viewProjection, drop.position + Vec3{0, 0.36f, 0}, {0.4f, 0.12f, 0.06f},
                {0.8f, 0.16f, 0.12f});
            }
        }
        const std::array<std::array<float, 3>, 4> botColors{{
            {0.27f, 0.38f, 0.25f}, {0.27f, 0.32f, 0.39f},
            {0.49f, 0.31f, 0.18f}, {0.36f, 0.3f, 0.22f}
        }};
        for (const Bot& bot : game.bots) {
            if (!bot.alive || length(bot.position - game.player) > 155.0f) continue;
            drawCharacter(viewProjection, bot.position, normalized(game.player - bot.position),
                          botColors[bot.appearance], false, bot.appearance);
            const float healthWidth = std::max(0.05f, bot.health / 100.0f);
            drawBox(viewProjection, bot.position + Vec3{0, 2.12f, 0}, {1.15f, 0.08f, 0.08f},
                    {0.14f, 0.12f, 0.11f});
            drawBox(viewProjection, bot.position + Vec3{-(1.0f - healthWidth) * 0.55f, 2.13f, 0},
                    {1.05f * healthWidth, 0.09f, 0.09f}, {0.82f, 0.2f, 0.14f});
        }
            drawProjectileTrails(game, viewProjection);
            drawFirstPersonWeapon(game, viewProjection);
        drawHud(game);
        eglSwapBuffers(display_, surface_);
    }

private:
    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLSurface surface_ = EGL_NO_SURFACE;
    EGLContext context_ = EGL_NO_CONTEXT;
    GLuint program_ = 0;
    GLuint buffer_ = 0;
    GLint matrixLocation_ = -1;
    GLint lightLocation_ = -1;
    GLint positionLocation_ = 0;
    GLint colorLocation_ = 1;
    int width_ = 1;
    int height_ = 1;

    bool createProgram() {
        const char* vertexSource =
            "attribute vec3 aPosition; attribute vec3 aColor; uniform mat4 uMatrix;"
            "uniform float uLight; varying vec3 vColor;"
            "void main(){ gl_Position=uMatrix*vec4(aPosition,1.0); vColor=aColor*uLight; }";
        const char* fragmentSource =
            "precision mediump float; varying vec3 vColor;"
            "void main(){ gl_FragColor=vec4(vColor,1.0); }";
        const GLuint vertex = compileShader(GL_VERTEX_SHADER, vertexSource);
        const GLuint fragment = compileShader(GL_FRAGMENT_SHADER, fragmentSource);
        if (!vertex || !fragment) return false;
        program_ = glCreateProgram();
        glAttachShader(program_, vertex);
        glAttachShader(program_, fragment);
        glBindAttribLocation(program_, 0, "aPosition");
        glBindAttribLocation(program_, 1, "aColor");
        glLinkProgram(program_);
        GLint linked = 0;
        glGetProgramiv(program_, GL_LINK_STATUS, &linked);
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        if (!linked) return false;
        matrixLocation_ = glGetUniformLocation(program_, "uMatrix");
        lightLocation_ = glGetUniformLocation(program_, "uLight");
        return true;
    }

    GLuint compileShader(GLenum type, const char* source) {
        const GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);
        GLint compiled = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (!compiled) {
            char message[512]{};
            glGetShaderInfoLog(shader, sizeof(message), nullptr, message);
            glDeleteShader(shader);
            return 0;
        }
        return shader;
    }

    void submit(const std::vector<Vertex>& vertices, const Mat4& matrix, GLenum mode = GL_TRIANGLES) {
        if (vertices.empty()) return;
        glUniformMatrix4fv(matrixLocation_, 1, GL_FALSE, matrix.v);
        glBindBuffer(GL_ARRAY_BUFFER, buffer_);
        glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex), vertices.data(), GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(positionLocation_);
        glEnableVertexAttribArray(colorLocation_);
        glVertexAttribPointer(positionLocation_, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), nullptr);
        glVertexAttribPointer(colorLocation_, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                              reinterpret_cast<void*>(3 * sizeof(float)));
        glDrawArrays(mode, 0, static_cast<GLsizei>(vertices.size()));
    }

    void drawBox(const Mat4& vp, Vec3 position, Vec3 size, std::array<float, 3> color,
                 float yaw = 0, float pitch = 0) {
        static constexpr float corners[8][3] = {
            {-0.5f,-0.5f,-0.5f},{0.5f,-0.5f,-0.5f},{0.5f,0.5f,-0.5f},{-0.5f,0.5f,-0.5f},
            {-0.5f,-0.5f,0.5f},{0.5f,-0.5f,0.5f},{0.5f,0.5f,0.5f},{-0.5f,0.5f,0.5f}
        };
        static constexpr int faces[36] = {
            0,1,2,0,2,3, 5,4,7,5,7,6, 4,0,3,4,3,7,
            1,5,6,1,6,2, 3,2,6,3,6,7, 4,5,1,4,1,0
        };
        static constexpr float faceShade[6] = {0.78f, 0.88f, 0.68f, 0.82f, 1.08f, 0.62f};
        std::vector<Vertex> vertices;
        vertices.reserve(36);
        for (size_t i = 0; i < 36; ++i) {
            const int index = faces[i];
            const float shade = faceShade[i / 6];
            vertices.push_back({corners[index][0], corners[index][1], corners[index][2],
                                color[0] * shade, color[1] * shade, color[2] * shade});
        }
        const Mat4 orientation = multiply(rotationY(yaw), rotationX(pitch));
        const Mat4 model = multiply(multiply(translation(position), orientation), scaling(size));
        submit(vertices, multiply(vp, model));
    }

    void drawSphere(const Mat4& vp, Vec3 position, Vec3 size, std::array<float, 3> color) {
        std::vector<Vertex> vertices;
        constexpr int slices = 10;
        constexpr int stacks = 7;
        for (int stack = 0; stack < stacks; ++stack) {
            const float p0 = -kPi / 2 + kPi * stack / stacks;
            const float p1 = -kPi / 2 + kPi * (stack + 1) / stacks;
            for (int slice = 0; slice < slices; ++slice) {
                const float t0 = 2 * kPi * slice / slices;
                const float t1 = 2 * kPi * (slice + 1) / slices;
                const Vec3 points[] = {
                    {std::cos(p0)*std::cos(t0),std::sin(p0),std::cos(p0)*std::sin(t0)},
                    {std::cos(p1)*std::cos(t0),std::sin(p1),std::cos(p1)*std::sin(t0)},
                    {std::cos(p1)*std::cos(t1),std::sin(p1),std::cos(p1)*std::sin(t1)},
                    {std::cos(p0)*std::cos(t0),std::sin(p0),std::cos(p0)*std::sin(t0)},
                    {std::cos(p1)*std::cos(t1),std::sin(p1),std::cos(p1)*std::sin(t1)},
                    {std::cos(p0)*std::cos(t1),std::sin(p0),std::cos(p0)*std::sin(t1)}
                };
                for (Vec3 point : points) vertices.push_back({point.x, point.y, point.z,
                                                               color[0], color[1], color[2]});
            }
        }
        const Mat4 model = multiply(translation(position), scaling(size));
        submit(vertices, multiply(vp, model));
    }

    void drawArena(Game&, const Mat4& vp) {
        const std::array<float, 3> roadColor{0.32f, 0.35f, 0.32f};
        const std::array<float, 3> fenceColor{0.48f, 0.43f, 0.31f};
        drawBox(vp, {0, -0.01f, 0}, {7, 0.04f, 380}, roadColor);
        drawBox(vp, {0, -0.01f, 0}, {380, 0.04f, 7}, roadColor);
        for (int offset = -170; offset <= 170; offset += 28) {
            drawBox(vp, {0, 0.025f, static_cast<float>(offset)}, {0.28f, 0.03f, 9.0f},
                {0.83f, 0.75f, 0.48f});
            drawBox(vp, {static_cast<float>(offset), 0.025f, 0}, {9.0f, 0.03f, 0.28f},
                {0.83f, 0.75f, 0.48f});
        }
        drawBox(vp, {0, 0.1f, -190}, {380, 0.2f, 0.5f}, fenceColor);
        drawBox(vp, {0, 0.1f, 190}, {380, 0.2f, 0.5f}, fenceColor);
        drawBox(vp, {-190, 0.1f, 0}, {0.5f, 0.2f, 380}, fenceColor);
        drawBox(vp, {190, 0.1f, 0}, {0.5f, 0.2f, 380}, fenceColor);

        const std::array<std::array<float, 3>, 6> buildingColors{{
            {0.62f, 0.39f, 0.22f}, {0.55f, 0.57f, 0.49f}, {0.68f, 0.52f, 0.31f},
            {0.46f, 0.52f, 0.48f}, {0.63f, 0.43f, 0.29f}, {0.58f, 0.59f, 0.48f}
        }};
        for (size_t i = 0; i < kOutpostPositions.size(); ++i) {
            const Vec3 position = kOutpostPositions[i];
            drawBox(vp, position + Vec3{0, 0.12f, 0}, {8.8f, 0.24f, 6.8f},
                {0.37f, 0.36f, 0.3f});
            drawBox(vp, position + Vec3{-4.35f, 1.3f, 0}, {0.3f, 2.6f, 7}, buildingColors[i]);
            drawBox(vp, position + Vec3{4.35f, 1.3f, 0}, {0.3f, 2.6f, 7}, buildingColors[i]);
            drawBox(vp, position + Vec3{0, 1.3f, 3.35f}, {8.8f, 2.6f, 0.3f}, buildingColors[i]);
            drawBox(vp, position + Vec3{-2.8f, 1.3f, -3.35f}, {3.1f, 2.6f, 0.3f},
                buildingColors[i]);
            drawBox(vp, position + Vec3{2.8f, 1.3f, -3.35f}, {3.1f, 2.6f, 0.3f},
                buildingColors[i]);
            drawBox(vp, position + Vec3{0, 2.35f, -3.35f}, {2.5f, 0.5f, 0.3f},
                buildingColors[i]);
            drawBox(vp, position + Vec3{0, 0.65f, -3.47f}, {1.35f, 1.3f, 0.08f},
                {0.2f, 0.15f, 0.11f});
            drawBox(vp, position + Vec3{-2.7f, 1.75f, -3.53f}, {1.25f, 0.9f, 0.08f},
                {0.2f, 0.52f, 0.58f});
            drawBox(vp, position + Vec3{2.7f, 1.75f, -3.53f}, {1.25f, 0.9f, 0.08f},
                {0.2f, 0.52f, 0.58f});
            drawBox(vp, position + Vec3{-2.8f, 0.48f, 1.5f}, {1.2f, 0.72f, 1.2f},
                {0.49f, 0.34f, 0.19f});
            drawBox(vp, position + Vec3{2.8f, 0.48f, 1.5f}, {1.2f, 0.72f, 1.2f},
                {0.49f, 0.34f, 0.19f});
            drawBox(vp, position + Vec3{0, 2.75f, 0}, {10, 0.35f, 8}, {0.25f, 0.28f, 0.25f});
            drawBox(vp, position + Vec3{0, 3.95f, 0.25f}, {4.8f, 2.25f, 5.2f},
                {buildingColors[i][0] * 0.88f, buildingColors[i][1] * 0.92f,
                 buildingColors[i][2] * 0.9f});
            drawBox(vp, position + Vec3{0, 5.2f, 0.25f}, {5.6f, 0.3f, 6},
                {0.23f, 0.27f, 0.26f});
            drawBox(vp, position + Vec3{8, 0.6f, 2}, {2, 1.2f, 2}, {0.43f, 0.31f, 0.19f});
        }

        for (int i = 0; i < 9; ++i) {
            const float x = -160.0f + i * 40.0f;
            const float offsets[] = {-150.0f + (i % 2) * 18.0f, 150.0f - (i % 2) * 18.0f};
            for (float z : offsets) {
                drawBox(vp, {x, 0.8f, z}, {1.2f, 1.6f, 1.2f}, {0.38f, 0.24f, 0.13f});
                drawBox(vp, {x, 2.25f, z}, {4.4f, 3.6f, 4.4f}, {0.13f, 0.38f, 0.22f});
                drawBox(vp, {x, 4.35f, z}, {2.8f, 2.1f, 2.8f}, {0.17f, 0.44f, 0.24f});
            }
        }
        for (int i = 0; i < 12; ++i) {
            const float angle = 2 * kPi * i / 12;
            const float radius = 72.0f + (i % 3) * 14.0f;
            const float x = std::cos(angle) * radius;
            const float z = std::sin(angle) * radius;
            drawBox(vp, {x, 0.65f, z}, {1.0f, 1.3f, 1.0f}, {0.38f, 0.24f, 0.13f});
            drawBox(vp, {x, 2.15f, z}, {3.5f, 3.0f, 3.5f}, {0.13f, 0.38f, 0.22f});
            drawBox(vp, {x, 3.95f, z}, {2.2f, 1.7f, 2.2f}, {0.17f, 0.44f, 0.24f});
        }
    }

    void drawSkyGradient() {
        const std::array<std::array<float, 3>, 7> colors{{
            {0.035f, 0.11f, 0.23f}, {0.055f, 0.2f, 0.36f}, {0.09f, 0.31f, 0.48f},
            {0.16f, 0.43f, 0.57f}, {0.34f, 0.59f, 0.63f}, {0.57f, 0.67f, 0.58f},
            {0.76f, 0.69f, 0.48f}
        }};
        glUniform1f(lightLocation_, 1.0f);
        const float bandHeight = static_cast<float>(height_) / colors.size();
        for (size_t i = 0; i < colors.size(); ++i) {
            uiRect(0, i * bandHeight, width_, bandHeight + 1, colors[i]);
        }
        glUniform1f(lightLocation_, 0.78f);
    }

    void drawSkyClouds(const Mat4& vp) {
        drawBox(vp, {-118, 63, -118}, {13, 8, 10}, {0.95f, 0.78f, 0.57f});
        const std::array<std::array<float, 3>, 3> cloudColors{{
            {0.75f, 0.86f, 0.84f}, {0.65f, 0.79f, 0.8f}, {0.84f, 0.84f, 0.73f}
        }};
        for (int i = 0; i < 12; ++i) {
            const float angle = 2 * kPi * i / 12;
            const float radius = 115.0f + (i % 3) * 24.0f;
            const float x = std::cos(angle) * radius;
            const float z = std::sin(angle) * radius;
            const float y = 38.0f + (i % 3) * 8.0f;
            const auto color = cloudColors[i % cloudColors.size()];
            drawBox(vp, {x, y, z}, {15, 3.2f, 7}, color);
            drawBox(vp, {x - 3.5f, y + 2.0f, z}, {7, 2.2f, 6},
                    {std::min(1.0f, color[0] * 1.08f), std::min(1.0f, color[1] * 1.06f),
                     std::min(1.0f, color[2] * 1.05f)});
            drawBox(vp, {x + 4.0f, y + 1.3f, z + 1.0f}, {6, 2.0f, 5}, color);
        }
    }

    void drawZone(const Game& game, const Mat4& vp) {
        std::vector<Vertex> vertices;
        constexpr int segments = 96;
        for (int i = 0; i < segments; ++i) {
            const float a = 2 * kPi * i / segments;
            const float b = 2 * kPi * (i + 1) / segments;
            vertices.push_back({game.zoneCenter.x + std::cos(a) * game.zoneRadius, 0.08f,
                                game.zoneCenter.z + std::sin(a) * game.zoneRadius,
                                0.2f, 0.85f, 1.0f});
            vertices.push_back({game.zoneCenter.x + std::cos(b) * game.zoneRadius, 0.08f,
                                game.zoneCenter.z + std::sin(b) * game.zoneRadius,
                                0.2f, 0.85f, 1.0f});
        }
        glUniform1f(lightLocation_, 1.0f);
        submit(vertices, vp, GL_LINES);
        glUniform1f(lightLocation_, 0.78f);
    }

        void drawCharacter(const Mat4& vp, Vec3 position, Vec3 direction,
                           std::array<float, 3> color, bool player, int appearance = 0,
                           bool crouched = false) {
            const float poseScale = crouched ? 0.76f : 1.0f;
        const float yaw = std::atan2(direction.x, -direction.z);
        const Vec3 right{std::cos(yaw), 0, std::sin(yaw)};
        const std::array<std::array<float, 3>, 4> armorColors{{
            {0.17f, 0.24f, 0.16f}, {0.18f, 0.21f, 0.27f},
            {0.3f, 0.18f, 0.11f}, {0.26f, 0.22f, 0.15f}
        }};
        const std::array<std::array<float, 3>, 4> helmetColors{{
            {0.25f, 0.31f, 0.2f}, {0.21f, 0.25f, 0.31f},
            {0.38f, 0.25f, 0.15f}, {0.35f, 0.3f, 0.2f}
        }};
        const auto armor = player ? std::array<float, 3>{0.1f, 0.24f, 0.58f}
                      : armorColors[appearance % armorColors.size()];
        const auto helmet = player ? std::array<float, 3>{0.12f, 0.28f, 0.65f}
                       : helmetColors[appearance % helmetColors.size()];
        const std::array<float, 3> dark{0.11f, 0.13f, 0.12f};
        const std::array<float, 3> skin{0.72f, 0.53f, 0.39f};

        drawBox(vp, position + Vec3{0, 0.8f * poseScale, 0},
            {0.68f, 0.94f * poseScale, 0.4f}, color, yaw);
        drawBox(vp, position + Vec3{0, 0.88f * poseScale, 0},
            {0.76f, 0.58f * poseScale, 0.48f}, armor, yaw);
        drawBox(vp, position + Vec3{0, 0.92f * poseScale, 0} + direction * 0.26f,
            {0.4f, 0.4f * poseScale, 0.08f}, color, yaw);
        drawBox(vp, position + Vec3{0, 0.88f * poseScale, 0} - direction * 0.34f,
            {0.4f, 0.58f * poseScale, 0.27f}, dark, yaw);
        for (float side : {-1.0f, 1.0f}) {
            drawBox(vp, position + right * (side * 0.45f) + Vec3{0, 1.08f * poseScale, 0},
                {0.25f, 0.3f * poseScale, 0.31f}, armor, yaw);
            drawBox(vp, position + right * (side * 0.49f) + Vec3{0, 0.7f * poseScale, 0},
                {0.2f, 0.48f * poseScale, 0.22f}, color, yaw);
            drawBox(vp, position + right * (side * 0.2f) + Vec3{0, 0.24f * poseScale, 0},
                {0.23f, 0.44f * poseScale, 0.26f}, dark, yaw);
            drawBox(vp, position + right * (side * 0.2f) + Vec3{0, 0.04f * poseScale, 0} + direction * 0.08f,
                {0.25f, 0.13f * poseScale, 0.34f}, {0.08f, 0.09f, 0.09f}, yaw);
        }
        drawBox(vp, position + Vec3{0, 1.48f * poseScale, 0},
            {0.4f, 0.42f * poseScale, 0.4f}, skin, yaw);
        drawBox(vp, position + Vec3{0, 1.72f * poseScale, 0},
            {0.51f, 0.29f * poseScale, 0.5f}, helmet, yaw);
        drawBox(vp, position + Vec3{0, 1.56f * poseScale, 0} + direction * 0.21f,
            {0.34f, 0.1f * poseScale, 0.1f}, dark, yaw);
        drawBox(vp, position + Vec3{0, 1.02f * poseScale, 0} + direction * 0.54f,
            {0.14f, 0.15f * poseScale, 0.95f}, {0.1f, 0.11f, 0.1f}, yaw);
        drawBox(vp, position + Vec3{0, 1.02f * poseScale, 0} + direction * 1.02f,
            {0.1f, 0.1f * poseScale, 0.16f}, {0.32f, 0.34f, 0.31f}, yaw);
    }

        void drawFirstPersonWeapon(const Game& game, const Mat4& vp) {
        const float weaponPitch = game.pitch + game.recoil;
        const Vec3 forward = lookDirection(game.yaw, weaponPitch);
        const Vec3 right{std::cos(game.yaw), 0, std::sin(game.yaw)};
        const Vec3 eye = game.player + Vec3{0, playerEyeHeight(game), 0};
        const float bob = game.grounded
            ? std::sin(game.roundTime * 13.0f) * std::min(length(game.playerVelocity) / 7.5f, 1.0f) * 0.025f
            : 0.0f;
        const float sideOffset = game.aiming ? 0.14f : 0.36f;
        const Vec3 origin = eye + forward * 1.0f + right * sideOffset + Vec3{0, -0.5f + bob, 0};
        const bool smg = game.selectedWeapon == 1;
        const bool sniper = game.selectedWeapon == 2;
        const float receiverLength = sniper ? 0.78f : (smg ? 0.48f : 0.62f);
        const float barrelLength = sniper ? 1.0f : (smg ? 0.36f : 0.52f);
        const float barrelStart = receiverLength * 0.5f + barrelLength * 0.5f;
        const std::array<float, 3> receiver = sniper ? std::array<float, 3>{0.18f, 0.2f, 0.2f}
            : (smg ? std::array<float, 3>{0.15f, 0.19f, 0.16f}
                   : std::array<float, 3>{0.12f, 0.15f, 0.15f});
        const std::array<float, 3> steel{0.32f, 0.36f, 0.34f};
        const std::array<float, 3> grip{0.12f, 0.1f, 0.08f};
        const auto sleeve = playerSkinColor(game.selectedSkin);
        const std::array<float, 3> skin{0.68f, 0.48f, 0.34f};
        const float yaw = game.yaw;

        drawBox(vp, origin + right * 0.28f - forward * 0.23f + Vec3{0, -0.23f, 0},
            {0.32f, 0.3f, 0.56f}, sleeve, yaw, weaponPitch);
        drawBox(vp, origin - right * 0.11f - forward * 0.3f + Vec3{0, -0.28f, 0},
            {0.3f, 0.28f, 0.5f}, sleeve, yaw, weaponPitch);
        drawBox(vp, origin + right * 0.18f - forward * 0.02f + Vec3{0, -0.03f, 0},
            {0.28f, 0.18f, 0.32f}, skin, yaw, weaponPitch);
        drawBox(vp, origin + forward * 0.08f, {0.34f, 0.24f, receiverLength}, receiver, yaw, weaponPitch);
        drawBox(vp, origin - forward * 0.38f, {0.28f, 0.2f, 0.34f}, grip, yaw, weaponPitch);
        drawBox(vp, origin + forward * barrelStart, {sniper ? 0.1f : 0.13f,
            sniper ? 0.1f : 0.13f, barrelLength}, steel, yaw, weaponPitch);
        drawBox(vp, origin + forward * (barrelStart + barrelLength * 0.5f),
            {0.17f, 0.16f, 0.12f}, grip, yaw, weaponPitch);
        drawBox(vp, origin + forward * 0.08f + Vec3{0, -0.23f, 0},
            {0.18f, 0.32f, 0.24f}, grip, yaw, weaponPitch);
        drawBox(vp, origin + forward * 0.04f + Vec3{0, sniper ? 0.24f : 0.17f, 0},
            {sniper ? 0.18f : 0.1f, sniper ? 0.22f : 0.1f, sniper ? 0.38f : 0.22f},
            steel, yaw, weaponPitch);
        if (game.recoil > 0.055f) {
            drawBox(vp, origin + forward * 0.86f, {0.22f, 0.2f, 0.12f},
                {1.0f, 0.61f, 0.18f}, yaw, weaponPitch);
        }
        }

        void drawProjectileTrails(const Game& game, const Mat4& vp) {
        std::vector<Vertex> vertices;
        for (const Projectile& projectile : game.projectiles) {
            if (!projectile.active) continue;
            const Vec3 trail = normalized(projectile.velocity) * 0.85f;
            const std::array<float, 3> color = projectile.playerOwned
                ? std::array<float, 3>{1.0f, 0.78f, 0.36f}
                : std::array<float, 3>{0.94f, 0.3f, 0.18f};
            const Vec3 start = projectile.position - trail;
            vertices.push_back({start.x, start.y, start.z, color[0], color[1], color[2]});
            vertices.push_back({projectile.position.x, projectile.position.y, projectile.position.z,
                                color[0], color[1], color[2]});
        }
        glUniform1f(lightLocation_, 1.0f);
        submit(vertices, vp, GL_LINES);
        glUniform1f(lightLocation_, 0.78f);
    }

    static std::array<uint8_t, 7> glyph(char character) {
        switch (character) {
            case 'A': return {14,17,17,31,17,17,17}; case 'B': return {30,17,17,30,17,17,30};
            case 'C': return {14,17,16,16,16,17,14}; case 'D': return {30,17,17,17,17,17,30};
            case 'E': return {31,16,16,30,16,16,31}; case 'F': return {31,16,16,30,16,16,16};
            case 'G': return {14,17,16,23,17,17,15}; case 'H': return {17,17,17,31,17,17,17};
            case 'I': return {14,4,4,4,4,4,14}; case 'J': return {7,2,2,2,18,18,12};
            case 'K': return {17,18,20,24,20,18,17}; case 'L': return {16,16,16,16,16,16,31};
            case 'M': return {17,27,21,21,17,17,17}; case 'N': return {17,25,21,19,17,17,17};
            case 'O': return {14,17,17,17,17,17,14}; case 'P': return {30,17,17,30,16,16,16};
            case 'Q': return {14,17,17,17,21,18,13}; case 'R': return {30,17,17,30,20,18,17};
            case 'S': return {15,16,16,14,1,1,30}; case 'T': return {31,4,4,4,4,4,4};
            case 'U': return {17,17,17,17,17,17,14}; case 'V': return {17,17,17,17,17,10,4};
            case 'W': return {17,17,17,21,21,21,10}; case 'X': return {17,17,10,4,10,17,17};
            case 'Y': return {17,17,10,4,4,4,4}; case 'Z': return {31,1,2,4,8,16,31};
            case '0': return {14,17,19,21,25,17,14}; case '1': return {4,12,4,4,4,4,14};
            case '2': return {14,17,1,2,4,8,31}; case '3': return {30,1,1,14,1,1,30};
            case '4': return {2,6,10,18,31,2,2}; case '5': return {31,16,16,30,1,1,30};
            case '6': return {14,16,16,30,17,17,14}; case '7': return {31,1,2,4,8,8,8};
            case '8': return {14,17,17,14,17,17,14}; case '9': return {14,17,17,15,1,1,14};
            default: return {};
        }
    }

    void uiRect(float x, float y, float w, float h, std::array<float, 3> color) {
        const float left = 2 * x / width_ - 1;
        const float right = 2 * (x + w) / width_ - 1;
        const float top = 1 - 2 * y / height_;
        const float bottom = 1 - 2 * (y + h) / height_;
        std::vector<Vertex> vertices = {
            {left,top,0,color[0],color[1],color[2]}, {left,bottom,0,color[0],color[1],color[2]},
            {right,bottom,0,color[0],color[1],color[2]}, {left,top,0,color[0],color[1],color[2]},
            {right,bottom,0,color[0],color[1],color[2]}, {right,top,0,color[0],color[1],color[2]}
        };
        glDisable(GL_DEPTH_TEST);
        submit(vertices, identity());
        glEnable(GL_DEPTH_TEST);
    }

    void uiText(float x, float y, const std::string& text, float scale, std::array<float, 3> color) {
        float cursor = x;
        for (char character : text) {
            const auto rows = glyph(character);
            for (int row = 0; row < 7; ++row) {
                for (int column = 0; column < 5; ++column) {
                    if ((rows[row] & (1 << (4 - column))) != 0) {
                        uiRect(cursor + column * scale, y + row * scale, scale, scale, color);
                    }
                }
            }
            cursor += 6 * scale;
        }
    }

    void uiCircle(float centerX, float centerY, float radius, std::array<float, 3> color) {
        std::vector<Vertex> vertices;
        constexpr int segments = 36;
        for (int i = 0; i < segments; ++i) {
            const float a = 2 * kPi * i / segments;
            const float b = 2 * kPi * (i + 1) / segments;
            const float ax = 2 * (centerX + std::cos(a) * radius) / width_ - 1;
            const float ay = 1 - 2 * (centerY + std::sin(a) * radius) / height_;
            const float bx = 2 * (centerX + std::cos(b) * radius) / width_ - 1;
            const float by = 1 - 2 * (centerY + std::sin(b) * radius) / height_;
            vertices.push_back({ax, ay, 0, color[0], color[1], color[2]});
            vertices.push_back({bx, by, 0, color[0], color[1], color[2]});
        }
        glDisable(GL_DEPTH_TEST);
        glUniform1f(lightLocation_, 1.0f);
        submit(vertices, identity(), GL_LINES);
        glUniform1f(lightLocation_, 0.78f);
        glEnable(GL_DEPTH_TEST);
    }

    void uiDisc(float centerX, float centerY, float radius, std::array<float, 3> color) {
        std::vector<Vertex> vertices;
        constexpr int segments = 36;
        const float cx = 2 * centerX / width_ - 1;
        const float cy = 1 - 2 * centerY / height_;
        for (int i = 0; i < segments; ++i) {
            const float a = 2 * kPi * i / segments;
            const float b = 2 * kPi * (i + 1) / segments;
            const float ax = 2 * (centerX + std::cos(a) * radius) / width_ - 1;
            const float ay = 1 - 2 * (centerY + std::sin(a) * radius) / height_;
            const float bx = 2 * (centerX + std::cos(b) * radius) / width_ - 1;
            const float by = 1 - 2 * (centerY + std::sin(b) * radius) / height_;
            vertices.push_back({cx, cy, 0, color[0], color[1], color[2]});
            vertices.push_back({ax, ay, 0, color[0], color[1], color[2]});
            vertices.push_back({bx, by, 0, color[0], color[1], color[2]});
        }
        glDisable(GL_DEPTH_TEST);
        glUniform1f(lightLocation_, 1.0f);
        submit(vertices, identity());
        glUniform1f(lightLocation_, 0.78f);
        glEnable(GL_DEPTH_TEST);
    }

    void uiButton(float centerX, float centerY, float radius, const std::string& label,
                  std::array<float, 3> fill, std::array<float, 3> accent,
                  bool active = false, float textScale = 1.2f) {
        if (active) fill = {0.13f, 0.29f, 0.29f};
        uiDisc(centerX, centerY, radius, fill);
        uiCircle(centerX, centerY, radius, accent);
        uiCircle(centerX, centerY, radius - 4, {0.32f, 0.42f, 0.4f});
        const float textWidth = static_cast<float>(label.size()) * 6 * textScale;
        uiText(centerX - textWidth * 0.5f, centerY - 3.5f * textScale,
               label, textScale, active ? accent : std::array<float, 3>{0.93f, 0.95f, 0.88f});
    }

        void drawControlIcon(HudControl control, float centerX, float centerY, float radius,
                    std::array<float, 3> color) {
         const float thickness = std::max(2.0f, radius * 0.1f);
         if (control == JumpHud) {
             uiRect(centerX - thickness * 0.5f, centerY - radius * 0.22f,
                 thickness, radius * 0.58f, color);
             uiRect(centerX - radius * 0.27f, centerY - radius * 0.3f,
                 radius * 0.19f, thickness, color);
             uiRect(centerX + radius * 0.08f, centerY - radius * 0.3f,
                 radius * 0.19f, thickness, color);
             uiRect(centerX - radius * 0.27f, centerY - radius * 0.43f,
                 thickness, radius * 0.16f, color);
             uiRect(centerX + radius * 0.18f, centerY - radius * 0.43f,
                 thickness, radius * 0.16f, color);
         } else if (control == CrouchHud) {
             uiDisc(centerX - radius * 0.12f, centerY - radius * 0.25f,
                 radius * 0.12f, color);
             uiRect(centerX - radius * 0.04f, centerY - radius * 0.12f,
                 radius * 0.38f, thickness, color);
             uiRect(centerX - radius * 0.02f, centerY - radius * 0.04f,
                 thickness, radius * 0.28f, color);
             uiRect(centerX + radius * 0.25f, centerY - radius * 0.02f,
                 thickness, radius * 0.24f, color);
             uiRect(centerX - radius * 0.08f, centerY + radius * 0.2f,
                 radius * 0.31f, thickness, color);
             uiRect(centerX + radius * 0.12f, centerY + radius * 0.15f,
                 radius * 0.28f, thickness, color);
         } else if (control == AimHud) {
             uiCircle(centerX, centerY, radius * 0.3f, color);
             uiDisc(centerX, centerY, radius * 0.07f, color);
             uiRect(centerX - thickness * 0.5f, centerY - radius * 0.48f,
                 thickness, radius * 0.25f, color);
             uiRect(centerX - thickness * 0.5f, centerY + radius * 0.23f,
                 thickness, radius * 0.25f, color);
             uiRect(centerX - radius * 0.48f, centerY - thickness * 0.5f,
                 radius * 0.25f, thickness, color);
             uiRect(centerX + radius * 0.23f, centerY - thickness * 0.5f,
                 radius * 0.25f, thickness, color);
         } else if (control == FireHud) {
             uiDisc(centerX, centerY, radius * 0.11f, color);
             uiRect(centerX - thickness * 0.5f, centerY - radius * 0.46f,
                 thickness, radius * 0.23f, color);
             uiRect(centerX - thickness * 0.5f, centerY + radius * 0.23f,
                 thickness, radius * 0.23f, color);
             uiRect(centerX - radius * 0.46f, centerY - thickness * 0.5f,
                 radius * 0.23f, thickness, color);
             uiRect(centerX + radius * 0.23f, centerY - thickness * 0.5f,
                 radius * 0.23f, thickness, color);
             uiDisc(centerX, centerY - radius * 0.27f, radius * 0.055f, color);
         }
        }

    void drawHud(const Game& game) {
        glUniform1f(lightLocation_, 1.0f);
         uiRect(22, 20, 280, 62, {0.035f, 0.08f, 0.1f});
         uiText(34, 29, "ALIVE", 1.8f, {0.82f, 0.91f, 0.87f});
         uiText(104, 29, std::to_string(1 + std::count_if(game.bots.begin(), game.bots.end(),
                                    [](const Bot& bot) { return bot.alive; })),
             1.8f, {0.4f, 0.94f, 0.7f});
         uiText(144, 29, "KILL", 1.5f, {0.82f, 0.91f, 0.87f});
         uiText(203, 29, std::to_string(game.kills), 1.8f, {1.0f, 0.72f, 0.28f});
        uiText(34, 57, "HP", 1.6f, {0.9f, 0.9f, 0.85f});
        uiRect(72, 57, 145, 12, {0.18f, 0.2f, 0.19f});
        uiRect(72, 57, 145 * std::clamp(game.health / 100.0f, 0.0f, 1.0f), 12,
               game.health > 30 ? std::array<float, 3>{0.24f, 0.82f, 0.42f}
                                : std::array<float, 3>{0.94f, 0.22f, 0.14f});

         uiRect(width_ - 204, 20, 180, 112, {0.035f, 0.08f, 0.1f});
         uiText(width_ - 190, 29, "MAG", 1.5f, {0.87f, 0.89f, 0.82f});
         uiText(width_ - 122, 29, std::to_string(game.magazine), 1.8f, {1.0f, 0.78f, 0.29f});
         uiText(width_ - 190, 52, "RES", 1.5f, {0.87f, 0.89f, 0.82f});
         uiText(width_ - 122, 52, std::to_string(game.reserveAmmo), 1.8f, {1.0f, 0.78f, 0.29f});
        const bool outsideZone = length(game.player - game.zoneCenter) > game.zoneRadius;
        const std::string status = game.reloading ? "RELOAD" : (outsideZone ? "STORM" : "SAFE");
        const std::array<float, 3> statusColor = game.reloading
            ? std::array<float, 3>{1.0f, 0.66f, 0.24f}
            : (outsideZone ? std::array<float, 3>{1.0f, 0.3f, 0.2f}
                           : std::array<float, 3>{0.31f, 0.83f, 0.96f});
        uiText(width_ - 190, 76, status, 1.1f, statusColor);
         uiText(width_ - 190, 96, weaponStats(game.selectedWeapon).name, 1.2f,
             {0.52f, 0.85f, 0.77f});

        const float centerX = width_ * 0.5f;
        const float centerY = height_ * 0.47f;
        const float reticleSize = std::clamp((game.aiming ? 6.0f : 10.0f) +
                                              length(game.playerVelocity) * 0.55f + game.recoil * 35,
                                              6.0f, 22.0f);
        bool targetCentered = false;
        const Vec3 aimOrigin = game.player + Vec3{0, playerEyeHeight(game), 0};
        for (const Bot& bot : game.bots) {
            if (!bot.alive) continue;
            const Vec3 target = bot.position + Vec3{0, 1.05f, 0};
            const Vec3 offset = target - aimOrigin;
            const float distance = length(offset);
            const float threshold = game.aiming ? 0.992f : 0.986f;
            if (distance < 160.0f && dot(normalized(offset), game.aim) > threshold) {
                targetCentered = true;
                break;
            }
        }
        const auto reticleColor = targetCentered
            ? std::array<float, 3>{1.0f, 0.2f, 0.16f}
            : (game.hitMarkerTimer > 0 ? std::array<float, 3>{1.0f, 0.55f, 0.24f}
                                       : std::array<float, 3>{0.96f, 0.95f, 0.81f});
        uiCircle(centerX, centerY, reticleSize, reticleColor);
        uiDisc(centerX, centerY, 2.5f, reticleColor);
        const float stickX = game.movePointer >= 0 ? game.moveOriginX
                                : width_ * game.hudAnchors[MoveHud].x;
        const float stickY = game.movePointer >= 0 ? game.moveOriginY
                                : height_ * game.hudAnchors[MoveHud].y;
        uiDisc(stickX, stickY, 47, {0.045f, 0.085f, 0.09f});
        uiCircle(stickX, stickY, 47, {0.55f, 0.72f, 0.66f});
        uiCircle(stickX, stickY, 33, {0.22f, 0.31f, 0.29f});
        const float knobX = stickX + game.moveX * 22;
        const float knobY = stickY + game.moveY * 22;
        uiDisc(knobX, knobY, 17, {0.24f, 0.43f, 0.38f});
        uiCircle(knobX, knobY, 17, {0.83f, 0.91f, 0.82f});

         const HudAnchor& crouchAnchor = game.hudAnchors[CrouchHud];
         const HudAnchor& aimAnchor = game.hudAnchors[AimHud];
         const HudAnchor& jumpAnchor = game.hudAnchors[JumpHud];
         const HudAnchor& medAnchor = game.hudAnchors[MedHud];
         const HudAnchor& reloadAnchor = game.hudAnchors[ReloadHud];
         const HudAnchor& fireAnchor = game.hudAnchors[FireHud];
         const float crouchX = width_ * crouchAnchor.x;
         const float crouchY = height_ * crouchAnchor.y;
         const float aimX = width_ * aimAnchor.x;
         const float aimY = height_ * aimAnchor.y;
         const float jumpX = width_ * jumpAnchor.x;
         const float jumpY = height_ * jumpAnchor.y;
         const float medX = width_ * medAnchor.x;
         const float medY = height_ * medAnchor.y;
         const float reloadX = width_ * reloadAnchor.x;
         const float reloadY = height_ * reloadAnchor.y;
         const float fireX = width_ * fireAnchor.x;
         const float fireY = height_ * fireAnchor.y;
        uiButton(crouchX, crouchY, 29, "",
             {0.055f, 0.095f, 0.1f}, {0.72f, 0.8f, 0.68f}, game.crouching);
        drawControlIcon(CrouchHud, crouchX, crouchY, 29, {0.93f, 0.95f, 0.88f});
        uiButton(aimX, aimY, 29, "",
             {0.055f, 0.095f, 0.1f}, {0.3f, 0.85f, 0.88f}, game.aiming);
        drawControlIcon(AimHud, aimX, aimY, 29, {0.93f, 0.95f, 0.88f});
        uiButton(jumpX, jumpY, 29, "",
             {0.18f, 0.11f, 0.07f}, {0.98f, 0.67f, 0.29f});
        drawControlIcon(JumpHud, jumpX, jumpY, 29, {1.0f, 0.95f, 0.81f});
         uiButton(medX, medY, 27, "MED",
                 {0.055f, 0.12f, 0.09f}, {0.47f, 0.88f, 0.62f}, game.healing, 1.0f);
         uiText(medX - 3, medY + 12, std::to_string(game.medkits), 1.0f,
               {0.88f, 0.96f, 0.84f});
         uiButton(reloadX, reloadY, 27, game.reloading ? "RLD" : "REL",
                 {0.13f, 0.11f, 0.06f}, {0.95f, 0.77f, 0.34f}, game.reloading, 1.0f);
        uiButton(fireX, fireY, 43, "",
             {0.22f, 0.075f, 0.055f}, {1.0f, 0.53f, 0.28f}, game.firePointer >= 0);
        drawControlIcon(FireHud, fireX, fireY, 43, {1.0f, 0.95f, 0.81f});

         if (game.healing) {
             uiText(centerX - 38, height_ * 0.63f, "HEALING", 1.8f, {0.49f, 0.96f, 0.63f});
         }

        if (game.gameOver) {
            uiRect(width_ * 0.25f, height_ * 0.36f, width_ * 0.5f, height_ * 0.28f,
                   {0.035f, 0.08f, 0.1f});
            const std::string message = game.won ? "VICTORY" : "ELIMINATED";
            uiText(width_ * 0.5f - message.size() * 12, height_ * 0.47f, message, 4,
                   game.won ? std::array<float, 3>{0.35f, 0.96f, 0.61f}
                            : std::array<float, 3>{1.0f, 0.34f, 0.24f});
             uiRect(width_ * 0.5f - 100, height_ * 0.57f, 200, 46, {0.1f, 0.29f, 0.27f});
             uiText(width_ * 0.5f - 54, height_ * 0.57f + 15, "RESTART", 1.8f,
                 {0.92f, 0.96f, 0.87f});
        }
        glUniform1f(lightLocation_, 0.78f);
    }

    void drawMenuScene(const Game& game) {
        drawSkyGradient();
        glClear(GL_DEPTH_BUFFER_BIT);
        const float aspect = static_cast<float>(width_) / std::max(height_, 1);
        const Mat4 viewProjection = multiply(perspective(0.82f, aspect, 0.1f, 100.0f),
            lookAt({0, 4.1f, 14.5f}, {0, 1.35f, 0}, {0, 1, 0}));
        glUniformMatrix4fv(matrixLocation_, 1, GL_FALSE, viewProjection.v);
        glUniform1f(lightLocation_, 0.82f);
        drawBox(viewProjection, {0, -0.55f, 0}, {18, 0.7f, 15}, {0.19f, 0.27f, 0.25f});
        drawBox(viewProjection, {0, 3.2f, -7.3f}, {21, 8, 0.35f}, {0.08f, 0.16f, 0.18f});
        drawBox(viewProjection, {-7.6f, 1.0f, -3.0f}, {2.1f, 2.0f, 2.1f}, {0.25f, 0.42f, 0.38f});
        drawBox(viewProjection, {7.4f, 1.3f, -3.4f}, {2.5f, 2.6f, 2.5f}, {0.48f, 0.3f, 0.2f});
        drawCharacter(viewProjection, {0, 0, 0}, {0, 0, -1},
                      playerSkinColor(game.selectedSkin), true);
        glUniform1f(lightLocation_, 0.78f);
    }

    void drawSplash(const Game& game) {
        uiRect(24, 22, 225, 82, {0.035f, 0.065f, 0.075f});
        uiText(42, 38, "RIFTLINE", 3.6f, {0.92f, 0.94f, 0.85f});
        uiText(42, 75, "VOXEL ROYALE", 1.4f, {0.39f, 0.81f, 0.75f});
        uiText(32, height_ - 47, "COINS " + std::to_string(game.coins), 1.5f,
               {0.98f, 0.74f, 0.32f});
        uiButton(width_ * 0.12f, height_ * 0.45f, 37, "SKINS",
                 {0.055f, 0.095f, 0.1f}, {0.36f, 0.83f, 0.68f}, false, 1.1f);
        uiButton(width_ * 0.12f, height_ * 0.59f, 37, "STORE",
                 {0.055f, 0.095f, 0.1f}, {0.94f, 0.68f, 0.29f}, false, 1.0f);
        uiButton(width_ * 0.12f, height_ * 0.73f, 37, "SETTINGS",
                 {0.055f, 0.095f, 0.1f}, {0.32f, 0.78f, 0.85f}, false, 0.82f);
        uiButton(width_ * 0.85f, height_ * 0.86f, 63, "PLAY",
                 {0.12f, 0.28f, 0.24f}, {0.97f, 0.58f, 0.28f}, false, 2.2f);
        const std::string skinLabel = game.selectedSkin == 0 ? "DEFAULT SKIN"
            : (game.selectedSkin == 1 ? "MOSS SKIN"
               : (game.selectedSkin == 2 ? "EMBER SKIN" : "FROST SKIN"));
        uiText(width_ * 0.42f, height_ * 0.86f, skinLabel, 1.4f, {0.88f, 0.91f, 0.84f});
    }

        void drawModeSelect(const Game& game) {
         uiRect(0, 0, width_, height_, {0.025f, 0.055f, 0.075f});
         const float centerX = width_ * 0.5f;
         uiText(centerX - 100, height_ * 0.1f, "GAME MODES", 3.0f,
             {0.93f, 0.95f, 0.88f});
         const std::array<const char*, 3> names{"SOLO", "TRAINING", "HARDCORE"};
         const std::array<const char*, 3> details{"8 BOTS", "3 BOTS NO STORM", "HEAVY DAMAGE"};
         const std::array<std::array<float, 3>, 3> accents{{
             {0.36f, 0.83f, 0.68f}, {0.3f, 0.72f, 0.88f}, {0.95f, 0.42f, 0.25f}
         }};
         const std::array<float, 3> centers{width_ * 0.18f, width_ * 0.5f, width_ * 0.82f};
         const float cardY = height_ * 0.32f;
         const float cardWidth = width_ * 0.26f;
         const float cardHeight = height_ * 0.38f;
         for (size_t i = 0; i < names.size(); ++i) {
             const float x = centers[i] - cardWidth * 0.5f;
             const bool selected = game.selectedMode == static_cast<int>(i);
             uiRect(x, cardY, cardWidth, cardHeight,
                 selected ? std::array<float, 3>{0.1f, 0.22f, 0.21f}
                    : std::array<float, 3>{0.045f, 0.08f, 0.09f});
             uiRect(x, cardY, cardWidth, 5, accents[i]);
             uiRect(x, cardY, 4, cardHeight, accents[i]);
             if (selected) uiRect(x + cardWidth - 4, cardY, 4, cardHeight, accents[i]);
             const float nameWidth = static_cast<float>(std::string(names[i]).size()) * 12.0f;
             uiText(centers[i] - nameWidth * 0.5f, cardY + 36, names[i], 2.0f,
                 {0.94f, 0.95f, 0.89f});
             const float detailWidth = static_cast<float>(std::string(details[i]).size()) * 7.5f;
             uiText(centers[i] - detailWidth * 0.5f, cardY + cardHeight * 0.7f,
                 details[i], 1.2f, accents[i]);
         }
         uiButton(width_ * 0.16f, height_ * 0.84f, 32, "BACK",
               {0.08f, 0.12f, 0.13f}, {0.74f, 0.82f, 0.78f});
         uiButton(width_ * 0.84f, height_ * 0.84f, 40, "NEXT",
               {0.1f, 0.29f, 0.26f}, {0.97f, 0.6f, 0.3f}, false, 1.5f);
        }

        void drawSkinSelect(const Game& game) {
         uiRect(0, 0, width_, height_, {0.025f, 0.055f, 0.075f});
         const float centerX = width_ * 0.5f;
         uiText(centerX - 80, height_ * 0.1f, "CHARACTER SKINS", 2.7f,
             {0.93f, 0.95f, 0.88f});
         const std::array<const char*, 4> names{"DEFAULT", "MOSS", "EMBER", "FROST"};
         const float cardWidth = width_ * 0.19f;
         const float cardHeight = height_ * 0.43f;
         const float cardY = height_ * 0.31f;
         for (size_t i = 0; i < names.size(); ++i) {
             const float cardCenter = width_ * (0.13f + i * 0.245f);
             const float cardX = cardCenter - cardWidth * 0.5f;
             const auto color = playerSkinColor(static_cast<int>(i));
             const bool selected = game.selectedSkin == static_cast<int>(i);
             uiRect(cardX, cardY, cardWidth, cardHeight,
                 selected ? std::array<float, 3>{0.1f, 0.21f, 0.2f}
                    : std::array<float, 3>{0.045f, 0.08f, 0.09f});
             uiRect(cardX, cardY, cardWidth, 5, color);
             uiDisc(cardCenter, cardY + cardHeight * 0.45f, 42, color);
             uiCircle(cardCenter, cardY + cardHeight * 0.45f, 42, {0.85f, 0.9f, 0.83f});
             const float labelWidth = static_cast<float>(std::string(names[i]).size()) * 8.0f;
             uiText(cardCenter - labelWidth * 0.5f, cardY + cardHeight * 0.78f,
                 names[i], 1.35f, {0.92f, 0.94f, 0.87f});
             const std::string status = game.ownedSkins[i] ? "OWNED" : "LOCKED";
             uiText(cardCenter - 24, cardY + cardHeight * 0.9f, status, 1.0f,
                 game.ownedSkins[i] ? color : std::array<float, 3>{0.9f, 0.63f, 0.28f});
         }
         uiButton(width_ * 0.16f, height_ * 0.84f, 32, "BACK",
               {0.08f, 0.12f, 0.13f}, {0.74f, 0.82f, 0.78f});
         uiButton(width_ * 0.84f, height_ * 0.84f, 37, "STORE",
               {0.14f, 0.11f, 0.06f}, {0.96f, 0.69f, 0.3f}, false, 1.0f);
        }

        void drawStore(const Game& game) {
         uiRect(0, 0, width_, height_, {0.025f, 0.055f, 0.075f});
         const float centerX = width_ * 0.5f;
         uiText(centerX - 60, height_ * 0.1f, "SKIN STORE", 2.8f,
             {0.93f, 0.95f, 0.88f});
         uiText(width_ - 190, height_ * 0.12f, "COINS " + std::to_string(game.coins), 1.6f,
             {0.98f, 0.74f, 0.32f});
         const std::array<int, 4> prices{0, 100, 150, 200};
         for (int skin = 1; skin < 4; ++skin) {
             const float cardCenter = width_ * (0.25f + (skin - 1) * 0.25f);
             const float cardY = height_ * 0.3f;
             const float cardWidth = width_ * 0.22f;
             const float cardHeight = height_ * 0.42f;
             const auto color = playerSkinColor(skin);
             const bool selected = game.selectedStoreSkin == skin;
             uiRect(cardCenter - cardWidth * 0.5f, cardY, cardWidth, cardHeight,
                 selected ? std::array<float, 3>{0.1f, 0.21f, 0.2f}
                    : std::array<float, 3>{0.045f, 0.08f, 0.09f});
             uiRect(cardCenter - cardWidth * 0.5f, cardY, cardWidth, 5, color);
             uiDisc(cardCenter, cardY + cardHeight * 0.42f, 40, color);
             const char* name = skin == 1 ? "MOSS" : (skin == 2 ? "EMBER" : "FROST");
             const float labelWidth = static_cast<float>(std::string(name).size()) * 8.0f;
             uiText(cardCenter - labelWidth * 0.5f, cardY + cardHeight * 0.73f,
                 name, 1.4f, {0.92f, 0.94f, 0.87f});
             const std::string status = game.ownedSkins[skin]
              ? "OWNED" : std::to_string(prices[skin]);
             uiText(cardCenter - static_cast<float>(status.size()) * 3.2f,
                 cardY + cardHeight * 0.87f, status, 1.1f,
                 game.ownedSkins[skin] ? color : std::array<float, 3>{0.98f, 0.74f, 0.32f});
         }
         uiButton(width_ * 0.16f, height_ * 0.84f, 32, "BACK",
               {0.08f, 0.12f, 0.13f}, {0.74f, 0.82f, 0.78f});
         uiButton(width_ * 0.84f, height_ * 0.84f, 40, "BUY",
               {0.12f, 0.28f, 0.24f}, {0.97f, 0.58f, 0.28f}, false, 1.5f);
        }

        void drawSettings(const Game& game) {
        uiRect(0, 0, width_, height_, {0.025f, 0.055f, 0.075f});
        const float centerX = width_ * 0.5f;
        uiText(centerX - 70, height_ * 0.1f, game.hudEditMode ? "EDIT HUD" : "SETTINGS",
               3.0f, {0.93f, 0.95f, 0.88f});
        if (game.hudEditMode) {
            const std::array<const char*, HudControlCount> labels{
                "MOVE", "CROUCH", "AIM", "JUMP", "MED", "REL", "FIRE"
            };
            const std::array<float, 3> accent{0.32f, 0.85f, 0.75f};
            for (size_t i = 0; i < HudControlCount; ++i) {
                const float x = width_ * game.hudAnchors[i].x;
                const float y = height_ * game.hudAnchors[i].y;
                uiButton(x, y, i == FireHud ? 43 : 32, labels[i],
                         {0.055f, 0.095f, 0.1f}, accent, game.draggedHudControl == static_cast<int>(i),
                         i == MoveHud || i == CrouchHud ? 0.8f : 1.0f);
            }
            uiButton(width_ * 0.88f, height_ * 0.12f, 30, "DONE",
                     {0.11f, 0.28f, 0.24f}, {0.42f, 0.86f, 0.67f});
            uiText(centerX - 105, height_ * 0.2f, "DRAG BUTTONS TO MOVE", 1.5f,
                   {0.74f, 0.83f, 0.77f});
            return;
        }

        uiText(centerX - 100, height_ * 0.29f, "AIM SENSITIVITY", 1.8f,
               {0.82f, 0.89f, 0.84f});
        const float sliderX = centerX - width_ * 0.25f;
        const float sliderWidth = width_ * 0.5f;
        const float sliderY = height_ * 0.43f;
        uiRect(sliderX, sliderY, sliderWidth, 10, {0.13f, 0.2f, 0.19f});
        const float sensitivity01 = (game.aimSensitivity - 0.35f) / 2.05f;
        uiRect(sliderX, sliderY, sliderWidth * sensitivity01, 10, {0.29f, 0.78f, 0.66f});
        uiCircle(sliderX + sliderWidth * sensitivity01, sliderY + 5, 18, {0.91f, 0.69f, 0.3f});
        uiText(centerX - 42, sliderY + 28,
               "SENS " + std::to_string(static_cast<int>(game.aimSensitivity * 100)), 1.7f,
               {0.93f, 0.93f, 0.85f});
        uiButton(centerX, height_ * 0.65f, 34, "EDIT HUD",
                 {0.055f, 0.13f, 0.12f}, {0.36f, 0.85f, 0.74f}, false, 1.1f);
        uiButton(centerX, height_ * 0.82f, 32, "BACK",
                 {0.12f, 0.14f, 0.13f}, {0.82f, 0.85f, 0.77f});
    }

        void drawWeaponSelect(const Game& game) {
         uiRect(0, 0, width_, height_, {0.025f, 0.055f, 0.075f});
         const float centerX = width_ * 0.5f;
         uiText(centerX - 115, height_ * 0.08f, "SELECT WEAPON", 3.0f,
             {0.93f, 0.95f, 0.88f});
         uiText(centerX - 95, height_ * 0.18f, "CHOOSE YOUR LOADOUT", 1.6f,
             {0.42f, 0.82f, 0.76f});

         const float cardY = height_ * 0.29f;
         const float cardHeight = height_ * 0.43f;
         const float cardWidth = width_ * 0.27f;
         const std::array<float, 3> centers{width_ * 0.18f, width_ * 0.5f, width_ * 0.82f};
         const std::array<std::array<float, 3>, 3> accents{{
             {0.36f, 0.83f, 0.68f}, {0.95f, 0.68f, 0.29f}, {0.88f, 0.43f, 0.28f}
         }};

         for (size_t i = 0; i < kWeaponStats.size(); ++i) {
             const float cardX = centers[i] - cardWidth * 0.5f;
             const bool selected = game.selectedWeapon == static_cast<int>(i);
             const auto accent = accents[i];
             uiRect(cardX, cardY, cardWidth, cardHeight,
                 selected ? std::array<float, 3>{0.1f, 0.2f, 0.2f}
                    : std::array<float, 3>{0.045f, 0.08f, 0.09f});
             uiRect(cardX, cardY, cardWidth, 5, accent);
             uiRect(cardX, cardY, 4, cardHeight, accent);
             if (selected) {
              uiRect(cardX + cardWidth - 4, cardY, 4, cardHeight, accent);
             }

             const WeaponStats& weapon = kWeaponStats[i];
             const float nameWidth = static_cast<float>(std::string(weapon.name).size()) * 15.0f;
             uiText(centers[i] - nameWidth * 0.5f, cardY + 24, weapon.name, 2.5f,
                 {0.94f, 0.95f, 0.89f});
             const float roleWidth = static_cast<float>(std::string(weapon.role).size()) * 8.4f;
             uiText(centers[i] - roleWidth * 0.5f, cardY + 62, weapon.role, 1.4f, accent);

             const float iconY = cardY + cardHeight * 0.46f;
             const float barrelLength = i == 2 ? cardWidth * 0.28f
                           : (i == 1 ? cardWidth * 0.15f : cardWidth * 0.21f);
             uiRect(centers[i] - barrelLength * 0.15f, iconY, barrelLength, 11, {0.42f, 0.48f, 0.43f});
             uiRect(centers[i] - cardWidth * 0.17f, iconY - 12, cardWidth * 0.36f, 31,
                 {0.16f, 0.2f, 0.19f});
             uiRect(centers[i] - cardWidth * 0.06f, iconY + 17, cardWidth * 0.09f, 23,
                 {0.12f, 0.11f, 0.09f});
             if (i == 2) {
              uiRect(centers[i] - 8, iconY - 24, 35, 12, {0.35f, 0.43f, 0.4f});
             }

             uiText(cardX + 18, cardY + cardHeight * 0.72f, "DAMAGE", 1.3f,
                 {0.78f, 0.83f, 0.77f});
             uiText(cardX + cardWidth - 48, cardY + cardHeight * 0.72f,
                 std::to_string(static_cast<int>(weapon.damage)), 1.5f, accent);
             uiRect(cardX + 18, cardY + cardHeight * 0.82f, cardWidth - 36, 7,
                 {0.15f, 0.19f, 0.18f});
             uiRect(cardX + 18, cardY + cardHeight * 0.82f,
                 (cardWidth - 36) * weapon.damage / 100.0f, 7, accent);
             uiText(cardX + 18, cardY + cardHeight * 0.89f,
                 "MAG " + std::to_string(weapon.magazineSize), 1.2f,
                 {0.82f, 0.86f, 0.79f});
         }

         const float buttonY = height_ * 0.81f;
         uiRect(centerX - 110, buttonY, 220, 54, {0.12f, 0.3f, 0.29f});
         uiRect(centerX - 110, buttonY, 5, 54, {0.96f, 0.54f, 0.27f});
         uiText(centerX - 38, buttonY + 17, "DEPLOY", 2.0f, {0.95f, 0.96f, 0.88f});
         uiText(28, buttonY + 19, "BACK", 1.5f, {0.73f, 0.8f, 0.74f});
        }
};

Renderer renderer;

void separateFromCircle(Vec3& position, Vec3& velocity, float centerX, float centerZ, float radius) {
    float dx = position.x - centerX;
    float dz = position.z - centerZ;
    float distanceSquared = dx * dx + dz * dz;
    if (distanceSquared >= radius * radius) return;
    if (distanceSquared < 0.0001f) {
        dx = 0.01f;
        distanceSquared = dx * dx;
    }
    const float distance = std::sqrt(distanceSquared);
    const float normalX = dx / distance;
    const float normalZ = dz / distance;
    const float push = radius - distance;
    position.x += normalX * push;
    position.z += normalZ * push;
    const float velocityIntoSurface = velocity.x * normalX + velocity.z * normalZ;
    if (velocityIntoSurface < 0) {
        velocity.x -= velocityIntoSurface * normalX;
        velocity.z -= velocityIntoSurface * normalZ;
    }
}

void resolveBoxCollision(Vec3& position, Vec3& velocity, float centerX, float centerZ,
                         float halfWidth, float halfDepth, float radius) {
    const float nearestX = std::clamp(position.x, centerX - halfWidth, centerX + halfWidth);
    const float nearestZ = std::clamp(position.z, centerZ - halfDepth, centerZ + halfDepth);
    float dx = position.x - nearestX;
    float dz = position.z - nearestZ;
    const float distanceSquared = dx * dx + dz * dz;
    if (distanceSquared >= radius * radius) return;

    float normalX = 0;
    float normalZ = 0;
    float push = 0;
    if (distanceSquared < 0.0001f) {
        const float overlapX = halfWidth + radius - std::abs(position.x - centerX);
        const float overlapZ = halfDepth + radius - std::abs(position.z - centerZ);
        if (overlapX < overlapZ) {
            normalX = position.x < centerX ? -1.0f : 1.0f;
            push = overlapX;
        } else {
            normalZ = position.z < centerZ ? -1.0f : 1.0f;
            push = overlapZ;
        }
    } else {
        const float distance = std::sqrt(distanceSquared);
        normalX = dx / distance;
        normalZ = dz / distance;
        push = radius - distance;
    }
    position.x += normalX * push;
    position.z += normalZ * push;
    const float velocityIntoSurface = velocity.x * normalX + velocity.z * normalZ;
    if (velocityIntoSurface < 0) {
        velocity.x -= velocityIntoSurface * normalX;
        velocity.z -= velocityIntoSurface * normalZ;
    }
}

void resolveWorldCollisions(Vec3& position, Vec3& velocity, float radius) {
    if (position.y < 2.7f) {
        for (const Vec3& outpost : kOutpostPositions) {
            resolveBoxCollision(position, velocity, outpost.x - 4.35f, outpost.z,
                               0.15f, 3.5f, radius);
            resolveBoxCollision(position, velocity, outpost.x + 4.35f, outpost.z,
                               0.15f, 3.5f, radius);
            resolveBoxCollision(position, velocity, outpost.x, outpost.z + 3.35f,
                               4.4f, 0.15f, radius);
            resolveBoxCollision(position, velocity, outpost.x - 2.8f, outpost.z - 3.35f,
                               1.55f, 0.15f, radius);
            resolveBoxCollision(position, velocity, outpost.x + 2.8f, outpost.z - 3.35f,
                               1.55f, 0.15f, radius);
        }
    }

    if (position.y < 1.6f) {
        for (int i = 0; i < 9; ++i) {
            const float x = -160.0f + i * 40.0f;
            const float offsets[] = {-150.0f + (i % 2) * 18.0f, 150.0f - (i % 2) * 18.0f};
            for (float z : offsets) separateFromCircle(position, velocity, x, z, radius + 0.75f);
        }
        for (int i = 0; i < 12; ++i) {
            const float angle = 2 * kPi * i / 12;
            const float distance = 72.0f + (i % 3) * 14.0f;
            separateFromCircle(position, velocity, std::cos(angle) * distance,
                               std::sin(angle) * distance, radius + 0.65f);
        }
    }
}

void spawnProjectile(Game& game, Vec3 origin, Vec3 direction, float speed,
                     float damage, bool playerOwned) {
    auto projectile = std::find_if(game.projectiles.begin(), game.projectiles.end(),
                                   [](const Projectile& candidate) { return !candidate.active; });
    if (projectile == game.projectiles.end()) return;
    projectile->position = origin;
    projectile->velocity = normalized(direction) * speed;
    projectile->damage = damage;
    projectile->lifetime = 3.0f;
    projectile->playerOwned = playerOwned;
    projectile->active = true;
}

void spawnLoot(Game& game, Vec3 position) {
    auto drop = std::find_if(game.loot.begin(), game.loot.end(),
                             [](const LootDrop& item) { return !item.active || item.collected; });
    if (drop == game.loot.end()) return;
    drop->position = position + Vec3{0, 0.8f, 0};
    drop->velocity = {static_cast<float>(std::rand() % 9 - 4), 4.2f,
                      static_cast<float>(std::rand() % 9 - 4)};
    drop->type = std::rand() % 4 == 0 ? LootType::Medkit : LootType::Ammo;
    drop->active = true;
    drop->collected = false;
    drop->airborne = true;
}

void initializeGame(Game& game) {
    for (size_t i = 0; i < game.bots.size(); ++i) {
        const float angle = 2 * kPi * i / game.bots.size();
        const float radius = 62 + static_cast<float>(i) * 9;
        game.bots[i].position = {std::cos(angle) * radius, 0, std::sin(angle) * radius};
        game.bots[i].shotTimer = 1.4f + static_cast<float>(i) * 0.23f;
        game.bots[i].appearance = static_cast<int>(i % 4);
        game.bots[i].alive = game.selectedMode != 1 || i < 3;
    }
    game.loot = {{
        {{10, 0, -10}, {}, LootType::Ammo, true, false, false},
        {{-10, 0, 12}, {}, LootType::Medkit, true, false, false},
        {{42, 0, -45}, {}, LootType::Ammo, true, false, false},
        {{-45, 0, -30}, {}, LootType::Medkit, true, false, false},
        {{70, 0, 75}, {}, LootType::Ammo, true, false, false},
        {{-85, 0, 60}, {}, LootType::Medkit, true, false, false},
        {{-125, 0, 15}, {}, LootType::Ammo, true, false, false},
        {{130, 0, -60}, {}, LootType::Ammo, true, false, false},
        {{-105, 0, -105}, {}, LootType::Medkit, true, false, false},
        {{25, 0, -130}, {}, LootType::Ammo, true, false, false}
    }};
}

void restartGame(Game& game) {
    game.player = {};
    game.playerVelocity = {};
    game.yaw = game.pitch = game.targetYaw = game.targetPitch = 0;
    game.aim = {0, 0, -1};
    game.bots = {};
    game.loot = {};
    game.projectiles = {};
    game.health = 100;
    game.zoneCenter = {};
    game.zoneRadius = 160;
    game.fireCooldown = game.footstepTimer = game.hitMarkerTimer = game.recoil = 0;
    game.reloadTimer = game.healTimer = game.stormTimer = game.roundTime = 0;
    game.kills = 0;
    game.medkits = 1;
    game.movePointer = game.aimPointer = game.firePointer = game.healPointer = -1;
    game.aimAssistTarget = -1;
    game.moveX = game.moveY = 0;
    game.reloading = game.healing = game.aiming = game.crouching = false;
    game.grounded = true;
    game.won = game.gameOver = false;
    selectWeapon(game, game.selectedWeapon);
    initializeGame(game);
}

void beginReload(Game& game) {
    const WeaponStats& weapon = weaponStats(game.selectedWeapon);
    if (game.reloading || game.magazine >= weapon.magazineSize || game.reserveAmmo <= 0) return;
    game.reloading = true;
    game.reloadTimer = weapon.reloadSeconds;
    game.audio.play(SoundEffect::Reload);
}

void updateGame(Game& game, float deltaTime) {
    if (!game.started || game.gameOver) return;
    const float dt = std::min(deltaTime, 0.05f);
    const float aimResponse = 1.0f - std::exp(-22.0f * dt);
    const float yawDifference = std::remainder(game.targetYaw - game.yaw, 2 * kPi);
    game.yaw += yawDifference * aimResponse;
    game.pitch += (game.targetPitch - game.pitch) * aimResponse;
    game.roundTime += dt;
    game.zoneRadius = std::max(12.0f, 160.0f - game.roundTime * 0.42f);
    const float zoneShift = std::clamp((game.roundTime - 55.0f) / 55.0f, 0.0f, 1.0f);
    game.zoneCenter = {35.0f * zoneShift, 0, -25.0f * zoneShift};

    const float moveLength = std::sqrt(game.moveX * game.moveX + game.moveY * game.moveY);
    const Vec3 right{std::cos(game.yaw), 0, std::sin(game.yaw)};
    const Vec3 forward{std::sin(game.yaw), 0, -std::cos(game.yaw)};
    const Vec3 moveDirection = right * game.moveX + forward * -game.moveY;
    const float moveScale = std::min(moveLength, 1.0f);
    const float targetSpeed = moveLength > 0.08f ? (game.crouching ? 4.0f : 7.5f) * moveScale : 0;
    const float response = 1.0f - std::exp(-(game.grounded ? 18.0f : 5.0f) * dt);
    game.playerVelocity.x += (moveDirection.x * targetSpeed - game.playerVelocity.x) * response;
    game.playerVelocity.z += (moveDirection.z * targetSpeed - game.playerVelocity.z) * response;
    game.playerVelocity.y -= 22.0f * dt;
    game.player = game.player + game.playerVelocity * dt;
    if (game.player.y <= 0) {
        game.player.y = 0;
        game.playerVelocity.y = 0;
        game.grounded = true;
    } else {
        game.grounded = false;
    }
    resolveWorldCollisions(game.player, game.playerVelocity, 0.42f);
    if (game.grounded && moveLength > 0.08f) {
        game.footstepTimer -= dt;
        if (game.footstepTimer <= 0) {
            game.audio.play(SoundEffect::Footstep);
            game.footstepTimer = 0.36f;
        }
    } else {
        game.footstepTimer = 0;
    }
    const float previousX = game.player.x;
    const float previousZ = game.player.z;
    game.player.x = std::clamp(game.player.x, -188.0f, 188.0f);
    game.player.z = std::clamp(game.player.z, -188.0f, 188.0f);
    if (game.player.x != previousX) game.playerVelocity.x = 0;
    if (game.player.z != previousZ) game.playerVelocity.z = 0;
    game.fireCooldown = std::max(0.0f, game.fireCooldown - dt);
    game.hitMarkerTimer = std::max(0.0f, game.hitMarkerTimer - dt);
    game.recoil = std::max(0.0f, game.recoil - dt * (game.aiming ? 0.3f : 0.48f));
    game.aim = lookDirection(game.yaw, game.pitch + game.recoil);

    const float acquireAlignment = game.aiming ? 0.975f : 0.988f;
    const float retainAlignment = game.aiming ? 0.945f : 0.968f;
    const Vec3 aimOrigin = game.player + Vec3{0, playerEyeHeight(game), 0};
    int assistedTarget = -1;
    float bestAlignment = -1.0f;
    if (game.aimAssistTarget >= 0 && game.aimAssistTarget < static_cast<int>(game.bots.size())) {
        const Bot& previousTarget = game.bots[game.aimAssistTarget];
        if (previousTarget.alive) {
            const Vec3 target = previousTarget.position + Vec3{0, 1.05f, 0};
            const Vec3 offset = target - aimOrigin;
            const float distance = length(offset);
            const float alignment = dot(normalized(offset), game.aim);
            if (distance < 42.0f && offset.y <= 0.03f && alignment >= retainAlignment) {
                assistedTarget = game.aimAssistTarget;
                bestAlignment = alignment;
            }
        }
    }
    if (assistedTarget < 0) {
        for (size_t i = 0; i < game.bots.size(); ++i) {
            const Bot& bot = game.bots[i];
            if (!bot.alive) continue;
            const Vec3 target = bot.position + Vec3{0, 1.05f, 0};
            const Vec3 offset = target - aimOrigin;
            const float distance = length(offset);
            const float alignment = dot(normalized(offset), game.aim);
            if (distance < 42.0f && offset.y <= 0.03f &&
                alignment >= acquireAlignment && alignment > bestAlignment) {
                assistedTarget = static_cast<int>(i);
                bestAlignment = alignment;
            }
        }
    }
    game.aimAssistTarget = assistedTarget;
    if (assistedTarget >= 0) {
        const Vec3 target = game.bots[assistedTarget].position + Vec3{0, 1.05f, 0};
        const Vec3 offset = target - aimOrigin;
        const float distance = std::max(length(offset), 0.001f);
        const float desiredYaw = std::atan2(offset.x, -offset.z);
        const float desiredPitch = std::min(
            std::asin(std::clamp(offset.y / distance, -0.95f, 0.95f)), -0.005f);
        const float magnetism = 1.0f - std::exp(-(game.aiming ? 3.8f : 1.8f) * dt);
        game.targetYaw += std::remainder(desiredYaw - game.targetYaw, 2 * kPi) * magnetism;
        game.targetPitch += (desiredPitch - game.targetPitch) * magnetism;
    }

    if (game.reloading) {
        game.reloadTimer -= dt;
        if (game.reloadTimer <= 0) {
            const int loaded = std::min(weaponStats(game.selectedWeapon).magazineSize - game.magazine,
                                        game.reserveAmmo);
            game.magazine += loaded;
            game.reserveAmmo -= loaded;
            game.reloading = false;
        }
    }
    if (game.healing) {
        game.healTimer -= dt;
        if (game.healTimer <= 0) {
            game.health = std::min(100.0f, game.health + 55.0f);
            --game.medkits;
            game.healing = false;
            game.healPointer = -1;
            game.audio.play(SoundEffect::Heal);
        }
    }

    if (game.firePointer >= 0 && !game.reloading && game.fireCooldown <= 0) {
        if (game.magazine == 0) beginReload(game);
        else {
            const WeaponStats& weapon = weaponStats(game.selectedWeapon);
            game.fireCooldown = weapon.fireInterval;
            --game.magazine;
            game.audio.play(SoundEffect::PlayerShot);
            const float movementSpread = moveLength > 0.3f ? (game.aiming ? 0.006f : 0.018f) : 0.0f;
            const float spread = (game.aiming ? weapon.adsSpread : weapon.hipSpread) + movementSpread;
            const float randomX = static_cast<float>(std::rand() % 2001) / 1000.0f - 1.0f;
            const float randomY = static_cast<float>(std::rand() % 2001) / 1000.0f - 1.0f;
            const Vec3 shotDirection = normalized({game.aim.x + randomX * spread,
                                                   game.aim.y + randomY * spread,
                                                   game.aim.z});
            spawnProjectile(game, game.player + Vec3{0, playerEyeHeight(game), 0} + shotDirection * 0.65f,
                            shotDirection, weapon.projectileSpeed, weapon.damage, true);
            game.recoil = std::min(0.3f, game.recoil + weapon.recoil);
        }
    }

    for (Bot& bot : game.bots) {
        if (!bot.alive) continue;
        const Vec3 towardPlayer = game.player - bot.position;
        const float distance = length(towardPlayer);
        const Vec3 direction = normalized(towardPlayer);
        bot.strafeTimer -= dt;
        if (bot.strafeTimer <= 0) {
            bot.strafeTimer = 1.2f + static_cast<float>(std::rand() % 100) / 100.0f;
            bot.strafeDirection = std::rand() % 2 == 0 ? -1.0f : 1.0f;
        }
        Vec3 desiredVelocity{};
        if (distance > 11.0f) {
            desiredVelocity = direction * 2.4f;
        } else if (distance < 7.0f) {
            desiredVelocity = direction * -1.3f;
        } else {
            const Vec3 strafe{-direction.z, 0, direction.x};
            desiredVelocity = strafe * (bot.strafeDirection * 1.5f);
        }
        const float botResponse = 1.0f - std::exp(-8.0f * dt);
        bot.velocity.x += (desiredVelocity.x - bot.velocity.x) * botResponse;
        bot.velocity.z += (desiredVelocity.z - bot.velocity.z) * botResponse;
        bot.position = bot.position + bot.velocity * dt;
        bot.position.y = 0;
        resolveWorldCollisions(bot.position, bot.velocity, 0.38f);
        bot.shotTimer -= dt;
        if (distance < 34.0f && bot.shotTimer <= 0) {
            bot.shotTimer = 1.8f + static_cast<float>(std::rand() % 100) / 100.0f;
            game.audio.play(SoundEffect::EnemyShot);
            const Vec3 origin = bot.position + Vec3{0, 1.0f, 0};
            const Vec3 target = game.player + Vec3{0, game.crouching ? 0.68f : 1.05f, 0};
            const float spreadX = static_cast<float>(std::rand() % 201) / 1000.0f - 0.1f;
            const float spreadY = static_cast<float>(std::rand() % 201) / 1000.0f - 0.1f;
            const float botDamage = game.selectedMode == 2 ? 12.0f : 7.0f;
            spawnProjectile(game, origin, normalized(target - origin + Vec3{spreadX, spreadY, 0}),
                            42.0f, botDamage, false);
        }
        if (length(bot.position - game.zoneCenter) > game.zoneRadius) {
            bot.health -= 4 * dt;
            if (bot.health <= 0) bot.alive = false;
        }
    }

    for (size_t i = 0; i < game.bots.size(); ++i) {
        if (!game.bots[i].alive) continue;
        for (size_t j = i + 1; j < game.bots.size(); ++j) {
            if (!game.bots[j].alive) continue;
            separateFromCircle(game.bots[i].position, game.bots[i].velocity,
                               game.bots[j].position.x, game.bots[j].position.z, 0.76f);
            separateFromCircle(game.bots[j].position, game.bots[j].velocity,
                               game.bots[i].position.x, game.bots[i].position.z, 0.76f);
        }
    }

    for (Bot& bot : game.bots) {
        if (bot.alive && length(bot.position - game.player) < 0.95f) {
            separateFromCircle(game.player, game.playerVelocity, bot.position.x, bot.position.z, 0.95f);
        }
    }
    resolveWorldCollisions(game.player, game.playerVelocity, 0.42f);

    for (Projectile& projectile : game.projectiles) {
        if (!projectile.active) continue;
        const Vec3 previous = projectile.position;
        projectile.velocity.y -= 5.2f * dt;
        projectile.position = projectile.position + projectile.velocity * dt;
        projectile.lifetime -= dt;
        if (projectile.lifetime <= 0 || std::abs(projectile.position.x) > 195 ||
            std::abs(projectile.position.z) > 195 || projectile.position.y < 0) {
            projectile.active = false;
            continue;
        }
        Vec3 collisionProbe = projectile.position;
        Vec3 collisionVelocity = projectile.velocity;
        resolveWorldCollisions(collisionProbe, collisionVelocity, 0.06f);
        if (length(collisionProbe - projectile.position) > 0.01f) {
            projectile.active = false;
            continue;
        }
        const Vec3 segment = projectile.position - previous;
        const float segmentLengthSquared = dot(segment, segment);
        if (projectile.playerOwned) {
            for (Bot& bot : game.bots) {
                if (!bot.alive) continue;
                const Vec3 target = bot.position + Vec3{0, 1.05f, 0};
                const float along = segmentLengthSquared > 0.0001f
                    ? std::clamp(dot(target - previous, segment) / segmentLengthSquared, 0.0f, 1.0f)
                    : 0.0f;
                const Vec3 closest = previous + segment * along;
                if (length(target - closest) > 0.58f) continue;
                bot.health -= projectile.damage;
                bot.velocity = bot.velocity + normalized(projectile.velocity) * 1.4f;
                game.hitMarkerTimer = 0.18f;
                game.audio.play(SoundEffect::Hit);
                if (bot.health <= 0) {
                    bot.alive = false;
                    ++game.kills;
                    game.audio.play(SoundEffect::Elimination);
                    spawnLoot(game, bot.position);
                }
                projectile.active = false;
                break;
            }
        } else {
            const Vec3 target = game.player + Vec3{0, 1.05f, 0};
            const float along = segmentLengthSquared > 0.0001f
                ? std::clamp(dot(target - previous, segment) / segmentLengthSquared, 0.0f, 1.0f)
                : 0.0f;
            const Vec3 closest = previous + segment * along;
            if (length(target - closest) < 0.62f) {
                game.health -= projectile.damage;
                game.playerVelocity = game.playerVelocity - normalized(projectile.velocity) * 1.5f;
                game.audio.play(SoundEffect::Damage);
                projectile.active = false;
            }
        }
    }

    for (LootDrop& drop : game.loot) {
        if (!drop.active || !drop.airborne || drop.collected) continue;
        drop.velocity.y -= 18.0f * dt;
        drop.position = drop.position + drop.velocity * dt;
        if (drop.position.y <= 0) {
            drop.position.y = 0;
            drop.velocity.y = -drop.velocity.y * 0.28f;
            drop.velocity.x *= 0.65f;
            drop.velocity.z *= 0.65f;
            if (drop.velocity.y < 0.9f) {
                drop.velocity = {};
                drop.airborne = false;
            }
        }
    }

    game.stormTimer -= dt;
    if (game.selectedMode != 1 && length(game.player - game.zoneCenter) > game.zoneRadius) {
        if (game.stormTimer <= 0) {
            game.health -= 5;
            game.stormTimer = 0.8f;
            game.audio.play(SoundEffect::Storm);
        }
    } else {
        game.stormTimer = 0;
    }

    for (LootDrop& drop : game.loot) {
        if (!drop.active || drop.collected || length(game.player - drop.position) > 2.2f) continue;
        if (drop.type == LootType::Ammo && game.reserveAmmo < 180) {
            game.reserveAmmo = std::min(180, game.reserveAmmo + 30);
            drop.collected = true;
            game.audio.play(SoundEffect::Pickup);
        } else if (drop.type == LootType::Medkit && game.medkits < 3) {
            ++game.medkits;
            drop.collected = true;
            game.audio.play(SoundEffect::Pickup);
        }
    }

    game.health = std::clamp(game.health, 0.0f, 100.0f);
    game.won = game.selectedMode != 1 &&
        std::none_of(game.bots.begin(), game.bots.end(), [](const Bot& bot) { return bot.alive; });
    game.gameOver = game.health <= 0 || game.won;
    if (game.gameOver) {
        game.coins += game.kills * 25;
        game.audio.play(game.won ? SoundEffect::Victory : SoundEffect::Defeat);
    }
}

int32_t handleInput(android_app* app, AInputEvent* event) {
    auto* game = static_cast<Game*>(app->userData);
    if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION) return 0;
    const int action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
    const size_t index = (AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
                         AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
    const int pointer = AMotionEvent_getPointerId(event, index);
    const float x = AMotionEvent_getX(event, index);
    const float y = AMotionEvent_getY(event, index);
    const float width = std::max(game->width, 1);
    const float height = std::max(game->height, 1);

    const bool pointerDown = action == AMOTION_EVENT_ACTION_DOWN ||
                             action == AMOTION_EVENT_ACTION_POINTER_DOWN;
    const bool pointerUp = action == AMOTION_EVENT_ACTION_UP ||
                           action == AMOTION_EVENT_ACTION_POINTER_UP ||
                           action == AMOTION_EVENT_ACTION_CANCEL;

    if (game->started && game->gameOver) {
        if (pointerDown && x > width * 0.35f && x < width * 0.65f &&
            y > height * 0.54f && y < height * 0.68f) {
            restartGame(*game);
        }
        return 1;
    }

    if (!game->started) {
        if (game->settingsOpen && game->hudEditMode) {
            if (pointerDown) {
                if (x > width * 0.72f && y < height * 0.23f) {
                    game->hudEditMode = false;
                    game->draggedHudControl = -1;
                } else {
                    game->draggedHudControl = hitHudControl(*game, x, y, width, height);
                    if (game->draggedHudControl >= 0) game->hudDragPointer = pointer;
                }
            } else if (action == AMOTION_EVENT_ACTION_MOVE) {
                for (size_t i = 0; i < AMotionEvent_getPointerCount(event); ++i) {
                    if (AMotionEvent_getPointerId(event, i) != game->hudDragPointer ||
                        game->draggedHudControl < 0) continue;
                    HudAnchor& anchor = game->hudAnchors[game->draggedHudControl];
                    anchor.x = std::clamp(AMotionEvent_getX(event, i) / width, 0.06f, 0.96f);
                    anchor.y = std::clamp(AMotionEvent_getY(event, i) / height, 0.12f, 0.94f);
                }
            } else if (pointerUp && pointer == game->hudDragPointer) {
                game->hudDragPointer = -1;
                game->draggedHudControl = -1;
            }
            return 1;
        }

        if (game->settingsOpen) {
            if (pointerDown && x > width * 0.25f && x < width * 0.75f &&
                y > height * 0.35f && y < height * 0.53f) {
                game->sensitivityPointer = pointer;
                setAimSensitivity(*game, x, width);
            } else if (pointerDown && x > width * 0.3f && x < width * 0.7f &&
                       y > height * 0.56f && y < height * 0.74f) {
                game->hudEditMode = true;
                game->audio.play(SoundEffect::Aim);
            } else if (pointerDown && y > height * 0.76f) {
                game->settingsOpen = false;
                game->audio.play(SoundEffect::Aim);
            } else if (action == AMOTION_EVENT_ACTION_MOVE) {
                for (size_t i = 0; i < AMotionEvent_getPointerCount(event); ++i) {
                    if (AMotionEvent_getPointerId(event, i) == game->sensitivityPointer) {
                        setAimSensitivity(*game, AMotionEvent_getX(event, i), width);
                    }
                }
            } else if (pointerUp && pointer == game->sensitivityPointer) {
                game->sensitivityPointer = -1;
            }
            return 1;
        }

        if (game->modeSelectOpen) {
            if (pointerDown) {
                if (y > height * 0.76f && x < width * 0.3f) {
                    game->modeSelectOpen = false;
                } else if (y > height * 0.76f && x > width * 0.7f) {
                    game->modeSelectOpen = false;
                    game->loadoutOpen = true;
                } else if (y > height * 0.28f && y < height * 0.72f) {
                    game->selectedMode = x < width * 0.34f ? 0 : (x < width * 0.66f ? 1 : 2);
                }
                game->audio.play(SoundEffect::Aim);
            }
            return 1;
        }

        if (game->skinSelectOpen) {
            if (pointerDown) {
                if (y > height * 0.76f && x < width * 0.3f) {
                    game->skinSelectOpen = false;
                } else if (y > height * 0.76f && x > width * 0.7f) {
                    game->skinSelectOpen = false;
                    game->storeOpen = true;
                } else if (y > height * 0.3f && y < height * 0.72f) {
                    const int skin = x < width * 0.25f ? 0 : (x < width * 0.5f ? 1
                        : (x < width * 0.75f ? 2 : 3));
                    if (game->ownedSkins[skin]) game->selectedSkin = skin;
                    else {
                        game->selectedStoreSkin = skin;
                        game->skinSelectOpen = false;
                        game->storeOpen = true;
                    }
                    game->audio.play(SoundEffect::Pickup);
                }
            }
            return 1;
        }

        if (game->storeOpen) {
            if (pointerDown) {
                if (y > height * 0.76f && x < width * 0.3f) {
                    game->storeOpen = false;
                    game->skinSelectOpen = true;
                } else if (y > height * 0.76f && x > width * 0.7f) {
                    const int skin = std::clamp(game->selectedStoreSkin, 1, 3);
                    const int price = skin == 1 ? 100 : (skin == 2 ? 150 : 200);
                    if (!game->ownedSkins[skin] && game->coins >= price) {
                        game->coins -= price;
                        game->ownedSkins[skin] = true;
                    }
                    if (game->ownedSkins[skin]) game->selectedSkin = skin;
                    game->storeOpen = false;
                    game->skinSelectOpen = true;
                    game->audio.play(SoundEffect::Pickup);
                } else if (y > height * 0.28f && y < height * 0.72f) {
                    game->selectedStoreSkin = x < width * 0.375f ? 1 : (x < width * 0.625f ? 2 : 3);
                }
            }
            return 1;
        }

        if (game->loadoutOpen) {
            if (pointerDown) {
                if (x < width * 0.22f && y > height * 0.76f) {
                    game->loadoutOpen = false;
                    game->modeSelectOpen = true;
                } else if (x > width * 0.32f && x < width * 0.68f && y > height * 0.74f) {
                    game->started = true;
                    game->audio.play(SoundEffect::Aim);
                } else if (y > height * 0.28f && y < height * 0.72f) {
                    const int selection = x < width * 0.34f ? 0 : (x < width * 0.66f ? 1 : 2);
                    selectWeapon(*game, selection);
                    game->audio.play(SoundEffect::Pickup);
                }
            }
            return 1;
        }

        if (pointerDown && y > height * 0.38f && y < height * 0.53f && x < width * 0.25f) {
            game->skinSelectOpen = true;
            game->audio.play(SoundEffect::Aim);
        } else if (pointerDown && y > height * 0.52f && y < height * 0.66f && x < width * 0.25f) {
            game->storeOpen = true;
            game->audio.play(SoundEffect::Aim);
        } else if (pointerDown && y > height * 0.66f && y < height * 0.81f && x < width * 0.25f) {
            game->settingsOpen = true;
            game->audio.play(SoundEffect::Aim);
        } else if (pointerDown && y > height * 0.75f && x > width * 0.68f) {
            game->modeSelectOpen = true;
            game->audio.play(SoundEffect::Aim);
        }
        return 1;
    }

    if (pointerDown) {
        const int control = hitHudControl(*game, x, y, width, height);
        if (control == FireHud) {
            game->firePointer = pointer;
        } else if (control == JumpHud) {
            if (game->grounded) {
                game->playerVelocity.y = 8.5f;
                game->grounded = false;
                game->audio.play(SoundEffect::Jump);
            }
        } else if (control == AimHud) {
            game->aiming = !game->aiming;
            game->audio.play(SoundEffect::Aim);
        } else if (control == CrouchHud) {
            game->crouching = !game->crouching;
            game->audio.play(SoundEffect::Aim);
        } else if (control == ReloadHud) {
            beginReload(*game);
        } else if (control == MedHud) {
            if (game->medkits > 0 && game->health < 100 && !game->healing) {
                game->healPointer = pointer;
                game->healing = true;
                game->healTimer = 1.6f;
            }
        } else if (control == MoveHud || (x < width * 0.42f && y > height * 0.35f)) {
            game->movePointer = pointer;
            game->moveOriginX = x;
            game->moveOriginY = y;
            game->moveX = 0;
            game->moveY = 0;
        } else if (x > width * 0.48f && game->aimPointer < 0) {
            game->aimPointer = pointer;
            game->aimStartX = x;
            game->aimStartY = y;
        }
    } else if (action == AMOTION_EVENT_ACTION_MOVE) {
        for (size_t i = 0; i < AMotionEvent_getPointerCount(event); ++i) {
            const int id = AMotionEvent_getPointerId(event, i);
            const float px = AMotionEvent_getX(event, i);
            const float py = AMotionEvent_getY(event, i);
            if (id == game->movePointer) {
                game->moveX = std::clamp((px - game->moveOriginX) / (width * 0.11f), -1.0f, 1.0f);
                game->moveY = std::clamp((py - game->moveOriginY) / (height * 0.15f), -1.0f, 1.0f);
            }
            if (id == game->aimPointer) {
                const float dx = px - game->aimStartX;
                const float dy = py - game->aimStartY;
                const float yawSensitivity = (game->aiming ? 1.65f : 3.2f) * game->aimSensitivity;
                const float pitchSensitivity = (game->aiming ? 1.2f : 2.1f) * game->aimSensitivity;
                game->targetYaw += dx / width * yawSensitivity;
                game->targetPitch = std::clamp(game->targetPitch - dy / height * pitchSensitivity,
                                               -0.55f, 0.45f);
                game->aimStartX = px;
                game->aimStartY = py;
            }
        }
    } else if (action == AMOTION_EVENT_ACTION_UP || action == AMOTION_EVENT_ACTION_POINTER_UP ||
               action == AMOTION_EVENT_ACTION_CANCEL) {
        if (pointer == game->movePointer) {
            game->movePointer = -1;
            game->moveX = game->moveY = 0;
        }
        if (pointer == game->aimPointer) game->aimPointer = -1;
        if (pointer == game->firePointer) game->firePointer = -1;
        if (pointer == game->healPointer) {
            game->healPointer = -1;
            game->healing = false;
            game->healTimer = 0;
        }
    }
    return 1;
}

} // namespace

void android_main(android_app* app) {
    Game game;
    game.audio.initialize();
    initializeGame(game);
    app->userData = &game;
    app->onInputEvent = handleInput;
    auto previousTime = std::chrono::steady_clock::now();
    bool rendererReady = false;

    while (true) {
        int events = 0;
        android_poll_source* source = nullptr;
        ALooper_pollOnce(rendererReady ? 0 : -1, nullptr, &events,
                         reinterpret_cast<void**>(&source));
        if (source) source->process(app, source);
        if (app->destroyRequested) {
            renderer.shutdown();
            return;
        }
        if (app->window && !rendererReady) {
            rendererReady = renderer.initialize(app->window);
            previousTime = std::chrono::steady_clock::now();
        } else if (!app->window && rendererReady) {
            renderer.shutdown();
            rendererReady = false;
        }
        if (rendererReady) {
            const auto currentTime = std::chrono::steady_clock::now();
            const float dt = std::chrono::duration<float>(currentTime - previousTime).count();
            previousTime = currentTime;
            updateGame(game, dt);
            renderer.render(game);
        }
    }
}