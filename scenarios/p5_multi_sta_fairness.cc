#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/wifi-module.h"
#include "ns3/csma-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/propagation-module.h"
#include "ns3/ipv4-l3-protocol.h"
#include "ns3/ipv4-static-routing-helper.h"
#include "ns3/ipv4-static-routing.h"
#include "ns3/ipv4-header.h"
#include "ns3/udp-header.h"
#include "ns3/tcp-header.h"
#include "ns3/output-stream-wrapper.h"

#include <cmath>
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

// ===================== Utilities =====================

static bool
IsFileNonEmpty(const std::string &path)
{
  std::error_code ec;
  if (!std::filesystem::exists(path, ec))
    return false;
  if (std::filesystem::is_directory(path, ec))
    return false;
  return (std::filesystem::file_size(path, ec) > 0);
}

static double
ComputeJain(const std::vector<double> &x)
{
  if (x.empty())
    return 0.0;

  double sum = 0.0;
  double sumsq = 0.0;
  for (double v : x)
  {
    sum += v;
    sumsq += v * v;
  }
  if (sumsq <= 0.0)
    return 0.0;
  return (sum * sum) / (static_cast<double>(x.size()) * sumsq);
}

// ===================== Globals for time series sampling =====================

static std::vector<Ptr<PacketSink>> g_sinks;
static std::ofstream g_tsCsv;
static uint64_t g_lastSumRx = 0;
static double g_interval = 0.5;
static double g_simStop = 20.0;

// ===================== Debug log (path tracing) =====================

static std::ofstream g_dbg;

static std::string
NowStr()
{
  std::ostringstream os;
  os << std::fixed << std::setprecision(6) << Simulator::Now().GetSeconds();
  return os.str();
}

static std::string
AddrStr(Ipv4Address a)
{
  std::ostringstream os;
  os << a;
  return os.str();
}

static void
PrintIpTuple(std::ostream &os, Ptr<const Packet> p)
{
  Ipv4Header ip;
  Ptr<Packet> q = p->Copy();

  if (!q->PeekHeader(ip))
  {
    os << " [no-ipv4hdr]";
    return;
  }

  os << " IP " << AddrStr(ip.GetSource()) << "->" << AddrStr(ip.GetDestination())
     << " proto=" << int(ip.GetProtocol());

  if (ip.GetProtocol() == 17) // UDP
  {
    q->RemoveHeader(ip);
    UdpHeader udp;
    if (q->PeekHeader(udp))
    {
      os << " UDP " << udp.GetSourcePort() << "->" << udp.GetDestinationPort();
    }
  }
  else if (ip.GetProtocol() == 6) // TCP
  {
    q->RemoveHeader(ip);
    TcpHeader tcp;
    if (q->PeekHeader(tcp))
    {
      os << " TCP " << tcp.GetSourcePort() << "->" << tcp.GetDestinationPort();
    }
  }
}

static void
AppTxTrace(std::string who, Ptr<const Packet> p)
{
  if (!g_dbg.is_open())
    return;
  g_dbg << NowStr() << " [APP-TX] " << who << " bytes=" << p->GetSize();
  PrintIpTuple(g_dbg, p);
  g_dbg << "\n";
}

static void
SinkRxTrace(std::string who, Ptr<const Packet> p, const Address &from)
{
  (void)from;
  if (!g_dbg.is_open())
    return;
  g_dbg << NowStr() << " [SINK-RX] " << who << " bytes=" << p->GetSize();
  PrintIpTuple(g_dbg, p);
  g_dbg << "\n";
}

// ---- IPv4 L3 traces (path debugging) ----
static void
Ipv4TxTrace(Ptr<const Packet> p, Ptr<Ipv4> ipv4, uint32_t interface)
{
  if (!g_dbg.is_open())
    return;
  g_dbg << NowStr() << " [IP-TX] node=" << ipv4->GetObject<Node>()->GetId()
        << " if=" << interface << " bytes=" << p->GetSize();
  PrintIpTuple(g_dbg, p);
  g_dbg << "\n";
}

static void
Ipv4RxTrace(Ptr<const Packet> p, Ptr<Ipv4> ipv4, uint32_t interface)
{
  if (!g_dbg.is_open())
    return;
  g_dbg << NowStr() << " [IP-RX] node=" << ipv4->GetObject<Node>()->GetId()
        << " if=" << interface << " bytes=" << p->GetSize();
  PrintIpTuple(g_dbg, p);
  g_dbg << "\n";
}

static void
Ipv4DropTrace(const Ipv4Header &h,
              Ptr<const Packet> p,
              Ipv4L3Protocol::DropReason reason,
              Ptr<Ipv4> ipv4,
              uint32_t interface)
{
  if (!g_dbg.is_open())
    return;

  g_dbg << NowStr() << " [IP-DROP] node=" << ipv4->GetObject<Node>()->GetId()
        << " if=" << interface
        << " reason=" << int(reason)
        << " bytes=" << p->GetSize()
        << " " << AddrStr(h.GetSource()) << "->" << AddrStr(h.GetDestination())
        << " proto=" << int(h.GetProtocol())
        << "\n";
}

// ---- CSMA device traces ----
static void
CsmaMacTxTrace(Ptr<const Packet> p)
{
  if (!g_dbg.is_open())
    return;
  g_dbg << NowStr() << " [CSMA-MacTx] bytes=" << p->GetSize();
  PrintIpTuple(g_dbg, p);
  g_dbg << "\n";
}

static void
CsmaMacRxTrace(Ptr<const Packet> p)
{
  if (!g_dbg.is_open())
    return;
  g_dbg << NowStr() << " [CSMA-MacRx] bytes=" << p->GetSize();
  PrintIpTuple(g_dbg, p);
  g_dbg << "\n";
}

// ===================== Time series helpers =====================

static uint64_t
SumRxBytes()
{
  uint64_t s = 0;
  for (auto &sink : g_sinks)
  {
    s += sink->GetTotalRx();
  }
  return s;
}

static void
InitAggregatedSampling()
{
  g_lastSumRx = SumRxBytes();
}

static void
SampleAggregatedThroughput()
{
  const double now = Simulator::Now().GetSeconds();
  if (now + 1e-9 > g_simStop)
    return;

  const uint64_t cur = SumRxBytes();
  const uint64_t diff = (cur >= g_lastSumRx) ? (cur - g_lastSumRx) : 0;
  const double thrBps = (g_interval > 0.0) ? (8.0 * static_cast<double>(diff)) / g_interval : 0.0;
  g_lastSumRx = cur;

  if (g_tsCsv.is_open())
  {
    g_tsCsv << std::fixed << std::setprecision(6) << now << ","
            << std::setprecision(3) << thrBps << ","
            << cur << "\n";
    g_tsCsv.flush();
  }

  if (now + g_interval <= g_simStop + 1e-9)
    Simulator::Schedule(Seconds(g_interval), &SampleAggregatedThroughput);
}

// ===================== Main =====================

int main(int argc, char *argv[])
{
  double simTime = 20.0;
  double appStart = 3.0;
  uint64_t tcpMaxBytes = 0;

  int nSta = 2;
  double distance = 10.0;
  double radius = 10.0;

  std::string ssidStr = "wifi6-ter";
  std::string outDir = "results/p5";
  bool pcap = false;
  bool flowmon = true;
  int seed = 1;
  int run = 1;

  // Traffic
  std::string transport = "udp";       // "udp" or "tcp"
  std::string udpRatePerSta = "6Mbps"; // per-STA
  std::string tcpRatePerSta = "6Mbps"; // per-STA (OnOff over TCP uses DataRate too)
  int pktSize = 1200;

  // Timeseries
  double interval = 0.1; // seconds

  // PHY
  double txPowerDbm = 20.0;
  double noiseFigureDb = 7.0;
  double logExp = 3.0;
  double refDist = 1.0;
  double refLoss = 46.6777;

  // Rate control
  std::string rateManager = "ns3::MinstrelHtWifiManager";

  // -------------------- CommandLine --------------------
  CommandLine cmd;
  cmd.AddValue("simTime", "Total simulation time (s)", simTime);
  cmd.AddValue("appStart", "Application start time (s)", appStart);
  cmd.AddValue("nSta", "Number of STA nodes", nSta);
  cmd.AddValue("distance", "STA-AP distance for line placement (m)", distance);
  cmd.AddValue("radius", "Radius for circle placement (m). If >0, circle placement is used", radius);
  cmd.AddValue("ssid", "Wi-Fi SSID", ssidStr);
  cmd.AddValue("outDir", "Output directory", outDir);
  cmd.AddValue("pcap", "Enable PCAP", pcap);
  cmd.AddValue("flowmon", "Enable FlowMonitor", flowmon);
  cmd.AddValue("seed", "RNG seed", seed);
  cmd.AddValue("run", "RNG run", run);

  cmd.AddValue("transport", "udp|tcp", transport);
  cmd.AddValue("udpRatePerSta", "Per-STA UDP OnOff rate", udpRatePerSta);
  cmd.AddValue("tcpRatePerSta", "Per-STA TCP OnOff rate", tcpRatePerSta);
  cmd.AddValue("pktSize", "Application packet size (bytes)", pktSize);

  cmd.AddValue("interval", "Aggregated throughput sampling interval (s)", interval);

  cmd.AddValue("txPowerDbm", "Tx power (dBm)", txPowerDbm);
  cmd.AddValue("noiseFigureDb", "Rx noise figure (dB)", noiseFigureDb);
  cmd.AddValue("logExp", "LogDistance exponent", logExp);
  cmd.AddValue("refDist", "LogDistance reference distance (m)", refDist);
  cmd.AddValue("refLoss", "LogDistance reference loss (dB)", refLoss);

  cmd.AddValue("rateManager", "Wifi remote station manager TypeId", rateManager);
  cmd.AddValue("tcpMaxBytes", "MaxBytes for TCP BulkSend (0 = unlimited)", tcpMaxBytes);

  cmd.Parse(argc, argv);

  // -------------------- Validation --------------------
  if (nSta <= 0 || pktSize <= 0)
  {
    std::cerr << "ERROR: invalid nSta/pktSize\n";
    return 1;
  }
  if (simTime <= 0.0 || appStart < 0.0 || appStart >= simTime)
  {
    std::cerr << "ERROR: invalid simTime/appStart\n";
    return 1;
  }
  if (!(transport == "udp" || transport == "tcp"))
  {
    std::cerr << "ERROR: transport must be udp or tcp\n";
    return 1;
  }

  const double effectiveDistance = (radius > 0.0) ? radius : distance;

  g_interval = interval;
  g_simStop = simTime;

  // Enable node-level IPv4 forwarding by default (AP must route between subnets)
  // Config::SetDefault("ns3::Ipv4L3Protocol::IpForward", BooleanValue(true));

  // Reproducibility
  RngSeedManager::SetSeed(static_cast<uint32_t>(seed));
  RngSeedManager::SetRun(static_cast<uint64_t>(run));

  // Output directories
  std::filesystem::create_directories(outDir + "/raw");
  std::filesystem::create_directories(outDir + "/logs");
  std::filesystem::create_directories(outDir + "/plots");

  // Debug log file (traffic path)
  {
    std::ostringstream fp;
    fp << outDir << "/logs/p5_path_" << transport << "_n" << nSta << "_run" << run << ".log";
    g_dbg.open(fp.str(), std::ios::out);
    if (g_dbg.is_open())
    {
      g_dbg << "# time [TAG] details\n";
    }
  }

  // -------------------- Nodes --------------------
  NodeContainer staNodes;
  staNodes.Create(static_cast<uint32_t>(nSta));
  Ptr<Node> apNode = CreateObject<Node>();
  Ptr<Node> serverNode = CreateObject<Node>();

  // -------------------- Mobility --------------------
  MobilityHelper mobility;
  mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
  mobility.Install(NodeContainer(apNode, serverNode));
  mobility.Install(staNodes);

  apNode->GetObject<MobilityModel>()->SetPosition(Vector(0.0, 0.0, 0.0));
  serverNode->GetObject<MobilityModel>()->SetPosition(Vector(0.0, 1.0, 0.0));

  if (radius > 0.0)
  {
    for (int i = 0; i < nSta; ++i)
    {
      const double angle = 2.0 * M_PI * (static_cast<double>(i) / static_cast<double>(nSta));
      staNodes.Get(static_cast<uint32_t>(i))
          ->GetObject<MobilityModel>()
          ->SetPosition(Vector(radius * std::cos(angle), radius * std::sin(angle), 0.0));
    }
  }
  else
  {
    for (int i = 0; i < nSta; ++i)
    {
      staNodes.Get(static_cast<uint32_t>(i))
          ->GetObject<MobilityModel>()
          ->SetPosition(Vector(distance, 0.0, 0.0));
    }
  }

  // -------------------- Wi-Fi channel/PHY --------------------
  // IMPORTANT: do NOT use Default() + AddPropagationLoss() together (can chain losses).
  YansWifiChannelHelper channel;
  channel.SetPropagationDelay("ns3::ConstantSpeedPropagationDelayModel");
  channel.AddPropagationLoss("ns3::LogDistancePropagationLossModel",
                             "Exponent", DoubleValue(logExp),
                             "ReferenceDistance", DoubleValue(refDist),
                             "ReferenceLoss", DoubleValue(refLoss));

  YansWifiPhyHelper phy;
  phy.SetChannel(channel.Create());
  phy.Set("TxPowerStart", DoubleValue(txPowerDbm));
  phy.Set("TxPowerEnd", DoubleValue(txPowerDbm));
  phy.Set("RxNoiseFigure", DoubleValue(noiseFigureDb));

  // -------------------- Wi-Fi MAC + rate control --------------------
  WifiHelper wifi;
  wifi.SetStandard(WIFI_STANDARD_80211n);
  wifi.SetRemoteStationManager(rateManager);

  WifiMacHelper mac;
  Ssid ssid = Ssid(ssidStr);

  mac.SetType("ns3::StaWifiMac",
              "Ssid", SsidValue(ssid),
              "ActiveProbing", BooleanValue(false));
  NetDeviceContainer staDevs = wifi.Install(phy, mac, staNodes);

  mac.SetType("ns3::ApWifiMac",
              "Ssid", SsidValue(ssid));
  NetDeviceContainer apDev = wifi.Install(phy, mac, apNode);

  // -------------------- CSMA (AP <-> Server) --------------------
  CsmaHelper csma;
  csma.SetChannelAttribute("DataRate", StringValue("1Gbps"));
  csma.SetChannelAttribute("Delay", TimeValue(MicroSeconds(50)));
  NetDeviceContainer csmaDevs = csma.Install(NodeContainer(apNode, serverNode));

  // -------------------- Internet stack --------------------
  InternetStackHelper internet;
  internet.Install(staNodes);
  internet.Install(apNode);
  internet.Install(serverNode);

  // -------------------- Addressing --------------------
  Ipv4AddressHelper ipv4;

  // Wi-Fi subnet: 10.1.0.0/24
  ipv4.SetBase("10.1.0.0", "255.255.255.0");
  Ipv4InterfaceContainer staIfs = ipv4.Assign(staDevs);
  Ipv4InterfaceContainer apIfWifi = ipv4.Assign(apDev);

  // CSMA subnet: 10.2.0.0/24
  ipv4.SetBase("10.2.0.0", "255.255.255.0");
  Ipv4InterfaceContainer csmaIfs = ipv4.Assign(csmaDevs);

  // ===================== ROUTING (STATIC + GLOBAL FALLBACK) =====================
  Ipv4StaticRoutingHelper staticRouting;

  // Grab Ipv4 objects
  Ptr<Ipv4> apIpv4 = apNode->GetObject<Ipv4>();
  Ptr<Ipv4> srvIpv4 = serverNode->GetObject<Ipv4>();

  // Robust CSMA device mapping (don’t assume csmaDevs[0] is always on AP)
  Ptr<NetDevice> csmaDev0 = csmaDevs.Get(0);
  Ptr<NetDevice> csmaDev1 = csmaDevs.Get(1);
  Ptr<NetDevice> apCsmaDev = (csmaDev0->GetNode() == apNode) ? csmaDev0 : csmaDev1;
  Ptr<NetDevice> srvCsmaDev = (csmaDev0->GetNode() == serverNode) ? csmaDev0 : csmaDev1;

  // Interface indices
  const int32_t apWifiIf = apIpv4->GetInterfaceForDevice(apDev.Get(0)); // AP Wi-Fi
  const int32_t apCsmaIf = apIpv4->GetInterfaceForDevice(apCsmaDev);    // AP CSMA
  const int32_t srvCsmaIf = srvIpv4->GetInterfaceForDevice(srvCsmaDev); // Server CSMA

  NS_ABORT_MSG_IF(apWifiIf < 0 || apCsmaIf < 0 || srvCsmaIf < 0,
                  "Interface index lookup failed (AP/Server)");

  // Bring interfaces up + enable per-interface forwarding on AP
  apIpv4->SetUp(apWifiIf);
  apIpv4->SetUp(apCsmaIf);
  apIpv4->SetForwarding(apWifiIf, true);
  apIpv4->SetForwarding(apCsmaIf, true);

  // AP IPs (next-hops)
  const Ipv4Address apWifiIp = apIfWifi.GetAddress(0); // 10.1.0.x (AP)

  // csmaIfs.GetAddress(0/1) order matches csmaDevs.Get(0/1). We map by device ownership.
  Ipv4Address apCsmaIp;
  Ipv4Address serverIp;
  if (apCsmaDev == csmaDev0)
  {
    apCsmaIp = csmaIfs.GetAddress(0);
    serverIp = csmaIfs.GetAddress(1);
  }
  else
  {
    apCsmaIp = csmaIfs.GetAddress(1);
    serverIp = csmaIfs.GetAddress(0);
  }

  // ---- AP: explicit routes for both directly-connected networks ----
  Ptr<Ipv4StaticRouting> apSr = staticRouting.GetStaticRouting(apIpv4);
  apSr->AddNetworkRouteTo(Ipv4Address("10.1.0.0"), Ipv4Mask("255.255.255.0"), apWifiIf);
  apSr->AddNetworkRouteTo(Ipv4Address("10.2.0.0"), Ipv4Mask("255.255.255.0"), apCsmaIf);

  // ---- STA: default route -> AP over Wi-Fi ----
  for (int i = 0; i < nSta; ++i)
  {
    Ptr<Ipv4> staIpv4 = staNodes.Get(static_cast<uint32_t>(i))->GetObject<Ipv4>();
    const int32_t staWifiIf = staIpv4->GetInterfaceForDevice(staDevs.Get(static_cast<uint32_t>(i)));
    NS_ABORT_MSG_IF(staWifiIf < 0, "STA interface index not found");
    Ptr<Ipv4StaticRouting> staSr = staticRouting.GetStaticRouting(staIpv4);
    staSr->SetDefaultRoute(apWifiIp, staWifiIf);
  }

  // ---- Server: default route -> AP over CSMA ----
  {
    Ptr<Ipv4StaticRouting> srvSr = staticRouting.GetStaticRouting(srvIpv4);
    srvSr->SetDefaultRoute(apCsmaIp, srvCsmaIf);
  }

  // Fallback: populate global routing tables too (does NOT remove static routes)
  Ipv4GlobalRoutingHelper::PopulateRoutingTables();

  // Print once (helps debugging)
  std::cout << "[P5] apWifiIp=" << apWifiIp
            << " apCsmaIp=" << apCsmaIp
            << " serverIp=" << serverIp
            << std::endl;

  // Also write routing tables to a file
  {
    std::ostringstream rtf;
    rtf << outDir << "/logs/routing_run" << run << ".txt";
    Ptr<OutputStreamWrapper> routingStream = Create<OutputStreamWrapper>(rtf.str(), std::ios::out);
    Ipv4GlobalRoutingHelper gr;
    gr.PrintRoutingTableAllAt(Seconds(1.0), routingStream);
  }

  // =======================================================================

  // -------------------- Traffic path tracing (L3 + CSMA) --------------------
  if (g_dbg.is_open())
  {
    Config::ConnectWithoutContext("/NodeList/*/$ns3::Ipv4L3Protocol/Tx", MakeCallback(&Ipv4TxTrace));
    Config::ConnectWithoutContext("/NodeList/*/$ns3::Ipv4L3Protocol/Rx", MakeCallback(&Ipv4RxTrace));
    Config::ConnectWithoutContext("/NodeList/*/$ns3::Ipv4L3Protocol/Drop",
                              MakeCallback(&Ipv4DropTrace));


    Config::ConnectWithoutContext("/NodeList/*/DeviceList/*/$ns3::CsmaNetDevice/MacTx", MakeCallback(&CsmaMacTxTrace));
    Config::ConnectWithoutContext("/NodeList/*/DeviceList/*/$ns3::CsmaNetDevice/MacRx", MakeCallback(&CsmaMacRxTrace));
  }

  // -------------------- Applications (per-STA ports) --------------------
  const uint16_t basePort = 9000;
  g_sinks.clear();
  g_sinks.reserve(static_cast<size_t>(nSta));

  for (int i = 0; i < nSta; ++i)
  {
    const uint16_t port = static_cast<uint16_t>(basePort + i);

    PacketSinkHelper sinkHelper(
        (transport == "udp") ? "ns3::UdpSocketFactory" : "ns3::TcpSocketFactory",
        InetSocketAddress(Ipv4Address::GetAny(), port));

    ApplicationContainer sinkApp = sinkHelper.Install(serverNode);
    sinkApp.Start(Seconds(0.0));
    sinkApp.Stop(Seconds(simTime));

    Ptr<PacketSink> sink = DynamicCast<PacketSink>(sinkApp.Get(0));
    NS_ABORT_MSG_IF(!sink, "PacketSink cast failed");
    sink->TraceConnectWithoutContext("Rx", MakeBoundCallback(&SinkRxTrace, std::string("SERVER-sink") + std::to_string(i)));
    g_sinks.push_back(sink);

    Address dest(InetSocketAddress(serverIp, port));

    if (transport == "udp")
    {
      OnOffHelper onoff("ns3::UdpSocketFactory", dest);
      onoff.SetAttribute("DataRate", StringValue(udpRatePerSta));
      onoff.SetAttribute("PacketSize", UintegerValue(static_cast<uint32_t>(pktSize)));
      onoff.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1]"));
      onoff.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));

      ApplicationContainer app = onoff.Install(staNodes.Get(static_cast<uint32_t>(i)));
      app.Get(0)->TraceConnectWithoutContext("Tx", MakeBoundCallback(&AppTxTrace, std::string("STA") + std::to_string(i)));
      app.Start(Seconds(appStart));
      app.Stop(Seconds(simTime));
    }
    else
    {
      BulkSendHelper bulk("ns3::TcpSocketFactory", dest);
      bulk.SetAttribute("MaxBytes", UintegerValue(tcpMaxBytes));
      ApplicationContainer app = bulk.Install(staNodes.Get(i));
      app.Start(Seconds(appStart));
      app.Stop(Seconds(simTime));
    }
  }

  // -------------------- Optional: aggregated throughput time series --------------------
  if (interval > 0.0)
  {
    std::ostringstream tsName;
    tsName << outDir << "/raw/ts_" << transport << "_n" << nSta << "_run" << run << ".csv";
    g_tsCsv.open(tsName.str(), std::ios::out);
    if (g_tsCsv.is_open())
    {
      g_tsCsv << "time_s,throughput_bps,sumRxBytes\n";
      g_tsCsv.flush();
      Simulator::Schedule(Seconds(appStart), &InitAggregatedSampling);
      Simulator::Schedule(Seconds(appStart), &SampleAggregatedThroughput);
    }
  }

  // -------------------- PCAP --------------------
  if (pcap)
  {
    std::ostringstream base;
    base << outDir << "/raw/p5_" << transport << "_n" << nSta << "_run" << run;
    phy.EnablePcap(base.str() + "_ap", apDev.Get(0), true);
    csma.EnablePcap(base.str() + "_csma", csmaDevs.Get(0), true);
  }

  // -------------------- FlowMonitor --------------------
  Ptr<FlowMonitor> monitor;
  FlowMonitorHelper flowmonHelper;
  if (flowmon)
  {
    monitor = flowmonHelper.InstallAll();
  }

  // -------------------- Run --------------------
  Simulator::Stop(Seconds(simTime));
  Simulator::Run();

  // -------------------- FlowMonitor output --------------------
  if (flowmon && monitor)
  {
    monitor->CheckForLostPackets();
    std::ostringstream xmlPath;
    xmlPath << outDir << "/raw/flowmon_" << transport << "_n" << nSta << "_run" << run << ".xml";
    monitor->SerializeToXmlFile(xmlPath.str(), true, true);
  }

  // -------------------- Metrics --------------------
  const double utile = simTime - appStart;

  std::vector<uint64_t> rxBytes(static_cast<size_t>(nSta), 0);
  std::vector<double> goodputBps(static_cast<size_t>(nSta), 0.0);

  for (int i = 0; i < nSta; ++i)
  {
    rxBytes[static_cast<size_t>(i)] = g_sinks[static_cast<size_t>(i)]->GetTotalRx();
    goodputBps[static_cast<size_t>(i)] =
        (utile > 0.0) ? (8.0 * static_cast<double>(rxBytes[static_cast<size_t>(i)]) / utile) : 0.0;
  }

  double sumGoodput = 0.0;
  for (double v : goodputBps)
    sumGoodput += v;

  const double jain = ComputeJain(goodputBps);

  // per-STA CSV
  {
    std::ostringstream perstaPath;
    perstaPath << outDir << "/raw/persta_" << transport << "_n" << nSta << "_run" << run << ".csv";
    std::ofstream persta(perstaPath.str(), std::ios::out);
    persta << "staId,rxBytes,goodputbps\n";
    for (int i = 0; i < nSta; ++i)
    {
      persta << i << ","
             << rxBytes[static_cast<size_t>(i)] << ","
             << std::fixed << std::setprecision(3) << goodputBps[static_cast<size_t>(i)] << "\n";
    }
  }

  // Summary CSV (append)
// Summary CSV (append) - normalized schema for Project 5
{
  std::ostringstream sumPath;
  sumPath << outDir << "/raw/p5_summary.csv";
  const bool fileExists = IsFileNonEmpty(sumPath.str());

  std::ofstream sum(sumPath.str(), std::ios::out | std::ios::app);
  if (!fileExists)
  {
    sum << "transport,nSta,run,seed,distance,pktSize,udpRatePerSta,tcpMaxBytes,appStart,simTime,"
           "sumGoodputbps,meanGoodputbps,jain\n";
  }

  const double meanGoodput = (nSta > 0) ? (sumGoodput / static_cast<double>(nSta)) : 0.0;

  sum << transport << ","
      << nSta << ","
      << run << ","
      << seed << ","
      << std::fixed << std::setprecision(3) << effectiveDistance << ","
      << pktSize << ","
      << udpRatePerSta << ","
      << tcpMaxBytes << ","
      << std::fixed << std::setprecision(3) << appStart << ","
      << std::fixed << std::setprecision(3) << simTime << ","
      << std::fixed << std::setprecision(3) << sumGoodput << ","
      << std::fixed << std::setprecision(3) << meanGoodput << ","
      << std::fixed << std::setprecision(6) << jain << "\n";
}

  if (g_tsCsv.is_open())
  {
    g_tsCsv.flush();
    g_tsCsv.close();
  }

  if (g_dbg.is_open())
  {
    g_dbg.flush();
    g_dbg.close();
  }

  Simulator::Destroy();

  std::cout << "[P5] transport=" << transport
            << " nSta=" << nSta
            << " effectiveDistance=" << effectiveDistance
            << " pktSize=" << pktSize
            << " rateManager=" << rateManager
            << " seed=" << seed
            << " run=" << run
            << " sumGoodput(Mbps)=" << (sumGoodput / 1e6)
            << " jain=" << jain
            << std::endl;

  return 0;
}
