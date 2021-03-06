// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <fts.h>
#include <sys/types.h>

#include <string>

#include <mesos/resources.hpp>

#include <process/dispatch.hpp>
#include <process/id.hpp>

#include <stout/os/exists.hpp>
#include <stout/os/su.hpp>

#include "common/values.hpp"

#ifdef __linux__
#include "linux/fs.hpp"
#endif // __linux__

#include "slave/paths.hpp"
#include "slave/state.hpp"

#include "slave/volume_gid_manager/volume_gid_manager.hpp"

using std::string;
using std::vector;

using process::dispatch;
using process::Failure;
using process::Future;
using process::Owned;

using mesos::internal::values::intervalSetToRanges;
using mesos::internal::values::rangesToIntervalSet;

namespace mesos {
namespace internal {
namespace slave {

// Recursively change the owner group of the given path
// to the given gid and set/unset the `setgid` bit.
static Try<Nothing> setVolumeOwnership(
    const string& path,
    gid_t gid,
    bool setgid)
{
  LOG(INFO) << "Start setting the owner group of the volume path '"
            << path << "' " << (setgid ? "" : "back ") << "to " << gid;

  char* path_[] = {const_cast<char*>(path.c_str()), nullptr};

  FTS* tree = ::fts_open(path_, FTS_NOCHDIR | FTS_PHYSICAL, nullptr);
  if (tree == nullptr) {
    return ErrnoError("Failed to open '" + path + "'");
  }

  FTSENT *node;
  while ((node = ::fts_read(tree)) != nullptr) {
    const Path path = Path(node->fts_path);

    switch (node->fts_info) {
      // Preorder directory.
      case FTS_D:
      // Regular file.
      case FTS_F:
      // Symbolic link.
      case FTS_SL: {
        CHECK_NOTNULL(node->fts_statp);

        // Change the owner group to the given gid.
        if (::lchown(node->fts_path, node->fts_statp->st_uid, gid) < 0) {
          Error error = ErrnoError();
          ::fts_close(tree);
          return Error(
              "Chown failed on '" + path.string() + "': " + error.message);
        }

        if (node->fts_info == FTS_D) {
          // Set the `setgid` bit for directories and add the write
          // permission for the owner group.
          if (setgid) {
            if (::chmod(
                node->fts_path,
                node->fts_statp->st_mode | S_ISGID | S_IWGRP)) {
              Error error = ErrnoError();
              ::fts_close(tree);
              return Error(
                  "Chmod failed on '" + path.string() + "': " + error.message);
            }
          } else {
            // Unset the `setgid` bit for directories and remove the write
            // permission for the owner group.
            if (::chmod(
                node->fts_path,
                node->fts_statp->st_mode & ~S_ISGID & ~S_IWGRP)) {
              Error error = ErrnoError();
              ::fts_close(tree);
              return Error(
                  "Chmod failed on '" + path.string() + "': " + error.message);
            }
          }
        }

        break;
      }

      // Unreadable directory.
      case FTS_DNR:
      // Error; errno is set.
      case FTS_ERR:
      // `stat(2)` failed.
      case FTS_NS: {
        Error error = ErrnoError(node->fts_errno);
        ::fts_close(tree);
        return Error(
            "Failed to read '" + path.string() + "': " + error.message);
      }

      default:
        break;
    }
  }

  if (errno != 0) {
    Error error = ErrnoError();
    ::fts_close(tree);
    return error;
  }

  if (::fts_close(tree) != 0) {
    return ErrnoError("Failed to stop traversing file system");
  }

  LOG(INFO) << "Finished setting the owner group of the volume path '"
            << path << "' " << (setgid ? "" : "back ") << "to " << gid;

  return Nothing();
}


class VolumeGidManagerProcess : public process::Process<VolumeGidManagerProcess>
{
public:
  VolumeGidManagerProcess(
      const IntervalSet<gid_t>& gids,
      const string& workDir)
    : ProcessBase(process::ID::generate("volume-gid-manager")),
      totalGids(gids),
      freeGids(gids),
      metaDir(paths::getMetaRootDir(workDir)) {}

  Future<Nothing> recover(bool rebooted)
  {
    LOG(INFO) << "Recovering volume gid manager";

    const string volumeGidsPath = paths::getVolumeGidsPath(metaDir);
    if (os::exists(volumeGidsPath)) {
      Result<VolumeGidInfos> volumeGidInfos =
        state::read<VolumeGidInfos>(volumeGidsPath);

      if (volumeGidInfos.isError()) {
        return Failure(
            "Failed to read volume gid infos from '" + volumeGidsPath +
            "' " + volumeGidInfos.error());
      } else if (volumeGidInfos.isNone()) {
        // This could happen if the agent is hard rebooted after the file is
        // created but before the data is synced on disk.
        LOG(WARNING) << "The volume gids file '"
                     << volumeGidsPath << "' is empty";
      } else {
        CHECK_SOME(volumeGidInfos);

        hashset<string> orphans;
        foreach (const VolumeGidInfo& info, volumeGidInfos->infos()) {
          freeGids -= info.gid();
          infos.put(info.path(), info);

          // Normally the gid allocated to the PARENT type SANDBOX_PATH
          // volume is deallocated when the parent container is destroyed,
          // However after agent reboot, containerizer will not destroy any
          // containers since all containers are already gone, so to avoid
          // gid leak in this case, we need to deallocate gid for the PARENT
          // type SANDBOX_PATH volume here.
          if (rebooted && info.type() == VolumeGidInfo::SANDBOX_PATH) {
            LOG(INFO) << "Deallocating gid " << info.gid() << " for the PARENT"
                      << "type SANDBOX_PATH volume '" << info.path()
                      << "' after agent reboot";

            orphans.insert(Path(info.path()).dirname());
            continue;
          }

          // This could happen in the case that agent crashes after the
          // shared persistent volume is deleted but before volume gid
          // manager deallocates its gid.
          if (!os::exists(info.path())) {
            LOG(WARNING) << "Deallocating gid " << info.gid() << " for the "
                         << "non-existent volume path '" << info.path() << "'";

            orphans.insert(info.path());
          }
        }

        // Deallocate all the orphaned paths.
        foreach (const string& path, orphans) {
          deallocate(path);
        }
      }
    }

    return Nothing();
  }

  // This method will be called when a container running as non-root user tries
  // to use a shared persistent volume or a PARENT type SANDBOX_PATH volume, the
  // parameter `path` will be the source path of the volume.
  Future<gid_t> allocate(const string& path, VolumeGidInfo::Type type)
  {
    gid_t gid;

    // If a gid has already been allocated for the specified path,
    // just return the gid.
    if (infos.contains(path)) {
      gid = infos[path].gid();

      LOG(INFO) << "Use the allocated gid " << gid << " of the volume path '"
                << path << "'";
    } else {
      Option<string> realPath;
#ifdef __linux__
      // This is to handle the case that nested containers use shared persistent
      // volume, in which case we did a workaround in the default executor to
      // set up a volume mapping (i.e., map the shared persistent volume to a
      // SANDBOX_PATH volume of PARENT type for nested containers) so that the
      // nested container can access the shared persistent volume. So here we
      // need to search the mount table to see if the `path` is the target of a
      // mount, and check if we have already allocated a gid to the source of
      // that mount, if yes, then we just return that gid instead of allocating
      // a new one, otherwise the ownership of the shared persistent volume will
      // be wrongly overwritten.
      Try<fs::MountInfoTable> table = fs::MountInfoTable::read();
      if (table.isError()) {
        return Failure("Failed to get mount table: " + table.error());
      }

      foreach (const fs::MountInfoTable::Entry& entry, table->entries) {
        if (path == entry.target) {
          realPath = entry.root;
          break;
        }
      }
#endif // __linux__

      if (realPath.isSome() && infos.contains(realPath.get())) {
        gid = infos[realPath.get()].gid();

        LOG(INFO) << "Use the allocated gid " << gid << " of the volume path '"
                  << realPath.get() << "' which is mounted at the volume path '"
                  << path << "'";
      } else {
        // Allocate a free gid to the specified path and then set the
        // ownership for it.
        if (freeGids.empty()) {
          return Failure(
              "Failed to allocate gid to the volume path '" + path +
              "' because the free gid range is exhausted");
        }

        gid = freeGids.begin()->lower();

        LOG(INFO) << "Allocating gid " << gid << " to the volume path '"
                  << path << "'";

        freeGids -= gid;

        VolumeGidInfo info;
        info.set_type(type);
        info.set_path(path);
        info.set_gid(gid);

        infos.put(path, info);

        Try<Nothing> status = persist();
        if (status.isError()) {
          return Failure(
              "Failed to save state of volume gid infos: " + status.error());
        }

        Try<Nothing> result = setVolumeOwnership(path, gid, true);
        if (result.isError()) {
          return Failure(
              "Failed to set the owner group of the volume path '" + path +
              "' to " + stringify(gid) + ": " + result.error());
        }
      }
    }

    return gid;
  }

  // This method will be called in two cases:
  //   1. When a shared persistent volume is destroyed by agent, the parameter
  //      `path` will be the shared persistent volume's path.
  //   2. When a container is destroyed by containerizer, the parameter `path`
  //      will be the container's sandbox path.
  // We search if the given path is contained in `infos` (for the case 1) or is
  // the parent directory of any volume paths in `infos` (for the case 2, i.e.,
  // the PARENT type SANDBOX_PATH volume must be a subdirectory in the parent
  // container's sandbox) and then free the allocated gid for the found path(s).
  Future<Nothing> deallocate(const string& path)
  {
    vector<string> sandboxPathVolumes;

    bool changed = false;
    for (auto it = infos.begin(); it != infos.end(); ) {
      const VolumeGidInfo& info = it->second;
      const string& volumePath = info.path();

      if (strings::startsWith(volumePath, path)) {
        if (volumePath != path) {
          // This is the case of the PARENT type SANDBOX_PATH volume.
          sandboxPathVolumes.push_back(volumePath);
        }

        gid_t gid = info.gid();

        LOG(INFO) << "Deallocated gid " << gid << " for the volume path '"
                  << volumePath << "'";

        // Only return the gid to the free range if it is in the total
        // range. The gid may not be in the total range in the case that
        // Mesos agent is restarted with a different total range and we
        // deallocate gid for a previous volume path from the old range.
        if (totalGids.contains(gid)) {
          freeGids += gid;
        }

        it = infos.erase(it);
        changed = true;
      } else {
        ++it;
      }
    }

    // For the PARENT type SANDBOX_PATH volume, it will exist for a while
    // (depending on GC policy) after the container is destroyed. So to
    // avoid leaking it to other containers in the case that its gid is
    // allocated to another volume, we need to change its owner group back
    // to the original one (i.e., the primary group of its owner).
    foreach (const string& volume, sandboxPathVolumes) {
      // Get the uid of the volume's owner.
      struct stat s;
      if (::stat(volume.c_str(), &s) < 0) {
        LOG(WARNING) << "Failed to stat '" << volume << "': "
                     << os::strerror(errno);

        continue;
      }

      Result<string> user = os::user(s.st_uid);
      if (!user.isSome()) {
        LOG(WARNING) << "Failed to get username for the uid " << s.st_uid
                     << ": " << (user.isError() ? user.error() : "not found");

        continue;
      }

      // Get the primary group ID of the user.
      Result<gid_t> gid = os::getgid(user.get());
      if (!gid.isSome()) {
        LOG(WARNING) << "Failed to get gid for the user '" << user.get()
                     << "': " << (gid.isError() ? gid.error() : "not found");

        continue;
      }

      Try<Nothing> result = setVolumeOwnership(volume, gid.get(), false);
      if (result.isError()) {
        LOG(WARNING) << "Failed to set the owner group of the volume path '"
                     << volume << "' back to " << gid.get() << ": "
                     << result.error();
      }
    }

    if (changed) {
      Try<Nothing> status = persist();
      if (status.isError()) {
        return Failure(
            "Failed to save state of volume gid infos: " + status.error());
      }
    }

    return Nothing();
  }

private:
  Try<Nothing> persist()
  {
    VolumeGidInfos volumeGidInfos;
    foreachvalue (const VolumeGidInfo& info, infos) {
      volumeGidInfos.add_infos()->CopyFrom(info);
    }

    Try<Nothing> status = state::checkpoint(
        paths::getVolumeGidsPath(metaDir), volumeGidInfos);

    if (status.isError()) {
      return Error("Failed to perform checkpoint: " + status.error());
    }

    return Nothing();
  }

  const IntervalSet<gid_t> totalGids;
  IntervalSet<gid_t> freeGids;

  const string metaDir;

  // Allocated gid infos keyed by the volume path.
  hashmap<string, VolumeGidInfo> infos;
};


Try<VolumeGidManager*> VolumeGidManager::create(const Flags& flags)
{
  if (geteuid() != 0) {
    return Error("Volume gid manager requires root privileges");
  }

  CHECK_SOME(flags.volume_gid_range);

  Try<Resource> parse =
    Resources::parse("gids", flags.volume_gid_range.get(), "*");

  if (parse.isError()) {
    return Error(
        "Failed to parse volume gid range '" +
        flags.volume_gid_range.get() + "'");
  }

  if (parse->type() != Value::RANGES) {
    return Error(
        "Invalid volume gid range type " +
        mesos::Value_Type_Name(parse->type()) +
        ", expecting " +
        mesos::Value_Type_Name(Value::RANGES));
  }

  Try<IntervalSet<gid_t>> gids =
    rangesToIntervalSet<gid_t>(parse->ranges());

  if (gids.isError()) {
    return Error("Invalid volume gid range '" +
        stringify(parse->ranges()) + "': " + gids.error());
  } else if (gids->empty()) {
    return Error("Empty volume gid range");
  }

  return new VolumeGidManager(Owned<VolumeGidManagerProcess>(
      new VolumeGidManagerProcess(gids.get(), flags.work_dir)));
}


VolumeGidManager::VolumeGidManager(
    const Owned<VolumeGidManagerProcess>& _process)
  : process(_process)
{
  spawn(process.get());
}


VolumeGidManager::~VolumeGidManager()
{
  terminate(process.get());
  process::wait(process.get());
}


Future<Nothing> VolumeGidManager::recover(bool rebooted) const
{
  return dispatch(process.get(), &VolumeGidManagerProcess::recover, rebooted);
}


Future<gid_t> VolumeGidManager::allocate(
    const string& path,
    VolumeGidInfo::Type type) const
{
  return dispatch(process.get(),
                  &VolumeGidManagerProcess::allocate,
                  path,
                  type);
}


Future<Nothing> VolumeGidManager::deallocate(const string& path) const
{
  return dispatch(process.get(), &VolumeGidManagerProcess::deallocate, path);
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
