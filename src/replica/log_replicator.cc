//
// log_appender.cc
// Copyright (C) 2017 4paradigm.com
// Author wangtaize 
// Date 2017-06-07
//

#include "replica/log_replicator.h"

#include <stdlib.h>
#include <stdio.h>
#include <cstring>
#include "leveldb/options.h"
#include "logging.h"
#include "base/strings.h"
#include "base/file_util.h"
#include <gflags/gflags.h>

DECLARE_int32(binlog_single_file_max_size);

namespace rtidb {
namespace replica {

using ::baidu::common::INFO;
using ::baidu::common::WARNING;
using ::baidu::common::DEBUG;

const static StringComparator scmp;
const static std::string LOG_META_PREFIX="/logs/";

LogReplicator::LogReplicator(const std::string& path,
                             const std::vector<std::string>& endpoints,
                             const ReplicatorRole& role):path_(path), meta_path_(), log_path_(),
    meta_(NULL), term_(0), log_offset_(0), logs_(NULL), wh_(NULL), wsize_(0),
    role_(role), last_log_term_(0), last_log_offset_(0),
    endpoints_(endpoints), nodes_(), mu_(), cv_(&mu_),rpc_client_(NULL),
    sf_(NULL), lr_(NULL),apply_log_offset_(0),running_(true), tp_(1){}

LogReplicator::LogReplicator(const std::string& path,
                             ApplyLogFunc func,
                             const ReplicatorRole& role):path_(path), meta_path_(), log_path_(),
    meta_(NULL), term_(0), log_offset_(0), logs_(NULL), wh_(NULL), wsize_(0),
    role_(role), last_log_term_(0), last_log_offset_(0),
    endpoints_(), nodes_(), mu_(), cv_(&mu_),rpc_client_(NULL),
    sf_(NULL), lr_(NULL),apply_log_offset_(0),
    func_(func),running_(true), tp_(1){}

LogReplicator::~LogReplicator() {}

bool LogReplicator::Init() {
    leveldb::Options options;
    options.create_if_missing = true;
    meta_path_ = path_ + "/meta/";
    bool ok = ::rtidb::base::MkdirRecur(meta_path_);
    if (!ok) {
        LOG(WARNING, "fail to create db meta path %s", meta_path_.c_str());
        return false;
    }
    log_path_ = path_ + "/logs/";
    ok = ::rtidb::base::MkdirRecur(log_path_);
    if (!ok) {
        LOG(WARNING, "fail to log dir %s", log_path_.c_str());
        return false;
    }
    leveldb::Status status = leveldb::DB::Open(options,
                                               meta_path_, 
                                               &meta_);
    if (!status.ok()) {
        LOG(WARNING, "fail to create meta db for %s", status.ToString().c_str());
        return false;
    }

    ok = Recover();
    if (!ok) {
        return false;
    }
    if (role_ == kLeaderNode) {
        std::vector<std::string>::iterator it = endpoints_.begin();
        uint32_t index = 0;
        for (; it != endpoints_.end(); ++it) {
            ReplicaNode* node = new ReplicaNode();
            node->endpoint = *it;
            index ++;
            nodes_.push_back(node);
            LOG(INFO, "add replica node with endpoint %s", node->endpoint.c_str());
        }
        tp_.AddTask(boost::bind(&LogReplicator::ReplicateLog, this));
        LOG(INFO, "init leader node for path %s ok", path_.c_str());
    }else {
        tp_.AddTask(boost::bind(&LogReplicator::ApplyLog, this));
        LOG(INFO, "init follower node for path %s ok", path_.c_str());
    }
    rpc_client_ = new ::rtidb::RpcClient();
    return ok;
}

bool LogReplicator::AppendEntries(const ::rtidb::api::AppendEntriesRequest* request) {
    if (wh_ == NULL || (wsize_ / (1024* 1024)) > (uint32_t)FLAGS_binlog_single_file_max_size) {
        bool ok = RollWLogFile();
        if (!ok) {
            LOG(WARNING, "fail to roll write log for path %s", path_.c_str());
            return false;
        }
    }
    if (request->pre_log_index() !=  last_log_offset_
        || request->pre_log_term() != last_log_term_) {
        LOG(WARNING, "log mismatch for path %s, pre_log_index %lld, come log index %lld", path_.c_str(),
                last_log_offset_, request->pre_log_index());
        return false;
    }
    for (int32_t i = 0; i < request->entries_size(); i++) {
        std::string buffer;
        request->entries(i).SerializeToString(&buffer);
        ::rtidb::base::Slice slice(buffer.c_str(), buffer.size());
        ::rtidb::base::Status status = wh_->Write(slice);
        if (!status.ok()) {
            LOG(WARNING, "fail to write replication log in dir %s for %s", path_.c_str(), status.ToString().c_str());
            return false;
        }
        last_log_term_ = request->term();
        last_log_offset_ = request->entries(i).log_index();
    }
    LOG(DEBUG, "sync log entry to offset %lld for %s", last_log_offset_, path_.c_str());
    return true;
}

bool LogReplicator::AppendEntry(::rtidb::api::LogEntry& entry) {
    if (wh_ == NULL || wsize_ > (uint32_t)FLAGS_binlog_single_file_max_size) {
        bool ok = RollWLogFile();
        if (!ok) {
            return false;
        }
    }
    entry.set_log_index(log_offset_.fetch_add(1, boost::memory_order_relaxed));
    std::string buffer;
    entry.SerializeToString(&buffer);
    ::rtidb::base::Slice slice(buffer.c_str(), buffer.size());
    ::rtidb::base::Status status = wh_->Write(slice);
    if (!status.ok()) {
        LOG(WARNING, "fail to write replication log in dir %s for %s", path_.c_str(), status.ToString().c_str());
        return false;
    }
    return true;
}

bool LogReplicator::RollRLogFile(SequentialFile** sf,
        Reader** lr, uint64_t offset) {
    if (sf == NULL || lr == NULL) {
        LOG(WARNING, "invalid input sf and lr which is null");
        return false;
    }
    mu_.AssertHeld();
    LogParts::Iterator* it = logs_->NewIterator();
    it->SeekToFirst();
    while (it->Valid()) {
        LogPart* part = it->GetValue();
        LOG(DEBUG, "log with name %s and start offset %lld", part->log_name_.c_str(),
                part->slog_id_);
        if (part->slog_id_ < offset) {
            it->Next();
            continue;
        }
        delete it;
        std::string full_path = log_path_ + "/" + part->log_name_;
        FILE* fd = fopen(full_path.c_str(), "rb");
        if (fd == NULL) {
            LOG(WARNING, "fail to create file %s", full_path.c_str());
            return false;
        }
        if ((*sf) != NULL) {
            delete (*sf);
            delete (*lr);
        }
        LOG(INFO, "roll log file to %s for path %s", part->log_name_.c_str(),
                path_.c_str());
        *sf = ::rtidb::log::NewSeqFile(full_path, fd);
        *lr = new Reader(*sf, NULL, false, 0);
        return true;
    }
    delete it;
    LOG(WARNING, "fail to find log include offset %lld", offset);
    return false;
}

bool LogReplicator::RollWLogFile() {
    if (wh_ != NULL) {
        delete wh_;
    }
    std::string name = ::rtidb::base::FormatToString(logs_->GetSize(), 8) + ".log";
    std::string full_path = log_path_ + "/" + name;
    FILE* fd = fopen(full_path.c_str(), "ab+");
    if (fd == NULL) {
        LOG(WARNING, "fail to create file %s", full_path.c_str());
        return false;
    }

    uint64_t offset = log_offset_.load(boost::memory_order_relaxed);
    LogPart* part = new LogPart(offset, name);
    std::string buffer;
    buffer.resize(8 + 4 + name.size() + 1);
    char* cbuffer = reinterpret_cast<char*>(&(buffer[0]));
    memcpy(cbuffer, static_cast<const void*>(&part->slog_id_), 8);
    cbuffer += 8;
    uint32_t name_len = name.size() + 1;
    memcpy(cbuffer, static_cast<const void*>(&name_len), 4);
    cbuffer += 4;
    memcpy(cbuffer, static_cast<const void*>(name.c_str()), name_len);
    std::string key = LOG_META_PREFIX + name;
    leveldb::WriteOptions options;
    options.sync = true;
    const leveldb::Slice value(buffer);
    leveldb::Status status = meta_->Put(options, key, value);
    if (!status.ok()) {
        LOG(WARNING, "fail to save meta for %s", name.c_str());
        delete part;
        return false;
    }
    logs_->Insert(name, part);
    LOG(INFO, "roll write log for name %s and start offset %lld", name.c_str(), part->slog_id_);
    wh_ = new WriteHandle(name, fd);
    wsize_ = 0;
    return true;
}

bool LogReplicator::Recover() {
    logs_ = new LogParts(12, 4, scmp);
    ::leveldb::ReadOptions options;
    leveldb::Iterator* it = meta_->NewIterator(options);
    it->Seek(LOG_META_PREFIX);
    std::string log_meta_end = LOG_META_PREFIX + "~";
    while(it->Valid()) {
        if (it->key().compare(log_meta_end) >= 0) {
            break;
        }
        leveldb::Slice data = it->value();
        if (data.size() < 12) {
            LOG(WARNING, "bad data formate");
            assert(0);
        }
        LogPart* log_part = new LogPart();
        const char* buffer = data.data();
        memcpy(static_cast<void*>(&log_part->slog_id_), buffer, 8);
        buffer += 8;
        uint32_t name_size = 0;
        memcpy(static_cast<void*>(&name_size), buffer, 4);
        buffer += 4;
        char name_con[name_size];
        memcpy(reinterpret_cast<char*>(&name_con), buffer, name_size);
        std::string name_str(name_con);
        log_part->log_name_ = name_str;
        LOG(INFO, "recover log part with slog_id %lld name %s",
                log_part->slog_id_, log_part->log_name_.c_str());
        logs_->Insert(name_str, log_part);
        it->Next();
    }
    delete it;
    //TODO(wangtaize) recover term and log offset from log part
    return true;
}

void LogReplicator::Notify() {
    MutexLock lock(&mu_);
    cv_.Signal();
}

void LogReplicator::ReplicateLog() {
    while (running_.load(boost::memory_order_relaxed)) {
        MutexLock lock(&mu_);
        cv_.TimeWait(10000);
        //TODO (wangtaize) one thread per replicate node
        std::vector<ReplicaNode*>::iterator it = nodes_.begin();
        for (; it != nodes_.end(); ++it) {
            ReplicateToNode(*it);
        }
    }
    LOG(INFO, "replicate log exits for path %s", path_.c_str());
}

bool LogReplicator::ReadNextRecord(Reader** lr, 
                                   SequentialFile** sf,
                                   ::rtidb::base::Slice* record,
                                   std::string* buffer,
                                   uint64_t offset) {
    mu_.AssertHeld();
    // for the first run
    if (lr_ == NULL 
        || sf_ == NULL) {
        bool ok = RollRLogFile(&sf_, &lr_, offset);
        if (!ok) {
            LOG(WARNING, "fail to roll read log for path %s", path_.c_str());
            return false;
        }
     }
     bool ok = lr_->ReadRecord(record, buffer);
     if (!ok) {
         LOG(DEBUG, "reach the end of file for path %s", path_.c_str());
         // reache the end of file;
         ok = RollRLogFile(&sf_, &lr_, offset);
         if (!ok) {
             LOG(WARNING, "fail to roll read log for path %s", path_.c_str());
             return false;
         }
         ok = lr_->ReadRecord(record, buffer);
         if (!ok) {
             LOG(WARNING, "fail to read log for path %s", path_.c_str());
             return false;
         }
     }
     return true;
}

void LogReplicator::ApplyLog() {
    while(running_.load(boost::memory_order_relaxed)) {
        MutexLock lock(&mu_);
        cv_.TimeWait(10000);
        if (apply_log_offset_ >= log_offset_.load(boost::memory_order_relaxed)) {
            continue;
        }
        std::string buffer;
        ::rtidb::base::Slice record;
        bool ok = ReadNextRecord(&lr_, &sf_, &record, &buffer, apply_log_offset_);
        if (!ok) {
            LOG(WARNING, "fail to read next record for path %s", path_.c_str());
            // forever retry
            continue;
        }
        ::rtidb::api::LogEntry entry;
        entry.ParseFromString(record.ToString());
        ok = func_(entry);
        if (ok) {
            LOG(DEBUG, "apply log with path %s ok to offset %lld", path_.c_str(),
                    entry.log_index());
            apply_log_offset_ = entry.log_index();
        }else {
            //TODO cache log entry and reapply 
            LOG(WARNING, "fail to apply log with path %s to offset %lld", path_.c_str(),
                    entry.log_index());
        }
    }
}

void LogReplicator::ReplicateToNode(ReplicaNode* node) {
    mu_.AssertHeld();
    LOG(DEBUG, "node offset %lld, log offset %lld", node->last_sync_offset, log_offset_.load(boost::memory_order_relaxed));
    if (node->last_sync_offset >= (log_offset_.load(boost::memory_order_relaxed) - 1)) {
        return; 
    }
    ::rtidb::api::TabletServer_Stub* stub;
    bool ok = rpc_client_->GetStub(node->endpoint, &stub);
    if (!ok) {
        LOG(WARNING, "fail to get rpc stub with endpoint %s", node->endpoint.c_str());
        return;
    }
    std::string buffer;
    ::rtidb::base::Slice record;
    ok = ReadNextRecord(&node->lr, 
            &node->sf, &record, 
            &buffer, node->last_sync_offset);
    if (!ok) {
        LOG(WARNING, "fail to read next record for path %s", path_.c_str());
        // forever retry
        return;
    }
    ::rtidb::api::AppendEntriesRequest request;
    ::rtidb::api::AppendEntriesResponse response;
    request.set_pre_log_index(node->last_sync_offset);
    request.set_pre_log_term(node->last_sync_term);
    ::rtidb::api::LogEntry* entry = request.add_entries();
    entry->ParseFromString(record.ToString());
    LOG(DEBUG, "entry log index %lld, pk %s , val %s, ts %lld", entry->log_index(), entry->pk().c_str(), entry->value().c_str(),
            entry->ts());
    ok = rpc_client_->SendRequest(stub, 
                                 &::rtidb::api::TabletServer_Stub::AppendEntries,
                                 &request, &response, 12, 1);
    if (ok && response.code() == 0) {
        LOG(DEBUG, "sync log to node %s to offset %lld",
                node->endpoint.c_str(), entry->log_index());
        node->last_sync_offset = entry->log_index();
        node->last_sync_term = entry->term();
    }else {
        node->cache.push_back(request);
        LOG(WARNING, "fail to sync log to node %s", node->endpoint.c_str());
    }
}

void LogReplicator::Stop() {
    running_.store(false, boost::memory_order_relaxed);
    tp_.Stop(1000);
    LOG(INFO, "stop replicator for path %s ok", path_.c_str());
}

} // end of replica
} // end of ritdb
