
#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/wifi-module.h"
#include "ns3/csma-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/header.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

using namespace ns3;

// -----------------------------
// Throughput sampling (CSV time series)
// -----------------------------
static uint64_t g_lastRxBytes = 0;
static std::ofstream g_thrCsv;
static double g_thrInterval = 0.5; // seconds
static bool g_thrHeaderWritten = false;

static void
SampleThroughput (Ptr<PacketSink> sink)
{
  const double now = Simulator::Now ().GetSeconds ();
  const uint64_t cur = sink->GetTotalRx ();
  const uint64_t diff = (cur >= g_lastRxBytes) ? (cur - g_lastRxBytes) : 0ULL;
  const double thr_bps = (8.0 * static_cast<double> (diff)) / g_thrInterval;

  g_lastRxBytes = cur;

  if (g_thrCsv.is_open ())
    {
      if (!g_thrHeaderWritten)
        {
          g_thrCsv << "time_s,throughput_bps\n";
          g_thrHeaderWritten = true;
        }
      g_thrCsv << now << "," << thr_bps << "\n";
      g_thrCsv.flush ();
    }

  Simulator::Schedule (Seconds (g_thrInterval), &SampleThroughput, sink);
}

static void
InitThroughputSampling (Ptr<PacketSink> sink)
{
  g_lastRxBytes = sink->GetTotalRx ();
  Simulator::Schedule (Seconds (g_thrInterval), &SampleThroughput, sink);
}

// -----------------------------
// RTT probe: custom header (seq + tx timestamp) + UDP echo
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

  void Print (std::ostream &os) const override
  {
    os << "seq=" << m_seq << " txTimeNs=" << m_txTimeNs;
  }

private:
  uint32_t m_seq = 0;
  uint64_t m_txTimeNs = 0;
};

class RttEchoServer : public Application
{
public:
  void Setup (uint16_t port) { m_port = port; }

private:
  void StartApplication () override
  {
    m_socket = Socket::CreateSocket (GetNode (), UdpSocketFactory::GetTypeId ());
    InetSocketAddress local = InetSocketAddress (Ipv4Address::GetAny (), m_port);
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
        // Echo back exactly what we received (keeps RttHeader inside)
        socket->SendTo (p, 0, from);
      }
  }

private:
  Ptr<Socket> m_socket;
  uint16_t m_port = 9000;
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
    m_socket->Bind ();
    m_socket->Connect (InetSocketAddress (m_peerIp, m_peerPort));
    m_socket->SetRecvCallback (MakeCallback (&RttEchoClient::HandleRead, this));

    m_csv.open (m_csvPath, std::ios::out);
    if (m_csv.is_open ())
      {
        m_csv << "time_s,seq,rtt_ms\n";
        m_csv.flush ();
      }

    // Send immediately, then periodic
    Send ();
  }

  void StopApplication () override
  {
    m_running = false;

    // ns-3.46: EventId has IsPending() (not IsRunning())
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
      {
        m_sendEvent = Simulator::Schedule (m_interval, &RttEchoClient::Send, this);
      }
  }

  void Send ()
  {
    if (!m_running || !m_socket)
      {
        return;
      }

    RttHeader hdr;
    hdr.SetSeq (m_seq++);
    hdr.SetTxTimeNs (static_cast<uint64_t> (Simulator::Now ().GetNanoSeconds ()));

    Ptr<Packet> p = Create<Packet> (m_payloadSize);
    p->AddHeader (hdr);
    m_socket->Send (p);

    ScheduleNextTx ();
  }

  void HandleRead (Ptr<Socket> socket)
  {
    Ptr<Packet> p;
    while ((p = socket->Recv ()))
      {
        RttHeader hdr;
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

private:
  Ptr<Socket> m_socket;
  EventId m_sendEvent;
  bool m_running = false;

  Ipv4Address m_peerIp;
  uint16_t m_peerPort = 9000;
  Time m_interval = MilliSeconds (200); // 5 Hz default
  uint32_t m_payloadSize = 32;
  uint32_t m_seq = 0;

  std::string m_csvPath;
  std::ofstream m_csv;
  bool m_verbose = false;
};

// -----------------------------
// Helpers
// -----------------------------
static std::string
ToLower (std::string s)
{
  std::transform (s.begin (), s.end (), s.begin (),
                  [] (unsigned char c) { return static_cast<char> (std::tolower (c)); });
  return s;
}

static bool
FileExistsAndNonEmpty (const std::string& path)
{
  std::error_code ec;
  if (!std::filesystem::exists (path, ec)) return false;
  if (!std::filesystem::is_regular_file (path, ec)) return false;
  return (std::filesystem::file_size (path, ec) > 0);
}

int
main (int argc, char* argv[])
{
  // -----------------------------
  // Command line parameters
  // -----------------------------
  double simTime = 20.0;
  double appStart = 2.0;
  double distance = 5.0;

  std::string ssidStr = "wifi6-ter";
  std::string outDir = "results/p2";

  bool pcap = true;
  bool flowmon = true;

  int seed = 1;
  int run = 1;

  std::string transport = "udp";  // udp or tcp
  int pktSize = 1200;
  std::string udpRate = "50Mbps";
  uint64_t tcpMaxBytes = 0;

  double thrInterval = 0.5;
  double rttHz = 5.0;
  bool rttVerbose = false;

  CommandLine cmd;
  cmd.AddValue ("simTime", "Total simulation time (s)", simTime);
  cmd.AddValue ("appStart", "Start time of main traffic (s)", appStart);
  cmd.AddValue ("distance", "AP-STA distance (m)", distance);
  cmd.AddValue ("ssid", "Wi-Fi SSID", ssidStr);
  cmd.AddValue ("outDir", "Output directory (e.g., results/p2)", outDir);
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
  cmd.AddValue ("rttVerbose", "Print RTT lines to console", rttVerbose);

  cmd.Parse (argc, argv);

  transport = ToLower (transport);
  if (transport != "udp" && transport != "tcp")
    {
      std::cerr << "ERROR: --transport must be 'udp' or 'tcp'\n";
      return 1;
    }
  if (!(0.0 <= appStart && appStart < simTime))
    {
      std::cerr << "ERROR: require 0 <= appStart < simTime\n";
      return 1;
    }
  if (pktSize <= 0 || thrInterval <= 0.0 || rttHz <= 0.0)
    {
      std::cerr << "ERROR: invalid pktSize/thrInterval/rttHz\n";
      return 1;
    }

  g_thrInterval = thrInterval;

  // RNG reproducibility
  RngSeedManager::SetSeed (static_cast<uint32_t> (seed));
  RngSeedManager::SetRun (static_cast<uint64_t> (run));

  // Output dirs
  std::filesystem::create_directories (outDir + "/raw");
  std::filesystem::create_directories (outDir + "/logs");
  std::filesystem::create_directories (outDir + "/plots");

  // -----------------------------
  // Nodes (STA, AP, Server)
  // -----------------------------
  Ptr<Node> staNode = CreateObject<Node> ();
  Ptr<Node> apNode = CreateObject<Node> ();
  Ptr<Node> serverNode = CreateObject<Node> ();

  NodeContainer wifiSta (staNode);
  NodeContainer wifiAp (apNode);
  NodeContainer csmaNodes (apNode, serverNode);

  // Fixed positions
  MobilityHelper mobility;
  mobility.SetMobilityModel ("ns3::ConstantPositionMobilityModel");
  mobility.Install (NodeContainer (staNode, apNode, serverNode));

  apNode->GetObject<MobilityModel> ()->SetPosition (Vector (0.0, 0.0, 0.0));
  staNode->GetObject<MobilityModel> ()->SetPosition (Vector (distance, 0.0, 0.0));
  serverNode->GetObject<MobilityModel> ()->SetPosition (Vector (0.0, 1.0, 0.0));

  // -----------------------------
  // Wi-Fi (STA <-> AP)
  // -----------------------------
  YansWifiChannelHelper channel = YansWifiChannelHelper::Default ();
  YansWifiPhyHelper phy;
  phy.SetChannel (channel.Create ());

  WifiHelper wifi;
  wifi.SetStandard (WIFI_STANDARD_80211ax);
  wifi.SetRemoteStationManager ("ns3::IdealWifiManager");

  WifiMacHelper mac;
  Ssid ssid = Ssid (ssidStr);

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
  csma.SetChannelAttribute ("DataRate", StringValue ("1Gbps"));
  csma.SetChannelAttribute ("Delay", TimeValue (MilliSeconds (1)));
  NetDeviceContainer csmaDevs = csma.Install (csmaNodes);

  // -----------------------------
  // Internet + IPv4 (two subnets)
  // -----------------------------
  InternetStackHelper internet;
  internet.Install (NodeContainer (staNode, apNode, serverNode));

  Ipv4AddressHelper ipv4;

  // Wi-Fi subnet 10.1.0.0/24
  ipv4.SetBase ("10.1.0.0", "255.255.255.0");
  Ipv4InterfaceContainer ifSta = ipv4.Assign (staDev);
  Ipv4InterfaceContainer ifApWifi = ipv4.Assign (apWifiDev);

  // CSMA subnet 10.2.0.0/24
  ipv4.SetBase ("10.2.0.0", "255.255.255.0");
  Ipv4InterfaceContainer ifCsma = ipv4.Assign (csmaDevs);

  const Ipv4Address serverIp = ifCsma.GetAddress (1);

  Ipv4GlobalRoutingHelper::PopulateRoutingTables ();

  // -----------------------------
  // Main traffic: Sink on server
  // -----------------------------
  const uint16_t port = 5000;
  const std::string sinkFactory = (transport == "udp") ? "ns3::UdpSocketFactory"
                                                       : "ns3::TcpSocketFactory";

  PacketSinkHelper sinkHelper (sinkFactory,
                               InetSocketAddress (Ipv4Address::GetAny (), port));
  ApplicationContainer sinkApps = sinkHelper.Install (serverNode);
  sinkApps.Start (Seconds (0.0));
  sinkApps.Stop (Seconds (simTime));
  Ptr<PacketSink> sink = DynamicCast<PacketSink> (sinkApps.Get (0));

  // Sender on STA
  ApplicationContainer srcApps;
  if (transport == "udp")
    {
      OnOffHelper onoff ("ns3::UdpSocketFactory", InetSocketAddress (serverIp, port));
      onoff.SetAttribute ("DataRate", DataRateValue (DataRate (udpRate)));
      onoff.SetAttribute ("PacketSize", UintegerValue ((uint32_t)pktSize));
      onoff.SetAttribute ("OnTime", StringValue ("ns3::ConstantRandomVariable[Constant=1]"));
      onoff.SetAttribute ("OffTime", StringValue ("ns3::ConstantRandomVariable[Constant=0]"));
      srcApps = onoff.Install (staNode);
    }
  else
    {
      BulkSendHelper bulk ("ns3::TcpSocketFactory", InetSocketAddress (serverIp, port));
      bulk.SetAttribute ("MaxBytes", UintegerValue (tcpMaxBytes));
      bulk.SetAttribute ("SendSize", UintegerValue ((uint32_t)pktSize));
      srcApps = bulk.Install (staNode);
    }

  srcApps.Start (Seconds (appStart));
  srcApps.Stop (Seconds (simTime));

  // -----------------------------
  // Throughput CSV
  // -----------------------------
  const std::string thrPath = outDir + "/raw/throughput_timeseries.csv";
  g_thrCsv.open (thrPath, std::ios::out);
  g_thrHeaderWritten = false;
  Simulator::Schedule (Seconds (appStart), &InitThroughputSampling, sink);

  // -----------------------------
  // RTT probe (UDP echo with custom header)
  // -----------------------------
  const uint16_t rttPort = 9000;
  const double rttInterval = 1.0 / rttHz;
  const double rttStart = std::max (1.0, appStart);
  const std::string rttCsvPath = outDir + "/raw/rtt_timeseries.csv";

  Ptr<RttEchoServer> rttSrv = CreateObject<RttEchoServer> ();
  rttSrv->Setup (rttPort);
  serverNode->AddApplication (rttSrv);
  rttSrv->SetStartTime (Seconds (0.0));
  rttSrv->SetStopTime (Seconds (simTime));

  Ptr<RttEchoClient> rttCli = CreateObject<RttEchoClient> ();
  rttCli->Setup (serverIp, rttPort, Seconds (rttInterval), 32, rttCsvPath, rttVerbose);
  staNode->AddApplication (rttCli);
  rttCli->SetStartTime (Seconds (rttStart));
  rttCli->SetStopTime (Seconds (simTime));

  // -----------------------------
  // FlowMonitor
  // -----------------------------
  Ptr<FlowMonitor> monitor;
  FlowMonitorHelper flowHelper;
  if (flowmon)
    {
      monitor = flowHelper.InstallAll ();
    }

  // -----------------------------
  // PCAP
  // -----------------------------
  if (pcap)
    {
      std::ostringstream base;
      base << outDir << "/raw/"
           << "p2_" << transport
           << "_d" << static_cast<int> (distance)
           << "_run" << run;

      phy.EnablePcap (base.str () + "_wifi_ap", apWifiDev.Get (0), true);
      phy.EnablePcap (base.str () + "_wifi_sta", staDev.Get (0), true);
      csma.EnablePcap (base.str () + "_csma", csmaDevs, true);
    }

  // -----------------------------
  // Run
  // -----------------------------
  Simulator::Stop (Seconds (simTime));
  Simulator::Run ();

  // -----------------------------
  // Goodput (global)
  // -----------------------------
  const uint64_t rxBytes = sink->GetTotalRx ();
  const double tUseful = simTime - appStart;
  const double goodputbps = (tUseful > 0.0) ? (8.0 * (double)rxBytes / tUseful) : 0.0;

  // Summary CSV
  const std::string sumPath = outDir + "/raw/p2_summary.csv";
  const bool needHeader = !FileExistsAndNonEmpty (sumPath);

  std::ofstream sum (sumPath, std::ios::out | std::ios::app);
  if (needHeader)
    {
      sum << "transport,simTime,appStart,distance,pktSize,udpRate,tcpMaxBytes,seed,run,rxBytes,goodputbps\n";
    }
  sum << transport << ","
      << simTime << ","
      << appStart << ","
      << distance << ","
      << pktSize << ","
      << ((transport == "udp") ? udpRate : "0") << ","
      << ((transport == "tcp") ? tcpMaxBytes : 0ULL) << ","
      << seed << ","
      << run << ","
      << rxBytes << ","
      << goodputbps << "\n";
  sum.close ();

  // FlowMonitor XML
  if (flowmon && monitor)
    {
      monitor->CheckForLostPackets ();
      monitor->SerializeToXmlFile (outDir + "/raw/flowmon.xml", true, true);
    }

  // Close CSV
  if (g_thrCsv.is_open ())
    {
      g_thrCsv.flush ();
      g_thrCsv.close ();
    }

  Simulator::Destroy ();

  std::cout << "=== Part 2 terminé ===\n";
  std::cout << "transport=" << transport
            << " serverIp=" << serverIp
            << " rxBytes=" << rxBytes
            << " goodputbps=" << goodputbps << "\n";
  std::cout << "CSV: " << thrPath << " , " << rttCsvPath << " , " << sumPath << "\n";

  return 0;
}
