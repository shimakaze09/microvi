#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/Mode.hpp"

namespace core {

struct CommandInvocation;

enum class RegistryResourceKind : std::uint8_t {
  kCommand,
  kKeybinding,
  kTheme,
  kFiletype,
  kPlugin,
  kOption,
};

enum class RegistryOriginKind : std::uint8_t {
  kCore,
  kNative,
  kPlugin,
  kUser,
};

enum class RegistrationLifetime : std::uint8_t {
  kStatic,
  kSession,
};

enum class RegistrationStatus : std::uint8_t {
  kApplied,
  kShadowed,
  kRejected,
};

enum class UndoScope : std::uint8_t {
  kNone,
  kLine,
  kBuffer,
};

enum class CommandParameterKind : std::uint8_t {
  kString,
  kInteger,
  kNumber,
  kBoolean,
  kArray,
  kObject,
};

enum class CommandCapability : std::uint8_t {
  kNone = 0u,
  kReadBuffer = 0x01u,
  kWriteBuffer = 0x02u,
  kFilesystem = 0x04u,
  kNetwork = 0x08u,
  kSpawnProcess = 0x10u,
};

inline constexpr CommandCapability operator|(CommandCapability lhs,
                                             CommandCapability rhs) {
  return static_cast<CommandCapability>(static_cast<std::uint32_t>(lhs) |
                                        static_cast<std::uint32_t>(rhs));
}

inline constexpr CommandCapability operator&(CommandCapability lhs,
                                             CommandCapability rhs) {
  return static_cast<CommandCapability>(static_cast<std::uint32_t>(lhs) &
                                        static_cast<std::uint32_t>(rhs));
}

using CommandCapabilityMask = std::uint32_t;

struct Origin {
  RegistryOriginKind kind = RegistryOriginKind::kCore;
  std::string name;
};

struct CommandParameter {
  std::string name;
  CommandParameterKind kind = CommandParameterKind::kString;
  bool required = false;
  std::string default_value;

  bool operator==(const CommandParameter& other) const {
    return name == other.name && kind == other.kind &&
           required == other.required && default_value == other.default_value;
  }
};

struct CommandDescriptor {
  std::string id;
  std::string label;
  std::string short_description;
  std::string doc_url;
  std::vector<Mode> modes;
  std::vector<CommandParameter> parameters;
  CommandCapabilityMask capabilities = 0;
  UndoScope undo_scope = UndoScope::kNone;

  bool operator==(const CommandDescriptor& other) const {
    return id == other.id && label == other.label &&
           short_description == other.short_description &&
           doc_url == other.doc_url && modes == other.modes &&
           parameters == other.parameters &&
           capabilities == other.capabilities && undo_scope == other.undo_scope;
  }
};

struct CommandInvocation {
  std::string command_id;
  std::unordered_map<std::string, std::string> arguments;
};

struct CommandCallable {
  using NativeCallback = std::function<void(const CommandInvocation&)>;

  NativeCallback native_callback;
  std::string rpc_endpoint;

  bool IsValid() const noexcept {
    return static_cast<bool>(native_callback) || !rpc_endpoint.empty();
  }
};

struct CommandRegistration {
  CommandDescriptor descriptor;
  CommandCallable callable;
  int priority = 0;
  RegistrationLifetime lifetime = RegistrationLifetime::kStatic;
};

struct CommandRecord {
  CommandDescriptor descriptor;
  CommandCallable callable;
  Origin origin;
  int priority = 0;
  RegistrationLifetime lifetime = RegistrationLifetime::kStatic;
  std::uint64_t token = 0;
  std::uint64_t sequence = 0;
  RegistrationStatus status = RegistrationStatus::kApplied;
};

enum class KeybindingMode : std::uint8_t {
  kNormal = static_cast<std::uint8_t>(Mode::kNormal),
  kInsert = static_cast<std::uint8_t>(Mode::kInsert),
  kCommand = static_cast<std::uint8_t>(Mode::kCommandLine),
  kVisual = static_cast<std::uint8_t>(Mode::kVisual),
  kAny,
};

struct KeybindingDescriptor {
  std::string id;
  std::string command_id;
  KeybindingMode mode = KeybindingMode::kAny;
  std::string gesture;
  std::string when_clause;
  std::unordered_map<std::string, std::string> arguments;

  bool operator==(const KeybindingDescriptor& other) const {
    return id == other.id && command_id == other.command_id &&
           mode == other.mode && gesture == other.gesture &&
           when_clause == other.when_clause && arguments == other.arguments;
  }
};

struct KeybindingRegistration {
  KeybindingDescriptor descriptor;
  int priority = 0;
  RegistrationLifetime lifetime = RegistrationLifetime::kStatic;
};

struct KeybindingRecord {
  KeybindingDescriptor descriptor;
  Origin origin;
  int priority = 0;
  RegistrationLifetime lifetime = RegistrationLifetime::kStatic;
  std::uint64_t token = 0;
  std::uint64_t sequence = 0;
  RegistrationStatus status = RegistrationStatus::kApplied;
};

struct RegistrationHandle {
  RegistryResourceKind resource = RegistryResourceKind::kCommand;
  std::string id;
  std::uint64_t token = 0;

  bool IsValid() const noexcept { return token != 0; }
};

struct ConflictRecord {
  RegistryResourceKind resource = RegistryResourceKind::kCommand;
  std::string id;
  Origin winner_origin;
  Origin loser_origin;
  std::string message;
};

struct RegistrationResult {
  RegistrationStatus status = RegistrationStatus::kRejected;
  RegistrationHandle handle;
  std::optional<ConflictRecord> conflict;
};

struct RegistryEvent {
  RegistryResourceKind resource = RegistryResourceKind::kCommand;
  std::string id;
  RegistrationStatus status = RegistrationStatus::kApplied;
};

using RegistrySubscriptionToken = std::uint64_t;
using RegistryCallback = std::function<void(const RegistryEvent&)>;

class Registry {
 public:
  static Registry& Instance();

  RegistrationResult RegisterCommand(const CommandRegistration& registration,
                                     const Origin& origin);
  RegistrationResult RegisterKeybinding(
      const KeybindingRegistration& registration, const Origin& origin);

  std::optional<CommandRecord> FindCommand(std::string_view id,
                                           bool include_shadow = false) const;
  std::vector<CommandRecord> ListCommands() const;

  std::optional<KeybindingRecord> FindKeybinding(
      std::string_view id, bool include_shadow = false) const;
  std::optional<KeybindingRecord> ResolveKeybinding(
      KeybindingMode mode, std::string_view gesture) const;
  std::vector<KeybindingRecord> ListKeybindings() const;

  bool Unregister(const RegistrationHandle& handle);

  std::vector<ConflictRecord> ListConflicts() const;

  std::uint64_t Version() const noexcept;

  RegistrySubscriptionToken Subscribe(RegistryCallback callback);
  bool Unsubscribe(RegistrySubscriptionToken token);

 private:
  Registry();

  struct CommandEntry {
    CommandDescriptor descriptor;
    CommandCallable callable;
    Origin origin;
    int priority = 0;
    RegistrationLifetime lifetime = RegistrationLifetime::kStatic;
    std::uint64_t token = 0;
    std::uint64_t sequence = 0;
  };

  struct KeybindingEntry {
    KeybindingDescriptor descriptor;
    Origin origin;
    int priority = 0;
    RegistrationLifetime lifetime = RegistrationLifetime::kStatic;
    std::uint64_t token = 0;
    std::uint64_t sequence = 0;
    std::string binding_key;
  };

  struct CommandResolution;
  struct KeybindingResolution;

  CommandResolution ResolveCommandConflict(const CommandEntry& existing,
                                           const CommandEntry& incoming) const;
  KeybindingResolution ResolveKeybindingConflict(
      const KeybindingEntry& existing, const KeybindingEntry& incoming) const;

  void Notify(const RegistryEvent& event);

  void PromoteCommandShadow(const std::string& id);
  void PromoteKeybindingShadow(const std::string& binding_key);

  static int PrecedenceRank(const Origin& origin);
  static std::string ComposeBindingKey(KeybindingMode mode,
                                       std::string_view gesture);

  static bool CommandDescriptorsCompatible(const CommandDescriptor& a,
                                           const CommandDescriptor& b);

  mutable std::mutex mutex_;
  std::unordered_map<std::string, CommandEntry> commands_;
  std::unordered_map<std::string, std::vector<CommandEntry>> command_shadow_;
  std::unordered_map<std::string, KeybindingEntry> keybindings_by_id_;
  std::unordered_map<std::string, std::string> keybinding_active_key_to_id_;
  std::unordered_map<std::string, std::vector<KeybindingEntry>>
      keybinding_shadow_;
  std::unordered_map<std::uint64_t, std::string> keybinding_token_to_key_;
  std::vector<ConflictRecord> conflicts_;
  std::unordered_map<RegistrySubscriptionToken, RegistryCallback> subscribers_;
  std::atomic<std::uint64_t> version_{1};
  std::uint64_t next_token_ = 1;
  std::uint64_t next_sequence_ = 1;
  RegistrySubscriptionToken next_subscription_token_ = 1;
};

}  // namespace core
