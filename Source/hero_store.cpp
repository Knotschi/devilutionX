#include "hero_store.hpp"

#include <cstdint>
#include <map>
#include <utility>

namespace devilution {

namespace {

constexpr uint8_t HeroStoreVersion = 0;

using HeroStore = std::map<std::string, std::string, std::less<>>;

HeroStore &Store()
{
	static HeroStore store;
	return store;
}

[[nodiscard]] bool IsValidKeySize(size_t size)
{
	return size > 0 && size <= HeroStoreMaxKeySize;
}

class Reader {
public:
	explicit Reader(std::span<const std::byte> data)
	    : data_(data)
	{
	}

	[[nodiscard]] bool AtEnd() const { return data_.empty(); }

	std::optional<uint8_t> NextU8()
	{
		if (data_.empty())
			return std::nullopt;
		const auto value = static_cast<uint8_t>(data_.front());
		data_ = data_.subspan(1);
		return value;
	}

	std::optional<uint16_t> NextU16()
	{
		const std::optional<uint8_t> low = NextU8();
		const std::optional<uint8_t> high = NextU8();
		if (!low || !high)
			return std::nullopt;
		return static_cast<uint16_t>(*low | (*high << 8));
	}

	std::optional<std::string> NextString(size_t size)
	{
		if (data_.size() < size)
			return std::nullopt;
		std::string value(reinterpret_cast<const char *>(data_.data()), size);
		data_ = data_.subspan(size);
		return value;
	}

private:
	std::span<const std::byte> data_;
};

std::optional<HeroStore> Parse(std::span<const std::byte> data)
{
	Reader reader(data);
	if (reader.NextU8() != HeroStoreVersion)
		return std::nullopt;
	const std::optional<uint16_t> count = reader.NextU16();
	if (!count || *count > HeroStoreMaxEntries)
		return std::nullopt;

	HeroStore result;
	for (uint16_t i = 0; i < *count; i++) {
		const std::optional<uint8_t> keySize = reader.NextU8();
		if (!keySize || !IsValidKeySize(*keySize))
			return std::nullopt;
		std::optional<std::string> key = reader.NextString(*keySize);
		const std::optional<uint16_t> valueSize = reader.NextU16();
		if (!key || !valueSize || *valueSize > HeroStoreMaxValueSize)
			return std::nullopt;
		std::optional<std::string> value = reader.NextString(*valueSize);
		if (!value || !result.try_emplace(std::move(*key), std::move(*value)).second)
			return std::nullopt;
	}
	if (!reader.AtEnd())
		return std::nullopt;
	return result;
}

void AppendU16(std::vector<std::byte> &out, size_t value)
{
	out.push_back(static_cast<std::byte>(value & 0xFF));
	out.push_back(static_cast<std::byte>((value >> 8) & 0xFF));
}

void AppendString(std::vector<std::byte> &out, std::string_view value)
{
	const auto *bytes = reinterpret_cast<const std::byte *>(value.data());
	out.insert(out.end(), bytes, bytes + value.size());
}

} // namespace

std::optional<std::string> HeroStoreGet(std::string_view key)
{
	const HeroStore &store = Store();
	const auto it = store.find(key);
	if (it == store.end())
		return std::nullopt;
	return it->second;
}

bool HeroStoreSet(std::string_view key, std::optional<std::string_view> value)
{
	if (!IsValidKeySize(key.size()))
		return false;

	HeroStore &store = Store();
	const auto it = store.find(key);
	if (!value) {
		if (it != store.end())
			store.erase(it);
		return true;
	}
	if (value->size() > HeroStoreMaxValueSize)
		return false;
	if (it != store.end()) {
		it->second = *value;
		return true;
	}
	if (store.size() >= HeroStoreMaxEntries)
		return false;
	store.emplace(key, *value);
	return true;
}

void ClearHeroStore()
{
	Store().clear();
}

bool IsHeroStoreEmpty()
{
	return Store().empty();
}

std::vector<std::byte> SerializeHeroStore()
{
	const HeroStore &store = Store();
	std::vector<std::byte> out;
	out.push_back(static_cast<std::byte>(HeroStoreVersion));
	AppendU16(out, store.size());
	for (const auto &[key, value] : store) {
		out.push_back(static_cast<std::byte>(key.size()));
		AppendString(out, key);
		AppendU16(out, value.size());
		AppendString(out, value);
	}
	return out;
}

bool DeserializeHeroStore(std::span<const std::byte> data)
{
	std::optional<HeroStore> parsed = Parse(data);
	if (!parsed) {
		ClearHeroStore();
		return false;
	}
	Store() = std::move(*parsed);
	return true;
}

} // namespace devilution
