#include "client/ExitSignals.hpp"

#include <atomic>
#include <csignal>
#include <initializer_list>

namespace perlin::exitsignals {
    namespace {
        volatile std::sig_atomic_t quitSignals = 0;
        std::atomic<LeaveFn> leaveFn{nullptr};
        std::atomic<void*> leaveContext{nullptr};
        std::atomic_flag crashing = ATOMIC_FLAG_INIT;

        extern "C" void OnTerminate(int sig) {
            if (quitSignals > 0) {
                std::signal(sig, SIG_DFL);
                std::raise(sig);
                return;
            }
            quitSignals = quitSignals + 1;
        }

        extern "C" void OnCrash(int sig) {
            std::signal(sig, SIG_DFL);
            if (!crashing.test_and_set()) {
                if (const LeaveFn fn = leaveFn.load())
                    fn(leaveContext.load());
            }
            std::raise(sig);
        }
    }

    void Install() {
        std::signal(SIGTERM, OnTerminate);
        std::signal(SIGINT, OnTerminate);
#ifdef SIGHUP
        std::signal(SIGHUP, OnTerminate);
#endif
        for (const int sig : {SIGSEGV, SIGABRT, SIGFPE, SIGILL})
            std::signal(sig, OnCrash);
#ifdef SIGBUS
        std::signal(SIGBUS, OnCrash);
#endif
    }

    bool QuitRequested() {
        return quitSignals > 0;
    }

    void SetLeaveHook(LeaveFn fn, void* context) {
        leaveContext.store(context);
        leaveFn.store(fn);
    }
}
