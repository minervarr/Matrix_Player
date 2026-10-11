// Import and export a folder of theme files.
//
//   theme_pack import <src> <dst> [--replace]
//   theme_pack export <src> <dst>
//
// import reads the .theme and .toml files in <src>, not its subfolders.
// Each file is placed in morning, afternoon, evening, or night from its
// background. A file that already says one of those four words keeps it.
// The word is written on the group line, which is the line a person edits.
// A built-in id is written as alacritty-<file> so it does not cover the
// palette that shipped with the app. The body of the file is left as it
// was, and the result is <dst>/<id>.theme. An existing file is left in
// place unless --replace is given, so a later edit of the group line
// survives another import.
//
// export copies the theme files and rewrites INDEX.txt beside them.
//
// The app reads the group line inside each file. INDEX.txt is for a person.
// A file that fails the contrast rules is named and not written.
//
// Exit 0 when the folders could be read, including when some files were
// left out. Exit 1 when a folder could not. Exit 2 when the command is
// not one of the two above.

#include "theme.hh"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

static std::string trim(std::string s) {
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    size_t b = s.size();
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

static std::string sanitize(const std::string& s) {
    std::string o;
    bool dash = false;
    for (unsigned char ch : s) {
        char c = (char)ch;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            o.push_back(c);
            dash = false;
        } else if (!o.empty() && !dash) {
            o.push_back('-');
            dash = true;
        }
    }
    while (!o.empty() && o.back() == '-') o.pop_back();
    return o;
}

static std::string readFile(const fs::path& path) {
    std::ifstream in(path);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

static bool themeExt(const fs::path& path) {
    const std::string ext = lower(path.extension().string());
    return ext == ".theme" || ext == ".toml";
}

static bool listThemes(const fs::path& dir, std::vector<fs::path>& out, std::string& error) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec) || ec) {
        error = "could not read " + dir.string();
        return false;
    }
    fs::directory_iterator it(dir, ec);
    if (ec) {
        error = "could not read " + dir.string() + " (" + ec.message() + ")";
        return false;
    }
    for (; it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        std::error_code fec;
        if (!it->is_regular_file(fec) || fec) continue;
        if (themeExt(it->path())) out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    return true;
}

static bool sameFolder(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    if (!fs::exists(a, ec) || !fs::exists(b, ec)) return false;
    return fs::equivalent(a, b, ec) && !ec;
}

static bool identityLine(const std::string& raw) {
    const std::string s = trim(raw);
    if (s.empty() || s[0] == '#' || s[0] == '[') return false;
    const auto eq = s.find('=');
    if (eq == std::string::npos) return false;
    const std::string key = lower(trim(s.substr(0, eq)));
    return key == "id" || key == "name" || key == "group";
}

// Drop id, name, and group from the text before the first [section].
// A later copy of one of those keys would overwrite the line we write.
static std::string stripIdentity(const std::string& text) {
    std::string out;
    bool section = false;
    size_t i = 0;
    while (i <= text.size()) {
        size_t nl = text.find('\n', i);
        const bool last = nl == std::string::npos;
        if (last) nl = text.size();
        std::string line = text.substr(i, nl - i);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!section) {
            const std::string t = trim(line);
            if (!t.empty() && t.front() == '[' && t.back() == ']') section = true;
            else if (identityLine(line)) {
                if (last) break;
                i = nl + 1;
                continue;
            }
        }
        out += line;
        out.push_back('\n');
        if (last) break;
        i = nl + 1;
    }
    return out;
}

static std::string quoteName(std::string name) {
    std::string o;
    for (char c : name) {
        if (c == '"' || c == '\'' || c == '\n' || c == '\r') continue;
        o.push_back(c);
    }
    o = trim(o);
    if (o.empty()) o = "Theme";
    return "\"" + o + "\"";
}

static std::string chooseId(const Palette& parsed, const std::string& stem) {
    std::string id = sanitize(parsed.id);
    if (id.empty()) id = sanitize(stem);
    if (id.empty()) id = "theme";
    if (themeIsBuiltinId(id.c_str())) {
        std::string fromFile = sanitize(stem);
        if (fromFile.empty()) fromFile = "theme";
        id = "alacritty-" + fromFile;
        if (themeIsBuiltinId(id.c_str())) id += "-file";
    }
    return id;
}

struct Listed {
    std::string name;
    std::string id;
    std::string file;
    int part = 0;
};

static void writeIndex(const fs::path& dst, std::vector<Listed> rows) {
    std::sort(rows.begin(), rows.end(), [](const Listed& a, const Listed& b) {
        if (a.part != b.part) return a.part < b.part;
        const std::string na = lower(a.name);
        const std::string nb = lower(b.name);
        if (na != nb) return na < nb;
        return a.id < b.id;
    });
    std::ofstream out((dst / "INDEX.txt").string());
    if (!out) {
        std::cout << "could not write " << (dst / "INDEX.txt").string() << "\n";
        return;
    }
    out << "Matrix Player theme index\n"
        << "The app reads the group line inside each file. This list is for a person.\n"
        << "A source file that already says group = evening keeps evening; "
           "otherwise the background decides.\n"
        << "\n";
    const char* titles[] = { "Morning", "Afternoon", "Evening", "Night" };
    for (int g = 0; g < 4; ++g) {
        out << titles[g] << "\n";
        for (const Listed& row : rows) {
            if (row.part != g) continue;
            out << row.name << "\t" << row.id << "\t" << row.file << "\n";
        }
        out << "\n";
    }
}

static std::vector<Listed> indexDir(const fs::path& dir) {
    std::vector<fs::path> files;
    std::string error;
    std::vector<Listed> rows;
    if (!listThemes(dir, files, error)) return rows;
    for (const fs::path& path : files) {
        const std::string text = readFile(path);
        Palette parsed;
        std::string err;
        if (!parseThemeText(text, path.stem().string(), parsed, err)) continue;
        Listed row;
        row.name = parsed.name;
        row.id = parsed.id;
        row.file = path.filename().string();
        row.part = (int)themeDaypartOf(parsed);
        rows.push_back(std::move(row));
    }
    return rows;
}

static int usage() {
    std::cout
        << "theme_pack import <src> <dst> [--replace]\n"
        << "theme_pack export <src> <dst>\n"
        << "\n"
        << "import writes one .theme file per source into <dst>, with a group\n"
        << "line of morning, afternoon, evening, or night. export copies the\n"
        << "theme files and rewrites INDEX.txt.\n";
    return 2;
}

static int cmdImport(const fs::path& src, const fs::path& dst, bool replace) {
    if (sameFolder(src, dst)) {
        std::cout << "source and destination are the same folder\n";
        return 1;
    }
    std::vector<fs::path> files;
    std::string error;
    if (!listThemes(src, files, error)) {
        std::cout << error << "\n";
        return 1;
    }
    std::error_code ec;
    fs::create_directories(dst, ec);
    if (ec || !fs::is_directory(dst)) {
        std::cout << "could not create " << dst.string() << "\n";
        return 1;
    }
    if (sameFolder(src, dst)) {
        std::cout << "source and destination are the same folder\n";
        return 1;
    }

    int wrote = 0, already = 0, left = 0;
    std::set<std::string> seen;
    for (const fs::path& path : files) {
        const std::string text = readFile(path);
        if (text.empty() && !fs::is_regular_file(path)) {
            std::cout << path.filename().string() << ": could not read\n";
            ++left;
            continue;
        }
        Palette parsed;
        std::string err;
        if (!parseThemeText(text, path.stem().string(), parsed, err)) {
            std::cout << path.filename().string() << ": " << err << "\n";
            ++left;
            continue;
        }
        ThemeDaypart part;
        if (!themeDaypartFromName(parsed.group, part))
            part = themeDaypartFromLuma(themeLuma(parsed.bgMain));
        const std::string id = chooseId(parsed, path.stem().string());
        if (!seen.insert(id).second) {
            std::cout << path.filename().string() << ": id " << id
                      << " already written this run\n";
            ++left;
            continue;
        }
        const fs::path dest = dst / (id + ".theme");
        if (!replace && fs::exists(dest)) {
            ++already;
            continue;
        }
        const std::string stamped =
            "id = " + id + "\n"
            "name = " + quoteName(parsed.name) + "\n"
            "group = " + themeDaypartId(part) + "\n"
            "\n" + stripIdentity(text);
        Palette again;
        std::string againErr;
        if (!parseThemeText(stamped, id, again, againErr) || again.id != id) {
            seen.erase(id);
            std::cout << path.filename().string() << ": "
                      << (againErr.empty() ? "stamp did not parse" : againErr) << "\n";
            ++left;
            continue;
        }
        std::ofstream out(dest.string(), std::ios::binary);
        if (!out) {
            seen.erase(id);
            std::cout << dest.filename().string() << ": could not write\n";
            ++left;
            continue;
        }
        out << stamped;
        ++wrote;
    }
    writeIndex(dst, indexDir(dst));
    std::cout << "wrote " << wrote << "\n"
              << "already " << already << "\n"
              << "left out " << left << "\n";
    return 0;
}

static int cmdExport(const fs::path& src, const fs::path& dst) {
    if (sameFolder(src, dst)) {
        std::cout << "source and destination are the same folder\n";
        return 1;
    }
    std::vector<fs::path> files;
    std::string error;
    if (!listThemes(src, files, error)) {
        std::cout << error << "\n";
        return 1;
    }
    std::error_code ec;
    fs::create_directories(dst, ec);
    if (ec || !fs::is_directory(dst)) {
        std::cout << "could not create " << dst.string() << "\n";
        return 1;
    }
    if (sameFolder(src, dst)) {
        std::cout << "source and destination are the same folder\n";
        return 1;
    }
    int wrote = 0, left = 0;
    for (const fs::path& path : files) {
        std::error_code cec;
        fs::copy_file(path, dst / path.filename(),
                      fs::copy_options::overwrite_existing, cec);
        if (cec) {
            std::cout << path.filename().string() << ": " << cec.message() << "\n";
            ++left;
            continue;
        }
        ++wrote;
    }
    writeIndex(dst, indexDir(dst));
    std::cout << "wrote " << wrote << "\n"
              << "left out " << left << "\n";
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    const std::string cmd = argv[1];
    if (cmd == "import") {
        if (argc != 4 && argc != 5) return usage();
        bool replace = false;
        if (argc == 5) {
            if (std::string(argv[4]) != "--replace") return usage();
            replace = true;
        }
        return cmdImport(argv[2], argv[3], replace);
    }
    if (cmd == "export") {
        if (argc != 4) return usage();
        return cmdExport(argv[2], argv[3]);
    }
    return usage();
}
