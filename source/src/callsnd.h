#pragma once
// ZT17: a ridden animal's call / bite that the game itself does not sound (some species' 3D sounds never start - the
// game skips emitters far from its listener before Miles sees them) is played from the game's OWN data: the species'
// sound table (entities/sound/<Species>.xml, factory "<Species>_<Age>_<anim>") and the .wav it names, read straight
// from the installed .z2f archives (later archives win, like the game). Nothing is copied or shipped.
namespace callsnd {
    void Play(const char* entityType, const char* anim);   // async; entityType "TyrannosaurusJPOG_Adult_F"
}
