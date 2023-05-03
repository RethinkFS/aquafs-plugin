//
// Created by chiro on 23-4-28.
//

#include "zone_raid.h"

#include <memory>
#include <queue>
#include <utility>

#include "rocksdb/io_status.h"
#include "rocksdb/rocksdb_namespace.h"
#include "util/coding.h"
#include "util/mutexlock.h"

namespace AQUAFS_NAMESPACE {
using namespace ROCKSDB_NAMESPACE;

class RaidConsoleLogger : public Logger {
 public:
  using Logger::Logv;
  RaidConsoleLogger() : Logger(InfoLogLevel::DEBUG_LEVEL) {}

  void Logv(const char *format, va_list ap) override {
    MutexLock _(&lock_);
    printf("[RAID] ");
    vprintf(format, ap);
    printf("\n");
    fflush(stdout);
  }

  port::Mutex lock_;
};

/**
 * @brief Construct a new Raid Zoned Block Device object
 * @param mode main mode. RAID_A for auto-raid
 * @param devices all devices under management
 */
RaidZonedBlockDevice::RaidZonedBlockDevice(
    std::vector<std::unique_ptr<ZonedBlockDeviceBackend>> devices,
    RaidMode mode, std::shared_ptr<Logger> logger)
    : logger_(std::move(logger)),
      main_mode_(mode),
      devices_(std::move(devices)) {
  if (!logger_) logger_.reset(new RaidConsoleLogger());
  assert(!devices_.empty());
  Info(logger_, "RAID Devices: ");
  for (auto &&d : devices_) Info(logger_, "  %s", d->GetFilename().c_str());
  // create temporal device map: AQUAFS_META_ZONES in the first device is used
  // as meta zones, and marked as RAID_NONE; others are marked as RAID_C
  for (idx_t idx = 0; idx < AQUAFS_META_ZONES; idx++) {
    for (size_t i = 0; i < nr_dev(); i++)
      device_zone_map_[idx * nr_dev() + i] = {
          0, static_cast<idx_t>(idx * nr_dev() + i), 0};
    mode_map_[idx] = {RaidMode::RAID_NONE, 0};
  }
  syncBackendInfo();
}

IOStatus RaidZonedBlockDevice::Open(bool readonly, bool exclusive,
                                    unsigned int *max_active_zones,
                                    unsigned int *max_open_zones) {
  Info(logger_, "Open(readonly=%s, exclusive=%s)",
       std::to_string(readonly).c_str(), std::to_string(exclusive).c_str());
  IOStatus s;
  for (auto &&d : devices_) {
    s = d->Open(readonly, exclusive, max_active_zones, max_open_zones);
    if (!s.ok()) return s;
    Info(logger_,
         "%s opened, sz=%lx, nr_zones=%x, zone_sz=%lx blk_sz=%x "
         "max_active_zones=%x, max_open_zones=%x",
         d->GetFilename().c_str(), d->GetNrZones() * d->GetZoneSize(),
         d->GetNrZones(), d->GetZoneSize(), d->GetBlockSize(),
         *max_active_zones, *max_open_zones);
    assert(d->GetNrZones() == def_dev()->GetNrZones());
    assert(d->GetZoneSize() == def_dev()->GetZoneSize());
    assert(d->GetBlockSize() == def_dev()->GetBlockSize());
  }
  syncBackendInfo();
  Info(logger_, "after Open(): nr_zones=%x, zone_sz=%lx blk_sz=%x", nr_zones_,
       zone_sz_, block_sz_);
  if (main_mode_ == RaidMode::RAID_A) {
    // allocate default layout
    a_zones_.reset(new raid_zone_t[nr_zones_]);
    memset(a_zones_.get(), 0, sizeof(raid_zone_t) * nr_zones_);
    std::queue<size_t> available_devices;
    std::vector<std::queue<idx_t>> available_zones(nr_dev());
    for (size_t i = 0; i < nr_dev(); i++) {
      available_devices.push(i);
      for (idx_t idx = (i ? 0 : AQUAFS_META_ZONES); idx < nr_zones_; idx++)
        available_zones[i].push(idx);
    }
    for (idx_t idx = AQUAFS_META_ZONES; idx < nr_zones_; idx++) {
      for (size_t i = 0; i < nr_dev(); i++) {
        idx_t d = available_devices.front();
        auto d_next = (d == nr_dev() - 1) ? 0 : d + 1;
        available_devices.pop();
        idx_t ti;
        if (available_zones[d].empty()) {
          assert(!available_zones[d_next].empty());
          ti = available_zones[d_next].front();
          available_zones[d_next].pop();
        } else {
          assert(!available_zones[d].empty());
          ti = available_zones[d].front();
          available_zones[d].pop();
          available_devices.push(d_next);
        }
        device_zone_map_[idx * nr_dev() + i] = {d, ti, 0};
        // Info(logger_,
        //      "RAID-A: pre-allocate raid zone %x device_zone_map_[(idx*nr_dev
        //      + " "i)=%zx] = "
        //      "{d=%x, ti=%x, 0}",
        //      idx, idx * nr_dev() + i, d, ti);
      }
      mode_map_[idx] = {RaidMode::RAID0, 0};
      // mode_map_[idx] = {RaidMode::RAID1, 0};
      // mode_map_[idx] = {RaidMode::RAID_C, 0};
      // mode_map_[idx] = {RaidMode::RAID_NONE, 0};
    }
    flush_zone_info();
  }
  return s;
}

void RaidZonedBlockDevice::syncBackendInfo() {
  total_nr_devices_zones_ = std::accumulate(
      devices_.begin(), devices_.end(), 0,
      [](int sum, const std::unique_ptr<ZonedBlockDeviceBackend> &dev) {
        return sum + dev->GetNrZones();
      });
  block_sz_ = def_dev()->GetBlockSize();
  zone_sz_ = def_dev()->GetZoneSize();
  nr_zones_ = def_dev()->GetNrZones();
  switch (main_mode_) {
    case RaidMode::RAID_C:
      nr_zones_ = total_nr_devices_zones_;
      break;
    case RaidMode::RAID_A:
    case RaidMode::RAID0:
      zone_sz_ *= nr_dev();
      break;
    case RaidMode::RAID1:
      break;
    default:
      nr_zones_ = 0;
      break;
  }
  // Debug(logger_, "syncBackendInfo(): blksz=%x, zone_sz=%lx, nr_zones=%x",
  //       block_sz_, zone_sz_, nr_zones_);
}

std::unique_ptr<ZoneList> RaidZonedBlockDevice::ListZones() {
  // Debug(logger_, "ListZones()");
  if (main_mode_ == RaidMode::RAID_C) {
    std::vector<std::unique_ptr<ZoneList>> list;
    for (auto &&dev : devices_) {
      auto zones = dev->ListZones();
      if (zones) {
        list.emplace_back(std::move(zones));
      }
    }
    // merge zones
    auto nr_zones = std::accumulate(
        list.begin(), list.end(), 0,
        [](int sum, auto &zones) { return sum + zones->ZoneCount(); });
    auto data = new struct zbd_zone[nr_zones];
    auto ptr = data;
    for (auto &&zones : list) {
      auto nr = zones->ZoneCount();
      memcpy(ptr, zones->GetData(), sizeof(struct zbd_zone) * nr);
      ptr += nr;
    }
    return std::make_unique<ZoneList>(data, nr_zones);
  } else if (main_mode_ == RaidMode::RAID1) {
    return def_dev()->ListZones();
  } else if (main_mode_ == RaidMode::RAID0) {
    auto zones = def_dev()->ListZones();
    if (zones) {
      auto nr_zones = zones->ZoneCount();
      // TODO: mix use of ZoneFS and libzbd
      auto data = new struct zbd_zone[nr_zones];
      auto ptr = data;
      memcpy(data, zones->GetData(), sizeof(struct zbd_zone) * nr_zones);
      for (decltype(nr_zones) i = 0; i < nr_zones; i++) {
        ptr->start *= nr_dev();
        ptr->capacity *= nr_dev();
        // what's this? len == capacity?
        ptr->len *= nr_dev();
        ptr++;
      }
      return std::make_unique<ZoneList>(data, nr_zones);
    }
  } else if (main_mode_ == RaidMode::RAID_A) {
    auto data = new raid_zone_t[nr_zones_];
    memcpy(data, a_zones_.get(), sizeof(raid_zone_t) * nr_zones_);
    return std::make_unique<ZoneList>(data, nr_zones_);
  }
  return {};
}

IOStatus RaidZonedBlockDevice::Reset(uint64_t start, bool *offline,
                                     uint64_t *max_capacity) {
  Info(logger_, "Reset(start=%lx)", start);
  if (main_mode_ == RaidMode::RAID_C) {
    for (auto &&d : devices_) {
      auto sz = d->GetNrZones() * d->GetZoneSize();
      if (sz > start) {
        return d->Reset(start, offline, max_capacity);
      } else {
        start -= sz;
      }
    }
    return IOStatus::IOError();
  } else if (main_mode_ == RaidMode::RAID1) {
    IOStatus s;
    for (auto &&d : devices_) {
      s = d->Reset(start, offline, max_capacity);
      if (!s.ok()) return s;
    }
    return s;
  } else if (main_mode_ == RaidMode::RAID0) {
    assert(start % GetBlockSize() == 0);
    assert(start % GetZoneSize() == 0);
    // auto idx_dev = get_idx_dev(start);
    auto s = start / nr_dev();
    // auto r = devices_[idx_dev]->Reset(s, offline, max_capacity);
    IOStatus r{};
    for (auto &&d : devices_) {
      r = d->Reset(s, offline, max_capacity);
      if (r.ok()) {
        *max_capacity *= nr_dev();
      }
    }
    return r;
  } else if (main_mode_ == RaidMode::RAID_A) {
    assert(start % GetZoneSize() == 0);
    IOStatus r{};
    auto zone_idx = start / zone_sz_;
    for (size_t i = 0; i < nr_dev(); i++) {
      auto m = device_zone_map_[i + zone_idx * nr_dev()];
      r = devices_[m.device_idx]->Reset(m.zone_idx * def_dev()->GetZoneSize(),
                                        offline, max_capacity);
      Info(logger_, "RAID-A: do reset for device %d, zone %d", m.device_idx,
           m.zone_idx);
      if (!r.ok())
        return r;
      else
        *max_capacity *= nr_dev();
    }
    flush_zone_info();
    return r;
  }
  return unsupported;
}

IOStatus RaidZonedBlockDevice::Finish(uint64_t start) {
  Info(logger_, "Finish(%lx)", start);
  if (main_mode_ == RaidMode::RAID_C) {
    for (auto &&d : devices_) {
      auto sz = d->GetNrZones() * d->GetZoneSize();
      if (sz > start) {
        return d->Finish(start);
      } else {
        start -= sz;
      }
    }
  } else if (main_mode_ == RaidMode::RAID1) {
    IOStatus s;
    for (auto &&d : devices_) {
      s = d->Finish(start);
      if (!s.ok()) return s;
    }
    return s;
  } else if (main_mode_ == RaidMode::RAID0) {
    assert(start % GetBlockSize() == 0);
    assert(start % GetZoneSize() == 0);
    // auto idx_dev = get_idx_dev(start);
    auto s = start / nr_dev();
    // auto r = devices_[idx_dev]->Finish(s);
    IOStatus r{};
    for (auto &&d : devices_) {
      r = d->Finish(s);
      if (!r.ok()) return r;
    }
    return r;
  } else if (main_mode_ == RaidMode::RAID_A) {
    assert(start % GetZoneSize() == 0);
    IOStatus r{};
    auto zone_idx = start / zone_sz_;
    for (size_t i = 0; i < nr_dev(); i++) {
      auto m = device_zone_map_[i + zone_idx * nr_dev()];
      r = devices_[m.device_idx]->Finish(m.zone_idx * def_dev()->GetZoneSize());
      Info(logger_, "RAID-A: do finish for device %d, zone %d", m.device_idx,
           m.zone_idx);
      if (!r.ok()) return r;
    }
    flush_zone_info();
    return r;
  }
  return unsupported;
}

IOStatus RaidZonedBlockDevice::Close(uint64_t start) {
  Info(logger_, "Close(start=%lx)", start);
  if (main_mode_ == RaidMode::RAID_C) {
    for (auto &&d : devices_) {
      auto sz = d->GetNrZones() * d->GetZoneSize();
      if (sz > start) {
        return d->Close(start);
      } else {
        start -= sz;
      }
    }
  } else if (main_mode_ == RaidMode::RAID1) {
    IOStatus s;
    for (auto &&d : devices_) {
      s = d->Close(start);
      if (!s.ok()) return s;
    }
    return s;
  } else if (main_mode_ == RaidMode::RAID0) {
    assert(start % GetBlockSize() == 0);
    assert(start % GetZoneSize() == 0);
    // auto idx_dev = get_idx_dev(start);
    auto s = start / nr_dev();
    // auto r = devices_[idx_dev]->Close(s);
    IOStatus r{};
    for (auto &&d : devices_) {
      r = d->Close(s);
      if (!r.ok()) {
        return r;
      }
    }
    return r;
  } else if (main_mode_ == RaidMode::RAID_A) {
    IOStatus r{};
    auto zone_idx = start / zone_sz_;
    for (size_t i = 0; i < nr_dev(); i++) {
      auto m = device_zone_map_[i + zone_idx * nr_dev()];
      r = devices_[m.device_idx]->Close(m.zone_idx * def_dev()->GetZoneSize());
      Info(logger_, "RAID-A: do close for device %d, zone %d", m.device_idx,
           m.zone_idx);
      if (!r.ok()) return r;
    }
    flush_zone_info();
    return r;
  }
  return unsupported;
}

int RaidZonedBlockDevice::Read(char *buf, int size, uint64_t pos, bool direct) {
  // Debug(logger_, "Read(sz=%x, pos=%lx, direct=%s)", size, pos,
  //       std::to_string(direct).c_str());
  if (main_mode_ == RaidMode::RAID_C) {
    for (auto &&d : devices_) {
      auto sz = d->GetNrZones() * d->GetZoneSize();
      if (sz > pos) {
        return d->Read(buf, size, pos, direct);
      } else {
        pos -= sz;
      }
    }
  } else if (main_mode_ == RaidMode::RAID1) {
    int r = 0;
    for (auto &&d : devices_) {
      if ((r = d->Read(buf, size, pos, direct))) {
        return r;
      }
    }
    return r;
  } else if (main_mode_ == RaidMode::RAID0) {
    // split read range as blocks
    int sz_read = 0;
    // TODO: Read blocks in multi-threads
    int r;
    while (size > 0) {
      auto req_size = std::min(
          size, static_cast<int>(GetBlockSize() - pos % GetBlockSize()));
      r = devices_[get_idx_dev(pos)]->Read(buf, req_size, req_pos(pos), direct);
      if (r > 0) {
        size -= r;
        sz_read += r;
        buf += r;
        pos += r;
      } else {
        return r;
      }
    }
    return sz_read;
  } else if (main_mode_ == RaidMode::RAID_A) {
    if (static_cast<decltype(zone_sz_)>(size) > zone_sz_) {
      // may cross raid zone, split read range as zones
      int sz_read = 0;
      int r;
      while (size > 0) {
        auto req_size =
            std::min(size, static_cast<int>(zone_sz_ - pos % zone_sz_));
        r = Read(buf, req_size, pos, direct);
        if (r > 0) {
          buf += r;
          pos += r;
          sz_read += r;
          size -= r;
        } else {
          return r;
        }
      }
      flush_zone_info();
      return sz_read;
    } else {
      assert(static_cast<decltype(zone_sz_)>(size) <= zone_sz_);
      auto mode_item = mode_map_[pos / zone_sz_];
      auto m = getAutoDeviceZone(pos);
      auto mapped_pos = getAutoMappedDevicePos(pos);
      if (mode_item.mode == RaidMode::RAID_C ||
          mode_item.mode == RaidMode::RAID1 ||
          mode_item.mode == RaidMode::RAID_NONE) {
        // Info(logger_,
        //      "RAID-A: READ raid%s mapping pos=%lx to mapped_pos=%lx, dev=%x, "
        //      "zone=%x",
        //      raid_mode_str(mode_item.mode), pos, mapped_pos, m.device_idx,
        //      m.zone_idx);
        return devices_[m.device_idx]->Read(buf, size, mapped_pos, direct);
      } else if (mode_item.mode == RaidMode::RAID0) {
        // split read range as blocks
        int sz_read = 0;
        // TODO: Read blocks in multi-threads
        int r;
        while (size > 0) {
          m = getAutoDeviceZone(pos);
          mapped_pos = getAutoMappedDevicePos(pos);
          auto req_size = std::min(
              size,
              static_cast<int>(GetBlockSize() - mapped_pos % GetBlockSize()));
          r = devices_[m.device_idx]->Read(buf, req_size, mapped_pos, direct);
          // Info(
          //     logger_,
          //     "RAID-A: [read=%x] READ raid0 mapping pos=%lx to mapped_pos=%lx, "
          //     "dev=%x, zone=%x; r=%x",
          //     sz_read, pos, mapped_pos, m.device_idx, m.zone_idx, r);
          if (r > 0) {
            size -= r;
            sz_read += r;
            buf += r;
            pos += r;
          } else {
            return r;
          }
        }
        flush_zone_info();
        return sz_read;
      } else {
        assert(false);
      }
    }
  }
  return 0;
}

int RaidZonedBlockDevice::Write(char *data, uint32_t size, uint64_t pos) {
  // Debug(logger_, "Write(size=%x, pos=%lx)", size, pos);
  if (main_mode_ == RaidMode::RAID_C) {
    for (auto &&d : devices_) {
      auto sz = d->GetNrZones() * d->GetZoneSize();
      if (sz > pos) {
        return d->Write(data, size, pos);
      } else {
        pos -= sz;
      }
    }
  } else if (main_mode_ == RaidMode::RAID1) {
    int r = 0;
    for (auto &&d : devices_) {
      if ((r = d->Write(data, size, pos))) {
        return r;
      }
    }
    return r;
  } else if (main_mode_ == RaidMode::RAID0) {
    // split read range as blocks
    int sz_written = 0;
    // TODO: write blocks in multi-threads
    int r;
    while (size > 0) {
      auto req_size = std::min(
          size, GetBlockSize() - (static_cast<uint32_t>(pos)) % GetBlockSize());
      auto p = req_pos(pos);
      auto idx_dev = get_idx_dev(pos);
      r = devices_[idx_dev]->Write(data, req_size, p);
      // Debug(logger_, "WRITE: pos=%lx, dev=%lu, req_sz=%x, req_pos=%lx,
      // ret=%d",
      //       pos, idx_dev, req_size, p, r);
      if (r > 0) {
        size -= r;
        sz_written += r;
        data += r;
        pos += r;
      } else {
        return r;
      }
    }
    flush_zone_info();
    return sz_written;
  } else if (main_mode_ == RaidMode::RAID_A) {
    if (static_cast<decltype(zone_sz_)>(size) > zone_sz_) {
      // may cross raid zone, split write range as zones
      int sz_written = 0;
      int r;
      while (size > 0) {
        auto req_size =
            std::min(size, static_cast<uint32_t>(zone_sz_ - pos % zone_sz_));
        r = Write(data, req_size, pos);
        if (r > 0) {
          data += r;
          pos += r;
          sz_written += r;
          size -= r;
        } else {
          return r;
        }
      }
      return sz_written;
    } else {
      assert(static_cast<decltype(zone_sz_)>(size) <= zone_sz_);
      auto mode_item = mode_map_[pos / zone_sz_];
      auto m = getAutoDeviceZone(pos);
      auto mapped_pos = getAutoMappedDevicePos(pos);
      if (mode_item.mode == RaidMode::RAID_C ||
          mode_item.mode == RaidMode::RAID1 ||
          mode_item.mode == RaidMode::RAID_NONE) {
        auto r = devices_[m.device_idx]->Write(data, size, mapped_pos);
        // Info(logger_,
        //      "RAID-A: WRITE raid%s mapping pos=%lx to mapped_pos=%lx, dev=%x,
        //      " "zone=%x; r=%x", raid_mode_str(mode_item.mode), pos,
        //      mapped_pos, m.device_idx, m.zone_idx, r);
        return r;
      } else if (mode_item.mode == RaidMode::RAID0) {
        // split write range as blocks
        int sz_written = 0;
        // TODO: Write blocks in multi-threads
        int r;
        while (size > 0) {
          m = getAutoDeviceZone(pos);
          mapped_pos = getAutoMappedDevicePos(pos);
          // auto mi = getAutoDeviceZoneIdx(pos);
          // auto z = devices_[m.device_idx]->ListZones();
          // auto p = reinterpret_cast<raid_zone_t *>(z->GetData());
          // auto pp = p[m.zone_idx];
          // if (pos == 0x10001000 || pos == 0x10000000 ||
          //     (m.device_idx == 1 && m.zone_idx == 0)) {
          //   Info(logger_,
          //        "[DBG] RAID-A-0: dev_zone_info: st=%llx, cap=%llx, "
          //        "wp=%llx, sz=%llx; to write: dev=%x, zone=%x, pos=%lx, sz=%x",
          //        pp.start, pp.capacity, pp.wp, pp.capacity, m.device_idx,
          //        m.zone_idx, mapped_pos, size);
          // }
          auto req_size =
              std::min(size, static_cast<uint32_t>(
                                 GetBlockSize() - mapped_pos % GetBlockSize()));
          r = devices_[m.device_idx]->Write(data, req_size, mapped_pos);
          // Info(logger_,
          //      "RAID-A: [written=%x] WRITE raid0 mapping pos=%lx to "
          //      "mapped_pos=%lx, "
          //      "dev=%x, zone=%x; r=%x",
          //      sz_written, pos, mapped_pos, m.device_idx, m.zone_idx, r);
          if (r > 0) {
            size -= r;
            sz_written += r;
            data += r;
            pos += r;
          } else {
            return r;
          }
        }
        flush_zone_info();
        return sz_written;
      }
    }
  }
  return 0;
}

int RaidZonedBlockDevice::InvalidateCache(uint64_t pos, uint64_t size) {
  // Debug(logger_, "InvalidateCache(pos=%lx, sz=%lx)", pos, size);
  if (main_mode_ == RaidMode::RAID_C) {
    for (auto &&d : devices_) {
      auto sz = d->GetNrZones() * d->GetZoneSize();
      if (sz > pos) {
        return d->InvalidateCache(pos, size);
      } else {
        pos -= sz;
      }
    }
  } else if (main_mode_ == RaidMode::RAID1) {
    int r = 0;
    for (auto &&d : devices_) r = d->InvalidateCache(pos, size);
    return r;
  } else if (main_mode_ == RaidMode::RAID0) {
    assert(size % GetBlockSize() == 0);
    for (size_t i = 0; i < nr_dev(); i++) {
      devices_[i]->InvalidateCache(req_pos(pos), size / nr_dev());
    }
  } else if (main_mode_ == RaidMode::RAID_A) {
    assert(size % zone_sz_ == 0);
    if (static_cast<decltype(zone_sz_)>(size) > zone_sz_) {
      // may cross raid zone, split range as zones
      int r;
      while (size > 0) {
        auto req_size = std::min(
            size, static_cast<decltype(size)>(zone_sz_ - pos % zone_sz_));
        r = InvalidateCache(pos, req_size);
        if (!r) {
          pos += zone_sz_;
          size -= zone_sz_;
        } else {
          return r;
        }
      }
      return 0;
    } else {
      assert(pos % GetZoneSize() == 0);
      assert(static_cast<decltype(zone_sz_)>(size) <= zone_sz_);
      auto m = getAutoDeviceZone(pos);
      auto mapped_pos = getAutoMappedDevicePos(pos);
      auto r = devices_[m.device_idx]->InvalidateCache(mapped_pos, size);
      flush_zone_info();
      return r;
    }
  }
  // default OK
  return 0;
}

bool RaidZonedBlockDevice::ZoneIsSwr(std::unique_ptr<ZoneList> &zones,
                                     idx_t idx) {
  // Debug(logger_, "ZoneIsSwr(idx=%x)", idx);
  if (main_mode_ == RaidMode::RAID_C) {
    for (auto &&d : devices_) {
      if (d->GetNrZones() > idx) {
        auto z = d->ListZones();
        return d->ZoneIsSwr(z, idx);
      } else {
        idx -= d->GetNrZones();
      }
    }
    return false;
  } else if (main_mode_ == RaidMode::RAID1) {
    return def_dev()->ZoneIsSwr(zones, idx);
  } else if (main_mode_ == RaidMode::RAID0) {
    // asserts that all devices have the same zone layout
    auto z = def_dev()->ListZones();
    return def_dev()->ZoneIsSwr(z, idx);
  } else if (main_mode_ == RaidMode::RAID_A) {
    auto m = getAutoDeviceZone(idx);
    auto z = devices_[m.device_idx]->ListZones();
    return devices_[m.device_idx]->ZoneIsSwr(z, m.zone_idx);
  }
  return false;
}

bool RaidZonedBlockDevice::ZoneIsOffline(std::unique_ptr<ZoneList> &zones,
                                         idx_t idx) {
  // Debug(logger_, "ZoneIsOffline(idx=%x)", idx);
  if (main_mode_ == RaidMode::RAID_C) {
    for (auto &&d : devices_) {
      if (d->GetNrZones() > idx) {
        // FIXME: optimize list-zones
        auto z = d->ListZones();
        return d->ZoneIsOffline(z, idx);
      } else {
        idx -= d->GetNrZones();
      }
    }
    return false;
  } else if (main_mode_ == RaidMode::RAID1) {
    return def_dev()->ZoneIsOffline(zones, idx);
  } else if (main_mode_ == RaidMode::RAID0) {
    // asserts that all devices have the same zone layout
    auto z = def_dev()->ListZones();
    return def_dev()->ZoneIsOffline(z, idx);
  } else if (main_mode_ == RaidMode::RAID_A) {
    auto m = getAutoDeviceZone(idx);
    auto z = devices_[m.device_idx]->ListZones();
    return devices_[m.device_idx]->ZoneIsOffline(z, m.zone_idx);
  }
  return false;
}

bool RaidZonedBlockDevice::ZoneIsWritable(std::unique_ptr<ZoneList> &zones,
                                          idx_t idx) {
  // Debug(logger_, "ZoneIsWriteable(idx=%x)", idx);
  if (main_mode_ == RaidMode::RAID_C) {
    for (auto &&d : devices_) {
      if (d->GetNrZones() > idx) {
        auto z = d->ListZones();
        return d->ZoneIsWritable(z, idx);
      } else {
        idx -= d->GetNrZones();
      }
    }
  } else if (main_mode_ == RaidMode::RAID1) {
    return def_dev()->ZoneIsWritable(zones, idx);
  } else if (main_mode_ == RaidMode::RAID0) {
    // asserts that all devices have the same zone layout
    auto z = def_dev()->ListZones();
    return def_dev()->ZoneIsWritable(z, idx);
  } else if (main_mode_ == RaidMode::RAID_A) {
    auto m = getAutoDeviceZone(idx);
    auto z = devices_[m.device_idx]->ListZones();
    return devices_[m.device_idx]->ZoneIsWritable(z, m.zone_idx);
  }
  return false;
}

bool RaidZonedBlockDevice::ZoneIsActive(std::unique_ptr<ZoneList> &zones,
                                        idx_t idx) {
  // Debug(logger_, "ZoneIsActive(idx=%x)", idx);
  if (main_mode_ == RaidMode::RAID_C) {
    for (auto &&d : devices_) {
      if (d->GetNrZones() > idx) {
        auto z = d->ListZones();
        return d->ZoneIsActive(z, idx);
      } else {
        idx -= d->GetNrZones();
      }
    }
  } else if (main_mode_ == RaidMode::RAID1) {
    return def_dev()->ZoneIsActive(zones, idx);
  } else if (main_mode_ == RaidMode::RAID0) {
    // asserts that all devices have the same zone layout
    auto z = def_dev()->ListZones();
    return def_dev()->ZoneIsActive(z, idx);
  } else if (main_mode_ == RaidMode::RAID_A) {
    auto m = getAutoDeviceZone(idx);
    auto z = devices_[m.device_idx]->ListZones();
    return devices_[m.device_idx]->ZoneIsActive(z, m.zone_idx);
  }
  return false;
}

bool RaidZonedBlockDevice::ZoneIsOpen(std::unique_ptr<ZoneList> &zones,
                                      idx_t idx) {
  // Debug(logger_, "ZoneIsOpen(idx=%x)", idx);
  if (main_mode_ == RaidMode::RAID_C) {
    for (auto &&d : devices_) {
      if (d->GetNrZones() > idx) {
        auto z = d->ListZones();
        return d->ZoneIsOpen(z, idx);
      } else {
        idx -= d->GetNrZones();
      }
    }
  } else if (main_mode_ == RaidMode::RAID1) {
    return def_dev()->ZoneIsOpen(zones, idx);
  } else if (main_mode_ == RaidMode::RAID0) {
    // asserts that all devices have the same zone layout
    auto z = def_dev()->ListZones();
    return def_dev()->ZoneIsOpen(z, idx);
  } else if (main_mode_ == RaidMode::RAID_A) {
    auto m = getAutoDeviceZone(idx);
    auto z = devices_[m.device_idx]->ListZones();
    return devices_[m.device_idx]->ZoneIsOpen(z, m.zone_idx);
  }
  return false;
}

uint64_t RaidZonedBlockDevice::ZoneStart(std::unique_ptr<ZoneList> &zones,
                                         idx_t idx) {
  // Debug(logger_, "ZoneStart(idx=%x)", idx);
  if (main_mode_ == RaidMode::RAID_C) {
    for (auto &&d : devices_) {
      if (d->GetNrZones() > idx) {
        auto z = d->ListZones();
        return d->ZoneStart(z, idx);
      } else {
        idx -= d->GetNrZones();
      }
    }
  } else if (main_mode_ == RaidMode::RAID1) {
    return def_dev()->ZoneStart(zones, idx);
  } else if (main_mode_ == RaidMode::RAID0) {
    auto r =
        std::accumulate(devices_.begin(), devices_.end(),
                        static_cast<uint64_t>(0), [&](uint64_t sum, auto &d) {
                          auto z = d->ListZones();
                          return sum + d->ZoneStart(z, idx);
                        });
    return r;
  } else if (main_mode_ == RaidMode::RAID_A) {
    // FIXME?
    return reinterpret_cast<raid_zone_t *>(zones.get()->GetData())[idx].start;
  }
  return 0;
}

uint64_t RaidZonedBlockDevice::ZoneMaxCapacity(std::unique_ptr<ZoneList> &zones,
                                               idx_t idx) {
  // Debug(logger_, "ZoneMaxCapacity(idx=%x)", idx);
  if (main_mode_ == RaidMode::RAID_C) {
    for (auto &&d : devices_) {
      if (d->GetNrZones() > idx) {
        auto z = d->ListZones();
        return d->ZoneMaxCapacity(z, idx);
      } else {
        idx -= d->GetNrZones();
      }
    }
  } else if (main_mode_ == RaidMode::RAID1) {
    return def_dev()->ZoneMaxCapacity(zones, idx);
  } else if (main_mode_ == RaidMode::RAID0) {
    // asserts that all devices have the same zone layout
    auto z = def_dev()->ListZones();
    return def_dev()->ZoneMaxCapacity(z, idx) * nr_dev();
  } else if (main_mode_ == RaidMode::RAID_A) {
    // FIXME: capacity == max_capacity ?
    return reinterpret_cast<raid_zone_t *>(zones.get()->GetData())[idx]
        .capacity;
  }
  return 0;
}

uint64_t RaidZonedBlockDevice::ZoneWp(std::unique_ptr<ZoneList> &zones,
                                      idx_t idx) {
  // Debug(logger_, "ZoneWp(idx=%x)", idx);
  if (main_mode_ == RaidMode::RAID_C) {
    for (auto &&d : devices_) {
      if (d->GetNrZones() > idx) {
        auto z = d->ListZones();
        return d->ZoneWp(z, idx);
      } else {
        idx -= d->GetNrZones();
      }
    }
  } else if (main_mode_ == RaidMode::RAID1) {
    return def_dev()->ZoneWp(zones, idx);
  } else if (main_mode_ == RaidMode::RAID0) {
    return std::accumulate(devices_.begin(), devices_.end(),
                           static_cast<uint64_t>(0),
                           [&](uint64_t sum, auto &d) {
                             auto z = d->ListZones();
                             return sum + d->ZoneWp(z, idx);
                           });
  } else if (main_mode_ == RaidMode::RAID_A) {
    flush_zone_info();
    auto r = reinterpret_cast<raid_zone_t *>(zones.get()->GetData())[idx].wp;
    // Info(logger_, "RAID-A: ZoneWp=%llx", r);
    return r;
  }
  return 0;
}

std::string RaidZonedBlockDevice::GetFilename() {
  std::string name = std::string("raid") + raid_mode_str(main_mode_) + ":";
  for (auto p = devices_.begin(); p != devices_.end(); p++) {
    name += (*p)->GetFilename();
    if (p + 1 != devices_.end()) name += ",";
  }
  return name;
}
bool RaidZonedBlockDevice::IsRAIDEnabled() const { return true; }
RaidMode RaidZonedBlockDevice::getMainMode() const { return main_mode_; }
void RaidZonedBlockDevice::flush_zone_info() {
  // TODO
  // std::vector<std::unique_ptr<ZoneList>> dev_zone_list(nr_dev());
  // for (idx_t idx = 0; idx < nr_dev(); idx++) {
  //   auto z = devices_[idx]->ListZones();
  //   dev_zone_list[idx] = std::move(z);
  // }
  for (idx_t idx = 0; idx < nr_zones_; idx++) {
    auto mode_item = mode_map_[idx];
    std::vector<RaidMapItem> map_items(nr_dev());
    for (idx_t i = 0; i < nr_dev(); i++)
      map_items[i] =
          getAutoDeviceZone(idx * zone_sz_ + i * def_dev()->GetZoneSize());
    auto map_item = map_items.front();
    auto zone_list = devices_[map_item.device_idx]->ListZones();
    auto zone_list_ptr = reinterpret_cast<raid_zone_t *>(zone_list->GetData());
    auto p = a_zones_.get();
    p[idx].start = idx * zone_sz_;
    if (mode_item.mode == RaidMode::RAID_NONE ||
        mode_item.mode == RaidMode::RAID0 ||
        mode_item.mode == RaidMode::RAID_C) {
      uint64_t wp = std::accumulate(
          map_items.begin(), map_items.end(), static_cast<uint64_t>(0),
          [&](uint64_t sum, auto &item) {
            auto z = devices_[item.device_idx]->ListZones();
            auto s = devices_[item.device_idx]->ZoneStart(z, item.zone_idx);
            auto w = devices_[item.device_idx]->ZoneWp(z, item.zone_idx);
            // printf("\tdev[%x][%x] st=%lx, wp=%lx\n", item.device_idx,
            //        item.zone_idx, s, w);
            return sum + (w - s);
          });
      wp += p[idx].start;
      // printf("[%x] total wp=%lx\n", idx, wp);
      p[idx].wp = wp;
    } else if (mode_item.mode == RaidMode::RAID1) {
      p[idx].wp =
          devices_[map_item.device_idx]->ZoneWp(zone_list, map_item.zone_idx);
    }
    // FIXME: ZoneFS
    p[idx].flags = zone_list_ptr->flags;
    p[idx].type = zone_list_ptr->type;
    p[idx].cond = zone_list_ptr->cond;
    memcpy(p[idx].reserved, zone_list_ptr->reserved, sizeof(p[idx].reserved));
    p[idx].capacity = devices_[map_item.device_idx]->ZoneMaxCapacity(
                          zone_list, map_item.zone_idx) *
                      nr_dev();
    p[idx].len = p[idx].capacity;
  }
}
template <class T>
T RaidZonedBlockDevice::getAutoMappedDevicePos(T pos) {
  auto raid_zone_idx = pos / zone_sz_;
  RaidMapItem map_item = getAutoDeviceZone(pos);
  auto mode_item = mode_map_[raid_zone_idx];
  auto blk_idx = pos / block_sz_;
  if (mode_item.mode == RaidMode::RAID0) {
    auto base = map_item.zone_idx * def_dev()->GetZoneSize();
    auto nr_blk_in_raid_zone = zone_sz_ / block_sz_;
    auto blk_idx_raid_zone = blk_idx % nr_blk_in_raid_zone;
    auto blk_idx_dev_zone = blk_idx_raid_zone / nr_dev();
    auto offset_in_blk = pos % block_sz_;
    auto offset_in_zone = blk_idx_dev_zone * block_sz_;
    return base + offset_in_zone + offset_in_blk;
  } else {
    return map_item.zone_idx * def_dev()->GetZoneSize() +
           ((blk_idx % (def_dev()->GetZoneSize() / block_sz_)) * block_sz_) +
           pos % block_sz_;
  }
}
template <class T>
RaidMapItem RaidZonedBlockDevice::getAutoDeviceZone(T pos) {
  return device_zone_map_[getAutoDeviceZoneIdx(pos)];
}
template <class T>
idx_t RaidZonedBlockDevice::getAutoDeviceZoneIdx(T pos) {
  auto raid_zone_idx = pos / zone_sz_;
  auto raid_zone_inner_idx =
      (pos - (raid_zone_idx * zone_sz_)) / def_dev()->GetZoneSize();
  auto raid_block_idx = pos / block_sz_;
  // index of block in this raid zone
  auto raid_zone_block_idx =
      raid_block_idx - (raid_zone_idx * (zone_sz_ / block_sz_));
  auto mode_item = mode_map_[raid_zone_idx];
  if (mode_item.mode == RaidMode::RAID_NONE ||
      mode_item.mode == RaidMode::RAID_C || mode_item.mode == RaidMode::RAID1) {
    return raid_zone_idx * nr_dev() + raid_zone_inner_idx;
  } else if (mode_item.mode == RaidMode::RAID0) {
    // Info(logger_, "\t[pos=%x] raid_zone_idx=%lx raid_zone_block_idx = %lx",
    //      static_cast<uint32_t>(pos), raid_zone_idx, raid_zone_block_idx);
    return raid_zone_idx * nr_dev() + raid_zone_block_idx % nr_dev();
  }
  Warn(logger_, "Cannot locate device zone at pos=%x",
       static_cast<uint32_t>(pos));
  return {};
}
Status RaidMapItem::DecodeFrom(Slice *input) {
  GetFixed32(input, &device_idx);
  GetFixed32(input, &zone_idx);
  GetFixed16(input, &invalid);
  return Status::OK();
}
Status RaidModeItem::DecodeFrom(Slice *input) {
  GetFixed32(input, reinterpret_cast<uint32_t *>(&mode));
  GetFixed32(input, &option);
  return Status::OK();
}
}  // namespace AQUAFS_NAMESPACE