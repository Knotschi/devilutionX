#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "codec.h"
#include "game_mode.hpp"
#include "hero_store.hpp"
#include "loadsave.h"
#include "lua_test_fixture.hpp"
#include "menu.h"
#include "pfile.h"
#include "utils/file_util.h"
#include "utils/str_cat.hpp"
#include "utils/utf8.hpp"

namespace devilution {
namespace {

std::vector<std::byte> Bytes(std::initializer_list<int> values)
{
	std::vector<std::byte> result;
	for (const int value : values)
		result.push_back(static_cast<std::byte>(value));
	return result;
}

class HeroStoreTest : public ::testing::Test {
protected:
	void SetUp() override { ClearHeroStore(); }
	void TearDown() override { ClearHeroStore(); }
};

TEST_F(HeroStoreTest, Set_Get_Delete)
{
	EXPECT_EQ(HeroStoreGet("mod.key"), std::nullopt);
	EXPECT_TRUE(HeroStoreSet("mod.key", "one"));
	EXPECT_EQ(HeroStoreGet("mod.key"), "one");
	EXPECT_TRUE(HeroStoreSet("mod.key", "two"));
	EXPECT_EQ(HeroStoreGet("mod.key"), "two");
	EXPECT_FALSE(IsHeroStoreEmpty());
	EXPECT_TRUE(HeroStoreSet("mod.key", std::nullopt));
	EXPECT_EQ(HeroStoreGet("mod.key"), std::nullopt);
	EXPECT_TRUE(IsHeroStoreEmpty());
}

TEST_F(HeroStoreTest, EmptyValue_IsAValue)
{
	EXPECT_TRUE(HeroStoreSet("k", ""));
	EXPECT_EQ(HeroStoreGet("k"), "");
}

TEST_F(HeroStoreTest, Delete_MissingKey_Succeeds)
{
	EXPECT_TRUE(HeroStoreSet("k", std::nullopt));
}

TEST_F(HeroStoreTest, Limits_Rejected)
{
	EXPECT_FALSE(HeroStoreSet("", "v"));
	EXPECT_TRUE(HeroStoreSet(std::string(64, 'k'), "v"));
	EXPECT_FALSE(HeroStoreSet(std::string(65, 'k'), "v"));
	EXPECT_EQ(HeroStoreGet(std::string(65, 'k')), std::nullopt);
	EXPECT_TRUE(HeroStoreSet("k", std::string(4096, 'v')));
	EXPECT_FALSE(HeroStoreSet("k", std::string(4097, 'v')));
	EXPECT_EQ(HeroStoreGet("k")->size(), 4096U);
}

TEST_F(HeroStoreTest, EntryLimit_StillAllowsReplaceAndDelete)
{
	for (int i = 0; i < 256; i++)
		ASSERT_TRUE(HeroStoreSet(std::to_string(i), "v"));
	EXPECT_FALSE(HeroStoreSet("one too many", "v"));
	EXPECT_EQ(HeroStoreGet("one too many"), std::nullopt);
	EXPECT_TRUE(HeroStoreSet("0", "new"));
	EXPECT_EQ(HeroStoreGet("0"), "new");
	EXPECT_TRUE(HeroStoreSet("0", std::nullopt));
	EXPECT_TRUE(HeroStoreSet("one too many", "v"));
}

TEST_F(HeroStoreTest, Serialize_MatchesFormat)
{
	HeroStoreSet("ab", "xyz");
	EXPECT_EQ(SerializeHeroStore(), Bytes({ 0, 1, 0, 2, 'a', 'b', 3, 0, 'x', 'y', 'z' }));
}

TEST_F(HeroStoreTest, Serialize_Deserialize_FullStore)
{
	for (int i = 0; i < 256; i++)
		ASSERT_TRUE(HeroStoreSet(std::string(63, 'k') + static_cast<char>(i), std::string(4096, static_cast<char>(i))));
	const std::vector<std::byte> data = SerializeHeroStore();
	ClearHeroStore();
	ASSERT_TRUE(DeserializeHeroStore(data));
	EXPECT_EQ(HeroStoreGet(std::string(63, 'k') + static_cast<char>(200)), std::string(4096, static_cast<char>(200)));
	EXPECT_EQ(SerializeHeroStore(), data);
}

TEST_F(HeroStoreTest, Deserialize_EmptyStore)
{
	HeroStoreSet("old", "v");
	EXPECT_TRUE(DeserializeHeroStore(Bytes({ 0, 0, 0 })));
	EXPECT_TRUE(IsHeroStoreEmpty());
}

TEST_F(HeroStoreTest, Corrupt_EveryTruncation_ClearsSafely)
{
	const std::vector<std::byte> whole = Bytes({ 0, 2, 0, 1, 'a', 1, 0, 'x', 1, 'b', 2, 0, 'y', 'z' });
	for (size_t size = 0; size < whole.size(); size++) {
		HeroStoreSet("old", "v");
		EXPECT_FALSE(DeserializeHeroStore(std::span(whole).first(size))) << size;
		EXPECT_TRUE(IsHeroStoreEmpty()) << size;
	}
	EXPECT_TRUE(DeserializeHeroStore(whole));
}

TEST_F(HeroStoreTest, Corrupt_BadContents_ClearsSafely)
{
	const std::vector<std::vector<std::byte>> cases = {
		Bytes({ 0, 1, 0, 0, 0, 0 }),                    // empty key
		Bytes({ 0, 1, 0, 65 }),                         // key over the limit
		Bytes({ 0, 1, 0, 1, 'a', 0x01, 0x10 }),         // value over the limit
		Bytes({ 0, 2, 0, 1, 'a', 0, 0, 1, 'a', 0, 0 }), // duplicate key
		Bytes({ 0, 0, 0, 1 }),                          // trailing data
		Bytes({ 0, 0xff, 0xff }),                       // huge count
		Bytes({ 0, 1, 0, 1, 'a', 0, 0, 1 }),            // more data than the count says
	};
	for (const std::vector<std::byte> &data : cases) {
		HeroStoreSet("old", "v");
		EXPECT_FALSE(DeserializeHeroStore(data));
		EXPECT_TRUE(IsHeroStoreEmpty());
	}
}

TEST_F(HeroStoreTest, Corrupt_TooManyEntries_ClearsSafely)
{
	std::vector<std::byte> data = Bytes({ 0, 1, 1 }); // 257
	for (int i = 0; i < 257; i++) {
		const std::vector<std::byte> entry = Bytes({ 2, i & 0xff, i >> 8, 0, 0 });
		data.insert(data.end(), entry.begin(), entry.end());
	}
	EXPECT_FALSE(DeserializeHeroStore(data));
	EXPECT_TRUE(IsHeroStoreEmpty());
}

TEST_F(HeroStoreTest, Corrupt_ValueOverLimitWithData_ClearsSafely)
{
	std::vector<std::byte> data = Bytes({ 0, 1, 0, 1, 'a', 0x01, 0x10 }); // 4097
	data.resize(data.size() + 4097, std::byte { 'v' });
	EXPECT_FALSE(DeserializeHeroStore(data));
	EXPECT_TRUE(IsHeroStoreEmpty());
}

TEST_F(HeroStoreTest, UnknownVersion_Ignored)
{
	HeroStoreSet("old", "v");
	EXPECT_FALSE(DeserializeHeroStore(Bytes({ 1, 1, 0, 1, 'a', 0, 0 })));
	EXPECT_TRUE(IsHeroStoreEmpty());
}

class HeroStoreSaveTest : public LuaUITest {
protected:
	static constexpr uint32_t SaveNumber = 7;

	void SetUp() override
	{
		LuaUITest::SetUp();
		if (IsSkipped())
			return;
		gbVanilla = false;
		giNumberOfLevels = 17;
		gSaveNumber = SaveNumber;
		RemoveFile(SavePath().c_str());
		ClearHeroStore();
		CreateHero(SaveNumber);
	}

	void TearDown() override
	{
		ClearHeroStore();
		gbVanilla = false;
		LuaUITest::TearDown();
	}

	static std::string SavePath(uint32_t saveNumber = SaveNumber)
	{
		return StrCat(paths::PrefPath(), gbIsSpawn ? "spawn_" : "single_", saveNumber, ".sv");
	}

	void CreateHero(uint32_t saveNumber)
	{
		_uiheroinfo info {};
		info.heroclass = HeroClass::Warrior;
		info.saveNumber = saveNumber;
		CopyUtf8(info.name, "Hero", sizeof(info.name));
		FreeCursor();
		EXPECT_TRUE(pfile_ui_save_create(&info));
		InitCursor();
	}

	static void ReadHeroBack()
	{
		FreeCursor();
		pfile_read_player_from_save(SaveNumber, *MyPlayer);
		InitCursor();
	}

	static bool HasStoreEntry()
	{
		std::optional<SaveReader> archive = OpenSaveArchive(SaveNumber);
		return archive && archive->HasFile("herostore");
	}

	static void WriteRawEntry(std::span<const std::byte> encoded)
	{
		SaveWriter writer(SavePath());
		EXPECT_TRUE(writer.WriteFile("herostore", encoded.data(), encoded.size()));
	}

	static void WriteEncodedEntry(const std::vector<std::byte> &data)
	{
		const size_t encodedSize = codec_get_encoded_len(data.size());
		std::vector<std::byte> encoded(encodedSize);
		std::copy(data.begin(), data.end(), encoded.begin());
		codec_encode(encoded.data(), data.size(), encodedSize, pfile_get_password());
		WriteRawEntry(encoded);
	}
};

TEST_F(HeroStoreSaveTest, Roundtrip_ThroughSaveArchive)
{
	HeroStoreSet("mod.a", "alpha");
	HeroStoreSet("mod.b", "");
	pfile_write_hero();
	ClearHeroStore();
	ReadHeroBack();
	EXPECT_EQ(HeroStoreGet("mod.a"), "alpha");
	EXPECT_EQ(HeroStoreGet("mod.b"), "");
}

TEST_F(HeroStoreSaveTest, MissingEntry_Clears)
{
	HeroStoreSet("stale", "v");
	ASSERT_FALSE(HasStoreEntry());
	ReadHeroBack();
	EXPECT_TRUE(IsHeroStoreEmpty());
}

TEST_F(HeroStoreSaveTest, EmptyStore_WritesNoEntry)
{
	pfile_write_hero();
	EXPECT_FALSE(HasStoreEntry());
}

TEST_F(HeroStoreSaveTest, EmptiedStore_RemovesEntry)
{
	HeroStoreSet("k", "v");
	pfile_write_hero();
	ASSERT_TRUE(HasStoreEntry());
	ClearHeroStore();
	pfile_write_hero();
	EXPECT_FALSE(HasStoreEntry());
}

TEST_F(HeroStoreSaveTest, EmptiedStore_FullSave_RemovesEntry)
{
	HeroStoreSet("k", "v");
	SaveGame();
	ASSERT_TRUE(HasStoreEntry());
	ClearHeroStore();
	SaveGame();
	EXPECT_FALSE(HasStoreEntry());
}

TEST_F(HeroStoreSaveTest, Vanilla_WritesNoEntry)
{
	gbVanilla = true;
	HeroStoreSet("k", "v");
	pfile_write_hero();
	EXPECT_FALSE(HasStoreEntry());
}

TEST_F(HeroStoreSaveTest, NewHero_InReusedSlot_SeesNothing)
{
	HeroStoreSet("k", "v");
	pfile_write_hero();
	ASSERT_TRUE(HasStoreEntry());

	CreateHero(SaveNumber);
	EXPECT_TRUE(IsHeroStoreEmpty());
	EXPECT_FALSE(HasStoreEntry());
	ReadHeroBack();
	EXPECT_TRUE(IsHeroStoreEmpty());
}

TEST_F(HeroStoreSaveTest, NewHero_InOtherSlot_StartsEmpty)
{
	HeroStoreSet("k", "v");
	pfile_write_hero();
	RemoveFile(SavePath(SaveNumber + 1).c_str());
	CreateHero(SaveNumber + 1);
	EXPECT_TRUE(IsHeroStoreEmpty());
	RemoveFile(SavePath(SaveNumber + 1).c_str());
}

TEST_F(HeroStoreSaveTest, Corrupt_InvalidEntry_ClearsSafely)
{
	HeroStoreSet("stale", "v");
	WriteEncodedEntry(Bytes({ 0, 5, 0, 1, 'a' }));
	ReadHeroBack();
	EXPECT_TRUE(IsHeroStoreEmpty());
}

TEST_F(HeroStoreSaveTest, UnknownVersion_ClearsSafely)
{
	HeroStoreSet("stale", "v");
	WriteEncodedEntry(Bytes({ 9, 0, 0 }));
	ReadHeroBack();
	EXPECT_TRUE(IsHeroStoreEmpty());
}

TEST_F(HeroStoreSaveTest, Corrupt_NotEncodable_ClearsSafely)
{
	const std::vector<std::vector<std::byte>> cases = {
		Bytes({ 1 }), std::vector<std::byte>(8), std::vector<std::byte>(64, std::byte { 0xAB }), std::vector<std::byte>(72, std::byte { 0xAB })
	};
	for (const std::vector<std::byte> &raw : cases) {
		HeroStoreSet("stale", "v");
		WriteRawEntry(raw);
		ReadHeroBack();
		EXPECT_TRUE(IsHeroStoreEmpty()) << raw.size();
	}
}

TEST_F(HeroStoreSaveTest, LoadHeroStore_InGameLoadReplacesStore)
{
	HeroStoreSet("saved", "v");
	pfile_write_hero();
	HeroStoreSet("saved", "changed");
	LoadHeroStore(SaveNumber);
	EXPECT_EQ(HeroStoreGet("saved"), "v");
}

TEST_F(HeroStoreSaveTest, LoadGame_LoadsStore)
{
	HeroStoreSet("saved", "v");
	SaveGame();
	HeroStoreSet("saved", "changed");
	ASSERT_TRUE(LoadGame(false).has_value());
	EXPECT_EQ(HeroStoreGet("saved"), "v");
}

using HeroStoreLuaTest = LuaUITest;

TEST_F(HeroStoreLuaTest, Lua_get_set_nil_delete)
{
	ClearHeroStore();
	EXPECT_EQ(EvalAs<bool>("hs = require('devilutionx.herostore'); return hs.set('mod.k', 'v')"), true);
	EXPECT_EQ(HeroStoreGet("mod.k"), "v");
	EXPECT_EQ(EvalAs<std::string>("return require('devilutionx.herostore').get('mod.k')"), "v");
	EXPECT_EQ(EvalAs<bool>("return require('devilutionx.herostore').set('mod.k', nil)"), true);
	EXPECT_EQ(HeroStoreGet("mod.k"), std::nullopt);
	EXPECT_EQ(EvalAs<bool>("return require('devilutionx.herostore').get('mod.k') == nil"), true);
	HeroStoreSet("mod.k", "v");
	EXPECT_EQ(EvalAs<bool>("return require('devilutionx.herostore').set('mod.k')"), true);
	EXPECT_EQ(HeroStoreGet("mod.k"), std::nullopt);
}

TEST_F(HeroStoreLuaTest, Lua_Value_KeepsEmbeddedZero)
{
	ClearHeroStore();
	EXPECT_EQ(EvalAs<bool>("local hs = require('devilutionx.herostore'); hs.set('k', 'a\\0b'); return hs.get('k') == 'a\\0b'"), true);
	ClearHeroStore();
}

TEST_F(HeroStoreLuaTest, Lua_Limits_ReturnFalse)
{
	ClearHeroStore();
	EXPECT_EQ(EvalAs<bool>("return require('devilutionx.herostore').set(string.rep('k', 65), 'v')"), false);
	EXPECT_EQ(EvalAs<bool>("return require('devilutionx.herostore').set('k', string.rep('v', 4097))"), false);
	EXPECT_TRUE(IsHeroStoreEmpty());
}

TEST_F(HeroStoreLuaTest, Lua_BadArgs_NoCrash)
{
	ClearHeroStore();
	for (const std::string_view args : { "", "nil", "1", "{}", "true", "print" })
		EXPECT_EQ(EvalAs<bool>(StrCat("return require('devilutionx.herostore').get(", args, ") == nil")), true) << args;
	for (const std::string_view args : { "", "nil", "nil, 'v'", "1, 'v'", "{}, 'v'", "'k', 1", "'k', {}", "'k', true", "'k', false", "'k', print" })
		EXPECT_EQ(EvalAs<bool>(StrCat("return require('devilutionx.herostore').set(", args, ")")), false) << args;
	EXPECT_TRUE(IsHeroStoreEmpty());
}

TEST_F(HeroStoreLuaTest, Lua_NumberKey_IsNotItsString)
{
	ClearHeroStore();
	HeroStoreSet("1", "v");
	EXPECT_EQ(EvalAs<bool>("return require('devilutionx.herostore').get(1) == nil"), true);
	ClearHeroStore();
}

TEST_F(HeroStoreLuaTest, Lua_FunctionsAreDocumented)
{
	EXPECT_EQ(EvalAs<bool>("local hs = require('devilutionx.herostore'); return hs.__sig_get ~= nil or hs.__doc_get ~= nil"), true);
}

} // namespace
} // namespace devilution
