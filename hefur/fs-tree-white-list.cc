#include <fstream>
#include <fnmatch.h>

#include <mimosa/fs/find.hh>

#include "fs-tree-white-list.hh"
#include "hefur.hh"
#include "options.hh"
#include "log.hh"

#define D8ANNOUNCE "d8:announce"
#define D13ANNOUNCELIST "d13:announce-list"

#ifndef FNM_CASEFOLD
# define FNM_CASEFOLD 0
#endif

#ifndef FNM_IGNORECASE
# define FNM_IGNORECASE 0
#endif

namespace hefur
{
  FsTreeWhiteList::FsTreeWhiteList(const std::string & root,
                                   m::Time             rescan_interval)
    : root_(root),
      rescan_interval_(rescan_interval),
      stop_(),
      scan_thread_([this] { this->loopScan(); })
  {
    scan_thread_.start();
  }

  FsTreeWhiteList::~FsTreeWhiteList()
  {
    stop_.set(true);
    scan_thread_.join();
  }

  void
  FsTreeWhiteList::scan()
  {
    uint32_t nb_inodes = 0;
    m::fs::find(root_, MAX_SCAN_DEPTH, [&] (const std::string & path) {
        if (++nb_inodes > MAX_SCAN_INODES) {
          log->error("reached the limit of scanned inodes: %d", MAX_SCAN_INODES);
          return false;
        }

        std::ifstream file(path.c_str(), std::ios::in);
        if (!file.is_open()) {
          log->error("could not open file %s: %s", path, strerror(errno));
          return true;
        }

        char file_start[17] = {0};
        try {
          file.get(file_start, sizeof (file_start));
        }
        catch (std::ifstream::failure e) {
          log->error("could not read file %s: ", path, strerror(errno));
          return true;
        }

        if (!::strncmp(file_start, D8ANNOUNCE, sizeof (D8ANNOUNCE))
         || !::strncmp(file_start, D13ANNOUNCELIST, sizeof (D13ANNOUNCELIST)))
        {
          log->error("file %s is not a torrent file", path);
          return true;
        }

        auto tdb = Hefur::instance().torrentDb();
        if (tdb)
          tdb->addTorrent(Torrent::parseFile(path));
        return true;
      });
  }

  void
  FsTreeWhiteList::check()
  {
    std::vector<m::StringRef> keys;
    auto db = Hefur::instance().torrentDb();
    m::SharedMutex::Locker locker(db->torrents_lock_);
    db->torrents_.foreach([this, &keys] (Torrent::Ptr torrent) {
        if (::strncmp(torrent->path().c_str(), root_.c_str(), root_.size()))
          return;

        struct ::stat st;
        if (::stat(torrent->path().c_str(), &st) && errno == ENOENT)
          keys.push_back(torrent->key());
      });

    for (auto it = keys.begin(); it != keys.end(); ++it)
      db->torrents_.erase(*it);
  }

  void
  FsTreeWhiteList::loopScan()
  {
    do {
      scan();
      check();
    } while (!stop_.timedWait(m::time() + rescan_interval_));
  }
}
