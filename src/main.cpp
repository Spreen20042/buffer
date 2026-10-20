#include <Geode/Geode.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <vector>
#include <algorithm>
#include <cctype>
#include <string>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/HardStreak.hpp>

using namespace geode::prelude;

// ============================================================================
//  CONSTANTS
// ============================================================================

namespace {
    // GD runs 4 physics ticks per frame at 60 fps.
    constexpr float TICKS_PER_FRAME = 4.f;

    // Lookahead (in ticks) comes from the mod settings; Mini Wave uses 50/65 of the Wave horizon.
    int horizonNormal() { return static_cast<int>(Mod::get()->getSettingValue<int64_t>("normal-horizon")); }
    int horizonWave()   { return static_cast<int>(Mod::get()->getSettingValue<int64_t>("wave-horizon")); }
    int horizonMiniWave() { return horizonWave() * 50 / 65; }

    // Early exit for the simulation: if the player stays airborne this long and
    // then lands safely for SAFE_LANDED_TICKS, the rest of the horizon is assumed safe.
    constexpr int SAFE_AIRBORNE_TICKS = 15;
    constexpr int SAFE_LANDED_TICKS   = 10;

    struct Preset {
        bool enabled;
        int  normalTicks;   // -1 = read from the "custom-normal-ticks" setting
        int  waveTicks;     // -1 = read from the "custom-wave-ticks" setting
    };

    // The hotkey cycles through these in order.
    constexpr Preset PRESETS[] = {
        { true,  2,  2 },
        { true,  4,  4 },
        { true,  8,  8 },
        { true, 16,  8 },
        { true, -1, -1 },   // CUSTOM: values from the mod settings (stronger assist)
        { false, 0,  0 },   // OFF: 100% raw input
    };
    constexpr int PRESET_COUNT = static_cast<int>(sizeof(PRESETS) / sizeof(PRESETS[0]));

    // Spam guard: only bypass the buffer after at least `spam-streak` consecutive
    // inputs closer than `spam-interval` ticks. Both are editable in the mod settings.
    int spamInterval() { return static_cast<int>(Mod::get()->getSettingValue<int64_t>("spam-interval")); }
    int spamStreak()   { return static_cast<int>(Mod::get()->getSettingValue<int64_t>("spam-streak")); }

    // Ship assist: max delay in ticks (0 = ship is never touched, like the original mod).
    int shipTicks()    { return static_cast<int>(Mod::get()->getSettingValue<int64_t>("ship-ticks")); }

    // Rescue mode: lookahead in ticks (0 = off). When the simulation predicts a death within
    // this many ticks, the mod injects its own press/release if that avoids it.
    int rescueTicks()  { return static_cast<int>(Mod::get()->getSettingValue<int64_t>("rescue-ticks")); }
    // Minimum extra survival ticks an alternative must give before rescue acts (lower = more aggressive).
    int rescueMargin() { return static_cast<int>(Mod::get()->getSettingValue<int64_t>("rescue-margin")); }
    // Minimum ticks between two rescue actions (stops it from spamming clicks).
    int rescueCooldown() { return static_cast<int>(Mod::get()->getSettingValue<int64_t>("rescue-cooldown")); }
}

// Defined below, next to the settings panel.
void openBufferPanel();

// ============================================================================

class TrajectoryBufferSim {
private:
    PlayerObject* m_simPlayer1 = nullptr;
    PlayerObject* m_simPlayer2 = nullptr;

    bool m_simulating = false;
    bool m_firingBuffered = false;
    bool m_simulationDead = false;

    // Actual physical button state vs. what the game currently has active
    bool m_physicalHolding = false;
    bool m_gameHolding = false;

    // Preset state
    int  m_presetIndex = 0;
    bool m_enabled = true;
    int  m_maxTicksNormal = PRESETS[0].normalTicks;
    int  m_maxTicksWave = PRESETS[0].waveTicks;
    bool m_hotkeyWasDown = false;
    bool m_panelKeyWasDown = false;
    int  m_rescueCooldownLeft = 0;
    bool m_rescueOverride = false;   // rescue changed the hold state; the next real input hands control back

    // Buffer state
    int  m_waitTicks = -1;          // -1 = inactive, >= 0 = countdown, -2 = queued opposite action
    bool m_bufferedPush = true;     // true = buffered PRESS, false = buffered RELEASE
    bool m_queuedOpposite = false;  // user released/re-pressed while an input was waiting
    float m_frameDt = 0.25f;        // normalized dt of one physics step

    // Spam guard state
    int  m_ticksSinceLastInput = 999;
    int  m_rapidInputStreak = 0;

    PlayerObject* createTrajectoryPlayer() {
        PlayLayer* pl = PlayLayer::get();
        if (!pl) return nullptr;

        PlayerObject* player = PlayerObject::create(1, 1, pl, pl, true);
        if (!player) return nullptr;
        player->retain();
        player->setPosition({0, 105});
        player->setVisible(false);
        pl->m_objectLayer->addChild(player);
        return player;
    }

    static void destroyTrajectoryPlayer(PlayerObject*& player) {
        if (!player) return;
        // The object layer holds its own reference; drop it before our retain.
        if (player->getParent()) player->removeFromParent();
        player->release();
        player = nullptr;
    }

    static void resetCollisionLog(PlayerObject* self) {
        self->m_collisionLogTop->removeAllObjects();
        self->m_collisionLogBottom->removeAllObjects();
        self->m_collisionLogLeft->removeAllObjects();
        self->m_collisionLogRight->removeAllObjects();
    }

    // Matches Show Trajectory's attribute + hitbox synchronization
    static void syncSimPlayer(PlayerObject* sim, PlayerObject* real) {
        sim->setVisible(false);
        sim->copyAttributes(real);
        sim->setScale(real->getScale());
        sim->setContentSize(real->getContentSize());
        sim->m_width = real->m_width;
        sim->m_height = real->m_height;
        sim->m_gravityMod = real->m_gravityMod;
        sim->m_isOnGround = real->m_isOnGround;
        sim->m_isUpsideDown = real->m_isUpsideDown;
        sim->m_vehicleSize = real->m_vehicleSize;
        sim->m_playerSpeed = real->m_playerSpeed;
    }

    void setSimButtons(bool isDual, bool push) {
        auto apply = [push](PlayerObject* p) {
            if (!p) return;
            if (push) p->pushButton(PlayerButton::Jump);
            else      p->releaseButton(PlayerButton::Jump);
        };
        apply(m_simPlayer1);
        if (isDual) apply(m_simPlayer2);
    }

    float getValidDt() const {
        return (m_frameDt > 0.001f && m_frameDt < 2.0f) ? m_frameDt : 0.25f;
    }

    void applyPreset(int index, bool announce) {
        m_presetIndex = index % PRESET_COUNT;
        const Preset& p = PRESETS[m_presetIndex];
        m_enabled = p.enabled;
        m_maxTicksNormal = p.normalTicks >= 0 ? p.normalTicks
            : static_cast<int>(Mod::get()->getSettingValue<int64_t>("custom-normal-ticks"));
        m_maxTicksWave = p.waveTicks >= 0 ? p.waveTicks
            : static_cast<int>(Mod::get()->getSettingValue<int64_t>("custom-wave-ticks"));
        m_waitTicks = -1;
        m_queuedOpposite = false;

        if (!announce) return;
        std::string msg;
        if (p.enabled) {
            msg = fmt::format("Buffer stage {}: {} all | {} wave ticks ({:.1f} / {:.1f} frames)",
                m_presetIndex + 1, m_maxTicksNormal, m_maxTicksWave,
                m_maxTicksNormal / TICKS_PER_FRAME, m_maxTicksWave / TICKS_PER_FRAME);
        } else {
            msg = fmt::format("Buffer stage {}: OFF (raw input)", m_presetIndex + 1);
        }
        log::info("{}", msg);
        Notification::create(msg, NotificationIcon::Info, 1.2f)->show();
    }

#ifdef GEODE_IS_WINDOWS
    // Maps the "hotkey" setting to a Windows virtual-key code.
    static int hotkeyToVK(std::string const& key) {
        if (key.size() == 1) return std::toupper(static_cast<unsigned char>(key[0]));
        if (key == "Insert")    return VK_INSERT;
        if (key == "Delete")    return VK_DELETE;
        if (key == "Home")      return VK_HOME;
        if (key == "End")       return VK_END;
        if (key == "Page Up")   return VK_PRIOR;
        if (key == "Page Down") return VK_NEXT;
        return 'B';
    }
#endif

    // Edge-triggered key check, only while the game window has focus.
    static bool keyPressedEdge(const char* settingId, bool& wasDown) {
#ifdef GEODE_IS_WINDOWS
        HWND fg = GetForegroundWindow();
        bool focused = fg != nullptr && fg == GetActiveWindow();
        bool down = focused && (GetAsyncKeyState(hotkeyToVK(Mod::get()->getSettingValue<std::string>(settingId))) & 0x8000) != 0;
        bool edge = down && !wasDown;
        wasDown = down;
        return edge;
#else
        return false;
#endif
    }

    void pollHotkeys() {
        if (keyPressedEdge("hotkey", m_hotkeyWasDown)) {
            applyPreset(m_presetIndex + 1, true);
        }
        if (keyPressedEdge("panel-hotkey", m_panelKeyWasDown)) {
            // Open next frame: we are in the middle of a physics step right now.
            queueInMainThread([] { openBufferPanel(); });
        }
    }

public:
    // Used by the settings panel
    int stage() const { return m_presetIndex; }                       // 0-based
    void setStage(int index) { applyPreset(index, false); }           // 0-based, clamped by caller
    void refreshPreset() { applyPreset(m_presetIndex, false); }       // re-read CUSTOM values

    void setStartStage(int stage) {
        if (stage < 1) stage = 1;
        if (stage > PRESET_COUNT) stage = PRESET_COUNT;
        applyPreset(stage - 1, false);
    }

    // Clears every pending/buffered input. Does NOT touch the physical button state.
    void resetState() {
        m_waitTicks = -1;
        m_queuedOpposite = false;
        m_gameHolding = false;
        m_ticksSinceLastInput = 999;
        m_rapidInputStreak = 0;
        m_rescueOverride = false;
        m_rescueCooldownLeft = 0;
    }

    void init() {
        if (!PlayLayer::get()) return;

        // Defensive: never leak the previous level's simulation players.
        destroyTrajectoryPlayer(m_simPlayer1);
        destroyTrajectoryPlayer(m_simPlayer2);

        m_simPlayer1 = createTrajectoryPlayer();
        m_simPlayer2 = createTrajectoryPlayer();
        m_physicalHolding = false;
        m_simulating = false;
        m_firingBuffered = false;
        resetState();
    }

    void quit() {
        destroyTrajectoryPlayer(m_simPlayer1);
        destroyTrajectoryPlayer(m_simPlayer2);
        m_simulating = false;
        m_firingBuffered = false;
        resetState();
    }

    bool isSimulating() const { return m_simulating; }
    bool isFiringBuffered() const { return m_firingBuffered; }

    // Returns true if the death belongs to a simulation player (and must be swallowed).
    bool checkSimulationDeath(PlayerObject* player) {
        if (m_simulating && (player == m_simPlayer1 || player == m_simPlayer2)) {
            m_simulationDead = true;
            return true;
        }
        // Real player died: clear any pending buffer so the respawn is clean.
        resetState();
        return false;
    }

    void setFrameDelta(float dt) {
        PlayLayer* pl = PlayLayer::get();
        if (pl && pl->m_gameState.m_timeWarp > 0.f) {
            m_frameDt = dt / pl->m_gameState.m_timeWarp;
        } else {
            m_frameDt = dt;
        }
    }

    int getSurvivalTicks(PlayLayer* pl, PlayerObject* realP1, PlayerObject* realP2, bool isDual, int delayFrames, bool actionPush, int maxTicks, int flipBackAt = -1) {
        if (!pl || !m_simPlayer1 || !realP1) return maxTicks;

        const bool useP2 = isDual && m_simPlayer2 && realP2;

        m_simulating = true;
        m_simulationDead = false;

        syncSimPlayer(m_simPlayer1, realP1);
        if (useP2) syncSimPlayer(m_simPlayer2, realP2);

        // Before delayFrames, hold the pre-input button state
        if (delayFrames > 0) {
            setSimButtons(useP2, !actionPush);
        }

        const float dt = getValidDt();
        int survived = 0;
        const bool isContinuousMode = realP1->m_isDart || realP1->m_isBird || realP1->m_isSwing || realP1->m_isShip ||
                                      (isDual && realP2 && (realP2->m_isDart || realP2->m_isBird || realP2->m_isSwing || realP2->m_isShip));
        int airborneTicks = 0;
        int safeLandedTicks = 0;

        for (int i = 0; i < delayFrames + maxTicks; i++) {
            if (i == delayFrames) {
                setSimButtons(useP2, actionPush);
            }
            if (i == flipBackAt) {
                setSimButtons(useP2, !actionPush);
            }

            m_simPlayer1->update(dt);
            m_simPlayer1->updateRotation(dt);
            if (useP2) {
                m_simPlayer2->update(dt);
                m_simPlayer2->updateRotation(dt);
            }
            if (m_simulationDead) break;

            resetCollisionLog(m_simPlayer1);
            pl->checkCollisions(m_simPlayer1, dt, false);
            if (m_simulationDead) break;

            if (useP2) {
                resetCollisionLog(m_simPlayer2);
                pl->checkCollisions(m_simPlayer2, dt, false);
                if (m_simulationDead) break;
            }

            if (i >= delayFrames) {
                survived++;
                if (!isContinuousMode) {
                    bool p1Grounded = m_simPlayer1->m_isOnGround;
                    bool p2Grounded = !useP2 || m_simPlayer2->m_isOnGround;
                    if (!p1Grounded || !p2Grounded) {
                        airborneTicks++;
                        safeLandedTicks = 0;
                    } else if (airborneTicks >= SAFE_AIRBORNE_TICKS) {
                        safeLandedTicks++;
                        if (safeLandedTicks >= SAFE_LANDED_TICKS) {
                            survived = maxTicks;
                            break;
                        }
                    }
                }
            }
        }

        m_simulating = false;
        return survived;
    }

    // Returns true if the input should pass through to the game right now,
    // false if it was swallowed (buffered) and will be fired later.
    bool interceptInput(GJBaseGameLayer* layer, bool push, bool isPlayer1) {
        PlayLayer* pl = PlayLayer::get();
        if (!pl) return true;

        const bool isDual = pl->m_gameState.m_isDualMode && pl->m_player2 != nullptr;
        const bool isTwoPlayerControl = pl->m_level && pl->m_level->m_twoPlayerMode;

        // Keep Player 2 in sync with Player 1 in 1-player Dual mode
        if (!isPlayer1) {
            if (isDual && !isTwoPlayerControl && (m_waitTicks >= 0 || m_waitTicks == -2)) {
                return false;
            }
            return true;
        }

        // Always record the true physical state first
        m_physicalHolding = push;
        m_rescueOverride = false;   // a real input always takes control back from rescue mode

        if (!m_enabled) {
            m_gameHolding = push;
            return true;
        }

        PlayerObject* p1 = layer->m_player1;
        PlayerObject* p2 = isDual ? pl->m_player2 : nullptr;
        if (!p1 || p1->m_isDead) {
            m_waitTicks = -1;
            m_queuedOpposite = false;
            m_gameHolding = push;
            return true;
        }

        // 1. Ship: only assisted when "ship-ticks" > 0 (otherwise untouched, as in the original mod)
        const bool isShip = p1->m_isShip || (isDual && p2 && p2->m_isShip);
        if (isShip && shipTicks() <= 0) {
            m_waitTicks = -1;
            m_queuedOpposite = false;
            m_gameHolding = push;
            return true;
        }

        // 2. An input is already waiting: remember an opposite action so fast taps are never eaten
        if (m_waitTicks >= 0) {
            m_queuedOpposite = (push != m_bufferedPush);
            m_ticksSinceLastInput = 0;
            return false;
        }

        // A queued opposite action is due next tick and the user clicked again: fire it first
        if (m_waitTicks == -2) {
            m_waitTicks = -1;
            m_queuedOpposite = false;
            if (m_gameHolding != !m_bufferedPush) {
                fireButton(pl, !m_bufferedPush);
            }
        }

        const bool isWave = p1->m_isDart || (isDual && p2 && p2->m_isDart);
        const bool isMiniWave = (p1->m_isDart && p1->m_vehicleSize < 1.0f) ||
                                (isDual && p2 && p2->m_isDart && p2->m_vehicleSize < 1.0f);

        // 3. Spam guard (Wave and Ship): after N rapid inputs in a row, bypass the buffer entirely
        if (m_ticksSinceLastInput < spamInterval()) {
            m_rapidInputStreak++;
        } else {
            m_rapidInputStreak = 0;
        }
        m_ticksSinceLastInput = 0;

        if ((isWave || isShip) && m_rapidInputStreak >= spamStreak()) {
            m_gameHolding = push;
            return true;
        }

        // Wave, Ship and Robot change trajectory on RELEASE
        const bool releaseAffectsPhysics = p1->m_isDart || p1->m_isRobot || p1->m_isShip ||
                                           (isDual && p2 && (p2->m_isDart || p2->m_isRobot || p2->m_isShip));
        if (!push && !releaseAffectsPhysics) {
            m_gameHolding = false;
            return true;
        }

        m_queuedOpposite = false;

        const int targetHorizon = isMiniWave ? horizonMiniWave()
            : ((isWave || isShip) ? horizonWave() : horizonNormal());
        const int maxDelayFrames = isWave ? m_maxTicksWave : (isShip ? shipTicks() : m_maxTicksNormal);

        // 4. Immediate input survives the whole horizon: nothing to do
        const int ticksFrame0 = getSurvivalTicks(pl, p1, p2, isDual, 0, push, targetHorizon);
        if (ticksFrame0 >= targetHorizon) {
            m_gameHolding = push;
            return true;
        }

        // 5. Immediate input hits something: look for the earliest delay that clears it
        int bestDelay = 0;
        int bestTicks = ticksFrame0;

        for (int delay = 1; delay <= maxDelayFrames; delay++) {
            const int ticks = getSurvivalTicks(pl, p1, p2, isDual, delay, push, targetHorizon);

            if (isWave) {
                // Delaying a turn inside a flat corridor naturally adds ~(2 * delay) ticks
                // against the opposite wall, so require a real gain beyond that.
                const int minClearedGain = (delay * 2) + 10;
                if (ticks >= targetHorizon && (ticks - ticksFrame0) >= (delay * 2) + 6) {
                    bestDelay = delay;
                    bestTicks = ticks;
                    break;
                }
                if (ticks >= ticksFrame0 + minClearedGain && ticks > bestTicks + 2) {
                    bestDelay = delay;
                    bestTicks = ticks;
                    if (ticks >= targetHorizon) break;
                }
            } else {
                if (ticks >= targetHorizon) {
                    bestDelay = delay;
                    bestTicks = ticks;
                    break;
                }
                if (ticks > bestTicks + 12) {
                    bestDelay = delay;
                    bestTicks = ticks;
                }
            }
        }

        if (bestDelay > 0) {
            log::debug("Buffered {} by +{} ticks ({:.2f} frames @ 60fps) (frame0: {} -> delayed: {}, limit: {})",
                push ? "PRESS" : "RELEASE", bestDelay, bestDelay / TICKS_PER_FRAME,
                ticksFrame0, bestTicks, maxDelayFrames);
            m_bufferedPush = push;
            m_waitTicks = bestDelay;
            return false;
        }

        m_gameHolding = push;
        return true;
    }

    // RESCUE MODE: every physics tick, check whether "keep doing what the game is doing" dies
    // within the lookahead. If so, try "toggle the button now" (and, for tap modes that are
    // currently holding, "release now, press next tick") and fire it when it survives longer.
    void rescueTick(PlayLayer* pl, PlayerObject* player) {
        const int horizon = rescueTicks();
        if (horizon <= 0 || player->m_isDead || !m_simPlayer1) return;
        if (m_rescueCooldownLeft > 0) { m_rescueCooldownLeft--; return; }
        if (m_waitTicks != -1) return;   // a buffered input is pending: let the buffer finish

        const bool isDual = pl->m_gameState.m_isDualMode && pl->m_player2 != nullptr;
        PlayerObject* p2 = isDual ? pl->m_player2 : nullptr;
        const bool hold = m_gameHolding;

        // Hand control back as soon as it is safe: if rescue is overriding you and your REAL
        // button state survives the lookahead, go back to it instead of staying on its own path.
        if (m_rescueOverride) {
            if (hold == m_physicalHolding) {
                m_rescueOverride = false;
            } else {
                const int mine = getSurvivalTicks(pl, player, p2, isDual, 0, m_physicalHolding, horizon);
                if (mine >= horizon) {
                    fireButton(pl, m_physicalHolding);
                    m_rescueOverride = false;
                    m_rescueCooldownLeft = rescueCooldown();
                    return;
                }
            }
        }

        const int keep = getSurvivalTicks(pl, player, p2, isDual, 0, hold, horizon);
        if (keep >= horizon) return;     // safe as it is

        const int margin = rescueMargin();
        int best = keep;
        bool act = false;

        const int toggle = getSurvivalTicks(pl, player, p2, isDual, 0, !hold, horizon);
        if (toggle > best + margin || (toggle >= horizon && toggle > best)) { best = toggle; act = true; }

        if (hold) {
            // Tap modes: a new click needs a release first (release now, press on the next tick)
            const int repress = getSurvivalTicks(pl, player, p2, isDual, 0, false, horizon, 1);
            if (repress > best + margin || (repress >= horizon && repress > best)) { best = repress; act = true; }
        }

        if (act) {
            log::debug("Rescue: {} (survival {} -> {} of {} ticks)",
                hold ? "RELEASE" : "PRESS", keep, best, horizon);
            fireButton(pl, !hold);
            m_rescueOverride = true;
            m_rescueCooldownLeft = rescueCooldown();
        }
    }

    void onPlayerUpdate(PlayerObject* player) {
        PlayLayer* pl = PlayLayer::get();
        if (!pl || player != pl->m_player1) return;

        // Polled before the m_enabled check so the hotkey can always cycle back ON
        pollHotkeys();

        rescueTick(pl, player);

        if (!m_enabled) return;

        if (m_ticksSinceLastInput < 999) {
            m_ticksSinceLastInput++;
        }

        const bool shipActive = player->m_isShip || (pl->m_gameState.m_isDualMode && pl->m_player2 && pl->m_player2->m_isShip);
        if (shipActive && shipTicks() <= 0) {
            m_waitTicks = -1;
            m_queuedOpposite = false;
            if (m_gameHolding != m_physicalHolding && !m_rescueOverride) {
                fireButton(pl, m_physicalHolding);
            }
            return;
        }

        if (m_waitTicks == 0) {
            fireButton(pl, m_bufferedPush);
            if (m_queuedOpposite) {
                m_waitTicks = -2;
                m_queuedOpposite = false;
            } else {
                m_waitTicks = -1;
            }
        } else if (m_waitTicks > 0) {
            m_waitTicks--;
        } else if (m_waitTicks == -2) {
            fireButton(pl, !m_bufferedPush);
            m_queuedOpposite = false;
            m_waitTicks = -1;
        } else if (m_gameHolding != m_physicalHolding && !m_rescueOverride) {
            // Failsafe: with no buffer active, the game's hold state must match the
            // physical button so Wave can never stay stuck holding.
            fireButton(pl, m_physicalHolding);
        }
    }

    void fireButton(PlayLayer* pl, bool push) {
        m_firingBuffered = true;
        m_gameHolding = push;
        pl->handleButton(push, 1, true);
        if (pl->m_gameState.m_isDualMode && (!pl->m_level || !pl->m_level->m_twoPlayerMode)) {
            pl->handleButton(push, 1, false);
        }
        m_firingBuffered = false;
    }
};

static TrajectoryBufferSim s_sim;

// ============================================================================
//  SETTINGS PANEL (opened from the pause menu)
// ============================================================================

class BufferPopup : public geode::Popup {
protected:
    struct Row {
        const char* key;     // setting id (nullptr = current stage)
        const char* label;
        int min, max, step;
    };
    static constexpr Row ROWS[] = {
        { nullptr,               "Stage",                1,  PRESET_COUNT, 1 },
        { "custom-normal-ticks", "Custom delay (all)",   1,  64,           2 },
        { "custom-wave-ticks",   "Custom delay (wave)",  1,  32,           1 },
        { "normal-horizon",      "Lookahead (all)",      40, 320,          20 },
        { "wave-horizon",        "Lookahead (wave)",     30, 160,          5 },
        { "spam-streak",         "Wave/Ship spam streak",1,  10,           1 },
        { "ship-ticks",          "Ship delay (0 = off)", 0,  32,           1 },
        { "rescue-ticks",        "Rescue (0 = off)",     0,  80,           4 },
        { "rescue-margin",       "Rescue margin (low=aggr.)", 0, 20,       1 },
        { "rescue-cooldown",     "Rescue cooldown (ticks)",   0, 40,       2 },
    };
    static constexpr int ROW_COUNT = sizeof(ROWS) / sizeof(ROWS[0]);
    static constexpr const char* STAGE_NAMES[] = { "2/2", "4/4", "8/8", "16/8", "CUSTOM", "OFF" };

    std::vector<CCLabelBMFont*> m_valueLabels;
public:
    inline static bool s_open = false;
    ~BufferPopup() override { s_open = false; }
protected:

    static int readValue(int row) {
        if (!ROWS[row].key) return s_sim.stage() + 1;
        return static_cast<int>(Mod::get()->getSettingValue<int64_t>(ROWS[row].key));
    }

    static void writeValue(int row, int value) {
        if (!ROWS[row].key) {
            s_sim.setStage(value - 1);
            return;
        }
        Mod::get()->setSettingValue<int64_t>(ROWS[row].key, value);
        s_sim.refreshPreset();
    }

    void refreshLabels() {
        for (int i = 0; i < ROW_COUNT; i++) {
            int v = readValue(i);
            if (!ROWS[i].key) {
                int idx = std::clamp(v - 1, 0, PRESET_COUNT - 1);
                m_valueLabels[i]->setString(fmt::format("{} ({})", v, STAGE_NAMES[idx]).c_str());
            } else {
                m_valueLabels[i]->setString(std::to_string(v).c_str());
            }
        }
    }

    bool init() {
        if (!Popup::init(320.f, 250.f)) return false;
        this->setTitle("Predictive Input Buffer");

        // Scrollable list (mouse wheel or drag) so every row fits on any screen
        const CCSize viewSize { 290.f, 178.f };
        const float rowH = 32.f;
        const float contentH = std::max(viewSize.height, ROW_COUNT * rowH + 12.f);

        auto scroll = ScrollLayer::create(viewSize);
        scroll->m_contentLayer->setContentSize({ viewSize.width, contentH });
        m_mainLayer->addChildAtPosition(scroll, Anchor::Center, { -viewSize.width / 2, -viewSize.height / 2 - 12.f });

        auto menu = CCMenu::create();
        menu->setPosition({ 0.f, 0.f });
        menu->setContentSize({ viewSize.width, contentH });
        scroll->m_contentLayer->addChild(menu);

        m_valueLabels.clear();
        for (int i = 0; i < ROW_COUNT; i++) {
            const float y = contentH - 20.f - i * rowH;

            auto name = CCLabelBMFont::create(ROWS[i].label, "bigFont.fnt");
            name->setScale(0.38f);
            name->setAnchorPoint({ 0.f, 0.5f });
            name->setPosition({ 8.f, y });
            scroll->m_contentLayer->addChild(name);

            auto value = CCLabelBMFont::create("0", "goldFont.fnt");
            value->setScale(0.5f);
            value->setPosition({ viewSize.width - 108.f, y });
            scroll->m_contentLayer->addChild(value);
            m_valueLabels.push_back(value);

            for (int dir = 0; dir < 2; dir++) {
                auto spr = ButtonSprite::create(dir ? "+" : "-", 28, true, "bigFont.fnt", "GJ_button_04.png", 26.f, 0.7f);
                auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(BufferPopup::onStep));
                btn->setTag(i * 2 + dir);
                btn->setPosition({ viewSize.width - (dir ? 20.f : 56.f), y });
                menu->addChild(btn);
            }
        }
        scroll->scrollToTop();

        auto hint = CCLabelBMFont::create("Scroll with the mouse wheel. 4 ticks = 1 frame @ 60fps", "chatFont.fnt");
        hint->setScale(0.5f);
        hint->setOpacity(160);
        m_mainLayer->addChildAtPosition(hint, Anchor::Bottom, { 0.f, 10.f });

        refreshLabels();
        handleTouchPriority(this);
        return true;
    }

    void onStep(CCObject* sender) {
        int tag = static_cast<CCNode*>(sender)->getTag();
        int row = tag / 2;
        int dir = (tag % 2) ? 1 : -1;
        int v = std::clamp(readValue(row) + dir * ROWS[row].step, ROWS[row].min, ROWS[row].max);
        writeValue(row, v);
        refreshLabels();
    }

public:
    static BufferPopup* create() {
        auto ret = new BufferPopup();
        if (ret->init()) {
            ret->autorelease();
            s_open = true;
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// Opens the panel from anywhere; pauses the level first if we are playing.
void openBufferPanel() {
    if (BufferPopup::s_open) return;
    if (auto pl = PlayLayer::get()) {
        auto scene = CCDirector::get()->getRunningScene();
        bool alreadyPaused = scene && scene->getChildByType<PauseLayer>(0) != nullptr;
        if (!alreadyPaused) pl->pauseGame(false);
    }
    if (auto popup = BufferPopup::create()) popup->show();
}

class $modify(BufPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto spr = ButtonSprite::create("Buffer", "goldFont.fnt", "GJ_button_04.png", 0.8f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(BufPauseLayer::onBufferPanel));

        auto menu = CCMenu::create();
        auto winSize = CCDirector::get()->getWinSize();
        menu->setPosition({winSize.width - 50.f, 30.f});
        menu->addChild(btn);
        this->addChild(menu);
    }

    void onBufferPanel(CCObject*) {
        openBufferPanel();
    }
};

$on_mod(Loaded) {
    int64_t startStage = Mod::get()->getSettingValue<int64_t>("start-stage");
    s_sim.setStartStage(static_cast<int>(startStage));
    log::info("Predictive Input Buffer loaded. Press the hotkey (see mod settings) in-level to cycle stages.");
}

class $modify(BufPlayLayer, PlayLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayLayer::destroyPlayer", -1000000);
        (void)self.setHookPriority("PlayLayer::playEndAnimationToPos", -1000000);
        (void)self.setHookPriority("PlayLayer::flipGravity", -1000000);
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        s_sim.init();
        return true;
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        s_sim.resetState();
    }

    void destroyPlayer(PlayerObject* player, GameObject* gameObject) {
        if (s_sim.checkSimulationDeath(player)) return;
        PlayLayer::destroyPlayer(player, gameObject);
    }

    void playEndAnimationToPos(cocos2d::CCPoint p0) {
        if (s_sim.isSimulating()) return;
        PlayLayer::playEndAnimationToPos(p0);
    }

    void flipGravity(PlayerObject* p0, bool p1, bool p2) {
        if (s_sim.isSimulating()) return;
        PlayLayer::flipGravity(p0, p1, p2);
    }

    void onQuit() {
        PlayLayer::onQuit();
        s_sim.quit();
    }
};

class $modify(BufEditorLayer, LevelEditorLayer) {
    bool init(GJGameLevel* level, bool unk) {
        bool res = LevelEditorLayer::init(level, unk);
        s_sim.quit();
        return res;
    }
};

// ---- Suppress every side effect while the hidden simulation players run ----

class $modify(BufHardStreak, HardStreak) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("HardStreak::addPoint", -1000000);
    }

    void addPoint(cocos2d::CCPoint p0) {
        if (s_sim.isSimulating()) return;
        HardStreak::addPoint(p0);
    }
};

class $modify(BufEffectGameObject, EffectGameObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("EffectGameObject::triggerObject", -1000000);
    }

    void triggerObject(GJBaseGameLayer* p0, int p1, const gd::vector<int>* p2) {
        if (s_sim.isSimulating()) return;
        EffectGameObject::triggerObject(p0, p1, p2);
    }
};

class $modify(BufGameObject, GameObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("GameObject::playShineEffect", -1000000);
    }

    void playShineEffect() {
        if (s_sim.isSimulating()) return;
        GameObject::playShineEffect();
    }
};

class $modify(BufPlayerObject, PlayerObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayerObject::playSpiderDashEffect", -1000000);
        (void)self.setHookPriority("PlayerObject::incrementJumps", -1000000);
        (void)self.setHookPriority("PlayerObject::ringJump", -1000000);
    }

    void playSpiderDashEffect(cocos2d::CCPoint from, cocos2d::CCPoint to) {
        if (s_sim.isSimulating()) return;
        PlayerObject::playSpiderDashEffect(from, to);
    }

    void incrementJumps() {
        if (s_sim.isSimulating()) return;
        PlayerObject::incrementJumps();
    }

    void update(float dt) {
        if (!s_sim.isSimulating()) {
            s_sim.onPlayerUpdate(this);
        }
        PlayerObject::update(dt);
        if (PlayLayer::get() && !s_sim.isSimulating()) {
            s_sim.setFrameDelta(dt);
        }
    }

    void ringJump(RingObject* p0, bool p1) {
        if (s_sim.isSimulating()) return;
        PlayerObject::ringJump(p0, p1);
    }
};

class $modify(BufBaseGameLayer, GJBaseGameLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("GJBaseGameLayer::canBeActivatedByPlayer", -1000000);
        (void)self.setHookPriority("GJBaseGameLayer::handleButton", -1000000);
        (void)self.setHookPriority("GJBaseGameLayer::collisionCheckObjects", -1000000);
        (void)self.setHookPriority("GJBaseGameLayer::playerTouchedRing", -1000000);
    }

    bool canBeActivatedByPlayer(PlayerObject* p0, EffectGameObject* p1) {
        if (s_sim.isSimulating()) return false;
        return GJBaseGameLayer::canBeActivatedByPlayer(p0, p1);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (!s_sim.isSimulating() && !s_sim.isFiringBuffered() && button == 1 && PlayLayer::get()) {
            if (!s_sim.interceptInput(this, down, isPlayer1)) {
                return;
            }
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void collisionCheckObjects(PlayerObject* player, gd::vector<GameObject*>* vec, int objectsCount, float dt) {
        if (s_sim.isSimulating()) {
            // Only solid/hazard-type objects matter for the trajectory
            gd::vector<GameObject*> relevant;
            relevant.reserve(objectsCount);
            for (int i = 0; i < objectsCount; i++) {
                GameObject* obj = vec->at(i);
                if (!obj) continue;
                switch (obj->m_objectType) {
                    case GameObjectType::Solid:
                    case GameObjectType::Hazard:
                    case GameObjectType::AnimatedHazard:
                    case GameObjectType::CollisionObject:
                    case GameObjectType::Slope:
                        relevant.push_back(obj);
                        break;
                    default:
                        break;
                }
            }
            GJBaseGameLayer::collisionCheckObjects(player, &relevant, static_cast<int>(relevant.size()), dt);
            return;
        }
        GJBaseGameLayer::collisionCheckObjects(player, vec, objectsCount, dt);
    }

    void playerTouchedRing(PlayerObject* player, RingObject* ring) {
        if (s_sim.isSimulating()) return;
        GJBaseGameLayer::playerTouchedRing(player, ring);
    }
};
