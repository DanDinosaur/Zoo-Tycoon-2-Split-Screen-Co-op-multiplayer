#include <windows.h>
#include <mmsystem.h>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include "callsnd.h"
#include "log.h"
#include "miniz.h"

namespace {
    CRITICAL_SECTION g_cs; bool g_init = false;
    std::vector<mz_zip_archive*> g_zips;           // load order (alphabetical); search from the back
    std::vector<std::string> g_names;
    std::map<std::string, std::vector<char>> g_cache;   // factory -> wav bytes (empty = none)
    void OpenAll() {
        char dir[MAX_PATH]; GetModuleFileNameA(nullptr, dir, MAX_PATH); char* s = strrchr(dir, '\\'); if (s) s[1] = 0;
        std::vector<std::string> files;
        WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA((std::string(dir) + "*.z2f").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) { do files.push_back(fd.cFileName); while (FindNextFileA(h, &fd)); FindClose(h); }
        std::sort(files.begin(), files.end(), [](const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) < 0; });
        for (auto& f : files) {
            auto* z = new mz_zip_archive(); memset(z, 0, sizeof *z);
            if (mz_zip_reader_init_file(z, (std::string(dir) + f).c_str(), 0)) { g_zips.push_back(z); g_names.push_back(f); }
            else delete z;
        }
        LOG("callsnd: %u game archives open for animal calls", (unsigned)g_zips.size());
    }
    bool Read(const std::string& path, std::vector<char>& out) {
        std::string p = path; std::replace(p.begin(), p.end(), '\\', '/');
        for (int i = (int)g_zips.size() - 1; i >= 0; --i) {
            int idx = mz_zip_reader_locate_file(g_zips[i], p.c_str(), nullptr, 0);   // 0 = case-insensitive
            if (idx < 0) continue;
            size_t n = 0; void* d = mz_zip_reader_extract_to_heap(g_zips[i], idx, &n, 0);
            if (!d) continue;
            out.assign((char*)d, (char*)d + n); mz_free(d); return true;
        }
        return false;
    }
    std::string Lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
    // factory "<Species>_<Age>_<anim>" -> wav bytes
    bool Resolve(const std::string& species, const std::string& factory, std::vector<char>& wav) {
        std::vector<char> xml;
        if (!Read("entities/sound/" + species + ".xml", xml)) return false;
        std::string x(xml.begin(), xml.end()), lx = Lower(x);
        size_t i = lx.find("name=\"" + Lower(factory) + "\"");
        if (i == std::string::npos) return false;
        size_t j = lx.find("<snd", i); if (j == std::string::npos) return false;
        size_t k = lx.find("name=\"", j); if (k == std::string::npos) return false; k += 6;
        size_t e = x.find('"', k); if (e == std::string::npos) return false;
        std::string wavPath = x.substr(k, e - k);
        bool ok = Read(wavPath, wav);
        LOG("callsnd: %s -> %s%s", factory.c_str(), wavPath.c_str(), ok ? "" : " (file not found)");
        return ok;
    }
    // ZT18d (owner: every biting animal gets its best vanilla sound): the sound factories are named without the pose,
    // e.g. "CrocodileNile_Adult_Bite" for the Stand_Bite anim - and some only exist as Lunge / HeadStrike / Attacked.
    // Pick the best factory of this species + age: the anim name without its pose prefix first, then any attack sound
    // (bite > lunge / attack / strike / swipe / kick > roar / growl / hiss / snarl), sex-specific names allowed.
    bool BestOf(const std::string& species, const std::string& age, const std::string& anim, std::vector<char>& wav) {
        std::vector<char> xml;
        if (!Read("entities/sound/" + species + ".xml", xml)) return false;
        std::string x(xml.begin(), xml.end()), lx = Lower(x), la = Lower(age), an = Lower(anim);
        size_t us = an.rfind('_'); std::string core = us == std::string::npos ? an : an.substr(us + 1);   // stand_bitevicious -> bitevicious
        bool attack = false;
        for (const char* k : { "bite", "bit", "attack", "lunge", "swipe", "kick", "strike", "hit", "ram", "slam", "pounce", "peck", "tail", "spit", "gore" })
            if (core.find(k) != std::string::npos) attack = true;
        int best = 0; std::string bestName;
        for (size_t i = lx.find("name=\"" + la + "_"); i != std::string::npos; i = lx.find("name=\"" + la + "_", i + 1)) {
            size_t b = i + 6, e = lx.find('"', b); if (e == std::string::npos) break;
            std::string f = lx.substr(b, e - b); if (f.find(".wav") != std::string::npos) continue;
            std::string tail = f.substr(la.size() + 1);
            int sc = 0;
            if (tail == core || tail == "m_" + core || tail == "f_" + core) sc = 100;
            else if (!core.empty() && tail.find(core) != std::string::npos) sc = 90;
            else if (attack) {
                if (tail.find("bite") != std::string::npos) sc = 60;
                else if (tail.find("lunge") != std::string::npos || (tail.find("attack") != std::string::npos && tail.find("attacked") == std::string::npos) ||
                         tail.find("strike") != std::string::npos || tail.find("swipe") != std::string::npos || tail.find("kick") != std::string::npos) sc = 50;
                else if (tail.find("roar") != std::string::npos || tail.find("growl") != std::string::npos || tail.find("snarl") != std::string::npos ||
                         tail.find("hiss") != std::string::npos) sc = 40;
            }
            if (sc > best) { best = sc; bestName = x.substr(b, e - b); }
        }
        if (!best) return false;
        size_t i = lx.find("name=\"" + Lower(bestName) + "\"");
        size_t j = lx.find("<snd", i); if (j == std::string::npos) return false;
        size_t k = lx.find("name=\"", j); if (k == std::string::npos) return false; k += 6;
        size_t e = x.find('"', k); if (e == std::string::npos) return false;
        std::string wavPath = x.substr(k, e - k);
        bool ok = Read(wavPath, wav);
        LOG("callsnd: %s_%s -> best match %s -> %s%s", age.c_str(), anim.c_str(), bestName.c_str(), wavPath.c_str(), ok ? "" : " (file not found)");
        return ok;
    }
    struct Job { std::string type, anim; };
    DWORD WINAPI Worker(LPVOID p) {
        Job* j = (Job*)p;
        EnterCriticalSection(&g_cs);
        if (!g_init) { g_init = true; OpenAll(); }
        // "TyrannosaurusJPOG_Adult_F" -> species "TyrannosaurusJPOG", age prefix "TyrannosaurusJPOG_Adult"
        std::string t = j->type; size_t u1 = t.find('_'); std::string species = t.substr(0, u1);
        size_t u2 = u1 == std::string::npos ? std::string::npos : t.find('_', u1 + 1);
        std::string age = u2 == std::string::npos ? t : t.substr(0, u2);
        std::string key = age + "_" + j->anim;
        auto it = g_cache.find(key);
        if (it == g_cache.end()) {
            std::vector<char> w;
            if (!Resolve(species, key, w)) {
                std::string alt = species + (age.find("_Young") != std::string::npos ? "_Adult_" : "_Young_") + j->anim;
                if (!Resolve(species, alt, w) && !BestOf(species, age, j->anim, w) &&
                    !BestOf(species, species + (age.find("_Young") != std::string::npos ? "_Adult" : "_Young"), j->anim, w)) {
                    // ZT19 (owner: every croc must make a sound when it bites): a species without its own attack sound
                    // (Sarcosuchus, Deinosuchus... from add-on packs) borrows the nearest vanilla relative's best match
                    std::string ls = Lower(species), donor;
                    for (const char* k : { "sarcosuchus", "deinosuchus", "crocodil", "alligator", "caiman", "gharial", "gavial", "croc", "purussaurus", "kaprosuchus" })
                        if (ls.find(k) != std::string::npos) donor = "CrocodileNile";
                    if (!donor.empty() && Lower(donor) != ls && (Resolve(donor, donor + "_Adult_" + j->anim, w) || BestOf(donor, donor + "_Adult", j->anim, w)))
                        LOG("callsnd: %s has no sound of its own - using %s's", key.c_str(), donor.c_str());
                    else LOG("callsnd: no sound for %s", key.c_str());
                }
            }
            it = g_cache.emplace(key, std::move(w)).first;
        }
        if (!it->second.empty()) {
            PlaySoundA(it->second.data(), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
            LOG("callsnd: played %s", key.c_str());
        }
        LeaveCriticalSection(&g_cs);
        delete j; return 0;
    }
}
namespace callsnd {
void Play(const char* type, const char* anim) {
    static bool s_cs = false; if (!s_cs) { s_cs = true; InitializeCriticalSection(&g_cs); }
    Job* j = new Job{ type ? type : "", anim ? anim : "" };
    HANDLE h = CreateThread(nullptr, 0, Worker, j, 0, nullptr); if (h) CloseHandle(h); else delete j;
}
}
