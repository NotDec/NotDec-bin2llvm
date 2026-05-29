#include "notdec-bin2llvm/NativeAbi.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/MemoryBuffer.h"

#include <cctype>
#include <charconv>
#include <map>
#include <memory>
#include <sstream>
#include <string_view>
#include <type_traits>

namespace notdec::bin2llvm {
namespace {

struct XmlElement {
  std::string Name;
  std::map<std::string, std::string> Attrs;
  std::vector<XmlElement> Children;
};

class SmallXmlParser {
public:
  explicit SmallXmlParser(std::string_view text) : Text(text) {}

  std::optional<XmlElement> parse(std::string &errorMessage) {
    skipSpaceAndMisc();
    if (Pos >= Text.size()) {
      errorMessage = "empty XML document";
      return std::nullopt;
    }
    std::optional<XmlElement> root = parseElement(errorMessage);
    if (!root) {
      return std::nullopt;
    }
    skipSpaceAndMisc();
    if (Pos != Text.size()) {
      errorMessage = "trailing XML content";
      return std::nullopt;
    }
    return root;
  }

private:
  bool startsWith(std::string_view prefix) const {
    return Text.substr(Pos, prefix.size()) == prefix;
  }

  void skipSpace() {
    while (Pos < Text.size() &&
           std::isspace(static_cast<unsigned char>(Text[Pos]))) {
      ++Pos;
    }
  }

  void skipSpaceAndMisc() {
    for (;;) {
      skipSpace();
      if (startsWith("<?")) {
        size_t end = Text.find("?>", Pos + 2);
        Pos = end == std::string_view::npos ? Text.size() : end + 2;
        continue;
      }
      if (startsWith("<!--")) {
        size_t end = Text.find("-->", Pos + 4);
        Pos = end == std::string_view::npos ? Text.size() : end + 3;
        continue;
      }
      break;
    }
  }

  std::string parseName() {
    size_t begin = Pos;
    while (Pos < Text.size()) {
      char ch = Text[Pos];
      if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' ||
          ch == '-' || ch == ':' || ch == '.') {
        ++Pos;
        continue;
      }
      break;
    }
    return std::string(Text.substr(begin, Pos - begin));
  }

  std::optional<std::string> parseQuotedValue(std::string &errorMessage) {
    if (Pos >= Text.size() || (Text[Pos] != '"' && Text[Pos] != '\'')) {
      errorMessage = "expected XML attribute quote";
      return std::nullopt;
    }
    char quote = Text[Pos++];
    size_t begin = Pos;
    while (Pos < Text.size() && Text[Pos] != quote) {
      ++Pos;
    }
    if (Pos >= Text.size()) {
      errorMessage = "unterminated XML attribute";
      return std::nullopt;
    }
    std::string value(Text.substr(begin, Pos - begin));
    ++Pos;
    return value;
  }

  std::optional<XmlElement> parseElement(std::string &errorMessage) {
    if (Pos >= Text.size() || Text[Pos] != '<') {
      errorMessage = "expected XML element";
      return std::nullopt;
    }
    ++Pos;
    if (Pos < Text.size() && Text[Pos] == '/') {
      errorMessage = "unexpected closing XML element";
      return std::nullopt;
    }

    XmlElement elem;
    elem.Name = parseName();
    if (elem.Name.empty()) {
      errorMessage = "expected XML element name";
      return std::nullopt;
    }

    for (;;) {
      skipSpace();
      if (Pos >= Text.size()) {
        errorMessage = "unterminated XML element";
        return std::nullopt;
      }
      if (startsWith("/>")) {
        Pos += 2;
        return elem;
      }
      if (Text[Pos] == '>') {
        ++Pos;
        break;
      }

      std::string key = parseName();
      if (key.empty()) {
        errorMessage = "expected XML attribute name";
        return std::nullopt;
      }
      skipSpace();
      if (Pos >= Text.size() || Text[Pos] != '=') {
        errorMessage = "expected XML attribute '='";
        return std::nullopt;
      }
      ++Pos;
      skipSpace();
      std::optional<std::string> value = parseQuotedValue(errorMessage);
      if (!value) {
        return std::nullopt;
      }
      elem.Attrs.emplace(std::move(key), std::move(*value));
    }

    for (;;) {
      skipSpaceAndMisc();
      if (Pos >= Text.size()) {
        errorMessage = "missing XML closing element";
        return std::nullopt;
      }
      if (startsWith("</")) {
        Pos += 2;
        std::string closeName = parseName();
        skipSpace();
        if (Pos >= Text.size() || Text[Pos] != '>') {
          errorMessage = "unterminated XML closing element";
          return std::nullopt;
        }
        ++Pos;
        if (closeName != elem.Name) {
          errorMessage = "mismatched XML closing element";
          return std::nullopt;
        }
        return elem;
      }
      if (startsWith("<![CDATA[")) {
        size_t end = Text.find("]]>", Pos + 9);
        if (end == std::string_view::npos) {
          errorMessage = "unterminated XML CDATA";
          return std::nullopt;
        }
        Pos = end + 3;
        continue;
      }
      if (Text[Pos] == '<') {
        std::optional<XmlElement> child = parseElement(errorMessage);
        if (!child) {
          return std::nullopt;
        }
        elem.Children.push_back(std::move(*child));
        continue;
      }
      while (Pos < Text.size() && Text[Pos] != '<') {
        ++Pos;
      }
    }
  }

  std::string_view Text;
  size_t Pos = 0;
};

const XmlElement *firstChild(const XmlElement &elem, llvm::StringRef name) {
  for (const XmlElement &child : elem.Children) {
    if (child.Name == name) {
      return &child;
    }
  }
  return nullptr;
}

std::string attr(const XmlElement &elem, llvm::StringRef name) {
  auto it = elem.Attrs.find(name.str());
  if (it == elem.Attrs.end()) {
    return "";
  }
  return it->second;
}

template <typename IntT>
IntT parseIntegerOr(std::string_view text, IntT fallback) {
  if (text.empty()) {
    return fallback;
  }
  int base = 10;
  bool negative = false;
  if constexpr (std::is_signed_v<IntT>) {
    if (!text.empty() && text.front() == '-') {
      negative = true;
      text.remove_prefix(1);
    }
  }
  if (text.size() > 2 && text[0] == '0' &&
      (text[1] == 'x' || text[1] == 'X')) {
    base = 16;
    text.remove_prefix(2);
  }
  IntT value = 0;
  const char *begin = text.data();
  const char *end = text.data() + text.size();
  std::from_chars_result result = std::from_chars(begin, end, value, base);
  if (result.ec != std::errc() || result.ptr != end) {
    return fallback;
  }
  if constexpr (std::is_signed_v<IntT>) {
    if (negative) {
      value = -value;
    }
  }
  return value;
}

std::optional<NativeAbiStorage> storageFromElement(const XmlElement &elem) {
  if (elem.Name == "register") {
    NativeAbiStorage storage;
    storage.Kind = NativeAbiStorageKind::Register;
    storage.Name = attr(elem, "name");
    if (storage.Name.empty()) {
      return std::nullopt;
    }
    return storage;
  }
  if (elem.Name == "addr") {
    NativeAbiStorage storage;
    storage.Kind = NativeAbiStorageKind::Stack;
    storage.Space = attr(elem, "space");
    storage.Offset = parseIntegerOr<uint64_t>(attr(elem, "offset"), 0);
    if (storage.Space.empty()) {
      return std::nullopt;
    }
    return storage;
  }
  return std::nullopt;
}

std::optional<NativeAbiParamEntry> paramEntryFromElement(
    const XmlElement &elem) {
  if (elem.Name != "pentry") {
    return std::nullopt;
  }
  for (const XmlElement &child : elem.Children) {
    std::optional<NativeAbiStorage> storage = storageFromElement(child);
    if (!storage) {
      continue;
    }

    NativeAbiParamEntry entry;
    entry.MinSize = parseIntegerOr<uint32_t>(attr(elem, "minsize"), 0);
    entry.MaxSize = parseIntegerOr<uint32_t>(attr(elem, "maxsize"), 0);
    entry.Align = parseIntegerOr<uint32_t>(attr(elem, "align"), 0);
    entry.MetaType = attr(elem, "metatype");
    entry.Storage = std::move(*storage);
    return entry;
  }
  return std::nullopt;
}

void collectParamEntries(const XmlElement &elem,
                         std::vector<NativeAbiParamEntry> &entries) {
  for (const XmlElement &child : elem.Children) {
    if (std::optional<NativeAbiParamEntry> entry =
            paramEntryFromElement(child)) {
      entries.push_back(std::move(*entry));
      continue;
    }
    if (child.Name == "group") {
      collectParamEntries(child, entries);
    }
  }
}

void collectEffects(const XmlElement &elem, NativeAbiEffectKind kind,
                    std::vector<NativeAbiEffect> &effects) {
  for (const XmlElement &child : elem.Children) {
    std::optional<NativeAbiStorage> storage = storageFromElement(child);
    if (!storage) {
      continue;
    }
    NativeAbiEffect effect;
    effect.Kind = kind;
    effect.Storage = std::move(*storage);
    effects.push_back(std::move(effect));
  }
}

std::string storageKindName(NativeAbiStorageKind kind) {
  switch (kind) {
  case NativeAbiStorageKind::Register:
    return "register";
  case NativeAbiStorageKind::Stack:
    return "stack";
  }
  return "unknown";
}

std::string effectKindName(NativeAbiEffectKind kind) {
  switch (kind) {
  case NativeAbiEffectKind::Unaffected:
    return "unaffected";
  case NativeAbiEffectKind::KilledByCall:
    return "killedbycall";
  }
  return "unknown";
}

llvm::MDString *field(llvm::LLVMContext &context, const std::string &key,
                      const std::string &value) {
  return llvm::MDString::get(context, key + "=" + value);
}

llvm::MDString *field(llvm::LLVMContext &context, const std::string &key,
                      uint64_t value) {
  return field(context, key, std::to_string(value));
}

llvm::MDNode *storageNode(llvm::LLVMContext &context,
                          const NativeAbiStorage &storage) {
  std::vector<llvm::Metadata *> fields = {
      field(context, "kind", storageKindName(storage.Kind)),
      field(context, "name", storage.Name),
      field(context, "space", storage.Space),
      field(context, "offset", storage.Offset),
  };
  return llvm::MDNode::get(context, fields);
}

llvm::MDNode *paramEntryNode(llvm::LLVMContext &context,
                             const NativeAbiParamEntry &entry) {
  std::vector<llvm::Metadata *> fields = {
      field(context, "minsize", entry.MinSize),
      field(context, "maxsize", entry.MaxSize),
      field(context, "align", entry.Align),
      field(context, "metatype", entry.MetaType),
      storageNode(context, entry.Storage),
  };
  return llvm::MDNode::get(context, fields);
}

llvm::MDNode *effectNode(llvm::LLVMContext &context,
                         const NativeAbiEffect &effect) {
  std::vector<llvm::Metadata *> fields = {
      field(context, "effect", effectKindName(effect.Kind)),
      storageNode(context, effect.Storage),
  };
  return llvm::MDNode::get(context, fields);
}

} // namespace

std::optional<NativeAbiSpec> parseGhidraCspecDefaultAbi(
    llvm::StringRef cspecPath, std::string &errorMessage) {
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> buffer =
      llvm::MemoryBuffer::getFile(cspecPath);
  if (!buffer) {
    errorMessage = "failed to read cspec: " + buffer.getError().message();
    return std::nullopt;
  }

  llvm::StringRef text = (*buffer)->getBuffer();
  SmallXmlParser parser(std::string_view(text.data(), text.size()));
  std::optional<XmlElement> root = parser.parse(errorMessage);
  if (!root) {
    return std::nullopt;
  }
  if (root->Name != "compiler_spec") {
    errorMessage = "cspec root is not <compiler_spec>";
    return std::nullopt;
  }

  const XmlElement *defaultProto = firstChild(*root, "default_proto");
  const XmlElement *prototype =
      defaultProto == nullptr ? nullptr : firstChild(*defaultProto, "prototype");
  if (prototype == nullptr) {
    errorMessage = "cspec has no default prototype";
    return std::nullopt;
  }

  NativeAbiSpec abi;
  abi.PrototypeName = attr(*prototype, "name");
  abi.ExtraPop = parseIntegerOr<int64_t>(attr(*prototype, "extrapop"), 0);
  abi.StackShift = parseIntegerOr<uint64_t>(attr(*prototype, "stackshift"), 0);

  if (const XmlElement *stackPointer = firstChild(*root, "stackpointer")) {
    abi.StackPointerRegister = attr(*stackPointer, "register");
    abi.StackPointerSpace = attr(*stackPointer, "space");
  }
  if (const XmlElement *input = firstChild(*prototype, "input")) {
    collectParamEntries(*input, abi.Inputs);
  }
  if (const XmlElement *output = firstChild(*prototype, "output")) {
    collectParamEntries(*output, abi.Outputs);
  }
  if (const XmlElement *unaffected = firstChild(*prototype, "unaffected")) {
    collectEffects(*unaffected, NativeAbiEffectKind::Unaffected, abi.Effects);
  }
  if (const XmlElement *killed = firstChild(*prototype, "killedbycall")) {
    collectEffects(*killed, NativeAbiEffectKind::KilledByCall, abi.Effects);
  }

  if (abi.PrototypeName.empty() || abi.Inputs.empty() || abi.Outputs.empty()) {
    errorMessage = "default prototype is missing ABI entries";
    return std::nullopt;
  }
  return abi;
}

void attachNativeAbiMetadata(llvm::Module &module, const NativeAbiSpec &abi) {
  llvm::LLVMContext &context = module.getContext();

  std::vector<llvm::Metadata *> inputEntries;
  for (const NativeAbiParamEntry &entry : abi.Inputs) {
    inputEntries.push_back(paramEntryNode(context, entry));
  }
  std::vector<llvm::Metadata *> outputEntries;
  for (const NativeAbiParamEntry &entry : abi.Outputs) {
    outputEntries.push_back(paramEntryNode(context, entry));
  }
  std::vector<llvm::Metadata *> effectEntries;
  for (const NativeAbiEffect &effect : abi.Effects) {
    effectEntries.push_back(effectNode(context, effect));
  }

  std::vector<llvm::Metadata *> fields = {
      field(context, "prototype", abi.PrototypeName),
      field(context, "stackpointer.register", abi.StackPointerRegister),
      field(context, "stackpointer.space", abi.StackPointerSpace),
      field(context, "extrapop", static_cast<uint64_t>(abi.ExtraPop)),
      field(context, "stackshift", abi.StackShift),
      llvm::MDNode::get(context, inputEntries),
      llvm::MDNode::get(context, outputEntries),
      llvm::MDNode::get(context, effectEntries),
  };

  module.getOrInsertNamedMetadata("notdec.abi")->addOperand(
      llvm::MDNode::get(context, fields));
}

} // namespace notdec::bin2llvm
