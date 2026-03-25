#pragma once

// Fcitx5 headers
#include <fcitx/addonfactory.h>
#include <fcitx/addoninstance.h>
#include <fcitx/addonmanager.h>
#include <fcitx/event.h>
#include <fcitx/instance.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextproperty.h>
#include <fcitx/userinterfacemanager.h>
#include <fcitx-config/configuration.h>
#include <fcitx-config/iniparser.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/keysym.h>
#include <fcitx-utils/i18n.h>

#include "InputTracker.h"
#include "../core/AutoSwitchHeuristic.h"

#include <memory>
#include <unordered_map>

namespace punto {

// ---------------------------------------------------------------------------
// Persistent configuration (stored in ~/.config/fcitx5/conf/punto-switcher.conf)
// ---------------------------------------------------------------------------
FCITX_CONFIGURATION(
    PuntoConfig,
    // Alt+apostrophe — US layout physical key '
    // Alt+Cyrillic_e  — same physical key in Russian JCUKEN layout (generates э/Э)
    fcitx::Option<fcitx::KeyList> swapLastTextKey{
        this, "SwapLastText", _("Swap Last Word"),
        fcitx::KeyList{fcitx::Key("Alt+apostrophe"), fcitx::Key("Alt+Cyrillic_e")}};

    // Same dual-keysym logic for swap-selection hotkey
    fcitx::Option<fcitx::KeyList> swapSelectionKey{
        this, "SwapSelection", _("Swap Selection"),
        fcitx::KeyList{fcitx::Key("Alt+shift+apostrophe"), fcitx::Key("Alt+shift+Cyrillic_e")}};

    fcitx::Option<fcitx::KeyList> toggleAutoSwitchKey{
        this, "ToggleAutoSwitch", _("Toggle Auto-Switch"),
        fcitx::KeyList{fcitx::Key("Alt+shift+a")}};

    // Undo the last layout switch (layout + best-effort text revert).
    fcitx::Option<fcitx::KeyList> undoSwitchKey{
        this, "UndoLastSwitch", _("Undo Last Layout Switch"),
        // Alt+Shift+BackSpace is uncommon and should not conflict often.
        fcitx::KeyList{fcitx::Key("Alt+shift+BackSpace")}};

    fcitx::Option<bool> autoSwitchEnabled{
        this, "AutoSwitch", _("Enable Auto-Switch"), true};

    fcitx::Option<int> minWordLength{
        this, "MinWordLength", _("Minimum Word Length for Auto-Switch"), 3};

    // Threshold stored as integer percentage × 100 (e.g. 15 = 0.15).
    // Fcitx5 config system supports only: bool, int, string, Key, Color.
    fcitx::Option<int> confidenceThresholdPct{
        this, "ConfidenceThresholdPct", _("Auto-Switch Confidence Threshold (×100)"), 15};
);

// ---------------------------------------------------------------------------
// Per-InputContext property
// ---------------------------------------------------------------------------
class PuntoICProperty : public fcitx::InputContextProperty {
public:
    InputTracker tracker;
};

// ---------------------------------------------------------------------------
// Main addon class
// ---------------------------------------------------------------------------
class PuntoModule : public fcitx::AddonInstance {
public:
    explicit PuntoModule(fcitx::Instance* instance);
    ~PuntoModule() override = default;

    // Reload config hot-path (called by Fcitx5 on SIGHUP / GUI save)
    void reloadConfig() override;

    // Expose config for the GUI tool
    const fcitx::Configuration* getConfig() const override { return &config_; }
    void setConfig(const fcitx::RawConfig& cfg) override;

private:
    fcitx::Instance* instance_;
    PuntoConfig      config_;
    AutoSwitchHeuristic heuristic_;

    // Fcitx5 event handler registrations (RAII)
    std::vector<std::unique_ptr<fcitx::HandlerTableEntry<fcitx::EventHandler>>> handlers_;

    // Per-IC property slot
    fcitx::FactoryFor<PuntoICProperty> propertyFactory_;

    // --- event handlers ---
    void onKeyEvent(fcitx::KeyEvent& ev);
    void onFocusOut(fcitx::InputContextEvent& ev);

    // --- actions ---
    bool doSwapLastWord(fcitx::InputContext* ic);
    bool doSwapSelection(fcitx::InputContext* ic);
    void doToggleAutoSwitch();
    bool doUndoLastSwitch(fcitx::InputContext* ic);

    // --- helpers ---
    void applyConfig();
    PuntoICProperty* prop(fcitx::InputContext* ic) const;

    // Perform auto-switch check after a word boundary was committed.
    // `word` is the completed word, `ic` is the current input context.
    void checkAutoSwitch(const std::string& word, fcitx::InputContext* ic);
};

class PuntoModuleFactory : public fcitx::AddonFactory {
public:
    fcitx::AddonInstance* create(fcitx::AddonManager* manager) override {
        return new PuntoModule(manager->instance());
    }
};

} // namespace punto
