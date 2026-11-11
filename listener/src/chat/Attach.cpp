#include "chat/Attach.hpp"

#include "util/Base64.hpp"
#include "util/Sha256.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

namespace ke::chat {

namespace fs = std::filesystem;

std::string mimeOf(const std::string& name) {
    std::string ext = fs::path(name).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const struct {
        const char* ext;
        const char* mime;
    } table[] = {
        {".md", "text/markdown"},     {".txt", "text/plain"},        {".json", "application/json"},
        {".edn", "application/edn"},  {".clj", "text/x-clojure"},    {".cljs", "text/x-clojure"},
        {".cpp", "text/x-c++src"},    {".cc", "text/x-c++src"},      {".hpp", "text/x-c++hdr"},
        {".h", "text/x-chdr"},        {".c", "text/x-csrc"},         {".py", "text/x-python"},
        {".sh", "text/x-shellscript"}, {".zsh", "text/x-shellscript"}, {".yaml", "text/yaml"},
        {".yml", "text/yaml"},        {".toml", "text/toml"},        {".csv", "text/csv"},
        {".tsv", "text/tab-separated-values"}, {".html", "text/html"}, {".css", "text/css"},
        {".js", "text/javascript"},   {".ts", "text/typescript"},    {".swift", "text/x-swift"},
        {".rs", "text/x-rust"},       {".go", "text/x-go"},          {".java", "text/x-java"},
        {".xml", "application/xml"},  {".plist", "application/xml"}, {".cmake", "text/x-cmake"},
        {".png", "image/png"},        {".jpg", "image/jpeg"},        {".jpeg", "image/jpeg"},
        {".gif", "image/gif"},        {".bmp", "image/bmp"},         {".webp", "image/webp"},
        {".svg", "image/svg+xml"},    {".pdf", "application/pdf"},   {".zip", "application/zip"},
        {".gz", "application/gzip"},  {".tar", "application/x-tar"}, {".bin", "application/octet-stream"},
        {".wav", "audio/wav"},        {".mp3", "audio/mpeg"},        {".mp4", "video/mp4"},
    };
    for (const auto& row : table) {
        if (ext == row.ext) {
            return row.mime;
        }
    }
    return "";
}

bool isText(const std::string& s) {
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const std::size_t n = s.size();
    std::size_t i = 0;
    while (i < n) {
        const unsigned char c = p[i];
        if (c == 0) {
            return false;
        }
        std::size_t len = 0;
        if (c < 0x80) {
            len = 1;
        } else if ((c & 0xE0) == 0xC0) {
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
        } else {
            return false;
        }
        if (i + len > n) {
            return false;
        }
        for (std::size_t k = 1; k < len; ++k) {
            if ((p[i + k] & 0xC0) != 0x80) {
                return false;
            }
        }
        i += len;
    }
    return true;
}

Attachment classify(const std::string& path) {
    Attachment a;
    a.path = path;
    a.name = fs::path(path).filename().string();
    if (a.name.empty()) {
        a.name = fs::path(path).parent_path().filename().string();
    }
    std::error_code ec;
    if (fs::is_directory(path, ec)) {
        a.directory = true;
        a.mime = "inode/directory";
        return a;
    }
    if (!fs::is_regular_file(path, ec)) {
        a.refusal = "not a regular file";
        return a;
    }
    const auto sz = fs::file_size(path, ec);
    if (ec) {
        a.refusal = "unreadable: " + ec.message();
        return a;
    }
    a.size = static_cast<std::size_t>(sz);
    if (a.size > kAttachMaxBytes) {
        a.refusal = "too big: " + std::to_string(a.size / 1024 / 1024) + " MiB, the cap is " +
                    std::to_string(kAttachMaxBytes / 1024 / 1024) + " MiB";
        return a;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        a.refusal = "cannot open";
        return a;
    }
    a.content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    a.size = a.content.size();
    a.sha256 = util::Sha256::hex(a.content);
    a.mime = mimeOf(a.name);
    const bool textual = a.size <= kAttachTextMax && isText(a.content);
    if (a.mime.empty()) {
        a.mime = textual ? "text/plain" : "application/octet-stream";
    }
    a.text = textual && a.mime.rfind("image/", 0) != 0 && a.mime != "application/pdf" && a.mime != "application/zip" &&
             a.mime != "application/gzip";
    return a;
}

json createFields(const Attachment& a) {
    return json{{"file/name", a.name},
                {"file/mime", a.mime},
                {"file/size", a.size},
                {"file/sha256", a.sha256},
                {"file/path", a.path}};
}

ContentField contentField(const Attachment& a) {
    if (a.text) {
        return ContentField{"file/text", json("string"), json(a.content)};
    }
    return ContentField{"file/bytes", json("bytes"), json(util::base64Encode(a.content))};
}

namespace {

json entryNode(const fs::path& p, int depth, std::size_t maxEntries, bool& truncated) {
    std::error_code ec;
    const bool dir = fs::is_directory(p, ec);
    json node{{"label", p.filename().string() + (dir ? "/" : "")}};
    if (dir) {
        if (depth < 2) {
            std::vector<fs::directory_entry> entries;
            for (const auto& e : fs::directory_iterator(p, fs::directory_options::skip_permission_denied, ec)) {
                const std::string n = e.path().filename().string();
                if (n.empty() || n[0] == '.') {
                    continue;
                }
                entries.push_back(e);
            }
            std::sort(entries.begin(), entries.end(), [](const fs::directory_entry& a, const fs::directory_entry& b) {
                std::error_code e1, e2;
                const bool da = a.is_directory(e1), db = b.is_directory(e2);
                if (da != db) {
                    return da;
                }
                return a.path().filename().string() < b.path().filename().string();
            });
            json children = json::array();
            std::size_t n = 0;
            for (const auto& e : entries) {
                if (n++ >= maxEntries) {
                    truncated = true;
                    children.push_back(json{{"label", "… " + std::to_string(entries.size() - maxEntries) + " more"}});
                    break;
                }
                children.push_back(entryNode(e.path(), depth + 1, maxEntries, truncated));
            }
            node["children"] = children;
        }
    } else {
        const auto sz = fs::file_size(p, ec);
        if (!ec) {
            std::ostringstream v;
            if (sz < 1024) {
                v << sz << " B";
            } else if (sz < 1024 * 1024) {
                v << (sz / 1024) << " KiB";
            } else {
                v << (sz / 1024 / 1024) << " MiB";
            }
            node["value"] = v.str();
        }
    }
    return node;
}

} // namespace

json folderTree(const std::string& path, std::size_t maxEntries, bool& truncated) {
    truncated = false;
    return entryNode(fs::path(path), 0, maxEntries, truncated);
}

json folderViewFields(const std::string& path, const json& tree, bool truncated) {
    return json{{"view/kind", "tree"},
                {"view/title", fs::path(path).filename().string() + "/ (listing, two levels)"},
                {"view/data", tree},
                {"view/truncated?", truncated},
                {"file/path", path},
                {"file/mime", "inode/directory"}};
}

} // namespace ke::chat
