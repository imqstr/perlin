#pragma once
#include "network/FriendWorld.hpp"
#include "network/LanDiscovery.hpp"
#include <array>
#include <mutex>
#include "client/audio/SoundSystem.hpp"
#include "client/audio/Subtitles.hpp"
#include <chrono>
#include "client/Accounts.hpp"
#include "client/Camera.hpp"
#include "client/Options.hpp"
#include "client/ClientControl.hpp"
#include "client/entity/EntityRenderer.hpp"
#include "client/renderer/WeatherRenderer.hpp"
#include "client/hand/HandRenderer.hpp"
#include "client/input/Input.hpp"
#include "client/input/InputManager.hpp"
#include "client/multiplayer/BedrockSession.hpp"
#include "client/replay/ExportConfig.hpp"
#include "client/replay/Ffmpeg.hpp"
#include "client/replay/FfmpegDownload.hpp"
#include "client/replay/ReplayController.hpp"
#include "client/replay/Timeline.hpp"
#include "client/particle/ParticleSystem.hpp"
#include "client/particle/DataParticles.hpp"
#include "client/renderer/Fog.hpp"
#include "client/renderer/FogDefs.hpp"
#include "client/player/LocalPlayer.hpp"
#include "client/renderer/LevelRenderer.hpp"
#include "client/renderer/Lightmap.hpp"
#include "client/renderer/RenderContext.hpp"
#include "client/ui/TitleScreen.hpp"
#include "client/FrameGenPacer.hpp"
#include "client/ui/ChestScreen.hpp"
#include "client/ui/FormScreen.hpp"
#include "client/ui/ChatLog.hpp"
#include "client/ui/Hud.hpp"
#include "client/ui/InventoryScreen.hpp"
#include "client/mods/ModManager.hpp"
#include "util/ThreadPool.hpp"

#include <atomic>
#include <deque>
#include <condition_variable>
#include <vector>
#include <future>
#include "client/resources/PackInfo.hpp"
#include <memory>
#include <map>
#include <set>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

class AppPlatform;

namespace perlin {
    struct LocalBreakState { bool active = false; int x = 0, y = 0, z = 0; float progress = 0.0f; };
    LocalBreakState GetLocalBreakState();

    struct LaunchArgs {
        std::string server;
        std::string playerName;
        std::string skin;
        std::string cape;
        bool slimArms = false;
        bool login = false;
        bool offline = false;
        std::string transport;
        bool logout = false;
        bool secure = false;
        bool acceptEula = false;
        std::string stun;
        bool hidden = false;
        int controlPort = 0;
    };

    class Perlin {
    public:
        explicit Perlin(LaunchArgs args) : args(std::move(args)) {}

        bool Initialize(AppPlatform* platform);
        void Shutdown();

        Input& GetInput() { return input; }

    private:
        void RenderThreadMain();
        void PollControl();
        void PublishControl();
        void TraceControlTick(uint64_t tick, const physics::State& state, const physics::Input& input);
        std::shared_ptr<ClientControl> control;
        ControlSnapshot controlState;
        void FillHudState(ui::HudState& state, BedrockSession* live, bool preview);
        void Connect();
        bool AcceptEula();
        void TakeScreenshot();
        void UpdateBedScreen(ui::Hud& hud, BedrockSession* live, int fbW, int fbH);
        bool bedShown = false, bedLeftDown = false;
        BedrockSession::ScreenAnimation screenAnimation;
        bool creditsShown = false;
        double creditsScroll = 0;
        std::string creditsSignature;
        std::chrono::steady_clock::time_point creditsLast{};
        void UpdateCreditsScreen(ui::Hud& hud, BedrockSession* live, int fbW, int fbH);
        std::string bedSignature;
        void UpdateDeathScreen(ui::Hud& hud, BedrockSession* live, const ui::HudState& state, int fbW, int fbH);
        bool deathShown = false, deathLeftDown = false, deathRespawnSent = false, deathTextMode = false;
        void UpdateFormScreen(ui::Hud& hud, BedrockSession* live, int fbW, int fbH);
        void UpdateServerLayer(ui::Hud& hud, BedrockSession* live, int fbW, int fbH);
        bool formShown = false, formLeftDown = false, uiClickWasDown = false;
        ui::FormModel formModel;
        ui::FormUi formUi;
        ui::ChestUi chestUi;
        bool formEngine = false;
        bool formEngineFailed = false;  // the json ui could not draw this form: stay on the built-in layout
        std::string formSignature, serverLayerSignature;
        float formScroll = 0.0f, formContentH = 0.0f, formViewH = 0.0f;
        std::chrono::steady_clock::time_point formOpenedAt{};
        double toastUntil = 0.0;
        std::string toastText, lastToastTitle, lastToastMessage;
        std::chrono::steady_clock::time_point toastAt{};
        std::chrono::steady_clock::time_point deathAt{}, deathSentAt{};
        std::string deathSignature, deathTemplate;
        uint64_t deathTemplateVersion = 0;
        bool deathHoverLogged = false;
        std::chrono::steady_clock::time_point deathCheckAt{};
        void UpdateCreativeScreen(ui::Hud& hud, BedrockSession* live, const ui::HudState& state, int fbW, int fbH);
        void UpdateInventoryScreen(ui::Hud& hud, BedrockSession* live, const ui::HudState& state, int fbW, int fbH);
        bool invShown = false, invLeftDown = false, invRightDown = false, invPreviewOpened = false;
        float invMouseX = 0, invMouseY = 0;
        int creativeTab = 0;
        bool creativeDollVisible = false;
        float creativeScroll = 0.0f;
        bool creativeDragging = false;
        std::string creativeSearch, creativeSignature, creativeTemplate;
        uint64_t creativeTemplateVersion = 0;
        std::optional<entity::Entity> dollSelf;
        int invHoverAddr = -1;
        bool invWorkbench = false, invBook = false, invHorse = false, invChest = false;
        std::chrono::steady_clock::time_point invMountAsk{};
        int invDragButton = 0;
        bool invMiddleDown = false;
        std::vector<int> invDragSlots;
        int invBookPage = 0;
        std::string invHoverId;
        std::vector<ui::RecipeCard> invCards;
        std::vector<BedrockSession::RecipeEntry> invRecipeAll;
        std::chrono::steady_clock::time_point invRecipeAt{};
        const Level* particleLevel = nullptr;
        int invPicked = -1, invHover = -1;
        std::chrono::steady_clock::time_point invOpenedAt{}, invCheckAt{};
        std::string invSignature, invTemplate;
        float invPanel[4] = {};
        bool invJsonPanel = false, invCloseClicked = false;
        std::string invChestScreen, invChestProblem;
        uint64_t invTemplateVersion = 0;
        void UpdatePauseScreen(ui::Hud& hud, BedrockSession* live, int fbW, int fbH);
        bool UpdateDisconnectScreen(ui::Hud& hud, int fbW, int fbH);
        bool UpdateLoadingScreen(ui::Hud& hud, int fbW, int fbH);
        bool UpdateTitleScreen(ui::Hud& hud, int fbW, int fbH);
        void JoinServer(const std::string& address);
        bool UpdateAssetScreen(ui::Hud& hud, int fbW, int fbH);
        bool assetShown = false, assetSkip = false, assetLeftDown = false;
        std::string assetSignature;
        std::chrono::steady_clock::time_point assetOpenedAt{}, assetSampleAt{};
        uint64_t assetLastBytes[2] = {0, 0};
        double assetSpeed[2] = {0.0, 0.0};
        void StartAssetDownloads();
        void LeaveToServerList(ui::Hud& hud);
        bool titleReturnToServers = false;
        bool loadingLeftDown = false, loadingTextMode = false;
        std::chrono::steady_clock::time_point loadingOpenedAt{};
        std::chrono::steady_clock::time_point logoRetryAt{};
        bool logoBedrock = false;
        bool loadingShown = false, loadingDone = false;
        std::string loadingSignature, loadingTemplate;
        uint64_t loadingTemplateVersion = 0;
        std::chrono::steady_clock::time_point loadingCheckAt{}, loadingReadyAt{}, loadingStartAt{};
        bool loadingReadyArmed = false;
        int loadingChunkBase = 0;
        int lastDimId = -1;
        float portalTime = 0.0f;
        bool titleActive = false, titleShown = false, titleLeftDown = false;
        ui::TitleModel title;
        std::string titleSignature;
        std::chrono::steady_clock::time_point titleOpenedAt{}, titleClickAt{};
        bool titleOptions = false;
        int titleOpenView = 1;
        void DrawTitlePanorama(int fbW, int fbH);
        uint32_t panoramaTextures[6] = {};
        bool panoramaReady = false;
        uint64_t panoramaPackStamp = 0;
        FrameGenPacer fgPacer;
        int fgActive = 1;
        int FrameGenMultiplier();
        std::chrono::steady_clock::time_point panoramaClock{}, panoramaRetryAt{};
        int titleClickRow = -1;
        ui::TitlePage titleLastPage = ui::TitlePage::Main;

        AccountList accounts;
        void ApplyActiveAccount();
        std::string ActiveAuthPath() const;
        void PrewarmSignIn();
        void StartAccountSignIn();
        void CancelAccountSignIn();
        std::mutex signInMutex;
        std::shared_ptr<xbox::Account> signInAccount;
        std::string signInAuthFile, signInUrl, signInCode, signInStatus, signInGamertag;
        std::vector<std::pair<std::string, std::string>> pendingRenames;
        void FetchMissingGamertags();
        std::chrono::steady_clock::time_point gamertagFetchAt{};
        std::atomic<int> signInState{0};

        void PingServers(const std::vector<std::string>& addresses);
        std::mutex pingMutex;
        std::map<std::string, ui::ServerEntry> pingResults;

        void RefreshTab(ui::ServerTab tab);
        void JoinRealm(long long id);
        void JoinFriend(const ui::ServerEntry& entry);
        void BeginLeaseJoin(const std::string& text);
        void FailLeaseJoin(const std::string& text);
        std::atomic<bool> leaseJoining{false}, leaseJoinFailed{false};
        void JoinLanGame(const ui::ServerEntry& entry);
        std::shared_ptr<lan::Session> lanSession;
        std::mutex lanSessionMutex;
        void ReleaseFriendLease();
        std::shared_ptr<friendworld::Lease> friendLease;
        std::shared_ptr<friendworld::Lease> friendLeaseReady;
        std::atomic<bool> friendJoinBusy{false};
        void AddRealm(const std::string& code);
        std::atomic<bool> realmAddBusy{false}, realmAddedFlag{false};
        std::chrono::steady_clock::time_point realmAddedAt{};
        void SetJoinMessage(const std::string& text);
        std::mutex tabMutex;
        std::vector<ui::ServerEntry> tabLan, tabRealms, tabFriends, tabFeatured;
        std::atomic<bool> featuredBusy{false}, featuredPing{false};
        std::chrono::steady_clock::time_point featuredAt{};
        void RefreshFeatured(bool force);
        std::string tabLanMsg, tabRealmsMsg, tabFriendsMsg, tabJoinMsg, realmJoinAddress;
        bool tabDirty = false, realmJoinReady = false;
        std::atomic<bool> lanBusy{false}, realmsBusy{false}, friendsBusy{false}, realmJoinBusy{false};
        bool disconnectShown = false, disconnectLeftDown = false, disconnectTextMode = false;
        std::chrono::steady_clock::time_point disconnectOpenedAt{}, disconnectCheckAt{};
        std::string disconnectSignature, disconnectTemplate;
        uint64_t disconnectTemplateVersion = 0;
        bool pauseShown = false, pauseLeftDown = false, pausePreviewOpened = false;
        bool pausePlayers = false;
        int pauseView = 0, optionsDrag = -1, optionsPage = 0, optionsSub = 0, bindCapture = -1;
        int controlsTab = 0;
        int bindDevice = 0;
        bool touchEditing = false;
        bool touchEditHidden = false;
        const Binding& BindingOf(Action a) const { return bindDevice == 1 ? options.GetPadKey(a) : options.GetKey(a); }
        void SetBindingOf(Action a, const Binding& b) { if (bindDevice == 1) options.SetPadKey(a, b); else options.SetKey(a, b); }
        bool bindWasCapturing = false;
        float kbScroll = 0.0f, optScroll = 0.0f;
        std::atomic<bool> fontsDownloaded{false};
        int optScrollKey = -1;
        int wardLine = 0;
        bool wardSkinOpen = false, wardCapeOpen = false, wardDrag = false;
        float wardYaw = -25.0f, wardPitch = 0.0f, wardDragX = 0.0f, wardDragY = 0.0f;
        int wardPreviewSerial = 0;
        std::string wardPreviewKey;
        std::vector<std::string> wardSkins, wardCapes;
        std::chrono::steady_clock::time_point wardListedAt{};
        std::unordered_map<std::string, std::string> wardStamp;
        std::unordered_map<std::string, bool> wardSlimGuess;
        std::string wardSkin, wardCape, wardPreviewSource;
        bool wardInit = false;
        std::map<std::string, bool> wardSlim;
        LoadedSkin wardPreviewLoaded;
        SkinConfig MakeSkinConfig(const std::string& skinPath, const std::string& capePath) const;
        int optHoverRow = -1;
        std::chrono::steady_clock::time_point optHoverSince{};
        std::map<std::string, std::pair<int, double>> worldOverride;
        float packScrollA = 0.0f, packScrollS = 0.0f, shaderScroll = 0.0f, modScroll = 0.0f, modDetailScroll = 0.0f;
        std::string modSelected, modSearch, shaderPendingPack, packMetaStampDir;
        bool shaderPendingEnabled = false, shaderPendingInit = false;
        struct PackMetaCacheEntry {
            std::string stamp;
            PackMeta meta;
        };
        std::map<std::string, PackMetaCacheEntry> packMetaCache;
        std::map<std::string, std::string> modIconStamp;
        std::vector<std::string> packFoundCache;
        std::chrono::steady_clock::time_point packFoundAt{};
        int packRowsHover = -1;
        void ApplyTexturePacks();
        void UnloadServerPacks(bool leavingLevel = false);
        bool levelUnloadPending = false;
        void CommitTexturePacks();
        std::vector<std::string> packPending;
        bool packPendingInit = false;
        void ApplyShaderPack();
        void PollJavaPack(BedrockSession* live);
        std::string javaPackPending;
        int javaPackDimension = 0;
        struct HotStamps {
            std::string tex, shader;
        };
        void PollHotReload();
        std::future<HotStamps> hotScan;
        std::chrono::steady_clock::time_point hotPollAt{};
        std::string hotTexStamp, hotShaderStamp, shaderBuildReport;
        std::chrono::steady_clock::time_point bindStartedAt{};
        std::string optionsSignature;
        std::chrono::steady_clock::time_point pauseOpenedAt{}, pauseCheckAt{};
        std::string pauseSignature, pauseTemplate;
        uint64_t pauseTemplateVersion = 0;
        void UpdateChatClicks(ui::Hud& hud, BedrockSession* live, int fbW, int fbH);
        bool chatLeftDown = false;
        uint32_t commandsSeen = 0;
        std::chrono::steady_clock::time_point playersAt{};
        void UpdateTitle(int fps);
        void SetupMods();
        void SetupModActions();
    void ToggleMod(const std::string& name);
        int modOpenView = -1, modOpenPage = 0;
        std::string modSettingsFor;
    std::vector<std::string> modRowTips;
        bool modSettingsReload = false;
        void PumpMods(BedrockSession* live, Level* level, ui::Hud* hudPtr, float dt);
        void TickMods(BedrockSession* live);
        bool InterceptOutgoing(BedrockSession* live, const std::string& text);
        bool FilterIncomingChat(const std::string& line);
        void UpdateModLayer(ui::Hud& hud);
        void ApplyModHud(ui::HudState& s);
        void NoteModSound(const std::string& name, double x, double y, double z, float volume, float pitch);
        void MergePluginCommands(BedrockSession* live);
        void StartModHttp(const std::string& plugin, uint32_t id, std::string method, std::string url, int timeoutMs,
                          std::vector<std::pair<std::string, std::string>> headers, std::vector<uint8_t> body, std::string savePath = {});
        abi::Screen CurrentScreen() const;
        BedrockSession* modLive = nullptr;
        Level* modLevel = nullptr;
        ui::Hud* modHud = nullptr;
        float modDt = 0.0f;
        int modGuiW = 0, modGuiH = 0;
        std::mutex modMutex;
        struct ModAdvanced {
            std::mutex mutex;
            abi::CameraControlState camera;
            std::chrono::steady_clock::time_point cameraUntil{};
            abi::ModelPose pose;
            bool lookTaken = false;
            glm::dvec3 lookDir{0.0, 0.0, -1.0};
            std::chrono::steady_clock::time_point poseUntil{};
            struct DrawSet {
                std::chrono::steady_clock::time_point until{};
                std::vector<entity::EntityRenderer::FrameIn::ModLine> lines;
            };
            std::map<std::string, DrawSet> lines;
            std::map<std::string, std::pair<std::chrono::steady_clock::time_point, std::vector<uint32_t>>> drops;
            std::map<std::string, std::pair<std::chrono::steady_clock::time_point, std::vector<uint32_t>>> outDrops;
            std::array<float, 16> viewProjection{};
            std::vector<entity::EntityRenderer::FrameIn::ModLine> frameLines;
        } modAdv;
        void SetupModAdvanced();
        void ApplyModCamera(float& fov, glm::mat4& viewEffect);
        // mod hooks (PerlinModsHooks.cpp): keyed by plugin, dropped on unload
        struct ModHooks {
            using TimePoint = std::chrono::steady_clock::time_point;
            std::mutex mutex;
            std::map<std::string, std::map<abi::HudElement, abi::HudElementState>> hud;
            std::map<std::string, abi::DebugLinesData> debug;
            std::pair<TimePoint, abi::FogOverride> fog{};
            std::pair<TimePoint, abi::ScreenTintData> tint{};
            struct EntityRule {
                TimePoint until{};
                abi::EntityOverride rule;
            };
            std::map<std::string, std::map<std::string, EntityRule>> entities;  // plugin -> "id:N" / "type:X"
            struct Mesh {
                TimePoint until{};
                abi::WorldMeshData data;
            };
            std::map<std::string, Mesh> meshes;  // "plugin/id"
            std::map<std::string, abi::FrameLimitData> frame;
            struct Button {
                std::string plugin;
                abi::ScreenButtonDef def;
            };
            std::vector<Button> buttons;
            std::string customOwner, customTitle;
            bool customOpen = false;
            std::vector<std::string> tooltipLines;
            std::set<std::string> loadingHidden;
            std::map<std::string, abi::SkinOverrideData> skins;
            uint64_t skinSerial = 0;
            std::map<std::string, abi::AmbientSoundData> ambient;  // "plugin/id"
            std::vector<std::pair<std::string, abi::EventServerPing>> pings;
            std::atomic<uint32_t> nextPing{1};
            std::vector<std::pair<std::string, std::string>> packs;  // plugin, absolute path
            std::vector<abi::ParticleRequest> particles;
            bool windowFocused = true, windowMinimized = false;
        } modHooks;
        void SetupModHooks();
        void DropModHooks(const std::string& plugin);
        bool ModHudHidden(abi::HudElement element, float& dx, float& dy);
        void ApplyModDebugLines(std::vector<std::string>& left, std::vector<std::string>& right);
        void ApplyModFog(FogState& fog);
        void DrawModTint(int fbW, int fbH);
        int ModFrameLimit(int fps) const;
        void PumpModHooks(BedrockSession* live);
        void ApplyModSkins(std::vector<entity::Entity>& entities);
        void PrepareModRender(entity::EntityRenderer::FrameIn& in);
        std::vector<entity::EntityRenderer::FrameIn::ModMesh> modFrameMeshes;
        struct ScreenButtonView {
            abi::ScreenButtonDef def;
            bool hover = false;
        };
        std::vector<ScreenButtonView> modScreenButtons;
        bool modLayerOnScreen = false;
        std::string modLayerButtons;
        std::unordered_map<uint64_t, entity::EntityRenderer::FrameIn::ModStyle> modFrameStyles;
        std::unordered_map<std::string, entity::EntityRenderer::FrameIn::ModStyle> modFrameTypeStyles;
        std::map<std::string, std::shared_ptr<const entity::SkinData>> modSkinCache;
        uint64_t modSkinCacheSerial = 0;
        bool DispatchModInteract(abi::InteractKind kind, int x, int y, int z, int face, uint64_t entity);  // true: a mod cancelled
        bool DispatchModSlotClick(int slot, bool right, bool shift);
        bool DispatchModToast(const std::string& title, const std::string& body);
        std::vector<std::string> ModTooltipLines(const ui::HudItem& item);
        std::vector<std::pair<std::string, abi::EventHttp>> modHttp;
        std::atomic<int> modHttpActive{0};
        std::vector<abi::EventSound> modSounds;
        std::map<std::string, bool> modKeyDown;
        std::atomic<bool> modWantPackets{false};
        bool modWasConnected = false, modWasDead = false;
        bool modGrabMouseWas = false, modGrabKeyboardWas = false;
        std::atomic<int> modFbW{0}, modFbH{0};
        std::mutex modNativeMutex;
        abi::NativeHandles modNative;
        bool modNativeReady = false;
        struct ModGfxTexture {
            uint32_t gpu = 0;
            uint64_t version = 0;
        };
        std::unordered_map<std::string, ModGfxTexture> modGfxTextures;
        void PumpModInput(mods::ModManager& mm);
        void DrawModGfx(int fbW, int fbH);
        int modLastDim = 0, modLastSlot = -1;
        uint32_t modLastHurt = 0, modTitleSerial = 0;
        abi::Screen modLastScreen = abi::Screen::None;
        std::unordered_set<uint64_t> modEntityIds;
        bool modEntitySeeded = false;
        std::string modToastText, modTitleText, modSubtitleText;
        std::chrono::steady_clock::time_point modToastUntil{}, modTitleStart{};
        int modTitleFadeIn = 500, modTitleStay = 3500, modTitleFadeOut = 1000;
        uint64_t modLayerVersion = 0;
        int modLayerW = 0, modLayerH = 0;
        bool modLayerShown = false;
        std::string modCommandSig;
        uint32_t modCommandsVersion = 0;
        std::vector<std::pair<uint32_t, std::vector<uint8_t>>> modPackets;
        std::chrono::steady_clock::time_point modLastFrame{};
        double modTickAccum = 0.0;
        void FillDebug(ui::HudState& state, BedrockSession* live, Level* level);
        bool debugOn = false;
        int fpsShown = 0;
        uint64_t rateTx = 0, rateRx = 0;
        int txRate = 0, rxRate = 0;
        std::chrono::steady_clock::time_point rateAt{};
        int heightSurface = 0;
        int dbgSky = 15, dbgBlock = 0;
        std::chrono::steady_clock::time_point heightAt{};

        LaunchArgs args;
        AppPlatform* platform = nullptr;
        void VibrateBlockBroken();
        Options options;
        std::string runningRenderer;
        Input input;
        InputManager inputManager{input, options};

        std::unique_ptr<RenderContext> renderer;
        std::unique_ptr<LevelRenderer> levelRenderer;
        std::unique_ptr<ui::Hud> hud;
        ui::HudState hudState;
        HandRenderer handRenderer;
        entity::EntityRenderer entityRenderer;
        WeatherRenderer weather;
        particle::ParticleSystem particles;
        particle::DataParticles dataParticles;
        FogDefs fogDefs;
        double particleClock = 0;
        std::vector<particle::Quad> particleQuads;
        struct LocalBreak {
            int x = 0, y = 0, z = 0;
            std::chrono::steady_clock::time_point at{};
        } lastLocalBreak;
        audio::AudioEngine audioEngine;
        audio::SoundSystem sounds{audioEngine};
        audio::Subtitles subtitles;
        void UpdateAudio(BedrockSession* live, double now, float dt);
        entity::EntityTracker previewEntities;
        bool previewEntitiesSpawned = false;
        std::vector<uint64_t> previewWalk;
        void SpawnPreviewEntities();
        std::shared_ptr<const entity::SkinData> selfSkinData;
        std::string selfSkinKey;
        struct EntityLightEntry {
            std::chrono::steady_clock::time_point at{};
            glm::vec3 colour{1.0f};
            int sky = 0, block = 0;
        };
        std::unordered_map<int64_t, EntityLightEntry> entityLight;
        ui::ChatLog chat;
        std::chrono::steady_clock::time_point chatOpenedAt{};
        struct HudAnim {
            uint64_t ticks = 0;
            int toolTimer = 0;
            ui::HudItem lastTool;
            std::chrono::steady_clock::time_point titleAtSeen{}, actionAtSeen{};
            int overlayTime = 0, titleTime = 0, titleFadeIn = 10, titleStay = 70, titleFadeOut = 20;
            bool haveHealth = false;
            int lastHealth = 0;
            int lastHealthTick = 0, lastHurtTick = -1000;
            int blinkUntil = 0, displayHealth = 0;
        } hudAnim;
        float hudPartial = 0.0f;
        bool headUnderwater = false;
        bool headInLava = false;
        float waterSurfaceAbove = 0.0f;
        int previewSlot = 0;
        void TickHud();
        std::unique_ptr<ThreadPool> workers;
        std::thread renderThread;
        std::atomic<bool> renderRunning{false};
        std::atomic<bool> renderReady{false};

        Camera camera;
        LocalPlayer player;
        BlockLightFlicker blockLight;
        double tickDebt = 0.0;
        float rainLevel = 0.0f;
        float thunderLevel = 0.0f;
        float previousRainLevel = 0.0f;
        float previousThunderLevel = 0.0f;

        replay::ReplayController replayCtl;
        void ToggleReplayRecording(bool start);
        void FinishReplayRecording(const char* why);
        struct ReplayUi {
            bool menuOpen = true, grab = false, hideHud = true, uiHidden = false, camInit = false, leftDown = false, scrubbing = false, dirty = false;
            glm::vec3 flyVel{0.0f};
            bool showPlayers = true, showEntities = true, showNametags = true, editorOverUi = false;
            float editorViewX0 = 0, editorViewY0 = 0, editorViewX1 = 1e9f, editorViewY1 = 1e9f;
            double lastScrubSeekMs = -1e9;
            std::chrono::steady_clock::time_point lastScrubAt{};
            double scrubMs = -1, flySpeed = 16.0, inMs = -1, outMs = -1;
            int speedIdx = 3;
            std::string signature, status;
            std::chrono::steady_clock::time_point openedAt{}, statusUntil{};
            bool keyPrev[256] = {};
            float roll = 0, fovOverride = -1;
        } replayUi;
        replay::Timeline timeline;
        bool timelineDirty = false;

        struct KeyframeUi {
            bool open = false;
            int tab = 0, scroll = 0;
            int sel[3] = {-1, -1, -1};
            int interp = 0;
            float fov = 70, roll = 0, speed = 1.0f, timeTicks = 6000;
            std::string signature;
            bool leftDown = false;
            std::chrono::steady_clock::time_point openedAt{};
        } kfUi;
        bool UpdateKeyframePanel(ui::Hud& hud, int fbW, int fbH);
        struct EditorUi {
            bool open = false, leftDown = false;
            double viewStart = 0, viewSpan = 0;
            int selTrack = -1, selIndex = -1, sideScroll = 0;
            int drag = 0, dragTrack = -1;
            double dragOffset = 0;
            double lastSeekMs = -1e9;
            std::chrono::steady_clock::time_point lastSeekAt{}, openedAt{};
            std::string signature;
            bool keyPrev[256] = {};
        } editorUi;
        bool UpdateEditor(ui::Hud& hud, int fbW, int fbH);
        void LoadTimeline();
        void SaveTimeline();

        struct ExportRequest {
            replay::ExportConfig cfg;
            bool screenshot = false;
        };
        struct ExportUi {
            bool open = false, cfgLoaded = false, catQueried = false, leftDown = false;
            int tab = 0, scroll = 0;
            std::string focus, signature, status;
            std::map<std::string, std::string> text;
            replay::ExportConfig cfg;
            replay::ffmpeg::Catalog cat;
            std::unique_ptr<replay::ffmpeg::Downloader> downloader;
            std::vector<ExportRequest> queue;
            std::chrono::steady_clock::time_point openedAt{}, caretAt{};
            bool caret = true;
            bool downloadHandled = false;
        } exportUi;
        struct ExportJob {
            bool offscreen = false;
            bool active = false, screenshot = false, useFfmpeg = true, imageSeq = false, finishing = false, advanceNow = false;
            replay::ExportConfig cfg;
            replay::ffmpeg::Settings fs;
            std::unique_ptr<replay::ffmpeg::Encoder> encoder;
            std::string outPath, name, error;
            int renderW = 0, renderH = 0, outW = 0, outH = 0;
            double startMs = 0, endMs = 0, targetMs = 0, stepMs = 0;
            long long frame = 0, total = 0;
            enum class Phase { Seeking, Settle, Capture, Waiting, Captured, Advance, Draining } phase = Phase::Seeking;
            int settleIters = 0, quietIters = 0, waitIters = 0;
            long long virtualUs = 0;
            std::chrono::steady_clock::time_point virtualBase{}, realStart{}, phaseStart{}, lastOverlay{};
            struct CapturedFrame {
                std::vector<uint8_t> data;
            };
            std::mutex capMutex;
            std::condition_variable capCv;
            std::deque<CapturedFrame> ready;
            std::thread writer;
            bool stopWriter = false;
            bool sizeMismatch = false;
            std::string writerError;
            std::atomic<long long> written{0};
            long long maxInFlight = 6;
            std::string overlayRml;
            std::chrono::steady_clock::time_point lastTitle{}, lastProgress{};
            long long lastWritten = 0;
            ~ExportJob() {
                {
                    std::lock_guard<std::mutex> lk(capMutex);
                    stopWriter = true;
                    ready.clear();
                }
                capCv.notify_all();
                if (writer.joinable())
                    writer.join();
            }
            bool savedHideHud = true, savedPaused = false, savedMenu = false;
            double savedSpeed = 1, savedTimeMs = 0;
            replay::CameraMode savedMode = replay::CameraMode::Free;
            std::string lastMessage;
        } exportJob;
        bool UpdateExportMenu(ui::Hud& hud, int fbW, int fbH);
        void ExportOpenMenu();
        bool ExportStart(const ExportRequest& req);
        void ExportPreFrame(std::chrono::steady_clock::time_point& frameStart, float& dt);
        void ExportPostRender();
        void ExportFinish(bool cancelled, const std::string& error);
        void ExportWriterLoop();
        void ExportStartNext();
        void ExportOverlay(ui::Hud& hud, int fbW, int fbH);
        void FramebufferSizeEffective(int& w, int& h);
        void LoadExportConfig();
        void SaveExportConfig();

        void StartReplayPlayback(const std::string& path);
        void MakeReplaySession();
        void MountReplayPacks();
        void ExitReplayPlayback(ui::Hud& hud);
        void PumpReplay(float dt);
        void UpdateReplayCursor();
        bool UpdateReplayScreen(ui::Hud& hud, BedrockSession* live, int fbW, int fbH);
        void ReplayStatus(const std::string& text);
        void RefreshReplayList();
        bool TitleReplayTyped(const InputManager::Typed& t);
        bool TitleReplayClick(const std::string& hover);
        bool ImportReplayFile(const std::string& source);
        bool replayRecoveryChecked = false;
        double replayPrevMs = 0;
        double replayWorldFromMs = 0;
        bool titleReturnToReplays = false;
        int replayClickRow = -1;
        std::chrono::steady_clock::time_point replayClickAt{};
        std::unique_ptr<BedrockSession> session;
        std::shared_ptr<xbox::Account> account;
        std::mutex statusMutex;
        std::string statusHint;
        void SetStatusHint(const std::string& text);
        bool onlineThisRun = false;
        std::atomic<bool> sessionReady{false};
        std::atomic<uint64_t> connectGeneration{0};
        std::atomic<int> connectsInFlight{0};
        std::vector<std::unique_ptr<BedrockSession>> retiredSessions;
    };
}
