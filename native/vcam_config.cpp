#include "vcam_config.h"
#include <sys/stat.h>
#include <fstream>
#include <sstream>
#include <cstdlib>

namespace vcam {

// --- Mini parser JSON (sin dependencias, para config plana) ---
static std::string jget(const std::string& s, const std::string& key) {
    std::string pat = "\"" + key + "\"";
    size_t p = s.find(pat);
    if (p == std::string::npos) return "";
    p = s.find(':', p + pat.size());
    if (p == std::string::npos) return "";
    p++;
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) p++;
    if (s[p] == '"') { size_t e = s.find('"', p + 1); return s.substr(p + 1, e - p - 1); }
    size_t e = p;
    while (e < s.size() && s[e] != ',' && s[e] != '}' && s[e] != '\n') e++;
    return s.substr(p, e - p);
}
static float jnum(const std::string& s, const std::string& k, float def) {
    std::string v = jget(s, k);
    return v.empty() ? def : strtof(v.c_str(), nullptr);
}

ConfigManager& ConfigManager::get() {
    static ConfigManager inst;
    return inst;
}

void ConfigManager::start(const std::string& path) {
    std::lock_guard<std::mutex> l(mtx_);
    path_ = path + "/config.json";
    struct stat st{};
    if (stat(path_.c_str(), &st) == 0) lastMtime_ = st.st_mtime;
}

bool ConfigManager::reloadIfChanged() {
    struct stat st{};
    if (stat(path_.c_str(), &st) != 0) return false;
    if (st.st_mtime == lastMtime_) return false;
    lastMtime_ = st.st_mtime;

    std::ifstream f(path_);
    std::stringstream ss; ss << f.rdbuf();
    std::string s = ss.str();

    std::lock_guard<std::mutex> l(mtx_);
    cfg_.active       = jget(s, "active") == "true";
    cfg_.mode         = jget(s, "mode",   cfg_.mode);
    cfg_.source       = jget(s, "source", cfg_.source);
    cfg_.panX         = jnum(s, "panX", cfg_.panX);
    cfg_.panY         = jnum(s, "panY", cfg_.panY);
    cfg_.zoom         = jnum(s, "zoom", cfg_.zoom);
    cfg_.rotation     = (int)jnum(s, "rotation", cfg_.rotation);
    cfg_.brightness   = jnum(s, "brightness", cfg_.brightness);
    cfg_.contrast     = jnum(s, "contrast", cfg_.contrast);
    cfg_.noiseLevel   = jnum(s, "noiseLevel", cfg_.noiseLevel);
    cfg_.dustLevel    = jnum(s, "dustLevel", cfg_.dustLevel);
    cfg_.autoFocusWobble = jget(s, "autoFocusWobble") != "false";
    cfg_.aiFaceBypass = jget(s, "aiFaceBypass") != "false";
    cfg_.microMoveScale   = jnum(s, "microMoveScale", cfg_.microMoveScale);
    cfg_.microMoveRotation= jnum(s, "microMoveRotation", cfg_.microMoveRotation);
    return true;
}

Config ConfigManager::snapshot() {
    std::lock_guard<std::mutex> l(mtx_);
    return cfg_;
}

} // namespace vcam
