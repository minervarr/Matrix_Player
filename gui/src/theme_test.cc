#undef NDEBUG
#include "theme.hh"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

static void assertContiguousGroups() {
    std::vector<std::string> closed;
    std::string cur;
    for (int i = 0; i < themeCount(); ++i) {
        const std::string& g = themeAt(i).group;
        if (g == cur) continue;
        for (const std::string& seen : closed)
            assert(seen != g && "a group reappears");
        if (!cur.empty()) closed.push_back(cur);
        cur = g;
    }
}

static const Palette* findId(const char* id) {
    for (int i = 0; i < themeCount(); ++i)
        if (themeAt(i).id == id) return &themeAt(i);
    return nullptr;
}

int main() {
    checkBuiltinThemes();
    assert(themeCount() == 24);
    assertContiguousGroups();

    const Palette* matrix = findId("matrix");
    const Palette* paper = findId("paper");
    const Palette* hyper = findId("hyper");
    const Palette* nord = findId("nord");
    const Palette* cat = findId("catppuccin");
    const Palette* tokyo = findId("tokyo-night");
    const Palette* rose = findId("rose-pine");
    const Palette* kana = findId("kanagawa");
    assert(matrix && paper && hyper && nord && cat && tokyo && rose && kana);
    assert(matrix->bgMain == RGB(10, 10, 10));
    assert(matrix->accent == RGB(0, 200, 83));
    assert(paper->bgMain == RGB(255, 255, 255));
    assert(hyper->accent == RGB(0, 255, 255));
    assert(nord->bgMain == RGB(46, 52, 64));
    assert(nord->accent == RGB(136, 192, 208));
    assert(cat->accent == RGB(203, 166, 247));
    assert(tokyo->accent == RGB(122, 162, 247));
    assert(rose->accent == RGB(235, 111, 146));
    assert(rose->error == RGB(255, 110, 110));
    assert(kana->accent == RGB(127, 180, 202));

    // The original eight ids are still there, so a saved ui_theme resolves.
    for (const char* id : { "matrix", "ink", "paper", "nordic", "dracula",
                            "pink", "hyper", "chicago95" })
        assert(findId(id) != nullptr);

    assert(applyTheme("matrix"));
    assert(themeRule() == CLR_SEPARATOR);
    assert(themeButtonFill(true, false) == CLR_ACCENT);
    assert(applyTheme("paper"));
    assert(themeRule() == CLR_SEPARATOR);
    assert(applyTheme("hyper"));
    assert(themeRule() == CLR_SEPARATOR);
    assert(std::string(activeThemeId()) == "hyper");

    const char* nordText =
        "# Colors (Nord)\n"
        "[colors.primary]\n"
        "background = '#2E3440'\n"
        "foreground = '#D8DEE9'\n"
        "[colors.normal]\n"
        "black   = '#3B4252'\n"
        "red     = '#BF616A'\n"
        "green   = '#A3BE8C'\n"
        "yellow  = '#EBCB8B'\n"
        "blue    = '#81A1C1'\n"
        "magenta = '#B48EAD'\n"
        "cyan    = '#88C0D0'\n"
        "white   = '#E5E9F0'\n"
        "[colors.bright]\n"
        "black   = '#4C566A'\n"
        "red     = '#BF616A'\n"
        "green   = '#A3BE8C'\n"
        "yellow  = '#EBCB8B'\n"
        "blue    = '#81A1C1'\n"
        "magenta = '#B48EAD'\n"
        "cyan    = '#8FBCBB'\n"
        "white   = '#ECEFF4'\n";
    Palette parsed;
    std::string err;
    if (!parseThemeText(nordText, "nord-file", parsed, err)) {
        fprintf(stderr, "nord: %s\n", err.c_str());
        assert(false);
    }
    assert(parsed.id == "nord-file");
    assert(parsed.name == "Nord");
    assert(parsed.group == "Imported");
    assert(parsed.bgMain == RGB(46, 52, 64));
    assert(parsed.accent == RGB(136, 192, 208));
    assert(paletteAcceptable(parsed));

    assert(!parseThemeText("this is not a theme\n", "x", parsed, err));

    const char* direct =
        "id = custom-ink\n"
        "name = Custom Ink\n"
        "group = Imported\n"
        "blurb = A direct file.\n"
        "bg = #0a0a0a\n"
        "text = #f2f2f2\n"
        "accent = #00c853\n";
    if (!parseThemeText(direct, "custom-ink", parsed, err)) {
        fprintf(stderr, "direct: %s\n", err.c_str());
        assert(false);
    }
    assert(parsed.id == "custom-ink");
    assert(parsed.name == "Custom Ink");
    assert(parsed.blurb == "A direct file.");
    assert(parsed.bgMain == RGB(10, 10, 10));
    assert(parsed.accent == RGB(0, 200, 83));
    assert(paletteAcceptable(parsed));

    // A file that names every slot, and names them so they fail, is refused.
    // Deriving from bg/text/accent alone would have repaired it.
    const char* bad =
        "id = bad\nname = Bad\ngroup = Imported\nblurb = no\n"
        "bg = #ffffff\nbg.track = #ffffff\nbg.sidebar = #ffffff\nbg.transport = #ffffff\n"
        "text = #eeeeee\ntext.secondary = #eeeeee\ntext.dim = #eeeeee\ntext.album = #eeeeee\n"
        "accent = #dddddd\nhover = #f0f0f0\nseparator = #f8f8f8\n"
        "input = #ffffff\nplaceholder = #ffffff\ntile = #cccccc\n"
        "error = #eeeeee\nwarning = #eeeeee\n"
        "quality.dsd = #ffffff\nquality.dxd = #ffffff\n"
        "quality.hires = #ffffff\nquality.standard = #ffffff\n";
    assert(!parseThemeText(bad, "bad", parsed, err));

    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "matrix-player-theme-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        std::ofstream((dir / "matrix.theme").string())
            << "id = matrix\nname = Fake\nbg = #000000\ntext = #ffffff\naccent = #00ff00\n";
        std::ofstream((dir / "hello.theme").string())
            << "id = hello-theme\nname = Hello\ngroup = Imported\n"
               "blurb = Hi.\nbg = #111111\ntext = #eeeeee\naccent = #33cc77\n";
        std::ofstream((dir / "again.theme").string())
            << "id = hello-theme\nname = Second\nbg = #111111\ntext = #eeeeee\naccent = #33cc77\n";
        std::ofstream((dir / "aaa-night.theme").string())
            << "id = aaa-night\nname = Extra Night\ngroup = Night\n"
               "blurb = Joins the night group.\nbg = #101820\ntext = #e8eef8\naccent = #70c0d0\n";
        std::ofstream((dir / "garbage.theme").string()) << "not a theme at all\n";
    }

    const int added = reloadImportedThemes(dir.string());
    assert(added == 2);   // hello-theme and aaa-night; matrix and the duplicate skipped
    assert(themeCount() == 26);
    const Palette* still = findId("matrix");
    assert(still && still->name == "Matrix");
    assert(GetRValue(still->bgMain) == 10);
    assert(findId("hello-theme") != nullptr);
    assert(findId("aaa-night") != nullptr);
    assertContiguousGroups();

    // aaa-night sorts first and joins Night, so it sits before Day.
    int nightAt = -1, dayAt = -1, helloAt = -1;
    for (int i = 0; i < themeCount(); ++i) {
        if (themeAt(i).id == "aaa-night") nightAt = i;
        if (themeAt(i).id == "catppuccin-latte") dayAt = i;
        if (themeAt(i).id == "hello-theme") helloAt = i;
    }
    assert(nightAt >= 0 && dayAt > nightAt);
    assert(helloAt == themeCount() - 1);

    assert(applyTheme("paper"));
    assert(reloadImportedThemes(dir.string()) == 2);
    assert(std::string(activeThemeId()) == "paper");
    assert(activeThemeIndex() >= 0);
    assert(themeAt(activeThemeIndex()).id == "paper");

    assert(applyTheme("hello-theme"));
    const ColorRef keptBg = CLR_BG_MAIN;
    const ColorRef keptAccent = CLR_ACCENT;
    fs::remove(dir / "hello.theme");
    fs::remove(dir / "again.theme");
    fs::remove(dir / "aaa-night.theme");
    assert(reloadImportedThemes(dir.string()) == 0);
    assert(activeThemeIndex() < 0);
    assert(std::string(activeThemeId()) == "hello-theme");
    // The applied tokens stay. Nothing in the list is highlighted, and the
    // page does not snap back to Matrix just because the file went away.
    assert(CLR_BG_MAIN == keptBg);
    assert(CLR_ACCENT == keptAccent);

    fs::remove_all(dir);
    applyTheme("matrix");
    return 0;
}
