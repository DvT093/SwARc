// kernel32: private profile (.ini) files. GetPrivateProfileStringA,
// GetPrivateProfileIntA, WritePrivateProfileStringA.
//
// Behaviour (Win32, Windows-1252 files):
//   - A file name without a path separator lives in the Windows directory
//     (C:\WINDOWS); anything else resolves like CreateFile.
//   - Sections and keys match case-insensitively; whitespace around names
//     and values is dropped; a value wrapped in matching single or double
//     quotes loses them. Text after ';' on a value line is part of the
//     value (only whole lines starting with ';' are comments).
//   - The file is read again whenever it changed on disk (games write it
//     between runs and between their own processes); writes go to the
//     per-user overlay through HostPath(kUpdate), so the install stays
//     untouched. Reads come from a parsed copy otherwise: games that read
//     thousands of keys while they load would spend seconds re-reading
//     and re-parsing the file for each one.
//   - Enumeration: section NULL lists all section names, key NULL lists
//     the section's keys, each NUL-terminated, the list ending in a second
//     NUL. Truncated lists return size - 2, truncated strings size - 1.

#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "runtime/log.h"
#include "win32/hle.h"
#include "win32/kernel32_state.h"
#include "win32/win32.h"

namespace recomp::hle {
namespace {

using namespace win32;

struct Line {
  std::string text;     // the raw line, without its line break
};

std::string Trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t");
  if (a == std::string::npos) return {};
  size_t b = s.find_last_not_of(" \t");
  return s.substr(a, b - a + 1);
}

std::string GuestProfilePath(const std::string& name) {
  if (name.find('\\') == std::string::npos && name.find('/') == std::string::npos &&
      name.find(':') == std::string::npos)
    return "C:\\WINDOWS\\" + name;
  return name;
}

std::vector<std::string> ReadHostLines(const std::string& host_file) {
  std::vector<std::string> lines;
  std::ifstream in(host_file, std::ios::binary);
  if (!in) return lines;
  std::string l;
  while (std::getline(in, l)) {
    if (!l.empty() && l.back() == '\r') l.pop_back();
    lines.push_back(l);
  }
  return lines;
}

std::vector<std::string> ReadLines(const std::string& guest_file) {
  return ReadHostLines(HostPath(guest_file));
}

bool IsSection(const std::string& line, std::string* name) {
  std::string t = Trim(line);
  if (t.size() < 2 || t[0] != '[') return false;
  size_t close = t.find(']');
  if (close == std::string::npos) return false;
  *name = Trim(t.substr(1, close - 1));
  return true;
}

bool IsComment(const std::string& line) {
  std::string t = Trim(line);
  return t.empty() || t[0] == ';';
}

void SplitKey(const std::string& line, std::string* key, std::string* value) {
  size_t eq = line.find('=');
  if (eq == std::string::npos) {
    *key = Trim(line);
    value->clear();
    return;
  }
  *key = Trim(line.substr(0, eq));
  std::string v = Trim(line.substr(eq + 1));
  if (v.size() >= 2 && (v[0] == '"' || v[0] == '\'') && v.back() == v[0])
    v = v.substr(1, v.size() - 2);
  *value = v;
}

// Index range [begin, end) of a section's body lines, or false.
bool FindSection(const std::vector<std::string>& lines, const std::string& sec,
                 size_t* begin, size_t* end) {
  std::string want = Lower(sec), name;
  for (size_t i = 0; i < lines.size(); i++) {
    if (!IsSection(lines[i], &name) || Lower(name) != want) continue;
    size_t j = i + 1;
    while (j < lines.size() && !IsSection(lines[j], &name)) j++;
    *begin = i + 1;
    *end = j;
    return true;
  }
  return false;
}

// Copies a double-NUL-terminated list; returns what Win32 returns.
uint32_t PutList(gaddr buf, uint32_t cap, const std::vector<std::string>& items) {
  if (!buf || cap == 0) return 0;
  if (cap == 1) {
    Wr8(buf, 0);
    return 0;
  }
  uint32_t pos = 0;
  for (const std::string& it : items) {
    if (pos + it.size() + 2 > cap) {
      // Truncated: copy what fits, keep the double NUL, return cap - 2.
      std::string part = it.substr(0, cap - 2 - pos);
      for (char ch : part) Wr8(buf + pos++, uint8_t(ch));
      Wr8(buf + cap - 2, 0);
      Wr8(buf + cap - 1, 0);
      return cap - 2;
    }
    for (char ch : it) Wr8(buf + pos++, uint8_t(ch));
    Wr8(buf + pos++, 0);
  }
  Wr8(buf + pos, 0);
  return pos;
}

uint32_t PutString(gaddr buf, uint32_t cap, const std::string& s) {
  if (!buf || cap == 0) return 0;
  std::string out = s.size() + 1 > cap ? s.substr(0, cap - 1) : s;
  PutStr(buf, out);
  return uint32_t(out.size());
}

// A profile file as GetPrivateProfile* sees it.
struct Profile {
  std::vector<std::string> sections;  // every section header, in order
  struct Section {
    std::vector<std::string> keys;                         // in order
    std::unordered_map<std::string, std::string> values;  // lower key -> first value
  };
  std::unordered_map<std::string, Section> by_name;  // lower name -> first such section
};

Profile Parse(const std::vector<std::string>& lines) {
  Profile p;
  Profile::Section* cur = nullptr;
  std::string name;
  for (const std::string& l : lines) {
    if (IsSection(l, &name)) {
      p.sections.push_back(name);
      auto [it, fresh] = p.by_name.try_emplace(Lower(name));
      cur = fresh ? &it->second : nullptr;  // a repeated section is never found
      continue;
    }
    if (!cur || IsComment(l)) continue;
    std::string k, v;
    SplitKey(l, &k, &v);
    cur->keys.push_back(k);
    cur->values.try_emplace(Lower(k), v);
  }
  return p;
}

struct timespec MTime(const struct stat& st) {
#if defined(__APPLE__)
  return st.st_mtimespec;
#else
  return st.st_mtim;
#endif
}

struct Cached {
  uint64_t generation = 0;  // HostWriteGeneration() when resolved
  std::string host;
  bool exists = false;
  struct timespec mtime{};
  off_t size = 0;
  ino_t ino = 0;
  std::shared_ptr<const Profile> profile;
};

std::mutex g_profile_mu;
std::map<std::string, Cached> g_profiles;  // by current dir + guest file

// The parsed file, read again when it changed on disk or a write may have
// moved it (into the overlay).
std::shared_ptr<const Profile> Load(const std::string& guest_file) {
  const std::string id = GuestCurrentDir() + "\n" + guest_file;
  const uint64_t gen = HostWriteGeneration();
  std::lock_guard<std::mutex> l(g_profile_mu);
  Cached& c = g_profiles[id];
  if (!c.profile || c.generation != gen) {
    c.host = HostPath(guest_file);
    c.generation = gen;
    c.profile.reset();
  }
  struct stat st;
  const bool exists = stat(c.host.c_str(), &st) == 0;
  if (c.profile && exists == c.exists &&
      (!exists || (MTime(st).tv_sec == c.mtime.tv_sec &&
                   MTime(st).tv_nsec == c.mtime.tv_nsec && st.st_size == c.size &&
                   st.st_ino == c.ino)))
    return c.profile;
  c.exists = exists;
  if (exists) {
    c.mtime = MTime(st);
    c.size = st.st_size;
    c.ino = st.st_ino;
  }
  c.profile = std::make_shared<const Profile>(Parse(ReadHostLines(c.host)));
  return c.profile;
}

// The value of `key` (lower case) in section `sec`, or null.
const std::string* Find(const Profile& p, const std::string& sec, const std::string& key) {
  auto s = p.by_name.find(Lower(sec));
  if (s == p.by_name.end()) return nullptr;
  auto v = s->second.values.find(key);
  return v == s->second.values.end() ? nullptr : &v->second;
}

uint32_t GetProfileString(gaddr sec_va, gaddr key_va, gaddr def_va, gaddr buf,
                          uint32_t cap, const std::string& guest_file) {
  const std::shared_ptr<const Profile> p = Load(guest_file);
  if (!sec_va) return PutList(buf, cap, p->sections);
  std::string sec = GStr(sec_va);
  if (!key_va) {
    auto s = p->by_name.find(Lower(sec));
    return PutList(buf, cap, s == p->by_name.end() ? std::vector<std::string>{} : s->second.keys);
  }
  if (const std::string* v = Find(*p, sec, Lower(Trim(GStr(key_va)))))
    return PutString(buf, cap, *v);
  // Not found: the default, with trailing blanks removed.
  std::string def = def_va ? GStr(def_va) : std::string();
  size_t last = def.find_last_not_of(" \t");
  def = last == std::string::npos ? std::string() : def.substr(0, last + 1);
  return PutString(buf, cap, def);
}

// GetPrivateProfileStringA(section, key, default, buffer, size, file)
HLE("kernel32.dll", GetPrivateProfileStringA, 6) {
  std::string file = GuestProfilePath(GStr(Arg(c, 5)));
  return GetProfileString(Arg(c, 0), Arg(c, 1), Arg(c, 2), Arg(c, 3), Arg(c, 4), file);
}

// GetPrivateProfileIntA(section, key, default, file): the value's leading
// decimal number (optional sign); the default when the key is missing;
// 0 when the value does not start with a number.
HLE("kernel32.dll", GetPrivateProfileIntA, 4) {
  std::string file = GuestProfilePath(GStr(Arg(c, 3)));
  const std::shared_ptr<const Profile> p = Load(file);
  const std::string* v = Find(*p, GStr(Arg(c, 0)), Lower(Trim(GStr(Arg(c, 1)))));
  if (!v) return Arg(c, 2);
  const char* s = v->c_str();
  bool neg = false;
  if (*s == '-' || *s == '+') neg = *s++ == '-';
  uint32_t n = 0;
  while (*s >= '0' && *s <= '9') n = n * 10 + uint32_t(*s++ - '0');
  return neg ? uint32_t(0) - n : n;
}

// WritePrivateProfileStringA(section, key, string, file). key NULL deletes
// the section, string NULL deletes the key, section NULL only flushes.
HLE("kernel32.dll", WritePrivateProfileStringA, 4) {
  if (!Arg(c, 0)) return 1;
  std::string file = GuestProfilePath(GStr(Arg(c, 3)));
  std::vector<std::string> lines = ReadLines(file);
  std::string sec = GStr(Arg(c, 0));
  size_t b, e;
  bool have = FindSection(lines, sec, &b, &e);
  if (!Arg(c, 1)) {
    if (have) lines.erase(lines.begin() + long(b) - 1, lines.begin() + long(e));
  } else {
    std::string key = Trim(GStr(Arg(c, 1)));
    bool del = !Arg(c, 2);
    std::string entry = key + "=" + (del ? std::string() : GStr(Arg(c, 2)));
    if (!have) {
      if (del) return 1;
      if (!lines.empty() && !Trim(lines.back()).empty()) lines.push_back("");
      lines.push_back("[" + sec + "]");
      lines.push_back(entry);
    } else {
      bool done = false;
      for (size_t i = b; i < e && !done; i++) {
        if (IsComment(lines[i])) continue;
        std::string k, v;
        SplitKey(lines[i], &k, &v);
        if (Lower(k) != Lower(key)) continue;
        if (del) lines.erase(lines.begin() + long(i));
        else lines[i] = entry;
        done = true;
      }
      if (!done && !del) {
        // After the section's last non-blank line.
        size_t at = e;
        while (at > b && Trim(lines[at - 1]).empty()) at--;
        lines.insert(lines.begin() + long(at), entry);
      }
    }
  }
  std::ofstream out(HostPath(file, PathAccess::kUpdate), std::ios::binary | std::ios::trunc);
  if (!out) return 0;
  for (const std::string& l : lines) out << l << "\r\n";
  return out.good() ? 1 : 0;
}

}  // namespace
}  // namespace recomp::hle
