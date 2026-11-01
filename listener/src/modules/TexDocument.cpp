#include "modules/TexDocument.hpp"

#include <sstream>
#include <vector>

namespace ke::modules {

namespace {

// `\name{arg}` -> before + arg + after, wherever it occurs on the line.
void swapCommand(std::string& s, const std::string& name, const std::string& before, const std::string& after) {
    const std::string open = "\\" + name + "{";
    for (std::size_t at = s.find(open); at != std::string::npos; at = s.find(open, at)) {
        std::size_t i = at + open.size();
        int depth = 1;
        while (i < s.size() && depth > 0) {
            depth += s[i] == '{' ? 1 : s[i] == '}' ? -1 : 0;
            ++i;
        }
        if (depth != 0) {
            return;
        }
        const std::string arg = s.substr(at + open.size(), i - at - open.size() - 1);
        s.replace(at, i - at, before + arg + after);
        at += before.size();
    }
}

std::string stripComment(const std::string& line) {
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '%' && (i == 0 || line[i - 1] != '\\')) {
            return line.substr(0, i);
        }
    }
    return line;
}

bool startsWith(const std::string& s, const char* w) { return s.rfind(w, 0) == 0; }

std::string envOf(const std::string& line, const char* verb) {
    const std::string open = std::string("\\") + verb + "{";
    const std::size_t at = line.find(open);
    if (at == std::string::npos) {
        return "";
    }
    const std::size_t close = line.find('}', at);
    return close == std::string::npos ? "" : line.substr(at + open.size(), close - at - open.size());
}

} // namespace

std::string texToMarkdown(std::string_view tex) {
    std::istringstream in{std::string(tex)};
    std::ostringstream out;
    const bool hasDocument = tex.find("\\begin{document}") != std::string_view::npos;
    bool inBody = !hasDocument;
    std::string title, author;
    std::vector<std::string> lists;
    bool inMath = false;
    std::string mathEnv;
    for (std::string raw; std::getline(in, raw);) {
        std::string line = stripComment(raw);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
            line.erase(line.begin());
        }
        if (!inBody) {
            if (const std::string t = envOf(line, "title"); !t.empty()) {
                title = t;
            }
            if (const std::string a = envOf(line, "author"); !a.empty()) {
                author = a;
            }
            if (line.find("\\begin{document}") != std::string::npos) {
                inBody = true;
                if (!title.empty()) {
                    out << "# " << title << "\n\n";
                }
                if (!author.empty()) {
                    out << "*" << author << "*\n\n";
                }
            }
            continue;
        }
        if (line.find("\\end{document}") != std::string::npos) {
            break;
        }
        if (inMath) {
            if (envOf(line, "end") == mathEnv || startsWith(line, "\\]")) {
                out << (mathEnv.rfind("align", 0) == 0 ? "\\end{aligned}\n" : "") << "$$\n\n";
                inMath = false;
            } else if (!line.empty()) {
                out << line << "\n";
            }
            continue;
        }
        const std::string begin = envOf(line, "begin");
        if (begin == "equation" || begin == "equation*" || begin == "align" || begin == "align*" || begin == "gather" ||
            begin == "gather*" || startsWith(line, "\\[")) {
            mathEnv = begin;
            inMath = true;
            const bool aligned = begin.rfind("align", 0) == 0;
            out << "$$\n" << (aligned ? "\\begin{aligned}\n" : "");
            continue;
        }
        if (begin == "itemize" || begin == "enumerate") {
            lists.push_back(begin);
            continue;
        }
        if (const std::string end = envOf(line, "end"); end == "itemize" || end == "enumerate") {
            if (!lists.empty()) {
                lists.pop_back();
            }
            out << "\n";
            continue;
        }
        if (line == "\\maketitle" || line == "\\tableofcontents" || line == "\\newpage" || line == "\\clearpage" ||
            startsWith(line, "\\label{")) {
            continue;
        }
        swapCommand(line, "section*", "## ", "");
        swapCommand(line, "section", "## ", "");
        swapCommand(line, "subsection*", "### ", "");
        swapCommand(line, "subsection", "### ", "");
        swapCommand(line, "subsubsection", "#### ", "");
        swapCommand(line, "chapter", "# ", "");
        swapCommand(line, "textbf", "**", "**");
        swapCommand(line, "emph", "*", "*");
        swapCommand(line, "textit", "*", "*");
        swapCommand(line, "texttt", "`", "`");
        swapCommand(line, "url", "", "");
        swapCommand(line, "label", "", "");
        if (startsWith(line, "\\item")) {
            std::string rest = line.substr(5);
            while (!rest.empty() && rest.front() == ' ') {
                rest.erase(rest.begin());
            }
            const std::string indent((lists.empty() ? 0 : lists.size() - 1) * 2, ' ');
            out << indent << (!lists.empty() && lists.back() == "enumerate" ? "1. " : "- ") << rest << "\n";
            continue;
        }
        out << line << "\n";
        if (startsWith(line, "#")) {
            out << "\n";
        }
    }
    return out.str();
}

} // namespace ke::modules
