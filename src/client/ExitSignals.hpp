#pragma once

namespace perlin::exitsignals {
    // Termination signals (SIGTERM, SIGINT, SIGHUP) ask the main loop to quit, so the game leaves the
    // server through its normal shutdown instead of vanishing and leaving a session the server keeps
    // for about 30 seconds. A second termination signal ends the process at once.
    void Install();
    bool QuitRequested();

    // Called once, best effort, when the process crashes: lets the transport tell the server goodbye
    // before the default handler takes over.
    using LeaveFn = void (*)(void* context);
    void SetLeaveHook(LeaveFn fn, void* context);
}
