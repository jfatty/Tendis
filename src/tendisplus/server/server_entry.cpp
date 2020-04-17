#include <utility>
#include <memory>
#include <algorithm>
#include <chrono>
#include <string>
#include <list>
#include <mutex>
#include "glog/logging.h"
#include "tendisplus/server/server_entry.h"
#include "tendisplus/server/server_params.h"
#include "tendisplus/utils/redis_port.h"
#include "tendisplus/utils/invariant.h"
#include "tendisplus/utils/time.h"
#include "tendisplus/commands/command.h"
#include "tendisplus/storage/rocks/rocks_kvstore.h"
#include "tendisplus/utils/string.h"

namespace tendisplus {

ServerStat::ServerStat() {
    memset(&instMetric, 0, sizeof(instMetric));
}

void ServerStat::reset() {
    std::lock_guard<std::mutex> lk(_mutex);
    expiredkeys = 0;
    keyspaceHits = 0;
    keyspaceMisses = 0;
    keyspaceIncorrectEp = 0;
    rejectedConn = 0;
    syncFull = 0;
    syncPartialOk = 0;
    syncPartialErr = 0;
    netInputBytes = 0;
    netOutputBytes = 0;
    memset(&instMetric, 0, sizeof(instMetric));
}

CompactionStat::CompactionStat()
    : curDBid(""), startTime(sinceEpoch()), isRunning(false) {}

void CompactionStat::reset() {
    std::lock_guard<std::mutex> lk(_mutex);
    isRunning = false;
    curDBid = "";
}

/* Return the mean of all the samples. */
uint64_t ServerStat::getInstantaneousMetric(int metric) const {
    std::lock_guard<std::mutex> lk(_mutex);
    int j;
    uint64_t sum = 0;

    for (j = 0; j < STATS_METRIC_SAMPLES; j++)
        sum += instMetric[metric].samples[j];
    return sum / STATS_METRIC_SAMPLES;
}

/* Add a sample to the operations per second array of samples. */
void ServerStat::trackInstantaneousMetric(int metric, uint64_t current_reading) {
    std::lock_guard<std::mutex> lk(_mutex);
    uint64_t t = msSinceEpoch() - instMetric[metric].lastSampleTime;
    uint64_t ops = current_reading -
        instMetric[metric].lastSampleCount;
    uint64_t ops_sec;

    ops_sec = t > 0 ? (ops * 1000 / t) : 0;

    instMetric[metric].samples[instMetric[metric].idx] =
        ops_sec;
    instMetric[metric].idx++;
    instMetric[metric].idx %= STATS_METRIC_SAMPLES;
    instMetric[metric].lastSampleTime = msSinceEpoch();
    instMetric[metric].lastSampleCount = current_reading;
}

ServerEntry::ServerEntry()
        :_ftmcEnabled(false),
         _isRunning(false),
         _isStopped(true),
         _isShutdowned(false),
         _startupTime(nsSinceEpoch()),
         _network(nullptr),
         _segmentMgr(nullptr),
         _replMgr(nullptr),
         _indexMgr(nullptr),
         _pessimisticMgr(nullptr),
         _mgLockMgr(nullptr),
         _catalog(nullptr),
         _netMatrix(std::make_shared<NetworkMatrix>()),
         _poolMatrix(std::make_shared<PoolMatrix>()),
         _reqMatrix(std::make_shared<RequestMatrix>()),
         _cronThd(nullptr),
         _requirepass(""),
         _masterauth(""),
         _versionIncrease(true),
         _generalLog(false),
         _checkKeyTypeForSet(false),
         _protoMaxBulkLen(CONFIG_DEFAULT_PROTO_MAX_BULK_LEN),
         _dbNum(CONFIG_DEFAULT_DBNUM),
         _slowlogId(0),
         _scheduleNum(0),
         _cfg(nullptr),
         _lastBackupTime(0),
         _backupTimes(0),
         _lastBackupFailedTime(0),
         _backupFailedTimes(0),
         _lastBackupFailedErr("") {
}

ServerEntry::ServerEntry(const std::shared_ptr<ServerParams>& cfg)
    : ServerEntry() {
    _requirepass = cfg->requirepass;
    _masterauth = cfg->masterauth;
    _versionIncrease = cfg->versionIncrease;
    _generalLog = cfg->generalLog;
    _checkKeyTypeForSet = cfg->checkKeyTypeForSet;
    _protoMaxBulkLen = cfg->protoMaxBulkLen;
    _dbNum = cfg->dbNum;
    _cfg = cfg;
}

void ServerEntry::resetServerStat() {
    std::lock_guard<std::mutex> lk(_mutex);

    _poolMatrix->reset();
    _netMatrix->reset();
    _reqMatrix->reset();

    _serverStat.reset();
}

void ServerEntry::installPessimisticMgrInLock(
        std::unique_ptr<PessimisticMgr> o) {
    _pessimisticMgr = std::move(o);
}

void ServerEntry::installMGLockMgrInLock(
    std::unique_ptr<mgl::MGLockMgr> o) {
    _mgLockMgr = std::move(o);
}

void ServerEntry::installStoresInLock(const std::vector<PStore>& o) {
    // TODO(deyukong): assert mutex held
    _kvstores = o;
}

void ServerEntry::installSegMgrInLock(std::unique_ptr<SegmentMgr> o) {
    // TODO(deyukong): assert mutex held
    _segmentMgr = std::move(o);
}

void ServerEntry::installCatalog(std::unique_ptr<Catalog> o) {
    _catalog = std::move(o);
}

Catalog* ServerEntry::getCatalog() {
    return _catalog.get();
}

void ServerEntry::logGeneral(Session *sess) {
    if (!_generalLog) {
        return;
    }
    LOG(INFO) << sess->getCmdStr();
}

void ServerEntry::logWarning(const std::string& str, Session* sess) {
    std::stringstream ss;
    if (sess) {
        ss << sess->id() << "cmd:" << sess->getCmdStr();
    }

    ss << ", warning:" << str;

    LOG(WARNING) << ss.str();
}

void ServerEntry::logError(const std::string& str, Session* sess) {
    std::stringstream ss;
    if (sess) {
        ss << sess->id() << " cmd:" << sess->getCmdStr();
    }

    ss << ", error:" << str;

    LOG(ERROR) << ss.str();
}

uint32_t ServerEntry::getKVStoreCount() const {
    return _catalog->getKVStoreCount();
}
extern string gRenameCmdList;
extern string gMappingCmdList;
Status ServerEntry::startup(const std::shared_ptr<ServerParams>& cfg) {
    std::lock_guard<std::mutex> lk(_mutex);

    LOG(INFO) << "ServerEntry::startup,,,";

    uint32_t kvStoreCount = cfg->kvStoreCount;
    uint32_t chunkSize = cfg->chunkSize;

    // set command config
    Command::setNoExpire(cfg->noexpire);
    Command::changeCommand(gRenameCmdList, "rename");
    Command::changeCommand(gMappingCmdList, "mapping");

    // catalog init
    auto catalog = std::make_unique<Catalog>(
        std::move(std::unique_ptr<KVStore>(
            new RocksKVStore(CATALOG_NAME, cfg, nullptr, false,
                KVStore::StoreMode::READ_WRITE, RocksKVStore::TxnMode::TXN_PES))),
          kvStoreCount, chunkSize);
    installCatalog(std::move(catalog));

    // kvstore init
    auto blockCache =
        rocksdb::NewLRUCache(cfg->rocksBlockcacheMB * 1024 * 1024LL, 6, cfg->rocksStrictCapacityLimit);
    std::vector<PStore> tmpStores;
    tmpStores.reserve(kvStoreCount);
    for (size_t i = 0; i < kvStoreCount; ++i) {
        auto meta = _catalog->getStoreMainMeta(i);
        KVStore::StoreMode mode = KVStore::StoreMode::READ_WRITE;

        if (meta.ok()) {
            mode = meta.value()->storeMode;
        } else if (meta.status().code() == ErrorCodes::ERR_NOTFOUND) {
            auto pMeta = std::unique_ptr<StoreMainMeta>(
                    new StoreMainMeta(i, KVStore::StoreMode::READ_WRITE));
            Status s = _catalog->setStoreMainMeta(*pMeta);
            if (!s.ok()) {
                LOG(FATAL) << "catalog setStoreMainMeta error:"
                    << s.toString();
                return s;
            }
        } else {
            LOG(FATAL) << "catalog getStoreMainMeta error:"
                << meta.status().toString();
            return meta.status();
        }

        tmpStores.emplace_back(std::unique_ptr<KVStore>(
            new RocksKVStore(std::to_string(i), cfg, blockCache, true, mode,
                RocksKVStore::TxnMode::TXN_PES)));
    }

    /*auto vm = _catalog-> getVersionMeta();
    if (vm.ok()) {
        _cfrmTs = vm.value()->timestamp;
        _cfrmVersion = vm.value()->version;
    } else if (vm.status().code() == ErrorCodes::ERR_NOTFOUND) {
        auto pVm = std::make_unique<VersionMeta>();
        Status s = _catalog->setVersionMeta(*pVm);
        if (!s.ok()) {
            LOG(FATAL) << "catalog setVersionMeta error:"
                << s.toString();
            return s;
        }
    } else {
        LOG(FATAL) << "catalog getVersionMeta error:"
            << vm.status().toString();
        return vm.status();
    }*/

    installStoresInLock(tmpStores);
    INVARIANT_D(getKVStoreCount() == kvStoreCount);

    // segment mgr
    auto tmpSegMgr = std::unique_ptr<SegmentMgr>(
        new SegmentMgrFnvHash64(_kvstores, chunkSize));
    installSegMgrInLock(std::move(tmpSegMgr));

    // pessimisticMgr
    auto tmpPessimisticMgr = std::make_unique<PessimisticMgr>(
        kvStoreCount);
    installPessimisticMgrInLock(std::move(tmpPessimisticMgr));

    auto tmpMGLockMgr = std::make_unique <mgl::MGLockMgr>();
    installMGLockMgrInLock(std::move(tmpMGLockMgr));

    // request executePool
    size_t cpuNum = std::thread::hardware_concurrency();
    if (cpuNum == 0) {
        LOG(ERROR) << "ServerEntry::startup failed, cpuNum:" << cpuNum;
        return {ErrorCodes::ERR_INTERNAL, "cpu num cannot be detected"};
    }
    uint32_t threadnum = std::max(size_t(4), cpuNum/2);
    if (cfg->executorThreadNum != 0) {
        threadnum = cfg->executorThreadNum;
    }
    LOG(INFO) << "ServerEntry::startup executor thread num:" << threadnum
        << " executorThreadNum:" << cfg->executorThreadNum;
    for (uint32_t i = 0; i < threadnum; ++i) {
        auto executor = std::make_unique<WorkerPool>("req-exec-" + std::to_string(i), _poolMatrix);
        Status s = executor->startup(1);
        if (!s.ok()) {
            LOG(ERROR) << "ServerEntry::startup failed, executor->startup:" << s.toString();
            return s;
        }
        _executorList.push_back(std::move(executor));
    }

    // network
    _network = std::make_unique<NetworkAsio>(shared_from_this(),
                                             _netMatrix,
                                             _reqMatrix, cfg);
    Status s = _network->prepare(cfg->bindIp, cfg->port, cfg->netIoThreadNum);
    if (!s.ok()) {
        LOG(ERROR) << "ServerEntry::startup failed, _network->prepare:" << s.toString()
            << " ip:" << cfg->bindIp << " port:" << cfg->port;
        return s;
    }
    LOG(INFO) << "_network->prepare ok. ip :" << cfg->bindIp << " port:" << cfg->port;

    // replication
    // replication relys on blocking-client
    // must startup after network prepares ok
    _replMgr = std::make_unique<ReplManager>(shared_from_this(), cfg);
    s = _replMgr->startup();
    if (!s.ok()) {
        LOG(ERROR) << "ServerEntry::startup failed, _replMgr->startup:" << s.toString();
        return s;
    }

    if (!cfg->noexpire) {
        _indexMgr = std::make_unique<IndexManager>(shared_from_this(), cfg);
        s = _indexMgr->startup();
        if (!s.ok()) {
            LOG(ERROR) << "ServerEntry::startup failed, _indexMgr->startup:" << s.toString();
            return s;
        }
    }

    // listener should be the lastone to run.
    s = _network->run();
    if (!s.ok()) {
        LOG(ERROR) << "ServerEntry::startup failed, _network->run:" << s.toString();
        return s;
    } else {
        LOG(WARNING) << "ready to accept connections at "
            << cfg->bindIp << ":" << cfg->port;
    }

    _isRunning.store(true, std::memory_order_relaxed);
    _isStopped.store(false, std::memory_order_relaxed);

    // server stats monitor
    _cronThd = std::make_unique<std::thread>([this] {
        serverCron();
    });

    // init slowlog
    initSlowlog(cfg->slowlogPath);
    LOG(INFO) << "ServerEntry::startup sucess.";
    return {ErrorCodes::ERR_OK, ""};
}

uint64_t ServerEntry::getStartupTimeNs() const {
    return _startupTime;
}

NetworkAsio* ServerEntry::getNetwork() {
    return _network.get();
}

ReplManager* ServerEntry::getReplManager() {
    return _replMgr.get();
}

SegmentMgr* ServerEntry::getSegmentMgr() const {
    return _segmentMgr.get();
}

PessimisticMgr* ServerEntry::getPessimisticMgr() {
    return _pessimisticMgr.get();
}

mgl::MGLockMgr* ServerEntry::getMGLockMgr() {
    return _mgLockMgr.get();
}

IndexManager* ServerEntry::getIndexMgr() {
    return _indexMgr.get();
}

std::string ServerEntry::requirepass() const {
    std::lock_guard<std::mutex> lk(_mutex);
    return _requirepass;
}

void ServerEntry::setRequirepass(const string& v) {
    std::lock_guard<std::mutex> lk(_mutex);
    _requirepass = v;
}

std::string ServerEntry::masterauth() const {
    std::lock_guard<std::mutex> lk(_mutex);
    return _masterauth;
}

void ServerEntry::setMasterauth(const string& v) {
    std::lock_guard<std::mutex> lk(_mutex);
    _masterauth = v;
}

bool ServerEntry::versionIncrease() const {
    return _versionIncrease;
}

bool ServerEntry::addSession(std::shared_ptr<Session> sess) {
    std::lock_guard<std::mutex> lk(_mutex);
    if (!_isRunning.load(std::memory_order_relaxed)) {
        LOG(WARNING) << "session:" << sess->id()
            << " comes when stopping, ignore it";
        return false;
    }

    // NOTE(deyukong): first driving force
    sess->start();
    uint64_t id = sess->id();
    if (_sessions.find(id) != _sessions.end()) {
        LOG(FATAL) << "add session:" << id << ",session id already exists";
    }
    // DLOG(INFO) << "ServerEntry addSession id:" << id << " addr:" << sess->getRemote();
    _sessions[id] = std::move(sess);
    return true;
}

size_t ServerEntry::getSessionCount() {
    std::lock_guard<std::mutex> lk(_mutex);
    return _sessions.size();
}

Status ServerEntry::cancelSession(uint64_t connId) {
    std::lock_guard<std::mutex> lk(_mutex);
    if (!_isRunning.load(std::memory_order_relaxed)) {
        return {ErrorCodes::ERR_BUSY, "server is shutting down"};
    }
    auto it = _sessions.find(connId);
    if (it == _sessions.end()) {
        return {ErrorCodes::ERR_NOTFOUND, "session not found:" + std::to_string(connId)};
    }
    LOG(INFO) << "ServerEntry cancelSession id:" << connId << " addr:" << it->second->getRemote();
    return it->second->cancel();
}

void ServerEntry::endSession(uint64_t connId) {
    std::lock_guard<std::mutex> lk(_mutex);
    if (!_isRunning.load(std::memory_order_relaxed)) {
        return;
    }
    auto it = _sessions.find(connId);
    if (it == _sessions.end()) {
        LOG(FATAL) << "destroy conn:" << connId << ",not exists";
    }
    SessionCtx* pCtx = it->second->getCtx();
    INVARIANT(pCtx != nullptr);
    if (pCtx->getIsMonitor()) {
        DelMonitorNoLock(connId);
    }
    // DLOG(INFO) << "ServerEntry endSession id:" << connId << " addr:" << it->second->getRemote();
    _sessions.erase(it);
}

std::list<std::shared_ptr<Session>> ServerEntry::getAllSessions() const {
    std::lock_guard<std::mutex> lk(_mutex);
    uint64_t start = nsSinceEpoch();
    std::list<std::shared_ptr<Session>> sesses;
    for (const auto& kv : _sessions) {
        sesses.push_back(kv.second);
    }
    uint64_t delta = (nsSinceEpoch() - start)/1000000;
    if (delta >= 5) {
        LOG(WARNING) << "get sessions cost:" << delta << "ms"
                     << "length:" << sesses.size();
    }
    return sesses;
}

void ServerEntry::AddMonitor(uint64_t sessId) {
    std::lock_guard<std::mutex> lk(_mutex);
    for (const auto& monSess : _monitors) {
        if (monSess->id() == sessId) {
            return;
        }
    }
    auto it = _sessions.find(sessId);
    if (it == _sessions.end()) {
        LOG(ERROR) << "AddMonitor session not found:" << sessId;
        return;
    }

    _monitors.push_back(it->second);
}

void ServerEntry::DelMonitorNoLock(uint64_t connId) {
    for (auto it = _monitors.begin(); it != _monitors.end(); ++it) {
        if (it->get()->id() == connId) {
            _monitors.erase(it);
            break;
        }
    }
}

void ServerEntry::replyMonitors(Session* sess) {
    if (_monitors.size() <= 0) {
        return;
    }

    std::string info = "+";

    auto timeNow = std::chrono::duration_cast<std::chrono::microseconds>
        (std::chrono::system_clock::now().time_since_epoch());
    uint64_t timestamp = timeNow.count();

    SessionCtx* pCtx = sess->getCtx();
    INVARIANT(pCtx != nullptr);
    uint32_t dbId = pCtx->getDbId();

    info += std::to_string(timestamp/1000000) + "." + std::to_string(timestamp%1000000);
    info += " [" + std::to_string(dbId) + " " + sess->getRemote() + "] ";
    const auto& args = sess->getArgs();
    for (uint32_t i = 0; i < args.size(); ++i) {
        info += "\"" + args[i] + "\"";
        if (i != (args.size() -1)) {
            info += " ";
        }
    }
    info += "\r\n";

    std::lock_guard<std::mutex> lk(_mutex);

    for (auto& it : _monitors) {
        it->setResponse(info);
    }
}

bool ServerEntry::processRequest(Session *sess) {
    if (!_isRunning.load(std::memory_order_relaxed)) {
        return false;
    }
    // general log if nessarry
    sess->getServerEntry()->logGeneral(sess);
    // NOTE(vinchen): process the ExtraProtocol of timestamp and version
    /*auto s = sess->processExtendProtocol();
    if (!s.ok()) {
        sess->setResponse(
            redis_port::errorReply(s.toString()));
        return true;
    }*/

    auto expCmdName = Command::precheck(sess);
    if (!expCmdName.ok()) {
        sess->setResponse(
            redis_port::errorReply(expCmdName.status().toString()));
        return true;
    }

    replyMonitors(sess);

    if (expCmdName.value() == "fullsync") {
        LOG(WARNING) << "[master] session id:" << sess->id() << " socket borrowed";
        NetSession *ns = dynamic_cast<NetSession*>(sess);
        INVARIANT(ns != nullptr);
        std::vector<std::string> args = ns->getArgs();
        // we have called precheck, it should have 4 args
        INVARIANT(args.size() == 4);
        _replMgr->supplyFullSync(ns->borrowConn(), args[1], args[2], args[3]);
        ++_serverStat.syncFull;
        return false;
    } else if (expCmdName.value() == "incrsync") {
        LOG(WARNING) << "[master] session id:" << sess->id() << " socket borrowed";
        NetSession *ns = dynamic_cast<NetSession*>(sess);
        INVARIANT(ns != nullptr);
        std::vector<std::string> args = ns->getArgs();
        // we have called precheck, it should have 2 args
        INVARIANT(args.size() == 6);
        bool ret = _replMgr->registerIncrSync(ns->borrowConn(), args[1], args[2], args[3], args[4], args[5]);
        if (ret) {
            ++_serverStat.syncPartialOk;
        } else {
            ++_serverStat.syncPartialErr;
        }
        return false;
    } else if (expCmdName.value() == "quit") {
        LOG(INFO) << "quit command";
        NetSession *ns = dynamic_cast<NetSession*>(sess);
        INVARIANT(ns != nullptr);
        ns->setCloseAfterRsp();
        ns->setResponse(Command::fmtOK());
        return true;
    }

    auto expect = Command::runSessionCmd(sess);
    if (!expect.ok()) {
        sess->setResponse(Command::fmtErr(expect.status().toString()));
        return true;
    }
    sess->setResponse(expect.value());
    return true;
}

void ServerEntry::getStatInfo(std::stringstream& ss) const {
    ss << "total_connections_received:" << _netMatrix->connCreated.get() << "\r\n";
    ss << "total_connections_released:" << _netMatrix->connReleased.get() << "\r\n";
    auto executed = _reqMatrix->processed.get();
    ss << "total_commands_processed:" << executed << "\r\n";
    ss << "instantaneous_ops_per_sec:" << _serverStat.getInstantaneousMetric(STATS_METRIC_COMMAND) << "\r\n";

    auto allCost = _poolMatrix->executeTime.get() + _poolMatrix->queueTime.get()
        + _reqMatrix->sendPacketCost.get();
    ss << "total_commands_cost(ns):" << allCost << "\r\n";
    ss << "total_commands_workpool_queue_cost(ns):" << _poolMatrix->queueTime.get() << "\r\n";
    ss << "total_commands_workpool_execute_cost(ns):" << _poolMatrix->executeTime.get() << "\r\n";
    ss << "total_commands_send_packet_cost(ns):" << _reqMatrix->sendPacketCost.get() << "\r\n";
    ss << "total_commands_execute_cost(ns):" << _reqMatrix->processCost.get() << "\r\n";

    if (executed == 0) executed = 1;
    ss << "avg_commands_cost(ns):" << allCost/executed << "\r\n";
    ss << "avg_commands_workpool_queue_cost(ns):" << _poolMatrix->queueTime.get()/executed << "\r\n";
    ss << "avg_commands_workpool_execute_cost(ns):" << _poolMatrix->executeTime.get()/executed << "\r\n";
    ss << "avg_commands_send_packet_cost(ns):" << _reqMatrix->sendPacketCost.get()/executed << "\r\n";
    ss << "avg_commands_execute_cost(ns):" << _reqMatrix->processCost.get()/executed << "\r\n";

    ss << "commands_in_queue:" << _poolMatrix->inQueue.get() << "\r\n";
    ss << "commands_executed_in_workpool:" << _poolMatrix->executed.get() << "\r\n";

    ss << "total_stricky_packets:" << _netMatrix->stickyPackets.get() << "\r\n";
    ss << "total_invalid_packets:" << _netMatrix->invalidPackets.get() << "\r\n";

    ss << "total_net_input_bytes:" << _serverStat.netInputBytes.get() << "\r\n";
    ss << "total_net_output_bytes:" << _serverStat.netOutputBytes.get() << "\r\n";
    ss << "instantaneous_input_kbps:" <<
        static_cast<float>(_serverStat.getInstantaneousMetric(STATS_METRIC_NET_INPUT))/1024 << "\r\n";
    ss << "instantaneous_output_kbps:" <<
        static_cast<float>(_serverStat.getInstantaneousMetric(STATS_METRIC_NET_OUTPUT))/1024 << "\r\n";
    ss << "rejected_connections:" << _serverStat.rejectedConn.get() << "\r\n";
    ss << "sync_full:" << _serverStat.syncFull.get()  << "\r\n";
    ss << "sync_partial_ok:" << _serverStat.syncPartialOk.get()  << "\r\n";
    ss << "sync_partial_err:" << _serverStat.syncPartialErr.get()  << "\r\n";
    ss << "keyspace_hits:" << _serverStat.keyspaceHits.get() << "\r\n";
    ss << "keyspace_misses:" << _serverStat.keyspaceMisses.get() << "\r\n";
    ss << "keyspace_wrong_versionep:" << _serverStat.keyspaceIncorrectEp.get() << "\r\n";
}

void ServerEntry::appendJSONStat(rapidjson::PrettyWriter<rapidjson::StringBuffer>& w,
                                 const std::set<std::string>& sections) const {
    if (sections.find("network") != sections.end()) {
        w.Key("network");
        w.StartObject();
        w.Key("sticky_packets");
        w.Uint64(_netMatrix->stickyPackets.get());
        w.Key("conn_created");
        w.Uint64(_netMatrix->connCreated.get());
        w.Key("conn_released");
        w.Uint64(_netMatrix->connReleased.get());
        w.Key("invalid_packets");
        w.Uint64(_netMatrix->invalidPackets.get());
        w.EndObject();
    }
    if (sections.find("request") != sections.end()) {
        w.Key("request");
        w.StartObject();
        w.Key("processed");
        w.Uint64(_reqMatrix->processed.get());
        w.Key("process_cost");
        w.Uint64(_reqMatrix->processCost.get());
        w.Key("send_packet_cost");
        w.Uint64(_reqMatrix->sendPacketCost.get());
        w.EndObject();
    }
    if (sections.find("req_pool") != sections.end()) {
        w.Key("req_pool");
        w.StartObject();
        w.Key("in_queue");
        w.Uint64(_poolMatrix->inQueue.get());
        w.Key("executed");
        w.Uint64(_poolMatrix->executed.get());
        w.Key("queue_time");
        w.Uint64(_poolMatrix->queueTime.get());
        w.Key("execute_time");
        w.Uint64(_poolMatrix->executeTime.get());
        w.EndObject();
    }
}

bool ServerEntry::getTotalIntProperty(Session* sess, const std::string& property, uint64_t* value) const {
    *value = 0;
    for (uint64_t i = 0; i < getKVStoreCount(); i++) {
        auto expdb = getSegmentMgr()->getDb(sess, i,
            mgl::LockMode::LOCK_IS);
        if (!expdb.ok()) {
            return false;
        }

        auto store = expdb.value().store;
        uint64_t tmp = 0;
        bool ok = store->getIntProperty(property, &tmp);
        if (!ok) {
            return false;
        }
        *value += tmp;
    }

    return true;
}

bool ServerEntry::getAllProperty(Session* sess, const std::string& property, std::string* value) const {
    std::stringstream ss;
    for (uint64_t i = 0; i < getKVStoreCount(); i++) {
        auto expdb = getSegmentMgr()->getDb(sess, i,
            mgl::LockMode::LOCK_IS);
        if (!expdb.ok()) {
            return false;
        }

        auto store = expdb.value().store;
        std::string tmp;
        bool ok = store->getProperty(property, &tmp);
        if (!ok) {
            return false;
        }
        ss << "store_" << store->dbId() << ":" << tmp << "\r\n";
    }
    *value = ss.str();

    return true;
}

void ServerEntry::resetRocksdbStats(Session* sess) {
    std::stringstream ss;
    for (uint64_t i = 0; i < getKVStoreCount(); i++) {
        auto expdb = getSegmentMgr()->getDb(sess, i,
            mgl::LockMode::LOCK_IS);
        if (!expdb.ok()) {
            continue;
        }

        auto store = expdb.value().store;
        store->resetStatistics();
    }
}

Status ServerEntry::destroyStore(Session *sess,
            uint32_t storeId, bool isForce) {
    auto expdb = getSegmentMgr()->getDb(sess, storeId,
        mgl::LockMode::LOCK_X);
    if (!expdb.ok()) {
        return expdb.status();
    }

    auto store = expdb.value().store;
    if (!isForce) {
        if (!store->isEmpty()) {
            return{ ErrorCodes::ERR_INTERNAL,
                "try to close an unempty store" };
        }
    }

    if (!store->isPaused()) {
        return{ ErrorCodes::ERR_INTERNAL,
            "please pausestore first before destroystore" };
    }

    if (store->getMode() == KVStore::StoreMode::READ_WRITE) {
        // TODO(vinchen)
        // NOTE(vinchen): maybe it should create a binlog here to
        // destroy the store of slaves.
        // But it maybe hard to confirm whether all the slaves apply
        // this binlog before the master destroy. (check MPOVStatus?)
    }

    auto meta = getCatalog()->getStoreMainMeta(storeId);
    if (!meta.ok()) {
        LOG(WARNING) << "get store main meta:" << storeId
            << " failed:" << meta.status().toString();
        return meta.status();
    }
    meta.value()->storeMode = KVStore::StoreMode::STORE_NONE;
    Status status = getCatalog()->setStoreMainMeta(*meta.value());
    if (!status.ok()) {
        LOG(WARNING) << "set store main meta:" << storeId
            << " failed:" << status.toString();
        return status;
    }

    status = store->destroy();
    if (!status.ok()) {
        LOG(ERROR) << "destroy store :" << storeId
            << " failed:" << status.toString();
        return status;
    }
    INVARIANT_D(store->getMode() == KVStore::StoreMode::STORE_NONE);

    status = _replMgr->stopStore(storeId);
    if (!status.ok()) {
        LOG(ERROR) << "replMgr stopStore :" << storeId
            << " failed:" << status.toString();
        return status;
    }

    status = _indexMgr->stopStore(storeId);
    if (!status.ok()) {
        LOG(ERROR) << "indexMgr stopStore :" << storeId
            << " failed:" << status.toString();
        return status;
    }

    return status;
}

Status ServerEntry::setStoreMode(PStore store,
    KVStore::StoreMode mode) {

    // assert held the X lock of store
    if (store->getMode() == mode) {
        return{ ErrorCodes::ERR_OK, "" };
    }

    auto catalog = getCatalog();
    Status status = store->setMode(mode);
    if (!status.ok()) {
        LOG(FATAL) << "ServerEntry::setStoreMode error, "
                << status.toString();
        return status;
    }
    auto storeId = tendisplus::stoul(store->dbId());
    if (!storeId.ok()) {
        return storeId.status();
    }
    auto meta = catalog->getStoreMainMeta(storeId.value());
    meta.value()->storeMode = mode;

    return catalog->setStoreMainMeta(*meta.value());
}

#define run_with_period(_ms_) if ((_ms_ <= 1000/hz) || !(cronLoop%((_ms_)/(1000/hz))))

void ServerEntry::serverCron() {
    using namespace std::chrono_literals;  // NOLINT(build/namespaces)

    auto oldNetMatrix = *_netMatrix;
    auto oldPoolMatrix = *_poolMatrix;
    auto oldReqMatrix = *_reqMatrix;

    uint64_t cronLoop = 0;
    auto interval = 100ms;  // every 100ms execute one time
    uint64_t hz = 1000ms / interval;

    LOG(INFO) << "serverCron thread starts, hz:" << hz;
    while (_isRunning.load(std::memory_order_relaxed)) {
        std::unique_lock<std::mutex> lk(_mutex);

        bool ok = _eventCV.wait_for(lk, interval, [this] {
            return _isRunning.load(std::memory_order_relaxed) == false;
            });
        if (ok) {
            LOG(INFO) << "serverCron thread exits";
            return;
        }

        run_with_period(100) {
            _serverStat.trackInstantaneousMetric(STATS_METRIC_COMMAND,
                _reqMatrix->processed.get());
            _serverStat.trackInstantaneousMetric(STATS_METRIC_NET_INPUT,
                _serverStat.netInputBytes.get());
            _serverStat.trackInstantaneousMetric(STATS_METRIC_NET_OUTPUT,
                _serverStat.netOutputBytes.get());
        }

        run_with_period(1000) {
            // full-time matrix collect
            if (_ftmcEnabled.load(std::memory_order_relaxed)) {
                auto tmpNetMatrix = *_netMatrix - oldNetMatrix;
                auto tmpPoolMatrix = *_poolMatrix - oldPoolMatrix;
                auto tmpReqMatrix = *_reqMatrix - oldReqMatrix;
                oldNetMatrix = *_netMatrix;
                oldPoolMatrix = *_poolMatrix;
                oldReqMatrix = *_reqMatrix;
                // TODO(vinchen): we should create a view here
                LOG(INFO) << "network matrix status:\n" << tmpNetMatrix.toString();
                LOG(INFO) << "pool matrix status:\n" << tmpPoolMatrix.toString();
                LOG(INFO) << "req matrix status:\n" << tmpReqMatrix.toString();
            }
        }

        cronLoop++;
    }
}

void ServerEntry::waitStopComplete() {
    using namespace std::chrono_literals;  // NOLINT(build/namespaces)
    bool shutdowned = false;
    while (_isRunning.load(std::memory_order_relaxed)) {
        std::unique_lock<std::mutex> lk(_mutex);
        bool ok = _eventCV.wait_for(lk, 1000ms, [this] {
            return _isRunning.load(std::memory_order_relaxed) == false
                && _isStopped.load(std::memory_order_relaxed) == true;
        });
        if (ok) {
            return;
        }

        if (_isShutdowned.load(std::memory_order_relaxed)) {
            LOG(INFO) << "shutdown command";
            shutdowned = true;
            break;
        }
    }

    // NOTE(vinchen): it can't hold the _mutex before stop()
    if (shutdowned) {
        stop();
    }
}

void ServerEntry::handleShutdownCmd() {
    _isShutdowned.store(true, std::memory_order_relaxed);
}

void ServerEntry::stop() {
    if (_isRunning.load(std::memory_order_relaxed) == false) {
        LOG(INFO) << "server is stopping, plz donot kill again";
        return;
    }
    LOG(INFO) << "server begins to stop...";
    _isRunning.store(false, std::memory_order_relaxed);
    _eventCV.notify_all();
    _network->stop();
    for (auto& executor : _executorList) {
        executor->stop();
    }
    _replMgr->stop();
    _indexMgr->stop();
    _sessions.clear();

    if (!_isShutdowned.load(std::memory_order_relaxed)) {
        // NOTE(vinchen): if it's not the shutdown command, it should reset the
        // workerpool to decr the referent count of share_ptr<server>
        _network.reset();
        for (auto& executor : _executorList) {
            executor.reset();
        }
        _replMgr.reset();
        _indexMgr.reset();
        _pessimisticMgr.reset();
        _mgLockMgr.reset();
        _segmentMgr.reset();
    }

    // stop the rocksdb
    std::stringstream ss;
    Status status = _catalog->stop();
    if (!status.ok()) {
        ss << "stop kvstore catalog failed: "
                        << status.toString();
        LOG(ERROR) << ss.str();
    }

    for (auto& store : _kvstores) {
        Status status = store->stop();
        if (!status.ok()) {
            ss.clear();
            ss << "stop kvstore " << store->dbId() << "failed: "
                        << status.toString();
            LOG(ERROR) << ss.str();
        }
    }

    _cronThd->join();
    _slowLog.close();
    LOG(INFO) << "server stops complete...";
    _isStopped.store(true, std::memory_order_relaxed);
    _eventCV.notify_all();
}

void ServerEntry::toggleFtmc(bool enable) {
    _ftmcEnabled.store(enable, std::memory_order_relaxed);
}

uint64_t ServerEntry::getTsEp() const {
    return _tsFromExtendedProtocol.load(std::memory_order_relaxed);
}

void ServerEntry::setTsEp(uint64_t timestamp) {
    _tsFromExtendedProtocol.store(timestamp, std::memory_order_relaxed);
}

Status ServerEntry::initSlowlog(std::string logPath) {
    _slowLog.open(logPath, std::ofstream::app);
    if (!_slowLog.is_open()) {
        std::stringstream ss;
        ss << "open:" << logPath << " failed";
        return {ErrorCodes::ERR_INTERNAL, ss.str()};
    }

    return {ErrorCodes::ERR_OK, ""};
}

void ServerEntry::slowlogPushEntryIfNeeded(uint64_t time, uint64_t duration,
            const std::vector<std::string>& args) {
    if (duration > _cfg->slowlogLogSlowerThan) {
        std::unique_lock<std::mutex> lk(_mutex);
        _slowLog << "#Id: " << _slowlogId.load(std::memory_order_relaxed) << "\n";
        _slowLog << "#Time: " << time << "\n";
        _slowLog << "#Query_time: " << duration << "\n";
        for (size_t i = 0; i < args.size(); ++i) {
            _slowLog << args[i] << " ";
        }
        _slowLog << "\n";
        _slowLog << "#argc: " << args.size() << "\n\n";
        if ((_slowlogId.load(std::memory_order_relaxed)%_cfg->slowlogFlushInterval) == 0) {
            _slowLog.flush();
        }

        _slowlogId.fetch_add(1, std::memory_order_relaxed);
    }
}

}  // namespace tendisplus
