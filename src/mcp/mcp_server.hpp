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
#include <mutex>
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

        std::atomic<bool> m_running{false};
        int m_port = 0;
        std::thread m_thread;
        std::vector<Tool> m_tools;

        // One in-flight main-thread call at a time: the socket thread parks
        // here until pump() has run the tool and filled in the result.
        std::mutex m_callMutex;
        std::condition_variable m_callCv;
        bool m_callPending = false;
        bool m_callDone = false;
        std::function<void()> m_callFn;
    };

    // Registers GucciBot's own tools. Defined in mcp_tools.cpp.
    void registerTools(Server& server);

} // namespace gucci::mcp
