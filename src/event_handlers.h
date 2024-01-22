#pragma once

#include "keys.h"
#include "settings.h"
#include "shouts.h"
#include "tes_util.h"

namespace esas {
namespace internal {

/// Among the given input events, find a button event that matches `user_event`. `user_event` should
/// be a member of `RE::UserEvents`.
inline const RE::ButtonEvent*
GetUserEventButtonInput(std::string_view user_event, const RE::InputEvent* events) {
    const auto* cm = RE::ControlMap::GetSingleton();
    if (!cm) {
        return nullptr;
    }

    for (; events; events = events->next) {
        const auto* button = events->AsButtonEvent();
        if (!button || !button->HasIDCode()) {
            continue;
        }
        if (cm->GetMappedKey(user_event, button->GetDevice()) != button->GetIDCode()) {
            continue;
        }
        return button;
    }

    return nullptr;
}

inline const RE::ButtonEvent*
GetShoutButtonInput(const RE::InputEvent* events) {
    const auto* user_events = RE::UserEvents::GetSingleton();
    if (!user_events) {
        return nullptr;
    }
    return GetUserEventButtonInput(user_events->shout, events);
}

}  // namespace internal

class FafHandler final : public RE::BSTEventSink<SKSE::ActionEvent> {
  public:
    [[nodiscard]] static bool
    Init(std::mutex& mutex, Shoutmap& map, const Settings& settings) {
        auto* action_ev_src = SKSE::GetActionEventSource();
        if (!action_ev_src) {
            return false;
        }

        static auto instance = FafHandler(mutex, map, settings);
        action_ev_src->AddEventSink(&instance);
        return true;
    }

    RE::BSEventNotifyControl
    ProcessEvent(const SKSE::ActionEvent* event, RE::BSTEventSource<SKSE::ActionEvent>*) override {
        Cast(event);
        return RE::BSEventNotifyControl::kContinue;
    }

  private:
    FafHandler(std::mutex& mutex, Shoutmap& map, const Settings& settings)
        : mutex_(mutex),
          map_(map),
          magicka_scale_(settings.magicka_scale_faf) {}

    FafHandler(const FafHandler&) = delete;
    FafHandler& operator=(const FafHandler&) = delete;
    FafHandler(FafHandler&&) = delete;
    FafHandler& operator=(FafHandler&&) = delete;

    void
    Cast(const SKSE::ActionEvent* event) {
        if (!event || event->type != SKSE::ActionEvent::Type::kVoiceFire) {
            return;
        }
        auto* player = event->actor;
        if (!player || !player->IsPlayerRef()) {
            return;
        }
        auto* high_data = tes_util::GetHighProcessData(*player);
        if (!high_data) {
            return;
        }

        auto* shout = event->sourceForm ? event->sourceForm->As<RE::TESShout>() : nullptr;
        if (!shout) {
            return;
        }
        RE::SpellItem* spell = nullptr;
        {
            auto lock = std::lock_guard(mutex_);
            spell = map_[*shout];
        }
        if (!spell) {
            return;
        }
        if (spell->GetCastingType() != RE::MagicSystem::CastingType::kFireAndForget) {
            SKSE::log::warn("cannot cast {} as a fire-and-forget spell", *spell);
            return;
        }

        auto casting_src = RE::MagicSystem::CastingSource::kInstant;
        // Bound weapon must be cast from hands.
        if (const auto* av_eff = spell->GetAVEffect();
            av_eff && av_eff->GetArchetype() == RE::EffectArchetypes::ArchetypeID::kBoundWeapon) {
            casting_src = high_data->currentShoutVariation == RE::TESShout::VariationID::kOne
                              ? RE::MagicSystem::CastingSource::kRightHand
                              : RE::MagicSystem::CastingSource::kLeftHand;
        }

        auto* magic_caster = player ? player->GetMagicCaster(casting_src) : nullptr;
        auto* av_owner = player ? player->AsActorValueOwner() : nullptr;
        if (!magic_caster || !av_owner) {
            return;
        }

        if (!tes_util::CheckCast(
                *magic_caster,
                *spell,
                std::array{
                    RE::MagicSystem::CannotCastReason::kMagicka,
                    RE::MagicSystem::CannotCastReason::kCastWhileShouting
                }
            )) {
            tes_util::ActorPlayMagicFailureSound(*player);
            return;
        }
        if (!RE::PlayerCharacter::IsGodMode()
            && !tes_util::HasEnoughMagicka(*player, *av_owner, *spell, magicka_scale_)) {
            tes_util::ActorPlayMagicFailureSound(*player);
            tes_util::FlashMagickaBar();
            return;
        }

        tes_util::ApplyMagickaCost(*player, *av_owner, *spell, magicka_scale_);
        tes_util::ActorPlaySound(
            *player, tes_util::GetSpellSound(spell, RE::MagicSystem::SoundID::kRelease)
        );
        tes_util::CastSpellImmediate(player, magic_caster, spell);
    }

    std::mutex& mutex_;
    Shoutmap& map_;
    const float magicka_scale_ = 1.f;
};

class ConcHandler final : public RE::BSTEventSink<SKSE::ActionEvent>,
                          public RE::BSTEventSink<RE::InputEvent*> {
  public:
    [[nodiscard]] static bool
    Init(std::mutex& mutex, Shoutmap& map, const Settings& settings) {
        auto* action_ev_src = SKSE::GetActionEventSource();
        auto* input_ev_src = RE::BSInputDeviceManager::GetSingleton();
        if (!action_ev_src || !input_ev_src) {
            return false;
        }

        static auto instance = ConcHandler(mutex, map, settings);
        action_ev_src->AddEventSink(&instance);
        input_ev_src->AddEventSink(&instance);
        return true;
    }

    RE::BSEventNotifyControl
    ProcessEvent(const SKSE::ActionEvent* event, RE::BSTEventSource<SKSE::ActionEvent>*) override {
        Cast(event);
        return RE::BSEventNotifyControl::kContinue;
    }

    RE::BSEventNotifyControl
    ProcessEvent(RE::InputEvent* const* events, RE::BSTEventSource<RE::InputEvent*>*) override {
        Poll(events);
        return RE::BSEventNotifyControl::kContinue;
    }

  private:
    ConcHandler(std::mutex& mutex, Shoutmap& map, const Settings& settings)
        : mutex_(mutex),
          map_(map),
          magicka_scale_(settings.magicka_scale_conc) {}

    ConcHandler(const ConcHandler&) = delete;
    ConcHandler& operator=(const ConcHandler&) = delete;
    ConcHandler(ConcHandler&&) = delete;
    ConcHandler& operator=(ConcHandler&&) = delete;

    void
    Cast(const SKSE::ActionEvent* event) {
        if (current_spell_) {
            return;
        }

        if (!event || event->type != SKSE::ActionEvent::Type::kVoiceFire) {
            return;
        }
        auto* player = event->actor;
        if (!player || !player->IsPlayerRef()) {
            return;
        }
        auto* shout = event->sourceForm ? event->sourceForm->As<RE::TESShout>() : nullptr;
        if (!shout) {
            return;
        }
        RE::SpellItem* spell = nullptr;
        {
            auto lock = std::lock_guard(mutex_);
            spell = map_[*shout];
        }
        if (!spell) {
            return;
        }
        if (spell->GetCastingType() != RE::MagicSystem::CastingType::kConcentration) {
            SKSE::log::warn("cannot cast {} as a concentration spell", *spell);
            return;
        }

        auto* magic_caster = player->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant);
        if (!magic_caster) {
            return;
        }
        Clear(magic_caster);
        if (!tes_util::CheckCast(*magic_caster, *spell)) {
            tes_util::ActorPlayMagicFailureSound(*player);
            return;
        }

        loop_soundhandle_ = tes_util::ActorPlaySound(
            *player, tes_util::GetSpellSound(spell, RE::MagicSystem::SoundID::kCastLoop)
        );
        tes_util::ActorPlaySound(
            *player, tes_util::GetSpellSound(spell, RE::MagicSystem::SoundID::kRelease)
        );
        magic_caster->currentSpellCost = spell->CalculateMagickaCost(player) * magicka_scale_;
        tes_util::CastSpellImmediate(player, magic_caster, spell);
        current_spell_ = spell;
    }

    void
    Poll(RE::InputEvent* const* events) {
        if (!current_spell_) {
            return;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* magic_caster = player
                                 ? player->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant)
                                 : nullptr;
        if (!magic_caster || magic_caster->state != RE::MagicCaster::State::kCasting) {
            Clear(magic_caster);
            return;
        }

        auto* ui = RE::UI::GetSingleton();
        if (!ui || ui->GameIsPaused()) {
            return;
        }
        const auto* control_map = RE::ControlMap::GetSingleton();
        if (!control_map || !control_map->IsFightingControlsEnabled()) {
            return;
        }
        const auto& cmstack = control_map->GetRuntimeData().contextPriorityStack;
        if (cmstack.empty() || cmstack.back() != RE::UserEvents::INPUT_CONTEXT_ID::kGameplay) {
            return;
        }

        if (!events) {
            Clear(magic_caster);
            return;
        }
        const auto* button = internal::GetShoutButtonInput(*events);
        if (!button || button->IsUp()) {
            Clear(magic_caster);
            return;
        }
    }

    void
    Clear(RE::MagicCaster* caster) {
        if (caster) {
            caster->FinishCast();
        }
        current_spell_ = nullptr;
        if (loop_soundhandle_) {
            loop_soundhandle_->Stop();
            loop_soundhandle_.reset();
        }
    }

    RE::SpellItem* current_spell_ = nullptr;
    std::optional<RE::BSSoundHandle> loop_soundhandle_;
    std::mutex& mutex_;
    Shoutmap& map_;
    const float magicka_scale_ = 1.f;
};

class AssignmentHandler final : public RE::BSTEventSink<RE::InputEvent*> {
  public:
    [[nodiscard]] static bool
    Init(std::mutex& mutex, Shoutmap& faf_map, Shoutmap& conc_map, const Settings& settings) {
        auto* input_ev_src = RE::BSInputDeviceManager::GetSingleton();
        if (!input_ev_src) {
            return false;
        }

        static auto instance = AssignmentHandler(mutex, faf_map, conc_map, settings);
        input_ev_src->AddEventSink(&instance);
        return true;
    }

    RE::BSEventNotifyControl
    ProcessEvent(RE::InputEvent* const* events, RE::BSTEventSource<RE::InputEvent*>*) override {
        HandleInput(events);
        return RE::BSEventNotifyControl::kContinue;
    }

  private:
    AssignmentHandler(
        std::mutex& mutex, Shoutmap& faf_map, Shoutmap& conc_map, const Settings& settings
    )
        : mutex_(mutex),
          faf_map_(faf_map),
          conc_map_(conc_map),
          allow_2h_(settings.allow_2h_spells),
          assign_keysets_(settings.convert_spell_keysets),
          unassign_keysets_(settings.remove_shout_keysets) {}

    AssignmentHandler(const AssignmentHandler&) = delete;
    AssignmentHandler& operator=(const AssignmentHandler&) = delete;
    AssignmentHandler(AssignmentHandler&&) = delete;
    AssignmentHandler& operator=(AssignmentHandler&&) = delete;

    void
    HandleInput(RE::InputEvent* const* events) {
        if (!events) {
            return;
        }
        buf_.clear();
        Keystroke::InputEventsToBuffer(*events, buf_);
        if (buf_.empty()) {
            return;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return;
        }
        if (assign_keysets_.Match(buf_) == Keypress::kPress) {
            Assign(*player);
        }
        if (unassign_keysets_.Match(buf_) == Keypress::kPress) {
            Unassign(*player);
        }
    }

    void
    Assign(RE::Actor& player) {
        auto* spell = tes_util::GetRightHandSpellItem(player);
        if (!spell) {
            return;
        }
        if (!tes_util::IsHandEquippedSpell(*spell, allow_2h_)) {
            SKSE::log::trace("{} is not eligible for spell shout assignment", *spell);
            return;
        }
        auto ct = spell->GetCastingType();
        auto lock = std::lock_guard(mutex_);
        auto* map = ct == RE::MagicSystem::CastingType::kFireAndForget   ? &faf_map_
                    : ct == RE::MagicSystem::CastingType::kConcentration ? &conc_map_
                                                                         : nullptr;
        if (!map) {
            return;
        }

        SKSE::log::trace("assigning {} ...", *spell);
        RE::TESShout* shout = nullptr;
        switch (auto status = map->Assign(player, *spell, shout)) {
            case Shoutmap::AssignStatus::kOk:
                tes_util::DebugNotification("{} added", shout->GetName());
                break;
            case Shoutmap::AssignStatus::kAlreadyAssigned:
                tes_util::DebugNotification("{} already assigned", spell->GetName());
                break;
            case Shoutmap::AssignStatus::kOutOfSlots:
                tes_util::DebugNotification(
                    "No remaining {} shout slots",
                    ct == RE::MagicSystem::CastingType::kFireAndForget ? "Fire and Forget"
                                                                       : "Concentration"
                );
                break;
            case Shoutmap::AssignStatus::kUnknownShout:
            case Shoutmap::AssignStatus::kInternalError:
                SKSE::log::error(
                    "unexpected error assigning {} to {}: status code {}",
                    *spell,
                    *shout,
                    std::to_underlying(status)
                );
                break;
        }
    }

    void
    Unassign(RE::Actor& player) {
        auto* shout = tes_util::GetEquippedShout(player);
        if (!shout) {
            return;
        }
        auto lock = std::lock_guard(mutex_);
        auto* map = faf_map_.Has(*shout) ? &faf_map_ : conc_map_.Has(*shout) ? &conc_map_ : nullptr;
        if (!map) {
            return;
        }

        SKSE::log::trace("unassigning {} ...", *shout);
        switch (auto status = map->Unassign(player, *shout)) {
            case Shoutmap::AssignStatus::kOk:
                tes_util::DebugNotification("{} removed", shout->GetName());
                break;
            default:
                SKSE::log::error(
                    "unexpected error unassigning {}: status code {}",
                    *shout,
                    std::to_underlying(status)
                );
                break;
        }
    }

    std::vector<Keystroke> buf_;
    std::mutex& mutex_;
    Shoutmap& faf_map_;
    Shoutmap& conc_map_;
    const bool allow_2h_;
    const Keysets assign_keysets_;
    const Keysets unassign_keysets_;
};

}  // namespace esas