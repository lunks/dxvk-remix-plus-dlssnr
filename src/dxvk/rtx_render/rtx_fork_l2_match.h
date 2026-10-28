/*
* [nfs2se 0032] The exact-transform candidate choice of DrawCallTracker::findOrCreateReplacementInstance (level 2), as a pure template so a host test can
* run it without Remix (tools/remix-plus/test/test_l2_match.cpp).  A candidate is anything with frameLastSeen, materialHash, hostIdentity and vertexPositionHash;
* forEach(visitor) calls visitor(const C*) for each candidate at the draw's transform and stops when the visitor returns true.
*
* The rule (rtx.keepInstanceAcrossMaterialChange):
*  1. A candidate with the SAME material hash that no draw of this frame has taken: the old rule, taken at once.
*  2. Otherwise, when the option is on and the draw has no host identity: the one candidate with another material hash, but ONLY if it is the only
*     unclaimed candidate (same vertex-position hash, same host identity) at that transform.  Two or more are two layers of one surface (a base and a
*     decal, a multi-pass draw): which of them this draw is cannot be known, so none is taken and the draw makes its own instance (the old behaviour).
*  3. A candidate that a draw of THIS frame already took (frameLastSeen == frame) is never taken by either step; step 2 counts them (claimedSkipped).
* So two live draws of one geometry at one transform with different materials always end as two instances.
*/
#pragma once

#include <cstdint>

namespace dxvk {
  namespace nfs2se_inst {
    template<typename C>
    struct ExactPick {
      const C* pick = nullptr;
      bool relaxed = false;            // the pick has another material hash than the draw (kept across the change)
      uint32_t unclaimedOtherMat = 0;  // step 2: candidates with another material that were free (the ambiguity count)
      uint32_t claimedSkipped = 0;     // step 2: candidates of the same geometry already taken this frame
    };

    template<typename C, typename ForEach>
    ExactPick<C> pickExact(ForEach&& forEach, uint32_t frame, uint64_t vertexPositionHash, uint64_t materialHash, uint64_t hostIdentity, bool keepAcrossMaterial) {
      ExactPick<C> r;
      forEach([&](const C* c) {
        if (c->vertexPositionHash == vertexPositionHash && c->hostIdentity == hostIdentity && c->frameLastSeen != frame && c->materialHash == materialHash) {
          r.pick = c;
          return true;
        }
        return false;
      });
      if (r.pick != nullptr || !keepAcrossMaterial || hostIdentity != 0) {
        return r;
      }
      const C* only = nullptr;
      forEach([&](const C* c) {
        if (c->vertexPositionHash != vertexPositionHash || c->hostIdentity != hostIdentity) {
          return false;
        }
        if (c->frameLastSeen == frame) {
          ++r.claimedSkipped;
        } else {
          ++r.unclaimedOtherMat;
          only = c;
        }
        return false;
      });
      if (r.unclaimedOtherMat == 1) {
        r.pick = only;
        r.relaxed = true;
      }
      return r;
    }
  }
}
