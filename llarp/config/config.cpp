#include <chrono>
#include <config/config.hpp>

#include <config/ini.hpp>
#include <constants/defaults.hpp>
#include <constants/files.hpp>
#include <net/net.hpp>
#include <router_contact.hpp>
#include <stdexcept>
#include <util/fs.hpp>
#include <util/logging/logger.hpp>
#include <util/mem.hpp>
#include <util/str.hpp>

#include <cstdlib>
#include <fstream>
#include <ios>
#include <iostream>
#include <filesystem>

namespace llarp
{
  // constants for config file default values
  constexpr int DefaultMinConnectionsForRouter = 6;
  constexpr int DefaultMaxConnectionsForRouter = 60;

  constexpr int DefaultMinConnectionsForClient = 4;
  constexpr int DefaultMaxConnectionsForClient = 6;

  void
  RouterConfig::defineConfigOptions(ConfigDefinition& conf, const ConfigGenParameters& params)
  {
    constexpr int DefaultJobQueueSize = 1024 * 8;
    constexpr auto DefaultNetId = "lokinet";
    constexpr int DefaultPublicPort = 1090;
    constexpr int DefaultWorkerThreads = 1;
    constexpr int DefaultNetThreads = 1;
    constexpr bool DefaultBlockBogons = true;

    conf.defineOption<int>("router", "job-queue-size", false, DefaultJobQueueSize, [this](int arg) {
      if (arg < 1024)
        throw std::invalid_argument("job-queue-size must be 1024 or greater");

      m_JobQueueSize = arg;
    });

    conf.defineOption<std::string>("router", "netid", false, DefaultNetId, [this](std::string arg) {
      if (arg.size() > NetID::size())
        throw std::invalid_argument(stringify("netid is too long, max length is ", NetID::size()));

      m_netId = std::move(arg);
    });

    int minConnections =
        (params.isRelay ? DefaultMinConnectionsForRouter : DefaultMinConnectionsForClient);
    conf.defineOption<int>("router", "min-connections", false, minConnections, [=](int arg) {
      if (arg < minConnections)
        throw std::invalid_argument(stringify("min-connections must be >= ", minConnections));

      m_minConnectedRouters = arg;
    });

    int maxConnections =
        (params.isRelay ? DefaultMaxConnectionsForRouter : DefaultMaxConnectionsForClient);
    conf.defineOption<int>("router", "max-connections", false, maxConnections, [=](int arg) {
      if (arg < maxConnections)
        throw std::invalid_argument(stringify("max-connections must be >= ", maxConnections));

      m_maxConnectedRouters = arg;
    });

    conf.defineOption<std::string>("router", "nickname", false, "", AssignmentAcceptor(m_nickname));

    conf.defineOption<fs::path>(
        "router", "data-dir", false, params.defaultDataDir, [this](fs::path arg) {
          if (not fs::exists(arg))
            throw std::runtime_error(
                stringify("Specified [router]:data-dir ", arg, " does not exist"));

          m_dataDir = std::move(arg);
        });

    conf.defineOption<std::string>("router", "public-address", false, "", [this](std::string arg) {
      if (not arg.empty())
      {
        llarp::LogInfo("public ip ", arg, " size ", arg.size());

        if (arg.size() > 16)
          throw std::invalid_argument(stringify("Not a valid IPv4 addr: ", arg));

        // assume IPv4
        llarp::Addr a(arg);
        llarp::LogInfo("setting public ipv4 ", a);
        m_addrInfo.ip = *a.addr6();
        m_publicOverride = true;
      }
    });

    conf.defineOption<int>("router", "public-port", false, DefaultPublicPort, [this](int arg) {
      if (arg <= 0)
        throw std::invalid_argument("public-port must be > 0");

      // Not needed to flip upside-down - this is done in llarp::Addr(const AddressInfo&)
      m_ip4addr.sin_port = arg;
      m_addrInfo.port = arg;
      m_publicOverride = true;
    });

    conf.defineOption<int>(
        "router", "worker-threads", false, DefaultWorkerThreads, [this](int arg) {
          if (arg <= 0)
            throw std::invalid_argument("worker-threads must be > 0");

          m_workerThreads = arg;
        });

    conf.defineOption<int>("router", "net-threads", false, DefaultNetThreads, [this](int arg) {
      if (arg <= 0)
        throw std::invalid_argument("net-threads must be > 0");

      m_numNetThreads = arg;
    });

    conf.defineOption<bool>(
        "router", "block-bogons", false, DefaultBlockBogons, AssignmentAcceptor(m_blockBogons));

    conf.defineOption<std::string>(
        "router", "contact-file", false, "", AssignmentAcceptor(m_routerContactFile));

    conf.defineOption<std::string>(
        "router", "encryption-privkey", false, "", AssignmentAcceptor(m_encryptionKeyFile));

    conf.defineOption<std::string>(
        "router", "ident-privkey", false, "", AssignmentAcceptor(m_identityKeyFile));

    conf.defineOption<std::string>(
        "router", "transport-privkey", false, "", AssignmentAcceptor(m_transportKeyFile));
  }

  void
  NetworkConfig::defineConfigOptions(ConfigDefinition& conf, const ConfigGenParameters& params)
  {
    (void)params;

    constexpr bool DefaultProfilingValue = true;
    static constexpr bool ReachableDefault = false;
    static constexpr int HopsDefault = 4;
    static constexpr int PathsDefault = 6;

    conf.defineOption<bool>(
        "network",
        "profiling",
        false,
        DefaultProfilingValue,
        AssignmentAcceptor(m_enableProfiling));

    // TODO: this should be implied from [router]:data-dir
    conf.defineOption<std::string>(
        "network",
        "profiles",
        false,
        m_routerProfilesFile,
        AssignmentAcceptor(m_routerProfilesFile));

    conf.defineOption<std::string>(
        "network", "strict-connect", false, "", AssignmentAcceptor(m_strictConnect));

    conf.defineOption<std::string>(
        "network", "keyfile", false, "", [this](std::string arg) { m_keyfile = arg; });

    conf.defineOption<bool>(
        "network", "reachable", false, ReachableDefault, AssignmentAcceptor(m_reachable));

    conf.defineOption<int>("network", "hops", false, HopsDefault, [this](int arg) {
      if (arg < 1 or arg > 8)
        throw std::invalid_argument("[endpoint]:hops must be >= 1 and <= 8");
    });

    conf.defineOption<int>("network", "paths", false, PathsDefault, [this](int arg) {
      if (arg < 1 or arg > 8)
        throw std::invalid_argument("[endpoint]:paths must be >= 1 and <= 8");
    });

#ifdef LOKINET_EXITS
    conf.defineOption<std::string>("network", "exit-node", false, "", [this](std::string arg) {
      // TODO: validate as valid .loki / .snode address
      //       probably not .snode...?
      m_exitNode = arg;
    });
#endif

    conf.defineOption<std::string>("network", "mapaddr", false, "", [this](std::string arg) {
      // TODO: parse / validate as loki_addr : IP addr pair
      m_mapAddr = arg;
    });

    conf.defineOption<std::string>("network", "ifaddr", false, "", [this](std::string arg) {
      if (arg.empty())
      {
        const auto maybe = llarp::FindFreeRange();
        if (not maybe.has_value())
          throw std::invalid_argument("cannot determine free ip range");
        arg = maybe.value();
      }
      m_ifaddr = arg;
    });

    conf.defineOption<std::string>("network", "ifname", false, "", [this](std::string arg) {
      if (arg.empty())
      {
        const auto maybe = llarp::FindFreeTun();
        if (not maybe.has_value())
          throw std::invalid_argument("cannot determine free interface name");
        arg = maybe.value();
      }
      m_ifname = arg;
    });

    conf.defineOption<std::string>(
        "network", "blacklist-snode", false, true, "", [this](std::string arg) {
          RouterID id;
          if (not id.FromString(arg))
            throw std::invalid_argument(stringify("Invalid RouterID: ", arg));

          auto itr = m_snodeBlacklist.emplace(std::move(id));
          if (itr.second)
            throw std::invalid_argument(stringify("Duplicate blacklist-snode: ", arg));
        });
  }

  void
  DnsConfig::defineConfigOptions(ConfigDefinition& conf, const ConfigGenParameters& params)
  {
    (void)params;

    auto parseAddr = [](std::string input) {
      Addr addr;
      bool success = addr.from_char_array(input);
      if (not success)
        throw std::invalid_argument(stringify(input, " is not a valid address"));

      // default port if it wasn't specified
      if (input.find(":") == std::string::npos)
        addr.port(53);

      return addr;
    };

    conf.defineOption<std::string>(
        "dns", "upstream", false, true, std::nullopt, [=](std::string arg) {
          // m_upstreamDNS.push_back(parseAddr(arg));
        });

    conf.defineOption<std::string>(
        "dns", "bind", false, std::nullopt, [=](std::string arg) { /*m_bind = parseAddr(arg);*/ });
  }

  LinksConfig::LinkInfo
  LinksConfig::LinkInfoFromINIValues(std::string_view name, std::string_view value)
  {
    // we treat the INI k:v pair as:
    // k: interface name, * indicating outbound
    // v: a comma-separated list of values, an int indicating port (everything else ignored)
    //    this is somewhat of a backwards- and forwards-compatibility thing

    LinkInfo info;
    info.port = 0;
    info.addressFamily = AF_INET;
    info.interface = str(name);

    std::vector<std::string_view> splits = split(value, ',');
    for (std::string_view str : splits)
    {
      int asNum = std::atoi(str.data());
      if (asNum > 0)
        info.port = asNum;

      // otherwise, ignore ("future-proofing")
    }

    return info;
  }

  void
  LinksConfig::defineConfigOptions(ConfigDefinition& conf, const ConfigGenParameters& params)
  {
    (void)params;

    constexpr auto DefaultOutboundLinkValue = "0";

    conf.defineOption<std::string>(
        "bind", "*", false, false, DefaultOutboundLinkValue, [this](std::string arg) {
          m_OutboundLink = LinkInfoFromINIValues("*", arg);
        });

    conf.addUndeclaredHandler(
        "bind", [&](std::string_view, std::string_view name, std::string_view value) {
          LinkInfo info = LinkInfoFromINIValues(name, value);

          if (info.port <= 0)
            throw std::invalid_argument(
                stringify("Invalid [bind] port specified on interface", name));

          assert(name != "*");  // handled by defineOption("bind", "*", ...) above

          m_InboundLinks.emplace_back(std::move(info));
        });
  }

  void
  ConnectConfig::defineConfigOptions(ConfigDefinition& conf, const ConfigGenParameters& params)
  {
    (void)params;

    conf.addUndeclaredHandler(
        "connect", [this](std::string_view section, std::string_view name, std::string_view value) {
          fs::path file = str(value);
          if (not fs::exists(file))
            throw std::runtime_error(stringify(
                "Specified bootstrap file ",
                value,
                "specified in [",
                section,
                "]:",
                name,
                " does not exist"));

          routers.emplace_back(std::move(file));
          return true;
        });
  }

  void
  ApiConfig::defineConfigOptions(ConfigDefinition& conf, const ConfigGenParameters& params)
  {
    (void)params;

    constexpr bool DefaultRPCEnabled = true;
    constexpr auto DefaultRPCBindAddr = "127.0.0.1:1190";

    conf.defineOption<bool>(
        "api", "enabled", false, DefaultRPCEnabled, AssignmentAcceptor(m_enableRPCServer));

    conf.defineOption<std::string>(
        "api", "bind", false, DefaultRPCBindAddr, AssignmentAcceptor(m_rpcBindAddr));

    // TODO: this was from pre-refactor:
    // TODO: add pubkey to whitelist
  }

  void
  LokidConfig::defineConfigOptions(ConfigDefinition& conf, const ConfigGenParameters& params)
  {
    (void)params;

    constexpr bool DefaultWhitelistRouters = false;
    constexpr auto DefaultLokidRPCAddr = "127.0.0.1:22023";

    conf.defineOption<std::string>(
        "lokid", "service-node-seed", false, our_identity_filename, [this](std::string arg) {
          if (not arg.empty())
          {
            usingSNSeed = true;
            ident_keyfile = std::move(arg);
          }
        });

    conf.defineOption<bool>(
        "lokid", "enabled", false, DefaultWhitelistRouters, AssignmentAcceptor(whitelistRouters));

    conf.defineOption<std::string>(
        "lokid", "jsonrpc", false, DefaultLokidRPCAddr, AssignmentAcceptor(lokidRPCAddr));

    conf.defineOption<std::string>(
        "lokid", "username", false, "", AssignmentAcceptor(lokidRPCUser));

    conf.defineOption<std::string>(
        "lokid", "password", false, "", AssignmentAcceptor(lokidRPCPassword));
  }

  void
  BootstrapConfig::defineConfigOptions(ConfigDefinition& conf, const ConfigGenParameters& params)
  {
    (void)params;

    conf.defineOption<std::string>(
        "bootstrap", "add-node", false, true, "", [this](std::string arg) {
          // TODO: validate as router fs path
          routers.emplace_back(std::move(arg));
        });
  }

  void
  LoggingConfig::defineConfigOptions(ConfigDefinition& conf, const ConfigGenParameters& params)
  {
    (void)params;

    constexpr auto DefaultLogType = "file";
    constexpr auto DefaultLogFile = "stdout";
    constexpr auto DefaultLogLevel = "info";

    conf.defineOption<std::string>(
        "logging", "type", false, DefaultLogType, [this](std::string arg) {
          LogType type = LogTypeFromString(arg);
          if (type == LogType::Unknown)
            throw std::invalid_argument(stringify("invalid log type: ", arg));

          m_logType = type;
        });

    conf.defineOption<std::string>(
        "logging", "level", false, DefaultLogLevel, [this](std::string arg) {
          std::optional<LogLevel> level = LogLevelFromString(arg);
          if (not level.has_value())
            throw std::invalid_argument(stringify("invalid log level value: ", arg));

          m_logLevel = level.value();
        });

    conf.defineOption<std::string>(
        "logging", "file", false, DefaultLogFile, AssignmentAcceptor(m_logFile));
  }

  bool
  Config::Load(const char* fname, bool isRelay, fs::path defaultDataDir)
  {
    try
    {
      ConfigGenParameters params;
      params.isRelay = isRelay;
      params.defaultDataDir = std::move(defaultDataDir);

      ConfigDefinition conf;
      initializeConfig(conf, params);
      addBackwardsCompatibleConfigOptions(conf);

      ConfigParser parser;
      if (!parser.LoadFile(fname))
      {
        return false;
      }

      parser.IterAll([&](std::string_view section, const SectionValues_t& values) {
        for (const auto& pair : values)
        {
          conf.addConfigValue(section, pair.first, pair.second);
        }
      });

      conf.acceptAllOptions();

      // TODO: better way to support inter-option constraints
      if (router.m_maxConnectedRouters < router.m_minConnectedRouters)
        throw std::invalid_argument("[router]:min-connections must be <= [router]:max-connections");

      return true;
    }
    catch (const std::exception& e)
    {
      LogError("Error trying to init and parse config from file: ", e.what());
      return false;
    }
  }

  bool
  Config::LoadDefault(bool isRelay, fs::path dataDir)
  {
    try
    {
      ConfigGenParameters params;
      params.isRelay = isRelay;
      params.defaultDataDir = std::move(dataDir);

      ConfigDefinition conf;
      initializeConfig(conf, params);

      conf.acceptAllOptions();

      return true;
    }
    catch (const std::exception& e)
    {
      LogError("Error trying to init default config: ", e.what());
      return false;
    }
  }

  void
  Config::initializeConfig(ConfigDefinition& conf, const ConfigGenParameters& params)
  {
    router.defineConfigOptions(conf, params);
    network.defineConfigOptions(conf, params);
    connect.defineConfigOptions(conf, params);
    dns.defineConfigOptions(conf, params);
    links.defineConfigOptions(conf, params);
    api.defineConfigOptions(conf, params);
    lokid.defineConfigOptions(conf, params);
    bootstrap.defineConfigOptions(conf, params);
    logging.defineConfigOptions(conf, params);
  }

  void
  Config::addBackwardsCompatibleConfigOptions(ConfigDefinition& conf)
  {
    auto addIgnoreOption = [&](const std::string& section, const std::string& name) {
      conf.defineOption<std::string>(section, name, false, true, "", [=](std::string arg) {
        (void)arg;
        LogWarn("*** WARNING: The config option [", section, "]:", name, " is deprecated");
      });
    };

    addIgnoreOption("system", "user");
    addIgnoreOption("system", "group");
    addIgnoreOption("system", "pidfile");

    addIgnoreOption("api", "authkey");

    addIgnoreOption("netdb", "dir");

    // these weren't even ever used!
    addIgnoreOption("router", "max-routers");
    addIgnoreOption("router", "min-routers");

    // TODO: this may have been a synonym for [router]worker-threads
    addIgnoreOption("router", "threads");

    addIgnoreOption("metrics", "json-metrics-path");

    addIgnoreOption("network", "enabled");
  }

  void
  ensureConfig(
      const fs::path& defaultDataDir, const fs::path& confFile, bool overwrite, bool asRouter)
  {
    std::error_code ec;

    // fail to overwrite if not instructed to do so
    if (fs::exists(confFile, ec) && !overwrite)
    {
      LogDebug("Not creating config file; it already exists.");
      return;
    }

    if (ec)
      throw std::runtime_error(stringify("filesystem error: ", ec));

    // create parent dir if it doesn't exist
    if (not fs::exists(confFile.parent_path(), ec))
    {
      if (not fs::create_directory(confFile.parent_path()))
        throw std::runtime_error(stringify("Failed to create parent directory for ", confFile));
    }
    if (ec)
      throw std::runtime_error(stringify("filesystem error: ", ec));

    llarp::LogInfo("Attempting to create config file, asRouter: ", asRouter, " path: ", confFile);

    llarp::Config config;
    std::string confStr;
    if (asRouter)
      confStr = config.generateBaseRouterConfig(std::move(defaultDataDir));
    else
      confStr = config.generateBaseClientConfig(std::move(defaultDataDir));

    // open a filestream
    auto stream = llarp::util::OpenFileStream<std::ofstream>(confFile.c_str(), std::ios::binary);
    if (not stream.has_value() or not stream.value().is_open())
      throw std::runtime_error(stringify("Failed to open file ", confFile, " for writing"));

    llarp::LogInfo("confStr: ", confStr);

    stream.value() << confStr;
    stream.value().flush();

    llarp::LogInfo("Generated new config ", confFile);
  }

  void
  generateCommonConfigComments(ConfigDefinition& def)
  {
    // router
    def.addSectionComments(
        "router",
        {
            "Configuration for routing activity.",
        });

    def.addOptionComments(
        "router",
        "threads",
        {
            "The number of threads available for performing cryptographic functions.",
            "The minimum is one thread, but network performance may increase with more.",
            "threads. Should not exceed the number of logical CPU cores.",
        });

    def.addOptionComments(
        "router",
        "data-dir",
        {
            "Optional directory for containing lokinet runtime data. This includes generated",
            "private keys.",
        });

    // TODO: why did Kee want this, and/or what does it really do? Something about logs?
    def.addOptionComments("router", "nickname", {"Router nickname. Kee wanted it."});

    def.addOptionComments(
        "router",
        "min-connections",
        {
            "Minimum number of routers lokinet will attempt to maintain connections to.",
        });

    def.addOptionComments(
        "router",
        "max-connections",
        {
            "Maximum number (hard limit) of routers lokinet will be connected to at any time.",
        });

    // logging
    def.addSectionComments(
        "logging",
        {
            "logging settings",
        });

    def.addOptionComments(
        "logging",
        "level",
        {
            "Minimum log level to print. Logging below this level will be ignored.",
            "Valid log levels, in ascending order, are:",
            "  trace",
            "  debug",
            "  info",
            "  warn",
            "  error",
        });

    def.addOptionComments(
        "logging",
        "type",
        {
            "Log type (format). Valid options are:",
            "  file - plaintext formatting",
            "  json - json-formatted log statements",
            "  syslog - logs directed to syslog",
        });

    // api
    def.addSectionComments(
        "api",
        {
            "JSON API settings",
        });

    def.addOptionComments(
        "api",
        "enabled",
        {
            "Determines whether or not the JSON API is enabled.",
        });

    def.addOptionComments(
        "api",
        "bind",
        {
            "IP address and port to bind to.",
            "Recommend localhost-only for security purposes.",
        });

    // dns
    def.addSectionComments(
        "dns",
        {
            "DNS configuration",
        });

    def.addOptionComments(
        "dns",
        "upstream-dns",
        {
            "Upstream resolver(s) to use as fallback for non-loki addresses.",
            "Multiple values accepted.",
        });

    def.addOptionComments(
        "dns",
        "bind",
        {
            "Address to bind to for handling DNS requests.",
            "Multiple values accepted.",
        });

    // bootstrap
    def.addSectionComments(
        "bootstrap",
        {
            "Configure nodes that will bootstrap us onto the network",
        });

    def.addOptionComments(
        "bootstrap",
        "add-node",
        {
            "Specify a bootstrap file containing a signed RouterContact of a service node",
            "which can act as a bootstrap. Accepts multiple values.",
        });

    // network
    def.addSectionComments(
        "network",
        {
            "Network settings",
        });

    def.addOptionComments(
        "network",
        "profiles",
        {
            "File to contain router profiles.",
        });

    def.addOptionComments(
        "network",
        "strict-connect",
        {
            "Public key of a router which will act as sole first-hop. This may be used to",
            "provide a trusted router (consider that you are not fully anonymous with your",
            "first hop).",
        });

    def.addOptionComments(
        "network",
        "exit-node",
        {
            "Public key of an exit-node.",
        });

    def.addOptionComments(
        "network",
        "ifname",
        {
            "Interface name for lokinet traffic.",
        });

    def.addOptionComments(
        "network",
        "ifaddr",
        {
            "Local IP address for lokinet traffic.",
        });
  }

  std::string
  Config::generateBaseClientConfig(fs::path defaultDataDir)
  {
    ConfigGenParameters params;
    params.isRelay = false;
    params.defaultDataDir = std::move(defaultDataDir);

    llarp::ConfigDefinition def;
    initializeConfig(def, params);
    generateCommonConfigComments(def);

    def.addSectionComments(
        "network",
        {
            "Snapp settings",
        });

    def.addOptionComments(
        "network",
        "keyfile",
        {
            "The private key to persist address with. If not specified the address will be",
            "ephemeral.",
        });

    // TODO: is this redundant with / should be merged with basic client config?
    def.addOptionComments(
        "network",
        "reachable",
        {
            "Determines whether we will publish our snapp's introset to the DHT.",
        });

    def.addOptionComments(
        "network",
        "hops",
        {
            "Number of hops in a path. Min 1, max 8.",
        });

    def.addOptionComments(
        "network",
        "paths",
        {
            "Number of paths to maintain at any given time.",
        });

    def.addOptionComments(
        "network",
        "blacklist-snode",
        {
            "Adds a `.snode` address to the blacklist.",
        });

#ifdef LOKINET_EXITS
    def.addOptionComments(
        "network",
        "exit-node",
        {
            "Specify a `.snode` or `.loki` address to use as an exit broker.",
        });
#endif

    def.addOptionComments(
        "network",
        "mapaddr",
        {
            "Permanently map a `.loki` address to an IP owned by the snapp. Example:",
            "mapaddr=whatever.loki:10.0.10.10 # maps `whatever.loki` to `10.0.10.10`.",
        });

    return def.generateINIConfig(true);
  }

  std::string
  Config::generateBaseRouterConfig(fs::path defaultDataDir)
  {
    ConfigGenParameters params;
    params.isRelay = true;
    params.defaultDataDir = std::move(defaultDataDir);

    llarp::ConfigDefinition def;
    initializeConfig(def, params);
    generateCommonConfigComments(def);

    // lokid
    def.addSectionComments(
        "lokid",
        {
            "Lokid configuration (settings for talking to lokid",
        });

    def.addOptionComments(
        "lokid",
        "enabled",
        {
            "Whether or not we should talk to lokid. Must be enabled for staked routers.",
        });

    def.addOptionComments(
        "lokid",
        "jsonrpc",
        {
            "Host and port of running lokid that we should talk to.",
        });

    // TODO: doesn't appear to be used in the codebase
    def.addOptionComments(
        "lokid",
        "service-node-seed",
        {
            "File containing service node's seed.",
        });

    // extra [network] options
    // TODO: probably better to create an [exit] section and only allow it for routers
    def.addOptionComments(
        "network",
        "exit",
        {
            "Whether or not we should act as an exit node. Beware that this increases demand",
            "on the server and may pose liability concerns. Enable at your own risk.",
        });

    // TODO: define the order of precedence (e.g. is whitelist applied before blacklist?)
    //       additionally, what's default? What if I don't whitelist anything?
    def.addOptionComments(
        "network",
        "exit-whitelist",
        {
            "List of destination protocol:port pairs to whitelist, example: udp:*",
            "or tcp:80. Multiple values supported.",
        });

    def.addOptionComments(
        "network",
        "exit-blacklist",
        {
            "Blacklist of destinations (same format as whitelist).",
        });

    return def.generateINIConfig(true);
  }

}  // namespace llarp
