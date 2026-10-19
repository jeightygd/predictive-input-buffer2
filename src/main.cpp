// Predictive Input Buffer v0.2 - UNTESTED in-game. Written for Geode 5.11.2.
//
// F6 (in a level) cycles the buffer: 2 -> 4 -> 6 -> 8 -> 14 ticks -> off. It is off whenever
// you enter or leave a level.
//
// When you click, the mod snapshots the level state (a practice-mode style checkpoint), then for
// each candidate delay d in [0, buffer] ticks: restores the snapshot, applies the click d ticks
// later, runs the game forward `horizon` ticks and checks whether the player dies. The click is
// then really applied after the chosen surviving delay (1 tick = 1/240 s).
//
// Assumptions to verify first if it misbehaves:
//  - GJBaseGameLayer::handleButton(bool down, int button, bool isPlayer1) is where inputs land
//  - GJBaseGameLayer::processCommands(float) runs once per physics tick
//  - GJBaseGameLayer::update(1/240) advances exactly one tick
//  - PlayLayer::createCheckpoint / loadFromCheckpoint restore enough state to simulate from
//  - the button stays held for the whole look-ahead (so ship/wave are skipped)

#include <Geode/Geode.hpp>
#include <Geode/loader/GameEvent.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <string>
#include <vector>

using namespace geode::prelude;

namespace {
	struct Input { bool down; int button; bool isPlayer1; };
	struct Delayed { Input in; int ticksLeft; };

	// stage 0 = off, then 2, 4, 6, 8, 14 extra ticks
	constexpr int STAGE_TICKS[] = {0, 2, 4, 6, 8, 14};
	constexpr int STAGE_COUNT = 6;

	int g_stage = 0;
	bool g_simulating = false;        // true while a speculative simulation is running
	bool g_simDied = false;           // set when a simulated tick kills the player
	int g_activeShift = -1;           // delay used by the press that is currently held (-1 = none)
	std::vector<Input> g_undecided;   // inputs captured this tick, waiting for a decision
	std::vector<Delayed> g_delayed;   // inputs with a decided delay

	void resetState() {
		g_activeShift = -1;
		g_undecided.clear();
		g_delayed.clear();
	}

	bool isLivePlayLayer(GJBaseGameLayer* gl) {
		auto pl = PlayLayer::get();
		return pl && static_cast<GJBaseGameLayer*>(pl) == gl;
	}

	// Ship and wave need the exact hold pattern, which we can't predict, so skip them.
	bool modeSupported(PlayerObject* p) {
		return p && !p->m_isShip && !p->m_isDart;
	}

	bool shouldCapture(GJBaseGameLayer* gl, bool down, int button) {
		if (g_simulating || button != 1) return false;
		if (!isLivePlayLayer(gl)) return false;
		if (down) return g_stage > 0 && modeSupported(gl->m_player1);
		// releases follow the press they belong to, even if F6 turned the buffer off meanwhile
		return g_activeShift >= 0 || !g_undecided.empty();
	}
}

$on_game(Loaded) {
	listenForKeybindSettingPresses("cycle-key", [](Keybind const&, bool down, bool repeat, double) {
		if (!down || repeat) return;
		if (!PlayLayer::get()) return; // only works inside a level
		g_stage = (g_stage + 1) % STAGE_COUNT;
		std::string text = g_stage == 0
			? std::string("Input buffer: OFF")
			: fmt::format("Input buffer: up to {} ticks", STAGE_TICKS[g_stage]);
		Notification::create(text, NotificationIcon::None, 1.f)->show();
	});
}

class $modify(PIBPlayLayer, PlayLayer) {
	void setupHasCompleted() {
		g_stage = 0; // always start a level with the buffer off
		resetState();
		PlayLayer::setupHasCompleted();
	}

	void onQuit() {
		g_stage = 0; // off when leaving the level
		resetState();
		PlayLayer::onQuit();
	}

	void destroyPlayer(PlayerObject* player, GameObject* object) {
		if (g_simulating) {
			g_simDied = true; // swallow the death, just remember it
			return;
		}
		PlayLayer::destroyPlayer(player, object);
	}

	void resetLevel() {
		if (!g_simulating) resetState();
		PlayLayer::resetLevel();
	}
};

class $modify(PIBGameLayer, GJBaseGameLayer) {
	void handleButton(bool down, int button, bool isPlayer1) {
		if (!shouldCapture(this, down, button)) {
			GJBaseGameLayer::handleButton(down, button, isPlayer1);
			return;
		}
		Input in{down, button, isPlayer1};
		if (!down && g_undecided.empty()) {
			// release of a press we already decided on: use the same delay
			g_delayed.push_back({in, g_activeShift + 1});
			g_activeShift = -1;
		} else {
			g_undecided.push_back(in);
		}
	}

	void processCommands(float dt) {
		if (g_simulating) {
			GJBaseGameLayer::processCommands(dt);
			return;
		}
		pibApplyDue();
		GJBaseGameLayer::processCommands(dt);
		if (!g_undecided.empty()) pibDecide();
	}

	void pibApplyDue() {
		for (auto it = g_delayed.begin(); it != g_delayed.end();) {
			if (--it->ticksLeft <= 0) {
				Input in = it->in;
				it = g_delayed.erase(it);
				GJBaseGameLayer::handleButton(in.down, in.button, in.isPlayer1); // original, not our hook
			} else {
				++it;
			}
		}
	}

	void pibDecide() {
		auto inputs = std::move(g_undecided);
		g_undecided.clear();

		auto mod = Mod::get();
		int window = STAGE_TICKS[g_stage];
		int horizon = static_cast<int>(mod->getSettingValue<int64_t>("horizon"));
		auto pick = mod->getSettingValue<std::string>("pick");
		if (horizon < window + 8) horizon = window + 8;

		int shift = 0;
		size_t survivors = 0;
		auto pl = PlayLayer::get();
		CheckpointObject* snap = (pl && window > 0) ? pl->createCheckpoint() : nullptr;

		if (snap) {
			snap->retain();
			std::vector<int> good;
			g_simulating = true;

			for (int d = 0; d <= window; ++d) {
				pl->loadFromCheckpoint(snap);
				g_simDied = false;
				for (int t = 0; t < horizon && !g_simDied; ++t) {
					if (t == d) {
						for (auto& in : inputs)
							GJBaseGameLayer::handleButton(in.down, in.button, in.isPlayer1);
					}
					GJBaseGameLayer::update(1.f / 240.f); // one tick (see assumptions above)
				}
				if (!g_simDied) {
					good.push_back(d);
					if (pick == "earliest") break;
				}
			}

			pl->loadFromCheckpoint(snap); // back to the real state
			g_simulating = false;
			snap->release();

			survivors = good.size();
			if (!good.empty()) {
				if (pick == "latest") shift = good.back();
				else if (pick == "middle") shift = good[good.size() / 2];
				else shift = good.front();
			}
		}

		log::debug("press delayed by {} ticks ({} surviving candidates)", shift, survivors);

		for (auto& in : inputs) g_delayed.push_back({in, shift + 1});
		g_activeShift = inputs.back().down ? shift : -1;
	}
};
