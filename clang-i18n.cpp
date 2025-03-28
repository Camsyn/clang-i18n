// SPDX-License-Identifier: MIT License
// Copyright (c) 2025 Yingwei Zheng
// This file is licensed under the MIT License.
// See the LICENSE file for more information.

#include <llvm/ADT/ScopeExit.h>
#include <llvm/ADT/StringExtras.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Option/OptTable.h>
#include <llvm/Option/Option.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SHA1.h>
#include <llvm/Support/raw_ostream.h>
#include <clang/Basic/DiagnosticIDs.h>
#include <atomic>
#include <cstdlib>
#include <dlfcn.h>
#include <string>
#include <unordered_map>

using clang::StringRef;

static StringRef getLang() {
  if (auto *Lang = std::getenv("CLANG_I18N_LANG"))
    return StringRef{Lang}.split('.').first;
  if (auto *Lang = std::getenv("LANG"))
    return StringRef{Lang}.split('.').first;
  return "";
}

static StringRef getTranslationDir() {
  if (auto *Path = std::getenv("CLANG_I18N_TRANSLATION_DIR"))
    return Path;
  return "/usr/share/clang-i18n";
}

class TranslationTable {
  std::unordered_map<std::string, std::string> Table;

  static void unescape(std::string &Str) {
    uint32_t Pos = 0;
    for (uint32_t I = 0; I != Str.size(); ++I) {
      if (Str[I] == '\\' && I != Str.size() - 1) {
        switch (Str[I + 1]) {
        default:
          llvm_unreachable("Unexpected escape character");
        case 'n':
          Str[Pos++] = '\n';
          break;
        case '"':
          Str[Pos++] = '\"';
          break;
        case '\'':
          Str[Pos++] = '\'';
          break;
        case '\\':
          Str[Pos++] = '\\';
          break;
        }
      } else {
        Str[Pos++] = Str[I];
      }
    }
  }

public:
  static std::string Hash(StringRef Src) {
    llvm::SHA1 S;
    S.update(Src);
    auto Res = S.result();
    return llvm::toHex(Res).substr(0, 12);
  }

  TranslationTable() {
    using namespace llvm;
    auto Lang = getLang();
    if (Lang.empty() || Lang == "en_US" || Lang == "en_UK")
      return;
    auto TranslationDir = getTranslationDir();
    auto Path = TranslationDir.str() + "/" + Lang.str() + ".yml";
    auto MapFile = MemoryBuffer::getFile(Path, true);
    if (!MapFile) {
      fprintf(stderr, "Failed to open translation file: %s\n", Path.c_str());
      return;
    }

    SmallVector<StringRef, 0> Lines;
    (*MapFile)->getBuffer().split(Lines, '\n');
    for (auto Line : Lines) {
      if (!Line.starts_with('H'))
        continue;
      auto Key = Line.substr(1, 12);
      auto Val = Line.substr(15).drop_front().drop_back().str();
      unescape(Val);
      Table[Key.str()] = Val;
    }
  }

  StringRef replace(StringRef Src) const {
    auto It = Table.find(Hash(Src.str()));
    return It == Table.end() ? Src : It->second;
  }
};

static StringRef replace(StringRef Src) {
  static TranslationTable Table;
  return Table.replace(Src);
}

static void *getRealFuncAddrImpl(const char *ManagledName,
                                 void *InterceptorAddr) {
  void *RealAddr = dlsym(RTLD_NEXT, ManagledName);
  if (!RealAddr) {
    RealAddr = dlsym(RTLD_DEFAULT, ManagledName);
    if (RealAddr == InterceptorAddr)
      RealAddr = nullptr;
  }
  return RealAddr;
}

template <typename F> static F *getRealFuncAddr(F *InterceptorFunc) {
  void *InterceptorAddr;
  std::memcpy(&InterceptorAddr, &InterceptorFunc, sizeof(InterceptorAddr));
  Dl_info Info;
  dladdr(InterceptorAddr, &Info);
  void *RealFuncAddr = getRealFuncAddrImpl(Info.dli_sname, InterceptorAddr);
  assert(RealFuncAddr && "Failed to find the real function address");
  F *RealFunc;
  std::memcpy(&RealFunc, &RealFuncAddr, sizeof(RealFunc));
  return RealFunc;
}

template <typename T, typename F>
static F T::*getRealFuncAddr(F T::*InterceptorFunc) {
  void *InterceptorAddr;
  std::memcpy(&InterceptorAddr, &InterceptorFunc, sizeof(InterceptorAddr));
  Dl_info Info;
  dladdr(InterceptorAddr, &Info);
  void *RealFuncAddr = getRealFuncAddrImpl(Info.dli_sname, InterceptorAddr);
  assert(RealFuncAddr && "Failed to find the real function address");
  F T::*RealFunc;
  std::memcpy(&RealFunc, &RealFuncAddr, sizeof(RealFunc));
  return RealFunc;
}

#define INTERCEPTOR_ATTRIBUTE __attribute__((visibility("default")))

namespace llvm {
class ReplaceStream final : public raw_ostream {
  raw_ostream &OS;

public:
  // Turn off buffering to avoid using the fast path.
  explicit ReplaceStream(raw_ostream &OS)
      : raw_ostream(true, OS.get_kind()), OS(OS) {}

  void write_impl(const char *Ptr, size_t Size) override {
    auto Rep = ::replace(StringRef{Ptr, Size});
    OS.write(Rep.data(), Rep.size());
  }

  uint64_t current_pos() const override { return OS.tell(); }
};
} // namespace llvm

namespace clang {

INTERCEPTOR_ATTRIBUTE
StringRef DiagnosticIDs::getDescription(unsigned DiagID) const {
  static auto RealFunc = getRealFuncAddr(&DiagnosticIDs::getDescription);
  return replace((this->*RealFunc)(DiagID));
}

} // namespace clang

namespace llvm {

namespace opt {

INTERCEPTOR_ATTRIBUTE
void OptTable::printHelp(raw_ostream &OS, const char *Usage, const char *Title,
                         bool ShowHidden, bool ShowAllAliases,
                         Visibility VisibilityMask) const {
  ReplaceStream Wrapper{OS};
  return internalPrintHelp(
      Wrapper, Usage, Title, ShowHidden, ShowAllAliases,
      [VisibilityMask](const Info &CandidateInfo) -> bool {
        return (CandidateInfo.Visibility & VisibilityMask) == 0;
      },
      VisibilityMask);
}

INTERCEPTOR_ATTRIBUTE
void OptTable::printHelp(raw_ostream &OS, const char *Usage, const char *Title,
                         unsigned FlagsToInclude, unsigned FlagsToExclude,
                         bool ShowAllAliases) const {
  ReplaceStream Wrapper{OS};
  bool ShowHidden = !(FlagsToExclude & HelpHidden);
  FlagsToExclude &= ~HelpHidden;
  return internalPrintHelp(
      Wrapper, Usage, Title, ShowHidden, ShowAllAliases,
      [FlagsToInclude, FlagsToExclude](const Info &CandidateInfo) {
        if (FlagsToInclude && !(CandidateInfo.Flags & FlagsToInclude))
          return true;
        if (CandidateInfo.Flags & FlagsToExclude)
          return true;
        return false;
      },
      Visibility(0));
}

} // namespace opt

static std::atomic_uint32_t PendingParse;

INTERCEPTOR_ATTRIBUTE __attribute__((no_sanitize("undefined"))) raw_fd_ostream &
outs() {
  static auto RealFunc = getRealFuncAddr(&outs);
  if (PendingParse == 0)
    return RealFunc();
  static ReplaceStream Stream{RealFunc()};
  // NOTE: This is UB.
  return *reinterpret_cast<raw_fd_ostream *>(&Stream);
}

namespace cl {
INTERCEPTOR_ATTRIBUTE
bool ParseCommandLineOptions(int argc, const char *const *argv,
                             StringRef Overview, raw_ostream *Errs,
                             const char *EnvVar,
                             bool LongOptionsUseDoubleDash) {
  static auto RealFunc = getRealFuncAddr(&ParseCommandLineOptions);
  ++PendingParse;
  auto Exit = make_scope_exit([&] { --PendingParse; });
  return RealFunc(argc, argv, ::replace(Overview), Errs, EnvVar,
                  LongOptionsUseDoubleDash);
}
} // namespace cl

} // namespace llvm
