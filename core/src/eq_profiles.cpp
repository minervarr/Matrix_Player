#include "core/eq_profiles.h"
#include "json.hpp"
#include <fstream>
#include <algorithm>
#include <cctype>
#include <cstdio>

using json = nlohmann::json;

bool EqProfileStore::load(const std::string& jsonPath) {
    std::ifstream file(jsonPath);
    if (!file.is_open()) {
        fprintf(stderr, "[EQ][ERROR] Failed to open %s\n", jsonPath.c_str());
        return false;
    }

    try {
        json arr = json::parse(file);
        profiles_.reserve(arr.size());

        for (auto& obj : arr) {
            EqProfile p;
            p.name   = obj.value("name", "");
            p.source = obj.value("source", "");
            p.form   = obj.value("form", "");
            p.preamp = obj.value("preamp", 0.0);

            for (auto& fo : obj["filters"]) {
                EqFilter f;
                f.type = fo.value("type", "PK");
                f.fc   = fo.value("fc", 1000.0);
                f.gain = fo.value("gain", 0.0);
                f.q    = fo.value("q", 1.0);
                p.filters.push_back(std::move(f));
            }
            profiles_.push_back(std::move(p));
        }

        std::sort(profiles_.begin(), profiles_.end(),
            [](const EqProfile& a, const EqProfile& b) {
                return std::lexicographical_compare(
                    a.name.begin(), a.name.end(), b.name.begin(), b.name.end(),
                    [](char x, char y) {
                        return std::tolower((unsigned char)x) < std::tolower((unsigned char)y);
                    });
            });

        printf("[EQ] Loaded %zu profiles from %s\n", profiles_.size(), jsonPath.c_str());
        return true;

    } catch (const std::exception& e) {
        fprintf(stderr, "[EQ][ERROR] JSON parse error: %s\n", e.what());
        return false;
    }
}

const EqProfile* EqProfileStore::findByKey(const std::string& name,
                                           const std::string& source,
                                           const std::string& form) const {
    for (auto& p : profiles_) {
        if (p.name == name && p.source == source && p.form == form)
            return &p;
    }
    return nullptr;
}
