/*
 * Project 8 (ns-3.41): QoS / WMM (802.11e/EDCA) — VoIP (VO), Video (VI) vs Best Effort (BE)
 *
 * What this program does:
 *  - Builds a single-AP Wi-Fi scenario with 3 STAs (fixed positions):
 *      STA0 = VoIP (VO), STA1 = Video (VI), STA2 = Best Effort (BE)
 *  - Generates uplink traffic (STA -> AP) to create real medium contention.
 *  - Supports two modes:
 *      --mode=OFF : QosSupported=false on STA/AP (DCF / Best Effort behavior)
 *      --mode=ON  : QosSupported=true on STA/AP (WMM/EDCA enabled)
 *  - When mode=ON, marks packets using IP TOS/DSCP via Socket::SetIpTos():
 *      VO: DSCP EF=46 => TOS 0xB8
 *      VI: DSCP AF41=34 => TOS 0x88
 *      BE: DSCP 0      => TOS 0x00
 *
 * Metrics (via FlowMonitor):
 *  - Goodput (Mbps): VO, VI, BE
 *  - Delay/Jitter/Loss for VO and VI
 *
 * Output:
 *  - Appends one CSV line per run to:
 *      <outDir>/raw/p8_summary.csv
 *    NOTE: The runner script should create the CSV header first.
 *
 * ns-3.41 compatibility notes:
 *  - Use WifiMacHelper (not QosWifiMacHelper) and toggle QosSupported attribute.
 *  - Use YansWifiPhyHelper phy; (no YansWifiPhyHelper::Default()).
 *  - Use EventId::IsPending() (no IsRunning()).
 */

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/wifi-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"

#include <fstream>
#include <iomanip>
#include <sstream>

using namespace ns3;

// ---------------------------------------------------------------------
// A minimal UDP CBR generator that sets DSCP/TOS using Socket::SetIpTos()
// ---------------------------------------------------------------------
class DscpUdpCbrApp : public Application
{
public:
  DscpUdpCbrApp() = default;
  ~DscpUdpCbrApp() override = default;

  void Configure(const Address &peer,
                 uint32_t packetSizeBytes,
                 Time interval,
                 uint8_t ipTos,
                 bool verbose)
  {
    m_peer = peer;
    m_packetSize = packetSizeBytes;
    m_interval = interval;
    m_tos = ipTos;
    m_verbose = verbose;
  }

private:
  void StartApplication() override
  {
    // Create UDP socket
    m_socket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
    NS_ASSERT(m_socket);

    // Set IP TOS/DSCP for QoS classification
    // VO: 0xB8 (EF=46), VI: 0x88 (AF41=34), BE: 0x00
    m_socket->SetIpTos(m_tos);

    // Connect to remote endpoint
    if (!InetSocketAddress::IsMatchingType(m_peer))
    {
      NS_FATAL_ERROR("Peer must be InetSocketAddress");
    }
    m_socket->Connect(m_peer);

    m_running = true;
    m_sent = 0;

    // Kick off transmission immediately
    ScheduleNextTx(Seconds(0.0));
  }

  void StopApplication() override
  {
    m_running = false;

    // In ns-3.41, EventId has IsPending() (no IsRunning()).
    if (m_sendEvent.IsPending())
    {
      Simulator::Cancel(m_sendEvent);
    }

    if (m_socket)
    {
      m_socket->Close();
      m_socket = nullptr;
    }
  }

  void ScheduleNextTx(Time delay)
  {
    if (!m_running)
    {
      return;
    }
    m_sendEvent = Simulator::Schedule(delay, &DscpUdpCbrApp::SendPacket, this);
  }

  void SendPacket()
  {
    if (!m_running)
    {
      return;
    }

    Ptr<Packet> p = Create<Packet>(m_packetSize);
    m_socket->Send(p);
    m_sent++;

    if (m_verbose && (m_sent % 200 == 0))
    {
      NS_LOG_UNCOND("[" << Simulator::Now().GetSeconds() << "s] "
                        << "Node " << GetNode()->GetId()
                        << " sent " << m_sent
                        << " packets, tos=0x" << std::hex << (uint32_t)m_tos << std::dec);
    }

    ScheduleNextTx(m_interval);
  }

private:
  Ptr<Socket> m_socket;
  Address m_peer;

  uint32_t m_packetSize{1200};
  Time m_interval{MilliSeconds(10)};
  uint8_t m_tos{0x00};
  bool m_verbose{false};

  bool m_running{false};
  uint64_t m_sent{0};
  EventId m_sendEvent;
};

// ------------------------------------------------------------
// Append one line to the P8 summary CSV (header by bash script)
// ------------------------------------------------------------
static void AppendSummaryLine(const std::string &outDir,
                              const std::string &mode,
                              uint32_t beRateMbps,
                              uint32_t seed,
                              uint32_t run,
                              double goodputBE,
                              double goodputVO,
                              double goodputVI,
                              double delayVOms,
                              double jitterVOms,
                              double lossVO,
                              double delayVIms,
                              double jitterVIms,
                              double lossVI)
{
  const std::string path = outDir + "/raw/p8_summary.csv";
  std::ofstream f(path, std::ios::app);

  if (!f.is_open())
  {
    NS_LOG_UNCOND("ERROR: cannot open " << path << " (did you create outDir/raw and the CSV header?)");
    return;
  }

  f << std::fixed << std::setprecision(6)
    << mode << ","
    << beRateMbps << ","
    << seed << ","
    << run << ","
    << goodputBE << ","
    << goodputVO << ","
    << goodputVI << ","
    << delayVOms << ","
    << jitterVOms << ","
    << lossVO << ","
    << delayVIms << ","
    << jitterVIms << ","
    << lossVI
    << "\n";

  f.close();
}

// ------------------------------
// Wi-Fi standard parser
// ------------------------------
static WifiStandard ParseStandard(const std::string &s)
{
  if (s == "ax")
    return WIFI_STANDARD_80211ax;
  if (s == "ac")
    return WIFI_STANDARD_80211ac;
  if (s == "n")
    return WIFI_STANDARD_80211n;
  return WIFI_STANDARD_80211ax;
}

static void Ipv4TxTrace(std::string context, Ptr<const Packet> p, Ptr<Ipv4> ipv4, uint32_t iface)
{
  Ptr<Packet> copy = p->Copy();
  Ipv4Header ip;
  if (copy->PeekHeader(ip))
  {
    uint8_t tos = ip.GetTos();
    NS_LOG_UNCOND(Simulator::Now().GetSeconds()
                  << " IPv4-TX " << context
                  << " uid=" << p->GetUid()
                  << " tos=0x" << std::hex << (uint32_t)tos << std::dec);
  }
}

int main(int argc, char **argv)
{
  // ---------------- CLI parameters ----------------
  std::string mode = "ON"; // OFF | ON
  uint32_t beRateMbps = 40;
  double duration = 30.0;
  double appStart = 1.0;
  uint32_t seed = 1;
  uint32_t run = 0;
  std::string outDir = "results/p8";
  bool flowmon = true;
  bool pcap = false;
  bool verboseApp = false;

  // Wi-Fi knobs
  std::string standardStr = "ax"; // ax|ac|n
  std::string dataMode = "HeMcs7";
  std::string ctrlMode = "HeMcs0";
  double txPowerDbm = 16.0;
  uint32_t channelWidth = 20;

  // Traffic knobs
  uint32_t voPktSize = 160;
  double voPps = 50.0;

  uint32_t viPktSize = 1200;
  double viRateMbps = 6.0;

  uint32_t bePktSize = 1200;

  uint32_t channelNumber = 36;

  CommandLine cmd;
  cmd.AddValue("mode", "QoS mode: OFF or ON", mode);
  cmd.AddValue("beRateMbps", "Best Effort background rate (Mbps)", beRateMbps);
  cmd.AddValue("duration", "Simulation duration (s)", duration);
  cmd.AddValue("appStart", "Applications start time (s)", appStart);
  cmd.AddValue("seed", "RNG seed", seed);
  cmd.AddValue("run", "RNG run", run);
  cmd.AddValue("outDir", "Output directory root (e.g., results/p8_qos)", outDir);
  cmd.AddValue("flowmon", "Enable FlowMonitor", flowmon);
  cmd.AddValue("pcap", "Enable PCAP captures", pcap);
  cmd.AddValue("verboseApp", "Verbose app prints", verboseApp);

  cmd.AddValue("standard", "Wi-Fi standard: ax|ac|n", standardStr);
  cmd.AddValue("dataMode", "ConstantRateWifiManager DataMode (e.g., HeMcs7, VhtMcs7)", dataMode);
  cmd.AddValue("ctrlMode", "ConstantRateWifiManager ControlMode (e.g., HeMcs0, VhtMcs0)", ctrlMode);
  cmd.AddValue("txPowerDbm", "Tx power (dBm)", txPowerDbm);
  cmd.AddValue("channelWidth", "Channel width (MHz)", channelWidth);

  cmd.AddValue("voPktSize", "VO packet size (bytes)", voPktSize);
  cmd.AddValue("voPps", "VO packets per second", voPps);
  cmd.AddValue("viPktSize", "VI packet size (bytes)", viPktSize);
  cmd.AddValue("viRateMbps", "VI rate (Mbps)", viRateMbps);
  cmd.AddValue("bePktSize", "BE packet size (bytes)", bePktSize);

  cmd.AddValue("channelNumber", "Wi-Fi channel number (e.g., 36 for 5GHz)", channelNumber);

  cmd.Parse(argc, argv);

  if (voPps <= 0.0)
  {
    NS_FATAL_ERROR("voPps must be > 0");
  }
  if (viRateMbps <= 0.0)
  {
    NS_FATAL_ERROR("viRateMbps must be > 0");
  }
  if (channelWidth != 20 && channelWidth != 40 && channelWidth != 80 && channelWidth != 160)
  {
    NS_LOG_UNCOND("WARN: unusual channelWidth=" << channelWidth << " MHz");
  }

  // Normalize mode for robustness (accept ON/on/On, OFF/off/Off)
  for (auto &c : mode)
  {
    c = std::toupper(static_cast<unsigned char>(c));
  }
  if (mode != "ON" && mode != "OFF")
  {
    NS_FATAL_ERROR("Invalid --mode. Use ON or OFF.");
  }
  const bool qosOn = (mode == "ON");

  // ---------------- Reproducibility ----------------
  RngSeedManager::SetSeed(seed);
  RngSeedManager::SetRun(run);

  // ---------------- Nodes ----------------
  NodeContainer apNode;
  apNode.Create(1);
  NodeContainer staNodes;
  staNodes.Create(3); // STA0=VO, STA1=VI, STA2=BE

  // ---------------- Wi-Fi PHY/Channel ----------------
  YansWifiChannelHelper channel = YansWifiChannelHelper::Default();

  // ns-3.41: no YansWifiPhyHelper::Default()
  YansWifiPhyHelper phy;
  phy.SetChannel(channel.Create());
  // Configure operating channel safely in ns-3.41 using ChannelSettings.
  // Format: {channelNumber, channelWidthMHz, band, primary20Index}
  // Example: {36, 20, BAND_5GHZ, 0}
  // Pick band based on channel number (simple rule: 1-14 => 2.4GHz, otherwise 5GHz)
  std::string band = (channelNumber >= 1 && channelNumber <= 14) ? "BAND_2_4GHZ" : "BAND_5GHZ";

  std::ostringstream ch;
  ch << "{" << channelNumber << ", " << channelWidth << ", " << band << ", 0}";
  phy.Set("ChannelSettings", StringValue(ch.str()));

  // Typical PHY knobs
  phy.Set("TxPowerStart", DoubleValue(txPowerDbm));
  phy.Set("TxPowerEnd", DoubleValue(txPowerDbm));

  // ---------------- Wi-Fi helper ----------------
  WifiHelper wifi;
  wifi.SetStandard(ParseStandard(standardStr));

  // Fixed rate for stable comparisons
  wifi.SetRemoteStationManager("ns3::ConstantRateWifiManager",
                               "DataMode", StringValue(dataMode),
                               "ControlMode", StringValue(ctrlMode));

  // ns-3.41: use WifiMacHelper and toggle QosSupported attribute on MAC types
  WifiMacHelper mac;
  Ssid ssid("wifi6-ter");

  // STA devices
  mac.SetType("ns3::StaWifiMac",
              "Ssid", SsidValue(ssid),
              "ActiveProbing", BooleanValue(false),
              "QosSupported", BooleanValue(qosOn));
  NetDeviceContainer staDevs = wifi.Install(phy, mac, staNodes);

  // AP device
  mac.SetType("ns3::ApWifiMac",
              "Ssid", SsidValue(ssid),
              "QosSupported", BooleanValue(qosOn));
  NetDeviceContainer apDev = wifi.Install(phy, mac, apNode);

  // PCAP (optional)
  if (pcap)
  {
    phy.SetPcapDataLinkType(WifiPhyHelper::DLT_IEEE802_11_RADIO);
    phy.EnablePcap(outDir + "/pcap/p8_ap", apDev.Get(0), true);
    for (uint32_t i = 0; i < staDevs.GetN(); ++i)
    {
      phy.EnablePcap(outDir + "/pcap/p8_sta" + std::to_string(i), staDevs.Get(i), true);
    }
  }

  // ---------------- Mobility (fixed) ----------------
  MobilityHelper mobility;
  mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
  mobility.Install(apNode);
  mobility.Install(staNodes);

  apNode.Get(0)->GetObject<MobilityModel>()->SetPosition(Vector(0.0, 0.0, 0.0));
  staNodes.Get(0)->GetObject<MobilityModel>()->SetPosition(Vector(3.0, 0.0, 0.0));
  staNodes.Get(1)->GetObject<MobilityModel>()->SetPosition(Vector(4.0, 1.0, 0.0));
  staNodes.Get(2)->GetObject<MobilityModel>()->SetPosition(Vector(5.0, -1.0, 0.0));

  // ---------------- Internet + IP ----------------
  InternetStackHelper stack;
  stack.Install(apNode);
  stack.Install(staNodes);

  Ipv4AddressHelper ipv4;
  ipv4.SetBase("10.1.0.0", "255.255.255.0");
  Ipv4InterfaceContainer apIf = ipv4.Assign(apDev);
  Ipv4InterfaceContainer staIf = ipv4.Assign(staDevs);
  NS_LOG_UNCOND("IP map:"
                << " STA0=" << staIf.GetAddress(0)
                << " STA1=" << staIf.GetAddress(1)
                << " STA2=" << staIf.GetAddress(2)
                << " AP=" << apIf.GetAddress(0));

  Ipv4GlobalRoutingHelper::PopulateRoutingTables();
  Config::Connect("/NodeList/*/$ns3::Ipv4L3Protocol/Tx", MakeCallback(&Ipv4TxTrace));

  // ---------------- Sinks (AP) ----------------
  const uint16_t portVO = 5000;
  const uint16_t portVI = 5001;
  const uint16_t portBE = 5002;

  ApplicationContainer sinks;
  {
    // Always install VO and VI sinks.
    PacketSinkHelper sinkVO("ns3::UdpSocketFactory", InetSocketAddress(apIf.GetAddress(0), portVO));
    PacketSinkHelper sinkVI("ns3::UdpSocketFactory", InetSocketAddress(apIf.GetAddress(0), portVI));

    sinks.Add(sinkVO.Install(apNode.Get(0)));
    sinks.Add(sinkVI.Install(apNode.Get(0)));

    // Install BE sink only when BE is enabled.
    if (beRateMbps > 0)
    {
      PacketSinkHelper sinkBE("ns3::UdpSocketFactory", InetSocketAddress(apIf.GetAddress(0), portBE));
      sinks.Add(sinkBE.Install(apNode.Get(0)));
    }

    sinks.Start(Seconds(0.1));
    sinks.Stop(Seconds(duration + 1.0));
  }

  // ---------------- Sources (STAs) ----------------
  // DSCP/TOS values (only meaningful in QoS ON; OFF still uses DCF)
  const uint8_t tosVO = qosOn ? 0xB8 : 0x00; // EF=46
  const uint8_t tosVI = qosOn ? 0x88 : 0x00; // AF41=34
  const uint8_t tosBE = 0x00;                // Best effort

  // Build traffic intervals
  const Time voInterval = Seconds(1.0 / std::max(1e-9, voPps));

  const double viRateBps = viRateMbps * 1e6;
  const double viIntervalSec = (viPktSize * 8.0) / std::max(1.0, viRateBps);
  const Time viInterval = Seconds(viIntervalSec);

  Time beInterval = Seconds(1.0); // dummy default (won't be used if BE disabled)
  if (beRateMbps > 0)
  {
    const double beRateBps = beRateMbps * 1e6;
    const double beIntervalSec = (bePktSize * 8.0) / std::max(1.0, beRateBps);
    beInterval = Seconds(beIntervalSec);
  }

  // ---------------- Sources (STAs) ----------------
  ApplicationContainer sources;

  // STA0 -> AP : VO (VoIP)
  {
    Ptr<DscpUdpCbrApp> app = CreateObject<DscpUdpCbrApp>();
    Address peer = InetSocketAddress(apIf.GetAddress(0), portVO);
    app->Configure(peer, voPktSize, voInterval, tosVO, verboseApp);
    staNodes.Get(0)->AddApplication(app);
    app->SetStartTime(Seconds(appStart));
    app->SetStopTime(Seconds(duration));
    sources.Add(app);
  }

  // STA1 -> AP : VI (Video)
  {
    Ptr<DscpUdpCbrApp> app = CreateObject<DscpUdpCbrApp>();
    Address peer = InetSocketAddress(apIf.GetAddress(0), portVI);
    app->Configure(peer, viPktSize, viInterval, tosVI, verboseApp);
    staNodes.Get(1)->AddApplication(app);
    app->SetStartTime(Seconds(appStart));
    app->SetStopTime(Seconds(duration));
    sources.Add(app);
  }

  // STA2 -> AP : BE (background congestion) — ONLY if enabled
  if (beRateMbps > 0)
  {
    Ptr<DscpUdpCbrApp> app = CreateObject<DscpUdpCbrApp>();
    Address peer = InetSocketAddress(apIf.GetAddress(0), portBE);
    app->Configure(peer, bePktSize, beInterval, tosBE, verboseApp);
    staNodes.Get(2)->AddApplication(app);
    app->SetStartTime(Seconds(appStart));
    app->SetStopTime(Seconds(duration));
    sources.Add(app);
  }
  else
  {
    if (verboseApp)
    {
      NS_LOG_UNCOND("BE disabled (beRateMbps=0): baseline will run VO+VI only.");
    }
  }

  // ---------------- FlowMonitor ----------------
  FlowMonitorHelper fmHelper;
  Ptr<FlowMonitor> monitor;
  Ptr<Ipv4FlowClassifier> classifier;

  if (flowmon)
  {
    monitor = fmHelper.InstallAll();
    classifier = DynamicCast<Ipv4FlowClassifier>(fmHelper.GetClassifier());
  }

  // ---------------- Run ----------------
  Simulator::Stop(Seconds(duration + 0.5));
  Simulator::Run();

  // ---------------- Extract metrics ----------------
  double goodputBE = 0.0, goodputVO = 0.0, goodputVI = 0.0;
  double delayVOms = 0.0, jitterVOms = 0.0, lossVO = 0.0;
  double delayVIms = 0.0, jitterVIms = 0.0, lossVI = 0.0;

  if (flowmon && monitor && classifier)
  {
    monitor->CheckForLostPackets();

    auto stats = monitor->GetFlowStats();
    for (const auto &kv : stats)
    {
      const FlowId flowId = kv.first;
      const FlowMonitor::FlowStats &st = kv.second;

      Ipv4FlowClassifier::FiveTuple t = classifier->FindFlow(flowId);
      const uint16_t dport = t.destinationPort;
      const Ipv4Address src = t.sourceAddress;

      // Goodput over active interval
      const double active = (st.timeLastRxPacket - st.timeFirstTxPacket).GetSeconds();
      const double gpMbps = (active > 0.0) ? (st.rxBytes * 8.0 / active / 1e6) : 0.0;

      // Mean delay/jitter per received packet
      const double meanDelayMs = (st.rxPackets > 0) ? (st.delaySum.GetSeconds() * 1000.0 / st.rxPackets) : 0.0;
      const double meanJitterMs = (st.rxPackets > 0) ? (st.jitterSum.GetSeconds() * 1000.0 / st.rxPackets) : 0.0;

      // Loss ratio
      const double lossRatio = (st.txPackets > 0) ? (1.0 - (double)st.rxPackets / (double)st.txPackets) : 0.0;

      if (dport == portVO && src == staIf.GetAddress(0))
      {
        goodputVO = gpMbps;
        delayVOms = meanDelayMs;
        jitterVOms = meanJitterMs;
        lossVO = lossRatio;
      }
      else if (dport == portVI && src == staIf.GetAddress(1))
      {
        goodputVI = gpMbps;
        delayVIms = meanDelayMs;
        jitterVIms = meanJitterMs;
        lossVI = lossRatio;
      }
      else if (beRateMbps > 0 && dport == portBE && src == staIf.GetAddress(2))
      {
        goodputBE = gpMbps;
      }
    }

    // Debug XML (overwritten each run; logs/ keep per-run console output)
    monitor->SerializeToXmlFile(outDir + "/raw/p8_flowmon.xml", true, true);
  }

  // ---------------- Append CSV summary line ----------------
  AppendSummaryLine(outDir, mode, beRateMbps, seed, run,
                    goodputBE, goodputVO, goodputVI,
                    delayVOms, jitterVOms, lossVO,
                    delayVIms, jitterVIms, lossVI);

  // Short console summary (your bash runner saves it into logs/)
  NS_LOG_UNCOND("P8 Summary: mode=" << mode
                                    << " beRate=" << beRateMbps << "Mbps"
                                    << " seed=" << seed << " run=" << run);
  NS_LOG_UNCOND("  Goodput(Mbps): BE=" << goodputBE
                                       << " VO=" << goodputVO
                                       << " VI=" << goodputVI);
  NS_LOG_UNCOND("  VO: delay(ms)=" << delayVOms
                                   << " jitter(ms)=" << jitterVOms
                                   << " loss=" << lossVO);
  NS_LOG_UNCOND("  VI: delay(ms)=" << delayVIms
                                   << " jitter(ms)=" << jitterVIms
                                   << " loss=" << lossVI);

  Simulator::Destroy();
  return 0;
}
