/**
 * @file hero_store.hpp
 *
 * Small key/value store that belongs to the current hero and is kept in the hero's save.
 */
#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace devilution {

constexpr size_t HeroStoreMaxKeySize = 64;
constexpr size_t HeroStoreMaxValueSize = 4096;
constexpr size_t HeroStoreMaxEntries = 256;

/** @brief Returns the value of a key, if it has one. */
[[nodiscard]] std::optional<std::string> HeroStoreGet(std::string_view key);

/**
 * @brief Sets a key, or deletes it if the value is empty (std::nullopt).
 * @return false if a limit is exceeded or the key is empty. The store does not change then.
 */
bool HeroStoreSet(std::string_view key, std::optional<std::string_view> value);

void ClearHeroStore();
[[nodiscard]] bool IsHeroStoreEmpty();

/** @brief Encodes the store: u8 version, u16 count, then { u8 keySize, key, u16 valueSize, value } per entry. */
[[nodiscard]] std::vector<std::byte> SerializeHeroStore();

/**
 * @brief Replaces the store with encoded data.
 * @return false if the data is invalid or from a newer version. The store is empty then.
 */
bool DeserializeHeroStore(std::span<const std::byte> data);

} // namespace devilution
