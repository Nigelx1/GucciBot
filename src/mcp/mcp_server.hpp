#pragma once

// GucciBot's in-game MCP server.
//
// Ported in concept from Absense, which runs a Model Context Protocol server
// inside Geometry Dash so an assistant can look at the level, step it, drive
// the tools and read the logs. Their transport and protocol layers are a
// reasonable design and this follows their shape -- a socket thread, a
// JSON-RPC layer, and tools that run on the MAIN thread -- without copying
// their code.
//
// Why it is worth having here specifically: nearly every hard bug this
// project has had was diagnosed by asking Nigel to run something, then
// reading a log after the fact. A Calculate run that dies at frame 398 takes
// a round trip to see. With this, the thing doing the diagnosing can start
// the run, read the state and step the game itself.
//
// Safety, deliberately narrow:
//   - binds 127.0.0.1 only, never a routable address
//   - off by default, started explicitly from the Settings tab
//   - every tool runs on the game's main thread, so nothing touches GD state
//     from the socket thread
//   - tools that would drive the game refuse while a render or an analyzer
//     run owns it, rather than fighting for control

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <matjson.hpp>

namespace gucci::mcp {

    // Thrown by a tool to return a clean error to the caller instead of a
    // crash or an empty result.
    struct ToolError {
        std::string message;
        explicit ToolError(std::string m) : message(std::move(m)) {}
    };

    // GucciBot (2026-11-01): thrown by a tool's run instead of returning,
    // for a tool that needs frames to pass (the menu captured, a scroll, a
    // shot written). pump() keeps the call and runs `poll` at the start of
    // every later frame; the first value it returns is the answer, and it may
    // throw ToolError. No other call starts meanwhile (there is one at a time
    // anyway). `cancel` puts back anything half done when the socket thread
    // stops waiting (its 10 s) or the server stops, and after `maxFrames`
    // frames the call ends with an error.
    struct Defer {
        std::function<std::optional<matjson::Value>()> poll;
        std::function<void()> cancel;
        int maxFrames = 300;
    };

    struct Tool {
        std::string name;
        std::string description;
        matjson::Value schema;
        std::function<matjson::Value(matjson::Value const&)> run;
    };

    class Server {
    public:
        static Server* get() {
            static Server inst;
            return &inst;
        }

        bool start(int port);
        void stop();
        bool running() const {
            return m_running.load();
        }
        int port() const {
            return m_port;
        }

        // Called every frame from the game's own update, on the main thread.
        // Anything a socket thread queued is executed here and only here.
        void pump();

        void addTool(Tool t);
        std::vector<Tool> const& tools() const {
            return m_tools;
        }

    private:
        Server() = default;
        void listenLoop();
        std::string handleRequest(std::string const& body);
        matjson::Value callToolOnMainThread(std::string const& name, matjson::Value const& args);

        // 0 = the listen thread has not reported yet, 1 = bound and listening,
        // 2 = it failed. start() used to return true the instant the thread was
        // spawned, before bind() had been attempted, so a port clash left the
        // toggle switched on, the log claiming success, and nothing listening.
        std::atomic<int> m_bindState{0};

        std::atomic<bool> m_running{false};
        int m_port = 0;
        std::thread m_thread;
        std::vector<Tool> m_tools;

        // One in-flight main-thread call at a time: the socket thread parks
        // here until pump() has run the tool and filled in the result. Each
        // call's state is its own and shared with pump(): a call the socket
        // thread has given up on (the 10 s wait) can still be running on the
        // main thread, and it needs somewhere to write that is still there.
        struct Call {
            std::string name;
            matjson::Value args;
            matjson::Value result;
            std::string error;
            bool done = false;       // pump() has run it (under m_callMutex)
            bool abandoned = false;  // the socket thread stopped waiting (under m_callMutex)
        };
        std::mutex m_callMutex;
        std::condition_variable m_callCv;
        std::shared_ptr<Call> m_call;  // queued for pump(), not yet taken

        // A call whose tool threw Defer, polled at the start of each pump
        // until it answers. Main thread only.
        struct Waiting {
            std::shared_ptr<Call> call;
            Defer defer;
            int frames = 0;
        };
        std::optional<Waiting> m_waiting;
        // The answer, into the call the socket thread also holds.
        void finish(std::shared_ptr<Call> const& call, matjson::Value result, std::string error);
        // A waiting call let go of: its cancel, then (unless the socket thread
        // already gave up) `why` as its answer.
        void dropWaiting(std::string const& why);
    };

    // Everything the server does, appended to guccibot_mcp.log in the mod's
    // save dir. Geode's own console log is not persisted on this machine, so a
    // failed bind or a tool call left no evidence at all -- which is a silly
    // way for a diagnostic tool to behave.
    void mcpFileLog(std::string const& line);

    // Registers GucciBot's own tools. Defined in mcp_tools.cpp.
    void registerTools(Server& server);
    // The AI pilot's tools (gucci_ai_*). Defined in ai_tools.cpp.
    void registerAiTools(Server& server);

    // Who owns the run right now: "render", "analyzer", "pathfinder", or ""
    // for nobody. The driving tools refuse while it isn't "". Defined in
    // mcp_tools.cpp.
    std::string runOwner();

} // namespace gucci::mcp
