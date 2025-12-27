#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/wifi-module.h"
#include "ns3/yans-wifi-helper.h"
#include "ns3/mobility-module.h"
#include "ns3/csma-module.h"
#include "ns3/applications-module.h"

#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <algorithm>
#include <cstdint>

using namespace ns3;

// -------------------- utilities --------------------
static void EnsureDir(const std::string &dir)
{
  SystemPath::MakeDirectories(dir);
}

static bool IsFileEmptyOrMissing(const std::string &path)
{
  std::ifstream f(path.c_str(), std::ios::in | std::ios::binary);
  if (!f.good())
    return true;
  f.seekg(0, std::ios::end);
  return (f.tellg() <= 0);
}

static void EnsureCsvHeader(const std::string &path, const std::string &headerLine)
{
  if (IsFileEmptyOrMissing(path))
  {
    std::ofstream out(path.c_str(), std::ios::out);
    out << headerLine << "\n";
  }
}

static std::string ToLower(std::string s)
{
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

// -------------------- Wi-Fi standard parser --------------------
static WifiStandard ParseStandard(const std::string &s0)
{
  std::string s = ToLower(s0);
  if (s == "ax")
    return WIFI_STANDARD_80211ax;
  if (s == "ac")
    return WIFI_STANDARD_80211ac;
  if (s == "n")
    return WIFI_STANDARD_80211n;
  return WIFI_STANDARD_80211ax;
}

// -------------------- PHY RSSI/SNR accumulation --------------------
static double g_measureStartS = 0.0;
static double g_rssiSumDbm = 0.0;
static double g_snrSumDb = 0.0;
static uint64_t g_rssiCount = 0;

static void MonitorSnifferRxTrace(Ptr<const Packet> /*packet*/,
                                  uint16_t /*channelFreqMhz*/,
                                  WifiTxVector /*txVector*/,
                                  MpduInfo /*aMpdu*/,
                                  SignalNoiseDbm signalNoise,
                                  uint16_t /*staId*/)
{
  if (Simulator::Now().GetSeconds() < g_measureStartS)
    return;

  g_rssiSumDbm += signalNoise.signal;
  g_snrSumDb += (signalNoise.signal - signalNoise.noise);
  g_rssiCount++;
}

// -------------------- custom header: seq + tx timestamp --------------------
class RttHeader : public Header
{
public:
  RttHeader() = default;

  static TypeId GetTypeId()
  {
    static TypeId tid = TypeId("RttHeader")
                            .SetParent<Header>()
                            .AddConstructor<RttHeader>();
    return tid;
  }

  TypeId GetInstanceTypeId() const override { return GetTypeId(); }

  void SetSeq(uint32_t s) { m_seq = s; }
  void SetTsNs(uint64_t t) { m_tsNs = t; }

  uint32_t GetSeq() const { return m_seq; }
  uint64_t GetTsNs() const { return m_tsNs; }

  uint32_t GetSerializedSize() const override { return 12; }

  void Serialize(Buffer::Iterator start) const override
  {
    start.WriteHtonU32(m_seq);
    start.WriteHtonU64(m_tsNs);
  }

  uint32_t Deserialize(Buffer::Iterator start) override
  {
    m_seq = start.ReadNtohU32();
    m_tsNs = start.ReadNtohU64();
    return 12;
  }

  void Print(std::ostream &os) const override
  {
    os << "seq=" << m_seq << " tsNs=" << m_tsNs;
  }

private:
  uint32_t m_seq{0};
  uint64_t m_tsNs{0};
};

// -------------------- UDP RTT server/client (echo-based) --------------------
static double g_rttSumMs = 0.0;
static uint64_t g_rttCount = 0;

class UdpRttServerApp : public Application
{
public:
  void Setup(uint16_t port) { m_port = port; }

private:
  void StartApplication() override
  {
    m_socket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
    m_socket->Bind(InetSocketAddress(Ipv4Address::GetAny(), m_port));
    m_socket->SetRecvCallback(MakeCallback(&UdpRttServerApp::HandleRead, this));
  }

  void StopApplication() override
  {
    if (m_socket)
    {
      m_socket->Close();
      m_socket = nullptr;
    }
  }

  void HandleRead(Ptr<Socket> socket)
  {
    Address from;
    while (Ptr<Packet> packet = socket->RecvFrom(from))
    {
      if (packet->GetSize() == 0)
        continue;
      socket->SendTo(packet, 0, from);
    }
  }

  Ptr<Socket> m_socket;
  uint16_t m_port{0};
};

class UdpRttClientApp : public Application
{
public:
  void Setup(Ipv4Address peerIp, uint16_t peerPort, Time interval, uint32_t payloadBytes, double measureStartS)
  {
    m_peerIp = peerIp;
    m_peerPort = peerPort;
    m_interval = interval;
    m_payloadBytes = payloadBytes;
    m_measureStartS = measureStartS;
  }

private:
  void StartApplication() override
  {
    m_socket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
    m_socket->Bind();
    m_socket->Connect(InetSocketAddress(m_peerIp, m_peerPort));
    m_socket->SetRecvCallback(MakeCallback(&UdpRttClientApp::HandleRead, this));

    m_running = true;
    m_seq = 0;
    SendOne();
  }

  void StopApplication() override
  {
    m_running = false;
    if (m_sendEvent.IsPending())
      Simulator::Cancel(m_sendEvent);

    if (m_socket)
    {
      m_socket->Close();
      m_socket = nullptr;
    }
  }

  void ScheduleNext()
  {
    if (!m_running)
      return;
    m_sendEvent = Simulator::Schedule(m_interval, &UdpRttClientApp::SendOne, this);
  }

  void SendOne()
  {
    if (!m_running)
      return;

    RttHeader hdr;
    hdr.SetSeq(m_seq++);
    hdr.SetTsNs(Simulator::Now().GetNanoSeconds());

    Ptr<Packet> p = Create<Packet>(m_payloadBytes);
    p->AddHeader(hdr);

    m_socket->Send(p);
    ScheduleNext();
  }

  void HandleRead(Ptr<Socket> socket)
  {
    Address from;
    while (Ptr<Packet> p = socket->RecvFrom(from))
    {
      if (!p || p->GetSize() == 0)
        continue;

      RttHeader hdr;
      p->RemoveHeader(hdr);

      Time sent = NanoSeconds(static_cast<int64_t>(hdr.GetTsNs()));
      Time rtt = Simulator::Now() - sent;

      if (Simulator::Now().GetSeconds() >= m_measureStartS)
      {
        g_rttSumMs += rtt.GetMilliSeconds();
        g_rttCount++;
      }
    }
  }

  Ptr<Socket> m_socket;
  EventId m_sendEvent;
  bool m_running{false};

  Ipv4Address m_peerIp;
  uint16_t m_peerPort{0};
  Time m_interval{MilliSeconds(200)};
  uint32_t m_payloadBytes{16};
  uint32_t m_seq{0};
  double m_measureStartS{0.0};
};

// -------------------- UDP data TX/RX (goodput/delay/loss without FlowMonitor) --------------------
class UdpDataTxApp : public Application
{
public:
  void Setup(Ipv4Address peerIp,
             uint16_t peerPort,
             const DataRate &rate,
             uint32_t packetSizeBytes,
             double measureStartS)
  {
    m_peerIp = peerIp;
    m_peerPort = peerPort;
    m_rate = rate;
    m_packetSizeBytes = packetSizeBytes;
    m_measureStartS = measureStartS;
  }

  uint64_t GetSentPackets() const { return m_sentPackets; }
  uint64_t GetSentBytes() const { return m_sentBytes; }

private:
  void StartApplication() override
  {
    m_socket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
    m_socket->Bind();
    m_socket->Connect(InetSocketAddress(m_peerIp, m_peerPort));

    m_running = true;
    m_seq = 0;
    ScheduleNext();
  }

  void StopApplication() override
  {
    m_running = false;
    if (m_sendEvent.IsPending())
      Simulator::Cancel(m_sendEvent);

    if (m_socket)
    {
      m_socket->Close();
      m_socket = nullptr;
    }
  }

  void ScheduleNext()
  {
    if (!m_running)
      return;

    const uint64_t br = m_rate.GetBitRate();
    if (br == 0)
      return;

    const double intervalS = (static_cast<double>(m_packetSizeBytes) * 8.0) / static_cast<double>(br);
    m_sendEvent = Simulator::Schedule(Seconds(intervalS), &UdpDataTxApp::SendOne, this);
  }

  void SendOne()
  {
    if (!m_running)
      return;

    const uint32_t hdrSize = 12;
    const uint32_t payload = (m_packetSizeBytes > hdrSize) ? (m_packetSizeBytes - hdrSize) : 0;

    RttHeader hdr;
    hdr.SetSeq(m_seq++);
    hdr.SetTsNs(Simulator::Now().GetNanoSeconds());

    Ptr<Packet> p = Create<Packet>(payload);
    p->AddHeader(hdr);

    int n = m_socket->Send(p);
    if (n > 0)
    {
      if (Simulator::Now().GetSeconds() >= m_measureStartS)
      {
        m_sentPackets++;
        m_sentBytes += static_cast<uint64_t>(n);
      }
    }

    ScheduleNext();
  }

  Ptr<Socket> m_socket;
  EventId m_sendEvent;
  bool m_running{false};

  Ipv4Address m_peerIp;
  uint16_t m_peerPort{0};
  DataRate m_rate{DataRate("1Mbps")};
  uint32_t m_packetSizeBytes{1200};
  double m_measureStartS{0.0};

  uint32_t m_seq{0};
  uint64_t m_sentPackets{0};
  uint64_t m_sentBytes{0};
};

class UdpDataRxApp : public Application
{
public:
  void Setup(uint16_t listenPort, double measureStartS)
  {
    m_listenPort = listenPort;
    m_measureStartS = measureStartS;
  }

  uint64_t GetRxPackets() const { return m_rxPackets; }
  uint64_t GetRxBytes() const { return m_rxBytes; }
  uint64_t GetLostPacketsEstimate() const { return m_lostPackets; }
  double GetAvgDelayMs() const
  {
    if (m_rxPackets == 0)
      return -1.0;
    return m_delaySumMs / static_cast<double>(m_rxPackets);
  }

private:
  void StartApplication() override
  {
    m_socket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
    m_socket->Bind(InetSocketAddress(Ipv4Address::GetAny(), m_listenPort));
    m_socket->SetRecvCallback(MakeCallback(&UdpDataRxApp::HandleRead, this));
  }

  void StopApplication() override
  {
    if (m_socket)
    {
      m_socket->Close();
      m_socket = nullptr;
    }
  }

  void HandleRead(Ptr<Socket> socket)
  {
    Address from;
    while (Ptr<Packet> p = socket->RecvFrom(from))
    {
      if (!p || p->GetSize() == 0)
        continue;

      if (Simulator::Now().GetSeconds() < m_measureStartS)
        continue;

      RttHeader hdr;
      p->RemoveHeader(hdr);

      const uint32_t seq = hdr.GetSeq();

      if (!m_seenAny)
      {
        m_seenAny = true;
        m_lastSeq = seq;
      }
      else
      {
        if (seq > m_lastSeq + 1)
        {
          m_lostPackets += static_cast<uint64_t>(seq - (m_lastSeq + 1));
          m_lastSeq = seq;
        }
        else if (seq > m_lastSeq)
        {
          m_lastSeq = seq;
        }
      }

      Time sent = NanoSeconds(static_cast<int64_t>(hdr.GetTsNs()));
      Time delay = Simulator::Now() - sent;

      m_rxPackets++;
      m_rxBytes += p->GetSize() + 12;
      m_delaySumMs += delay.GetMilliSeconds();
    }
  }

  Ptr<Socket> m_socket;
  uint16_t m_listenPort{0};
  double m_measureStartS{0.0};

  bool m_seenAny{false};
  uint32_t m_lastSeq{0};

  uint64_t m_rxPackets{0};
  uint64_t m_rxBytes{0};
  uint64_t m_lostPackets{0};
  double m_delaySumMs{0.0};
};

// -------------------- optional attribute setter --------------------
static void SetPhyAttributeIfExists(Ptr<WifiPhy> phy, const std::string &name, const AttributeValue &v)
{
  if (!phy)
    return;
  bool ok = phy->SetAttributeFailSafe(name, v);
  (void)ok;
}

static uint16_t Wifi5GhzChannelToFreqMhz(int ch)
{
  if (ch <= 0)
    return 5180;
  return static_cast<uint16_t>(5000 + 5 * ch);
}

static void ConfigureOperatingChannel(Ptr<WifiNetDevice> dev, int channelNumber, uint16_t widthMhz)
{
  if (!dev)
    return;

  Ptr<WifiPhy> phy = dev->GetPhy();
  if (!phy)
    return;

  const uint16_t freq = Wifi5GhzChannelToFreqMhz(channelNumber);

  SetPhyAttributeIfExists(phy, "ChannelNumber", UintegerValue(static_cast<uint32_t>(channelNumber)));
  SetPhyAttributeIfExists(phy, "ChannelWidth", UintegerValue(static_cast<uint32_t>(widthMhz)));
  SetPhyAttributeIfExists(phy, "Frequency", UintegerValue(static_cast<uint32_t>(freq)));

  std::ostringstream cs;
  cs << "{"
     << static_cast<uint32_t>(channelNumber) << ","
     << static_cast<uint32_t>(freq) << ","
     << static_cast<uint32_t>(widthMhz)
     << "}";
  SetPhyAttributeIfExists(phy, "ChannelSettings", StringValue(cs.str()));
}

int main(int argc, char *argv[])
{
  std::string outDir = "results/p9_heatmap";
  uint32_t seed = 1;
  uint32_t run = 0;

  double simTime = 8.0;
  double appStart = 2.0;

  double xMin = 0.0, xMax = 20.0;
  double yMin = 0.0, yMax = 20.0;

  double apX = 10.0, apY = 10.0;
  double x = 0.0, y = 0.0;

  std::string wifiStandardStr = "ax";
  std::string ssidStr = "heatmap";

  bool pcap = false;

  uint16_t channelWidthMhz = 20;
  int chan = 36;

  double txPowerDbm = 16.0;
  double noiseFigureDb = 7.0;

  double logExp = 3.0;
  double shadowingSigmaDb = 4.0;
  bool enableFading = false;

  uint32_t pktSize = 1200;
  std::string udpRate = "20Mbps";
  uint16_t udpPort = 9000;

  bool enableRtt = true;
  uint16_t rttPort = 9100;
  uint32_t rttIntervalMs = 200;
  uint32_t rttPayloadBytes = 16;

  CommandLine cmd;
  cmd.AddValue("outDir", "Output directory", outDir);
  cmd.AddValue("seed", "RNG seed", seed);
  cmd.AddValue("run", "RNG run id", run);

  cmd.AddValue("simTime", "Total simulation time (s)", simTime);
  cmd.AddValue("appStart", "Traffic start time (s)", appStart);

  cmd.AddValue("xMin", "Area min X (m)", xMin);
  cmd.AddValue("xMax", "Area max X (m)", xMax);
  cmd.AddValue("yMin", "Area min Y (m)", yMin);
  cmd.AddValue("yMax", "Area max Y (m)", yMax);

  cmd.AddValue("apX", "AP X position (m)", apX);
  cmd.AddValue("apY", "AP Y position (m)", apY);

  cmd.AddValue("x", "Probe STA X position (m)", x);
  cmd.AddValue("y", "Probe STA Y position (m)", y);

  cmd.AddValue("wifiStandard", "Wi-Fi standard: ax|ac|n", wifiStandardStr);
  cmd.AddValue("ssid", "SSID", ssidStr);

  cmd.AddValue("pcap", "Enable PCAP", pcap);

  cmd.AddValue("channelWidth", "Channel width (MHz)", channelWidthMhz);
  cmd.AddValue("chan", "5GHz channel number label (e.g., 36)", chan);

  cmd.AddValue("txPowerDbm", "Tx power (dBm)", txPowerDbm);
  cmd.AddValue("noiseFigureDb", "Rx noise figure (dB)", noiseFigureDb);

  cmd.AddValue("logExp", "LogDistance exponent", logExp);
  cmd.AddValue("shadowingSigmaDb", "Shadowing sigma (dB)", shadowingSigmaDb);
  cmd.AddValue("enableFading", "Enable Nakagami fading", enableFading);

  cmd.AddValue("pktSize", "UDP packet size (bytes)", pktSize);
  cmd.AddValue("udpRate", "UDP offered load (server->STA)", udpRate);
  cmd.AddValue("udpPort", "UDP port", udpPort);

  cmd.AddValue("enableRtt", "Enable UDP RTT measurement", enableRtt);
  cmd.AddValue("rttPort", "UDP RTT port", rttPort);
  cmd.AddValue("rttIntervalMs", "RTT probe interval (ms)", rttIntervalMs);
  cmd.AddValue("rttPayloadBytes", "RTT probe payload bytes (without header)", rttPayloadBytes);

  cmd.Parse(argc, argv);

  if (simTime <= appStart)
  {
    NS_LOG_UNCOND("ERROR: simTime must be > appStart");
    return 1;
  }

  RngSeedManager::SetSeed(seed);
  RngSeedManager::SetRun(run);

  EnsureDir(outDir);
  EnsureDir(outDir + "/raw");
  EnsureDir(outDir + "/logs");
  EnsureDir(outDir + "/pcap");
  EnsureDir(outDir + "/plots");
  EnsureDir(outDir + "/report");

  const std::string gridPath = outDir + "/raw/grid.csv";
  const std::string gridHeader = "x,y,seed,run,rssi_dbm,snr_db,goodput_mbps,rtt_ms,delay_ms,loss";
  EnsureCsvHeader(gridPath, gridHeader);

  NodeContainer apNode;
  apNode.Create(1);

  NodeContainer staNode;
  staNode.Create(1);

  NodeContainer serverNode;
  serverNode.Create(1);

  Ptr<Node> ap = apNode.Get(0);
  Ptr<Node> sta = staNode.Get(0);
  Ptr<Node> server = serverNode.Get(0);

  MobilityHelper mobility;
  mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
  mobility.Install(apNode);
  mobility.Install(staNode);
  mobility.Install(serverNode);

  ap->GetObject<MobilityModel>()->SetPosition(Vector(apX, apY, 0.0));
  sta->GetObject<MobilityModel>()->SetPosition(Vector(x, y, 0.0));
  server->GetObject<MobilityModel>()->SetPosition(Vector(apX, apY, 0.0));

  Ptr<LogDistancePropagationLossModel> logd = CreateObject<LogDistancePropagationLossModel>();
  logd->SetAttribute("Exponent", DoubleValue(logExp));
  logd->SetAttribute("ReferenceDistance", DoubleValue(1.0));
  logd->SetAttribute("ReferenceLoss", DoubleValue(46.6777));

  Ptr<NormalRandomVariable> normal = CreateObject<NormalRandomVariable>();
  normal->SetAttribute("Mean", DoubleValue(0.0));
  normal->SetAttribute("Variance", DoubleValue(shadowingSigmaDb * shadowingSigmaDb));

  Ptr<RandomPropagationLossModel> shadow = CreateObject<RandomPropagationLossModel>();
  shadow->SetAttribute("Variable", PointerValue(normal));
  logd->SetNext(shadow);

  if (enableFading)
  {
    Ptr<NakagamiPropagationLossModel> nak = CreateObject<NakagamiPropagationLossModel>();
    nak->SetAttribute("Distance1", DoubleValue(5.0));
    nak->SetAttribute("Distance2", DoubleValue(15.0));
    nak->SetAttribute("m0", DoubleValue(1.5));
    nak->SetAttribute("m1", DoubleValue(1.0));
    nak->SetAttribute("m2", DoubleValue(0.75));
    shadow->SetNext(nak);
  }

  Ptr<YansWifiChannel> wifiChannel = CreateObject<YansWifiChannel>();
  wifiChannel->SetPropagationDelayModel(CreateObject<ConstantSpeedPropagationDelayModel>());
  wifiChannel->SetPropagationLossModel(logd);

  WifiHelper wifi;
  wifi.SetStandard(ParseStandard(wifiStandardStr));
  wifi.SetRemoteStationManager("ns3::MinstrelHtWifiManager");

  YansWifiPhyHelper phy;
  phy.SetChannel(wifiChannel);
  phy.Set("TxPowerStart", DoubleValue(txPowerDbm));
  phy.Set("TxPowerEnd", DoubleValue(txPowerDbm));
  phy.Set("TxPowerLevels", UintegerValue(1));
  phy.Set("RxNoiseFigure", DoubleValue(noiseFigureDb));

  WifiMacHelper mac;
  Ssid ssid(ssidStr);

  mac.SetType("ns3::ApWifiMac",
              "Ssid", SsidValue(ssid),
              "BeaconInterval", TimeValue(MicroSeconds(1024 * 100)));
  NetDeviceContainer apDev = wifi.Install(phy, mac, ap);

  mac.SetType("ns3::StaWifiMac",
              "Ssid", SsidValue(ssid),
              "ActiveProbing", BooleanValue(true));
  NetDeviceContainer staDev = wifi.Install(phy, mac, staNode);

  ConfigureOperatingChannel(DynamicCast<WifiNetDevice>(apDev.Get(0)), chan, channelWidthMhz);
  ConfigureOperatingChannel(DynamicCast<WifiNetDevice>(staDev.Get(0)), chan, channelWidthMhz);

  CsmaHelper csma;
  csma.SetChannelAttribute("DataRate", StringValue("1Gbps"));
  csma.SetChannelAttribute("Delay", TimeValue(MicroSeconds(50)));

  NodeContainer csmaNodes;
  csmaNodes.Add(ap);
  csmaNodes.Add(server);

  NetDeviceContainer csmaDevs = csma.Install(csmaNodes);

  InternetStackHelper internet;
  internet.Install(apNode);
  internet.Install(staNode);
  internet.Install(serverNode);

  Ipv4AddressHelper ipv4;

  ipv4.SetBase("10.1.0.0", "255.255.255.0");
  Ipv4InterfaceContainer apWifiIf = ipv4.Assign(apDev);
  Ipv4InterfaceContainer staWifiIf = ipv4.Assign(staDev);

  ipv4.SetBase("10.2.0.0", "255.255.255.0");
  Ipv4InterfaceContainer csmaIf = ipv4.Assign(csmaDevs);

  Ipv4Address staIp = staWifiIf.GetAddress(0);
  Ipv4Address serverIp = csmaIf.GetAddress(1);
  Ipv4Address apWifiIp = apWifiIf.GetAddress(0);

  ap->GetObject<Ipv4>()->SetAttribute("IpForward", BooleanValue(true));
  Ipv4GlobalRoutingHelper::PopulateRoutingTables();

  Ipv4StaticRoutingHelper staticRouting;
  Ptr<Ipv4> staIpv4 = sta->GetObject<Ipv4>();
  uint32_t staIfIndex = staIpv4->GetInterfaceForDevice(staDev.Get(0));
  Ptr<Ipv4StaticRouting> staSr = staticRouting.GetStaticRouting(staIpv4);
  staSr->SetDefaultRoute(apWifiIp, staIfIndex);

  g_measureStartS = appStart;

  Ptr<WifiNetDevice> staWifi = DynamicCast<WifiNetDevice>(staDev.Get(0));
  if (staWifi && staWifi->GetPhy())
  {
    staWifi->GetPhy()->TraceConnectWithoutContext("MonitorSnifferRx",
                                                  MakeCallback(&MonitorSnifferRxTrace));
  }

  Ptr<UdpDataRxApp> dataRx = CreateObject<UdpDataRxApp>();
  dataRx->Setup(udpPort, appStart);
  sta->AddApplication(dataRx);
  dataRx->SetStartTime(Seconds(appStart));
  dataRx->SetStopTime(Seconds(simTime));

  Ptr<UdpDataTxApp> dataTx = CreateObject<UdpDataTxApp>();
  dataTx->Setup(staIp, udpPort, DataRate(udpRate), pktSize, appStart);
  server->AddApplication(dataTx);
  dataTx->SetStartTime(Seconds(appStart));
  dataTx->SetStopTime(Seconds(simTime));

  if (enableRtt)
  {
    Ptr<UdpRttServerApp> rttServer = CreateObject<UdpRttServerApp>();
    rttServer->Setup(rttPort);
    server->AddApplication(rttServer);
    rttServer->SetStartTime(Seconds(appStart));
    rttServer->SetStopTime(Seconds(simTime));

    Ptr<UdpRttClientApp> rttClient = CreateObject<UdpRttClientApp>();
    rttClient->Setup(serverIp, rttPort, MilliSeconds(rttIntervalMs), rttPayloadBytes, appStart);
    sta->AddApplication(rttClient);
    rttClient->SetStartTime(Seconds(appStart));
    rttClient->SetStopTime(Seconds(simTime));
  }

  if (pcap)
  {
    phy.SetPcapDataLinkType(WifiPhyHelper::DLT_IEEE802_11_RADIO);
    phy.EnablePcap(outDir + "/pcap/p9_ap", apDev.Get(0), true);
    phy.EnablePcap(outDir + "/pcap/p9_sta", staDev.Get(0), true);
    csma.EnablePcap(outDir + "/pcap/p9_csma", csmaDevs, true);
  }

  Simulator::Stop(Seconds(simTime));
  Simulator::Run();

  double rssiAvgDbm = -1.0;
  double snrAvgDb = -1.0;
  if (g_rssiCount > 0)
  {
    rssiAvgDbm = g_rssiSumDbm / static_cast<double>(g_rssiCount);
    snrAvgDb = g_snrSumDb / static_cast<double>(g_rssiCount);
  }

  double rttAvgMs = -1.0;
  if (enableRtt && g_rttCount > 0)
  {
    rttAvgMs = g_rttSumMs / static_cast<double>(g_rttCount);
  }

  const double dur = simTime - appStart;
  const uint64_t txPkts = dataTx->GetSentPackets();
  const uint64_t rxPkts = dataRx->GetRxPackets();
  const uint64_t rxBytes = dataRx->GetRxBytes();
  const uint64_t lostEst = dataRx->GetLostPacketsEstimate();

  double goodputMbps = -1.0;
  if (dur > 0.0)
    goodputMbps = (static_cast<double>(rxBytes) * 8.0) / (dur * 1e6);

  double delayMs = dataRx->GetAvgDelayMs();

  double loss = -1.0;
  if (txPkts > 0)
  {
    const uint64_t missing = (txPkts >= rxPkts) ? (txPkts - rxPkts) : 0;
    (void)lostEst;
    loss = static_cast<double>(missing) / static_cast<double>(txPkts);
  }

  {
    std::ofstream f(gridPath, std::ios::app);
    if (f.is_open())
    {
      f << std::fixed << std::setprecision(6)
        << x << ","
        << y << ","
        << seed << ","
        << run << ","
        << rssiAvgDbm << ","
        << snrAvgDb << ","
        << goodputMbps << ","
        << rttAvgMs << ","
        << delayMs << ","
        << loss
        << "\n";
    }
  }

  Simulator::Destroy();
  return 0;
}
