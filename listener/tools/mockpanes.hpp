// The pane actions of docs/panes.md, for mockworld only: a real PTY behind `terminal`, a real
// directory behind `files` and the file kinds. It is what the listener's module tests drive, so
// `vim` runs here the same as it will in the world. Desktop only, like the mock itself.
#pragma once

#include "net/Protocol.hpp"
#include "util/Base64.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <string>
#include <thread>

#include <csignal>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

namespace ke::mock {

using json = nlohmann::json;
namespace fs = std::filesystem;

struct Pane {
    std::string id, kind, target, path, title;
    int fd = -1;
    pid_t pid = -1;
    Seq seq = 0;
    std::string buffer;  // editor, markdown, latex: what the world holds
    std::string replay;  // terminal: everything so far, for a late subscriber
    std::set<std::string> subscribers;
    bool open = true;
};

// What an action needs from the world around it.
struct PaneHost {
    std::function<void(const std::string& session, const PaneData&)> push;
    std::function<void(const Pane&, const std::string& kind, json body)> event;
};

class Panes {
  public:
    explicit Panes(fs::path root) : root_(std::move(root)) {}

    // Answers {ok, result} or {err, message}, through the two references. Called under the world's lock.
    bool act(const std::string& action, const json& a, const std::string& session, PaneHost& host, json& result,
             std::string& code, std::string& message) {
        const std::string id = a.value("pane", std::string());
        if (action == "pane.open") {
            return open(a, host, result);
        }
        if (action == "file.open") { // the world resolves the path and picks the kind
            json b = a;
            const std::string path = a.value("path", std::string());
            const std::string ext = fs::path(path).extension().string();
            const std::string kind = a.value("as", ext == ".md" ? std::string("markdown") : ext == ".tex" ? "latex" : "editor");
            b["kind"] = kind;
            open(b, host, result);
            result["kind"] = kind;
            result["path"] = path;
            return true;
        }
        if (action == "pane.list") {
            result = {{"panes", json::array()}};
            for (const auto& [k, p] : panes_) {
                result["panes"].push_back(json{{"pane", k}, {"kind", p.kind}, {"title", p.title}});
            }
            return true;
        }
        auto it = panes_.find(id);
        if (it == panes_.end()) {
            code = "unresolved";
            message = "no pane " + id;
            return false;
        }
        Pane& p = it->second;
        if (action == "pane.subscribe") {
            p.subscribers.insert(session);
            if (p.kind == "terminal" || p.kind == "editor") {
                PaneData first;
                first.pane = p.id;
                first.seq = ++p.seq;
                first.full = true;
                first.data = p.kind == "terminal" ? p.replay : p.buffer;
                if (p.kind == "editor") {
                    first.state = {{"version", version_}};
                }
                host.push(session, first);
            }
            result = json::object();
            return true;
        }
        if (action == "pane.close") {
            closePane(p);
            host.event(p, "pane-closed", {{"pane", p.id}});
            panes_.erase(it);
            result = json::object();
            return true;
        }
        if (action == "pane.resize") {
            struct winsize ws {};
            ws.ws_col = static_cast<unsigned short>(a.value("cols", 80));
            ws.ws_row = static_cast<unsigned short>(a.value("rows", 24));
            if (p.fd >= 0) {
                ioctl(p.fd, TIOCSWINSZ, &ws);
            }
            result = json::object();
            return true;
        }
        if (action == "pane.send") {
            const std::string text = a.value("text", std::string());
            if (p.fd >= 0) {
                (void)!write(p.fd, text.data(), text.size());
            }
            host.event(p, "pane-input", {{"pane", p.id}, {"who", "root"}, {"from", session}, {"size", text.size()}});
            result = json::object();
            return true;
        }
        if (action == "pane.model-send") { // test only: what a model's input looks like on the log
            const std::string text = a.value("text", std::string());
            if (p.fd >= 0) {
                (void)!write(p.fd, text.data(), text.size());
            }
            host.event(p, "pane-input", {{"pane", p.id}, {"who", "modelhost"}, {"model", true}, {"text", text}});
            result = json::object();
            return true;
        }
        if (action == "pane.read") {
            return read(p, a, result, code, message);
        }
        if (action == "pane.frob") {
            return frob(p, a, host, result, code, message);
        }
        code = "unknown";
        message = "no action " + action;
        return false;
    }

    // Bytes from a PTY, on its reader thread; the caller holds the world's lock.
    void output(const std::string& id, const std::string& bytes, PaneHost& host) {
        auto it = panes_.find(id);
        if (it == panes_.end()) {
            return;
        }
        Pane& p = it->second;
        p.replay += bytes;
        PaneData d;
        d.pane = p.id;
        d.base = p.seq;
        d.seq = ++p.seq;
        d.data = bytes;
        for (const std::string& s : p.subscribers) {
            host.push(s, d);
        }
    }

    std::function<void(const std::string& id, int fd)> onTerminal;

  private:
    fs::path resolve(const std::string& path) const {
        fs::path p(path);
        return p.is_absolute() ? p : root_ / p;
    }

    static std::string slurp(const fs::path& p) {
        std::ifstream in(p, std::ios::binary);
        std::ostringstream s;
        s << in.rdbuf();
        return s.str();
    }

    bool open(const json& a, PaneHost& host, json& result) {
        Pane p;
        p.id = "6e0e5a7e-3333-4c2a-8f1e-" + std::string(12 - std::min<std::size_t>(12, std::to_string(next_).size()), '0') + std::to_string(next_);
        ++next_;
        p.kind = a.value("kind", std::string("terminal"));
        p.target = a.value("target", std::string());
        p.path = a.value("path", std::string());
        p.title = a.value("title", p.path.empty() ? p.kind : p.path);
        if (p.kind == "terminal") {
            struct winsize ws {};
            ws.ws_col = 80;
            ws.ws_row = 24;
            p.pid = forkpty(&p.fd, nullptr, nullptr, &ws);
            if (p.pid == 0) {
                setenv("TERM", "xterm-256color", 1);
                setenv("PS1", "$ ", 1);
                if (chdir(root_.c_str()) != 0) {
                    _exit(126);
                }
                execl("/bin/sh", "sh", static_cast<char*>(nullptr));
                _exit(127);
            }
        } else if (p.kind != "files" && !p.path.empty()) {
            p.buffer = slurp(resolve(p.path));
        }
        result = {{"pane", p.id}};
        const std::string id = p.id;
        const int fd = p.fd;
        const bool terminal = p.kind == "terminal";
        panes_[id] = std::move(p);
        if (terminal && onTerminal) {
            onTerminal(id, fd);
        }
        host.event(panes_[id], "pane-opened", {{"pane", id}});
        return true;
    }

    void closePane(Pane& p) const {
        if (p.fd >= 0) {
            close(p.fd);
            p.fd = -1;
        }
        if (p.pid > 0) {
            kill(p.pid, SIGHUP);
            waitpid(p.pid, nullptr, WNOHANG);
        }
    }

    bool read(Pane& p, const json& a, json& result, std::string& code, std::string& message) {
        const std::string what = a.value("what", std::string("screen"));
        if (what == "bytes") {
            const std::string bytes = slurp(resolve(p.path));
            result = {{"base64", util::base64Encode(bytes)}, {"size", bytes.size()}, {"offset", 0}, {"more", false}};
            return true;
        }
        if (what == "buffer") {
            if (!p.path.empty() && p.kind != "editor") {
                p.buffer = slurp(resolve(p.path)); // a viewer follows the file
            }
            result = {{"text", p.buffer}};
            return true;
        }
        if (what == "listing") {
            return listing(p.path, result, code, message);
        }
        result = {{"text", p.replay}};
        return true;
    }

    bool listing(const std::string& path, json& result, std::string& code, std::string& message) {
        std::error_code ec;
        const fs::path dir = path.empty() ? root_ : resolve(path);
        json entries = json::array();
        for (const fs::directory_entry& e : fs::directory_iterator(dir, ec)) {
            std::error_code se;
            const bool isDir = e.is_directory(se);
            entries.push_back(json{{"name", e.path().filename().string()},
                                   {"kind", isDir ? "dir" : "file"},
                                   {"size", isDir ? -1 : static_cast<long long>(e.file_size(se))},
                                   {"modified", "2026-10-31T12:00:00Z"}});
        }
        if (ec) {
            code = "unresolved";
            message = ec.message();
            return false;
        }
        result = {{"entries", std::move(entries)}};
        return true;
    }

    static std::size_t byteAt(const std::string& s, std::size_t cp) {
        std::size_t b = 0;
        while (cp > 0 && b < s.size()) {
            ++b;
            while (b < s.size() && (static_cast<unsigned char>(s[b]) & 0xC0) == 0x80) {
                ++b;
            }
            --cp;
        }
        return b;
    }

    bool frob(Pane& p, const json& a, PaneHost& host, json& result, std::string& code, std::string& message) {
        const std::string verb = a.value("verb", std::string());
        const json args = a.value("args", json::object());
        std::error_code ec;
        result = json::object();
        if (verb == "list") {
            return listing(args.value("path", std::string()), result, code, message);
        }
        if (verb == "insert") {
            p.buffer.insert(byteAt(p.buffer, args.value("at", std::size_t{0})), args.value("text", std::string()));
            result = {{"version", ++version_}};
        } else if (verb == "replace") {
            const std::size_t at = args.value("at", std::size_t{0});
            const std::size_t s = byteAt(p.buffer, at);
            const std::size_t e = byteAt(p.buffer, at + args.value("del", std::size_t{0}));
            p.buffer.replace(s, e - s, args.value("with", std::string()));
            result = {{"version", ++version_}};
        } else if (verb == "save") {
            if (conflict_ && !args.value("force", false)) {
                conflict_ = false;
                code = "conflict";
                message = "the file changed on disk";
                return false;
            }
            std::ofstream(resolve(p.path), std::ios::binary) << p.buffer;
        } else if (verb == "mkdir") {
            fs::create_directories(resolve(args.value("name", std::string())), ec);
        } else if (verb == "delete") {
            fs::remove_all(resolve(args.value("name", std::string())), ec);
        } else if (verb == "rename" || verb == "move") {
            fs::rename(resolve(args.value("from", std::string())), resolve(args.value("to", std::string())), ec);
        } else if (verb == "copy") {
            fs::copy(resolve(args.value("from", std::string())), resolve(args.value("to", std::string())),
                     fs::copy_options::recursive, ec);
        } else if (verb == "render") {
            code = "not-implemented";
            message = "no TeX renderer in the mock world";
            return false;
        } else if (verb == "conflict-next") { // test only
            conflict_ = true;
        } else if (verb == "model-insert") { // test only: a model's edit, on the record
            const std::size_t at = args.value("at", std::size_t{0});
            const std::string text = args.value("text", std::string());
            p.buffer.insert(byteAt(p.buffer, at), text);
            PaneData d; // the edit reaches an open editor as a frame, like the world's
            d.pane = p.id;
            d.base = p.seq;
            d.seq = ++p.seq;
            d.edit = {{"at", at}, {"del", 0}, {"text", text}, {"who", "modelhost"}, {"version", ++version_}};
            for (const std::string& s : p.subscribers) {
                host.push(s, d);
            }
            host.event(p, "pane-input", {{"pane", p.id}, {"who", "modelhost"}, {"model", true}, {"text", text}});
        } else {
            code = "invalid";
            message = "no verb " + verb;
            return false;
        }
        if (ec) {
            code = "failed";
            message = ec.message();
            return false;
        }
        return true;
    }

    fs::path root_;
    std::map<std::string, Pane> panes_;
    int next_ = 1;
    bool conflict_ = false;
    long long version_ = 0;
};

} // namespace ke::mock
