// p4_phy_mac_sweep.cc  (inspired by your p3_distance_sweep.cc style)
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
#include "ns3/propagation-module.h"

#include <fstream>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <vector>
#include <algorithm>
#include <map>


using namespace ns3;

// -------------------- RTT Header (same idea as p3) --------------------
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

// -------------------- RTT Echo Server (same idea as p3) --------------------
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
        socket->SendTo (p, 0, from); // echo back
      }
  }

  uint16_t m_port{9000};
  Ptr<Socket> m_socket;
};

// -------------------- RTT Echo Client (same idea as p3) --------------------
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
    {
      m_csv.flush (); 
      m_csv.close ();
    }
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
        if (p->PeekHeader (hdr) == 0)
          continue;

        p->RemoveHeader (hdr);

        const double now = Simulator::Now ().GetSeconds ();
        const Time tx = NanoSeconds (hdr.GetTxTimeNs ());
        const Time rtt = Simulator::Now () - tx;
        const double rttMs = rtt.GetMilliSeconds ();

        if (m_csv.is_open ())
  {
    m_csv << std::fixed << std::setprecision (6) << now << ","
          << hdr.GetSeq () << ","
          << std::setprecision (3) << rttMs << "\n";
    m_csv.flush ();  
  }


        if (m_verbose)
          {
            std::cout << "[RTT] t=" << now << "s seq=" << hdr.GetSeq ()
                      << " rtt=" << rttMs << " ms\n";
          }
      }
  }

  Ipv4Address m_peerIp;
  uint16_t    m_peerPort{9000};
  Time        m_interval{Seconds (0.2)};
  uint32_t    m_payloadSize{64};
  std::string m_csvPath{"rtt.csv"};
  bool        m_verbose{false};

  bool        m_running{false};
  uint32_t    m_seq{0};
  Ptr<Socket> m_socket;
  EventId     m_sendEvent;
  std::ofstream m_csv;
};

// -------------------- Correlated Shadowing (copied idea from p3) --------------------
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
    ns3::Time nextUpdate{ns3::Seconds (0.0)};
  };

  double DoCalcRxPower (double txPowerDbm,
                        ns3::Ptr<ns3::MobilityModel> a,
                        ns3::Ptr<ns3::MobilityModel> b) const override
  {
    uintptr_t pa = reinterpret_cast<uintptr_t> (PeekPointer (a));
    uintptr_t pb = reinterpret_cast<uintptr_t> (PeekPointer (b));
    LinkKey key{std::min (pa, pb), std::max (pa, pb)};

    const ns3::Time now = ns3::Simulator::Now ();

    auto& st = m_links[key];
    if (now >= st.nextUpdate)
      {
        // Normal(0, sigmaDb)
        m_normal->SetAttribute ("Variance", ns3::DoubleValue (m_sigmaDb * m_sigmaDb));
        st.shadowDb = m_normal->GetValue ();
        st.nextUpdate = now + m_updatePeriod;
      }

    // Shadowing is additional LOSS in dB => subtract from received power
    return txPowerDbm - st.shadowDb;
  }

  int64_t DoAssignStreams (int64_t stream) override
  {
    m_normal->SetStream (stream);
    return 1;
  }

  mutable std::map<LinkKey, LinkState> m_links;
  ns3::Ptr<ns3::NormalRandomVariable> m_normal;

  double   m_sigmaDb{5.0};
  ns3::Time m_updatePeriod{ns3::Seconds (1.0)};
};

// -------------------- Throughput sampling --------------------
static uint64_t      g_lastRx = 0;
static double        g_thrInterval = 0.5;
static std::ofstream g_tsCsv;

static void
SampleThroughput (Ptr<PacketSink> sink)
{
  const double now = Simulator::Now ().GetSeconds ();
  const uint64_t cur = sink->GetTotalRx ();
  const uint64_t diff = cur - g_lastRx;

  const double thr_bps = (8.0 * static_cast<double> (diff)) / g_thrInterval;
  g_lastRx = cur;

  g_tsCsv << std::fixed << std::setprecision (3) << now << ","
          << std::setprecision (3) << thr_bps << "\n";

  Simulator::Schedule (Seconds (g_thrInterval), &SampleThroughput, sink);
}

// -------------------- RTT stats from CSV (like p3) --------------------
static void
ComputeRttStats (const std::string& rttCsvPath,
                      double appStart,
                      double& meanMs, double& p95Ms, uint32_t& samples)
{
  meanMs = -1.0;
  p95Ms = -1.0;
  samples = 0;

  std::ifstream in (rttCsvPath);
  if (!in.is_open ())
    return;

  std::string line;
  std::getline (in, line); // header

  std::vector<double> rtts;
  rtts.reserve (1024);

  while (std::getline (in, line))
    {
      if (line.empty ()) continue;
      std::stringstream ss (line);
      std::string tok;

      // time_s
if (!std::getline (ss, tok, ',')) continue;
double t = 0.0;
try { t = std::stod(tok); } catch (...) { continue; }

// seq
if (!std::getline (ss, tok, ',')) continue;

// rtt_ms
if (!std::getline (ss, tok, ',')) continue;

if (t < appStart) continue;

      try {
        double r = std::stod (tok);
        rtts.push_back (r);
      } catch (...) {}
    }

  if (rtts.empty ())
    return;

  samples = static_cast<uint32_t> (rtts.size ());
  double sum = 0.0;
  for (double v : rtts) sum += v;
  meanMs = sum / rtts.size ();

  std::sort (rtts.begin (), rtts.end ());
  size_t idx = static_cast<size_t> (0.95 * (rtts.size () - 1));
  p95Ms = rtts[idx];
}

// -------------------- CSV header helper --------------------
static void
WriteHeaderIfNeeded (const std::string& path, const std::string& headerLine)
{
  std::error_code ec;
  if (!std::filesystem::exists (path, ec) || std::filesystem::file_size (path, ec) == 0)
    {
      std::ofstream out (path, std::ios::out);
      out << headerLine << "\n";
      out.close ();
    }
}

int
main (int argc, char *argv[])
{
  // -------------------- Parameters --------------------
  double simTime   = 20.0;
  double appStart  = 2.0;
  double distance  = 5.0;

  uint32_t channelWidth = 20;
  double   txPowerDbm   = 20.0;

  std::string rateMode = "adaptive"; // constant | adaptive
  int mcs = 0;

  std::string ssidStr = "wifi6-ter";
  std::string udpRate = "600Mbps";
  uint32_t pktSize    = 1200;

  // Realism knobs (like p3)
  double logExp = 3.0;
  double refDist = 1.0;
  double refLoss = 46.6777;

  double noiseFigureDb = 7.0;

  bool enableShadowing = false;
  double shadowSigmaDb = 5.0;
  double shadowUpdateS = 1.0;

  bool enableFading = false;

  bool useMinstrel = true;
  bool useMinstrelHe = false;


  // RTT probe knobs
  double rttHz = 2.0;
  uint32_t rttPayloadSize = 32;
  bool rttVerbose = false;

  bool pcap = false;
  bool flowmon = true;

  uint32_t seed = 1;
  uint32_t run  = 1;

  std::string outDir = "results/p4";
  std::string tagSuffix = "";

  // -------------------- CommandLine (your ns-3 needs help string) --------------------
  CommandLine cmd;
  cmd.AddValue ("simTime",      "Total simulation time (s)", simTime);
  cmd.AddValue ("appStart",     "Application start time (s)", appStart);
  cmd.AddValue ("distance",     "STA-AP distance (m)", distance);

  cmd.AddValue ("channelWidth", "Wi-Fi channel width (MHz)", channelWidth);
  cmd.AddValue ("txPowerDbm",   "Tx power (dBm)", txPowerDbm);

  cmd.AddValue ("rateMode",     "Rate mode: constant|adaptive", rateMode);
  cmd.AddValue ("mcs",          "MCS index used when rateMode=constant", mcs);
  cmd.AddValue ("useMinstrel",  "Use MinstrelHtWifiManager in adaptive mode", useMinstrel);

  cmd.AddValue ("ssid",         "Wi-Fi SSID", ssidStr);
  cmd.AddValue ("udpRate",      "UDP offered load (e.g., 50Mbps)", udpRate);
  cmd.AddValue ("pktSize",      "UDP packet size (bytes)", pktSize);

  cmd.AddValue ("logExp",       "LogDistance exponent", logExp);
  cmd.AddValue ("refDist",      "LogDistance reference distance (m)", refDist);
  cmd.AddValue ("refLoss",      "LogDistance reference loss (dB)", refLoss);

  cmd.AddValue ("noiseFigureDb","Rx noise figure (dB)", noiseFigureDb);

  cmd.AddValue ("enableShadowing","Enable correlated lognormal shadowing", enableShadowing);
  cmd.AddValue ("shadowSigmaDb",  "Shadowing sigma (dB)", shadowSigmaDb);
  cmd.AddValue ("shadowUpdateS",  "Shadowing update period (s)", shadowUpdateS);

  cmd.AddValue ("enableFading", "Enable Nakagami fading", enableFading);

  cmd.AddValue ("rttHz",        "RTT probe frequency (Hz)", rttHz);
  cmd.AddValue ("rttPayloadSize","RTT probe payload size (bytes)", rttPayloadSize);
  cmd.AddValue ("rttVerbose",   "Print RTT probe lines", rttVerbose);

  cmd.AddValue ("pcap",         "Enable PCAP", pcap);
  cmd.AddValue ("flowmon",      "Enable FlowMonitor", flowmon);

  cmd.AddValue ("seed",         "RNG seed", seed);
  cmd.AddValue ("run",          "RNG run number", run);

  cmd.AddValue ("outDir",       "Output directory", outDir);
  cmd.AddValue ("tag",          "Extra filename suffix", tagSuffix);
  cmd.Parse (argc, argv);

  RngSeedManager::SetSeed (seed);
  RngSeedManager::SetRun (run);

  // output dirs
  std::filesystem::create_directories (outDir + "/raw");
  std::filesystem::create_directories (outDir + "/logs");

  // -------------------- Nodes --------------------
  NodeContainer nodes;
  nodes.Create (3);
  Ptr<Node> sta    = nodes.Get (0);
  Ptr<Node> ap     = nodes.Get (1);
  Ptr<Node> server = nodes.Get (2);

  // -------------------- Mobility --------------------
  MobilityHelper mob;
  mob.SetMobilityModel ("ns3::ConstantPositionMobilityModel");
  mob.Install (nodes);

  sta->GetObject<MobilityModel> ()->SetPosition (Vector (distance, 0, 0));
  ap->GetObject<MobilityModel> ()->SetPosition (Vector (0, 0, 0));
  server->GetObject<MobilityModel> ()->SetPosition (Vector (0, 1, 0));

  // -------------------- Propagation chain (exact p3 style) --------------------
  Ptr<PropagationLossModel> head = CreateObject<LogDistancePropagationLossModel> ();
  head->SetAttribute ("Exponent", DoubleValue (logExp));
  head->SetAttribute ("ReferenceDistance", DoubleValue (refDist));
  head->SetAttribute ("ReferenceLoss", DoubleValue (refLoss));

  Ptr<PropagationLossModel> tail = head;

  if (enableShadowing)
    {
      Ptr<CorrelatedLogNormalShadowingLossModel> shad = CreateObject<CorrelatedLogNormalShadowingLossModel> ();
      shad->SetAttribute ("SigmaDb", DoubleValue (shadowSigmaDb));
      shad->SetAttribute ("UpdatePeriod", TimeValue (Seconds (shadowUpdateS)));
      tail->SetNext (shad);
      tail = shad;
    }

  if (enableFading)
    {
      Ptr<NakagamiPropagationLossModel> nak = CreateObject<NakagamiPropagationLossModel> ();
      nak->SetAttribute ("Distance1", DoubleValue (5.0));
      nak->SetAttribute ("Distance2", DoubleValue (15.0));
      nak->SetAttribute ("m0", DoubleValue (1.5));
      nak->SetAttribute ("m1", DoubleValue (1.0));
      nak->SetAttribute ("m2", DoubleValue (0.75));
      tail->SetNext (nak);
      tail = nak;
    }

  Ptr<YansWifiChannel> chan = CreateObject<YansWifiChannel> ();
  chan->SetPropagationDelayModel (CreateObject<ConstantSpeedPropagationDelayModel> ());
  chan->SetPropagationLossModel (head);

  // -------------------- PHY (exact p3 style) --------------------
  YansWifiPhyHelper phy;
  phy.SetChannel (chan);

uint16_t ch = 36; // default for 20 MHz in 5 GHz

if (channelWidth == 20)
  ch = 36;     // center 36 for 20 MHz
else if (channelWidth == 40)
  ch = 38;     // center 38 for 40 MHz (covers 36+40)
else if (channelWidth == 80)
  ch = 42;     // center 42 for 80 MHz (covers 36..48)
else
  {
    NS_LOG_UNCOND ("[ERR] Unsupported channelWidth=" << channelWidth
                   << " (supported: 20,40,80)");
    return 1;
  }

{
  std::ostringstream cs;
  cs << "{" << ch << ", " << channelWidth << ", BAND_5GHZ, 0}";
  phy.Set ("ChannelSettings", StringValue (cs.str ()));
}


  phy.Set ("TxPowerStart", DoubleValue (txPowerDbm));
  phy.Set ("TxPowerEnd",   DoubleValue (txPowerDbm));
  phy.Set ("TxPowerLevels", UintegerValue (1));

  phy.Set ("RxNoiseFigure", DoubleValue (noiseFigureDb));
  phy.Set ("TxGain", DoubleValue (0.0));
  phy.Set ("RxGain", DoubleValue (0.0));

  // -------------------- Wi-Fi --------------------
  WifiHelper wifi;
  wifi.SetStandard (WIFI_STANDARD_80211ax);

  if (rateMode == "constant")
    {
      std::ostringstream mode;
      mode << "HeMcs" << mcs; 

      wifi.SetRemoteStationManager ("ns3::ConstantRateWifiManager",
                                    "DataMode", StringValue (mode.str ()),
                                    "ControlMode", StringValue ("HeMcs0"));
    }
  else
    {
      cmd.AddValue ("useMinstrelHe", "Use MinstrelHeWifiManager in adaptive mode", useMinstrelHe);

if (rateMode != "constant")
{
  if (useMinstrelHe)
    wifi.SetRemoteStationManager ("ns3::MinstrelHeWifiManager");
  else
    wifi.SetRemoteStationManager ("ns3::MinstrelHtWifiManager");
}
    }

  WifiMacHelper mac;
  Ssid ssid = Ssid (ssidStr);

  mac.SetType ("ns3::StaWifiMac",
               "Ssid", SsidValue (ssid),
               "ActiveProbing", BooleanValue (false));
  NetDeviceContainer staDev = wifi.Install (phy, mac, sta);

  mac.SetType ("ns3::ApWifiMac",
               "Ssid", SsidValue (ssid));
  NetDeviceContainer apDev = wifi.Install (phy, mac, ap);

  // -------------------- CSMA --------------------
  CsmaHelper csma;
  csma.SetChannelAttribute ("DataRate", StringValue ("1Gbps"));
  csma.SetChannelAttribute ("Delay", TimeValue (NanoSeconds (500)));
  NetDeviceContainer csmaDev = csma.Install (NodeContainer (ap, server));

  // -------------------- Internet --------------------
  InternetStackHelper internet;
  internet.Install (nodes);

  Ipv4AddressHelper ipv4;

  ipv4.SetBase ("10.1.0.0", "255.255.255.0");
  ipv4.Assign (NetDeviceContainer (staDev.Get (0), apDev.Get (0)));

  ipv4.SetBase ("10.2.0.0", "255.255.255.0");
  Ipv4InterfaceContainer ifCsma = ipv4.Assign (csmaDev);

  Ipv4GlobalRoutingHelper::PopulateRoutingTables ();

  // -------------------- File tag (no collisions) --------------------
  std::ostringstream tag;
tag << "d" << std::fixed << std::setprecision (0) << distance
    << "_w" << channelWidth
    << "_p" << std::fixed << std::setprecision (0) << txPowerDbm
    << "_" << rateMode
    << "_mcs" << mcs
    << "_s" << seed
    << "_r" << run;

if (!tagSuffix.empty ())
  tag << "_" << tagSuffix;

const std::string runTag = tag.str ();

  // -------------------- Applications: UDP CBR --------------------
  const uint16_t port = 5000;
  Address sinkAddr (InetSocketAddress (ifCsma.GetAddress (1), port));

  PacketSinkHelper sink ("ns3::UdpSocketFactory", sinkAddr);
  ApplicationContainer sinkApp = sink.Install (server);
  sinkApp.Start (Seconds (0.0));
  sinkApp.Stop  (Seconds (simTime));

  OnOffHelper onoff ("ns3::UdpSocketFactory", sinkAddr);
  onoff.SetAttribute ("DataRate", StringValue (udpRate));
  onoff.SetAttribute ("PacketSize", UintegerValue (pktSize));
  // CBR: always ON
  onoff.SetAttribute ("OnTime",  StringValue ("ns3::ConstantRandomVariable[Constant=1]"));
  onoff.SetAttribute ("OffTime", StringValue ("ns3::ConstantRandomVariable[Constant=0]"));

  ApplicationContainer onApp = onoff.Install (sta);
  onApp.Start (Seconds (appStart));
  onApp.Stop  (Seconds (simTime));

  // -------------------- RTT probe: custom echo (p3 style) --------------------
  const uint16_t rttPort = 9000;
  const double rttIntervalS = 1.0 / std::max (0.1, rttHz);
const double rttStart = 1.0;
  const std::string rttCsvPath = outDir + "/raw/rtt_" + runTag + ".csv";

  Ptr<RttEchoServer> rttSrv = CreateObject<RttEchoServer> ();
  rttSrv->Setup (rttPort);
  server->AddApplication (rttSrv);
  rttSrv->SetStartTime (Seconds (0.5));
  rttSrv->SetStopTime (Seconds (simTime));

  Ptr<RttEchoClient> rttCli = CreateObject<RttEchoClient> ();
  rttCli->Setup (ifCsma.GetAddress (1), rttPort,
                 Seconds (rttIntervalS), rttPayloadSize,
                 rttCsvPath, rttVerbose);
  sta->AddApplication (rttCli);
  rttCli->SetStartTime (Seconds (rttStart));
  rttCli->SetStopTime (Seconds (simTime - 0.01));
  Simulator::Stop (Seconds (simTime + 0.05));

  // -------------------- Throughput time-series --------------------
  Ptr<PacketSink> sinkPtr = DynamicCast<PacketSink> (sinkApp.Get (0));
  const std::string tsPath = outDir + "/raw/ts_" + runTag + ".csv";
  g_tsCsv.open (tsPath, std::ios::out);
  g_tsCsv << "time_s,throughput_bps\n";
  g_lastRx = 0;
  Simulator::Schedule (Seconds (appStart + g_thrInterval), &SampleThroughput, sinkPtr);

  // -------------------- PCAP --------------------
  if (pcap)
    {
      phy.EnablePcap (outDir + "/raw/wifi-ap_" + runTag, apDev.Get (0), true);
      phy.EnablePcap (outDir + "/raw/wifi-sta_" + runTag, staDev.Get (0), true);
      csma.EnablePcap (outDir + "/raw/csma_" + runTag, csmaDev, true);
    }

  // -------------------- FlowMonitor --------------------
  FlowMonitorHelper fm;
  Ptr<FlowMonitor> monitor;
  if (flowmon)
    monitor = fm.InstallAll ();

  // -------------------- Run --------------------
  Simulator::Run ();

  if (g_tsCsv.is_open ()) g_tsCsv.close ();

  const uint64_t rxBytes = sinkPtr->GetTotalRx ();
  const double goodputMbps = (simTime > appStart)
  ? (8.0 * static_cast<double> (rxBytes) / (simTime - appStart)) / 1e6
  : 0.0;


  // RTT stats from RTT CSV (p3 style)
  double rttMeanMs = -1.0, rttP95Ms = -1.0;
  uint32_t rttSamples = 0;
  ComputeRttStats (rttCsvPath, appStart, rttMeanMs, rttP95Ms, rttSamples);


  if (flowmon && monitor)
    {
      const std::string fmPath = outDir + "/raw/flowmon_" + runTag + ".xml";
      monitor->SerializeToXmlFile (fmPath, true, true);
    }

  // -------------------- Summary CSV (spec-friendly) --------------------
  const std::string summaryPath = outDir + "/raw/p4_matrix.csv";
  WriteHeaderIfNeeded (
    summaryPath,
    "distance,channelWidth,txPowerDbm,rateMode,mcs,udpRate,pktSize,seed,run,rxBytes,goodputMbps,rttMeanMs"
  );

  std::ofstream summary (summaryPath, std::ios::app);
  summary << std::fixed << std::setprecision (0)
          << distance << ","
          << channelWidth << ","
          << txPowerDbm << ","
          << rateMode << ","
          << mcs << ","
          << udpRate << ","
          << pktSize << ","
          << seed << ","
          << run << ","
          << rxBytes << ","
          << std::setprecision (6) << goodputMbps << ","
          << std::setprecision (3) << rttMeanMs
          << "\n";
  summary.close ();

  // (Optional) also write a small extra file with RTT p95 and samples (won't break spec checkers)
  {
    std::ofstream extra (outDir + "/raw/rtt_stats_" + runTag + ".txt", std::ios::out);
    extra << "samples=" << rttSamples << "\n";
    extra << "mean_ms=" << rttMeanMs << "\n";
    extra << "p95_ms=" << rttP95Ms << "\n";
    extra.close ();
  }

  Simulator::Destroy ();
  return 0;
}
