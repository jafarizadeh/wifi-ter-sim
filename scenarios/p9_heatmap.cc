/*
 * Projet 9 (ns-3.41) - Heatmap generator (single point per run)
 *
 * Topology: 1 STA (probe) <Wi-Fi> 1 AP <CSMA> 1 Server
 * One run per (x,y) -> append one CSV line: <outDir>/heatmaps/heatmap.csv
 *
 * Metrics in window [appStart, appStart+measureTime]:
 *  - offered_mbps (app Tx bytes in window)
 *  - goodput_mbps (sink Rx bytes in window)
 *  - RTT (custom UDP timestamp echo)
 *  - loss_ratio (1 - rxBytes/txBytes in the window)
 *  - estimated RSSI/SNR (simple model)
 *
 * NOTE (your ns-3.41 build):
 *  - Do NOT set WifiPhy ChannelWidth via attributes (it triggers NS_FATAL).
 *  - channelWidth is used only for the SNR estimate + CSV column.
 */

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/wifi-module.h"
#include "ns3/wifi-net-device.h"
#include "ns3/yans-wifi-helper.h"
#include "ns3/csma-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"

#include <fstream>
#include <iomanip>
#include <string>
#include <algorithm>
#include <cmath>

using namespace ns3;

// -------------------- utilities --------------------
static std::string
ToLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c)
                   { return std::tolower(c); });
    return s;
}

static void
EnsureDir(const std::string &dir)
{
    SystemPath::MakeDirectories(dir);
}

static bool
IsFileEmptyOrMissing(const std::string &path)
{
    std::ifstream f(path.c_str(), std::ios::in | std::ios::binary);
    if (!f.good())
    {
        return true;
    }
    f.seekg(0, std::ios::end);
    return (f.tellg() <= 0);
}

static void
EnsureCsvHeader(const std::string &path, const std::string &headerLine)
{
    if (IsFileEmptyOrMissing(path))
    {
        std::ofstream out(path.c_str(), std::ios::out);
        out << headerLine << "\n";
    }
}

static WifiStandard
ParseStandard(const std::string &sIn)
{
    const std::string s = ToLower(sIn);
    if (s == "ax")
    {
        return WIFI_STANDARD_80211ax;
    }
    if (s == "ac")
    {
        return WIFI_STANDARD_80211ac;
    }
    if (s == "n")
    {
        return WIFI_STANDARD_80211n;
    }
    return WIFI_STANDARD_80211ax;
}

static double
Distance2d(double x1, double y1, double x2, double y2)
{
    const double dx = x1 - x2;
    const double dy = y1 - y2;
    return std::sqrt(dx * dx + dy * dy);
}

static double
EstimateRxPowerDbm(const std::string &propModel,
                   double txPowerDbm,
                   double dMeters,
                   double refDistance,
                   double refLossDb,
                   double exponent,
                   double freqMHz)
{
    const double d = std::max(dMeters, 0.001);
    const std::string pm = ToLower(propModel);

    if (pm == "friis" || pm == "freespace")
    {
        const double c = 299792458.0;
        const double freqHz = freqMHz * 1e6;
        const double lambda = c / freqHz;
        const double gain = 20.0 * std::log10(lambda / (4.0 * M_PI * d));
        return txPowerDbm + gain;
    }

    const double ratio = d / std::max(refDistance, 0.001);
    const double loss = refLossDb + 10.0 * exponent * std::log10(ratio);
    return txPowerDbm - loss;
}

static double
ThermalNoiseDbm(double bwHz, double noiseFigureDb)
{
    return -174.0 + 10.0 * std::log10(std::max(bwHz, 1.0)) + noiseFigureDb;
}

// -------------------- measurement state --------------------
struct MeasureState
{
    Time tStart;
    Time tEnd;

    uint64_t sinkRxStart{0};
    uint64_t sinkRxEnd{0};

    uint64_t txBytesWindow{0};

    uint32_t rttReplies{0};
    double rttSumMs{0.0};

    void Reset()
    {
        sinkRxStart = sinkRxEnd = 0;
        txBytesWindow = 0;
        rttReplies = 0;
        rttSumMs = 0.0;
    }
};

static MeasureState gMs;

static void
OnAppTx(Ptr<const Packet> p)
{
    const Time now = Simulator::Now();
    if (now >= gMs.tStart && now <= gMs.tEnd)
    {
        gMs.txBytesWindow += p->GetSize();
    }
}

static void
MarkSinkRxStart(Ptr<PacketSink> sink)
{
    gMs.sinkRxStart = sink->GetTotalRx();
}

static void
MarkSinkRxEnd(Ptr<PacketSink> sink)
{
    gMs.sinkRxEnd = sink->GetTotalRx();
}

// -------------------- RTT via custom UDP timestamp echo --------------------
class TxTimeHeader : public Header
{
public:
    TxTimeHeader() : m_txTimeNs(0) {}
    explicit TxTimeHeader(uint64_t ns) : m_txTimeNs(ns) {}

    static TypeId GetTypeId()
    {
        static TypeId tid = TypeId("ns3::TxTimeHeaderP9")
                                .SetParent<Header>()
                                .AddConstructor<TxTimeHeader>();
        return tid;
    }

    TypeId GetInstanceTypeId() const override { return GetTypeId(); }

    uint32_t GetSerializedSize() const override { return 8; }

    void Serialize(Buffer::Iterator start) const override
    {
        start.WriteHtonU64(m_txTimeNs);
    }

    uint32_t Deserialize(Buffer::Iterator start) override
    {
        m_txTimeNs = start.ReadNtohU64();
        return 8;
    }

    void Print(std::ostream &os) const override
    {
        os << "txTimeNs=" << m_txTimeNs;
    }

    uint64_t GetTxTimeNs() const { return m_txTimeNs; }

private:
    uint64_t m_txTimeNs;
};

class UdpEchoRttServer : public Application
{
public:
    void Setup(uint16_t port) { m_port = port; }

private:
    void StartApplication() override
    {
        m_socket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
        InetSocketAddress local(Ipv4Address::GetAny(), m_port);
        m_socket->Bind(local);
        m_socket->SetRecvCallback(MakeCallback(&UdpEchoRttServer::HandleRead, this));
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
        Ptr<Packet> packet = socket->RecvFrom(from);
        while (packet && packet->GetSize() > 0)
        {
            socket->SendTo(packet, 0, from);
            if (socket->GetRxAvailable() <= 0)
            {
                break;
            }
            packet = socket->RecvFrom(from);
        }
    }

    Ptr<Socket> m_socket;
    uint16_t m_port{6000};
};

class UdpEchoRttClient : public Application
{
public:
    void Setup(Ipv4Address remote, uint16_t port, Time interval, uint32_t pktSize)
    {
        m_remote = remote;
        m_port = port;
        m_interval = interval;
        m_pktSize = std::max<uint32_t>(pktSize, 16u);
    }

private:
    void StartApplication() override
    {
        m_socket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
        m_socket->Bind();
        m_socket->SetRecvCallback(MakeCallback(&UdpEchoRttClient::HandleRead, this));

        m_peer = InetSocketAddress(m_remote, m_port);
        m_running = true;

        // IMPORTANT: schedule only once; SendOnce() reschedules itself.
        Simulator::ScheduleNow(&UdpEchoRttClient::SendOnce, this);
    }

    void StopApplication() override
    {
        m_running = false;
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

    void SendOnce()
    {
        if (!m_running)
        {
            return;
        }

        Ptr<Packet> p = Create<Packet>(m_pktSize);
        TxTimeHeader h(static_cast<uint64_t>(Simulator::Now().GetNanoSeconds()));
        p->AddHeader(h);

        m_socket->SendTo(p, 0, m_peer);
        m_sendEvent = Simulator::Schedule(m_interval, &UdpEchoRttClient::SendOnce, this);
    }

    void HandleRead(Ptr<Socket> socket)
    {
        Address from;
        Ptr<Packet> p = socket->RecvFrom(from);
        while (p && p->GetSize() > 0)
        {
            TxTimeHeader h;
            p->RemoveHeader(h);

            const Time now = Simulator::Now();
            if (now >= gMs.tStart && now <= gMs.tEnd)
            {
                const uint64_t txNs = h.GetTxTimeNs();
                const int64_t nowNsSigned = now.GetNanoSeconds();
                if (txNs > 0 && nowNsSigned >= 0)
                {
                    const uint64_t nowNs = static_cast<uint64_t>(nowNsSigned);
                    if (nowNs >= txNs)
                    {
                        const double rttMs = static_cast<double>(nowNs - txNs) / 1e6;
                        gMs.rttReplies++;
                        gMs.rttSumMs += rttMs;
                    }
                }
            }

            if (socket->GetRxAvailable() <= 0)
            {
                break;
            }
            p = socket->RecvFrom(from);
        }
    }

    Ptr<Socket> m_socket;
    Address m_peer;

    Ipv4Address m_remote;
    uint16_t m_port{6000};
    Time m_interval{Seconds(0.2)};
    uint32_t m_pktSize{64};

    bool m_running{false};
    EventId m_sendEvent;
};

int main(int argc, char *argv[])
{
    std::string outDir = "results/p9";
    std::string ssidStr = "wifi-ter";
    std::string transport = "udp";        // udp|tcp
    std::string standardStr = "ax";       // ax|ac|n
    std::string rateControl = "adaptive"; // adaptive|constant
    std::string dataMode = "HeMcs7";
    std::string propModel = "logdistance"; // logdistance|friis

    double apX = 0.0, apY = 0.0;
    double x = 1.0, y = 1.0;

    double simTime = 7.0;
    double appStart = 2.0;
    double measureTime = 3.0;

    uint32_t pktSize = 1200;
    double udpRateMbps = 50.0;
    uint64_t tcpMaxBytes = 0;

    double rttInterval = 0.2;
    uint16_t rttPort = 6000;
    uint32_t rttPktSize = 64;

    double txPowerDbm = 20.0;

    // Do NOT set WifiPhy ChannelWidth via attributes in your ns-3.41 build.
    uint32_t channelWidth = 20; // MHz (only for SNR estimate + CSV)
    double freqMHz = 5180.0;

    double refDistance = 1.0;
    double refLossDb = 46.6777;
    double exponent = 3.0;

    double noiseFigureDb = 7.0;

    bool pcap = false;
    bool flowmon = false;

    uint32_t seed = 1;
    uint32_t run = 1;

    CommandLine cmd;
    cmd.AddValue("outDir", "Output directory", outDir);
    cmd.AddValue("ssid", "Wi-Fi SSID", ssidStr);
    cmd.AddValue("transport", "udp|tcp", transport);
    cmd.AddValue("standard", "ax|ac|n", standardStr);
    cmd.AddValue("rateControl", "adaptive|constant", rateControl);
    cmd.AddValue("dataMode", "ConstantRate Wifi DataMode", dataMode);
    cmd.AddValue("propModel", "logdistance|friis", propModel);

    cmd.AddValue("apX", "AP X", apX);
    cmd.AddValue("apY", "AP Y", apY);
    cmd.AddValue("x", "STA X", x);
    cmd.AddValue("y", "STA Y", y);

    cmd.AddValue("simTime", "Simulation time (s)", simTime);
    cmd.AddValue("appStart", "App start (s)", appStart);
    cmd.AddValue("measureTime", "Measure window (s)", measureTime);

    cmd.AddValue("pktSize", "Packet size", pktSize);
    cmd.AddValue("udpRateMbps", "UDP offered rate (Mbps)", udpRateMbps);
    cmd.AddValue("tcpMaxBytes", "TCP max bytes", tcpMaxBytes);

    cmd.AddValue("rttInterval", "RTT probe interval (s)", rttInterval);
    cmd.AddValue("rttPort", "RTT probe port", rttPort);
    cmd.AddValue("rttPktSize", "RTT probe pkt size", rttPktSize);

    cmd.AddValue("txPowerDbm", "Tx power (dBm)", txPowerDbm);
    cmd.AddValue("channelWidth", "Channel width for SNR estimate only (MHz)", channelWidth);
    cmd.AddValue("freqMHz", "Frequency for Friis estimate (MHz)", freqMHz);

    cmd.AddValue("refDistance", "LogDistance ref distance", refDistance);
    cmd.AddValue("refLossDb", "LogDistance ref loss", refLossDb);
    cmd.AddValue("exponent", "LogDistance exponent", exponent);

    cmd.AddValue("noiseFigureDb", "Noise figure (dB)", noiseFigureDb);

    cmd.AddValue("pcap", "Enable PCAP", pcap);
    cmd.AddValue("flowmon", "Enable FlowMonitor", flowmon);
    cmd.AddValue("seed", "RNG seed", seed);
    cmd.AddValue("run", "RNG run", run);

    cmd.Parse(argc, argv);

    transport = ToLower(transport);
    rateControl = ToLower(rateControl);
    propModel = ToLower(propModel);

    // --- Heatmap sanity: avoid saturating the link (flat goodput + huge loss) ---
    if (transport == "udp" && udpRateMbps > 20.0)
    {
        NS_LOG_UNCOND("WARN: udpRateMbps is high (" << udpRateMbps
                                                    << " Mbps). Heatmap may saturate. "
                                                    << "Try 5-20 Mbps for meaningful coverage heatmap.");
        // Optional auto-cap:
        // udpRateMbps = 20.0;
    }

    RngSeedManager::SetSeed(seed);
    RngSeedManager::SetRun(run);

    EnsureDir(outDir);
    EnsureDir(outDir + "/raw");
    EnsureDir(outDir + "/logs");
    EnsureDir(outDir + "/plots");
    EnsureDir(outDir + "/heatmaps");

    const std::string heatCsv = outDir + "/heatmaps/heatmap.csv";
    const std::string header =
        "x,y,associated,offered_mbps,goodput_mbps,avg_rtt_ms,rtt_replies,tx_bytes,rx_bytes,loss_ratio,"
        "rssi_est_dbm,snr_est_db,seed,run,standard,transport,rateControl,channelWidth";
    EnsureCsvHeader(heatCsv, header);

    // --- Project-required grid.csv (one line per point) ---
    const std::string gridCsv = outDir + "/raw/grid.csv";
    const std::string gridHeader =
        "x,y,seed,run,rssi_dbm,snr_db,goodput_mbps,rtt_ms,delay_ms,loss";
    EnsureCsvHeader(gridCsv, gridHeader);

    // -------------------- nodes --------------------
    NodeContainer wifiSta;
    wifiSta.Create(1);
    NodeContainer wifiAp;
    wifiAp.Create(1);
    NodeContainer server;
    server.Create(1);

    // -------------------- mobility --------------------
    MobilityHelper mobility;
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(wifiAp);
    mobility.Install(wifiSta);
    mobility.Install(server);

    wifiAp.Get(0)->GetObject<MobilityModel>()->SetPosition(Vector(apX, apY, 0.0));
    wifiSta.Get(0)->GetObject<MobilityModel>()->SetPosition(Vector(x, y, 0.0));
    server.Get(0)->GetObject<MobilityModel>()->SetPosition(Vector(apX, apY - 5.0, 0.0));

    // -------------------- CSMA (AP <-> Server) --------------------
    CsmaHelper csma;
    csma.SetChannelAttribute("DataRate", StringValue("100Mbps"));
    csma.SetChannelAttribute("Delay", TimeValue(MilliSeconds(1)));

    NodeContainer csmaNodes;
    csmaNodes.Add(wifiAp.Get(0));
    csmaNodes.Add(server.Get(0));
    NetDeviceContainer csmaDevs = csma.Install(csmaNodes);

    // -------------------- Wi-Fi --------------------
    WifiHelper wifi;
    wifi.SetStandard(ParseStandard(standardStr));

    if (rateControl == "constant")
    {
        wifi.SetRemoteStationManager("ns3::ConstantRateWifiManager",
                                     "DataMode", StringValue(dataMode),
                                     "ControlMode", StringValue(dataMode));
    }
    else
    {
        wifi.SetRemoteStationManager("ns3::MinstrelHtWifiManager");
    }

    Ptr<YansWifiChannel> wifiChannel = CreateObject<YansWifiChannel>();
    wifiChannel->SetPropagationDelayModel(CreateObject<ConstantSpeedPropagationDelayModel>());

    if (propModel == "friis" || propModel == "freespace")
    {
        wifiChannel->SetPropagationLossModel(CreateObject<FriisPropagationLossModel>());
    }
    else
    {
        Ptr<LogDistancePropagationLossModel> loss = CreateObject<LogDistancePropagationLossModel>();
        loss->SetAttribute("ReferenceDistance", DoubleValue(refDistance));
        loss->SetAttribute("ReferenceLoss", DoubleValue(refLossDb));
        loss->SetAttribute("Exponent", DoubleValue(exponent));
        wifiChannel->SetPropagationLossModel(loss);
    }

    YansWifiPhyHelper phy;
    phy.SetChannel(wifiChannel);
    phy.Set("TxPowerStart", DoubleValue(txPowerDbm));
    phy.Set("TxPowerEnd", DoubleValue(txPowerDbm));
    // DO NOT set ChannelWidth here (your build NS_FATALs).

    WifiMacHelper mac;
    Ssid ssid = Ssid(ssidStr);

    mac.SetType("ns3::ApWifiMac",
                "Ssid", SsidValue(ssid),
                "QosSupported", BooleanValue(true));
    NetDeviceContainer apDev = wifi.Install(phy, mac, wifiAp);

    mac.SetType("ns3::StaWifiMac",
                "Ssid", SsidValue(ssid),
                "ActiveProbing", BooleanValue(false),
                "QosSupported", BooleanValue(true));
    NetDeviceContainer staDev = wifi.Install(phy, mac, wifiSta);

    if (pcap)
    {
        phy.EnablePcap(outDir + "/raw/p9_sta", staDev.Get(0));
        phy.EnablePcap(outDir + "/raw/p9_ap", apDev.Get(0));
        csma.EnablePcap(outDir + "/raw/p9_csma", csmaDevs, true);
    }

    // -------------------- Internet stack & IP --------------------
    InternetStackHelper internet;
    internet.Install(wifiSta);
    internet.Install(wifiAp);
    internet.Install(server);

    Ipv4AddressHelper wifiIp;
    wifiIp.SetBase("10.1.0.0", "255.255.255.0");
    wifiIp.Assign(staDev);
    wifiIp.Assign(apDev);

    Ipv4AddressHelper csmaIp;
    csmaIp.SetBase("10.2.0.0", "255.255.255.0");
    Ipv4InterfaceContainer csmaIf = csmaIp.Assign(csmaDevs);

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    // -------------------- Apps: data sink --------------------
    const uint16_t port = 5000;
    Address sinkAddr(InetSocketAddress(csmaIf.GetAddress(1), port));

    PacketSinkHelper sinkHelper("ns3::UdpSocketFactory", sinkAddr);
    if (transport == "tcp")
    {
        sinkHelper = PacketSinkHelper("ns3::TcpSocketFactory", sinkAddr);
    }

    ApplicationContainer sinkApp = sinkHelper.Install(server.Get(0));
    sinkApp.Start(Seconds(0.0));
    sinkApp.Stop(Seconds(simTime));
    Ptr<PacketSink> sink = DynamicCast<PacketSink>(sinkApp.Get(0));

    // -------------------- Apps: data source --------------------
    ApplicationContainer clientApps;
    if (transport == "udp")
    {
        OnOffHelper onoff("ns3::UdpSocketFactory", sinkAddr);
        onoff.SetAttribute("PacketSize", UintegerValue(pktSize));
        onoff.SetAttribute("DataRate", DataRateValue(DataRate(std::to_string(udpRateMbps) + "Mbps")));
        onoff.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1]"));
        onoff.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));

        clientApps = onoff.Install(wifiSta.Get(0));
        clientApps.Start(Seconds(appStart));
        clientApps.Stop(Seconds(simTime));
        clientApps.Get(0)->TraceConnectWithoutContext("Tx", MakeCallback(&OnAppTx));
    }
    else
    {
        BulkSendHelper bulk("ns3::TcpSocketFactory", sinkAddr);
        bulk.SetAttribute("MaxBytes", UintegerValue(tcpMaxBytes));

        clientApps = bulk.Install(wifiSta.Get(0));
        clientApps.Start(Seconds(appStart));
        clientApps.Stop(Seconds(simTime));
        clientApps.Get(0)->TraceConnectWithoutContext("Tx", MakeCallback(&OnAppTx));
    }

    // -------------------- RTT probe server/client --------------------
    Ptr<UdpEchoRttServer> rttSrv = CreateObject<UdpEchoRttServer>();
    rttSrv->Setup(rttPort);
    server.Get(0)->AddApplication(rttSrv);
    rttSrv->SetStartTime(Seconds(0.0));
    rttSrv->SetStopTime(Seconds(simTime));

    Ptr<UdpEchoRttClient> rttCli = CreateObject<UdpEchoRttClient>();
    rttCli->Setup(csmaIf.GetAddress(1), rttPort, Seconds(rttInterval), rttPktSize);
    wifiSta.Get(0)->AddApplication(rttCli);
    rttCli->SetStartTime(Seconds(appStart + 0.1));
    rttCli->SetStopTime(Seconds(simTime));

    // -------------------- FlowMonitor (optional) --------------------
    Ptr<FlowMonitor> monitor;
    FlowMonitorHelper fmHelper;
    if (flowmon)
    {
        monitor = fmHelper.InstallAll();
    }

    // -------------------- measurement window --------------------
    gMs.Reset();
    gMs.tStart = Seconds(appStart);
    gMs.tEnd = Seconds(appStart + measureTime);

    Simulator::Schedule(gMs.tStart, &MarkSinkRxStart, sink);
    Simulator::Schedule(gMs.tEnd, &MarkSinkRxEnd, sink);

    Simulator::Stop(Seconds(simTime));
    Simulator::Run();

    if (flowmon && monitor)
    {
        monitor->CheckForLostPackets();
        std::ostringstream fn;
        fn << outDir << "/raw/flowmon_p9_x" << std::fixed << std::setprecision(1) << x
           << "_y" << y << "_run" << run << ".xml";
        monitor->SerializeToXmlFile(fn.str(), true, true);
    }

    Simulator::Destroy();

    // -------------------- metrics --------------------
    const uint64_t rxBytesWindow =
        (gMs.sinkRxEnd >= gMs.sinkRxStart) ? (gMs.sinkRxEnd - gMs.sinkRxStart) : 0;

    const double goodputMbps =
        (measureTime > 0.0) ? (static_cast<double>(rxBytesWindow) * 8.0 / (measureTime * 1e6)) : 0.0;

    const double offeredMbps =
        (measureTime > 0.0) ? (static_cast<double>(gMs.txBytesWindow) * 8.0 / (measureTime * 1e6)) : 0.0;

    const double avgRttMs =
        (gMs.rttReplies > 0) ? (gMs.rttSumMs / static_cast<double>(gMs.rttReplies)) : -1.0;

    const double lossRatio =
        (gMs.txBytesWindow > 0)
            ? std::max(0.0, std::min(1.0, 1.0 - (static_cast<double>(rxBytesWindow) /
                                                 static_cast<double>(gMs.txBytesWindow))))
            : -1.0;

    // association (real STA association state if available)
    bool associated = false;
    Ptr<WifiNetDevice> wnd = DynamicCast<WifiNetDevice>(staDev.Get(0));
    if (wnd)
    {
        Ptr<WifiMac> wmac = wnd->GetMac();
        Ptr<StaWifiMac> staMac = DynamicCast<StaWifiMac>(wmac);
        if (staMac)
        {
            associated = staMac->IsAssociated();
        }
    }
    // fallback if cast fails
    if (!associated)
    {
        associated = (rxBytesWindow > 0) || (gMs.rttReplies > 0);
    }

    const double d = Distance2d(apX, apY, x, y);
    const double rssiEstDbm =
        EstimateRxPowerDbm(propModel, txPowerDbm, d, refDistance, refLossDb, exponent, freqMHz);

    const double bwHz = static_cast<double>(channelWidth) * 1e6;
    const double noiseDbm = ThermalNoiseDbm(bwHz, noiseFigureDb);
    const double snrEstDb = rssiEstDbm - noiseDbm;

    // -------------------- append CSV --------------------
    std::ofstream f(heatCsv.c_str(), std::ios::app);
    if (!f.is_open())
    {
        NS_LOG_UNCOND("ERROR: cannot open " << heatCsv);
        return 1;
    }

    f << std::fixed << std::setprecision(6)
      << x << ","
      << y << ","
      << (associated ? 1 : 0) << ","
      << offeredMbps << ","
      << goodputMbps << ","
      << avgRttMs << ","
      << gMs.rttReplies << ","
      << gMs.txBytesWindow << ","
      << rxBytesWindow << ","
      << lossRatio << ","
      << rssiEstDbm << ","
      << snrEstDb << ","
      << seed << ","
      << run << ","
      << ToLower(standardStr) << ","
      << transport << ","
      << rateControl << ","
      << channelWidth
      << "\n";
    f.close();

    NS_LOG_UNCOND("P9 point (" << x << "," << y << ") assoc=" << (associated ? 1 : 0)
                               << " offered=" << offeredMbps << " Mbps"
                               << " goodput=" << goodputMbps << " Mbps"
                               << " rtt=" << avgRttMs << " ms");

    // --- append project grid.csv line (safe: per-run outDir, no race in parallel) ---
    // delay_ms: اگر delay جداگانه اندازه نگرفتی، -1 بگذار (طبق PDF مجاز است)
    const double delayMs = -1.0; // or NaN if you prefer: std::numeric_limits<double>::quiet_NaN()

    std::ofstream g(gridCsv.c_str(), std::ios::app);
    if (!g.is_open())
    {
        NS_LOG_UNCOND("ERROR: cannot open " << gridCsv);
        return 1;
    }

    g << std::fixed << std::setprecision(6)
      << x << ","
      << y << ","
      << seed << ","
      << run << ","
      << rssiEstDbm << ","
      << snrEstDb << ","
      << goodputMbps << ","
      << avgRttMs << ","
      << delayMs << ","
      << lossRatio
      << "\n";
    g.close();

    return 0;
}
