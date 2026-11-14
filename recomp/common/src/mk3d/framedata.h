#pragma once
// Frame data in practice, shared by the games: for player 1's last attack, its startup (the move frame its attack is
// first tested against the other fighter, the press being frame 1), active and recovery frames, and its frame
// advantage on hit or on block (the frame player 2 can act again against the frame player 1 can: its next attack when
// it comes first, else its next block, the other shown beside it where the two differ). Drawn with the runtime's ImGui
// (app::GameInfo::ui), shown or hidden with F7 and the launcher's setting (MK3D_FRAMEDATA=0|1).
//
// It works in a practice fight on this PC alone, and in Online Practice (sparring.h, MK3D Online 1.0.8), where it
// follows this PC's player on either side; never in another game the launcher drives (MK3D_NET) or with netplay
// configured, so both games of an online match run exactly as they would without it. It only reads game memory, but
// for what the player asks for offline: F8 makes the dummy (player 2) hold block, so blocked attacks can be measured,
// and F6 makes it tech roll. Online Practice has no dummy: the two keys do nothing there.
//
// Online, it reads both fighters at the pad update of every frame run (a rollback has put the frame's top back by then,
// whichever order the frame-top hooks run in) and keeps its own state, and the adapter's (Adapter::local), as each
// frame found them: a frame run again after a rollback starts from those, so what it shows is what the frames that
// count did.
//
// A game provides an Adapter (where its fighters are, the game's own rule for when a fighter may start a move), calls
// ActiveCheck from its hit test, the one place that knows an attack is live in a frame, and asks DummyHolds from its
// "is this action held" function.
#include <cstddef>
#include <cstdint>

struct ImFont;

namespace mk3d::framedata {

// one fighter at the top of a frame, as the game's own rules see it
struct Fighter {
    bool can_act = false;       // the game would start a new move for it in this frame (its own gate)
    bool can_act_next = false;  // holding block where letting go is possible now: it could attack from the next frame
    bool neutral = false;       // standing in its stance: not walking, not in a move or a reaction (the practice record)
    bool can_block = false;     // holding block, a hit landing in this frame would be blocked (Adapter::block_rule;
                                // else false)
    bool block_waits_over = false;  // ... but for its no-block flags (Adapter::block_flags_early)
    bool striking = false;      // its attack's hit window is open (counts active frames after a contact)
    int contact = 0;            // its current attack's result: 0 none yet, 1 hit, 2 blocked
};

struct Adapter {
    // a practice fight is on screen (the game's mode, a fight set up)
    bool (*practice)();
    // the game's frame counter, which stands still while the game is paused
    uint32_t (*time)();
    // a fighter (0 player 1, 1 player 2) at the top of this frame; false when there is no fight to read
    bool (*read)(int side, Fighter& out);
    // the fighter's name, or null
    const char* (*name)(int side);
    // Fighter::can_block is filled in: a game whose block and move gate open at different frames, so player 1's next
    // block and next attack are followed apart
    bool block_rule = false;
    // player 1's own task clears its no-block flags before player 2's strike tests in the same frame (Deception and
    // Armageddon): flags first seen clear at the top of a frame were clear for a hit in the frame before, if its waits
    // were over then
    bool block_flags_early = false;
    // what the adapter keeps between frames (its reads and hit test hook), saved with each online frame's tracker state
    // and put back with it when a rollback runs that frame again; null for none
    void* local = nullptr;
    size_t local_size = 0;
};

// the tracker reads later in the frame than its top (Online Practice: at the pad update), after every frame-top hook
// ran: an adapter that applies a game rule before it reads, in case the rule's own frame-top hook has not run yet,
// leaves it then (it ran, and the game must not see it twice)
bool AfterTop();

// the game's adapter, once at start (before the first frame)
void Install(const Adapter& adapter);

// a practice fight is being followed: the game's hooks report nothing otherwise
bool Following();

// A practice fight on this PC alone with both fighters readable, as of the top of this frame, whether or not the
// panel is shown (the practice record, record.h): the fighters as the adapter read them then, and their names.
bool PracticeFight();
bool TopFighter(int side, Fighter& out);
const char* FighterName(int side);
// the overlay's font (null: the runtime's default) and the size it was loaded at, for the other practice overlays
ImFont* OverlayFont();
float OverlayFontPixels();

// tests: <prefix>_FRAMEDATA_VERSUS=1 follows versus fights too, where player 2 can block with a second controller (a
// game's practice() accepts its versus mode then; still never online)
bool VersusToo();

// The practice dummy (player 2) blocking, F8: the one change to the game this makes, and only in a practice fight on
// this PC alone. No controller drives the dummy, so each game's "is this action held" function answers for it: the
// game asks it about the dummy's controls (its port or player number) and the block action (both standing and
// crouching) or down (crouching only).
enum class Dummy { Stands, Blocks, CrouchBlocks };
// what the dummy does now: Stands outside a practice fight, and while a recording or the player's controller drives
// the dummy (record.h)
Dummy DummyBlock();
// for a game's "is this action held" hook: the dummy holds this action now (the game's numbers for block and down)
bool DummyHolds(int action, int block_action, int down_action);

// The practice dummy's tech roll, F6: on a knockdown it can roll out of, the dummy rolls up, down, back (away from
// player 1) or one of those at random, as a player holding that direction as they land would; or it lies down and
// gets up as before (off). Like F8, only in a practice fight on this PC alone.
enum class Tech { Off, Up, Down, Back, Random };
Tech DummyTech();  // Off outside a practice fight, and while a recording or the player's controller drives the dummy
// Around a game's tech roll check, for the fighter landing now: when that is the dummy and a tech roll is set, the
// dummy holds the roll's direction (and nothing else) until the check is done. A roll starts through the game's task
// switch, which leaves the check by unwinding it: the guard ends then too.
class TechScope {
public:
    explicit TechScope(bool dummy_lands);
    ~TechScope();
    TechScope(const TechScope&) = delete;
    TechScope& operator=(const TechScope&) = delete;

private:
    bool on_ = false;
    int unwinding_ = 0;  // exceptions in flight as it began: one more as it ends, the roll's task switch
};
// For a game's "is this action held" hook, asked about the dummy's controls: -1 while no tech roll is being decided
// (the hook goes on as before), else whether the dummy holds the action (1) or not (0). The games' numbers: up 0xC,
// right 0xD, down 0xE, left 0xF; back holds both sides, as the games ask only for the one away from the opponent.
int TechHolds(int action);

// the game tested this side's attack against the other fighter during this frame (its hit test, main thread)
void ActiveCheck(int side);

// shown or hidden as the launcher's setting changed while the game runs (any thread); F7 still toggles it
void SetShown(bool on);

// app::GameInfo::window (adds the overlay's font before the renderer builds the font atlas), ::ui and ::message (F7)
void Window(void* hwnd);
void Ui(double fps);
bool Message(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);

}  // namespace mk3d::framedata
