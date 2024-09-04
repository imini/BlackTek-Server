// Copyright 2024 Black Tek Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#ifndef FS_TOOLS_H
#define FS_TOOLS_H

#include <random>
#include <string_view>

#include "position.h"
#include "const.h"
#include "enums.h"

void printXMLError(const std::string& where, const std::string& fileName, const pugi::xml_parse_result& result);

const std::vector<Direction>& getShuffleDirections();

std::string transformToSHA1(std::string_view input);
std::string generateToken(const std::string& key, uint32_t ticks);

void replaceString(std::string& str, const std::string& sought, const std::string& replacement);
void trim_right(std::string& source, char t);
void trim_left(std::string& source, char t);
void toLowerCaseString(std::string& source);
std::string asLowerCaseString(std::string source);
std::string asUpperCaseString(std::string source);

// checks that str1 is equivalent to str2 ignoring letter case
bool caseInsensitiveEqual(std::string_view str1, std::string_view str2);
// checks that str1 starts with str2 ignoring letter case
bool caseInsensitiveStartsWith(std::string_view str, std::string_view prefix);

using StringVector = std::vector<std::string>;
using IntegerVector = std::vector<int32_t>;

std::vector<std::string_view> explodeString(std::string_view inString, const std::string& separator, int32_t limit = -1);
IntegerVector vectorAtoi(const std::vector<std::string_view>& stringVector);
constexpr bool hasBitSet(uint32_t flag, uint32_t flags) {
	return (flags & flag) != 0;
}

std::mt19937& getRandomGenerator();
int32_t uniform_random(int32_t minNumber, int32_t maxNumber);
int32_t normal_random(int32_t minNumber, int32_t maxNumber);
bool boolean_random(double probability = 0.5);

Direction getDirection(const std::string& string);
Position getNextPosition(Direction direction, Position pos);
Direction getDirectionTo(const Position& from, const Position& to);

std::string getFirstLine(const std::string& str);

std::string formatDate(time_t time);
std::string formatDateShort(time_t time);
std::string convertIPToString(uint32_t ip);

void trimString(std::string& str);

uint16_t getDepotBoxId(uint16_t index);
MagicEffectClasses getMagicEffect(const std::string& strValue);
ShootType_t getShootType(const std::string& strValue);
Ammo_t getAmmoType(const std::string& strValue);
WeaponAction_t getWeaponAction(const std::string& strValue);
Skulls_t getSkullType(const std::string& strValue);
std::string getCombatName(CombatType_t combatType);

std::string getSpecialSkillName(uint8_t skillid);
std::string getSkillName(uint8_t skillid);
std::string getStatName(uint8_t id);

uint32_t adlerChecksum(const uint8_t* data, size_t length);

std::string ucfirst(std::string str);
std::string ucwords(std::string str);
bool booleanString(std::string_view str);

std::string getWeaponName(WeaponType_t weaponType);

size_t combatTypeToIndex(CombatType_t combatType);
CombatType_t indexToCombatType(size_t v);

uint8_t serverFluidToClient(uint8_t serverFluid);
uint8_t clientFluidToServer(uint8_t clientFluid);

itemAttrTypes stringToItemAttribute(const std::string& str);

const char* getReturnMessage(ReturnValue value);

int64_t OTSYS_TIME();

SpellGroup_t stringToSpellGroup(const std::string& value);

static uint8_t getCombatU8Value(CombatType_t combatType)
{
	switch (combatType) {
	case COMBAT_NONE:
		return 0;
	case COMBAT_PHYSICALDAMAGE:
		return 1;
	case COMBAT_ENERGYDAMAGE:
		return 2;
	case COMBAT_EARTHDAMAGE:
		return 3;
	case COMBAT_FIREDAMAGE:
		return 4;
	case COMBAT_UNDEFINEDDAMAGE:
		return 5;
	case COMBAT_LIFEDRAIN:
		return 6;
	case COMBAT_MANADRAIN:
		return 7;
	case COMBAT_HEALING:
		return 8;
	case COMBAT_DROWNDAMAGE:
		return 9;
	case COMBAT_ICEDAMAGE:
		return 10;
	case COMBAT_HOLYDAMAGE:
		return 11;
	case COMBAT_DEATHDAMAGE:
		return 12;
	default:
		std::cout << "Invalid Damage Conversion\n";
	}
	return 0;
}

static CombatType_t getCombatTypeFromU8(uint8_t value)
{
	switch (value) {
	case 0:
		return COMBAT_NONE;
	case 1:
		return COMBAT_PHYSICALDAMAGE;
	case 2:
		return COMBAT_ENERGYDAMAGE;
	case 3:
		return COMBAT_EARTHDAMAGE;
	case 4:
		return COMBAT_FIREDAMAGE;
	case 5:
		return COMBAT_UNDEFINEDDAMAGE;
	case 6:
		return COMBAT_LIFEDRAIN;
	case 7:
		return COMBAT_MANADRAIN;
	case 8:
		return COMBAT_HEALING;
	case 9:
		return COMBAT_DROWNDAMAGE;
	case 10:
		return COMBAT_ICEDAMAGE;
	case 11:
		return COMBAT_HOLYDAMAGE;
	case 12:
		return COMBAT_DEATHDAMAGE;
	default:
		std::cout << "Invalid u8 value for CombatType_t conversion\n";
	}
	return COMBAT_NONE;
}

namespace Titan {

#if __has_cpp_attribute(__cpp_lib_to_underlying)

	inline constexpr auto to_underlying(auto e) noexcept { return std::to_underlying(e); }

#else

	inline constexpr auto to_underlying(auto e) noexcept { return static_cast<std::underlying_type_t<decltype(e)>>(e); }

#endif

} // namespace Titan

#endif
