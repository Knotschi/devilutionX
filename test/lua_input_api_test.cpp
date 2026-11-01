/**
 * @file lua_input_api_test.cpp
 *
 * Tests for key and gamepad actions added while the game runs, and for the Lua API on top of them.
 */

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "lua_test_fixture.hpp"

#include "controls/controller.h"
#include "controls/controller_buttons.h"
#include "controls/keymapper.hpp"
#include "controls/padmapper.hpp"
#include "lua/metadoc.hpp"
#include "lua/modules/input.hpp"
#include "options.h"
#include "player.h"
#include "utils/ini.hpp"
#include "utils/paths.h"
#include "utils/str_cat.hpp"

namespace devilution {
namespace {

using ::testing::HasSubstr;
using ::testing::Not;

constexpr uint32_t FreeKey = 'W';
constexpr uint32_t OtherFreeKey = 'H';
// Plain buttons all have vanilla actions.
constexpr ControllerButtonCombo FreeCombo { ControllerButton_BUTTON_A, ControllerButton_BUTTON_B };
constexpr ControllerButtonCombo OtherFreeCombo { ControllerButton_BUTTON_A, ControllerButton_BUTTON_X };
constexpr ControllerButton FreeButton = FreeCombo.button;

std::vector<std::string> KeysOf(std::vector<OptionEntryBase *> entries)
{
	std::vector<std::string> keys;
	for (const OptionEntryBase *entry : entries)
		keys.emplace_back(entry->key);
	return keys;
}

KeymapperOptions::Action *FindKeyAction(std::string_view id)
{
	for (OptionEntryBase *entry : GetOptions().Keymapper.GetEntries()) {
		if (entry->key == id)
			return static_cast<KeymapperOptions::Action *>(entry);
	}
	return nullptr;
}

PadmapperOptions::Action *FindPadAction(std::string_view id)
{
	for (OptionEntryBase *entry : GetOptions().Padmapper.GetEntries()) {
		if (entry->key == id)
			return static_cast<PadmapperOptions::Action *>(entry);
	}
	return nullptr;
}

/** @brief Collects what is logged while it lives. */
class ScopedLogCapture {
public:
	ScopedLogCapture()
	{
#ifdef USE_SDL3
		SDL_GetLogOutputFunction(&previous_, &previousData_);
		SDL_SetLogOutputFunction(Append, &text_);
#else
		SDL_LogGetOutputFunction(&previous_, &previousData_);
		SDL_LogSetOutputFunction(Append, &text_);
#endif
	}

	~ScopedLogCapture()
	{
#ifdef USE_SDL3
		SDL_SetLogOutputFunction(previous_, previousData_);
#else
		SDL_LogSetOutputFunction(previous_, previousData_);
#endif
	}

	[[nodiscard]] const std::string &text() const { return text_; }

private:
	static void SDLCALL Append(void *text, int /*category*/, SDL_LogPriority /*priority*/, const char *message)
	{
		static_cast<std::string *>(text)->append(message);
	}

	SDL_LogOutputFunction previous_;
	void *previousData_;
	std::string text_;
};

class LuaInputTest : public LuaUITest {
protected:
	void SetUp() override
	{
		LuaUITest::SetUp();
		if (IsSkipped())
			return;
		ASSERT_THAT(paths::ConfigPath(), HasSubstr("devilutionx_lua_test_"));
		keysBefore_ = KeysOf(GetOptions().Keymapper.GetEntries());
		padsBefore_ = KeysOf(GetOptions().Padmapper.GetEntries());
	}

	void TearDown() override
	{
		if (!IsSkipped()) {
			ClearLuaActions();
			GetOptions().Keymapper.RemoveRuntimeAction(Id());
			PadmapperRemoveRuntimeAction(Id());
			EXPECT_EQ(KeysOf(GetOptions().Keymapper.GetEntries()), keysBefore_);
			EXPECT_EQ(KeysOf(GetOptions().Padmapper.GetEntries()), padsBefore_);
		}
		LuaUITest::TearDown();
	}

	/** @brief An id for this test only: removing an action leaves its binding in the ini. */
	static std::string Id(std::string_view suffix = "")
	{
		return StrCat("test.", ::testing::UnitTest::GetInstance()->current_test_info()->name(), suffix);
	}

	static bool AddKey(const std::string &id, uint32_t defaultKey, std::function<void()> pressed = nullptr, std::function<void()> released = nullptr)
	{
		return GetOptions().Keymapper.AddRuntimeAction(id, "Name", "Description", defaultKey, std::move(pressed), std::move(released));
	}

	static bool AddPad(const std::string &id, ControllerButtonCombo defaultInput, std::function<void()> pressed = nullptr, std::function<void()> released = nullptr)
	{
		return GetOptions().Padmapper.AddRuntimeAction(id, "Name", "Description", defaultInput, std::move(pressed), std::move(released));
	}

	/** @brief An action whose callbacks count in the sandbox's globals `presses` and `releases`. */
	static void AddSpy(const std::string &id, std::string_view defaultKey = "W")
	{
		const sol::protected_function_result result = Eval(StrCat(R"lua(
			input = require("devilutionx.input")
			presses, releases = 0, 0
			input.addAction {
				id = ")lua",
		    id, R"lua(",
				name = "Lua name",
				description = "Lua description",
				defaultKey = ")lua",
		    defaultKey, R"lua(",
				onPress = function() presses = presses + 1 end,
				onRelease = function() releases = releases + 1 end,
			}
		)lua"));
		ASSERT_TRUE(result.valid()) << result.get<sol::error>().what();
	}

	static int Presses() { return EvalAs<int>("return presses").value_or(-1); }
	static int Releases() { return EvalAs<int>("return releases").value_or(-1); }

	/** @brief The error of adding this spec, empty if it was accepted. */
	static std::string AddError(std::string_view spec)
	{
		const sol::protected_function_result result = Eval(StrCat(R"lua(return require("devilutionx.input").addAction()lua", spec, ")"));
		if (result.valid())
			return {};
		return result.get<sol::error>().what();
	}

	static std::string IniPath()
	{
		return StrCat(paths::ConfigPath(), "diablo.ini");
	}

	static std::string ReadIniFile()
	{
		std::ifstream file(IniPath(), std::ios::binary);
		std::stringstream contents;
		contents << file.rdbuf();
		return contents.str();
	}

	/** @brief Give a key a value on disk, the way a previous run of the game would have saved it, and load it. */
	static void SetIniValue(std::string_view section, std::string_view key, std::string_view value)
	{
		SaveOptions();
		std::expected<Ini, std::string> ini = Ini::parse(ReadIniFile());
		ASSERT_TRUE(ini.has_value());
		ini->set(section, key, value);
		std::ofstream(IniPath(), std::ios::binary | std::ios::trunc) << ini->serialize();
		LoadOptions();
	}

	std::vector<std::string> keysBefore_;
	std::vector<std::string> padsBefore_;
};

// Keymapper actions.

// Must stay the first suite: all others load the ini, which then stays loaded.
TEST(RuntimeActionBeforeIniTest, AddAndRemove_UseDefaults)
{
	int presses = 0;
	KeymapperOptions &keymapper = GetOptions().Keymapper;
	PadmapperOptions &padmapper = GetOptions().Padmapper;

	ASSERT_TRUE(keymapper.AddRuntimeAction("test.noIni", "Name", "Description", FreeKey, [&presses] { presses++; }, nullptr));
	ASSERT_TRUE(padmapper.AddRuntimeAction("test.noIni", "Name", "Description", FreeCombo, nullptr, nullptr));

	EXPECT_EQ(keymapper.KeyForAction("test.noIni"), FreeKey);
	EXPECT_EQ(padmapper.ButtonComboForAction("test.noIni").button, FreeCombo.button);
	KeymapperPress(static_cast<SDL_Keycode>(FreeKey));
	EXPECT_EQ(presses, 1);

	keymapper.RemoveRuntimeAction("test.noIni");
	PadmapperRemoveRuntimeAction("test.noIni");
	EXPECT_FALSE(keymapper.HasAction("test.noIni"));
	EXPECT_FALSE(padmapper.HasAction("test.noIni"));
}

TEST_F(LuaInputTest, AddRuntimeAction_ListedInEntries_BindsDefault)
{
	const std::vector<OptionEntryBase *> before = GetOptions().Keymapper.GetEntries();

	ASSERT_TRUE(AddKey(Id(), FreeKey));

	const std::vector<OptionEntryBase *> after = GetOptions().Keymapper.GetEntries();
	ASSERT_EQ(after.size(), before.size() + 1);
	EXPECT_TRUE(std::equal(before.begin(), before.end(), after.begin()));
	const OptionEntryBase &entry = *after.back();
	EXPECT_EQ(entry.key, Id());
	EXPECT_EQ(entry.GetName(), "Name");
	EXPECT_EQ(entry.GetDescription(), "Description");
	EXPECT_EQ(entry.GetType(), OptionEntryType::Key);
	EXPECT_EQ(entry.GetValueDescription(), "W");
	EXPECT_EQ(GetOptions().Keymapper.KeyForAction(Id()), FreeKey);
	ASSERT_NE(GetOptions().Keymapper.findAction(FreeKey), nullptr);
	EXPECT_EQ(GetOptions().Keymapper.findAction(FreeKey), FindKeyAction(Id()));
	EXPECT_TRUE(FindKeyAction(Id())->isRuntime());
	EXPECT_FALSE(FindKeyAction("Help")->isRuntime());
}

TEST_F(LuaInputTest, AddRuntimeAction_ExistingId_Refused)
{
	EXPECT_FALSE(AddKey("Help", FreeKey));
	EXPECT_EQ(GetOptions().Keymapper.KeyForAction("Help"), SDLK_F1);
	EXPECT_EQ(GetOptions().Keymapper.findAction(FreeKey), nullptr);

	ASSERT_TRUE(AddKey(Id(), FreeKey));
	EXPECT_FALSE(AddKey(Id(), OtherFreeKey));
	EXPECT_EQ(GetOptions().Keymapper.KeyForAction(Id()), FreeKey);
}

TEST_F(LuaInputTest, HasAction_FindsAnyAction)
{
	ASSERT_TRUE(AddKey(Id(), FreeKey));
	ASSERT_TRUE(AddPad(Id(), ControllerButton_NONE));

	EXPECT_TRUE(GetOptions().Keymapper.HasAction("Help"));
	EXPECT_TRUE(GetOptions().Padmapper.HasAction("Help"));
	EXPECT_TRUE(GetOptions().Keymapper.HasAction(Id()));
	EXPECT_TRUE(GetOptions().Padmapper.HasAction(Id()));
	EXPECT_FALSE(GetOptions().Keymapper.HasAction("NoSuchAction"));
	EXPECT_FALSE(GetOptions().Padmapper.HasAction("NoSuchAction"));
}

TEST_F(LuaInputTest, KeyFromName_KnownNames)
{
	EXPECT_EQ(GetOptions().Keymapper.KeyFromName("W"), FreeKey);
	EXPECT_EQ(GetOptions().Keymapper.KeyFromName("F9"), static_cast<uint32_t>(SDLK_F9));
	EXPECT_EQ(GetOptions().Keymapper.KeyFromName("LALT"), static_cast<uint32_t>(SDLK_LALT));
	EXPECT_EQ(GetOptions().Keymapper.KeyFromName("w"), std::nullopt);
	EXPECT_EQ(GetOptions().Keymapper.KeyFromName(""), std::nullopt);
}

TEST_F(LuaInputTest, Default_AlreadyBound_LeftUnbound)
{
	const uint32_t helpKey = GetOptions().Keymapper.KeyForAction("Help");
	ASSERT_NE(helpKey, SDLK_UNKNOWN);
	ASSERT_TRUE(AddKey(Id("a"), helpKey));
	EXPECT_EQ(GetOptions().Keymapper.KeyForAction("Help"), helpKey);
	EXPECT_EQ(GetOptions().Keymapper.KeyForAction(Id("a")), SDLK_UNKNOWN);
	EXPECT_EQ(GetOptions().Keymapper.findAction(helpKey), FindKeyAction("Help"));

	ASSERT_TRUE(AddKey(Id("b"), FreeKey));
	ASSERT_TRUE(AddKey(Id("c"), FreeKey));
	EXPECT_EQ(GetOptions().Keymapper.KeyForAction(Id("b")), FreeKey);
	EXPECT_EQ(GetOptions().Keymapper.KeyForAction(Id("c")), SDLK_UNKNOWN);

	GetOptions().Keymapper.RemoveRuntimeAction(Id("a"));
	GetOptions().Keymapper.RemoveRuntimeAction(Id("b"));
	GetOptions().Keymapper.RemoveRuntimeAction(Id("c"));
}

TEST_F(LuaInputTest, IniBinding_Respected)
{
	SetIniValue("Keymapping", Id(), "H");

	ASSERT_TRUE(AddKey(Id(), FreeKey));

	EXPECT_EQ(GetOptions().Keymapper.KeyForAction(Id()), OtherFreeKey);
	EXPECT_EQ(GetOptions().Keymapper.findAction(FreeKey), nullptr);
}

TEST_F(LuaInputTest, IniEmptyValue_StaysUnbound)
{
	SetIniValue("Keymapping", Id(), "");

	ASSERT_TRUE(AddKey(Id(), FreeKey));

	EXPECT_EQ(GetOptions().Keymapper.KeyForAction(Id()), SDLK_UNKNOWN);
}

TEST_F(LuaInputTest, IniUnknownKey_UsesDefault)
{
	SetIniValue("Keymapping", Id(), "NoSuchKey");

	ASSERT_TRUE(AddKey(Id(), FreeKey));

	EXPECT_EQ(GetOptions().Keymapper.KeyForAction(Id()), FreeKey);
}

TEST_F(LuaInputTest, IniBinding_HeldByAnotherAction_NotStolen)
{
	// The mod was off when the user gave this key to something else.
	const uint32_t helpKey = GetOptions().Keymapper.KeyForAction("Help");
	ASSERT_NE(helpKey, SDLK_UNKNOWN);
	SetIniValue("Keymapping", Id(), "F1");
	ASSERT_EQ(helpKey, static_cast<uint32_t>(SDLK_F1));

	ASSERT_TRUE(AddKey(Id(), FreeKey));

	EXPECT_EQ(GetOptions().Keymapper.KeyForAction("Help"), helpKey);
	EXPECT_EQ(GetOptions().Keymapper.KeyForAction(Id()), SDLK_UNKNOWN);
}

TEST_F(LuaInputTest, Strings_Owned)
{
	std::string id = Id("-a-long-id-past-the-small-string-buffer");
	const std::string expectedId = id;
	std::string name = "A name past the small string buffer";
	std::string description = "A description past the small string buffer";

	ASSERT_TRUE(GetOptions().Keymapper.AddRuntimeAction(id, name, description, FreeKey, nullptr, nullptr));
	ASSERT_TRUE(GetOptions().Padmapper.AddRuntimeAction(id, name, description, ControllerButton_NONE, nullptr, nullptr));
	std::fill(id.begin(), id.end(), 'x');
	std::fill(name.begin(), name.end(), 'x');
	std::fill(description.begin(), description.end(), 'x');

	const KeymapperOptions::Action *key = FindKeyAction(expectedId);
	ASSERT_NE(key, nullptr);
	EXPECT_EQ(key->GetName(), "A name past the small string buffer");
	EXPECT_EQ(key->GetDescription(), "A description past the small string buffer");
	const PadmapperOptions::Action *pad = FindPadAction(expectedId);
	ASSERT_NE(pad, nullptr);
	EXPECT_EQ(pad->GetName(), "A name past the small string buffer");
	EXPECT_EQ(pad->GetDescription(), "A description past the small string buffer");

	GetOptions().Keymapper.RemoveRuntimeAction(expectedId);
	PadmapperRemoveRuntimeAction(expectedId);
}

TEST_F(LuaInputTest, Remove_UnbindsAndDeletes)
{
	ASSERT_TRUE(AddKey(Id(), FreeKey));

	GetOptions().Keymapper.RemoveRuntimeAction(Id());

	EXPECT_EQ(FindKeyAction(Id()), nullptr);
	EXPECT_EQ(GetOptions().Keymapper.findAction(FreeKey), nullptr);
	EXPECT_EQ(GetOptions().Keymapper.KeyForAction(Id()), SDLK_UNKNOWN);
}

TEST_F(LuaInputTest, Remove_LeavesVanillaActions)
{
	GetOptions().Keymapper.RemoveRuntimeAction("Help");
	PadmapperRemoveRuntimeAction("Help");
	GetOptions().Keymapper.RemoveRuntimeAction("NoSuchAction");
	PadmapperRemoveRuntimeAction("NoSuchAction");

	EXPECT_NE(FindKeyAction("Help"), nullptr);
	EXPECT_NE(FindPadAction("Help"), nullptr);
	EXPECT_EQ(GetOptions().Keymapper.KeyForAction("Help"), SDLK_F1);
}

TEST_F(LuaInputTest, Remove_LostKey_LeavesNewOwner)
{
	ASSERT_TRUE(AddKey(Id(), FreeKey));
	ASSERT_TRUE(FindKeyAction("Help")->SetValue(FreeKey));

	GetOptions().Keymapper.RemoveRuntimeAction(Id());

	EXPECT_EQ(GetOptions().Keymapper.findAction(FreeKey), FindKeyAction("Help"));
	ASSERT_TRUE(FindKeyAction("Help")->SetValue(SDLK_F1));
}

TEST_F(LuaInputTest, Remove_KeepsRebindingNotYetSaved)
{
	// Settings saves on exit, but toggling a mod there reloads all mods first.
	ASSERT_TRUE(AddKey(Id(), FreeKey));
	ASSERT_TRUE(FindKeyAction(Id())->SetValue(OtherFreeKey));

	GetOptions().Keymapper.RemoveRuntimeAction(Id());
	ASSERT_TRUE(AddKey(Id(), FreeKey));

	EXPECT_EQ(GetOptions().Keymapper.KeyForAction(Id()), OtherFreeKey);
}

// Padmapper actions.

TEST_F(LuaInputTest, AddRuntimeAction_AfterCommit_Padmapper_Added)
{
	const std::vector<OptionEntryBase *> before = GetOptions().Padmapper.GetEntries();

	ASSERT_TRUE(AddPad(Id(), FreeCombo));

	const std::vector<OptionEntryBase *> after = GetOptions().Padmapper.GetEntries();
	ASSERT_EQ(after.size(), before.size() + 1);
	EXPECT_TRUE(std::equal(before.begin(), before.end(), after.begin()));
	const OptionEntryBase &entry = *after.back();
	EXPECT_EQ(entry.key, Id());
	EXPECT_EQ(entry.GetName(), "Name");
	EXPECT_EQ(entry.GetDescription(), "Description");
	EXPECT_EQ(entry.GetType(), OptionEntryType::PadButton);
	EXPECT_TRUE(FindPadAction(Id())->isRuntime());
	EXPECT_FALSE(FindPadAction("Help")->isRuntime());
}

TEST_F(LuaInputTest, Padmapper_ExistingId_Refused)
{
	EXPECT_FALSE(AddPad("Help", ControllerButton_NONE));
	ASSERT_TRUE(AddPad(Id(), ControllerButton_NONE));
	EXPECT_FALSE(AddPad(Id(), ControllerButton_NONE));
	EXPECT_EQ(GetOptions().Padmapper.GetEntries().size(), padsBefore_.size() + 1);
}

TEST_F(LuaInputTest, Padmapper_DefaultAlreadyBound_LeftUnbound)
{
	ControllerButtonCombo taken;
	for (const PadmapperOptions::Action &action : GetOptions().Padmapper.actions) {
		if (action.boundInput.button != ControllerButton_NONE) {
			taken = action.boundInput;
			break;
		}
	}
	ASSERT_NE(taken.button, ControllerButton_NONE);

	ASSERT_TRUE(AddPad(Id(), taken));

	EXPECT_EQ(FindPadAction(Id())->boundInput.button, ControllerButton_NONE);
}

TEST_F(LuaInputTest, Padmapper_FreeDefault_Bound)
{
	ASSERT_TRUE(AddPad(Id(), FreeCombo));

	EXPECT_EQ(FindPadAction(Id())->boundInput.button, FreeCombo.button);
	EXPECT_EQ(FindPadAction(Id())->boundInput.modifier, FreeCombo.modifier);
}

TEST_F(LuaInputTest, Padmapper_IniBinding_Respected)
{
	SetIniValue("Padmapping", Id(), "LB+A");

	ASSERT_TRUE(AddPad(Id(), ControllerButton_NONE));

	EXPECT_EQ(FindPadAction(Id())->boundInput.button, ControllerButton_BUTTON_A);
	EXPECT_EQ(FindPadAction(Id())->boundInput.modifier, ControllerButton_BUTTON_LEFTSHOULDER);
}

TEST_F(LuaInputTest, Padmapper_Remove_KeepsRebindingNotYetSaved)
{
	ASSERT_TRUE(AddPad(Id(), ControllerButton_NONE));
	ASSERT_TRUE(FindPadAction(Id())->SetValue(FreeCombo));

	PadmapperRemoveRuntimeAction(Id());
	ASSERT_TRUE(AddPad(Id(), ControllerButton_NONE));

	EXPECT_EQ(FindPadAction(Id())->boundInput.button, FreeCombo.button);
	EXPECT_EQ(FindPadAction(Id())->boundInput.modifier, FreeCombo.modifier);
}

TEST_F(LuaInputTest, Remove_SafeWithActivePadPress)
{
	int releases = 0;
	ASSERT_TRUE(AddPad(Id(), FreeCombo, nullptr, [&releases] { releases++; }));
	PadmapperPress(FreeButton, *FindPadAction(Id()));
	ASSERT_TRUE(PadmapperIsActionActive(Id()));

	PadmapperRemoveRuntimeAction(Id());

	EXPECT_EQ(releases, 1);
	EXPECT_EQ(FindPadAction(Id()), nullptr);
	EXPECT_TRUE(PadmapperActionNameTriggeredByButtonEvent(ControllerButtonEvent { FreeButton, true }).empty());
	PadmapperRelease(FreeButton, /*invokeAction=*/true);
	EXPECT_EQ(releases, 1);
}

TEST_F(LuaInputTest, Remove_SafeWhenRebindWhilePressed)
{
	int releases = 0;
	ASSERT_TRUE(AddPad(Id(), FreeCombo, nullptr, [&releases] { releases++; }));
	PadmapperPress(FreeButton, *FindPadAction(Id()));
	ASSERT_TRUE(FindPadAction(Id())->SetValue(OtherFreeCombo));

	PadmapperRemoveRuntimeAction(Id());

	EXPECT_EQ(releases, 1);
	EXPECT_TRUE(PadmapperActionNameTriggeredByButtonEvent(ControllerButtonEvent { FreeButton, true }).empty());
}

TEST_F(LuaInputTest, Remove_LeavesOtherHeldButtons)
{
	int otherReleases = 0;
	ASSERT_TRUE(AddPad(Id("a"), FreeCombo));
	ASSERT_TRUE(AddPad(Id("b"), OtherFreeCombo, nullptr, [&otherReleases] { otherReleases++; }));
	PadmapperPress(FreeButton, *FindPadAction(Id("a")));
	PadmapperPress(OtherFreeCombo.button, *FindPadAction(Id("b")));

	PadmapperRemoveRuntimeAction(Id("a"));

	EXPECT_EQ(otherReleases, 0);
	EXPECT_TRUE(PadmapperIsActionActive(Id("b")));
	PadmapperRemoveRuntimeAction(Id("b"));
	EXPECT_EQ(otherReleases, 1);
}

// Lua API.

TEST_F(LuaInputTest, Lua_AddAction_Documented)
{
	const std::optional<sol::table> module = EvalAs<sol::table>(R"lua(return require("devilutionx.input"))lua");
	ASSERT_TRUE(module.has_value());
	EXPECT_TRUE(GetSignature(*module, "addAction").has_value());
	EXPECT_TRUE(GetDocstring(*module, "addAction").has_value());
}

TEST_F(LuaInputTest, Lua_AddAction_InBothMappers_PadUnbound)
{
	AddSpy(Id());

	const KeymapperOptions::Action *key = FindKeyAction(Id());
	ASSERT_NE(key, nullptr);
	EXPECT_EQ(key->GetName(), "Lua name");
	EXPECT_EQ(key->GetDescription(), "Lua description");
	EXPECT_EQ(GetOptions().Keymapper.KeyForAction(Id()), FreeKey);
	const PadmapperOptions::Action *pad = FindPadAction(Id());
	ASSERT_NE(pad, nullptr);
	EXPECT_EQ(pad->GetName(), "Lua name");
	EXPECT_EQ(pad->GetDescription(), "Lua description");
	EXPECT_EQ(pad->boundInput.button, ControllerButton_NONE);
}

TEST_F(LuaInputTest, Lua_NoDefaultKey_Unbound)
{
	EXPECT_EQ(AddError(StrCat("{ id = '", Id(), "', name = 'n', description = 'd' }")), "");

	EXPECT_NE(FindKeyAction(Id()), nullptr);
	EXPECT_EQ(GetOptions().Keymapper.KeyForAction(Id()), SDLK_UNKNOWN);
}

TEST_F(LuaInputTest, Lua_DefaultKeyTaken_LeftUnbound)
{
	AddSpy(Id(), "F1");

	EXPECT_EQ(GetOptions().Keymapper.KeyForAction(Id()), SDLK_UNKNOWN);
	EXPECT_EQ(GetOptions().Keymapper.findAction(SDLK_F1), FindKeyAction("Help"));
}

TEST_F(LuaInputTest, Lua_OnPress_OnRelease_Fire)
{
	AddSpy(Id());

	KeymapperPress(static_cast<SDL_Keycode>(FreeKey));
	EXPECT_EQ(Presses(), 1);
	EXPECT_EQ(Releases(), 0);

	KeymapperRelease(static_cast<SDL_Keycode>(FreeKey));
	EXPECT_EQ(Presses(), 1);
	EXPECT_EQ(Releases(), 1);
}

TEST_F(LuaInputTest, Lua_Pad_OnPress_OnRelease_Fire)
{
	AddSpy(Id());
	ASSERT_TRUE(FindPadAction(Id())->SetValue(FreeCombo));

	PadmapperPress(FreeButton, *FindPadAction(Id()));
	EXPECT_EQ(Presses(), 1);
	EXPECT_EQ(Releases(), 0);

	PadmapperRelease(FreeButton, /*invokeAction=*/true);
	EXPECT_EQ(Presses(), 1);
	EXPECT_EQ(Releases(), 1);
}

TEST_F(LuaInputTest, Lua_NoCallbacks_InputIsSafe)
{
	EXPECT_EQ(AddError(StrCat("{ id = '", Id(), "', name = 'n', description = 'd', defaultKey = 'W' }")), "");
	ASSERT_TRUE(FindPadAction(Id())->SetValue(FreeCombo));
	ScopedLogCapture log;

	KeymapperPress(static_cast<SDL_Keycode>(FreeKey));
	KeymapperRelease(static_cast<SDL_Keycode>(FreeKey));
	PadmapperPress(FreeButton, *FindPadAction(Id()));
	PadmapperRelease(FreeButton, /*invokeAction=*/true);

	EXPECT_EQ(log.text(), "");
}

TEST_F(LuaInputTest, Lua_Press_BlockedWhenDead_ReleaseStillFires)
{
	AddSpy(Id());
	ASSERT_TRUE(FindPadAction(Id())->SetValue(FreeCombo));
	MyPlayerIsDead = true;

	KeymapperPress(static_cast<SDL_Keycode>(FreeKey));
	EXPECT_EQ(Presses(), 0);
	KeymapperRelease(static_cast<SDL_Keycode>(FreeKey));
	EXPECT_EQ(Releases(), 1);

	PadmapperPress(FreeButton, *FindPadAction(Id()));
	EXPECT_EQ(Presses(), 0);
	PadmapperRelease(FreeButton, /*invokeAction=*/true);
	EXPECT_EQ(Releases(), 2);

	MyPlayerIsDead = false;
}

TEST_F(LuaInputTest, Lua_Press_BlockedWhilePaused_ReleaseStillFires)
{
	AddSpy(Id());
	PauseMode = 2;

	KeymapperPress(static_cast<SDL_Keycode>(FreeKey));
	KeymapperRelease(static_cast<SDL_Keycode>(FreeKey));

	PauseMode = 0;
	EXPECT_EQ(Presses(), 0);
	EXPECT_EQ(Releases(), 1);
}

TEST_F(LuaInputTest, Lua_DiedWhileHeld_ReleaseStillFires)
{
	AddSpy(Id());

	KeymapperPress(static_cast<SDL_Keycode>(FreeKey));
	MyPlayerIsDead = true;
	KeymapperRelease(static_cast<SDL_Keycode>(FreeKey));
	MyPlayerIsDead = false;

	EXPECT_EQ(Presses(), 1);
	EXPECT_EQ(Releases(), 1);
}

TEST_F(LuaInputTest, Lua_ChatOpenedWhileHeld_ReleaseStillFires)
{
	AddSpy(Id());

	KeymapperPress(static_cast<SDL_Keycode>(FreeKey));
	ChatFlag = true;
	KeymapperRelease(static_cast<SDL_Keycode>(FreeKey));
	ChatFlag = false;

	EXPECT_EQ(Releases(), 1);
}

TEST_F(LuaInputTest, Lua_GoldDialogOpenedWhileHeld_ReleaseStillFires)
{
	AddSpy(Id(), "H");
	ASSERT_EQ(GetOptions().Keymapper.findAction('0'), nullptr);
	ASSERT_TRUE(FindKeyAction(Id())->SetValue('0'));

	KeymapperPress(static_cast<SDL_Keycode>('0'));
	DropGoldFlag = true;
	KeymapperRelease(static_cast<SDL_Keycode>('0'));
	DropGoldFlag = false;

	EXPECT_EQ(Releases(), 1);
}

TEST_F(LuaInputTest, Vanilla_ReleaseStaysGatedWhileTyping)
{
	KeymapperOptions::Action &help = *FindKeyAction("Help");
	const std::function<void()> originalRelease = help.actionReleased;
	int releases = 0;
	help.actionReleased = [&releases] { releases++; };
	ASSERT_TRUE(help.SetValue(FreeKey));

	ChatFlag = true;
	KeymapperRelease(static_cast<SDL_Keycode>(FreeKey));
	ChatFlag = false;
	EXPECT_EQ(releases, 0);
	KeymapperRelease(static_cast<SDL_Keycode>(FreeKey), /*onlyRuntimeActions=*/true);
	EXPECT_EQ(releases, 0);
	KeymapperRelease(static_cast<SDL_Keycode>(FreeKey));
	EXPECT_EQ(releases, 1);

	help.actionReleased = originalRelease;
	ASSERT_TRUE(help.SetValue(SDLK_F1));
}

TEST_F(LuaInputTest, Lua_NetworkTimeoutWhileHeld_ReleaseStillFires)
{
	AddSpy(Id());

	KeymapperPress(static_cast<SDL_Keycode>(FreeKey));
	KeymapperRelease(static_cast<SDL_Keycode>(FreeKey), /*onlyRuntimeActions=*/true);

	EXPECT_EQ(Releases(), 1);
}

TEST_F(LuaInputTest, GameKeyRelease_ReachesVanillaAndRuntimeActions)
{
	KeymapperOptions::Action &help = *FindKeyAction("Help");
	const std::function<void()> originalRelease = help.actionReleased;
	int releases = 0;
	help.actionReleased = [&releases] { releases++; };
	AddSpy(Id());

	PrintScreen(SDLK_F1);
	PrintScreen(static_cast<SDL_Keycode>(FreeKey));

	help.actionReleased = originalRelease;
	EXPECT_EQ(releases, 1);
	EXPECT_EQ(Releases(), 1);
}

TEST_F(LuaInputTest, Lua_ErrorInCallback_Logged)
{
	ASSERT_EQ(AddError(StrCat(R"lua({
		id = ')lua",
	              Id(), R"lua(', name = 'n', description = 'd', defaultKey = 'W',
		onPress = function() error("press boom") end,
		onRelease = function() error("release boom") end,
	})lua")),
	    "");
	ScopedLogCapture log;

	KeymapperPress(static_cast<SDL_Keycode>(FreeKey));
	EXPECT_THAT(log.text(), HasSubstr("press boom"));
	KeymapperRelease(static_cast<SDL_Keycode>(FreeKey));
	EXPECT_THAT(log.text(), HasSubstr("release boom"));
}

TEST_F(LuaInputTest, Lua_ErrorWithoutMessage_Survives)
{
	ASSERT_EQ(AddError(StrCat("{ id = '", Id(), "', name = 'n', description = 'd', defaultKey = 'W', onPress = function() error({}) end }")), "");

	KeymapperPress(static_cast<SDL_Keycode>(FreeKey));

	EXPECT_NE(FindKeyAction(Id()), nullptr);
}

TEST_F(LuaInputTest, SameId_Replaces)
{
	AddSpy(Id());
	ASSERT_TRUE(FindKeyAction(Id())->SetValue(OtherFreeKey));
	const size_t count = GetOptions().Keymapper.GetEntries().size();
	ASSERT_EQ(AddError(StrCat(R"lua({
		id = ')lua",
	              Id(), R"lua(', name = 'Second', description = 'Second too', defaultKey = 'W',
		onPress = function() secondPresses = (secondPresses or 0) + 1 end,
	})lua")),
	    "");

	EXPECT_EQ(GetOptions().Keymapper.GetEntries().size(), count);
	EXPECT_EQ(FindKeyAction(Id())->GetName(), "Second");
	EXPECT_EQ(FindPadAction(Id())->GetName(), "Second");
	EXPECT_EQ(GetOptions().Keymapper.KeyForAction(Id()), OtherFreeKey);

	KeymapperPress(static_cast<SDL_Keycode>(OtherFreeKey));
	EXPECT_EQ(Presses(), 0);
	EXPECT_EQ(EvalAs<int>("return secondPresses"), 1);
}

TEST_F(LuaInputTest, SameId_WhilePadHeld_ReleasesOld)
{
	AddSpy(Id());
	ASSERT_TRUE(FindPadAction(Id())->SetValue(FreeCombo));
	PadmapperPress(FreeButton, *FindPadAction(Id()));

	ASSERT_EQ(AddError(StrCat("{ id = '", Id(), "', name = 'n', description = 'd' }")), "");

	EXPECT_EQ(Releases(), 1);
	EXPECT_TRUE(PadmapperActionNameTriggeredByButtonEvent(ControllerButtonEvent { FreeButton, true }).empty());
	PadmapperRelease(FreeButton, /*invokeAction=*/true);
	EXPECT_EQ(Releases(), 1);
}

TEST_F(LuaInputTest, BadSpec_Errors)
{
	const std::string good = "name = 'n', description = 'd'";
	const std::vector<std::string> specs = {
		"",
		"nil",
		"42",
		"'text'",
		StrCat("{ ", good, " }"),
		StrCat("{ id = 42, ", good, " }"),
		StrCat("{ id = '', ", good, " }"),
		StrCat("{ id = 'has space', ", good, " }"),
		StrCat("{ id = 'a=b', ", good, " }"),
		StrCat("{ id = '[a]', ", good, " }"),
		StrCat("{ id = 'a;b', ", good, " }"),
		StrCat("{ id = 'a\\nb', ", good, " }"),
		StrCat("{ id = 'Help', ", good, " }"),
		"{ id = 'test.bad', description = 'd' }",
		"{ id = 'test.bad', name = '', description = 'd' }",
		"{ id = 'test.bad', name = 42, description = 'd' }",
		"{ id = 'test.bad', name = 'n' }",
		"{ id = 'test.bad', name = 'n', description = 42 }",
		StrCat("{ id = 'test.bad', ", good, ", defaultKey = 42 }"),
		StrCat("{ id = 'test.bad', ", good, ", defaultKey = 'NoSuchKey' }"),
		StrCat("{ id = 'test.bad', ", good, ", defaultKey = 'w' }"),
		StrCat("{ id = 'test.bad', ", good, ", onPress = 42 }"),
		StrCat("{ id = 'test.bad', ", good, ", onPress = 'text' }"),
		StrCat("{ id = 'test.bad', ", good, ", onRelease = {} }"),
	};
	for (const std::string &spec : specs) {
		EXPECT_THAT(AddError(spec), HasSubstr("addAction: ")) << spec;
	}
	EXPECT_EQ(FindKeyAction("test.bad"), nullptr);
	EXPECT_EQ(FindPadAction("test.bad"), nullptr);
}

TEST_F(LuaInputTest, BadSpec_IdOfVanillaActionInOneMapperOnly)
{
	const std::vector<std::string> keys = KeysOf(GetOptions().Keymapper.GetEntries());
	const std::vector<std::string> pads = KeysOf(GetOptions().Padmapper.GetEntries());
	const auto onlyKey = std::find_if(keys.begin(), keys.end(), [&](const std::string &key) { return std::find(pads.begin(), pads.end(), key) == pads.end(); });
	const auto onlyPad = std::find_if(pads.begin(), pads.end(), [&](const std::string &pad) { return std::find(keys.begin(), keys.end(), pad) == keys.end(); });
	ASSERT_NE(onlyKey, keys.end());
	ASSERT_NE(onlyPad, pads.end());

	EXPECT_THAT(AddError(StrCat("{ id = '", *onlyKey, "', name = 'n', description = 'd' }")), HasSubstr("addAction: "));
	EXPECT_THAT(AddError(StrCat("{ id = '", *onlyPad, "', name = 'n', description = 'd' }")), HasSubstr("addAction: "));
}

TEST_F(LuaInputTest, BadSpec_MetatableNeverRuns)
{
	const std::string error = AddError(R"lua(setmetatable({}, { __index = function() error("trap") end }))lua");

	EXPECT_THAT(error, HasSubstr("addAction: "));
	EXPECT_THAT(error, Not(HasSubstr("trap")));
}

TEST_F(LuaInputTest, BadSpec_IdOfCppRuntimeAction)
{
	ASSERT_TRUE(AddKey(Id(), FreeKey));
	ASSERT_TRUE(AddPad(Id("pad"), ControllerButton_NONE));

	EXPECT_THAT(AddError(StrCat("{ id = '", Id(), "', name = 'n', description = 'd' }")), HasSubstr("addAction: "));
	EXPECT_THAT(AddError(StrCat("{ id = '", Id("pad"), "', name = 'n', description = 'd' }")), HasSubstr("addAction: "));

	EXPECT_EQ(FindKeyAction(Id())->GetName(), "Name");
	EXPECT_EQ(FindPadAction(Id("pad"))->GetName(), "Name");
	PadmapperRemoveRuntimeAction(Id("pad"));
}

TEST_F(LuaInputTest, BadSpec_KeepsExistingAction)
{
	AddSpy(Id());

	EXPECT_THAT(AddError(StrCat("{ id = '", Id(), "', name = 'n', description = 'd', defaultKey = 'NoSuchKey' }")), HasSubstr("addAction: "));

	EXPECT_EQ(FindKeyAction(Id())->GetName(), "Lua name");
	KeymapperPress(static_cast<SDL_Keycode>(FreeKey));
	EXPECT_EQ(Presses(), 1);
}

TEST_F(LuaInputTest, BadSpec_NamesTheCaller)
{
	EXPECT_THAT(AddError("42"), HasSubstr(":1: addAction: "));
}

TEST_F(LuaInputTest, AddAction_InsideCallback_Errors)
{
	// Replacing an action inside its own callback would free the code that is running.
	ASSERT_EQ(AddError(StrCat(R"lua({
		id = ')lua",
	              Id(), R"lua(', name = 'n', description = 'd', defaultKey = 'W',
		onPress = function()
			pressOk, pressError = pcall(require("devilutionx.input").addAction, { id = ')lua",
	              Id(), R"lua(', name = 'n2', description = 'd' })
		end,
		onRelease = function()
			releaseOk = pcall(require("devilutionx.input").addAction, { id = ')lua",
	              Id("other"), R"lua(', name = 'n2', description = 'd' })
		end,
	})lua")),
	    "");

	KeymapperPress(static_cast<SDL_Keycode>(FreeKey));
	KeymapperRelease(static_cast<SDL_Keycode>(FreeKey));

	EXPECT_EQ(EvalAs<bool>("return pressOk"), false);
	EXPECT_THAT(EvalAs<std::string>("return pressError").value_or(""), HasSubstr("addAction: "));
	EXPECT_EQ(EvalAs<bool>("return releaseOk"), false);
	EXPECT_EQ(FindKeyAction(Id())->GetName(), "n");
	EXPECT_EQ(FindKeyAction(Id("other")), nullptr);

	// A callback that raised does not leave the guard on.
	EXPECT_EQ(AddError(StrCat("{ id = '", Id(), "', name = 'n3', description = 'd' }")), "");
}

TEST_F(LuaInputTest, AddAction_FromCoroutine_OutlivesIt)
{
	const sol::protected_function_result result = Eval(StrCat(R"lua(
		presses = 0
		coroutine.wrap(function()
			require("devilutionx.input").addAction {
				id = ')lua",
	    Id(), R"lua(', name = 'n', description = 'd', defaultKey = 'W',
				onPress = function() presses = presses + 1 end,
			}
		end)()
	)lua"));
	ASSERT_TRUE(result.valid()) << result.get<sol::error>().what();

	// With the coroutine gone, a function that remembers its thread points at freed memory.
	GetLuaState().collect_garbage();
	GetLuaState().collect_garbage();
	Eval("local t = {} for i = 1, 20000 do t[i] = coroutine.create(function() end) end");

	KeymapperPress(static_cast<SDL_Keycode>(FreeKey));

	EXPECT_EQ(Presses(), 1);
}

TEST_F(LuaInputTest, Callback_KeptAliveWithoutOtherReferences)
{
	AddSpy(Id());

	GetLuaState().collect_garbage();
	GetLuaState().collect_garbage();
	KeymapperPress(static_cast<SDL_Keycode>(FreeKey));

	EXPECT_EQ(Presses(), 1);
}

TEST_F(LuaInputTest, ClearLuaActions_RemovesFromBothMappers)
{
	AddSpy(Id("a"));
	AddSpy(Id("b"), "H");

	ClearLuaActions();

	EXPECT_EQ(FindKeyAction(Id("a")), nullptr);
	EXPECT_EQ(FindKeyAction(Id("b")), nullptr);
	EXPECT_EQ(FindPadAction(Id("a")), nullptr);
	EXPECT_EQ(FindPadAction(Id("b")), nullptr);
	EXPECT_EQ(GetOptions().Keymapper.findAction(FreeKey), nullptr);
	EXPECT_EQ(GetOptions().Keymapper.findAction(OtherFreeKey), nullptr);
}

TEST_F(LuaInputTest, ClearLuaActions_LeavesCppRuntimeActions)
{
	ASSERT_TRUE(AddKey(Id(), FreeKey));
	AddSpy(Id("lua"), "H");

	ClearLuaActions();

	EXPECT_NE(FindKeyAction(Id()), nullptr);
}

TEST_F(LuaInputTest, ClearLuaActions_SecondCallIsHarmless)
{
	AddSpy(Id());
	ClearLuaActions();
	AddSpy(Id());

	ClearLuaActions();
	ClearLuaActions();

	EXPECT_EQ(FindKeyAction(Id()), nullptr);
}

TEST_F(LuaInputTest, ClearLuaActions_ForgetsIds)
{
	AddSpy(Id());
	ClearLuaActions();
	ASSERT_TRUE(AddKey(Id(), FreeKey));

	EXPECT_THAT(AddError(StrCat("{ id = '", Id(), "', name = 'n', description = 'd' }")), HasSubstr("addAction: "));

	EXPECT_EQ(FindKeyAction(Id())->GetName(), "Name");
}

TEST_F(LuaInputTest, ClearLuaActions_WhilePadHeld_ReleaseFiresOnce)
{
	AddSpy(Id());
	ASSERT_TRUE(FindPadAction(Id())->SetValue(FreeCombo));
	PadmapperPress(FreeButton, *FindPadAction(Id()));

	ClearLuaActions();

	EXPECT_EQ(Releases(), 1);
	PadmapperRelease(FreeButton, /*invokeAction=*/true);
	EXPECT_EQ(Releases(), 1);
}

TEST_F(LuaInputTest, Reload_ClearsActions)
{
	AddSpy(Id());
	ASSERT_TRUE(FindPadAction(Id())->SetValue(FreeCombo));
	PadmapperPress(FreeButton, *FindPadAction(Id()));

	LuaReloadActiveMods();

	EXPECT_EQ(Releases(), 1);
	EXPECT_EQ(FindKeyAction(Id()), nullptr);
	EXPECT_EQ(FindPadAction(Id()), nullptr);
	EXPECT_EQ(GetOptions().Keymapper.findAction(FreeKey), nullptr);
}

TEST_F(LuaInputTest, Reload_KeepsRebindingNotYetSaved)
{
	AddSpy(Id());
	ASSERT_TRUE(FindKeyAction(Id())->SetValue(OtherFreeKey));
	ASSERT_TRUE(FindPadAction(Id())->SetValue(FreeCombo));

	LuaReloadActiveMods();
	AddSpy(Id());

	EXPECT_EQ(GetOptions().Keymapper.KeyForAction(Id()), OtherFreeKey);
	EXPECT_EQ(FindPadAction(Id())->boundInput.button, FreeCombo.button);
}

TEST_F(LuaInputTest, Shutdown_ClearsActions)
{
	AddSpy(Id());
	ASSERT_TRUE(FindPadAction(Id())->SetValue(FreeCombo));
	PadmapperPress(FreeButton, *FindPadAction(Id()));

	RestartLua([this] {
		EXPECT_EQ(FindKeyAction(Id()), nullptr);
		EXPECT_EQ(FindPadAction(Id()), nullptr);
		EXPECT_EQ(GetOptions().Keymapper.findAction(FreeKey), nullptr);
		KeymapperPress(static_cast<SDL_Keycode>(FreeKey));
		KeymapperRelease(static_cast<SDL_Keycode>(FreeKey));
		PadmapperRelease(FreeButton, /*invokeAction=*/true);
	});

	AddSpy(Id());
	KeymapperPress(static_cast<SDL_Keycode>(FreeKey));
	EXPECT_EQ(Presses(), 1);
}

// Persistence.

TEST_F(LuaInputTest, Save_WritesBindingsToIni)
{
	AddSpy(Id());
	ASSERT_TRUE(FindPadAction(Id())->SetValue(FreeCombo));

	SaveOptions();

	const std::expected<Ini, std::string> ini = Ini::parse(ReadIniFile());
	ASSERT_TRUE(ini.has_value());
	EXPECT_EQ(ini->getString("Keymapping", Id()), "W");
	EXPECT_EQ(ini->getString("Padmapping", Id()), "A+B");
}

TEST_F(LuaInputTest, Save_ThenRestart_RestoresUserBinding)
{
	AddSpy(Id());
	ASSERT_TRUE(FindKeyAction(Id())->SetValue(OtherFreeKey));
	ASSERT_TRUE(FindPadAction(Id())->SetValue(FreeCombo));
	SaveOptions();
	ClearLuaActions();

	LoadOptions();
	AddSpy(Id());

	EXPECT_EQ(GetOptions().Keymapper.KeyForAction(Id()), OtherFreeKey);
	EXPECT_EQ(FindPadAction(Id())->boundInput.button, FreeCombo.button);
	EXPECT_EQ(FindPadAction(Id())->boundInput.modifier, FreeCombo.modifier);
}

TEST_F(LuaInputTest, ModDisabled_SavingKeepsBindingsAndVanillaIni)
{
	SaveOptions();
	const std::expected<Ini, std::string> vanilla = Ini::parse(ReadIniFile());
	ASSERT_TRUE(vanilla.has_value());

	// Next run: the mod is on and the user binds its action.
	AddSpy(Id());
	ASSERT_TRUE(FindKeyAction(Id())->SetValue(OtherFreeKey));
	ASSERT_TRUE(FindPadAction(Id())->SetValue(FreeCombo));
	SaveOptions();

	// Next run: the mod is off, the game still loads and saves the ini.
	ClearLuaActions();
	LoadOptions();
	SaveOptions();

	const std::expected<Ini, std::string> saved = Ini::parse(ReadIniFile());
	ASSERT_TRUE(saved.has_value());
	EXPECT_EQ(saved->getString("Keymapping", Id()), "H");
	EXPECT_EQ(saved->getString("Padmapping", Id()), "A+B");
	for (const std::string_view section : { "Keymapping", "Padmapping" }) {
		std::vector<std::string> keys = saved->getKeys(section);
		EXPECT_EQ(saved->get(section, Id()).size(), 1U);
		keys.erase(std::remove(keys.begin(), keys.end(), Id()), keys.end());
		const std::vector<std::string> vanillaKeys = vanilla->getKeys(section);
		std::vector<std::string> common;
		std::copy_if(keys.begin(), keys.end(), std::back_inserter(common), [&](const std::string &key) { return std::find(vanillaKeys.begin(), vanillaKeys.end(), key) != vanillaKeys.end(); });
		EXPECT_EQ(common, vanillaKeys);
		for (const std::string &key : vanillaKeys)
			EXPECT_EQ(saved->getString(section, key), vanilla->getString(section, key)) << key;
	}

	// The run after that: the mod is on again.
	AddSpy(Id());
	EXPECT_EQ(GetOptions().Keymapper.KeyForAction(Id()), OtherFreeKey);
	EXPECT_EQ(FindPadAction(Id())->boundInput.button, FreeCombo.button);
}

TEST_F(LuaInputTest, ModDisabled_KeyGivenAwayMeanwhile_NewOwnerKeepsIt)
{
	AddSpy(Id());
	SaveOptions();
	ClearLuaActions();
	ASSERT_TRUE(FindKeyAction("Help")->SetValue(FreeKey));
	SaveOptions();

	AddSpy(Id());

	EXPECT_EQ(GetOptions().Keymapper.findAction(FreeKey), FindKeyAction("Help"));
	EXPECT_EQ(GetOptions().Keymapper.KeyForAction(Id()), SDLK_UNKNOWN);
	ASSERT_TRUE(FindKeyAction("Help")->SetValue(SDLK_F1));
}

TEST_F(LuaInputTest, VanillaOrder_Unaffected)
{
	const std::vector<std::string> keysBefore = KeysOf(GetOptions().Keymapper.GetEntries());
	const std::vector<std::string> padsBefore = KeysOf(GetOptions().Padmapper.GetEntries());

	AddSpy(Id("a"));
	AddSpy(Id("b"), "H");
	AddSpy(Id("a"));

	std::vector<std::string> keys = KeysOf(GetOptions().Keymapper.GetEntries());
	std::vector<std::string> pads = KeysOf(GetOptions().Padmapper.GetEntries());
	keys.resize(keysBefore.size());
	pads.resize(padsBefore.size());
	EXPECT_EQ(keys, keysBefore);
	EXPECT_EQ(pads, padsBefore);
}

} // namespace
} // namespace devilution
