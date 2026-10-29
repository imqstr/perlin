#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "perlin/meta.hpp"

namespace perlin::abi {
    inline constexpr uint32_t kApiVersion = 1;
    inline constexpr int32_t kNeedMoreBase = 16;
    inline constexpr uint32_t kEventCancel = 1;

    enum class Op : uint32_t {
        Log = 0x0001,
        Meta = 0x0002,
        Clock = 0x0003,

        PlayerState = 0x0100,
        PlayerEffects = 0x0101,
        PlayerLook = 0x0102,

        Inventory = 0x0200,
        InventorySelect = 0x0201,
        InventoryDrop = 0x0202,
        InventorySwap = 0x0203,
        InventoryClick = 0x0204,
        InventoryOpen = 0x0205,
        InventoryClose = 0x0206,

        WorldState = 0x0300,
        Block = 0x0301,
        Region = 0x0302,
        Raycast = 0x0303,
        Aim = 0x0304,
        FindBlocks = 0x0305,
        ChunkLoaded = 0x0306,
        BlockNames = 0x0307,
        BlockProps = 0x0308,
        BlockEntityData = 0x0309,
        DisplayName = 0x030A,
        BreakProgress = 0x030B,

        Entities = 0x0400,
        EntityById = 0x0401,
        PlayerList = 0x0402,

        Options = 0x0500,
        OptionGet = 0x0501,
        OptionSet = 0x0502,
        Screen = 0x0503,
        Translate = 0x0504,
        ReloadResources = 0x0505,
        PerspectiveGet = 0x0506,
        PerspectiveSet = 0x0507,
        ResourcePacks = 0x0508,

        InputDown = 0x0600,
        InputOverride = 0x0601,
        KeyRegister = 0x0602,
        InputActions = 0x0603,
        MousePosition = 0x0604,

        Chat = 0x0700,
        Command = 0x0701,
        LocalMessage = 0x0702,
        CommandRegister = 0x0703,
        CommandUnregister = 0x0704,
        ServerCommands = 0x0705,
        Toast = 0x0706,
        Title = 0x0707,
        ChatHistory = 0x0708,
        TitleState = 0x0709,

        SwingArm = 0x0800,
        UseItem = 0x0801,
        UseOnBlock = 0x0802,
        AttackEntity = 0x0803,
        InteractEntity = 0x0804,
        BreakBlock = 0x0805,
        Look = 0x0806,
        Respawn = 0x0807,
        Disconnect = 0x0808,
        PlaySound = 0x0809,
        Jump = 0x080B,

        UiSubmit = 0x0900,
        UiClear = 0x0901,
        UiMeasure = 0x0902,

        GfxTextureCreate = 0x0910,
        GfxTextureUpdate = 0x0911,
        GfxTextureDestroy = 0x0912,
        GfxSubmit = 0x0913,
        GfxClear = 0x0914,
        GfxDisplay = 0x0915,

        InputGrab = 0x0920,

        NativeInfo = 0x0930,

        StorageRead = 0x0A00,
        StorageWrite = 0x0A01,
        StorageList = 0x0A02,
        StorageRemove = 0x0A03,
        StorageExists = 0x0A04,
        StorageMakeDir = 0x0A05,

        HttpRequest = 0x0B00,
        PacketSend = 0x0B01,
        PacketName = 0x0B02,

        SettingGet = 0x0C01,
        SettingSet = 0x0C02,
        SettingList = 0x0C03,
        ModList = 0x0C10,
        IconSet = 0x0C11,

        OpenScreen = 0x0D00,
        OpenModSettings = 0x0D01,
        TakeScreenshot = 0x0D02,
        CyclePerspective = 0x0D03,
        CopyToClipboard = 0x0D04,
        OpenUrl = 0x0D05,
        ToggleHud = 0x0D06,

        CopyFile = 0x0E00,
        MoveFile = 0x0E01,
        DownloadFile = 0x0E02,

        CameraControl = 0x0F00,
        CameraState = 0x0F01,
        WorldToScreen = 0x0F02,
        PlayerMotion = 0x0F10,
        PlayerTeleport = 0x0F11,
        PlayerModelPose = 0x0F20,
        PlayerSetLook = 0x0F21,
        WorldLines = 0x0F30,
        WorldBoxes = 0x0F31,
        ClientBlockSet = 0x0F40,
        ClientBlockGet = 0x0F41,
        PacketDrop = 0x0F50,
        PacketCounters = 0x0F51,
        PacketInject = 0x0F52,

        // hooks into the client's rendering, screens and systems
        HudElementSet = 0x1000,
        DebugLines = 0x1001,
        FogSet = 0x1002,
        ScreenTint = 0x1003,
        EntityRenderOverride = 0x1010,
        WorldMesh = 0x1011,
        WorldMeshClear = 0x1012,
        FrameLimit = 0x1020,
        WindowState = 0x1021,
        ScreenButtonAdd = 0x1030,
        ScreenButtonRemove = 0x1031,
        ScreenOpenCustom = 0x1032,
        TooltipAppend = 0x1033,
        LoadingOverlay = 0x1040,
        SkinOverride = 0x1041,
        AmbientSound = 0x1050,
        ServerPing = 0x1060,
        ModSend = 0x1070,
        PackRegister = 0x1080,
        ShaderPackSet = 0x1081,
        ParticleSpawn = 0x1090,
        KeyRegisterEx = 0x10A0,
    };

    enum class Event : uint32_t {
        Load = 1,
        Unload = 2,
        Tick = 3,
        Frame = 4,
        ChatReceived = 5,
        ChatSending = 6,
        Connected = 7,
        Disconnected = 8,
        Key = 9,
        Command = 10,
        Damage = 11,
        Death = 12,
        Respawned = 13,
        DimensionChanged = 14,
        ScreenChanged = 15,
        SlotChanged = 16,
        EntityAdded = 17,
        EntityRemoved = 18,
        Sound = 19,
        HttpResponse = 20,
        Packet = 21,
        Title = 22,
        SettingChanged = 23,
        Look = 24,
        MouseMove = 25,
        MouseButton = 26,
        MouseWheel = 27,
        KeyRaw = 28,
        TextInput = 29,
        ScreenRender = 30,
        SlotClick = 31,
        Interact = 32,
        ChunkLoaded = 33,
        ChunkUnloaded = 34,
        BlockChanged = 35,
        ResourcesReloaded = 36,
        LevelLoaded = 37,
        LevelUnloaded = 38,
        ModsLoaded = 39,
        ClientStopping = 40,
        Toast = 41,
        ModMessage = 42,
        Tooltip = 43,
        ScreenButton = 44,
        WindowFocus = 45,
        ServerPing = 46,
    };

    enum class Status : int32_t {
        Ok = 0,
        UnknownOp = -1,
        Denied = -2,
        BadArguments = -3,
        Unavailable = -4,
        NotFound = -5,
        Failed = -6,
    };

    enum class Permission : uint32_t {
        None = 0,
        Network = 1u << 0,
        Packets = 1u << 1,
        Control = 1u << 2,
        Storage = 1u << 3,
        Files = 1u << 4,
        Native = 1u << 5,
    };

    enum class Draw : uint8_t {
        Rect = 1,
        Text = 2,
        Item = 3,
        Image = 4,
        Line = 5,
        Gradient = 6,
        Border = 7,
    };

    enum class Screen : int32_t {
        None = 0,
        Pause = 1,
        Chat = 2,
        Inventory = 3,
        Options = 4,
        Mods = 5,
        Wardrobe = 6,
        Death = 7,
        Workbench = 8,
        Menu = 9,
        Other = 10,
        Custom = 11,
    };

    enum class HudElement : int32_t {
        Hearts = 0, Armor, Hunger, Air, Hotbar, Experience, Debug, Chat, Effects, Locator, Fire, Subtitles, Crosshair,
    };

    enum class InteractKind : int32_t { StartBreak = 0, Attack = 1, InteractEntity = 2, UseOnBlock = 3, UseItem = 4 };

    enum class RenderStage : int32_t { AfterTerrain = 0, AfterEntities = 1, Overlay = 2 };

    enum class KeyContext : int32_t { InGame = 0, InScreen = 1, Any = 2 };

    enum class GameMode : int32_t {
        Survival = 0,
        Creative = 1,
        Adventure = 2,
        SurvivalSpectator = 3,
        CreativeSpectator = 4,
        Default = 5,
        Spectator = 6,
    };

    enum class Dimension : int32_t { Overworld = 0, Nether = 1, End = 2 };

    enum class Difficulty : int32_t { Peaceful = 0, Easy = 1, Normal = 2, Hard = 3 };

    enum class PermissionLevel : int32_t { Unknown = -1, Visitor = 0, Member = 1, Operator = 2, Custom = 3 };

    enum class Face : int32_t { Down = 0, Up = 1, North = 2, South = 3, West = 4, East = 5 };

    enum class AimKind : int32_t { None = 0, Block = 1, Entity = 2 };

    enum class ChatType : int32_t {
        Raw = 0,
        Chat = 1,
        Translation = 2,
        Popup = 3,
        JukeboxPopup = 4,
        Tip = 5,
        System = 6,
        Whisper = 7,
        Announcement = 8,
        ObjectWhisper = 9,
        Object = 10,
        ObjectAnnouncement = 11,
    };

    enum class DisconnectReason : int32_t {
        Unknown = 0,
        CantConnectNoInternet = 1,
        NoPermissions = 2,
        UnrecoverableError = 3,
        ThirdPartyBlocked = 4,
        ThirdPartyNoInternet = 5,
        ThirdPartyBadIp = 6,
        ThirdPartyNoServerOrServerLocked = 7,
        VersionMismatch = 8,
        SkinIssue = 9,
        InviteSessionNotFound = 10,
    };

    enum class EffectId : int32_t {
        Speed = 1,
        Slowness = 2,
        Haste = 3,
        MiningFatigue = 4,
        Strength = 5,
        InstantHealth = 6,
        InstantDamage = 7,
        JumpBoost = 8,
        Nausea = 9,
        Regeneration = 10,
        Resistance = 11,
        FireResistance = 12,
        WaterBreathing = 13,
        Invisibility = 14,
        Blindness = 15,
        NightVision = 16,
        Hunger = 17,
        Weakness = 18,
        Poison = 19,
        Wither = 20,
        HealthBoost = 21,
        Absorption = 22,
        Saturation = 23,
        Levitation = 24,
        FatalPoison = 25,
        ConduitPower = 26,
        SlowFalling = 27,
        BadOmen = 28,
        VillageHero = 29,
        Darkness = 30,
        TrialOmen = 31,
        RaidOmen = 32,
        WindCharged = 33,
        Weaving = 34,
        Oozing = 35,
        Infested = 36,
    };

    enum class Perspective : int32_t { FirstPerson = 0, ThirdPersonBack = 1, ThirdPersonFront = 2 };

    enum class SettingKind : uint8_t {
        Toggle = 0,
        Integer,
        Number,
        Choice,
        Button,
        Label,
        Header,
    };

    enum class Area : uint8_t { Mod, Screenshots, ResourcePacks, ShaderPacks };

    enum class EntityFlag : uint8_t {
        OnFire = 0, Sneaking = 1, Riding = 2, Sprinting = 3, UsingItem = 4, Invisible = 5, Tempted = 6, InLove = 7,
        Saddled = 8, Powered = 9, Ignited = 10, Baby = 11, Converting = 12, Critical = 13, CanShowName = 14,
        AlwaysShowName = 15, NoAi = 16, Silent = 17, WallClimbing = 18, CanClimb = 19, CanSwim = 20, CanFly = 21,
        CanWalk = 22, Resting = 23, Sitting = 24, Angry = 25, Interested = 26, Charged = 27, Tamed = 28, Orphaned = 29,
        Leashed = 30, Sheared = 31, Gliding = 32, Elder = 33, Moving = 34, Breathing = 35, Chested = 36, Stackable = 37,
        ShowBase = 38, Rearing = 39, Vibrating = 40, Idling = 41, EvokerSpell = 42, ChargeAttack = 43, WasdControlled = 44,
        CanPowerJump = 45, Lingering = 46, HasCollision = 47, HasGravity = 48, FireImmune = 49, Dancing = 50,
        Enchanted = 51, ReturnTrident = 52, ContainerPrivate = 53, Transforming = 54, DamageNearbyMobs = 55,
        Swimming = 56, Bribed = 57, Pregnant = 58, LayingEgg = 59, PassengerCanPick = 60, TransitionSitting = 61,
        Eating = 62, LayingDown = 63,
    };
    using EntityFlags = meta::IndexSet<EntityFlag, uint64_t>;
}

template <>
struct perlin::meta::enable_flags<perlin::abi::Permission> : std::true_type {};
template <>
struct perlin::meta::enum_range<perlin::abi::EntityFlag> {
    static constexpr int min = 0;
    static constexpr int max = 63;
};
template <>
struct perlin::meta::enum_range<perlin::abi::EffectId> {
    static constexpr int min = 1;
    static constexpr int max = 63;
};

namespace perlin::abi {
    template <class E>
        requires std::is_enum_v<E>
    constexpr bool abi_enum_tag(E) {
        return true;
    }
}

namespace perlin::meta {
    template <class E>
        requires std::is_enum_v<E>
    struct enable_format<E> : std::bool_constant<requires(E e) { abi_enum_tag(e); }> {};
}

namespace perlin::abi {
    using Permissions = meta::Flags<Permission>;

    namespace wire {
        template <class T>
        concept Raw = (std::is_arithmetic_v<T> && !std::is_same_v<T, bool>) || std::is_enum_v<T> || requires { T::wire_raw_tag; };

        template <class T>
        struct is_vector : std::false_type {};
        template <class T, class A>
        struct is_vector<std::vector<T, A>> : std::true_type {};

        template <class T>
        struct is_array : std::false_type {};
        template <class T, size_t N>
        struct is_array<std::array<T, N>> : std::true_type {};

        template <class T>
        struct is_pair : std::false_type {};
        template <class A, class B>
        struct is_pair<std::pair<A, B>> : std::true_type {};

        template <class T>
        struct is_optional : std::false_type {};
        template <class T>
        struct is_optional<std::optional<T>> : std::true_type {};

        template <class T>
        struct is_variant : std::false_type {};
        template <class... Ts>
        struct is_variant<std::variant<Ts...>> : std::true_type {};
    }

    class Writer {
    public:
        Writer() = default;
        explicit Writer(size_t reserve) { data.reserve(reserve); }

        template <class T>
        Writer& put(T value) {
            static_assert(std::is_trivially_copyable_v<T>);
            const size_t at = data.size();
            data.resize(at + sizeof(T));
            std::memcpy(data.data() + at, &value, sizeof(T));
            return *this;
        }
        Writer& str(std::string_view text) {
            put<uint32_t>(static_cast<uint32_t>(text.size()));
            data.insert(data.end(), text.begin(), text.end());
            return *this;
        }
        Writer& bytes(const void* p, size_t n) {
            put<uint32_t>(static_cast<uint32_t>(n));
            const auto* b = static_cast<const uint8_t*>(p);
            data.insert(data.end(), b, b + n);
            return *this;
        }
        Writer& raw(const void* p, size_t n) {
            const auto* b = static_cast<const uint8_t*>(p);
            data.insert(data.end(), b, b + n);
            return *this;
        }
        Writer& boolean(bool v) { return put<uint8_t>(v ? 1 : 0); }

        template <class... Ts>
        Writer& write(const Ts&... values) {
            (write_one(values), ...);
            return *this;
        }

        const uint8_t* Data() const { return data.data(); }
        size_t Size() const { return data.size(); }
        void Clear() { data.clear(); }
        std::vector<uint8_t> data;

    private:
        template <class T>
        void write_one(const T& v) {
            if constexpr (std::is_same_v<T, bool>) {
                boolean(v);
            } else if constexpr (wire::Raw<T>) {
                put(v);
            } else if constexpr (std::is_convertible_v<const T&, std::string_view>) {
                str(std::string_view(v));
            } else if constexpr (wire::is_array<T>::value) {
                using E = typename T::value_type;
                if constexpr (wire::Raw<E>)
                    raw(v.data(), v.size() * sizeof(E));
                else
                    for (const E& e : v)
                        write_one(e);
            } else if constexpr (wire::is_vector<T>::value) {
                using E = typename T::value_type;
                put<uint32_t>(static_cast<uint32_t>(v.size()));
                if constexpr (wire::Raw<E>)
                    raw(v.data(), v.size() * sizeof(E));
                else
                    for (const E& e : v)
                        write_one(e);
            } else if constexpr (wire::is_pair<T>::value) {
                write_one(v.first);
                write_one(v.second);
            } else if constexpr (wire::is_optional<T>::value) {
                boolean(v.has_value());
                if (v)
                    write_one(*v);
            } else if constexpr (wire::is_variant<T>::value) {
                put<uint8_t>(static_cast<uint8_t>(v.index()));
                std::visit([this](const auto& alternative) { write_one(alternative); }, v);
            } else if constexpr (std::is_same_v<T, std::monostate>) {
            } else if constexpr (meta::Aggregate<T>) {
                meta::for_each_field(v, [&](const auto& member) { write_one(member); });
            } else {
                static_assert(sizeof(T) == 0, "type has no wire encoding");
            }
        }
    };

    class Reader {
    public:
        Reader() = default;
        Reader(const uint8_t* p, size_t n) : cur(p), end(p + n) {}

        template <class T>
        T get() {
            T value{};
            if (static_cast<size_t>(end - cur) < sizeof(T)) {
                failed = true;
                cur = end;
                return value;
            }
            std::memcpy(&value, cur, sizeof(T));
            cur += sizeof(T);
            return value;
        }
        std::string str() {
            const uint32_t n = get<uint32_t>();
            if (failed || static_cast<size_t>(end - cur) < n) {
                failed = true;
                cur = end;
                return {};
            }
            std::string out(reinterpret_cast<const char*>(cur), n);
            cur += n;
            return out;
        }
        std::vector<uint8_t> bytes() {
            const uint32_t n = get<uint32_t>();
            if (failed || static_cast<size_t>(end - cur) < n) {
                failed = true;
                cur = end;
                return {};
            }
            std::vector<uint8_t> out(cur, cur + n);
            cur += n;
            return out;
        }
        bool boolean() { return get<uint8_t>() != 0; }

        template <class T>
        T read() {
            T value{};
            read_into(value);
            return value;
        }
        template <class T>
        Reader& read_into(T& v) {
            if constexpr (std::is_same_v<T, bool>) {
                v = boolean();
            } else if constexpr (wire::Raw<T>) {
                v = get<T>();
            } else if constexpr (std::is_same_v<T, std::string>) {
                v = str();
            } else if constexpr (wire::is_array<T>::value) {
                using E = typename T::value_type;
                if constexpr (wire::Raw<E>) {
                    const size_t n = v.size() * sizeof(E);
                    if (Remaining() < n) {
                        fail();
                    } else {
                        std::memcpy(v.data(), cur, n);
                        cur += n;
                    }
                } else {
                    for (E& e : v)
                        read_into(e);
                }
            } else if constexpr (wire::is_vector<T>::value) {
                using E = typename T::value_type;
                const uint32_t n = get<uint32_t>();
                v.clear();
                if (failed)
                    return *this;
                if constexpr (wire::Raw<E>) {
                    if (static_cast<size_t>(n) * sizeof(E) > Remaining()) {
                        fail();
                        return *this;
                    }
                    v.resize(n);
                    if (n)
                        std::memcpy(v.data(), cur, static_cast<size_t>(n) * sizeof(E));
                    cur += static_cast<size_t>(n) * sizeof(E);
                } else {
                    if (n > Remaining()) {
                        fail();
                        return *this;
                    }
                    v.reserve(n);
                    for (uint32_t i = 0; i < n && !failed; i++) {
                        E e{};
                        read_into(e);
                        v.push_back(std::move(e));
                    }
                }
            } else if constexpr (wire::is_pair<T>::value) {
                read_into(v.first);
                read_into(v.second);
            } else if constexpr (wire::is_optional<T>::value) {
                if (boolean()) {
                    typename T::value_type e{};
                    read_into(e);
                    v = std::move(e);
                } else {
                    v.reset();
                }
            } else if constexpr (wire::is_variant<T>::value) {
                const uint8_t index = get<uint8_t>();
                if (failed || index >= std::variant_size_v<T>)
                    fail();
                else
                    read_alternative<0>(v, index);
            } else if constexpr (std::is_same_v<T, std::monostate>) {
            } else if constexpr (meta::Aggregate<T>) {
                meta::for_each_field(v, [&](auto& member) { read_into(member); });
            } else {
                static_assert(sizeof(T) == 0, "type has no wire encoding");
            }
            return *this;
        }

        size_t Remaining() const { return static_cast<size_t>(end - cur); }
        bool Ok() const { return !failed; }
        const uint8_t* Cursor() const { return cur; }
        void Skip(size_t n) {
            if (Remaining() < n)
                fail();
            else
                cur += n;
        }

    private:
        template <size_t I, class V>
        void read_alternative(V& v, size_t index) {
            if constexpr (I < std::variant_size_v<V>) {
                if (index == I) {
                    std::variant_alternative_t<I, V> alt{};
                    read_into(alt);
                    v = std::move(alt);
                } else {
                    read_alternative<I + 1>(v, index);
                }
            }
        }

        void fail() {
            failed = true;
            cur = end;
        }

        const uint8_t* cur = nullptr;
        const uint8_t* end = nullptr;
        bool failed = false;
    };

    struct Item {
        std::string name;
        int32_t count = 0, aux = 0, damage = 0;
        bool enchanted = false;
        std::string customName;
        std::vector<std::pair<int32_t, int32_t>> enchants;
        std::vector<std::string> lore;

        bool Empty() const { return name.empty() || count <= 0; }
    };

    struct PlayerState {
        uint64_t runtimeId = 0;
        int64_t uniqueId = 0;
        std::string name;
        double x = 0, y = 0, z = 0;
        double vx = 0, vy = 0, vz = 0;
        float yaw = 0, pitch = 0, headYaw = 0, eyeHeight = 1.62f;
        bool onGround = false, sprinting = false, sneaking = false, swimming = false, gliding = false, flying = false, mayFly = false,
             spectator = false, onFire = false, riding = false, connected = false;
        GameMode gameMode = GameMode::Survival;
        Dimension dimension = Dimension::Overworld;
        int32_t selectedSlot = 0;
        PermissionLevel permission = PermissionLevel::Unknown;
        float health = 20, maxHealth = 20, absorption = 0, hunger = 20, saturation = 5, xpProgress = 0, walkSpeed = 0.1f;
        int32_t xpLevel = 0, air = 300, maxAir = 300;
    };

    struct Effect {
        EffectId id{};
        int32_t amplifier = 0, ticks = 0;
        bool ambient = false, particles = true;
    };

    struct Inventory {
        std::array<Item, 36> slots;
        std::array<Item, 4> armor;
        Item offhand, cursor;
        int32_t selected = 0;
        bool open = false;
    };

    struct Attribute {
        std::string name;
        float min = 0, value = 0, max = 0;
    };

    struct Entity {
        uint64_t runtimeId = 0;
        int64_t uniqueId = 0;
        std::string type, name, nameTag, uuid;
        double x = 0, y = 0, z = 0;
        float yaw = 0, pitch = 0, headYaw = 0, health = -1, scale = 1;
        bool onGround = false, isPlayer = false, isItem = false, isSelf = false;
        EntityFlags flags;
        std::string itemName;
        int32_t itemCount = 0;
        std::string mainHand, offHand;
        std::array<std::string, 4> armor;
        int64_t vehicleUid = 0, ownerUid = 0, targetUid = 0;
        int32_t variant = 0, markVariant = 0, color = 0;
        float maxHealth = -1;
        bool dying = false;
        std::vector<Attribute> attributes;
        std::vector<std::pair<std::string, float>> properties;
        std::vector<std::pair<std::string, std::string>> propertyText;
    };

    struct PlayerEntry {
        std::string uuid, name;
        int64_t uid = 0;
        PermissionLevel permission = PermissionLevel::Unknown;
    };

    struct WorldState {
        Dimension dimension = Dimension::Overworld;
        Difficulty difficulty = Difficulty::Normal;
        GameMode worldGameMode = GameMode::Survival;
        int64_t day = 0;
        int32_t timeOfDay = 0;
        double totalTicks = 0;
        float rain = 0, thunder = 0;
        bool commandsEnabled = true, haveSpawn = false;
        int32_t spawnX = 0, spawnY = 0, spawnZ = 0;
        std::vector<std::pair<std::string, int32_t>> rules;
    };

    struct BlockInfo {
        bool loaded = false;
        uint32_t runtimeId = 0;
        std::string name;
        int32_t biome = 0;
        uint32_t liquid = 0;
        std::vector<std::pair<std::string, std::string>> states;
    };

    enum class Tool : uint8_t { None, Pickaxe, Axe, Shovel, Hoe, Shears, Sword };

    struct BlockProps {
        bool known = false;
        float hardness = 0;
        Tool tool = Tool::None;
        int32_t harvestLevel = 0;
        bool needsSupportBelow = false, breaksWholeStack = false;
        std::string displayName;
        std::vector<std::string> properties;
    };

    struct BlockEntityInfo {
        bool present = false;
        std::string id;
        int32_t color = -1;
        bool waxed = false;
        float rotation = 0;
        std::string frontText, backText;
        bool frontGlow = false, backGlow = false;
        int32_t base = -1, bannerType = 0;
        std::vector<std::pair<std::string, int32_t>> patterns;
        std::string nbt;
    };

    enum class NameKind : uint8_t { Item, Block, Entity };

    struct BreakState {
        bool active = false;
        int32_t x = 0, y = 0, z = 0;
        float progress = 0;
    };

    struct RayHit {
        bool hit = false;
        int32_t x = 0, y = 0, z = 0;
        Face face = Face::Up;
        double hx = 0, hy = 0, hz = 0, distance = 0;
        std::string block;
    };

    struct AimState {
        AimKind kind = AimKind::None;
        RayHit block;
        uint64_t entity = 0;
        std::string entityType;
        double distance = 0;
    };

    struct ScreenState {
        int32_t guiWidth = 0, guiHeight = 0, guiScale = 1, framebufferWidth = 0, framebufferHeight = 0;
        float fps = 0, frameMs = 0;
        Screen screen = Screen::None;
        bool paused = false, connected = false, hudHidden = false, debug = false;
        std::string server, language, platform, gameVersion;
    };

    struct Meta {
        uint32_t apiVersion = kApiVersion;
        std::string gameVersion, plugin, platform;
        Permissions permissions;
        int64_t nowMs = 0;
        uint64_t tick = 0;
    };

    struct InputOverrideState {
        float forward = 0, strafe = 0;
        bool jump = false, sneak = false, sprint = false, attack = false, use = false;
        int32_t ticks = 1;
    };

    struct CameraControlState {
        float roll = 0;
        float fovAdd = 0;
        float fovSet = 0;
        float offsetX = 0, offsetY = 0, offsetZ = 0;
        float yawAdd = 0, pitchAdd = 0;
        int32_t ticks = 2;
    };

    struct CameraInfo {
        double x = 0, y = 0, z = 0;
        float yaw = 0, pitch = 0, fov = 70, aspect = 1.78f, nearPlane = 0.1f, farPlane = 600;
        int32_t perspective = 0;
        std::array<float, 16> viewProjection{};
    };

    struct WorldPoint {
        double x = 0, y = 0, z = 0;
    };

    struct ScreenPoint {
        float x = 0, y = 0;
        float depth = 0;
        bool visible = false;
    };

    struct MotionRequest {
        bool add = false;
        double x = 0, y = 0, z = 0;
        bool axisX = true, axisY = true, axisZ = true;
    };

    struct TeleportRequest {
        double x = 0, y = 0, z = 0;
        bool keepVelocity = false;
    };

    struct ModelPose {
        float roll = 0, pitch = 0, yaw = 0;
        int32_t ticks = 2;
    };

    struct LookSet {
        double x = 0, y = 0, z = -1;
    };

    struct WorldLine {
        double x1 = 0, y1 = 0, z1 = 0, x2 = 0, y2 = 0, z2 = 0;
        uint32_t color = 0xFFFFFFFF;
        float width = 1.0f;
    };

    struct WorldLines {
        int32_t ticks = 2;
        std::vector<WorldLine> lines;
    };

    struct WorldBox {
        double x0 = 0, y0 = 0, z0 = 0, x1 = 1, y1 = 1, z1 = 1;
        uint32_t color = 0xFFFFFFFF;
        float width = 1.0f;
    };

    struct WorldBoxes {
        int32_t ticks = 2;
        std::vector<WorldBox> boxes;
    };

    struct ClientBlockEdit {
        int32_t x = 0, y = 0, z = 0;
        uint32_t runtimeId = 0;
    };

    struct ClientBlockEdits {
        std::vector<ClientBlockEdit> edits;
    };

    struct BlockQuery {
        std::string name;
        std::vector<std::pair<std::string, std::string>> states;
    };

    struct BlockQueryResult {
        bool found = false;
        uint32_t runtimeId = 0;
    };

    struct PacketDropRequest {
        int32_t ticks = 20;
        std::vector<uint32_t> ids;
        bool outgoing = false;
    };

    struct PacketCount {
        uint32_t id = 0, count = 0;
    };

    struct Region {
        int32_t x0 = 0, y0 = 0, z0 = 0, sx = 0, sy = 0, sz = 0;
        std::vector<std::string> palette;
        std::vector<uint16_t> cells;

        const std::string& at(int x, int y, int z) const {
            static const std::string empty;
            if (x < x0 || y < y0 || z < z0 || x >= x0 + sx || y >= y0 + sy || z >= z0 + sz)
                return empty;
            const size_t i = (static_cast<size_t>(y - y0) * static_cast<size_t>(sz) + static_cast<size_t>(z - z0)) * static_cast<size_t>(sx) +
                             static_cast<size_t>(x - x0);
            return i < cells.size() && cells[i] < palette.size() ? palette[cells[i]] : empty;
        }
    };

    struct FoundBlock {
        int32_t x = 0, y = 0, z = 0;
        std::string name;
    };

    struct SettingDef {
        std::string id;
        SettingKind kind = SettingKind::Toggle;
        std::string label, description;
        std::string icon;
        std::string value, fallback;
        double min = 0, max = 1, step = 0;
        std::vector<std::string> choices, choiceLabels;
        std::string unit;
        bool restart = false;
    };

    struct SettingEntry {
        std::string id, value;
    };

    struct FileLocation {
        Area area = Area::Mod;
        std::string path;
    };

    struct ModEntry {
        std::string name, displayName, description, version, author, state;
        bool enabled = true, self = false, hasSettings = false;
        std::string website, arch;
        std::vector<std::string> hooks;
        int32_t priority = 0;
        Permissions permissions;
    };

    struct HudElementState {
        HudElement element = HudElement::Hearts;
        bool visible = true;
        float dx = 0, dy = 0;
    };

    struct DebugLinesData {
        std::vector<std::string> left, right;
    };

    struct FogOverride {
        bool enabled = true;
        uint32_t color = 0;  // 0 keeps vanilla color; else 0xAARRGGBB
        float start = -1, end = -1;  // blocks; negative keeps vanilla
        int32_t ticks = 2;
    };

    struct ScreenTintData {
        uint32_t color = 0;  // 0xAARRGGBB drawn over the world
        float vignette = 0;  // 0..1 darkening at the edges
        int32_t ticks = 2;
    };

    struct EntityOverride {
        uint64_t entity = 0;  // runtime id, or 0 to match every entity of `type`
        std::string type;
        bool hidden = false;
        float scaleX = 1, scaleY = 1, scaleZ = 1;
        uint32_t tint = 0xFFFFFFFFu;
        std::string texture;  // resource-pack texture path; empty keeps the vanilla one
        int32_t nameTag = 0;  // 0 keep, 1 hide, 2 replace with nameText
        std::string nameText;
        int32_t ticks = 2;
    };

    struct MeshVertex {
        double x = 0, y = 0, z = 0;
        float u = 0, v = 0;
        uint32_t color = 0xFFFFFFFFu;
    };

    struct WorldMeshData {
        std::string id;
        RenderStage stage = RenderStage::AfterEntities;
        uint32_t texture = 0;  // gfx texture id, 0 for plain color
        bool depthTest = true;
        int32_t ticks = 2;
        std::vector<MeshVertex> triangles;  // three vertices per triangle
    };

    struct FrameLimitData {
        int32_t fps = 0;           // 0 keeps the player's setting
        int32_t unfocusedFps = 0;  // while unfocused or minimized; 0 keeps fps
    };

    struct WindowInfo {
        bool focused = true, minimized = false;
        int32_t width = 0, height = 0;
    };

    struct ScreenButtonDef {
        Screen screen = Screen::Pause;
        std::string id, label;
        int32_t x = 0, y = 0, width = 98, height = 20;  // GUI pixels from the screen's top left
    };

    struct LoadingOverlayData {
        bool hidden = false;
    };

    struct SkinOverrideData {
        std::string player;  // player name, empty for yourself
        bool slim = false;
        std::vector<uint8_t> png;  // empty removes the override
    };

    struct AmbientSoundData {
        std::string id, sound;
        float volume = 1, pitch = 1;
        bool play = true;
    };

    struct ServerPingRequest {
        std::string host;
        uint16_t port = 19132;
    };

    struct ModMessageData {
        std::string to, channel;  // empty `to` sends to every other mod
        std::vector<uint8_t> body;
    };

    struct PackRegistration {
        std::string path;  // folder or .zip inside the mod's area
        bool enabled = true;
    };

    struct ParticleRequest {
        std::string name;
        double x = 0, y = 0, z = 0;
        int32_t count = 1;
        float spread = 0;
    };

    struct KeyBindEx {
        std::string id, label, key;
        bool ctrl = false, shift = false, alt = false;
        KeyContext context = KeyContext::InGame;
    };

    struct ChatLine {
        std::string text;
        float alpha = 1;
    };

    struct PackEntry {
        std::string name;
        bool server = false;
    };

    struct TitleStateData {
        std::string title, subtitle, actionBar;
        float titleAlpha = 0, actionBarAlpha = 0;
    };

    struct EventNothing {};
    struct EventTick {
        float dt = 0.05f;
        uint64_t tick = 0;
    };
    struct EventFrame {
        float dt = 0, fps = 0;
        int32_t guiWidth = 0, guiHeight = 0;
    };
    struct EventChat {
        ChatType type = ChatType::Raw;
        std::string sender, message, raw;
    };
    struct EventKey {
        std::string id;
        bool down = false;
    };
    struct EventCommand {
        std::string name, args;
    };
    struct EventConnection {
        std::string server, message;
        DisconnectReason reason = DisconnectReason::Unknown;
    };
    struct EventDamage {
        bool known = false;
        float dx = 0, dz = 0, health = 0;
    };
    struct EventText {
        std::string text;
    };
    template <class T>
    struct EventChange {
        T from{}, to{};
    };
    using EventDimension = EventChange<Dimension>;
    using EventScreen = EventChange<Screen>;
    struct EventSlot {
        int32_t slot = 0;
        Item item;
    };
    struct EventEntity {
        Entity entity;
    };
    struct EventSound {
        std::string name;
        double x = 0, y = 0, z = 0;
        float volume = 1, pitch = 1;
    };
    struct EventHttp {
        uint32_t id = 0;
        int32_t status = 0;
        std::string error;
        std::vector<std::pair<std::string, std::string>> headers;
        std::vector<uint8_t> body;
    };
    struct EventPacket {
        uint32_t id = 0;
        bool outgoing = false;
        std::vector<uint8_t> body;
    };
    struct EventTitle {
        std::string title, subtitle, actionBar;
    };
    struct EventLook {
        float dx = 0, dy = 0, dt = 0;
        double dirX = 0, dirY = 0, dirZ = -1;
        double vx = 0, vy = 0, vz = 0;
        uint32_t keys = 0;
        bool gliding = false, underwater = false, thirdPerson = false, onGround = false;
    };
    struct EventSettingChanged {
        std::string id, value;
        SettingKind kind = SettingKind::Toggle;
    };
    struct EventMouseMove {
        float x = 0, y = 0;
    };
    struct EventMouseButton {
        int32_t button = 0;
        bool down = false;
        float x = 0, y = 0;
    };
    struct EventMouseWheel {
        float dx = 0, dy = 0;
    };
    struct EventKeyRaw {
        int32_t key = 0;
        bool down = false, repeat = false, ctrl = false, shift = false, alt = false;
    };

    struct EventScreenRender {
        Screen screen = Screen::None;
        int32_t guiWidth = 0, guiHeight = 0;
        float mouseX = 0, mouseY = 0;
    };
    struct EventSlotClick {
        int32_t slot = 0;  // same slot address as InventoryClick
        bool right = false, shift = false;
    };
    struct EventInteract {
        InteractKind kind = InteractKind::StartBreak;
        int32_t x = 0, y = 0, z = 0;
        Face face = Face::Up;
        uint64_t entity = 0;
    };
    struct EventChunk {
        int32_t x = 0, z = 0;
        Dimension dimension = Dimension::Overworld;
    };
    struct EventBlockChanged {
        int32_t x = 0, y = 0, z = 0;
        std::string from, to;
    };
    struct EventLevel {
        Dimension dimension = Dimension::Overworld;
        std::string server;
    };
    struct EventToast {
        std::string title, body;
    };
    struct EventModMessage {
        std::string from, channel;
        std::vector<uint8_t> body;
    };
    struct EventTooltip {
        Item item;
        std::vector<std::string> lines;
    };
    struct EventScreenButton {
        std::string id;
    };
    struct EventWindow {
        bool focused = true, minimized = false;
    };
    struct EventServerPing {
        uint32_t id = 0;
        bool ok = false;
        std::string motd, version;
        int32_t players = 0, maxPlayers = 0, protocol = 0;
        float latencyMs = 0;
    };

    struct GfxVertex {
        float x = 0, y = 0, u = 0, v = 0;
        uint32_t color = 0xFFFFFFFFu;
    };
    static_assert(sizeof(GfxVertex) == 20);
    struct GfxCmd {
        float clip[4] = {0, 0, 0, 0};
        uint32_t texture = 0;
        uint32_t indexOffset = 0, indexCount = 0, vertexOffset = 0;
    };
    static_assert(sizeof(GfxCmd) == 32);
    struct GfxHeader {
        uint32_t vertexCount = 0, indexCount = 0, commandCount = 0;
        uint32_t flags = 0;
    };
    inline constexpr uint32_t kGfxIndex32 = 1;
    inline constexpr uint32_t kGfxMaxBytes = 8u << 20;
    inline constexpr uint32_t kGfxMaxVertices = 1u << 20;
    inline constexpr uint32_t kGfxMaxLayers = 8;
    inline constexpr uint32_t kGfxMaxTextures = 64;
    inline constexpr uint32_t kGfxMaxTextureSide = 4096;
    inline constexpr uint64_t kGfxMaxTextureBytes = 128ull << 20;

    struct NativeHandles {
        std::string api;
        std::string adapter;
        uint64_t window = 0;
        uint64_t display = 0;
        uint64_t device = 0;
    };

    struct GfxDisplayInfo {
        int32_t width = 0, height = 0;
        int32_t guiScale = 1;
        float mouseX = 0, mouseY = 0;
        bool mouseFree = false;
        bool inGame = false;
    };

    using EventPayloads = std::tuple<EventNothing,
                                     EventNothing,
                                     EventTick,
                                     EventFrame,
                                     EventChat,
                                     EventText,
                                     EventConnection,
                                     EventConnection,
                                     EventKey,
                                     EventCommand,
                                     EventDamage,
                                     EventText,
                                     EventNothing,
                                     EventDimension,
                                     EventScreen,
                                     EventSlot,
                                     EventEntity,
                                     EventEntity,
                                     EventSound,
                                     EventHttp,
                                     EventPacket,
                                     EventTitle,
                                     EventSettingChanged,
                                     EventLook,
                                     EventMouseMove,
                                     EventMouseButton,
                                     EventMouseWheel,
                                     EventKeyRaw,
                                     EventText,
                                     EventScreenRender,
                                     EventSlotClick,
                                     EventInteract,
                                     EventChunk,
                                     EventChunk,
                                     EventBlockChanged,
                                     EventNothing,
                                     EventLevel,
                                     EventNothing,
                                     EventNothing,
                                     EventNothing,
                                     EventToast,
                                     EventModMessage,
                                     EventTooltip,
                                     EventScreenButton,
                                     EventWindow,
                                     EventServerPing>;
    static_assert(std::tuple_size_v<EventPayloads> == meta::enum_count<Event>, "every Event needs a payload type");

    template <Event E>
    using payload_t = std::tuple_element_t<static_cast<size_t>(E) - 1, EventPayloads>;
}
