// Copyright 2024 Black Tek Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#include "otpch.h"

#include <boost/range/adaptor/reversed.hpp>
#include <fmt/format.h>

#include "matrixarea.h"
#include "luascript.h"
#include "chat.h"
#include "player.h"
#include "game.h"
#include "protocolstatus.h"
#include "spells.h"
#include "iologindata.h"
#include "iomapserialize.h"
#include "configmanager.h"
#include "teleport.h"
#include "databasemanager.h"
#include "bed.h"
#include "monster.h"
#include "scheduler.h"
#include "databasetasks.h"
#include "events.h"
#include "movement.h"
#include "globalevent.h"
#include "script.h"
#include "weapons.h"
#include "luavariant.h"
#include "augments.h"
#include "zones.h"

extern Chat* g_chat;
extern Game g_game;
extern Monsters g_monsters;
extern ConfigManager g_config;
extern Vocations g_vocations;
extern Spells* g_spells;
extern Events* g_events;
extern Actions* g_actions;
extern TalkActions* g_talkActions;
extern CreatureEvents* g_creatureEvents;
extern MoveEvents* g_moveEvents;
extern GlobalEvents* g_globalEvents;
extern Scripts* g_scripts;
extern Weapons* g_weapons;

ScriptEnvironment::DBResultMap ScriptEnvironment::tempResults;
uint32_t ScriptEnvironment::lastResultId = 0;

std::multimap<ScriptEnvironment*, ItemPtr> ScriptEnvironment::tempItems;

LuaEnvironment g_luaEnvironment;

ScriptEnvironment::ScriptEnvironment()
{
	resetEnv();
}

ScriptEnvironment::~ScriptEnvironment()
{
	resetEnv();
}

void ScriptEnvironment::resetEnv()
{
	scriptId = 0;
	callbackId = 0;
	timerEvent = false;
	interface = nullptr;
	localMap.clear();
	tempResults.clear();

	auto pair = tempItems.equal_range(this);
	auto it = pair.first;
	while (it != pair.second) {
		auto item = it->second;
		if (item && item->getParent() == VirtualCylinder::virtualCylinder) {
			// g_game.ReleaseItem(item);
		}
		it = tempItems.erase(it);
	}
}

bool ScriptEnvironment::setCallbackId(int32_t callbackId, LuaScriptInterface* scriptInterface)
{
	if (this->callbackId != 0) {
		//nested callbacks are not allowed
		if (interface) {
			reportErrorFunc(interface->getLuaState(), "Nested callbacks!");
		}
		return false;
	}

	this->callbackId = callbackId;
	interface = scriptInterface;
	return true;
}

void ScriptEnvironment::getEventInfo(int32_t& scriptId, LuaScriptInterface*& scriptInterface, int32_t& callbackId, bool& timerEvent) const
{
	scriptId = this->scriptId;
	scriptInterface = interface;
	callbackId = this->callbackId;
	timerEvent = this->timerEvent;
}

uint32_t ScriptEnvironment::addThing(const ThingPtr& thing)
{
	if (!thing || thing->isRemoved()) {
		return 0;
	}

	if (const auto creature = thing->getCreature()) {
		return creature->getID();
	}

	const auto item = thing->getItem();
	if (item && item->hasAttribute(ITEM_ATTRIBUTE_UNIQUEID)) {
		return item->getUniqueId();
	}

	for (const auto& it : localMap) {
		if (it.second == item) {
			return it.first;
		}
	}

	localMap[++lastUID] = item;
	return lastUID;
}

void ScriptEnvironment::insertItem(uint32_t uid, const ItemPtr& item)
{
	auto result = localMap.emplace(uid, item);
	if (!result.second) {
		std::cout << std::endl << "Lua Script Error: Thing uid already taken.";
	}
}

ThingPtr ScriptEnvironment::getThingByUID(const uint32_t uid)
{
	if (uid >= 0x10000000) {
		return g_game.getCreatureByID(uid);
	}

	if (uid <= std::numeric_limits<uint16_t>::max()) {
		auto item = g_game.getUniqueItem(uid);
		if (item && !item->isRemoved()) {
			return item;
		}
		return nullptr;
	}

	if (const auto it = localMap.find(uid); it != localMap.end()) {
		auto item = it->second;
		if (!item->isRemoved()) {
			return item;
		}
	}
	return nullptr;
}

ItemPtr ScriptEnvironment::getItemByUID(const uint32_t uid)
{
	const auto thing = getThingByUID(uid);
	if (!thing) {
		return nullptr;
	}
	return thing->getItem();
}

ContainerPtr ScriptEnvironment::getContainerByUID(const uint32_t uid)
{
	const auto item = getItemByUID(uid);
	if (!item) {
		return nullptr;
	}
	return item->getContainer();
}

void ScriptEnvironment::removeItemByUID(const uint32_t uid)
{
	if (uid <= std::numeric_limits<uint16_t>::max()) {
		g_game.removeUniqueItem(uid);
		return;
	}

	if (auto it = localMap.find(uid); it != localMap.end()) {
		localMap.erase(it);
	}
}

void ScriptEnvironment::addTempItem(const ItemPtr& item)
{
	tempItems.emplace(this, item);
}

void ScriptEnvironment::removeTempItem(const ItemPtr& item)
{
	for (auto it = tempItems.begin(), end = tempItems.end(); it != end; ++it) {
		if (it->second == item) {
			tempItems.erase(it);
			break;
		}
	}
}

uint32_t ScriptEnvironment::addResult(const DBResult_ptr& res)
{
	tempResults[++lastResultId] = res;
	return lastResultId;
}

bool ScriptEnvironment::removeResult(uint32_t id)
{
	auto it = tempResults.find(id);
	if (it == tempResults.end()) {
		return false;
	}

	tempResults.erase(it);
	return true;
}

DBResult_ptr ScriptEnvironment::getResultByID(uint32_t id)
{
	auto it = tempResults.find(id);
	if (it == tempResults.end()) {
		return nullptr;
	}
	return it->second;
}

std::string LuaScriptInterface::getErrorDesc(ErrorCode_t code)
{
	switch (code) {
		case LUA_ERROR_PLAYER_NOT_FOUND: return "Player not found";
		case LUA_ERROR_CREATURE_NOT_FOUND: return "Creature not found";
		case LUA_ERROR_ITEM_NOT_FOUND: return "Item not found";
		case LUA_ERROR_THING_NOT_FOUND: return "Thing not found";
		case LUA_ERROR_TILE_NOT_FOUND: return "Tile not found";
		case LUA_ERROR_HOUSE_NOT_FOUND: return "House not found";
		case LUA_ERROR_COMBAT_NOT_FOUND: return "Combat not found";
		case LUA_ERROR_CONDITION_NOT_FOUND: return "Condition not found";
		case LUA_ERROR_AREA_NOT_FOUND: return "Area not found";
		case LUA_ERROR_CONTAINER_NOT_FOUND: return "Container not found";
		case LUA_ERROR_VARIANT_NOT_FOUND: return "Variant not found";
		case LUA_ERROR_VARIANT_UNKNOWN: return "Unknown variant type";
		case LUA_ERROR_SPELL_NOT_FOUND: return "Spell not found";
		default: return "Bad error code";
	}
}

ScriptEnvironment LuaScriptInterface::scriptEnv[16];
int32_t LuaScriptInterface::scriptEnvIndex = -1;

LuaScriptInterface::LuaScriptInterface(std::string interfaceName) : interfaceName(std::move(interfaceName))
{
	if (!g_luaEnvironment.getLuaState()) {
		g_luaEnvironment.initState();
	}
}

LuaScriptInterface::~LuaScriptInterface()
{
	closeState();
}

bool LuaScriptInterface::reInitState()
{
	g_luaEnvironment.clearCombatObjects(this);
	g_luaEnvironment.clearAreaObjects(this);

	closeState();
	return initState();
}

/// Same as lua_pcall, but adds stack trace to error strings in called function.
int LuaScriptInterface::protectedCall(lua_State* L, int nargs, int nresults)
{
	int error_index = lua_gettop(L) - nargs;
	lua_pushcfunction(L, luaErrorHandler);
	lua_insert(L, error_index);

	int ret = lua_pcall(L, nargs, nresults, error_index);
	lua_remove(L, error_index);
	return ret;
}

int32_t LuaScriptInterface::loadFile(const std::string& file, NpcPtr npc /* = std::nullopt*/)
{
	//loads file as a chunk at stack top
	int ret = luaL_loadfile(luaState, file.c_str());
	if (ret != 0) {
		lastLuaError = popString(luaState);
		return -1;
	}

	//check that it is loaded as a function
	if (!isFunction(luaState, -1)) {
		lua_pop(luaState, 1);
		return -1;
	}

	loadingFile = file;

	if (!reserveScriptEnv()) {
		lua_pop(luaState, 1);
		return -1;
	}

	ScriptEnvironment* env = getScriptEnv();
	env->setScriptId(EVENT_ID_LOADING, this);
	env->setNpc(npc);

	//execute it
	ret = protectedCall(luaState, 0, 0);
	if (ret != 0) {
		reportError(nullptr, popString(luaState));
		resetScriptEnv();
		return -1;
	}

	resetScriptEnv();
	return 0;
}

int32_t LuaScriptInterface::getEvent(std::string_view eventName)
{
	//get our events table
	lua_rawgeti(luaState, LUA_REGISTRYINDEX, eventTableRef);
	if (!isTable(luaState, -1)) {
		lua_pop(luaState, 1);
		return -1;
	}

	//get current event function pointer
	lua_getglobal(luaState, eventName.data());
	if (!isFunction(luaState, -1)) {
		lua_pop(luaState, 2);
		return -1;
	}

	//save in our events table
	lua_pushvalue(luaState, -1);
	lua_rawseti(luaState, -3, runningEventId);
	lua_pop(luaState, 2);

	//reset global value of this event
	lua_pushnil(luaState);
	lua_setglobal(luaState, eventName.data());

	cacheFiles[runningEventId] = fmt::format("{:s}:{:s}", loadingFile, eventName);
	return runningEventId++;
}

int32_t LuaScriptInterface::getEvent()
{
	//check if function is on the stack
	if (!isFunction(luaState, -1)) {
		return -1;
	}

	//get our events table
	lua_rawgeti(luaState, LUA_REGISTRYINDEX, eventTableRef);
	if (!isTable(luaState, -1)) {
		lua_pop(luaState, 1);
		return -1;
	}

	//save in our events table
	lua_pushvalue(luaState, -2);
	lua_rawseti(luaState, -2, runningEventId);
	lua_pop(luaState, 2);

	cacheFiles[runningEventId] = loadingFile + ":callback";
	return runningEventId++;
}

int32_t LuaScriptInterface::getMetaEvent(const std::string& globalName, const std::string& eventName)
{
	//get our events table
	lua_rawgeti(luaState, LUA_REGISTRYINDEX, eventTableRef);
	if (!isTable(luaState, -1)) {
		lua_pop(luaState, 1);
		return -1;
	}

	//get current event function pointer
	lua_getglobal(luaState, globalName.c_str());
	lua_getfield(luaState, -1, eventName.c_str());
	if (!isFunction(luaState, -1)) {
		lua_pop(luaState, 3);
		return -1;
	}

	//save in our events table
	lua_pushvalue(luaState, -1);
	lua_rawseti(luaState, -4, runningEventId);
	lua_pop(luaState, 1);

	//reset global value of this event
	lua_pushnil(luaState);
	lua_setfield(luaState, -2, eventName.c_str());
	lua_pop(luaState, 2);

	cacheFiles[runningEventId] = loadingFile + ":" + globalName + "@" + eventName;
	return runningEventId++;
}

const std::string& LuaScriptInterface::getFileById(int32_t scriptId)
{
	if (scriptId == EVENT_ID_LOADING) {
		return loadingFile;
	}

	auto it = cacheFiles.find(scriptId);
	if (it == cacheFiles.end()) {
		static const std::string& unk = "(Unknown scriptfile)";
		return unk;
	}
	return it->second;
}

std::string LuaScriptInterface::getStackTrace(lua_State* L, const std::string& error_desc)
{
	luaL_traceback(L, L, error_desc.c_str(), 1);
	return popString(L);
}

void LuaScriptInterface::reportError(const char* function, const std::string& error_desc, lua_State* L /*= nullptr*/, bool stack_trace /*= false*/)
{
	int32_t scriptId;
	int32_t callbackId;
	bool timerEvent;
	LuaScriptInterface* scriptInterface;
	getScriptEnv()->getEventInfo(scriptId, scriptInterface, callbackId, timerEvent);

	std::cout << std::endl << "Lua Script Error: ";

	if (scriptInterface) {
		std::cout << '[' << scriptInterface->getInterfaceName() << "] " << std::endl;

		if (timerEvent) {
			std::cout << "in a timer event called from: " << std::endl;
		}

		if (callbackId) {
			std::cout << "in callback: " << scriptInterface->getFileById(callbackId) << std::endl;
		}

		std::cout << scriptInterface->getFileById(scriptId) << std::endl;
	}

	if (function) {
		std::cout << function << "(). ";
	}

	if (L && stack_trace) {
		std::cout << getStackTrace(L, error_desc) << std::endl;
	} else {
		std::cout << error_desc << std::endl;
	}
}

bool LuaScriptInterface::pushFunction(int32_t functionId) const
{
	lua_rawgeti(luaState, LUA_REGISTRYINDEX, eventTableRef);
	if (!isTable(luaState, -1)) {
		return false;
	}

	lua_rawgeti(luaState, -1, functionId);
	lua_replace(luaState, -2);
	return isFunction(luaState, -1);
}

bool LuaScriptInterface::initState()
{
	luaState = g_luaEnvironment.getLuaState();
	if (!luaState) {
		return false;
	}

	lua_newtable(luaState);
	eventTableRef = luaL_ref(luaState, LUA_REGISTRYINDEX);
	runningEventId = EVENT_ID_USER;
	return true;
}

bool LuaScriptInterface::closeState()
{
	if (!g_luaEnvironment.getLuaState() || !luaState) {
		return false;
	}

	cacheFiles.clear();
	if (eventTableRef != -1) {
		luaL_unref(luaState, LUA_REGISTRYINDEX, eventTableRef);
		eventTableRef = -1;
	}

	luaState = nullptr;
	return true;
}

int LuaScriptInterface::luaErrorHandler(lua_State* L)
{
	const std::string& errorMessage = popString(L);
	pushString(L, LuaScriptInterface::getStackTrace(L, errorMessage));
	return 1;
}

bool LuaScriptInterface::callFunction(int params) const
{
	bool result = false;
	int size = lua_gettop(luaState);
	if (protectedCall(luaState, params, 1) != 0) {
		LuaScriptInterface::reportError(nullptr, LuaScriptInterface::getString(luaState, -1));
	} else {
		result = LuaScriptInterface::getBoolean(luaState, -1);
	}

	lua_pop(luaState, 1);
	if ((lua_gettop(luaState) + params + 1) != size) {
		LuaScriptInterface::reportError(nullptr, "Stack size changed!");
	}

	resetScriptEnv();
	return result;
}

void LuaScriptInterface::callVoidFunction(int params) const
{
	int size = lua_gettop(luaState);
	if (protectedCall(luaState, params, 0) != 0) {
		LuaScriptInterface::reportError(nullptr, LuaScriptInterface::popString(luaState));
	}

	if ((lua_gettop(luaState) + params + 1) != size) {
		LuaScriptInterface::reportError(nullptr, "Stack size changed!");
	}

	resetScriptEnv();
}

void LuaScriptInterface::pushVariant(lua_State* L, const LuaVariant& var)
{
	lua_createtable(L, 0, 2);
	setField(L, "type", var.type());
	switch (var.type()) {
		case VARIANT_NUMBER:{
			setField(L, "number", var.getNumber());
			break;
		}
		case VARIANT_STRING: {
			setField(L, "string", var.getString());
			break;
		}
		case VARIANT_TARGETPOSITION: {
			pushPosition(L, var.getTargetPosition());
			lua_setfield(L, -2, "pos");
			break;
		}
		case VARIANT_POSITION: {
			pushPosition(L, var.getPosition());
			lua_setfield(L, -2, "pos");
			break;
		}
		default:
			break;
	}
	setMetatable(L, -1, "Variant");
}

void LuaScriptInterface::pushThing(lua_State* L, const ThingPtr& thing)
{
	if (!thing) {
		lua_createtable(L, 0, 4);
		setField(L, "uid", 0);
		setField(L, "itemid", 0);
		setField(L, "actionid", 0);
		setField(L, "type", 0);
		return;
	}

	if (auto item = thing->getItem()) {
		pushSharedPtr(L, item);
		setItemMetatable(L, -1, item);
	} else if (auto creature = thing->getCreature()) {
		pushSharedPtr(L, creature);
		setCreatureMetatable(L, -1, creature);
	} else {
		lua_pushnil(L);
	}
}

void LuaScriptInterface::pushCylinder(lua_State* L, const CylinderPtr& cylinder)
{
	if (auto creature = cylinder->getCreature()) {
		pushSharedPtr(L, creature);
		setCreatureMetatable(L, -1, creature);
	} else if (auto parentItem = cylinder->getItem()) {
		pushSharedPtr(L, parentItem);
		setItemMetatable(L, -1, parentItem);
	} else if (auto tile = cylinder->getTile()) {
		pushSharedPtr(L, tile);
		setMetatable(L, -1, "Tile");
	} else if (cylinder == VirtualCylinder::virtualCylinder) {
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
}

void LuaScriptInterface::pushString(lua_State* L, std::string_view value)
{
	lua_pushlstring(L, value.data(), value.length());
}

void LuaScriptInterface::pushCallback(lua_State* L, int32_t callback)
{
	lua_rawgeti(L, LUA_REGISTRYINDEX, callback);
}

std::string LuaScriptInterface::popString(lua_State* L)
{
	if (lua_gettop(L) == 0) {
		return std::string();
	}

	std::string str(getString(L, -1));
	lua_pop(L, 1);
	return str;
}

int32_t LuaScriptInterface::popCallback(lua_State* L)
{
	return luaL_ref(L, LUA_REGISTRYINDEX);
}

// Metatables
void LuaScriptInterface::setMetatable(lua_State* L, int32_t index, const std::string& name)
{
	luaL_getmetatable(L, name.c_str());
	lua_setmetatable(L, index - 1);
}

void LuaScriptInterface::setWeakMetatable(lua_State* L, int32_t index, const std::string& name)
{
	static std::set<std::string> weakObjectTypes;
	const std::string& weakName = name + "_weak";

	auto result = weakObjectTypes.emplace(name);
	if (result.second) {
		luaL_getmetatable(L, name.c_str());
		int childMetatable = lua_gettop(L);

		luaL_newmetatable(L, weakName.c_str());
		int metatable = lua_gettop(L);

		static const std::vector<std::string> methodKeys = {"__index", "__metatable", "__eq"};
		for (const std::string& metaKey : methodKeys) {
			lua_getfield(L, childMetatable, metaKey.c_str());
			lua_setfield(L, metatable, metaKey.c_str());
		}

		static const std::vector<int> methodIndexes = {'h', 'p', 't'};
		for (int metaIndex : methodIndexes) {
			lua_rawgeti(L, childMetatable, metaIndex);
			lua_rawseti(L, metatable, metaIndex);
		}

		lua_pushnil(L);
		lua_setfield(L, metatable, "__gc");

		lua_remove(L, childMetatable);
	} else {
		luaL_getmetatable(L, weakName.c_str());
	}
	lua_setmetatable(L, index - 1);
}

void LuaScriptInterface::setItemMetatable(lua_State* L, int32_t index, const ItemConstPtr& item)
{
	if (item->getContainer()) {
		luaL_getmetatable(L, "Container");
	} else if (item->getTeleport()) {
		luaL_getmetatable(L, "Teleport");
	} else {
		luaL_getmetatable(L, "Item");
	}
	lua_setmetatable(L, index - 1);
}

void LuaScriptInterface::setCreatureMetatable(lua_State* L, int32_t index, const CreatureConstPtr& creature)
{
	if (creature->getPlayer()) {
		luaL_getmetatable(L, "Player");
	} else if (creature->getMonster()) {
		luaL_getmetatable(L, "Monster");
	} else {
		luaL_getmetatable(L, "Npc");
	}
	lua_setmetatable(L, index - 1);
}

// Get
std::string LuaScriptInterface::getString(lua_State* L, int32_t arg)
{
	size_t len;
	const char* c_str = lua_tolstring(L, arg, &len);
	if (!c_str || len == 0) {
		return std::string();
	}
	return std::string(c_str, len);
}

std::string LuaScriptInterface::getString(lua_State* L, int32_t arg, const std::string& fallback)
{
	size_t len;
	const char* c_str = lua_tolstring(L, arg, &len);
	if (!c_str || len == 0) {
		return fallback;
	}
	return std::string(c_str, len);
}

Position LuaScriptInterface::getPosition(lua_State* L, int32_t arg, int32_t& stackpos)
{
	Position position;
	position.x = getField<uint16_t>(L, arg, "x");
	position.y = getField<uint16_t>(L, arg, "y");
	position.z = getField<uint8_t>(L, arg, "z");

	lua_getfield(L, arg, "stackpos");
	if (lua_isnil(L, -1) == 1) {
		stackpos = 0;
	} else {
		stackpos = getNumber<int32_t>(L, -1);
	}

	lua_pop(L, 4);
	return position;
}

Position LuaScriptInterface::getPosition(lua_State* L, int32_t arg)
{
	Position position;
	position.x = getField<uint16_t>(L, arg, "x");
	position.y = getField<uint16_t>(L, arg, "y");
	position.z = getField<uint8_t>(L, arg, "z");

	lua_pop(L, 3);
	return position;
}

Outfit_t LuaScriptInterface::getOutfit(lua_State* L, int32_t arg)
{
	Outfit_t outfit;
	outfit.lookMount = getField<uint16_t>(L, arg, "lookMount");
	outfit.lookAddons = getField<uint8_t>(L, arg, "lookAddons");

	outfit.lookFeet = getField<uint8_t>(L, arg, "lookFeet");
	outfit.lookLegs = getField<uint8_t>(L, arg, "lookLegs");
	outfit.lookBody = getField<uint8_t>(L, arg, "lookBody");
	outfit.lookHead = getField<uint8_t>(L, arg, "lookHead");

	outfit.lookTypeEx = getField<uint16_t>(L, arg, "lookTypeEx");
	outfit.lookType = getField<uint16_t>(L, arg, "lookType");

	lua_pop(L, 8);
	return outfit;
}

Outfit LuaScriptInterface::getOutfitClass(lua_State* L, int32_t arg)
{
	uint16_t lookType = getField<uint16_t>(L, arg, "lookType");
	const std::string& name = getFieldString(L, arg, "name");
	bool premium = getField<uint8_t>(L, arg, "premium") == 1;
	bool unlocked = getField<uint8_t>(L, arg, "unlocked") == 1;
	lua_pop(L, 4);
	return Outfit(name, lookType, premium, unlocked);
}

static LuaVariant getVariant(lua_State* L, int32_t arg)
{
	LuaVariant var;
	switch (LuaScriptInterface::getField<LuaVariantType_t>(L, arg, "type")) {
		case VARIANT_NUMBER: {
			var.setNumber(LuaScriptInterface::getField<uint32_t>(L, arg, "number"));
			lua_pop(L, 2);
			break;
		}

		case VARIANT_STRING: {
			var.setString(LuaScriptInterface::getFieldString(L, arg, "string"));
			lua_pop(L, 2);
			break;
		}

		case VARIANT_POSITION: {
			lua_getfield(L, arg, "pos");
			var.setPosition(LuaScriptInterface::getPosition(L, lua_gettop(L)));
			lua_pop(L, 2);
			break;
		}

		case VARIANT_TARGETPOSITION: {
			lua_getfield(L, arg, "pos");
			var.setTargetPosition(LuaScriptInterface::getPosition(L, lua_gettop(L)));
			lua_pop(L, 2);
			break;
		}

		default: {
			var = {};
			lua_pop(L, 1);
			break;
		}
	}
	return var;
}

InstantSpell* LuaScriptInterface::getInstantSpell(lua_State* L, int32_t arg)
{
	InstantSpell* spell = g_spells->getInstantSpellByName(getFieldString(L, arg, "name"));
	lua_pop(L, 1);
	return spell;
}

ThingPtr LuaScriptInterface::getThing(lua_State* L, int32_t arg)
{
	ThingPtr thing;
	if (lua_getmetatable(L, arg) != 0) {
		lua_rawgeti(L, -1, 't');
		switch(getNumber<uint32_t>(L, -1)) {
			case LuaData_Item:
				thing = getSharedPtr<Item>(L, arg);
				break;
			case LuaData_Container:
				thing = getSharedPtr<Container>(L, arg);
				break;
			case LuaData_Teleport:
				thing = getSharedPtr<Teleport>(L, arg);
				break;
			case LuaData_Player:
				thing = getSharedPtr<Player>(L, arg);
				break;
			case LuaData_Monster:
				thing = getSharedPtr<Monster>(L, arg);
				break;
			case LuaData_Npc:
				thing = getSharedPtr<Npc>(L, arg);
				break;
			default:
				thing = nullptr;
				break;
		}
		lua_pop(L, 2);
	} else {
		thing = getScriptEnv()->getThingByUID(getNumber<uint32_t>(L, arg));
	}
	return thing;
}

CreaturePtr LuaScriptInterface::getCreature(lua_State* L, int32_t arg)
{
	if (isUserdata(L, arg)) {
		return getSharedPtr<Creature>(L, arg);
	}
	return g_game.getCreatureByID(getNumber<uint32_t>(L, arg));
}

PlayerPtr LuaScriptInterface::getPlayer(lua_State* L, int32_t arg)
{
	if (isUserdata(L, arg)) {
		return getSharedPtr<Player>(L, arg);
	}
	return g_game.getPlayerByID(getNumber<uint32_t>(L, arg));
}

std::string LuaScriptInterface::getFieldString(lua_State* L, int32_t arg, const std::string& key)
{
	lua_getfield(L, arg, key.c_str());
	return getString(L, -1);
}

LuaDataType LuaScriptInterface::getUserdataType(lua_State* L, int32_t arg)
{
	if (lua_getmetatable(L, arg) == 0) {
		return LuaData_Unknown;
	}
	lua_rawgeti(L, -1, 't');

	LuaDataType type = getNumber<LuaDataType>(L, -1);
	lua_pop(L, 2);

	return type;
}

// Push
void LuaScriptInterface::pushBoolean(lua_State* L, bool value)
{
	lua_pushboolean(L, value ? 1 : 0);
}

void LuaScriptInterface::pushCombatDamage(lua_State* L, const CombatDamage& damage)
{
	lua_pushinteger(L, damage.primary.value);
	lua_pushinteger(L, damage.primary.type);
	lua_pushinteger(L, damage.secondary.value);
	lua_pushinteger(L, damage.secondary.type);
	lua_pushinteger(L, damage.origin);
}

void LuaScriptInterface::pushInstantSpell(lua_State* L, const InstantSpell& spell)
{
	lua_createtable(L, 0, 7);

	setField(L, "name", spell.getName());
	setField(L, "words", spell.getWords());
	setField(L, "level", spell.getLevel());
	setField(L, "mlevel", spell.getMagicLevel());
	setField(L, "mana", spell.getMana());
	setField(L, "manapercent", spell.getManaPercent());
	setField(L, "params", spell.getHasParam());

	setMetatable(L, -1, "Spell");
}

void LuaScriptInterface::pushSpell(lua_State* L, const Spell& spell)
{
	lua_createtable(L, 0, 10);

	setField(L, "name", spell.getName());
	setField(L, "level", spell.getLevel());
	setField(L, "mlevel", spell.getMagicLevel());
	setField(L, "mana", spell.getMana());
	setField(L, "manapercent", spell.getManaPercent());
	setField(L, "aggressive", spell.getAggressive());
	setField(L, "needTarget", spell.getNeedTarget());
	setField(L, "needWeapon", spell.getNeedWeapon());
	setField(L, "selfTarget", spell.getSelfTarget());
	setField(L, "range", spell.getRange());

	setMetatable(L, -1, "Spell");
}

void LuaScriptInterface::pushPosition(lua_State* L, const Position& position, int32_t stackpos/* = 0*/)
{
	lua_createtable(L, 0, 4);

	setField(L, "x", position.x);
	setField(L, "y", position.y);
	setField(L, "z", static_cast<uint16_t>(position.z));
	setField(L, "stackpos", stackpos);

	setMetatable(L, -1, "Position");
}

void LuaScriptInterface::pushOutfit(lua_State* L, const Outfit_t& outfit)
{
	lua_createtable(L, 0, 8);
	setField(L, "lookType", outfit.lookType);
	setField(L, "lookTypeEx", outfit.lookTypeEx);
	setField(L, "lookHead", static_cast<uint16_t>(outfit.lookHead));
	setField(L, "lookBody", static_cast<uint16_t>(outfit.lookBody));
	setField(L, "lookLegs", static_cast<uint16_t>(outfit.lookLegs));
	setField(L, "lookFeet", static_cast<uint16_t>(outfit.lookFeet));
	setField(L, "lookAddons", static_cast<uint16_t>(outfit.lookAddons));
	setField(L, "lookMount", outfit.lookMount);
}

void LuaScriptInterface::pushOutfit(lua_State* L, const Outfit* outfit)
{
	lua_createtable(L, 0, 4);
	setField(L, "lookType", outfit->lookType);
	setField(L, "name", outfit->name);
	setField(L, "premium", outfit->premium);
	setField(L, "locked", outfit->locked);
	setMetatable(L, -1, "Outfit");
}

void LuaScriptInterface::pushMount(lua_State* L, const Mount* mount)
{
	lua_createtable(L, 0, 5);
	setField(L, "name", mount->name);
	setField(L, "speed", mount->speed);
	setField(L, "clientId", mount->clientId);
	setField(L, "id", static_cast<uint16_t>(mount->id));
	setField(L, "premium", mount->premium);
}

void LuaScriptInterface::pushLoot(lua_State* L, const std::vector<LootBlock>& lootList)
{
	lua_createtable(L, lootList.size(), 0);

	int index = 0;
	for (const auto& lootBlock : lootList) {
		lua_createtable(L, 0, 7);

		setField(L, "itemId", lootBlock.id);
		setField(L, "chance", lootBlock.chance);
		setField(L, "subType", lootBlock.subType);
		setField(L, "maxCount", lootBlock.countmax);
		setField(L, "actionId", lootBlock.actionId);
		setField(L, "text", lootBlock.text);

		pushLoot(L, lootBlock.childLoot);
		lua_setfield(L, -2, "childLoot");

		lua_rawseti(L, -2, ++index);
	}
}

#define registerEnum(value) { std::string enumName = #value; registerGlobalVariable(enumName.substr(enumName.find_last_of(':') + 1), value); }
#define registerEnumIn(tableName, value) { std::string enumName = #value; registerVariable(tableName, enumName.substr(enumName.find_last_of(':') + 1), value); }

void LuaScriptInterface::registerFunctions()
{
	//doPlayerAddItem(uid, itemid, <optional: default: 1> count/subtype)
	//doPlayerAddItem(cid, itemid, <optional: default: 1> count, <optional: default: 1> canDropOnMap, <optional: default: 1>subtype)
	//Returns uid of the created item
	lua_register(luaState, "doPlayerAddItem", LuaScriptInterface::luaDoPlayerAddItem);

	//isValidUID(uid)
	lua_register(luaState, "isValidUID", LuaScriptInterface::luaIsValidUID);

	//isDepot(uid)
	lua_register(luaState, "isDepot", LuaScriptInterface::luaIsDepot);

	//isMovable(uid)
	lua_register(luaState, "isMovable", LuaScriptInterface::luaIsMoveable);

	//doAddContainerItem(uid, itemid, <optional> count/subtype)

	//getDepotId(uid)
	lua_register(luaState, "getDepotId", LuaScriptInterface::luaGetDepotId);

	//getWorldTime()
	lua_register(luaState, "getWorldTime", LuaScriptInterface::luaGetWorldTime);

	//getWorldLight()
	lua_register(luaState, "getWorldLight", LuaScriptInterface::luaGetWorldLight);

	//setWorldLight(level, color)
	lua_register(luaState, "setWorldLight", LuaScriptInterface::luaSetWorldLight);

	//getWorldUpTime()
	lua_register(luaState, "getWorldUpTime", LuaScriptInterface::luaGetWorldUpTime);

	// getSubTypeName(subType)
	lua_register(luaState, "getSubTypeName", LuaScriptInterface::luaGetSubTypeName);

	//createCombatArea( {area}, <optional> {extArea} )
	lua_register(luaState, "createCombatArea", LuaScriptInterface::luaCreateCombatArea);

	//doAreaCombat(cid, type, pos, area, min, max, effect[, origin = ORIGIN_SPELL[, blockArmor = false[, blockShield = false[, ignoreResistances = false]]]])
	lua_register(luaState, "doAreaCombat", LuaScriptInterface::luaDoAreaCombat);

	//doTargetCombat(cid, target, type, min, max, effect[, origin = ORIGIN_SPELL[, blockArmor = false[, blockShield = false[, ignoreResistances = false]]]])
	lua_register(luaState, "doTargetCombat", LuaScriptInterface::luaDoTargetCombat);

	//doChallengeCreature(cid, target[, force = false])
	lua_register(luaState, "doChallengeCreature", LuaScriptInterface::luaDoChallengeCreature);

	//addEvent(callback, delay, ...)
	lua_register(luaState, "addEvent", LuaScriptInterface::luaAddEvent);

	//stopEvent(eventid)
	lua_register(luaState, "stopEvent", LuaScriptInterface::luaStopEvent);

	//saveServer()
	lua_register(luaState, "saveServer", LuaScriptInterface::luaSaveServer);

	//cleanMap()
	lua_register(luaState, "cleanMap", LuaScriptInterface::luaCleanMap);

	//debugPrint(text)
	lua_register(luaState, "debugPrint", LuaScriptInterface::luaDebugPrint);

	//isInWar(cid, target)
	lua_register(luaState, "isInWar", LuaScriptInterface::luaIsInWar);

	//getWaypointPosition(name)
	lua_register(luaState, "getWaypointPositionByName", LuaScriptInterface::luaGetWaypointPositionByName);

	//sendChannelMessage(channelId, type, message)
	lua_register(luaState, "sendChannelMessage", LuaScriptInterface::luaSendChannelMessage);

	//sendGuildChannelMessage(guildId, type, message)
	lua_register(luaState, "sendGuildChannelMessage", LuaScriptInterface::luaSendGuildChannelMessage);

	//isScriptsInterface()
	lua_register(luaState, "isScriptsInterface", LuaScriptInterface::luaIsScriptsInterface);

#ifndef LUAJIT_VERSION
	//bit operations for Lua, based on bitlib project release 24
	//bit.bnot, bit.band, bit.bor, bit.bxor, bit.lshift, bit.rshift
	luaL_register(luaState, "bit", LuaScriptInterface::luaBitReg);
	lua_pop(luaState, 1);
#endif

	//configManager table
	luaL_register(luaState, "configManager", LuaScriptInterface::luaConfigManagerTable);
	lua_pop(luaState, 1);

	//db table
	luaL_register(luaState, "db", LuaScriptInterface::luaDatabaseTable);
	lua_pop(luaState, 1);

	//result table
	luaL_register(luaState, "result", LuaScriptInterface::luaResultTable);
	lua_pop(luaState, 1);

	/* New functions */
	//registerClass(className, baseClass, newFunction)
	//registerTable(tableName)
	//registerMethod(className, functionName, function)
	//registerMetaMethod(className, functionName, function)
	//registerGlobalMethod(functionName, function)
	//registerVariable(tableName, name, value)
	//registerGlobalVariable(name, value)
	//registerEnum(value)
	//registerEnumIn(tableName, value)

	// Enums
	registerEnum(ACCOUNT_TYPE_NORMAL)
	registerEnum(ACCOUNT_TYPE_TUTOR)
	registerEnum(ACCOUNT_TYPE_SENIORTUTOR)
	registerEnum(ACCOUNT_TYPE_GAMEMASTER)
	registerEnum(ACCOUNT_TYPE_COMMUNITYMANAGER)
	registerEnum(ACCOUNT_TYPE_GOD)

	registerEnum(AMMO_NONE)
	registerEnum(AMMO_BOLT)
	registerEnum(AMMO_ARROW)
	registerEnum(AMMO_SPEAR)
	registerEnum(AMMO_THROWINGSTAR)
	registerEnum(AMMO_THROWINGKNIFE)
	registerEnum(AMMO_STONE)
	registerEnum(AMMO_SNOWBALL)

	registerEnum(BUG_CATEGORY_MAP)
	registerEnum(BUG_CATEGORY_TYPO)
	registerEnum(BUG_CATEGORY_TECHNICAL)
	registerEnum(BUG_CATEGORY_OTHER)

	registerEnum(CALLBACK_PARAM_LEVELMAGICVALUE)
	registerEnum(CALLBACK_PARAM_SKILLVALUE)
	registerEnum(CALLBACK_PARAM_TARGETTILE)
	registerEnum(CALLBACK_PARAM_TARGETCREATURE)

	registerEnum(COMBAT_FORMULA_UNDEFINED)
	registerEnum(COMBAT_FORMULA_LEVELMAGIC)
	registerEnum(COMBAT_FORMULA_SKILL)
	registerEnum(COMBAT_FORMULA_DAMAGE)

	registerEnum(DIRECTION_NORTH)
	registerEnum(DIRECTION_EAST)
	registerEnum(DIRECTION_SOUTH)
	registerEnum(DIRECTION_WEST)
	registerEnum(DIRECTION_SOUTHWEST)
	registerEnum(DIRECTION_SOUTHEAST)
	registerEnum(DIRECTION_NORTHWEST)
	registerEnum(DIRECTION_NORTHEAST)

	registerEnum(COMBAT_NONE)
	registerEnum(COMBAT_PHYSICALDAMAGE)
	registerEnum(COMBAT_ENERGYDAMAGE)
	registerEnum(COMBAT_EARTHDAMAGE)
	registerEnum(COMBAT_FIREDAMAGE)
	registerEnum(COMBAT_UNDEFINEDDAMAGE)
	registerEnum(COMBAT_LIFEDRAIN)
	registerEnum(COMBAT_MANADRAIN)
	registerEnum(COMBAT_HEALING)
	registerEnum(COMBAT_DROWNDAMAGE)
	registerEnum(COMBAT_ICEDAMAGE)
	registerEnum(COMBAT_HOLYDAMAGE)
	registerEnum(COMBAT_DEATHDAMAGE)

	registerEnum(COMBAT_PARAM_TYPE)
	registerEnum(COMBAT_PARAM_EFFECT)
	registerEnum(COMBAT_PARAM_DISTANCEEFFECT)
	registerEnum(COMBAT_PARAM_BLOCKSHIELD)
	registerEnum(COMBAT_PARAM_BLOCKARMOR)
	registerEnum(COMBAT_PARAM_TARGETCASTERORTOPMOST)
	registerEnum(COMBAT_PARAM_CREATEITEM)
	registerEnum(COMBAT_PARAM_AGGRESSIVE)
	registerEnum(COMBAT_PARAM_DISPEL)
	registerEnum(COMBAT_PARAM_USECHARGES)

	registerEnum(CONDITION_NONE)
	registerEnum(CONDITION_POISON)
	registerEnum(CONDITION_FIRE)
	registerEnum(CONDITION_ENERGY)
	registerEnum(CONDITION_BLEEDING)
	registerEnum(CONDITION_HASTE)
	registerEnum(CONDITION_PARALYZE)
	registerEnum(CONDITION_OUTFIT)
	registerEnum(CONDITION_INVISIBLE)
	registerEnum(CONDITION_LIGHT)
	registerEnum(CONDITION_MANASHIELD)
	registerEnum(CONDITION_INFIGHT)
	registerEnum(CONDITION_DRUNK)
	registerEnum(CONDITION_EXHAUST_WEAPON)
	registerEnum(CONDITION_REGENERATION)
	registerEnum(CONDITION_SOUL)
	registerEnum(CONDITION_DROWN)
	registerEnum(CONDITION_MUTED)
	registerEnum(CONDITION_CHANNELMUTEDTICKS)
	registerEnum(CONDITION_YELLTICKS)
	registerEnum(CONDITION_ATTRIBUTES)
	registerEnum(CONDITION_FREEZING)
	registerEnum(CONDITION_DAZZLED)
	registerEnum(CONDITION_CURSED)
	registerEnum(CONDITION_EXHAUST_COMBAT)
	registerEnum(CONDITION_EXHAUST_HEAL)
	registerEnum(CONDITION_PACIFIED)
	registerEnum(CONDITION_SPELLCOOLDOWN)
	registerEnum(CONDITION_SPELLGROUPCOOLDOWN)

	registerEnum(CONDITIONID_DEFAULT)
	registerEnum(CONDITIONID_COMBAT)
	registerEnum(CONDITIONID_HEAD)
	registerEnum(CONDITIONID_NECKLACE)
	registerEnum(CONDITIONID_BACKPACK)
	registerEnum(CONDITIONID_ARMOR)
	registerEnum(CONDITIONID_RIGHT)
	registerEnum(CONDITIONID_LEFT)
	registerEnum(CONDITIONID_LEGS)
	registerEnum(CONDITIONID_FEET)
	registerEnum(CONDITIONID_RING)
	registerEnum(CONDITIONID_AMMO)

	registerEnum(CONDITION_PARAM_OWNER)
	registerEnum(CONDITION_PARAM_TICKS)
	registerEnum(CONDITION_PARAM_DRUNKENNESS)
	registerEnum(CONDITION_PARAM_HEALTHGAIN)
	registerEnum(CONDITION_PARAM_HEALTHTICKS)
	registerEnum(CONDITION_PARAM_MANAGAIN)
	registerEnum(CONDITION_PARAM_MANATICKS)
	registerEnum(CONDITION_PARAM_DELAYED)
	registerEnum(CONDITION_PARAM_SPEED)
	registerEnum(CONDITION_PARAM_LIGHT_LEVEL)
	registerEnum(CONDITION_PARAM_LIGHT_COLOR)
	registerEnum(CONDITION_PARAM_SOULGAIN)
	registerEnum(CONDITION_PARAM_SOULTICKS)
	registerEnum(CONDITION_PARAM_MINVALUE)
	registerEnum(CONDITION_PARAM_MAXVALUE)
	registerEnum(CONDITION_PARAM_STARTVALUE)
	registerEnum(CONDITION_PARAM_TICKINTERVAL)
	registerEnum(CONDITION_PARAM_FORCEUPDATE)
	registerEnum(CONDITION_PARAM_SKILL_MELEE)
	registerEnum(CONDITION_PARAM_SKILL_FIST)
	registerEnum(CONDITION_PARAM_SKILL_CLUB)
	registerEnum(CONDITION_PARAM_SKILL_SWORD)
	registerEnum(CONDITION_PARAM_SKILL_AXE)
	registerEnum(CONDITION_PARAM_SKILL_DISTANCE)
	registerEnum(CONDITION_PARAM_SKILL_SHIELD)
	registerEnum(CONDITION_PARAM_SKILL_FISHING)
	registerEnum(CONDITION_PARAM_STAT_MAXHITPOINTS)
	registerEnum(CONDITION_PARAM_STAT_MAXMANAPOINTS)
	registerEnum(CONDITION_PARAM_STAT_MAGICPOINTS)
	registerEnum(CONDITION_PARAM_STAT_MAXHITPOINTSPERCENT)
	registerEnum(CONDITION_PARAM_STAT_MAXMANAPOINTSPERCENT)
	registerEnum(CONDITION_PARAM_STAT_MAGICPOINTSPERCENT)
	registerEnum(CONDITION_PARAM_PERIODICDAMAGE)
	registerEnum(CONDITION_PARAM_SKILL_MELEEPERCENT)
	registerEnum(CONDITION_PARAM_SKILL_FISTPERCENT)
	registerEnum(CONDITION_PARAM_SKILL_CLUBPERCENT)
	registerEnum(CONDITION_PARAM_SKILL_SWORDPERCENT)
	registerEnum(CONDITION_PARAM_SKILL_AXEPERCENT)
	registerEnum(CONDITION_PARAM_SKILL_DISTANCEPERCENT)
	registerEnum(CONDITION_PARAM_SKILL_SHIELDPERCENT)
	registerEnum(CONDITION_PARAM_SKILL_FISHINGPERCENT)
	registerEnum(CONDITION_PARAM_BUFF_SPELL)
	registerEnum(CONDITION_PARAM_SUBID)
	registerEnum(CONDITION_PARAM_FIELD)
	registerEnum(CONDITION_PARAM_DISABLE_DEFENSE)
	registerEnum(CONDITION_PARAM_SPECIALSKILL_CRITICALHITCHANCE)
	registerEnum(CONDITION_PARAM_SPECIALSKILL_CRITICALHITAMOUNT)
	registerEnum(CONDITION_PARAM_SPECIALSKILL_LIFELEECHCHANCE)
	registerEnum(CONDITION_PARAM_SPECIALSKILL_LIFELEECHAMOUNT)
	registerEnum(CONDITION_PARAM_SPECIALSKILL_MANALEECHCHANCE)
	registerEnum(CONDITION_PARAM_SPECIALSKILL_MANALEECHAMOUNT)
	registerEnum(CONDITION_PARAM_AGGRESSIVE)

	registerEnum(CONST_ME_NONE)
	registerEnum(CONST_ME_DRAWBLOOD)
	registerEnum(CONST_ME_LOSEENERGY)
	registerEnum(CONST_ME_POFF)
	registerEnum(CONST_ME_BLOCKHIT)
	registerEnum(CONST_ME_EXPLOSIONAREA)
	registerEnum(CONST_ME_EXPLOSIONHIT)
	registerEnum(CONST_ME_FIREAREA)
	registerEnum(CONST_ME_YELLOW_RINGS)
	registerEnum(CONST_ME_GREEN_RINGS)
	registerEnum(CONST_ME_HITAREA)
	registerEnum(CONST_ME_TELEPORT)
	registerEnum(CONST_ME_ENERGYHIT)
	registerEnum(CONST_ME_MAGIC_BLUE)
	registerEnum(CONST_ME_MAGIC_RED)
	registerEnum(CONST_ME_MAGIC_GREEN)
	registerEnum(CONST_ME_HITBYFIRE)
	registerEnum(CONST_ME_HITBYPOISON)
	registerEnum(CONST_ME_MORTAREA)
	registerEnum(CONST_ME_SOUND_GREEN)
	registerEnum(CONST_ME_SOUND_RED)
	registerEnum(CONST_ME_POISONAREA)
	registerEnum(CONST_ME_SOUND_YELLOW)
	registerEnum(CONST_ME_SOUND_PURPLE)
	registerEnum(CONST_ME_SOUND_BLUE)
	registerEnum(CONST_ME_SOUND_WHITE)
	registerEnum(CONST_ME_BUBBLES)
	registerEnum(CONST_ME_CRAPS)
	registerEnum(CONST_ME_GIFT_WRAPS)
	registerEnum(CONST_ME_FIREWORK_YELLOW)
	registerEnum(CONST_ME_FIREWORK_RED)
	registerEnum(CONST_ME_FIREWORK_BLUE)
	registerEnum(CONST_ME_STUN)
	registerEnum(CONST_ME_SLEEP)
	registerEnum(CONST_ME_WATERCREATURE)
	registerEnum(CONST_ME_GROUNDSHAKER)
	registerEnum(CONST_ME_HEARTS)
	registerEnum(CONST_ME_FIREATTACK)
	registerEnum(CONST_ME_ENERGYAREA)
	registerEnum(CONST_ME_SMALLCLOUDS)
	registerEnum(CONST_ME_HOLYDAMAGE)
	registerEnum(CONST_ME_BIGCLOUDS)
	registerEnum(CONST_ME_ICEAREA)
	registerEnum(CONST_ME_ICETORNADO)
	registerEnum(CONST_ME_ICEATTACK)
	registerEnum(CONST_ME_STONES)
	registerEnum(CONST_ME_SMALLPLANTS)
	registerEnum(CONST_ME_CARNIPHILA)
	registerEnum(CONST_ME_PURPLEENERGY)
	registerEnum(CONST_ME_YELLOWENERGY)
	registerEnum(CONST_ME_HOLYAREA)
	registerEnum(CONST_ME_BIGPLANTS)
	registerEnum(CONST_ME_CAKE)
	registerEnum(CONST_ME_GIANTICE)
	registerEnum(CONST_ME_WATERSPLASH)
	registerEnum(CONST_ME_PLANTATTACK)
	registerEnum(CONST_ME_TUTORIALARROW)
	registerEnum(CONST_ME_TUTORIALSQUARE)
	registerEnum(CONST_ME_MIRRORHORIZONTAL)
	registerEnum(CONST_ME_MIRRORVERTICAL)
	registerEnum(CONST_ME_SKULLHORIZONTAL)
	registerEnum(CONST_ME_SKULLVERTICAL)
	registerEnum(CONST_ME_ASSASSIN)
	registerEnum(CONST_ME_STEPSHORIZONTAL)
	registerEnum(CONST_ME_BLOODYSTEPS)
	registerEnum(CONST_ME_STEPSVERTICAL)
	registerEnum(CONST_ME_YALAHARIGHOST)
	registerEnum(CONST_ME_BATS)
	registerEnum(CONST_ME_SMOKE)
	registerEnum(CONST_ME_INSECTS)
	registerEnum(CONST_ME_DRAGONHEAD)
	registerEnum(CONST_ME_ORCSHAMAN)
	registerEnum(CONST_ME_ORCSHAMAN_FIRE)
	registerEnum(CONST_ME_THUNDER)
	registerEnum(CONST_ME_FERUMBRAS)
	registerEnum(CONST_ME_CONFETTI_HORIZONTAL)
	registerEnum(CONST_ME_CONFETTI_VERTICAL)
	registerEnum(CONST_ME_BLACKSMOKE)
	registerEnum(CONST_ME_REDSMOKE)
	registerEnum(CONST_ME_YELLOWSMOKE)
	registerEnum(CONST_ME_GREENSMOKE)
	registerEnum(CONST_ME_PURPLESMOKE)
	registerEnum(CONST_ME_EARLY_THUNDER)
	registerEnum(CONST_ME_RAGIAZ_BONECAPSULE)
	registerEnum(CONST_ME_CRITICAL_DAMAGE)
	registerEnum(CONST_ME_PLUNGING_FISH)

	registerEnum(CONST_ANI_NONE)
	registerEnum(CONST_ANI_SPEAR)
	registerEnum(CONST_ANI_BOLT)
	registerEnum(CONST_ANI_ARROW)
	registerEnum(CONST_ANI_FIRE)
	registerEnum(CONST_ANI_ENERGY)
	registerEnum(CONST_ANI_POISONARROW)
	registerEnum(CONST_ANI_BURSTARROW)
	registerEnum(CONST_ANI_THROWINGSTAR)
	registerEnum(CONST_ANI_THROWINGKNIFE)
	registerEnum(CONST_ANI_SMALLSTONE)
	registerEnum(CONST_ANI_DEATH)
	registerEnum(CONST_ANI_LARGEROCK)
	registerEnum(CONST_ANI_SNOWBALL)
	registerEnum(CONST_ANI_POWERBOLT)
	registerEnum(CONST_ANI_POISON)
	registerEnum(CONST_ANI_INFERNALBOLT)
	registerEnum(CONST_ANI_HUNTINGSPEAR)
	registerEnum(CONST_ANI_ENCHANTEDSPEAR)
	registerEnum(CONST_ANI_REDSTAR)
	registerEnum(CONST_ANI_GREENSTAR)
	registerEnum(CONST_ANI_ROYALSPEAR)
	registerEnum(CONST_ANI_SNIPERARROW)
	registerEnum(CONST_ANI_ONYXARROW)
	registerEnum(CONST_ANI_PIERCINGBOLT)
	registerEnum(CONST_ANI_WHIRLWINDSWORD)
	registerEnum(CONST_ANI_WHIRLWINDAXE)
	registerEnum(CONST_ANI_WHIRLWINDCLUB)
	registerEnum(CONST_ANI_ETHEREALSPEAR)
	registerEnum(CONST_ANI_ICE)
	registerEnum(CONST_ANI_EARTH)
	registerEnum(CONST_ANI_HOLY)
	registerEnum(CONST_ANI_SUDDENDEATH)
	registerEnum(CONST_ANI_FLASHARROW)
	registerEnum(CONST_ANI_FLAMMINGARROW)
	registerEnum(CONST_ANI_SHIVERARROW)
	registerEnum(CONST_ANI_ENERGYBALL)
	registerEnum(CONST_ANI_SMALLICE)
	registerEnum(CONST_ANI_SMALLHOLY)
	registerEnum(CONST_ANI_SMALLEARTH)
	registerEnum(CONST_ANI_EARTHARROW)
	registerEnum(CONST_ANI_EXPLOSION)
	registerEnum(CONST_ANI_CAKE)
	registerEnum(CONST_ANI_TARSALARROW)
	registerEnum(CONST_ANI_VORTEXBOLT)
	registerEnum(CONST_ANI_PRISMATICBOLT)
	registerEnum(CONST_ANI_CRYSTALLINEARROW)
	registerEnum(CONST_ANI_DRILLBOLT)
	registerEnum(CONST_ANI_ENVENOMEDARROW)
	registerEnum(CONST_ANI_GLOOTHSPEAR)
	registerEnum(CONST_ANI_SIMPLEARROW)
	registerEnum(CONST_ANI_WEAPONTYPE)

	registerEnum(CONST_PROP_BLOCKSOLID)
	registerEnum(CONST_PROP_HASHEIGHT)
	registerEnum(CONST_PROP_BLOCKPROJECTILE)
	registerEnum(CONST_PROP_BLOCKPATH)
	registerEnum(CONST_PROP_ISVERTICAL)
	registerEnum(CONST_PROP_ISHORIZONTAL)
	registerEnum(CONST_PROP_MOVEABLE)
	registerEnum(CONST_PROP_IMMOVABLEBLOCKSOLID)
	registerEnum(CONST_PROP_IMMOVABLEBLOCKPATH)
	registerEnum(CONST_PROP_IMMOVABLENOFIELDBLOCKPATH)
	registerEnum(CONST_PROP_NOFIELDBLOCKPATH)
	registerEnum(CONST_PROP_SUPPORTHANGABLE)

	registerEnum(CONST_SLOT_HEAD)
	registerEnum(CONST_SLOT_NECKLACE)
	registerEnum(CONST_SLOT_BACKPACK)
	registerEnum(CONST_SLOT_ARMOR)
	registerEnum(CONST_SLOT_RIGHT)
	registerEnum(CONST_SLOT_LEFT)
	registerEnum(CONST_SLOT_LEGS)
	registerEnum(CONST_SLOT_FEET)
	registerEnum(CONST_SLOT_RING)
	registerEnum(CONST_SLOT_AMMO)

	registerEnum(CREATURE_EVENT_NONE)
	registerEnum(CREATURE_EVENT_LOGIN)
	registerEnum(CREATURE_EVENT_LOGOUT)
	registerEnum(CREATURE_EVENT_THINK)
	registerEnum(CREATURE_EVENT_PREPAREDEATH)
	registerEnum(CREATURE_EVENT_DEATH)
	registerEnum(CREATURE_EVENT_KILL)
	registerEnum(CREATURE_EVENT_ADVANCE)
	registerEnum(CREATURE_EVENT_MODALWINDOW)
	registerEnum(CREATURE_EVENT_TEXTEDIT)
	registerEnum(CREATURE_EVENT_HEALTHCHANGE)
	registerEnum(CREATURE_EVENT_MANACHANGE)
	registerEnum(CREATURE_EVENT_EXTENDED_OPCODE)

	registerEnum(GAME_STATE_STARTUP)
	registerEnum(GAME_STATE_INIT)
	registerEnum(GAME_STATE_NORMAL)
	registerEnum(GAME_STATE_CLOSED)
	registerEnum(GAME_STATE_SHUTDOWN)
	registerEnum(GAME_STATE_CLOSING)
	registerEnum(GAME_STATE_MAINTAIN)

	registerEnum(MESSAGE_STATUS_CONSOLE_BLUE)
	registerEnum(MESSAGE_STATUS_CONSOLE_RED)
	registerEnum(MESSAGE_STATUS_DEFAULT)
	registerEnum(MESSAGE_STATUS_WARNING)
	registerEnum(MESSAGE_EVENT_ADVANCE)
	registerEnum(MESSAGE_STATUS_SMALL)
	registerEnum(MESSAGE_INFO_DESCR)
	registerEnum(MESSAGE_DAMAGE_DEALT)
	registerEnum(MESSAGE_DAMAGE_RECEIVED)
	registerEnum(MESSAGE_HEALED)
	registerEnum(MESSAGE_EXPERIENCE)
	registerEnum(MESSAGE_DAMAGE_OTHERS)
	registerEnum(MESSAGE_HEALED_OTHERS)
	registerEnum(MESSAGE_EXPERIENCE_OTHERS)
	registerEnum(MESSAGE_EVENT_DEFAULT)
	registerEnum(MESSAGE_GUILD)
	registerEnum(MESSAGE_PARTY_MANAGEMENT)
	registerEnum(MESSAGE_PARTY)
	registerEnum(MESSAGE_EVENT_ORANGE)
	registerEnum(MESSAGE_STATUS_CONSOLE_ORANGE)
	registerEnum(MESSAGE_LOOT)

	registerEnum(CREATURETYPE_PLAYER)
	registerEnum(CREATURETYPE_MONSTER)
	registerEnum(CREATURETYPE_NPC)
	registerEnum(CREATURETYPE_SUMMON_OWN)
	registerEnum(CREATURETYPE_SUMMON_HOSTILE)
	registerEnum(CREATURETYPE_SUMMON_GUILD)
	registerEnum(CREATURETYPE_SUMMON_PARTY)
	registerEnum(CREATURETYPE_BOSS)
	registerEnum(CREATURETYPE_SUMMON_HOSTILE)
	registerEnum(CREATURETYPE_ATTACKABLE)
	registerEnum(CREATURETYPE_SUMMON_ALL)

	registerEnum(CLIENTOS_LINUX)
	registerEnum(CLIENTOS_WINDOWS)
	registerEnum(CLIENTOS_FLASH)
	registerEnum(CLIENTOS_OTCLIENT_LINUX)
	registerEnum(CLIENTOS_OTCLIENT_WINDOWS)
	registerEnum(CLIENTOS_OTCLIENT_MAC)

	registerEnum(FIGHTMODE_ATTACK)
	registerEnum(FIGHTMODE_BALANCED)
	registerEnum(FIGHTMODE_DEFENSE)

	registerEnum(ITEM_ATTRIBUTE_NONE)
	registerEnum(ITEM_ATTRIBUTE_ACTIONID)
	registerEnum(ITEM_ATTRIBUTE_UNIQUEID)
	registerEnum(ITEM_ATTRIBUTE_DESCRIPTION)
	registerEnum(ITEM_ATTRIBUTE_TEXT)
	registerEnum(ITEM_ATTRIBUTE_DATE)
	registerEnum(ITEM_ATTRIBUTE_WRITER)
	registerEnum(ITEM_ATTRIBUTE_NAME)
	registerEnum(ITEM_ATTRIBUTE_ARTICLE)
	registerEnum(ITEM_ATTRIBUTE_PLURALNAME)
	registerEnum(ITEM_ATTRIBUTE_WEIGHT)
	registerEnum(ITEM_ATTRIBUTE_ATTACK)
	registerEnum(ITEM_ATTRIBUTE_ATTACK_SPEED)
	registerEnum(ITEM_ATTRIBUTE_CLASSIFICATION)
	registerEnum(ITEM_ATTRIBUTE_TIER)
	registerEnum(ITEM_ATTRIBUTE_REWARDID)
	registerEnum(ITEM_ATTRIBUTE_DEFENSE)
	registerEnum(ITEM_ATTRIBUTE_EXTRADEFENSE)
	registerEnum(ITEM_ATTRIBUTE_ARMOR)
	registerEnum(ITEM_ATTRIBUTE_HITCHANCE)
	registerEnum(ITEM_ATTRIBUTE_SHOOTRANGE)
	registerEnum(ITEM_ATTRIBUTE_OWNER)
	registerEnum(ITEM_ATTRIBUTE_DURATION)
	registerEnum(ITEM_ATTRIBUTE_DECAYSTATE)
	registerEnum(ITEM_ATTRIBUTE_CORPSEOWNER)
	registerEnum(ITEM_ATTRIBUTE_CHARGES)
	registerEnum(ITEM_ATTRIBUTE_FLUIDTYPE)
	registerEnum(ITEM_ATTRIBUTE_DOORID)
	registerEnum(ITEM_ATTRIBUTE_DECAYTO)
	registerEnum(ITEM_ATTRIBUTE_WRAPID)
	registerEnum(ITEM_ATTRIBUTE_STOREITEM)
	registerEnum(ITEM_ATTRIBUTE_ATTACK_SPEED)

	registerEnum(ITEM_TYPE_DEPOT)
	registerEnum(ITEM_TYPE_MAILBOX)
	registerEnum(ITEM_TYPE_TRASHHOLDER)
	registerEnum(ITEM_TYPE_CONTAINER)
	registerEnum(ITEM_TYPE_DOOR)
	registerEnum(ITEM_TYPE_MAGICFIELD)
	registerEnum(ITEM_TYPE_TELEPORT)
	registerEnum(ITEM_TYPE_BED)
	registerEnum(ITEM_TYPE_KEY)
	registerEnum(ITEM_TYPE_RUNE)

	registerEnum(ITEM_GROUP_GROUND)
	registerEnum(ITEM_GROUP_CONTAINER)
	registerEnum(ITEM_GROUP_WEAPON)
	registerEnum(ITEM_GROUP_AMMUNITION)
	registerEnum(ITEM_GROUP_ARMOR)
	registerEnum(ITEM_GROUP_CHARGES)
	registerEnum(ITEM_GROUP_TELEPORT)
	registerEnum(ITEM_GROUP_MAGICFIELD)
	registerEnum(ITEM_GROUP_WRITEABLE)
	registerEnum(ITEM_GROUP_KEY)
	registerEnum(ITEM_GROUP_SPLASH)
	registerEnum(ITEM_GROUP_FLUID)
	registerEnum(ITEM_GROUP_DOOR)
	registerEnum(ITEM_GROUP_DEPRECATED)

	registerEnum(ITEM_BROWSEFIELD)
	registerEnum(ITEM_BAG)
	registerEnum(ITEM_SHOPPING_BAG)
	registerEnum(ITEM_GOLD_COIN)
	registerEnum(ITEM_PLATINUM_COIN)
	registerEnum(ITEM_CRYSTAL_COIN)
	registerEnum(ITEM_AMULETOFLOSS)
	registerEnum(ITEM_PARCEL)
	registerEnum(ITEM_LABEL)
	registerEnum(ITEM_FIREFIELD_PVP_FULL)
	registerEnum(ITEM_FIREFIELD_PVP_MEDIUM)
	registerEnum(ITEM_FIREFIELD_PVP_SMALL)
	registerEnum(ITEM_FIREFIELD_PERSISTENT_FULL)
	registerEnum(ITEM_FIREFIELD_PERSISTENT_MEDIUM)
	registerEnum(ITEM_FIREFIELD_PERSISTENT_SMALL)
	registerEnum(ITEM_FIREFIELD_NOPVP)
	registerEnum(ITEM_POISONFIELD_PVP)
	registerEnum(ITEM_POISONFIELD_PERSISTENT)
	registerEnum(ITEM_POISONFIELD_NOPVP)
	registerEnum(ITEM_ENERGYFIELD_PVP)
	registerEnum(ITEM_ENERGYFIELD_PERSISTENT)
	registerEnum(ITEM_ENERGYFIELD_NOPVP)
	registerEnum(ITEM_MAGICWALL)
	registerEnum(ITEM_MAGICWALL_PERSISTENT)
	registerEnum(ITEM_MAGICWALL_SAFE)
	registerEnum(ITEM_WILDGROWTH)
	registerEnum(ITEM_WILDGROWTH_PERSISTENT)
	registerEnum(ITEM_WILDGROWTH_SAFE)

	registerEnum(ITEM_REWARD_CONTAINER)

	registerEnum(WIELDINFO_NONE)
	registerEnum(WIELDINFO_LEVEL)
	registerEnum(WIELDINFO_MAGLV)
	registerEnum(WIELDINFO_VOCREQ)
	registerEnum(WIELDINFO_PREMIUM)

	registerEnum(PlayerFlag_CannotUseCombat)
	registerEnum(PlayerFlag_CannotAttackPlayer)
	registerEnum(PlayerFlag_CannotAttackMonster)
	registerEnum(PlayerFlag_CannotBeAttacked)
	registerEnum(PlayerFlag_CanConvinceAll)
	registerEnum(PlayerFlag_CanSummonAll)
	registerEnum(PlayerFlag_CanIllusionAll)
	registerEnum(PlayerFlag_CanSenseInvisibility)
	registerEnum(PlayerFlag_IgnoredByMonsters)
	registerEnum(PlayerFlag_NotGainInFight)
	registerEnum(PlayerFlag_HasInfiniteMana)
	registerEnum(PlayerFlag_HasInfiniteSoul)
	registerEnum(PlayerFlag_HasNoExhaustion)
	registerEnum(PlayerFlag_CannotUseSpells)
	registerEnum(PlayerFlag_CannotPickupItem)
	registerEnum(PlayerFlag_CanAlwaysLogin)
	registerEnum(PlayerFlag_CanBroadcast)
	registerEnum(PlayerFlag_CanEditHouses)
	registerEnum(PlayerFlag_CannotBeBanned)
	registerEnum(PlayerFlag_CannotBePushed)
	registerEnum(PlayerFlag_HasInfiniteCapacity)
	registerEnum(PlayerFlag_CanPushAllCreatures)
	registerEnum(PlayerFlag_CanTalkRedPrivate)
	registerEnum(PlayerFlag_CanTalkRedChannel)
	registerEnum(PlayerFlag_TalkOrangeHelpChannel)
	registerEnum(PlayerFlag_NotGainExperience)
	registerEnum(PlayerFlag_NotGainMana)
	registerEnum(PlayerFlag_NotGainHealth)
	registerEnum(PlayerFlag_NotGainSkill)
	registerEnum(PlayerFlag_SetMaxSpeed)
	registerEnum(PlayerFlag_SpecialVIP)
	registerEnum(PlayerFlag_NotGenerateLoot)
	registerEnum(PlayerFlag_IgnoreProtectionZone)
	registerEnum(PlayerFlag_IgnoreSpellCheck)
	registerEnum(PlayerFlag_IgnoreWeaponCheck)
	registerEnum(PlayerFlag_CannotBeMuted)
	registerEnum(PlayerFlag_IsAlwaysPremium)

	registerEnum(PLAYERSEX_FEMALE)
	registerEnum(PLAYERSEX_MALE)

	registerEnum(REPORT_REASON_NAMEINAPPROPRIATE)
	registerEnum(REPORT_REASON_NAMEPOORFORMATTED)
	registerEnum(REPORT_REASON_NAMEADVERTISING)
	registerEnum(REPORT_REASON_NAMEUNFITTING)
	registerEnum(REPORT_REASON_NAMERULEVIOLATION)
	registerEnum(REPORT_REASON_INSULTINGSTATEMENT)
	registerEnum(REPORT_REASON_SPAMMING)
	registerEnum(REPORT_REASON_ADVERTISINGSTATEMENT)
	registerEnum(REPORT_REASON_UNFITTINGSTATEMENT)
	registerEnum(REPORT_REASON_LANGUAGESTATEMENT)
	registerEnum(REPORT_REASON_DISCLOSURE)
	registerEnum(REPORT_REASON_RULEVIOLATION)
	registerEnum(REPORT_REASON_STATEMENT_BUGABUSE)
	registerEnum(REPORT_REASON_UNOFFICIALSOFTWARE)
	registerEnum(REPORT_REASON_PRETENDING)
	registerEnum(REPORT_REASON_HARASSINGOWNERS)
	registerEnum(REPORT_REASON_FALSEINFO)
	registerEnum(REPORT_REASON_ACCOUNTSHARING)
	registerEnum(REPORT_REASON_STEALINGDATA)
	registerEnum(REPORT_REASON_SERVICEATTACKING)
	registerEnum(REPORT_REASON_SERVICEAGREEMENT)

	registerEnum(REPORT_TYPE_NAME)
	registerEnum(REPORT_TYPE_STATEMENT)
	registerEnum(REPORT_TYPE_BOT)

	registerEnum(VOCATION_NONE)

	registerEnum(SKILL_FIST)
	registerEnum(SKILL_CLUB)
	registerEnum(SKILL_SWORD)
	registerEnum(SKILL_AXE)
	registerEnum(SKILL_DISTANCE)
	registerEnum(SKILL_SHIELD)
	registerEnum(SKILL_FISHING)
	registerEnum(SKILL_MAGLEVEL)
	registerEnum(SKILL_LEVEL)

	registerEnum(SPECIALSKILL_CRITICALHITCHANCE)
	registerEnum(SPECIALSKILL_CRITICALHITAMOUNT)
	registerEnum(SPECIALSKILL_LIFELEECHCHANCE)
	registerEnum(SPECIALSKILL_LIFELEECHAMOUNT)
	registerEnum(SPECIALSKILL_MANALEECHCHANCE)
	registerEnum(SPECIALSKILL_MANALEECHAMOUNT)

	registerEnum(SKULL_NONE)
	registerEnum(SKULL_YELLOW)
	registerEnum(SKULL_GREEN)
	registerEnum(SKULL_WHITE)
	registerEnum(SKULL_RED)
	registerEnum(SKULL_BLACK)
	registerEnum(SKULL_ORANGE)

	registerEnum(TALKTYPE_SAY)
	registerEnum(TALKTYPE_WHISPER)
	registerEnum(TALKTYPE_YELL)
	registerEnum(TALKTYPE_PRIVATE_FROM)
	registerEnum(TALKTYPE_PRIVATE_TO)
	registerEnum(TALKTYPE_CHANNEL_Y)
	registerEnum(TALKTYPE_CHANNEL_O)
	registerEnum(TALKTYPE_PRIVATE_NP)
	registerEnum(TALKTYPE_PRIVATE_PN)
	registerEnum(TALKTYPE_BROADCAST)
	registerEnum(TALKTYPE_CHANNEL_R1)
	registerEnum(TALKTYPE_PRIVATE_RED_FROM)
	registerEnum(TALKTYPE_PRIVATE_RED_TO)
	registerEnum(TALKTYPE_MONSTER_SAY)
	registerEnum(TALKTYPE_MONSTER_YELL)

	registerEnum(TEXTCOLOR_BLUE)
	registerEnum(TEXTCOLOR_LIGHTGREEN)
	registerEnum(TEXTCOLOR_LIGHTBLUE)
	registerEnum(TEXTCOLOR_MAYABLUE)
	registerEnum(TEXTCOLOR_DARKRED)
	registerEnum(TEXTCOLOR_LIGHTGREY)
	registerEnum(TEXTCOLOR_SKYBLUE)
	registerEnum(TEXTCOLOR_PURPLE)
	registerEnum(TEXTCOLOR_ELECTRICPURPLE)
	registerEnum(TEXTCOLOR_RED)
	registerEnum(TEXTCOLOR_PASTELRED)
	registerEnum(TEXTCOLOR_ORANGE)
	registerEnum(TEXTCOLOR_YELLOW)
	registerEnum(TEXTCOLOR_WHITE_EXP)
	registerEnum(TEXTCOLOR_NONE)

	registerEnum(TILESTATE_NONE)
	registerEnum(TILESTATE_PROTECTIONZONE)
	registerEnum(TILESTATE_NOPVPZONE)
	registerEnum(TILESTATE_NOLOGOUT)
	registerEnum(TILESTATE_PVPZONE)
	registerEnum(TILESTATE_FLOORCHANGE)
	registerEnum(TILESTATE_FLOORCHANGE_DOWN)
	registerEnum(TILESTATE_FLOORCHANGE_NORTH)
	registerEnum(TILESTATE_FLOORCHANGE_SOUTH)
	registerEnum(TILESTATE_FLOORCHANGE_EAST)
	registerEnum(TILESTATE_FLOORCHANGE_WEST)
	registerEnum(TILESTATE_TELEPORT)
	registerEnum(TILESTATE_MAGICFIELD)
	registerEnum(TILESTATE_MAILBOX)
	registerEnum(TILESTATE_TRASHHOLDER)
	registerEnum(TILESTATE_BED)
	registerEnum(TILESTATE_DEPOT)
	registerEnum(TILESTATE_BLOCKSOLID)
	registerEnum(TILESTATE_BLOCKPATH)
	registerEnum(TILESTATE_IMMOVABLEBLOCKSOLID)
	registerEnum(TILESTATE_IMMOVABLEBLOCKPATH)
	registerEnum(TILESTATE_IMMOVABLENOFIELDBLOCKPATH)
	registerEnum(TILESTATE_NOFIELDBLOCKPATH)
	registerEnum(TILESTATE_FLOORCHANGE_SOUTH_ALT)
	registerEnum(TILESTATE_FLOORCHANGE_EAST_ALT)
	registerEnum(TILESTATE_SUPPORTS_HANGABLE)

	registerEnum(WEAPON_NONE)
	registerEnum(WEAPON_SWORD)
	registerEnum(WEAPON_CLUB)
	registerEnum(WEAPON_AXE)
	registerEnum(WEAPON_SHIELD)
	registerEnum(WEAPON_DISTANCE)
	registerEnum(WEAPON_WAND)
	registerEnum(WEAPON_AMMO)
	registerEnum(WEAPON_QUIVER);

	registerEnum(WORLD_TYPE_NO_PVP)
	registerEnum(WORLD_TYPE_PVP)
	registerEnum(WORLD_TYPE_PVP_ENFORCED)

	// Use with container:addItem, container:addItemEx and possibly other functions.
	registerEnum(FLAG_NOLIMIT)
	registerEnum(FLAG_IGNOREBLOCKITEM)
	registerEnum(FLAG_IGNOREBLOCKCREATURE)
	registerEnum(FLAG_CHILDISOWNER)
	registerEnum(FLAG_PATHFINDING)
	registerEnum(FLAG_IGNOREFIELDDAMAGE)
	registerEnum(FLAG_IGNORENOTMOVEABLE)
	registerEnum(FLAG_IGNOREAUTOSTACK)

	// Use with itemType:getSlotPosition
	registerEnum(SLOTP_WHEREEVER)
	registerEnum(SLOTP_HEAD)
	registerEnum(SLOTP_NECKLACE)
	registerEnum(SLOTP_BACKPACK)
	registerEnum(SLOTP_ARMOR)
	registerEnum(SLOTP_RIGHT)
	registerEnum(SLOTP_LEFT)
	registerEnum(SLOTP_LEGS)
	registerEnum(SLOTP_FEET)
	registerEnum(SLOTP_RING)
	registerEnum(SLOTP_AMMO)
	registerEnum(SLOTP_DEPOT)
	registerEnum(SLOTP_TWO_HAND)

	// Use with combat functions
	registerEnum(ORIGIN_NONE)
	registerEnum(ORIGIN_CONDITION)
	registerEnum(ORIGIN_SPELL)
	registerEnum(ORIGIN_MELEE)
	registerEnum(ORIGIN_RANGED)

	// Use with house:getAccessList, house:setAccessList
	registerEnum(GUEST_LIST)
	registerEnum(SUBOWNER_LIST)

	// Use with npc:setSpeechBubble
	registerEnum(SPEECHBUBBLE_NONE)
	registerEnum(SPEECHBUBBLE_NORMAL)
	registerEnum(SPEECHBUBBLE_TRADE)
	registerEnum(SPEECHBUBBLE_QUEST)
	registerEnum(SPEECHBUBBLE_QUESTTRADER)

	// Use with player:addMapMark
	registerEnum(MAPMARK_TICK)
	registerEnum(MAPMARK_QUESTION)
	registerEnum(MAPMARK_EXCLAMATION)
	registerEnum(MAPMARK_STAR)
	registerEnum(MAPMARK_CROSS)
	registerEnum(MAPMARK_TEMPLE)
	registerEnum(MAPMARK_KISS)
	registerEnum(MAPMARK_SHOVEL)
	registerEnum(MAPMARK_SWORD)
	registerEnum(MAPMARK_FLAG)
	registerEnum(MAPMARK_LOCK)
	registerEnum(MAPMARK_BAG)
	registerEnum(MAPMARK_SKULL)
	registerEnum(MAPMARK_DOLLAR)
	registerEnum(MAPMARK_REDNORTH)
	registerEnum(MAPMARK_REDSOUTH)
	registerEnum(MAPMARK_REDEAST)
	registerEnum(MAPMARK_REDWEST)
	registerEnum(MAPMARK_GREENNORTH)
	registerEnum(MAPMARK_GREENSOUTH)

	// Use with Game.getReturnMessage
	registerEnum(RETURNVALUE_NOERROR)
	registerEnum(RETURNVALUE_NOTPOSSIBLE)
	registerEnum(RETURNVALUE_NOTENOUGHROOM)
	registerEnum(RETURNVALUE_PLAYERISPZLOCKED)
	registerEnum(RETURNVALUE_PLAYERISNOTINVITED)
	registerEnum(RETURNVALUE_CANNOTTHROW)
	registerEnum(RETURNVALUE_THEREISNOWAY)
	registerEnum(RETURNVALUE_DESTINATIONOUTOFREACH)
	registerEnum(RETURNVALUE_CREATUREBLOCK)
	registerEnum(RETURNVALUE_NOTMOVEABLE)
	registerEnum(RETURNVALUE_DROPTWOHANDEDITEM)
	registerEnum(RETURNVALUE_BOTHHANDSNEEDTOBEFREE)
	registerEnum(RETURNVALUE_CANONLYUSEONEWEAPON)
	registerEnum(RETURNVALUE_NEEDEXCHANGE)
	registerEnum(RETURNVALUE_CANNOTBEDRESSED)
	registerEnum(RETURNVALUE_PUTTHISOBJECTINYOURHAND)
	registerEnum(RETURNVALUE_PUTTHISOBJECTINBOTHHANDS)
	registerEnum(RETURNVALUE_TOOFARAWAY)
	registerEnum(RETURNVALUE_FIRSTGODOWNSTAIRS)
	registerEnum(RETURNVALUE_FIRSTGOUPSTAIRS)
	registerEnum(RETURNVALUE_CONTAINERNOTENOUGHROOM)
	registerEnum(RETURNVALUE_NOTENOUGHCAPACITY)
	registerEnum(RETURNVALUE_CANNOTPICKUP)
	registerEnum(RETURNVALUE_THISISIMPOSSIBLE)
	registerEnum(RETURNVALUE_DEPOTISFULL)
	registerEnum(RETURNVALUE_CREATUREDOESNOTEXIST)
	registerEnum(RETURNVALUE_CANNOTUSETHISOBJECT)
	registerEnum(RETURNVALUE_PLAYERWITHTHISNAMEISNOTONLINE)
	registerEnum(RETURNVALUE_NOTREQUIREDLEVELTOUSERUNE)
	registerEnum(RETURNVALUE_YOUAREALREADYTRADING)
	registerEnum(RETURNVALUE_THISPLAYERISALREADYTRADING)
	registerEnum(RETURNVALUE_YOUMAYNOTLOGOUTDURINGAFIGHT)
	registerEnum(RETURNVALUE_DIRECTPLAYERSHOOT)
	registerEnum(RETURNVALUE_NOTENOUGHLEVEL)
	registerEnum(RETURNVALUE_NOTENOUGHMAGICLEVEL)
	registerEnum(RETURNVALUE_NOTENOUGHMANA)
	registerEnum(RETURNVALUE_NOTENOUGHSOUL)
	registerEnum(RETURNVALUE_YOUAREEXHAUSTED)
	registerEnum(RETURNVALUE_YOUCANNOTUSEOBJECTSTHATFAST)
	registerEnum(RETURNVALUE_PLAYERISNOTREACHABLE)
	registerEnum(RETURNVALUE_CANONLYUSETHISRUNEONCREATURES)
	registerEnum(RETURNVALUE_ACTIONNOTPERMITTEDINPROTECTIONZONE)
	registerEnum(RETURNVALUE_YOUMAYNOTATTACKTHISPLAYER)
	registerEnum(RETURNVALUE_YOUMAYNOTATTACKAPERSONINPROTECTIONZONE)
	registerEnum(RETURNVALUE_YOUMAYNOTATTACKAPERSONWHILEINPROTECTIONZONE)
	registerEnum(RETURNVALUE_YOUMAYNOTATTACKTHISCREATURE)
	registerEnum(RETURNVALUE_YOUCANONLYUSEITONCREATURES)
	registerEnum(RETURNVALUE_CREATUREISNOTREACHABLE)
	registerEnum(RETURNVALUE_TURNSECUREMODETOATTACKUNMARKEDPLAYERS)
	registerEnum(RETURNVALUE_YOUNEEDPREMIUMACCOUNT)
	registerEnum(RETURNVALUE_YOUNEEDTOLEARNTHISSPELL)
	registerEnum(RETURNVALUE_YOURVOCATIONCANNOTUSETHISSPELL)
	registerEnum(RETURNVALUE_YOUNEEDAWEAPONTOUSETHISSPELL)
	registerEnum(RETURNVALUE_PLAYERISPZLOCKEDLEAVEPVPZONE)
	registerEnum(RETURNVALUE_PLAYERISPZLOCKEDENTERPVPZONE)
	registerEnum(RETURNVALUE_ACTIONNOTPERMITTEDINANOPVPZONE)
	registerEnum(RETURNVALUE_YOUCANNOTLOGOUTHERE)
	registerEnum(RETURNVALUE_YOUNEEDAMAGICITEMTOCASTSPELL)
	registerEnum(RETURNVALUE_CANNOTCONJUREITEMHERE)
	registerEnum(RETURNVALUE_YOUNEEDTOSPLITYOURSPEARS)
	registerEnum(RETURNVALUE_NAMEISTOOAMBIGUOUS)
	registerEnum(RETURNVALUE_CANONLYUSEONESHIELD)
	registerEnum(RETURNVALUE_NOPARTYMEMBERSINRANGE)
	registerEnum(RETURNVALUE_YOUARENOTTHEOWNER)
	registerEnum(RETURNVALUE_TRADEPLAYERFARAWAY)
	registerEnum(RETURNVALUE_YOUDONTOWNTHISHOUSE)
	registerEnum(RETURNVALUE_TRADEPLAYERALREADYOWNSAHOUSE)
	registerEnum(RETURNVALUE_TRADEPLAYERHIGHESTBIDDER)
	registerEnum(RETURNVALUE_YOUCANNOTTRADETHISHOUSE)
	registerEnum(RETURNVALUE_YOUDONTHAVEREQUIREDPROFESSION)
	registerEnum(RETURNVALUE_YOUCANNOTUSETHISBED)

	registerEnum(RELOAD_TYPE_ALL)
	registerEnum(RELOAD_TYPE_ACTIONS)
	registerEnum(RELOAD_TYPE_AUGMENTS)
	registerEnum(RELOAD_TYPE_CHAT)
	registerEnum(RELOAD_TYPE_CONFIG)
	registerEnum(RELOAD_TYPE_CREATURESCRIPTS)
	registerEnum(RELOAD_TYPE_EVENTS)
	registerEnum(RELOAD_TYPE_GLOBAL)
	registerEnum(RELOAD_TYPE_GLOBALEVENTS)
	registerEnum(RELOAD_TYPE_ITEMS)
	registerEnum(RELOAD_TYPE_MONSTERS)
	registerEnum(RELOAD_TYPE_MOUNTS)
	registerEnum(RELOAD_TYPE_MOVEMENTS)
	registerEnum(RELOAD_TYPE_NPCS)
	registerEnum(RELOAD_TYPE_QUESTS)
	registerEnum(RELOAD_TYPE_RAIDS)
	registerEnum(RELOAD_TYPE_SCRIPTS)
	registerEnum(RELOAD_TYPE_SPELLS)
	registerEnum(RELOAD_TYPE_TALKACTIONS)
	registerEnum(RELOAD_TYPE_WEAPONS)

	registerEnum(ZONE_PROTECTION)
	registerEnum(ZONE_NOPVP)
	registerEnum(ZONE_PVP)
	registerEnum(ZONE_NOLOGOUT)
	registerEnum(ZONE_NORMAL)

	registerEnum(MAX_LOOTCHANCE)

	registerEnum(SPELL_INSTANT)
	registerEnum(SPELL_RUNE)

	registerEnum(MONSTERS_EVENT_THINK)
	registerEnum(MONSTERS_EVENT_APPEAR)
	registerEnum(MONSTERS_EVENT_DISAPPEAR)
	registerEnum(MONSTERS_EVENT_MOVE)
	registerEnum(MONSTERS_EVENT_SAY)

	registerEnum(DECAYING_FALSE)
	registerEnum(DECAYING_TRUE)
	registerEnum(DECAYING_PENDING)
	
	// Imbuements
	registerEnum(ImbuementType::IMBUEMENT_TYPE_NONE);
	registerEnum(ImbuementType::IMBUEMENT_TYPE_FIRE_DAMAGE);
	registerEnum(ImbuementType::IMBUEMENT_TYPE_EARTH_DAMAGE);
	registerEnum(ImbuementType::IMBUEMENT_TYPE_ICE_DAMAGE);
	registerEnum(ImbuementType::IMBUEMENT_TYPE_ENERGY_DAMAGE);
	registerEnum(ImbuementType::IMBUEMENT_TYPE_DEATH_DAMAGE);
	registerEnum(ImbuementType::IMBUEMENT_TYPE_HOLY_DAMAGE);
	registerEnum(ImbuementType::IMBUEMENT_TYPE_LIFE_LEECH);
	registerEnum(ImbuementType::IMBUEMENT_TYPE_MANA_LEECH);
	registerEnum(ImbuementType::IMBUEMENT_TYPE_CRITICAL_CHANCE);
	registerEnum(ImbuementType::IMBUEMENT_TYPE_CRITICAL_AMOUNT);
	registerEnum(ImbuementType::IMBUEMENT_TYPE_FIRE_RESIST);
	registerEnum(ImbuementType::IMBUEMENT_TYPE_EARTH_RESIST);
	registerEnum(ImbuementType::IMBUEMENT_TYPE_ICE_RESIST);
	registerEnum(ImbuementType::IMBUEMENT_TYPE_ENERGY_RESIST);
	registerEnum(ImbuementType::IMBUEMENT_TYPE_HOLY_RESIST);
	registerEnum(ImbuementType::IMBUEMENT_TYPE_DEATH_RESIST);
	registerEnum(ImbuementType::IMBUEMENT_TYPE_PARALYSIS_DEFLECTION);
	registerEnum(ImbuementType::IMBUEMENT_TYPE_SPEED_BOOST);
	registerEnum(ImbuementType::IMBUEMENT_TYPE_CAPACITY_BOOST);
	registerEnum(ImbuementType::IMBUEMENT_TYPE_FIST_SKILL);
	registerEnum(ImbuementType::IMBUEMENT_TYPE_AXE_SKILL);
	registerEnum(ImbuementType::IMBUEMENT_TYPE_SWORD_SKILL);
	registerEnum(ImbuementType::IMBUEMENT_TYPE_CLUB_SKILL);
	registerEnum(ImbuementType::IMBUEMENT_TYPE_DISTANCE_SKILL);
	registerEnum(ImbuementType::IMBUEMENT_TYPE_FISHING_SKILL);
	registerEnum(ImbuementType::IMBUEMENT_TYPE_SHIELD_SKILL);
	registerEnum(ImbuementType::IMBUEMENT_TYPE_MAGIC_LEVEL);
	registerEnum(ImbuementType::IMBUEMENT_TYPE_LAST);

	// Discord webhook enums
	registerEnum(DiscordMessageType::MESSAGE_NORMAL);
	registerEnum(DiscordMessageType::MESSAGE_ERROR);
	registerEnum(DiscordMessageType::MESSAGE_LOG);
	registerEnum(DiscordMessageType::MESSAGE_INFO);

	// Attack Modifiers
	registerEnum(ATTACK_MODIFIER_FIRST)
	registerEnum(ATTACK_MODIFIER_NONE)
	registerEnum(ATTACK_MODIFIER_LIFESTEAL)
	registerEnum(ATTACK_MODIFIER_MANASTEAL)
	registerEnum(ATTACK_MODIFIER_STAMINASTEAL)
	registerEnum(ATTACK_MODIFIER_SOULSTEAL)
	registerEnum(ATTACK_MODIFIER_CRITICAL)
	registerEnum(ATTACK_MODIFIER_PIERCING)
	registerEnum(ATTACK_MODIFIER_CONVERSION)
	registerEnum(ATTACK_MODIFIER_LAST)

	// Defense Modifiers
	registerEnum(DEFENSE_MODIFIER_FIRST)
	registerEnum(DEFENSE_MODIFIER_NONE)
	registerEnum(DEFENSE_MODIFIER_ABSORB)
	registerEnum(DEFENSE_MODIFIER_RESTORE)
	registerEnum(DEFENSE_MODIFIER_REPLENISH)
	registerEnum(DEFENSE_MODIFIER_REVIVE)
	registerEnum(DEFENSE_MODIFIER_REFLECT)
	registerEnum(DEFENSE_MODIFIER_DEFLECT)
	registerEnum(DEFENSE_MODIFIER_RICOCHET)
	registerEnum(DEFENSE_MODIFIER_RESIST)
	registerEnum(DEFENSE_MODIFIER_REFORM)
	registerEnum(DEFENSE_MODIFIER_WEAKNESS)
	registerEnum(DEFENSE_MODIFIER_LAST)

	// _G
	registerGlobalVariable("INDEX_WHEREEVER", INDEX_WHEREEVER);
	registerGlobalBoolean("VIRTUAL_PARENT", true);

	registerGlobalMethod("isType", LuaScriptInterface::luaIsType);
	registerGlobalMethod("rawgetmetatable", LuaScriptInterface::luaRawGetMetatable);

	// configKeys
	registerTable("configKeys");

	registerEnumIn("configKeys", ConfigManager::ALLOW_CHANGEOUTFIT);
	registerEnumIn("configKeys", ConfigManager::ONE_PLAYER_ON_ACCOUNT);
	registerEnumIn("configKeys", ConfigManager::AIMBOT_HOTKEY_ENABLED);
	registerEnumIn("configKeys", ConfigManager::REMOVE_RUNE_CHARGES);
	registerEnumIn("configKeys", ConfigManager::REMOVE_WEAPON_AMMO);
	registerEnumIn("configKeys", ConfigManager::REMOVE_WEAPON_CHARGES);
	registerEnumIn("configKeys", ConfigManager::REMOVE_POTION_CHARGES);
	registerEnumIn("configKeys", ConfigManager::EXPERIENCE_FROM_PLAYERS);
	registerEnumIn("configKeys", ConfigManager::FREE_PREMIUM);
	registerEnumIn("configKeys", ConfigManager::REPLACE_KICK_ON_LOGIN);
	registerEnumIn("configKeys", ConfigManager::ALLOW_CLONES);
	registerEnumIn("configKeys", ConfigManager::BIND_ONLY_GLOBAL_ADDRESS);
	registerEnumIn("configKeys", ConfigManager::OPTIMIZE_DATABASE);
	registerEnumIn("configKeys", ConfigManager::MARKET_PREMIUM);
	registerEnumIn("configKeys", ConfigManager::EMOTE_SPELLS);
	registerEnumIn("configKeys", ConfigManager::STAMINA_SYSTEM);
	registerEnumIn("configKeys", ConfigManager::WARN_UNSAFE_SCRIPTS);
	registerEnumIn("configKeys", ConfigManager::CONVERT_UNSAFE_SCRIPTS);
	registerEnumIn("configKeys", ConfigManager::CLASSIC_EQUIPMENT_SLOTS);
	registerEnumIn("configKeys", ConfigManager::CLASSIC_ATTACK_SPEED);
	registerEnumIn("configKeys", ConfigManager::SERVER_SAVE_NOTIFY_MESSAGE);
	registerEnumIn("configKeys", ConfigManager::SERVER_SAVE_NOTIFY_DURATION);
	registerEnumIn("configKeys", ConfigManager::SERVER_SAVE_CLEAN_MAP);
	registerEnumIn("configKeys", ConfigManager::SERVER_SAVE_CLOSE);
	registerEnumIn("configKeys", ConfigManager::SERVER_SAVE_SHUTDOWN);
	registerEnumIn("configKeys", ConfigManager::ONLINE_OFFLINE_CHARLIST);
	registerEnumIn("configKeys", ConfigManager::LUA_ITEM_DESC);
	registerEnumIn("configKeys", ConfigManager::CHECK_DUPLICATE_STORAGE_KEYS);

	registerEnumIn("configKeys", ConfigManager::MAP_NAME);
	registerEnumIn("configKeys", ConfigManager::HOUSE_RENT_PERIOD);
	registerEnumIn("configKeys", ConfigManager::SERVER_NAME);
	registerEnumIn("configKeys", ConfigManager::OWNER_NAME);
	registerEnumIn("configKeys", ConfigManager::OWNER_EMAIL);
	registerEnumIn("configKeys", ConfigManager::URL);
	registerEnumIn("configKeys", ConfigManager::LOCATION);
	registerEnumIn("configKeys", ConfigManager::IP);
	registerEnumIn("configKeys", ConfigManager::MOTD);
	registerEnumIn("configKeys", ConfigManager::WORLD_TYPE);
	registerEnumIn("configKeys", ConfigManager::MYSQL_HOST);
	registerEnumIn("configKeys", ConfigManager::MYSQL_USER);
	registerEnumIn("configKeys", ConfigManager::MYSQL_PASS);
	registerEnumIn("configKeys", ConfigManager::MYSQL_DB);
	registerEnumIn("configKeys", ConfigManager::MYSQL_SOCK);
	registerEnumIn("configKeys", ConfigManager::DEFAULT_PRIORITY);
	registerEnumIn("configKeys", ConfigManager::MAP_AUTHOR);
	registerEnumIn("configKeys", ConfigManager::AUGMENT_CRITICAL_ANIMATION);
	registerEnumIn("configKeys", ConfigManager::ACCOUNT_MANAGER_AUTH);
	registerEnumIn("configKeys", ConfigManager::ENABLE_ACCOUNT_MANAGER);
	registerEnumIn("configKeys", ConfigManager::ENABLE_NO_PASS_LOGIN);
	registerEnumIn("configKeys", ConfigManager::ACCOUNT_MANAGER_POS_X);
	registerEnumIn("configKeys", ConfigManager::ACCOUNT_MANAGER_POS_Y);
	registerEnumIn("configKeys", ConfigManager::ACCOUNT_MANAGER_POS_Z);


	registerEnumIn("configKeys", ConfigManager::SQL_PORT);
	registerEnumIn("configKeys", ConfigManager::MAX_PLAYERS);
	registerEnumIn("configKeys", ConfigManager::PZ_LOCKED);
	registerEnumIn("configKeys", ConfigManager::DEFAULT_DESPAWNRANGE);
	registerEnumIn("configKeys", ConfigManager::DEFAULT_DESPAWNRADIUS);
	registerEnumIn("configKeys", ConfigManager::DEFAULT_WALKTOSPAWNRADIUS);
	registerEnumIn("configKeys", ConfigManager::REMOVE_ON_DESPAWN);
	registerEnumIn("configKeys", ConfigManager::RATE_EXPERIENCE);
	registerEnumIn("configKeys", ConfigManager::RATE_SKILL);
	registerEnumIn("configKeys", ConfigManager::RATE_LOOT);
	registerEnumIn("configKeys", ConfigManager::RATE_MAGIC);
	registerEnumIn("configKeys", ConfigManager::RATE_SPAWN);
	registerEnumIn("configKeys", ConfigManager::HOUSE_PRICE);
	registerEnumIn("configKeys", ConfigManager::KILLS_TO_RED);
	registerEnumIn("configKeys", ConfigManager::KILLS_TO_BLACK);
	registerEnumIn("configKeys", ConfigManager::MAX_MESSAGEBUFFER);
	registerEnumIn("configKeys", ConfigManager::ACTIONS_DELAY_INTERVAL);
	registerEnumIn("configKeys", ConfigManager::EX_ACTIONS_DELAY_INTERVAL);
	registerEnumIn("configKeys", ConfigManager::KICK_AFTER_MINUTES);
	registerEnumIn("configKeys", ConfigManager::PROTECTION_LEVEL);
	registerEnumIn("configKeys", ConfigManager::DEATH_LOSE_PERCENT);
	registerEnumIn("configKeys", ConfigManager::STATUSQUERY_TIMEOUT);
	registerEnumIn("configKeys", ConfigManager::FRAG_TIME);
	registerEnumIn("configKeys", ConfigManager::WHITE_SKULL_TIME);
	registerEnumIn("configKeys", ConfigManager::GAME_PORT);
	registerEnumIn("configKeys", ConfigManager::LOGIN_PORT);
	registerEnumIn("configKeys", ConfigManager::STATUS_PORT);
	registerEnumIn("configKeys", ConfigManager::STAIRHOP_DELAY);
	registerEnumIn("configKeys", ConfigManager::MARKET_OFFER_DURATION);
	registerEnumIn("configKeys", ConfigManager::CHECK_EXPIRED_MARKET_OFFERS_EACH_MINUTES);
	registerEnumIn("configKeys", ConfigManager::MAX_MARKET_OFFERS_AT_A_TIME_PER_PLAYER);
	registerEnumIn("configKeys", ConfigManager::EXP_FROM_PLAYERS_LEVEL_RANGE);
	registerEnumIn("configKeys", ConfigManager::MAX_PACKETS_PER_SECOND);
	registerEnumIn("configKeys", ConfigManager::PLAYER_CONSOLE_LOGS);
	registerEnumIn("configKeys", ConfigManager::BED_OFFLINE_TRAINING);	
	registerEnumIn("configKeys", ConfigManager::HOUSE_DOOR_SHOW_PRICE);
	registerEnumIn("configKeys", ConfigManager::AUGMENT_SLOT_PROTECTION);
	registerEnumIn("configKeys", ConfigManager::AUGMENT_STAMINA_RULE);

	registerEnumIn("configKeys", ConfigManager::REWARD_BASE_RATE);
	registerEnumIn("configKeys", ConfigManager::REWARD_RATE_DAMAGE_DONE);
	registerEnumIn("configKeys", ConfigManager::REWARD_RATE_DAMAGE_TAKEN);
	registerEnumIn("configKeys", ConfigManager::REWARD_RATE_HEALING_DONE);

	// os
	registerMethod("os", "mtime", LuaScriptInterface::luaSystemTime);

	// table
	registerMethod("table", "create", LuaScriptInterface::luaTableCreate);
	registerMethod("table", "pack", LuaScriptInterface::luaTablePack);

	// DB Insert
	registerClass("DBInsert", "", LuaScriptInterface::luaDBInsertCreate);
	registerMetaMethod("DBInsert", "__gc", LuaScriptInterface::luaDBInsertDelete);

	registerMethod("DBInsert", "addRow", LuaScriptInterface::luaDBInsertAddRow);
	registerMethod("DBInsert", "execute", LuaScriptInterface::luaDBInsertExecute);

	// DB Transaction
	registerClass("DBTransaction", "", LuaScriptInterface::luaDBTransactionCreate);
	registerMetaMethod("DBTransaction", "__eq", LuaScriptInterface::luaUserdataCompare);
	registerMetaMethod("DBTransaction", "__gc", LuaScriptInterface::luaDBTransactionDelete);

	registerMethod("DBTransaction", "begin", LuaScriptInterface::luaDBTransactionBegin);
	registerMethod("DBTransaction", "commit", LuaScriptInterface::luaDBTransactionCommit);
	registerMethod("DBTransaction", "rollback", LuaScriptInterface::luaDBTransactionDelete);

	// Game
	registerTable("Game");

	registerMethod("Game", "getSpectators", LuaScriptInterface::luaGameGetSpectators);
	registerMethod("Game", "getPlayers", LuaScriptInterface::luaGameGetPlayers);
	registerMethod("Game", "getNpcs", LuaScriptInterface::luaGameGetNpcs);
	registerMethod("Game", "getMonsters", LuaScriptInterface::luaGameGetMonsters);
	registerMethod("Game", "loadMap", LuaScriptInterface::luaGameLoadMap);

	registerMethod("Game", "getExperienceStage", LuaScriptInterface::luaGameGetExperienceStage);
	registerMethod("Game", "getExperienceForLevel", LuaScriptInterface::luaGameGetExperienceForLevel);
	registerMethod("Game", "getMonsterCount", LuaScriptInterface::luaGameGetMonsterCount);
	registerMethod("Game", "getPlayerCount", LuaScriptInterface::luaGameGetPlayerCount);
	registerMethod("Game", "getNpcCount", LuaScriptInterface::luaGameGetNpcCount);
	registerMethod("Game", "getMonsterTypes", LuaScriptInterface::luaGameGetMonsterTypes);
	registerMethod("Game", "getCurrencyItems", LuaScriptInterface::luaGameGetCurrencyItems);

	registerMethod("Game", "getTowns", LuaScriptInterface::luaGameGetTowns);
	registerMethod("Game", "getHouses", LuaScriptInterface::luaGameGetHouses);
	registerMethod("Game", "getOutfits", LuaScriptInterface::luaGameGetOutfits);
	registerMethod("Game", "getMounts", LuaScriptInterface::luaGameGetMounts);
	registerMethod("Game", "getVocations", LuaScriptInterface::luaGameGetVocations);

	registerMethod("Game", "getGameState", LuaScriptInterface::luaGameGetGameState);
	registerMethod("Game", "setGameState", LuaScriptInterface::luaGameSetGameState);

	registerMethod("Game", "getWorldType", LuaScriptInterface::luaGameGetWorldType);
	registerMethod("Game", "setWorldType", LuaScriptInterface::luaGameSetWorldType);

	registerMethod("Game", "getItemAttributeByName", LuaScriptInterface::luaGameGetItemAttributeByName);
	registerMethod("Game", "getReturnMessage", LuaScriptInterface::luaGameGetReturnMessage);

	registerMethod("Game", "createItem", LuaScriptInterface::luaGameCreateItem);
	registerMethod("Game", "createContainer", LuaScriptInterface::luaGameCreateContainer);
	registerMethod("Game", "createMonster", LuaScriptInterface::luaGameCreateMonster);
	registerMethod("Game", "createNpc", LuaScriptInterface::luaGameCreateNpc);
	registerMethod("Game", "createTile", LuaScriptInterface::luaGameCreateTile);
	registerMethod("Game", "createMonsterType", LuaScriptInterface::luaGameCreateMonsterType);

	registerMethod("Game", "startRaid", LuaScriptInterface::luaGameStartRaid);

	registerMethod("Game", "getClientVersion", LuaScriptInterface::luaGameGetClientVersion);

	registerMethod("Game", "reload", LuaScriptInterface::luaGameReload);

	registerMethod("Game", "getAccountStorageValue", LuaScriptInterface::luaGameGetAccountStorageValue);
	registerMethod("Game", "setAccountStorageValue", LuaScriptInterface::luaGameSetAccountStorageValue);
	registerMethod("Game", "saveAccountStorageValues", LuaScriptInterface::luaGameSaveAccountStorageValues);

	registerMethod("Game", "sendDiscordMessage", LuaScriptInterface::luaGameSendDiscordWebhook);

	// Variant
	registerClass("Variant", "", LuaScriptInterface::luaVariantCreate);

	registerMethod("Variant", "getNumber", LuaScriptInterface::luaVariantGetNumber);
	registerMethod("Variant", "getString", LuaScriptInterface::luaVariantGetString);
	registerMethod("Variant", "getPosition", LuaScriptInterface::luaVariantGetPosition);

	// Position
	registerClass("Position", "", LuaScriptInterface::luaPositionCreate);
	registerMetaMethod("Position", "__add", LuaScriptInterface::luaPositionAdd);
	registerMetaMethod("Position", "__sub", LuaScriptInterface::luaPositionSub);
	registerMetaMethod("Position", "__eq", LuaScriptInterface::luaPositionCompare);

	registerMethod("Position", "getDistance", LuaScriptInterface::luaPositionGetDistance);
	registerMethod("Position", "isSightClear", LuaScriptInterface::luaPositionIsSightClear);

	registerMethod("Position", "sendMagicEffect", LuaScriptInterface::luaPositionSendMagicEffect);
	registerMethod("Position", "sendDistanceEffect", LuaScriptInterface::luaPositionSendDistanceEffect);
	registerMethod("Position", "getZones()", LuaScriptInterface::luaPositionGetZones);
	registerMethod("Position", "hasZone()", LuaScriptInterface::luaPositionHasZone);

	// Tile
	registerClass("Tile", "", LuaScriptInterface::luaTileCreate);
	registerMetaMethod("Tile", "__eq", LuaScriptInterface::luaUserdataCompare);
	registerMetaMethod("Tile", "__gc", LuaScriptInterface::luaTileDelete);

	registerMethod("Tile", "remove", LuaScriptInterface::luaTileRemove);

	registerMethod("Tile", "getPosition", LuaScriptInterface::luaTileGetPosition);
	registerMethod("Tile", "getGround", LuaScriptInterface::luaTileGetGround);
	registerMethod("Tile", "getThing", LuaScriptInterface::luaTileGetThing);
	registerMethod("Tile", "getThingCount", LuaScriptInterface::luaTileGetThingCount);
	registerMethod("Tile", "getTopVisibleThing", LuaScriptInterface::luaTileGetTopVisibleThing);

	registerMethod("Tile", "getTopTopItem", LuaScriptInterface::luaTileGetTopTopItem);
	registerMethod("Tile", "getTopDownItem", LuaScriptInterface::luaTileGetTopDownItem);
	registerMethod("Tile", "getFieldItem", LuaScriptInterface::luaTileGetFieldItem);

	registerMethod("Tile", "getItemById", LuaScriptInterface::luaTileGetItemById);
	registerMethod("Tile", "getItemByType", LuaScriptInterface::luaTileGetItemByType);
	registerMethod("Tile", "getItemByTopOrder", LuaScriptInterface::luaTileGetItemByTopOrder);
	registerMethod("Tile", "getItemCountById", LuaScriptInterface::luaTileGetItemCountById);

	registerMethod("Tile", "getBottomCreature", LuaScriptInterface::luaTileGetBottomCreature);
	registerMethod("Tile", "getTopCreature", LuaScriptInterface::luaTileGetTopCreature);
	registerMethod("Tile", "getBottomVisibleCreature", LuaScriptInterface::luaTileGetBottomVisibleCreature);
	registerMethod("Tile", "getTopVisibleCreature", LuaScriptInterface::luaTileGetTopVisibleCreature);

	registerMethod("Tile", "getItems", LuaScriptInterface::luaTileGetItems);
	registerMethod("Tile", "getItemCount", LuaScriptInterface::luaTileGetItemCount);
	registerMethod("Tile", "getDownItemCount", LuaScriptInterface::luaTileGetDownItemCount);
	registerMethod("Tile", "getTopItemCount", LuaScriptInterface::luaTileGetTopItemCount);

	registerMethod("Tile", "getCreatures", LuaScriptInterface::luaTileGetCreatures);
	registerMethod("Tile", "getCreatureCount", LuaScriptInterface::luaTileGetCreatureCount);

	registerMethod("Tile", "getThingIndex", LuaScriptInterface::luaTileGetThingIndex);

	registerMethod("Tile", "hasProperty", LuaScriptInterface::luaTileHasProperty);
	registerMethod("Tile", "hasFlag", LuaScriptInterface::luaTileHasFlag);

	registerMethod("Tile", "addItem", LuaScriptInterface::luaTileAddItem);
	registerMethod("Tile", "addItemEx", LuaScriptInterface::luaTileAddItemEx);

	registerMethod("Tile", "getHouse", LuaScriptInterface::luaTileGetHouse);

	// NetworkMessage
	registerClass("NetworkMessage", "", LuaScriptInterface::luaNetworkMessageCreate);
	registerMetaMethod("NetworkMessage", "__eq", LuaScriptInterface::luaUserdataCompare);
	registerMetaMethod("NetworkMessage", "__gc", LuaScriptInterface::luaNetworkMessageDelete);
	registerMethod("NetworkMessage", "delete", LuaScriptInterface::luaNetworkMessageDelete);

	registerMethod("NetworkMessage", "getByte", LuaScriptInterface::luaNetworkMessageGetByte);
	registerMethod("NetworkMessage", "getU16", LuaScriptInterface::luaNetworkMessageGetU16);
	registerMethod("NetworkMessage", "getU32", LuaScriptInterface::luaNetworkMessageGetU32);
	registerMethod("NetworkMessage", "getU64", LuaScriptInterface::luaNetworkMessageGetU64);
	registerMethod("NetworkMessage", "getString", LuaScriptInterface::luaNetworkMessageGetString);
	registerMethod("NetworkMessage", "getPosition", LuaScriptInterface::luaNetworkMessageGetPosition);

	registerMethod("NetworkMessage", "addByte", LuaScriptInterface::luaNetworkMessageAddByte);
	registerMethod("NetworkMessage", "addU16", LuaScriptInterface::luaNetworkMessageAddU16);
	registerMethod("NetworkMessage", "addU32", LuaScriptInterface::luaNetworkMessageAddU32);
	registerMethod("NetworkMessage", "addU64", LuaScriptInterface::luaNetworkMessageAddU64);
	registerMethod("NetworkMessage", "addString", LuaScriptInterface::luaNetworkMessageAddString);
	registerMethod("NetworkMessage", "addPosition", LuaScriptInterface::luaNetworkMessageAddPosition);
	registerMethod("NetworkMessage", "addDouble", LuaScriptInterface::luaNetworkMessageAddDouble);
	registerMethod("NetworkMessage", "addItem", LuaScriptInterface::luaNetworkMessageAddItem);
	registerMethod("NetworkMessage", "addItemId", LuaScriptInterface::luaNetworkMessageAddItemId);

	registerMethod("NetworkMessage", "reset", LuaScriptInterface::luaNetworkMessageReset);
	registerMethod("NetworkMessage", "seek", LuaScriptInterface::luaNetworkMessageSeek);
	registerMethod("NetworkMessage", "tell", LuaScriptInterface::luaNetworkMessageTell);
	registerMethod("NetworkMessage", "len", LuaScriptInterface::luaNetworkMessageLength);
	registerMethod("NetworkMessage", "skipBytes", LuaScriptInterface::luaNetworkMessageSkipBytes);
	registerMethod("NetworkMessage", "sendToPlayer", LuaScriptInterface::luaNetworkMessageSendToPlayer);

	// ModalWindow
	registerClass("ModalWindow", "", LuaScriptInterface::luaModalWindowCreate);
	registerMetaMethod("ModalWindow", "__eq", LuaScriptInterface::luaUserdataCompare);
	registerMetaMethod("ModalWindow", "__gc", LuaScriptInterface::luaModalWindowDelete);
	registerMethod("ModalWindow", "delete", LuaScriptInterface::luaModalWindowDelete);

	registerMethod("ModalWindow", "getId", LuaScriptInterface::luaModalWindowGetId);
	registerMethod("ModalWindow", "getTitle", LuaScriptInterface::luaModalWindowGetTitle);
	registerMethod("ModalWindow", "getMessage", LuaScriptInterface::luaModalWindowGetMessage);

	registerMethod("ModalWindow", "setTitle", LuaScriptInterface::luaModalWindowSetTitle);
	registerMethod("ModalWindow", "setMessage", LuaScriptInterface::luaModalWindowSetMessage);

	registerMethod("ModalWindow", "getButtonCount", LuaScriptInterface::luaModalWindowGetButtonCount);
	registerMethod("ModalWindow", "getChoiceCount", LuaScriptInterface::luaModalWindowGetChoiceCount);

	registerMethod("ModalWindow", "addButton", LuaScriptInterface::luaModalWindowAddButton);
	registerMethod("ModalWindow", "addChoice", LuaScriptInterface::luaModalWindowAddChoice);

	registerMethod("ModalWindow", "getDefaultEnterButton", LuaScriptInterface::luaModalWindowGetDefaultEnterButton);
	registerMethod("ModalWindow", "setDefaultEnterButton", LuaScriptInterface::luaModalWindowSetDefaultEnterButton);

	registerMethod("ModalWindow", "getDefaultEscapeButton", LuaScriptInterface::luaModalWindowGetDefaultEscapeButton);
	registerMethod("ModalWindow", "setDefaultEscapeButton", LuaScriptInterface::luaModalWindowSetDefaultEscapeButton);

	registerMethod("ModalWindow", "hasPriority", LuaScriptInterface::luaModalWindowHasPriority);
	registerMethod("ModalWindow", "setPriority", LuaScriptInterface::luaModalWindowSetPriority);

	registerMethod("ModalWindow", "sendToPlayer", LuaScriptInterface::luaModalWindowSendToPlayer);

	// Item
	registerClass("Item", "", LuaScriptInterface::luaItemCreate);
	registerMetaMethod("Item", "__eq", LuaScriptInterface::luaUserdataCompare);
	registerMetaMethod("Item", "__gc", LuaScriptInterface::luaItemDelete);

	registerMethod("Item", "isItem", LuaScriptInterface::luaItemIsItem);

	registerMethod("Item", "getParent", LuaScriptInterface::luaItemGetParent);
	registerMethod("Item", "getTopParent", LuaScriptInterface::luaItemGetTopParent);

	registerMethod("Item", "getId", LuaScriptInterface::luaItemGetId);

	registerMethod("Item", "clone", LuaScriptInterface::luaItemClone);
	registerMethod("Item", "split", LuaScriptInterface::luaItemSplit);
	registerMethod("Item", "remove", LuaScriptInterface::luaItemRemove);

	registerMethod("Item", "getUniqueId", LuaScriptInterface::luaItemGetUniqueId);
	registerMethod("Item", "getActionId", LuaScriptInterface::luaItemGetActionId);
	registerMethod("Item", "setActionId", LuaScriptInterface::luaItemSetActionId);

	registerMethod("Item", "getCount", LuaScriptInterface::luaItemGetCount);
	registerMethod("Item", "getCharges", LuaScriptInterface::luaItemGetCharges);
	registerMethod("Item", "getFluidType", LuaScriptInterface::luaItemGetFluidType);
	registerMethod("Item", "getWeight", LuaScriptInterface::luaItemGetWeight);
	registerMethod("Item", "getWorth", LuaScriptInterface::luaItemGetWorth);

	registerMethod("Item", "getSubType", LuaScriptInterface::luaItemGetSubType);

	registerMethod("Item", "getName", LuaScriptInterface::luaItemGetName);
	registerMethod("Item", "getPluralName", LuaScriptInterface::luaItemGetPluralName);
	registerMethod("Item", "getArticle", LuaScriptInterface::luaItemGetArticle);

	registerMethod("Item", "getPosition", LuaScriptInterface::luaItemGetPosition);
	registerMethod("Item", "getTile", LuaScriptInterface::luaItemGetTile);

	registerMethod("Item", "hasAttribute", LuaScriptInterface::luaItemHasAttribute);
	registerMethod("Item", "getAttribute", LuaScriptInterface::luaItemGetAttribute);
	registerMethod("Item", "setAttribute", LuaScriptInterface::luaItemSetAttribute);
	registerMethod("Item", "removeAttribute", LuaScriptInterface::luaItemRemoveAttribute);
	registerMethod("Item", "getCustomAttribute", LuaScriptInterface::luaItemGetCustomAttribute);
	registerMethod("Item", "setCustomAttribute", LuaScriptInterface::luaItemSetCustomAttribute);
	registerMethod("Item", "removeCustomAttribute", LuaScriptInterface::luaItemRemoveCustomAttribute);

	registerMethod("Item", "moveTo", LuaScriptInterface::luaItemMoveTo);
	registerMethod("Item", "transform", LuaScriptInterface::luaItemTransform);
	registerMethod("Item", "decay", LuaScriptInterface::luaItemDecay);

	registerMethod("Item", "getDescription", LuaScriptInterface::luaItemGetDescription);
	registerMethod("Item", "getSpecialDescription", LuaScriptInterface::luaItemGetSpecialDescription);

	registerMethod("Item", "hasProperty", LuaScriptInterface::luaItemHasProperty);
	registerMethod("Item", "isLoadedFromMap", LuaScriptInterface::luaItemIsLoadedFromMap);

	registerMethod("Item", "setStoreItem", LuaScriptInterface::luaItemSetStoreItem);
	registerMethod("Item", "isStoreItem", LuaScriptInterface::luaItemIsStoreItem);

	registerMethod("Item", "getImbuementSlots", LuaScriptInterface::luaItemGetImbuementSlots);
	registerMethod("Item", "getFreeImbuementSlots", LuaScriptInterface::luaItemGetFreeImbuementSlots);
	registerMethod("Item", "canImbue", LuaScriptInterface::luaItemCanImbue);
	registerMethod("Item", "addImbuementSlots", LuaScriptInterface::luaItemAddImbuementSlots);
	registerMethod("Item", "removeImbuementSlots", LuaScriptInterface::luaItemRemoveImbuementSlots);
	registerMethod("Item", "hasImbuementType", LuaScriptInterface::luaItemHasImbuementType);
	registerMethod("Item", "hasImbuements", LuaScriptInterface::luaItemHasImbuements); // change to isImbued
	registerMethod("Item", "addImbuement", LuaScriptInterface::luaItemAddImbuement);
	registerMethod("Item", "removeImbuement", LuaScriptInterface::luaItemRemoveImbuement);
	registerMethod("Item", "getImbuements", LuaScriptInterface::luaItemGetImbuements);

	registerMethod("Item", "addAugment", LuaScriptInterface::luaItemAddAugment);
	registerMethod("Item", "removeAugment", LuaScriptInterface::luaItemRemoveAugment);
	registerMethod("Item", "isAugmented", LuaScriptInterface::luaItemIsAugmented);
	registerMethod("Item", "hasAugment", LuaScriptInterface::luaItemHasAugment);
	registerMethod("Item", "getAugments", LuaScriptInterface::luaItemGetAugments);

	registerMethod("Item", "giveSkill", LuaScriptInterface::luaItemGiveCustomSkill);
	registerMethod("Item", "addSkill", LuaScriptInterface::luaItemAddCustomSkill);
	registerMethod("Item", "subtractSkill", LuaScriptInterface::luaItemSubtractCustomSkill);
	registerMethod("Item", "addBonusSkill", LuaScriptInterface::luaItemAddBonusCustomSkill);
	registerMethod("Item", "subtractBonusSkill", LuaScriptInterface::luaItemSubtractBonusSkill);
	registerMethod("Item", "clearBonusSkill", LuaScriptInterface::luaItemClearBonusSkill);
	registerMethod("Item", "removeSkill", LuaScriptInterface::luaItemRemoveCustomSkill);
	registerMethod("Item", "hasSkill", LuaScriptInterface::luaItemHasCustomSkill);
	registerMethod("Item", "canSkill", LuaScriptInterface::luaItemCanGainSkillLevels);
	registerMethod("Item", "getSkill", LuaScriptInterface::luaItemGetCustomSkillLevel);
	registerMethod("Item", "skill", LuaScriptInterface::luaItemGetCustomSkillUserData);

	// Imbuement
	registerClass("Imbuement", "", LuaScriptInterface::luaImbuementCreate);
	registerMetaMethod("Imbuement", "__eq", LuaScriptInterface::luaUserdataCompare);
	registerMethod("Imbuement", "getType", LuaScriptInterface::luaImbuementGetType);
	registerMethod("Imbuement", "isSkill", LuaScriptInterface::luaImbuementIsSkill);
	registerMethod("Imbuement", "isSpecialSkill", LuaScriptInterface::luaImbuementIsSpecialSkill);
	registerMethod("Imbuement", "isStat", LuaScriptInterface::luaImbuementIsStat);
	registerMethod("Imbuement", "isDamage", LuaScriptInterface::luaImbuementIsDamage);
	registerMethod("Imbuement", "isResist", LuaScriptInterface::luaImbuementIsResist);
	registerMethod("Imbuement", "getValue", LuaScriptInterface::luaImbuementGetValue);
	registerMethod("Imbuement", "setValue", LuaScriptInterface::luaImbuementSetValue);
	registerMethod("Imbuement", "getDuration", LuaScriptInterface::luaImbuementGetDuration);
	registerMethod("Imbuement", "setDuration", LuaScriptInterface::luaImbuementSetDuration);
	registerMethod("Imbuement", "makeEquipDecayed", LuaScriptInterface::luaImbuementSetEquipDecay);
	registerMethod("Imbuement", "makeInfightDecayed", LuaScriptInterface::luaImbuementSetInfightDecay);
	registerMethod("Imbuement", "isEquipDecayed", LuaScriptInterface::luaImbuementIsEquipDecay);
	registerMethod("Imbuement", "isInfightDecayed", LuaScriptInterface::luaImbuementIsInfightDecay);

	// DamageModifer
	registerClass("DamageModifier", "", LuaScriptInterface::luaDamageModifierCreate);
	registerMetaMethod("DamageModifier", "__eq", LuaScriptInterface::luaUserdataCompare);
	registerMethod("DamageModifier", "setType", LuaScriptInterface::luaDamageModifierSetType);
	registerMethod("DamageModifier", "setStance", LuaScriptInterface::luaDamageModifierSetStance);
	registerMethod("DamageModifier", "setValue", LuaScriptInterface::luaDamageModifierSetValue);
	registerMethod("DamageModifier", "setChance", LuaScriptInterface::luaDamageModifierSetChance);
	registerMethod("DamageModifier", "setFactor", LuaScriptInterface::luaDamageModifierSetRateFactor);
	registerMethod("DamageModifier", "setCombat", LuaScriptInterface::luaDamageModifierSetCombatFilter);
	registerMethod("DamageModifier", "setOrigin", LuaScriptInterface::luaDamageModifierSetOriginFilter);
	registerMethod("DamageModifier", "setCreatureType", LuaScriptInterface::luaDamageModifierSetCreatureTypeFilter);
	registerMethod("DamageModifier", "setRace", LuaScriptInterface::luaDamageModifierSetRaceFilter);
	registerMethod("DamageModifier", "setCreatureName", LuaScriptInterface::luaDamageModifierSetCreatureName);


	// Augment
	registerClass("Augment", "", LuaScriptInterface::luaAugmentCreate);
	registerMetaMethod("Augment", "__eq", LuaScriptInterface::luaUserdataCompare);
	registerMethod("Augment", "register", LuaScriptInterface::luaAugmentRegister);
	registerMethod("Augment", "setName", LuaScriptInterface::luaAugmentSetName);
	registerMethod("Augment", "setDescription", LuaScriptInterface::luaAugmentSetDescription);
	registerMethod("Augment", "getName", LuaScriptInterface::luaAugmentGetName);
	registerMethod("Augment", "getDescription", LuaScriptInterface::luaAugmentGetDescription);
	registerMethod("Augment", "addDamageModifier", LuaScriptInterface::luaAugmentAddDamageModifier);
	registerMethod("Augment", "removeDamageModifier", LuaScriptInterface::luaAugmentRemoveDamageModifier);
	registerMethod("Augment", "getAttackModifiers", LuaScriptInterface::luaAugmentGetDefenseModifiers);
	registerMethod("Augment", "getDefenseModifiers", LuaScriptInterface::luaAugmentGetDefenseModifiers);

	// Container
	registerClass("Container", "Item", LuaScriptInterface::luaContainerCreate);
	registerMetaMethod("Container", "__eq", LuaScriptInterface::luaUserdataCompare);
	registerMetaMethod("Container", "__gc", LuaScriptInterface::luaContainerDelete);

	registerMethod("Container", "getSize", LuaScriptInterface::luaContainerGetSize);
	registerMethod("Container", "getCapacity", LuaScriptInterface::luaContainerGetCapacity);
	registerMethod("Container", "getEmptySlots", LuaScriptInterface::luaContainerGetEmptySlots);
	registerMethod("Container", "getContentDescription", LuaScriptInterface::luaContainerGetContentDescription);
	registerMethod("Container", "getItems", LuaScriptInterface::luaContainerGetItems);
	registerMethod("Container", "getItemHoldingCount", LuaScriptInterface::luaContainerGetItemHoldingCount);
	registerMethod("Container", "getItemCountById", LuaScriptInterface::luaContainerGetItemCountById);

	registerMethod("Container", "getItem", LuaScriptInterface::luaContainerGetItem);
	registerMethod("Container", "hasItem", LuaScriptInterface::luaContainerHasItem);
	registerMethod("Container", "addItem", LuaScriptInterface::luaContainerAddItem);
	registerMethod("Container", "addItemEx", LuaScriptInterface::luaContainerAddItemEx);
	registerMethod("Container", "getCorpseOwner", LuaScriptInterface::luaContainerGetCorpseOwner);

	// Teleport
	registerClass("Teleport", "Item", LuaScriptInterface::luaTeleportCreate);
	registerMetaMethod("Teleport", "__eq", LuaScriptInterface::luaUserdataCompare);
	registerMetaMethod("Teleport", "__gc", LuaScriptInterface::luaTeleportDelete);

	registerMethod("Teleport", "getDestination", LuaScriptInterface::luaTeleportGetDestination);
	registerMethod("Teleport", "setDestination", LuaScriptInterface::luaTeleportSetDestination);

	// Creature
	registerClass("Creature", "", LuaScriptInterface::luaCreatureCreate);
	registerMetaMethod("Creature", "__eq", LuaScriptInterface::luaUserdataCompare);
	registerMetaMethod("Creature", "__gc", LuaScriptInterface::luaCreatureDelete);

	registerMethod("Creature", "getEvents", LuaScriptInterface::luaCreatureGetEvents);
	registerMethod("Creature", "registerEvent", LuaScriptInterface::luaCreatureRegisterEvent);
	registerMethod("Creature", "unregisterEvent", LuaScriptInterface::luaCreatureUnregisterEvent);

	registerMethod("Creature", "isRemoved", LuaScriptInterface::luaCreatureIsRemoved);
	registerMethod("Creature", "isCreature", LuaScriptInterface::luaCreatureIsCreature);
	registerMethod("Creature", "isInGhostMode", LuaScriptInterface::luaCreatureIsInGhostMode);
	registerMethod("Creature", "isHealthHidden", LuaScriptInterface::luaCreatureIsHealthHidden);
	registerMethod("Creature", "isMovementBlocked", LuaScriptInterface::luaCreatureIsMovementBlocked);
	registerMethod("Creature", "isImmune", LuaScriptInterface::luaCreatureIsImmune);

	registerMethod("Creature", "canSee", LuaScriptInterface::luaCreatureCanSee);
	registerMethod("Creature", "canSeeCreature", LuaScriptInterface::luaCreatureCanSeeCreature);
	registerMethod("Creature", "canSeeGhostMode", LuaScriptInterface::luaCreatureCanSeeGhostMode);
	registerMethod("Creature", "canSeeInvisibility", LuaScriptInterface::luaCreatureCanSeeInvisibility);

	registerMethod("Creature", "getParent", LuaScriptInterface::luaCreatureGetParent);

	registerMethod("Creature", "getId", LuaScriptInterface::luaCreatureGetId);
	registerMethod("Creature", "getName", LuaScriptInterface::luaCreatureGetName);

	registerMethod("Creature", "getTarget", LuaScriptInterface::luaCreatureGetTarget);
	registerMethod("Creature", "setTarget", LuaScriptInterface::luaCreatureSetTarget);

	registerMethod("Creature", "getFollowCreature", LuaScriptInterface::luaCreatureGetFollowCreature);
	registerMethod("Creature", "setFollowCreature", LuaScriptInterface::luaCreatureSetFollowCreature);

	registerMethod("Creature", "getMaster", LuaScriptInterface::luaCreatureGetMaster);
	registerMethod("Creature", "setMaster", LuaScriptInterface::luaCreatureSetMaster);

	registerMethod("Creature", "getLight", LuaScriptInterface::luaCreatureGetLight);
	registerMethod("Creature", "setLight", LuaScriptInterface::luaCreatureSetLight);

	registerMethod("Creature", "getSpeed", LuaScriptInterface::luaCreatureGetSpeed);
	registerMethod("Creature", "getBaseSpeed", LuaScriptInterface::luaCreatureGetBaseSpeed);
	registerMethod("Creature", "changeSpeed", LuaScriptInterface::luaCreatureChangeSpeed);

	registerMethod("Creature", "setDropLoot", LuaScriptInterface::luaCreatureSetDropLoot);
	registerMethod("Creature", "setSkillLoss", LuaScriptInterface::luaCreatureSetSkillLoss);

	registerMethod("Creature", "getPosition", LuaScriptInterface::luaCreatureGetPosition);
	registerMethod("Creature", "getTile", LuaScriptInterface::luaCreatureGetTile);
	registerMethod("Creature", "getDirection", LuaScriptInterface::luaCreatureGetDirection);
	registerMethod("Creature", "setDirection", LuaScriptInterface::luaCreatureSetDirection);

	registerMethod("Creature", "getHealth", LuaScriptInterface::luaCreatureGetHealth);
	registerMethod("Creature", "setHealth", LuaScriptInterface::luaCreatureSetHealth);
	registerMethod("Creature", "addHealth", LuaScriptInterface::luaCreatureAddHealth);
	registerMethod("Creature", "getMaxHealth", LuaScriptInterface::luaCreatureGetMaxHealth);
	registerMethod("Creature", "setMaxHealth", LuaScriptInterface::luaCreatureSetMaxHealth);
	registerMethod("Creature", "setHiddenHealth", LuaScriptInterface::luaCreatureSetHiddenHealth);
	registerMethod("Creature", "setMovementBlocked", LuaScriptInterface::luaCreatureSetMovementBlocked);

	registerMethod("Creature", "getSkull", LuaScriptInterface::luaCreatureGetSkull);
	registerMethod("Creature", "setSkull", LuaScriptInterface::luaCreatureSetSkull);

	registerMethod("Creature", "getOutfit", LuaScriptInterface::luaCreatureGetOutfit);
	registerMethod("Creature", "setOutfit", LuaScriptInterface::luaCreatureSetOutfit);

	registerMethod("Creature", "getCondition", LuaScriptInterface::luaCreatureGetCondition);
	registerMethod("Creature", "addCondition", LuaScriptInterface::luaCreatureAddCondition);
	registerMethod("Creature", "removeCondition", LuaScriptInterface::luaCreatureRemoveCondition);
	registerMethod("Creature", "hasCondition", LuaScriptInterface::luaCreatureHasCondition);

	registerMethod("Creature", "remove", LuaScriptInterface::luaCreatureRemove);
	registerMethod("Creature", "teleportTo", LuaScriptInterface::luaCreatureTeleportTo);
	registerMethod("Creature", "say", LuaScriptInterface::luaCreatureSay);

	registerMethod("Creature", "getDamageMap", LuaScriptInterface::luaCreatureGetDamageMap);

	registerMethod("Creature", "getSummons", LuaScriptInterface::luaCreatureGetSummons);

	registerMethod("Creature", "getDescription", LuaScriptInterface::luaCreatureGetDescription);

	registerMethod("Creature", "getPathTo", LuaScriptInterface::luaCreatureGetPathTo);
	registerMethod("Creature", "move", LuaScriptInterface::luaCreatureMove);

	registerMethod("Creature", "getZone", LuaScriptInterface::luaCreatureGetZone);

	registerMethod("Creature", "giveSkill", LuaScriptInterface::luaCreatureGiveCustomSkill);
	registerMethod("Creature", "addSkill", LuaScriptInterface::luaCreatureAddCustomSkill);
	registerMethod("Creature", "subtractSkill", LuaScriptInterface::luaCreatureSubtractCustomSkill);
	registerMethod("Creature", "addBonusSkill", LuaScriptInterface::luaCreatureAddBonusCustomSkill);
	registerMethod("Creature", "subtractBonusSkill", LuaScriptInterface::luaCreatureSubtractBonusSkill);
	registerMethod("Creature", "clearBonusSkill", LuaScriptInterface::luaCreatureClearBonusSkill);
	registerMethod("Creature", "removeSkill", LuaScriptInterface::luaCreatureRemoveCustomSkill);
	registerMethod("Creature", "hasSkill", LuaScriptInterface::luaCreatureHasCustomSkill);
	registerMethod("Creature", "canSkill", LuaScriptInterface::luaCreatureCanGainSkillLevels);
	registerMethod("Creature", "getSkill", LuaScriptInterface::luaCreatureGetCustomSkillLevel);
	registerMethod("Creature", "skill", LuaScriptInterface::luaCreatureGetCustomSkillUserData);

	// Player
	registerClass("Player", "Creature", LuaScriptInterface::luaPlayerCreate);
	registerMetaMethod("Player", "__eq", LuaScriptInterface::luaUserdataCompare);
	registerMetaMethod("Player", "__gc", LuaScriptInterface::luaPlayerDelete);

	registerMethod("Player", "isPlayer", LuaScriptInterface::luaPlayerIsPlayer);

	registerMethod("Player", "getGuid", LuaScriptInterface::luaPlayerGetGuid);
	registerMethod("Player", "getIp", LuaScriptInterface::luaPlayerGetIp);
	registerMethod("Player", "getAccountId", LuaScriptInterface::luaPlayerGetAccountId);
	registerMethod("Player", "getLastLoginSaved", LuaScriptInterface::luaPlayerGetLastLoginSaved);
	registerMethod("Player", "getLastLogout", LuaScriptInterface::luaPlayerGetLastLogout);

	registerMethod("Player", "getAccountType", LuaScriptInterface::luaPlayerGetAccountType);
	registerMethod("Player", "setAccountType", LuaScriptInterface::luaPlayerSetAccountType);

	registerMethod("Player", "getCapacity", LuaScriptInterface::luaPlayerGetCapacity);
	registerMethod("Player", "setCapacity", LuaScriptInterface::luaPlayerSetCapacity);

	registerMethod("Player", "getFreeCapacity", LuaScriptInterface::luaPlayerGetFreeCapacity);
	registerMethod("Player", "getDepotItemCount", LuaScriptInterface::luaPlayerGetDepotItemCount);

	registerMethod("Player", "getDepotChest", LuaScriptInterface::luaPlayerGetDepotChest);
	registerMethod("Player", "getInbox", LuaScriptInterface::luaPlayerGetInbox);
	registerMethod("Player", "getRewardChest", LuaScriptInterface::luaPlayerGetRewardChest);

	registerMethod("Player", "getSkullTime", LuaScriptInterface::luaPlayerGetSkullTime);
	registerMethod("Player", "setSkullTime", LuaScriptInterface::luaPlayerSetSkullTime);
	registerMethod("Player", "getDeathPenalty", LuaScriptInterface::luaPlayerGetDeathPenalty);

	registerMethod("Player", "getExperience", LuaScriptInterface::luaPlayerGetExperience);
	registerMethod("Player", "addExperience", LuaScriptInterface::luaPlayerAddExperience);
	registerMethod("Player", "removeExperience", LuaScriptInterface::luaPlayerRemoveExperience);
	registerMethod("Player", "getLevel", LuaScriptInterface::luaPlayerGetLevel);

	registerMethod("Player", "getMagicLevel", LuaScriptInterface::luaPlayerGetMagicLevel);
	registerMethod("Player", "getBaseMagicLevel", LuaScriptInterface::luaPlayerGetBaseMagicLevel);
	registerMethod("Player", "getMana", LuaScriptInterface::luaPlayerGetMana);
	registerMethod("Player", "addMana", LuaScriptInterface::luaPlayerAddMana);
	registerMethod("Player", "getMaxMana", LuaScriptInterface::luaPlayerGetMaxMana);
	registerMethod("Player", "setMaxMana", LuaScriptInterface::luaPlayerSetMaxMana);
	registerMethod("Player", "getManaSpent", LuaScriptInterface::luaPlayerGetManaSpent);
	registerMethod("Player", "addManaSpent", LuaScriptInterface::luaPlayerAddManaSpent);
	registerMethod("Player", "removeManaSpent", LuaScriptInterface::luaPlayerRemoveManaSpent);

	registerMethod("Player", "getBaseMaxHealth", LuaScriptInterface::luaPlayerGetBaseMaxHealth);
	registerMethod("Player", "getBaseMaxMana", LuaScriptInterface::luaPlayerGetBaseMaxMana);

	registerMethod("Player", "getSkillLevel", LuaScriptInterface::luaPlayerGetSkillLevel);
	registerMethod("Player", "getEffectiveSkillLevel", LuaScriptInterface::luaPlayerGetEffectiveSkillLevel);
	registerMethod("Player", "getSkillPercent", LuaScriptInterface::luaPlayerGetSkillPercent);
	registerMethod("Player", "getSkillTries", LuaScriptInterface::luaPlayerGetSkillTries);
	registerMethod("Player", "addSkillTries", LuaScriptInterface::luaPlayerAddSkillTries);
	registerMethod("Player", "removeSkillTries", LuaScriptInterface::luaPlayerRemoveSkillTries);
	registerMethod("Player", "getSpecialSkill", LuaScriptInterface::luaPlayerGetSpecialSkill);
	registerMethod("Player", "addSpecialSkill", LuaScriptInterface::luaPlayerAddSpecialSkill);

	registerMethod("Player", "addOfflineTrainingTime", LuaScriptInterface::luaPlayerAddOfflineTrainingTime);
	registerMethod("Player", "getOfflineTrainingTime", LuaScriptInterface::luaPlayerGetOfflineTrainingTime);
	registerMethod("Player", "removeOfflineTrainingTime", LuaScriptInterface::luaPlayerRemoveOfflineTrainingTime);

	registerMethod("Player", "addOfflineTrainingTries", LuaScriptInterface::luaPlayerAddOfflineTrainingTries);

	registerMethod("Player", "getOfflineTrainingSkill", LuaScriptInterface::luaPlayerGetOfflineTrainingSkill);
	registerMethod("Player", "setOfflineTrainingSkill", LuaScriptInterface::luaPlayerSetOfflineTrainingSkill);

	registerMethod("Player", "getItemCount", LuaScriptInterface::luaPlayerGetItemCount);
	registerMethod("Player", "getItemById", LuaScriptInterface::luaPlayerGetItemById);

	registerMethod("Player", "getVocation", LuaScriptInterface::luaPlayerGetVocation);
	registerMethod("Player", "setVocation", LuaScriptInterface::luaPlayerSetVocation);

	registerMethod("Player", "getSex", LuaScriptInterface::luaPlayerGetSex);
	registerMethod("Player", "setSex", LuaScriptInterface::luaPlayerSetSex);

	registerMethod("Player", "getTown", LuaScriptInterface::luaPlayerGetTown);
	registerMethod("Player", "setTown", LuaScriptInterface::luaPlayerSetTown);

	registerMethod("Player", "getGuild", LuaScriptInterface::luaPlayerGetGuild);
	registerMethod("Player", "setGuild", LuaScriptInterface::luaPlayerSetGuild);

	registerMethod("Player", "getGuildLevel", LuaScriptInterface::luaPlayerGetGuildLevel);
	registerMethod("Player", "setGuildLevel", LuaScriptInterface::luaPlayerSetGuildLevel);

	registerMethod("Player", "getGuildNick", LuaScriptInterface::luaPlayerGetGuildNick);
	registerMethod("Player", "setGuildNick", LuaScriptInterface::luaPlayerSetGuildNick);

	registerMethod("Player", "getGroup", LuaScriptInterface::luaPlayerGetGroup);
	registerMethod("Player", "setGroup", LuaScriptInterface::luaPlayerSetGroup);

	registerMethod("Player", "getStamina", LuaScriptInterface::luaPlayerGetStamina);
	registerMethod("Player", "setStamina", LuaScriptInterface::luaPlayerSetStamina);

	registerMethod("Player", "getSoul", LuaScriptInterface::luaPlayerGetSoul);
	registerMethod("Player", "addSoul", LuaScriptInterface::luaPlayerAddSoul);
	registerMethod("Player", "getMaxSoul", LuaScriptInterface::luaPlayerGetMaxSoul);

	registerMethod("Player", "getBankBalance", LuaScriptInterface::luaPlayerGetBankBalance);
	registerMethod("Player", "setBankBalance", LuaScriptInterface::luaPlayerSetBankBalance);

	registerMethod("Player", "getStorageValue", LuaScriptInterface::luaPlayerGetStorageValue);
	registerMethod("Player", "setStorageValue", LuaScriptInterface::luaPlayerSetStorageValue);

	registerMethod("Player", "addItem", LuaScriptInterface::luaPlayerAddItem);
	registerMethod("Player", "addItemEx", LuaScriptInterface::luaPlayerAddItemEx);
	registerMethod("Player", "removeItem", LuaScriptInterface::luaPlayerRemoveItem);

	registerMethod("Player", "getMoney", LuaScriptInterface::luaPlayerGetMoney);
	registerMethod("Player", "addMoney", LuaScriptInterface::luaPlayerAddMoney);
	registerMethod("Player", "removeMoney", LuaScriptInterface::luaPlayerRemoveMoney);

	registerMethod("Player", "showTextDialog", LuaScriptInterface::luaPlayerShowTextDialog);

	registerMethod("Player", "sendTextMessage", LuaScriptInterface::luaPlayerSendTextMessage);
	registerMethod("Player", "sendChannelMessage", LuaScriptInterface::luaPlayerSendChannelMessage);
	registerMethod("Player", "sendPrivateMessage", LuaScriptInterface::luaPlayerSendPrivateMessage);
	registerMethod("Player", "channelSay", LuaScriptInterface::luaPlayerChannelSay);
	registerMethod("Player", "openChannel", LuaScriptInterface::luaPlayerOpenChannel);

	registerMethod("Player", "getSlotItem", LuaScriptInterface::luaPlayerGetSlotItem);

	registerMethod("Player", "getParty", LuaScriptInterface::luaPlayerGetParty);

	registerMethod("Player", "addOutfit", LuaScriptInterface::luaPlayerAddOutfit);
	registerMethod("Player", "addOutfitAddon", LuaScriptInterface::luaPlayerAddOutfitAddon);
	registerMethod("Player", "removeOutfit", LuaScriptInterface::luaPlayerRemoveOutfit);
	registerMethod("Player", "removeOutfitAddon", LuaScriptInterface::luaPlayerRemoveOutfitAddon);
	registerMethod("Player", "hasOutfit", LuaScriptInterface::luaPlayerHasOutfit);
	registerMethod("Player", "canWearOutfit", LuaScriptInterface::luaPlayerCanWearOutfit);
	registerMethod("Player", "sendOutfitWindow", LuaScriptInterface::luaPlayerSendOutfitWindow);

	registerMethod("Player", "addMount", LuaScriptInterface::luaPlayerAddMount);
	registerMethod("Player", "removeMount", LuaScriptInterface::luaPlayerRemoveMount);
	registerMethod("Player", "hasMount", LuaScriptInterface::luaPlayerHasMount);

	registerMethod("Player", "getPremiumEndsAt", LuaScriptInterface::luaPlayerGetPremiumEndsAt);
	registerMethod("Player", "setPremiumEndsAt", LuaScriptInterface::luaPlayerSetPremiumEndsAt);

	registerMethod("Player", "hasBlessing", LuaScriptInterface::luaPlayerHasBlessing);
	registerMethod("Player", "addBlessing", LuaScriptInterface::luaPlayerAddBlessing);
	registerMethod("Player", "removeBlessing", LuaScriptInterface::luaPlayerRemoveBlessing);

	registerMethod("Player", "canLearnSpell", LuaScriptInterface::luaPlayerCanLearnSpell);
	registerMethod("Player", "learnSpell", LuaScriptInterface::luaPlayerLearnSpell);
	registerMethod("Player", "forgetSpell", LuaScriptInterface::luaPlayerForgetSpell);
	registerMethod("Player", "hasLearnedSpell", LuaScriptInterface::luaPlayerHasLearnedSpell);

	registerMethod("Player", "sendTutorial", LuaScriptInterface::luaPlayerSendTutorial);
	registerMethod("Player", "addMapMark", LuaScriptInterface::luaPlayerAddMapMark);

	registerMethod("Player", "save", LuaScriptInterface::luaPlayerSave);
	registerMethod("Player", "popupFYI", LuaScriptInterface::luaPlayerPopupFYI);

	registerMethod("Player", "isPzLocked", LuaScriptInterface::luaPlayerIsPzLocked);

	registerMethod("Player", "getClient", LuaScriptInterface::luaPlayerGetClient);

	registerMethod("Player", "getHouse", LuaScriptInterface::luaPlayerGetHouse);
	registerMethod("Player", "sendHouseWindow", LuaScriptInterface::luaPlayerSendHouseWindow);
	registerMethod("Player", "setEditHouse", LuaScriptInterface::luaPlayerSetEditHouse);

	registerMethod("Player", "setGhostMode", LuaScriptInterface::luaPlayerSetGhostMode);

	registerMethod("Player", "getContainerId", LuaScriptInterface::luaPlayerGetContainerId);
	registerMethod("Player", "getContainerById", LuaScriptInterface::luaPlayerGetContainerById);
	registerMethod("Player", "getContainerIndex", LuaScriptInterface::luaPlayerGetContainerIndex);

	registerMethod("Player", "getInstantSpells", LuaScriptInterface::luaPlayerGetInstantSpells);
	registerMethod("Player", "canCast", LuaScriptInterface::luaPlayerCanCast);

	registerMethod("Player", "hasChaseMode", LuaScriptInterface::luaPlayerHasChaseMode);
	registerMethod("Player", "hasSecureMode", LuaScriptInterface::luaPlayerHasSecureMode);
	registerMethod("Player", "getFightMode", LuaScriptInterface::luaPlayerGetFightMode);

	registerMethod("Player", "getStoreInbox", LuaScriptInterface::luaPlayerGetStoreInbox);
	registerMethod("Player", "isNearDepotBox", LuaScriptInterface::luaPlayerIsNearDepotBox);

	registerMethod("Player", "getIdleTime", LuaScriptInterface::luaPlayerGetIdleTime);
	registerMethod("Player", "resetIdleTime", LuaScriptInterface::luaPlayerResetIdleTime);

	registerMethod("Player", "sendCreatureSquare", LuaScriptInterface::luaPlayerSendCreatureSquare);
	registerMethod("Player", "getEquipment", LuaScriptInterface::luaPlayerGetEquipment);

	registerMethod("Player", "addAugment", LuaScriptInterface::luaPlayerAddAugment);
	registerMethod("Player", "removeAugment", LuaScriptInterface::luaPlayerRemoveAugment);
	registerMethod("Player", "isAugmented", LuaScriptInterface::luaPlayerIsAugmented);
	registerMethod("Player", "hasAugment", LuaScriptInterface::luaPlayerHasAugment);
	registerMethod("Player", "getAugments", LuaScriptInterface::luaPlayerGetAugments);

	// Monster
	registerClass("Monster", "Creature", LuaScriptInterface::luaMonsterCreate);
	registerMetaMethod("Monster", "__eq", LuaScriptInterface::luaUserdataCompare);
	registerMetaMethod("Monster", "__gc", LuaScriptInterface::luaMonsterDelete);

	registerMethod("Monster", "getId", LuaScriptInterface::luaMonsterGetId);
	registerMethod("Monster", "isMonster", LuaScriptInterface::luaMonsterIsMonster);

	registerMethod("Monster", "getType", LuaScriptInterface::luaMonsterGetType);

	registerMethod("Monster", "rename", LuaScriptInterface::luaMonsterRename);

	registerMethod("Monster", "getSpawnPosition", LuaScriptInterface::luaMonsterGetSpawnPosition);
	registerMethod("Monster", "isInSpawnRange", LuaScriptInterface::luaMonsterIsInSpawnRange);

	registerMethod("Monster", "isIdle", LuaScriptInterface::luaMonsterIsIdle);
	registerMethod("Monster", "setIdle", LuaScriptInterface::luaMonsterSetIdle);

	registerMethod("Monster", "isTarget", LuaScriptInterface::luaMonsterIsTarget);
	registerMethod("Monster", "isOpponent", LuaScriptInterface::luaMonsterIsOpponent);
	registerMethod("Monster", "isFriend", LuaScriptInterface::luaMonsterIsFriend);

	registerMethod("Monster", "addFriend", LuaScriptInterface::luaMonsterAddFriend);
	registerMethod("Monster", "removeFriend", LuaScriptInterface::luaMonsterRemoveFriend);
	registerMethod("Monster", "getFriendList", LuaScriptInterface::luaMonsterGetFriendList);
	registerMethod("Monster", "getFriendCount", LuaScriptInterface::luaMonsterGetFriendCount);

	registerMethod("Monster", "addTarget", LuaScriptInterface::luaMonsterAddTarget);
	registerMethod("Monster", "removeTarget", LuaScriptInterface::luaMonsterRemoveTarget);
	registerMethod("Monster", "getTargetList", LuaScriptInterface::luaMonsterGetTargetList);
	registerMethod("Monster", "getTargetCount", LuaScriptInterface::luaMonsterGetTargetCount);

	registerMethod("Monster", "selectTarget", LuaScriptInterface::luaMonsterSelectTarget);
	registerMethod("Monster", "searchTarget", LuaScriptInterface::luaMonsterSearchTarget);

	registerMethod("Monster", "isWalkingToSpawn", LuaScriptInterface::luaMonsterIsWalkingToSpawn);
	registerMethod("Monster", "walkToSpawn", LuaScriptInterface::luaMonsterWalkToSpawn);

	// Npc
	registerClass("Npc", "Creature", LuaScriptInterface::luaNpcCreate);
	registerMetaMethod("Npc", "__eq", LuaScriptInterface::luaUserdataCompare);
	registerMetaMethod("Npc", "__gc", LuaScriptInterface::luaNpcDelete);

	registerMethod("Npc", "isNpc", LuaScriptInterface::luaNpcIsNpc);

	registerMethod("Npc", "setMasterPos", LuaScriptInterface::luaNpcSetMasterPos);

	registerMethod("Npc", "getSpeechBubble", LuaScriptInterface::luaNpcGetSpeechBubble);
	registerMethod("Npc", "setSpeechBubble", LuaScriptInterface::luaNpcSetSpeechBubble);

	registerMethod("Npc", "getSpectators", LuaScriptInterface::luaNpcGetSpectators);

	// Guild
	registerClass("Guild", "", LuaScriptInterface::luaGuildCreate);
	registerMetaMethod("Guild", "__eq", LuaScriptInterface::luaUserdataCompare);

	registerMethod("Guild", "getId", LuaScriptInterface::luaGuildGetId);
	registerMethod("Guild", "getName", LuaScriptInterface::luaGuildGetName);
	registerMethod("Guild", "getMembersOnline", LuaScriptInterface::luaGuildGetMembersOnline);

	registerMethod("Guild", "addRank", LuaScriptInterface::luaGuildAddRank);
	registerMethod("Guild", "getRankById", LuaScriptInterface::luaGuildGetRankById);
	registerMethod("Guild", "getRankByLevel", LuaScriptInterface::luaGuildGetRankByLevel);

	registerMethod("Guild", "getMotd", LuaScriptInterface::luaGuildGetMotd);
	registerMethod("Guild", "setMotd", LuaScriptInterface::luaGuildSetMotd);

	// Group
	registerClass("Group", "", LuaScriptInterface::luaGroupCreate);
	registerMetaMethod("Group", "__eq", LuaScriptInterface::luaUserdataCompare);

	registerMethod("Group", "getId", LuaScriptInterface::luaGroupGetId);
	registerMethod("Group", "getName", LuaScriptInterface::luaGroupGetName);
	registerMethod("Group", "getFlags", LuaScriptInterface::luaGroupGetFlags);
	registerMethod("Group", "getAccess", LuaScriptInterface::luaGroupGetAccess);
	registerMethod("Group", "getMaxDepotItems", LuaScriptInterface::luaGroupGetMaxDepotItems);
	registerMethod("Group", "getMaxVipEntries", LuaScriptInterface::luaGroupGetMaxVipEntries);
	registerMethod("Group", "hasFlag", LuaScriptInterface::luaGroupHasFlag);

	// Vocation
	registerClass("Vocation", "", LuaScriptInterface::luaVocationCreate);
	registerMetaMethod("Vocation", "__eq", LuaScriptInterface::luaUserdataCompare);

	registerMethod("Vocation", "getId", LuaScriptInterface::luaVocationGetId);
	registerMethod("Vocation", "getClientId", LuaScriptInterface::luaVocationGetClientId);
	registerMethod("Vocation", "getName", LuaScriptInterface::luaVocationGetName);
	registerMethod("Vocation", "getDescription", LuaScriptInterface::luaVocationGetDescription);

	registerMethod("Vocation", "getRequiredSkillTries", LuaScriptInterface::luaVocationGetRequiredSkillTries);
	registerMethod("Vocation", "getRequiredManaSpent", LuaScriptInterface::luaVocationGetRequiredManaSpent);

	registerMethod("Vocation", "getCapacityGain", LuaScriptInterface::luaVocationGetCapacityGain);

	registerMethod("Vocation", "getHealthGain", LuaScriptInterface::luaVocationGetHealthGain);
	registerMethod("Vocation", "getHealthGainTicks", LuaScriptInterface::luaVocationGetHealthGainTicks);
	registerMethod("Vocation", "getHealthGainAmount", LuaScriptInterface::luaVocationGetHealthGainAmount);

	registerMethod("Vocation", "getManaGain", LuaScriptInterface::luaVocationGetManaGain);
	registerMethod("Vocation", "getManaGainTicks", LuaScriptInterface::luaVocationGetManaGainTicks);
	registerMethod("Vocation", "getManaGainAmount", LuaScriptInterface::luaVocationGetManaGainAmount);

	registerMethod("Vocation", "getMaxSoul", LuaScriptInterface::luaVocationGetMaxSoul);
	registerMethod("Vocation", "getSoulGainTicks", LuaScriptInterface::luaVocationGetSoulGainTicks);

	registerMethod("Vocation", "getAttackSpeed", LuaScriptInterface::luaVocationGetAttackSpeed);
	registerMethod("Vocation", "getBaseSpeed", LuaScriptInterface::luaVocationGetBaseSpeed);

	registerMethod("Vocation", "getDemotion", LuaScriptInterface::luaVocationGetDemotion);
	registerMethod("Vocation", "getPromotion", LuaScriptInterface::luaVocationGetPromotion);

	registerMethod("Vocation", "allowsPvp", LuaScriptInterface::luaVocationAllowsPvp);

	// Town
	registerClass("Town", "", LuaScriptInterface::luaTownCreate);
	registerMetaMethod("Town", "__eq", LuaScriptInterface::luaUserdataCompare);

	registerMethod("Town", "getId", LuaScriptInterface::luaTownGetId);
	registerMethod("Town", "getName", LuaScriptInterface::luaTownGetName);
	registerMethod("Town", "getTemplePosition", LuaScriptInterface::luaTownGetTemplePosition);

	// House
	registerClass("House", "", LuaScriptInterface::luaHouseCreate);
	registerMetaMethod("House", "__eq", LuaScriptInterface::luaUserdataCompare);

	registerMethod("House", "getId", LuaScriptInterface::luaHouseGetId);
	registerMethod("House", "getName", LuaScriptInterface::luaHouseGetName);
	registerMethod("House", "getTown", LuaScriptInterface::luaHouseGetTown);
	registerMethod("House", "getExitPosition", LuaScriptInterface::luaHouseGetExitPosition);
	registerMethod("House", "getRent", LuaScriptInterface::luaHouseGetRent);
	registerMethod("House", "setRent", LuaScriptInterface::luaHouseSetRent);

	registerMethod("House", "getPaidUntil", LuaScriptInterface::luaHouseGetPaidUntil);
	registerMethod("House", "setPaidUntil", LuaScriptInterface::luaHouseSetPaidUntil);

	registerMethod("House", "getPayRentWarnings", LuaScriptInterface::luaHouseGetPayRentWarnings);
	registerMethod("House", "setPayRentWarnings", LuaScriptInterface::luaHouseSetPayRentWarnings);

	registerMethod("House", "getOwnerGuid", LuaScriptInterface::luaHouseGetOwnerGuid);
	registerMethod("House", "setOwnerGuid", LuaScriptInterface::luaHouseSetOwnerGuid);
	registerMethod("House", "startTrade", LuaScriptInterface::luaHouseStartTrade);

	registerMethod("House", "getBeds", LuaScriptInterface::luaHouseGetBeds);
	registerMethod("House", "getBedCount", LuaScriptInterface::luaHouseGetBedCount);

	registerMethod("House", "getDoors", LuaScriptInterface::luaHouseGetDoors);
	registerMethod("House", "getDoorCount", LuaScriptInterface::luaHouseGetDoorCount);
	registerMethod("House", "getDoorIdByPosition", LuaScriptInterface::luaHouseGetDoorIdByPosition);

	registerMethod("House", "getTiles", LuaScriptInterface::luaHouseGetTiles);
	registerMethod("House", "getItems", LuaScriptInterface::luaHouseGetItems);
	registerMethod("House", "getTileCount", LuaScriptInterface::luaHouseGetTileCount);

	registerMethod("House", "canEditAccessList", LuaScriptInterface::luaHouseCanEditAccessList);
	registerMethod("House", "getAccessList", LuaScriptInterface::luaHouseGetAccessList);
	registerMethod("House", "setAccessList", LuaScriptInterface::luaHouseSetAccessList);

	registerMethod("House", "kickPlayer", LuaScriptInterface::luaHouseKickPlayer);

	registerMethod("House", "save", LuaScriptInterface::luaHouseSave);

	// ItemType
	registerClass("ItemType", "", LuaScriptInterface::luaItemTypeCreate);
	registerMetaMethod("ItemType", "__eq", LuaScriptInterface::luaUserdataCompare);

	registerMethod("ItemType", "isCorpse", LuaScriptInterface::luaItemTypeIsCorpse);
	registerMethod("ItemType", "isDoor", LuaScriptInterface::luaItemTypeIsDoor);
	registerMethod("ItemType", "isContainer", LuaScriptInterface::luaItemTypeIsContainer);
	registerMethod("ItemType", "isFluidContainer", LuaScriptInterface::luaItemTypeIsFluidContainer);
	registerMethod("ItemType", "isMovable", LuaScriptInterface::luaItemTypeIsMovable);
	registerMethod("ItemType", "isRune", LuaScriptInterface::luaItemTypeIsRune);
	registerMethod("ItemType", "isStackable", LuaScriptInterface::luaItemTypeIsStackable);
	registerMethod("ItemType", "isReadable", LuaScriptInterface::luaItemTypeIsReadable);
	registerMethod("ItemType", "isWritable", LuaScriptInterface::luaItemTypeIsWritable);
	registerMethod("ItemType", "isBlocking", LuaScriptInterface::luaItemTypeIsBlocking);
	registerMethod("ItemType", "isGroundTile", LuaScriptInterface::luaItemTypeIsGroundTile);
	registerMethod("ItemType", "isMagicField", LuaScriptInterface::luaItemTypeIsMagicField);
	registerMethod("ItemType", "isUseable", LuaScriptInterface::luaItemTypeIsUseable);
	registerMethod("ItemType", "isPickupable", LuaScriptInterface::luaItemTypeIsPickupable);
	registerMethod("ItemType", "isRotatable", LuaScriptInterface::luaItemTypeIsRotatable);

	registerMethod("ItemType", "getType", LuaScriptInterface::luaItemTypeGetType);
	registerMethod("ItemType", "getGroup", LuaScriptInterface::luaItemTypeGetGroup);
	registerMethod("ItemType", "getId", LuaScriptInterface::luaItemTypeGetId);
	registerMethod("ItemType", "getClientId", LuaScriptInterface::luaItemTypeGetClientId);
	registerMethod("ItemType", "getName", LuaScriptInterface::luaItemTypeGetName);
	registerMethod("ItemType", "getPluralName", LuaScriptInterface::luaItemTypeGetPluralName);
	registerMethod("ItemType", "getRotateTo", LuaScriptInterface::luaItemTypeGetRotateTo);
	registerMethod("ItemType", "getArticle", LuaScriptInterface::luaItemTypeGetArticle);
	registerMethod("ItemType", "getDescription", LuaScriptInterface::luaItemTypeGetDescription);
	registerMethod("ItemType", "getSlotPosition", LuaScriptInterface::luaItemTypeGetSlotPosition);

	registerMethod("ItemType", "getCharges", LuaScriptInterface::luaItemTypeGetCharges);
	registerMethod("ItemType", "getFluidSource", LuaScriptInterface::luaItemTypeGetFluidSource);
	registerMethod("ItemType", "getCapacity", LuaScriptInterface::luaItemTypeGetCapacity);
	registerMethod("ItemType", "getWeight", LuaScriptInterface::luaItemTypeGetWeight);
	registerMethod("ItemType", "getWorth", LuaScriptInterface::luaItemTypeGetWorth);

	registerMethod("ItemType", "getHitChance", LuaScriptInterface::luaItemTypeGetHitChance);
	registerMethod("ItemType", "getShootRange", LuaScriptInterface::luaItemTypeGetShootRange);

	registerMethod("ItemType", "getAttack", LuaScriptInterface::luaItemTypeGetAttack);
	registerMethod("ItemType", "getAttackSpeed", LuaScriptInterface::luaItemTypeGetAttackSpeed);
	registerMethod("ItemType", "getClassification", LuaScriptInterface::luaItemTypeGetClassification);
	registerMethod("ItemType", "getTier", LuaScriptInterface::luaItemTypeGetTier);
	registerMethod("ItemType", "getDefense", LuaScriptInterface::luaItemTypeGetDefense);
	registerMethod("ItemType", "getExtraDefense", LuaScriptInterface::luaItemTypeGetExtraDefense);
	registerMethod("ItemType", "getArmor", LuaScriptInterface::luaItemTypeGetArmor);
	registerMethod("ItemType", "getWeaponType", LuaScriptInterface::luaItemTypeGetWeaponType);

	registerMethod("ItemType", "getElementType", LuaScriptInterface::luaItemTypeGetElementType);
	registerMethod("ItemType", "getElementDamage", LuaScriptInterface::luaItemTypeGetElementDamage);

	registerMethod("ItemType", "getTransformEquipId", LuaScriptInterface::luaItemTypeGetTransformEquipId);
	registerMethod("ItemType", "getTransformDeEquipId", LuaScriptInterface::luaItemTypeGetTransformDeEquipId);
	registerMethod("ItemType", "getDestroyId", LuaScriptInterface::luaItemTypeGetDestroyId);
	registerMethod("ItemType", "getDecayId", LuaScriptInterface::luaItemTypeGetDecayId);
	registerMethod("ItemType", "getRequiredLevel", LuaScriptInterface::luaItemTypeGetRequiredLevel);
	registerMethod("ItemType", "getAmmoType", LuaScriptInterface::luaItemTypeGetAmmoType);
	registerMethod("ItemType", "getShootType", LuaScriptInterface::luaItemTypeGetShootType);
	registerMethod("ItemType", "getCorpseType", LuaScriptInterface::luaItemTypeGetCorpseType);

	registerMethod("ItemType", "getAbilities", LuaScriptInterface::luaItemTypeGetAbilities);

	registerMethod("ItemType", "hasShowAttributes", LuaScriptInterface::luaItemTypeHasShowAttributes);
	registerMethod("ItemType", "hasShowCount", LuaScriptInterface::luaItemTypeHasShowCount);
	registerMethod("ItemType", "hasShowCharges", LuaScriptInterface::luaItemTypeHasShowCharges);
	registerMethod("ItemType", "hasShowDuration", LuaScriptInterface::luaItemTypeHasShowDuration);
	registerMethod("ItemType", "hasAllowDistRead", LuaScriptInterface::luaItemTypeHasAllowDistRead);
	registerMethod("ItemType", "getWieldInfo", LuaScriptInterface::luaItemTypeGetWieldInfo);
	registerMethod("ItemType", "getDuration", LuaScriptInterface::luaItemTypeGetDuration);
	registerMethod("ItemType", "getLevelDoor", LuaScriptInterface::luaItemTypeGetLevelDoor);
	registerMethod("ItemType", "getVocationString", LuaScriptInterface::luaItemTypeGetVocationString);
	registerMethod("ItemType", "getMinReqLevel", LuaScriptInterface::luaItemTypeGetMinReqLevel);
	registerMethod("ItemType", "getMinReqMagicLevel", LuaScriptInterface::luaItemTypeGetMinReqMagicLevel);

	registerMethod("ItemType", "hasSubType", LuaScriptInterface::luaItemTypeHasSubType);

	registerMethod("ItemType", "isStoreItem", LuaScriptInterface::luaItemTypeIsStoreItem);

	// Combat
	registerClass("Combat", "", LuaScriptInterface::luaCombatCreate);
	registerMetaMethod("Combat", "__eq", LuaScriptInterface::luaUserdataCompare);
	registerMetaMethod("Combat", "__gc", LuaScriptInterface::luaCombatDelete);
	registerMethod("Combat", "delete", LuaScriptInterface::luaCombatDelete);

	registerMethod("Combat", "setParameter", LuaScriptInterface::luaCombatSetParameter);
	registerMethod("Combat", "getParameter", LuaScriptInterface::luaCombatGetParameter);

	registerMethod("Combat", "setFormula", LuaScriptInterface::luaCombatSetFormula);

	registerMethod("Combat", "setArea", LuaScriptInterface::luaCombatSetArea);
	registerMethod("Combat", "addCondition", LuaScriptInterface::luaCombatAddCondition);
	registerMethod("Combat", "clearConditions", LuaScriptInterface::luaCombatClearConditions);
	registerMethod("Combat", "setCallback", LuaScriptInterface::luaCombatSetCallback);
	registerMethod("Combat", "setOrigin", LuaScriptInterface::luaCombatSetOrigin);

	registerMethod("Combat", "execute", LuaScriptInterface::luaCombatExecute);

	// Condition
	registerClass("Condition", "", LuaScriptInterface::luaConditionCreate);
	registerMetaMethod("Condition", "__eq", LuaScriptInterface::luaUserdataCompare);
	registerMetaMethod("Condition", "__gc", LuaScriptInterface::luaConditionDelete);

	registerMethod("Condition", "getId", LuaScriptInterface::luaConditionGetId);
	registerMethod("Condition", "getSubId", LuaScriptInterface::luaConditionGetSubId);
	registerMethod("Condition", "getType", LuaScriptInterface::luaConditionGetType);
	registerMethod("Condition", "getIcons", LuaScriptInterface::luaConditionGetIcons);
	registerMethod("Condition", "getEndTime", LuaScriptInterface::luaConditionGetEndTime);

	registerMethod("Condition", "clone", LuaScriptInterface::luaConditionClone);

	registerMethod("Condition", "getTicks", LuaScriptInterface::luaConditionGetTicks);
	registerMethod("Condition", "setTicks", LuaScriptInterface::luaConditionSetTicks);

	registerMethod("Condition", "setParameter", LuaScriptInterface::luaConditionSetParameter);
	registerMethod("Condition", "getParameter", LuaScriptInterface::luaConditionGetParameter);

	registerMethod("Condition", "setFormula", LuaScriptInterface::luaConditionSetFormula);
	registerMethod("Condition", "setOutfit", LuaScriptInterface::luaConditionSetOutfit);

	registerMethod("Condition", "addDamage", LuaScriptInterface::luaConditionAddDamage);

	// Outfit
	registerClass("Outfit", "", LuaScriptInterface::luaOutfitCreate);
	registerMetaMethod("Outfit", "__eq", LuaScriptInterface::luaOutfitCompare);

	// MonsterType
	registerClass("MonsterType", "", LuaScriptInterface::luaMonsterTypeCreate);
	registerMetaMethod("MonsterType", "__eq", LuaScriptInterface::luaUserdataCompare);

	registerMethod("MonsterType", "isAttackable", LuaScriptInterface::luaMonsterTypeIsAttackable);
	registerMethod("MonsterType", "isRewardBoss", LuaScriptInterface::luaMonsterTypeIsRewardBoss);
	registerMethod("MonsterType", "isChallengeable", LuaScriptInterface::luaMonsterTypeIsChallengeable);
	registerMethod("MonsterType", "isConvinceable", LuaScriptInterface::luaMonsterTypeIsConvinceable);
	registerMethod("MonsterType", "isSummonable", LuaScriptInterface::luaMonsterTypeIsSummonable);
	registerMethod("MonsterType", "isIgnoringSpawnBlock", LuaScriptInterface::luaMonsterTypeIsIgnoringSpawnBlock);
	registerMethod("MonsterType", "isIllusionable", LuaScriptInterface::luaMonsterTypeIsIllusionable);
	registerMethod("MonsterType", "isHostile", LuaScriptInterface::luaMonsterTypeIsHostile);
	registerMethod("MonsterType", "isPushable", LuaScriptInterface::luaMonsterTypeIsPushable);
	registerMethod("MonsterType", "isHealthHidden", LuaScriptInterface::luaMonsterTypeIsHealthHidden);
	registerMethod("MonsterType", "isBoss", LuaScriptInterface::luaMonsterTypeIsBoss);

	registerMethod("MonsterType", "canPushItems", LuaScriptInterface::luaMonsterTypeCanPushItems);
	registerMethod("MonsterType", "canPushCreatures", LuaScriptInterface::luaMonsterTypeCanPushCreatures);

	registerMethod("MonsterType", "canWalkOnEnergy", LuaScriptInterface::luaMonsterTypeCanWalkOnEnergy);
	registerMethod("MonsterType", "canWalkOnFire", LuaScriptInterface::luaMonsterTypeCanWalkOnFire);
	registerMethod("MonsterType", "canWalkOnPoison", LuaScriptInterface::luaMonsterTypeCanWalkOnPoison);

	registerMethod("MonsterType", "name", LuaScriptInterface::luaMonsterTypeName);
	registerMethod("MonsterType", "nameDescription", LuaScriptInterface::luaMonsterTypeNameDescription);

	registerMethod("MonsterType", "health", LuaScriptInterface::luaMonsterTypeHealth);
	registerMethod("MonsterType", "maxHealth", LuaScriptInterface::luaMonsterTypeMaxHealth);
	registerMethod("MonsterType", "runHealth", LuaScriptInterface::luaMonsterTypeRunHealth);
	registerMethod("MonsterType", "experience", LuaScriptInterface::luaMonsterTypeExperience);
	registerMethod("MonsterType", "skull", LuaScriptInterface::luaMonsterTypeSkull);

	registerMethod("MonsterType", "combatImmunities", LuaScriptInterface::luaMonsterTypeCombatImmunities);
	registerMethod("MonsterType", "conditionImmunities", LuaScriptInterface::luaMonsterTypeConditionImmunities);

	registerMethod("MonsterType", "getAttackList", LuaScriptInterface::luaMonsterTypeGetAttackList);
	registerMethod("MonsterType", "addAttack", LuaScriptInterface::luaMonsterTypeAddAttack);

	registerMethod("MonsterType", "getDefenseList", LuaScriptInterface::luaMonsterTypeGetDefenseList);
	registerMethod("MonsterType", "addDefense", LuaScriptInterface::luaMonsterTypeAddDefense);

	registerMethod("MonsterType", "getElementList", LuaScriptInterface::luaMonsterTypeGetElementList);
	registerMethod("MonsterType", "addElement", LuaScriptInterface::luaMonsterTypeAddElement);

	registerMethod("MonsterType", "getVoices", LuaScriptInterface::luaMonsterTypeGetVoices);
	registerMethod("MonsterType", "addVoice", LuaScriptInterface::luaMonsterTypeAddVoice);

	registerMethod("MonsterType", "getLoot", LuaScriptInterface::luaMonsterTypeGetLoot);
	registerMethod("MonsterType", "addLoot", LuaScriptInterface::luaMonsterTypeAddLoot);

	registerMethod("MonsterType", "getCreatureEvents", LuaScriptInterface::luaMonsterTypeGetCreatureEvents);
	registerMethod("MonsterType", "registerEvent", LuaScriptInterface::luaMonsterTypeRegisterEvent);

	registerMethod("MonsterType", "eventType", LuaScriptInterface::luaMonsterTypeEventType);
	registerMethod("MonsterType", "onThink", LuaScriptInterface::luaMonsterTypeEventOnCallback);
	registerMethod("MonsterType", "onAppear", LuaScriptInterface::luaMonsterTypeEventOnCallback);
	registerMethod("MonsterType", "onDisappear", LuaScriptInterface::luaMonsterTypeEventOnCallback);
	registerMethod("MonsterType", "onMove", LuaScriptInterface::luaMonsterTypeEventOnCallback);
	registerMethod("MonsterType", "onSay", LuaScriptInterface::luaMonsterTypeEventOnCallback);

	registerMethod("MonsterType", "getSummonList", LuaScriptInterface::luaMonsterTypeGetSummonList);
	registerMethod("MonsterType", "addSummon", LuaScriptInterface::luaMonsterTypeAddSummon);

	registerMethod("MonsterType", "maxSummons", LuaScriptInterface::luaMonsterTypeMaxSummons);

	registerMethod("MonsterType", "armor", LuaScriptInterface::luaMonsterTypeArmor);
	registerMethod("MonsterType", "defense", LuaScriptInterface::luaMonsterTypeDefense);
	registerMethod("MonsterType", "outfit", LuaScriptInterface::luaMonsterTypeOutfit);
	registerMethod("MonsterType", "race", LuaScriptInterface::luaMonsterTypeRace);
	registerMethod("MonsterType", "corpseId", LuaScriptInterface::luaMonsterTypeCorpseId);
	registerMethod("MonsterType", "manaCost", LuaScriptInterface::luaMonsterTypeManaCost);
	registerMethod("MonsterType", "baseSpeed", LuaScriptInterface::luaMonsterTypeBaseSpeed);
	registerMethod("MonsterType", "light", LuaScriptInterface::luaMonsterTypeLight);

	registerMethod("MonsterType", "staticAttackChance", LuaScriptInterface::luaMonsterTypeStaticAttackChance);
	registerMethod("MonsterType", "targetDistance", LuaScriptInterface::luaMonsterTypeTargetDistance);
	registerMethod("MonsterType", "yellChance", LuaScriptInterface::luaMonsterTypeYellChance);
	registerMethod("MonsterType", "yellSpeedTicks", LuaScriptInterface::luaMonsterTypeYellSpeedTicks);
	registerMethod("MonsterType", "changeTargetChance", LuaScriptInterface::luaMonsterTypeChangeTargetChance);
	registerMethod("MonsterType", "changeTargetSpeed", LuaScriptInterface::luaMonsterTypeChangeTargetSpeed);

	// Loot
	registerClass("Loot", "", LuaScriptInterface::luaCreateLoot);
	registerMetaMethod("Loot", "__gc", LuaScriptInterface::luaDeleteLoot);
	registerMethod("Loot", "delete", LuaScriptInterface::luaDeleteLoot);

	registerMethod("Loot", "setId", LuaScriptInterface::luaLootSetId);
	registerMethod("Loot", "setMaxCount", LuaScriptInterface::luaLootSetMaxCount);
	registerMethod("Loot", "setSubType", LuaScriptInterface::luaLootSetSubType);
	registerMethod("Loot", "setChance", LuaScriptInterface::luaLootSetChance);
	registerMethod("Loot", "setActionId", LuaScriptInterface::luaLootSetActionId);
	registerMethod("Loot", "setDescription", LuaScriptInterface::luaLootSetDescription);
	registerMethod("Loot", "addChildLoot", LuaScriptInterface::luaLootAddChildLoot);

	// Skill (custom skills)
	registerClass("Skill", "", LuaScriptInterface::luaCreateCustomSkill);
	registerMetaMethod("Skill", "__gc", LuaScriptInterface::luaDeleteCustomSkill);
	registerMetaMethod("Skill", "__add", LuaScriptInterface::luaCustomSkillAddLevels);
	registerMetaMethod("Skill", "__sub", LuaScriptInterface::luaCustomSkillSubtractLevels);
	registerMethod("Skill", "level", LuaScriptInterface::luaCustomSkillGetLevel);
	registerMethod("Skill", "getLevel", LuaScriptInterface::luaCustomSkillGetLevel);
	registerMethod("Skill", "addLevels", LuaScriptInterface::luaCustomSkillAddLevels);
	registerMethod("Skill", "subtractLevels", LuaScriptInterface::luaCustomSkillSubtractLevels);
	registerMethod("Skill", "removeLevels", LuaScriptInterface::luaCustomSkillSubtractLevels);
	registerMethod("Skill", "clearLevels", LuaScriptInterface::luaCustomSkillClearLevels);
	registerMethod("Skill", "bonusLevel", LuaScriptInterface::luaCustomSkillGetBonusLevel);
	registerMethod("Skill", "addBonusLevels", LuaScriptInterface::luaCustomSkillAddBonusLevels);
	registerMethod("Skill", "subtractBonusLevels", LuaScriptInterface::luaCustomSkillSubtractBonusLevels);
	registerMethod("Skill", "removeBonusLevels", LuaScriptInterface::luaCustomSkillSubtractBonusLevels);
	registerMethod("Skill", "clearBonus", LuaScriptInterface::luaCustomSkillClearBonus);
	registerMethod("Skill", "points", LuaScriptInterface::luaCustomSkillGetPoints);
	registerMethod("Skill", "getPoints", LuaScriptInterface::luaCustomSkillGetPoints);
	registerMethod("Skill", "addPoints", LuaScriptInterface::luaCustomSkillAddPoints);
	registerMethod("Skill", "subtractPoints", LuaScriptInterface::luaCustomSkillSubtractPoints);
	registerMethod("Skill", "removePoints", LuaScriptInterface::luaCustomSkillSubtractPoints);
	registerMethod("Skill", "clearPoints", LuaScriptInterface::luaCustomSkillClearPoints);
	registerMethod("Skill", "pointsNeeded", LuaScriptInterface::luaCustomSkillGetPointsForLevel);
	registerMethod("Skill", "maxLevel", LuaScriptInterface::luaCustomSkillGetMaxLevel);
	registerMethod("Skill", "max", LuaScriptInterface::luaCustomSkillGetMaxLevel);
	registerMethod("Skill", "getMax", LuaScriptInterface::luaCustomSkillGetMaxLevel);
	registerMethod("Skill", "getMaxLevel", LuaScriptInterface::luaCustomSkillGetMaxLevel);
	registerMethod("Skill", "percent", LuaScriptInterface::luaCustomSkillGetPercentToLevel);
	registerMethod("Skill", "getPercent", LuaScriptInterface::luaCustomSkillGetPercentToLevel);

	// MonsterSpell
	registerClass("MonsterSpell", "", LuaScriptInterface::luaCreateMonsterSpell);
	registerMetaMethod("MonsterSpell", "__gc", LuaScriptInterface::luaDeleteMonsterSpell);
	registerMethod("MonsterSpell", "delete", LuaScriptInterface::luaDeleteMonsterSpell);

	registerMethod("MonsterSpell", "setType", LuaScriptInterface::luaMonsterSpellSetType);
	registerMethod("MonsterSpell", "setScriptName", LuaScriptInterface::luaMonsterSpellSetScriptName);
	registerMethod("MonsterSpell", "setChance", LuaScriptInterface::luaMonsterSpellSetChance);
	registerMethod("MonsterSpell", "setInterval", LuaScriptInterface::luaMonsterSpellSetInterval);
	registerMethod("MonsterSpell", "setRange", LuaScriptInterface::luaMonsterSpellSetRange);
	registerMethod("MonsterSpell", "setCombatValue", LuaScriptInterface::luaMonsterSpellSetCombatValue);
	registerMethod("MonsterSpell", "setCombatType", LuaScriptInterface::luaMonsterSpellSetCombatType);
	registerMethod("MonsterSpell", "setAttackValue", LuaScriptInterface::luaMonsterSpellSetAttackValue);
	registerMethod("MonsterSpell", "setNeedTarget", LuaScriptInterface::luaMonsterSpellSetNeedTarget);
	registerMethod("MonsterSpell", "setNeedDirection", LuaScriptInterface::luaMonsterSpellSetNeedDirection);
	registerMethod("MonsterSpell", "setCombatLength", LuaScriptInterface::luaMonsterSpellSetCombatLength);
	registerMethod("MonsterSpell", "setCombatSpread", LuaScriptInterface::luaMonsterSpellSetCombatSpread);
	registerMethod("MonsterSpell", "setCombatRadius", LuaScriptInterface::luaMonsterSpellSetCombatRadius);
	registerMethod("MonsterSpell", "setConditionType", LuaScriptInterface::luaMonsterSpellSetConditionType);
	registerMethod("MonsterSpell", "setConditionDamage", LuaScriptInterface::luaMonsterSpellSetConditionDamage);
	registerMethod("MonsterSpell", "setConditionSpeedChange", LuaScriptInterface::luaMonsterSpellSetConditionSpeedChange);
	registerMethod("MonsterSpell", "setConditionDuration", LuaScriptInterface::luaMonsterSpellSetConditionDuration);
	registerMethod("MonsterSpell", "setConditionDrunkenness", LuaScriptInterface::luaMonsterSpellSetConditionDrunkenness);
	registerMethod("MonsterSpell", "setConditionTickInterval", LuaScriptInterface::luaMonsterSpellSetConditionTickInterval);
	registerMethod("MonsterSpell", "setCombatShootEffect", LuaScriptInterface::luaMonsterSpellSetCombatShootEffect);
	registerMethod("MonsterSpell", "setCombatEffect", LuaScriptInterface::luaMonsterSpellSetCombatEffect);
	registerMethod("MonsterSpell", "setOutfit", LuaScriptInterface::luaMonsterSpellSetOutfit);

	// Party
	registerClass("Party", "", LuaScriptInterface::luaPartyCreate);
	registerMetaMethod("Party", "__eq", LuaScriptInterface::luaUserdataCompare);

	registerMethod("Party", "disband", LuaScriptInterface::luaPartyDisband);

	registerMethod("Party", "getLeader", LuaScriptInterface::luaPartyGetLeader);
	registerMethod("Party", "setLeader", LuaScriptInterface::luaPartySetLeader);

	registerMethod("Party", "getMembers", LuaScriptInterface::luaPartyGetMembers);
	registerMethod("Party", "getMemberCount", LuaScriptInterface::luaPartyGetMemberCount);

	registerMethod("Party", "getInvitees", LuaScriptInterface::luaPartyGetInvitees);
	registerMethod("Party", "getInviteeCount", LuaScriptInterface::luaPartyGetInviteeCount);

	registerMethod("Party", "addInvite", LuaScriptInterface::luaPartyAddInvite);
	registerMethod("Party", "removeInvite", LuaScriptInterface::luaPartyRemoveInvite);

	registerMethod("Party", "addMember", LuaScriptInterface::luaPartyAddMember);
	registerMethod("Party", "removeMember", LuaScriptInterface::luaPartyRemoveMember);

	registerMethod("Party", "isSharedExperienceActive", LuaScriptInterface::luaPartyIsSharedExperienceActive);
	registerMethod("Party", "isSharedExperienceEnabled", LuaScriptInterface::luaPartyIsSharedExperienceEnabled);
	registerMethod("Party", "shareExperience", LuaScriptInterface::luaPartyShareExperience);
	registerMethod("Party", "setSharedExperience", LuaScriptInterface::luaPartySetSharedExperience);

	// Spells
	registerClass("Spell", "", LuaScriptInterface::luaSpellCreate);
	registerMetaMethod("Spell", "__eq", LuaScriptInterface::luaUserdataCompare);

	registerMethod("Spell", "onCastSpell", LuaScriptInterface::luaSpellOnCastSpell);
	registerMethod("Spell", "register", LuaScriptInterface::luaSpellRegister);
	registerMethod("Spell", "name", LuaScriptInterface::luaSpellName);
	registerMethod("Spell", "id", LuaScriptInterface::luaSpellId);
	registerMethod("Spell", "group", LuaScriptInterface::luaSpellGroup);
	registerMethod("Spell", "cooldown", LuaScriptInterface::luaSpellCooldown);
	registerMethod("Spell", "groupCooldown", LuaScriptInterface::luaSpellGroupCooldown);
	registerMethod("Spell", "level", LuaScriptInterface::luaSpellLevel);
	registerMethod("Spell", "magicLevel", LuaScriptInterface::luaSpellMagicLevel);
	registerMethod("Spell", "mana", LuaScriptInterface::luaSpellMana);
	registerMethod("Spell", "manaPercent", LuaScriptInterface::luaSpellManaPercent);
	registerMethod("Spell", "soul", LuaScriptInterface::luaSpellSoul);
	registerMethod("Spell", "range", LuaScriptInterface::luaSpellRange);
	registerMethod("Spell", "isPremium", LuaScriptInterface::luaSpellPremium);
	registerMethod("Spell", "isEnabled", LuaScriptInterface::luaSpellEnabled);
	registerMethod("Spell", "needTarget", LuaScriptInterface::luaSpellNeedTarget);
	registerMethod("Spell", "needWeapon", LuaScriptInterface::luaSpellNeedWeapon);
	registerMethod("Spell", "needLearn", LuaScriptInterface::luaSpellNeedLearn);
	registerMethod("Spell", "isSelfTarget", LuaScriptInterface::luaSpellSelfTarget);
	registerMethod("Spell", "isBlocking", LuaScriptInterface::luaSpellBlocking);
	registerMethod("Spell", "isAggressive", LuaScriptInterface::luaSpellAggressive);
	registerMethod("Spell", "isPzLock", LuaScriptInterface::luaSpellPzLock);
	registerMethod("Spell", "vocation", LuaScriptInterface::luaSpellVocation);

	// only for InstantSpell
	registerMethod("Spell", "words", LuaScriptInterface::luaSpellWords);
	registerMethod("Spell", "needDirection", LuaScriptInterface::luaSpellNeedDirection);
	registerMethod("Spell", "hasParams", LuaScriptInterface::luaSpellHasParams);
	registerMethod("Spell", "hasPlayerNameParam", LuaScriptInterface::luaSpellHasPlayerNameParam);
	registerMethod("Spell", "needCasterTargetOrDirection", LuaScriptInterface::luaSpellNeedCasterTargetOrDirection);
	registerMethod("Spell", "isBlockingWalls", LuaScriptInterface::luaSpellIsBlockingWalls);

	// only for RuneSpells
	registerMethod("Spell", "runeLevel", LuaScriptInterface::luaSpellRuneLevel);
	registerMethod("Spell", "runeMagicLevel", LuaScriptInterface::luaSpellRuneMagicLevel);
	registerMethod("Spell", "runeId", LuaScriptInterface::luaSpellRuneId);
	registerMethod("Spell", "charges", LuaScriptInterface::luaSpellCharges);
	registerMethod("Spell", "allowFarUse", LuaScriptInterface::luaSpellAllowFarUse);
	registerMethod("Spell", "blockWalls", LuaScriptInterface::luaSpellBlockWalls);
	registerMethod("Spell", "checkFloor", LuaScriptInterface::luaSpellCheckFloor);

	// Action
	registerClass("Action", "", LuaScriptInterface::luaCreateAction);
	registerMethod("Action", "onUse", LuaScriptInterface::luaActionOnUse);
	registerMethod("Action", "register", LuaScriptInterface::luaActionRegister);
	registerMethod("Action", "id", LuaScriptInterface::luaActionItemId);
	registerMethod("Action", "aid", LuaScriptInterface::luaActionActionId);
	registerMethod("Action", "uid", LuaScriptInterface::luaActionUniqueId);
	registerMethod("Action", "allowFarUse", LuaScriptInterface::luaActionAllowFarUse);
	registerMethod("Action", "blockWalls", LuaScriptInterface::luaActionBlockWalls);
	registerMethod("Action", "checkFloor", LuaScriptInterface::luaActionCheckFloor);

	// TalkAction
	registerClass("TalkAction", "", LuaScriptInterface::luaCreateTalkaction);
	registerMethod("TalkAction", "onSay", LuaScriptInterface::luaTalkactionOnSay);
	registerMethod("TalkAction", "register", LuaScriptInterface::luaTalkactionRegister);
	registerMethod("TalkAction", "separator", LuaScriptInterface::luaTalkactionSeparator);
	registerMethod("TalkAction", "access", LuaScriptInterface::luaTalkactionAccess);
	registerMethod("TalkAction", "accountType", LuaScriptInterface::luaTalkactionAccountType);

	// CreatureEvent
	registerClass("CreatureEvent", "", LuaScriptInterface::luaCreateCreatureEvent);
	registerMethod("CreatureEvent", "type", LuaScriptInterface::luaCreatureEventType);
	registerMethod("CreatureEvent", "register", LuaScriptInterface::luaCreatureEventRegister);
	registerMethod("CreatureEvent", "onLogin", LuaScriptInterface::luaCreatureEventOnCallback);
	registerMethod("CreatureEvent", "onLogout", LuaScriptInterface::luaCreatureEventOnCallback);
	registerMethod("CreatureEvent", "onThink", LuaScriptInterface::luaCreatureEventOnCallback);
	registerMethod("CreatureEvent", "onPrepareDeath", LuaScriptInterface::luaCreatureEventOnCallback);
	registerMethod("CreatureEvent", "onDeath", LuaScriptInterface::luaCreatureEventOnCallback);
	registerMethod("CreatureEvent", "onKill", LuaScriptInterface::luaCreatureEventOnCallback);
	registerMethod("CreatureEvent", "onAdvance", LuaScriptInterface::luaCreatureEventOnCallback);
	registerMethod("CreatureEvent", "onModalWindow", LuaScriptInterface::luaCreatureEventOnCallback);
	registerMethod("CreatureEvent", "onTextEdit", LuaScriptInterface::luaCreatureEventOnCallback);
	registerMethod("CreatureEvent", "onHealthChange", LuaScriptInterface::luaCreatureEventOnCallback);
	registerMethod("CreatureEvent", "onManaChange", LuaScriptInterface::luaCreatureEventOnCallback);
	registerMethod("CreatureEvent", "onExtendedOpcode", LuaScriptInterface::luaCreatureEventOnCallback);

	// MoveEvent
	registerClass("MoveEvent", "", LuaScriptInterface::luaCreateMoveEvent);
	registerMethod("MoveEvent", "type", LuaScriptInterface::luaMoveEventType);
	registerMethod("MoveEvent", "register", LuaScriptInterface::luaMoveEventRegister);
	registerMethod("MoveEvent", "level", LuaScriptInterface::luaMoveEventLevel);
	registerMethod("MoveEvent", "magicLevel", LuaScriptInterface::luaMoveEventMagLevel);
	registerMethod("MoveEvent", "slot", LuaScriptInterface::luaMoveEventSlot);
	registerMethod("MoveEvent", "id", LuaScriptInterface::luaMoveEventItemId);
	registerMethod("MoveEvent", "aid", LuaScriptInterface::luaMoveEventActionId);
	registerMethod("MoveEvent", "uid", LuaScriptInterface::luaMoveEventUniqueId);
	registerMethod("MoveEvent", "position", LuaScriptInterface::luaMoveEventPosition);
	registerMethod("MoveEvent", "premium", LuaScriptInterface::luaMoveEventPremium);
	registerMethod("MoveEvent", "vocation", LuaScriptInterface::luaMoveEventVocation);
	registerMethod("MoveEvent", "tileItem", LuaScriptInterface::luaMoveEventTileItem);
	registerMethod("MoveEvent", "onEquip", LuaScriptInterface::luaMoveEventOnCallback);
	registerMethod("MoveEvent", "onDeEquip", LuaScriptInterface::luaMoveEventOnCallback);
	registerMethod("MoveEvent", "onStepIn", LuaScriptInterface::luaMoveEventOnCallback);
	registerMethod("MoveEvent", "onStepOut", LuaScriptInterface::luaMoveEventOnCallback);
	registerMethod("MoveEvent", "onAddItem", LuaScriptInterface::luaMoveEventOnCallback);
	registerMethod("MoveEvent", "onRemoveItem", LuaScriptInterface::luaMoveEventOnCallback);

	// GlobalEvent
	registerClass("GlobalEvent", "", LuaScriptInterface::luaCreateGlobalEvent);
	registerMethod("GlobalEvent", "type", LuaScriptInterface::luaGlobalEventType);
	registerMethod("GlobalEvent", "register", LuaScriptInterface::luaGlobalEventRegister);
	registerMethod("GlobalEvent", "time", LuaScriptInterface::luaGlobalEventTime);
	registerMethod("GlobalEvent", "interval", LuaScriptInterface::luaGlobalEventInterval);
	registerMethod("GlobalEvent", "onThink", LuaScriptInterface::luaGlobalEventOnCallback);
	registerMethod("GlobalEvent", "onTime", LuaScriptInterface::luaGlobalEventOnCallback);
	registerMethod("GlobalEvent", "onStartup", LuaScriptInterface::luaGlobalEventOnCallback);
	registerMethod("GlobalEvent", "onShutdown", LuaScriptInterface::luaGlobalEventOnCallback);
	registerMethod("GlobalEvent", "onRecord", LuaScriptInterface::luaGlobalEventOnCallback);
	registerMethod("GlobalEvent", "onSave", LuaScriptInterface::luaGlobalEventOnCallback);

	// Weapon
	registerClass("Weapon", "", LuaScriptInterface::luaCreateWeapon);
	registerMethod("Weapon", "action", LuaScriptInterface::luaWeaponAction);
	registerMethod("Weapon", "register", LuaScriptInterface::luaWeaponRegister);
	registerMethod("Weapon", "id", LuaScriptInterface::luaWeaponId);
	registerMethod("Weapon", "level", LuaScriptInterface::luaWeaponLevel);
	registerMethod("Weapon", "magicLevel", LuaScriptInterface::luaWeaponMagicLevel);
	registerMethod("Weapon", "mana", LuaScriptInterface::luaWeaponMana);
	registerMethod("Weapon", "manaPercent", LuaScriptInterface::luaWeaponManaPercent);
	registerMethod("Weapon", "health", LuaScriptInterface::luaWeaponHealth);
	registerMethod("Weapon", "healthPercent", LuaScriptInterface::luaWeaponHealthPercent);
	registerMethod("Weapon", "soul", LuaScriptInterface::luaWeaponSoul);
	registerMethod("Weapon", "breakChance", LuaScriptInterface::luaWeaponBreakChance);
	registerMethod("Weapon", "premium", LuaScriptInterface::luaWeaponPremium);
	registerMethod("Weapon", "wieldUnproperly", LuaScriptInterface::luaWeaponUnproperly);
	registerMethod("Weapon", "vocation", LuaScriptInterface::luaWeaponVocation);
	registerMethod("Weapon", "onUseWeapon", LuaScriptInterface::luaWeaponOnUseWeapon);
	registerMethod("Weapon", "element", LuaScriptInterface::luaWeaponElement);
	registerMethod("Weapon", "attack", LuaScriptInterface::luaWeaponAttack);
	registerMethod("Weapon", "defense", LuaScriptInterface::luaWeaponDefense);
	registerMethod("Weapon", "range", LuaScriptInterface::luaWeaponRange);
	registerMethod("Weapon", "charges", LuaScriptInterface::luaWeaponCharges);
	registerMethod("Weapon", "duration", LuaScriptInterface::luaWeaponDuration);
	registerMethod("Weapon", "decayTo", LuaScriptInterface::luaWeaponDecayTo);
	registerMethod("Weapon", "transformEquipTo", LuaScriptInterface::luaWeaponTransformEquipTo);
	registerMethod("Weapon", "transformDeEquipTo", LuaScriptInterface::luaWeaponTransformDeEquipTo);
	registerMethod("Weapon", "slotType", LuaScriptInterface::luaWeaponSlotType);
	registerMethod("Weapon", "hitChance", LuaScriptInterface::luaWeaponHitChance);
	registerMethod("Weapon", "extraElement", LuaScriptInterface::luaWeaponExtraElement);

	// exclusively for distance weapons
	registerMethod("Weapon", "ammoType", LuaScriptInterface::luaWeaponAmmoType);
	registerMethod("Weapon", "maxHitChance", LuaScriptInterface::luaWeaponMaxHitChance);

	// exclusively for wands
	registerMethod("Weapon", "damage", LuaScriptInterface::luaWeaponWandDamage);

	// exclusively for wands & distance weapons
	registerMethod("Weapon", "shootType", LuaScriptInterface::luaWeaponShootType);

	// XML
	registerClass("XMLDocument", "", LuaScriptInterface::luaCreateXmlDocument);
	registerMetaMethod("XMLDocument", "__gc", LuaScriptInterface::luaDeleteXmlDocument);
	registerMethod("XMLDocument", "delete", LuaScriptInterface::luaDeleteXmlDocument);

	registerMethod("XMLDocument", "child", LuaScriptInterface::luaXmlDocumentChild);

	registerClass("XMLNode", "");
	registerMetaMethod("XMLNode", "__gc", LuaScriptInterface::luaDeleteXmlNode);
	registerMethod("XMLNode", "delete", LuaScriptInterface::luaDeleteXmlNode);

	registerMethod("XMLNode", "attribute", LuaScriptInterface::luaXmlNodeAttribute);
	registerMethod("XMLNode", "name", LuaScriptInterface::luaXmlNodeName);
	registerMethod("XMLNode", "firstChild", LuaScriptInterface::luaXmlNodeFirstChild);
	registerMethod("XMLNode", "nextSibling", LuaScriptInterface::luaXmlNodeNextSibling);

	lua_register(luaState, "Zones", LuaScriptInterface::luaGetZones);
	registerClass("Zone", "", LuaScriptInterface::luaCreateZone);
	registerMetaMethod("Zone", "__gc", LuaScriptInterface::luaDeleteZone);
	registerMethod("Zone", "getId", LuaScriptInterface::luaZoneId);
	registerMethod("Zone", "getCreatures", LuaScriptInterface::luaZoneCreatures); // lets pass a creature_type for a filter
	registerMethod("Zone", "getGrounds", LuaScriptInterface::luaZoneGrounds);
	registerMethod("Zone", "getItems", LuaScriptInterface::luaZoneItems);
	registerMethod("Zone", "getTiles", LuaScriptInterface::luaZoneTiles);
	registerMethod("Zone", "getCreatureCount", LuaScriptInterface::luaZoneCreatureCount);
	registerMethod("Zone", "getItemCount", LuaScriptInterface::luaZoneItemCount);
	registerMethod("Zone", "getTileCount", LuaScriptInterface::luaZoneTileCount);
}

#undef registerEnum
#undef registerEnumIn

void LuaScriptInterface::registerClass(const std::string& className, const std::string& baseClass, lua_CFunction newFunction/* = nullptr*/) const
{
	// className = {}
	lua_newtable(luaState);
	lua_pushvalue(luaState, -1);
	lua_setglobal(luaState, className.c_str());
	int methods = lua_gettop(luaState);

	// methodsTable = {}
	lua_newtable(luaState);
	int methodsTable = lua_gettop(luaState);

	if (newFunction) {
		// className.__call = newFunction
		lua_pushcfunction(luaState, newFunction);
		lua_setfield(luaState, methodsTable, "__call");
	}

	uint32_t parents = 0;
	if (!baseClass.empty()) {
		lua_getglobal(luaState, baseClass.c_str());
		lua_rawgeti(luaState, -1, 'p');
		parents = getNumber<uint32_t>(luaState, -1) + 1;
		lua_pop(luaState, 1);
		lua_setfield(luaState, methodsTable, "__index");
	}

	// setmetatable(className, methodsTable)
	lua_setmetatable(luaState, methods);

	// className.metatable = {}
	luaL_newmetatable(luaState, className.c_str());
	int metatable = lua_gettop(luaState);

	// className.metatable.__metatable = className
	lua_pushvalue(luaState, methods);
	lua_setfield(luaState, metatable, "__metatable");

	// className.metatable.__index = className
	lua_pushvalue(luaState, methods);
	lua_setfield(luaState, metatable, "__index");

	// className.metatable['h'] = hash
	lua_pushinteger(luaState, std::hash<std::string>()(className));
	lua_rawseti(luaState, metatable, 'h');

	// className.metatable['p'] = parents
	lua_pushinteger(luaState, parents);
	lua_rawseti(luaState, metatable, 'p');

	// className.metatable['t'] = type
	if (className == "Item") {
		lua_pushinteger(luaState, LuaData_Item);
	} else if (className == "Container") {
		lua_pushinteger(luaState, LuaData_Container);
	} else if (className == "Teleport") {
		lua_pushinteger(luaState, LuaData_Teleport);
	} else if (className == "Player") {
		lua_pushinteger(luaState, LuaData_Player);
	} else if (className == "Monster") {
		lua_pushinteger(luaState, LuaData_Monster);
	} else if (className == "Npc") {
		lua_pushinteger(luaState, LuaData_Npc);
	} else if (className == "Tile") {
		lua_pushinteger(luaState, LuaData_Tile);
	} else {
		lua_pushinteger(luaState, LuaData_Unknown);
	}
	lua_rawseti(luaState, metatable, 't');

	// pop className, className.metatable
	lua_pop(luaState, 2);
}

void LuaScriptInterface::registerTable(const std::string& tableName) const
{
	// _G[tableName] = {}
	lua_newtable(luaState);
	lua_setglobal(luaState, tableName.c_str());
}

void LuaScriptInterface::registerMethod(const std::string& globalName, const std::string& methodName, lua_CFunction func) const
{
	// globalName.methodName = func
	lua_getglobal(luaState, globalName.c_str());
	lua_pushcfunction(luaState, func);
	lua_setfield(luaState, -2, methodName.c_str());

	// pop globalName
	lua_pop(luaState, 1);
}

void LuaScriptInterface::registerMetaMethod(const std::string& className, const std::string& methodName, lua_CFunction func) const
{
	// className.metatable.methodName = func
	luaL_getmetatable(luaState, className.c_str());
	lua_pushcfunction(luaState, func);
	lua_setfield(luaState, -2, methodName.c_str());

	// pop className.metatable
	lua_pop(luaState, 1);
}

void LuaScriptInterface::registerGlobalMethod(const std::string& functionName, lua_CFunction func) const
{
	// _G[functionName] = func
	lua_pushcfunction(luaState, func);
	lua_setglobal(luaState, functionName.c_str());
}

void LuaScriptInterface::registerVariable(const std::string& tableName, const std::string& name, lua_Number value) const
{
	// tableName.name = value
	lua_getglobal(luaState, tableName.c_str());
	setField(luaState, name.c_str(), value);

	// pop tableName
	lua_pop(luaState, 1);
}

void LuaScriptInterface::registerGlobalVariable(const std::string& name, lua_Number value) const
{
	// _G[name] = value
	lua_pushinteger(luaState, value);
	lua_setglobal(luaState, name.c_str());
}

void LuaScriptInterface::registerGlobalBoolean(const std::string& name, bool value) const
{
	// _G[name] = value
	pushBoolean(luaState, value);
	lua_setglobal(luaState, name.c_str());
}

int LuaScriptInterface::luaDoPlayerAddItem(lua_State* L)
{
	//doPlayerAddItem(cid, itemid, <optional: default: 1> count/subtype, <optional: default: 1> canDropOnMap)
	//doPlayerAddItem(cid, itemid, <optional: default: 1> count, <optional: default: 1> canDropOnMap, <optional: default: 1>subtype)
	auto player = getPlayer(L, 1);
	if (!player) {
		reportErrorFunc(L, getErrorDesc(LUA_ERROR_PLAYER_NOT_FOUND));
		pushBoolean(L, false);
		return 1;
	}

	uint16_t itemId = getNumber<uint16_t>(L, 2);
	int32_t count = getNumber<int32_t>(L, 3, 1);
	bool canDropOnMap = getBoolean(L, 4, true);
	uint16_t subType = getNumber<uint16_t>(L, 5, 1);

	const ItemType& it = Item::items[itemId];
	int32_t itemCount;

	auto parameters = lua_gettop(L);
	if (parameters > 4) {
		//subtype already supplied, count then is the amount
		itemCount = std::max<int32_t>(1, count);
	} else if (it.hasSubType()) {
		if (it.stackable) {
			itemCount = static_cast<int32_t>(std::ceil(static_cast<float>(count) / 100));
		} else {
			itemCount = 1;
		}
		subType = count;
	} else {
		itemCount = std::max<int32_t>(1, count);
	}

	while (itemCount > 0) {
		uint16_t stackCount = subType;
		if (it.stackable && stackCount > 100) {
			stackCount = 100;
		}

		auto newItem = Item::CreateItem(itemId, stackCount);
		if (!newItem) {
			reportErrorFunc(L, getErrorDesc(LUA_ERROR_ITEM_NOT_FOUND));
			pushBoolean(L, false);
			return 1;
		}

		if (it.stackable) {
			subType -= stackCount;
		}

		ReturnValue ret = g_game.internalPlayerAddItem(player, newItem, canDropOnMap);
		if (ret != RETURNVALUE_NOERROR) {
			newItem.reset();
			pushBoolean(L, false);
			return 1;
		}

		if (--itemCount == 0) {
			if (newItem->getParent()) {
				uint32_t uid = getScriptEnv()->addThing(newItem);
				lua_pushinteger(L, uid);
				return 1;
			} else {
				//stackable item stacked with existing object, newItem will be released
				pushBoolean(L, false);
				return 1;
			}
		}
	}

	pushBoolean(L, false);
	return 1;
}

int LuaScriptInterface::luaDebugPrint(lua_State* L)
{
	//debugPrint(text)
	reportErrorFunc(L, getString(L, -1));
	return 0;
}

int LuaScriptInterface::luaGetWorldTime(lua_State* L)
{
	//getWorldTime()
	int16_t time = g_game.getWorldTime();
	lua_pushinteger(L, time);
	return 1;
}

int LuaScriptInterface::luaGetWorldLight(lua_State* L)
{
	//getWorldLight()
	LightInfo lightInfo = g_game.getWorldLightInfo();
	lua_pushinteger(L, lightInfo.level);
	lua_pushinteger(L, lightInfo.color);
	return 2;
}

int LuaScriptInterface::luaSetWorldLight(lua_State* L)
{
	//setWorldLight(level, color)
	if (g_config.getBoolean(ConfigManager::DEFAULT_WORLD_LIGHT)) {
		pushBoolean(L, false);
		return 1;
	}

	LightInfo lightInfo;
	lightInfo.level = getNumber<uint8_t>(L, 1);
	lightInfo.color = getNumber<uint8_t>(L, 2);
	g_game.setWorldLightInfo(lightInfo);
	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaGetWorldUpTime(lua_State* L)
{
	//getWorldUpTime()
	uint64_t uptime = (OTSYS_TIME() - ProtocolStatus::start) / 1000;
	lua_pushinteger(L, uptime);
	return 1;
}

int LuaScriptInterface::luaGetSubTypeName(lua_State* L)
{
	// getSubTypeName(subType)
	int32_t subType = getNumber<int32_t>(L, 1);
	if (subType > 0) {
		pushString(L, Item::items[subType].name);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

bool LuaScriptInterface::getArea(lua_State* L, std::vector<uint32_t>& vec, uint32_t& rows)
{
	lua_pushnil(L);
	for (rows = 0; lua_next(L, -2) != 0; ++rows) {
		if (!isTable(L, -1)) {
			return false;
		}

		lua_pushnil(L);
		while (lua_next(L, -2) != 0) {
			if (!isNumber(L, -1)) {
				return false;
			}
			vec.push_back(getNumber<uint32_t>(L, -1));
			lua_pop(L, 1);
		}

		lua_pop(L, 1);
	}

	lua_pop(L, 1);
	return (rows != 0);
}

int LuaScriptInterface::luaCreateCombatArea(lua_State* L)
{
	//createCombatArea( {area}, <optional> {extArea} )
	ScriptEnvironment* env = getScriptEnv();
	if (env->getScriptId() != EVENT_ID_LOADING) {
		reportErrorFunc(L, "This function can only be used while loading the script.");
		pushBoolean(L, false);
		return 1;
	}

	uint32_t areaId = g_luaEnvironment.createAreaObject(env->getScriptInterface());
	AreaCombat* area = g_luaEnvironment.getAreaObject(areaId);

	int parameters = lua_gettop(L);
	if (parameters >= 2) {
		uint32_t rowsExtArea;
		std::vector<uint32_t> vecExtArea;
		if (!isTable(L, 2) || !getArea(L, vecExtArea, rowsExtArea)) {
			reportErrorFunc(L, "Invalid extended area table.");
			pushBoolean(L, false);
			return 1;
		}
		area->setupExtArea(vecExtArea, rowsExtArea);
	}

	uint32_t rowsArea = 0;
	std::vector<uint32_t> vecArea;
	if (!isTable(L, 1) || !getArea(L, vecArea, rowsArea)) {
		reportErrorFunc(L, "Invalid area table.");
		pushBoolean(L, false);
		return 1;
	}

	area->setupArea(vecArea, rowsArea);
	lua_pushinteger(L, areaId);
	return 1;
}

int LuaScriptInterface::luaDoAreaCombat(lua_State* L)
{
	//doAreaCombat(cid, type, pos, area, min, max, effect[, origin = ORIGIN_SPELL[, blockArmor = false[, blockShield = false[, ignoreResistances = false]]]])
	auto creature = getCreature(L, 1);
	if (!creature && (!isNumber(L, 1) || getNumber<uint32_t>(L, 1) != 0)) {
		reportErrorFunc(L, getErrorDesc(LUA_ERROR_CREATURE_NOT_FOUND));
		pushBoolean(L, false);
		return 1;
	}

	uint32_t areaId = getNumber<uint32_t>(L, 4);
	const AreaCombat* area = g_luaEnvironment.getAreaObject(areaId);
	if (area || areaId == 0) {
		CombatType_t combatType = getNumber<CombatType_t>(L, 2);

		CombatParams params;
		params.combatType = combatType;
		params.impactEffect = getNumber<uint8_t>(L, 7);
		params.blockedByArmor = getBoolean(L, 9, false);
		params.blockedByShield = getBoolean(L, 10, false);
		params.ignoreResistances = getBoolean(L, 11, false);

		CombatDamage damage;
		damage.origin = getNumber<CombatOrigin>(L, 8, ORIGIN_SPELL);
		damage.primary.type = combatType;
		damage.primary.value = normal_random(getNumber<int32_t>(L, 5), getNumber<int32_t>(L, 6));

		Combat::doAreaCombat(creature, getPosition(L, 3), area, damage, params);
		pushBoolean(L, true);
	} else {
		reportErrorFunc(L, getErrorDesc(LUA_ERROR_AREA_NOT_FOUND));
		pushBoolean(L, false);
	}
	return 1;
}

int LuaScriptInterface::luaDoTargetCombat(lua_State* L)
{
	//doTargetCombat(cid, target, type, min, max, effect[, origin = ORIGIN_SPELL[, blockArmor = false[, blockShield = false[, ignoreResistances = false]]]])
	auto creature = getCreature(L, 1);
	if (!creature && (!isNumber(L, 1) || getNumber<uint32_t>(L, 1) != 0)) {
		reportErrorFunc(L, getErrorDesc(LUA_ERROR_CREATURE_NOT_FOUND));
		pushBoolean(L, false);
		return 1;
	}

	auto target = getCreature(L, 2);
	if (!target) {
		reportErrorFunc(L, getErrorDesc(LUA_ERROR_CREATURE_NOT_FOUND));
		pushBoolean(L, false);
		return 1;
	}

	CombatType_t combatType = getNumber<CombatType_t>(L, 3);

	CombatParams params;
	params.combatType = combatType;
	params.impactEffect = getNumber<uint8_t>(L, 6);
	params.blockedByArmor = getBoolean(L, 8, false);
	params.blockedByShield = getBoolean(L, 9, false);
	params.ignoreResistances = getBoolean(L, 10, false);

	CombatDamage damage;
	damage.origin = getNumber<CombatOrigin>(L, 7, ORIGIN_SPELL);
	damage.primary.type = combatType;
	damage.primary.value = normal_random(getNumber<int32_t>(L, 4), getNumber<int32_t>(L, 5));

	Combat::doTargetCombat(creature, target, damage, params);
	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaDoChallengeCreature(lua_State* L)
{
	//doChallengeCreature(cid, target[, force = false])
	auto creature = getCreature(L, 1);
	if (!creature) {
		reportErrorFunc(L, getErrorDesc(LUA_ERROR_CREATURE_NOT_FOUND));
		pushBoolean(L, false);
		return 1;
	}

	auto target = getCreature(L, 2);
	if (!target) {
		reportErrorFunc(L, getErrorDesc(LUA_ERROR_CREATURE_NOT_FOUND));
		pushBoolean(L, false);
		return 1;
	}

	target->challengeCreature(creature, getBoolean(L, 3, false));
	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaIsValidUID(lua_State* L)
{
	//isValidUID(uid)
	pushBoolean(L, getScriptEnv()->getThingByUID(getNumber<uint32_t>(L, -1)) != nullptr);
	return 1;
}

int LuaScriptInterface::luaIsDepot(lua_State* L)
{
	//isDepot(uid)
	auto container = getScriptEnv()->getContainerByUID(getNumber<uint32_t>(L, -1));
	pushBoolean(L, container && container->getDepotLocker());
	return 1;
}

int LuaScriptInterface::luaIsMoveable(lua_State* L)
{
	//isMoveable(uid)
	//isMovable(uid)
	auto thing = getScriptEnv()->getThingByUID(getNumber<uint32_t>(L, -1));
	pushBoolean(L, thing && thing->isPushable());
	return 1;
}

int LuaScriptInterface::luaGetDepotId(lua_State* L)
{
	//getDepotId(uid)
	uint32_t uid = getNumber<uint32_t>(L, -1);

	auto container = getScriptEnv()->getContainerByUID(uid);
	if (!container) {
		reportErrorFunc(L, getErrorDesc(LUA_ERROR_CONTAINER_NOT_FOUND));
		pushBoolean(L, false);
		return 1;
	}

	auto depotLocker = container->getDepotLocker();
	if (!depotLocker) {
		reportErrorFunc(L, "Depot not found");
		pushBoolean(L, false);
		return 1;
	}

	lua_pushinteger(L, depotLocker->getDepotId());
	return 1;
}

int LuaScriptInterface::luaAddEvent(lua_State* L)
{
	//addEvent(callback, delay, ...)
	int parameters = lua_gettop(L);
	if (parameters < 2) {
		reportErrorFunc(L, fmt::format("Not enough parameters: {:d}.", parameters));
		pushBoolean(L, false);
		return 1;
	}

	if (!isFunction(L, 1)) {
		reportErrorFunc(L, "callback parameter should be a function.");
		pushBoolean(L, false);
		return 1;
	}

	if (!isNumber(L, 2)) {
		reportErrorFunc(L, "delay parameter should be a number.");
		pushBoolean(L, false);
		return 1;
	}

	if (g_config.getBoolean(ConfigManager::WARN_UNSAFE_SCRIPTS) || g_config.getBoolean(ConfigManager::CONVERT_UNSAFE_SCRIPTS)) {
		std::vector<std::pair<int32_t, LuaDataType>> indexes;
		for (int i = 3; i <= parameters; ++i) {
			if (lua_getmetatable(L, i) == 0) {
				continue;
			}
			lua_rawgeti(L, -1, 't');

			LuaDataType type = getNumber<LuaDataType>(L, -1);
			if (type != LuaData_Unknown && type != LuaData_Tile) {
				indexes.push_back({i, type});
			}
			lua_pop(L, 2);
		}

		if (!indexes.empty()) {
			if (g_config.getBoolean(ConfigManager::WARN_UNSAFE_SCRIPTS)) {
				bool plural = indexes.size() > 1;

				std::string warningString = "Argument";
				if (plural) {
					warningString += 's';
				}

				for (const auto& entry : indexes) {
					if (entry == indexes.front()) {
						warningString += ' ';
					} else if (entry == indexes.back()) {
						warningString += " and ";
					} else {
						warningString += ", ";
					}
					warningString += '#';
					warningString += std::to_string(entry.first);
				}

				if (plural) {
					warningString += " are unsafe";
				} else {
					warningString += " is unsafe";
				}

				reportErrorFunc(L, warningString);
			}

			if (g_config.getBoolean(ConfigManager::CONVERT_UNSAFE_SCRIPTS)) {
				for (const auto& entry : indexes) {
					switch (entry.second) {
						case LuaData_Item:
						case LuaData_Container:
						case LuaData_Teleport: {
							lua_getglobal(L, "Item");
							lua_getfield(L, -1, "getUniqueId");
							break;
						}
						case LuaData_Player:
						case LuaData_Monster:
						case LuaData_Npc: {
							lua_getglobal(L, "Creature");
							lua_getfield(L, -1, "getId");
							break;
						}
						default:
							break;
					}
					lua_replace(L, -2);
					lua_pushvalue(L, entry.first);
					lua_call(L, 1, 1);
					lua_replace(L, entry.first);
				}
			}
		}
	}

	LuaTimerEventDesc eventDesc;
	eventDesc.parameters.reserve(parameters - 2); // safe to use -2 since we garanteed that there is at least two parameters
	for (int i = 0; i < parameters - 2; ++i) {
		eventDesc.parameters.push_back(luaL_ref(L, LUA_REGISTRYINDEX));
	}

	uint32_t delay = std::max<uint32_t>(100, getNumber<uint32_t>(L, 2));
	lua_pop(L, 1);

	eventDesc.function = luaL_ref(L, LUA_REGISTRYINDEX);
	eventDesc.scriptId = getScriptEnv()->getScriptId();

	auto& lastTimerEventId = g_luaEnvironment.lastEventTimerId;
	eventDesc.eventId = g_scheduler.addEvent(createSchedulerTask(delay, [=]() { g_luaEnvironment.executeTimerEvent(lastTimerEventId); }));

	g_luaEnvironment.timerEvents.emplace(lastTimerEventId, std::move(eventDesc));
	lua_pushinteger(L, lastTimerEventId++);
	return 1;
}

int LuaScriptInterface::luaStopEvent(lua_State* L)
{
	//stopEvent(eventid)
	uint32_t eventId = getNumber<uint32_t>(L, 1);

	auto& timerEvents = g_luaEnvironment.timerEvents;
	auto it = timerEvents.find(eventId);
	if (it == timerEvents.end()) {
		pushBoolean(L, false);
		return 1;
	}

	LuaTimerEventDesc timerEventDesc = std::move(it->second);
	timerEvents.erase(it);

	g_scheduler.stopEvent(timerEventDesc.eventId);
	luaL_unref(L, LUA_REGISTRYINDEX, timerEventDesc.function);

	for (auto parameter : timerEventDesc.parameters) {
		luaL_unref(L, LUA_REGISTRYINDEX, parameter);
	}

	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaSaveServer(lua_State* L)
{
	g_globalEvents->save();
	g_game.saveGameState();
	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaCleanMap(lua_State* L)
{
	lua_pushinteger(L, g_game.map.clean());
	return 1;
}

int LuaScriptInterface::luaIsInWar(lua_State* L)
{
	//isInWar(cid, target)
	auto player = getPlayer(L, 1);
	if (!player) {
		reportErrorFunc(L, getErrorDesc(LUA_ERROR_PLAYER_NOT_FOUND));
		pushBoolean(L, false);
		return 1;
	}

	auto targetPlayer = getPlayer(L, 2);
	if (!targetPlayer) {
		reportErrorFunc(L, getErrorDesc(LUA_ERROR_PLAYER_NOT_FOUND));
		pushBoolean(L, false);
		return 1;
	}

	pushBoolean(L, player->isInWar(targetPlayer));
	return 1;
}

int LuaScriptInterface::luaGetWaypointPositionByName(lua_State* L)
{
	//getWaypointPositionByName(name)
	auto& waypoints = g_game.map.waypoints;

	if (const auto it = waypoints.find(getString(L, -1)); it != waypoints.end()) {
		pushPosition(L, it->second);
	} else {
		pushBoolean(L, false);
	}
	return 1;
}

int LuaScriptInterface::luaSendChannelMessage(lua_State* L)
{
	//sendChannelMessage(channelId, type, message)
	uint32_t channelId = getNumber<uint32_t>(L, 1);
	ChatChannel* channel = g_chat->getChannelById(channelId);
	if (!channel) {
		pushBoolean(L, false);
		return 1;
	}

	SpeakClasses type = getNumber<SpeakClasses>(L, 2);
	std::string message = getString(L, 3);
	channel->sendToAll(message, type);
	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaSendGuildChannelMessage(lua_State* L)
{
	//sendGuildChannelMessage(guildId, type, message)
	uint32_t guildId = getNumber<uint32_t>(L, 1);
	ChatChannel* channel = g_chat->getGuildChannelById(guildId);
	if (!channel) {
		pushBoolean(L, false);
		return 1;
	}

	SpeakClasses type = getNumber<SpeakClasses>(L, 2);
	std::string message = getString(L, 3);
	channel->sendToAll(message, type);
	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaIsScriptsInterface(lua_State* L)
{
	//isScriptsInterface()
	if (getScriptEnv()->getScriptInterface() == &g_scripts->getScriptInterface()) {
		pushBoolean(L, true);
	} else {
		reportErrorFunc(L, "EventCallback: can only be called inside (data/scripts/)");
		pushBoolean(L, false);
	}
	return 1;
}

std::string LuaScriptInterface::escapeString(const std::string& string)
{
	std::string s = string;
	replaceString(s, "\\", "\\\\");
	replaceString(s, "\"", "\\\"");
	replaceString(s, "'", "\\'");
	replaceString(s, "[[", "\\[[");
	return s;
}

#ifndef LUAJIT_VERSION
const luaL_Reg LuaScriptInterface::luaBitReg[] = {
	//{"tobit", LuaScriptInterface::luaBitToBit},
	{"bnot", LuaScriptInterface::luaBitNot},
	{"band", LuaScriptInterface::luaBitAnd},
	{"bor", LuaScriptInterface::luaBitOr},
	{"bxor", LuaScriptInterface::luaBitXor},
	{"lshift", LuaScriptInterface::luaBitLeftShift},
	{"rshift", LuaScriptInterface::luaBitRightShift},
	//{"arshift", LuaScriptInterface::luaBitArithmeticalRightShift},
	//{"rol", LuaScriptInterface::luaBitRotateLeft},
	//{"ror", LuaScriptInterface::luaBitRotateRight},
	//{"bswap", LuaScriptInterface::luaBitSwapEndian},
	//{"tohex", LuaScriptInterface::luaBitToHex},
	{nullptr, nullptr}
};

int LuaScriptInterface::luaBitNot(lua_State* L)
{
	lua_pushnumber(L, ~getNumber<uint32_t>(L, -1));
	return 1;
}

#define MULTIOP(name, op) \
int LuaScriptInterface::luaBit##name(lua_State* L) \
{ \
	int n = lua_gettop(L); \
	uint32_t w = getNumber<uint32_t>(L, -1); \
	for (int i = 1; i < n; ++i) \
		w op getNumber<uint32_t>(L, i); \
	lua_pushnumber(L, w); \
	return 1; \
}

MULTIOP(And, &= )
MULTIOP(Or, |= )
MULTIOP(Xor, ^= )

#define SHIFTOP(name, op) \
int LuaScriptInterface::luaBit##name(lua_State* L) \
{ \
	uint32_t n1 = getNumber<uint32_t>(L, 1), n2 = getNumber<uint32_t>(L, 2); \
	lua_pushnumber(L, (n1 op n2)); \
	return 1; \
}

SHIFTOP(LeftShift, << )
SHIFTOP(RightShift, >> )
#endif

const luaL_Reg LuaScriptInterface::luaConfigManagerTable[] = {
	{"getString", LuaScriptInterface::luaConfigManagerGetString},
	{"getNumber", LuaScriptInterface::luaConfigManagerGetNumber},
	{"getBoolean", LuaScriptInterface::luaConfigManagerGetBoolean},
	{nullptr, nullptr}
};

int LuaScriptInterface::luaConfigManagerGetString(lua_State* L)
{
	pushString(L, g_config.getString(getNumber<ConfigManager::string_config_t>(L, -1)));
	return 1;
}
// may need a new one of these for ints, same for registerGlobal methods
int LuaScriptInterface::luaConfigManagerGetNumber(lua_State* L)
{
	lua_pushnumber(L, g_config.getNumber(getNumber<ConfigManager::integer_config_t>(L, -1)));
	return 1;
}

int LuaScriptInterface::luaConfigManagerGetBoolean(lua_State* L)
{
	pushBoolean(L, g_config.getBoolean(getNumber<ConfigManager::boolean_config_t>(L, -1)));
	return 1;
}

const luaL_Reg LuaScriptInterface::luaDatabaseTable[] = {
	{"query", LuaScriptInterface::luaDatabaseExecute},
	{"asyncQuery", LuaScriptInterface::luaDatabaseAsyncExecute},
	{"storeQuery", LuaScriptInterface::luaDatabaseStoreQuery},
	{"asyncStoreQuery", LuaScriptInterface::luaDatabaseAsyncStoreQuery},
	{"escapeString", LuaScriptInterface::luaDatabaseEscapeString},
	{"escapeBlob", LuaScriptInterface::luaDatabaseEscapeBlob},
	{"lastInsertId", LuaScriptInterface::luaDatabaseLastInsertId},
	{"tableExists", LuaScriptInterface::luaDatabaseTableExists},
	{nullptr, nullptr}
};

int LuaScriptInterface::luaDatabaseExecute(lua_State* L)
{
	pushBoolean(L, Database::getInstance().executeQuery(getString(L, -1)));
	return 1;
}

int LuaScriptInterface::luaDatabaseAsyncExecute(lua_State* L)
{
	std::function<void(DBResult_ptr, bool)> callback;
	if (lua_gettop(L) > 1) {
		int32_t ref = luaL_ref(L, LUA_REGISTRYINDEX);
		auto scriptId = getScriptEnv()->getScriptId();
		callback = [ref, scriptId](DBResult_ptr, bool success) {
			lua_State* luaState = g_luaEnvironment.getLuaState();
			if (!luaState) {
				return;
			}

			if (!LuaScriptInterface::reserveScriptEnv()) {
				luaL_unref(luaState, LUA_REGISTRYINDEX, ref);
				return;
			}

			lua_rawgeti(luaState, LUA_REGISTRYINDEX, ref);
			pushBoolean(luaState, success);
			auto env = getScriptEnv();
			env->setScriptId(scriptId, &g_luaEnvironment);
			g_luaEnvironment.callFunction(1);

			luaL_unref(luaState, LUA_REGISTRYINDEX, ref);
		};
	}
	g_databaseTasks.addTask(getString(L, -1), callback);
	return 0;
}

int LuaScriptInterface::luaDatabaseStoreQuery(lua_State* L)
{
	if (DBResult_ptr res = Database::getInstance().storeQuery(getString(L, -1))) {
		lua_pushinteger(L, ScriptEnvironment::addResult(res));
	} else {
		pushBoolean(L, false);
	}
	return 1;
}

int LuaScriptInterface::luaDatabaseAsyncStoreQuery(lua_State* L)
{
	std::function<void(DBResult_ptr, bool)> callback;
	if (lua_gettop(L) > 1) {
		int32_t ref = luaL_ref(L, LUA_REGISTRYINDEX);
		auto scriptId = getScriptEnv()->getScriptId();
		callback = [ref, scriptId](const DBResult_ptr& result, bool) {
			lua_State* luaState = g_luaEnvironment.getLuaState();
			if (!luaState) {
				return;
			}

			if (!LuaScriptInterface::reserveScriptEnv()) {
				luaL_unref(luaState, LUA_REGISTRYINDEX, ref);
				return;
			}

			lua_rawgeti(luaState, LUA_REGISTRYINDEX, ref);
			if (result) {
				lua_pushinteger(luaState, ScriptEnvironment::addResult(result));
			} else {
				pushBoolean(luaState, false);
			}
			auto env = getScriptEnv();
			env->setScriptId(scriptId, &g_luaEnvironment);
			g_luaEnvironment.callFunction(1);

			luaL_unref(luaState, LUA_REGISTRYINDEX, ref);
		};
	}
	g_databaseTasks.addTask(getString(L, -1), callback, true);
	return 0;
}

int LuaScriptInterface::luaDatabaseEscapeString(lua_State* L)
{
	pushString(L, Database::getInstance().escapeString(getString(L, -1)));
	return 1;
}

int LuaScriptInterface::luaDatabaseEscapeBlob(lua_State* L)
{
	uint32_t length = getNumber<uint32_t>(L, 2);
	pushString(L, Database::getInstance().escapeBlob(getString(L, 1).c_str(), length));
	return 1;
}

int LuaScriptInterface::luaDatabaseLastInsertId(lua_State* L)
{
	lua_pushinteger(L, Database::getInstance().getLastInsertId());
	return 1;
}

int LuaScriptInterface::luaDatabaseTableExists(lua_State* L)
{
	pushBoolean(L, DatabaseManager::tableExists(getString(L, -1)));
	return 1;
}

const luaL_Reg LuaScriptInterface::luaResultTable[] = {
	{"getNumber", LuaScriptInterface::luaResultGetNumber},
	{"getString", LuaScriptInterface::luaResultGetString},
	{"getStream", LuaScriptInterface::luaResultGetStream},
	{"next", LuaScriptInterface::luaResultNext},
	{"free", LuaScriptInterface::luaResultFree},
	{nullptr, nullptr}
};

int LuaScriptInterface::luaResultGetNumber(lua_State* L)
{
	DBResult_ptr res = ScriptEnvironment::getResultByID(getNumber<uint32_t>(L, 1));
	if (!res) {
		pushBoolean(L, false);
		return 1;
	}

	const std::string& s = getString(L, 2);
	lua_pushinteger(L, res->getNumber<int64_t>(s));
	return 1;
}

int LuaScriptInterface::luaResultGetString(lua_State* L)
{
	DBResult_ptr res = ScriptEnvironment::getResultByID(getNumber<uint32_t>(L, 1));
	if (!res) {
		pushBoolean(L, false);
		return 1;
	}

	const std::string& s = getString(L, 2);
	pushString(L, res->getString(s));
	return 1;
}

int LuaScriptInterface::luaResultGetStream(lua_State* L)
{
	DBResult_ptr res = ScriptEnvironment::getResultByID(getNumber<uint32_t>(L, 1));
	if (!res) {
		pushBoolean(L, false);
		return 1;
	}

	auto stream = res->getString(getString(L, 2));
	lua_pushlstring(L, stream.data(), stream.size());
	lua_pushinteger(L, stream.size());
	return 2;
}

int LuaScriptInterface::luaResultNext(lua_State* L)
{
	DBResult_ptr res = ScriptEnvironment::getResultByID(getNumber<uint32_t>(L, -1));
	if (!res) {
		pushBoolean(L, false);
		return 1;
	}

	pushBoolean(L, res->next());
	return 1;
}

int LuaScriptInterface::luaResultFree(lua_State* L)
{
	pushBoolean(L, ScriptEnvironment::removeResult(getNumber<uint32_t>(L, -1)));
	return 1;
}

// Userdata
int LuaScriptInterface::luaUserdataCompare(lua_State* L)
{
	// userdataA == userdataB
	pushBoolean(L, getUserdata<void>(L, 1) == getUserdata<void>(L, 2));
	return 1;
}

// _G
int LuaScriptInterface::luaIsType(lua_State* L)
{
	// isType(derived, base)
	lua_getmetatable(L, -2);
	lua_getmetatable(L, -2);

	lua_rawgeti(L, -2, 'p');
	uint_fast8_t parentsB = getNumber<uint_fast8_t>(L, 1);

	lua_rawgeti(L, -3, 'h');
	size_t hashB = getNumber<size_t>(L, 1);

	lua_rawgeti(L, -3, 'p');
	uint_fast8_t parentsA = getNumber<uint_fast8_t>(L, 1);
	for (uint_fast8_t i = parentsA; i < parentsB; ++i) {
		lua_getfield(L, -3, "__index");
		lua_replace(L, -4);
	}

	lua_rawgeti(L, -4, 'h');
	size_t hashA = getNumber<size_t>(L, 1);

	pushBoolean(L, hashA == hashB);
	return 1;
}

int LuaScriptInterface::luaRawGetMetatable(lua_State* L)
{
	// rawgetmetatable(metatableName)
	luaL_getmetatable(L, getString(L, 1).c_str());
	return 1;
}

// os
int LuaScriptInterface::luaSystemTime(lua_State* L)
{
	// os.mtime()
	lua_pushinteger(L, OTSYS_TIME());
	return 1;
}

// table
int LuaScriptInterface::luaTableCreate(lua_State* L)
{
	// table.create(arrayLength, keyLength)
	lua_createtable(L, getNumber<int32_t>(L, 1), getNumber<int32_t>(L, 2));
	return 1;
}

int LuaScriptInterface::luaTablePack(lua_State* L)
{
	// table.pack(...)
	int i;
	int n = lua_gettop(L); /* number of elements to pack */
	lua_createtable(L, n, 1); /* create result table */
	lua_insert(L, 1); /* put it at index 1 */
	for (i = n; i >= 1; i--) { /* assign elements */
		lua_rawseti(L, 1, i);
	}
	if (luaL_callmeta(L, -1, "__index") != 0) {
		lua_replace(L, -2);
	}
	lua_pushinteger(L, n);
	lua_setfield(L, 1, "n"); /* t.n = number of elements */
	return 1; /* return table */
}

// DB Insert
int LuaScriptInterface::luaDBInsertCreate(lua_State* L)
{
	// DBInsert(query)
	if (isString(L, 2)) {
		pushUserdata<DBInsert>(L, new DBInsert(getString(L, 2)));
		setMetatable(L, -1, "DBInsert");
	}
	else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaDBInsertDelete(lua_State* L)
{
	DBInsert** insertPtr = getRawUserdata<DBInsert>(L, 1);
	if (insertPtr && *insertPtr) {
		delete* insertPtr;
		*insertPtr = nullptr;
	}
	return 0;
}

int LuaScriptInterface::luaDBInsertAddRow(lua_State* L)
{
	// insert:addRow(row)
	DBInsert* insert = getUserdata<DBInsert>(L, 1);
	if (insert) {
		pushBoolean(L, insert->addRow(getString(L, 2)));
	}
	else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaDBInsertExecute(lua_State* L)
{
	// insert:execute()
	DBInsert* insert = getUserdata<DBInsert>(L, 1);
	if (insert) {
		pushBoolean(L, insert->execute());
	}
	else {
		lua_pushnil(L);
	}
	return 1;
}

// DB Transaction
int LuaScriptInterface::luaDBTransactionCreate(lua_State* L)
{
	// DBTransaction()
	pushUserdata<DBTransaction>(L, new DBTransaction);
	setMetatable(L, -1, "DBTransaction");
	return 1;
}

int LuaScriptInterface::luaDBTransactionBegin(lua_State* L)
{
	// transaction:begin()
	DBTransaction* transaction = getUserdata<DBTransaction>(L, 1);
	if (transaction) {
		pushBoolean(L, transaction->begin());
	}
	else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaDBTransactionCommit(lua_State* L)
{
	// transaction:commit()
	DBTransaction* transaction = getUserdata<DBTransaction>(L, 1);
	if (transaction) {
		pushBoolean(L, transaction->commit());
	}
	else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaDBTransactionDelete(lua_State* L)
{
	DBTransaction** transactionPtr = getRawUserdata<DBTransaction>(L, 1);
	if (transactionPtr && *transactionPtr) {
		delete* transactionPtr;
		*transactionPtr = nullptr;
	}
	return 0;
}

// Game
int LuaScriptInterface::luaGameGetSpectators(lua_State* L)
{
	// Game.getSpectators(position[, multifloor = false[, onlyPlayer = false[, minRangeX = 0[, maxRangeX = 0[, minRangeY = 0[, maxRangeY = 0]]]]]])
	const Position& position = getPosition(L, 1);
	bool multifloor = getBoolean(L, 2, false);
	bool onlyPlayers = getBoolean(L, 3, false);
	int32_t minRangeX = getNumber<int32_t>(L, 4, 0);
	int32_t maxRangeX = getNumber<int32_t>(L, 5, 0);
	int32_t minRangeY = getNumber<int32_t>(L, 6, 0);
	int32_t maxRangeY = getNumber<int32_t>(L, 7, 0);

	SpectatorVec spectators;
	g_game.map.getSpectators(spectators, position, multifloor, onlyPlayers, minRangeX, maxRangeX, minRangeY, maxRangeY);

	lua_createtable(L, spectators.size(), 0);

	int index = 0;
	for (auto creature : spectators) {
		pushSharedPtr(L, creature);
		setCreatureMetatable(L, -1, creature);
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaGameGetPlayers(lua_State* L)
{
	// Game.getPlayers()
	lua_createtable(L, g_game.getPlayersOnline(), 0);

	int index = 0;
	for (const auto& val : g_game.getPlayers() | std::views::values) {
		pushSharedPtr(L, val);
		setMetatable(L, -1, "Player");
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaGameGetNpcs(lua_State* L)
{
	// Game.getNpcs()
	lua_createtable(L, g_game.getNpcsOnline(), 0);

	int index = 0;
	for (const auto& val : g_game.getNpcs() | std::views::values) {
		pushSharedPtr(L, val);
		setMetatable(L, -1, "Npc");
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaGameGetMonsters(lua_State* L)
{
	// Game.getMonsters()
	lua_createtable(L, g_game.getMonstersOnline(), 0);

	int index = 0;
	for (const auto& val : g_game.getMonsters() | std::views::values) {
		pushSharedPtr(L, val);
		setMetatable(L, -1, "Monster");
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaGameLoadMap(lua_State* L)
{
	// Game.loadMap(path)
	const std::string& path = getString(L, 1);
	g_dispatcher.addTask(createTask([path]() {
		try {
			g_game.loadMap(path);
		} catch (const std::exception& e) {
			// FIXME: Should only catch some exceptions
			std::cout << "[Error - LuaScriptInterface::luaGameLoadMap] Failed to load map: "
				<< e.what() << std::endl;
		}
	}));
	return 0;
}

int LuaScriptInterface::luaGameGetExperienceStage(lua_State* L)
{
	// Game.getExperienceStage(level)
	uint32_t level = getNumber<uint32_t>(L, 1);
	lua_pushinteger(L, g_config.getExperienceStage(level));
	return 1;
}

int LuaScriptInterface::luaGameGetExperienceForLevel(lua_State* L)
{
	// Game.getExperienceForLevel(level)
	const uint32_t level = getNumber<uint32_t>(L, 1);
	if (level == 0) {
		lua_pushinteger(L, 0);
	} else {
		lua_pushinteger(L, Player::getExpForLevel(level));
	}
	return 1;
}

int LuaScriptInterface::luaGameGetMonsterCount(lua_State* L)
{
	// Game.getMonsterCount()
	lua_pushinteger(L, g_game.getMonstersOnline());
	return 1;
}

int LuaScriptInterface::luaGameGetPlayerCount(lua_State* L)
{
	// Game.getPlayerCount()
	lua_pushinteger(L, g_game.getPlayersOnline());
	return 1;
}

int LuaScriptInterface::luaGameGetNpcCount(lua_State* L)
{
	// Game.getNpcCount()
	lua_pushinteger(L, g_game.getNpcsOnline());
	return 1;
}

int LuaScriptInterface::luaGameGetMonsterTypes(lua_State* L)
{
	// Game.getMonsterTypes()
	auto& type = g_monsters.monsters;
	lua_createtable(L, type.size(), 0);

	for (auto& mType : type) {
		pushUserdata<MonsterType>(L, &mType.second);
		setMetatable(L, -1, "MonsterType");
		lua_setfield(L, -2, mType.first.c_str());
	}
	return 1;
}

int LuaScriptInterface::luaGameGetCurrencyItems(lua_State* L)
{
	// Game.getCurrencyItems()
	const auto& currencyItems = Item::items.currencyItems;
	size_t size = currencyItems.size();
	lua_createtable(L, size, 0);

	for (const auto& val : currencyItems | std::views::values) {
		const ItemType& itemType = Item::items[val];
		pushUserdata<const ItemType>(L, &itemType);
		setMetatable(L, -1, "ItemType");
		lua_rawseti(L, -2, size--);
	}
	return 1;
}

int LuaScriptInterface::luaGameGetTowns(lua_State* L)
{
	// Game.getTowns()
	const auto& towns = g_game.map.towns.getTowns();
	lua_createtable(L, towns.size(), 0);

	int index = 0;
	for (auto val : towns | std::views::values) {
		pushUserdata<Town>(L, val);
		setMetatable(L, -1, "Town");
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaGameGetHouses(lua_State* L)
{
	// Game.getHouses()
	const auto& houses = g_game.map.houses.getHouses();
	lua_createtable(L, houses.size(), 0);

	int index = 0;
	for (auto val : houses | std::views::values) {
		pushUserdata<House>(L, val);
		setMetatable(L, -1, "House");
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}
int LuaScriptInterface::luaGameGetOutfits(lua_State* L)
{
	// Game.getOutfits(playerSex)
	if (!isNumber(L, 1)) {
		lua_pushnil(L);
		return 1;
	}

	PlayerSex_t playerSex = getNumber<PlayerSex_t>(L, 1);
	if (playerSex > PLAYERSEX_LAST) {
		lua_pushnil(L);
		return 1;
	}

	const auto& outfits = Outfits::getInstance().getOutfits(playerSex);
	lua_createtable(L, outfits.size(), 0);

	int index = 0;
	for (const auto& outfit : outfits) {
		pushOutfit(L, &outfit);
		lua_rawseti(L, -2, ++index);
	}

	return 1;
}

int LuaScriptInterface::luaGameGetMounts(lua_State* L)
{
	// Game.getMounts()
	const auto& mounts = g_game.mounts.getMounts();
	lua_createtable(L, mounts.size(), 0);
	int index = 0;
	for (const auto& mount : mounts) {
		pushMount(L, &mount);
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaGameGetVocations(lua_State* L)
{
	// Game.getVocations()
	const auto& vocations = g_vocations.getVocations();
	lua_createtable(L, vocations.size(), 0);

	int index = 0;
	for (const auto& [id, vocation] : vocations) {
		pushUserdata<const Vocation>(L, &vocation);
		setMetatable(L, -1, "Vocation");
		lua_rawseti(L, -2, ++index);
	}

	return 1;
}

int LuaScriptInterface::luaGameGetGameState(lua_State* L)
{
	// Game.getGameState()
	lua_pushinteger(L, g_game.getGameState());
	return 1;
}

int LuaScriptInterface::luaGameSetGameState(lua_State* L)
{
	// Game.setGameState(state)
	GameState_t state = getNumber<GameState_t>(L, 1);
	g_game.setGameState(state);
	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaGameGetWorldType(lua_State* L)
{
	// Game.getWorldType()
	lua_pushinteger(L, g_game.getWorldType());
	return 1;
}

int LuaScriptInterface::luaGameSetWorldType(lua_State* L)
{
	// Game.setWorldType(type)
	WorldType_t type = getNumber<WorldType_t>(L, 1);
	g_game.setWorldType(type);
	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaGameGetReturnMessage(lua_State* L)
{
	// Game.getReturnMessage(value)
	ReturnValue value = getNumber<ReturnValue>(L, 1);
	pushString(L, getReturnMessage(value));
	return 1;
}

int LuaScriptInterface::luaGameGetItemAttributeByName(lua_State* L)
{
	// Game.getItemAttributeByName(name)
	lua_pushinteger(L, stringToItemAttribute(getString(L, 1)));
	return 1;
}

int LuaScriptInterface::luaGameCreateItem(lua_State* L)
{
	// Game.createItem(itemId[, count[, position]])
	uint16_t count = getNumber<uint16_t>(L, 2, 1);
	uint16_t id;
	if (isNumber(L, 1)) {
		id = getNumber<uint16_t>(L, 1);
	} else {
		id = Item::items.getItemIdByName(getString(L, 1));
		if (id == 0) {
			lua_pushnil(L);
			return 1;
		}
	}

	const ItemType& it = Item::items[id];
	if (it.stackable) {
		count = std::min<uint16_t>(count, 100);
	}

	auto item = Item::CreateItem(id, count);
	if (!item) {
		lua_pushnil(L);
		return 1;
	}

	if (lua_gettop(L) >= 3) {
		const Position& position = getPosition(L, 3);
		CylinderPtr tile = g_game.map.getTile(position);
		if (!tile) {
			item.reset();
			lua_pushnil(L);
			return 1;
		}

		g_game.internalAddItem(tile, item, INDEX_WHEREEVER, FLAG_NOLIMIT);
	} else {
		getScriptEnv()->addTempItem(item);
		item->setParent(VirtualCylinder::virtualCylinder);
	}

	pushSharedPtr(L, item);
	setItemMetatable(L, -1, item);
	return 1;
}

int LuaScriptInterface::luaGameCreateContainer(lua_State* L)
{
	// Game.createContainer(itemId, size[, position])
	uint16_t size = getNumber<uint16_t>(L, 2);
	uint16_t id;
	if (isNumber(L, 1)) {
		id = getNumber<uint16_t>(L, 1);
	} else {
		id = Item::items.getItemIdByName(getString(L, 1));
		if (id == 0) {
			lua_pushnil(L);
			return 1;
		}
	}

	auto container = Item::CreateItemAsContainer(id, size);
	if (!container) {
		lua_pushnil(L);
		return 1;
	}

	if (lua_gettop(L) >= 3) {
		const Position& position = getPosition(L, 3);
		CylinderPtr tile = g_game.map.getTile(position);
		if (!tile) {
			container.reset();
			lua_pushnil(L);
			return 1;
		}

		g_game.internalAddItem(tile, container, INDEX_WHEREEVER, FLAG_NOLIMIT);
	} else {
		getScriptEnv()->addTempItem(container);
		container->setParent(VirtualCylinder::virtualCylinder);
	}

	pushSharedPtr(L, container);
	setMetatable(L, -1, "Container");
	return 1;
}

int LuaScriptInterface::luaGameCreateMonster(lua_State* L)
{
	// Game.createMonster(monsterName, position[, extended = false[, force = false[, magicEffect = CONST_ME_TELEPORT]]])
	auto monster = Monster::createMonster(getString(L, 1));
	if (!monster) {
		lua_pushnil(L);
		return 1;
	}

	const Position& position = getPosition(L, 2);
	bool extended = getBoolean(L, 3, false);
	bool force = getBoolean(L, 4, false);
	MagicEffectClasses magicEffect = getNumber<MagicEffectClasses>(L, 5, CONST_ME_TELEPORT);
	if (g_events->eventMonsterOnSpawn(monster, position, false, true) || force) {
		if (g_game.placeCreature(monster, position, extended, force, magicEffect)) {
			pushSharedPtr(L, monster);
			setMetatable(L, -1, "Monster");
		} else {
			lua_pushnil(L);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaGameCreateNpc(lua_State* L)
{
	// Game.createNpc(npcName, position[, extended = false[, force = false[, magicEffect = CONST_ME_TELEPORT]]])
	auto npc = Npc::createNpc(getString(L, 1));
	if (!npc) {
		lua_pushnil(L);
		return 1;
	}

	const Position& position = getPosition(L, 2);
	bool extended = getBoolean(L, 3, false);
	bool force = getBoolean(L, 4, false);
	MagicEffectClasses magicEffect = getNumber<MagicEffectClasses>(L, 5, CONST_ME_TELEPORT);
	if (g_game.placeCreature(npc, position, extended, force, magicEffect)) {
		pushSharedPtr(L, npc);
		setMetatable(L, -1, "Npc");
	} else {
		npc.reset();
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaGameCreateTile(lua_State* L)
{
	// Game.createTile(x, y, z[, isDynamic = false])
	// Game.createTile(position[, isDynamic = false])
	Position position;
	bool isDynamic;
	if (isTable(L, 1)) {
		position = getPosition(L, 1);
		isDynamic = getBoolean(L, 2, false);
	} else {
		position.x = getNumber<uint16_t>(L, 1);
		position.y = getNumber<uint16_t>(L, 2);
		position.z = getNumber<uint16_t>(L, 3);
		isDynamic = getBoolean(L, 4, false);
	}

	TilePtr tile = g_game.map.getTile(position);
	if (!tile) {
		tile = std::make_shared<Tile>(position.x, position.y, position.z);
		g_game.map.setTile(position, tile);
	}

	pushSharedPtr(L, tile);
	setMetatable(L, -1, "Tile");
	return 1;
}

int LuaScriptInterface::luaGameCreateMonsterType(lua_State* L)
{
	// Game.createMonsterType(name)
	if (getScriptEnv()->getScriptInterface() != &g_scripts->getScriptInterface()) {
		reportErrorFunc(L, "MonsterTypes can only be registered in the Scripts interface.");
		lua_pushnil(L);
		return 1;
	}

	const std::string& name = getString(L, 1);
	if (name.length() == 0) {
		lua_pushnil(L);
		return 1;
	}

	MonsterType* monsterType = g_monsters.getMonsterType(name, false);
	if (!monsterType) {
		monsterType = &g_monsters.monsters[asLowerCaseString(name)];
		monsterType->name = name;
		monsterType->nameDescription = "a " + name;
	} else {
		monsterType->info.lootItems.clear();
		monsterType->info.attackSpells.clear();
		monsterType->info.defenseSpells.clear();
		monsterType->info.scripts.clear();
		monsterType->info.thinkEvent = -1;
		monsterType->info.creatureAppearEvent = -1;
		monsterType->info.creatureDisappearEvent = -1;
		monsterType->info.creatureMoveEvent = -1;
		monsterType->info.creatureSayEvent = -1;
	}

	pushUserdata<MonsterType>(L, monsterType);
	setMetatable(L, -1, "MonsterType");
	return 1;
}

int LuaScriptInterface::luaGameStartRaid(lua_State* L)
{
	// Game.startRaid(raidName)
	const std::string& raidName = getString(L, 1);

	const auto raid = g_game.raids.getRaidByName(raidName);
	if (!raid || !raid->isLoaded()) {
		lua_pushinteger(L, RETURNVALUE_NOSUCHRAIDEXISTS);
		return 1;
	}

	if (g_game.raids.getRunning()) {
		lua_pushinteger(L, RETURNVALUE_ANOTHERRAIDISALREADYEXECUTING);
		return 1;
	}

	g_game.raids.setRunning(raid);
	raid->startRaid();
	lua_pushinteger(L, RETURNVALUE_NOERROR);
	return 1;
}

int LuaScriptInterface::luaGameGetClientVersion(lua_State* L)
{
	// Game.getClientVersion()
	lua_createtable(L, 0, 3);
	setField(L, "min", CLIENT_VERSION_MIN);
	setField(L, "max", CLIENT_VERSION_MAX);
	setField(L, "string", static_cast<std::string>(CLIENT_VERSION_STR));
	return 1;
}

int LuaScriptInterface::luaGameReload(lua_State* L)
{
	// Game.reload(reloadType)
	ReloadTypes_t reloadType = getNumber<ReloadTypes_t>(L, 1);
	if (reloadType == RELOAD_TYPE_GLOBAL) {
		pushBoolean(L, g_luaEnvironment.loadFile("data/global.lua") == 0);
		pushBoolean(L, g_scripts->loadScripts("scripts/lib", true, true));
		lua_gc(g_luaEnvironment.getLuaState(), LUA_GCCOLLECT, 0);
		return 2;
	}
	pushBoolean(L, g_game.reload(reloadType));
	lua_gc(g_luaEnvironment.getLuaState(), LUA_GCCOLLECT, 0);
	return 1;
}

int LuaScriptInterface::luaGameGetAccountStorageValue(lua_State* L)
{
	// Game.getAccountStorageValue(accountId, key)
	uint32_t accountId = getNumber<uint32_t>(L, 1);
	uint32_t key = getNumber<uint32_t>(L, 2);

	lua_pushinteger(L, g_game.getAccountStorageValue(accountId, key));

	return 1;
}

int LuaScriptInterface::luaGameSetAccountStorageValue(lua_State* L)
{
	// Game.setAccountStorageValue(accountId, key, value)
	uint32_t accountId = getNumber<uint32_t>(L, 1);
	uint32_t key = getNumber<uint32_t>(L, 2);
	int32_t value = getNumber<int32_t>(L, 3);

	g_game.setAccountStorageValue(accountId, key, value);
	lua_pushboolean(L, true);

	return 1;
}

int LuaScriptInterface::luaGameSaveAccountStorageValues(lua_State* L)
{
	// Game.saveAccountStorageValues()
	lua_pushboolean(L, g_game.saveAccountStorageValues());

	return 1;
}

int LuaScriptInterface::luaGameSendDiscordWebhook(lua_State* L)
{
	// Game.sendDiscordMessage(token, message_type, message)

	std::string token;
	DiscordMessageType messageType;
	std::string msg;

	if (isString(L, 1))
		token = getString(L, 1);
	if (isNumber(L, 2))
		messageType = getNumber<DiscordMessageType>(L, 2);
	if (isString(L, 3))
		msg = getString(L, 3);

	if (token.length() > 0 && msg.length() > 0 && messageType >= 0) {
		// "{\"content\": null,\"embeds\":[{\"title\": \"testing\",\"description\": \"tetsign\",\"color\": 4062976}],\"attachments\": []}"
		std::string field;
		struct curl_slist* headers = nullptr;
		headers = curl_slist_append(headers, "Content-Type: application/json");

		switch (messageType) {
		case DiscordMessageType::MESSAGE_NORMAL:
			field = R"({
				"content": null,
				"embeds" : [{
					"title": "~MESSAGE~",
					"description" : ")" + msg + R"(",
					"color" : 1815333
				}]
			})";
			break;
		case DiscordMessageType::MESSAGE_ERROR:
			field = R"({
				"content": null,
				"embeds" : [{
					"title": "~ERROR~",
					"description" : ")" + msg + R"(",
					"color" : 16711680
				}]
			})";
			break;
		case DiscordMessageType::MESSAGE_LOG:
			field = R"({
				"content": null,
				"embeds" : [{
					"title": "~LOG~",
					"description" : ")" + msg + R"(",
					"color" : 41727
				}]
			})";
			break;
		case DiscordMessageType::MESSAGE_INFO:
			field = R"({
				"content": null,
				"embeds" : [{
					"title": "~INFO~",
					"description" : ")" + msg + R"(",
					"color" : 16762880
				}]
			})";
			break;

		default:
			return 1;
		}

		auto curl = g_game.curl;

		g_dispatcher_discord.addTask(createTask([curl, token, field, headers]() {
			curl_easy_setopt(curl, CURLOPT_URL, token.data());
			curl_easy_setopt(curl, CURLOPT_POSTFIELDS, field.data());
			curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

			auto response = curl_easy_perform(curl);

			if (response != CURLE_OK)
				std::cout << "Curl failed - reason: " << curl_easy_strerror(response) << std::endl;
		}));
	}

	return 1;
}

// Variant
int LuaScriptInterface::luaVariantCreate(lua_State* L)
{
	// Variant(number or string or position or thing)
	LuaVariant variant;
	if (isUserdata(L, 2)) {
		if (auto thing = getThing(L, 2)) {
			variant.setTargetPosition(thing->getPosition());
		}
	} else if (isTable(L, 2)) {
		variant.setPosition(getPosition(L, 2));
	} else if (isNumber(L, 2)) {
		variant.setNumber(getNumber<uint32_t>(L, 2));
	} else if (isString(L, 2)) {
		variant.setString(getString(L, 2));
	}
	pushVariant(L, variant);
	return 1;
}

int LuaScriptInterface::luaVariantGetNumber(lua_State* L)
{
	// Variant:getNumber()
	const LuaVariant& variant = getVariant(L, 1);
	if (variant.isNumber()) {
		lua_pushinteger(L, variant.getNumber());
	} else {
		lua_pushinteger(L, 0);
	}
	return 1;
}

int LuaScriptInterface::luaVariantGetString(lua_State* L)
{
	// Variant:getString()
	const LuaVariant& variant = getVariant(L, 1);
	if (variant.isString()) {
		pushString(L, variant.getString());
	} else {
		pushString(L, std::string());
	}
	return 1;
}

int LuaScriptInterface::luaVariantGetPosition(lua_State* L)
{
	// Variant:getPosition()
	const LuaVariant& variant = getVariant(L, 1);
	if (variant.isPosition()) {
		pushPosition(L, variant.getPosition());
	}else if (variant.isTargetPosition()) {
		pushPosition(L, variant.getTargetPosition());
	} else {
		pushPosition(L, Position());
	}
	return 1;
}

// Position
int LuaScriptInterface::luaPositionCreate(lua_State* L)
{
	// Position([x = 0[, y = 0[, z = 0[, stackpos = 0]]]])
	// Position([position])
	if (lua_gettop(L) <= 1) {
		pushPosition(L, Position());
		return 1;
	}

	int32_t stackpos;
	if (isTable(L, 2)) {
		const Position& position = getPosition(L, 2, stackpos);
		pushPosition(L, position, stackpos);
	} else {
		uint16_t x = getNumber<uint16_t>(L, 2, 0);
		uint16_t y = getNumber<uint16_t>(L, 3, 0);
		uint8_t z = getNumber<uint8_t>(L, 4, 0);
		stackpos = getNumber<int32_t>(L, 5, 0);

		pushPosition(L, Position(x, y, z), stackpos);
	}
	return 1;
}

int LuaScriptInterface::luaPositionAdd(lua_State* L)
{
	// positionValue = position + positionEx
	int32_t stackpos;
	const Position& position = getPosition(L, 1, stackpos);

	Position positionEx;
	if (stackpos == 0) {
		positionEx = getPosition(L, 2, stackpos);
	} else {
		positionEx = getPosition(L, 2);
	}

	pushPosition(L, position + positionEx, stackpos);
	return 1;
}

int LuaScriptInterface::luaPositionSub(lua_State* L)
{
	// positionValue = position - positionEx
	int32_t stackpos;
	const Position& position = getPosition(L, 1, stackpos);

	Position positionEx;
	if (stackpos == 0) {
		positionEx = getPosition(L, 2, stackpos);
	} else {
		positionEx = getPosition(L, 2);
	}

	pushPosition(L, position - positionEx, stackpos);
	return 1;
}

int LuaScriptInterface::luaPositionCompare(lua_State* L)
{
	// position == positionEx
	const Position& positionEx = getPosition(L, 2);
	const Position& position = getPosition(L, 1);
	pushBoolean(L, position == positionEx);
	return 1;
}

int LuaScriptInterface::luaPositionGetDistance(lua_State* L)
{
	// position:getDistance(positionEx)
	const Position& positionEx = getPosition(L, 2);
	const Position& position = getPosition(L, 1);
	lua_pushinteger(L, std::max<int32_t>(
		std::max<int32_t>(
			std::abs(Position::getDistanceX(position, positionEx)),
			std::abs(Position::getDistanceY(position, positionEx))
		),
		std::abs(Position::getDistanceZ(position, positionEx))
	));
	return 1;
}

int LuaScriptInterface::luaPositionIsSightClear(lua_State* L)
{
	// position:isSightClear(positionEx[, sameFloor = true])
	bool sameFloor = getBoolean(L, 3, true);
	const Position& positionEx = getPosition(L, 2);
	const Position& position = getPosition(L, 1);
	pushBoolean(L, g_game.isSightClear(position, positionEx, sameFloor));
	return 1;
}

int LuaScriptInterface::luaPositionSendMagicEffect(lua_State* L)
{
	// position:sendMagicEffect(magicEffect[, player = nullptr])
	SpectatorVec spectators;
	if (lua_gettop(L) >= 3) {
		if (const auto player = getPlayer(L, 3)) {
			spectators.emplace_back(player);
		}
	}

	MagicEffectClasses magicEffect = getNumber<MagicEffectClasses>(L, 2);
	if (magicEffect == CONST_ME_NONE) {
		pushBoolean(L, false);
		return 1;
	}
	const Position& position = getPosition(L, 1);
	if (!spectators.empty()) {
		Game::addMagicEffect(spectators, position, magicEffect);
	} else {
		g_game.addMagicEffect(position, magicEffect);
	}

	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaPositionSendDistanceEffect(lua_State* L)
{
	// position:sendDistanceEffect(positionEx, distanceEffect[, player = nullptr])
	SpectatorVec spectators;
	if (lua_gettop(L) >= 4) {
		if (const auto player = getPlayer(L, 4)) {
			spectators.emplace_back(player);
		}
	}

	ShootType_t distanceEffect = getNumber<ShootType_t>(L, 3);
	const Position& positionEx = getPosition(L, 2);
	const Position& position = getPosition(L, 1);
	if (!spectators.empty()) {
		Game::addDistanceEffect(spectators, position, positionEx, distanceEffect);
	} else {
		g_game.addDistanceEffect(position, positionEx, distanceEffect);
	}

	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaPositionGetZones(lua_State* L)
{
	// position:getZones()
	// returns table of zone id's
	if (isTable(L, 1))
	{
		auto position = getPosition(L, 1);
		auto zones = Zones::getZonesByPosition(position);
		auto index = 0;
		lua_createtable(L, zones.size(), 0);

		for (auto& zone : zones)
		{
			lua_pushnumber(L, zone);
			lua_rawseti(L, -2, ++index);
		}
		return 1;
	}
	// should be unreachable
	lua_pushnil(L);
	return 1;
}

int LuaScriptInterface::luaPositionHasZone(lua_State* L)
{
	// position:hasZones()
	// returns true / false
	if (isTable(L, 1))
	{
		auto position = getPosition(L, 1);
		auto zones = Zones::getZonesByPosition(position);
		if (zones.empty()) 
		{
			pushBoolean(L, false);
			return 1;
		} 
		// else
		pushBoolean(L, true);
		return 1;
	}
	// should be unreachable
	return 0;
}

// Tile
int LuaScriptInterface::luaTileCreate(lua_State* L)
{
	// Tile(x, y, z)
	// Tile(position)
	TilePtr tile;
	if (isTable(L, 2)) {
		tile = g_game.map.getTile(getPosition(L, 2));
	} else {
		uint8_t z = getNumber<uint8_t>(L, 4);
		uint16_t y = getNumber<uint16_t>(L, 3);
		uint16_t x = getNumber<uint16_t>(L, 2);
		tile = g_game.map.getTile(x, y, z);
	}

	if (tile) {
		pushSharedPtr(L, tile);
		setMetatable(L, -1, "Tile");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaTileDelete(lua_State* L) 
{
	if (auto& tile = getSharedPtr<Tile>(L, 1)) {
		tile.reset();
	}
	return 0;
}

int LuaScriptInterface::luaTileRemove(lua_State* L)
{
	// tile:remove()
	const auto tile = getSharedPtr<Tile>(L, 1);
	if (!tile) {
		lua_pushnil(L);
		return 1;
	}

	if (g_game.isTileInCleanList(tile)) {
		g_game.removeTileToClean(tile);
	}

	g_game.map.removeTile(tile->getPosition());
	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaTileGetPosition(lua_State* L)
{
	// tile:getPosition()
	if (const auto tile = getSharedPtr<Tile>(L, 1)) {
		pushPosition(L, tile->getPosition());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaTileGetGround(lua_State* L)
{
	// tile:getGround()
	auto tile = getSharedPtr<Tile>(L, 1);
	if (tile && tile->getGround()) {
		pushSharedPtr(L, tile->getGround());
		setItemMetatable(L, -1, tile->getGround());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaTileGetThing(lua_State* L)
{
	// tile:getThing(index)
	int32_t index = getNumber<int32_t>(L, 2);
	const auto tile = getSharedPtr<Tile>(L, 1);
	if (!tile) {
		lua_pushnil(L);
		return 1;
	}

	const auto thing = tile->getThing(index);
	if (!thing) {
		lua_pushnil(L);
		return 1;
	}

	if (const auto creature = thing->getCreature()) {
		pushSharedPtr(L, creature);
		setCreatureMetatable(L, -1, creature);
	} else if (const auto item = thing->getItem()) {
		pushSharedPtr(L, item);
		setItemMetatable(L, -1, item);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaTileGetThingCount(lua_State* L)
{
	// tile:getThingCount()
	if (const auto tile = getSharedPtr<Tile>(L, 1)) {
		lua_pushinteger(L, tile->getThingCount());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaTileGetTopVisibleThing(lua_State* L)
{
	// tile:getTopVisibleThing(creature)
	const auto creature = getCreature(L, 2);
	const auto tile = getSharedPtr<Tile>(L, 1);
	if (!tile) {
		lua_pushnil(L);
		return 1;
	}

	const auto thing = tile->getTopVisibleThing(creature);
	if (!thing) {
		lua_pushnil(L);
		return 1;
	}

	if (const auto visibleCreature = thing->getCreature()) {
		pushSharedPtr(L, visibleCreature);
		setCreatureMetatable(L, -1, visibleCreature);
	} else if (const auto visibleItem = thing->getItem()) {
		pushSharedPtr(L, visibleItem);
		setItemMetatable(L, -1, visibleItem);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaTileGetTopTopItem(lua_State* L)
{
	// tile:getTopTopItem()
	const auto tile = getSharedPtr<Tile>(L, 1);
	if (!tile) {
		lua_pushnil(L);
		return 1;
	}

	if (const auto item = tile->getTopTopItem()) {
		pushSharedPtr(L, item);
		setItemMetatable(L, -1, item);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaTileGetTopDownItem(lua_State* L)
{
	// tile:getTopDownItem()
	const auto tile = getSharedPtr<Tile>(L, 1);
	if (!tile) {
		lua_pushnil(L);
		return 1;
	}

	if (const auto item = tile->getTopDownItem()) {
		pushSharedPtr(L, item);
		setItemMetatable(L, -1, item);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaTileGetFieldItem(lua_State* L)
{
	// tile:getFieldItem()
	const auto tile = getSharedPtr<Tile>(L, 1);
	if (!tile) {
		lua_pushnil(L);
		return 1;
	}

	if (const auto item = tile->getFieldItem()) {
		pushSharedPtr(L, item);
		setItemMetatable(L, -1, item);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaTileGetItemById(lua_State* L)
{
	// tile:getItemById(itemId[, subType = -1])
	const auto tile = getSharedPtr<Tile>(L, 1);
	if (!tile) {
		lua_pushnil(L);
		return 1;
	}

	uint16_t itemId;
	if (isNumber(L, 2)) {
		itemId = getNumber<uint16_t>(L, 2);
	} else {
		itemId = Item::items.getItemIdByName(getString(L, 2));
		if (itemId == 0) {
			lua_pushnil(L);
			return 1;
		}
	}
	int32_t subType = getNumber<int32_t>(L, 3, -1);

	if (const auto item = g_game.findItemOfType(tile, itemId, false, subType)) {
		pushSharedPtr(L, item);
		setItemMetatable(L, -1, item);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaTileGetItemByType(lua_State* L)
{
	// tile:getItemByType(itemType)
	const auto tile = getSharedPtr<Tile>(L, 1);
	if (!tile) {
		lua_pushnil(L);
		return 1;
	}

	bool found;

	ItemTypes_t itemType = getNumber<ItemTypes_t>(L, 2);
	switch (itemType) {
		case ITEM_TYPE_TELEPORT:
			found = tile->hasFlag(TILESTATE_TELEPORT);
			break;
		case ITEM_TYPE_MAGICFIELD:
			found = tile->hasFlag(TILESTATE_MAGICFIELD);
			break;
		case ITEM_TYPE_MAILBOX:
			found = tile->hasFlag(TILESTATE_MAILBOX);
			break;
		case ITEM_TYPE_TRASHHOLDER:
			found = tile->hasFlag(TILESTATE_TRASHHOLDER);
			break;
		case ITEM_TYPE_BED:
			found = tile->hasFlag(TILESTATE_BED);
			break;
		case ITEM_TYPE_DEPOT:
			found = tile->hasFlag(TILESTATE_DEPOT);
			break;
		default:
			found = true;
			break;
	}

	if (!found) {
		lua_pushnil(L);
		return 1;
	}

	if (const auto item = tile->getGround()) {
		const ItemType& it = Item::items[item->getID()];
		if (it.type == itemType) {
			pushSharedPtr(L, item);
			setItemMetatable(L, -1, item);
			return 1;
		}
	}

	if (const TileItemsPtr items = tile->getItemList()) {
		for (const auto item : *items) {
			const ItemType& it = Item::items[item->getID()];
			if (it.type == itemType) {
				pushSharedPtr(L, item);
				setItemMetatable(L, -1, item);
				return 1;
			}
		}
	}

	lua_pushnil(L);
	return 1;
}

int LuaScriptInterface::luaTileGetItemByTopOrder(lua_State* L)
{
	// tile:getItemByTopOrder(topOrder)
	TilePtr tile = getSharedPtr<Tile>(L, 1);
	if (!tile) {
		lua_pushnil(L);
		return 1;
	}

	int32_t topOrder = getNumber<int32_t>(L, 2);

	const auto item = tile->getItemByTopOrder(topOrder);
	if (!item) {
		lua_pushnil(L);
		return 1;
	}

	pushSharedPtr(L, item);
	setItemMetatable(L, -1, item);
	return 1;
}

int LuaScriptInterface::luaTileGetItemCountById(lua_State* L)
{
	// tile:getItemCountById(itemId[, subType = -1])
	const auto tile = getSharedPtr<Tile>(L, 1);
	if (!tile) {
		lua_pushnil(L);
		return 1;
	}

	int32_t subType = getNumber<int32_t>(L, 3, -1);

	uint16_t itemId;
	if (isNumber(L, 2)) {
		itemId = getNumber<uint16_t>(L, 2);
	} else {
		itemId = Item::items.getItemIdByName(getString(L, 2));
		if (itemId == 0) {
			lua_pushnil(L);
			return 1;
		}
	}

	lua_pushinteger(L, tile->getItemTypeCount(itemId, subType));
	return 1;
}

int LuaScriptInterface::luaTileGetBottomCreature(lua_State* L)
{
	// tile:getBottomCreature()
	const auto tile = getSharedPtr<Tile>(L, 1);
	if (!tile) {
		lua_pushnil(L);
		return 1;
	}

	const auto creature = tile->getBottomCreature();
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	pushSharedPtr(L, creature);
	setCreatureMetatable(L, -1, creature);
	return 1;
}

int LuaScriptInterface::luaTileGetTopCreature(lua_State* L)
{
	// tile:getTopCreature()
	const auto tile = getSharedPtr<Tile>(L, 1);
	if (!tile) {
		lua_pushnil(L);
		return 1;
	}

	const auto creature = tile->getTopCreature();
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	pushSharedPtr(L, creature);
	setCreatureMetatable(L, -1, creature);
	return 1;
}

int LuaScriptInterface::luaTileGetBottomVisibleCreature(lua_State* L)
{
	// tile:getBottomVisibleCreature(creature)
	const auto tile = getSharedPtr<Tile>(L, 1);
	if (!tile) {
		lua_pushnil(L);
		return 1;
	}

	const auto creature = getCreature(L, 2);
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	if (const auto visibleCreature = tile->getBottomVisibleCreature(creature)) {
		pushSharedPtr(L, visibleCreature);
		setCreatureMetatable(L, -1, visibleCreature);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaTileGetTopVisibleCreature(lua_State* L)
{
	// tile:getTopVisibleCreature(creature)
	const auto tile = getSharedPtr<Tile>(L, 1);
	if (!tile) {
		lua_pushnil(L);
		return 1;
	}

	const auto creature = getCreature(L, 2);
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	if (const auto visibleCreature = tile->getTopVisibleCreature(creature)) {
		pushSharedPtr(L, visibleCreature);
		setCreatureMetatable(L, -1, visibleCreature);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaTileGetItems(lua_State* L)
{
	// tile:getItems()
	const auto tile = getSharedPtr<Tile>(L, 1);
	if (!tile) {
		lua_pushnil(L);
		return 1;
	}

	auto itemVector = tile->getItemList();
	if (!itemVector) {
		lua_pushnil(L);
		return 1;
	}

	lua_createtable(L, itemVector->size(), 0);

	int index = 0;
	for (const auto item : *itemVector) {
		pushSharedPtr(L, item);
		setItemMetatable(L, -1, item);
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaTileGetItemCount(lua_State* L)
{
	// tile:getItemCount()
	const auto tile = getSharedPtr<Tile>(L, 1);
	if (!tile) {
		lua_pushnil(L);
		return 1;
	}

	lua_pushinteger(L, tile->getItemCount());
	return 1;
}

int LuaScriptInterface::luaTileGetDownItemCount(lua_State* L)
{
	// tile:getDownItemCount()
	if (const auto tile = getSharedPtr<Tile>(L, 1)) {
		lua_pushinteger(L, tile->getDownItemCount());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaTileGetTopItemCount(lua_State* L)
{
	// tile:getTopItemCount()
	const auto tile = getSharedPtr<Tile>(L, 1);
	if (!tile) {
		lua_pushnil(L);
		return 1;
	}

	lua_pushinteger(L, tile->getTopItemCount());
	return 1;
}

int LuaScriptInterface::luaTileGetCreatures(lua_State* L)
{
	// tile:getCreatures()
	const auto tile = getSharedPtr<Tile>(L, 1);
	if (!tile) {
		lua_pushnil(L);
		return 1;
	}

	const auto creatureVector = tile->getCreatures();
	if (!creatureVector) {
		lua_pushnil(L);
		return 1;
	}

	lua_createtable(L, creatureVector->size(), 0);

	int index = 0;
	for (const auto creature : *creatureVector) {
		pushSharedPtr(L, creature);
		setCreatureMetatable(L, -1, creature);
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaTileGetCreatureCount(lua_State* L)
{
	// tile:getCreatureCount()
	const auto tile = getSharedPtr<Tile>(L, 1);
	if (!tile) {
		lua_pushnil(L);
		return 1;
	}

	lua_pushinteger(L, tile->getCreatureCount());
	return 1;
}

int LuaScriptInterface::luaTileHasProperty(lua_State* L)
{
	// tile:hasProperty(property[, item])
	const auto tile = getSharedPtr<Tile>(L, 1);
	if (!tile) {
		lua_pushnil(L);
		return 1;
	}

	ItemPtr item;
	if (lua_gettop(L) >= 3) {
		item = getSharedPtr<Item>(L, 3);
	} else {
		item = nullptr;
	}

	ITEMPROPERTY property = getNumber<ITEMPROPERTY>(L, 2);
	if (item) {
		pushBoolean(L, tile->hasProperty(item, property));
	} else {
		pushBoolean(L, tile->hasProperty(property));
	}
	return 1;
}

int LuaScriptInterface::luaTileGetThingIndex(lua_State* L)
{
	// tile:getThingIndex(thing)
	const auto tile = getSharedPtr<Tile>(L, 1);
	if (!tile) {
		lua_pushnil(L);
		return 1;
	}

	const auto thing = getThing(L, 2);
	if (thing) {
		lua_pushinteger(L, tile->getThingIndex(thing));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaTileHasFlag(lua_State* L)
{
	// tile:hasFlag(flag)
	if (const auto tile = getSharedPtr<Tile>(L, 1)) {
		tileflags_t flag = getNumber<tileflags_t>(L, 2);
		pushBoolean(L, tile->hasFlag(flag));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaTileAddItem(lua_State* L)
{
	// tile:addItem(itemId[, count/subType = 1[, flags = 0]])
	const auto tile = getSharedPtr<Tile>(L, 1);
	if (!tile) {
		lua_pushnil(L);
		return 1;
	}

	uint16_t itemId;
	if (isNumber(L, 2)) {
		itemId = getNumber<uint16_t>(L, 2);
	} else {
		itemId = Item::items.getItemIdByName(getString(L, 2));
		if (itemId == 0) {
			lua_pushnil(L);
			return 1;
		}
	}

	uint32_t subType = getNumber<uint32_t>(L, 3, 1);

	auto item = Item::CreateItem(itemId, std::min<uint32_t>(subType, 100));
	if (!item) {
		lua_pushnil(L);
		return 1;
	}

	uint32_t flags = getNumber<uint32_t>(L, 4, 0);
	CylinderPtr holder = tile;
	ReturnValue ret = g_game.internalAddItem(holder, item, INDEX_WHEREEVER, flags);
	if (ret == RETURNVALUE_NOERROR) {
		pushSharedPtr(L, item);
		setItemMetatable(L, -1, item);
	} else {
		item.reset();
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaTileAddItemEx(lua_State* L)
{
	// tile:addItemEx(item[, flags = 0])
	const auto item = getSharedPtr<Item>(L, 2);
	if (!item) {
		lua_pushnil(L);
		return 1;
	}

	const auto tile = getSharedPtr<Tile>(L, 1);
	if (!tile) {
		lua_pushnil(L);
		return 1;
	}

	if (item->getParent() != VirtualCylinder::virtualCylinder) {
		reportErrorFunc(L, "Item already has a parent");
		lua_pushnil(L);
		return 1;
	}

	uint32_t flags = getNumber<uint32_t>(L, 3, 0);
	CylinderPtr holder = tile;
	ReturnValue ret = g_game.internalAddItem(holder, item, INDEX_WHEREEVER, flags);
	if (ret == RETURNVALUE_NOERROR) {
		ScriptEnvironment::removeTempItem(item);
	}
	lua_pushinteger(L, ret);
	return 1;
}

int LuaScriptInterface::luaTileGetHouse(lua_State* L)
{
	// tile:getHouse()
	const auto tile = getSharedPtr<Tile>(L, 1);
	if (!tile) {
		lua_pushnil(L);
		return 1;
	}

	if (tile->isHouseTile()) {
		pushUserdata<House>(L, tile->getHouse());
		setMetatable(L, -1, "House");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// NetworkMessage
int LuaScriptInterface::luaNetworkMessageCreate(lua_State* L)
{
	// NetworkMessage()
	pushUserdata<NetworkMessage>(L, new NetworkMessage);
	setMetatable(L, -1, "NetworkMessage");
	return 1;
}

int LuaScriptInterface::luaNetworkMessageDelete(lua_State* L)
{
	NetworkMessage** messagePtr = getRawUserdata<NetworkMessage>(L, 1);
	if (messagePtr && *messagePtr) {
		delete *messagePtr;
		*messagePtr = nullptr;
	}
	return 0;
}

int LuaScriptInterface::luaNetworkMessageGetByte(lua_State* L)
{
	// networkMessage:getByte()
	NetworkMessage* message = getUserdata<NetworkMessage>(L, 1);
	if (message) {
		lua_pushinteger(L, message->getByte());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaNetworkMessageGetU16(lua_State* L)
{
	// networkMessage:getU16()
	NetworkMessage* message = getUserdata<NetworkMessage>(L, 1);
	if (message) {
		lua_pushinteger(L, message->get<uint16_t>());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaNetworkMessageGetU32(lua_State* L)
{
	// networkMessage:getU32()
	NetworkMessage* message = getUserdata<NetworkMessage>(L, 1);
	if (message) {
		lua_pushinteger(L, message->get<uint32_t>());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaNetworkMessageGetU64(lua_State* L)
{
	// networkMessage:getU64()
	NetworkMessage* message = getUserdata<NetworkMessage>(L, 1);
	if (message) {
		lua_pushinteger(L, message->get<uint64_t>());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaNetworkMessageGetString(lua_State* L)
{
	// networkMessage:getString()
	NetworkMessage* message = getUserdata<NetworkMessage>(L, 1);
	if (message) {
		pushString(L, message->getString());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaNetworkMessageGetPosition(lua_State* L)
{
	// networkMessage:getPosition()
	NetworkMessage* message = getUserdata<NetworkMessage>(L, 1);
	if (message) {
		pushPosition(L, message->getPosition());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaNetworkMessageAddByte(lua_State* L)
{
	// networkMessage:addByte(number)
	uint8_t number = getNumber<uint8_t>(L, 2);
	NetworkMessage* message = getUserdata<NetworkMessage>(L, 1);
	if (message) {
		message->addByte(number);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaNetworkMessageAddU16(lua_State* L)
{
	// networkMessage:addU16(number)
	uint16_t number = getNumber<uint16_t>(L, 2);
	NetworkMessage* message = getUserdata<NetworkMessage>(L, 1);
	if (message) {
		message->add<uint16_t>(number);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaNetworkMessageAddU32(lua_State* L)
{
	// networkMessage:addU32(number)
	uint32_t number = getNumber<uint32_t>(L, 2);
	NetworkMessage* message = getUserdata<NetworkMessage>(L, 1);
	if (message) {
		message->add<uint32_t>(number);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaNetworkMessageAddU64(lua_State* L)
{
	// networkMessage:addU64(number)
	uint64_t number = getNumber<uint64_t>(L, 2);
	NetworkMessage* message = getUserdata<NetworkMessage>(L, 1);
	if (message) {
		message->add<uint64_t>(number);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaNetworkMessageAddString(lua_State* L)
{
	// networkMessage:addString(string)
	const std::string& string = getString(L, 2);
	NetworkMessage* message = getUserdata<NetworkMessage>(L, 1);
	if (message) {
		message->addString(string);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaNetworkMessageAddPosition(lua_State* L)
{
	// networkMessage:addPosition(position)
	const Position& position = getPosition(L, 2);
	NetworkMessage* message = getUserdata<NetworkMessage>(L, 1);
	if (message) {
		message->addPosition(position);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaNetworkMessageAddDouble(lua_State* L)
{
	// networkMessage:addDouble(number)
	double number = getNumber<double>(L, 2);
	NetworkMessage* message = getUserdata<NetworkMessage>(L, 1);
	if (message) {
		message->addDouble(number);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaNetworkMessageAddItem(lua_State* L)
{
	// networkMessage:addItem(item)
	const auto item = getSharedPtr<Item>(L, 2);
	if (!item) {
		reportErrorFunc(L, getErrorDesc(LUA_ERROR_ITEM_NOT_FOUND));
		lua_pushnil(L);
		return 1;
	}

	NetworkMessage* message = getUserdata<NetworkMessage>(L, 1);
	if (message) {
		message->addItem(item);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaNetworkMessageAddItemId(lua_State* L)
{
	// networkMessage:addItemId(itemId)
	NetworkMessage* message = getUserdata<NetworkMessage>(L, 1);
	if (!message) {
		lua_pushnil(L);
		return 1;
	}

	uint16_t itemId;
	if (isNumber(L, 2)) {
		itemId = getNumber<uint16_t>(L, 2);
	} else {
		itemId = Item::items.getItemIdByName(getString(L, 2));
		if (itemId == 0) {
			lua_pushnil(L);
			return 1;
		}
	}

	message->addItemId(itemId);
	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaNetworkMessageReset(lua_State* L)
{
	// networkMessage:reset()
	NetworkMessage* message = getUserdata<NetworkMessage>(L, 1);
	if (message) {
		message->reset();
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaNetworkMessageSeek(lua_State* L)
{
	// networkMessage:seek(position)
	NetworkMessage* message = getUserdata<NetworkMessage>(L, 1);
	if (message && isNumber(L, 2)) {
		pushBoolean(L, message->setBufferPosition(getNumber<uint16_t>(L, 2)));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaNetworkMessageTell(lua_State* L)
{
	// networkMessage:tell()
	NetworkMessage* message = getUserdata<NetworkMessage>(L, 1);
	if (message) {
		lua_pushinteger(L, message->getBufferPosition() - message->INITIAL_BUFFER_POSITION);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaNetworkMessageLength(lua_State* L)
{
	// networkMessage:len()
	NetworkMessage* message = getUserdata<NetworkMessage>(L, 1);
	if (message) {
		lua_pushinteger(L, message->getLength());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaNetworkMessageSkipBytes(lua_State* L)
{
	// networkMessage:skipBytes(number)
	int16_t number = getNumber<int16_t>(L, 2);
	NetworkMessage* message = getUserdata<NetworkMessage>(L, 1);
	if (message) {
		message->skipBytes(number);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaNetworkMessageSendToPlayer(lua_State* L)
{
	// networkMessage:sendToPlayer(player)
	NetworkMessage* message = getUserdata<NetworkMessage>(L, 1);
	if (!message) {
		lua_pushnil(L);
		return 1;
	}

	if (const auto player = getPlayer(L, 2)) {
		player->sendNetworkMessage(*message);
		pushBoolean(L, true);
	} else {
		reportErrorFunc(L, getErrorDesc(LUA_ERROR_PLAYER_NOT_FOUND));
		lua_pushnil(L);
	}
	return 1;
}

// ModalWindow
int LuaScriptInterface::luaModalWindowCreate(lua_State* L)
{
	// ModalWindow(id, title, message)
	const std::string& message = getString(L, 4);
	const std::string& title = getString(L, 3);
	uint32_t id = getNumber<uint32_t>(L, 2);

	pushUserdata<ModalWindow>(L, new ModalWindow(id, title, message));
	setMetatable(L, -1, "ModalWindow");
	return 1;
}

int LuaScriptInterface::luaModalWindowDelete(lua_State* L)
{
	ModalWindow** windowPtr = getRawUserdata<ModalWindow>(L, 1);
	if (windowPtr && *windowPtr) {
		delete *windowPtr;
		*windowPtr = nullptr;
	}
	return 0;
}

int LuaScriptInterface::luaModalWindowGetId(lua_State* L)
{
	// modalWindow:getId()
	ModalWindow* window = getUserdata<ModalWindow>(L, 1);
	if (window) {
		lua_pushinteger(L, window->id);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaModalWindowGetTitle(lua_State* L)
{
	// modalWindow:getTitle()
	ModalWindow* window = getUserdata<ModalWindow>(L, 1);
	if (window) {
		pushString(L, window->title);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaModalWindowGetMessage(lua_State* L)
{
	// modalWindow:getMessage()
	ModalWindow* window = getUserdata<ModalWindow>(L, 1);
	if (window) {
		pushString(L, window->message);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaModalWindowSetTitle(lua_State* L)
{
	// modalWindow:setTitle(text)
	const std::string& text = getString(L, 2);
	ModalWindow* window = getUserdata<ModalWindow>(L, 1);
	if (window) {
		window->title = text;
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaModalWindowSetMessage(lua_State* L)
{
	// modalWindow:setMessage(text)
	const std::string& text = getString(L, 2);
	ModalWindow* window = getUserdata<ModalWindow>(L, 1);
	if (window) {
		window->message = text;
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaModalWindowGetButtonCount(lua_State* L)
{
	// modalWindow:getButtonCount()
	ModalWindow* window = getUserdata<ModalWindow>(L, 1);
	if (window) {
		lua_pushinteger(L, window->buttons.size());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaModalWindowGetChoiceCount(lua_State* L)
{
	// modalWindow:getChoiceCount()
	ModalWindow* window = getUserdata<ModalWindow>(L, 1);
	if (window) {
		lua_pushinteger(L, window->choices.size());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaModalWindowAddButton(lua_State* L)
{
	// modalWindow:addButton(id, text)
	const std::string& text = getString(L, 3);
	uint8_t id = getNumber<uint8_t>(L, 2);
	ModalWindow* window = getUserdata<ModalWindow>(L, 1);
	if (window) {
		window->buttons.emplace_back(text, id);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaModalWindowAddChoice(lua_State* L)
{
	// modalWindow:addChoice(id, text)
	const std::string& text = getString(L, 3);
	uint8_t id = getNumber<uint8_t>(L, 2);
	ModalWindow* window = getUserdata<ModalWindow>(L, 1);
	if (window) {
		window->choices.emplace_back(text, id);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaModalWindowGetDefaultEnterButton(lua_State* L)
{
	// modalWindow:getDefaultEnterButton()
	ModalWindow* window = getUserdata<ModalWindow>(L, 1);
	if (window) {
		lua_pushinteger(L, window->defaultEnterButton);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaModalWindowSetDefaultEnterButton(lua_State* L)
{
	// modalWindow:setDefaultEnterButton(buttonId)
	ModalWindow* window = getUserdata<ModalWindow>(L, 1);
	if (window) {
		window->defaultEnterButton = getNumber<uint8_t>(L, 2);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaModalWindowGetDefaultEscapeButton(lua_State* L)
{
	// modalWindow:getDefaultEscapeButton()
	ModalWindow* window = getUserdata<ModalWindow>(L, 1);
	if (window) {
		lua_pushinteger(L, window->defaultEscapeButton);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaModalWindowSetDefaultEscapeButton(lua_State* L)
{
	// modalWindow:setDefaultEscapeButton(buttonId)
	ModalWindow* window = getUserdata<ModalWindow>(L, 1);
	if (window) {
		window->defaultEscapeButton = getNumber<uint8_t>(L, 2);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaModalWindowHasPriority(lua_State* L)
{
	// modalWindow:hasPriority()
	ModalWindow* window = getUserdata<ModalWindow>(L, 1);
	if (window) {
		pushBoolean(L, window->priority);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaModalWindowSetPriority(lua_State* L)
{
	// modalWindow:setPriority(priority)
	ModalWindow* window = getUserdata<ModalWindow>(L, 1);
	if (window) {
		window->priority = getBoolean(L, 2);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaModalWindowSendToPlayer(lua_State* L)
{
	// modalWindow:sendToPlayer(player)
	const auto player = getPlayer(L, 2);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}

	ModalWindow* window = getUserdata<ModalWindow>(L, 1);
	if (window) {
		if (!player->hasModalWindowOpen(window->id)) {
			player->sendModalWindow(*window);
		}
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// Item
int LuaScriptInterface::luaItemCreate(lua_State* L)
{
	// Item(uid)
	uint32_t id = getNumber<uint32_t>(L, 2);

	if (const auto item = getScriptEnv()->getItemByUID(id)) {
		pushSharedPtr(L, item);
		setItemMetatable(L, -1, item);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemDelete(lua_State* L)
{
	if (auto& item = getSharedPtr<Item>(L, 1)) {
		item.reset();
	}
	return 0;
}

int LuaScriptInterface::luaItemIsItem(lua_State* L)
{
	// item:isItem()
	pushBoolean(L, getSharedPtr<const Item>(L, 1) != nullptr);
	return 1;
}

int LuaScriptInterface::luaItemGetParent(lua_State* L)
{
	// item:getParent()
	const auto item = getSharedPtr<Item>(L, 1);
	if (!item) {
		lua_pushnil(L);
		return 1;
	}

	CylinderPtr parent = item->getParent();
	if (!parent) {
		lua_pushnil(L);
		return 1;
	}

	pushCylinder(L, parent);
	return 1;
}

int LuaScriptInterface::luaItemGetTopParent(lua_State* L)
{
	// item:getTopParent()
	const auto item = getSharedPtr<Item>(L, 1);
	if (!item) {
		lua_pushnil(L);
		return 1;
	}

	CylinderPtr topParent = item->getTopParent();
	if (!topParent) {
		lua_pushnil(L);
		return 1;
	}

	pushCylinder(L, topParent);
	return 1;
}

int LuaScriptInterface::luaItemGetId(lua_State* L)
{
	// item:getId()
	if (const auto item = getSharedPtr<Item>(L, 1)) {
		lua_pushinteger(L, item->getID());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemClone(lua_State* L)
{
	// item:clone()
	const auto item = getSharedPtr<Item>(L, 1);
	if (!item) {
		lua_pushnil(L);
		return 1;
	}

	const auto clone = item->clone();
	if (!clone) {
		lua_pushnil(L);
		return 1;
	}

	getScriptEnv()->addTempItem(clone);
	clone->setParent(VirtualCylinder::virtualCylinder);

	pushSharedPtr(L, clone);
	setItemMetatable(L, -1, clone);
	return 1;
}

int LuaScriptInterface::luaItemSplit(lua_State* L)
{
	// item:split([count = 1])
	auto& itemPtr = getSharedPtr<Item>(L, 1);
	if (!itemPtr) {
		lua_pushnil(L);
		return 1;
	}

	ItemPtr item = itemPtr;
	if (!item || !item->isStackable()) {
		lua_pushnil(L);
		return 1;
	}

	uint16_t count = std::min<uint16_t>(getNumber<uint16_t>(L, 2, 1), item->getItemCount());
	uint16_t diff = item->getItemCount() - count;

	ItemPtr splitItem = item->clone();
	if (!splitItem) {
		lua_pushnil(L);
		return 1;
	}

	splitItem->setItemCount(count);

	ScriptEnvironment* env = getScriptEnv();
	uint32_t uid = env->addThing(item);

	ItemPtr newItem = g_game.transformItem(item, item->getID(), diff);
	if (item->isRemoved()) {
		env->removeItemByUID(uid);
	}

	if (newItem && newItem != item) {
		env->insertItem(uid, newItem);
	}

	itemPtr.reset(newItem.get());
	
	splitItem->setParent(VirtualCylinder::virtualCylinder);
	env->addTempItem(splitItem);

	pushSharedPtr(L, splitItem);
	setItemMetatable(L, -1, splitItem);
	return 1;
}

int LuaScriptInterface::luaItemRemove(lua_State* L)
{
	// item:remove([count = -1])
	const auto item = getSharedPtr<Item>(L, 1);
	if (item) {
		const int32_t count = getNumber<int32_t>(L, 2, -1);
		pushBoolean(L, g_game.internalRemoveItem(item, count) == RETURNVALUE_NOERROR);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemGetUniqueId(lua_State* L)
{
	// item:getUniqueId()
	if (const auto item = getSharedPtr<Item>(L, 1)) {
		uint32_t uniqueId = item->getUniqueId();
		if (uniqueId == 0) {
			uniqueId = getScriptEnv()->addThing(item);
		}
		lua_pushinteger(L, uniqueId);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemGetActionId(lua_State* L)
{
	// item:getActionId()
	if (const auto item = getSharedPtr<Item>(L, 1)) {
		lua_pushinteger(L, item->getActionId());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemSetActionId(lua_State* L)
{
	// item:setActionId(actionId)
	uint16_t actionId = getNumber<uint16_t>(L, 2);
	if (const auto item = getSharedPtr<Item>(L, 1)) {
		item->setActionId(actionId);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemGetCount(lua_State* L)
{
	// item:getCount()
	if (const auto item = getSharedPtr<Item>(L, 1)) {
		lua_pushinteger(L, item->getItemCount());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemGetCharges(lua_State* L)
{
	// item:getCharges()
	if (const auto item = getSharedPtr<Item>(L, 1)) {
		lua_pushinteger(L, item->getCharges());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemGetFluidType(lua_State* L)
{
	// item:getFluidType()
	if (const auto item = getSharedPtr<Item>(L, 1)) {
		lua_pushinteger(L, item->getFluidType());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemGetWeight(lua_State* L)
{
	// item:getWeight()
	if (const auto item = getSharedPtr<Item>(L, 1)) {
		lua_pushinteger(L, item->getWeight());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemGetWorth(lua_State* L)
{
	// item:getWorth()
	if (const auto item = getSharedPtr<Item>(L, 1)) {
		lua_pushinteger(L, item->getWorth());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemGetSubType(lua_State* L)
{
	// item:getSubType()
	if (const auto item = getSharedPtr<Item>(L, 1)) {
		lua_pushinteger(L, item->getSubType());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemGetName(lua_State* L)
{
	// item:getName()
	if (const auto item = getSharedPtr<Item>(L, 1)) {
		pushString(L, item->getName());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemGetPluralName(lua_State* L)
{
	// item:getPluralName()
	if (const auto item = getSharedPtr<Item>(L, 1)) {
		pushString(L, item->getPluralName());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemGetArticle(lua_State* L)
{
	// item:getArticle()
	if (const auto item = getSharedPtr<Item>(L, 1)) {
		pushString(L, item->getArticle());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemGetPosition(lua_State* L)
{
	// item:getPosition()
	if (const auto item = getSharedPtr<Item>(L, 1)) {
		pushPosition(L, item->getPosition());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemGetTile(lua_State* L)
{
	// item:getTile()
	const auto item = getSharedPtr<Item>(L, 1);
	if (!item) {
		lua_pushnil(L);
		return 1;
	}

	if (const auto tile = item->getTile()) {
		pushSharedPtr(L, tile);
		setMetatable(L, -1, "Tile");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemHasAttribute(lua_State* L)
{
	// item:hasAttribute(key)
	const auto item = getUserdata<Item>(L, 1);
	if (!item) {
		lua_pushnil(L);
		return 1;
	}

	itemAttrTypes attribute;
	if (isNumber(L, 2)) {
		attribute = getNumber<itemAttrTypes>(L, 2);
	} else if (isString(L, 2)) {
		attribute = stringToItemAttribute(getString(L, 2));
	} else {
		attribute = ITEM_ATTRIBUTE_NONE;
	}

	pushBoolean(L, item->hasAttribute(attribute));
	return 1;
}

int LuaScriptInterface::luaItemGetAttribute(lua_State* L)
{
	// item:getAttribute(key)
	const auto item = getSharedPtr<Item>(L, 1);
	if (!item) {
		lua_pushnil(L);
		return 1;
	}

	itemAttrTypes attribute;
	if (isNumber(L, 2)) {
		attribute = getNumber<itemAttrTypes>(L, 2);
	} else if (isString(L, 2)) {
		attribute = stringToItemAttribute(getString(L, 2));
	} else {
		attribute = ITEM_ATTRIBUTE_NONE;
	}

	if (ItemAttributes::isIntAttrType(attribute)) {
		lua_pushinteger(L, item->getIntAttr(attribute));
	} else if (ItemAttributes::isStrAttrType(attribute)) {
		pushString(L, item->getStrAttr(attribute));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemSetAttribute(lua_State* L)
{
	// item:setAttribute(key, value)
	const auto item = getSharedPtr<Item>(L, 1);
	if (!item) {
		lua_pushnil(L);
		return 1;
	}

	itemAttrTypes attribute;
	if (isNumber(L, 2)) {
		attribute = getNumber<itemAttrTypes>(L, 2);
	} else if (isString(L, 2)) {
		attribute = stringToItemAttribute(getString(L, 2));
	} else {
		attribute = ITEM_ATTRIBUTE_NONE;
	}

	if (ItemAttributes::isIntAttrType(attribute)) {
		if (attribute == ITEM_ATTRIBUTE_UNIQUEID) {
			reportErrorFunc(L, "Attempt to set protected key \"uid\"");
			pushBoolean(L, false);
			return 1;
		}

		item->setIntAttr(attribute, getNumber<int32_t>(L, 3));
		pushBoolean(L, true);
	} else if (ItemAttributes::isStrAttrType(attribute)) {
		item->setStrAttr(attribute, getString(L, 3));
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemRemoveAttribute(lua_State* L)
{
	// item:removeAttribute(key)
	const auto item = getSharedPtr<Item>(L, 1);
	if (!item) {
		lua_pushnil(L);
		return 1;
	}

	itemAttrTypes attribute;
	if (isNumber(L, 2)) {
		attribute = getNumber<itemAttrTypes>(L, 2);
	} else if (isString(L, 2)) {
		attribute = stringToItemAttribute(getString(L, 2));
	} else {
		attribute = ITEM_ATTRIBUTE_NONE;
	}

	bool ret = attribute != ITEM_ATTRIBUTE_UNIQUEID;
	if (ret) {
		item->removeAttribute(attribute);
	} else {
		reportErrorFunc(L, "Attempt to erase protected key \"uid\"");
	}
	pushBoolean(L, ret);
	return 1;
}

int LuaScriptInterface::luaItemGetCustomAttribute(lua_State* L) {
	// item:getCustomAttribute(key)
	const auto item = getSharedPtr<Item>(L, 1);
	if (!item) {
		lua_pushnil(L);
		return 1;
	}

	const ItemAttributes::CustomAttribute* attr;
	if (isNumber(L, 2)) {
		attr = item->getCustomAttribute(getNumber<int64_t>(L, 2));
	} else if (isString(L, 2)) {
		attr = item->getCustomAttribute(getString(L, 2));
	} else {
		lua_pushnil(L);
		return 1;
	}

	if (attr) {
		attr->pushToLua(L);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemSetCustomAttribute(lua_State* L) {
	// item:setCustomAttribute(key, value)
	const auto item = getSharedPtr<Item>(L, 1);
	if (!item) {
		lua_pushnil(L);
		return 1;
	}

	std::string key;
	if (isNumber(L, 2)) {
		key = std::to_string(getNumber<int64_t>(L, 2));
	} else if (isString(L, 2)) {
		key = getString(L, 2);
	} else {
		lua_pushnil(L);
		return 1;
	}

	ItemAttributes::CustomAttribute val;
	if (isNumber(L, 3)) {
		double tmp = getNumber<double>(L, 3);
		if (std::floor(tmp) < tmp) {
			val.set<double>(tmp);
		} else {
			val.set<int64_t>(tmp);
		}
	} else if (isString(L, 3)) {
		val.set<std::string>(getString(L, 3));
	} else if (isBoolean(L, 3)) {
		val.set<bool>(getBoolean(L, 3));
	} else {
		lua_pushnil(L);
		return 1;
	}

	item->setCustomAttribute(key, val);
	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaItemRemoveCustomAttribute(lua_State* L) {
	// item:removeCustomAttribute(key)
	const auto item = getSharedPtr<Item>(L, 1);
	if (!item) {
		lua_pushnil(L);
		return 1;
	}

	if (isNumber(L, 2)) {
		pushBoolean(L, item->removeCustomAttribute(getNumber<int64_t>(L, 2)));
	} else if (isString(L, 2)) {
		pushBoolean(L, item->removeCustomAttribute(getString(L, 2)));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemMoveTo(lua_State* L)
{
	// item:moveTo(position or cylinder[, flags])
	auto& itemPtr = getSharedPtr<Item>(L, 1);
	if (!itemPtr) {
		lua_pushnil(L);
		return 1;
	}

	ItemPtr item = itemPtr;
	if (!item || item->isRemoved()) {
		lua_pushnil(L);
		return 1;
	}

	CylinderPtr toCylinder;
	if (isUserdata(L, 2)) {
		switch (const LuaDataType type = getUserdataType(L, 2)) {
			case LuaData_Container:
				toCylinder = getSharedPtr<Container>(L, 2);
				break;
			case LuaData_Player:
				toCylinder = getSharedPtr<Player>(L, 2);
				break;
			case LuaData_Tile:
				toCylinder = getSharedPtr<Tile>(L, 2);
				break;
			default:
				toCylinder = nullptr;
				break;
		}
	} else {
		toCylinder = g_game.map.getTile(getPosition(L, 2));
	}

	if (!toCylinder) {
		lua_pushnil(L);
		return 1;
	}

	if (item->getParent() == toCylinder) {
		pushBoolean(L, true);
		return 1;
	}

	uint32_t flags = getNumber<uint32_t>(L, 3, FLAG_NOLIMIT | FLAG_IGNOREBLOCKITEM | FLAG_IGNOREBLOCKCREATURE | FLAG_IGNORENOTMOVEABLE);

	if (item->getParent() == VirtualCylinder::virtualCylinder) {
		pushBoolean(L, g_game.internalAddItem(toCylinder, item, INDEX_WHEREEVER, flags) == RETURNVALUE_NOERROR);
	} else {
		ItemPtr moveItem = nullptr;
		CylinderPtr i_parent = item->getParent();
		ReturnValue ret = g_game.internalMoveItem(i_parent, toCylinder, INDEX_WHEREEVER, item, item->getItemCount(), std::ref(moveItem), flags);
		if (moveItem) {
			itemPtr = moveItem;
		}
		pushBoolean(L, ret == RETURNVALUE_NOERROR);
	}
	return 1;
}

int LuaScriptInterface::luaItemTransform(lua_State* L)
{
	// item:transform(itemId[, count/subType = -1])
	auto& itemPtr = getSharedPtr<Item>(L, 1);
	if (!itemPtr) {
		lua_pushnil(L);
		return 1;
	}

	ItemPtr item = itemPtr;
	if (!item) {
		lua_pushnil(L);
		return 1;
	}

	if (item->isAugmented() || item->hasImbuements()) {
		lua_pushboolean(L, false);
		return 1;
	}

	uint16_t itemId;
	if (isNumber(L, 2)) {
		itemId = getNumber<uint16_t>(L, 2);
	} else {
		itemId = Item::items.getItemIdByName(getString(L, 2));
		if (itemId == 0) {
			lua_pushnil(L);
			return 1;
		}
	}

	int32_t subType = getNumber<int32_t>(L, 3, -1);
	if (item->getID() == itemId && (subType == -1 || subType == item->getSubType())) {
		pushBoolean(L, true);
		return 1;
	}

	const ItemType& it = Item::items[itemId];
	if (it.stackable) {
		subType = std::min<int32_t>(subType, 100);
	}

	ScriptEnvironment* env = getScriptEnv();
	uint32_t uid = env->addThing(item);

	const auto newItem = g_game.transformItem(item, itemId, subType);
	if (item->isRemoved()) {
		env->removeItemByUID(uid);
	}

	if (newItem && newItem != item) {
		env->insertItem(uid, newItem);
	}

	item = newItem;
	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaItemDecay(lua_State* L)
{
	// item:decay(decayId)
	if (const auto item = getSharedPtr<Item>(L, 1)) {

		if (item->isAugmented() || item->hasImbuements()) {
			lua_pushboolean(L, false);
			return 1;
		}

		if (isNumber(L, 2)) {
			item->setDecayTo(getNumber<int32_t>(L, 2));
		}

		g_game.startDecay(item);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemGetDescription(lua_State* L)
{
	// item:getDescription(distance)
	if (const auto item = getSharedPtr<Item>(L, 1)) {
		int32_t distance = getNumber<int32_t>(L, 2);
		pushString(L, item->getDescription(distance));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemGetSpecialDescription(lua_State* L)
{
	// item:getSpecialDescription()
	if (const auto item = getSharedPtr<Item>(L, 1)) {
		pushString(L, item->getSpecialDescription());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemHasProperty(lua_State* L)
{
	// item:hasProperty(property)
	if (const auto item = getSharedPtr<Item>(L, 1)) {
		auto property = getNumber<ITEMPROPERTY>(L, 2);
		pushBoolean(L, item->hasProperty(property));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemIsLoadedFromMap(lua_State* L)
{
	// item:isLoadedFromMap()
	if (const auto item = getSharedPtr<Item>(L, 1)) {
		pushBoolean(L, item->isLoadedFromMap());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemSetStoreItem(lua_State* L)
{
	// item:setStoreItem(storeItem)
	const auto item = getSharedPtr<Item>(L, 1);
	if (!item) {
		lua_pushnil(L);
		return 1;
	}

	item->setStoreItem(getBoolean(L, 2, false));
	return 1;
}

int LuaScriptInterface::luaItemIsStoreItem(lua_State* L)
{
	// item:isStoreItem()
	if (const auto item = getSharedPtr<Item>(L, 1)) {
		pushBoolean(L, item->isStoreItem());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemGetImbuementSlots(lua_State* L)
{
	// item:getImbuementSlots() -- returns how many total slots
	if (const auto item = getSharedPtr<Item>(L, 1))
	{
		lua_pushinteger(L, item->getImbuementSlots());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemGetFreeImbuementSlots(lua_State* L)
{
	// item:getFreeImbuementSlots() -- returns how many slots are available for use
	if (const auto item = getSharedPtr<Item>(L, 1))
	{
		lua_pushinteger(L, item->getFreeImbuementSlots());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemCanImbue(lua_State* L)
{
	// item:canImbue(amount) -- returns true if item has slots that are free
	if (const auto item = getSharedPtr<Item>(L, 1))
	{
		pushBoolean(L, item->canImbue());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemAddImbuementSlots(lua_State* L)
{
	// item:addImbuementSlots(amount) -- tries to add imbuement slot(s), returns true if successful
	if (const auto item = getSharedPtr<Item>(L, 1))
	{
		pushBoolean(L, item->addImbuementSlots(getNumber<uint32_t>(L, 2)));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemRemoveImbuementSlots(lua_State* L)
{
	// item:removeImbuementSlots(amount, destroy) -- tries to remove imbuement slot(s), returns true if successful
	if (const auto item = getSharedPtr<Item>(L, 1))
	{
		pushBoolean(L, item->removeImbuementSlots(getNumber<uint32_t>(L, 2), getBoolean(L, 5, false)));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemHasImbuementType(lua_State* L)
{
	// item:hasImbuementType(type)
	if (const auto item = getSharedPtr<Item>(L, 1))
	{
		pushBoolean(L, item->hasImbuementType(getNumber<ImbuementType>(L, 2, ImbuementType::IMBUEMENT_TYPE_NONE)));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemHasImbuement(lua_State* L)
{
	// item:hasImbuement(imbueType, amount, duration, realtime)
	if (const auto item = getSharedPtr<Item>(L, 1))
	{
		if (const std::shared_ptr<Imbuement> imbue = getSharedPtr<Imbuement>(L, 2)) {
			pushBoolean(L, item->hasImbuement(imbue));
		} else {
			lua_pushnil(L);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemHasImbuements(lua_State* L)
{
	// item:hasImbuements() -- returns true if item has any imbuements
	if (const auto item = getSharedPtr<Item>(L, 1))
	{
		pushBoolean(L, item->hasImbuements());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemAddImbuement(lua_State* L)
{
	// item:addImbuement(imbuement) -- returns true if it successfully adds the imbuement
	if (const auto item = getSharedPtr<Item>(L, 1))
	{
		if (const std::shared_ptr<Imbuement> imbue = getSharedPtr<Imbuement>(L, 2)) {
			pushBoolean(L, item->addImbuement(imbue, true));
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemRemoveImbuement(lua_State* L)
{
	// item:removeImbuement(imbuement)
	if (const auto item = getSharedPtr<Item>(L, 1))
	{
		if (const std::shared_ptr<Imbuement> imbue = getSharedPtr<Imbuement>(L, 2)) {
			pushBoolean(L, item->removeImbuement(imbue, false));
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}


int LuaScriptInterface::luaItemGetImbuements(lua_State* L)
{
	// item:getImbuements() -- returns a table that contains values that are imbuement userdata?
	const auto item = getSharedPtr<Item>(L, 1);
	if (!item) {
		lua_pushnil(L);
		return 1;
	}

	const std::vector<std::shared_ptr<Imbuement>> imbuements = item->getImbuements();
	lua_createtable(L, imbuements.size(), 0);

	int index = 0;
	for (const auto imbuement : imbuements) {
		pushSharedPtr(L, imbuement);
		setMetatable(L, -1, "Imbuement");
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaItemAddAugment(lua_State* L)
{
	const auto item = getSharedPtr<Item>(L, 1);
	if (!item) {
		lua_pushnil(L);
		return 1;
	}

	if (item->isStackable() || item->canDecay() || !item->canTransform() || item->getCharges() || !item->hasProperty(CONST_PROP_MOVEABLE)) {
		lua_pushboolean(L, false);
		return 1;
	}

	if (isString(L, 2)) {
		std::cout << getString(L, 2) << " \n";
		if (auto augment = Augments::GetAugment(getString(L, 2))) {
			lua_pushboolean(L, item->addAugment(augment));
		} else {
			lua_pushnil(L);
			reportError(__FUNCTION__, "Item::addAugment() argument not found as any name in augments loaded on startup! \n");
		}
	} else if (isUserdata(L, 2)) {
		if (auto augment = getSharedPtr<Augment>(L, 2)) {
			lua_pushboolean(L, item->addAugment(augment));
		} else {
			lua_pushnil(L);
			reportError(__FUNCTION__, "Item::addAugment() invalid userdata passed as argument! \n");
		}
	} else {
		reportError(__FUNCTION__, "Item::addAugment() passed invalid type, must be string or userdata! \n");
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemRemoveAugment(lua_State* L)
{
	const auto item = getSharedPtr<Item>(L, 1);
	if (!item) {
		lua_pushnil(L);
		return 1;
	}

	if (isString(L, 2)) {
		const auto name = getString(L, 2);
		lua_pushboolean(L, item->removeAugment(name));
	} else if (isUserdata(L, 2)) {
		if (auto augment = getSharedPtr<Augment>(L, 2)) {
			lua_pushboolean(L, item->removeAugment(augment));
		} else {
			reportError(__FUNCTION__, "Item::removeAugment() invalid userdata type passed as argument! \n");
			lua_pushnil(L);
		}
	} else {
		reportError(__FUNCTION__, "Item::removeAugment() passed invalid type, must be string or userdata! \n");
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemIsAugmented(lua_State* L)
{
	const auto item = getSharedPtr<Item>(L, 1);
	if (!item) {
		lua_pushnil(L);
		return 1;
	}
	lua_pushboolean(L, item->isAugmented());
	return 1;
}

int LuaScriptInterface::luaItemHasAugment(lua_State* L)
{
	const auto item = getSharedPtr<Item>(L, 1);
	if (!item) {
		lua_pushnil(L);
		return 1;
	}
	
	if (isString(L, 2)) {
		const auto name = getString(L, 2);
		lua_pushboolean(L, item->hasAugment(name));
	} else if (isUserdata(L, 2)) {
		if (std::shared_ptr<Augment> augment = getSharedPtr<Augment>(L, 2)) {
			lua_pushboolean(L, item->hasAugment(augment));
		} else {
			reportError(__FUNCTION__, "Item::hasAugment() invalid userdata type passed as argument! \n");
			lua_pushnil(L);
		}
	} else {
		reportError(__FUNCTION__, "Item::hasAugment() passed invalid type, must be string or userdata! \n");
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemGetAugments(lua_State* L)
{
	const auto item = getSharedPtr<Item>(L, 1);
	if (!item) {
		lua_pushnil(L);
		return 1;
	}

	const auto augments = item->getAugments();
	lua_createtable(L, augments.size(), 0);

	int index = 0;
	for (const auto augment : augments) {
		pushSharedPtr(L, augment);
		setMetatable(L, -1, "Augment");
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaItemGiveCustomSkill(lua_State* L)
{
	// item:giveCustomSkill("skillname")
	// item:giveCustomSkill(CustomSkill)
	// returns bool
	if (auto& item = getSharedPtr<Item>(L, 1))
	{
		if (isString(L, 2) and isUserdata(L, 3))
		{
			auto skill_name = getString(L, 2);
			if (auto& skill = getSharedPtr<CustomSkill>(L, 3))
			{
				pushBoolean(L, item->giveCustomSkill(skill_name, std::move(skill)));
			}
		}
		else if (isString(L, 2))
		{
			auto skill_name = getString(L, 2);
			// Items might have predefined skills based on item type/ID
			if (auto skill = Items::getItemSkill(skill_name, item->getID()))
			{
				if (skill.has_value())
				{
					auto new_skill = Components::Skills::CustomSkill::make_skill(skill.value());
					pushBoolean(L, item->giveCustomSkill(skill_name, std::move(new_skill)));
					return 1;
				}
			}
			// Skill name not found, log it and send error to console
			lua_pushnil(L);
			return 1;
		}
		else
		{
			// first param is not a string, something is wrong, log it.
			lua_pushnil(L);
			return 1;
		}
	}
	else
	{
		// another log location
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemAddCustomSkill(lua_State* L)
{
	// item:addSkill("SkillName", levels)
	// returns bool
	if (auto& item = getSharedPtr<Item>(L, 1))
	{
		if (isString(L, 2) and isNumber(L, 3))
		{
			auto skill_name = getString(L, 2);
			auto levels_to_add = getNumber<uint16_t>(L, 3);
			if (auto skill = item->getCustomSkill(skill_name))
			{
				pushBoolean(L, skill->addLevels(levels_to_add));
				return 1;
			}
			else
			{
				// item doesn't have that skill
				// possible log?
				pushBoolean(L, false);
				return 1;
			}
		}
		else if (not isString(L, 2))
		{
			// todo send console error
			// log location
			pushBoolean(L, false);
			return 1;
		}
		else if (not isNumber(L, 3))
		{
			// todo send console error
			// log location
			pushBoolean(L, false);
			return 1;
		}
		else
		{
			// this should for sure be unreachable, log here
			pushBoolean(L, false);
		}
	}
	return 1;
}

int LuaScriptInterface::luaItemSubtractCustomSkill(lua_State* L)
{
	// item:subtractSkill("SkillName", levels)
	// returns bool
	if (auto& item = getSharedPtr<Item>(L, 1))
	{
		if (isString(L, 2) and isNumber(L, 3))
		{
			auto skill_name = getString(L, 2);
			auto levels_to_subtract = getNumber<uint16_t>(L, 3);

			if (auto skill = item->getCustomSkill(skill_name))
			{
				pushBoolean(L, skill->removeLevels(levels_to_subtract));
				return 1;
			}
			else
			{
				// item doesn't have that skill
				// possible log?
				pushBoolean(L, false);
				return 1;
			}
		}
		else if (not isString(L, 2))
		{
			// todo send console error
			// log location
			pushBoolean(L, false);
			return 1;
		}
		else if (not isNumber(L, 3))
		{
			// todo send console error
			// log location
			pushBoolean(L, false);
			return 1;
		}
		else
		{
			// this should for sure be unreachable, log here
			pushBoolean(L, false);
		}
	}
	return 1;
}

int LuaScriptInterface::luaItemAddBonusCustomSkill(lua_State* L)
{
	// item:addBonusSkill("SkillName", levels)
	// returns bool
	if (auto& item = getSharedPtr<Item>(L, 1))
	{
		if (isString(L, 2) and isNumber(L, 3))
		{
			auto skill_name = getString(L, 2);
			auto levels_to_add = getNumber<uint16_t>(L, 3);
			if (auto skill = item->getCustomSkill(skill_name))
			{
				pushBoolean(L, skill->addBonusLevels(levels_to_add));
				return 1;
			}
			else
			{
				// item doesn't have that skill
				// possible log?
				pushBoolean(L, false);
				return 1;
			}
		}
		else if (not isString(L, 2))
		{
			// todo send console error
			// log location
			pushBoolean(L, false);
			return 1;
		}
		else if (not isNumber(L, 3))
		{
			// todo send console error
			// log location
			pushBoolean(L, false);
			return 1;
		}
		else
		{
			// this should for sure be unreachable, log here
			pushBoolean(L, false);
		}
	}
	return 1;
}

int LuaScriptInterface::luaItemSubtractBonusSkill(lua_State* L)
{
	// item:subtractBonusSkill("SkillName", levels)
	// returns bool
	if (auto& item = getSharedPtr<Item>(L, 1))
	{
		if (isString(L, 2) and isNumber(L, 3))
		{
			auto skill_name = getString(L, 2);
			auto levels_to_remove = getNumber<uint16_t>(L, 3);

			if (auto skill = item->getCustomSkill(skill_name))
			{
				pushBoolean(L, skill->removeBonusLevels(levels_to_remove));
				return 1;
			}
			else
			{
				// item doesn't have that skill
				// possible log?
				pushBoolean(L, false);
				return 1;
			}
		}
		else if (not isString(L, 2))
		{
			// todo send console error
			// log location
			pushBoolean(L, false);
			return 1;
		}
		else if (not isNumber(L, 3))
		{
			// todo send console error
			// log location
			pushBoolean(L, false);
			return 1;
		}
		else
		{
			// this should for sure be unreachable, log here
			pushBoolean(L, false);
		}
	}
	return 1;
}

int LuaScriptInterface::luaItemClearBonusSkill(lua_State* L)
{
	// item:clearBonusSkill("SkillName")
	// returns bool
	if (auto& item = getSharedPtr<Item>(L, 1))
	{
		if (isString(L, 2))
		{
			auto skill_name = getString(L, 2);

			if (auto skill = item->getCustomSkill(skill_name))
			{
				skill->setBonus(0);
				pushBoolean(L, true);
				return 1;
			}
			else
			{
				// item doesn't have that skill
				// possible log?
				pushBoolean(L, false);
				return 1;
			}
		}
		else
		{
			// first param is not a string, something is wrong, log it.
			pushBoolean(L, false);
			return 1;
		}
	}
	return 1;
}

int LuaScriptInterface::luaItemRemoveCustomSkill(lua_State* L)
{
	// item:removeSkill("SkillName")
	// returns bool
	if (auto& item = getSharedPtr<Item>(L, 1))
	{
		if (isString(L, 2))
		{
			auto skill_name = getString(L, 2);
			pushBoolean(L, item->removeCustomSkill(skill_name));
			return 1;
		}
		else
		{
			// first param is not a string, something is wrong, log it.
			pushBoolean(L, false);
			return 1;
		}
	}
	return 1;
}

int LuaScriptInterface::luaItemHasCustomSkill(lua_State* L)
{
	// item:hasSkill("SkillName")
	// returns bool
	if (auto& item = getSharedPtr<Item>(L, 1))
	{
		if (isString(L, 2))
		{
			auto skill_name = getString(L, 2);
			auto has_skill = (item->getCustomSkill(skill_name) != nullptr) ? true : false;
			pushBoolean(L, has_skill);
			return 1;
		}
		else
		{
			// first param is not a string, something is wrong, log it.
			pushBoolean(L, false);
			return 1;
		}
	}
	return 1;
}

int LuaScriptInterface::luaItemCanGainSkillLevels(lua_State* L)
{
	// item:canSkill("SkillName")
	// returns bool
	if (auto& item = getSharedPtr<Item>(L, 1))
	{
		if (isString(L, 2))
		{
			auto skill_name = getString(L, 2);

			if (auto skill = item->getCustomSkill(skill_name))
			{
				auto can_skill = (skill->max() > skill->level()) ? true : false;
				pushBoolean(L, can_skill);
				return 1;
			}
			else
			{
				// item doesn't have that skill
				pushBoolean(L, false);
				return 1;
			}
		}
		else
		{
			// first param is not a string, something is wrong, log it.
			pushBoolean(L, false);
			return 1;
		}
	}
	return 1;
}

int LuaScriptInterface::luaItemGetCustomSkillLevel(lua_State* L)
{
	// item:getSkillLevel("SkillName", bonus = true)
	// returns number value
	if (auto& item = getSharedPtr<Item>(L, 1))
	{
		if (isString(L, 2))
		{
			auto skill_name = getString(L, 2);
			bool include_bonus = isBoolean(L, 3) ? (getBoolean(L, 3)) : true;

			if (auto skill = item->getCustomSkill(skill_name))
			{
				lua_pushinteger(L, skill->level(include_bonus));
			}
			else
			{
				// item doesn't have that skill
				lua_pushinteger(L, 0);
				return 1;
			}
		}
		else
		{
			// first param is not a string, something is wrong, log it.
			lua_pushinteger(L, 0);
			return 1;
		}
	}
	return 1;
}

int LuaScriptInterface::luaItemGetCustomSkillUserData(lua_State* L)
{
	// item:getSkill("SkillName")
	// returns userdata
	if (auto& item = getSharedPtr<Item>(L, 1))
	{
		if (isString(L, 2))
		{
			auto skill_name = getString(L, 2);

			if (auto skill = item->getCustomSkill(skill_name))
			{
				pushSharedPtr(L, skill);
				setMetatable(L, -1, "CustomSkill");
				return 1;
			}
			else
			{
				// item doesn't have that skill
				lua_pushnil(L);
				return 1;
			}
		}
		else
		{
			// first param is not a string, something is wrong, log it.
			lua_pushnil(L);
			return 1;
		}
	}
	return 1;
}

// Imbuement

int LuaScriptInterface::luaImbuementCreate(lua_State* L)
{
	// Imbuement(type, amount, duration, decayType = IMBUE_DECAY_EQUIPPED)
	ImbuementType imbueType = getNumber<ImbuementType>(L, 2, ImbuementType::IMBUEMENT_TYPE_NONE);
	uint32_t amount = getNumber<uint32_t>(L, 3);
	uint32_t duration = getNumber<uint32_t>(L, 4);
	ImbuementDecayType decayType = getNumber<ImbuementDecayType>(L, 5, ImbuementDecayType::IMBUEMENT_DECAY_EQUIPPED);
	pushSharedPtr(L, std::make_shared<Imbuement>(Imbuement(imbueType, amount, duration, decayType)));
	setMetatable(L, -1, "Imbuement");
	return 1;
}

int LuaScriptInterface::luaImbuementGetType(lua_State* L)
{
	// imbuement:getType()
	if (const auto imbue = getSharedPtr<Imbuement>(L, 1)) {
		lua_pushinteger(L, imbue->imbuetype);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaImbuementGetValue(lua_State* L)
{
	// imbuement:getValue()
	if (const auto imbue = getSharedPtr<Imbuement>(L, 1)) {
		lua_pushinteger(L, imbue->value);
	} else {
		lua_pushnil(L);
	}
	return 1;
}


int LuaScriptInterface::luaImbuementGetDuration(lua_State* L)
{
	// imbuement:getDuration()
	if (const auto imbue = getSharedPtr<Imbuement>(L, 1)) {
		lua_pushinteger(L, imbue->duration);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaImbuementIsSkill(lua_State* L)
{
	// imbuement:isSkill()
	if (const auto imbue = getSharedPtr<Imbuement>(L, 1)) {
		lua_pushinteger(L, imbue->isSkill());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaImbuementIsSpecialSkill(lua_State* L)
{
	// imbuement:isSpecialSkill()
	if (const auto imbue = getSharedPtr<Imbuement>(L, 1)) {
		lua_pushinteger(L, imbue->isSpecialSkill());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaImbuementIsDamage(lua_State* L)
{
	// imbuement:isDamage()
	if (const auto imbue = getSharedPtr<Imbuement>(L, 1)) {
		lua_pushinteger(L, imbue->isDamage());
	}
	else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaImbuementIsResist(lua_State* L)
{
	// imbuement:isResist()
	if (const auto imbue = getSharedPtr<Imbuement>(L, 1)) {
		lua_pushinteger(L, imbue->isResist());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaImbuementIsStat(lua_State* L)
{
	// imbuement:isStat()
	if (const auto imbue = getSharedPtr<Imbuement>(L, 1)) {
		lua_pushinteger(L, imbue->isStat());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaImbuementSetValue(lua_State* L)
{
	// imbuement:setAmount(amount)
	if (const auto imbue = getSharedPtr<Imbuement>(L, 1)) {
		imbue->value = getNumber<uint32_t>(L, 2);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaImbuementSetDuration(lua_State* L)
{
	// imbuement:setDuration(duration)
	if (const auto imbue = getSharedPtr<Imbuement>(L, 1)) {
		imbue->duration = getNumber<uint32_t>(L, 2);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaImbuementSetEquipDecay(lua_State* L)
{
	// imbuement:makeEquipDecayed()
	if (const auto imbue = getSharedPtr<Imbuement>(L, 1)) {
		imbue->decaytype = ImbuementDecayType::IMBUEMENT_DECAY_EQUIPPED;
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaImbuementSetInfightDecay(lua_State* L)
{
	// imbuement:makeInfightDecayed()
	if (const auto imbue = getSharedPtr<Imbuement>(L, 1)) {
		imbue->decaytype = ImbuementDecayType::IMBUEMENT_DECAY_INFIGHT;
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaImbuementIsEquipDecay(lua_State* L)
{
	// imbuement:isEquipDecayed()
	if (const auto imbue = getSharedPtr<Imbuement>(L, 1)) {
		pushBoolean(L, imbue->decaytype == ImbuementDecayType::IMBUEMENT_DECAY_EQUIPPED);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaImbuementIsInfightDecay(lua_State* L)
{
	// imbuement:isInfightDecayed()
	if (const auto imbue = getSharedPtr<Imbuement>(L, 1)) {
		pushBoolean(L, imbue->decaytype == ImbuementDecayType::IMBUEMENT_DECAY_INFIGHT);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaDamageModifierCreate(lua_State* L)
{
	// DamageModifier()
	if (lua_gettop(L) < 2) {
		pushSharedPtr(L, DamageModifier());
		setMetatable(L, -1, "DamageModifier");
		return 1;
	}

	// DamageModifier(stance, type, value, percent/flat, chance, combatType, originType, creatureType, race)
	if (const auto stance = getNumber<ImbuementType>(L, 2))
	{
		const auto modType = getNumber<uint8_t>(L, 3);
		const auto amount = getNumber<uint16_t>(L, 4);
		const auto factor = getNumber<ModFactor>(L, 5);
		const auto chance = getNumber<uint8_t>(L, 6, 100);
		const auto combatType = getNumber<CombatType_t>(L, 7, COMBAT_NONE);
		const auto originType = getNumber<CombatOrigin>(L, 8, ORIGIN_NONE);
		const auto creatureType = getNumber<CreatureType_t>(L, 9, CREATURETYPE_ATTACKABLE);
		const auto race = getNumber<RaceType_t>(L, 10, RACE_NONE);
		const auto& creatureName = getString(L, 11, "none");

		if (modType && amount && factor)
		{
			const auto& modifier = DamageModifier::makeModifier(stance, modType, amount, factor, chance, combatType, originType, creatureType, race, creatureName);
			pushSharedPtr(L, modifier);
			setMetatable(L, -1, "DamageModifier");
			return 1;
		}
		else
		{
			lua_pushnil(L);
			return 1;
		}
	}

	// DamageModifier(damageModifier)
	if (const auto& originalModifier = getSharedPtr<DamageModifier>(L, 2))
	{
		pushSharedPtr(L, DamageModifier(*originalModifier.get()));
		setMetatable(L, -1, "DamageModifier");
		return 1;
	}

	lua_pushnil(L);
	return 1;
}

int LuaScriptInterface::luaDamageModifierSetType(lua_State* L)
{
	if (const auto& modifier = getSharedPtr<DamageModifier>(L, 1)) 
	{
		if (const auto modType = getNumber<uint8_t>(L, 2)) 
		{
			modifier->setType(modType);
		}
	}
	else 
	{
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaDamageModifierSetStance(lua_State* L)
{
	if (const auto& modifier = getSharedPtr<DamageModifier>(L, 1))
	{
		if (const auto stance = getNumber<uint8_t>(L, 2))
		{
			modifier->setStance(stance);
		}
	}
	else
	{
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaDamageModifierSetChance(lua_State* L)
{
	if (const auto& modifier = getSharedPtr<DamageModifier>(L, 1))
	{
		if (const auto chance = getNumber<uint8_t>(L, 2))
		{
			modifier->setChance(chance);
		}
	}
	else
	{
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaDamageModifierSetValue(lua_State* L)
{
	if (const auto& modifier = getSharedPtr<DamageModifier>(L, 1)) 
	{
		if (const auto amount = getNumber<uint8_t>(L, 2)) 
		{
			modifier->setValue(amount);
		}
	} 
	else 
	{
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaDamageModifierSetRateFactor(lua_State* L)
{
	if (const auto& modifier = getSharedPtr<DamageModifier>(L, 1)) 
	{
		if (const auto factor = getNumber<uint8_t>(L, 2)) 
		{
			modifier->setFactor(factor);
		}
	} 
	else 
	{
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaDamageModifierSetCombatFilter(lua_State* L)
{
	if (const auto& modifier = getSharedPtr<DamageModifier>(L, 1)) 
	{
		if (auto combatType = getNumber<CombatType_t>(L, 2); combatType >= 0)
		{
			modifier->setCombatType(combatType);
		}
	} 
	else 
	{
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaDamageModifierSetOriginFilter(lua_State* L)
{
	if (const auto& modifier = getSharedPtr<DamageModifier>(L, 1)) 
	{
		if (const auto origin = getNumber<CombatOrigin>(L, 2)) 
		{
			modifier->setOriginType(origin);
		}
	}
	else 
	{
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaDamageModifierSetRaceFilter(lua_State* L)
{
	if (const auto& modifier = getSharedPtr<DamageModifier>(L, 1))
	{
		if (const auto race = getNumber<RaceType_t>(L, 2))
		{
			modifier->setRaceType(race);
		}
	}
	else
	{
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaDamageModifierSetCreatureTypeFilter(lua_State* L)
{
	if (const auto& modifier = getSharedPtr<DamageModifier>(L, 1))
	{
		if (const auto c_type = getNumber<CreatureType_t>(L, 2))
		{
			modifier->setCreatureType(c_type);
		}
	}
	else
	{
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaDamageModifierSetCreatureName(lua_State* L)
{
	if (const auto& modifier = getSharedPtr<DamageModifier>(L, 1))
	{
			modifier->setCreatureName(getString(L, 2, "none"));
	}
	else
	{
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaAugmentRegister(lua_State* L)
{
	if (lua_gettop(L) < 2) {
		lua_pushnil(L);
		return 1;
	}

	const auto& augment = getSharedPtr<Augment>(L, 1);
	if (Augments::GetAugment(augment->getName())) 
	{
		// already registered
		lua_pushboolean(L, 0);
		return 1;
	}
	Augments::AddAugment(augment);
	lua_pushboolean(L, 1);
	return 1;
}

int LuaScriptInterface::luaAugmentCreate(lua_State* L)
{	// To-do : Augment(augment) and Augment(name) <-- where name is looked up from global collection.
	// Augment(name, description, modifier or table_of_modifiers)

	if (isString(L, 2)) {
		const auto name = getString(L, 2);

		if (const auto augment = Augments::GetAugment(name)) {
			pushSharedPtr(L, augment);
			setMetatable(L, -1, "Augment");
			return 1; // return early here because we found a global augment with this name
		}

		auto description = getString(L, 3);
		if (isUserdata(L, 4)) {
			if (auto modifier = getSharedPtr<DamageModifier>(L, 4)) {
				auto augment = Augment::MakeAugment(name, description);
				augment->addModifier(modifier);
				pushSharedPtr(L, augment);
				setMetatable(L, -1, "Augment");
			} else {
				reportError(__FUNCTION__, "Invalid Userdata For Modifier Parameter used during Augment Creation \n");
				lua_pushnil(L);
			}
		} else if (isTable(L, 4)) {
			auto list = std::vector<std::shared_ptr<DamageModifier>>();
			list.reserve(24);

			// Iterate over the table at index 4
			lua_pushnil(L);  // First key for lua_next
			while (lua_next(L, 4) != 0) {
				// Check if the value is userdata and of type DamageModifier
				if (isUserdata(L, -1)) {
					if (auto modifier = getSharedPtr<DamageModifier>(L, -1)) {
						list.emplace_back(modifier);
					} else {
						reportError(__FUNCTION__, "Invalid userdata in table element\n");
					}
				} else {
					reportError(__FUNCTION__, "Non-userdata found in table element\n");
				}
				// Remove the value, keep the key for lua_next
				lua_pop(L, 1);
			}

			// Create augment with all modifiers
			// To-do : Add augments created this particular way to global table
			const auto augment = Augment::MakeAugment(name, description);
			for (auto& modifier : list) {
				augment->addModifier(modifier);
			}
			pushSharedPtr(L, augment);
			setMetatable(L, -1, "Augment");
		} else {
			reportError(__FUNCTION__, "Invalid parameter for Augment Creation\n");
			lua_pushnil(L);
		}
	}
	
	
	return 1;
}

	
int LuaScriptInterface::luaAugmentSetName(lua_State* L)
{
	// Augment:setName(newName)
	const auto augment = getSharedPtr<Augment>(L, 1);
	if (!augment) {
		reportError(__FUNCTION__, "Invalid Augment userdata\n");
		return 0;
	}

	auto newName = getString(L, 2);
	augment->setName(newName);
	return 0;
}

int LuaScriptInterface::luaAugmentSetDescription(lua_State* L)
{
	// Augment:getDescription(newDescription)
	const auto augment = getSharedPtr<Augment>(L, 1);
	if (!augment) {
		reportError(__FUNCTION__, "Invalid Augment userdata\n");
		return 0;
	}

	const auto newDescription = getString(L, 2);
	augment->setDescription(newDescription);
	return 0;
}

int LuaScriptInterface::luaAugmentGetName(lua_State* L) {
	// Augment:getName()
	const auto augment = getSharedPtr<Augment>(L, 1); // Get augment object
	if (!augment) {
		reportError(__FUNCTION__, "Invalid Augment userdata\n");
		lua_pushnil(L);
		return 1;
	}

	const std::string name = augment->getName();
	pushString(L, name);
	return 1;
}

int LuaScriptInterface::luaAugmentGetDescription(lua_State* L) {
	// Augment:getDescription()
	const auto augment = getSharedPtr<Augment>(L, 1);
	if (!augment) {
		reportError(__FUNCTION__, "Invalid Augment userdata\n");
		lua_pushnil(L);
		return 1;
	}

	const std::string description = augment->getDescription();
	pushString(L, description);
	return 1;
}

// To-do : The following methods that return 0 should all be converted to return something (boolean for most).
int LuaScriptInterface::luaAugmentAddDamageModifier(lua_State* L)
{
	// Augment:addDamageModifier(modifier)
	const auto augment = getSharedPtr<Augment>(L, 1);
	if (!augment) {
		reportError(__FUNCTION__, "Invalid Augment userdata\n");
		return 0;
	}

	auto modifier = getSharedPtr<DamageModifier>(L, 2);
	if (!modifier) {
		reportError(__FUNCTION__, "Invalid DamageModifier userdata\n");
		return 0;
	}

	augment->addModifier(modifier);
	return 0;
}

int LuaScriptInterface::luaAugmentRemoveDamageModifier(lua_State* L)
{
	// Augment:RemoveDamageModifier(modifier)
	const auto augment = getSharedPtr<Augment>(L, 1);
	if (!augment) {
		reportError(__FUNCTION__, "Invalid Augment userdata\n");
		return 0;
	}

	auto modifier = getSharedPtr<DamageModifier>(L, 2);
	if (!modifier) {
		reportError(__FUNCTION__, "Invalid DamageModifier userdata\n");
		return 0;
	}

	augment->removeModifier(modifier);
	return 0;
}

int LuaScriptInterface::luaAugmentGetAttackModifiers(lua_State* L) {
	// Augment:GetAttackModifiers([modType])
	const auto augment = getSharedPtr<Augment>(L, 1);
	if (!augment) {
		reportError(__FUNCTION__, "Invalid Augment userdata\n");
		lua_pushnil(L);
		return 1;
	}

	std::vector<std::shared_ptr<DamageModifier>> modifiers;

	if (lua_gettop(L) > 1 && lua_isinteger(L, 2)) {
		const uint8_t modType = static_cast<uint8_t>(lua_tointeger(L, 2));
		modifiers = augment->getAttackModifiers(modType);
	} else {
		modifiers = augment->getAttackModifiers();
	}

	lua_newtable(L);
	int index = 1;
	for (const auto& modifier : modifiers) {
		pushSharedPtr(L, modifier);
		setMetatable(L, -1, "DamageModifier");
		lua_rawseti(L, -2, index++);
	}

	return 1;
}

int LuaScriptInterface::luaAugmentGetDefenseModifiers(lua_State* L) {
	// Augment:GetDefenseModifiers([modType])
	const auto augment = getSharedPtr<Augment>(L, 1);
	if (!augment) {
		reportError(__FUNCTION__, "Invalid Augment userdata\n");
		lua_pushnil(L);
		return 1;
	}

	std::vector<std::shared_ptr<DamageModifier>> modifiers;

	if (lua_gettop(L) > 1 && lua_isinteger(L, 2)) {
		uint8_t modType = static_cast<uint8_t>(lua_tointeger(L, 2));
		modifiers = augment->getDefenseModifiers(modType);
	} else {
		modifiers = augment->getDefenseModifiers();
	}

	lua_newtable(L);
	int index = 1;
	for (const auto& modifier : modifiers) {
		pushSharedPtr(L, modifier);
		setMetatable(L, -1, "DamageModifier");
		lua_rawseti(L, -2, index++);
	}

	return 1;
}


// Container
int LuaScriptInterface::luaContainerCreate(lua_State* L)
{
	// Container(uid)
	const uint32_t id = getNumber<uint32_t>(L, 2);

	if (const auto container = getScriptEnv()->getContainerByUID(id)) {
		pushSharedPtr(L, container);
		setMetatable(L, -1, "Container");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaContainerDelete(lua_State* L)
{
	if (auto& container = getSharedPtr<Container>(L, 1)) {
		container.reset();
	}
	return 0;
}

int LuaScriptInterface::luaContainerGetSize(lua_State* L)
{
	// container:getSize()
	if (const auto container = getSharedPtr<Container>(L, 1)) {
		lua_pushinteger(L, container->size());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaContainerGetCapacity(lua_State* L)
{
	// container:getCapacity()
	if (const auto container = getSharedPtr<Container>(L, 1)) {
		lua_pushinteger(L, container->capacity());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaContainerGetEmptySlots(lua_State* L)
{
	// container:getEmptySlots([recursive = false])
	const auto container = getSharedPtr<Container>(L, 1);
	if (!container) {
		lua_pushnil(L);
		return 1;
	}

	uint32_t slots = container->capacity() - container->size();
	if (getBoolean(L, 2, false)) {
		for (ContainerIterator it = container->iterator(); it.hasNext(); it.advance()) {
			if (const auto tmpContainer = (*it)->getContainer()) {
				slots += tmpContainer->capacity() - tmpContainer->size();
			}
		}
	}
	lua_pushinteger(L, slots);
	return 1;
}

int LuaScriptInterface::luaContainerGetItemHoldingCount(lua_State* L)
{
	// container:getItemHoldingCount()
	if (const auto container = getSharedPtr<Container>(L, 1)) {
		lua_pushinteger(L, container->getItemHoldingCount());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaContainerGetItem(lua_State* L)
{
	// container:getItem(index)
	const auto container = getSharedPtr<Container>(L, 1);
	if (!container) {
		lua_pushnil(L);
		return 1;
	}

	uint32_t index = getNumber<uint32_t>(L, 2);
	if (const auto item = container->getItemByIndex(index)) {
		pushSharedPtr(L, item);
		setItemMetatable(L, -1, item);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaContainerHasItem(lua_State* L)
{
	// container:hasItem(item)
	if (const auto container = getSharedPtr<Container>(L, 1);
		const auto item = getSharedPtr<Item>(L, 2)) {
		pushBoolean(L, container->isHoldingItem(item));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaContainerAddItem(lua_State* L)
{
	// container:addItem(itemId[, count/subType = 1[, index = INDEX_WHEREEVER[, flags = 0]]])
	const auto container = getSharedPtr<Container>(L, 1);
	if (!container) {
		lua_pushnil(L);
		return 1;
	}

	uint16_t itemId;
	if (isNumber(L, 2)) {
		itemId = getNumber<uint16_t>(L, 2);
	} else {
		itemId = Item::items.getItemIdByName(getString(L, 2));
		if (itemId == 0) {
			lua_pushnil(L);
			return 1;
		}
	}

	const ItemType& it = Item::items[itemId];
	int32_t itemCount = 1;
	int32_t subType = 1;
	uint32_t count = getNumber<uint32_t>(L, 3, 1);

	if (it.hasSubType()) {
		if (it.stackable) {
			itemCount = std::ceil(count / 100.f);
		}
		subType = count;
	} else {
		itemCount = std::max<int32_t>(1, count);
	}

	bool hasTable = itemCount > 1;
	if (hasTable) {
		lua_newtable(L);
	} else if (itemCount == 0) {
		lua_pushnil(L);
		return 1;
	}

	int32_t index = getNumber<int32_t>(L, 4, INDEX_WHEREEVER);
	uint32_t flags = getNumber<uint32_t>(L, 5, 0);

	for (int32_t i = 1; i <= itemCount; ++i) {
		const int32_t stackCount = std::min<int32_t>(subType, 100);
		auto item = Item::CreateItem(itemId, stackCount);
		if (!item) {
			reportErrorFunc(L, getErrorDesc(LUA_ERROR_ITEM_NOT_FOUND));
			if (!hasTable) {
				lua_pushnil(L);
			}
			return 1;
		}

		if (it.stackable) {
			subType -= stackCount;
		}
		CylinderPtr holder = container;
		ReturnValue ret = g_game.internalAddItem(holder, item, index, flags);
		if (ret != RETURNVALUE_NOERROR) {
			item.reset();
			if (!hasTable) {
				lua_pushnil(L);
			}
			return 1;
		}

		if (hasTable) {
			lua_pushinteger(L, i);
			pushSharedPtr(L, item);
			setItemMetatable(L, -1, item);
			lua_settable(L, -3);
		} else {
			pushSharedPtr(L, item);
			setItemMetatable(L, -1, item);
		}
	}
	return 1;
}

int LuaScriptInterface::luaContainerAddItemEx(lua_State* L)
{
	// container:addItemEx(item[, index = INDEX_WHEREEVER[, flags = 0]])
	const auto item = getSharedPtr<Item>(L, 2);
	if (!item) {
		lua_pushnil(L);
		return 1;
	}

	const auto container = getSharedPtr<Container>(L, 1);
	if (!container) {
		lua_pushnil(L);
		return 1;
	}

	if (item->getParent() != VirtualCylinder::virtualCylinder) {
		reportErrorFunc(L, "Item already has a parent");
		lua_pushnil(L);
		return 1;
	}

	int32_t index = getNumber<int32_t>(L, 3, INDEX_WHEREEVER);
	uint32_t flags = getNumber<uint32_t>(L, 4, 0);
	CylinderPtr holder = container;
	ReturnValue ret = g_game.internalAddItem(holder, item, index, flags);
	if (ret == RETURNVALUE_NOERROR) {
		ScriptEnvironment::removeTempItem(item);
	}
	lua_pushinteger(L, ret);
	return 1;
}

int LuaScriptInterface::luaContainerGetCorpseOwner(lua_State* L)
{
	// container:getCorpseOwner()
	if (const auto container = getSharedPtr<Container>(L, 1)) {
		lua_pushinteger(L, container->getCorpseOwner());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaContainerGetItemCountById(lua_State* L)
{
	// container:getItemCountById(itemId[, subType = -1])
	const auto container = getSharedPtr<Container>(L, 1);
	if (!container) {
		lua_pushnil(L);
		return 1;
	}

	uint16_t itemId;
	if (isNumber(L, 2)) {
		itemId = getNumber<uint16_t>(L, 2);
	} else {
		itemId = Item::items.getItemIdByName(getString(L, 2));
		if (itemId == 0) {
			lua_pushnil(L);
			return 1;
		}
	}

	int32_t subType = getNumber<int32_t>(L, 3, -1);
	lua_pushinteger(L, container->getItemTypeCount(itemId, subType));
	return 1;
}

int LuaScriptInterface::luaContainerGetContentDescription(lua_State* L)
{
	// container:getContentDescription()
	if (const auto container = getSharedPtr<Container>(L, 1)) {
		pushString(L, container->getContentDescription());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaContainerGetItems(lua_State* L)
{
	// container:getItems([recursive = false])
	const auto container = getSharedPtr<Container>(L, 1);
	if (!container) {
		lua_pushnil(L);
		return 1;
	}

	bool recursive = getBoolean(L, 2, false);
	const auto items = container->getItems(recursive);

	lua_createtable(L, items.size(), 0);

	int index = 0;
	for (const auto item : items) {
		pushSharedPtr(L, item);
		setItemMetatable(L, -1, item);
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

// Teleport
int LuaScriptInterface::luaTeleportCreate(lua_State* L)
{
	// Teleport(uid)
	const uint32_t id = getNumber<uint32_t>(L, 2);

	if (const auto item = getScriptEnv()->getItemByUID(id); item && item->getTeleport()) {
		pushSharedPtr(L, item);
		setMetatable(L, -1, "Teleport");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaTeleportDelete(lua_State* L)
{
	if (auto& teleport = getSharedPtr<Teleport>(L, 1)) {
		teleport.reset();
	}
	return 0;
}

int LuaScriptInterface::luaTeleportGetDestination(lua_State* L)
{
	// teleport:getDestination()
	if (const auto teleport = getSharedPtr<Teleport>(L, 1)) {
		pushPosition(L, teleport->getDestPos());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaTeleportSetDestination(lua_State* L)
{
	// teleport:setDestination(position)
	if (const auto teleport = getSharedPtr<Teleport>(L, 1)) {
		teleport->setDestPos(getPosition(L, 2));
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// Creature
int LuaScriptInterface::luaCreatureCreate(lua_State* L)
{
	// Creature(id or name or userdata)
	CreaturePtr creature;
	if (isNumber(L, 2)) {
		creature = g_game.getCreatureByID(getNumber<uint32_t>(L, 2));
	} else if (isString(L, 2)) {
		creature = g_game.getCreatureByName(getString(L, 2));
	} else if (isUserdata(L, 2)) {
		if (const LuaDataType type = getUserdataType(L, 2); type != LuaData_Player && type != LuaData_Monster && type != LuaData_Npc) {
			lua_pushnil(L);
			return 1;
		}
		creature = getSharedPtr<Creature>(L, 2);
	} else {
		creature = nullptr;
	}

	if (creature) {
		pushSharedPtr(L, creature);
		setCreatureMetatable(L, -1, creature);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureDelete(lua_State* L)
{
	if (auto& creature = getSharedPtr<Creature>(L, 1)) {
		creature.reset();
	}
	return 0;
}

int LuaScriptInterface::luaCreatureGetEvents(lua_State* L)
{
	// creature:getEvents(type)
	const auto creature = getSharedPtr<Creature>(L, 1);
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	const CreatureEventType_t eventType = getNumber<CreatureEventType_t>(L, 2);
	const auto& eventList = creature->getCreatureEvents(eventType);
	lua_createtable(L, eventList.size(), 0);

	int index = 0;
	for (const auto event : eventList) {
		pushString(L, event->getName());
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureRegisterEvent(lua_State* L)
{
	// creature:registerEvent(name)
	if (const auto creature = getSharedPtr<Creature>(L, 1)) {
		const std::string& name = getString(L, 2);
		pushBoolean(L, creature->registerCreatureEvent(name));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureUnregisterEvent(lua_State* L)
{
	// creature:unregisterEvent(name)
	const std::string& name = getString(L, 2);
	if (const auto creature = getSharedPtr<Creature>(L, 1)) {
		pushBoolean(L, creature->unregisterCreatureEvent(name));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureIsRemoved(lua_State* L)
{
	// creature:isRemoved()
	if (const auto creature = getSharedPtr<const Creature>(L, 1)) {
		pushBoolean(L, creature->isRemoved());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureIsCreature(lua_State* L)
{
	// creature:isCreature()
	pushBoolean(L, getSharedPtr<const Creature>(L, 1) != nullptr);
	return 1;
}

int LuaScriptInterface::luaCreatureIsInGhostMode(lua_State* L)
{
	// creature:isInGhostMode()
	if (const auto creature = getSharedPtr<const Creature>(L, 1)) {
		pushBoolean(L, creature->isInGhostMode());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureIsHealthHidden(lua_State* L)
{
	// creature:isHealthHidden()
	if (const auto creature = getSharedPtr<const Creature>(L, 1)) {
		pushBoolean(L, creature->isHealthHidden());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureIsMovementBlocked(lua_State* L)
{
	// creature:isMovementBlocked()
	if (const auto creature = getSharedPtr<const Creature>(L, 1)) {
		pushBoolean(L, creature->isMovementBlocked());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureCanSee(lua_State* L)
{
	// creature:canSee(position)
	if (const auto creature = getSharedPtr<const Creature>(L, 1)) {
		const Position& position = getPosition(L, 2);
		pushBoolean(L, creature->canSee(position));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureCanSeeCreature(lua_State* L)
{
	// creature:canSeeCreature(creature)
	if (const auto creature = getSharedPtr<const Creature>(L, 1)) {
		const auto other_creature = getCreature(L, 2);
		if (!other_creature) {
			reportErrorFunc(L, getErrorDesc(LUA_ERROR_CREATURE_NOT_FOUND));
			pushBoolean(L, false);
			return 1;
		}

		pushBoolean(L, creature->canSeeCreature(other_creature));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureCanSeeGhostMode(lua_State* L)
{
	// creature:canSeeGhostMode(creature)
	if (const auto creature = getSharedPtr<const Creature>(L, 1)) {
		const auto otherCreature = getCreature(L, 2);
		if (!otherCreature) {
			reportErrorFunc(L, getErrorDesc(LUA_ERROR_CREATURE_NOT_FOUND));
			pushBoolean(L, false);
			return 1;
		}

		pushBoolean(L, creature->canSeeGhostMode(otherCreature));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureCanSeeInvisibility(lua_State* L)
{
	// creature:canSeeInvisibility()
	if (const auto creature = getSharedPtr<const Creature>(L, 1)) {
		pushBoolean(L, creature->canSeeInvisibility());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureGetParent(lua_State* L)
{
	// creature:getParent()
	const auto creature = getSharedPtr<Creature>(L, 1);
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	CylinderPtr parent = creature->getParent();
	if (!parent) {
		lua_pushnil(L);
		return 1;
	}

	pushCylinder(L, parent);
	return 1;
}

int LuaScriptInterface::luaCreatureGetId(lua_State* L)
{
	// creature:getId()
	if (const auto creature = getSharedPtr<const Creature>(L, 1)) {
		lua_pushinteger(L, creature->getID());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureGetName(lua_State* L)
{
	// creature:getName()
	if (const auto creature = getSharedPtr<const Creature>(L, 1)) {
		pushString(L, creature->getName());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureGetTarget(lua_State* L)
{
	// creature:getTarget()
	const auto creature = getSharedPtr<Creature>(L, 1);
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	if (const auto target = creature->getAttackedCreature()) {
		pushSharedPtr(L, target);
		setCreatureMetatable(L, -1, target);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureSetTarget(lua_State* L)
{
	// creature:setTarget(target)
	if (const auto creature = getSharedPtr<Creature>(L, 1)) {
		pushBoolean(L, creature->setAttackedCreature(getCreature(L, 2)));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureGetFollowCreature(lua_State* L)
{
	// creature:getFollowCreature()
	const auto creature = getSharedPtr<Creature>(L, 1);
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	if (const auto followCreature = creature->getFollowCreature()) {
		pushSharedPtr(L, followCreature);
		setCreatureMetatable(L, -1, followCreature);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureSetFollowCreature(lua_State* L)
{
	// creature:setFollowCreature(followedCreature)
	if (const auto creature = getSharedPtr<Creature>(L, 1)) {
		pushBoolean(L, creature->setFollowCreature(getCreature(L, 2)));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureGetMaster(lua_State* L)
{
	// creature:getMaster()
	const auto creature = getSharedPtr<Creature>(L, 1);
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	const auto master = creature->getMaster();
	if (!master) {
		lua_pushnil(L);
		return 1;
	}

	pushSharedPtr(L, master);
	setCreatureMetatable(L, -1, master);
	return 1;
}

int LuaScriptInterface::luaCreatureSetMaster(lua_State* L)
{
	// creature:setMaster(master)
	const auto creature = getSharedPtr<Creature>(L, 1);
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	pushBoolean(L, creature->setMaster(getCreature(L, 2)));

	// update summon icon
	g_game.notifySpectators(creature);
	return 1;
}

int LuaScriptInterface::luaCreatureGetLight(lua_State* L)
{
	// creature:getLight()
	const auto creature = getSharedPtr<const Creature>(L, 1);
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	const LightInfo lightInfo = creature->getCreatureLight();
	lua_pushinteger(L, lightInfo.level);
	lua_pushinteger(L, lightInfo.color);
	return 2;
}

int LuaScriptInterface::luaCreatureSetLight(lua_State* L)
{
	// creature:setLight(color, level)
	const auto creature = getSharedPtr<Creature>(L, 1);
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	LightInfo light;
	light.color = getNumber<uint8_t>(L, 2);
	light.level = getNumber<uint8_t>(L, 3);
	creature->setCreatureLight(light);
	g_game.changeLight(creature);
	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaCreatureGetSpeed(lua_State* L)
{
	// creature:getSpeed()
	if (const auto creature = getUserdata<const Creature>(L, 1)) {
		lua_pushinteger(L, creature->getSpeed());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureGetBaseSpeed(lua_State* L)
{
	// creature:getBaseSpeed()
	if (const auto creature = getSharedPtr<const Creature>(L, 1)) {
		lua_pushinteger(L, creature->getBaseSpeed());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureChangeSpeed(lua_State* L)
{
	// creature:changeSpeed(delta)
	const auto creature = getCreature(L, 1);
	if (!creature) {
		reportErrorFunc(L, getErrorDesc(LUA_ERROR_CREATURE_NOT_FOUND));
		pushBoolean(L, false);
		return 1;
	}

	int32_t delta = getNumber<int32_t>(L, 2);
	g_game.changeSpeed(creature, delta);
	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaCreatureSetDropLoot(lua_State* L)
{
	// creature:setDropLoot(doDrop)
	if (const auto creature = getSharedPtr<Creature>(L, 1)) {
		creature->setDropLoot(getBoolean(L, 2));
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureSetSkillLoss(lua_State* L)
{
	// creature:setSkillLoss(skillLoss)
	if (const auto creature = getSharedPtr<Creature>(L, 1)) {
		creature->setSkillLoss(getBoolean(L, 2));
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureGetPosition(lua_State* L)
{
	// creature:getPosition()
	if (const auto creature = getSharedPtr<const Creature>(L, 1)) {
		pushPosition(L, creature->getPosition());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureGetTile(lua_State* L)
{
	// creature:getTile()
	const auto creature = getSharedPtr<Creature>(L, 1);
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	if (const auto tile = creature->getTile()) {
		pushSharedPtr(L, tile);
		setMetatable(L, -1, "Tile");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureGetDirection(lua_State* L)
{
	// creature:getDirection()
	if (const auto creature = getSharedPtr<const Creature>(L, 1)) {
		lua_pushinteger(L, creature->getDirection());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureSetDirection(lua_State* L)
{
	// creature:setDirection(direction)
	if (const auto creature = getSharedPtr<Creature>(L, 1)) {
		pushBoolean(L, g_game.internalCreatureTurn(creature, getNumber<Direction>(L, 2)));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureGetHealth(lua_State* L)
{
	// creature:getHealth()
	if (const auto creature = getSharedPtr<const Creature>(L, 1)) {
		lua_pushinteger(L, creature->getHealth());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureSetHealth(lua_State* L)
{
	// creature:setHealth(health)
	const auto creature = getSharedPtr<Creature>(L, 1);
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	creature->health = std::min<int32_t>(getNumber<uint32_t>(L, 2), creature->healthMax);
	g_game.addCreatureHealth(creature);

	if (const auto player = creature->getPlayer()) {
		player->sendStats();
	}
	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaCreatureAddHealth(lua_State* L)
{
	// creature:addHealth(healthChange)
	const auto creature = getSharedPtr<Creature>(L, 1);
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	CombatDamage damage;
	damage.primary.value = getNumber<int32_t>(L, 2);
	if (damage.primary.value >= 0) {
		damage.primary.type = COMBAT_HEALING;
	} else {
		damage.primary.type = COMBAT_UNDEFINEDDAMAGE;
	}
	pushBoolean(L, g_game.combatChangeHealth(nullptr, creature, damage));
	return 1;
}

int LuaScriptInterface::luaCreatureGetMaxHealth(lua_State* L)
{
	// creature:getMaxHealth()
	if (const auto creature = getSharedPtr<const Creature>(L, 1)) {
		lua_pushinteger(L, creature->getMaxHealth());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureSetMaxHealth(lua_State* L)
{
	// creature:setMaxHealth(maxHealth)
	const auto creature = getSharedPtr<Creature>(L, 1);
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	creature->healthMax = getNumber<uint32_t>(L, 2);
	creature->health = std::min<int32_t>(creature->health, creature->healthMax);
	g_game.addCreatureHealth(creature);

	if (const auto player = creature->getPlayer()) {
		player->sendStats();
	}
	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaCreatureSetHiddenHealth(lua_State* L)
{
	// creature:setHiddenHealth(hide)
	if (const auto creature = getSharedPtr<Creature>(L, 1)) {
		creature->setHiddenHealth(getBoolean(L, 2));
		g_game.addCreatureHealth(creature);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureSetMovementBlocked(lua_State* L)
{
	// creature:setMovementBlocked(state)
	if (const auto creature = getSharedPtr<Creature>(L, 1)) {
		creature->setMovementBlocked(getBoolean(L, 2));
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureGetSkull(lua_State* L)
{
	// creature:getSkull()
	if (const auto creature = getSharedPtr<Creature>(L, 1)) {
		lua_pushinteger(L, creature->getSkull());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureSetSkull(lua_State* L)
{
	// creature:setSkull(skull)
	if (const auto creature = getSharedPtr<Creature>(L, 1)) {
		creature->setSkull(getNumber<Skulls_t>(L, 2));
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureGetOutfit(lua_State* L)
{
	// creature:getOutfit()
	if (const auto creature = getSharedPtr<const Creature>(L, 1)) {
		pushOutfit(L, creature->getCurrentOutfit());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureSetOutfit(lua_State* L)
{
	// creature:setOutfit(outfit)
	if (const auto creature = getSharedPtr<Creature>(L, 1)) {
		creature->defaultOutfit = getOutfit(L, 2);
		g_game.internalCreatureChangeOutfit(creature, creature->defaultOutfit);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureGetCondition(lua_State* L)
{
	// creature:getCondition(conditionType[, conditionId = CONDITIONID_COMBAT[, subId = 0]])
	const auto creature = getSharedPtr<Creature>(L, 1);
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	ConditionType_t conditionType = getNumber<ConditionType_t>(L, 2);
	ConditionId_t conditionId = getNumber<ConditionId_t>(L, 3, CONDITIONID_COMBAT);
	uint32_t subId = getNumber<uint32_t>(L, 4, 0);

	if (const auto condition = creature->getCondition(conditionType, conditionId, subId)) {
		pushUserdata<Condition>(L, condition);
		setWeakMetatable(L, -1, "Condition");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureAddCondition(lua_State* L)
{
	// creature:addCondition(condition[, force = false])
	const auto creature = getSharedPtr<Creature>(L, 1);
	Condition* condition = getUserdata<Condition>(L, 2);
	if (creature && condition) {
		const bool force = getBoolean(L, 3, false);
		pushBoolean(L, creature->addCondition(condition->clone(), force));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureRemoveCondition(lua_State* L)
{
	// creature:removeCondition(conditionType[, conditionId = CONDITIONID_COMBAT[, subId = 0[, force = false]]])
	// creature:removeCondition(condition[, force = false])
	const auto creature = getSharedPtr<Creature>(L, 1);
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	Condition* condition = nullptr;

	bool force = false;

	if (isUserdata(L, 2)) {
		condition = getUserdata<Condition>(L, 2);
		force = getBoolean(L, 3, false);
	} else {
		ConditionType_t conditionType = getNumber<ConditionType_t>(L, 2);
		ConditionId_t conditionId = getNumber<ConditionId_t>(L, 3, CONDITIONID_COMBAT);
		uint32_t subId = getNumber<uint32_t>(L, 4, 0);
		condition = creature->getCondition(conditionType, conditionId, subId);
		force = getBoolean(L, 5, false);
	}

	if (condition) {
		creature->removeCondition(condition, force);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureHasCondition(lua_State* L)
{
	// creature:hasCondition(conditionType[, subId = 0])
	const auto creature = getSharedPtr<Creature>(L, 1);
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	const auto conditionType = getNumber<ConditionType_t>(L, 2);
	const uint32_t subId = getNumber<uint32_t>(L, 3, 0);
	pushBoolean(L, creature->hasCondition(conditionType, subId));
	return 1;
}

int LuaScriptInterface::luaCreatureIsImmune(lua_State* L)
{
	// creature:isImmune(condition or conditionType)
	const auto creature = getSharedPtr<Creature>(L, 1);
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	if (isNumber(L, 2)) {
		pushBoolean(L, creature->isImmune(getNumber<ConditionType_t>(L, 2)));
	} else if (Condition* condition = getUserdata<Condition>(L, 2)) {
		pushBoolean(L, creature->isImmune(condition->getType()));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureRemove(lua_State* L)
{
	// creature:remove()
	auto creaturePtr = getSharedPtr<Creature>(L, 1);
	if (!creaturePtr) {
		lua_pushnil(L);
		return 1;
	}

	auto creature = creaturePtr;
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	if (auto player = creature->getPlayer()) {
		player->kickPlayer(true);
	} else {
		g_game.removeCreature(creature);
	}

	creaturePtr = nullptr;
	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaCreatureTeleportTo(lua_State* L)
{
	// creature:teleportTo(position[, pushMovement = false])
	bool pushMovement = getBoolean(L, 3) || false ;

	const Position& position = getPosition(L, 2);
	const auto creature = getSharedPtr<Creature>(L, 1);
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	const Position oldPosition = creature->getPosition();
	if (g_game.internalTeleport(creature, position, pushMovement) != RETURNVALUE_NOERROR) {
		pushBoolean(L, false);
		return 1;
	}

	if (pushMovement) {
		if (oldPosition.x == position.x) {
			if (oldPosition.y < position.y) {
				g_game.internalCreatureTurn(creature, DIRECTION_SOUTH);
			} else {
				g_game.internalCreatureTurn(creature, DIRECTION_NORTH);
			}
		} else if (oldPosition.x > position.x) {
			g_game.internalCreatureTurn(creature, DIRECTION_WEST);
		} else if (oldPosition.x < position.x) {
			g_game.internalCreatureTurn(creature, DIRECTION_EAST);
		}
	}
	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaCreatureSay(lua_State* L)
{
	// creature:say(text[, type = TALKTYPE_MONSTER_SAY[, ghost = false[, target = nullptr[, position]]]])
	int parameters = lua_gettop(L);

	Position position;
	if (parameters >= 6) {
		position = getPosition(L, 6);
		if (!position.x || !position.y) {
			reportErrorFunc(L, "Invalid position specified.");
			pushBoolean(L, false);
			return 1;
		}
	}

	CreaturePtr target = nullptr;
	if (parameters >= 5) {
		target = getCreature(L, 5);
	}

	const bool ghost = getBoolean(L, 4, false);

	const SpeakClasses type = getNumber<SpeakClasses>(L, 3, TALKTYPE_MONSTER_SAY);
	const std::string& text = getString(L, 2);
	const CreaturePtr creature = getSharedPtr<Creature>(L, 1);
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	SpectatorVec spectators;
	if (target) {
		spectators.emplace_back(target);
	}

	// Prevent infinity echo on event onHear
	const bool echo = getScriptEnv()->getScriptId() == g_events->getScriptId(EventInfoId::CREATURE_ONHEAR);

	if (position.x != 0) {
		pushBoolean(L, g_game.internalCreatureSay(creature, type, text, ghost, &spectators, &position, echo));
	} else {
		pushBoolean(L, g_game.internalCreatureSay(creature, type, text, ghost, &spectators, nullptr, echo));
	}
	return 1;
}

int LuaScriptInterface::luaCreatureGetDamageMap(lua_State* L)
{
	// creature:getDamageMap()
	const auto creature = getSharedPtr<Creature>(L, 1);
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	lua_createtable(L, creature->damageMap.size(), 0);
	for (const auto& [index, value] : creature->damageMap) {
		lua_createtable(L, 0, 2);
		setField(L, "total", value.total);
		setField(L, "ticks", value.ticks);
		lua_rawseti(L, -2, index);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureGetSummons(lua_State* L)
{
	// creature:getSummons()
	const auto creature = getSharedPtr<Creature>(L, 1);
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	lua_createtable(L, creature->getSummonCount(), 0);

	int index = 0;
	for (const auto summon : creature->getSummons()) {
		pushSharedPtr(L, summon);
		setCreatureMetatable(L, -1, summon);
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureGetDescription(lua_State* L)
{
	// creature:getDescription(distance)
	int32_t distance = getNumber<int32_t>(L, 2);
	if (const auto creature = getSharedPtr<Creature>(L, 1)) {
		pushString(L, creature->getDescription(distance));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureGetPathTo(lua_State* L)
{
	// creature:getPathTo(pos[, minTargetDist = 0[, maxTargetDist = 1[, fullPathSearch = true[, clearSight = true[, maxSearchDist = 0]]]]])
	const auto creature = getSharedPtr<Creature>(L, 1);
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	const Position& position = getPosition(L, 2);

	FindPathParams fpp;
	fpp.minTargetDist = getNumber<int32_t>(L, 3, 0);
	fpp.maxTargetDist = getNumber<int32_t>(L, 4, 1);
	fpp.fullPathSearch = getBoolean(L, 5, fpp.fullPathSearch);
	fpp.clearSight = getBoolean(L, 6, fpp.clearSight);
	fpp.maxSearchDist = getNumber<int32_t>(L, 7, fpp.maxSearchDist);

	if (std::vector<Direction> dirList; creature->getPathTo(position, dirList, fpp)) {
		lua_newtable(L);

		int index = 0;
		for (auto it = dirList.rbegin(); it != dirList.rend(); ++it) {
			lua_pushinteger(L, *it);
			lua_rawseti(L, -2, ++index);
		}
	} else {
		pushBoolean(L, false);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureMove(lua_State* L)
{
	// creature:move(direction)
	// creature:move(tile[, flags = 0])
	auto creature = getSharedPtr<Creature>(L, 1);
	if (!creature) {
		lua_pushnil(L);
		return 1;
	}

	if (isNumber(L, 2)) {
		const Direction direction = getNumber<Direction>(L, 2);
		if (direction > DIRECTION_LAST) {
			lua_pushnil(L);
			return 1;
		}
		lua_pushinteger(L, g_game.internalMoveCreature(creature, direction, FLAG_NOLIMIT));
	} else {
		const auto tile = getSharedPtr<Tile>(L, 2);
		if (!tile) {
			lua_pushnil(L);
			return 1;
		}
		lua_pushinteger(L, g_game.internalMoveCreature(creature, tile, getNumber<uint32_t>(L, 3)));
	}
	return 1;
}

int LuaScriptInterface::luaCreatureGetZone(lua_State* L)
{
	// creature:getZone()
	if (const auto creature = getSharedPtr<Creature>(L, 1)) {
		lua_pushinteger(L, creature->getZone());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// Todo : Consider hard replacing boolean return values, with userdata
// This would change the style of scripting significantly, and effectively
// break all backwards compatibility, but it might be worth doing, for the "chaining" capabilities.
// Note, this may mean not getting to use const& as much, not sure.
// I think I can return userdata for true, and false for false...

int LuaScriptInterface::luaCreatureGiveCustomSkill(lua_State* L)
{
	// creature:giveCustomSkill("skillname", make_new = true)
	// creature:giveCustomSkill("skillname", CustomSkill)
	// returns bool
	
	auto make_new = (isBoolean(L, 3)) ? getBoolean(L, 3) : true;

	if (auto& creature = getSharedPtr<Creature>(L, 1)) 
	{
		if (isString(L, 2) and isUserdata(L, 3))
		{
			auto skill_name = getString(L, 2);
			if (auto& skill = getSharedPtr<CustomSkill>(L, 3))
			{
				pushBoolean(L, creature->giveCustomSkill(skill_name, std::move(skill)));
			}
		}
		else if (isString(L, 2) and make_new)
		{
			auto skill_name = getString(L, 2);
			if (const auto& player = creature->getPlayer())
			{
				if (auto skill = Vocations::getVocationSkill(skill_name, player->getVocation()->getId()))
				{
					// Todo : create creature methods which accept the optional<CustomSkill>
					if (skill.has_value())
					{
						auto new_skill = Components::Skills::CustomSkill::make_skill(skill.value());
						pushBoolean(L, player->giveCustomSkill(skill_name, std::move(new_skill)));
						return 1;
					}
				}

				// Skill name not found but "make_new" is true
				auto new_skill = Components::Skills::CustomSkill::make_skill();
				pushBoolean(L, player->giveCustomSkill(skill_name, std::move(new_skill)));
				return 1;
			}
			else if (const auto& npc = creature->getNpc())
			{
				if (auto skill = Npcs::getNpcSkill(skill_name, npc->getName()))
				{
					// Todo : create creature methods which accept the optional<std::shared_ptr<CustomSkill>>
					if (skill.has_value())
					{
						auto new_skill = Components::Skills::CustomSkill::make_skill(skill.value());
						pushBoolean(L, npc->giveCustomSkill(skill_name, std::move(new_skill)));
						return 1;
					}
				}
				// Skill name not found but "make_new" is true
				auto new_skill = Components::Skills::CustomSkill::make_skill();
				pushBoolean(L, player->giveCustomSkill(skill_name, std::move(new_skill)));
				return 1;
			}
			else if (const auto& monster = creature->getMonster())
			{
				if (auto skill = Monsters::getMonsterSkill(skill_name, monster->getName()))
				{
					// Todo : create creature methods which accept the optional<std::shared_ptr<CustomSkill>>
					if (skill.has_value())
					{
						auto new_skill = Components::Skills::CustomSkill::make_skill(skill.value());
						pushBoolean(L, monster->giveCustomSkill(skill_name, std::move(new_skill)));
						return 1;
					}
				}
				// Skill name not found but "make_new" is true
				auto new_skill = Components::Skills::CustomSkill::make_skill();
				pushBoolean(L, player->giveCustomSkill(skill_name, std::move(new_skill)));
				return 1;
			}
			// creature isn't known type, error. Log place
			lua_pushnil(L);
			return 1;
		}
		else
		{
			// first param is not a string, something is wrong, log it.
			lua_pushnil(L);
			return 1;
		}
	}
	else 
	{
		// another log location
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureAddCustomSkill(lua_State* L)
{
	// creature:addSkill("SkillName")
	// returns bool
	if (auto& creature = getSharedPtr<Creature>(L, 1))
	{
		if (isString(L, 2) and isNumber(L, 3))
		{
			auto skill_name = getString(L, 2);
			auto levels_to_add = getNumber<uint16_t>(L, 3);

			if (auto skill = creature->getCustomSkill(skill_name))
			{
				pushBoolean(L, skill->addLevels(levels_to_add));
				return 1;
			}
			else
			{
				// creature doesn't have that skill
				// possible log?
				pushBoolean(L, false);
				return 1;
			}
		}
		else if (not isString(L, 2))
		{
			// todo send console error
			// log location
			pushBoolean(L, false);
			return 1;
		}
		else if (not isNumber(L, 3))
		{
			// todo send console error
			// log location
			pushBoolean(L, false);
			return 1;
		}
		else
		{
			// this should for sure be unreachable, log here
			pushBoolean(L, false);
		}

	}
	return 1;
}

int LuaScriptInterface::luaCreatureSubtractCustomSkill(lua_State* L)
{
	// creature:subtractSkill("SkillName", levels)
	// returns bool
	if (auto& creature = getSharedPtr<Creature>(L, 1))
	{
		if (isString(L, 2) and isNumber(L, 3))
		{
			auto skill_name = getString(L, 2);
			auto levels_to_remove = getNumber<uint16_t>(L, 3);

			if (auto skill = creature->getCustomSkill(skill_name))
			{
				pushBoolean(L, skill->removeLevels(levels_to_remove));
				return 1;
			}
			else
			{
				// creature doesn't have that skill
				// possible log?
				pushBoolean(L, false);
				return 1;
			}
		}
		else if (not isString(L, 2))
		{
			// todo send console error
			// log location
			pushBoolean(L, false);
			return 1;
		}
		else if (not isNumber(L, 3))
		{
			// todo send console error
			// log location
			pushBoolean(L, false);
			return 1;
		}
		else
		{
			// this should for sure be unreachable, log here
			pushBoolean(L, false);
		}
	}
	return 1;
}

int LuaScriptInterface::luaCreatureAddBonusCustomSkill(lua_State* L)
{
	// creature:addBonusSkill("SkillName")
	// returns bool
	if (auto& creature = getSharedPtr<Creature>(L, 1))
	{
		if (isString(L, 2) and isNumber(L, 3))
		{
			auto skill_name = getString(L, 2);
			auto levels_to_add = getNumber<uint16_t>(L, 3);

			if (auto skill = creature->getCustomSkill(skill_name))
			{
				pushBoolean(L, skill->addBonusLevels(levels_to_add));
				return 1;
			}
			else
			{
				// creature doesn't have that skill
				// possible log?
				pushBoolean(L, false);
				return 1;
			}
		}
		else if (not isString(L, 2))
		{
			// todo send console error
			// log location
			pushBoolean(L, false);
			return 1;
		}
		else if (not isNumber(L, 3))
		{
			// todo send console error
			// log location
			pushBoolean(L, false);
			return 1;
		}
		else
		{
			// this should for sure be unreachable, log here
			pushBoolean(L, false);
		}

	}
	return 1;
}

int LuaScriptInterface::luaCreatureSubtractBonusSkill(lua_State* L)
{
	// creature:subtractBonusSkill("SkillName", levels)
	// returns bool
	if (auto& creature = getSharedPtr<Creature>(L, 1))
	{
		if (isString(L, 2) and isNumber(L, 3))
		{
			auto skill_name = getString(L, 2);
			auto levels_to_remove = getNumber<uint16_t>(L, 3);

			if (auto skill = creature->getCustomSkill(skill_name))
			{
				pushBoolean(L, skill->removeBonusLevels(levels_to_remove));
				return 1;
			}
			else
			{
				// creature doesn't have that skill
				// possible log?
				pushBoolean(L, false);
				return 1;
			}
		}
		else if (not isString(L, 2))
		{
			// todo send console error
			// log location
			pushBoolean(L, false);
			return 1;
		}
		else if (not isNumber(L, 3))
		{
			// todo send console error
			// log location
			pushBoolean(L, false);
			return 1;
		}
		else
		{
			// this should for sure be unreachable, log here
			pushBoolean(L, false);
		}
	}
	return 1;
}

int LuaScriptInterface::luaCreatureClearBonusSkill(lua_State* L)
{
	// creature:clearBonusSkill("SkillName")
	// returns bool
	if (auto& creature = getSharedPtr<Creature>(L, 1))
	{
		if (isString(L, 2))
		{
			auto skill_name = getString(L, 2);

			if (auto skill = creature->getCustomSkill(skill_name))
			{
				skill->setBonus(0);
				pushBoolean(L, true);
				return 1;
			}
			else
			{
				// creature doesn't have that skill
				// possible log?
				pushBoolean(L, false);
				return 1;
			}
		}
		else
		{
			// first param is not a string, something is wrong, log it.
			pushBoolean(L, false);
			return 1;
		}
	}
	return 1;
}

int LuaScriptInterface::luaCreatureRemoveCustomSkill(lua_State* L)
{
	// creature:removeSkill("SkillName")
	// returns bool
	if (auto& creature = getSharedPtr<Creature>(L, 1))
	{
		if (isString(L, 2))
		{
			auto skill_name = getString(L, 2);
			pushBoolean(L, creature->removeCustomSkill(skill_name));
			return 1;
		}
		else
		{
			// first param is not a string, something is wrong, log it.
			pushBoolean(L, false);
			return 1;
		}
	}
	return 1;
}


int LuaScriptInterface::luaCreatureHasCustomSkill(lua_State* L)
{
	// creature:hasSkill("SkillName")
	// returns bool
	if (auto& creature = getSharedPtr<Creature>(L, 1))
	{
		if (isString(L, 2))
		{
			auto skill_name = getString(L, 2);
			auto has_skill = (creature->getCustomSkill(skill_name) != nullptr) ? true : false;
			pushBoolean(L, has_skill);
			return 1;
		}
		else
		{
			// first param is not a string, something is wrong, log it.
			pushBoolean(L, false);
			return 1;
		}
	}
	return 1;
}

int LuaScriptInterface::luaCreatureCanGainSkillLevels(lua_State* L)
{
	// creature:canSkill("SkillName")
	// returns bool
	if (auto& creature = getSharedPtr<Creature>(L, 1))
	{
		if (isString(L, 2))
		{
			auto skill_name = getString(L, 2);

			if (auto skill = creature->getCustomSkill(skill_name))
			{
				auto can_skill = (skill->max() > skill->level(false)) ? true : false;
				pushBoolean(L, can_skill);
				return 1;
			}
			else
			{
				// creature doesn't have that skill
				pushBoolean(L, false);
				return 1;
			}
		}
		else
		{
			// first param is not a string, something is wrong, log it.
			pushBoolean(L, false);
			return 1;
		}
	}
	return 1;
}

int LuaScriptInterface::luaCreatureGetCustomSkillLevel(lua_State* L)
{
	// creature:getSkillLevel("SkillName", bonus = true)
	// returns number value
	if (auto& creature = getSharedPtr<Creature>(L, 1))
	{
		if (isString(L, 2))
		{
			auto skill_name = getString(L, 2);
			bool include_bonus = (lua_gettop(L) >=3 and isBoolean(L, 3)) ? getBoolean(L, 3) : true; // default to true

			if (auto skill = creature->getCustomSkill(skill_name))
			{
				lua_pushinteger(L, skill->level(include_bonus));
				return 1;
			}
			else
			{
				// creature doesn't have that skill
				lua_pushinteger(L, 0);
				return 1;
			}
		}
		else
		{
			// first param is not a string, something is wrong, log it.
			lua_pushinteger(L, 0);
			return 1;
		}
	}
	return 1;
}

int LuaScriptInterface::luaCreatureGetCustomSkillUserData(lua_State* L)
{
	// creature:getSkill("SkillName")
	// returns userdata (shared pointer)
	if (auto& creature = getSharedPtr<Creature>(L, 1))
	{
		if (isString(L, 2))
		{
			auto skill_name = getString(L, 2);

			if (auto skill = creature->getCustomSkill(skill_name))
			{
				pushSharedPtr(L, skill);
				std::cout << "We found the creature skill \n";
				setMetatable(L, -1, "Skill");
				return 1;
			}
			else
			{
				// creature doesn't have that skill
				lua_pushnil(L);
				return 1;
			}
		}
		else
		{
			// first param is not a string, something is wrong, log it.
			lua_pushnil(L);
			return 1;
		}
	}
	return 1;
}

// Player
int LuaScriptInterface::luaPlayerCreate(lua_State* L)
{
	// Player(id or guid or name or userdata)
	PlayerPtr player;
	if (isNumber(L, 2)) {
		if (const uint32_t id = getNumber<uint32_t>(L, 2); id >= 0x10000000 && id <= Player::playerAutoID) {
			player = g_game.getPlayerByID(id);
		} else {
			player = g_game.getPlayerByGUID(id);
		}
	} else if (isString(L, 2)) {
		if (const ReturnValue ret = g_game.getPlayerByNameWildcard(getString(L, 2)); ret != RETURNVALUE_NOERROR) {
			lua_pushnil(L);
			lua_pushinteger(L, ret);
			return 2;
		}
		player = g_game.getPlayerByName(getString(L, 2));
	} else if (isUserdata(L, 2)) {
		if (getUserdataType(L, 2) != LuaData_Player) {
			lua_pushnil(L);
			return 1;
		}
		player = getSharedPtr<Player>(L, 2);
	} else {
		player = nullptr;
	}

	if (player) {
		pushSharedPtr(L, player);
		setMetatable(L, -1, "Player");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerDelete(lua_State* L)
{
	if (auto& player = getSharedPtr<Player>(L, 1)) {
		player.reset();
	}
	return 0;
}

int LuaScriptInterface::luaPlayerIsPlayer(lua_State* L)
{
	// player:isPlayer()
	pushBoolean(L, getSharedPtr<const Player>(L, 1) != nullptr);
	return 1;
}

int LuaScriptInterface::luaPlayerGetGuid(lua_State* L)
{
	// player:getGuid()
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		lua_pushinteger(L, player->getGUID());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetIp(lua_State* L)
{
	// player:getIp()
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		lua_pushinteger(L, player->getIP());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetAccountId(lua_State* L)
{
	// player:getAccountId()
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		lua_pushinteger(L, player->getAccount());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetLastLoginSaved(lua_State* L)
{
	// player:getLastLoginSaved()
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		lua_pushinteger(L, player->getLastLoginSaved());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetLastLogout(lua_State* L)
{
	// player:getLastLogout()
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		lua_pushinteger(L, player->getLastLogout());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetAccountType(lua_State* L)
{
	// player:getAccountType()
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		lua_pushinteger(L, player->getAccountType());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerSetAccountType(lua_State* L)
{
	// player:setAccountType(accountType)
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		player->accountType = getNumber<AccountType_t>(L, 2);
		IOLoginData::setAccountType(player->getAccount(), player->accountType);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetCapacity(lua_State* L)
{
	// player:getCapacity()
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		lua_pushinteger(L, player->getCapacity());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerSetCapacity(lua_State* L)
{
	// player:setCapacity(capacity)
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		player->capacity = getNumber<uint32_t>(L, 2);
		player->sendStats();
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetFreeCapacity(lua_State* L)
{
	// player:getFreeCapacity()
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		lua_pushinteger(L, player->getFreeCapacity());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetDepotItemCount(lua_State* L)
{
	// player:getDepotItemCount()
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		lua_pushinteger(L, player->getDepotItemCount());
	}
	else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetDepotChest(lua_State* L)
{
	// player:getDepotChest(depotId[, autoCreate = false])
	const auto player = getSharedPtr<Player>(L, 1);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}

	const uint32_t depotId = getNumber<uint32_t>(L, 2);
	const bool autoCreate = getBoolean(L, 3, true);

	if (const auto depotChest = player->getDepotChest(depotId, autoCreate)) {
		pushSharedPtr(L, depotChest);
		setItemMetatable(L, -1, depotChest);
	} else {
		pushBoolean(L, false);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetInbox(lua_State* L)
{
	// player:getInbox()
	const auto player = getSharedPtr<Player>(L, 1);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}

	if (const auto inbox = player->getInbox()) {
		pushSharedPtr(L, inbox);
		setItemMetatable(L, -1, inbox);
	} else {
		pushBoolean(L, false);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetRewardChest(lua_State* L)
{
	// player:getRewardChest()
	const auto player = getSharedPtr<Player>(L, 1);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}

	if (const auto rewardChest = player->getRewardChest()) {
		pushSharedPtr(L, rewardChest);
		setItemMetatable(L, -1, rewardChest);
	}
	else {
		pushBoolean(L, false);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetSkullTime(lua_State* L)
{
	// player:getSkullTime()
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		lua_pushinteger(L, player->getSkullTicks());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerSetSkullTime(lua_State* L)
{
	// player:setSkullTime(skullTime)
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		player->setSkullTicks(getNumber<int64_t>(L, 2));
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetDeathPenalty(lua_State* L)
{
	// player:getDeathPenalty()
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		lua_pushnumber(L, player->getLostPercent() * 100);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetExperience(lua_State* L)
{
	// player:getExperience()
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		lua_pushinteger(L, player->getExperience());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerAddExperience(lua_State* L)
{
	// player:addExperience(experience[, sendText = false])
	const auto player = getSharedPtr<Player>(L, 1);
	if (player) {
		const uint64_t experience = getNumber<uint64_t>(L, 2);
		const bool sendText = getBoolean(L, 3, false);
		player->addExperience(nullptr, experience, sendText);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerRemoveExperience(lua_State* L)
{
	// player:removeExperience(experience[, sendText = false])
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		const uint64_t experience = getNumber<uint64_t>(L, 2);
		const bool sendText = getBoolean(L, 3, false);
		player->removeExperience(experience, sendText);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetLevel(lua_State* L)
{
	// player:getLevel()
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		lua_pushinteger(L, player->getLevel());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetMagicLevel(lua_State* L)
{
	// player:getMagicLevel()
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		lua_pushinteger(L, player->getMagicLevel());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetBaseMagicLevel(lua_State* L)
{
	// player:getBaseMagicLevel()
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		lua_pushinteger(L, player->getBaseMagicLevel());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetMana(lua_State* L)
{
	// player:getMana()
	if (const auto player = getSharedPtr<const Player>(L, 1)) {
		lua_pushinteger(L, player->getMana());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerAddMana(lua_State* L)
{
	// player:addMana(manaChange[, animationOnLoss = false])
	const auto player = getSharedPtr<Player>(L, 1);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}

	const int32_t manaChange = getNumber<int32_t>(L, 2);
	const bool animationOnLoss = getBoolean(L, 3, false);
	if (!animationOnLoss && manaChange < 0) {
		player->changeMana(manaChange);
	} else {
		CombatDamage damage;
		damage.primary.value = manaChange;
		damage.origin = ORIGIN_NONE;
		g_game.combatChangeMana(nullptr, player, damage);
	}
	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaPlayerGetMaxMana(lua_State* L)
{
	// player:getMaxMana()
	if (const auto player = getSharedPtr<const Player>(L, 1)) {
		lua_pushinteger(L, player->getMaxMana());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerSetMaxMana(lua_State* L)
{
	// player:setMaxMana(maxMana)
	if (const auto player = getPlayer(L, 1)) {
		player->manaMax = getNumber<int32_t>(L, 2);
		player->mana = std::min<int32_t>(player->mana, player->manaMax);
		player->sendStats();
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetManaSpent(lua_State* L)
{
	// player:getManaSpent()
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		lua_pushinteger(L, player->getSpentMana());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerAddManaSpent(lua_State* L)
{
	// player:addManaSpent(amount)
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		player->addManaSpent(getNumber<uint64_t>(L, 2));
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerRemoveManaSpent(lua_State* L)
{
	// player:removeManaSpent(amount[, notify = true])
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		player->removeManaSpent(getNumber<uint64_t>(L, 2), getBoolean(L, 3, true));
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetBaseMaxHealth(lua_State* L)
{
	// player:getBaseMaxHealth()
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		lua_pushinteger(L, player->healthMax);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetBaseMaxMana(lua_State* L)
{
	// player:getBaseMaxMana()
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		lua_pushinteger(L, player->manaMax);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetSkillLevel(lua_State* L)
{
	// player:getSkillLevel(skillType)
	const auto skillType = getNumber<skills_t>(L, 2);
	const auto player = getSharedPtr<Player>(L, 1);
	if (player && skillType <= SKILL_LAST) {
		lua_pushinteger(L, player->skills[skillType].level);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetEffectiveSkillLevel(lua_State* L)
{
	// player:getEffectiveSkillLevel(skillType)
	const auto skillType = getNumber<skills_t>(L, 2);
	const auto player = getSharedPtr<Player>(L, 1);
	if (player && skillType <= SKILL_LAST) {
		lua_pushinteger(L, player->getSkillLevel(skillType));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetSkillPercent(lua_State* L)
{
	// player:getSkillPercent(skillType)
	const auto skillType = getNumber<skills_t>(L, 2);
	const auto player = getSharedPtr<Player>(L, 1);
	if (player && skillType <= SKILL_LAST) {
		lua_pushinteger(L, player->skills[skillType].percent);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetSkillTries(lua_State* L)
{
	// player:getSkillTries(skillType)
	const auto skillType = getNumber<skills_t>(L, 2);
	const auto player = getSharedPtr<Player>(L, 1);
	if (player && skillType <= SKILL_LAST) {
		lua_pushinteger(L, player->skills[skillType].tries);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerAddSkillTries(lua_State* L)
{
	// player:addSkillTries(skillType, tries)
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		const auto skillType = getNumber<skills_t>(L, 2);
		const auto tries = getNumber<uint64_t>(L, 3);
		player->addSkillAdvance(skillType, tries);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerRemoveSkillTries(lua_State* L)
{
	// player:removeSkillTries(skillType, tries[, notify = true])
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		const auto skillType = getNumber<skills_t>(L, 2);
		const uint64_t tries = getNumber<uint64_t>(L, 3);
		player->removeSkillTries(skillType, tries, getBoolean(L, 4, true));
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetSpecialSkill(lua_State* L)
{
	// player:getSpecialSkill(specialSkillType)
	const auto specialSkillType = getNumber<SpecialSkills_t>(L, 2);
	const auto player = getSharedPtr<Player>(L, 1);
	if (player && specialSkillType <= SPECIALSKILL_LAST) {
		lua_pushinteger(L, player->getSpecialSkill(specialSkillType));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerAddSpecialSkill(lua_State* L)
{
	// player:addSpecialSkill(specialSkillType, value)
	const auto player = getSharedPtr<Player>(L, 1);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}

	const auto specialSkillType = getNumber<SpecialSkills_t>(L, 2);
	if (specialSkillType > SPECIALSKILL_LAST) {
		lua_pushnil(L);
		return 1;
	}

	player->setVarSpecialSkill(specialSkillType, getNumber<int32_t>(L, 3));
	player->sendSkills();
	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaPlayerAddOfflineTrainingTime(lua_State* L)
{
	// player:addOfflineTrainingTime(time)
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		const int32_t time = getNumber<int32_t>(L, 2);
		player->addOfflineTrainingTime(time);
		player->sendStats();
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetOfflineTrainingTime(lua_State* L)
{
	// player:getOfflineTrainingTime()
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		lua_pushinteger(L, player->getOfflineTrainingTime());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerRemoveOfflineTrainingTime(lua_State* L)
{
	// player:removeOfflineTrainingTime(time)
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		const int32_t time = getNumber<int32_t>(L, 2);
		player->removeOfflineTrainingTime(time);
		player->sendStats();
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerAddOfflineTrainingTries(lua_State* L)
{
	// player:addOfflineTrainingTries(skillType, tries)
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		const auto skillType = getNumber<skills_t>(L, 2);
		const uint64_t tries = getNumber<uint64_t>(L, 3);
		pushBoolean(L, player->addOfflineTrainingTries(skillType, tries));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetOfflineTrainingSkill(lua_State* L)
{
	// player:getOfflineTrainingSkill()
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		lua_pushinteger(L, player->getOfflineTrainingSkill());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerSetOfflineTrainingSkill(lua_State* L)
{
	// player:setOfflineTrainingSkill(skillId)
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		const int32_t skillId = getNumber<int32_t>(L, 2);
		player->setOfflineTrainingSkill(skillId);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetItemCount(lua_State* L)
{
	// player:getItemCount(itemId[, subType = -1])
	const auto player = getSharedPtr<Player>(L, 1);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}

	uint16_t itemId;
	if (isNumber(L, 2)) {
		itemId = getNumber<uint16_t>(L, 2);
	} else {
		itemId = Item::items.getItemIdByName(getString(L, 2));
		if (itemId == 0) {
			lua_pushnil(L);
			return 1;
		}
	}

	int32_t subType = getNumber<int32_t>(L, 3, -1);
	lua_pushinteger(L, player->getItemTypeCount(itemId, subType));
	return 1;
}

int LuaScriptInterface::luaPlayerGetItemById(lua_State* L)
{
	// player:getItemById(itemId, deepSearch[, subType = -1])
	const auto player = getSharedPtr<Player>(L, 1);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}

	uint16_t itemId;
	if (isNumber(L, 2)) {
		itemId = getNumber<uint16_t>(L, 2);
	} else {
		itemId = Item::items.getItemIdByName(getString(L, 2));
		if (itemId == 0) {
			lua_pushnil(L);
			return 1;
		}
	}
	const bool deepSearch = getBoolean(L, 3);
	const int32_t subType = getNumber<int32_t>(L, 4, -1);

	if (const auto item = g_game.findItemOfType(player, itemId, deepSearch, subType)) {
		pushSharedPtr(L, item);
		setItemMetatable(L, -1, item);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetVocation(lua_State* L)
{
	// player:getVocation()
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		pushUserdata<Vocation>(L, player->getVocation());
		setMetatable(L, -1, "Vocation");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerSetVocation(lua_State* L)
{
	// player:setVocation(id or name or userdata)
	const auto player = getSharedPtr<Player>(L, 1);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}

	Vocation* vocation;
	if (isNumber(L, 2)) {
		vocation = g_vocations.getVocation(getNumber<uint16_t>(L, 2));
	} else if (isString(L, 2)) {
		vocation = g_vocations.getVocation(g_vocations.getVocationId(getString(L, 2)));
	} else if (isUserdata(L, 2)) {
		vocation = getUserdata<Vocation>(L, 2);
	} else {
		vocation = nullptr;
	}

	if (!vocation) {
		pushBoolean(L, false);
		return 1;
	}

	player->setVocation(vocation->getId());
	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaPlayerGetSex(lua_State* L)
{
	// player:getSex()
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		lua_pushinteger(L, player->getSex());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerSetSex(lua_State* L)
{
	// player:setSex(newSex)
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		const PlayerSex_t newSex = getNumber<PlayerSex_t>(L, 2);
		player->setSex(newSex);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetTown(lua_State* L)
{
	// player:getTown()
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		pushUserdata<Town>(L, player->getTown());
		setMetatable(L, -1, "Town");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerSetTown(lua_State* L)
{
	// player:setTown(town)
	const auto town = getUserdata<Town>(L, 2);
	if (!town) {
		pushBoolean(L, false);
		return 1;
	}

	if (const auto player = getSharedPtr<Player>(L, 1)) {
		player->setTown(town);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetGuild(lua_State* L)
{
	// player:getGuild()
	const auto player = getSharedPtr<Player>(L, 1);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}

	const auto guild = player->getGuild();
	if (!guild) {
		lua_pushnil(L);
		return 1;
	}

	pushSharedPtr(L, guild);
	setMetatable(L, -1, "Guild");
	return 1;
}

int LuaScriptInterface::luaPlayerSetGuild(lua_State* L)
{
	// player:setGuild(guild)
	const auto player = getSharedPtr<Player>(L, 1);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}

	player->setGuild(getSharedPtr<Guild>(L, 2));
	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaPlayerGetGuildLevel(lua_State* L)
{
	// player:getGuildLevel()
	if (const auto player = getSharedPtr<Player>(L, 1); player && player->getGuild()) {
		lua_pushinteger(L, player->getGuildRank()->level);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerSetGuildLevel(lua_State* L)
{
	// player:setGuildLevel(level)
	const auto level = getNumber<uint8_t>(L, 2);
	const auto player = getSharedPtr<Player>(L, 1);
	if (!player || !player->getGuild()) {
		lua_pushnil(L);
		return 1;
	}

	const auto rank = player->getGuild()->getRankByLevel(level);
	if (!rank) {
		pushBoolean(L, false);
	} else {
		player->setGuildRank(rank);
		pushBoolean(L, true);
	}

	return 1;
}

int LuaScriptInterface::luaPlayerGetGuildNick(lua_State* L)
{
	// player:getGuildNick()
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		pushString(L, player->getGuildNick());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerSetGuildNick(lua_State* L)
{
	// player:setGuildNick(nick)
	const std::string& nick = getString(L, 2);
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		player->setGuildNick(nick);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetGroup(lua_State* L)
{
	// player:getGroup()
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		pushUserdata<Group>(L, player->getGroup());
		setMetatable(L, -1, "Group");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerSetGroup(lua_State* L)
{
	// player:setGroup(group)
	Group* group = getUserdata<Group>(L, 2);
	if (!group) {
		pushBoolean(L, false);
		return 1;
	}

	if (const auto player = getSharedPtr<Player>(L, 1)) {
		player->setGroup(group);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetStamina(lua_State* L)
{
	// player:getStamina()
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		lua_pushinteger(L, player->getStaminaMinutes());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerSetStamina(lua_State* L)
{
	// player:setStamina(stamina)
	const uint16_t stamina = getNumber<uint16_t>(L, 2);
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		player->staminaMinutes = std::min<uint16_t>(2520, stamina);
		player->sendStats();
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetSoul(lua_State* L)
{
	// player:getSoul()
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		lua_pushinteger(L, player->getSoul());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerAddSoul(lua_State* L)
{
	// player:addSoul(soulChange)
	int32_t soulChange = getNumber<int32_t>(L, 2);
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		player->changeSoul(soulChange);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetMaxSoul(lua_State* L)
{
	// player:getMaxSoul()
	if (const auto player = getSharedPtr<Player>(L, 1); player && player->vocation) {
		lua_pushinteger(L, player->vocation->getSoulMax());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetBankBalance(lua_State* L)
{
	// player:getBankBalance()
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		lua_pushinteger(L, player->getBankBalance());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerSetBankBalance(lua_State* L)
{
	// player:setBankBalance(bankBalance)
	const auto player = getSharedPtr<Player>(L, 1);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}

	int64_t balance = getNumber<int64_t>(L, 2);
	if (balance < 0) {
		reportErrorFunc(L, "Invalid bank balance value.");
		lua_pushnil(L);
		return 1;
	}

	player->setBankBalance(balance);
	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaPlayerGetStorageValue(lua_State* L)
{
	// player:getStorageValue(key)
	const auto player = getSharedPtr<Player>(L, 1);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}

	const uint32_t key = getNumber<uint32_t>(L, 2);
	if (int32_t value; player->getStorageValue(key, value)) {
		lua_pushinteger(L, value);
	} else {
		lua_pushinteger(L, -1);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerSetStorageValue(lua_State* L)
{
	// player:setStorageValue(key, value)
	const int32_t value = getNumber<int32_t>(L, 3);
	const uint32_t key = getNumber<uint32_t>(L, 2);
	const auto player = getSharedPtr<Player>(L, 1);
	if (IS_IN_KEYRANGE(key, RESERVED_RANGE)) {
		reportErrorFunc(L, fmt::format("Accessing reserved range: {:d}", key));
		pushBoolean(L, false);
		return 1;
	}

	if (player) {
		player->addStorageValue(key, value);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerAddItem(lua_State* L)
{
	// player:addItem(itemId[, count = 1[, canDropOnMap = true[, subType = 1[, slot = CONST_SLOT_WHEREEVER]]]])
	const auto player = getSharedPtr<Player>(L, 1);
	if (!player) {
		pushBoolean(L, false);
		return 1;
	}

	uint16_t itemId;
	if (isNumber(L, 2)) {
		itemId = getNumber<uint16_t>(L, 2);
	} else {
		itemId = Item::items.getItemIdByName(getString(L, 2));
		if (itemId == 0) {
			lua_pushnil(L);
			return 1;
		}
	}

	const int32_t count = getNumber<int32_t>(L, 3, 1);
	int32_t subType = getNumber<int32_t>(L, 5, 1);

	const ItemType& it = Item::items[itemId];

	int32_t itemCount = 1;
	int parameters = lua_gettop(L);
	if (parameters >= 5) {
		itemCount = std::max<int32_t>(1, count);
	} else if (it.hasSubType()) {
		if (it.stackable) {
			itemCount = std::ceil(count / 100.f);
		}

		subType = count;
	} else {
		itemCount = std::max<int32_t>(1, count);
	}

	const bool hasTable = itemCount > 1;
	if (hasTable) {
		lua_newtable(L);
	} else if (itemCount == 0) {
		lua_pushnil(L);
		return 1;
	}

	bool canDropOnMap = getBoolean(L, 4, true);
	const auto slot = getNumber<slots_t>(L, 6, CONST_SLOT_WHEREEVER);
	for (int32_t i = 1; i <= itemCount; ++i) {
		int32_t stackCount = subType;
		if (it.stackable) {
			stackCount = std::min<int32_t>(stackCount, 100);
			subType -= stackCount;
		}

		auto item = Item::CreateItem(itemId, stackCount);
		if (!item) {
			if (!hasTable) {
				lua_pushnil(L);
			}
			return 1;
		}

		ReturnValue ret = g_game.internalPlayerAddItem(player, item, canDropOnMap, slot);
		if (ret != RETURNVALUE_NOERROR) {
			item.reset();
			if (!hasTable) {
				lua_pushnil(L);
			}
			return 1;
		}

		if (hasTable) {
			lua_pushinteger(L, i);
			pushSharedPtr(L, item);
			setItemMetatable(L, -1, item);
			lua_settable(L, -3);
		} else {
			pushSharedPtr(L, item);
			setItemMetatable(L, -1, item);
		}
	}
	return 1;
}

int LuaScriptInterface::luaPlayerAddItemEx(lua_State* L)
{
	// player:addItemEx(item[, canDropOnMap = false[, index = INDEX_WHEREEVER[, flags = 0]]])
	// player:addItemEx(item[, canDropOnMap = true[, slot = CONST_SLOT_WHEREEVER]])
	const auto item = getSharedPtr<Item>(L, 2);
	if (!item) {
		reportErrorFunc(L, getErrorDesc(LUA_ERROR_ITEM_NOT_FOUND));
		pushBoolean(L, false);
		return 1;
	}

	const auto player = getSharedPtr<Player>(L, 1);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}

	if (item->getParent() != VirtualCylinder::virtualCylinder) {
		reportErrorFunc(L, "Item already has a parent");
		pushBoolean(L, false);
		return 1;
	}

	bool canDropOnMap = getBoolean(L, 3, false);
	ReturnValue returnValue;
	if (canDropOnMap) {
		const auto slot = getNumber<slots_t>(L, 4, CONST_SLOT_WHEREEVER);
		returnValue = g_game.internalPlayerAddItem(player, item, true, slot);
	} else {
		int32_t index = getNumber<int32_t>(L, 4, INDEX_WHEREEVER);
		uint32_t flags = getNumber<uint32_t>(L, 5, 0);
		CylinderPtr holder = player;
		returnValue = g_game.internalAddItem(holder, item, index, flags);
	}

	if (returnValue == RETURNVALUE_NOERROR) {
		ScriptEnvironment::removeTempItem(item);
	}
	lua_pushinteger(L, returnValue);
	return 1;
}

int LuaScriptInterface::luaPlayerRemoveItem(lua_State* L)
{
	// player:removeItem(itemId, count[, subType = -1[, ignoreEquipped = false]])
	const auto player = getSharedPtr<Player>(L, 1);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}

	uint16_t itemId;
	if (isNumber(L, 2)) {
		itemId = getNumber<uint16_t>(L, 2);
	} else {
		itemId = Item::items.getItemIdByName(getString(L, 2));
		if (itemId == 0) {
			lua_pushnil(L);
			return 1;
		}
	}

	const uint32_t count = getNumber<uint32_t>(L, 3);
	const int32_t subType = getNumber<int32_t>(L, 4, -1);
	const bool ignoreEquipped = getBoolean(L, 5, false);
	pushBoolean(L, player->removeItemOfType(itemId, count, subType, ignoreEquipped));
	return 1;
}

int LuaScriptInterface::luaPlayerGetMoney(lua_State* L)
{
	// player:getMoney()
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		lua_pushinteger(L, player->getMoney());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerAddMoney(lua_State* L)
{
	// player:addMoney(money)
	uint64_t money = getNumber<uint64_t>(L, 2);
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		CylinderPtr holder = player;
		g_game.addMoney(holder, money);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerRemoveMoney(lua_State* L)
{
	// player:removeMoney(money)
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		const uint64_t money = getNumber<uint64_t>(L, 2);
		CylinderPtr holder = player;
		pushBoolean(L, g_game.removeMoney(holder, money));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerShowTextDialog(lua_State* L)
{
	// player:showTextDialog(id or name or userdata[, text[, canWrite[, length]]])
	const auto player = getSharedPtr<Player>(L, 1);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}

	int32_t length = getNumber<int32_t>(L, 5, -1);
	bool canWrite = getBoolean(L, 4, false);
	std::string text;

	int parameters = lua_gettop(L);
	if (parameters >= 3) {
		text = getString(L, 3);
	}

	ItemPtr item;
	if (isNumber(L, 2)) {
		item = Item::CreateItem(getNumber<uint16_t>(L, 2));
	} else if (isString(L, 2)) {
		item = Item::CreateItem(Item::items.getItemIdByName(getString(L, 2)));
	} else if (isUserdata(L, 2)) {
		if (getUserdataType(L, 2) != LuaData_Item) {
			pushBoolean(L, false);
			return 1;
		}

		item = getSharedPtr<Item>(L, 2);
	} else {
		item = nullptr;
	}

	if (!item) {
		reportErrorFunc(L, getErrorDesc(LUA_ERROR_ITEM_NOT_FOUND));
		pushBoolean(L, false);
		return 1;
	}

	if (length < 0) {
		length = Item::items[item->getID()].maxTextLen;
	}

	if (!text.empty()) {
		item->setText(text);
		length = std::max<int32_t>(text.size(), length);
	}
	item->setParent(player);
	player->windowTextId++;
	player->writeItem = item;
	player->maxWriteLen = length;
	player->sendTextWindow(item, length, canWrite);
	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaPlayerSendTextMessage(lua_State* L)
{
	// player:sendTextMessage(type, text[, position, primaryValue = 0, primaryColor = TEXTCOLOR_NONE[, secondaryValue = 0, secondaryColor = TEXTCOLOR_NONE]])
	// player:sendTextMessage(type, text, channelId)

	const auto player = getSharedPtr<Player>(L, 1);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}

	int parameters = lua_gettop(L);

	TextMessage message(getNumber<MessageClasses>(L, 2), getString(L, 3));
	if (parameters == 4) {
		const uint16_t channelId = getNumber<uint16_t>(L, 4);
		const auto channel = g_chat->getChannel(player, channelId);
		if (!channel || !channel->hasUser(player)) {
			pushBoolean(L, false);
			return 1;
		}
		message.channelId = channelId;
	} else {
		if (parameters >= 6) {
			message.position = getPosition(L, 4);
			message.primary.value = getNumber<int32_t>(L, 5);
			message.primary.color = getNumber<TextColor_t>(L, 6);
		}

		if (parameters >= 8) {
			message.secondary.value = getNumber<int32_t>(L, 7);
			message.secondary.color = getNumber<TextColor_t>(L, 8);
		}
	}

	player->sendTextMessage(message);
	pushBoolean(L, true);

	return 1;
}

int LuaScriptInterface::luaPlayerSendChannelMessage(lua_State* L)
{
	// player:sendChannelMessage(author, text, type, channelId)
	const auto player = getSharedPtr<Player>(L, 1);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}

	const uint16_t channelId = getNumber<uint16_t>(L, 5);
	const auto type = getNumber<SpeakClasses>(L, 4);
	const std::string& text = getString(L, 3);
	const std::string& author = getString(L, 2);
	player->sendChannelMessage(author, text, type, channelId);
	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaPlayerSendPrivateMessage(lua_State* L)
{
	// player:sendPrivateMessage(speaker, text[, type])
	const auto player = getSharedPtr<Player>(L, 1);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}

	const auto speaker = getSharedPtr<const Player>(L, 2);
	const std::string& text = getString(L, 3);
	const auto type = getNumber<SpeakClasses>(L, 4, TALKTYPE_PRIVATE_FROM);
	player->sendPrivateMessage(speaker, type, text);
	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaPlayerChannelSay(lua_State* L)
{
	// player:channelSay(speaker, type, text, channelId)
	const auto player = getSharedPtr<Player>(L, 1);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}

	const auto speaker = getCreature(L, 2);
	const auto type = getNumber<SpeakClasses>(L, 3);
	const std::string& text = getString(L, 4);
	const uint16_t channelId = getNumber<uint16_t>(L, 5);
	player->sendToChannel(speaker, type, text, channelId);
	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaPlayerOpenChannel(lua_State* L)
{
	// player:openChannel(channelId)
	uint16_t channelId = getNumber<uint16_t>(L, 2);
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		g_game.playerOpenChannel(player->getID(), channelId);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetSlotItem(lua_State* L)
{
	// player:getSlotItem(slot)
	auto player = getSharedPtr<Player>(L, 1);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}

	const uint32_t slot = getNumber<uint32_t>(L, 2);
	auto thing = player->getThing(slot);
	if (!thing) {
		lua_pushnil(L);
		return 1;
	}

	if (const auto item = thing->getItem()) {
		pushSharedPtr(L, item);
		setItemMetatable(L, -1, item);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetParty(lua_State* L)
{
	// player:getParty()
	const auto player = getSharedPtr<const Player>(L, 1);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}

	if (const auto party = player->getParty()) {
		pushUserdata<Party>(L, party);
		setMetatable(L, -1, "Party");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerAddOutfit(lua_State* L)
{
	// player:addOutfit(lookType)
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		player->addOutfit(getNumber<uint16_t>(L, 2), 0);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerAddOutfitAddon(lua_State* L)
{
	// player:addOutfitAddon(lookType, addon)
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		const uint16_t lookType = getNumber<uint16_t>(L, 2);
		const uint8_t addon = getNumber<uint8_t>(L, 3);
		player->addOutfit(lookType, addon);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerRemoveOutfit(lua_State* L)
{
	// player:removeOutfit(lookType)
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		const uint16_t lookType = getNumber<uint16_t>(L, 2);
		pushBoolean(L, player->removeOutfit(lookType));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerRemoveOutfitAddon(lua_State* L)
{
	// player:removeOutfitAddon(lookType, addon)
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		const uint16_t lookType = getNumber<uint16_t>(L, 2);
		const uint8_t addon = getNumber<uint8_t>(L, 3);
		pushBoolean(L, player->removeOutfitAddon(lookType, addon));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerHasOutfit(lua_State* L)
{
	// player:hasOutfit(lookType[, addon = 0])
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		const uint16_t lookType = getNumber<uint16_t>(L, 2);
		const uint8_t addon = getNumber<uint8_t>(L, 3, 0);
		pushBoolean(L, player->hasOutfit(lookType, addon));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerCanWearOutfit(lua_State* L)
{
	// player:canWearOutfit(lookType[, addon = 0])
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		const uint16_t lookType = getNumber<uint16_t>(L, 2);
		const uint8_t addon = getNumber<uint8_t>(L, 3, 0);
		pushBoolean(L, player->canWear(lookType, addon));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerSendOutfitWindow(lua_State* L)
{
	// player:sendOutfitWindow()
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		player->sendOutfitWindow();
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerAddMount(lua_State* L) {
	// player:addMount(mountId or mountName)
	const auto player = getSharedPtr<Player>(L, 1);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}

	uint8_t mountId;
	if (isNumber(L, 2)) {
		mountId = getNumber<uint8_t>(L, 2);
	} else {
		const auto mount = g_game.mounts.getMountByName(getString(L, 2));
		if (!mount) {
			lua_pushnil(L);
			return 1;
		}
		mountId = mount->id;
	}
	pushBoolean(L, player->tameMount(mountId));
	return 1;
}

int LuaScriptInterface::luaPlayerRemoveMount(lua_State* L) {
	// player:removeMount(mountId or mountName)
	const auto player = getSharedPtr<Player>(L, 1);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}

	uint8_t mountId;
	if (isNumber(L, 2)) {
		mountId = getNumber<uint8_t>(L, 2);
	} else {
		const auto mount = g_game.mounts.getMountByName(getString(L, 2));
		if (!mount) {
			lua_pushnil(L);
			return 1;
		}
		mountId = mount->id;
	}
	pushBoolean(L, player->untameMount(mountId));
	return 1;
}

int LuaScriptInterface::luaPlayerHasMount(lua_State* L) {
	// player:hasMount(mountId or mountName)
	const auto player = getSharedPtr<const Player>(L, 1);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}

	Mount* mount = nullptr;
	if (isNumber(L, 2)) {
		mount = g_game.mounts.getMountByID(getNumber<uint8_t>(L, 2));
	} else {
		mount = g_game.mounts.getMountByName(getString(L, 2));
	}

	if (mount) {
		pushBoolean(L, player->hasMount(mount));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetPremiumEndsAt(lua_State* L)
{
	// player:getPremiumEndsAt()
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		lua_pushinteger(L, player->premiumEndsAt);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerSetPremiumEndsAt(lua_State* L)
{
	// player:setPremiumEndsAt(timestamp)
	const auto player = getSharedPtr<Player>(L, 1);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}

	const time_t timestamp = getNumber<time_t>(L, 2);

	player->setPremiumTime(timestamp);
	IOLoginData::updatePremiumTime(player->getAccount(), timestamp);
	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaPlayerHasBlessing(lua_State* L)
{
	// player:hasBlessing(blessing)
	const uint8_t blessing = getNumber<uint8_t>(L, 2) - 1;
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		pushBoolean(L, player->hasBlessing(blessing));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerAddBlessing(lua_State* L)
{
	// player:addBlessing(blessing)
	const auto player = getSharedPtr<Player>(L, 1);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}

	const auto blessing = getNumber<uint8_t>(L, 2) - 1;
	if (player->hasBlessing(blessing)) {
		pushBoolean(L, false);
		return 1;
	}

	player->addBlessing(blessing);
	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaPlayerRemoveBlessing(lua_State* L)
{
	// player:removeBlessing(blessing)
	const auto player = getSharedPtr<Player>(L, 1);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}

	const auto blessing = getNumber<uint8_t>(L, 2) - 1;
	if (!player->hasBlessing(blessing)) {
		pushBoolean(L, false);
		return 1;
	}

	player->removeBlessing(blessing);
	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaPlayerCanLearnSpell(lua_State* L)
{
	// player:canLearnSpell(spellName)
	const auto player = getSharedPtr<const Player>(L, 1);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}

	const std::string& spellName = getString(L, 2);
	const auto spell = g_spells->getInstantSpellByName(spellName);
	if (!spell) {
		reportErrorFunc(L, "Spell \"" + spellName + "\" not found");
		pushBoolean(L, false);
		return 1;
	}

	if (player->hasFlag(PlayerFlag_IgnoreSpellCheck)) {
		pushBoolean(L, true);
		return 1;
	}

	const auto& vocMap = spell->getVocMap();
	if (!vocMap.contains(player->getVocationId())) {
		pushBoolean(L, false);
	} else if (player->getLevel() < spell->getLevel()) {
		pushBoolean(L, false);
	} else if (player->getMagicLevel() < spell->getMagicLevel()) {
		pushBoolean(L, false);
	} else {
		pushBoolean(L, true);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerLearnSpell(lua_State* L)
{
	// player:learnSpell(spellName)
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		const std::string& spellName = getString(L, 2);
		player->learnInstantSpell(spellName);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerForgetSpell(lua_State* L)
{
	// player:forgetSpell(spellName)
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		const std::string& spellName = getString(L, 2);
		player->forgetInstantSpell(spellName);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerHasLearnedSpell(lua_State* L)
{
	// player:hasLearnedSpell(spellName)
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		const std::string& spellName = getString(L, 2);
		pushBoolean(L, player->hasLearnedInstantSpell(spellName));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerSendTutorial(lua_State* L)
{
	// player:sendTutorial(tutorialId)
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		const auto tutorialId = getNumber<uint8_t>(L, 2);
		player->sendTutorial(tutorialId);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerAddMapMark(lua_State* L)
{
	// player:addMapMark(position, type, description)
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		const auto& position = getPosition(L, 2);
		const auto type = getNumber<uint8_t>(L, 3);
		const auto& description = getString(L, 4);
		player->sendAddMarker(position, type, description);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerSave(lua_State* L)
{
	// player:save()
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		player->loginPosition = player->getPosition();
		pushBoolean(L, IOLoginData::savePlayer(player));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerPopupFYI(lua_State* L)
{
	// player:popupFYI(message)
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		const auto& message = getString(L, 2);
		player->sendFYIBox(message);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerIsPzLocked(lua_State* L)
{
	// player:isPzLocked()
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		pushBoolean(L, player->isPzLocked());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetClient(lua_State* L)
{
	// player:getClient()
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		lua_createtable(L, 0, 2);
		setField(L, "version", player->getProtocolVersion());
		setField(L, "os", player->getOperatingSystem());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetHouse(lua_State* L)
{
	// player:getHouse()
	const auto player = getSharedPtr<Player>(L, 1);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}

	if (const auto house = g_game.map.houses.getHouseByPlayerId(player->getGUID())) {
		pushUserdata<House>(L, house);
		setMetatable(L, -1, "House");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerSendHouseWindow(lua_State* L)
{
	// player:sendHouseWindow(house, listId)
	const auto player = getSharedPtr<Player>(L, 1);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}

	const auto house = getUserdata<House>(L, 2);
	if (!house) {
		lua_pushnil(L);
		return 1;
	}

	const auto listId = getNumber<uint32_t>(L, 3);
	player->sendHouseWindow(house, listId);
	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaPlayerSetEditHouse(lua_State* L)
{
	// player:setEditHouse(house, listId)
	const auto player = getSharedPtr<Player>(L, 1);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}

	const auto house = getUserdata<House>(L, 2);
	if (!house) {
		lua_pushnil(L);
		return 1;
	}

	const auto listId = getNumber<uint32_t>(L, 3);
	player->setEditHouse(house, listId);
	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaPlayerSetGhostMode(lua_State* L)
{
	// player:setGhostMode(enabled[, magicEffect = CONST_ME_TELEPORT])
	const auto player = getSharedPtr<Player>(L, 1);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}

	const bool enabled = getBoolean(L, 2);
	if (player->isInGhostMode() == enabled) {
		pushBoolean(L, true);
		return 1;
	}

	const auto magicEffect = getNumber<MagicEffectClasses>(L, 3, CONST_ME_TELEPORT);

	player->switchGhostMode();

	const auto tile = player->getTile();
	const Position& position = player->getPosition();
	const bool isInvisible = player->isInvisible();

	SpectatorVec spectators;
	g_game.map.getSpectators(spectators, position, true, true);
	for (auto spectator : spectators) {
		assert(std::dynamic_pointer_cast<Player>(spectator) != nullptr);

		PlayerPtr spectatorPlayer = std::static_pointer_cast<Player>(spectator);
		if (spectatorPlayer != player && !spectatorPlayer->isAccessPlayer()) {
			if (enabled) {
				spectatorPlayer->sendRemoveTileCreature(player, position, tile->getClientIndexOfCreature(spectatorPlayer, player));
			} else {
				spectatorPlayer->sendCreatureAppear(player, position, magicEffect);
			}
		} else {
			if (isInvisible) {
				continue;
			}

			spectatorPlayer->sendCreatureChangeVisible(player, !enabled);
		}
	}

	if (player->isInGhostMode()) {
		for (const auto& val : g_game.getPlayers() | std::views::values) {
			if (!val->isAccessPlayer()) {
				val->notifyStatusChange(player, VIPSTATUS_OFFLINE);
			}
		}
		IOLoginData::updateOnlineStatus(player->getGUID(), false);
	} else {
		for (const auto& val : g_game.getPlayers() | std::views::values) {
			if (!val->isAccessPlayer()) {
				val->notifyStatusChange(player, VIPSTATUS_ONLINE);
			}
		}
		IOLoginData::updateOnlineStatus(player->getGUID(), true);
	}
	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaPlayerGetContainerId(lua_State* L)
{
	// player:getContainerId(container)
	const auto player = getSharedPtr<Player>(L, 1);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}

	if (const auto container = getSharedPtr<Container>(L, 2)) {
		lua_pushinteger(L, player->getContainerID(container));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetContainerById(lua_State* L)
{
	// player:getContainerById(id)
	const auto player = getSharedPtr<Player>(L, 1);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}

	if (const auto container = player->getContainerByID(getNumber<uint8_t>(L, 2))) {
		pushSharedPtr(L, container);
		setMetatable(L, -1, "Container");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetContainerIndex(lua_State* L)
{
	// player:getContainerIndex(id)
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		lua_pushinteger(L, player->getContainerIndex(getNumber<uint8_t>(L, 2)));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetInstantSpells(lua_State* L)
{
	// player:getInstantSpells()
	const auto player = getSharedPtr<Player>(L, 1);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}

	std::vector<const InstantSpell*> spells;
	for (const auto& val : g_spells->getInstantSpells() | std::views::values) {
		if (val.canCast(player)) {
			spells.push_back(&val);
		}
	}

	lua_createtable(L, spells.size(), 0);

	int index = 0;
	for (const auto spell : spells) {
		pushInstantSpell(L, *spell);
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerCanCast(lua_State* L)
{
	// player:canCast(spell)
	const auto player = getSharedPtr<Player>(L, 1);
	const auto spell = getUserdata<InstantSpell>(L, 2);
	if (player && spell) {
		pushBoolean(L, spell->canCast(player));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerHasChaseMode(lua_State* L)
{
	// player:hasChaseMode()
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		pushBoolean(L, player->chaseMode);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerHasSecureMode(lua_State* L)
{
	// player:hasSecureMode()
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		pushBoolean(L, player->secureMode);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetFightMode(lua_State* L)
{
	// player:getFightMode()
	if (const auto player = getSharedPtr<Player>(L, 1)) {
		lua_pushinteger(L, player->fightMode);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetStoreInbox(lua_State* L)
{
	// player:getStoreInbox()
	const auto player = getSharedPtr<Player>(L, 1);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}

	const auto storeInbox = player->getStoreInbox();
	if (!storeInbox) {
		lua_pushnil(L);
		return 1;
	}

	pushSharedPtr(L, storeInbox);
	setMetatable(L, -1, "Container");
	return 1;
}

int LuaScriptInterface::luaPlayerIsNearDepotBox(lua_State* L)
{
	// player:isNearDepotBox()
	const auto player = getSharedPtr<const Player>(L, 1);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}
	pushBoolean(L, player->isNearDepotBox());
	return 1;
}
int LuaScriptInterface::luaPlayerGetIdleTime(lua_State* L)
{
	// player:getIdleTime()
	const auto player = getSharedPtr<const Player>(L, 1);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}
	lua_pushinteger(L, player->getIdleTime());
	return 1;
}

int LuaScriptInterface::luaPlayerResetIdleTime(lua_State* L)
{
	// player:resetIdleTime()
	const auto player = getSharedPtr<Player>(L, 1);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}

	player->resetIdleTime();
	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaPlayerSendCreatureSquare(lua_State* L)
{
	// player:sendCreatureSquare(creature, color)
	const auto player = getSharedPtr<Player>(L, 1);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}
	const auto creature = getCreature(L, 2);
	if (!creature) {
		reportErrorFunc(L, getErrorDesc(LUA_ERROR_CREATURE_NOT_FOUND));
		pushBoolean(L, false);
		return 1;
	}
	player->sendCreatureSquare(creature, getNumber<SquareColor_t>(L, 3));
	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaPlayerGetEquipment(lua_State* L)
{
	// player:getEquipment([validate = true])
	const auto& player = getSharedPtr<Player>(L, 1);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}

	auto validate = (isBoolean(L, 2)) ? getBoolean(L, 2) : true;
	auto equipment = player->getEquipment(validate);

	lua_newtable(L);
	int index = 1;
	for (const auto& item : equipment) {
		pushSharedPtr(L, item);
		setMetatable(L, -1, "Item");
		lua_rawseti(L, -2, index++);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerAddAugment(lua_State* L)
{
	const auto player = getSharedPtr<Player>(L, 1);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}

	if (isString(L, 2)) {
		if (auto augment = Augments::GetAugment(getString(L, 2))) {
			lua_pushboolean(L, player->addAugment(augment));
		} else {
			lua_pushnil(L);
			reportError(__FUNCTION__, "Player::addAugment() argument not found as any name in augments loaded on startup! \n");
		}
	} else if (isUserdata(L, 2)) {
		if (std::shared_ptr augment = getSharedPtr<Augment>(L, 2)) {
			lua_pushboolean(L, player->addAugment(augment));
		} else {
			lua_pushnil(L);
			reportError(__FUNCTION__, "Player::addAugment() invalid userdata passed as argument! \n");
		}
	} else {
		reportError(__FUNCTION__, "Player::addAugment() passed invalid type, must be string or userdata! \n");
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerRemoveAugment(lua_State* L)
{
	const auto player = getSharedPtr<Player>(L, 1);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}

	if (isString(L, 2)) {
		const auto name = getString(L, 2);
		lua_pushboolean(L, player->removeAugment(name));
	} else if (isUserdata(L, 2)) {
		if (std::shared_ptr<Augment> augment = getSharedPtr<Augment>(L, 2)) {
			lua_pushboolean(L, player->removeAugment(augment));
		} else {
			reportError(__FUNCTION__, "Player::removeAugment() invalid userdata type passed as argument! \n");
			lua_pushnil(L);
		}
	} else {
		reportError(__FUNCTION__, "Player::removeAugment() passed invalid type, must be string or userdata! \n");
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPlayerIsAugmented(lua_State* L)
{
	const auto player = getSharedPtr<Player>(L, 1);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}
	lua_pushboolean(L, player->isAugmented());
	return 1;
}

int LuaScriptInterface::luaPlayerHasAugment(lua_State* L)
{
	const auto player = getSharedPtr<Player>(L, 1);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}

	if (isString(L, 2) && isBoolean(L, 3)) {
		const auto name = getString(L, 2);
		const auto checkItems = getBoolean(L, 3);
		lua_pushboolean(L, player->hasAugment(name, checkItems));
	} else if (isUserdata(L, 2)) {
		if (std::shared_ptr<Augment>& augment = getSharedPtr<Augment>(L, 2)) {
			if (isBoolean(L, 3)) {
				const auto checkItems = getBoolean(L, 3);
				lua_pushboolean(L, player->hasAugment(augment, checkItems));
			} else {
				lua_pushboolean(L, player->hasAugment(augment, true));
			}
		}
	}
	return 1;
}

int LuaScriptInterface::luaPlayerGetAugments(lua_State* L)
{
	const auto player = getSharedPtr<Player>(L, 1);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}

	const std::vector<std::shared_ptr<Augment>> augments;
	
	lua_newtable(L);
	int index = 1;
	for (const auto& augment : augments) {
		pushSharedPtr(L, augment);
		setMetatable(L, -1, "Augment");
		lua_rawseti(L, -2, index++);
	}
	return 1;
}

// Monster
int LuaScriptInterface::luaMonsterCreate(lua_State* L)
{
	// Monster(id or userdata)
	MonsterPtr monster;
	if (isNumber(L, 2)) {
		monster = g_game.getMonsterByID(getNumber<uint32_t>(L, 2));
	} else if (isUserdata(L, 2)) {
		if (getUserdataType(L, 2) != LuaData_Monster) {
			lua_pushnil(L);
			return 1;
		}
		monster = getSharedPtr<Monster>(L, 2);
	} else {
		monster = nullptr;
	}

	if (monster) {
		pushSharedPtr(L, monster);
		setMetatable(L, -1, "Monster");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterDelete(lua_State* L)
{
	if (auto& monster = getSharedPtr<Monster>(L, 1)) {
		monster.reset();
	}
	return 0;
}

int LuaScriptInterface::luaMonsterGetId(lua_State* L)
{
	// monster:getId()
	if (auto& monster = getSharedPtr<Monster>(L, 1)) {
		// Set monster id if it's not set yet (only for onSpawn event)
		if (getScriptEnv()->getScriptId() == g_events->getScriptId(EventInfoId::MONSTER_ONSPAWN)) {
			monster->setID();
		}

		lua_pushinteger(L, monster->getID());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterIsMonster(lua_State* L)
{
	// monster:isMonster()
	pushBoolean(L, getUserdata<const Monster>(L, 1) != nullptr);
	return 1;
}

int LuaScriptInterface::luaMonsterGetType(lua_State* L)
{
	// monster:getType()
	if (const auto monster = getSharedPtr<const Monster>(L, 1)) {
		pushUserdata<MonsterType>(L, monster->mType);
		setMetatable(L, -1, "MonsterType");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterRename(lua_State* L)
{
	// monster:rename(name[, nameDescription])
	const auto monster = getSharedPtr<Monster>(L, 1);
	if (!monster) {
		lua_pushnil(L);
		return 1;
	}

	monster->setName(getString(L, 2));
	if (lua_gettop(L) >= 3) {
		monster->setNameDescription(getString(L, 3));
	}

	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaMonsterGetSpawnPosition(lua_State* L)
{
	// monster:getSpawnPosition()
	if (const auto monster = getSharedPtr<const Monster>(L, 1)) {
		pushPosition(L, monster->getMasterPos());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterIsInSpawnRange(lua_State* L)
{
	// monster:isInSpawnRange([position])
	if (const auto monster = getSharedPtr<Monster>(L, 1)) {
		pushBoolean(L, monster->isInSpawnRange(lua_gettop(L) >= 2 ? getPosition(L, 2) : monster->getPosition()));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterIsIdle(lua_State* L)
{
	// monster:isIdle()
	if (const auto monster = getSharedPtr<Monster>(L, 1)) {
		pushBoolean(L, monster->getIdleStatus());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSetIdle(lua_State* L)
{
	// monster:setIdle(idle)
	const auto monster = getSharedPtr<Monster>(L, 1);
	if (!monster) {
		lua_pushnil(L);
		return 1;
	}

	monster->setIdle(getBoolean(L, 2));
	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaMonsterIsTarget(lua_State* L)
{
	// monster:isTarget(creature)
	if (const auto monster = getSharedPtr<Monster>(L, 1)) {
		const auto creature = getCreature(L, 2);
		if (!creature) {
			reportErrorFunc(L, getErrorDesc(LUA_ERROR_CREATURE_NOT_FOUND));
			pushBoolean(L, false);
			return 1;
		}

		pushBoolean(L, monster->isTarget(creature));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterIsOpponent(lua_State* L)
{
	// monster:isOpponent(creature)
	if (const auto monster = getSharedPtr<Monster>(L, 1)) {
		const auto creature = getCreature(L, 2);
		if (!creature) {
			reportErrorFunc(L, getErrorDesc(LUA_ERROR_CREATURE_NOT_FOUND));
			pushBoolean(L, false);
			return 1;
		}

		pushBoolean(L, monster->isOpponent(creature));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterIsFriend(lua_State* L)
{
	// monster:isFriend(creature)
	if (const auto monster = getSharedPtr<Monster>(L, 1)) {
		const auto creature = getCreature(L, 2);
		if (!creature) {
			reportErrorFunc(L, getErrorDesc(LUA_ERROR_CREATURE_NOT_FOUND));
			pushBoolean(L, false);
			return 1;
		}

		pushBoolean(L, monster->isFriend(creature));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterAddFriend(lua_State* L)
{
	// monster:addFriend(creature)
	if (const auto monster = getSharedPtr<Monster>(L, 1)) {
		const auto creature = getCreature(L, 2);
		if (!creature) {
			reportErrorFunc(L, getErrorDesc(LUA_ERROR_CREATURE_NOT_FOUND));
			pushBoolean(L, false);
			return 1;
		}

		monster->addFriend(creature);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterRemoveFriend(lua_State* L)
{
	// monster:removeFriend(creature)
	if (const auto monster = getSharedPtr<Monster>(L, 1)) {
		const auto creature = getCreature(L, 2);
		if (!creature) {
			reportErrorFunc(L, getErrorDesc(LUA_ERROR_CREATURE_NOT_FOUND));
			pushBoolean(L, false);
			return 1;
		}

		monster->removeFriend(creature);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterGetFriendList(lua_State* L)
{
	// monster:getFriendList()
	const auto monster = getSharedPtr<Monster>(L, 1);
	if (!monster) {
		lua_pushnil(L);
		return 1;
	}

	const auto& friendList = monster->getFriendList();
	lua_createtable(L, friendList.size(), 0);

	int index = 0;
	for (const auto creature : friendList) {
		pushSharedPtr(L, creature);
		setCreatureMetatable(L, -1, creature);
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterGetFriendCount(lua_State* L)
{
	// monster:getFriendCount()
	if (const auto monster = getSharedPtr<Monster>(L, 1)) {
		lua_pushinteger(L, monster->getFriendList().size());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterAddTarget(lua_State* L)
{
	// monster:addTarget(creature[, pushFront = false])
	const auto monster = getSharedPtr<Monster>(L, 1);
	if (!monster) {
		lua_pushnil(L);
		return 1;
	}

	const auto creature = getCreature(L, 2);
	if (!creature) {
		reportErrorFunc(L, getErrorDesc(LUA_ERROR_CREATURE_NOT_FOUND));
		pushBoolean(L, false);
		return 1;
	}

	const bool pushFront = getBoolean(L, 3, false);
	monster->addTarget(creature, pushFront);
	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaMonsterRemoveTarget(lua_State* L)
{
	// monster:removeTarget(creature)
	const auto monster = getSharedPtr<Monster>(L, 1);
	if (!monster) {
		lua_pushnil(L);
		return 1;
	}

	const auto creature = getCreature(L, 2);
	if (!creature) {
		reportErrorFunc(L, getErrorDesc(LUA_ERROR_CREATURE_NOT_FOUND));
		pushBoolean(L, false);
		return 1;
	}

	monster->removeTarget(creature);
	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaMonsterGetTargetList(lua_State* L)
{
	// monster:getTargetList()
	const auto monster = getUserdata<Monster>(L, 1);
	if (!monster) {
		lua_pushnil(L);
		return 1;
	}

	const auto& targetList = monster->getTargetList();
	lua_createtable(L, targetList.size(), 0);

	int index = 0;
	for (const auto weakTarget : targetList) {
		if (auto creature = weakTarget.lock()) {
			pushSharedPtr(L, creature);
			setCreatureMetatable(L, -1, creature);
			lua_rawseti(L, -2, ++index);
		}
	}
	return 1;
}

int LuaScriptInterface::luaMonsterGetTargetCount(lua_State* L)
{
	// monster:getTargetCount()
	if (const auto monster = getSharedPtr<Monster>(L, 1)) {
		lua_pushinteger(L, monster->getTargetList().size());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSelectTarget(lua_State* L)
{
	// monster:selectTarget(creature)
	if (const auto monster = getSharedPtr<Monster>(L, 1)) {
		const auto creature = getCreature(L, 2);
		if (!creature) {
			reportErrorFunc(L, getErrorDesc(LUA_ERROR_CREATURE_NOT_FOUND));
			pushBoolean(L, false);
			return 1;
		}

		pushBoolean(L, monster->selectTarget(creature));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSearchTarget(lua_State* L)
{
	// monster:searchTarget([searchType = TARGETSEARCH_DEFAULT])
	if (const auto monster = getSharedPtr<Monster>(L, 1)) {
		const auto searchType = getNumber<TargetSearchType_t>(L, 2, TARGETSEARCH_DEFAULT);
		pushBoolean(L, monster->searchTarget(searchType));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterIsWalkingToSpawn(lua_State* L)
{
	// monster:isWalkingToSpawn()
	if (const auto monster = getSharedPtr<Monster>(L, 1)) {
		pushBoolean(L, monster->isWalkingToSpawn());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterWalkToSpawn(lua_State* L)
{
	// monster:walkToSpawn()
	if (const auto monster = getSharedPtr<Monster>(L, 1)) {
		pushBoolean(L, monster->walkToSpawn());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// Npc
int LuaScriptInterface::luaNpcCreate(lua_State* L)
{
	// Npc([id or name or userdata])
	NpcPtr npc;
	if (lua_gettop(L) >= 2) {
		if (isNumber(L, 2)) {
			npc = g_game.getNpcByID(getNumber<uint32_t>(L, 2));
		} else if (isString(L, 2)) {
			npc = g_game.getNpcByName(getString(L, 2));
		} else if (isUserdata(L, 2)) {
			if (getUserdataType(L, 2) != LuaData_Npc) {
				lua_pushnil(L);
				return 1;
			}
			npc = getSharedPtr<Npc>(L, 2);
		} else {
			npc = nullptr;
		}
	} else {
		npc = getScriptEnv()->getNpc();
	}

	if (npc) {
		pushSharedPtr(L, npc);
		setMetatable(L, -1, "Npc");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaNpcDelete(lua_State* L)
{
	if (auto& npc = getSharedPtr<Npc>(L, 1)) {
		npc.reset();
	}
	return 0;
}

int LuaScriptInterface::luaNpcIsNpc(lua_State* L)
{
	// npc:isNpc()
	pushBoolean(L, getSharedPtr<const Npc>(L, 1) != nullptr);
	return 1;
}

int LuaScriptInterface::luaNpcSetMasterPos(lua_State* L)
{
	// npc:setMasterPos(pos[, radius])
	const auto npc = getSharedPtr<Npc>(L, 1);
	if (!npc) {
		lua_pushnil(L);
		return 1;
	}

	const Position& pos = getPosition(L, 2);
	const int32_t radius = getNumber<int32_t>(L, 3, 1);
	npc->setMasterPos(pos, radius);
	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaNpcGetSpeechBubble(lua_State* L)
{
	// npc:getSpeechBubble()
	if (const auto npc = getSharedPtr<Npc>(L, 1)) {
		lua_pushinteger(L, npc->getSpeechBubble());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaNpcSetSpeechBubble(lua_State* L)
{
	// npc:setSpeechBubble(speechBubble)
	const auto npc = getSharedPtr<Npc>(L, 1);
	if (!npc) {
		lua_pushnil(L);
		return 1;
	}

	if (!isNumber(L, 2)) {
		lua_pushnil(L);
		return 1;
	}

	if (const uint8_t speechBubble = getNumber<uint8_t>(L, 2); speechBubble > SPEECHBUBBLE_LAST) {
		lua_pushnil(L);
	} else {
		npc->setSpeechBubble(speechBubble);
		// update creature speech bubble
		g_game.notifySpectators(npc);
		pushBoolean(L, true);
	}
	return 1;
}

int LuaScriptInterface::luaNpcGetSpectators(lua_State* L)
{
	// npc:getSpectators()
	const auto npc = getSharedPtr<Npc>(L, 1);
	if (!npc) {
		lua_pushnil(L);
		return 1;
	}

	const auto& spectators = npc->getSpectators();
	lua_createtable(L, spectators.size(), 0);
	int index = 0;

	for (const auto& spectatorPlayer : npc->getSpectators()) {
		pushSharedPtr(L, spectatorPlayer);
		setMetatable(L, -1, "Player");
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

// Guild
int LuaScriptInterface::luaGuildCreate(lua_State* L)
{
	// Guild(id)
	const uint32_t id = getNumber<uint32_t>(L, 2);

	if (const auto guild = g_game.getGuild(id)) {
		pushSharedPtr(L, guild);
		setMetatable(L, -1, "Guild");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaGuildGetId(lua_State* L)
{
	// guild:getId()
	if (const auto guild = getUserdata<Guild>(L, 1)) {
		lua_pushinteger(L, guild->getId());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaGuildGetName(lua_State* L)
{
	// guild:getName()
	if (const auto guild = getUserdata<Guild>(L, 1)) {
		pushString(L, guild->getName());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaGuildGetMembersOnline(lua_State* L)
{
	// guild:getMembersOnline()
	const auto guild = getUserdata<const Guild>(L, 1);
	if (!guild) {
		lua_pushnil(L);
		return 1;
	}

	const auto& members = guild->getMembersOnline();
	lua_createtable(L, members.size(), 0);

	int index = 0;
	for (const auto player : members) {
		pushSharedPtr(L, player);
		setMetatable(L, -1, "Player");
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaGuildAddRank(lua_State* L)
{
	// guild:addRank(id, name, level)
	if (const auto guild = getSharedPtr<Guild>(L, 1)) {
		const uint32_t id = getNumber<uint32_t>(L, 2);
		const std::string& name = getString(L, 3);
		const uint8_t level = getNumber<uint8_t>(L, 4);
		guild->addRank(id, name, level);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaGuildGetRankById(lua_State* L)
{
	// guild:getRankById(id)
	const auto guild = getUserdata<Guild>(L, 1);
	if (!guild) {
		lua_pushnil(L);
		return 1;
	}

	const uint32_t id = getNumber<uint32_t>(L, 2);
	if (const auto rank = guild->getRankById(id)) {
		lua_createtable(L, 0, 3);
		setField(L, "id", rank->id);
		setField(L, "name", rank->name);
		setField(L, "level", static_cast<uint16_t>(rank->level));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaGuildGetRankByLevel(lua_State* L)
{
	// guild:getRankByLevel(level)
	const auto guild = getUserdata<const Guild>(L, 1);
	if (!guild) {
		lua_pushnil(L);
		return 1;
	}

	const uint8_t level = getNumber<uint8_t>(L, 2);
	if (const auto rank = guild->getRankByLevel(level)) {
		lua_createtable(L, 0, 3);
		setField(L, "id", rank->id);
		setField(L, "name", rank->name);
		setField(L, "level", static_cast<uint16_t>(rank->level));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaGuildGetMotd(lua_State* L)
{
	// guild:getMotd()
	if (const auto guild = getUserdata<Guild>(L, 1)) {
		pushString(L, guild->getMotd());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaGuildSetMotd(lua_State* L)
{
	// guild:setMotd(motd)
	const std::string& motd = getString(L, 2);
	if (const auto guild = getUserdata<Guild>(L, 1)) {
		guild->setMotd(motd);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// Group
int LuaScriptInterface::luaGroupCreate(lua_State* L)
{
	// Group(id)
	const uint32_t id = getNumber<uint32_t>(L, 2);

	if (const auto group = g_game.groups.getGroup(id)) {
		pushUserdata<Group>(L, group);
		setMetatable(L, -1, "Group");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaGroupGetId(lua_State* L)
{
	// group:getId()
	if (const auto group = getUserdata<Group>(L, 1)) {
		lua_pushinteger(L, group->id);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaGroupGetName(lua_State* L)
{
	// group:getName()
	if (const auto group = getUserdata<Group>(L, 1)) {
		pushString(L, group->name);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaGroupGetFlags(lua_State* L)
{
	// group:getFlags()
	if (const auto group = getUserdata<Group>(L, 1)) {
		lua_pushinteger(L, group->flags);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaGroupGetAccess(lua_State* L)
{
	// group:getAccess()
	if (const auto group = getUserdata<Group>(L, 1)) {
		pushBoolean(L, group->access);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaGroupGetMaxDepotItems(lua_State* L)
{
	// group:getMaxDepotItems()
	if (const auto group = getUserdata<Group>(L, 1)) {
		lua_pushinteger(L, group->maxDepotItems);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaGroupGetMaxVipEntries(lua_State* L)
{
	// group:getMaxVipEntries()
	if (const auto group = getUserdata<Group>(L, 1)) {
		lua_pushinteger(L, group->maxVipEntries);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaGroupHasFlag(lua_State* L)
{
	// group:hasFlag(flag)
	if (const auto group = getUserdata<Group>(L, 1)) {
		const auto flag = getNumber<PlayerFlags>(L, 2);
		pushBoolean(L, (group->flags & flag) != 0);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// Vocation
int LuaScriptInterface::luaVocationCreate(lua_State* L)
{
	// Vocation(id or name)
	uint32_t id;
	if (isNumber(L, 2)) {
		id = getNumber<uint32_t>(L, 2);
	} else {
		id = g_vocations.getVocationId(getString(L, 2));
	}

	if (const auto vocation = g_vocations.getVocation(id)) {
		pushUserdata<Vocation>(L, vocation);
		setMetatable(L, -1, "Vocation");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaVocationGetId(lua_State* L)
{
	// vocation:getId()
	if (const auto vocation = getUserdata<Vocation>(L, 1)) {
		lua_pushinteger(L, vocation->getId());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaVocationGetClientId(lua_State* L)
{
	// vocation:getClientId()
	if (const auto vocation = getUserdata<Vocation>(L, 1)) {
		lua_pushinteger(L, vocation->getClientId());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaVocationGetName(lua_State* L)
{
	// vocation:getName()
	if (const auto vocation = getUserdata<Vocation>(L, 1)) {
		pushString(L, vocation->getVocName());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaVocationGetDescription(lua_State* L)
{
	// vocation:getDescription()
	if (const auto vocation = getUserdata<Vocation>(L, 1)) {
		pushString(L, vocation->getVocDescription());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaVocationGetRequiredSkillTries(lua_State* L)
{
	// vocation:getRequiredSkillTries(skillType, skillLevel)
	if (const auto vocation = getUserdata<Vocation>(L, 1)) {
		const auto skillType = getNumber<skills_t>(L, 2);
		const uint16_t skillLevel = getNumber<uint16_t>(L, 3);
		lua_pushinteger(L, vocation->getReqSkillTries(skillType, skillLevel));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaVocationGetRequiredManaSpent(lua_State* L)
{
	// vocation:getRequiredManaSpent(magicLevel)
	if (const auto vocation = getUserdata<Vocation>(L, 1)) {
		const uint32_t magicLevel = getNumber<uint32_t>(L, 2);
		lua_pushinteger(L, vocation->getReqMana(magicLevel));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaVocationGetCapacityGain(lua_State* L)
{
	// vocation:getCapacityGain()
	if (const auto vocation = getUserdata<Vocation>(L, 1)) {
		lua_pushinteger(L, vocation->getCapGain());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaVocationGetHealthGain(lua_State* L)
{
	// vocation:getHealthGain()
	if (const auto vocation = getUserdata<Vocation>(L, 1)) {
		lua_pushinteger(L, vocation->getHPGain());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaVocationGetHealthGainTicks(lua_State* L)
{
	// vocation:getHealthGainTicks()
	if (const auto vocation = getUserdata<Vocation>(L, 1)) {
		lua_pushinteger(L, vocation->getHealthGainTicks());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaVocationGetHealthGainAmount(lua_State* L)
{
	// vocation:getHealthGainAmount()
	if (const auto vocation = getUserdata<Vocation>(L, 1)) {
		lua_pushinteger(L, vocation->getHealthGainAmount());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaVocationGetManaGain(lua_State* L)
{
	// vocation:getManaGain()
	if (const auto vocation = getUserdata<Vocation>(L, 1)) {
		lua_pushinteger(L, vocation->getManaGain());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaVocationGetManaGainTicks(lua_State* L)
{
	// vocation:getManaGainTicks()
	if (const auto vocation = getUserdata<Vocation>(L, 1)) {
		lua_pushinteger(L, vocation->getManaGainTicks());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaVocationGetManaGainAmount(lua_State* L)
{
	// vocation:getManaGainAmount()
	if (const auto vocation = getUserdata<Vocation>(L, 1)) {
		lua_pushinteger(L, vocation->getManaGainAmount());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaVocationGetMaxSoul(lua_State* L)
{
	// vocation:getMaxSoul()
	if (const auto vocation = getUserdata<Vocation>(L, 1)) {
		lua_pushinteger(L, vocation->getSoulMax());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaVocationGetSoulGainTicks(lua_State* L)
{
	// vocation:getSoulGainTicks()
	if (const auto vocation = getUserdata<Vocation>(L, 1)) {
		lua_pushinteger(L, vocation->getSoulGainTicks());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaVocationGetAttackSpeed(lua_State* L)
{
	// vocation:getAttackSpeed()
	if (const auto vocation = getUserdata<Vocation>(L, 1)) {
		lua_pushinteger(L, vocation->getAttackSpeed());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaVocationGetBaseSpeed(lua_State* L)
{
	// vocation:getBaseSpeed()
	if (const auto vocation = getUserdata<Vocation>(L, 1)) {
		lua_pushinteger(L, vocation->getBaseSpeed());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaVocationGetDemotion(lua_State* L)
{
	// vocation:getDemotion()
	const auto vocation = getUserdata<Vocation>(L, 1);
	if (!vocation) {
		lua_pushnil(L);
		return 1;
	}

	const uint16_t fromId = vocation->getFromVocation();
	if (fromId == VOCATION_NONE) {
		lua_pushnil(L);
		return 1;
	}

	const auto demotedVocation = g_vocations.getVocation(fromId);
	if (demotedVocation && demotedVocation != vocation) {
		pushSharedPtr(L, demotedVocation);
		setMetatable(L, -1, "Vocation");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaVocationGetPromotion(lua_State* L)
{
	// vocation:getPromotion()
	const auto vocation = getUserdata<Vocation>(L, 1);
	if (!vocation) {
		lua_pushnil(L);
		return 1;
	}

	const uint16_t promotedId = g_vocations.getPromotedVocation(vocation->getId());
	if (promotedId == VOCATION_NONE) {
		lua_pushnil(L);
		return 1;
	}

	const auto promotedVocation = g_vocations.getVocation(promotedId);
	if (promotedVocation && promotedVocation != vocation) {
		pushSharedPtr(L, promotedVocation);
		setMetatable(L, -1, "Vocation");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaVocationAllowsPvp(lua_State* L)
{
	// vocation:allowsPvp()
	if (const auto vocation = getUserdata<Vocation>(L, 1)) {
		pushBoolean(L, vocation->allowsPvp());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// Town
int LuaScriptInterface::luaTownCreate(lua_State* L)
{
	// Town(id or name)
	Town* town;
	if (isNumber(L, 2)) {
		town = g_game.map.towns.getTown(getNumber<uint32_t>(L, 2));
	} else if (isString(L, 2)) {
		town = g_game.map.towns.getTown(getString(L, 2));
	} else {
		town = nullptr;
	}

	if (town) {
		pushUserdata<Town>(L, town);
		setMetatable(L, -1, "Town");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaTownGetId(lua_State* L)
{
	// town:getId()
	if (const auto town = getUserdata<Town>(L, 1)) {
		lua_pushinteger(L, town->getID());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaTownGetName(lua_State* L)
{
	// town:getName()
	if (const auto town = getUserdata<Town>(L, 1)) {
		pushString(L, town->getName());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaTownGetTemplePosition(lua_State* L)
{
	// town:getTemplePosition()
	if (const auto town = getUserdata<Town>(L, 1)) {
		pushPosition(L, town->getTemplePosition());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// House
int LuaScriptInterface::luaHouseCreate(lua_State* L)
{
	// House(id)
	if (const auto house = g_game.map.houses.getHouse(getNumber<uint32_t>(L, 2))) {
		pushUserdata<House>(L, house);
		setMetatable(L, -1, "House");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaHouseGetId(lua_State* L)
{
	// house:getId()
	if (const auto house = getUserdata<House>(L, 1)) {
		lua_pushinteger(L, house->getId());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaHouseGetName(lua_State* L)
{
	// house:getName()
	if (const auto house = getUserdata<House>(L, 1)) {
		pushString(L, house->getName());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaHouseGetTown(lua_State* L)
{
	// house:getTown()
	const auto house = getUserdata<House>(L, 1);
	if (!house) {
		lua_pushnil(L);
		return 1;
	}

	if (const auto town = g_game.map.towns.getTown(house->getTownId())) {
		pushUserdata<Town>(L, town);
		setMetatable(L, -1, "Town");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaHouseGetExitPosition(lua_State* L)
{
	// house:getExitPosition()
	if (const auto house = getUserdata<House>(L, 1)) {
		pushPosition(L, house->getEntryPosition());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaHouseGetRent(lua_State* L)
{
	// house:getRent()
	if (const auto house = getUserdata<House>(L, 1)) {
		lua_pushinteger(L, house->getRent());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaHouseSetRent(lua_State* L)
{
	// house:setRent(rent)
	const uint32_t rent = getNumber<uint32_t>(L, 2);
	if (const auto house = getUserdata<House>(L, 1)) {
		house->setRent(rent);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaHouseGetPaidUntil(lua_State* L)
{
	// house:getPaidUntil()
	if (const auto house = getUserdata<House>(L, 1)) {
		lua_pushinteger(L, house->getPaidUntil());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaHouseSetPaidUntil(lua_State* L)
{
	// house:setPaidUntil(timestamp)
	const time_t timestamp = getNumber<time_t>(L, 2);
	if (const auto house = getUserdata<House>(L, 1)) {
		house->setPaidUntil(timestamp);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaHouseGetPayRentWarnings(lua_State* L)
{
	// house:getPayRentWarnings()
	if (const auto house = getUserdata<House>(L, 1)) {
		lua_pushinteger(L, house->getPayRentWarnings());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaHouseSetPayRentWarnings(lua_State* L)
{
	// house:setPayRentWarnings(warnings)
	const uint32_t warnings = getNumber<uint32_t>(L, 2);
	if (const auto house = getUserdata<House>(L, 1)) {
		house->setPayRentWarnings(warnings);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaHouseGetOwnerGuid(lua_State* L)
{
	// house:getOwnerGuid()
	if (const auto house = getUserdata<House>(L, 1)) {
		lua_pushinteger(L, house->getOwner());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaHouseSetOwnerGuid(lua_State* L)
{
	// house:setOwnerGuid(guid[, updateDatabase = true])
	if (const auto house = getUserdata<House>(L, 1)) {
		const uint32_t guid = getNumber<uint32_t>(L, 2);
		const bool updateDatabase = getBoolean(L, 3, true);
		house->setOwner(guid, updateDatabase);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaHouseStartTrade(lua_State* L)
{
	// house:startTrade(player, tradePartner)
	const auto house = getUserdata<House>(L, 1);
	const auto player = getSharedPtr<Player>(L, 2);
	const auto tradePartner = getSharedPtr<Player>(L, 3);

	if (!player || !tradePartner || !house) {
		lua_pushnil(L);
		return 1;
	}

	if (!Position::areInRange<2, 2, 0>(tradePartner->getPosition(), player->getPosition())) {
		lua_pushinteger(L, RETURNVALUE_TRADEPLAYERFARAWAY);
		return 1;
	}

	if (house->getOwner() != player->getGUID()) {
		lua_pushinteger(L, RETURNVALUE_YOUDONTOWNTHISHOUSE);
		return 1;
	}

	if (g_game.map.houses.getHouseByPlayerId(tradePartner->getGUID())) {
		lua_pushinteger(L, RETURNVALUE_TRADEPLAYERALREADYOWNSAHOUSE);
		return 1;
	}

	if (IOLoginData::hasBiddedOnHouse(tradePartner->getGUID())) {
		lua_pushinteger(L, RETURNVALUE_TRADEPLAYERHIGHESTBIDDER);
		return 1;
	}

	const auto transferItem = house->getTransferItem();
	if (!transferItem) {
		lua_pushinteger(L, RETURNVALUE_YOUCANNOTTRADETHISHOUSE);
		return 1;
	}
	transferItem->getParent()->setParent(player);
	if (!g_game.internalStartTrade(player, tradePartner, transferItem)) {
		house->resetTransferItem();
	}

	lua_pushinteger(L, RETURNVALUE_NOERROR);
	return 1;
}

int LuaScriptInterface::luaHouseGetBeds(lua_State* L)
{
	// house:getBeds()
	const auto house = getUserdata<House>(L, 1);
	if (!house) {
		lua_pushnil(L);
		return 1;
	}

	const auto& beds = house->getBeds();
	lua_createtable(L, beds.size(), 0);

	int index = 0;
	for (const auto bedItem : beds) {
		pushSharedPtr(L, bedItem);
		setItemMetatable(L, -1, bedItem);
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaHouseGetBedCount(lua_State* L)
{
	// house:getBedCount()
	if (const auto house = getUserdata<House>(L, 1)) {
		lua_pushinteger(L, house->getBedCount());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaHouseGetDoors(lua_State* L)
{
	// house:getDoors()
	const auto house = getUserdata<House>(L, 1);
	if (!house) {
		lua_pushnil(L);
		return 1;
	}

	const auto& doors = house->getDoors();
	lua_createtable(L, doors.size(), 0);

	int index = 0;
	for (const auto door : doors) {
		pushSharedPtr(L, door);
		setItemMetatable(L, -1, door);
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaHouseGetDoorCount(lua_State* L)
{
	// house:getDoorCount()
	if (const auto house = getUserdata<House>(L, 1)) {
		lua_pushinteger(L, house->getDoors().size());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaHouseGetDoorIdByPosition(lua_State* L)
{
	// house:getDoorIdByPosition(position)
	const auto house = getUserdata<House>(L, 1);
	if (!house) {
		lua_pushnil(L);
		return 1;
	}

	if (const auto door = house->getDoorByPosition(getPosition(L, 2))) {
		lua_pushinteger(L, door->getDoorId());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaHouseGetTiles(lua_State* L)
{
	// house:getTiles()
	const auto house = getUserdata<House>(L, 1);
	if (!house) {
		lua_pushnil(L);
		return 1;
	}

	const auto& tiles = house->getTiles();
	lua_createtable(L, tiles.size(), 0);

	int index = 0;
	for (const auto tile : tiles) {
		pushSharedPtr(L, tile);
		setMetatable(L, -1, "Tile");
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaHouseGetItems(lua_State* L)
{
	// house:getItems()
	const auto house = getUserdata<House>(L, 1);
	if (!house) {
		lua_pushnil(L);
		return 1;
	}

	const auto& tiles = house->getTiles();
	lua_newtable(L);

	int index = 0;
	for (const auto tile : tiles) {
		if(const auto itemVector = tile->getItemList()) {
			for(const auto item : *itemVector) {
				pushSharedPtr(L, item);
				setItemMetatable(L, -1, item);
				lua_rawseti(L, -2, ++index);
			}
		}
	}
	return 1;
}

int LuaScriptInterface::luaHouseGetTileCount(lua_State* L)
{
	// house:getTileCount()
	if (const auto house = getUserdata<House>(L, 1)) {
		lua_pushinteger(L, house->getTiles().size());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaHouseCanEditAccessList(lua_State* L)
{
	// house:canEditAccessList(listId, player)
	const auto house = getUserdata<House>(L, 1);
	if (!house) {
		lua_pushnil(L);
		return 1;
	}

	uint32_t listId = getNumber<uint32_t>(L, 2);
	const auto player = getPlayer(L, 3);

	pushBoolean(L, house->canEditAccessList(listId, player));
	return 1;
}

int LuaScriptInterface::luaHouseGetAccessList(lua_State* L)
{
	// house:getAccessList(listId)
	const auto house = getUserdata<House>(L, 1);
	if (!house) {
		lua_pushnil(L);
		return 1;
	}

	std::string list;
	if (const uint32_t listId = getNumber<uint32_t>(L, 2); house->getAccessList(listId, list)) {
		pushString(L, list);
	} else {
		pushBoolean(L, false);
	}
	return 1;
}

int LuaScriptInterface::luaHouseSetAccessList(lua_State* L)
{
	// house:setAccessList(listId, list)
	const auto house = getUserdata<House>(L, 1);
	if (!house) {
		lua_pushnil(L);
		return 1;
	}

	const uint32_t listId = getNumber<uint32_t>(L, 2);
	const std::string& list = getString(L, 3);
	house->setAccessList(listId, list);
	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaHouseKickPlayer(lua_State* L)
{
	// house:kickPlayer(player, targetPlayer)
	const auto house = getUserdata<House>(L, 1);
	if (!house) {
		lua_pushnil(L);
		return 1;
	}

	pushBoolean(L, house->kickPlayer(getPlayer(L, 2), getPlayer(L, 3)));
	return 1;
}

int LuaScriptInterface::luaHouseSave(lua_State* L)
{
	// house:save()
	const auto house = getUserdata<House>(L, 1);
	if (!house) {
		lua_pushnil(L);
		return 1;
	}

	pushBoolean(L, IOMapSerialize::saveHouse(house));
	return 1;
}

// ItemType
int LuaScriptInterface::luaItemTypeCreate(lua_State* L)
{
	// ItemType(id or name)
	uint32_t id;
	if (isNumber(L, 2)) {
		id = getNumber<uint32_t>(L, 2);
	} else if (isString(L, 2)) {
		id = Item::items.getItemIdByName(getString(L, 2));
	} else {
		lua_pushnil(L);
		return 1;
	}

	const ItemType& itemType = Item::items[id];
	pushUserdata<const ItemType>(L, &itemType);
	setMetatable(L, -1, "ItemType");
	return 1;
}

int LuaScriptInterface::luaItemTypeIsCorpse(lua_State* L)
{
	// itemType:isCorpse()
	if (const auto itemType = getUserdata<const ItemType>(L, 1)) {
		pushBoolean(L, itemType->corpseType != RACE_NONE);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeIsDoor(lua_State* L)
{
	// itemType:isDoor()
	if (const auto itemType = getUserdata<const ItemType>(L, 1)) {
		pushBoolean(L, itemType->isDoor());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeIsContainer(lua_State* L)
{
	// itemType:isContainer()
	if (const auto itemType = getUserdata<const ItemType>(L, 1)) {
		pushBoolean(L, itemType->isContainer());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeIsFluidContainer(lua_State* L)
{
	// itemType:isFluidContainer()
	if (const auto itemType = getUserdata<const ItemType>(L, 1)) {
		pushBoolean(L, itemType->isFluidContainer());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeIsMovable(lua_State* L)
{
	// itemType:isMovable()
	if (const auto itemType = getUserdata<const ItemType>(L, 1)) {
		pushBoolean(L, itemType->moveable);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeIsRune(lua_State* L)
{
	// itemType:isRune()
	if (const auto itemType = getUserdata<const ItemType>(L, 1)) {
		pushBoolean(L, itemType->isRune());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeIsStackable(lua_State* L)
{
	// itemType:isStackable()
	if (const auto itemType = getUserdata<const ItemType>(L, 1)) {
		pushBoolean(L, itemType->stackable);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeIsReadable(lua_State* L)
{
	// itemType:isReadable()
	if (const auto itemType = getUserdata<const ItemType>(L, 1)) {
		pushBoolean(L, itemType->canReadText);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeIsWritable(lua_State* L)
{
	// itemType:isWritable()
	if (const auto itemType = getUserdata<const ItemType>(L, 1)) {
		pushBoolean(L, itemType->canWriteText);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeIsBlocking(lua_State* L)
{
	// itemType:isBlocking()
	if (const auto itemType = getUserdata<const ItemType>(L, 1)) {
		pushBoolean(L, itemType->blockProjectile || itemType->blockSolid);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeIsGroundTile(lua_State* L)
{
	// itemType:isGroundTile()
	if (const auto itemType = getUserdata<const ItemType>(L, 1)) {
		pushBoolean(L, itemType->isGroundTile());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeIsMagicField(lua_State* L)
{
	// itemType:isMagicField()
	if (const auto itemType = getUserdata<const ItemType>(L, 1)) {
		pushBoolean(L, itemType->isMagicField());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeIsUseable(lua_State* L)
{
	// itemType:isUseable()
	if (const auto itemType = getUserdata<const ItemType>(L, 1)) {
		pushBoolean(L, itemType->isUseable());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeIsPickupable(lua_State* L)
{
	// itemType:isPickupable()
	if (const auto itemType = getUserdata<const ItemType>(L, 1)) {
		pushBoolean(L, itemType->isPickupable());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeIsRotatable(lua_State* L)
{
	// itemType:isRotatable()
	if (const auto itemType = getUserdata<const ItemType>(L, 1)) {
		pushBoolean(L, itemType->rotatable);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetType(lua_State* L)
{
	// itemType:getType()
	if (const auto itemType = getUserdata<const ItemType>(L, 1)) {
		lua_pushinteger(L, itemType->type);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetGroup(lua_State* L)
{
	// itemType:getGroup()
	if (const auto itemType = getUserdata<const ItemType>(L, 1)) {
		lua_pushinteger(L, itemType->group);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetId(lua_State* L)
{
	// itemType:getId()
	if (const auto itemType = getUserdata<const ItemType>(L, 1)) {
		lua_pushinteger(L, itemType->id);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetClientId(lua_State* L)
{
	// itemType:getClientId()
	if (const auto itemType = getUserdata<const ItemType>(L, 1)) {
		lua_pushinteger(L, itemType->clientId);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetName(lua_State* L)
{
	// itemType:getName()
	const auto itemType = getUserdata<const ItemType>(L, 1);
	if (itemType) {
		pushString(L, itemType->name);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetPluralName(lua_State* L)
{
	// itemType:getPluralName()
	if (const auto itemType = getUserdata<const ItemType>(L, 1)) {
		pushString(L, itemType->getPluralName());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetRotateTo(lua_State* L)
{
	// itemType:getRotateTo()
	if (const auto itemType = getUserdata<const ItemType>(L, 1)) {
		lua_pushinteger(L, itemType->rotateTo);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetArticle(lua_State* L)
{
	// itemType:getArticle()
	const auto itemType = getUserdata<const ItemType>(L, 1);
	if (itemType) {
		pushString(L, itemType->article);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetDescription(lua_State* L)
{
	// itemType:getDescription()
	const auto itemType = getUserdata<const ItemType>(L, 1);
	if (itemType) {
		pushString(L, itemType->description);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetSlotPosition(lua_State *L)
{
	// itemType:getSlotPosition()
	const auto itemType = getUserdata<const ItemType>(L, 1);
	if (itemType) {
		lua_pushinteger(L, itemType->slotPosition);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetCharges(lua_State* L)
{
	// itemType:getCharges()
	const auto itemType = getUserdata<const ItemType>(L, 1);
	if (itemType) {
		lua_pushinteger(L, itemType->charges);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetFluidSource(lua_State* L)
{
	// itemType:getFluidSource()
	const auto itemType = getUserdata<const ItemType>(L, 1);
	if (itemType) {
		lua_pushinteger(L, itemType->fluidSource);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetCapacity(lua_State* L)
{
	// itemType:getCapacity()
	const auto itemType = getUserdata<const ItemType>(L, 1);
	if (itemType) {
		lua_pushinteger(L, itemType->maxItems);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetWeight(lua_State* L)
{
	// itemType:getWeight([count = 1])
	const uint16_t count = getNumber<uint16_t>(L, 2, 1);

	const auto itemType = getUserdata<const ItemType>(L, 1);
	if (!itemType) {
		lua_pushnil(L);
		return 1;
	}

	const uint64_t weight = static_cast<uint64_t>(itemType->weight) * std::max<int32_t>(1, count);
	lua_pushinteger(L, weight);
	return 1;
}

int LuaScriptInterface::luaItemTypeGetWorth(lua_State* L)
{
	// itemType:getWorth()
	const auto itemType = getUserdata<const ItemType>(L, 1);
	if (!itemType) {
		lua_pushnil(L);
		return 1;
	}

	lua_pushinteger(L, itemType->worth);
	return 1;
}

int LuaScriptInterface::luaItemTypeGetHitChance(lua_State* L)
{
	// itemType:getHitChance()
	if (const auto itemType = getUserdata<const ItemType>(L, 1)) {
		lua_pushinteger(L, itemType->hitChance);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetShootRange(lua_State* L)
{
	// itemType:getShootRange()
	if (const auto itemType = getUserdata<const ItemType>(L, 1)) {
		lua_pushinteger(L, itemType->shootRange);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetAttack(lua_State* L)
{
	// itemType:getAttack()
	if (const auto itemType = getUserdata<const ItemType>(L, 1)) {
		lua_pushinteger(L, itemType->attack);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetAttackSpeed(lua_State* L)
{
	// itemType:getAttackSpeed()
	if (const auto itemType = getUserdata<const ItemType>(L, 1)) {
		lua_pushinteger(L, itemType->attackSpeed);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetClassification(lua_State* L)
{
	// itemType:getClassification()
	const auto itemType = getUserdata<const ItemType>(L, 1);
	if (itemType) {
		pushString(L, itemType->classification);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetTier(lua_State* L)
{
	// itemType:getTier()
	const auto itemType = getUserdata<const ItemType>(L, 1);
	if (itemType) {
		pushString(L, itemType->tier);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetDefense(lua_State* L)
{
	// itemType:getDefense()
	const auto itemType = getUserdata<const ItemType>(L, 1);
	if (itemType) {
		lua_pushinteger(L, itemType->defense);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetExtraDefense(lua_State* L)
{
	// itemType:getExtraDefense()
	const auto itemType = getUserdata<const ItemType>(L, 1);
	if (itemType) {
		lua_pushinteger(L, itemType->extraDefense);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetArmor(lua_State* L)
{
	// itemType:getArmor()
	const auto itemType = getUserdata<const ItemType>(L, 1);
	if (itemType) {
		lua_pushinteger(L, itemType->armor);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetWeaponType(lua_State* L)
{
	// itemType:getWeaponType()
	const auto itemType = getUserdata<const ItemType>(L, 1);
	if (itemType) {
		lua_pushinteger(L, itemType->weaponType);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetAmmoType(lua_State* L)
{
	// itemType:getAmmoType()
	if (const auto itemType = getUserdata<const ItemType>(L, 1)) {
		lua_pushinteger(L, itemType->ammoType);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetShootType(lua_State* L)
{
	// itemType:getShootType()
	if (const auto itemType = getUserdata<const ItemType>(L, 1)) {
		lua_pushinteger(L, itemType->shootType);
	}
	else {
		lua_pushnil(L);
	}
	return 1;
}



int LuaScriptInterface::luaItemTypeGetCorpseType(lua_State* L)
{
	// itemType:getCorpseType()
	if (const auto itemType = getUserdata<const ItemType>(L, 1)) {
		lua_pushinteger(L, itemType->corpseType);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetAbilities(lua_State* L)
{
	// itemType:getAbilities()
	if (const auto itemType = getUserdata<ItemType>(L, 1)) {
		Abilities& abilities = itemType->getAbilities();
		lua_createtable(L, 10, 12);
		setField(L, "healthGain", abilities.healthGain);
		setField(L, "healthTicks", abilities.healthTicks);
		setField(L, "manaGain", abilities.manaGain);
		setField(L, "manaTicks", abilities.manaTicks);
		setField(L, "conditionImmunities", abilities.conditionImmunities);
		setField(L, "conditionSuppressions", abilities.conditionSuppressions);
		setField(L, "speed", abilities.speed);
		setField(L, "elementDamage", abilities.elementDamage);
		setField(L, "elementType", abilities.elementType);

		lua_pushboolean(L, abilities.manaShield);
		lua_setfield(L, -2, "manaShield");
		lua_pushboolean(L, abilities.invisible);
		lua_setfield(L, -2, "invisible");
		lua_pushboolean(L, abilities.regeneration);
		lua_setfield(L, -2, "regeneration");

		// Stats
		lua_createtable(L, 0, STAT_LAST + 1);
		for (int32_t i = STAT_FIRST; i <= STAT_LAST; i++) {
			lua_pushinteger(L, abilities.stats[i]);
			lua_rawseti(L, -2, i + 1);
		}
		lua_setfield(L, -2, "stats");

		// Stats percent
		lua_createtable(L, 0, STAT_LAST + 1);
		for (int32_t i = STAT_FIRST; i <= STAT_LAST; i++) {
			lua_pushinteger(L, abilities.statsPercent[i]);
			lua_rawseti(L, -2, i + 1);
		}
		lua_setfield(L, -2, "statsPercent");

		// Skills
		lua_createtable(L, 0, SKILL_LAST + 1);
		for (int32_t i = SKILL_FIRST; i <= SKILL_LAST; i++) {
			lua_pushinteger(L, abilities.skills[i]);
			lua_rawseti(L, -2, i + 1);
		}
		lua_setfield(L, -2, "skills");

		// Special skills
		lua_createtable(L, 0, SPECIALSKILL_LAST + 1);
		for (int32_t i = SPECIALSKILL_FIRST; i <= SPECIALSKILL_LAST; i++) {
			lua_pushinteger(L, abilities.specialSkills[i]);
			lua_rawseti(L, -2, i + 1);
		}
		lua_setfield(L, -2, "specialSkills");
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeHasShowAttributes(lua_State* L)
{
	// itemType:hasShowAttributes()
	if (const auto itemType = getUserdata<const ItemType>(L, 1)) {
		pushBoolean(L, itemType->showAttributes);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeHasShowCount(lua_State* L)
{
	// itemType:hasShowCount()
	if (const auto itemType = getUserdata<const ItemType>(L, 1)) {
		pushBoolean(L, itemType->showCount);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeHasShowCharges(lua_State* L)
{
	// itemType:hasShowCharges()
	if (const auto itemType = getUserdata<const ItemType>(L, 1)) {
		pushBoolean(L, itemType->showCharges);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeHasShowDuration(lua_State* L)
{
	// itemType:hasShowDuration()
	if (const auto itemType = getUserdata<const ItemType>(L, 1)) {
		pushBoolean(L, itemType->showDuration);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeHasAllowDistRead(lua_State* L)
{
	// itemType:hasAllowDistRead()
	if (const auto itemType = getUserdata<const ItemType>(L, 1)) {
		pushBoolean(L, itemType->allowDistRead);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetWieldInfo(lua_State* L)
{
	// itemType:getWieldInfo()
	if (const auto itemType = getUserdata<const ItemType>(L, 1)) {
		lua_pushinteger(L, itemType->wieldInfo);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetDuration(lua_State* L)
{
	// itemType:getDuration()
	if (const auto itemType = getUserdata<const ItemType>(L, 1)) {
		lua_pushinteger(L, itemType->decayTime);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetLevelDoor(lua_State* L)
{
	// itemType:getLevelDoor()
	if (const auto itemType = getUserdata<const ItemType>(L, 1)) {
		lua_pushinteger(L, itemType->levelDoor);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetVocationString(lua_State* L)
{
	// itemType:getVocationString()
	const auto itemType = getUserdata<const ItemType>(L, 1);
	if (itemType) {
		pushString(L, itemType->vocationString);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetMinReqLevel(lua_State* L)
{
	// itemType:getMinReqLevel()
	if (const auto itemType = getUserdata<const ItemType>(L, 1)) {
		lua_pushinteger(L, itemType->minReqLevel);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetMinReqMagicLevel(lua_State* L)
{
	// itemType:getMinReqMagicLevel()
	if (const auto itemType = getUserdata<const ItemType>(L, 1)) {
		lua_pushinteger(L, itemType->minReqMagicLevel);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetElementType(lua_State* L)
{
	// itemType:getElementType()
	const auto itemType = getUserdata<const ItemType>(L, 1);
	if (!itemType) {
		lua_pushnil(L);
		return 1;
	}

	if (auto& abilities = itemType->abilities) {
		lua_pushinteger(L, abilities->elementType);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetElementDamage(lua_State* L)
{
	// itemType:getElementDamage()
	const auto itemType = getUserdata<const ItemType>(L, 1);
	if (!itemType) {
		lua_pushnil(L);
		return 1;
	}

	if (auto& abilities = itemType->abilities) {
		lua_pushinteger(L, abilities->elementDamage);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetTransformEquipId(lua_State* L)
{
	// itemType:getTransformEquipId()
	if (const auto itemType = getUserdata<const ItemType>(L, 1)) {
		lua_pushinteger(L, itemType->transformEquipTo);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetTransformDeEquipId(lua_State* L)
{
	// itemType:getTransformDeEquipId()
	if (const auto itemType = getUserdata<const ItemType>(L, 1)) {
		lua_pushinteger(L, itemType->transformDeEquipTo);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetDestroyId(lua_State* L)
{
	// itemType:getDestroyId()
	if (const auto itemType = getUserdata<const ItemType>(L, 1)) {
		lua_pushinteger(L, itemType->destroyTo);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetDecayId(lua_State* L)
{
	// itemType:getDecayId()
	if (const auto itemType = getUserdata<const ItemType>(L, 1)) {
		lua_pushinteger(L, itemType->decayTo);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeGetRequiredLevel(lua_State* L)
{
	// itemType:getRequiredLevel()
	if (const auto itemType = getUserdata<const ItemType>(L, 1)) {
		lua_pushinteger(L, itemType->minReqLevel);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeHasSubType(lua_State* L)
{
	// itemType:hasSubType()
	if (const auto itemType = getUserdata<const ItemType>(L, 1)) {
		pushBoolean(L, itemType->hasSubType());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaItemTypeIsStoreItem(lua_State* L)
{
	// itemType:isStoreItem()
	if (const auto itemType = getUserdata<const ItemType>(L, 1)) {
		pushBoolean(L, itemType->storeItem);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// Combat
int LuaScriptInterface::luaCombatCreate(lua_State* L)
{
	// Combat()
	pushSharedPtr(L, g_luaEnvironment.createCombatObject(getScriptEnv()->getScriptInterface()));
	setMetatable(L, -1, "Combat");
	return 1;
}

int LuaScriptInterface::luaCombatDelete(lua_State* L)
{
	if (Combat_ptr& combat = getSharedPtr<Combat>(L, 1)) {
		combat.reset();
	}
	return 0;
}

int LuaScriptInterface::luaCombatSetParameter(lua_State* L)
{
	// combat:setParameter(key, value)
	const Combat_ptr& combat = getSharedPtr<Combat>(L, 1);
	if (!combat) {
		reportErrorFunc(L, getErrorDesc(LUA_ERROR_COMBAT_NOT_FOUND));
		lua_pushnil(L);
		return 1;
	}

	CombatParam_t key = getNumber<CombatParam_t>(L, 2);
	uint32_t value;
	if (isBoolean(L, 3)) {
		value = getBoolean(L, 3) ? 1 : 0;
	} else {
		value = getNumber<uint32_t>(L, 3);
	}
	combat->setParam(key, value);
	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaCombatGetParameter(lua_State* L)
{
	// combat:getParameter(key)
	const Combat_ptr& combat = getSharedPtr<Combat>(L, 1);
	if (!combat) {
		reportErrorFunc(L, getErrorDesc(LUA_ERROR_COMBAT_NOT_FOUND));
		lua_pushnil(L);
		return 1;
	}

	int32_t value = combat->getParam(getNumber<CombatParam_t>(L, 2));
	if (value == std::numeric_limits<int32_t>().max()) {
		lua_pushnil(L);
		return 1;
	}

	lua_pushinteger(L, value);
	return 1;
}

int LuaScriptInterface::luaCombatSetFormula(lua_State* L)
{
	// combat:setFormula(type, mina, minb, maxa, maxb)
	const Combat_ptr& combat = getSharedPtr<Combat>(L, 1);
	if (!combat) {
		reportErrorFunc(L, getErrorDesc(LUA_ERROR_COMBAT_NOT_FOUND));
		lua_pushnil(L);
		return 1;
	}

	formulaType_t type = getNumber<formulaType_t>(L, 2);
	double mina = getNumber<double>(L, 3);
	double minb = getNumber<double>(L, 4);
	double maxa = getNumber<double>(L, 5);
	double maxb = getNumber<double>(L, 6);
	combat->setPlayerCombatValues(type, mina, minb, maxa, maxb);
	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaCombatSetArea(lua_State* L)
{
	// combat:setArea(area)
	if (getScriptEnv()->getScriptId() != EVENT_ID_LOADING) {
		reportErrorFunc(L, "This function can only be used while loading the script.");
		lua_pushnil(L);
		return 1;
	}

	const AreaCombat* area = g_luaEnvironment.getAreaObject(getNumber<uint32_t>(L, 2));
	if (!area) {
		reportErrorFunc(L, getErrorDesc(LUA_ERROR_AREA_NOT_FOUND));
		lua_pushnil(L);
		return 1;
	}

	const Combat_ptr& combat = getSharedPtr<Combat>(L, 1);
	if (!combat) {
		reportErrorFunc(L, getErrorDesc(LUA_ERROR_COMBAT_NOT_FOUND));
		lua_pushnil(L);
		return 1;
	}

	combat->setArea(new AreaCombat(*area));
	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaCombatAddCondition(lua_State* L)
{
	// combat:addCondition(condition)
	const Combat_ptr& combat = getSharedPtr<Combat>(L, 1);
	if (!combat) {
		reportErrorFunc(L, getErrorDesc(LUA_ERROR_COMBAT_NOT_FOUND));
		lua_pushnil(L);
		return 1;
	}

	Condition* condition = getUserdata<Condition>(L, 2);
	if (condition) {
		combat->addCondition(condition->clone());
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCombatClearConditions(lua_State* L)
{
	// combat:clearConditions()
	const Combat_ptr& combat = getSharedPtr<Combat>(L, 1);
	if (!combat) {
		reportErrorFunc(L, getErrorDesc(LUA_ERROR_COMBAT_NOT_FOUND));
		lua_pushnil(L);
		return 1;
	}

	combat->clearConditions();
	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaCombatSetCallback(lua_State* L)
{
	// combat:setCallback(key, function)
	const Combat_ptr& combat = getSharedPtr<Combat>(L, 1);
	if (!combat) {
		reportErrorFunc(L, getErrorDesc(LUA_ERROR_COMBAT_NOT_FOUND));
		lua_pushnil(L);
		return 1;
	}

	CallBackParam_t key = getNumber<CallBackParam_t>(L, 2);
	if (!combat->setCallback(key)) {
		lua_pushnil(L);
		return 1;
	}

	CallBack* callback = combat->getCallback(key);
	if (!callback) {
		lua_pushnil(L);
		return 1;
	}

	const std::string& function = getString(L, 3);
	pushBoolean(L, callback->loadCallBack(getScriptEnv()->getScriptInterface(), function));
	return 1;
}

int LuaScriptInterface::luaCombatSetOrigin(lua_State* L)
{
	// combat:setOrigin(origin)
	const Combat_ptr& combat = getSharedPtr<Combat>(L, 1);
	if (!combat) {
		reportErrorFunc(L, getErrorDesc(LUA_ERROR_COMBAT_NOT_FOUND));
		lua_pushnil(L);
		return 1;
	}

	combat->setOrigin(getNumber<CombatOrigin>(L, 2));
	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaCombatExecute(lua_State* L)
{
	// combat:execute(creature, variant)
	const Combat_ptr& combat = getSharedPtr<Combat>(L, 1);
	if (!combat) {
		reportErrorFunc(L, getErrorDesc(LUA_ERROR_COMBAT_NOT_FOUND));
		lua_pushnil(L);
		return 1;
	}

	if (isUserdata(L, 2)) {
		LuaDataType type = getUserdataType(L, 2);
		if (type != LuaData_Player && type != LuaData_Monster && type != LuaData_Npc) {
			pushBoolean(L, false);
			return 1;
		}
	}

	auto creature = getCreature(L, 2);

	const LuaVariant& variant = getVariant(L, 3);
	switch (variant.type()) {
		case VARIANT_NUMBER: {
			const auto target = g_game.getCreatureByID(variant.getNumber());
			if (!target) {
				pushBoolean(L, false);
				return 1;
			}

			if (combat->hasArea()) {
				combat->doCombat(creature, target->getPosition());
			} else {
				combat->doCombat(creature, target);
			}
			break;
		}

		case VARIANT_POSITION: {
			combat->doCombat(creature, variant.getPosition());
			break;
		}

		case VARIANT_TARGETPOSITION: {
			if (combat->hasArea()) {
				combat->doCombat(creature, variant.getTargetPosition());
			} else {
				combat->postCombatEffects(creature, variant.getTargetPosition());
				g_game.addMagicEffect(variant.getTargetPosition(), CONST_ME_POFF);
			}
			break;
		}

		case VARIANT_STRING: {
			const auto target = g_game.getPlayerByName(variant.getString());
			if (!target) {
				pushBoolean(L, false);
				return 1;
			}

			combat->doCombat(creature, target);
			break;
		}

		case VARIANT_NONE: {
			reportErrorFunc(L, getErrorDesc(LUA_ERROR_VARIANT_NOT_FOUND));
			pushBoolean(L, false);
			return 1;
		}

		default: {
			break;
		}
	}

	pushBoolean(L, true);
	return 1;
}

// Condition
int LuaScriptInterface::luaConditionCreate(lua_State* L)
{
	// Condition(conditionType[, conditionId = CONDITIONID_COMBAT])
	ConditionType_t conditionType = getNumber<ConditionType_t>(L, 2);
	ConditionId_t conditionId = getNumber<ConditionId_t>(L, 3, CONDITIONID_COMBAT);

	Condition* condition = Condition::createCondition(conditionId, conditionType, 0, 0);
	if (condition) {
		pushUserdata<Condition>(L, condition);
		setMetatable(L, -1, "Condition");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaConditionDelete(lua_State* L)
{
	// condition:delete()
	Condition** conditionPtr = getRawUserdata<Condition>(L, 1);
	if (conditionPtr && *conditionPtr) {
		delete *conditionPtr;
		*conditionPtr = nullptr;
	}
	return 0;
}

int LuaScriptInterface::luaConditionGetId(lua_State* L)
{
	// condition:getId()
	Condition* condition = getUserdata<Condition>(L, 1);
	if (condition) {
		lua_pushinteger(L, condition->getId());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaConditionGetSubId(lua_State* L)
{
	// condition:getSubId()
	Condition* condition = getUserdata<Condition>(L, 1);
	if (condition) {
		lua_pushinteger(L, condition->getSubId());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaConditionGetType(lua_State* L)
{
	// condition:getType()
	Condition* condition = getUserdata<Condition>(L, 1);
	if (condition) {
		lua_pushinteger(L, condition->getType());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaConditionGetIcons(lua_State* L)
{
	// condition:getIcons()
	Condition* condition = getUserdata<Condition>(L, 1);
	if (condition) {
		lua_pushinteger(L, condition->getIcons());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaConditionGetEndTime(lua_State* L)
{
	// condition:getEndTime()
	Condition* condition = getUserdata<Condition>(L, 1);
	if (condition) {
		lua_pushinteger(L, condition->getEndTime());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaConditionClone(lua_State* L)
{
	// condition:clone()
	Condition* condition = getUserdata<Condition>(L, 1);
	if (condition) {
		pushUserdata<Condition>(L, condition->clone());
		setMetatable(L, -1, "Condition");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaConditionGetTicks(lua_State* L)
{
	// condition:getTicks()
	Condition* condition = getUserdata<Condition>(L, 1);
	if (condition) {
		lua_pushinteger(L, condition->getTicks());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaConditionSetTicks(lua_State* L)
{
	// condition:setTicks(ticks)
	int32_t ticks = getNumber<int32_t>(L, 2);
	Condition* condition = getUserdata<Condition>(L, 1);
	if (condition) {
		condition->setTicks(ticks);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaConditionSetParameter(lua_State* L)
{
	// condition:setParameter(key, value)
	Condition* condition = getUserdata<Condition>(L, 1);
	if (!condition) {
		lua_pushnil(L);
		return 1;
	}

	ConditionParam_t key = getNumber<ConditionParam_t>(L, 2);
	int32_t value;
	if (isBoolean(L, 3)) {
		value = getBoolean(L, 3) ? 1 : 0;
	} else {
		value = getNumber<int32_t>(L, 3);
	}
	condition->setParam(key, value);
	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaConditionGetParameter(lua_State* L)
{
	// condition:getParameter(key)
	Condition* condition = getUserdata<Condition>(L, 1);
	if (!condition) {
		lua_pushnil(L);
		return 1;
	}

	int32_t value = condition->getParam(getNumber<ConditionParam_t>(L, 2));
	if (value == std::numeric_limits<int32_t>().max()) {
		lua_pushnil(L);
		return 1;
	}

	lua_pushinteger(L, value);
	return 1;
}

int LuaScriptInterface::luaConditionSetFormula(lua_State* L)
{
	// condition:setFormula(mina, minb, maxa, maxb)
	double maxb = getNumber<double>(L, 5);
	double maxa = getNumber<double>(L, 4);
	double minb = getNumber<double>(L, 3);
	double mina = getNumber<double>(L, 2);
	ConditionSpeed* condition = dynamic_cast<ConditionSpeed*>(getUserdata<Condition>(L, 1));
	if (condition) {
		condition->setFormulaVars(mina, minb, maxa, maxb);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaConditionSetOutfit(lua_State* L)
{
	// condition:setOutfit(outfit)
	// condition:setOutfit(lookTypeEx, lookType, lookHead, lookBody, lookLegs, lookFeet[, lookAddons[, lookMount]])
	Outfit_t outfit;
	if (isTable(L, 2)) {
		outfit = getOutfit(L, 2);
	} else {
		outfit.lookMount = getNumber<uint16_t>(L, 9, outfit.lookMount);
		outfit.lookAddons = getNumber<uint8_t>(L, 8, outfit.lookAddons);
		outfit.lookFeet = getNumber<uint8_t>(L, 7);
		outfit.lookLegs = getNumber<uint8_t>(L, 6);
		outfit.lookBody = getNumber<uint8_t>(L, 5);
		outfit.lookHead = getNumber<uint8_t>(L, 4);
		outfit.lookType = getNumber<uint16_t>(L, 3);
		outfit.lookTypeEx = getNumber<uint16_t>(L, 2);
	}

	ConditionOutfit* condition = dynamic_cast<ConditionOutfit*>(getUserdata<Condition>(L, 1));
	if (condition) {
		condition->setOutfit(outfit);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaConditionAddDamage(lua_State* L)
{
	// condition:addDamage(rounds, time, value)
	int32_t value = getNumber<int32_t>(L, 4);
	int32_t time = getNumber<int32_t>(L, 3);
	int32_t rounds = getNumber<int32_t>(L, 2);
	ConditionDamage* condition = dynamic_cast<ConditionDamage*>(getUserdata<Condition>(L, 1));
	if (condition) {
		pushBoolean(L, condition->addDamage(rounds, time, value));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// Outfit
int LuaScriptInterface::luaOutfitCreate(lua_State* L)
{
	// Outfit(looktype)
	const Outfit* outfit = Outfits::getInstance().getOutfitByLookType(getNumber<uint16_t>(L, 2));
	if (outfit) {
		pushOutfit(L, outfit);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaOutfitCompare(lua_State* L)
{
	// outfit == outfitEx
	Outfit outfitEx = getOutfitClass(L, 2);
	Outfit outfit = getOutfitClass(L, 1);
	pushBoolean(L, outfit == outfitEx);
	return 1;
}

// MonsterType
int LuaScriptInterface::luaMonsterTypeCreate(lua_State* L)
{
	// MonsterType(name)
	MonsterType* monsterType = g_monsters.getMonsterType(getString(L, 2));
	if (monsterType) {
		pushUserdata<MonsterType>(L, monsterType);
		setMetatable(L, -1, "MonsterType");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeIsAttackable(lua_State* L)
{
	// get: monsterType:isAttackable() set: monsterType:isAttackable(bool)
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			pushBoolean(L, monsterType->info.isAttackable);
		} else {
			monsterType->info.isAttackable = getBoolean(L, 2);
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeIsRewardBoss(lua_State* L)
{
	// lua monsterType:isRewardBoss()
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		pushBoolean(L, monsterType->info.isRewardBoss);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeIsChallengeable(lua_State* L)
{
	// get: monsterType:isChallengeable() set: monsterType:isChallengeable(bool)
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			pushBoolean(L, monsterType->info.isChallengeable);
		} else {
			monsterType->info.isChallengeable = getBoolean(L, 2);
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeIsConvinceable(lua_State* L)
{
	// get: monsterType:isConvinceable() set: monsterType:isConvinceable(bool)
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			pushBoolean(L, monsterType->info.isConvinceable);
		} else {
			monsterType->info.isConvinceable = getBoolean(L, 2);
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeIsSummonable(lua_State* L)
{
	// get: monsterType:isSummonable() set: monsterType:isSummonable(bool)
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			pushBoolean(L, monsterType->info.isSummonable);
		} else {
			monsterType->info.isSummonable = getBoolean(L, 2);
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeIsIgnoringSpawnBlock(lua_State* L)
{
	// get: monsterType:isIgnoringSpawnBlock() set: monsterType:isIgnoringSpawnBlock(bool)
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			pushBoolean(L, monsterType->info.isIgnoringSpawnBlock);
		} else {
			monsterType->info.isIgnoringSpawnBlock = getBoolean(L, 2);
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeIsIllusionable(lua_State* L)
{
	// get: monsterType:isIllusionable() set: monsterType:isIllusionable(bool)
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			pushBoolean(L, monsterType->info.isIllusionable);
		} else {
			monsterType->info.isIllusionable = getBoolean(L, 2);
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeIsHostile(lua_State* L)
{
	// get: monsterType:isHostile() set: monsterType:isHostile(bool)
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			pushBoolean(L, monsterType->info.isHostile);
		} else {
			monsterType->info.isHostile = getBoolean(L, 2);
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeIsPushable(lua_State* L)
{
	// get: monsterType:isPushable() set: monsterType:isPushable(bool)
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			pushBoolean(L, monsterType->info.pushable);
		} else {
			monsterType->info.pushable = getBoolean(L, 2);
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeIsHealthHidden(lua_State* L)
{
	// get: monsterType:isHealthHidden() set: monsterType:isHealthHidden(bool)
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			pushBoolean(L, monsterType->info.hiddenHealth);
		} else {
			monsterType->info.hiddenHealth = getBoolean(L, 2);
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeIsBoss(lua_State* L)
{
	// get: monsterType:isBoss() set: monsterType:isBoss(bool)
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			pushBoolean(L, monsterType->info.isBoss);
		} else {
			monsterType->info.isBoss = getBoolean(L, 2);
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeCanPushItems(lua_State* L)
{
	// get: monsterType:canPushItems() set: monsterType:canPushItems(bool)
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			pushBoolean(L, monsterType->info.canPushItems);
		} else {
			monsterType->info.canPushItems = getBoolean(L, 2);
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeCanPushCreatures(lua_State* L)
{
	// get: monsterType:canPushCreatures() set: monsterType:canPushCreatures(bool)
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			pushBoolean(L, monsterType->info.canPushCreatures);
		} else {
			monsterType->info.canPushCreatures = getBoolean(L, 2);
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeCanWalkOnEnergy(lua_State* L)
{
	// get: monsterType:canWalkOnEnergy() set: monsterType:canWalkOnEnergy(bool)
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			pushBoolean(L, monsterType->info.canWalkOnEnergy);
		} else {
			monsterType->info.canWalkOnEnergy = getBoolean(L, 2);
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeCanWalkOnFire(lua_State* L)
{
	// get: monsterType:canWalkOnFire() set: monsterType:canWalkOnFire(bool)
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			pushBoolean(L, monsterType->info.canWalkOnFire);
		} else {
			monsterType->info.canWalkOnFire = getBoolean(L, 2);
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeCanWalkOnPoison(lua_State* L)
{
	// get: monsterType:canWalkOnPoison() set: monsterType:canWalkOnPoison(bool)
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			pushBoolean(L, monsterType->info.canWalkOnPoison);
		} else {
			monsterType->info.canWalkOnPoison = getBoolean(L, 2);
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int32_t LuaScriptInterface::luaMonsterTypeName(lua_State* L)
{
	// get: monsterType:name() set: monsterType:name(name)
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			pushString(L, monsterType->name);
		} else {
			monsterType->name = getString(L, 2);
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeNameDescription(lua_State* L)
{
	// get: monsterType:nameDescription() set: monsterType:nameDescription(desc)
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			pushString(L, monsterType->nameDescription);
		} else {
			monsterType->nameDescription = getString(L, 2);
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeHealth(lua_State* L)
{
	// get: monsterType:health() set: monsterType:health(health)
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			lua_pushinteger(L, monsterType->info.health);
		} else {
			monsterType->info.health = getNumber<int32_t>(L, 2);
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeMaxHealth(lua_State* L)
{
	// get: monsterType:maxHealth() set: monsterType:maxHealth(health)
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			lua_pushinteger(L, monsterType->info.healthMax);
		} else {
			monsterType->info.healthMax = getNumber<int32_t>(L, 2);
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeRunHealth(lua_State* L)
{
	// get: monsterType:runHealth() set: monsterType:runHealth(health)
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			lua_pushinteger(L, monsterType->info.runAwayHealth);
		} else {
			monsterType->info.runAwayHealth = getNumber<int32_t>(L, 2);
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeExperience(lua_State* L)
{
	// get: monsterType:experience() set: monsterType:experience(exp)
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			lua_pushinteger(L, monsterType->info.experience);
		} else {
			monsterType->info.experience = getNumber<uint64_t>(L, 2);
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeSkull(lua_State* L)
{
	// get: monsterType:skull() set: monsterType:skull(str/constant)
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			lua_pushinteger(L, monsterType->info.skull);
		} else {
			if (isNumber(L, 2)) {
				monsterType->info.skull = getNumber<Skulls_t>(L, 2);
			} else {
				monsterType->info.skull = getSkullType(getString(L, 2));
			}
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeCombatImmunities(lua_State* L)
{
	// get: monsterType:combatImmunities() set: monsterType:combatImmunities(immunity)
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			lua_pushinteger(L, monsterType->info.damageImmunities);
		} else {
			std::string immunity = getString(L, 2);
			if (immunity == "physical") {
				monsterType->info.damageImmunities |= COMBAT_PHYSICALDAMAGE;
				pushBoolean(L, true);
			} else if (immunity == "energy") {
				monsterType->info.damageImmunities |= COMBAT_ENERGYDAMAGE;
				pushBoolean(L, true);
			} else if (immunity == "fire") {
				monsterType->info.damageImmunities |= COMBAT_FIREDAMAGE;
				pushBoolean(L, true);
			} else if (immunity == "poison" || immunity == "earth") {
				monsterType->info.damageImmunities |= COMBAT_EARTHDAMAGE;
				pushBoolean(L, true);
			} else if (immunity == "drown") {
				monsterType->info.damageImmunities |= COMBAT_DROWNDAMAGE;
				pushBoolean(L, true);
			} else if (immunity == "ice") {
				monsterType->info.damageImmunities |= COMBAT_ICEDAMAGE;
				pushBoolean(L, true);
			} else if (immunity == "holy") {
				monsterType->info.damageImmunities |= COMBAT_HOLYDAMAGE;
				pushBoolean(L, true);
			} else if (immunity == "death") {
				monsterType->info.damageImmunities |= COMBAT_DEATHDAMAGE;
				pushBoolean(L, true);
			} else if (immunity == "lifedrain") {
				monsterType->info.damageImmunities |= COMBAT_LIFEDRAIN;
				pushBoolean(L, true);
			} else if (immunity == "manadrain") {
				monsterType->info.damageImmunities |= COMBAT_MANADRAIN;
				pushBoolean(L, true);
			} else {
				std::cout << "[Warning - Monsters::loadMonster] Unknown immunity name " << immunity << " for monster: " << monsterType->name << std::endl;
				lua_pushnil(L);
			}
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeConditionImmunities(lua_State* L)
{
	// get: monsterType:conditionImmunities() set: monsterType:conditionImmunities(immunity)
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			lua_pushinteger(L, monsterType->info.conditionImmunities);
		} else {
			std::string immunity = getString(L, 2);
			if (immunity == "physical") {
				monsterType->info.conditionImmunities |= CONDITION_BLEEDING;
				pushBoolean(L, true);
			} else if (immunity == "energy") {
				monsterType->info.conditionImmunities |= CONDITION_ENERGY;
				pushBoolean(L, true);
			} else if (immunity == "fire") {
				monsterType->info.conditionImmunities |= CONDITION_FIRE;
				pushBoolean(L, true);
			} else if (immunity == "poison" || immunity == "earth") {
				monsterType->info.conditionImmunities |= CONDITION_POISON;
				pushBoolean(L, true);
			} else if (immunity == "drown") {
				monsterType->info.conditionImmunities |= CONDITION_DROWN;
				pushBoolean(L, true);
			} else if (immunity == "ice") {
				monsterType->info.conditionImmunities |= CONDITION_FREEZING;
				pushBoolean(L, true);
			} else if (immunity == "holy") {
				monsterType->info.conditionImmunities |= CONDITION_DAZZLED;
				pushBoolean(L, true);
			} else if (immunity == "death") {
				monsterType->info.conditionImmunities |= CONDITION_CURSED;
				pushBoolean(L, true);
			} else if (immunity == "paralyze") {
				monsterType->info.conditionImmunities |= CONDITION_PARALYZE;
				pushBoolean(L, true);
			} else if (immunity == "outfit") {
				monsterType->info.conditionImmunities |= CONDITION_OUTFIT;
				pushBoolean(L, true);
			} else if (immunity == "drunk") {
				monsterType->info.conditionImmunities |= CONDITION_DRUNK;
				pushBoolean(L, true);
			} else if (immunity == "invisible" || immunity == "invisibility") {
				monsterType->info.conditionImmunities |= CONDITION_INVISIBLE;
				pushBoolean(L, true);
			} else if (immunity == "bleed") {
				monsterType->info.conditionImmunities |= CONDITION_BLEEDING;
				pushBoolean(L, true);
			} else {
				std::cout << "[Warning - Monsters::loadMonster] Unknown immunity name " << immunity << " for monster: " << monsterType->name << std::endl;
				lua_pushnil(L);
			}
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeGetAttackList(lua_State* L)
{
	// monsterType:getAttackList()
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (!monsterType) {
		lua_pushnil(L);
		return 1;
	}

	lua_createtable(L, monsterType->info.attackSpells.size(), 0);

	int index = 0;
	for (const auto& spellBlock : monsterType->info.attackSpells) {
		lua_createtable(L, 0, 8);

		setField(L, "chance", spellBlock.chance);
		setField(L, "isCombatSpell", spellBlock.combatSpell ? 1 : 0);
		setField(L, "isMelee", spellBlock.isMelee ? 1 : 0);
		setField(L, "minCombatValue", spellBlock.minCombatValue);
		setField(L, "maxCombatValue", spellBlock.maxCombatValue);
		setField(L, "range", spellBlock.range);
		setField(L, "speed", spellBlock.speed);
		pushUserdata<CombatSpell>(L, static_cast<CombatSpell*>(spellBlock.spell));
		lua_setfield(L, -2, "spell");

		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeAddAttack(lua_State* L)
{
	// monsterType:addAttack(monsterspell)
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		MonsterSpell* spell = getUserdata<MonsterSpell>(L, 2);
		if (spell) {
			spellBlock_t sb;
			if (g_monsters.deserializeSpell(spell, sb, monsterType->name)) {
				monsterType->info.attackSpells.push_back(std::move(sb));
			} else {
				std::cout << monsterType->name << std::endl;
				std::cout << "[Warning - Monsters::loadMonster] Cant load spell. " << spell->name << std::endl;
			}
		} else {
			lua_pushnil(L);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeGetDefenseList(lua_State* L)
{
	// monsterType:getDefenseList()
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (!monsterType) {
		lua_pushnil(L);
		return 1;
	}

	lua_createtable(L, monsterType->info.defenseSpells.size(), 0);

	int index = 0;
	for (const auto& spellBlock : monsterType->info.defenseSpells) {
		lua_createtable(L, 0, 8);

		setField(L, "chance", spellBlock.chance);
		setField(L, "isCombatSpell", spellBlock.combatSpell ? 1 : 0);
		setField(L, "isMelee", spellBlock.isMelee ? 1 : 0);
		setField(L, "minCombatValue", spellBlock.minCombatValue);
		setField(L, "maxCombatValue", spellBlock.maxCombatValue);
		setField(L, "range", spellBlock.range);
		setField(L, "speed", spellBlock.speed);
		pushUserdata<CombatSpell>(L, static_cast<CombatSpell*>(spellBlock.spell));
		lua_setfield(L, -2, "spell");

		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeAddDefense(lua_State* L)
{
	// monsterType:addDefense(monsterspell)
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		MonsterSpell* spell = getUserdata<MonsterSpell>(L, 2);
		if (spell) {
			spellBlock_t sb;
			if (g_monsters.deserializeSpell(spell, sb, monsterType->name)) {
				monsterType->info.defenseSpells.push_back(std::move(sb));
			} else {
				std::cout << monsterType->name << std::endl;
				std::cout << "[Warning - Monsters::loadMonster] Cant load spell. " << spell->name << std::endl;
			}
		} else {
			lua_pushnil(L);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeGetElementList(lua_State* L)
{
	// monsterType:getElementList()
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (!monsterType) {
		lua_pushnil(L);
		return 1;
	}

	lua_createtable(L, monsterType->info.elementMap.size(), 0);
	for (const auto& elementEntry : monsterType->info.elementMap) {
		lua_pushinteger(L, elementEntry.second);
		lua_rawseti(L, -2, elementEntry.first);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeAddElement(lua_State* L)
{
	// monsterType:addElement(type, percent)
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		CombatType_t element = getNumber<CombatType_t>(L, 2);
		monsterType->info.elementMap[element] = getNumber<int32_t>(L, 3);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeGetVoices(lua_State* L)
{
	// monsterType:getVoices()
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (!monsterType) {
		lua_pushnil(L);
		return 1;
	}

	int index = 0;
	lua_createtable(L, monsterType->info.voiceVector.size(), 0);
	for (const auto& voiceBlock : monsterType->info.voiceVector) {
		lua_createtable(L, 0, 2);
		setField(L, "text", voiceBlock.text);
		setField(L, "yellText", voiceBlock.yellText);
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeAddVoice(lua_State* L)
{
	// monsterType:addVoice(sentence, interval, chance, yell)
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		voiceBlock_t voice;
		voice.text = getString(L, 2);
		monsterType->info.yellSpeedTicks = getNumber<uint32_t>(L, 3);
		monsterType->info.yellChance = getNumber<uint32_t>(L, 4);
		voice.yellText = getBoolean(L, 5);
		monsterType->info.voiceVector.push_back(voice);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeGetLoot(lua_State* L)
{
	// monsterType:getLoot()
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (!monsterType) {
		lua_pushnil(L);
		return 1;
	}

	pushLoot(L, monsterType->info.lootItems);
	return 1;
}

int LuaScriptInterface::luaMonsterTypeAddLoot(lua_State* L)
{
	// monsterType:addLoot(loot)
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		Loot* loot = getUserdata<Loot>(L, 2);
		if (loot) {
			monsterType->loadLoot(monsterType, loot->lootBlock);
			pushBoolean(L, true);
		} else {
			lua_pushnil(L);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeGetCreatureEvents(lua_State* L)
{
	// monsterType:getCreatureEvents()
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (!monsterType) {
		lua_pushnil(L);
		return 1;
	}

	int index = 0;
	lua_createtable(L, monsterType->info.scripts.size(), 0);
	for (const std::string& creatureEvent : monsterType->info.scripts) {
		pushString(L, creatureEvent);
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeRegisterEvent(lua_State* L)
{
	// monsterType:registerEvent(name)
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		monsterType->info.scripts.push_back(getString(L, 2));
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeEventOnCallback(lua_State* L)
{
	// monsterType:onThink(callback)
	// monsterType:onAppear(callback)
	// monsterType:onDisappear(callback)
	// monsterType:onMove(callback)
	// monsterType:onSay(callback)
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (monsterType->loadCallback(&g_scripts->getScriptInterface())) {
			pushBoolean(L, true);
			return 1;
		}
		pushBoolean(L, false);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeEventType(lua_State* L)
{
	// monstertype:eventType(event)
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		monsterType->info.eventType = getNumber<MonstersEvent_t>(L, 2);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeGetSummonList(lua_State* L)
{
	// monsterType:getSummonList()
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (!monsterType) {
		lua_pushnil(L);
		return 1;
	}

	int index = 0;
	lua_createtable(L, monsterType->info.summons.size(), 0);
	for (const auto& summonBlock : monsterType->info.summons) {
		lua_createtable(L, 0, 6);
		setField(L, "name", summonBlock.name);
		setField(L, "speed", summonBlock.speed);
		setField(L, "chance", summonBlock.chance);
		setField(L, "max", summonBlock.max);
		setField(L, "effect", summonBlock.effect);
		setField(L, "masterEffect", summonBlock.masterEffect);
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeAddSummon(lua_State* L)
{
	// monsterType:addSummon(name, interval, chance[, max = -1[, effect = CONST_ME_TELEPORT[, masterEffect = CONST_ME_NONE]]])
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		summonBlock_t summon;
		summon.name = getString(L, 2);
		summon.speed = getNumber<int32_t>(L, 3);
		summon.chance = getNumber<int32_t>(L, 4);
		summon.max = getNumber<int32_t>(L, 5, -1);
		summon.effect = getNumber<MagicEffectClasses>(L, 6, CONST_ME_TELEPORT);
		summon.masterEffect = getNumber<MagicEffectClasses>(L, 7, CONST_ME_NONE);
		monsterType->info.summons.push_back(summon);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeMaxSummons(lua_State* L)
{
	// get: monsterType:maxSummons() set: monsterType:maxSummons(ammount)
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			lua_pushinteger(L, monsterType->info.maxSummons);
		} else {
			monsterType->info.maxSummons = getNumber<uint32_t>(L, 2);
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeArmor(lua_State* L)
{
	// get: monsterType:armor() set: monsterType:armor(armor)
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			lua_pushinteger(L, monsterType->info.armor);
		} else {
			monsterType->info.armor = getNumber<int32_t>(L, 2);
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeDefense(lua_State* L)
{
	// get: monsterType:defense() set: monsterType:defense(defense)
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			lua_pushinteger(L, monsterType->info.defense);
		} else {
			monsterType->info.defense = getNumber<int32_t>(L, 2);
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeOutfit(lua_State* L)
{
	// get: monsterType:outfit() set: monsterType:outfit(outfit)
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			pushOutfit(L, monsterType->info.outfit);
		} else {
			monsterType->info.outfit = getOutfit(L, 2);
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeRace(lua_State* L)
{
	// get: monsterType:race() set: monsterType:race(race)
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	std::string race = getString(L, 2);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			lua_pushinteger(L, monsterType->info.race);
		} else {
			if (race == "venom") {
				monsterType->info.race = RACE_VENOM;
			} else if (race == "blood") {
				monsterType->info.race = RACE_BLOOD;
			} else if (race == "undead") {
				monsterType->info.race = RACE_UNDEAD;
			} else if (race == "fire") {
				monsterType->info.race = RACE_FIRE;
			} else if (race == "energy") {
				monsterType->info.race = RACE_ENERGY;
			} else {
				std::cout << "[Warning - Monsters::loadMonster] Unknown race type " << race << "." << std::endl;
				lua_pushnil(L);
				return 1;
			}
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeCorpseId(lua_State* L)
{
	// get: monsterType:corpseId() set: monsterType:corpseId(id)
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			lua_pushinteger(L, monsterType->info.lookcorpse);
		} else {
			monsterType->info.lookcorpse = getNumber<uint16_t>(L, 2);
			lua_pushboolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeManaCost(lua_State* L)
{
	// get: monsterType:manaCost() set: monsterType:manaCost(mana)
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			lua_pushinteger(L, monsterType->info.manaCost);
		} else {
			monsterType->info.manaCost = getNumber<uint32_t>(L, 2);
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeBaseSpeed(lua_State* L)
{
	// get: monsterType:baseSpeed() set: monsterType:baseSpeed(speed)
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			lua_pushinteger(L, monsterType->info.baseSpeed);
		} else {
			monsterType->info.baseSpeed = getNumber<uint32_t>(L, 2);
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeLight(lua_State* L)
{
	// get: monsterType:light() set: monsterType:light(color, level)
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (!monsterType) {
		lua_pushnil(L);
		return 1;
	}
	if (lua_gettop(L) == 1) {
		lua_pushinteger(L, monsterType->info.light.level);
		lua_pushinteger(L, monsterType->info.light.color);
		return 2;
	} else {
		monsterType->info.light.color = getNumber<uint8_t>(L, 2);
		monsterType->info.light.level = getNumber<uint8_t>(L, 3);
		pushBoolean(L, true);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeStaticAttackChance(lua_State* L)
{
	// get: monsterType:staticAttackChance() set: monsterType:staticAttackChance(chance)
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			lua_pushinteger(L, monsterType->info.staticAttackChance);
		} else {
			monsterType->info.staticAttackChance = getNumber<uint32_t>(L, 2);
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeTargetDistance(lua_State* L)
{
	// get: monsterType:targetDistance() set: monsterType:targetDistance(distance)
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			lua_pushinteger(L, monsterType->info.targetDistance);
		} else {
			monsterType->info.targetDistance = getNumber<int32_t>(L, 2);
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeYellChance(lua_State* L)
{
	// get: monsterType:yellChance() set: monsterType:yellChance(chance)
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			lua_pushinteger(L, monsterType->info.yellChance);
		} else {
			monsterType->info.yellChance = getNumber<uint32_t>(L, 2);
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeYellSpeedTicks(lua_State* L)
{
	// get: monsterType:yellSpeedTicks() set: monsterType:yellSpeedTicks(rate)
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			lua_pushinteger(L, monsterType->info.yellSpeedTicks);
		} else {
			monsterType->info.yellSpeedTicks = getNumber<uint32_t>(L, 2);
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeChangeTargetChance(lua_State* L)
{
	// get: monsterType:changeTargetChance() set: monsterType:changeTargetChance(chance)
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			lua_pushinteger(L, monsterType->info.changeTargetChance);
		} else {
			monsterType->info.changeTargetChance = getNumber<int32_t>(L, 2);
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterTypeChangeTargetSpeed(lua_State* L)
{
	// get: monsterType:changeTargetSpeed() set: monsterType:changeTargetSpeed(speed)
	MonsterType* monsterType = getUserdata<MonsterType>(L, 1);
	if (monsterType) {
		if (lua_gettop(L) == 1) {
			lua_pushinteger(L, monsterType->info.changeTargetSpeed);
		} else {
			monsterType->info.changeTargetSpeed = getNumber<uint32_t>(L, 2);
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// Loot
int LuaScriptInterface::luaCreateLoot(lua_State* L)
{
	// Loot() will create a new loot item
	Loot* loot = new Loot();
	if (loot) {
		pushUserdata<Loot>(L, loot);
		setMetatable(L, -1, "Loot");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaDeleteLoot(lua_State* L)
{
	// loot:delete() loot:__gc()
	Loot** lootPtr = getRawUserdata<Loot>(L, 1);
	if (lootPtr && *lootPtr) {
		delete *lootPtr;
		*lootPtr = nullptr;
	}
	return 0;
}

int LuaScriptInterface::luaLootSetId(lua_State* L)
{
	// loot:setId(id or name)
	Loot* loot = getUserdata<Loot>(L, 1);
	if (loot) {
		if (isNumber(L, 2)) {
			loot->lootBlock.id = getNumber<uint16_t>(L, 2);
		} else {
			auto name = getString(L, 2);
			auto ids = Item::items.nameToItems.equal_range(asLowerCaseString(name));

			if (ids.first == Item::items.nameToItems.cend()) {
				std::cout << "[Warning - Loot:setId] Unknown loot item \"" << name << "\". " << std::endl;
				pushBoolean(L, false);
				return 1;
			}

			if (std::next(ids.first) != ids.second) {
				std::cout << "[Warning - Loot:setId] Non-unique loot item \"" << name << "\". " << std::endl;
				pushBoolean(L, false);
				return 1;
			}

			loot->lootBlock.id = ids.first->second;
		}
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaLootSetSubType(lua_State* L)
{
	// loot:setSubType(type)
	Loot* loot = getUserdata<Loot>(L, 1);
	if (loot) {
		loot->lootBlock.subType = getNumber<uint16_t>(L, 2);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaLootSetChance(lua_State* L)
{
	// loot:setChance(chance)
	Loot* loot = getUserdata<Loot>(L, 1);
	if (loot) {
		loot->lootBlock.chance = getNumber<uint32_t>(L, 2);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaLootSetMaxCount(lua_State* L)
{
	// loot:setMaxCount(max)
	Loot* loot = getUserdata<Loot>(L, 1);
	if (loot) {
		loot->lootBlock.countmax = getNumber<uint32_t>(L, 2);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaLootSetActionId(lua_State* L)
{
	// loot:setActionId(actionid)
	Loot* loot = getUserdata<Loot>(L, 1);
	if (loot) {
		loot->lootBlock.actionId = getNumber<uint32_t>(L, 2);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaLootSetDescription(lua_State* L)
{
	// loot:setDescription(desc)
	Loot* loot = getUserdata<Loot>(L, 1);
	if (loot) {
		loot->lootBlock.text = getString(L, 2);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaLootAddChildLoot(lua_State* L)
{
	// loot:addChildLoot(loot)
	Loot* loot = getUserdata<Loot>(L, 1);
	if (loot) {
		Loot* childLoot = getUserdata<Loot>(L, 2);
		if (childLoot) {
			loot->lootBlock.childLoot.push_back(childLoot->lootBlock);
			pushBoolean(L, true);
		} else {
			pushBoolean(L, false);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// Skill (custom)

int LuaScriptInterface::luaCreateCustomSkill(lua_State* L)
{
	// Skill(level = 1, [ max = 0, [ formula = FormulaType::EXPONENTIAL, [ multiplier = 1.0, [ difficulty = 50, [ threshold = 10]]]]]])
	// return type is userdata

	auto level = (isNumber(L, 2)) ? getNumber<uint16_t>(L, 2) : 1;
	auto max = (isNumber(L, 3)) ? getNumber<uint16_t>(L, 3) : 0;
	auto formula = (isNumber(L, 4)) ? getNumber<FormulaType>(L, 4) : FormulaType::EXPONENTIAL;
	auto multiplier = (isNumber(L, 5)) ? getNumber<float>(L, 5) : 1.0;
	auto difficulty = (isNumber(L, 6)) ? getNumber<float>(L, 6) : 50;
	auto threshold = (isNumber(L, 7)) ? getNumber<float>(L, 7) : 10;

	auto skill = Components::Skills::CustomSkill::make_skill();
	pushSharedPtr(L, skill);
	setMetatable(L, -1, "Skill");

	return 1;
}

int LuaScriptInterface::luaDeleteCustomSkill(lua_State* L)
{
	// for lua's garbage collector
	if (auto& skill = getSharedPtr<CustomSkill>(L, 1))
	{
		skill.reset();
	}

	return 1;
}

int LuaScriptInterface::luaCustomSkillGetLevel(lua_State* L)
{
	// skill:level(include_bonus = true)
	if (auto& skill = getSharedPtr<CustomSkill>(L, 1))
	{
		auto include_bonus = (isBoolean(L, 2)) ? getBoolean(L, 2) : true;
		lua_pushinteger(L, skill->level(include_bonus));
		return 1;
	}
	lua_pushnil(L);
	return 1;
}

int LuaScriptInterface::luaCustomSkillAddLevels(lua_State* L)
{
	// skill:addLevels(levels)
	if (auto& skill = getSharedPtr<CustomSkill>(L, 1))
	{
		if (isNumber(L, 2))
		{
			auto levels = getNumber<uint16_t>(L, 2);
			pushBoolean(L, skill->addLevels(levels));
			return 1;
		}
		// this shouldn't be being called without a number, log it
		pushBoolean(L, false);
		return 1;
	}
	lua_pushnil(L);
	return 1;
}

int LuaScriptInterface::luaCustomSkillSubtractLevels(lua_State* L)
{
	// skill:removeLevels(levels)
	if (auto& skill = getSharedPtr<CustomSkill>(L, 1))
	{
		if (isNumber(L, 2))
		{
			auto levels = getNumber<uint16_t>(L, 2);
			pushBoolean(L, skill->removeLevels(levels));
			return 1;
		}
		// this shouldn't be being called without a number, log it
		pushBoolean(L, false);
		return 1;
	}
	lua_pushnil(L);
	return 1;
}

int LuaScriptInterface::luaCustomSkillClearLevels(lua_State* L)
{
	// skill:clearLevels() -- resets to 1 not 0
	if (auto& skill = getSharedPtr<CustomSkill>(L, 1))
	{
		auto include_bonus = (isBoolean(L, 2)) ? getBoolean(L, 2) : true;
		skill->clearLevels(include_bonus);
		pushBoolean(L, true);
		return 1;
	}
	lua_pushnil(L);
	return 1;
}

int LuaScriptInterface::luaCustomSkillGetBonusLevel(lua_State* L)
{
	// skill:getBonusLevels()
	if (auto& skill = getSharedPtr<CustomSkill>(L, 1))
	{
		lua_pushinteger(L, skill->bonus());
		return 1;
	}
	lua_pushnil(L);
	return 1;
}

int LuaScriptInterface::luaCustomSkillAddBonusLevels(lua_State* L)
{
	// skill:addBonusLevels(levels)
	if (auto& skill = getSharedPtr<CustomSkill>(L, 1))
	{
		if (isNumber(L, 2))
		{
			auto levels = getNumber<uint16_t>(L, 2);
			pushBoolean(L, skill->addBonusLevels(levels));
			return 1;
		}
		pushBoolean(L, false);
		return 1;
	}
	lua_pushnil(L);
	return 1;
}

int LuaScriptInterface::luaCustomSkillSubtractBonusLevels(lua_State* L)
{
	// skill:removeBonusLevels(levels)
	if (auto& skill = getSharedPtr<CustomSkill>(L, 1))
	{
		if (isNumber(L, 2))
		{
			auto levels = getNumber<uint16_t>(L, 2);
			pushBoolean(L, skill->removeBonusLevels(levels));
			return 1;
		}
		pushBoolean(L, false);
		return 1;
	}
	lua_pushnil(L);
	return 1;
}

int LuaScriptInterface::luaCustomSkillClearBonus(lua_State* L)
{
	// skill:clearBonusLevels() -- resets to 0 not 1
	if (auto& skill = getSharedPtr<CustomSkill>(L, 1))
	{
		skill->setBonus(0);
		pushBoolean(L, true);
		return 1;
	}
	lua_pushnil(L);
	return 1;
}

int LuaScriptInterface::luaCustomSkillGetPoints(lua_State* L)
{
	// skill:points()
	// skill:getPoints()
	if (auto& skill = getSharedPtr<CustomSkill>(L, 1))
	{
		lua_pushinteger(L, skill->points());
		return 1;
	}
	lua_pushnil(L);
	return 1;
}

int LuaScriptInterface::luaCustomSkillAddPoints(lua_State* L)
{
	// skill:addPoints(points)
	if (auto& skill = getSharedPtr<CustomSkill>(L, 1))
	{
		if (isNumber(L, 2))
		{
			auto points = getNumber<uint32_t>(L, 2);
			pushBoolean(L, skill->addPoints(points));
			return 1;
		}
		pushBoolean(L, false);
		return 1;
	}
	lua_pushnil(L);
	return 1;
}

int LuaScriptInterface::luaCustomSkillSubtractPoints(lua_State* L)
{
	// skill:removePoints(points)
	if (auto& skill = getSharedPtr<CustomSkill>(L, 1))
	{
		if (isNumber(L, 2))
		{
			auto points = getNumber<uint32_t>(L, 2);
			pushBoolean(L, skill->removePoints(points));
			return 1;
		}
		pushBoolean(L, false);
		return 1;
	}
	lua_pushnil(L);
	return 1;
}

int LuaScriptInterface::luaCustomSkillClearPoints(lua_State* L)
{
	// skill:clearPoints() -- sets current points to 0 for current level
	if (auto& skill = getSharedPtr<CustomSkill>(L, 1))
	{
		skill->clearPoints();
		pushBoolean(L, true);
		return 1;
	}
	lua_pushnil(L);
	return 1;
}

int LuaScriptInterface::luaCustomSkillGetPointsForLevel(lua_State* L)
{
	if (auto& skill = getSharedPtr<CustomSkill>(L, 1))
	{
		lua_pushinteger(L, skill->needed());
		return 1;
	}
	lua_pushnil(L);
	return 1;
}

int LuaScriptInterface::luaCustomSkillGetMaxLevel(lua_State* L)
{
	if (auto& skill = getSharedPtr<CustomSkill>(L, 1))
	{
		lua_pushinteger(L, skill->max());
		return 1;
	}
	lua_pushnil(L);
	return 1;
}

int LuaScriptInterface::luaCustomSkillGetPercentToLevel(lua_State* L)
{
	if (auto& skill = getSharedPtr<CustomSkill>(L, 1))
	{
		lua_pushnumber(L, skill->percent());
		return 1;
	}
	lua_pushnil(L);
	return 1;
}

// MonsterSpell
int LuaScriptInterface::luaCreateMonsterSpell(lua_State* L)
{
	// MonsterSpell() will create a new Monster Spell
	MonsterSpell* spell = new MonsterSpell();
	if (spell) {
		pushUserdata<MonsterSpell>(L, spell);
		setMetatable(L, -1, "MonsterSpell");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaDeleteMonsterSpell(lua_State* L)
{
	// monsterSpell:delete() monsterSpell:__gc()
	MonsterSpell** monsterSpellPtr = getRawUserdata<MonsterSpell>(L, 1);
	if (monsterSpellPtr && *monsterSpellPtr) {
		delete *monsterSpellPtr;
		*monsterSpellPtr = nullptr;
	}
	return 0;
}

int LuaScriptInterface::luaMonsterSpellSetType(lua_State* L)
{
	// monsterSpell:setType(type)
	MonsterSpell* spell = getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->name = getString(L, 2);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSpellSetScriptName(lua_State* L)
{
	// monsterSpell:setScriptName(name)
	MonsterSpell* spell = getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->scriptName = getString(L, 2);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSpellSetChance(lua_State* L)
{
	// monsterSpell:setChance(chance)
	MonsterSpell* spell = getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->chance = getNumber<uint8_t>(L, 2);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSpellSetInterval(lua_State* L)
{
	// monsterSpell:setInterval(interval)
	MonsterSpell* spell = getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->interval = getNumber<uint16_t>(L, 2);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSpellSetRange(lua_State* L)
{
	// monsterSpell:setRange(range)
	MonsterSpell* spell = getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->range = getNumber<uint8_t>(L, 2);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSpellSetCombatValue(lua_State* L)
{
	// monsterSpell:setCombatValue(min, max)
	MonsterSpell* spell = getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->minCombatValue = getNumber<int32_t>(L, 2);
		spell->maxCombatValue = getNumber<int32_t>(L, 3);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSpellSetCombatType(lua_State* L)
{
	// monsterSpell:setCombatType(combatType_t)
	MonsterSpell* spell = getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->combatType = getNumber<CombatType_t>(L, 2);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSpellSetAttackValue(lua_State* L)
{
	// monsterSpell:setAttackValue(attack, skill)
	MonsterSpell* spell = getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->attack = getNumber<int32_t>(L, 2);
		spell->skill = getNumber<int32_t>(L, 3);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSpellSetNeedTarget(lua_State* L)
{
	// monsterSpell:setNeedTarget(bool)
	MonsterSpell* spell = getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->needTarget = getBoolean(L, 2);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSpellSetNeedDirection(lua_State* L)
{
	// monsterSpell:setNeedDirection(bool)
	MonsterSpell* spell = getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->needDirection = getBoolean(L, 2);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSpellSetCombatLength(lua_State* L)
{
	// monsterSpell:setCombatLength(length)
	MonsterSpell* spell = getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->length = getNumber<int32_t>(L, 2);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSpellSetCombatSpread(lua_State* L)
{
	// monsterSpell:setCombatSpread(spread)
	MonsterSpell* spell = getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->spread = getNumber<int32_t>(L, 2);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSpellSetCombatRadius(lua_State* L)
{
	// monsterSpell:setCombatRadius(radius)
	MonsterSpell* spell = getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->radius = getNumber<int32_t>(L, 2);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSpellSetConditionType(lua_State* L)
{
	// monsterSpell:setConditionType(type)
	MonsterSpell* spell = getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->conditionType = getNumber<ConditionType_t>(L, 2);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSpellSetConditionDamage(lua_State* L)
{
	// monsterSpell:setConditionDamage(min, max, start)
	MonsterSpell* spell = getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->conditionMinDamage = getNumber<int32_t>(L, 2);
		spell->conditionMaxDamage = getNumber<int32_t>(L, 3);
		spell->conditionStartDamage = getNumber<int32_t>(L, 4);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSpellSetConditionSpeedChange(lua_State* L)
{
	// monsterSpell:setConditionSpeedChange(minSpeed[, maxSpeed])
	MonsterSpell* spell = getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->minSpeedChange = getNumber<int32_t>(L, 2);
		spell->maxSpeedChange = getNumber<int32_t>(L, 3, 0);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSpellSetConditionDuration(lua_State* L)
{
	// monsterSpell:setConditionDuration(duration)
	MonsterSpell* spell = getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->duration = getNumber<int32_t>(L, 2);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSpellSetConditionDrunkenness(lua_State* L)
{
	// monsterSpell:setConditionDrunkenness(drunkenness)
	MonsterSpell* spell = getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->drunkenness = getNumber<uint8_t>(L, 2);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSpellSetConditionTickInterval(lua_State* L)
{
	// monsterSpell:setConditionTickInterval(interval)
	MonsterSpell* spell = getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->tickInterval = getNumber<int32_t>(L, 2);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSpellSetCombatShootEffect(lua_State* L)
{
	// monsterSpell:setCombatShootEffect(effect)
	MonsterSpell* spell = getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->shoot = getNumber<ShootType_t>(L, 2);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSpellSetCombatEffect(lua_State* L)
{
	// monsterSpell:setCombatEffect(effect)
	MonsterSpell* spell = getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		spell->effect = getNumber<MagicEffectClasses>(L, 2);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMonsterSpellSetOutfit(lua_State* L)
{
	// monsterSpell:setOutfit(outfit)
	MonsterSpell* spell = getUserdata<MonsterSpell>(L, 1);
	if (spell) {
		if (isTable(L, 2)) {
			spell->outfit = getOutfit(L, 2);
		} else if (isNumber(L, 2)) {
			spell->outfit.lookTypeEx = getNumber<uint16_t>(L, 2);
		} else if (isString(L, 2)) {
			MonsterType* mType = g_monsters.getMonsterType(getString(L, 2));
			if (mType) {
				spell->outfit = mType->info.outfit;
			}
		}
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// Party
int32_t LuaScriptInterface::luaPartyCreate(lua_State* L)
{
	// Party(userdata)
	auto player = getSharedPtr<Player>(L, 2);
	if (!player) {
		lua_pushnil(L);
		return 1;
	}

	auto party = player->getParty();
	if (!party) {
		party = new Party(player);
		g_game.updatePlayerShield(player);
		player->sendCreatureSkull(player);
		pushUserdata<Party>(L, party);
		setMetatable(L, -1, "Party");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPartyDisband(lua_State* L)
{
	// party:disband()
	Party** partyPtr = getRawUserdata<Party>(L, 1);
	if (partyPtr && *partyPtr) {
		Party*& party = *partyPtr;
		party->disband();
		party = nullptr;
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPartyGetLeader(lua_State* L)
{
	// party:getLeader()
	Party* party = getUserdata<Party>(L, 1);
	if (!party) {
		lua_pushnil(L);
		return 1;
	}

	if (const auto leader = party->getLeader()) {
		pushSharedPtr(L, leader);
		setMetatable(L, -1, "Player");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPartySetLeader(lua_State* L)
{
	// party:setLeader(player)
	const auto player = getPlayer(L, 2);
	const auto party = getUserdata<Party>(L, 1);
	if (party && player) {
		pushBoolean(L, party->passPartyLeadership(player, true));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPartyGetMembers(lua_State* L)
{
	// party:getMembers()
	Party* party = getUserdata<Party>(L, 1);
	if (!party) {
		lua_pushnil(L);
		return 1;
	}

	int index = 0;
	lua_createtable(L, party->getMemberCount(), 0);
	for (const auto player : party->getMembers()) {
		pushSharedPtr(L, player);
		setMetatable(L, -1, "Player");
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaPartyGetMemberCount(lua_State* L)
{
	// party:getMemberCount()
	Party* party = getUserdata<Party>(L, 1);
	if (party) {
		lua_pushinteger(L, party->getMemberCount());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPartyGetInvitees(lua_State* L)
{
	// party:getInvitees()
	if (const auto party = getUserdata<Party>(L, 1)) {
		lua_createtable(L, party->getInvitationCount(), 0);

		int index = 0;
		for (const auto player : party->getInvitees()) {
			pushSharedPtr(L, player);
			setMetatable(L, -1, "Player");
			lua_rawseti(L, -2, ++index);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPartyGetInviteeCount(lua_State* L)
{
	// party:getInviteeCount()
	const auto party = getUserdata<Party>(L, 1);
	if (party) {
		lua_pushinteger(L, party->getInvitationCount());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPartyAddInvite(lua_State* L)
{
	// party:addInvite(player)
	auto player = getPlayer(L, 2);
	const auto party = getUserdata<Party>(L, 1);
	if (party && player) {
		pushBoolean(L, party->invitePlayer(player));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPartyRemoveInvite(lua_State* L)
{
	// party:removeInvite(player)
	auto player = getPlayer(L, 2);
	const auto party = getUserdata<Party>(L, 1);
	if (party && player) {
		pushBoolean(L, party->removeInvite(player));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPartyAddMember(lua_State* L)
{
	// party:addMember(player)
	auto player = getPlayer(L, 2);
	const auto party = getUserdata<Party>(L, 1);
	if (party && player) {
		pushBoolean(L, party->joinParty(player));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPartyRemoveMember(lua_State* L)
{
	// party:removeMember(player)
	const auto player = getPlayer(L, 2);
	const auto party = getUserdata<Party>(L, 1);
	if (party && player) {
		pushBoolean(L, party->leaveParty(player));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPartyIsSharedExperienceActive(lua_State* L)
{
	// party:isSharedExperienceActive()
	if (const auto party = getUserdata<Party>(L, 1)) {
		pushBoolean(L, party->isSharedExperienceActive());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPartyIsSharedExperienceEnabled(lua_State* L)
{
	// party:isSharedExperienceEnabled()
	if (const auto party = getUserdata<Party>(L, 1)) {
		pushBoolean(L, party->isSharedExperienceEnabled());
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPartyShareExperience(lua_State* L)
{
	// party:shareExperience(experience)
	uint64_t experience = getNumber<uint64_t>(L, 2);
	if (const auto party = getUserdata<Party>(L, 1)) {
		party->shareExperience(experience);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaPartySetSharedExperience(lua_State* L)
{
	// party:setSharedExperience(active)
	const bool active = getBoolean(L, 2);
	if (const auto party = getUserdata<Party>(L, 1)) {
		pushBoolean(L, party->setSharedExperience(party->getLeader(), active));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// Spells
int LuaScriptInterface::luaSpellCreate(lua_State* L)
{
	// Spell(words, name or id) to get an existing spell
	// Spell(type) ex: Spell(SPELL_INSTANT) or Spell(SPELL_RUNE) to create a new spell
	if (lua_gettop(L) == 1) {
		std::cout << "[Error - Spell::luaSpellCreate] There is no parameter set!" << std::endl;
		lua_pushnil(L);
		return 1;
	}

	SpellType_t spellType = SPELL_UNDEFINED;

	if (isNumber(L, 2)) {
		int32_t id = getNumber<int32_t>(L, 2);
		RuneSpell* rune = g_spells->getRuneSpell(id);

		if (rune) {
			pushUserdata<Spell>(L, rune);
			setMetatable(L, -1, "Spell");
			return 1;
		}

		spellType = static_cast<SpellType_t>(id);
	} else if (isString(L, 2)) {
		std::string arg = getString(L, 2);
		InstantSpell* instant = g_spells->getInstantSpellByName(arg);
		if (instant) {
			pushUserdata<Spell>(L, instant);
			setMetatable(L, -1, "Spell");
			return 1;
		}
		instant = g_spells->getInstantSpell(arg);
		if (instant) {
			pushUserdata<Spell>(L, instant);
			setMetatable(L, -1, "Spell");
			return 1;
		}
		RuneSpell* rune = g_spells->getRuneSpellByName(arg);
		if (rune) {
			pushUserdata<Spell>(L, rune);
			setMetatable(L, -1, "Spell");
			return 1;
		}

		std::string tmp = asLowerCaseString(arg);
		if (tmp == "instant") {
			spellType = SPELL_INSTANT;
		} else if (tmp == "rune") {
			spellType = SPELL_RUNE;
		}
	}

	if (spellType == SPELL_INSTANT) {
		InstantSpell* spell = new InstantSpell(getScriptEnv()->getScriptInterface());
		spell->fromLua = true;
		pushUserdata<Spell>(L, spell);
		setMetatable(L, -1, "Spell");
		spell->spellType = SPELL_INSTANT;
		return 1;
	} else if (spellType == SPELL_RUNE) {
		RuneSpell* spell = new RuneSpell(getScriptEnv()->getScriptInterface());
		spell->fromLua = true;
		pushUserdata<Spell>(L, spell);
		setMetatable(L, -1, "Spell");
		spell->spellType = SPELL_RUNE;
		return 1;
	}

	lua_pushnil(L);
	return 1;
}

int LuaScriptInterface::luaSpellOnCastSpell(lua_State* L)
{
	// spell:onCastSpell(callback)
	Spell* spell = getUserdata<Spell>(L, 1);
	if (spell) {
		if (spell->spellType == SPELL_INSTANT) {
			InstantSpell* instant = dynamic_cast<InstantSpell*>(getUserdata<Spell>(L, 1));
			if (!instant->loadCallback()) {
				pushBoolean(L, false);
				return 1;
			}
			instant->scripted = true;
			pushBoolean(L, true);
		} else if (spell->spellType == SPELL_RUNE) {
			RuneSpell* rune = dynamic_cast<RuneSpell*>(getUserdata<Spell>(L, 1));
			if (!rune->loadCallback()) {
				pushBoolean(L, false);
				return 1;
			}
			rune->scripted = true;
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaSpellRegister(lua_State* L)
{
	// spell:register()
	Spell* spell = getUserdata<Spell>(L, 1);
	if (spell) {
		if (spell->spellType == SPELL_INSTANT) {
			InstantSpell* instant = dynamic_cast<InstantSpell*>(getUserdata<Spell>(L, 1));
			if (!instant->isScripted()) {
				pushBoolean(L, false);
				return 1;
			}
			pushBoolean(L, g_spells->registerInstantLuaEvent(instant));
		} else if (spell->spellType == SPELL_RUNE) {
			RuneSpell* rune = dynamic_cast<RuneSpell*>(getUserdata<Spell>(L, 1));
			if (rune->getMagicLevel() != 0 || rune->getLevel() != 0) {
				//Change information in the ItemType to get accurate description
				ItemType& iType = Item::items.getItemType(rune->getRuneItemId());
				iType.name = rune->getName();
				iType.runeMagLevel = rune->getMagicLevel();
				iType.runeLevel = rune->getLevel();
				iType.charges = rune->getCharges();
			}
			if (!rune->isScripted()) {
				pushBoolean(L, false);
				return 1;
			}
			pushBoolean(L, g_spells->registerRuneLuaEvent(rune));
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaSpellName(lua_State* L)
{
	// spell:name(name)
	Spell* spell = getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			pushString(L, spell->getName());
		} else {
			spell->setName(getString(L, 2));
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaSpellId(lua_State* L)
{
	// spell:id(id)
	Spell* spell = getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			lua_pushinteger(L, spell->getId());
		} else {
			spell->setId(getNumber<uint8_t>(L, 2));
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaSpellGroup(lua_State* L)
{
	// spell:group(primaryGroup[, secondaryGroup])
	Spell* spell = getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			lua_pushinteger(L, spell->getGroup());
			lua_pushinteger(L, spell->getSecondaryGroup());
			return 2;
		} else if (lua_gettop(L) == 2) {
			SpellGroup_t group = getNumber<SpellGroup_t>(L, 2);
			if (group) {
				spell->setGroup(group);
				pushBoolean(L, true);
			} else if (isString(L, 2)) {
				group = stringToSpellGroup(getString(L, 2));
				if (group != SPELLGROUP_UNKNOWN) {
					spell->setGroup(group);
				} else {
					std::cout << "[Warning - Spell::group] Unknown group: " << getString(L, 2) << std::endl;
					pushBoolean(L, false);
					return 1;
				}
				pushBoolean(L, true);
			} else {
				std::cout << "[Warning - Spell::group] Unknown group: " << getString(L, 2) << std::endl;
				pushBoolean(L, false);
				return 1;
			}
		} else {
			SpellGroup_t primaryGroup = getNumber<SpellGroup_t>(L, 2);
			SpellGroup_t secondaryGroup = getNumber<SpellGroup_t>(L, 3);
			if (primaryGroup && secondaryGroup) {
				spell->setGroup(primaryGroup);
				spell->setSecondaryGroup(secondaryGroup);
				pushBoolean(L, true);
			} else if (isString(L, 2) && isString(L, 3)) {
				primaryGroup = stringToSpellGroup(getString(L, 2));
				if (primaryGroup != SPELLGROUP_UNKNOWN) {
					spell->setGroup(primaryGroup);
				} else {
					std::cout << "[Warning - Spell::group] Unknown primaryGroup: " << getString(L, 2) << std::endl;
					pushBoolean(L, false);
					return 1;
				}
				secondaryGroup = stringToSpellGroup(getString(L, 3));
				if (secondaryGroup != SPELLGROUP_UNKNOWN) {
					spell->setSecondaryGroup(secondaryGroup);
				} else {
					std::cout << "[Warning - Spell::group] Unknown secondaryGroup: " << getString(L, 3) << std::endl;
					pushBoolean(L, false);
					return 1;
				}
				pushBoolean(L, true);
			} else {
				std::cout << "[Warning - Spell::group] Unknown primaryGroup: " << getString(L, 2) << " or secondaryGroup: " << getString(L, 3) << std::endl;
				pushBoolean(L, false);
				return 1;
			}
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaSpellCooldown(lua_State* L)
{
	// spell:cooldown(cooldown)
	Spell* spell = getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			lua_pushinteger(L, spell->getCooldown());
		} else {
			spell->setCooldown(getNumber<uint32_t>(L, 2));
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaSpellGroupCooldown(lua_State* L)
{
	// spell:groupCooldown(primaryGroupCd[, secondaryGroupCd])
	Spell* spell = getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			lua_pushinteger(L, spell->getGroupCooldown());
			lua_pushinteger(L, spell->getSecondaryCooldown());
			return 2;
		} else if (lua_gettop(L) == 2) {
			spell->setGroupCooldown(getNumber<uint32_t>(L, 2));
			pushBoolean(L, true);
		} else {
			spell->setGroupCooldown(getNumber<uint32_t>(L, 2));
			spell->setSecondaryCooldown(getNumber<uint32_t>(L, 3));
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaSpellLevel(lua_State* L)
{
	// spell:level(lvl)
	Spell* spell = getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			lua_pushinteger(L, spell->getLevel());
		} else {
			spell->setLevel(getNumber<uint32_t>(L, 2));
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaSpellMagicLevel(lua_State* L)
{
	// spell:magicLevel(lvl)
	Spell* spell = getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			lua_pushinteger(L, spell->getMagicLevel());
		} else {
			spell->setMagicLevel(getNumber<uint32_t>(L, 2));
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaSpellMana(lua_State* L)
{
	// spell:mana(mana)
	Spell* spell = getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			lua_pushinteger(L, spell->getMana());
		} else {
			spell->setMana(getNumber<uint32_t>(L, 2));
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaSpellManaPercent(lua_State* L)
{
	// spell:manaPercent(percent)
	Spell* spell = getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			lua_pushinteger(L, spell->getManaPercent());
		} else {
			spell->setManaPercent(getNumber<uint32_t>(L, 2));
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaSpellSoul(lua_State* L)
{
	// spell:soul(soul)
	Spell* spell = getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			lua_pushinteger(L, spell->getSoulCost());
		} else {
			spell->setSoulCost(getNumber<uint32_t>(L, 2));
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaSpellRange(lua_State* L)
{
	// spell:range(range)
	Spell* spell = getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			lua_pushinteger(L, spell->getRange());
		} else {
			spell->setRange(getNumber<int32_t>(L, 2));
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaSpellPremium(lua_State* L)
{
	// spell:isPremium(bool)
	Spell* spell = getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			pushBoolean(L, spell->isPremium());
		} else {
			spell->setPremium(getBoolean(L, 2));
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaSpellEnabled(lua_State* L)
{
	// spell:isEnabled(bool)
	Spell* spell = getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			pushBoolean(L, spell->isEnabled());
		} else {
			spell->setEnabled(getBoolean(L, 2));
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaSpellNeedTarget(lua_State* L)
{
	// spell:needTarget(bool)
	Spell* spell = getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			pushBoolean(L, spell->getNeedTarget());
		} else {
			spell->setNeedTarget(getBoolean(L, 2));
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaSpellNeedWeapon(lua_State* L)
{
	// spell:needWeapon(bool)
	Spell* spell = getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			pushBoolean(L, spell->getNeedWeapon());
		} else {
			spell->setNeedWeapon(getBoolean(L, 2));
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaSpellNeedLearn(lua_State* L)
{
	// spell:needLearn(bool)
	Spell* spell = getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			pushBoolean(L, spell->getNeedLearn());
		} else {
			spell->setNeedLearn(getBoolean(L, 2));
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaSpellSelfTarget(lua_State* L)
{
	// spell:isSelfTarget(bool)
	Spell* spell = getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			pushBoolean(L, spell->getSelfTarget());
		} else {
			spell->setSelfTarget(getBoolean(L, 2));
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaSpellBlocking(lua_State* L)
{
	// spell:isBlocking(blockingSolid, blockingCreature)
	Spell* spell = getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			pushBoolean(L, spell->getBlockingSolid());
			pushBoolean(L, spell->getBlockingCreature());
			return 2;
		} else {
			spell->setBlockingSolid(getBoolean(L, 2));
			spell->setBlockingCreature(getBoolean(L, 3));
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaSpellAggressive(lua_State* L)
{
	// spell:isAggressive(bool)
	Spell* spell = getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			pushBoolean(L, spell->getAggressive());
		} else {
			spell->setAggressive(getBoolean(L, 2));
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaSpellPzLock(lua_State* L)
{
	// spell:isPzLock(bool)
	Spell* spell = getUserdata<Spell>(L, 1);
	if (spell) {
		if (lua_gettop(L) == 1) {
			pushBoolean(L, spell->getPzLock());
		} else {
			spell->setPzLock(getBoolean(L, 2));
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaSpellVocation(lua_State* L)
{
	// spell:vocation(vocation)
	Spell* spell = getUserdata<Spell>(L, 1);
	if (!spell) {
		lua_pushnil(L);
		return 1;
	}

	if (lua_gettop(L) == 1) {
		lua_createtable(L, 0, 0);
		int i = 0;
		for (auto& voc : spell->getVocMap()) {
			std::string name = g_vocations.getVocation(voc.first)->getVocName();
			pushString(L, name);
			lua_rawseti(L, -2, ++i);
		}
	} else {
		int parameters = lua_gettop(L) - 1; // - 1 because self is a parameter aswell, which we want to skip ofc
		for (int i = 0; i < parameters; ++i) {
			std::string vocData = getString(L, 2 + i);
			auto vocParts = explodeString(vocData, ";");

			std::string name = std::string(vocParts[0]);
			bool description = false;

			if (vocParts.size() > 1) {
				description = booleanString(vocParts[1]);
			}

			spell->addVocMap(g_vocations.getVocationId(name), description);
		}
		pushBoolean(L, true);
	}
	return 1;
}

// only for InstantSpells
int LuaScriptInterface::luaSpellWords(lua_State* L)
{
	// spell:words(words[, separator = ""])
	InstantSpell* spell = dynamic_cast<InstantSpell*>(getUserdata<Spell>(L, 1));
	if (spell) {
		// if spell != SPELL_INSTANT, it means that this actually is no InstantSpell, so we return nil
		if (spell->spellType != SPELL_INSTANT) {
			lua_pushnil(L);
			return 1;
		}

		if (lua_gettop(L) == 1) {
			pushString(L, spell->getWords());
			pushString(L, spell->getSeparator());
			return 2;
		} else {
			std::string sep = "";
			if (lua_gettop(L) == 3) {
				sep = getString(L, 3);
			}
			spell->setWords(getString(L, 2));
			spell->setSeparator(sep);
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// only for InstantSpells
int LuaScriptInterface::luaSpellNeedDirection(lua_State* L)
{
	// spell:needDirection(bool)
	InstantSpell* spell = dynamic_cast<InstantSpell*>(getUserdata<Spell>(L, 1));
	if (spell) {
		// if spell != SPELL_INSTANT, it means that this actually is no InstantSpell, so we return nil
		if (spell->spellType != SPELL_INSTANT) {
			lua_pushnil(L);
			return 1;
		}

		if (lua_gettop(L) == 1) {
			pushBoolean(L, spell->getNeedDirection());
		} else {
			spell->setNeedDirection(getBoolean(L, 2));
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// only for InstantSpells
int LuaScriptInterface::luaSpellHasParams(lua_State* L)
{
	// spell:hasParams(bool)
	InstantSpell* spell = dynamic_cast<InstantSpell*>(getUserdata<Spell>(L, 1));
	if (spell) {
		// if spell != SPELL_INSTANT, it means that this actually is no InstantSpell, so we return nil
		if (spell->spellType != SPELL_INSTANT) {
			lua_pushnil(L);
			return 1;
		}

		if (lua_gettop(L) == 1) {
			pushBoolean(L, spell->getHasParam());
		} else {
			spell->setHasParam(getBoolean(L, 2));
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// only for InstantSpells
int LuaScriptInterface::luaSpellHasPlayerNameParam(lua_State* L)
{
	// spell:hasPlayerNameParam(bool)
	InstantSpell* spell = dynamic_cast<InstantSpell*>(getUserdata<Spell>(L, 1));
	if (spell) {
		// if spell != SPELL_INSTANT, it means that this actually is no InstantSpell, so we return nil
		if (spell->spellType != SPELL_INSTANT) {
			lua_pushnil(L);
			return 1;
		}

		if (lua_gettop(L) == 1) {
			pushBoolean(L, spell->getHasPlayerNameParam());
		} else {
			spell->setHasPlayerNameParam(getBoolean(L, 2));
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// only for InstantSpells
int LuaScriptInterface::luaSpellNeedCasterTargetOrDirection(lua_State* L)
{
	// spell:needCasterTargetOrDirection(bool)
	InstantSpell* spell = dynamic_cast<InstantSpell*>(getUserdata<Spell>(L, 1));
	if (spell) {
		// if spell != SPELL_INSTANT, it means that this actually is no InstantSpell, so we return nil
		if (spell->spellType != SPELL_INSTANT) {
			lua_pushnil(L);
			return 1;
		}

		if (lua_gettop(L) == 1) {
			pushBoolean(L, spell->getNeedCasterTargetOrDirection());
		} else {
			spell->setNeedCasterTargetOrDirection(getBoolean(L, 2));
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// only for InstantSpells
int LuaScriptInterface::luaSpellIsBlockingWalls(lua_State* L)
{
	// spell:blockWalls(bool)
	InstantSpell* spell = dynamic_cast<InstantSpell*>(getUserdata<Spell>(L, 1));
	if (spell) {
		// if spell != SPELL_INSTANT, it means that this actually is no InstantSpell, so we return nil
		if (spell->spellType != SPELL_INSTANT) {
			lua_pushnil(L);
			return 1;
		}

		if (lua_gettop(L) == 1) {
			pushBoolean(L, spell->getBlockWalls());
		} else {
			spell->setBlockWalls(getBoolean(L, 2));
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// only for RuneSpells
int LuaScriptInterface::luaSpellRuneLevel(lua_State* L)
{
	// spell:runeLevel(level)
	RuneSpell* spell = dynamic_cast<RuneSpell*>(getUserdata<Spell>(L, 1));
	int32_t level = getNumber<int32_t>(L, 2);
	if (spell) {
		// if spell != SPELL_RUNE, it means that this actually is no RuneSpell, so we return nil
		if (spell->spellType != SPELL_RUNE) {
			lua_pushnil(L);
			return 1;
		}

		if (lua_gettop(L) == 1) {
			lua_pushinteger(L, spell->getLevel());
		} else {
			spell->setLevel(level);
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// only for RuneSpells
int LuaScriptInterface::luaSpellRuneMagicLevel(lua_State* L)
{
	// spell:runeMagicLevel(magLevel)
	RuneSpell* spell = dynamic_cast<RuneSpell*>(getUserdata<Spell>(L, 1));
	int32_t magLevel = getNumber<int32_t>(L, 2);
	if (spell) {
		// if spell != SPELL_RUNE, it means that this actually is no RuneSpell, so we return nil
		if (spell->spellType != SPELL_RUNE) {
			lua_pushnil(L);
			return 1;
		}

		if (lua_gettop(L) == 1) {
			lua_pushinteger(L, spell->getMagicLevel());
		} else {
			spell->setMagicLevel(magLevel);
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// only for RuneSpells
int LuaScriptInterface::luaSpellRuneId(lua_State* L)
{
	// spell:runeId(id)
	RuneSpell* rune = dynamic_cast<RuneSpell*>(getUserdata<Spell>(L, 1));
	if (rune) {
		// if spell != SPELL_RUNE, it means that this actually is no RuneSpell, so we return nil
		if (rune->spellType != SPELL_RUNE) {
			lua_pushnil(L);
			return 1;
		}

		if (lua_gettop(L) == 1) {
			lua_pushinteger(L, rune->getRuneItemId());
		} else {
			rune->setRuneItemId(getNumber<uint16_t>(L, 2));
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// only for RuneSpells
int LuaScriptInterface::luaSpellCharges(lua_State* L)
{
	// spell:charges(charges)
	RuneSpell* spell = dynamic_cast<RuneSpell*>(getUserdata<Spell>(L, 1));
	if (spell) {
		// if spell != SPELL_RUNE, it means that this actually is no RuneSpell, so we return nil
		if (spell->spellType != SPELL_RUNE) {
			lua_pushnil(L);
			return 1;
		}

		if (lua_gettop(L) == 1) {
			lua_pushinteger(L, spell->getCharges());
		} else {
			spell->setCharges(getNumber<uint32_t>(L, 2));
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// only for RuneSpells
int LuaScriptInterface::luaSpellAllowFarUse(lua_State* L)
{
	// spell:allowFarUse(bool)
	RuneSpell* spell = dynamic_cast<RuneSpell*>(getUserdata<Spell>(L, 1));
	if (spell) {
		// if spell != SPELL_RUNE, it means that this actually is no RuneSpell, so we return nil
		if (spell->spellType != SPELL_RUNE) {
			lua_pushnil(L);
			return 1;
		}

		if (lua_gettop(L) == 1) {
			pushBoolean(L, spell->getAllowFarUse());
		} else {
			spell->setAllowFarUse(getBoolean(L, 2));
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// only for RuneSpells
int LuaScriptInterface::luaSpellBlockWalls(lua_State* L)
{
	// spell:blockWalls(bool)
	RuneSpell* spell = dynamic_cast<RuneSpell*>(getUserdata<Spell>(L, 1));
	if (spell) {
		// if spell != SPELL_RUNE, it means that this actually is no RuneSpell, so we return nil
		if (spell->spellType != SPELL_RUNE) {
			lua_pushnil(L);
			return 1;
		}

		if (lua_gettop(L) == 1) {
			pushBoolean(L, spell->getCheckLineOfSight());
		} else {
			spell->setCheckLineOfSight(getBoolean(L, 2));
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// only for RuneSpells
int LuaScriptInterface::luaSpellCheckFloor(lua_State* L)
{
	// spell:checkFloor(bool)
	RuneSpell* spell = dynamic_cast<RuneSpell*>(getUserdata<Spell>(L, 1));
	if (spell) {
		// if spell != SPELL_RUNE, it means that this actually is no RuneSpell, so we return nil
		if (spell->spellType != SPELL_RUNE) {
			lua_pushnil(L);
			return 1;
		}

		if (lua_gettop(L) == 1) {
			pushBoolean(L, spell->getCheckFloor());
		} else {
			spell->setCheckFloor(getBoolean(L, 2));
			pushBoolean(L, true);
		}
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCreateAction(lua_State* L)
{
	// Action()
	if (getScriptEnv()->getScriptInterface() != &g_scripts->getScriptInterface()) {
		reportErrorFunc(L, "Actions can only be registered in the Scripts interface.");
		lua_pushnil(L);
		return 1;
	}

	Action* action = new Action(getScriptEnv()->getScriptInterface());
	if (action) {
		action->fromLua = true;
		pushUserdata<Action>(L, action);
		setMetatable(L, -1, "Action");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaActionOnUse(lua_State* L)
{
	// action:onUse(callback)
	Action* action = getUserdata<Action>(L, 1);
	if (action) {
		if (!action->loadCallback()) {
			pushBoolean(L, false);
			return 1;
		}
		action->scripted = true;
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaActionRegister(lua_State* L)
{
	// action:register()
	Action* action = getUserdata<Action>(L, 1);
	if (action) {
		if (!action->isScripted()) {
			pushBoolean(L, false);
			return 1;
		}
		pushBoolean(L, g_actions->registerLuaEvent(action));
		action->clearActionIdRange();
		action->clearItemIdRange();
		action->clearUniqueIdRange();
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaActionItemId(lua_State* L)
{
	// action:id(ids)
	Action* action = getUserdata<Action>(L, 1);
	if (action) {
		int parameters = lua_gettop(L) - 1; // - 1 because self is a parameter aswell, which we want to skip ofc
		if (parameters > 1) {
			for (int i = 0; i < parameters; ++i) {
				action->addItemId(getNumber<uint32_t>(L, 2 + i));
			}
		} else {
			action->addItemId(getNumber<uint32_t>(L, 2));
		}
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaActionActionId(lua_State* L)
{
	// action:aid(aids)
	Action* action = getUserdata<Action>(L, 1);
	if (action) {
		int parameters = lua_gettop(L) - 1; // - 1 because self is a parameter aswell, which we want to skip ofc
		if (parameters > 1) {
			for (int i = 0; i < parameters; ++i) {
				action->addActionId(getNumber<uint32_t>(L, 2 + i));
			}
		} else {
			action->addActionId(getNumber<uint32_t>(L, 2));
		}
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaActionUniqueId(lua_State* L)
{
	// action:uid(uids)
	Action* action = getUserdata<Action>(L, 1);
	if (action) {
		int parameters = lua_gettop(L) - 1; // - 1 because self is a parameter aswell, which we want to skip ofc
		if (parameters > 1) {
			for (int i = 0; i < parameters; ++i) {
				action->addUniqueId(getNumber<uint32_t>(L, 2 + i));
			}
		} else {
			action->addUniqueId(getNumber<uint32_t>(L, 2));
		}
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaActionAllowFarUse(lua_State* L)
{
	// action:allowFarUse(bool)
	Action* action = getUserdata<Action>(L, 1);
	if (action) {
		action->setAllowFarUse(getBoolean(L, 2));
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaActionBlockWalls(lua_State* L)
{
	// action:blockWalls(bool)
	Action* action = getUserdata<Action>(L, 1);
	if (action) {
		action->setCheckLineOfSight(getBoolean(L, 2));
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaActionCheckFloor(lua_State* L)
{
	// action:checkFloor(bool)
	Action* action = getUserdata<Action>(L, 1);
	if (action) {
		action->setCheckFloor(getBoolean(L, 2));
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCreateTalkaction(lua_State* L)
{
	// TalkAction(words)
	if (getScriptEnv()->getScriptInterface() != &g_scripts->getScriptInterface()) {
		reportErrorFunc(L, "TalkActions can only be registered in the Scripts interface.");
		lua_pushnil(L);
		return 1;
	}

	TalkAction* talk = new TalkAction(getScriptEnv()->getScriptInterface());
	if (talk) {
		for (int i = 2; i <= lua_gettop(L); i++) {
			talk->setWords(getString(L, i));
		}
		talk->fromLua = true;
		pushUserdata<TalkAction>(L, talk);
		setMetatable(L, -1, "TalkAction");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaTalkactionOnSay(lua_State* L)
{
	// talkAction:onSay(callback)
	TalkAction* talk = getUserdata<TalkAction>(L, 1);
	if (talk) {
		if (!talk->loadCallback()) {
			pushBoolean(L, false);
			return 1;
		}
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaTalkactionRegister(lua_State* L)
{
	// talkAction:register()
	TalkAction* talk = getUserdata<TalkAction>(L, 1);
	if (talk) {
		if (!talk->isScripted()) {
			pushBoolean(L, false);
			return 1;
		}
		pushBoolean(L, g_talkActions->registerLuaEvent(talk));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaTalkactionSeparator(lua_State* L)
{
	// talkAction:separator(sep)
	TalkAction* talk = getUserdata<TalkAction>(L, 1);
	if (talk) {
		talk->setSeparator(getString(L, 2));
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaTalkactionAccess(lua_State* L)
{
	// talkAction:access(needAccess = false)
	TalkAction* talk = getUserdata<TalkAction>(L, 1);
	if (talk) {
		talk->setNeedAccess(getBoolean(L, 2));
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaTalkactionAccountType(lua_State* L)
{
	// talkAction:accountType(AccountType_t = ACCOUNT_TYPE_NORMAL)
	TalkAction* talk = getUserdata<TalkAction>(L, 1);
	if (talk) {
		talk->setRequiredAccountType(getNumber<AccountType_t>(L, 2));
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCreateCreatureEvent(lua_State* L)
{
	// CreatureEvent(eventName)
	if (getScriptEnv()->getScriptInterface() != &g_scripts->getScriptInterface()) {
		reportErrorFunc(L, "CreatureEvents can only be registered in the Scripts interface.");
		lua_pushnil(L);
		return 1;
	}

	CreatureEvent* creature = new CreatureEvent(getScriptEnv()->getScriptInterface());
	if (creature) {
		creature->setName(getString(L, 2));
		creature->fromLua = true;
		pushUserdata<CreatureEvent>(L, creature);
		setMetatable(L, -1, "CreatureEvent");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureEventType(lua_State* L)
{
	// creatureevent:type(callback)
	CreatureEvent* creature = getUserdata<CreatureEvent>(L, 1);
	if (creature) {
		std::string typeName = getString(L, 2);
		std::string tmpStr = asLowerCaseString(typeName);
		if (tmpStr == "login") {
			creature->setEventType(CREATURE_EVENT_LOGIN);
		} else if (tmpStr == "logout") {
			creature->setEventType(CREATURE_EVENT_LOGOUT);
		} else if (tmpStr == "think") {
			creature->setEventType(CREATURE_EVENT_THINK);
		} else if (tmpStr == "preparedeath") {
			creature->setEventType(CREATURE_EVENT_PREPAREDEATH);
		} else if (tmpStr == "death") {
			creature->setEventType(CREATURE_EVENT_DEATH);
		} else if (tmpStr == "kill") {
			creature->setEventType(CREATURE_EVENT_KILL);
		} else if (tmpStr == "advance") {
			creature->setEventType(CREATURE_EVENT_ADVANCE);
		} else if (tmpStr == "modalwindow") {
			creature->setEventType(CREATURE_EVENT_MODALWINDOW);
		} else if (tmpStr == "textedit") {
			creature->setEventType(CREATURE_EVENT_TEXTEDIT);
		} else if (tmpStr == "healthchange") {
			creature->setEventType(CREATURE_EVENT_HEALTHCHANGE);
		} else if (tmpStr == "manachange") {
			creature->setEventType(CREATURE_EVENT_MANACHANGE);
		} else if (tmpStr == "extendedopcode") {
			creature->setEventType(CREATURE_EVENT_EXTENDED_OPCODE);
		} else {
			std::cout << "[Error - CreatureEvent::configureLuaEvent] Invalid type for creature event: " << typeName << std::endl;
			pushBoolean(L, false);
		}
		creature->setLoaded(true);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureEventRegister(lua_State* L)
{
	// creatureevent:register()
	CreatureEvent* creature = getUserdata<CreatureEvent>(L, 1);
	if (creature) {
		if (!creature->isScripted()) {
			pushBoolean(L, false);
			return 1;
		}
		pushBoolean(L, g_creatureEvents->registerLuaEvent(creature));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCreatureEventOnCallback(lua_State* L)
{
	// creatureevent:onLogin / logout / etc. (callback)
	CreatureEvent* creature = getUserdata<CreatureEvent>(L, 1);
	if (creature) {
		if (!creature->loadCallback()) {
			pushBoolean(L, false);
			return 1;
		}
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCreateMoveEvent(lua_State* L)
{
	// MoveEvent()
	if (getScriptEnv()->getScriptInterface() != &g_scripts->getScriptInterface()) {
		reportErrorFunc(L, "MoveEvents can only be registered in the Scripts interface.");
		lua_pushnil(L);
		return 1;
	}

	MoveEvent* moveevent = new MoveEvent(getScriptEnv()->getScriptInterface());
	if (moveevent) {
		moveevent->fromLua = true;
		pushUserdata<MoveEvent>(L, moveevent);
		setMetatable(L, -1, "MoveEvent");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMoveEventType(lua_State* L)
{
	// moveevent:type(callback)
	MoveEvent* moveevent = getUserdata<MoveEvent>(L, 1);
	if (moveevent) {
		std::string typeName = getString(L, 2);
		std::string tmpStr = asLowerCaseString(typeName);
		if (tmpStr == "stepin") {
			moveevent->setEventType(MOVE_EVENT_STEP_IN);
			moveevent->stepFunction = moveevent->StepInField;
		} else if (tmpStr == "stepout") {
			moveevent->setEventType(MOVE_EVENT_STEP_OUT);
			moveevent->stepFunction = moveevent->StepOutField;
		} else if (tmpStr == "equip") {
			moveevent->setEventType(MOVE_EVENT_EQUIP);
			moveevent->equipFunction = moveevent->EquipItem;
		} else if (tmpStr == "deequip") {
			moveevent->setEventType(MOVE_EVENT_DEEQUIP);
			moveevent->equipFunction = moveevent->DeEquipItem;
		} else if (tmpStr == "additem") {
			moveevent->setEventType(MOVE_EVENT_ADD_ITEM);
			moveevent->moveFunction = moveevent->AddItemField;
		} else if (tmpStr == "removeitem") {
			moveevent->setEventType(MOVE_EVENT_REMOVE_ITEM);
			moveevent->moveFunction = moveevent->RemoveItemField;
		} else {
			std::cout << "Error: [MoveEvent::configureMoveEvent] No valid event name " << typeName << std::endl;
			pushBoolean(L, false);
		}
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMoveEventRegister(lua_State* L)
{
	// moveevent:register()
	MoveEvent* moveevent = getUserdata<MoveEvent>(L, 1);
	if (moveevent) {
		if ((moveevent->getEventType() == MOVE_EVENT_EQUIP || moveevent->getEventType() == MOVE_EVENT_DEEQUIP) && moveevent->getSlot() == SLOTP_WHEREEVER) {
			uint32_t id = moveevent->getItemIdRange().at(0);
			ItemType& it = Item::items.getItemType(id);
			moveevent->setSlot(it.slotPosition);
		}
		if (!moveevent->isScripted()) {
			pushBoolean(L, g_moveEvents->registerLuaFunction(moveevent));
			return 1;
		}
		pushBoolean(L, g_moveEvents->registerLuaEvent(moveevent));
		moveevent->clearItemIdRange();
		moveevent->clearActionIdRange();
		moveevent->clearUniqueIdRange();
		moveevent->clearPosList();
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMoveEventOnCallback(lua_State* L)
{
	// moveevent:onEquip / deEquip / etc. (callback)
	MoveEvent* moveevent = getUserdata<MoveEvent>(L, 1);
	if (moveevent) {
		if (!moveevent->loadCallback()) {
			pushBoolean(L, false);
			return 1;
		}
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMoveEventSlot(lua_State* L)
{
	// moveevent:slot(slot)
	MoveEvent* moveevent = getUserdata<MoveEvent>(L, 1);
	if (!moveevent) {
		lua_pushnil(L);
		return 1;
	}

	if (moveevent->getEventType() == MOVE_EVENT_EQUIP || moveevent->getEventType() == MOVE_EVENT_DEEQUIP) {
		std::string slotName = asLowerCaseString(getString(L, 2));
		if (slotName == "head") {
			moveevent->setSlot(SLOTP_HEAD);
		} else if (slotName == "necklace") {
			moveevent->setSlot(SLOTP_NECKLACE);
		} else if (slotName == "backpack") {
			moveevent->setSlot(SLOTP_BACKPACK);
		} else if (slotName == "armor" || slotName == "body") {
			moveevent->setSlot(SLOTP_ARMOR);
		} else if (slotName == "right-hand") {
			moveevent->setSlot(SLOTP_RIGHT);
		} else if (slotName == "left-hand") {
			moveevent->setSlot(SLOTP_LEFT);
		} else if (slotName == "hand" || slotName == "shield") {
			moveevent->setSlot(SLOTP_RIGHT | SLOTP_LEFT);
		} else if (slotName == "legs") {
			moveevent->setSlot(SLOTP_LEGS);
		} else if (slotName == "feet") {
			moveevent->setSlot(SLOTP_FEET);
		} else if (slotName == "ring") {
			moveevent->setSlot(SLOTP_RING);
		} else if (slotName == "ammo") {
			moveevent->setSlot(SLOTP_AMMO);
		} else {
			std::cout << "[Warning - MoveEvent::configureMoveEvent] Unknown slot type: " << slotName << std::endl;
			pushBoolean(L, false);
			return 1;
		}
	}

	pushBoolean(L, true);
	return 1;
}

int LuaScriptInterface::luaMoveEventLevel(lua_State* L)
{
	// moveevent:level(lvl)
	MoveEvent* moveevent = getUserdata<MoveEvent>(L, 1);
	if (moveevent) {
		moveevent->setRequiredLevel(getNumber<uint32_t>(L, 2));
		moveevent->setWieldInfo(WIELDINFO_LEVEL);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMoveEventMagLevel(lua_State* L)
{
	// moveevent:magicLevel(lvl)
	MoveEvent* moveevent = getUserdata<MoveEvent>(L, 1);
	if (moveevent) {
		moveevent->setRequiredMagLevel(getNumber<uint32_t>(L, 2));
		moveevent->setWieldInfo(WIELDINFO_MAGLV);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMoveEventPremium(lua_State* L)
{
	// moveevent:premium(bool)
	MoveEvent* moveevent = getUserdata<MoveEvent>(L, 1);
	if (moveevent) {
		moveevent->setNeedPremium(getBoolean(L, 2));
		moveevent->setWieldInfo(WIELDINFO_PREMIUM);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMoveEventVocation(lua_State* L)
{
	// moveevent:vocation(vocName[, showInDescription = false, lastVoc = false])
	MoveEvent* moveevent = getUserdata<MoveEvent>(L, 1);
	if (moveevent) {
		moveevent->addVocEquipMap(getString(L, 2));
		moveevent->setWieldInfo(WIELDINFO_VOCREQ);
		std::string tmp;
		bool showInDescription = false;
		bool lastVoc = false;
		if (getBoolean(L, 3)) {
			showInDescription = getBoolean(L, 3);
		}
		if (getBoolean(L, 4)) {
			lastVoc = getBoolean(L, 4);
		}
		if (showInDescription) {
			if (moveevent->getVocationString().empty()) {
				tmp = asLowerCaseString(getString(L, 2));
				tmp += "s";
				moveevent->setVocationString(tmp);
			} else {
				tmp = moveevent->getVocationString();
				if (lastVoc) {
					tmp += " and ";
				} else {
					tmp += ", ";
				}
				tmp += asLowerCaseString(getString(L, 2));
				tmp += "s";
				moveevent->setVocationString(tmp);
			}
		}
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMoveEventTileItem(lua_State* L)
{
	// moveevent:tileItem(bool)
	MoveEvent* moveevent = getUserdata<MoveEvent>(L, 1);
	if (moveevent) {
		moveevent->setTileItem(getBoolean(L, 2));
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMoveEventItemId(lua_State* L)
{
	// moveevent:id(ids)
	MoveEvent* moveevent = getUserdata<MoveEvent>(L, 1);
	if (moveevent) {
		int parameters = lua_gettop(L) - 1; // - 1 because self is a parameter aswell, which we want to skip ofc
		if (parameters > 1) {
			for (int i = 0; i < parameters; ++i) {
				moveevent->addItemId(getNumber<uint32_t>(L, 2 + i));
			}
		} else {
			moveevent->addItemId(getNumber<uint32_t>(L, 2));
		}
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMoveEventActionId(lua_State* L)
{
	// moveevent:aid(ids)
	MoveEvent* moveevent = getUserdata<MoveEvent>(L, 1);
	if (moveevent) {
		int parameters = lua_gettop(L) - 1; // - 1 because self is a parameter aswell, which we want to skip ofc
		if (parameters > 1) {
			for (int i = 0; i < parameters; ++i) {
				moveevent->addActionId(getNumber<uint32_t>(L, 2 + i));
			}
		} else {
			moveevent->addActionId(getNumber<uint32_t>(L, 2));
		}
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMoveEventUniqueId(lua_State* L)
{
	// moveevent:uid(ids)
	MoveEvent* moveevent = getUserdata<MoveEvent>(L, 1);
	if (moveevent) {
		int parameters = lua_gettop(L) - 1; // - 1 because self is a parameter aswell, which we want to skip ofc
		if (parameters > 1) {
			for (int i = 0; i < parameters; ++i) {
				moveevent->addUniqueId(getNumber<uint32_t>(L, 2 + i));
			}
		} else {
			moveevent->addUniqueId(getNumber<uint32_t>(L, 2));
		}
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaMoveEventPosition(lua_State* L)
{
	// moveevent:position(positions)
	MoveEvent* moveevent = getUserdata<MoveEvent>(L, 1);
	if (moveevent) {
		int parameters = lua_gettop(L) - 1; // - 1 because self is a parameter aswell, which we want to skip ofc
		if (parameters > 1) {
			for (int i = 0; i < parameters; ++i) {
				moveevent->addPosList(getPosition(L, 2 + i));
			}
		} else {
			moveevent->addPosList(getPosition(L, 2));
		}
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaCreateGlobalEvent(lua_State* L)
{
	// GlobalEvent(eventName)
	if (getScriptEnv()->getScriptInterface() != &g_scripts->getScriptInterface()) {
		reportErrorFunc(L, "GlobalEvents can only be registered in the Scripts interface.");
		lua_pushnil(L);
		return 1;
	}

	GlobalEvent* global = new GlobalEvent(getScriptEnv()->getScriptInterface());
	if (global) {
		global->setName(getString(L, 2));
		global->setEventType(GLOBALEVENT_NONE);
		global->fromLua = true;
		pushUserdata<GlobalEvent>(L, global);
		setMetatable(L, -1, "GlobalEvent");
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaGlobalEventType(lua_State* L)
{
	// globalevent:type(callback)
	GlobalEvent* global = getUserdata<GlobalEvent>(L, 1);
	if (global) {
		std::string typeName = getString(L, 2);
		std::string tmpStr = asLowerCaseString(typeName);
		if (tmpStr == "startup") {
			global->setEventType(GLOBALEVENT_STARTUP);
		} else if (tmpStr == "shutdown") {
			global->setEventType(GLOBALEVENT_SHUTDOWN);
		} else if (tmpStr == "record") {
			global->setEventType(GLOBALEVENT_RECORD);
		} else if (tmpStr == "save") {
			global->setEventType(GLOBALEVENT_SAVE);
		} else {
			std::cout << "[Error - CreatureEvent::configureLuaEvent] Invalid type for global event: " << typeName << std::endl;
			pushBoolean(L, false);
		}
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaGlobalEventRegister(lua_State* L)
{
	// globalevent:register()
	GlobalEvent* globalevent = getUserdata<GlobalEvent>(L, 1);
	if (globalevent) {
		if (!globalevent->isScripted()) {
			pushBoolean(L, false);
			return 1;
		}
		
		if (globalevent->getEventType() == GLOBALEVENT_NONE && globalevent->getInterval() == 0) {
			std::cout << "[Error - LuaScriptInterface::luaGlobalEventRegister] No interval for globalevent with name " << globalevent->getName() << std::endl;
			pushBoolean(L, false);
			return 1;
		}
		
		pushBoolean(L, g_globalEvents->registerLuaEvent(globalevent));
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaGlobalEventOnCallback(lua_State* L)
{
	// globalevent:onThink / record / etc. (callback)
	GlobalEvent* globalevent = getUserdata<GlobalEvent>(L, 1);
	if (globalevent) {
		if (!globalevent->loadCallback()) {
			pushBoolean(L, false);
			return 1;
		}
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaGlobalEventTime(lua_State* L)
{
	// globalevent:time(time)
	GlobalEvent* globalevent = getUserdata<GlobalEvent>(L, 1);
	if (globalevent) {
		std::string timer = getString(L, 2);
		std::vector<int32_t> params = vectorAtoi(explodeString(timer, ":"));

		int32_t hour = params.front();
		if (hour < 0 || hour > 23) {
			std::cout << "[Error - GlobalEvent::configureEvent] Invalid hour \"" << timer << "\" for globalevent with name: " << globalevent->getName() << std::endl;
			pushBoolean(L, false);
			return 1;
		}

		globalevent->setInterval(hour << 16);

		int32_t min = 0;
		int32_t sec = 0;
		if (params.size() > 1) {
			min = params[1];
			if (min < 0 || min > 59) {
				std::cout << "[Error - GlobalEvent::configureEvent] Invalid minute \"" << timer << "\" for globalevent with name: " << globalevent->getName() << std::endl;
				pushBoolean(L, false);
				return 1;
			}

			if (params.size() > 2) {
				sec = params[2];
				if (sec < 0 || sec > 59) {
					std::cout << "[Error - GlobalEvent::configureEvent] Invalid second \"" << timer << "\" for globalevent with name: " << globalevent->getName() << std::endl;
					pushBoolean(L, false);
					return 1;
				}
			}
		}

		time_t current_time = time(nullptr);
		tm* timeinfo = localtime(&current_time);
		timeinfo->tm_hour = hour;
		timeinfo->tm_min = min;
		timeinfo->tm_sec = sec;

		time_t difference = static_cast<time_t>(difftime(mktime(timeinfo), current_time));
		if (difference < 0) {
			difference += 86400;
		}

		globalevent->setNextExecution(current_time + difference);
		globalevent->setEventType(GLOBALEVENT_TIMER);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaGlobalEventInterval(lua_State* L)
{
	// globalevent:interval(interval)
	GlobalEvent* globalevent = getUserdata<GlobalEvent>(L, 1);
	if (globalevent) {
		globalevent->setInterval(getNumber<uint32_t>(L, 2));
		globalevent->setNextExecution(OTSYS_TIME() + getNumber<uint32_t>(L, 2));
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// Weapon
int LuaScriptInterface::luaCreateWeapon(lua_State* L)
{
	// Weapon(type)
	if (getScriptEnv()->getScriptInterface() != &g_scripts->getScriptInterface()) {
		reportErrorFunc(L, "Weapons can only be registered in the Scripts interface.");
		lua_pushnil(L);
		return 1;
	}

	WeaponType_t type = getNumber<WeaponType_t>(L, 2);
	switch (type) {
		case WEAPON_SWORD:
		case WEAPON_AXE:
		case WEAPON_CLUB: {
			WeaponMelee* weapon = new WeaponMelee(getScriptEnv()->getScriptInterface());
			if (weapon) {
				pushUserdata<WeaponMelee>(L, weapon);
				setMetatable(L, -1, "Weapon");
				weapon->weaponType = type;
				weapon->fromLua = true;
			} else {
				lua_pushnil(L);
			}
			break;
		}
		case WEAPON_DISTANCE:
		case WEAPON_AMMO: {
			WeaponDistance* weapon = new WeaponDistance(getScriptEnv()->getScriptInterface());
			if (weapon) {
				pushUserdata<WeaponDistance>(L, weapon);
				setMetatable(L, -1, "Weapon");
				weapon->weaponType = type;
				weapon->fromLua = true;
			} else {
				lua_pushnil(L);
			}
			break;
		}
		case WEAPON_WAND: {
			WeaponWand* weapon = new WeaponWand(getScriptEnv()->getScriptInterface());
			if (weapon) {
				pushUserdata<WeaponWand>(L, weapon);
				setMetatable(L, -1, "Weapon");
				weapon->weaponType = type;
				weapon->fromLua = true;
			} else {
				lua_pushnil(L);
			}
			break;
		}
		default: {
			lua_pushnil(L);
			break;
		}
	}
	return 1;
}

int LuaScriptInterface::luaWeaponAction(lua_State* L)
{
	// weapon:action(callback)
	Weapon* weapon = getUserdata<Weapon>(L, 1);
	if (weapon) {
		std::string typeName = getString(L, 2);
		std::string tmpStr = asLowerCaseString(typeName);
		if (tmpStr == "removecount") {
			weapon->action = WEAPONACTION_REMOVECOUNT;
		} else if (tmpStr == "removecharge") {
			weapon->action = WEAPONACTION_REMOVECHARGE;
		} else if (tmpStr == "move") {
			weapon->action = WEAPONACTION_MOVE;
		} else {
			std::cout << "Error: [Weapon::action] No valid action " << typeName << std::endl;
			pushBoolean(L, false);
		}
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaWeaponRegister(lua_State* L)
{
	// weapon:register()
	Weapon** weaponPtr = getRawUserdata<Weapon>(L, 1);
	if (!weaponPtr) {
		lua_pushnil(L);
		return 1;
	}

	if (auto* weapon = *weaponPtr) {
		if (weapon->weaponType == WEAPON_DISTANCE || weapon->weaponType == WEAPON_AMMO) {
			weapon = getUserdata<WeaponDistance>(L, 1);
		} else if (weapon->weaponType == WEAPON_WAND) {
			weapon = getUserdata<WeaponWand>(L, 1);
		} else {
			weapon = getUserdata<WeaponMelee>(L, 1);
		}

		uint16_t id = weapon->getID();
		ItemType& it = Item::items.getItemType(id);
		it.weaponType = weapon->weaponType;

		if (weapon->getWieldInfo() != 0) {
			it.wieldInfo = weapon->getWieldInfo();
			it.vocationString = weapon->getVocationString();
			it.minReqLevel = weapon->getReqLevel();
			it.minReqMagicLevel = weapon->getReqMagLv();
		}

		weapon->configureWeapon(it);
		pushBoolean(L, g_weapons->registerLuaEvent(weapon));
		*weaponPtr = nullptr; // Remove luascript reference
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaWeaponOnUseWeapon(lua_State* L)
{
	// weapon:onUseWeapon(callback)
	Weapon* weapon = getUserdata<Weapon>(L, 1);
	if (weapon) {
		if (!weapon->loadCallback()) {
			pushBoolean(L, false);
			return 1;
		}
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaWeaponUnproperly(lua_State* L)
{
	// weapon:wieldedUnproperly(bool)
	Weapon* weapon = getUserdata<Weapon>(L, 1);
	if (weapon) {
		weapon->setWieldUnproperly(getBoolean(L, 2));
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaWeaponLevel(lua_State* L)
{
	// weapon:level(lvl)
	Weapon* weapon = getUserdata<Weapon>(L, 1);
	if (weapon) {
		weapon->setRequiredLevel(getNumber<uint32_t>(L, 2));
		weapon->setWieldInfo(WIELDINFO_LEVEL);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaWeaponMagicLevel(lua_State* L)
{
	// weapon:magicLevel(lvl)
	Weapon* weapon = getUserdata<Weapon>(L, 1);
	if (weapon) {
		weapon->setRequiredMagLevel(getNumber<uint32_t>(L, 2));
		weapon->setWieldInfo(WIELDINFO_MAGLV);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaWeaponMana(lua_State* L)
{
	// weapon:mana(mana)
	Weapon* weapon = getUserdata<Weapon>(L, 1);
	if (weapon) {
		weapon->setMana(getNumber<uint32_t>(L, 2));
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaWeaponManaPercent(lua_State* L)
{
	// weapon:manaPercent(percent)
	Weapon* weapon = getUserdata<Weapon>(L, 1);
	if (weapon) {
		weapon->setManaPercent(getNumber<uint32_t>(L, 2));
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaWeaponHealth(lua_State* L)
{
	// weapon:health(health)
	Weapon* weapon = getUserdata<Weapon>(L, 1);
	if (weapon) {
		weapon->setHealth(getNumber<int32_t>(L, 2));
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaWeaponHealthPercent(lua_State* L)
{
	// weapon:healthPercent(percent)
	Weapon* weapon = getUserdata<Weapon>(L, 1);
	if (weapon) {
		weapon->setHealthPercent(getNumber<uint32_t>(L, 2));
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaWeaponSoul(lua_State* L)
{
	// weapon:soul(soul)
	Weapon* weapon = getUserdata<Weapon>(L, 1);
	if (weapon) {
		weapon->setSoul(getNumber<uint32_t>(L, 2));
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaWeaponBreakChance(lua_State* L)
{
	// weapon:breakChance(percent)
	Weapon* weapon = getUserdata<Weapon>(L, 1);
	if (weapon) {
		weapon->setBreakChance(getNumber<uint32_t>(L, 2));
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaWeaponWandDamage(lua_State* L)
{
	// weapon:damage(damage[min, max]) only use this if the weapon is a wand!
	WeaponWand* weapon = getUserdata<WeaponWand>(L, 1);
	if (weapon) {
		weapon->setMinChange(getNumber<uint32_t>(L, 2));
		if (lua_gettop(L) > 2) {
			weapon->setMaxChange(getNumber<uint32_t>(L, 3));
		} else {
			weapon->setMaxChange(getNumber<uint32_t>(L, 2));
		}
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaWeaponElement(lua_State* L)
{
	// weapon:element(combatType)
	Weapon* weapon = getUserdata<Weapon>(L, 1);
	if (weapon) {
		if (!getNumber<CombatType_t>(L, 2)) {
			std::string element = getString(L, 2);
			std::string tmpStrValue = asLowerCaseString(element);
			if (tmpStrValue == "earth") {
				weapon->params.combatType = COMBAT_EARTHDAMAGE;
			} else if (tmpStrValue == "ice") {
				weapon->params.combatType = COMBAT_ICEDAMAGE;
			} else if (tmpStrValue == "energy") {
				weapon->params.combatType = COMBAT_ENERGYDAMAGE;
			} else if (tmpStrValue == "fire") {
				weapon->params.combatType = COMBAT_FIREDAMAGE;
			} else if (tmpStrValue == "death") {
				weapon->params.combatType = COMBAT_DEATHDAMAGE;
			} else if (tmpStrValue == "holy") {
				weapon->params.combatType = COMBAT_HOLYDAMAGE;
			} else {
				std::cout << "[Warning - weapon:element] Type \"" << element << "\" does not exist." << std::endl;
			}
		} else {
			weapon->params.combatType = getNumber<CombatType_t>(L, 2);
		}
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaWeaponPremium(lua_State* L)
{
	// weapon:premium(bool)
	Weapon* weapon = getUserdata<Weapon>(L, 1);
	if (weapon) {
		weapon->setNeedPremium(getBoolean(L, 2));
		weapon->setWieldInfo(WIELDINFO_PREMIUM);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaWeaponVocation(lua_State* L)
{
	// weapon:vocation(vocName[, showInDescription = false, lastVoc = false])
	Weapon* weapon = getUserdata<Weapon>(L, 1);
	if (weapon) {
		weapon->addVocWeaponMap(getString(L, 2));
		weapon->setWieldInfo(WIELDINFO_VOCREQ);
		std::string tmp;
		bool showInDescription = getBoolean(L, 3, false);
		bool lastVoc = getBoolean(L, 4, false);

		if (showInDescription) {
			if (weapon->getVocationString().empty()) {
				tmp = asLowerCaseString(getString(L, 2));
				tmp += "s";
				weapon->setVocationString(tmp);
			} else {
				tmp = weapon->getVocationString();
				if (lastVoc) {
					tmp += " and ";
				} else {
					tmp += ", ";
				}
				tmp += asLowerCaseString(getString(L, 2));
				tmp += "s";
				weapon->setVocationString(tmp);
			}
		}
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaWeaponId(lua_State* L)
{
	// weapon:id(id)
	Weapon* weapon = getUserdata<Weapon>(L, 1);
	if (weapon) {
		weapon->setID(getNumber<uint32_t>(L, 2));
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaWeaponAttack(lua_State* L)
{
	// weapon:attack(atk)
	Weapon* weapon = getUserdata<Weapon>(L, 1);
	if (weapon) {
		uint16_t id = weapon->getID();
		ItemType& it = Item::items.getItemType(id);
		it.attack = getNumber<int32_t>(L, 2);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaWeaponDefense(lua_State* L)
{
	// weapon:defense(defense[, extraDefense])
	Weapon* weapon = getUserdata<Weapon>(L, 1);
	if (weapon) {
		uint16_t id = weapon->getID();
		ItemType& it = Item::items.getItemType(id);
		it.defense = getNumber<int32_t>(L, 2);
		if (lua_gettop(L) > 2) {
			it.extraDefense = getNumber<int32_t>(L, 3);
		}
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaWeaponRange(lua_State* L)
{
	// weapon:range(range)
	Weapon* weapon = getUserdata<Weapon>(L, 1);
	if (weapon) {
		uint16_t id = weapon->getID();
		ItemType& it = Item::items.getItemType(id);
		it.shootRange = getNumber<uint8_t>(L, 2);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaWeaponCharges(lua_State* L)
{
	// weapon:charges(charges[, showCharges = true])
	Weapon* weapon = getUserdata<Weapon>(L, 1);
	if (weapon) {
		bool showCharges = getBoolean(L, 3, true);
		uint16_t id = weapon->getID();
		ItemType& it = Item::items.getItemType(id);

		it.charges = getNumber<uint32_t>(L, 2);
		it.showCharges = showCharges;
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaWeaponDuration(lua_State* L)
{
	// weapon:duration(duration[, showDuration = true])
	Weapon* weapon = getUserdata<Weapon>(L, 1);
	if (weapon) {
		bool showDuration = getBoolean(L, 3, true);
		uint16_t id = weapon->getID();
		ItemType& it = Item::items.getItemType(id);

		it.decayTime = getNumber<uint32_t>(L, 2);
		it.showDuration = showDuration;
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaWeaponDecayTo(lua_State* L)
{
	// weapon:decayTo([itemid = 0])
	Weapon* weapon = getUserdata<Weapon>(L, 1);
	if (weapon) {
		uint16_t itemid = getNumber<uint16_t>(L, 2, 0);
		uint16_t id = weapon->getID();
		ItemType& it = Item::items.getItemType(id);

		it.decayTo = itemid;
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaWeaponTransformEquipTo(lua_State* L)
{
	// weapon:transformEquipTo(itemid)
	Weapon* weapon = getUserdata<Weapon>(L, 1);
	if (weapon) {
		uint16_t id = weapon->getID();
		ItemType& it = Item::items.getItemType(id);
		it.transformEquipTo = getNumber<uint16_t>(L, 2);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaWeaponTransformDeEquipTo(lua_State* L)
{
	// weapon:transformDeEquipTo(itemid)
	Weapon* weapon = getUserdata<Weapon>(L, 1);
	if (weapon) {
		uint16_t id = weapon->getID();
		ItemType& it = Item::items.getItemType(id);
		it.transformDeEquipTo = getNumber<uint16_t>(L, 2);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaWeaponShootType(lua_State* L)
{
	// weapon:shootType(type)
	Weapon* weapon = getUserdata<Weapon>(L, 1);
	if (weapon) {
		uint16_t id = weapon->getID();
		ItemType& it = Item::items.getItemType(id);
		it.shootType = getNumber<ShootType_t>(L, 2);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaWeaponSlotType(lua_State* L)
{
	// weapon:slotType(slot)
	Weapon* weapon = getUserdata<Weapon>(L, 1);
	if (weapon) {
		uint16_t id = weapon->getID();
		ItemType& it = Item::items.getItemType(id);
		std::string slot = getString(L, 2);

		if (slot == "two-handed") {
			it.slotPosition |= SLOTP_TWO_HAND;
		} else {
			it.slotPosition |= SLOTP_HAND;
		}
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaWeaponAmmoType(lua_State* L)
{
	// weapon:ammoType(type)
	WeaponDistance* weapon = getUserdata<WeaponDistance>(L, 1);
	if (weapon) {
		uint16_t id = weapon->getID();
		ItemType& it = Item::items.getItemType(id);
		std::string type = getString(L, 2);

		if (type == "arrow") {
			it.ammoType = AMMO_ARROW;
		} else if (type == "bolt"){
			it.ammoType = AMMO_BOLT;
		} else {
			std::cout << "[Warning - weapon:ammoType] Type \"" << type << "\" does not exist." << std::endl;
			lua_pushnil(L);
			return 1;
		}
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaWeaponHitChance(lua_State* L)
{
	// weapon:hitChance(chance)
	Weapon* weapon = getUserdata<Weapon>(L, 1);
	if (weapon) {
		uint16_t id = weapon->getID();
		ItemType& it = Item::items.getItemType(id);
		it.hitChance = getNumber<int8_t>(L, 2);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaWeaponMaxHitChance(lua_State* L)
{
	// weapon:maxHitChance(max)
	Weapon* weapon = getUserdata<Weapon>(L, 1);
	if (weapon) {
		uint16_t id = weapon->getID();
		ItemType& it = Item::items.getItemType(id);
		it.maxHitChance = getNumber<int32_t>(L, 2);
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaWeaponExtraElement(lua_State* L)
{
	// weapon:extraElement(atk, combatType)
	Weapon* weapon = getUserdata<Weapon>(L, 1);
	if (weapon) {
		uint16_t id = weapon->getID();
		ItemType& it = Item::items.getItemType(id);
		it.abilities.get()->elementDamage = getNumber<uint16_t>(L, 2);

		if (!getNumber<CombatType_t>(L, 3)) {
			std::string element = getString(L, 3);
			std::string tmpStrValue = asLowerCaseString(element);
			if (tmpStrValue == "earth") {
				it.abilities.get()->elementType = COMBAT_EARTHDAMAGE;
			} else if (tmpStrValue == "ice") {
				it.abilities.get()->elementType = COMBAT_ICEDAMAGE;
			} else if (tmpStrValue == "energy") {
				it.abilities.get()->elementType = COMBAT_ENERGYDAMAGE;
			} else if (tmpStrValue == "fire") {
				it.abilities.get()->elementType = COMBAT_FIREDAMAGE;
			} else if (tmpStrValue == "death") {
				it.abilities.get()->elementType = COMBAT_DEATHDAMAGE;
			} else if (tmpStrValue == "holy") {
				it.abilities.get()->elementType = COMBAT_HOLYDAMAGE;
			} else {
				std::cout << "[Warning - weapon:extraElement] Type \"" << element << "\" does not exist." << std::endl;
			}
		} else {
			it.abilities.get()->elementType = getNumber<CombatType_t>(L, 3);
		}
		pushBoolean(L, true);
	} else {
		lua_pushnil(L);
	}
	return 1;
}

// XML
int LuaScriptInterface::luaCreateXmlDocument(lua_State* L)
{
	// XMLDocument(filename)
	std::string filename = getString(L, 2);
	if (filename.empty()) {
		lua_pushnil(L);
		return 1;
	}

	auto doc = std::make_unique<pugi::xml_document>();
	if (auto result = doc->load_file(filename.c_str())) {
		pushUserdata<pugi::xml_document>(L, doc.release());
		setMetatable(L, -1, "XMLDocument");
	}
	else {
		printXMLError("Error - LuaScriptInterface::luaCreateXmlDocument", filename, result);
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaDeleteXmlDocument(lua_State* L)
{
	// doc:delete() or doc:__gc()
	pugi::xml_document** document = getRawUserdata<pugi::xml_document>(L, 1);
	if (document && *document) {
		delete* document;
		*document = nullptr;
	}
	return 1;
}

int LuaScriptInterface::luaXmlDocumentChild(lua_State* L)
{
	// doc:child(name)
	pugi::xml_document* document = getUserdata<pugi::xml_document>(L, 1);
	if (!document) {
		lua_pushnil(L);
		return 1;
	}

	std::string name = getString(L, 2);
	if (name.empty()) {
		lua_pushnil(L);
		return 1;
	}

	auto node = std::make_unique<pugi::xml_node>(document->child(name.c_str()));
	pushUserdata<pugi::xml_node>(L, node.release());
	setMetatable(L, -1, "XMLNode");
	return 1;
}

int LuaScriptInterface::luaDeleteXmlNode(lua_State* L)
{
	// node:delete() or node:__gc()
	pugi::xml_node** node = getRawUserdata<pugi::xml_node>(L, 1);
	if (node && *node) {
		delete* node;
		*node = nullptr;
	}
	return 1;
}

int LuaScriptInterface::luaXmlNodeAttribute(lua_State* L)
{
	// node:attribute(name)
	pugi::xml_node* node = getUserdata<pugi::xml_node>(L, 1);
	if (!node) {
		lua_pushnil(L);
		return 1;
	}

	std::string name = getString(L, 2);
	if (name.empty()) {
		lua_pushnil(L);
		return 1;
	}

	pugi::xml_attribute attribute = node->attribute(name.c_str());
	if (attribute) {
		pushString(L, attribute.value());
	}
	else {
		lua_pushnil(L);
	}
	return 1;
}

int LuaScriptInterface::luaXmlNodeName(lua_State* L)
{
	// node:name()
	pugi::xml_node* node = getUserdata<pugi::xml_node>(L, 1);
	if (!node) {
		lua_pushnil(L);
		return 1;
	}

	pushString(L, node->name());
	return 1;
}

int LuaScriptInterface::luaXmlNodeFirstChild(lua_State* L)
{
	// node:firstChild()
	pugi::xml_node* node = getUserdata<pugi::xml_node>(L, 1);
	if (!node) {
		lua_pushnil(L);
		return 1;
	}

	auto firstChild = node->first_child();
	if (!firstChild) {
		lua_pushnil(L);
		return 1;
	}

	auto newNode = std::make_unique<pugi::xml_node>(std::move(firstChild));
	pushUserdata<pugi::xml_node>(L, newNode.release());
	setMetatable(L, -1, "XMLNode");
	return 1;
}

int LuaScriptInterface::luaXmlNodeNextSibling(lua_State* L)
{
	// node:nextSibling()
	pugi::xml_node* node = getUserdata<pugi::xml_node>(L, 1);
	if (!node) {
		lua_pushnil(L);
		return 1;
	}

	auto nextSibling = node->next_sibling();
	if (!nextSibling) {
		lua_pushnil(L);
		return 1;
	}

	auto newNode = std::make_unique<pugi::xml_node>(std::move(nextSibling));
	pushUserdata<pugi::xml_node>(L, newNode.release());
	setMetatable(L, -1, "XMLNode");
	return 1;
}

int LuaScriptInterface::luaGetZones(lua_State* L)
{
	auto& zones = Zones::get();
	auto index = 0;
	lua_createtable(L, zones.size(), 0);

	for (auto& zone : zones)
	{
		pushUserdata<Zone>(L, &zone);
		setMetatable(L, -1, "Zone");
		lua_rawseti(L, -2, ++index);
	}
	return 1;
}

int LuaScriptInterface::luaCreateZone(lua_State* L)
{
	// Zone(id)
	// looks up the zone by id
	// can return false if zone can't be found or id is 0;

	// Zone(id, positions)
	// creates zone manually

	// both return userdata
	if (auto zoneId = getNumber<int>(L, 2))
	{
		if (zoneId != 0)
		{
			if (auto hasPositions = isTable(L, 3))
			{
				std::vector<Position> positions{};
				positions.reserve(64);

				lua_pushnil(L);
				while (lua_next(L, 3) != 0) {
					if (isTable(L, -1)) {
						auto position = getPosition(L, -1);
						positions.emplace_back(position);
					}
					lua_pop(L, 1);
				}
				auto& zone = Zones::createZone(zoneId, positions);
				pushUserdata<Zone>(L, &zone);
				setMetatable(L, -1, "Zone");
				return 1;
			}
			else
			{
				auto& zone = Zones::getZone(zoneId);
				// we check the id here again to ensure we don't have a default initialied zone.
				if (zone.id != 0) 
				{
					pushUserdata<Zone>(L, &zone);
					setMetatable(L, -1, "Zone");
					return 1;
				}
			}
		} 
		else
		{
			// can't use zone id 0, could send log or console error
			lua_pushnil(L);
			return 1;
		}
	}

	// if they didn't include a number, we should probably throw console error

	lua_pushnil(L);
	return 1;
}

int LuaScriptInterface::luaDeleteZone(lua_State* L)
{
	// zone __gc()
	Zone** zonePtr = getRawUserdata<Zone>(L, 1);
	if (zonePtr && *zonePtr) {
		delete* zonePtr;
		*zonePtr = nullptr;
	}
	return 0;
}

int LuaScriptInterface::luaZoneId(lua_State* L)
{
	if (auto zone = getUserdata<Zone>(L, 1))
	{
		lua_pushnumber(L, zone->id);
		return 1;
	}
	// should be unreachable
	return 0;
}

int LuaScriptInterface::luaZoneCreatures(lua_State* L)
{
	// zone:getCreatures()
	// zone:getCreatures(creatureType)
	if (auto zone = getUserdata<Zone>(L, 1))
	{
		int index = 0;
		bool isFiltered = (isNumber(L, 2));
		CreatureType_t creatureType;
		lua_createtable(L, zone->positions.size() * 4, 0);

		if (isFiltered)
		{
			creatureType = getNumber<CreatureType_t>(L, 2);
		}

		for (auto& position : zone->positions)
		{
			if (auto tile = g_game.map.getTile(position.x, position.y, position.z))
			{
				if (tile->getCreatureCount() == 0)
				{
					continue;
				}

				for (auto& creature : *tile->getCreatures())
				{
					if (isFiltered and creature->getType() != creatureType) 
					{
						continue;
					}

					pushSharedPtr(L, creature);
					setCreatureMetatable(L, -1, creature);
					lua_rawseti(L, -2, ++index);
				}
			}
		}

		return 1;
	}
	// should be unreachable
	return 0;
}

int LuaScriptInterface::luaZoneGrounds(lua_State* L)
{
	// zone:getGrounds()
	if (auto zone = getUserdata<Zone>(L, 1))
	{
		lua_createtable(L, zone->positions.size(), 0);
		int index = 0;
		for (auto& position : zone->positions)
		{
			if (auto tile = g_game.map.getTile(position.x, position.y, position.z))
			{
				if (auto ground = tile->getGround()) 
				{
					pushSharedPtr(L, ground);
					setItemMetatable(L, -1, ground);
					lua_rawseti(L, -2, ++index);
				}
			}
		}
		return 1;
	}
	// should be unreachable
	return 0;
}

int LuaScriptInterface::luaZoneItems(lua_State* L)
{
	// zone::getItems()
	// zone::getItems(id)
	if (auto zone = getUserdata<Zone>(L, 1))
	{
		bool isFiltered = (isNumber(L, 2));
		int index = 0;
		uint16_t item_type = 0;
		lua_createtable(L, zone->positions.size() * 10, 0);

		if (isFiltered)
		{
			item_type = getNumber<uint16_t>(L, 2);
		}

		for (auto& position : zone->positions)
		{
			if (auto tile = g_game.map.getTile(position.x, position.y, position.z))
			{
				if (auto items = tile->getItemList())
				{
					for (auto& item : *items)
					{
						if (isFiltered and item->getID() != item_type)
						{
							continue;
						}

						pushSharedPtr(L, item);
						setItemMetatable(L, -1, item);
						lua_rawseti(L, -2, ++index);
					}
				}
			}
		}
		return 1;
	}
	// should be unreachable
	return 0;
}

int LuaScriptInterface::luaZoneTiles(lua_State* L)
{
	// zone::getTiles()
	// zone::getTiles(flags)
	if (auto zone = getUserdata<Zone>(L, 1))
	{
		int index = 0;
		auto isFiltered = (isNumber(L, 2));
		uint32_t flags = 0;

		if (isFiltered)
		{
			flags = (getNumber<uint32_t>(L, 2));
		}

		lua_createtable(L, zone->positions.size(), 0);

		for (auto& position : zone->positions)
		{
			if (auto tile = g_game.map.getTile(position.x, position.y, position.z))
			{
				if (isFiltered and (tile->getFlags() & flags) != flags)
				{
					continue;
				}
				pushSharedPtr(L, tile);
				setMetatable(L, -1, "Tile");
				lua_rawseti(L, -2, ++index);
			}
		}
		return 1;
	}
	// should be unreachable
	return 0;
}

int LuaScriptInterface::luaZoneCreatureCount(lua_State* L)
{
	// zone::getCreatureCount()
	// zone::getCreatureCount(CreatureType_t)
	if (auto zone = getUserdata<Zone>(L, 1))
	{
		auto c_count = 0;
		auto isFiltered = (isNumber(L, 2));
		CreatureType_t creatureType;

		if (isFiltered)
		{
			creatureType = (getNumber<CreatureType_t>(L, 2));
		}

		for (auto& position : zone->positions)
		{
			if (auto tile = g_game.map.getTile(position.x, position.y, position.z))
			{
				if (auto t_count = tile->getCreatureCount())
				{
					if (isFiltered)
					{
						for (auto& creature : *tile->getCreatures())
						{
							if (creature->getType() != creatureType)
							{
								continue;
							}
							++c_count;
						}
					}
					else // no filter, count them all
					{
						c_count += t_count;
					}
				}
			}
		}
		lua_pushnumber(L, c_count);
		return 1;
	}
	// should be unreachable
	return 0;
}

int LuaScriptInterface::luaZoneItemCount(lua_State* L)
{
	// zone::getItemCount()
	// zone::getItemCount(id)
	if (auto zone = getUserdata<Zone>(L, 1))
	{
		auto i_count = 0;
		auto isFiltered = (isNumber(L, 2));
		int itemId = 0;

		if (isFiltered)
		{
			itemId = (getNumber<int>(L, 2));
		}

		for (auto& position : zone->positions)
		{
			if (auto tile = g_game.map.getTile(position.x, position.y, position.z))
			{
				if (auto t_count = tile->getItemCount())
				{
					if (isFiltered)
					{
						for (auto& item : *tile->getItemList())
						{
							if (item->getID() != itemId)
							{
								continue;
							}
							++i_count;
						}
					}
					else // no filter, count them all
					{
						i_count += t_count;
					}
				}
			}
		}
		lua_pushnumber(L, i_count);
		return 1;
	}
	// should be unreachable
	return 0;
}

int LuaScriptInterface::luaZoneTileCount(lua_State* L)
{
	// zone::getTileCount()
	// zone::getTileCount(flags)
	if (auto zone = getUserdata<Zone>(L, 1))
	{
		auto t_count = 0;
		auto isFiltered = (isTable(L, 2));
		tileflags_t flags;

		if (isFiltered)
		{
			flags = (getNumber<tileflags_t>(L, 2));
		}

		for (auto& position : zone->positions)
		{
			if (auto tile = g_game.map.getTile(position.x, position.y, position.z))
			{
				if (isFiltered and (tile->getFlags() & flags) != flags)
				{
					continue;
				}
				++t_count;
			}
		}
		lua_pushnumber(L, t_count);
		return 1;
	}
	// should be unreachable
	return 0;
}

//
LuaEnvironment::LuaEnvironment() : LuaScriptInterface("Main Interface") {}

LuaEnvironment::~LuaEnvironment()
{
	delete testInterface;
	closeState();
}

bool LuaEnvironment::initState()
{
	luaState = luaL_newstate();
	if (!luaState) {
		return false;
	}

	luaL_openlibs(luaState);
	registerFunctions();

	runningEventId = EVENT_ID_USER;
	return true;
}

bool LuaEnvironment::reInitState()
{
	// TODO: get children, reload children
	closeState();
	return initState();
}

bool LuaEnvironment::closeState()
{
	if (!luaState) {
		return false;
	}

	for (const auto& combatEntry : combatIdMap) {
		clearCombatObjects(combatEntry.first);
	}

	for (const auto& areaEntry : areaIdMap) {
		clearAreaObjects(areaEntry.first);
	}

	for (auto& timerEntry : timerEvents) {
		LuaTimerEventDesc timerEventDesc = std::move(timerEntry.second);
		for (int32_t parameter : timerEventDesc.parameters) {
			luaL_unref(luaState, LUA_REGISTRYINDEX, parameter);
		}
		luaL_unref(luaState, LUA_REGISTRYINDEX, timerEventDesc.function);
	}

	combatIdMap.clear();
	areaIdMap.clear();
	timerEvents.clear();
	cacheFiles.clear();

	lua_close(luaState);
	luaState = nullptr;
	return true;
}

LuaScriptInterface* LuaEnvironment::getTestInterface()
{
	if (!testInterface) {
		testInterface = new LuaScriptInterface("Test Interface");
		testInterface->initState();
	}
	return testInterface;
}

Combat_ptr LuaEnvironment::getCombatObject(uint32_t id) const
{
	auto it = combatMap.find(id);
	if (it == combatMap.end()) {
		return nullptr;
	}
	return it->second;
}

Combat_ptr LuaEnvironment::createCombatObject(LuaScriptInterface* interface)
{
	Combat_ptr combat = std::make_shared<Combat>();
	combatMap[++lastCombatId] = combat;
	combatIdMap[interface].push_back(lastCombatId);
	return combat;
}

void LuaEnvironment::clearCombatObjects(LuaScriptInterface* interface)
{
	auto it = combatIdMap.find(interface);
	if (it == combatIdMap.end()) {
		return;
	}

	for (uint32_t id : it->second) {
		auto itt = combatMap.find(id);
		if (itt != combatMap.end()) {
			combatMap.erase(itt);
		}
	}
	it->second.clear();
}

AreaCombat* LuaEnvironment::getAreaObject(uint32_t id) const
{
	auto it = areaMap.find(id);
	if (it == areaMap.end()) {
		return nullptr;
	}
	return it->second;
}

uint32_t LuaEnvironment::createAreaObject(LuaScriptInterface* interface)
{
	areaMap[++lastAreaId] = new AreaCombat;
	areaIdMap[interface].push_back(lastAreaId);
	return lastAreaId;
}

void LuaEnvironment::clearAreaObjects(LuaScriptInterface* interface)
{
	auto it = areaIdMap.find(interface);
	if (it == areaIdMap.end()) {
		return;
	}

	for (uint32_t id : it->second) {
		auto itt = areaMap.find(id);
		if (itt != areaMap.end()) {
			delete itt->second;
			areaMap.erase(itt);
		}
	}
	it->second.clear();
}

void LuaEnvironment::executeTimerEvent(uint32_t eventIndex)
{
	auto it = timerEvents.find(eventIndex);
	if (it == timerEvents.end()) {
		return;
	}

	LuaTimerEventDesc timerEventDesc = std::move(it->second);
	timerEvents.erase(it);

	//push function
	lua_rawgeti(luaState, LUA_REGISTRYINDEX, timerEventDesc.function);

	//push parameters
	for (auto parameter : boost::adaptors::reverse(timerEventDesc.parameters)) {
		lua_rawgeti(luaState, LUA_REGISTRYINDEX, parameter);
	}

	//call the function
	if (reserveScriptEnv()) {
		ScriptEnvironment* env = getScriptEnv();
		env->setTimerEvent();
		env->setScriptId(timerEventDesc.scriptId, this);
		callFunction(timerEventDesc.parameters.size());
	} else {
		std::cout << "[Error - LuaScriptInterface::executeTimerEvent] Call stack overflow" << std::endl;
	}

	//free resources
	luaL_unref(luaState, LUA_REGISTRYINDEX, timerEventDesc.function);
	for (auto parameter : timerEventDesc.parameters) {
		luaL_unref(luaState, LUA_REGISTRYINDEX, parameter);
	}
}
