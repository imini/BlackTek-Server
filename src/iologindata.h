// Copyright 2024 Black Tek Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#ifndef FS_IOLOGINDATA_H
#define FS_IOLOGINDATA_H

#include "account.h"
#include "player.h"
#include "database.h"

using ItemBlockList = std::list<std::pair<int32_t, ItemPtr>>;

class IOLoginData
{
	public:
		static Account loadAccount(uint32_t accno);

		static bool loginserverAuthentication(const std::string& name, const std::string& password, Account& account);
		static std::pair<uint32_t, uint32_t> gameworldAuthentication(std::string_view accountName, std::string_view password, std::string_view characterName, std::string_view token, uint32_t tokenTime);
		static uint32_t getAccountIdByPlayerName(const std::string& playerName);
		static uint32_t getAccountIdByPlayerId(uint32_t playerId);

		static AccountType_t getAccountType(uint32_t accountId);
		static void setAccountType(uint32_t accountId, AccountType_t accountType);
		static std::pair<uint32_t, uint32_t> getAccountIdByAccountName(std::string_view accountName, std::string_view password, std::string_view characterName);
		static void updateOnlineStatus(uint32_t guid, bool login);
		static bool preloadPlayer(const PlayerPtr& player);

		static bool loadPlayerById(const PlayerPtr& player, uint32_t id);
		static bool loadPlayerByName(const PlayerPtr& player, const std::string& name);
		static bool loadPlayer(const PlayerPtr& player, DBResult_ptr result);
		static bool savePlayer(const PlayerPtr& player);
		static uint32_t getGuidByName(const std::string& name);
		static bool getGuidByNameEx(uint32_t& guid, bool& specialVip, std::string& name);
		static std::string getNameByGuid(uint32_t guid);
		static bool formatPlayerName(std::string& name);
		static void increaseBankBalance(uint32_t guid, uint64_t bankBalance);
		static bool hasBiddedOnHouse(uint32_t guid);

		static std::forward_list<VIPEntry> getVIPEntries(uint32_t accountId);
		static void addVIPEntry(uint32_t accountId, uint32_t guid, const std::string& description, uint32_t icon, bool notify);
		static void editVIPEntry(uint32_t accountId, uint32_t guid, const std::string& description, uint32_t icon, bool notify);
		static void removeVIPEntry(uint32_t accountId, uint32_t guid);

		static void updatePremiumTime(uint32_t accountId, time_t endTime);

		static bool addRewardItems(uint32_t playerId, const ItemBlockList& itemList, DBInsert& query_insert, PropWriteStream& propWriteStream);

		static bool accountExists(const std::string& accountName);

	private:
		using ItemMap = std::map<uint32_t, std::pair<ItemPtr, uint32_t>>;

		static void loadItems(ItemMap& itemMap, const DBResult_ptr& result);
		static bool saveItems(const PlayerConstPtr& player, const ItemBlockList& itemList, DBInsert& query_insert, PropWriteStream& propWriteStream);
		static bool saveAugments(const PlayerConstPtr& player, DBInsert& query_insert, PropWriteStream& augmentStream);
		static void loadPlayerAugments(std::vector<std::shared_ptr<Augment>>& augmentList, const DBResult_ptr& result);
		static void serializeCustomSkills(const PlayerConstPtr player, DBInsert query, PropWriteStream& binary_stream);
		static void serializeCustomSkills(const ItemConstPtr item, DBInsert query, PropWriteStream& binary_stream);
		static SkillRegistry deserializeCustomSkills(PropStream binary_stream);
		static bool savePlayerCustomSkills(const PlayerConstPtr& player, DBInsert& query_insert, PropWriteStream& binary_stream);
};

#endif
