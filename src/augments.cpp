// Credits: BlackTek Server Creator Codinablack@github.com.
// This project is based of otland's The Forgottenserver.
// Any and all code taken from otland's The Forgottenserver is licensed under GPL 2.0
// Any code Authored by: Codinablack or BlackTek contributers, that is not already licensed, is hereby licesned MIT. 
// The GPL 2.0 License that can be found in the LICENSE file.
// All code found in this file is licensed under MIT and can be found in the LICENSE file.

#include <toml++/toml.hpp>
#include <iostream>
#include <filesystem>
#include <fstream>

#include "augments.h"

static std::unordered_map<std::string_view, std::shared_ptr<Augment>> global_augments {};

std::shared_ptr<Augment> Augments::MakeAugment(std::string_view augmentName)
{
    auto it = global_augments.find(augmentName);

    if (it != global_augments.end()) {
        auto augmentClone = Augment::MakeAugment(it->second);
        return augmentClone;
    }
    std::cout << "Failed to find augment named : " << augmentName;
    return nullptr;
}

void Augments::loadAll() {
    for (const auto& entry : std::filesystem::directory_iterator(path)) {
        if (entry.is_regular_file() && entry.path().extension() == ".toml") {

            try {

                auto file = toml::parse_file(entry.path().string());

                for (const auto& [index, entry] : file) {

                    toml::table augment_info = *entry.as_table();
                    auto modifier_data = augment_info["modifiers"];
                    const std::string& name = augment_info["name"].value_or("unknown");

                    if (name == "unkown") {
                        std::cout << "Error: All augments require a name \n";
                        break;
                    }
                    std::shared_ptr<Augment> augment = Augment::MakeAugment(name);
                    
                    if (auto mod_list = modifier_data.as_array()) {
                        mod_list->for_each([augment](auto&& prop) {
                            if (prop.is_table()) {
                                auto& table                               = *prop.as_table();
                                std::string_view stance                   = table["stance"].value_or("none");
                                std::string_view modType                  = table["type"].value_or("none");
                                uint8_t chance                            = table["chance"].value_or(100);
                                uint16_t amount /* no safety checks */    = table["value"].value_or(0);
                                std::string_view damageType               = table["damageType"].value_or("none");
                                std::string_view originType               = table["originType"].value_or("none");
                                bool useOnAll /*m_use_on_any*/            = table["useOnAll"].value_or(false);
                                bool flatRate /*m_flat_value*/            = table["flatRate"].value_or(false);
                                bool useOnOrigin /*m_use_with_origin*/    = table["flatRate"].value_or(false);

                                // To-do: Change all static methods used below to accept const values and use const variables above.
                                if (GetStance(stance) == ATTACK_MOD) {
                                    std::shared_ptr<DamageModifier> damage_modifier = DamageModifier::makeModifier(
                                        (uint8_t)GetStance(stance),
                                        (uint8_t)GetAttackModifier(modType),
                                        amount,
                                        chance,
                                        GetDamage(damageType),
                                        GetOrigin(originType),
                                        flatRate, useOnAll, useOnOrigin);

                                    if (modType == "conversion") {
                                        damage_modifier->setTransformDamageType((CombatType_t)table["toDamageType"].value_or(0));
                                    }

                                    augment->addModifier(damage_modifier);

                                } else if (GetStance(stance) == DEFENSE_MOD) {
                                    std::shared_ptr<DamageModifier> damage_modifier = DamageModifier::makeModifier(
                                        (uint8_t)GetStance(stance),
                                        (uint8_t)GetDefenseModifier(modType),
                                        amount,
                                        chance,
                                        GetDamage(damageType),
                                        GetOrigin(originType),
                                        flatRate, useOnAll, useOnOrigin);

                                    if (modType == "reform") {
                                        damage_modifier->setTransformDamageType((CombatType_t)table["toDamageType"].value_or(0));
                                    }

                                    augment->addModifier(damage_modifier);

                                } else {

                                    std::cout << "Modifier has unknown stance " << table["stance"] << "\n";
                                }
                            }
                        });
                    }
                    std::cout << "Loaded Augment named : " << index << std::endl;
                }
            } catch (const toml::parse_error& err) {
                std::cerr << "Error parsing file " << entry.path() << ": " << err << "\n";
            }
        }
    }
}

void Augments::clearAll()
{
    global_augments.clear();
}

void Augments::reload()
{
    clearAll();
    loadAll();
    // if (config::deleteOldAugments) { CleanPlayerAugments(); }
}

const ModifierStance Augments::GetStance(std::string_view stanceName) noexcept
{
    if (stanceName == "attack") {
        return ATTACK_MOD;
    } else if (stanceName == "defense") {
        return DEFENSE_MOD;
    }
    return NO_MOD;
}

const CombatType_t Augments::GetDamage(std::string_view damageName) noexcept
{   // Note : If you add values to the list you must increase the size manually
    // current size is : 12
    const std::array<std::pair<std::string_view, CombatType_t>, 12> static_map{ {
        {"none",            COMBAT_NONE},
        {"physical",        COMBAT_PHYSICALDAMAGE},
        {"energy",          COMBAT_ENERGYDAMAGE},
        {"earth",           COMBAT_EARTHDAMAGE},
        {"fire",            COMBAT_FIREDAMAGE},
        {"lifedrain",       COMBAT_LIFEDRAIN},
        {"manadrain",       COMBAT_MANADRAIN},
        {"healing",         COMBAT_HEALING},
        {"drown",           COMBAT_DROWNDAMAGE},
        {"ice",             COMBAT_ICEDAMAGE},
        {"holy",            COMBAT_HOLYDAMAGE},
        {"death",           COMBAT_DEATHDAMAGE},
    } };

    for (const auto& [key, value] : static_map) {
        if (key == damageName) {
            return value;
        }
    }

    return COMBAT_NONE;
}

const CombatOrigin Augments::GetOrigin(std::string_view originName) noexcept
{   // Note : If you add values to the list you must increase the size manually
    // current size is : 11
    const std::array<std::pair<std::string_view, CombatOrigin>, 11> static_map{ {
        {"none",            ORIGIN_NONE},
        {"condition",       ORIGIN_CONDITION},
        {"spell",           ORIGIN_SPELL},
        {"melee",           ORIGIN_MELEE},
        {"ranged",          ORIGIN_RANGED},
        {"reflect",         ORIGIN_REFLECT},
        {"deflect",         ORIGIN_DEFLECT},
        {"ricochet",        ORIGIN_RICOCHET},
        {"modifier",        ORIGIN_DMGMOD},
        {"augment",         ORIGIN_AUGMENT},
        {"imbuement",       ORIGIN_IMBUEMENT},
    } };

    for (const auto& [key, value] : static_map) {
        if (key == originName) {
            return value;
        }
    }

    return ORIGIN_NONE;
}

const ModifierAttackType Augments::GetAttackModifier(std::string_view modName) noexcept
{   // Note : If you add values to the list you must increase the size manually
    // current size is : 8
    const std::array<std::pair<std::string_view, ModifierAttackType>, 8> static_map{ {
        {"none",            ATTACK_MODIFIER_NONE},
        {"lifesteal",       ATTACK_MODIFIER_LIFESTEAL},
        {"manasteal",       ATTACK_MODIFIER_MANASTEAL},
        {"staminasteal",    ATTACK_MODIFIER_STAMINASTEAL},
        {"soulsteal",       ATTACK_MODIFIER_SOULSTEAL},
        {"critical",        ATTACK_MODIFIER_CRITICAL},
        {"piercing",        ATTACK_MODIFIER_PIERCING},
        {"conversion",      ATTACK_MODIFIER_CONVERSION},
    } };

    for (const auto& [key, value] : static_map) {
        if (key == modName) {
            return value;
        }
    }

    return ATTACK_MODIFIER_NONE;
}

const ModifierDefenseType Augments::GetDefenseModifier(std::string_view modName) noexcept
{   // Note : If you add values to the list you must increase the size manually
    // current size is : 10
    const std::array<std::pair<std::string_view, ModifierDefenseType>, 10> static_map{ {
        {"none",            DEFENSE_MODIFIER_NONE},
        {"absorb",          DEFENSE_MODIFIER_ABSORB},
        {"restore",         DEFENSE_MODIFIER_RESTORE},
        {"replenish",       DEFENSE_MODIFIER_REPLENISH},
        {"revive",          DEFENSE_MODIFIER_REVIVE},
        {"reflect",         DEFENSE_MODIFIER_REFLECT},
        {"deflect",         DEFENSE_MODIFIER_DEFLECT},
        {"ricochet",        DEFENSE_MODIFIER_RICOCHET},
        {"resist",          DEFENSE_MODIFIER_RESIST},
        {"reform",          DEFENSE_MODIFIER_REFORM},
    } };

    for (const auto& [key, value] : static_map) {
        if (key == modName) {
            return value;
        }
    }

    return DEFENSE_MODIFIER_NONE;
}

void Augments::AddAugment(std::shared_ptr<Augment> augment) {
    auto [it, inserted] = global_augments.try_emplace(augment->getName(), augment);
    if (!inserted) {
        std::cout << "[Warning][Augments] " << augment->getName() << " already exists! \n";
    }
}

void Augments::RemoveAugment(std::shared_ptr<Augment> augment) {
    auto it = global_augments.find(augment->getName());
    if (it != global_augments.end()) {
        global_augments.erase(it);
    }
}

void Augments::RemoveAugment(std::string_view augName) {
    auto it = global_augments.find(std::string(augName));
    if (it != global_augments.end()) {
        global_augments.erase(it);
    }
}

void Augments::RemoveAugment(std::string augName) {
    auto it = global_augments.find(augName);
    if (it != global_augments.end()) {
        global_augments.erase(it);
    }
}
