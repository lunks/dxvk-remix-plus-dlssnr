/*
* [nfs2se 0032] Counters of what Remix does with an instance when a draw's material changes, printed as one
* "[nfs2se-inst]" line per 120 frames (rtx_draw_call_tracker.cpp, garbageCollectReplacementInstances).
* Header only; every counter is a relaxed atomic (the D3D9 draws and the API draws reach the tracker from the CS thread,
* the garbage collection from the same thread, so the line is a plain read).
*/
#pragma once

#include <atomic>
#include <cstdint>

namespace dxvk {
  namespace nfs2se_inst {
    struct Counters {
      std::atomic<uint64_t> instCreated { 0 };   // InstanceManager::addInstance: an RtInstance was made
      std::atomic<uint64_t> instGone { 0 };      // InstanceManager::garbageCollection: an RtInstance was destroyed
      std::atomic<uint64_t> matChanged { 0 };    // updateInstance: an existing instance took another material in place (hasMaterialChanged)
      std::atomic<uint64_t> relink { 0 };        // processSceneObject: an existing instance was re-linked to another BLAS entry (an API mesh swap kept in place)
      std::atomic<uint64_t> riCreated { 0 };     // DrawCallTracker level 3: a ReplacementInstance was made
      std::atomic<uint64_t> riGone { 0 };        // DrawCallTracker GC: a ReplacementInstance was destroyed
      std::atomic<uint64_t> l2Exact { 0 };       // level 2, exact transform: a ReplacementInstance matched after the identity hash missed
      std::atomic<uint64_t> l2XMat { 0 };        // ... of which the material hash differed (kept across the change: option rtx.keepInstanceAcrossMaterialChange)
      std::atomic<uint64_t> l2Declined { 0 };    // layered surfaces: a draw whose same-geometry candidates were two or more, or already taken this frame, and so made its own instance
      std::atomic<uint64_t> l2ClaimedSkipped { 0 }; // ... of which at least one candidate had been taken by a draw of this frame (the claim guard acted)
      std::atomic<uint64_t> events { 0 };        // the individually logged kept-instance events so far (the first few only)
    };

    inline Counters& counters() {
      static Counters s;
      return s;
    }

    constexpr uint64_t kMaxLoggedEvents = 24;
  }
}
