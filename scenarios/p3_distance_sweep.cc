// p3_distance_sweep.cc
#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/wifi-module.h"
#include "ns3/csma-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/header.h"
#include "ns3/random-variable-stream.h"
#include "ns3/traffic-control-module.h"
#include "ns3/propagation-module.h"

#include <cmath>
#include <map>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

using namespace ns3;

// -----------------------------
// Small helpers
// -----------------------------
static std::string
ToLower (std::string s)
{
  std::transform (s.begin (), s.end (), s.begin (),
                  [] (unsigned char c) { return std::tolower (c); });
  return s;
}

static bool
FileExistsAndNonEmpty (const std::string& path)
{
  std::error_code ec;
  if (!std::filesystem::exists (path, ec))
    return false;
  if (std::filesystem::is_directory (path, ec))
    return false;
  return (std::filesystem::file_size (path, ec) > 0);
}

static std::string
DistanceTag (double d)
{
  // For distances like 1,5,10... => "d5m"
  // For non-integers => "d2p5m" (dot replaced by p)
  std::ostringstream oss;
  if (std::fabs (d - std::round (d)) < 1e-9)
    {
      oss << "d" << static_cast<int> (std::round (d)) << "m";
    }
  else
    {
      oss << std::fixed << std::setprecision (2) << d;
      std::string s = oss.str ();
      for (char& c : s)
        {
          if (c == '.')
            c = 'p';
        }
      return "d" + s + "m";
    }
  return oss.str ();
}

// -----------------------------
// RTT probe (same idea as your p2_baseline.cc)
// -----------------------------
class RttHeader : public Header
{
public:
  RttHeader () = default;

  void SetSeq (uint32_t s) { m_seq = s; }
  void SetTxTimeNs (uint64_t t) { m_txTimeNs = t; }

  uint32_t GetSeq () const { return m_seq; }
  uint64_t GetTxTimeNs () const { return m_txTimeNs; }

  static TypeId GetTypeId ()
  {
    static TypeId tid =
      TypeId ("ns3::RttHeader")
        .SetParent<Header> ()
        .AddConstructor<RttHeader> ();
    return tid;
  }

  TypeId GetInstanceTypeId () const override { return GetTypeId (); }

  uint32_t GetSerializedSize () const override { return 4 + 8; }

  void Serialize (Buffer::Iterator start) const override
  {
    start.WriteHtonU32 (m_seq);
    start.WriteHtonU64 (m_txTimeNs);
  }

  uint32_t Deserialize (Buffer::Iterator start) override
  {
    m_seq = start.ReadNtohU32 ();
    m_txTimeNs = start.ReadNtohU64 ();
    return GetSerializedSize ();
  }

  void Print (std::ostream& os) const override
  {
    os << "seq=" << m_seq << " txNs=" << m_txTimeNs;
  }

private:
  uint32_t m_seq{0};
  uint64_t m_txTimeNs{0};
};

class RttEchoServer : public Application
{
public:
  void Setup (uint16_t port) { m_port = port; }

private:
  void StartApplication () override
  {
    m_socket = Socket::CreateSocket (GetNode (), UdpSocketFactory::GetTypeId ());
    InetSocketAddress local (Ipv4Address::GetAny (), m_port);
    m_socket->Bind (local);
    m_socket->SetRecvCallback (MakeCallback (&RttEchoServer::HandleRead, this));
  }

  void StopApplication () override
  {
    if (m_socket)
      {
        m_socket->Close ();
        m_socket = nullptr;
      }
  }

  void HandleRead (Ptr<Socket> socket)
  {
    Address from;
    Ptr<Packet> p;
    while ((p = socket->RecvFrom (from)))
      {
        // Echo back as-is
        socket->SendTo (p, 0, from);
      }
  }

  uint16_t m_port{9000};
  Ptr<Socket> m_socket;
};

class RttEchoClient : public Application
{
public:
  void Setup (Ipv4Address peerIp,
              uint16_t peerPort,
              Time interval,
              uint32_t payloadSize,
              std::string csvPath,
              bool verbose)
  {
    m_peerIp = peerIp;
    m_peerPort = peerPort;
    m_interval = interval;
    m_payloadSize = payloadSize;
    m_csvPath = std::move (csvPath);
    m_verbose = verbose;
  }

private:
  void StartApplication () override
  {
    m_running = true;
    m_seq = 0;

    m_socket = Socket::CreateSocket (GetNode (), UdpSocketFactory::GetTypeId ());
    m_socket->Connect (InetSocketAddress (m_peerIp, m_peerPort));
    m_socket->SetRecvCallback (MakeCallback (&RttEchoClient::HandleRead, this));

    m_csv.open (m_csvPath, std::ios::out);
    if (m_csv.is_open ())
      {
        m_csv << "time_s,seq,rtt_ms\n";
        m_csv.flush ();
      }

    Send ();
  }

  void StopApplication () override
  {
    m_running = false;
    if (m_sendEvent.IsPending ())
  {
    Simulator::Cancel (m_sendEvent);
  }


    if (m_socket)
      {
        m_socket->Close ();
        m_socket = nullptr;
      }

    if (m_csv.is_open ())
      m_csv.close ();
  }

  void ScheduleNextTx ()
  {
    if (m_running)
      m_sendEvent = Simulator::Schedule (m_interval, &RttEchoClient::Send, this);
  }

  void Send ()
  {
    Ptr<Packet> p = Create<Packet> (m_payloadSize);

    RttHeader hdr;
    hdr.SetSeq (m_seq++);
    hdr.SetTxTimeNs (static_cast<uint64_t> (Simulator::Now ().GetNanoSeconds ()));
    p->AddHeader (hdr);

    m_socket->Send (p);
    ScheduleNextTx ();
  }

  void HandleRead (Ptr<Socket> socket)
  {
    Address from;
    Ptr<Packet> p;
    while ((p = socket->RecvFrom (from)))
      {
        RttHeader hdr;
        if (p->GetSize () >= hdr.GetSerializedSize ())
          {
            p->RemoveHeader (hdr);
            Time tx = NanoSeconds (static_cast<int64_t> (hdr.GetTxTimeNs ()));
            Time rtt = Simulator::Now () - tx;

            const double now = Simulator::Now ().GetSeconds ();
            const double rttMs = rtt.GetMilliSeconds ();

            if (m_csv.is_open ())
              {
                m_csv << now << "," << hdr.GetSeq () << "," << rttMs << "\n";
                m_csv.flush ();
              }

            if (m_verbose)
              {
                std::cout << "[RTT] t=" << now << "s seq=" << hdr.GetSeq ()
                          << " rtt=" << rttMs << " ms\n";
              }
          }
      }
  }

  Ipv4Address m_peerIp;
  uint16_t m_peerPort{9000};
  Time m_interval{MilliSeconds (200)};
  uint32_t m_payloadSize{32};
  std::string m_csvPath{"rtt.csv"};
  bool m_verbose{false};

  Ptr<Socket> m_socket;
  EventId m_sendEvent;
  bool m_running{false};
  uint32_t m_seq{0};

  std::ofstream m_csv;
};

// -----------------------------
// Throughput sampling
// -----------------------------
static std::ofstream g_thrCsv;
static bool g_thrHeaderWritten = false;
static uint64_t g_lastRx = 0;
static double g_thrInterval = 0.5;

static void
SampleThroughput (Ptr<PacketSink> sink)
{
  const double now = Simulator::Now ().GetSeconds ();
  const uint64_t cur = sink->GetTotalRx ();
  const uint64_t delta = (cur >= g_lastRx) ? (cur - g_lastRx) : 0;

  const double thrBps = 8.0 * static_cast<double> (delta) / g_thrInterval;
  const double thrMbps = thrBps / 1e6;

  if (!g_thrHeaderWritten)
    {
      g_thrCsv << "time_s,throughput_Mbps,rxBytes_total\n";
      g_thrHeaderWritten = true;
    }
  g_thrCsv << now << "," << thrMbps << "," << cur << "\n";
  g_thrCsv.flush ();

  g_lastRx = cur;
  Simulator::Schedule (Seconds (g_thrInterval), &SampleThroughput, sink);
}

// Parse RTT CSV and compute mean + p95
static void
ComputeRttStats (const std::string& rttCsvPath, double& meanMs, double& p95Ms, uint32_t& samples)
{
  // -1 means "no RTT samples"
  meanMs = -1.0;
  p95Ms = -1.0;
  samples = 0;

  std::ifstream in (rttCsvPath);
  if (!in.is_open ())
    return;

  std::string line;
  // skip header
  std::getline (in, line);

  std::vector<double> rtts;
  rtts.reserve (1024);

  while (std::getline (in, line))
    {
      if (line.empty ())
        continue;

      // columns: time_s,seq,rtt_ms
      std::stringstream ss (line);
      std::string tok;

      // time_s
      std::getline (ss, tok, ',');
      // seq
      std::getline (ss, tok, ',');
      // rtt_ms
      std::getline (ss, tok, ',');

      if (tok.empty ())
        continue;

      try
        {
          const double r = std::stod (tok);
          if (std::isfinite (r) && r >= 0.0)
            rtts.push_back (r);
        }
      catch (...)
        {
          // ignore malformed row
        }
    }

  samples = static_cast<uint32_t> (rtts.size ());
  if (rtts.empty ())
    return; // keep -1 values

  const double sum = std::accumulate (rtts.begin (), rtts.end (), 0.0);
  meanMs = sum / static_cast<double> (rtts.size ());

  std::sort (rtts.begin (), rtts.end ());
  const size_t idx = static_cast<size_t> (std::ceil (0.95 * rtts.size ())) - 1;
  p95Ms = rtts[std::min (idx, rtts.size () - 1)];
}


class CorrelatedLogNormalShadowingLossModel : public ns3::PropagationLossModel
{
public:
  static ns3::TypeId GetTypeId ()
  {
    static ns3::TypeId tid =
      ns3::TypeId ("ns3::CorrelatedLogNormalShadowingLossModel")
        .SetParent<ns3::PropagationLossModel> ()
        .AddConstructor<CorrelatedLogNormalShadowingLossModel> ()
        .AddAttribute ("SigmaDb",
                       "Shadowing standard deviation in dB (Normal(0,sigma)).",
                       ns3::DoubleValue (5.0),
                       ns3::MakeDoubleAccessor (&CorrelatedLogNormalShadowingLossModel::m_sigmaDb),
                       ns3::MakeDoubleChecker<double> (0.0))
        .AddAttribute ("UpdatePeriod",
                       "How often (time) the shadowing value can change for a given link.",
                       ns3::TimeValue (ns3::Seconds (1.0)),
                       ns3::MakeTimeAccessor (&CorrelatedLogNormalShadowingLossModel::m_updatePeriod),
                       ns3::MakeTimeChecker ());
    return tid;
  }

  CorrelatedLogNormalShadowingLossModel ()
  {
    m_normal = ns3::CreateObject<ns3::NormalRandomVariable> ();
    m_normal->SetAttribute ("Mean", ns3::DoubleValue (0.0));
    // Variance را بعداً بر اساس SigmaDb تنظیم می‌کنیم
  }

private:
  struct LinkKey
{
  uintptr_t a;
  uintptr_t b;
  bool operator< (const LinkKey& o) const
  {
    return (a < o.a) || (a == o.a && b < o.b);
  }
};


  struct LinkState
  {
    double shadowDb{0.0};
    ns3::Time nextUpdate{ns3::Seconds (0)};
  };

  int64_t DoAssignStreams (int64_t stream) override
  {
    m_normal->SetStream (stream);
    return 1;
  }

  double DoCalcRxPower (double txPowerDbm,
                      ns3::Ptr<ns3::MobilityModel> a,
                      ns3::Ptr<ns3::MobilityModel> b) const override
{
  // Use the pointer identity of MobilityModels as a stable per-link key
  uintptr_t pa = reinterpret_cast<uintptr_t> (PeekPointer (a));
  uintptr_t pb = reinterpret_cast<uintptr_t> (PeekPointer (b));
  LinkKey key{std::min (pa, pb), std::max (pa, pb)};

  const ns3::Time now = ns3::Simulator::Now ();

  auto& st = m_links[key];
  if (now >= st.nextUpdate)
    {
      // Variance = sigma^2  (NormalRandomVariable uses Mean/Variance)
      const double var = m_sigmaDb * m_sigmaDb;
      m_normal->SetAttribute ("Variance", ns3::DoubleValue (var));
      st.shadowDb = m_normal->GetValue (); // Normal(0, sigmaDb)
      st.nextUpdate = now + m_updatePeriod;
    }

  // Shadowing is an additional LOSS in dB: subtract from received power
  return txPowerDbm - st.shadowDb;
}


  mutable std::map<LinkKey, LinkState> m_links;
  ns3::Ptr<ns3::NormalRandomVariable> m_normal;

  double m_sigmaDb{5.0};
  ns3::Time m_updatePeriod{ns3::Seconds (1.0)};
};

// -----------------------------
// Main
// -----------------------------

// Realism knobs
double txPowerDbm = 16.0;      // Typical Wi-Fi AP/STA power (dBm)
double noiseFigureDb = 7.0;    // Typical client NF
double shadowingSigmaDb = 5.0; // Indoor shadowing std-dev (dB)
bool enableFading = true;      // multipath fading
bool useMinstrel = true;       // realistic rate adaptation

// Queue realism (avoid bufferbloat)
std::string wifiMacQueueMaxSize = "50p";   // Wi-Fi MAC queue (smaller is more realistic)
double wifiMacQueueMaxDelayMs = 50.0;      // ms

// AQM (closer to real Linux routers / hosts)
bool enableAqm = true;
std::string aqmQueueDisc = "ns3::FqCoDelQueueDisc";
std::string aqmMaxSize = "1000p"; // queue disc limit (packets)


int
main (int argc, char* argv[])
{
  // Baseline params (similar to your p2)
  double simTime = 20.0;
  double appStart = 2.0;
  double distance = 5.0;

  std::string ssidStr = "wifi6-ter";
  std::string outDir = "results/p3";

  bool pcap = true;
  bool flowmon = true;

  int seed = 1;
  int run = 1;

  std::string transport = "udp"; // udp or tcp
  int pktSize = 1200;
  std::string udpRate = "50Mbps";
  uint64_t tcpMaxBytes = 0;

  double thrInterval = 0.5;
  double rttHz = 5.0;
  bool rttVerbose = false;

  std::string propModel = "logdistance"; // required
  double logExp = 3.0;
  double refDist = 1.0;
  double refLoss = 46.6777; // dB, ~Friis @ 5GHz around 1m (reasonable default)
  std::string tag = "";     // optional suffix

  CommandLine cmd;
  cmd.AddValue ("simTime", "Total simulation time (s)", simTime);
  cmd.AddValue ("appStart", "Start time of main traffic (s)", appStart);
  cmd.AddValue ("distance", "AP-STA distance (m)", distance);
  cmd.AddValue ("ssid", "Wi-Fi SSID", ssidStr);
  cmd.AddValue ("outDir", "Output directory (e.g., results/p3)", outDir);
  cmd.AddValue ("pcap", "Enable/disable PCAP", pcap);
  cmd.AddValue ("flowmon", "Enable/disable FlowMonitor", flowmon);
  cmd.AddValue ("seed", "RNG seed", seed);
  cmd.AddValue ("run", "RNG run number", run);

  cmd.AddValue ("transport", "Traffic type: udp or tcp", transport);
  cmd.AddValue ("pktSize", "Packet size (bytes)", pktSize);
  cmd.AddValue ("udpRate", "UDP offered rate (e.g., 50Mbps)", udpRate);
  cmd.AddValue ("tcpMaxBytes", "TCP MaxBytes (0=unlimited)", tcpMaxBytes);

  cmd.AddValue ("thrInterval", "Throughput sampling interval (s)", thrInterval);
  cmd.AddValue ("rttHz", "RTT probe frequency (Hz)", rttHz);
  cmd.AddValue ("rttVerbose", "Print RTT probe lines", rttVerbose);

  cmd.AddValue ("propModel", "Wi-Fi propagation loss: logdistance or friis", propModel);
  cmd.AddValue ("logExp", "LogDistance exponent", logExp);
  cmd.AddValue ("refDist", "LogDistance reference distance (m)", refDist);
  cmd.AddValue ("refLoss", "LogDistance reference loss at refDist (dB)", refLoss);
  cmd.AddValue ("tag", "Optional tag to add in filenames", tag);

  cmd.AddValue("txPowerDbm", "Wi-Fi Tx power (dBm)", txPowerDbm);
  cmd.AddValue("noiseFigureDb", "Receiver noise figure (dB)", noiseFigureDb);
  cmd.AddValue("shadowingSigmaDb", "LogNormal shadowing sigma (dB)", shadowingSigmaDb);
  cmd.AddValue("enableFading", "Enable Nakagami fading", enableFading);
  cmd.AddValue("useMinstrel", "Use MinstrelHt rate control (recommended)", useMinstrel);
  cmd.AddValue("wifiMacQueueMaxSize", "WifiMacQueue MaxSize (e.g., 200p)", wifiMacQueueMaxSize);
  cmd.AddValue("wifiMacQueueMaxDelayMs", "WifiMacQueue MaxDelay (ms)", wifiMacQueueMaxDelayMs);
cmd.AddValue("enableAqm", "Enable AQM (FqCoDel) on IP layer queues", enableAqm);
cmd.AddValue("aqmQueueDisc", "Root queue disc type (e.g., ns3::FqCoDelQueueDisc)", aqmQueueDisc);
cmd.AddValue("aqmMaxSize", "AQM queue disc MaxSize (e.g., 1000p)", aqmMaxSize);


  cmd.Parse (argc, argv);

  transport = ToLower (transport);
  propModel = ToLower (propModel);

  if (simTime <= 0.0 || appStart < 0.0 || appStart >= simTime)
    {
      std::cerr << "ERROR: invalid simTime/appStart\n";
      return 1;
    }
  if (distance <= 0.0)
    {
      std::cerr << "ERROR: distance must be > 0\n";
      return 1;
    }
  if (pktSize <= 0 || thrInterval <= 0.0 || rttHz <= 0.0)
    {
      std::cerr << "ERROR: invalid pktSize/thrInterval/rttHz\n";
      return 1;
    }
  if (transport != "udp" && transport != "tcp")
    {
      std::cerr << "ERROR: transport must be udp or tcp\n";
      return 1;
    }
  if (propModel != "logdistance" && propModel != "friis")
    {
      std::cerr << "ERROR: propModel must be logdistance or friis\n";
      return 1;
    }

  g_thrInterval = thrInterval;

  // RNG
  RngSeedManager::SetSeed (static_cast<uint32_t> (seed));
  RngSeedManager::SetRun (static_cast<uint64_t> (run));

  // Output dirs
  std::filesystem::create_directories (outDir + "/raw");
  std::filesystem::create_directories (outDir + "/logs");
  std::filesystem::create_directories (outDir + "/plots");

  const std::string dtag = DistanceTag (distance);
  const std::string tagSuffix = (tag.empty () ? "" : ("_" + tag));

  // Nodes
  Ptr<Node> staNode = CreateObject<Node> ();
  Ptr<Node> apNode = CreateObject<Node> ();
  Ptr<Node> serverNode = CreateObject<Node> ();

  NodeContainer wifiSta (staNode);
  NodeContainer wifiAp (apNode);
  NodeContainer csmaNodes (apNode, serverNode);

  // Mobility
  MobilityHelper mobility;
  mobility.SetMobilityModel ("ns3::ConstantPositionMobilityModel");
  mobility.Install (NodeContainer (staNode, apNode, serverNode));

  apNode->GetObject<MobilityModel> ()->SetPosition (Vector (0.0, 0.0, 0.0));
  staNode->GetObject<MobilityModel> ()->SetPosition (Vector (distance, 0.0, 0.0));
  serverNode->GetObject<MobilityModel> ()->SetPosition (Vector (0.0, 1.0, 0.0));

// -----------------------------
// Wi-Fi channel with explicit propagation loss
// -----------------------------

// 1) Choose base propagation loss model based on CLI propModel
Ptr<PropagationLossModel> baseLoss;

if (propModel == "friis")
{
  Ptr<FriisPropagationLossModel> friis = CreateObject<FriisPropagationLossModel> ();
  // Optional but recommended: set frequency for Friis (5 GHz band)
  friis->SetAttribute ("Frequency", DoubleValue (5.18e9));
  baseLoss = friis;
}
else // "logdistance"
{
  Ptr<LogDistancePropagationLossModel> logd = CreateObject<LogDistancePropagationLossModel> ();
  logd->SetAttribute ("Exponent", DoubleValue (logExp));
  logd->SetAttribute ("ReferenceDistance", DoubleValue (refDist));
  logd->SetAttribute ("ReferenceLoss", DoubleValue (refLoss));
  baseLoss = logd;
}

// 2) Shadowing (optional but you already use it)
Ptr<CorrelatedLogNormalShadowingLossModel> shad =
  CreateObject<CorrelatedLogNormalShadowingLossModel> ();
shad->SetAttribute ("SigmaDb", DoubleValue (shadowingSigmaDb));
shad->SetAttribute ("UpdatePeriod", TimeValue (Seconds (1.0)));
baseLoss->SetNext (shad);

// 3) Small-scale fading (optional)
if (enableFading)
{
  Ptr<NakagamiPropagationLossModel> nak = CreateObject<NakagamiPropagationLossModel> ();
  nak->SetAttribute ("Distance1", DoubleValue (5.0));
  nak->SetAttribute ("Distance2", DoubleValue (15.0));
  nak->SetAttribute ("m0", DoubleValue (1.5));
  nak->SetAttribute ("m1", DoubleValue (1.0));
  nak->SetAttribute ("m2", DoubleValue (0.75));
  shad->SetNext (nak);
}

// 4) Create the channel directly (NOT via helper)
Ptr<YansWifiChannel> chan = CreateObject<YansWifiChannel> ();
chan->SetPropagationDelayModel (CreateObject<ConstantSpeedPropagationDelayModel> ());
chan->SetPropagationLossModel (baseLoss);

// 5) Plug channel into PHY
YansWifiPhyHelper phy;
phy.SetChannel (chan);



phy.Set ("ChannelSettings", StringValue ("{0, 80, BAND_5GHZ, 0}")); 
//Config::SetDefault ("ns3::WifiPhy::GuardInterval", TimeValue (NanoSeconds (800)));
  phy.Set("TxPowerStart", DoubleValue(txPowerDbm));
phy.Set("TxPowerEnd", DoubleValue(txPowerDbm));
phy.Set("TxPowerLevels", UintegerValue(1));

phy.Set("RxNoiseFigure", DoubleValue(noiseFigureDb));
phy.Set("TxGain", DoubleValue(0.0));
phy.Set("RxGain", DoubleValue(0.0));


  // You can swap to Minstrel if you want more "realistic" adaptation:
  // wifi.SetRemoteStationManager ("ns3::MinstrelHtWifiManager");
WifiHelper wifi;
wifi.SetStandard(WIFI_STANDARD_80211ax); 

if (useMinstrel)
{
  wifi.SetRemoteStationManager ("ns3::MinstrelHtWifiManager");
}
else
{
  // Simple alternative when you want "no minstrel"
  wifi.SetRemoteStationManager ("ns3::IdealWifiManager");
}


  WifiMacHelper mac;
  Ssid ssid = Ssid (ssidStr);


  Config::SetDefault("ns3::WifiMacQueue::MaxSize",
                   QueueSizeValue(QueueSize(wifiMacQueueMaxSize)));
  Config::SetDefault("ns3::WifiMacQueue::MaxDelay",
                   TimeValue(MilliSeconds(wifiMacQueueMaxDelayMs)));
 
  mac.SetType ("ns3::StaWifiMac",
               "Ssid", SsidValue (ssid),
               "ActiveProbing", BooleanValue (false));


  NetDeviceContainer staDev = wifi.Install (phy, mac, wifiSta);

  mac.SetType ("ns3::ApWifiMac",
               "Ssid", SsidValue (ssid));
  NetDeviceContainer apWifiDev = wifi.Install (phy, mac, wifiAp);

  // -----------------------------
  // CSMA (AP <-> Server)
  // -----------------------------
  CsmaHelper csma;
  csma.SetChannelAttribute ("DataRate", StringValue ("100Mbps"));
  csma.SetChannelAttribute ("Delay", TimeValue (MilliSeconds (1)));
  NetDeviceContainer csmaDevs = csma.Install (csmaNodes);

  // Internet stack
  InternetStackHelper internet;
  internet.Install (NodeContainer (staNode, apNode, serverNode));

  Ipv4AddressHelper ipv4;

  // Wi-Fi subnet 10.1.0.0/24
  ipv4.SetBase ("10.1.0.0", "255.255.255.0");
  Ipv4InterfaceContainer ifStaWifi = ipv4.Assign (staDev);
  Ipv4InterfaceContainer ifApWifi = ipv4.Assign (apWifiDev);

  // CSMA subnet 10.2.0.0/24
  ipv4.SetBase ("10.2.0.0", "255.255.255.0");
  Ipv4InterfaceContainer ifCsma = ipv4.Assign (csmaDevs);

  const Ipv4Address serverIp = ifCsma.GetAddress (1);

  Ipv4GlobalRoutingHelper::PopulateRoutingTables ();

    // -----------------------------
  // AQM (FqCoDel) - install ONLY on the Wi-Fi bottleneck (STA + AP Wi-Fi devices)
  // Make it safe: uninstall existing root queue disc first if any.
  // -----------------------------
  if (enableAqm)
    {
      TypeId tid;
      if (!TypeId::LookupByNameFailSafe (aqmQueueDisc, &tid))
        {
          std::cerr << "[P3][WARN] AQM requested but QueueDisc type not found: "
                    << aqmQueueDisc << " (AQM disabled)\n";
        }
      else
        {
          TrafficControlHelper tch;
          tch.SetRootQueueDisc (aqmQueueDisc,
                                "MaxSize", QueueSizeValue (QueueSize (aqmMaxSize)));

          // Build a container with ONLY Wi-Fi devices (avoid CSMA; Wi-Fi is the bottleneck)
          NetDeviceContainer wifiDevs;
          wifiDevs.Add (staDev);     // STA Wi-Fi NetDeviceContainer
          wifiDevs.Add (apWifiDev);  // AP Wi-Fi NetDeviceContainer

          // IMPORTANT: if a root queue disc is already present, remove it first to avoid NS_FATAL
          tch.Uninstall (wifiDevs);

          // Install AQM
          tch.Install (wifiDevs);
        }
    }


  // -----------------------------
  // Main traffic: Sink on server
  // -----------------------------
  const uint16_t port = 5000;
  const std::string sinkFactory = (transport == "udp")
                                    ? "ns3::UdpSocketFactory"
                                    : "ns3::TcpSocketFactory";

  PacketSinkHelper sinkHelper (sinkFactory,
                               InetSocketAddress (Ipv4Address::GetAny (), port));
  ApplicationContainer sinkApp = sinkHelper.Install (serverNode);
  sinkApp.Start (Seconds (0.0));
  sinkApp.Stop (Seconds (simTime));

  Ptr<PacketSink> sink = DynamicCast<PacketSink> (sinkApp.Get (0));

  ApplicationContainer clientApp;

  if (transport == "udp")
    {
      OnOffHelper onoff ("ns3::UdpSocketFactory",
                         InetSocketAddress (serverIp, port));
      onoff.SetAttribute ("PacketSize", UintegerValue (static_cast<uint32_t> (pktSize)));
      onoff.SetAttribute ("DataRate", StringValue (udpRate));
      onoff.SetAttribute ("OnTime", StringValue ("ns3::ConstantRandomVariable[Constant=1]"));
      onoff.SetAttribute ("OffTime", StringValue ("ns3::ConstantRandomVariable[Constant=0]"));

      clientApp = onoff.Install (staNode);
    }
  else // tcp
    {
      BulkSendHelper bulk ("ns3::TcpSocketFactory",
                           InetSocketAddress (serverIp, port));
      bulk.SetAttribute ("MaxBytes", UintegerValue (tcpMaxBytes));
      bulk.SetAttribute ("SendSize", UintegerValue (static_cast<uint32_t> (pktSize)));

      clientApp = bulk.Install (staNode);
    }

  clientApp.Start (Seconds (appStart));
  clientApp.Stop (Seconds (simTime));

  // -----------------------------
  // Throughput CSV (per distance)
  // -----------------------------
  const std::string thrPath = outDir + "/raw/ts_" + dtag + "_" + transport + tagSuffix + ".csv";
  g_thrCsv.open (thrPath, std::ios::out);
  g_thrHeaderWritten = false;
  g_lastRx = 0;

  Simulator::Schedule (Seconds (std::max (0.01, appStart)),
                       &SampleThroughput, sink);

  // -----------------------------
  // RTT probe (per distance)
  // -----------------------------
  const uint16_t rttPort = 9000;
  const double rttIntervalS = 1.0 / rttHz;
  const double rttStart = std::max (1.0, appStart);
  const std::string rttCsvPath = outDir + "/raw/rtt_" + dtag + "_" + transport + tagSuffix + ".csv";

  Ptr<RttEchoServer> rttSrv = CreateObject<RttEchoServer> ();
  rttSrv->Setup (rttPort);
  serverNode->AddApplication (rttSrv);
  rttSrv->SetStartTime (Seconds (0.5));
  rttSrv->SetStopTime (Seconds (simTime));

  Ptr<RttEchoClient> rttCli = CreateObject<RttEchoClient> ();
  rttCli->Setup (serverIp, rttPort,
                 Seconds (rttIntervalS),
                 16, // payload
                 rttCsvPath,
                 rttVerbose);
  staNode->AddApplication (rttCli);
  rttCli->SetStartTime (Seconds (rttStart));
  rttCli->SetStopTime (Seconds (simTime));

  // -----------------------------
  // PCAP
  // -----------------------------
  if (pcap)
    {
      const std::string pfx = outDir + "/raw/p3_" + transport + "_" + dtag + "_" + propModel + tagSuffix;
      phy.EnablePcap (pfx + "_wifi_sta", staDev.Get (0));
      phy.EnablePcap (pfx + "_wifi_ap", apWifiDev.Get (0));
      csma.EnablePcap (pfx + "_csma_ap", csmaDevs.Get (0), true);
      csma.EnablePcap (pfx + "_csma_server", csmaDevs.Get (1), true);
    }

  // -----------------------------
  // FlowMonitor
  // -----------------------------
  FlowMonitorHelper flowHelper;
  Ptr<FlowMonitor> monitor;
  if (flowmon)
    {
      monitor = flowHelper.InstallAll ();
    }

  Simulator::Stop (Seconds (simTime));
  Simulator::Run ();

  // Close throughput csv
  if (g_thrCsv.is_open ())
    g_thrCsv.close ();

  // Compute summary
  const uint64_t rxBytes = sink->GetTotalRx ();
  const double tUseful = simTime - appStart;
  const double goodputbps = (tUseful > 0.0) ? (8.0 * static_cast<double> (rxBytes) / tUseful) : 0.0;
  const double goodputMbps = goodputbps / 1e6;

  // RTT stats
  double rttMeanMs = 0.0;
  double rttP95Ms = 0.0;
  uint32_t rttSamples = 0;
  ComputeRttStats (rttCsvPath, rttMeanMs, rttP95Ms, rttSamples);

  // Append to sweep CSV
  const std::string sweepPath = outDir + "/raw/p3_sweep.csv";
  const bool needHeader = !FileExistsAndNonEmpty (sweepPath);

  std::ofstream sweep (sweepPath, std::ios::out | std::ios::app);
  if (needHeader)
    {
      sweep << "distance_m,transport,propModel,logExp,refDist,refLoss,simTime,appStart,"
               "pktSize,udpRate,tcpMaxBytes,seed,run,rxBytes,goodput_Mbps,rtt_mean_ms,rtt_p95_ms,rtt_samples\n";
    }

  sweep << distance << ","
        << transport << ","
        << propModel << ","
        << ((propModel == "logdistance") ? logExp : 0.0) << ","
        << ((propModel == "logdistance") ? refDist : 0.0) << ","
        << ((propModel == "logdistance") ? refLoss : 0.0) << ","
        << simTime << ","
        << appStart << ","
        << pktSize << ","
        << ((transport == "udp") ? udpRate : "0") << ","
        << ((transport == "tcp") ? tcpMaxBytes : 0ULL) << ","
        << seed << ","
        << run << ","
        << rxBytes << ","
        << goodputMbps << ","
        << rttMeanMs << ","
        << rttP95Ms << ","
        << rttSamples
        << "\n";
  sweep.close ();

  // Save FlowMonitor XML per distance
  if (flowmon && monitor)
    {
      monitor->CheckForLostPackets ();
      const std::string flowPath = outDir + "/raw/flowmon_" + dtag + "_" + transport + "_" + propModel + tagSuffix + ".xml";
      monitor->SerializeToXmlFile (flowPath, true, true);
    }

  Simulator::Destroy ();

std::cout << "[P3] d" << distance << "m transport=" << transport
          << " prop=" << propModel
          << " rxBytes=" << rxBytes
          << " goodput=" << goodputMbps << " Mbps"
          << " rttMean=" << rttMeanMs << " ms"
          << " rttP95=" << rttP95Ms << " ms"
          << " rttSamples=" << rttSamples
          << "\n";


  return 0;
}
