#include <router/router.hpp>

#include <config/config.hpp>
#include <constants/limits.hpp>
#include <constants/proto.hpp>
#include <crypto/crypto_libsodium.hpp>
#include <crypto/crypto.hpp>
#include <dht/context.hpp>
#include <dht/node.hpp>
#include <iwp/iwp.hpp>
#include <link/server.hpp>
#include <messages/link_message.hpp>
#include <net/net.hpp>
#include <rpc/rpc.hpp>
#include <util/buffer.hpp>
#include <util/encode.hpp>
#include <util/logging/file_logger.hpp>
#include <util/logging/json_logger.hpp>
#include <util/logging/logger_syslog.hpp>
#include <util/logging/logger.hpp>
#include <util/meta/memfn.hpp>
#include <util/metrics/metrics.hpp>
#include <util/str.hpp>
#include <ev/ev.hpp>

#include <fstream>
#include <cstdlib>
#include <iterator>
#include <unordered_map>
#include <utility>
#if defined(RPI) || defined(ANDROID)
#include <unistd.h>
#endif

bool
llarp_loadServiceNodeIdentityKey(const fs::path &fpath,
                                 llarp::SecretKey &secret)
{
  std::string path = fpath.string();
  llarp::IdentitySecret ident;

  if(!ident.LoadFromFile(path.c_str()))
    return false;

  return llarp::CryptoManager::instance()->seed_to_secretkey(secret, ident);
}

bool
llarp_findOrCreateIdentity(const fs::path &path, llarp::SecretKey &secretkey)
{
  std::string fpath = path.string();
  llarp::LogDebug("find or create ", fpath);
  std::error_code ec;
  if(!fs::exists(path, ec))
  {
    llarp::LogInfo("generating new identity key");
    llarp::CryptoManager::instance()->identity_keygen(secretkey);
    if(!secretkey.SaveToFile(fpath.c_str()))
      return false;
  }
  return secretkey.LoadFromFile(fpath.c_str());
}

// C++ ...
bool
llarp_findOrCreateEncryption(const fs::path &path, llarp::SecretKey &encryption)
{
  std::string fpath = path.string();
  llarp::LogDebug("find or create ", fpath);
  std::error_code ec;
  if(!fs::exists(path, ec))
  {
    llarp::LogInfo("generating new encryption key");
    llarp::CryptoManager::instance()->encryption_keygen(encryption);
    if(!encryption.SaveToFile(fpath.c_str()))
      return false;
  }
  return encryption.LoadFromFile(fpath.c_str());
}

namespace llarp
{
  Router::Router(std::shared_ptr< llarp::thread::ThreadPool > _tp,
                 llarp_ev_loop_ptr __netloop, std::shared_ptr< Logic > l)
      : ready(false)
      , _netloop(std::move(__netloop))
      , cryptoworker(std::move(_tp))
      , _logic(std::move(l))
      , paths(this)
      , _exitContext(this)
      , disk(std::make_shared< llarp::thread::ThreadPool >(1, 1000,
                                                           "diskworker"))
      , _dht(llarp_dht_context_new(this))
      , inbound_link_msg_parser(this)
      , _hiddenServiceContext(this)
  {
    // set rational defaults
    this->ip4addr.sin_family = AF_INET;
    this->ip4addr.sin_port   = htons(1090);

    _stopping.store(false);
    _running.store(false);
  }

  Router::~Router()
  {
    llarp_dht_context_free(_dht);
  }

  util::StatusObject
  Router::ExtractStatus() const
  {
    if(_running)
    {
      return util::StatusObject{
          {"running", true},
          {"numNodesKnown", _nodedb->num_loaded()},
          {"dht", _dht->impl->ExtractStatus()},
          {"services", _hiddenServiceContext.ExtractStatus()},
          {"exit", _exitContext.ExtractStatus()},
          {"links", _linkManager.ExtractStatus()}};
    }
    else
    {
      return util::StatusObject{{"running", false}};
    }
  }

  bool
  Router::HandleRecvLinkMessageBuffer(ILinkSession *session,
                                      const llarp_buffer_t &buf)
  {
    if(_stopping)
      return true;

    if(!session)
    {
      LogWarn("no link session");
      return false;
    }
    return inbound_link_msg_parser.ProcessFrom(session, buf);
  }

  void
  Router::PersistSessionUntil(const RouterID &remote, llarp_time_t until)
  {
    _linkManager.PersistSessionUntil(remote, until);
  }

  bool
  Router::GetRandomGoodRouter(RouterID &router)
  {
    if(whitelistRouters)
    {
      return _rcLookupHandler.GetRandomWhitelistRouter(router);
    }

    auto pick_router = [&](auto &collection) -> bool {
      const auto sz = collection.size();
      auto itr      = collection.begin();
      if(sz == 0)
        return false;
      if(sz > 1)
        std::advance(itr, randint() % sz);
      router = itr->first;
      return true;
    };

    absl::ReaderMutexLock l(&nodedb()->access);
    return pick_router(nodedb()->entries);
  }

  void
  Router::PumpLL()
  {
    if(_stopping.load())
      return;

    paths.PumpDownstream();
    paths.PumpUpstream();

    _outboundMessageHandler.Tick();

    _linkManager.PumpLinks();
  }

  bool
  Router::SendToOrQueue(const RouterID &remote, const ILinkMessage *msg,
                        SendStatusHandler handler)
  {
    if(handler == nullptr)
    {
      using std::placeholders::_1;
      handler = std::bind(&Router::MessageSent, this, remote, _1);
    }
    return _outboundMessageHandler.QueueMessage(remote, msg, handler);
  }

  void
  Router::ForEachPeer(std::function< void(const ILinkSession *, bool) > visit,
                      bool randomize) const
  {
    _linkManager.ForEachPeer(visit, randomize);
  }

  void
  Router::ForEachPeer(std::function< void(ILinkSession *) > visit)
  {
    _linkManager.ForEachPeer(visit);
  }

  void
  Router::try_connect(fs::path rcfile)
  {
    RouterContact remote;
    if(!remote.Read(rcfile.string().c_str()))
    {
      LogError("failure to decode or verify of remote RC");
      return;
    }
    if(remote.Verify(Now()))
    {
      LogDebug("verified signature");
      _outboundSessionMaker.CreateSessionTo(remote, nullptr);
    }
    else
      LogError(rcfile, " contains invalid RC");
  }

  bool
  Router::EnsureIdentity()
  {
    if(!EnsureEncryptionKey())
      return false;
    if(usingSNSeed)
      return llarp_loadServiceNodeIdentityKey(ident_keyfile, _identity);

    return llarp_findOrCreateIdentity(ident_keyfile, _identity);
  }

  bool
  Router::EnsureEncryptionKey()
  {
    return llarp_findOrCreateEncryption(encryption_keyfile, _encryption);
  }

  bool
  Router::Configure(Config *conf, llarp_nodedb *nodedb)
  {
    if(nodedb == nullptr)
    {
      LogError(
          "Attempting to Router::Configure but passed null nodedb pointer");
      return false;
    }
    _nodedb = nodedb;

    if(!FromConfig(conf))
      return false;

    if(!InitOutboundLinks())
      return false;
    return EnsureIdentity();
  }

  /// called in disk worker thread
  void
  Router::HandleSaveRC() const
  {
    std::string fname = our_rc_file.string();
    _rc.Write(fname.c_str());
  }

  bool
  Router::SaveRC()
  {
    LogDebug("verify RC signature");
    if(!_rc.Verify(Now()))
    {
      Dump< MAX_RC_SIZE >(rc());
      LogError("RC is invalid, not saving");
      return false;
    }
    diskworker()->addJob(std::bind(&Router::HandleSaveRC, this));
    return true;
  }

  bool
  Router::IsServiceNode() const
  {
    return m_isServiceNode;
  }

  void
  Router::Close()
  {
    LogInfo("closing router");
    llarp_ev_loop_stop(_netloop);
    disk->stop();
    disk->shutdown();
  }

  void
  Router::handle_router_ticker(void *user, uint64_t orig, uint64_t left)
  {
    if(left)
      return;
    auto *self          = static_cast< Router * >(user);
    self->ticker_job_id = 0;
    LogicCall(self->logic(), std::bind(&Router::Tick, self));
    self->ScheduleTicker(orig);
  }

  bool
  Router::ParseRoutingMessageBuffer(const llarp_buffer_t &buf,
                                    routing::IMessageHandler *h,
                                    const PathID_t &rxid)
  {
    return inbound_routing_msg_parser.ParseMessageBuffer(buf, h, rxid, this);
  }

  bool
  Router::ConnectionToRouterAllowed(const RouterID &router) const
  {
    return _rcLookupHandler.RemoteIsAllowed(router);
  }

  size_t
  Router::NumberOfConnectedRouters() const
  {
    return _linkManager.NumberOfConnectedRouters();
  }

  size_t
  Router::NumberOfConnectedClients() const
  {
    return _linkManager.NumberOfConnectedClients();
  }

  bool
  Router::UpdateOurRC(bool rotateKeys)
  {
    SecretKey nextOnionKey;
    RouterContact nextRC = _rc;
    if(rotateKeys)
    {
      CryptoManager::instance()->encryption_keygen(nextOnionKey);
      std::string f = encryption_keyfile.string();
      // TODO: use disk worker
      if(nextOnionKey.SaveToFile(f.c_str()))
      {
        nextRC.enckey = seckey_topublic(nextOnionKey);
        _encryption   = nextOnionKey;
      }
    }
    if(!nextRC.Sign(identity()))
      return false;
    if(!nextRC.Verify(time_now_ms(), false))
      return false;
    _rc = std::move(nextRC);
    // propagate RC by renegotiating sessions
    ForEachPeer([](ILinkSession *s) {
      if(s->RenegotiateSession())
        LogInfo("renegotiated session");
      else
        LogWarn("failed to renegotiate session");
    });

    return SaveRC();
  }

  bool
  Router::FromConfig(Config *conf)
  {
    // Set netid before anything else
    if(!conf->router.netId().empty()
       && strcmp(conf->router.netId().c_str(), Version::LLARP_NET_ID))
    {
      const auto &netid = conf->router.netId();
      llarp::LogWarn("!!!! you have manually set netid to be '", netid,
                     "' which does not equal '", Version::LLARP_NET_ID,
                     "' you will run as a different network, good luck "
                     "and don't forget: something something MUH traffic "
                     "shape correlation !!!!");
      NetID::DefaultValue() =
          NetID(reinterpret_cast< const byte_t * >(netid.c_str()));
      // reset netid in our rc
      _rc.netID = llarp::NetID();
    }
    const auto linktypename = conf->router.defaultLinkProto();
    _defaultLinkType        = LinkFactory::TypeFromName(linktypename);
    if(_defaultLinkType == LinkFactory::LinkType::eLinkUnknown)
    {
      LogError("failed to set link type to '", linktypename,
               "' as that is invalid");
      return false;
    }

    // IWP config
    m_OutboundPort = std::get< LinksConfig::Port >(conf->links.outboundLink());
    // Router config
    _rc.SetNick(conf->router.nickname());
    _outboundSessionMaker.maxConnectedRouters =
        conf->router.maxConnectedRouters();
    _outboundSessionMaker.minConnectedRouters =
        conf->router.minConnectedRouters();
    encryption_keyfile = conf->router.encryptionKeyfile();
    our_rc_file        = conf->router.ourRcFile();
    transport_keyfile  = conf->router.transportKeyfile();
    addrInfo           = conf->router.addrInfo();
    publicOverride     = conf->router.publicOverride();
    ip4addr            = conf->router.ip4addr();

    if(!conf->router.blockBogons().value_or(true))
    {
      RouterContact::BlockBogons = false;
    }

    // Lokid Config
    usingSNSeed      = conf->lokid.usingSNSeed;
    ident_keyfile    = conf->lokid.ident_keyfile;
    whitelistRouters = conf->lokid.whitelistRouters;
    lokidRPCAddr     = conf->lokid.lokidRPCAddr;
    lokidRPCUser     = conf->lokid.lokidRPCUser;
    lokidRPCPassword = conf->lokid.lokidRPCPassword;

    // TODO: add config flag for "is service node"
    if(conf->links.inboundLinks().size())
    {
      m_isServiceNode = true;
    }

    std::set< RouterID > strictConnectPubkeys;

    if(!conf->network.strictConnect().empty())
    {
      const auto &val = conf->network.strictConnect();
      if(IsServiceNode())
      {
        llarp::LogError("cannot use strict-connect option as service node");
        return false;
      }
      llarp::RouterID snode;
      llarp::PubKey pk;
      if(pk.FromString(val))
      {
        if(strictConnectPubkeys.emplace(pk).second)
          llarp::LogInfo("added ", pk, " to strict connect list");
        else
          llarp::LogWarn("duplicate key for strict connect: ", pk);
      }
      else if(snode.FromString(val))
      {
        if(strictConnectPubkeys.insert(snode).second)
        {
          llarp::LogInfo("added ", snode, " to strict connect list");
          netConfig.emplace("strict-connect", val);
        }
        else
          llarp::LogWarn("duplicate key for strict connect: ", snode);
      }
      else
        llarp::LogError("invalid key for strict-connect: ", val);
    }

    std::vector< std::string > configRouters = conf->connect.routers;
    configRouters.insert(configRouters.end(), conf->bootstrap.routers.begin(),
                         conf->bootstrap.routers.end());
    for(const auto &router : configRouters)
    {
      // llarp::LogDebug("connect section has ", key, "=", val);
      RouterContact rc;
      if(!rc.Read(router.c_str()))
      {
        llarp::LogWarn("failed to decode bootstrap RC, file='", router,
                       "' rc=", rc);
        return false;
      }
      if(rc.Verify(Now()))
      {
        const auto result = bootstrapRCList.insert(rc);
        if(result.second)
          llarp::LogInfo("Added bootstrap node ", RouterID(rc.pubkey));
        else
          llarp::LogWarn("Duplicate bootstrap node ", RouterID(rc.pubkey));
      }
      else
      {
        if(rc.IsExpired(Now()))
        {
          llarp::LogWarn("Bootstrap node ", RouterID(rc.pubkey),
                         " is too old and needs to be refreshed");
        }
        else
        {
          llarp::LogError("malformed rc file='", router, "' rc=", rc);
        }
      }
    }

    // Init components after relevant config settings loaded
    _outboundMessageHandler.Init(&_linkManager, _logic);
    _outboundSessionMaker.Init(&_linkManager, &_rcLookupHandler, _logic,
                               _nodedb, threadpool());
    _linkManager.Init(&_outboundSessionMaker);
    _rcLookupHandler.Init(_dht, _nodedb, threadpool(), &_linkManager,
                          &_hiddenServiceContext, strictConnectPubkeys,
                          bootstrapRCList, whitelistRouters, m_isServiceNode);

    if(!usingSNSeed)
    {
      ident_keyfile = conf->router.identKeyfile();
    }

    // create inbound links, if we are a service node
    for(const auto &serverConfig : conf->links.inboundLinks())
    {
      // get default factory
      auto inboundLinkFactory = LinkFactory::Obtain(_defaultLinkType, true);
      // for each option if provided ...
      for(const auto &opt : std::get< LinksConfig::Options >(serverConfig))
      {
        // try interpreting it as a link type
        const auto linktype = LinkFactory::TypeFromName(opt);
        if(linktype != LinkFactory::LinkType::eLinkUnknown)
        {
          // override link factory if it's a valid link type
          auto factory = LinkFactory::Obtain(linktype, true);
          if(factory)
          {
            inboundLinkFactory = std::move(factory);
            break;
          }
        }
      }

      auto server = inboundLinkFactory(
          encryption(), util::memFn(&AbstractRouter::rc, this),
          util::memFn(&AbstractRouter::HandleRecvLinkMessageBuffer, this),
          util::memFn(&AbstractRouter::Sign, this),
          util::memFn(&IOutboundSessionMaker::OnSessionEstablished,
                      &_outboundSessionMaker),
          util::memFn(&AbstractRouter::CheckRenegotiateValid, this),
          util::memFn(&IOutboundSessionMaker::OnConnectTimeout,
                      &_outboundSessionMaker),
          util::memFn(&AbstractRouter::SessionClosed, this),
          util::memFn(&AbstractRouter::PumpLL, this));

      if(!server->EnsureKeys(transport_keyfile.string().c_str()))
      {
        llarp::LogError("failed to ensure keyfile ", transport_keyfile);
        return false;
      }

      const auto &key = std::get< LinksConfig::Interface >(serverConfig);
      int af          = std::get< LinksConfig::AddressFamily >(serverConfig);
      uint16_t port   = std::get< LinksConfig::Port >(serverConfig);
      if(!server->Configure(netloop(), key, af, port))
      {
        LogError("failed to bind inbound link on ", key, " port ", port);
        return false;
      }
      _linkManager.AddLink(std::move(server), true);
    }

    // set network config
    netConfig = conf->network.netConfig();

    // Network config
    if(conf->network.enableProfiling().has_value())
    {
      if(conf->network.enableProfiling().value())
      {
        routerProfiling().Enable();
        LogInfo("router profiling explicitly enabled");
      }
      else
      {
        routerProfiling().Disable();
        LogInfo("router profiling explicitly disabled");
      }
    }

    if(!conf->network.routerProfilesFile().empty())
    {
      routerProfilesFile = conf->network.routerProfilesFile();
      routerProfiling().Load(routerProfilesFile.c_str());
      llarp::LogInfo("setting profiles to ", routerProfilesFile);
    }

    // API config
    enableRPCServer = conf->api.enableRPCServer();
    rpcBindAddr     = conf->api.rpcBindAddr();

    // Services config
    for(const auto &service : conf->services.services)
    {
      if(LoadHiddenServiceConfig(service.second))
      {
        llarp::LogInfo("loaded hidden service config for ", service.first);
      }
      else
      {
        llarp::LogWarn("failed to load hidden service config for ",
                       service.first);
      }
    }

    // Logging config

    auto logfile = conf->logging.m_LogFile;

    if(conf->logging.m_LogJSON)
    {
      LogContext::Instance().logStream = std::make_unique< JSONLogStream >(
          diskworker(), logfile, 100, logfile != stdout);
    }
    else if(logfile != stdout)
    {
      LogContext::Instance().logStream =
          std::make_unique< FileLogStream >(diskworker(), logfile, 100, true);
    }

    netConfig.insert(conf->dns.netConfig.begin(), conf->dns.netConfig.end());

    return true;
  }

  bool
  Router::CheckRenegotiateValid(RouterContact newrc, RouterContact oldrc)
  {
    return _rcLookupHandler.CheckRenegotiateValid(newrc, oldrc);
  }

  bool
  Router::IsBootstrapNode(const RouterID r) const
  {
    return std::count_if(
               bootstrapRCList.begin(), bootstrapRCList.end(),
               [r](const RouterContact &rc) -> bool { return rc.pubkey == r; })
        > 0;
  }

  bool
  Router::ShouldReportStats(llarp_time_t now) const
  {
    static constexpr llarp_time_t ReportStatsInterval = 60 * 60 * 1000;
    return now - m_LastStatsReport > ReportStatsInterval;
  }

  void
  Router::ReportStats()
  {
    const auto now = Now();
    LogInfo(nodedb()->num_loaded(), " RCs loaded");
    LogInfo(bootstrapRCList.size(), " bootstrap peers");
    LogInfo(NumberOfConnectedRouters(), " router connections");
    if(IsServiceNode())
    {
      LogInfo(NumberOfConnectedClients(), " client connections");
      LogInfo(_rc.Age(now), " ms since we last updated our RC");
      LogInfo(_rc.TimeUntilExpires(now), " ms until our RC expires");
    }
    LogInfo(now, " system time");
    LogInfo(m_LastStatsReport, " last reported stats");
    m_LastStatsReport = now;
  }

  void
  Router::Tick()
  {
    if(_stopping)
      return;
    // LogDebug("tick router");
    const auto now = Now();

    routerProfiling().Tick();

    if(ShouldReportStats(now))
    {
      ReportStats();
    }

    _rcLookupHandler.PeriodicUpdate(now);

    const bool isSvcNode = IsServiceNode();

    if(_rc.ExpiresSoon(now, randint() % 10000)
       || (now - _rc.last_updated) > rcRegenInterval)
    {
      LogInfo("regenerating RC");
      if(!UpdateOurRC(false))
        LogError("Failed to update our RC");
    }

    if(isSvcNode)
    {
      // remove RCs for nodes that are no longer allowed by network policy
      nodedb()->RemoveIf([&](const RouterContact &rc) -> bool {
        if(IsBootstrapNode(rc.pubkey))
          return false;
        return !_rcLookupHandler.RemoteIsAllowed(rc.pubkey);
      });
    }

    _linkManager.CheckPersistingSessions(now);

    const size_t connected = NumberOfConnectedRouters();
    const size_t N         = nodedb()->num_loaded();
    if(N < llarp::path::default_len)
    {
      LogInfo("We need at least ", llarp::path::default_len,
              " service nodes to build paths but we have ", N, " in nodedb");

      _rcLookupHandler.ExploreNetwork();
    }
    size_t connectToNum      = _outboundSessionMaker.minConnectedRouters;
    const auto strictConnect = _rcLookupHandler.NumberOfStrictConnectRouters();
    if(strictConnect > 0 && connectToNum > strictConnect)
    {
      connectToNum = strictConnect;
    }

    if(connected < connectToNum)
    {
      size_t dlt = connectToNum - connected;
      LogInfo("connecting to ", dlt, " random routers to keep alive");
      _outboundSessionMaker.ConnectToRandomRouters(dlt, now);
    }

    _hiddenServiceContext.Tick(now);
    _exitContext.Tick(now);

    if(rpcCaller)
      rpcCaller->Tick(now);
    // save profiles async
    if(routerProfiling().ShouldSave(now))
    {
      diskworker()->addJob(
          [&]() { routerProfiling().Save(routerProfilesFile.c_str()); });
    }

    // get connected peers
    std::set< dht::Key_t > peersWeHave;
    _linkManager.ForEachPeer([&peersWeHave](ILinkSession *s) {
      if(!s->IsEstablished())
        return;
      peersWeHave.emplace(s->GetPubKey());
    });
    // remove any nodes we don't have connections to
    _dht->impl->Nodes()->RemoveIf([&peersWeHave](const dht::Key_t &k) -> bool {
      return peersWeHave.count(k) == 0;
    });
    // expire paths
    paths.ExpirePaths(now);
  }  // namespace llarp

  bool
  Router::Sign(Signature &sig, const llarp_buffer_t &buf) const
  {
    metrics::TimerGuard t("Router", "Sign");
    return CryptoManager::instance()->sign(sig, identity(), buf);
  }

  void
  Router::ScheduleTicker(uint64_t ms)
  {
    ticker_job_id = _logic->call_later({ms, this, &handle_router_ticker});
  }

  void
  Router::SessionClosed(RouterID remote)
  {
    dht::Key_t k(remote);
    dht()->impl->Nodes()->DelNode(k);

    LogInfo("Session to ", remote, " fully closed");
  }

  bool
  Router::GetRandomConnectedRouter(RouterContact &result) const
  {
    return _linkManager.GetRandomConnectedRouter(result);
  }

  void
  Router::HandleDHTLookupForExplore(ABSL_ATTRIBUTE_UNUSED RouterID remote,
                                    const std::vector< RouterContact > &results)
  {
    for(const auto &rc : results)
    {
      _rcLookupHandler.CheckRC(rc);
    }
  }

  // TODO: refactor callers and remove this function
  void
  Router::LookupRouter(RouterID remote, RouterLookupHandler resultHandler)
  {
    _rcLookupHandler.GetRC(
        remote,
        [=](const RouterID &id, const RouterContact *const rc,
            const RCRequestResult result) {
          (void)id;
          if(resultHandler)
          {
            std::vector< RouterContact > routers;
            if(result == RCRequestResult::Success && rc != nullptr)
            {
              routers.push_back(*rc);
            }
            resultHandler(routers);
          }
        });
  }

  void
  Router::SetRouterWhitelist(const std::vector< RouterID > &routers)
  {
    _rcLookupHandler.SetRouterWhitelist(routers);
  }

  /// this function ensure there are sane defualts in a net config
  static void
  EnsureNetConfigDefaultsSane(
      std::unordered_multimap< std::string, std::string > &netConfig)
  {
    static const std::unordered_map< std::string,
                                     std::function< std::string(void) > >
        netConfigDefaults = {
            {"ifname", llarp::FindFreeTun},
            {"ifaddr", llarp::FindFreeRange},
            {"local-dns", []() -> std::string { return "127.0.0.1:53"; }}};
    // populate with fallback defaults if values not present
    auto itr = netConfigDefaults.begin();
    while(itr != netConfigDefaults.end())
    {
      auto found = netConfig.find(itr->first);
      if(found == netConfig.end() || found->second.empty())
      {
        auto val = itr->second();
        if(!val.empty())
          netConfig.emplace(itr->first, std::move(val));
      }
      ++itr;
    }
  }

  bool
  Router::StartJsonRpc()
  {
    if(_running || _stopping)
      return false;

    if(enableRPCServer)
    {
      if(rpcBindAddr.empty())
      {
        rpcBindAddr = DefaultRPCBindAddr;
      }
      rpcServer = std::make_unique< rpc::Server >(this);
      while(!rpcServer->Start(rpcBindAddr))
      {
        LogError("failed to bind jsonrpc to ", rpcBindAddr);
#if defined(ANDROID) || defined(RPI)
        sleep(1);
#else
        std::this_thread::sleep_for(std::chrono::seconds(1));
#endif
      }
      LogInfo("Bound RPC server to ", rpcBindAddr);
    }

    return true;
  }

  bool
  Router::Run()
  {
    if(_running || _stopping)
      return false;

    if(whitelistRouters)
    {
      rpcCaller = std::make_unique< rpc::Caller >(this);
      rpcCaller->SetAuth(lokidRPCUser, lokidRPCPassword);
      while(!rpcCaller->Start(lokidRPCAddr))
      {
        LogError("failed to start jsonrpc caller to ", lokidRPCAddr);
#if defined(ANDROID) || defined(RPI)
        sleep(1);
#else
        std::this_thread::sleep_for(std::chrono::seconds(1));
#endif
      }
      LogInfo("RPC Caller to ", lokidRPCAddr, " started");
    }

    if(!cryptoworker->start())
    {
      LogError("crypto worker failed to start");
      return false;
    }

    if(!disk->start())
    {
      LogError("disk worker failed to start");
      return false;
    }

    routerProfiling().Load(routerProfilesFile.c_str());

    Addr publicAddr(this->addrInfo);

    if(this->publicOverride)
    {
      LogDebug("public address:port ", publicAddr);
    }

    // set public signing key
    _rc.pubkey = seckey_topublic(identity());

    AddressInfo ai;
    _linkManager.ForEachInboundLink([&](LinkLayer_ptr link) {
      if(link->GetOurAddressInfo(ai))
      {
        // override ip and port
        if(this->publicOverride)
        {
          ai.ip   = *publicAddr.addr6();
          ai.port = publicAddr.port();
        }
        if(RouterContact::BlockBogons && IsBogon(ai.ip))
          return;
        _rc.addrs.push_back(ai);
        if(ExitEnabled())
        {
          const llarp::Addr addr(ai);
          const nuint32_t a{addr.addr4()->s_addr};
          _rc.exits.emplace_back(_rc.pubkey, a);
          LogInfo("Exit relay started, advertised as exiting at: ", a);
        }
      }
    });

    // set public encryption key
    _rc.enckey = seckey_topublic(encryption());

    LogInfo("Signing rc...");
    if(!_rc.Sign(identity()))
    {
      LogError("failed to sign rc");
      return false;
    }

    if(!SaveRC())
    {
      LogError("failed to save RC");
      return false;
    }
    _outboundSessionMaker.SetOurRouter(pubkey());
    if(!_linkManager.StartLinks(_logic, cryptoworker))
    {
      LogWarn("One or more links failed to start.");
      return false;
    }

    EnsureNetConfigDefaultsSane(netConfig);

    const auto limits =
        IsServiceNode() ? llarp::limits::snode : llarp::limits::client;

    _outboundSessionMaker.minConnectedRouters = std::max(
        _outboundSessionMaker.minConnectedRouters, limits.DefaultMinRouters);
    _outboundSessionMaker.maxConnectedRouters = std::max(
        _outboundSessionMaker.maxConnectedRouters, limits.DefaultMaxRouters);

    if(IsServiceNode())
    {
      // initialize as service node
      if(!InitServiceNode())
      {
        LogError("Failed to initialize service node");
        return false;
      }
      RouterID us = pubkey();
      LogInfo("initalized service node: ", us);

      // relays do not use profiling
      routerProfiling().Disable();
    }
    else
    {
      // we are a client
      // regenerate keys and resign rc before everything else
      CryptoManager::instance()->identity_keygen(_identity);
      CryptoManager::instance()->encryption_keygen(_encryption);
      _rc.pubkey = seckey_topublic(identity());
      _rc.enckey = seckey_topublic(encryption());
      if(!_rc.Sign(identity()))
      {
        LogError("failed to regenerate keys and sign RC");
        return false;
      }

      if(!CreateDefaultHiddenService())
      {
        LogError("failed to set up default network endpoint");
        return false;
      }
    }

    LogInfo("starting hidden service context...");
    if(!hiddenServiceContext().StartAll())
    {
      LogError("Failed to start hidden service context");
      return false;
    }

    llarp_dht_context_start(dht(), pubkey());

    for(const auto &rc : bootstrapRCList)
    {
      if(this->nodedb()->Insert(rc))
      {
        LogInfo("added bootstrap node ", RouterID(rc.pubkey));
      }
      else
      {
        LogError("Failed to add bootstrap node ", RouterID(rc.pubkey));
      }
      _dht->impl->Nodes()->PutNode(rc);
    }

    LogInfo("have ", _nodedb->num_loaded(), " routers");

    _netloop->add_ticker(std::bind(&Router::PumpLL, this));

    ScheduleTicker(1000);
    _running.store(true);
    _startedAt = Now();
    return _running;
  }

  bool
  Router::IsRunning() const
  {
    return _running;
  }

  llarp_time_t
  Router::Uptime() const
  {
    const llarp_time_t _now = Now();
    if(_startedAt && _now > _startedAt)
      return _now - _startedAt;
    return 0;
  }

  static void
  RouterAfterStopLinks(void *u, uint64_t, uint64_t)
  {
    auto *self = static_cast< Router * >(u);
    self->Close();
  }

  static void
  RouterAfterStopIssued(void *u, uint64_t, uint64_t)
  {
    auto *self = static_cast< Router * >(u);
    self->StopLinks();
    self->nodedb()->AsyncFlushToDisk();
    self->_logic->call_later({200, self, &RouterAfterStopLinks});
  }

  void
  Router::StopLinks()
  {
    _linkManager.Stop();
  }

  void
  Router::Stop()
  {
    if(!_running)
      return;
    if(_stopping)
      return;

    _stopping.store(true);
    LogInfo("stopping router");
    hiddenServiceContext().StopAll();
    _exitContext.Stop();
    if(rpcServer)
      rpcServer->Stop();
    paths.PumpUpstream();
    _linkManager.PumpLinks();
    _logic->call_later({200, this, &RouterAfterStopIssued});
  }

  bool
  Router::HasSessionTo(const RouterID &remote) const
  {
    return _linkManager.HasSessionTo(remote);
  }

  void
  Router::ConnectToRandomRouters(int want)
  {
    _outboundSessionMaker.ConnectToRandomRouters(want, Now());
  }

  bool
  Router::InitServiceNode()
  {
    LogInfo("accepting transit traffic");
    paths.AllowTransit();
    llarp_dht_allow_transit(dht());
    return _exitContext.AddExitEndpoint("default-connectivity", netConfig);
  }

  bool
  Router::ValidateConfig(ABSL_ATTRIBUTE_UNUSED Config *conf) const
  {
    return true;
  }

  bool
  Router::Reconfigure(Config *)
  {
    // TODO: implement me
    return true;
  }

  bool
  Router::TryConnectAsync(RouterContact rc, uint16_t tries)
  {
    (void)tries;

    if(rc.pubkey == pubkey())
    {
      return false;
    }

    if(!_rcLookupHandler.RemoteIsAllowed(rc.pubkey))
    {
      return false;
    }

    _outboundSessionMaker.CreateSessionTo(rc, nullptr);

    return true;
  }

  bool
  Router::InitOutboundLinks()
  {
    const auto linkTypeName = LinkFactory::NameFromType(_defaultLinkType);
    LogInfo("initialize outbound link: ", linkTypeName);
    auto factory = LinkFactory::Obtain(_defaultLinkType, false);
    if(factory == nullptr)
    {
      LogError("cannot initialize outbound link of type '", linkTypeName,
               "' as it has no implementation");
      return false;
    }
    auto link =
        factory(encryption(), util::memFn(&AbstractRouter::rc, this),
                util::memFn(&AbstractRouter::HandleRecvLinkMessageBuffer, this),
                util::memFn(&AbstractRouter::Sign, this),
                util::memFn(&IOutboundSessionMaker::OnSessionEstablished,
                            &_outboundSessionMaker),
                util::memFn(&AbstractRouter::CheckRenegotiateValid, this),
                util::memFn(&IOutboundSessionMaker::OnConnectTimeout,
                            &_outboundSessionMaker),
                util::memFn(&AbstractRouter::SessionClosed, this),
                util::memFn(&AbstractRouter::PumpLL, this));

    if(!link)
      return false;
    if(!link->EnsureKeys(transport_keyfile.string().c_str()))
    {
      LogError("failed to load ", transport_keyfile);
      return false;
    }

    const auto afs = {AF_INET, AF_INET6};

    for(const auto af : afs)
    {
      if(!link->Configure(netloop(), "*", af, m_OutboundPort))
        continue;
      _linkManager.AddLink(std::move(link), false);
      return true;
    }
    return false;
  }

  bool
  Router::CreateDefaultHiddenService()
  {
    // add endpoint
    return hiddenServiceContext().AddDefaultEndpoint(netConfig);
  }

  bool
  Router::LoadHiddenServiceConfig(string_view fname)
  {
    LogDebug("opening hidden service config ", fname);
    service::Config conf;
    if(!conf.Load(fname))
      return false;
    for(const auto &config : conf.services)
    {
      service::Config::section_t filteredConfig;
      mergeHiddenServiceConfig(config.second, filteredConfig.second);
      filteredConfig.first = config.first;
      if(!hiddenServiceContext().AddEndpoint(filteredConfig))
        return false;
    }
    return true;
  }

  void
  Router::MessageSent(const RouterID &remote, SendStatus status)
  {
    if(status == SendStatus::Success)
    {
      LogDebug("Message successfully sent to ", remote);
    }
    else
    {
      LogDebug("Message failed sending to ", remote);
    }
  }
}  // namespace llarp
