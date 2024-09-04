// Credits: BlackTek Server Creator Codinablack@github.com.
// This project is based of otland's The Forgottenserver.
// Any and all code taken from otland's The Forgottenserver is licensed under GPL 2.0
// Any code Authored by: Codinablack or BlackTek contributers, that is not already licensed, is hereby licesned MIT. 
// The GPL 2.0 License that can be found in the LICENSE file.
// All code found in this file is licensed under MIT and can be found in the LICENSE file.


#ifndef FS_AUGMENT_H
#define FS_AUGMENT_H

#include "damagemodifier.h"

class Augment : public std::enable_shared_from_this<Augment> {

public:
	Augment() = default;
	Augment(std::string_view name);
	Augment(std::shared_ptr<Augment>& original);

	~Augment() = default;

	// allow copying
	explicit Augment(const Augment&) = default;
	Augment& operator=(const Augment&) = default;

	const std::string_view getName() const;
	static std::shared_ptr<Augment> MakeAugment(std::string_view augmentName);
	static std::shared_ptr<Augment> MakeAugment(std::shared_ptr<Augment>& originalPointer);

	void addModifier(std::shared_ptr<DamageModifier> modifier);
	void removeModifier(std::shared_ptr<DamageModifier>& modifier);

	std::vector<std::shared_ptr<DamageModifier>>& getAttackModifiers(uint8_t modType);
	std::vector<std::shared_ptr<DamageModifier>>& getDefenseModifiers(uint8_t modType);

private:

	std::shared_ptr<ModifierList> m_mod_list;
	std::string_view m_name;
};


#endif