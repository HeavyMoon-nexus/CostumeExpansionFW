#pragma once

#include <string>

namespace RE
{
    class Actor;
}

namespace CostumeFW
{
    // `cef av [base | diff | <name> | all]` - what CEF's stat passthrough has
    // actually done to an actor, without having to guess which actor value a
    // costume fortifies.
    //
    // `player.getavinfo <name>` answers one question at a time and only if you
    // already know the name, which is its own trap: the console spelling is
    // "fireresist", not "resistfire", and "magickaratemult", not "magickarate".
    // Get it wrong and you read a clean zero off an actor value that is fine
    // while the stranded one sits somewhere else.
    //
    //   cef av          every actor value with a non-zero modifier
    //   cef av base     remember the current numbers for this actor
    //   cef av diff     what changed since `base` (the only line that matters
    //                   when measuring an equip)
    //   cef av all      every actor value CEF can touch, zero or not
    //   cef av <name>   one actor value, by console name
    //
    // Reads only. Targets the console's selected reference if it is an actor,
    // otherwise the player - so an NPC's stranded modifiers are reachable by
    // clicking them, which is the one case `player.` cannot express.
    void AvReport(RE::Actor* a_actor, const std::string& a_arg);
}
