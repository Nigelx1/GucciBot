// Winsock MUST come before anything that reaches windows.h. Geode.hpp does,
// and windows.h pulls in the 1.1 winsock, which then collides with winsock2
// in ws2tcpip.h. Nothing subtle to debug here -- it is purely include order,
// so these two lines stay at the top of the file.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "mcp_server.hpp"

#include <Geode/Geode.hpp>

#include <cctype>
#include <chrono>
#include <cstdlib>

using namespace geode::prelude;

namespace gucci::mcp {

    namespace {
        // Smallest HTTP that serves the purpose: read a request, find the body
        // after the blank line, honour Content-Length. No keep-alive, no
        // chunking -- a local client on loopback needs neither, and every line
        // of protocol here is a line that can be wrong.
        std::string readRequest(SOCKET sock) {
            std::string data;
            char buf[4096];
            size_t headerEnd = std::string::npos;
            long long contentLength = -1;

            while (true) {
                int got = recv(sock, buf, sizeof(buf), 0);
                if (got <= 0)
                    break;
                data.append(buf, (size_t)got);

                if (headerEnd == std::string::npos) {
                    headerEnd = data.find("\r\n\r\n");
                    if (headerEnd != std::string::npos) {
                        auto head = data.substr(0, headerEnd);
                        for (auto& c : head)
                            c = (char)std::tolower((unsigned char)c);
                        auto at = head.find("content-length:");
                        if (at != std::string::npos)
                            contentLength = std::strtoll(head.c_str() + at + 15, nullptr, 10);
                    }
                }
                if (headerEnd != std::string::npos) {
                    size_t const have = data.size() - (headerEnd + 4);
                    if (contentLength < 0 || (long long)have >= contentLength)
                        break;
                }
            }
            if (headerEnd == std::string::npos)
                return "";
            return data.substr(headerEnd + 4);
        }

        void sendResponse(SOCKET sock, std::string const& body) {
            auto const head = fmt::format(
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                "Content-Length: {}\r\nConnection: close\r\n\r\n",
                body.size());
            send(sock, head.c_str(), (int)head.size(), 0);
            if (!body.empty())
                send(sock, body.c_str(), (int)body.size(), 0);
        }

        matjson::Value rpcError(matjson::Value const& id, int code, std::string const& msg) {
            auto err = matjson::Value::object();
            err["code"] = code;
            err["message"] = msg;
            auto out = matjson::Value::object();
            out["jsonrpc"] = "2.0";
            out["id"] = id;
            out["error"] = err;
            return out;
        }

        matjson::Value rpcOk(matjson::Value const& id, matjson::Value result) {
            auto out = matjson::Value::object();
            out["jsonrpc"] = "2.0";
            out["id"] = id;
            out["result"] = std::move(result);
            return out;
        }
    } // namespace

    void Server::addTool(Tool t) {
        for (auto& existing : m_tools) {
            if (existing.name == t.name) {
                existing = std::move(t);
                return;
            }
        }
        m_tools.push_back(std::move(t));
    }

    bool Server::start(int port) {
        if (m_running.load())
            return true;

        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            log::error("[GucciBot] MCP: WSAStartup failed");
            return false;
        }

        m_port = port;
        m_running = true;
        m_thread = std::thread(&Server::listenLoop, this);
        log::info("[GucciBot] MCP server listening on 127.0.0.1:{}", port);
        return true;
    }

    void Server::stop() {
        if (!m_running.exchange(false))
            return;
        // accept() below is behind a select() timeout, so the loop notices the
        // flag rather than needing its socket torn out from under it.
        if (m_thread.joinable())
            m_thread.join();
        WSACleanup();
        log::info("[GucciBot] MCP server stopped");
    }

    void Server::listenLoop() {
        SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == INVALID_SOCKET) {
            m_running = false;
            return;
        }

        int yes = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (char const*)&yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons((u_short)m_port);
        // Loopback only. This is a debugging surface that can drive the game;
        // it has no business being reachable from the network.
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

        if (bind(listener, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR ||
            listen(listener, 4) == SOCKET_ERROR) {
            log::error("[GucciBot] MCP: could not bind 127.0.0.1:{}", m_port);
            closesocket(listener);
            m_running = false;
            return;
        }

        while (m_running.load()) {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(listener, &set);
            timeval tv{0, 200000}; // 200ms, so stopping stays responsive
            if (select(0, &set, nullptr, nullptr, &tv) <= 0)
                continue;

            SOCKET client = accept(listener, nullptr, nullptr);
            if (client == INVALID_SOCKET)
                continue;

            auto const body = readRequest(client);
            sendResponse(client, body.empty() ? std::string{} : this->handleRequest(body));
            closesocket(client);
        }

        closesocket(listener);
    }

    // Runs a tool on the game's main thread and waits for it. Every tool
    // touches GD state, and GD is not thread safe -- running one from the
    // socket thread is the single mistake that would turn a debugging aid into
    // a crash generator.
    matjson::Value Server::callToolOnMainThread(std::string const& name,
                                                matjson::Value const& args) {
        matjson::Value result;
        std::string error;

        {
            std::lock_guard lock(m_callMutex);
            m_callDone = false;
            m_callPending = true;
            m_callFn = [this, &name, &args, &result, &error] {
                for (auto const& t : m_tools) {
                    if (t.name != name)
                        continue;
                    try {
                        result = t.run(args);
                    } catch (ToolError const& e) {
                        error = e.message;
                    } catch (std::exception const& e) {
                        error = std::string("internal error: ") + e.what();
                    } catch (...) {
                        error = "internal error";
                    }
                    return;
                }
                error = "no tool called " + name;
            };
        }

        {
            std::unique_lock lock(m_callMutex);
            // If the game is not pumping -- hard paused, or shutting down --
            // this must not park a socket thread forever.
            if (!m_callCv.wait_for(lock, std::chrono::seconds(10),
                                   [this] { return m_callDone; })) {
                m_callPending = false;
                throw ToolError("the game did not run the tool within 10s (is it frozen?)");
            }
        }

        if (!error.empty())
            throw ToolError(error);
        return result;
    }

    void Server::pump() {
        std::function<void()> fn;
        {
            std::lock_guard lock(m_callMutex);
            if (!m_callPending)
                return;
            fn = m_callFn;
            m_callPending = false;
        }
        if (fn)
            fn();
        {
            std::lock_guard lock(m_callMutex);
            m_callDone = true;
        }
        m_callCv.notify_all();
    }

    std::string Server::handleRequest(std::string const& body) {
        auto parsed = matjson::parse(body);
        if (!parsed.isOk())
            return rpcError(matjson::Value(), -32700, "invalid JSON").dump();

        auto req = parsed.unwrap();
        auto id = req.contains("id") ? req["id"] : matjson::Value();
        auto const method = req.contains("method") ? req["method"].asString().unwrapOr("") : "";

        if (method == "initialize") {
            auto caps = matjson::Value::object();
            caps["tools"] = matjson::Value::object();
            auto info = matjson::Value::object();
            info["name"] = "GucciBot";
            info["version"] = MOD_VERSION;
            auto res = matjson::Value::object();
            res["protocolVersion"] = "2024-11-05";
            res["capabilities"] = caps;
            res["serverInfo"] = info;
            return rpcOk(id, res).dump();
        }

        if (method == "tools/list") {
            auto arr = matjson::Value::array();
            for (auto const& t : m_tools) {
                auto e = matjson::Value::object();
                e["name"] = t.name;
                e["description"] = t.description;
                e["inputSchema"] = t.schema;
                arr.push(e);
            }
            auto res = matjson::Value::object();
            res["tools"] = arr;
            return rpcOk(id, res).dump();
        }

        if (method == "tools/call") {
            auto params = req.contains("params") ? req["params"] : matjson::Value::object();
            auto const name =
                params.contains("name") ? params["name"].asString().unwrapOr("") : "";
            auto args =
                params.contains("arguments") ? params["arguments"] : matjson::Value::object();

            auto content = matjson::Value::array();
            auto block = matjson::Value::object();
            block["type"] = "text";
            auto res = matjson::Value::object();
            try {
                auto out = this->callToolOnMainThread(name, args);
                // MCP returns tool output as content blocks; one text block
                // holding the JSON is the least surprising shape.
                block["text"] = out.dump();
                content.push(block);
                res["content"] = content;
            } catch (ToolError const& e) {
                block["text"] = e.message;
                content.push(block);
                res["content"] = content;
                res["isError"] = true;
            }
            return rpcOk(id, res).dump();
        }

        return rpcError(id, -32601, "unknown method").dump();
    }

} // namespace gucci::mcp
