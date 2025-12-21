#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/wifi-module.h"
#include "ns3/csma-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/propagation-module.h"

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

static bool
IsFileNonEmpty (const std::string& path)
{
  std::error_code ec;
  if (!std::filesystem::exists (path, ec))
    return false;
  if (std::filesystem::is_directory (path, ec))
    return false;
  return (std::filesystem::file_size (path, ec) > 0);
}

static double
ComputeJain (const std::vector<double>& x)
{
  if (x.empty ())
    return 0.0;

  double sum = 0.0;
  double sumSq = 0.0;
  for (double v : x)
    {
      sum += v;
      sumSq += v * v;
    }
  if (sumSq <= 0.0)
    return 0.0;

  const double n = static_cast<double> (x.size ());
  return (sum * sum) / (n * sumSq);
}

static std::vector<Ptr<PacketSink>> g_sinks;
static uint64_t g_lastSumRx = 0;
static double g_interval = 0.5;
static double g_simStop = 20.0;
static std::ofstream g_tsCsv;

static uint64_t
SumRxBytes ()
{
  uint64_t s = 0;
  for (const auto& sink : g_sinks)
    {
      if (sink)
        s += sink->GetTotalRx ();
    }
  return s;
}

static void
InitAggregatedSampling ()
{
  g_lastSumRx = SumRxBytes ();
}

static void
SampleAggregatedThroughput ()
{
  const double now = Simulator::Now ().GetSeconds ();
  if (now + 1e-9 > g_simStop)
    return;

  const uint64_t cur = SumRxBytes ();
  const uint64_t diff = (cur >= g_lastSumRx) ? (cur - g_lastSumRx) : 0;

  const double thrBps = (8.0 * static_cast<double> (diff)) / g_interval;
  g_lastSumRx = cur;

  if (g_tsCsv.is_open ())
    {
      g_tsCsv << std::fixed << std::setprecision (6) << now << ","
              << std::setprecision (3) << thrBps << ","
              << cur << "\n";
      g_tsCsv.flush ();
    }

  if (now + g_interval <= g_simStop + 1e-9)
    Simulator::Schedule (Seconds (g_interval), &SampleAggregatedThroughput);
}

int
main (int argc, char* argv[])
{
  double simTime = 20.0;
  double appStart = 2.0;

  int nSta = 2;
  double distance = 10.0;
  double radius = 10.0;

  std::string ssidStr = "wifi6-ter";
  std::string outDir = "results/p5";
  bool pcap = false;
  bool flowmon = true;
  int seed = 1;
  int run = 1;

  std::string transport = "udp";
  int pktSize = 1200;
  std::string udpRatePerSta = "10Mbps";
  uint64_t tcpMaxBytes = 0;

  double interval = 0.5;

  double txPowerDbm = 20.0;
  double noiseFigureDb = 7.0;

  double logExp = 3.0;
  double refDist = 1.0;
  double refLoss = 46.6777;

  std::string rateManager = "ns3::MinstrelHtWifiManager";

  CommandLine cmd;
  cmd.AddValue ("simTime", "Total simulation time (s)", simTime);
  cmd.AddValue ("appStart", "Application start time (s)", appStart);
  cmd.AddValue ("nSta", "Number of STA nodes", nSta);
  cmd.AddValue ("distance", "STA-AP distance for line placement (m)", distance);
  cmd.AddValue ("radius", "Radius for circle placement (m). If >0, used as effective distance", radius);
  cmd.AddValue ("ssid", "Wi-Fi SSID", ssidStr);
  cmd.AddValue ("outDir", "Output directory", outDir);
  cmd.AddValue ("pcap", "Enable PCAP", pcap);
  cmd.AddValue ("flowmon", "Enable FlowMonitor", flowmon);
  cmd.AddValue ("seed", "RNG seed", seed);
  cmd.AddValue ("run", "RNG run number", run);

  cmd.AddValue ("transport", "udp|tcp", transport);
  cmd.AddValue ("pktSize", "Packet size (bytes)", pktSize);
  cmd.AddValue ("udpRatePerSta", "UDP offered load per STA (e.g., 10Mbps)", udpRatePerSta);
  cmd.AddValue ("tcpMaxBytes", "TCP max bytes per STA (0 = unlimited)", tcpMaxBytes);

  cmd.AddValue ("interval", "Throughput sampling interval (s)", interval);

  cmd.AddValue ("txPowerDbm", "Tx power (dBm)", txPowerDbm);
  cmd.AddValue ("noiseFigureDb", "Rx noise figure (dB)", noiseFigureDb);
  cmd.AddValue ("logExp", "LogDistance exponent", logExp);
  cmd.AddValue ("refDist", "LogDistance reference distance (m)", refDist);
  cmd.AddValue ("refLoss", "LogDistance reference loss (dB)", refLoss);
  cmd.AddValue ("rateManager", "Wifi remote station manager TypeId", rateManager);

  cmd.Parse (argc, argv);

  if (nSta <= 0 || pktSize <= 0 || interval <= 0.0)
    {
      std::cerr << "ERROR: invalid nSta/pktSize/interval\n";
      return 1;
    }
  if (simTime <= 0.0 || appStart < 0.0 || appStart >= simTime)
    {
      std::cerr << "ERROR: invalid simTime/appStart\n";
      return 1;
    }
  if (transport != "udp" && transport != "tcp")
    {
      std::cerr << "ERROR: transport must be udp or tcp\n";
      return 1;
    }

  const double effectiveDistance = (radius > 0.0) ? radius : distance;

  g_interval = interval;
  g_simStop = simTime;

  RngSeedManager::SetSeed (static_cast<uint32_t> (seed));
  RngSeedManager::SetRun (static_cast<uint64_t> (run));

  std::filesystem::create_directories (outDir + "/raw");
  std::filesystem::create_directories (outDir + "/logs");
  std::filesystem::create_directories (outDir + "/plots");

  NodeContainer staNodes;
  staNodes.Create (static_cast<uint32_t> (nSta));
  Ptr<Node> apNode = CreateObject<Node> ();
  Ptr<Node> serverNode = CreateObject<Node> ();

  MobilityHelper mobility;
  mobility.SetMobilityModel ("ns3::ConstantPositionMobilityModel");
  mobility.Install (NodeContainer (apNode, serverNode));
  mobility.Install (staNodes);

  apNode->GetObject<MobilityModel> ()->SetPosition (Vector (0.0, 0.0, 0.0));
  serverNode->GetObject<MobilityModel> ()->SetPosition (Vector (0.0, 1.0, 0.0));

  if (radius > 0.0)
    {
      for (int i = 0; i < nSta; ++i)
        {
          const double angle = 2.0 * M_PI * (static_cast<double> (i) / static_cast<double> (nSta));
          staNodes.Get (static_cast<uint32_t> (i))
            ->GetObject<MobilityModel> ()
            ->SetPosition (Vector (radius * std::cos (angle), radius * std::sin (angle), 0.0));
        }
    }
  else
    {
      for (int i = 0; i < nSta; ++i)
        {
          staNodes.Get (static_cast<uint32_t> (i))
            ->GetObject<MobilityModel> ()
            ->SetPosition (Vector (distance, 0.0, 0.0));
        }
    }

  YansWifiChannelHelper channel = YansWifiChannelHelper::Default ();
  channel.SetPropagationDelay ("ns3::ConstantSpeedPropagationDelayModel");
  channel.AddPropagationLoss ("ns3::LogDistancePropagationLossModel",
                             "Exponent", DoubleValue (logExp),
                             "ReferenceDistance", DoubleValue (refDist),
                             "ReferenceLoss", DoubleValue (refLoss));

  YansWifiPhyHelper phy;
  phy.SetChannel (channel.Create ());
  phy.Set ("TxPowerStart", DoubleValue (txPowerDbm));
  phy.Set ("TxPowerEnd", DoubleValue (txPowerDbm));
  phy.Set ("RxNoiseFigure", DoubleValue (noiseFigureDb));

  WifiHelper wifi;
  wifi.SetStandard (WIFI_STANDARD_80211ac);
  wifi.SetRemoteStationManager (rateManager);

  WifiMacHelper mac;
  Ssid ssid = Ssid (ssidStr);

  mac.SetType ("ns3::StaWifiMac", "Ssid", SsidValue (ssid), "ActiveProbing", BooleanValue (false));
  NetDeviceContainer staDevs = wifi.Install (phy, mac, staNodes);

  mac.SetType ("ns3::ApWifiMac", "Ssid", SsidValue (ssid));
  NetDeviceContainer apDev = wifi.Install (phy, mac, apNode);

  CsmaHelper csma;
  csma.SetChannelAttribute ("DataRate", StringValue ("1Gbps"));
  csma.SetChannelAttribute ("Delay", TimeValue (MicroSeconds (50)));
  NetDeviceContainer csmaDevs = csma.Install (NodeContainer (apNode, serverNode));

  InternetStackHelper internet;
  internet.Install (staNodes);
  internet.Install (apNode);
  internet.Install (serverNode);

  Ipv4AddressHelper ipv4;
  ipv4.SetBase ("10.1.0.0", "255.255.255.0");
  Ipv4InterfaceContainer staIfs = ipv4.Assign (staDevs);
  Ipv4InterfaceContainer apIfWifi = ipv4.Assign (apDev);

  ipv4.SetBase ("10.2.0.0", "255.255.255.0");
  Ipv4InterfaceContainer csmaIfs = ipv4.Assign (csmaDevs);
  const Ipv4Address serverIp = csmaIfs.GetAddress (1);

  Ipv4GlobalRoutingHelper::PopulateRoutingTables ();

  const uint16_t basePort = 9000;
  g_sinks.clear ();
  g_sinks.reserve (static_cast<size_t> (nSta));

  for (int i = 0; i < nSta; ++i)
    {
      const uint16_t port = static_cast<uint16_t> (basePort + i);

      PacketSinkHelper sinkHelper (
        (transport == "udp") ? "ns3::UdpSocketFactory" : "ns3::TcpSocketFactory",
        InetSocketAddress (Ipv4Address::GetAny (), port));

      ApplicationContainer sinkApp = sinkHelper.Install (serverNode);
      sinkApp.Start (Seconds (0.0));
      sinkApp.Stop (Seconds (simTime));

      Ptr<PacketSink> sink = DynamicCast<PacketSink> (sinkApp.Get (0));
      if (!sink)
        {
          std::cerr << "ERROR: PacketSink cast failed\n";
          return 1;
        }
      g_sinks.push_back (sink);

      Address dest (InetSocketAddress (serverIp, port));

      if (transport == "udp")
        {
          OnOffHelper onoff ("ns3::UdpSocketFactory", dest);
          onoff.SetAttribute ("DataRate", StringValue (udpRatePerSta));
          onoff.SetAttribute ("PacketSize", UintegerValue (static_cast<uint32_t> (pktSize)));
          onoff.SetAttribute ("OnTime", StringValue ("ns3::ConstantRandomVariable[Constant=1]"));
          onoff.SetAttribute ("OffTime", StringValue ("ns3::ConstantRandomVariable[Constant=0]"));

          ApplicationContainer app = onoff.Install (staNodes.Get (static_cast<uint32_t> (i)));
          app.Start (Seconds (appStart));
          app.Stop (Seconds (simTime));
        }
      else
        {
          BulkSendHelper bulk ("ns3::TcpSocketFactory", dest);
          bulk.SetAttribute ("MaxBytes", UintegerValue (tcpMaxBytes));
          bulk.SetAttribute ("SendSize", UintegerValue (static_cast<uint32_t> (pktSize)));

          ApplicationContainer app = bulk.Install (staNodes.Get (static_cast<uint32_t> (i)));
          app.Start (Seconds (appStart));
          app.Stop (Seconds (simTime));
        }
    }

  {
    std::ostringstream tsName;
    tsName << outDir << "/raw/ts_" << transport << "_n" << nSta << "_run" << run << ".csv";
    g_tsCsv.open (tsName.str (), std::ios::out);
    if (g_tsCsv.is_open ())
      {
        g_tsCsv << "time_s,throughput_bps,sumRxBytes\n";
        g_tsCsv.flush ();
      }
    Simulator::Schedule (Seconds (appStart), &InitAggregatedSampling);
    Simulator::Schedule (Seconds (appStart + g_interval), &SampleAggregatedThroughput);
  }

  if (pcap)
    {
      std::ostringstream base;
      base << outDir << "/raw/p5_" << transport << "_n" << nSta << "_run" << run;
      phy.EnablePcap (base.str () + "_ap", apDev.Get (0), true);
      csma.EnablePcap (base.str () + "_csma", csmaDevs.Get (0), true);
    }

  FlowMonitorHelper fmHelper;
  Ptr<FlowMonitor> monitor;
  if (flowmon)
    monitor = fmHelper.InstallAll ();

  Simulator::Stop (Seconds (simTime));
  Simulator::Run ();

  if (g_tsCsv.is_open ())
    g_tsCsv.close ();

  const double usefulTime = simTime - appStart;

  std::vector<uint64_t> rxBytes (static_cast<size_t> (nSta), 0);
  std::vector<double> goodputBps (static_cast<size_t> (nSta), 0.0);

  for (int i = 0; i < nSta; ++i)
    {
      rxBytes[static_cast<size_t> (i)] = g_sinks[static_cast<size_t> (i)]->GetTotalRx ();
      goodputBps[static_cast<size_t> (i)] =
        (usefulTime > 0.0) ? (8.0 * static_cast<double> (rxBytes[static_cast<size_t> (i)]) / usefulTime) : 0.0;
    }

  const double sumGoodput = std::accumulate (goodputBps.begin (), goodputBps.end (), 0.0);
  const double meanGoodput = sumGoodput / static_cast<double> (nSta);
  const double jain = ComputeJain (goodputBps);

  {
    std::ostringstream perstaPath;
    perstaPath << outDir << "/raw/persta_" << transport << "_n" << nSta << "_run" << run << ".csv";
    std::ofstream persta (perstaPath.str (), std::ios::out);
    persta << "staId,rxBytes,goodputbps\n";
    for (int i = 0; i < nSta; ++i)
      {
        persta << i << ","
               << rxBytes[static_cast<size_t> (i)] << ","
               << std::fixed << std::setprecision (3) << goodputBps[static_cast<size_t> (i)] << "\n";
      }
  }

  {
    const std::string summaryPath = outDir + "/raw/p5_summary.csv";
    const bool needHeader = !IsFileNonEmpty (summaryPath);

    std::ofstream sum (summaryPath, std::ios::out | std::ios::app);
    if (needHeader)
      {
        sum << "transport,nSta,distance,pktSize,udpRatePerSta,tcpMaxBytes,seed,run,"
               "sumGoodputbps,meanGoodputbps,jainFairness\n";
      }

    const std::string udpRateOut = (transport == "udp") ? udpRatePerSta : "0";
    const uint64_t tcpMaxBytesOut = (transport == "tcp") ? tcpMaxBytes : 0;

    sum << transport << ","
        << nSta << ","
        << std::fixed << std::setprecision (3) << effectiveDistance << ","
        << pktSize << ","
        << udpRateOut << ","
        << tcpMaxBytesOut << ","
        << seed << ","
        << run << ","
        << std::fixed << std::setprecision (3) << sumGoodput << ","
        << meanGoodput << ","
        << jain << "\n";
  }

  if (flowmon && monitor)
    {
      monitor->CheckForLostPackets ();
      std::ostringstream fmPath;
      fmPath << outDir << "/raw/flowmon_" << transport << "_n" << nSta << "_run" << run << ".xml";
      monitor->SerializeToXmlFile (fmPath.str (), true, true);
    }

  Simulator::Destroy ();

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
