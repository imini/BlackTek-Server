// Credits: BlackTek Server Creator Codinablack@github.com.
// This project is based of otland's The Forgottenserver.
// Any and all code taken from otland's The Forgottenserver is licensed under GPL 2.0
// Any code Authored by: Codinablack or BlackTek contributers, that is not already licensed, is hereby licesned MIT. 
// The GPL 2.0 License that can be found in the LICENSE file.
// All code found in this file is licensed under MIT and can be found in the LICENSE file.


#ifndef FS_AUGMENTS_H
#define FS_AUGMENTS_H

#include "augment.h"

class Augments {

public:
	// No Constructors! Purely static class.
	Augments() = delete;
	~Augments() = delete;
	Augments(const Augments&) = delete;
	Augments& operator=(const Augments&) = delete;
	Augments(Augments&&) = delete;
	Augments& operator=(Augments&&) = delete;

	static constexpr auto path = "data/augments/";
	static const ModifierStance GetStance(std::string_view stanceName) noexcept;
	static const ModifierAttackType GetAttackModifier(std::string_view modName) noexcept;
	static const ModifierDefenseType GetDefenseModifier(std::string_view modName) noexcept;
	static const CombatType_t GetDamage(std::string_view damageName) noexcept;
	static const CombatOrigin GetOrigin(std::string_view originName) noexcept;
	static std::shared_ptr<Augment> MakeAugment(std::string_view augmentName);

	// The following is vital as its used during every combat.
	// Please do not alter the below values without adjusting
	// the arrays below them and vice-a-versa.
	static constexpr int AttackModCount = 10;
	static constexpr int DefenseModCount = 12;

	static constexpr std::array<uint8_t, Augments::AttackModCount> AttackCombatList {
		ATTACK_MODIFIER_LIFESTEAL,
		ATTACK_MODIFIER_MANASTEAL,
		ATTACK_MODIFIER_STAMINASTEAL,
		ATTACK_MODIFIER_SOULSTEAL,
		ATTACK_MODIFIER_CRITICAL,
		ATTACK_MODIFIER_PIERCING,
		ATTACK_MODIFIER_BUTCHER,
		ATTACK_MODIFIER_HUNTER,
		ATTACK_MODIFIER_SLAYER,
		ATTACK_MODIFIER_CULL,
	};

	static constexpr std::array<uint8_t, Augments::DefenseModCount> DefenseCombatList {
		DEFENSE_MODIFIER_ABSORB,
		DEFENSE_MODIFIER_RESTORE,
		DEFENSE_MODIFIER_REPLENISH,
		DEFENSE_MODIFIER_REVIVE,
		DEFENSE_MODIFIER_REFLECT,
		DEFENSE_MODIFIER_DEFLECT,
		DEFENSE_MODIFIER_RICOCHET,
		DEFENSE_MODIFIER_RESIST,
		DEFENSE_MODIFIER_BEASTARMOR,
		DEFENSE_MODIFIER_AEGIS,
		DEFENSE_MODIFIER_IMMORTAL,
		DEFENSE_MODIFIER_SLAYER,
	};

	static void loadAll();
	static void clearAll();
	static void reload();
	static void AddAugment(std::shared_ptr<Augment> augment);
	static void RemoveAugment(std::shared_ptr<Augment> augment);
	static void RemoveAugment(std::string_view augName);
	static void RemoveAugment(std::string augName);
};


#endif