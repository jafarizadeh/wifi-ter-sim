/*
 * Projet 7 (ns-3) - Channel planning (co-channel vs separate)
 * Realistic + robust version (inspired by P5: per-flow + Jain; P6: CSMA backbone + clean outputs)
 *
 * Key idea:
 *  - cocanal : AP1+Cell1 and AP2+Cell2 share SAME YansWifiChannel (strong mutual contention)
 *  - separe  : AP1+Cell1 on channel 1, AP2+Cell2 on channel 2
 *              We TRY to set real channel/frequency at PHY level. If not supported by your ns-3,
 *              we fallback to separate YansWifiChannel objects (robust across versions).
 *
 * Outputs (outDir/raw):
 *  - perflow_{plan}_n{N}_run{run}.csv   columns: cellId,staId,rxBytes,goodputbps
 *  - p7_summary.csv  columns:
 *      channelPlan,nStaPerCell,chan1,chan2,seed,run,goodputCell1,goodputCell2,goodputTotal,jainCells
 * Optional:
 *  - flowmon xml
 *  - pcap
 *
 * IMPORTANT FIXES (per your request):
 *  - Removed manual override of WifiDefaultAssocManager (can break association in some builds).
 *  - Added robust logging toggles + log prefixes (node/time/function) to verify Auth/Assoc.
 *  - Kept ~90% structure/details the same: same knobs, same outputs, same scenario logic.
 */

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/wifi-module.h"
#include "ns3/yans-wifi-helper.h"
#include "ns3/mobility-module.h"
#include "ns3/csma-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/random-variable-stream.h"

#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>

using namespace ns3;

// -------------------- small utilities --------------------
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
        return true;
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

static double
Jain2(double g1, double g2)
{
    const double denom = 2.0 * (g1 * g1 + g2 * g2);
    if (denom <= 0.0)
        return 0.0;
    const double num = (g1 + g2) * (g1 + g2);
    return num / denom;
}

static Vector
PointOnCircle(double cx, double cy, double r, uint32_t i, uint32_t n, double z = 0.0)
{
    if (n == 0)
        return Vector(cx, cy, z);
    const double ang = (2.0 * M_PI * static_cast<double>(i)) / static_cast<double>(n);
    return Vector(cx + r * std::cos(ang), cy + r * std::sin(ang), z);
}

// Map common 5GHz channel numbers to center frequency in MHz.
// (sufficient for channels 36..165; for this project 36/40 enough)
static uint16_t
Wifi5GhzChannelToFreqMhz(int ch)
{
    // 5 GHz: f = 5000 + 5*ch (MHz) for common channels (e.g., 36->5180, 40->5200)
    if (ch <= 0)
        return 5180;
    return static_cast<uint16_t>(5000 + 5 * ch);
}

// Try to set an attribute on WifiPhy; if not supported, do nothing.
// Works across versions without breaking compilation.
static void
SetPhyAttributeIfExists(Ptr<WifiPhy> phy, const std::string &name, const AttributeValue &v)
{
    if (!phy)
        return;
    bool ok = phy->SetAttributeFailSafe(name, v);
    (void)ok;
}

// Configure channel/frequency as realistically as possible.
// If your ns-3 supports channel/frequency attributes, it will apply them.
// Otherwise, the code still works and relies on shared vs separate YansWifiChannel objects.
static void
ConfigureOperatingChannel(Ptr<WifiNetDevice> dev, int channelNumber, uint16_t widthMhz)
{
    if (!dev)
        return;

    Ptr<WifiPhy> phy = dev->GetPhy();
    if (!phy)
        return;

    const uint16_t freq = Wifi5GhzChannelToFreqMhz(channelNumber);

    // Prefer numeric attributes (most robust)
    SetPhyAttributeIfExists(phy, "ChannelNumber", UintegerValue(static_cast<uint32_t>(channelNumber)));
    SetPhyAttributeIfExists(phy, "ChannelWidth", UintegerValue(static_cast<uint32_t>(widthMhz)));
    SetPhyAttributeIfExists(phy, "Frequency", UintegerValue(static_cast<uint32_t>(freq)));

    // Optional: some ns-3 versions expose a composite ChannelSettings attribute.
    // NOTE: We avoid "channelNumber=36" style strings (they may crash Uinteger parsing).
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
    // -------------------- Defaults --------------------
    double simTime = 25.0;
    double appStart = 2.0;
    uint32_t nStaPerCell = 10;

    // For project-7, using different SSIDs per cell avoids accidental cross-association
    // while still allowing co-channel contention if they share the medium.
    std::string ssid1Str = "cell1";
    std::string ssid2Str = "cell2";

    std::string outDir = "results/p7";
    bool pcap = false;
    bool flowmon = true;

    // Geometry: two APs close enough so their cells overlap/interfere
    double apSeparation = 15.0; // meters
    double rSta = 5.0;          // meters

    // Traffic (per STA)
    uint32_t pktSize = 1200;
    std::string udpRatePerSta = "10Mbps";

    // Channel plan
    std::string channelPlan = "cocanal"; // cocanal | separe
    int chan1 = 36;
    int chan2 = 40;
    uint16_t channelWidthMhz = 20;

    // Realism knobs
    double txPowerDbm = 16.0;
    double noiseFigureDb = 7.0;

    // Propagation realism
    double logExp = 3.0;
    double shadowingSigmaDb = 4.0;
    bool enableFading = true;

    // Logging controls (NEW but optional; defaults keep behavior close)
    bool enableWifiLogs = true;          // was always enabled in your code; keep default true
    std::string wifiLogLevel = "INFO";   // INFO|DEBUG
    bool enableLogPrefixes = true;       // helpful to disambiguate node/time
    bool enableAssocManagerLogs = false; // turn on if you want to see Auth/Assoc details

    int seed = 1;
    int run = 1;

    CommandLine cmd;
    cmd.AddValue("simTime", "Total simulation time (s)", simTime);
    cmd.AddValue("appStart", "Traffic start time (s)", appStart);
    cmd.AddValue("nStaPerCell", "Number of STAs per cell (N)", nStaPerCell);

    cmd.AddValue("ssid1", "SSID for cell 1", ssid1Str);
    cmd.AddValue("ssid2", "SSID for cell 2", ssid2Str);

    cmd.AddValue("outDir", "Output directory", outDir);
    cmd.AddValue("pcap", "Enable PCAP", pcap);
    cmd.AddValue("flowmon", "Enable FlowMonitor", flowmon);

    cmd.AddValue("apSeparation", "Distance AP1-AP2 (m)", apSeparation);
    cmd.AddValue("rSta", "STA radius around each AP (m)", rSta);

    cmd.AddValue("pktSize", "UDP packet size (bytes)", pktSize);
    cmd.AddValue("udpRatePerSta", "UDP offered load per STA", udpRatePerSta);

    cmd.AddValue("channelPlan", "Channel plan: cocanal or separe", channelPlan);
    cmd.AddValue("chan1", "Channel number label/AP1 channel", chan1);
    cmd.AddValue("chan2", "Channel number label/AP2 channel", chan2);
    cmd.AddValue("channelWidth", "Channel width in MHz (20/40)", channelWidthMhz);

    cmd.AddValue("txPowerDbm", "Tx power (dBm) on all Wi-Fi PHY", txPowerDbm);
    cmd.AddValue("noiseFigureDb", "Rx noise figure (dB)", noiseFigureDb);

    cmd.AddValue("logExp", "LogDistance exponent", logExp);
    cmd.AddValue("shadowingSigmaDb", "Shadowing sigma (dB)", shadowingSigmaDb);
    cmd.AddValue("enableFading", "Enable Nakagami fading", enableFading);

    cmd.AddValue("seed", "RNG seed", seed);
    cmd.AddValue("run", "RNG run id", run);

    // NEW logging toggles (optional)
    cmd.AddValue("enableWifiLogs", "Enable Wi-Fi MAC logs (StaWifiMac/ApWifiMac)", enableWifiLogs);
    cmd.AddValue("wifiLogLevel", "Wi-Fi log level: INFO or DEBUG", wifiLogLevel);
    cmd.AddValue("enableLogPrefixes", "Enable log prefixes (time/node/function)", enableLogPrefixes);
    cmd.AddValue("enableAssocManagerLogs", "Enable WifiAssocManager/WifiDefaultAssocManager logs (DEBUG)", enableAssocManagerLogs);

    cmd.Parse(argc, argv);

    auto SetDefaultSafe = [](const std::string &name, const AttributeValue &v)
    {
        Config::SetDefaultFailSafe(name, v);
    };

    // Safer association/scan timers (helps with passive probing builds)
    SetDefaultSafe("ns3::StaWifiMac::AssocRequestTimeout", TimeValue(Seconds(0.5)));
    SetDefaultSafe("ns3::StaWifiMac::ProbeRequestTimeout", TimeValue(MilliSeconds(100)));
    SetDefaultSafe("ns3::StaWifiMac::WaitBeaconTimeout", TimeValue(MilliSeconds(200)));
    SetDefaultSafe("ns3::StaWifiMac::MaxMissedBeacons", UintegerValue(5));

    // ---- Logging (robust + optional) ----
    wifiLogLevel = ToLower(wifiLogLevel);
    LogLevel level = LOG_LEVEL_INFO;
    if (wifiLogLevel == "debug")
        level = LOG_LEVEL_DEBUG;

    if (enableLogPrefixes)
    {
        LogComponentEnableAll(LOG_PREFIX_TIME);
        LogComponentEnableAll(LOG_PREFIX_NODE);
        LogComponentEnableAll(LOG_PREFIX_FUNC);
    }

    if (enableWifiLogs)
    {
        LogComponentEnable("StaWifiMac", level);
        LogComponentEnable("ApWifiMac", level);
    }
    if (enableAssocManagerLogs)
    {
        LogComponentEnable("WifiAssocManager", LOG_LEVEL_DEBUG);
        LogComponentEnable("WifiDefaultAssocManager", LOG_LEVEL_DEBUG);
    }

    channelPlan = ToLower(channelPlan);
    if (channelPlan != "cocanal" && channelPlan != "separe")
    {
        NS_LOG_UNCOND("ERROR: --channelPlan must be 'cocanal' or 'separe'");
        return 1;
    }
    if (simTime <= appStart)
    {
        NS_LOG_UNCOND("ERROR: simTime must be > appStart");
        return 1;
    }
    if (nStaPerCell == 0 || apSeparation <= 0.0 || rSta <= 0.0)
    {
        NS_LOG_UNCOND("ERROR: invalid geometry parameters");
        return 1;
    }

    // -------------------- Reproducibility --------------------
    RngSeedManager::SetSeed(seed);
    RngSeedManager::SetRun(run);

    // -------------------- Output directories --------------------
    EnsureDir(outDir);
    EnsureDir(outDir + "/raw");
    EnsureDir(outDir + "/logs");
    EnsureDir(outDir + "/plots");

    const std::string summaryPath = outDir + "/raw/p7_summary.csv";
    const std::string summaryHeader =
        "channelPlan,nStaPerCell,chan1,chan2,seed,run,goodputCell1,goodputCell2,goodputTotal,jainCells";
    EnsureCsvHeader(summaryPath, summaryHeader);

    std::ostringstream perflowName;
    perflowName << outDir << "/raw/perflow_" << channelPlan << "_n" << nStaPerCell << "_run" << run << ".csv";
    const std::string perflowPath = perflowName.str();

    // -------------------- Nodes --------------------
    NodeContainer apNodes;
    apNodes.Create(2);
    Ptr<Node> ap1 = apNodes.Get(0);
    Ptr<Node> ap2 = apNodes.Get(1);

    NodeContainer staCell1, staCell2;
    staCell1.Create(nStaPerCell);
    staCell2.Create(nStaPerCell);

    NodeContainer serverNode;
    serverNode.Create(1);
    Ptr<Node> server = serverNode.Get(0);

    // -------------------- Mobility (ConstantPosition) --------------------
    MobilityHelper mobility;
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");

    mobility.Install(apNodes);
    ap1->GetObject<MobilityModel>()->SetPosition(Vector(0.0, 0.0, 0.0));
    ap2->GetObject<MobilityModel>()->SetPosition(Vector(apSeparation, 0.0, 0.0));

    mobility.Install(staCell1);
    mobility.Install(staCell2);

    for (uint32_t i = 0; i < nStaPerCell; ++i)
    {
        Vector p1 = PointOnCircle(0.0, 0.0, rSta, i, nStaPerCell);
        staCell1.Get(i)->GetObject<MobilityModel>()->SetPosition(p1);

        Vector p2 = PointOnCircle(apSeparation, 0.0, rSta, i, nStaPerCell);
        staCell2.Get(i)->GetObject<MobilityModel>()->SetPosition(p2);
    }

    // -------------------- Propagation: LogDistance + Shadowing + Nakagami --------------------
    Ptr<LogDistancePropagationLossModel> logd = CreateObject<LogDistancePropagationLossModel>();
    logd->SetAttribute("Exponent", DoubleValue(logExp));
    logd->SetAttribute("ReferenceDistance", DoubleValue(1.0));
    logd->SetAttribute("ReferenceLoss", DoubleValue(46.6777)); // close to 5GHz @1m

    // Shadowing as log-normal via normal RV on dB (RandomPropagationLossModel adds/subtracts dB)
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

    // -------------------- Channel plan (robust, P6-style) --------------------
    Ptr<YansWifiChannel> chA = CreateObject<YansWifiChannel>();
    chA->SetPropagationDelayModel(CreateObject<ConstantSpeedPropagationDelayModel>());
    chA->SetPropagationLossModel(logd);

    Ptr<YansWifiChannel> chB = chA;
    if (channelPlan == "separe")
    {
        // Separate channel object for the second cell.
        chB = CreateObject<YansWifiChannel>();
        chB->SetPropagationDelayModel(CreateObject<ConstantSpeedPropagationDelayModel>());

        // Reuse same propagation chain instance (keeps behavior close to your original).
        // If you need independent randomness per channel, rebuild the chain here.
        chB->SetPropagationLossModel(logd);
    }

    // -------------------- Wi-Fi helpers --------------------
    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211ac); // robust + realistic for 5 GHz
    wifi.SetRemoteStationManager("ns3::MinstrelHtWifiManager");

    YansWifiPhyHelper phyA;
    YansWifiPhyHelper phyB;

    phyA.SetChannel(chA);
    phyB.SetChannel(chB);

    // Tx power + noise figure (realistic knobs)
    phyA.Set("TxPowerStart", DoubleValue(txPowerDbm));
    phyA.Set("TxPowerEnd", DoubleValue(txPowerDbm));
    phyA.Set("TxPowerLevels", UintegerValue(1));
    phyA.Set("RxNoiseFigure", DoubleValue(noiseFigureDb));

    phyB.Set("TxPowerStart", DoubleValue(txPowerDbm));
    phyB.Set("TxPowerEnd", DoubleValue(txPowerDbm));
    phyB.Set("TxPowerLevels", UintegerValue(1));
    phyB.Set("RxNoiseFigure", DoubleValue(noiseFigureDb));

    WifiMacHelper mac;

    Ssid ssid1(ssid1Str);
    Ssid ssid2(ssid2Str);

    // -------------------- Install AP devices --------------------
    mac.SetType("ns3::ApWifiMac",
                "Ssid", SsidValue(ssid1),
                "BeaconInterval", TimeValue(MicroSeconds(1024 * 100)));
    NetDeviceContainer ap1Dev = wifi.Install(phyA, mac, ap1);

    mac.SetType("ns3::ApWifiMac",
                "Ssid", SsidValue(ssid2),
                "BeaconInterval", TimeValue(MicroSeconds(1024 * 100)));
    NetDeviceContainer ap2Dev = wifi.Install(phyB, mac, ap2);

    // -------------------- Install STA devices (Cell1 uses phyA, Cell2 uses phyB) --------------------
    mac.SetType("ns3::StaWifiMac",
                "Ssid", SsidValue(ssid1),
                "ActiveProbing", BooleanValue(true));
    NetDeviceContainer sta1Devs = wifi.Install(phyA, mac, staCell1);

    mac.SetType("ns3::StaWifiMac",
                "Ssid", SsidValue(ssid2),
                "ActiveProbing", BooleanValue(true));
    NetDeviceContainer sta2Devs = wifi.Install(phyB, mac, staCell2);

    // -------------------- Attempt to set real channel numbers (better realism) --------------------
    Ptr<WifiNetDevice> ap1Wifi = DynamicCast<WifiNetDevice>(ap1Dev.Get(0));
    Ptr<WifiNetDevice> ap2Wifi = DynamicCast<WifiNetDevice>(ap2Dev.Get(0));

    // Decide per-cell channels
    int c1 = chan1;
    int c2 = (channelPlan == "cocanal") ? chan1 : chan2;

    // Apply to APs
    ConfigureOperatingChannel(ap1Wifi, c1, channelWidthMhz);
    ConfigureOperatingChannel(ap2Wifi, c2, channelWidthMhz);

    // Apply to ALL STAs (helps scanning/association in many ns-3 builds)
    for (uint32_t i = 0; i < nStaPerCell; ++i)
    {
        Ptr<WifiNetDevice> d1 = DynamicCast<WifiNetDevice>(sta1Devs.Get(i));
        Ptr<WifiNetDevice> d2 = DynamicCast<WifiNetDevice>(sta2Devs.Get(i));
        ConfigureOperatingChannel(d1, c1, channelWidthMhz);
        ConfigureOperatingChannel(d2, c2, channelWidthMhz);
    }

    // -------------------- CSMA backbone: AP1, AP2, Server --------------------
    CsmaHelper csma;
    csma.SetChannelAttribute("DataRate", StringValue("1Gbps"));
    csma.SetChannelAttribute("Delay", TimeValue(MicroSeconds(50)));

    NodeContainer csmaNodes;
    csmaNodes.Add(ap1);
    csmaNodes.Add(ap2);
    csmaNodes.Add(server);

    NetDeviceContainer csmaDevs = csma.Install(csmaNodes);

    // -------------------- Internet stack + addressing (as in P6) --------------------
    InternetStackHelper internet;
    internet.Install(staCell1);
    internet.Install(staCell2);
    internet.Install(apNodes);
    internet.Install(serverNode);

    Ipv4AddressHelper ipv4;

    // ---------- Addressing (keep project convention) ----------
    // Wi-Fi subnet 10.1.0.0/24
    ipv4.SetBase("10.1.0.0", "255.255.255.0");

    // Assign in a controlled order (no ambiguity)
    Ipv4InterfaceContainer ap1WifiIf = ipv4.Assign(ap1Dev);
    Ipv4InterfaceContainer sta1WifiIf = ipv4.Assign(sta1Devs);
    Ipv4InterfaceContainer ap2WifiIf = ipv4.Assign(ap2Dev);
    Ipv4InterfaceContainer sta2WifiIf = ipv4.Assign(sta2Devs);

    // CSMA subnet 10.2.0.0/24
    ipv4.SetBase("10.2.0.0", "255.255.255.0");
    Ipv4InterfaceContainer csmaIfs = ipv4.Assign(csmaDevs);

    // csmaNodes = [ap1, ap2, server] => server is index 2
    Ipv4Address serverIp = csmaIfs.GetAddress(2);

    // ---------- Routing + Forwarding ----------
    ap1->GetObject<Ipv4>()->SetAttribute("IpForward", BooleanValue(true));
    ap2->GetObject<Ipv4>()->SetAttribute("IpForward", BooleanValue(true));

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    // ---- Deterministic routing for STA -> CSMA network ----
    Ipv4StaticRoutingHelper staticRouting;

    Ipv4Address ap1Gw = ap1WifiIf.GetAddress(0); // AP1 Wi-Fi IP
    Ipv4Address ap2Gw = ap2WifiIf.GetAddress(0); // AP2 Wi-Fi IP

    Ipv4Address csmaNet("10.2.0.0");
    Ipv4Mask csmaMask("255.255.255.0");

    // Cell 1 STAs
    for (uint32_t i = 0; i < nStaPerCell; ++i)
    {
        Ptr<Node> sta = staCell1.Get(i);
        Ptr<Ipv4> ip = sta->GetObject<Ipv4>();
        uint32_t ifIndex = ip->GetInterfaceForDevice(sta1Devs.Get(i));
        Ptr<Ipv4StaticRouting> sr = staticRouting.GetStaticRouting(ip);

        sr->AddNetworkRouteTo(csmaNet, csmaMask, ap1Gw, ifIndex);
        sr->SetDefaultRoute(ap1Gw, ifIndex);
    }

    // Cell 2 STAs
    for (uint32_t i = 0; i < nStaPerCell; ++i)
    {
        Ptr<Node> sta = staCell2.Get(i);
        Ptr<Ipv4> ip = sta->GetObject<Ipv4>();
        uint32_t ifIndex = ip->GetInterfaceForDevice(sta2Devs.Get(i));
        Ptr<Ipv4StaticRouting> sr = staticRouting.GetStaticRouting(ip);

        sr->AddNetworkRouteTo(csmaNet, csmaMask, ap2Gw, ifIndex);
        sr->SetDefaultRoute(ap2Gw, ifIndex);
    }

    const double usefulDuration = simTime - appStart;

    std::vector<Ptr<PacketSink>> sinks;
    sinks.reserve(2 * nStaPerCell);

    uint16_t basePort = 9000;

    // -------------------- Apps (OnOff UDP -> Server sinks) --------------------
    // Cell 1
    for (uint32_t i = 0; i < nStaPerCell; ++i)
    {
        uint16_t port = basePort + static_cast<uint16_t>(0 * 100 + i);

        PacketSinkHelper sinkHelper("ns3::UdpSocketFactory",
                                    InetSocketAddress(Ipv4Address::GetAny(), port));
        ApplicationContainer sinkApp = sinkHelper.Install(server);
        sinkApp.Start(Seconds(0.0));
        sinkApp.Stop(Seconds(simTime));

        Ptr<PacketSink> sink = DynamicCast<PacketSink>(sinkApp.Get(0));
        sinks.push_back(sink);

        OnOffHelper onoff("ns3::UdpSocketFactory", InetSocketAddress(serverIp, port));
        onoff.SetAttribute("DataRate", DataRateValue(DataRate(udpRatePerSta)));
        onoff.SetAttribute("PacketSize", UintegerValue(pktSize));

        Ptr<ConstantRandomVariable> onRv = CreateObject<ConstantRandomVariable>();
        onRv->SetAttribute("Constant", DoubleValue(1.0));

        Ptr<ConstantRandomVariable> offRv = CreateObject<ConstantRandomVariable>();
        offRv->SetAttribute("Constant", DoubleValue(0.0));

        onoff.SetAttribute("OnTime", PointerValue(onRv));
        onoff.SetAttribute("OffTime", PointerValue(offRv));

        ApplicationContainer client = onoff.Install(staCell1.Get(i));

        Ptr<UniformRandomVariable> startJitter = CreateObject<UniformRandomVariable>();
        startJitter->SetAttribute("Min", DoubleValue(0.0));
        startJitter->SetAttribute("Max", DoubleValue(0.2));

        client.Start(Seconds(appStart + startJitter->GetValue()));
        client.Stop(Seconds(simTime));
    }

    // Cell 2
    for (uint32_t i = 0; i < nStaPerCell; ++i)
    {
        uint16_t port = basePort + static_cast<uint16_t>(1 * 100 + i);

        PacketSinkHelper sinkHelper("ns3::UdpSocketFactory",
                                    InetSocketAddress(Ipv4Address::GetAny(), port));
        ApplicationContainer sinkApp = sinkHelper.Install(server);
        sinkApp.Start(Seconds(0.0));
        sinkApp.Stop(Seconds(simTime));

        Ptr<PacketSink> sink = DynamicCast<PacketSink>(sinkApp.Get(0));
        sinks.push_back(sink);

        OnOffHelper onoff("ns3::UdpSocketFactory", InetSocketAddress(serverIp, port));
        onoff.SetAttribute("DataRate", DataRateValue(DataRate(udpRatePerSta)));
        onoff.SetAttribute("PacketSize", UintegerValue(pktSize));

        // Pareto with Mean≈0.4 and Shape=1.5  => Scale = 0.1333333333
        const double shape = 1.5;
        const double meanDesired = 0.4;
        const double scale = meanDesired * (shape - 1.0) / shape; // 0.1333333333

        Ptr<ConstantRandomVariable> onRv = CreateObject<ConstantRandomVariable>();
        onRv->SetAttribute("Constant", DoubleValue(1.0));

        Ptr<ConstantRandomVariable> offRv = CreateObject<ConstantRandomVariable>();
        offRv->SetAttribute("Constant", DoubleValue(0.0));

        onoff.SetAttribute("OnTime", PointerValue(onRv));
        onoff.SetAttribute("OffTime", PointerValue(offRv));

        ApplicationContainer client = onoff.Install(staCell2.Get(i));

        Ptr<UniformRandomVariable> startJitter = CreateObject<UniformRandomVariable>();
        startJitter->SetAttribute("Min", DoubleValue(0.0));
        startJitter->SetAttribute("Max", DoubleValue(0.2));

        client.Start(Seconds(appStart + startJitter->GetValue()));
        client.Stop(Seconds(simTime));
    }

    // -------------------- FlowMonitor (optional) --------------------
    FlowMonitorHelper flowHelper;
    Ptr<FlowMonitor> monitor;
    if (flowmon)
    {
        monitor = flowHelper.InstallAll();
    }

    // -------------------- PCAP (optional) --------------------
    if (pcap)
    {
        std::ostringstream base;
        base << outDir << "/raw/pcap_" << channelPlan << "_n" << nStaPerCell << "_run" << run;
        phyA.EnablePcap(base.str() + "_ap1", ap1Dev.Get(0), true);
        phyB.EnablePcap(base.str() + "_ap2", ap2Dev.Get(0), true);
        csma.EnablePcap(base.str() + "_server_csma", csmaDevs.Get(2), true);
    }

    // -------------------- Run simulation --------------------
    Simulator::Stop(Seconds(simTime));
    Simulator::Run();

    // -------------------- Collect stats + write CSVs --------------------
    double goodputCell1 = 0.0;
    double goodputCell2 = 0.0;

    {
        std::ofstream pf(perflowPath.c_str(), std::ios::out);
        pf << "cellId,staId,rxBytes,goodputbps\n";
        pf << std::fixed << std::setprecision(6);

        // first N sinks -> cell1
        for (uint32_t i = 0; i < nStaPerCell; ++i)
        {
            const uint64_t rx = sinks.at(i)->GetTotalRx();
            const double gp = (usefulDuration > 0.0) ? (8.0 * static_cast<double>(rx) / usefulDuration) : 0.0;
            goodputCell1 += gp;
            pf << 1 << "," << i << "," << rx << "," << gp << "\n";
        }

        // next N sinks -> cell2
        for (uint32_t i = 0; i < nStaPerCell; ++i)
        {
            const uint64_t rx = sinks.at(nStaPerCell + i)->GetTotalRx();
            const double gp = (usefulDuration > 0.0) ? (8.0 * static_cast<double>(rx) / usefulDuration) : 0.0;
            goodputCell2 += gp;
            pf << 2 << "," << i << "," << rx << "," << gp << "\n";
        }
    }

    const double goodputTotal = goodputCell1 + goodputCell2;
    const double jCells = Jain2(goodputCell1, goodputCell2);

    {
        std::ofstream sum(summaryPath.c_str(), std::ios::app);
        sum << std::fixed << std::setprecision(6);
        sum << channelPlan << ","
            << nStaPerCell << ","
            << chan1 << ","
            << chan2 << ","
            << seed << ","
            << run << ","
            << goodputCell1 << ","
            << goodputCell2 << ","
            << goodputTotal << ","
            << jCells << "\n";
    }

    // Console summary
    std::cout << "=== Projet 7 summary ===\n";
    std::cout << " Plan: " << channelPlan << "\n";
    std::cout << " N per cell: " << nStaPerCell
              << " | chan1=" << chan1 << " chan2=" << chan2
              << " | width=" << channelWidthMhz << "MHz\n";
    std::cout << " Goodput cell1 (Mbps): " << (goodputCell1 / 1e6) << "\n";
    std::cout << " Goodput cell2 (Mbps): " << (goodputCell2 / 1e6) << "\n";
    std::cout << " Goodput total (Mbps): " << (goodputTotal / 1e6) << "\n";
    std::cout << " JainCells: " << jCells << "\n";
    std::cout << " Perflow CSV: " << perflowPath << "\n";
    std::cout << " Summary CSV: " << summaryPath << "\n";

    if (flowmon && monitor)
    {
        monitor->CheckForLostPackets();
        std::ostringstream fm;
        fm << outDir << "/raw/flowmon_" << channelPlan << "_n" << nStaPerCell << "_run" << run << ".xml";
        monitor->SerializeToXmlFile(fm.str(), true, true);
    }

    Simulator::Destroy();
    return 0;
}
