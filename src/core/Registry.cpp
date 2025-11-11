#include "core/Registry.hpp"

#include <algorithm>
#include <cassert>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace core {

namespace {
constexpr int kPrecedenceCore = 0;
constexpr int kPrecedenceNative = 1;
constexpr int kPrecedencePlugin = 2;
constexpr int kPrecedenceUser = 3;
}  // namespace

struct Registry::CommandResolution {
  enum class Decision : std::uint8_t {
    kReplaceExisting,
    kShadowIncoming,
    kRejectIncoming,
  };

  Decision decision = Decision::kShadowIncoming;
  std::optional<ConflictRecord> conflict;
};

struct Registry::KeybindingResolution {
  enum class Decision : std::uint8_t {
    kReplaceExisting,
    kShadowIncoming,
    kRejectIncoming,
  };

  Decision decision = Decision::kShadowIncoming;
  std::optional<ConflictRecord> conflict;
};

Registry& Registry::Instance() {
  static Registry instance;
  return instance;
}

Registry::Registry() = default;

RegistrationResult Registry::RegisterCommand(
    const CommandRegistration& registration, const Origin& origin) {
  RegistrationResult result;
  if (registration.descriptor.id.empty()) {
    ConflictRecord conflict;
    conflict.resource = RegistryResourceKind::kCommand;
    conflict.id = "";
    conflict.winner_origin = origin;
    conflict.loser_origin = origin;
    conflict.message = "Command id must not be empty";
    result.status = RegistrationStatus::kRejected;
    result.conflict = conflict;
    {
      std::scoped_lock lock(mutex_);
      conflicts_.push_back(conflict);
    }
    return result;
  }

  if (!registration.callable.IsValid()) {
    ConflictRecord conflict;
    conflict.resource = RegistryResourceKind::kCommand;
    conflict.id = registration.descriptor.id;
    conflict.winner_origin = origin;
    conflict.loser_origin = origin;
    conflict.message =
        "Command callable must provide native callback or RPC endpoint";
    result.status = RegistrationStatus::kRejected;
    result.conflict = conflict;
    {
      std::scoped_lock lock(mutex_);
      conflicts_.push_back(conflict);
    }
    return result;
  }

  CommandEntry incoming;
  incoming.descriptor = registration.descriptor;
  incoming.callable = registration.callable;
  incoming.origin = origin;
  incoming.priority = registration.priority;
  incoming.lifetime = registration.lifetime;

  std::vector<RegistryEvent> events;

  {
    std::scoped_lock lock(mutex_);
    incoming.token = next_token_++;
    incoming.sequence = next_sequence_++;

    auto it = commands_.find(incoming.descriptor.id);
    if (it == commands_.end()) {
      commands_.emplace(incoming.descriptor.id, incoming);
      version_.fetch_add(1, std::memory_order_relaxed);
      result.status = RegistrationStatus::kApplied;
      result.handle = {RegistryResourceKind::kCommand, incoming.descriptor.id,
                       incoming.token};
      events.push_back({RegistryResourceKind::kCommand, incoming.descriptor.id,
                        RegistrationStatus::kApplied});
    } else {
      CommandEntry existing_entry = it->second;
      CommandResolution resolution =
          ResolveCommandConflict(existing_entry, incoming);

      if (resolution.conflict.has_value()) {
        conflicts_.push_back(*resolution.conflict);
        result.conflict = resolution.conflict;
      }

      switch (resolution.decision) {
        case CommandResolution::Decision::kReplaceExisting: {
          command_shadow_[incoming.descriptor.id].push_back(existing_entry);
          it->second = incoming;
          version_.fetch_add(1, std::memory_order_relaxed);
          result.status = RegistrationStatus::kApplied;
          result.handle = {RegistryResourceKind::kCommand,
                           incoming.descriptor.id, incoming.token};
          events.push_back({RegistryResourceKind::kCommand,
                            existing_entry.descriptor.id,
                            RegistrationStatus::kShadowed});
          events.push_back({RegistryResourceKind::kCommand,
                            incoming.descriptor.id,
                            RegistrationStatus::kApplied});
          break;
        }
        case CommandResolution::Decision::kShadowIncoming: {
          command_shadow_[incoming.descriptor.id].push_back(incoming);
          version_.fetch_add(1, std::memory_order_relaxed);
          result.status = RegistrationStatus::kShadowed;
          result.handle = {RegistryResourceKind::kCommand,
                           incoming.descriptor.id, incoming.token};
          events.push_back({RegistryResourceKind::kCommand,
                            incoming.descriptor.id,
                            RegistrationStatus::kShadowed});
          break;
        }
        case CommandResolution::Decision::kRejectIncoming: {
          result.status = RegistrationStatus::kRejected;
          break;
        }
      }
    }
  }

  for (const auto& event : events) {
    Notify(event);
  }

  return result;
}

RegistrationResult Registry::RegisterKeybinding(
    const KeybindingRegistration& registration, const Origin& origin) {
  RegistrationResult result;
  const KeybindingDescriptor& descriptor = registration.descriptor;
  if (descriptor.id.empty()) {
    ConflictRecord conflict;
    conflict.resource = RegistryResourceKind::kKeybinding;
    conflict.id = "";
    conflict.winner_origin = origin;
    conflict.loser_origin = origin;
    conflict.message = "Keybinding id must not be empty";
    result.status = RegistrationStatus::kRejected;
    result.conflict = conflict;
    {
      std::scoped_lock lock(mutex_);
      conflicts_.push_back(conflict);
    }
    return result;
  }

  if (descriptor.gesture.empty()) {
    ConflictRecord conflict;
    conflict.resource = RegistryResourceKind::kKeybinding;
    conflict.id = descriptor.id;
    conflict.winner_origin = origin;
    conflict.loser_origin = origin;
    conflict.message = "Keybinding gesture must not be empty";
    result.status = RegistrationStatus::kRejected;
    result.conflict = conflict;
    {
      std::scoped_lock lock(mutex_);
      conflicts_.push_back(conflict);
    }
    return result;
  }

  KeybindingEntry incoming;
  incoming.descriptor = descriptor;
  incoming.origin = origin;
  incoming.priority = registration.priority;
  incoming.lifetime = registration.lifetime;
  incoming.binding_key = ComposeBindingKey(descriptor.mode, descriptor.gesture);

  std::vector<RegistryEvent> events;

  {
    std::scoped_lock lock(mutex_);

    if (keybindings_by_id_.find(descriptor.id) != keybindings_by_id_.end()) {
      ConflictRecord conflict;
      conflict.resource = RegistryResourceKind::kKeybinding;
      conflict.id = descriptor.id;
      conflict.winner_origin = keybindings_by_id_.at(descriptor.id).origin;
      conflict.loser_origin = origin;
      conflict.message = "Keybinding id already registered";
      result.status = RegistrationStatus::kRejected;
      result.conflict = conflict;
      conflicts_.push_back(conflict);
      return result;
    }

    incoming.token = next_token_++;
    incoming.sequence = next_sequence_++;
    keybinding_token_to_key_[incoming.token] = incoming.binding_key;

    auto occupant = keybinding_active_key_to_id_.find(incoming.binding_key);
    if (occupant == keybinding_active_key_to_id_.end()) {
      keybindings_by_id_.emplace(incoming.descriptor.id, incoming);
      keybinding_active_key_to_id_[incoming.binding_key] =
          incoming.descriptor.id;
      version_.fetch_add(1, std::memory_order_relaxed);
      result.status = RegistrationStatus::kApplied;
      result.handle = {RegistryResourceKind::kKeybinding,
                       incoming.descriptor.id, incoming.token};
      events.push_back({RegistryResourceKind::kKeybinding,
                        incoming.descriptor.id, RegistrationStatus::kApplied});
    } else {
      const std::string& occupant_id = occupant->second;
      auto existing_it = keybindings_by_id_.find(occupant_id);
      assert(existing_it != keybindings_by_id_.end());
      KeybindingEntry existing_entry = existing_it->second;

      KeybindingResolution resolution =
          ResolveKeybindingConflict(existing_entry, incoming);

      if (resolution.conflict.has_value()) {
        conflicts_.push_back(*resolution.conflict);
        result.conflict = resolution.conflict;
      }

      switch (resolution.decision) {
        case KeybindingResolution::Decision::kReplaceExisting: {
          keybinding_shadow_[incoming.binding_key].push_back(existing_entry);
          keybindings_by_id_.erase(existing_it);
          keybindings_by_id_.emplace(incoming.descriptor.id, incoming);
          keybinding_active_key_to_id_[incoming.binding_key] =
              incoming.descriptor.id;
          version_.fetch_add(1, std::memory_order_relaxed);
          result.status = RegistrationStatus::kApplied;
          result.handle = {RegistryResourceKind::kKeybinding,
                           incoming.descriptor.id, incoming.token};
          events.push_back({RegistryResourceKind::kKeybinding,
                            existing_entry.descriptor.id,
                            RegistrationStatus::kShadowed});
          events.push_back({RegistryResourceKind::kKeybinding,
                            incoming.descriptor.id,
                            RegistrationStatus::kApplied});
          break;
        }
        case KeybindingResolution::Decision::kShadowIncoming: {
          keybinding_shadow_[incoming.binding_key].push_back(incoming);
          version_.fetch_add(1, std::memory_order_relaxed);
          result.status = RegistrationStatus::kShadowed;
          result.handle = {RegistryResourceKind::kKeybinding,
                           incoming.descriptor.id, incoming.token};
          events.push_back({RegistryResourceKind::kKeybinding,
                            incoming.descriptor.id,
                            RegistrationStatus::kShadowed});
          break;
        }
        case KeybindingResolution::Decision::kRejectIncoming: {
          keybinding_token_to_key_.erase(incoming.token);
          result.status = RegistrationStatus::kRejected;
          break;
        }
      }
    }
  }

  for (const auto& event : events) {
    Notify(event);
  }

  return result;
}

bool Registry::Unregister(const RegistrationHandle& handle) {
  if (!handle.IsValid()) {
    return false;
  }

  std::vector<RegistryEvent> events;
  bool success = false;

  {
    std::scoped_lock lock(mutex_);

    switch (handle.resource) {
      case RegistryResourceKind::kCommand: {
        auto it = commands_.find(handle.id);
        if (it != commands_.end() && it->second.token == handle.token) {
          commands_.erase(it);
          PromoteCommandShadow(handle.id);
          version_.fetch_add(1, std::memory_order_relaxed);
          events.push_back({RegistryResourceKind::kCommand, handle.id,
                            RegistrationStatus::kRejected});
          success = true;
          break;
        }

        auto shadow_it = command_shadow_.find(handle.id);
        if (shadow_it != command_shadow_.end()) {
          auto& list = shadow_it->second;
          auto erase_it =
              std::remove_if(list.begin(), list.end(),
                             [token = handle.token](const CommandEntry& entry) {
                               return entry.token == token;
                             });
          if (erase_it != list.end()) {
            list.erase(erase_it, list.end());
            if (list.empty()) {
              command_shadow_.erase(shadow_it);
            }
            version_.fetch_add(1, std::memory_order_relaxed);
            success = true;
          }
        }
        break;
      }
      case RegistryResourceKind::kKeybinding: {
        auto active_it = keybindings_by_id_.find(handle.id);
        if (active_it != keybindings_by_id_.end() &&
            active_it->second.token == handle.token) {
          std::string binding_key = active_it->second.binding_key;
          keybindings_by_id_.erase(active_it);
          keybinding_active_key_to_id_.erase(binding_key);
          keybinding_token_to_key_.erase(handle.token);
          PromoteKeybindingShadow(binding_key);
          version_.fetch_add(1, std::memory_order_relaxed);
          events.push_back({RegistryResourceKind::kKeybinding, handle.id,
                            RegistrationStatus::kRejected});
          success = true;
          break;
        }

        auto token_it = keybinding_token_to_key_.find(handle.token);
        if (token_it != keybinding_token_to_key_.end()) {
          auto shadow_it = keybinding_shadow_.find(token_it->second);
          if (shadow_it != keybinding_shadow_.end()) {
            auto& list = shadow_it->second;
            auto erase_it = std::remove_if(
                list.begin(), list.end(),
                [token = handle.token](const KeybindingEntry& entry) {
                  return entry.token == token;
                });
            if (erase_it != list.end()) {
              list.erase(erase_it, list.end());
              if (list.empty()) {
                keybinding_shadow_.erase(shadow_it);
              }
              keybinding_token_to_key_.erase(token_it);
              version_.fetch_add(1, std::memory_order_relaxed);
              success = true;
            }
          }
        }
        break;
      }
      default:
        break;
    }
  }

  for (const auto& event : events) {
    Notify(event);
  }

  return success;
}

std::vector<ConflictRecord> Registry::ListConflicts() const {
  std::scoped_lock lock(mutex_);
  return conflicts_;
}

std::uint64_t Registry::Version() const noexcept {
  return version_.load(std::memory_order_relaxed);
}

std::optional<CommandRecord> Registry::FindCommand(std::string_view id,
                                                   bool include_shadow) const {
  std::string key(id);
  std::scoped_lock lock(mutex_);

  auto it = commands_.find(key);
  if (it != commands_.end()) {
    const CommandEntry& entry = it->second;
    return CommandRecord{entry.descriptor, entry.callable,
                         entry.origin,     entry.priority,
                         entry.lifetime,   entry.token,
                         entry.sequence,   RegistrationStatus::kApplied};
  }

  if (!include_shadow) {
    return std::nullopt;
  }

  auto shadow_it = command_shadow_.find(key);
  if (shadow_it == command_shadow_.end() || shadow_it->second.empty()) {
    return std::nullopt;
  }

  const CommandEntry& shadow_entry = shadow_it->second.back();
  return CommandRecord{shadow_entry.descriptor, shadow_entry.callable,
                       shadow_entry.origin,     shadow_entry.priority,
                       shadow_entry.lifetime,   shadow_entry.token,
                       shadow_entry.sequence,   RegistrationStatus::kShadowed};
}

std::vector<CommandRecord> Registry::ListCommands() const {
  std::vector<CommandRecord> records;
  std::scoped_lock lock(mutex_);
  records.reserve(commands_.size());
  for (const auto& pair : commands_) {
    const CommandEntry& entry = pair.second;
    records.push_back(CommandRecord{entry.descriptor, entry.callable,
                                    entry.origin, entry.priority,
                                    entry.lifetime, entry.token, entry.sequence,
                                    RegistrationStatus::kApplied});
  }
  return records;
}

std::optional<KeybindingRecord> Registry::FindKeybinding(
    std::string_view id, bool include_shadow) const {
  std::string key(id);
  std::scoped_lock lock(mutex_);

  auto it = keybindings_by_id_.find(key);
  if (it != keybindings_by_id_.end()) {
    const KeybindingEntry& entry = it->second;
    return KeybindingRecord{entry.descriptor,
                            entry.origin,
                            entry.priority,
                            entry.lifetime,
                            entry.token,
                            entry.sequence,
                            RegistrationStatus::kApplied};
  }

  if (!include_shadow) {
    return std::nullopt;
  }

  for (const auto& pair : keybinding_shadow_) {
    const auto& entries = pair.second;
    for (const KeybindingEntry& entry : entries) {
      if (entry.descriptor.id == key) {
        return KeybindingRecord{entry.descriptor,
                                entry.origin,
                                entry.priority,
                                entry.lifetime,
                                entry.token,
                                entry.sequence,
                                RegistrationStatus::kShadowed};
      }
    }
  }

  return std::nullopt;
}

std::optional<KeybindingRecord> Registry::ResolveKeybinding(
    KeybindingMode mode, std::string_view gesture) const {
  std::string binding_key = ComposeBindingKey(mode, gesture);
  std::scoped_lock lock(mutex_);

  auto active_it = keybinding_active_key_to_id_.find(binding_key);
  if (active_it == keybinding_active_key_to_id_.end()) {
    return std::nullopt;
  }

  auto record_it = keybindings_by_id_.find(active_it->second);
  if (record_it == keybindings_by_id_.end()) {
    return std::nullopt;
  }

  const KeybindingEntry& entry = record_it->second;
  return KeybindingRecord{entry.descriptor,
                          entry.origin,
                          entry.priority,
                          entry.lifetime,
                          entry.token,
                          entry.sequence,
                          RegistrationStatus::kApplied};
}

std::vector<KeybindingRecord> Registry::ListKeybindings() const {
  std::vector<KeybindingRecord> records;
  std::scoped_lock lock(mutex_);
  records.reserve(keybindings_by_id_.size());
  for (const auto& pair : keybindings_by_id_) {
    const KeybindingEntry& entry = pair.second;
    records.push_back(KeybindingRecord{
        entry.descriptor, entry.origin, entry.priority, entry.lifetime,
        entry.token, entry.sequence, RegistrationStatus::kApplied});
  }
  return records;
}

RegistrySubscriptionToken Registry::Subscribe(RegistryCallback callback) {
  if (!callback) {
    return 0;
  }

  std::scoped_lock lock(mutex_);
  RegistrySubscriptionToken subscription_token = next_subscription_token_++;
  subscribers_.emplace(subscription_token, std::move(callback));
  return subscription_token;
}

bool Registry::Unsubscribe(RegistrySubscriptionToken token) {
  if (token == 0) {
    return false;
  }

  std::scoped_lock lock(mutex_);
  return subscribers_.erase(token) > 0;
}

void Registry::Notify(const RegistryEvent& event) {
  std::vector<RegistryCallback> callbacks;
  {
    std::scoped_lock lock(mutex_);
    callbacks.reserve(subscribers_.size());
    for (auto& entry : subscribers_) {
      callbacks.push_back(entry.second);
    }
  }

  for (auto& callback : callbacks) {
    if (callback) {
      callback(event);
    }
  }
}

void Registry::PromoteCommandShadow(const std::string& id) {
  auto shadow_it = command_shadow_.find(id);
  if (shadow_it == command_shadow_.end() || shadow_it->second.empty()) {
    command_shadow_.erase(id);
    return;
  }

  auto& list = shadow_it->second;
  auto prefer = [this](const CommandEntry& lhs, const CommandEntry& rhs) {
    int lhs_rank = PrecedenceRank(lhs.origin);
    int rhs_rank = PrecedenceRank(rhs.origin);
    if (lhs_rank != rhs_rank) {
      return lhs_rank > rhs_rank;
    }
    if (lhs.priority != rhs.priority) {
      return lhs.priority > rhs.priority;
    }
    return lhs.sequence < rhs.sequence;
  };

  auto best_it = std::max_element(
      list.begin(), list.end(),
      [&prefer](const CommandEntry& lhs, const CommandEntry& rhs) {
        return !prefer(lhs, rhs);
      });
  if (best_it == list.end()) {
    command_shadow_.erase(shadow_it);
    return;
  }

  CommandEntry promoted = std::move(*best_it);
  list.erase(best_it);
  commands_[id] = promoted;
  version_.fetch_add(1, std::memory_order_relaxed);

  if (list.empty()) {
    command_shadow_.erase(shadow_it);
  }
}

void Registry::PromoteKeybindingShadow(const std::string& binding_key) {
  auto shadow_it = keybinding_shadow_.find(binding_key);
  if (shadow_it == keybinding_shadow_.end() || shadow_it->second.empty()) {
    keybinding_shadow_.erase(binding_key);
    return;
  }

  auto& list = shadow_it->second;
  auto prefer = [this](const KeybindingEntry& lhs, const KeybindingEntry& rhs) {
    int lhs_rank = PrecedenceRank(lhs.origin);
    int rhs_rank = PrecedenceRank(rhs.origin);
    if (lhs_rank != rhs_rank) {
      return lhs_rank > rhs_rank;
    }
    if (lhs.priority != rhs.priority) {
      return lhs.priority > rhs.priority;
    }
    return lhs.sequence < rhs.sequence;
  };

  auto best_it = std::max_element(
      list.begin(), list.end(),
      [&prefer](const KeybindingEntry& lhs, const KeybindingEntry& rhs) {
        return !prefer(lhs, rhs);
      });
  if (best_it == list.end()) {
    keybinding_shadow_.erase(shadow_it);
    return;
  }

  KeybindingEntry promoted = std::move(*best_it);
  list.erase(best_it);
  keybindings_by_id_[promoted.descriptor.id] = promoted;
  keybinding_active_key_to_id_[binding_key] = promoted.descriptor.id;
  keybinding_token_to_key_[promoted.token] = binding_key;
  version_.fetch_add(1, std::memory_order_relaxed);

  if (list.empty()) {
    keybinding_shadow_.erase(shadow_it);
  }
}

int Registry::PrecedenceRank(const Origin& origin) {
  switch (origin.kind) {
    case RegistryOriginKind::kCore:
      return kPrecedenceCore;
    case RegistryOriginKind::kNative:
      return kPrecedenceNative;
    case RegistryOriginKind::kPlugin:
      return kPrecedencePlugin;
    case RegistryOriginKind::kUser:
      return kPrecedenceUser;
  }
  return kPrecedenceCore;
}

std::string Registry::ComposeBindingKey(KeybindingMode mode,
                                        std::string_view gesture) {
  return std::to_string(static_cast<int>(mode)) + ':' + std::string(gesture);
}

bool Registry::CommandDescriptorsCompatible(const CommandDescriptor& a,
                                            const CommandDescriptor& b) {
  return a.modes == b.modes && a.parameters == b.parameters &&
         a.undo_scope == b.undo_scope;
}

Registry::CommandResolution Registry::ResolveCommandConflict(
    const CommandEntry& existing, const CommandEntry& incoming) const {
  CommandResolution resolution;

  int existing_rank = PrecedenceRank(existing.origin);
  int incoming_rank = PrecedenceRank(incoming.origin);

  if (incoming_rank > existing_rank) {
    resolution.decision = CommandResolution::Decision::kReplaceExisting;
  } else if (incoming_rank < existing_rank) {
    resolution.decision = CommandResolution::Decision::kShadowIncoming;
  } else {
    if (incoming.priority > existing.priority) {
      resolution.decision = CommandResolution::Decision::kReplaceExisting;
    } else if (incoming.priority < existing.priority) {
      resolution.decision = CommandResolution::Decision::kShadowIncoming;
    } else {
      if (CommandDescriptorsCompatible(existing.descriptor,
                                       incoming.descriptor)) {
        resolution.decision = CommandResolution::Decision::kShadowIncoming;
        ConflictRecord conflict;
        conflict.resource = RegistryResourceKind::kCommand;
        conflict.id = incoming.descriptor.id;
        conflict.winner_origin = existing.origin;
        conflict.loser_origin = incoming.origin;
        conflict.message =
            "Duplicate command ignored (same precedence and priority)";
        resolution.conflict = conflict;
        return resolution;
      }

      resolution.decision = CommandResolution::Decision::kRejectIncoming;
      ConflictRecord conflict;
      conflict.resource = RegistryResourceKind::kCommand;
      conflict.id = incoming.descriptor.id;
      conflict.winner_origin = existing.origin;
      conflict.loser_origin = incoming.origin;
      conflict.message =
          "Command signature conflict with identical precedence and priority";
      resolution.conflict = conflict;
      return resolution;
    }
  }

  ConflictRecord conflict;
  conflict.resource = RegistryResourceKind::kCommand;
  conflict.id = incoming.descriptor.id;
  conflict.winner_origin =
      resolution.decision == CommandResolution::Decision::kReplaceExisting
          ? incoming.origin
          : existing.origin;
  conflict.loser_origin =
      resolution.decision == CommandResolution::Decision::kReplaceExisting
          ? existing.origin
          : incoming.origin;
  conflict.message =
      resolution.decision == CommandResolution::Decision::kReplaceExisting
          ? "Replaced command due to higher precedence or priority"
          : "Command shadowed by higher precedence or priority";
  resolution.conflict = conflict;
  return resolution;
}

Registry::KeybindingResolution Registry::ResolveKeybindingConflict(
    const KeybindingEntry& existing, const KeybindingEntry& incoming) const {
  KeybindingResolution resolution;

  int existing_rank = PrecedenceRank(existing.origin);
  int incoming_rank = PrecedenceRank(incoming.origin);

  if (incoming_rank > existing_rank) {
    resolution.decision = KeybindingResolution::Decision::kReplaceExisting;
  } else if (incoming_rank < existing_rank) {
    resolution.decision = KeybindingResolution::Decision::kShadowIncoming;
  } else {
    if (incoming.priority > existing.priority) {
      resolution.decision = KeybindingResolution::Decision::kReplaceExisting;
    } else if (incoming.priority < existing.priority) {
      resolution.decision = KeybindingResolution::Decision::kShadowIncoming;
    } else {
      if (incoming.descriptor == existing.descriptor) {
        resolution.decision = KeybindingResolution::Decision::kShadowIncoming;
        ConflictRecord conflict;
        conflict.resource = RegistryResourceKind::kKeybinding;
        conflict.id = incoming.descriptor.id;
        conflict.winner_origin = existing.origin;
        conflict.loser_origin = incoming.origin;
        conflict.message =
            "Duplicate keybinding ignored (same precedence and priority)";
        resolution.conflict = conflict;
        return resolution;
      }

      resolution.decision = KeybindingResolution::Decision::kRejectIncoming;
      ConflictRecord conflict;
      conflict.resource = RegistryResourceKind::kKeybinding;
      conflict.id = incoming.descriptor.id;
      conflict.winner_origin = existing.origin;
      conflict.loser_origin = incoming.origin;
      conflict.message =
          "Conflicting keybinding with identical precedence and priority";
      resolution.conflict = conflict;
      return resolution;
    }
  }

  ConflictRecord conflict;
  conflict.resource = RegistryResourceKind::kKeybinding;
  conflict.id = incoming.descriptor.id;
  conflict.winner_origin =
      resolution.decision == KeybindingResolution::Decision::kReplaceExisting
          ? incoming.origin
          : existing.origin;
  conflict.loser_origin =
      resolution.decision == KeybindingResolution::Decision::kReplaceExisting
          ? existing.origin
          : incoming.origin;
  conflict.message =
      resolution.decision == KeybindingResolution::Decision::kReplaceExisting
          ? "Replaced keybinding due to higher precedence or priority"
          : "Keybinding shadowed by higher precedence or priority";
  resolution.conflict = conflict;
  return resolution;
}

}  // namespace core
