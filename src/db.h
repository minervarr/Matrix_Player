#pragma once
#include "library.h"
#include <string>
#include <map>

struct EqAssignment {
    std::string name;
    std::string source;
    std::string form;
};

class Db {
public:
    bool open(const std::string& dbPath);
    void close();

    void saveTracks(const std::vector<Track>& tracks);
    std::vector<Track> loadTracks();

    void saveAlbums(const std::vector<Album>& albums);
    std::vector<Album> loadAlbums();

    void saveSetting(const std::string& key, const std::string& value);
    std::string loadSetting(const std::string& key);

    void addMusicRoot(const std::string& path);
    void removeMusicRoot(const std::string& path);
    std::vector<std::string> loadMusicRoots();

    std::map<std::string, FileCache> loadFileCache();
    void removeTracksByPaths(const std::vector<std::string>& paths);

    void saveEqAssignment(const std::string& deviceKey,
                          const std::string& name,
                          const std::string& source,
                          const std::string& form);
    void clearEqAssignment(const std::string& deviceKey);
    bool loadEqAssignment(const std::string& deviceKey, EqAssignment& out);

    ~Db() { close(); }

private:
    struct Impl;
    Impl* impl_ = nullptr;
};
