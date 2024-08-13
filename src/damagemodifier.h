// Credits: BlackTek Server Creator Codinablack@github.com.
// This project is based of otland's The Forgottenserver.
// Any and all code taken from otland's The Forgottenserver is licensed under GPL 2.0
// Any code Authored by: Codinablack or BlackTek contributers, that is not already licensed, is hereby licesned MIT. 
// The GPL 2.0 License that can be found in the LICENSE file.
// All code found in this file is licensed under MIT and can be found in the LICENSE file.

#ifndef FS_DAMAGEMODIFIER_H
#define FS_DAMAGEMODIFIER_H

#include "otpch.h"
#include "tools.h"
#include "const.h"
#include "fileloader.h"

struct ModifierTotals {
	ModifierTotals(uint8_t flat, uint8_t percent) : flatTotal(flat), percentTotal(percent) {}
	uint8_t flatTotal = 0;
	uint8_t percentTotal = 0;
};

enum ModifierAttackType : uint8_t {
	ATTACK_MODIFIER_NONE,				// default
	ATTACK_MODIFIER_LIFESTEAL,			// damage is converted to health
	ATTACK_MODIFIER_MANASTEAL,			// damage is converted to mana
	ATTACK_MODIFIER_STAMINASTEAL,		// damage is converted stamina
	ATTACK_MODIFIER_SOULSTEAL,			// damage is converted soul
	ATTACK_MODIFIER_CRITICAL,			// damage can critcally hit
	ATTACK_MODIFIER_PIERCING,			// damage ignores defenses
	ATTACK_MODIFIER_CONVERSION,			// damage is converted to different type
	ATTACK_MODIFIER_BUTCHER,			// damage is increased against specific monster.
	ATTACK_MODIFIER_HUNTER,				// damage is increase against a specific race.
	ATTACK_MODIFIER_SLAYER,				// damage is increased against a specific boss.
	ATTACK_MODIFIER_CULL,				// damage is increased against bosses


	ATTACK_MODIFIER_LAST
};

enum ModifierDefenseType : uint8_t {
	DEFENSE_MODIFIER_NONE,				// default
	DEFENSE_MODIFIER_ABSORB,			// damage is converted to health
	DEFENSE_MODIFIER_RESTORE,			// damage is converted to mana 
	DEFENSE_MODIFIER_REPLENISH,			// damage is converted to stamina
	DEFENSE_MODIFIER_REVIVE,			// damage is converted to soul
	DEFENSE_MODIFIER_REFLECT,			// damage is reduced on defender and returns to attacker
	DEFENSE_MODIFIER_DEFLECT,			// damage is negated on defender but hits all nearby enemies
	DEFENSE_MODIFIER_RICOCHET,			// damage is negated on defender but hits one random enemy
	DEFENSE_MODIFIER_RESIST,			// damage reduction
	DEFENSE_MODIFIER_REFORM,			// convert damage to another type 
	DEFENSE_MODIFIER_BEASTARMOR,		// reduces damage done by a specific monster
	DEFENSE_MODIFIER_AEGIS,				// reduces damage done by a specific race
	DEFENSE_MODIFIER_IMMORTAL,			// reduces damage done by bosses
	DEFENSE_MODIFIER_SLAYER,			// reduces damage dones by a specific boss


	DEFENSE_MODIFIER_LAST
};

enum ModifierStance : uint8_t {
	NO_MOD,
	ATTACK_MOD,
	DEFENSE_MOD
};

class DamageModifier : public std::enable_shared_from_this<DamageModifier> {

public:
	DamageModifier() = default;
	~DamageModifier() = default;

	// allow copying
	DamageModifier(const DamageModifier&) = default;
	DamageModifier& operator=(const DamageModifier&) = default;
	auto operator<=>(const DamageModifier&) const = default;

	void initializeSharedPointer() { m_shared_this = shared_from_this(); }

	DamageModifier(uint8_t stance, uint8_t modType, uint16_t amount, uint8_t chance, CombatType_t combatType, CombatOrigin source = ORIGIN_NONE, bool flatRate = false, bool onAnyDmg = false, bool origin = false) :
		m_mod_stance(stance),					// attack / defense
		m_mod_type(modType),					// the enum specific type
		m_value(amount),						// value to modify; default = percent
		m_chance(chance),						// chance; if chance is 0, chance is not used.
		m_damage_type(combatType),				// if none, defaults to all damage types
		m_origin_type(source),					// if none, is used on all origin types
		m_flat_value(flatRate),					// if true, the value will be a flat value instead of percent
		m_use_on_any(onAnyDmg),					// if true, the modifier will apply for all damage types
		m_use_with_origin(origin)				// if true, modifier only applies to damage from the specified origin type saved to this object
	{}

	std::shared_ptr<DamageModifier>& getModifier();
	static std::shared_ptr<DamageModifier> makeModifier(uint8_t stance, uint8_t modType, uint16_t amount, uint8_t chance, CombatType_t combatType, CombatOrigin source, bool flatRate, bool onAnyDmg, bool origin);

	const uint8_t getStance() const;
	const uint8_t getType() const;
	const uint8_t getValue() const;
	const uint16_t getChance() const;

	const CombatType_t getDamageType() const;
	const CombatOrigin getOriginType() const;

	const bool isPercent() const;
	const bool isFlatValue() const;
	const bool appliesToAllDamage() const;
	const bool isOriginBased() const;
	const bool isMonsterBased() const;
	const bool isRaceBased() const;
	const bool isBossBased() const;
	const bool isAttackStance() const;
	const bool isDefenseStance() const;
	const std::string getMonsterName() const;
	const uint8_t getRaceType() const;
	const CombatType_t conversionType() const;

	void serialize(PropWriteStream& propWriteStream) const;
	bool unserialize(PropStream& propReadStream);

	void increaseValue(uint16_t amount);
	void decreaseValue(uint16_t amount);
	void setTransformDamageType(CombatType_t damageType);

private:
	std::shared_ptr<DamageModifier> m_shared_this;
	uint8_t m_mod_stance = 0;						// 0 = none, 1 = attack, 2 = defense;
	uint8_t m_mod_type = 0;
	uint16_t m_value = 0;
	uint8_t m_chance = 0;
	CombatType_t m_damage_type = COMBAT_NONE;
	CombatOrigin m_origin_type = ORIGIN_NONE;
	bool m_flat_value = false;						// is flat value? if false its percent value
	bool m_use_on_any = false;						// this modifier applies to all damage types? if false then only specified type.
	bool m_use_with_origin = false;					// does this modifier target a specific origin type? if false it applies on all origin types
	std::any m_aux_attribute;						// can be used for determining a type of damage, when transforming from one type to another.
};

class ModifierList : public std::enable_shared_from_this<ModifierList> {

public:

	ModifierList() { }
	auto operator<=>(const ModifierList&) const = default;


	void initializeSharedPointer() { m_shared_this = shared_from_this(); }

	//void serialize(PropWriteStream& propWriteStream) const {
	//	if (!m_modifiers.empty()) {
	//		propWriteStream.write<uint16_t>(m_modifiers.size());
	//		for (const auto& modifier : m_modifiers) {
	//			modifier->serialize(propWriteStream);
	//		}
	//	}
	//}

	//bool unserialize(PropStream& propReadStream) {
	//	uint16_t size = 0;
	//	if (!propReadStream.read<uint16_t>(size)) {
	//		return false;
	//	}

	//	m_modifiers.clear();
	//	for (uint16_t i = 0; i < size; ++i) {
	//		auto modifier = std::make_shared<DamageModifier>();
	//		if (!modifier->unserialize(propReadStream)) {
	//			return false;
	//		}
	//		m_modifiers.push_back(modifier);
	//	}
	//	return true;
	//}

	void addModifier(std::shared_ptr<DamageModifier>& mod);
	void removeModifier(std::shared_ptr<DamageModifier>& mod);

	std::vector<std::shared_ptr<DamageModifier>>& getAttackModifiers(uint8_t modType);
	std::vector<std::shared_ptr<DamageModifier>>& getDefenseModifiers(uint8_t modType);

private:
	std::shared_ptr<ModifierList> m_shared_this;
	std::vector<std::shared_ptr<DamageModifier>> m_attack_modifiers;
	std::vector<std::shared_ptr<DamageModifier>> m_defense_modifiers;
};


/// Inline Methods' Definitions

inline const bool DamageModifier::isPercent() const {
	return !m_flat_value;
}

inline const bool DamageModifier::isFlatValue() const {
	return m_flat_value;
}

inline const bool DamageModifier::appliesToAllDamage() const {
	return m_use_on_any;
}

inline const bool DamageModifier::isOriginBased() const {
	return m_use_with_origin;
}

inline const uint8_t DamageModifier::getStance() const {
	return m_mod_stance;
}

inline const uint8_t DamageModifier::getType() const {
	return m_mod_type;
}

inline const uint8_t DamageModifier::getValue() const {
	return m_value;
}

inline const uint16_t DamageModifier::getChance() const {
	return m_chance;
}

inline const CombatType_t DamageModifier::getDamageType() const {
	return m_damage_type;
}

inline const CombatOrigin DamageModifier::getOriginType() const {
	return m_origin_type;
}

inline const bool DamageModifier::isAttackStance() const
{
	return m_mod_stance == ATTACK_MOD;
}

inline const bool DamageModifier::isDefenseStance() const
{
	return m_mod_stance == DEFENSE_MOD;
}

#endif