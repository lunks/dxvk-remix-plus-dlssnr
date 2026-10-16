#pragma once

// [nfs2se 0015] DIAGNOSTIC: GPU time per stage of a frame, from Vulkan timestamp queries, printed as one log line per
// window of frames. Remix's own GPU zones are Tracy-only (dxvk_scoped_annotation.h), so a release build had no way to
// say where the GPU spends a frame. Header-only; one instance per recording thread (a function-local static), its query
// pool lives for the process (diagnostic: never destroyed, the driver reclaims it at exit).
//
// Use: beginFrame(device, cmd) at the frame's first point, mark(cmd, i) at each later boundary i = 1 .. stages, endFrame()
// after the last mark (abortFrame() if the frame never reached its last mark). A ring of kRing frames is read back without
// waiting: a slot is read when it is about to be reused, and skipped (counted) if the GPU has not written it yet.

#include <cstdint>
#include <vector>

#include "../dxvk_device.h"
#include "../../util/log/log.h"
#include "../../util/util_string.h"

namespace dxvk {

  class Nfs2seGpuTimer {
  public:
    static constexpr uint32_t kRing = 8;
    static constexpr uint32_t kWindow = 600;

    // stageNames.size() stages => stageNames.size() + 1 marks (mark 0 = beginFrame).
    Nfs2seGpuTimer(const char* name, std::vector<const char*> stageNames)
      : m_name(name), m_stageNames(std::move(stageNames)) {
      m_marks = uint32_t(m_stageNames.size()) + 1;
      m_sum.assign(m_stageNames.size(), 0.0);
    }

    void beginFrame(DxvkDevice* device, VkCommandBuffer cmd) {
      if (!ensurePool(device)) {
        return;
      }
      m_slot = (m_slot + 1) % kRing;
      readSlot(m_slot);                       // the frame kRing frames ago, before its queries are reset
      m_device->vkd()->vkCmdResetQueryPool(cmd, m_pool, m_slot * m_marks, m_marks);
      m_device->vkd()->vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_pool, m_slot * m_marks);
      m_inFrame = true;
      m_written[m_slot] = false;
    }

    void mark(VkCommandBuffer cmd, uint32_t i) {
      if (!m_inFrame || i == 0 || i >= m_marks) {
        return;
      }
      m_device->vkd()->vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_pool, m_slot * m_marks + i);
    }

    void endFrame() {
      if (m_inFrame) {
        m_written[m_slot] = true;
        m_frameSeq[m_slot] = ++m_seq;
      }
      m_inFrame = false;
    }

    void abortFrame() {
      m_inFrame = false;
    }

  private:
    bool ensurePool(DxvkDevice* device) {
      if (m_pool != VK_NULL_HANDLE && m_device == device) {
        return true;
      }
      if (m_failed) {
        return false;
      }
      m_device = device;
      VkQueryPoolCreateInfo info = {};
      info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
      info.queryType = VK_QUERY_TYPE_TIMESTAMP;
      info.queryCount = kRing * m_marks;
      if (device->vkd()->vkCreateQueryPool(device->handle(), &info, nullptr, &m_pool) != VK_SUCCESS) {
        Logger::err(str::format("[nfs2se-gpu] ", m_name, ": vkCreateQueryPool failed; GPU timers off"));
        m_failed = true;
        m_pool = VK_NULL_HANDLE;
        return false;
      }
      m_nsPerTick = double(device->adapter()->deviceProperties().limits.timestampPeriod);
      for (uint32_t s = 0; s < kRing; ++s) {
        m_written[s] = false;
      }
      Logger::info(str::format("[nfs2se-gpu] ", m_name, ": GPU timers on (", m_stageNames.size(), " stages, ", m_nsPerTick, " ns per tick)"));
      return true;
    }

    void readSlot(uint32_t slot) {
      if (!m_written[slot]) {
        return;
      }
      m_written[slot] = false;
      uint64_t t[32] = {};
      if (m_marks > 32) {
        return;
      }
      const VkResult res = m_device->vkd()->vkGetQueryPoolResults(m_device->handle(), m_pool, slot * m_marks, m_marks,
        sizeof(uint64_t) * m_marks, t, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
      if (res != VK_SUCCESS) {
        ++m_notReady;                          // VK_NOT_READY: the GPU has not reached this frame's marks yet
        m_havePrev = false;
        return;
      }
      const double msPerTick = m_nsPerTick * 1e-6;
      for (uint32_t i = 0; i + 1 < m_marks; ++i) {
        m_sum[i] += double(t[i + 1] - t[i]) * msPerTick;
      }
      m_total += double(t[m_marks - 1] - t[0]) * msPerTick;
      // Consecutive frames only: the gap from the previous frame's last mark and the frame-to-frame interval.
      if (m_havePrev && m_frameSeq[slot] == m_prevSeq + 1) {
        m_outside += double(t[0] - m_prevEnd) * msPerTick;
        m_interval += double(t[0] - m_prevBegin) * msPerTick;
        ++m_pairs;
      }
      m_havePrev = true;
      m_prevSeq = m_frameSeq[slot];
      m_prevBegin = t[0];
      m_prevEnd = t[m_marks - 1];
      if (++m_frames >= kWindow) {
        flush();
      }
    }

    void flush() {
      const double n = double(m_frames);
      std::string line = str::format("[nfs2se-gpu] ", m_name, " frames=", m_frames, " not_ready=", m_notReady, " | ms per frame:");
      for (size_t i = 0; i < m_stageNames.size(); ++i) {
        line += str::format(" ", m_stageNames[i], "=", m_sum[i] / n);
        m_sum[i] = 0.0;
      }
      line += str::format(" | total=", m_total / n);
      if (m_pairs > 0) {
        line += str::format(" outside=", m_outside / double(m_pairs), " interval=", m_interval / double(m_pairs));
      }
      Logger::info(line);
      m_frames = 0;
      m_notReady = 0;
      m_total = m_outside = m_interval = 0.0;
      m_pairs = 0;
    }

    const char* m_name;
    std::vector<const char*> m_stageNames;
    uint32_t m_marks = 0;
    DxvkDevice* m_device = nullptr;
    VkQueryPool m_pool = VK_NULL_HANDLE;
    bool m_failed = false;
    double m_nsPerTick = 1.0;
    uint32_t m_slot = 0;
    bool m_inFrame = false;
    bool m_written[kRing] = {};
    uint64_t m_frameSeq[kRing] = {};
    uint64_t m_seq = 0;
    bool m_havePrev = false;
    uint64_t m_prevSeq = 0;
    uint64_t m_prevBegin = 0;
    uint64_t m_prevEnd = 0;
    std::vector<double> m_sum;
    double m_total = 0.0;
    double m_outside = 0.0;
    double m_interval = 0.0;
    uint64_t m_pairs = 0;
    uint32_t m_frames = 0;
    uint32_t m_notReady = 0;
  };

}
