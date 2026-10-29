#pragma once
#include <concepts>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "perlin/perlin.hpp"

namespace perlin {
    class PluginCtx {
    public:
        explicit PluginCtx(Context& c) : c_(&c) {}

        Context& raw() const { return *c_; }

        template <EventTagLike Tag, class Fn> void on(Fn&& fn) const { c_->on<Tag>(std::forward<Fn>(fn)); }
        template <class Fn> void on_unload(Fn&& fn) const { c_->on_unload(std::forward<Fn>(fn)); }
        template <class Rep, class Period, class Fn> TimerId after(std::chrono::duration<Rep, Period> d, Fn&& fn) const { return c_->after(d, std::forward<Fn>(fn)); }
        template <class Rep, class Period, class Fn> TimerId every(std::chrono::duration<Rep, Period> d, Fn&& fn) const { return c_->every(d, std::forward<Fn>(fn)); }
        void cancel(TimerId t) const { c_->cancel(t); }
        template <class T, class Fn> void on_change(const Setting<T>& s, Fn&& fn) const { c_->on_change(s, std::forward<Fn>(fn)); }
        template <class Fn> void on_press(const Button& b, Fn&& fn) const { c_->on_press(b, std::forward<Fn>(fn)); }
        template <class... Args> void print(std::format_string<Args...> fmt, Args&&... args) const { c_->print(fmt, std::forward<Args>(args)...); }
        Config& config() const { return c_->config(); }
        uint64_t tick_count() const { return c_->tick_count(); }
        double uptime() const { return c_->uptime(); }

        template <meta::fixed_string Name, class Fn>
        void command(std::string_view help, Fn&& fn) const { c_->command<Name>(help, std::forward<Fn>(fn)); }
        template <meta::fixed_string Name, class C, class R, class... A>
        void command(std::string_view help, C* self, R (C::*member)(A...)) const { c_->command<Name>(help, Bound<C, R, A...>{self, member}); }
        template <meta::fixed_string Name, class C, class R, class... A>
        void command(std::string_view help, const C* self, R (C::*member)(A...) const) const { c_->command<Name>(help, BoundConst<C, R, A...>{self, member}); }

        template <meta::fixed_string Id, class Fn>
        void bind(std::string_view label, Key k, Fn&& fn) const { c_->bind<Id>(label, k, std::forward<Fn>(fn)); }
        template <meta::fixed_string Id, class C, class R, class... A>
        void bind(std::string_view label, Key k, C* self, R (C::*member)(A...)) const { c_->bind<Id>(label, k, Bound<C, R, A...>{self, member}); }

        template <meta::fixed_string Layer, class Fn>
        void hud(Fn&& fn, int z = 0) const { c_->hud<Layer>(std::forward<Fn>(fn), z); }
        template <meta::fixed_string Layer, class C, class R, class... A>
        void hud(C* self, R (C::*member)(A...), int z = 0) const { c_->hud<Layer>(Bound<C, R, A...>{self, member}, z); }

    private:
        template <class C, class R, class... A>
        struct Bound {
            C* self;
            R (C::*member)(A...);
            R operator()(A... a) const { return (self->*member)(std::forward<A>(a)...); }
        };
        template <class C, class R, class... A>
        struct BoundConst {
            const C* self;
            R (C::*member)(A...) const;
            R operator()(A... a) const { return (self->*member)(std::forward<A>(a)...); }
        };

        Context* c_;
    };

    class Plugin {
    public:
        virtual ~Plugin() = default;
        virtual void registerCommands(PluginCtx&) {}
        virtual void onEnable(PluginCtx&) {}
        virtual void onDisable(PluginCtx&) {}

        PluginCtx& ctx() const { return *ctx_; }

    private:
        template <class T> friend void detail_boot(Context&);
        PluginCtx* ctx_ = nullptr;
    };

#define PERLIN_PLUGIN_EVENTS(X)                                                                                         \
    X(Tick, onTick) X(Frame, onFrame) X(ChatReceived, onChatReceived) X(ChatSending, onChatSending)                    \
    X(Connected, onConnected) X(Disconnected, onDisconnected) X(Key, onKey) X(Damage, onDamage) X(Death, onDeath)     \
    X(Respawned, onRespawned) X(DimensionChanged, onDimensionChanged) X(ScreenChanged, onScreenChanged)                \
    X(SlotChanged, onSlotChanged) X(EntityAdded, onEntityAdded) X(EntityRemoved, onEntityRemoved) X(Sound, onSound)   \
    X(Packet, onPacket) X(Title, onTitle) X(SettingChanged, onSettingChanged) X(Look, onLook)                          \
    X(MouseMove, onMouseMove) X(MouseButton, onMouseButton) X(MouseWheel, onMouseWheel) X(KeyRaw, onKeyRaw)           \
    X(TextInput, onTextInput) X(ScreenRender, onScreenRender) X(SlotClick, onSlotClick) X(Interact, onInteract)        \
    X(ChunkLoaded, onChunkLoaded) X(ChunkUnloaded, onChunkUnloaded) X(BlockChanged, onBlockChanged)                    \
    X(ResourcesReloaded, onResourcesReloaded) X(LevelLoaded, onLevelLoaded) X(LevelUnloaded, onLevelUnloaded)          \
    X(ModsLoaded, onModsLoaded) X(ClientStopping, onClientStopping) X(Toast, onToast) X(ModMessage, onModMessage)      \
    X(Tooltip, onTooltip) X(ScreenButton, onScreenButton) X(WindowFocus, onWindowFocus) X(ServerPing, onServerPing)

    namespace detail_plugin {
        template <class T>
        T& instance() {
            static T value;
            return value;
        }

        template <class T>
        void wire_events(T& self, PluginCtx& ctx) {
#define PERLIN_WIRE(Ev, method)                                                                                             \
    if constexpr (requires(T& t, const typename event::Ev::Payload& p) { t.method(p); }) {                                \
        ctx.on<event::Ev>([&self](const typename event::Ev::Payload& p) { return self.method(p); });                       \
    } else if constexpr (requires(T& t) { t.method(); }) {                                                                  \
        ctx.on<event::Ev>([&self]() { return self.method(); });                                                             \
    }
            PERLIN_PLUGIN_EVENTS(PERLIN_WIRE)
#undef PERLIN_WIRE
        }
    }

    template <class T>
    void detail_boot(Context& c) {
        static_assert(std::is_base_of_v<Plugin, T>, "a plugin derives from perlin::Plugin");
        T& self = detail_plugin::instance<T>();
        static PluginCtx ctx(c);
        static_cast<Plugin&>(self).ctx_ = &ctx;
        self.registerCommands(ctx);
        detail_plugin::wire_events(self, ctx);
        self.onEnable(ctx);
        c.on_unload([&self] { self.onDisable(ctx); });
    }
}

#define PERLIN_REGISTER(T) \
    void perlin_plugin_main(::perlin::Context& c) { ::perlin::detail_boot<T>(c); }
